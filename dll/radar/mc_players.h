// mc_players.h — МОДУЛЬ радара игроков + предмет в руке (real-time). Цели по указателю: swordIron, bow.
// Адреса игроков сканируются РЕДКО (~1.5с — они в old-gen, почти не «переезжают»), а позиция и предмет в руке
// перечитываются ЧАСТО (~100мс) по уже известным адресам — это и есть real-time без поиска по куче каждый кадр.
// Данные отдаются функцией Get() (потокобезопасно). isTarget=true, если в руке одна из целей.
#pragma once
#include "mc_core.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <cmath>

namespace mc {

class PlayerRadar {
public:
    ~PlayerRadar() { Stop(); }

    // Требует, чтобы дискавери нашёл классы игроков (nPlayers>0) и смещение ника (entName). Резолвит цели.
    // target1/target2 — unlocalizedName целей в руке (по умолчанию "swordIron"/"bow"); nullptr = слот не используется.
    // held игрока помечается isTarget=true, если совпал с одной из целей (матч по указателю Item).
    bool Start(const Layout& L, const wchar_t* target1 = L"swordIron", const wchar_t* target2 = L"bow", int readMs = 100, int rescanMs = 1500) {
        L_ = L; readMs_ = readMs < 30 ? 30 : readMs; rescanMs_ = rescanMs < 300 ? 300 : rescanMs;
        sword_ = target1 ? ResolveItem(L_, target1) : 0;
        bow_   = target2 ? ResolveItem(L_, target2) : 0;
        if (!L_.nPlayers || !L_.entName || !L_.entWorldObj) return false;
        run_ = true; th_ = std::thread(&PlayerRadar::Loop, this); return true;
    }
    void Stop() { run_ = false; if (th_.joinable()) th_.join(); }

    std::vector<PlayerInfo> Get() { std::lock_guard<std::mutex> lk(mtx_); return latest_; }   // свежий список игроков
    // Мировая позиция ГЛАЗ локального игрока (feet + ~1.62) — для сопоставления с eye-space дистанцией рендера. false=нет игрока.
    bool LocalEye(double& x, double& y, double& z) const {
        const uintptr_t mc = CurrentMinecraft(L_);
        const uintptr_t lp = (mc && L_.mcThePlayer) ? ReadOop(mc, L_.mcThePlayer) : 0;
        if (!lp) return false;
        x = 0; y = 0; z = 0; SafeRead(lp + L_.entPosX, x); SafeRead(lp + L_.entPosY, y); SafeRead(lp + L_.entPosZ, z);
        y += 1.62; return true;
    }
    void StopAsync() { run_ = false; if (th_.joinable()) th_.detach(); }   // для выгрузки из DllMain: без join (избегаем deadlock загрузчика)
    void SetCoords(int x, int y, int z) { L_.entPosX = x; L_.entPosY = y; L_.entPosZ = z; }   // самолечение координат в рантайме (int-запись атомарна на x86)
    void Offsets(int& x, int& y, int& z) const { x = L_.entPosX; y = L_.entPosY; z = L_.entPosZ; }   // текущие оффсеты (диагностика)
    uintptr_t swordItem() const { return sword_; }
    uintptr_t bowItem()   const { return bow_; }
    size_t    knownAddrs() const { std::lock_guard<std::mutex> lk(amtx_); return addrs_.size(); }
    bool      UsingWorldList() const { return L_.worldPlayerList != 0; }   // true = берём игроков из списка мира, false = фолбэк-скан

    // Чистая функция (тестируемая офлайн): из адресов игроков собрать список текущего мира (ник, координаты,
    // дистанция, предмет в руке + isTarget по указателю). targets — до 4 целевых Item* (0 в конце игнорируются).
    static std::vector<PlayerInfo> Collect(const Layout& L, const std::vector<uintptr_t>& addrs, const uintptr_t* targets, int nTargets) {
        std::vector<PlayerInfo> out;
        const uintptr_t mc = CurrentMinecraft(L);
        const uintptr_t world = IsHeapAddr(mc) ? ReadOop(mc, L.mcTheWorld) : 0;
        if (!world) return out;
        const uintptr_t localP = L.mcThePlayer ? ReadOop(mc, L.mcThePlayer) : 0;
        double lx = 0, ly = 0, lz = 0; bool haveL = false;
        if (localP) { SafeRead(localP + L.entPosX, lx); SafeRead(localP + L.entPosY, ly); SafeRead(localP + L.entPosZ, lz); haveL = true; }
        for (uintptr_t e : addrs) {
            if (IsForwarded(e)) continue;
            const uint32_t nk = NarrowKlassOf(e);
            bool isP = false; for (int i = 0; i < L.nPlayers; ++i) if (nk == L.nkPlayers[i]) { isP = true; break; }
            if (!isP) continue;
            if (ReadOop(e, L.entWorldObj) != world) continue;                 // только текущий мир игрока
            PlayerInfo p; p.entity = e; p.isLocal = (e == localP);
            const uintptr_t s = ReadOop(e, L.entName);
            if (s && NarrowKlassOf(s) == L.nkString) ReadJavaString(L, s, p.name);
            p.resolving = (!p.name.empty() && p.name[0] == L'-');
            SafeRead(e + L.entPosX, p.x); SafeRead(e + L.entPosY, p.y); SafeRead(e + L.entPosZ, p.z);
            if (!(p.x == p.x && p.z == p.z && fabs(p.x) < 3e7 && fabs(p.z) < 3e7)) continue;   // мусорные оси (смещение не выучено)
            if (p.x == 0.0 && p.y == 0.0 && p.z == 0.0) continue;             // непозиционированный
            p.dist = haveL ? std::sqrt((p.x - lx) * (p.x - lx) + (p.y - ly) * (p.y - ly) + (p.z - lz) * (p.z - lz)) : 0.0;
            if (!p.isLocal) {                                                 // предмет в руке (чужих)
                uintptr_t ip = 0, mt = 0; std::wstring nm; int cnt = 0;
                if (L.invMainInventory) ReadEquipDirect(L, e, targets, nTargets, ip, nm, cnt, mt);   // O(1) прямое чтение руки
                else                    ReadEquip(L, e, targets, nTargets, ip, nm, cnt, mt);          // фолбэк: BFS по графу
                if (ip) { p.held = nm; p.heldPtr = ip; p.heldCount = cnt; if (!nm.empty()) p.items.push_back({ nm, cnt }); }
                if (mt) {                                                     // цель = оружие в руке
                    p.isTarget = true; p.heldPtr = mt;
                    std::wstring wn; if (ReadJavaString(L, ReadOop(mt, L.itemUnlocalizedName), wn)) p.held = wn;
                }
            }
            out.push_back(std::move(p));
        }
        return out;
    }

private:
    Layout L_{}; uintptr_t sword_ = 0, bow_ = 0; int readMs_ = 100, rescanMs_ = 1500;
    std::atomic<bool> run_{ false }; std::thread th_;
    std::mutex mtx_; std::vector<PlayerInfo> latest_;
    mutable std::mutex amtx_; std::vector<uintptr_t> addrs_;   // кэш адресов игроков (обновляется редко)
    ScanNarrow narrow_; ULONGLONG lastRescan_ = 0;

    void Loop() {
        uint32_t cls[4]; int nn = 0; for (int i = 0; i < L_.nPlayers && i < 4; ++i) cls[nn++] = L_.nkPlayers[i];
        const uintptr_t targets[2] = { sword_, bow_ };
        while (run_.load(std::memory_order_relaxed)) {
            const ULONGLONG now = GetTickCount64();
            std::vector<uintptr_t> a;
            if (L_.worldPlayerList) {
                a = PlayersFromWorldList(L_);                                  // живой список игроков из мира: полный, только живые, real-time (без скана кучи)
            } else {                                                          // фолбэк: скан кучи (если playerEntities не найден)
                if (addrs_.empty() || now - lastRescan_ > (ULONGLONG)rescanMs_) {
                    std::vector<uintptr_t> s = ScanInstancesSmart(cls, nn, narrow_, 1);
                    { std::lock_guard<std::mutex> lk(amtx_); addrs_.swap(s); }
                    lastRescan_ = now;
                }
                { std::lock_guard<std::mutex> lk(amtx_); a = addrs_; }
            }
            std::vector<PlayerInfo> out = Collect(L_, a, targets, 2);          // чтение позиции/руки по адресам
            { std::lock_guard<std::mutex> lk(mtx_); latest_.swap(out); }
            const ULONGLONG dt = GetTickCount64() - now;
            if ((int)dt < readMs_) Sleep((DWORD)(readMs_ - (int)dt));
        }
    }
};

} // namespace mc
