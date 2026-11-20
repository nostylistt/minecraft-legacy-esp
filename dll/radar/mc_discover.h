// mc_discover.h — динамический поиск всех классов и смещений Minecraft ПО СТРУКТУРНЫМ ПРИЗНАКАМ.
// Ничего минкрафт-специфичного не захардкожено: используются только ABI JVM (narrow klass в объект+8,
// Klass::layout_helper в klass+8, Klass::_java_mirror в klass+192, oop shift 3, раскладка массивов
// длина+12/данные+16) и стабильная раскладка JDK (String.value+12, ArrayList.size+16/elementData+20).
// Алгоритм проверен: Python-порт этой же логики на сыром дампе восстановил все 15 значений точь-в-точь
// (nkEntityItem, nkItemStack, ItemStack.item, Item.unlocalizedName, dataWatcher, цепочку DataWatcher,
//  WatchableObject.*, theWorld, loadedEntityList, posY, синглтон Minecraft).
//
// Дискавери — одноразовый (несколько проходов по куче, ~секунды), результат кэшируется в Layout.
// Дальше радар только разыменовывает указатели по найденным смещениям.
//
// Зависит от mc_dropped_items.h (SafeRead, ReadOop, NarrowKlassOf, IsHeapAddr, ReadJavaString, Layout).
#pragma once
#include "mc_dropped_items.h"
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <cwctype>

namespace mc {

// ------------------------------------------------------------------ ABI-помощники (стабильны для сборки JVM)
constexpr int      KL_LAYOUT_HELPER = 8, KL_MIRROR = 192;
constexpr uint32_t NK_LO = (uint32_t)(CLASS_LO >> OOP_SHIFT), NK_HI = (uint32_t)(CLASS_HI >> OOP_SHIFT);
constexpr uint32_t OOP_NLO = (uint32_t)(HEAP_LO >> OOP_SHIFT), OOP_NHI = (uint32_t)(HEAP_HI >> OOP_SHIFT);

inline int32_t  LayoutHelper(uint32_t nk) { int32_t v = 0; return SafeRead(KlassAddr(nk) + KL_LAYOUT_HELPER, v) ? v : 0; }   // klass-декод через рантайм-кодек (A: nk<<3; B: base+nk)
inline uint32_t SizeOf(uint32_t nk)       { int32_t l = LayoutHelper(nk); return l > 0 ? (uint32_t)(l & ~7) : 0; }        // 0 = массив/абстрактный
inline int      ArrTypeByte(uint32_t nk)  { int32_t l = LayoutHelper(nk); return l < 0 ? ((l >> 8) & 0xff) : -1; }        // BasicType: T_CHAR=5, T_OBJECT=12
inline bool     IsObjArray(uint32_t nk)   { return nk && ArrTypeByte(nk) == 12; }
inline bool     IsCharArray(uint32_t nk)  { return nk && ArrTypeByte(nk) == 5; }
inline bool     IsByteArray(uint32_t nk)  { return nk && ArrTypeByte(nk) == 8; }        // Java 9+ compact strings: String.value = byte[]
// Правдоподобный layout_helper: положительный размер instance (16..256К, 8-выровнен) ИЛИ валидный массив (etype 4..13, hsize 12/16/20/24).
inline bool     KlassLhSane(uint32_t nk) {
    int32_t lh = LayoutHelper(nk);
    if (lh > 0) return (lh & 7) == 0 && lh >= 16 && lh <= 262144;
    const int et = (lh >> 8) & 0xff, hs = (lh >> 16) & 0xff, l2 = lh & 0xff;
    return et >= 4 && et <= 13 && (hs == 12 || hs == 16 || hs == 20 || hs == 24) && l2 <= 3;
}
inline size_t   ObjBytes(uint32_t nk, uintptr_t o) {
    int32_t l = LayoutHelper(nk);
    if (l > 0) return (size_t)(l & ~7);
    uint32_t len = 0; SafeRead(o + 12, len);
    const int esz = (ArrTypeByte(nk) == 12) ? 4 : (ArrTypeByte(nk) == 5 ? 2 : 4);
    return 16 + (size_t)len * esz;
}

// ------------------------------------------------------------------ индекс объектов кучи (один проход)
// Адреса и narrow klass всех объектов, в порядке возрастания адреса (VirtualQuery идёт снизу вверх).
struct ObjectIndex {
    std::vector<uintptr_t> addr;
    std::vector<uint32_t>  nk;
    void Build() {
        if (Codec().validateLh) { BuildWalk(); return; }   // клиент B: узкий klass-диапазон совпадает с обычными int-данными -> обход по размерам (tiling)
        // клиент A: быстрый скан по диапазону (narrow klass 0xF8000000+ не пересекается с данными кучи)
        std::vector<uint8_t> buf(8u << 20);
        MEMORY_BASIC_INFORMATION mbi{};
        addr.reserve(1u << 24); nk.reserve(1u << 24);
        for (uintptr_t p = HEAP_LO; p < HEAP_HI; ) {
            if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) break;
            const uintptr_t base = (uintptr_t)mbi.BaseAddress, next = base + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                const uintptr_t lo = base < HEAP_LO ? HEAP_LO : base, hi = next > HEAP_HI ? HEAP_HI : next;
                for (uintptr_t c = lo; c < hi; c += buf.size()) {
                    const size_t n = (size_t)((hi - c < buf.size()) ? hi - c : buf.size());
                    if (!detail::SafeReadRaw(c, buf.data(), n)) continue;
                    for (size_t i = 0; i + 12 <= n; i += 8) {
                        uint32_t k; std::memcpy(&k, buf.data() + i + 8, 4);
                        if (k >= Codec().nkLo && k < Codec().nkHi) { addr.push_back(c + i); nk.push_back(k); }
                    }
                }
            }
            p = next > p ? next : p + 0x1000;
        }
    }

    // Клиент B: обход кучи ПО РАЗМЕРАМ объектов (layout_helper) — каждый записанный объект настоящий (замощение),
    // без ложных срабатываний от мелких int-значений, попадающих в низкий narrow-klass диапазон. Буфер + скольжение окна.
    void BuildWalk() {
        addr.reserve(1u << 23); nk.reserve(1u << 23);
        std::unordered_map<uint32_t, int32_t> lhc; lhc.reserve(1u << 16);
        auto lhOf = [&](uint32_t k) -> int32_t { auto it = lhc.find(k); if (it != lhc.end()) return it->second; int32_t v = LayoutHelper(k); lhc.emplace(k, v); return v; };
        std::vector<uint8_t> buf(16u << 20);
        const uint32_t nkLo = Codec().nkLo, nkHi = Codec().nkHi;
        MEMORY_BASIC_INFORMATION mbi{};
        for (uintptr_t p = HEAP_LO; p < HEAP_HI; ) {
            if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) break;
            const uintptr_t rbase = (uintptr_t)mbi.BaseAddress, rnext = rbase + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                const uintptr_t rlo = rbase < HEAP_LO ? HEAP_LO : rbase, rhi = rnext > HEAP_HI ? HEAP_HI : rnext;
                uintptr_t o = (rlo + 7) & ~(uintptr_t)7;
                while (o + 16 <= rhi) {
                    const size_t want = (size_t)((rhi - o < buf.size()) ? rhi - o : buf.size());
                    if (!detail::SafeReadRaw(o, buf.data(), want)) { o += 0x1000; continue; }   // нечитаемая страница -> шаг
                    size_t i = 0;
                    while (i + 16 <= want) {
                        uint32_t k; std::memcpy(&k, buf.data() + i + 8, 4);
                        if (!k || (k & 7) || k < nkLo || k >= nkHi) { i += 8; continue; }
                        const int32_t lh = lhOf(k);
                        uintptr_t sz;
                        if (lh > 0) { if ((lh & 7) || lh < 16 || lh > 262144) { i += 8; continue; } sz = (uint32_t)lh; }
                        else {
                            const int et = (lh >> 8) & 0xff, hs = (lh >> 16) & 0xff, l2 = lh & 0xff;
                            if (et < 4 || et > 13 || !(hs == 12 || hs == 16 || hs == 20 || hs == 24) || l2 > 3) { i += 8; continue; }
                            uint32_t len; std::memcpy(&len, buf.data() + i + 12, 4);
                            if (len > 50000000u) { i += 8; continue; }
                            sz = (uintptr_t)hs + ((uintptr_t)len << l2);
                        }
                        sz = (sz + 7) & ~(uintptr_t)7;
                        if (sz < 16 || sz > 16000000u) { i += 8; continue; }
                        if (i + sz > want) {                                   // объект выходит за окно
                            if (want == buf.size() && sz <= buf.size()) break; // полный буфер -> перечитать с o+i
                            addr.push_back(o + i); nk.push_back(k); i += sz; break;   // хвост региона / объект>буфера -> записать и перешагнуть
                        }
                        addr.push_back(o + i); nk.push_back(k); i += sz;
                    }
                    o += (i ? i : want);                                       // прогресс окна
                }
            }
            p = rnext > p ? rnext : p + 0x1000;
        }
    }
    size_t ObjAt(uintptr_t va) const {                       // индекс объекта, содержащего адрес va (или SIZE_MAX)
        if (addr.empty() || va < addr[0]) return SIZE_MAX;
        size_t i = (size_t)(std::upper_bound(addr.begin(), addr.end(), va) - addr.begin()) - 1;
        return i;
    }
    std::vector<uintptr_t> Instances(uint32_t k, size_t limit = SIZE_MAX) const {
        std::vector<uintptr_t> v;
        for (size_t i = 0; i < nk.size() && v.size() < limit; ++i) if (nk[i] == k) v.push_back(addr[i]);
        return v;
    }
    uint32_t Count(uint32_t k) const { uint32_t c = 0; for (uint32_t x : nk) if (x == k) ++c; return c; }
    std::unordered_map<uint32_t, uint32_t> Histogram() const {
        std::unordered_map<uint32_t, uint32_t> h; h.reserve(8192);
        for (uint32_t x : nk) ++h[x];
        return h;
    }
};

// референсы: все (объект-источник, смещение, цель) в куче, где лежит narrow oop одной из целей.
struct Ref { uintptr_t obj; int off; uint32_t nk; uintptr_t tgt; };
inline void ScanReferrers(const ObjectIndex& idx, const std::vector<uintptr_t>& targets, std::vector<Ref>& out) {
    std::unordered_set<uint32_t> want; want.reserve(targets.size() * 2);
    for (uintptr_t t : targets) want.insert((uint32_t)(t >> OOP_SHIFT));
    std::vector<uint8_t> buf(8u << 20);
    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t p = HEAP_LO; p < HEAP_HI; ) {
        if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) break;
        const uintptr_t base = (uintptr_t)mbi.BaseAddress, next = base + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            const uintptr_t lo = base < HEAP_LO ? HEAP_LO : base, hi = next > HEAP_HI ? HEAP_HI : next;
            for (uintptr_t c = lo; c < hi; c += buf.size()) {
                const size_t n = (size_t)((hi - c < buf.size()) ? hi - c : buf.size());
                if (!detail::SafeReadRaw(c, buf.data(), n)) continue;
                for (size_t i = 0; i + 4 <= n; i += 4) {
                    uint32_t v; std::memcpy(&v, buf.data() + i, 4);
                    if (v < OOP_NLO || v >= OOP_NHI || !want.count(v)) continue;
                    const uintptr_t wa = c + i;
                    const size_t oi = idx.ObjAt(wa);
                    if (oi == SIZE_MAX) continue;
                    const uintptr_t ostart = idx.addr[oi]; const uint32_t onk = idx.nk[oi];
                    const int off = (int)(wa - ostart);
                    if (off >= 12 && (size_t)off < ObjBytes(onk, ostart))
                        out.push_back({ ostart, off, onk, (uintptr_t)v << OOP_SHIFT });
                }
            }
        }
        p = next > p ? next : p + 0x1000;
    }
}

// ------------------------------------------------------------------ мелкие утилиты дискавери
inline uintptr_t Oop(uintptr_t o, int off) { return ReadOop(o, off); }
inline bool IsIdent(const std::wstring& s) {
    if (s.size() < 2 || s.size() > 48) return false;
    for (wchar_t c : s) if (!(iswalnum(c) || c == L'_' || c == L'.')) return false;
    return true;
}
// строгий ник Minecraft (для поиска смещения ника у локального игрока — у него ник всегда реальный)
inline bool IsUserName(const std::wstring& s) {
    if (s.size() < 3 || s.size() > 16) return false;
    for (wchar_t c : s) if (!(iswalnum(c) || c == L'_')) return false;
    return true;
}
// ник ИЛИ временный хендл VimeWorld '-XXXX' (для распознавания классов чужих игроков, пока профиль не подгрузился)
inline bool IsPlayerName(const std::wstring& s) {
    if (s.size() < 3 || s.size() > 16) return false;
    for (wchar_t c : s) if (!(iswalnum(c) || c == L'_' || c == L'-')) return false;
    return true;
}

// группировка референсов по (klass, offset)
struct GroupKey { uint32_t nk; int off; bool operator==(const GroupKey& o) const { return nk == o.nk && off == o.off; } };
struct GroupKeyHash { size_t operator()(const GroupKey& k) const { return (size_t)k.nk * 131 + k.off; } };

// Вывод posX/Y/Z по bounding box ОДНОЙ сущности: поле -> AABB (>=6 double, 3 пары min<max). posX/posZ = центр
// горизонтальных пар, posY = нижняя грань. Смещения совпавших полей сущности и есть posX/Y/Z. Используется и в
// дискавери, и в рантайме (самолечение — как только появился предмет, координаты выставляются сразу).
inline bool DeriveCoords(Layout& L, uintptr_t e) {
    const int esz = (int)SizeOf(NarrowKlassOf(e));
    if (esz <= 0 || esz > 4096) return false;
    for (int off = 12; off + 3 < esz; off += 4) {
        const uintptr_t bb = ReadOop(e, off);
        const int bsz = bb ? (int)SizeOf(NarrowKlassOf(bb)) : 0;
        if (bsz < 32 || bsz > 96) continue;
        double bd[24]; int nb = 0;
        for (int o = 12; o + 7 < bsz && nb < 24; o += 4) { double v = 0; if (SafeRead(bb + o, v) && v == v && fabs(v) < 3e7) bd[nb++] = v; }
        if (nb < 6) continue;
        int cy = 0; std::vector<int> cxz;
        for (int i = 0; i < nb; ++i) for (int j = 0; j < nb; ++j) {
            if (i == j) continue; const double a = bd[i], b = bd[j];
            if (!(a < b && b - a > 0.05 && b - a < 8.0)) continue;
            const double c = (a + b) * 0.5;
            for (int eo = 16; eo + 7 < esz; eo += 8) { double ev = 0; if (!SafeRead(e + eo, ev) || ev != ev) continue;
                if (fabs(ev - c) < 0.06) { bool dup = false; for (int q : cxz) if (q == eo) dup = true; if (!dup) cxz.push_back(eo); break; }
                if (fabs(ev - a) < 0.06 && ev >= -64.0 && ev <= 320.0) { cy = eo; break; }
            }
        }
        for (size_t k = 0; k < cxz.size(); ++k) if (cxz[k] == cy) { cxz.erase(cxz.begin() + k); --k; }
        if (cy && cxz.size() >= 2) {   // в Entity поля идут posX<posY<posZ -> меньший горизонтальный оффсет = X (иначе X/Z путаются)
            const int a = cxz[0], b = cxz[1];
            L.entPosY = cy; L.entPosX = (a < b ? a : b); L.entPosZ = (a < b ? b : a);
            return true;
        }
    }
    return false;
}

// Найти список игроков мира (playerEntities/loadedEntityList): java.util.ArrayList на World, чей elementData
// СОДЕРЖИТ локального игрока. Пишет L.worldPlayerList (offset World->ArrayList). true, если найдено.
// Это источник правды самой игры: полный, только ЖИВЫЕ сущности текущего мира — без скана кучи и гонки с GC.
inline bool DiscoverPlayerList(Layout& L, uintptr_t world, uintptr_t localPlayer) {
    if (!world || !localPlayer || !L.nkArrayList) return false;
    const int wsz = (int)SizeOf(NarrowKlassOf(world));
    if (wsz <= 0 || wsz > 16384) return false;
    for (int off = 12; off + 3 < wsz; off += 4) {
        const uintptr_t lst = ReadOop(world, off);
        if (!IsHeapAddr(lst) || NarrowKlassOf(lst) != L.nkArrayList) continue;   // именно java.util.ArrayList
        const uintptr_t arr = ReadOop(lst, L.alElementData);
        if (!IsHeapAddr(arr) || !IsObjArray(NarrowKlassOf(arr))) continue;
        int32_t sz = 0; if (!SafeRead(lst + L.alSize, sz) || sz <= 0 || sz > 100000) continue;
        uint32_t len = 0; SafeRead(arr + L.arrLength, len);
        uint32_t n = ((uint32_t)sz < len ? (uint32_t)sz : len); if (n > 8192) n = 8192;
        for (uint32_t i = 0; i < n; ++i)
            if (ReadOop(arr, L.arrData + 4 * i) == localPlayer) { L.worldPlayerList = off; return true; }
    }
    return false;
}

// Найти EntityPlayer.inventory (InventoryPlayer) + InventoryPlayer.mainInventory (ItemStack[36]) на локальном игроке.
// Сигнатура InventoryPlayer сильная: в одном объекте есть массив на 36 (main) И массив на 4 (armor), оба из ItemStack/null.
// Пишет L.playerInventory (player->inv) и L.invMainInventory (inv->main[36]). Для O(1) прямого чтения руки вместо BFS.
inline bool DiscoverInventory(Layout& L, uintptr_t player) {
    if (!player || !L.nkItemStack) return false;
    const int psz = (int)SizeOf(NarrowKlassOf(player));
    if (psz <= 0 || psz > 16384) return false;
    auto allStacks = [&](uintptr_t arr, uint32_t len) -> bool {   // все непустые элементы — ItemStack
        for (uint32_t i = 0; i < len; ++i) { const uintptr_t s = ReadOop(arr, L.arrData + 4 * i); if (s && NarrowKlassOf(s) != L.nkItemStack) return false; }
        return true;
    };
    for (int po = 12; po + 3 < psz; po += 4) {
        const uintptr_t inv = ReadOop(player, po);
        if (!IsHeapAddr(inv)) continue;
        const int isz = (int)SizeOf(NarrowKlassOf(inv));
        if (isz <= 0 || isz > 2048) continue;
        int mainOff = -1; bool haveArmor = false;
        for (int io = 12; io + 3 < isz; io += 4) {
            const uintptr_t arr = ReadOop(inv, io);
            if (!IsHeapAddr(arr) || !IsObjArray(NarrowKlassOf(arr))) continue;
            uint32_t len = 0; if (!SafeRead(arr + L.arrLength, len)) continue;
            if (len == 36 && mainOff < 0 && allStacks(arr, 36)) mainOff = io;
            else if (len == 4 && allStacks(arr, 4)) haveArmor = true;
        }
        if (mainOff >= 0 && haveArmor) { L.playerInventory = po; L.invMainInventory = mainOff; return true; }
    }
    return false;
}

// Прямой BFS от сущности до объекта класса L.nkItemStack (обход oop-полей и элементов object-массивов,
// глубина <=6, не заходит в крупные объекты >=300Б). Возвращает путь смещений в out[0..*outN-1]. true если найден.
// Настоящий EntityItem: путь длины 5 вида [dataWatcher, +16/+20, +12, +56 (индекс слота 10), +woValue].
inline bool BfsPathToItemStack(const Layout& L, uintptr_t root, int out[8], int& outN) {
    if (!L.nkItemStack) return false;
    std::vector<uintptr_t> fr{ root };
    std::unordered_map<uintptr_t, std::pair<uintptr_t, int>> par; par[root] = { 0, -1 };
    std::unordered_set<uintptr_t> seen; seen.insert(root);
    int visited = 0;
    for (int depth = 0; depth < 6 && visited < 5000; ++depth) {
        std::vector<uintptr_t> nx;
        for (uintptr_t o : fr) {
            if (++visited > 5000) break;
            const uint32_t nk = NarrowKlassOf(o); int lo, hi;
            if (LayoutHelper(nk) < 0) { if (!IsObjArray(nk)) continue; uint32_t len = 0; SafeRead(o + 12, len); if (len > 64) len = 64; lo = 16; hi = 16 + (int)len * 4; }
            else { int sz = (int)SizeOf(nk); if (sz <= 0 || sz > 4096) sz = 64; lo = 12; hi = sz; }
            for (int off = lo; off + 3 < hi; off += 4) {
                const uintptr_t t = ReadOop(o, off);
                if (!t || !seen.insert(t).second) continue;
                par[t] = { o, off };
                if (NarrowKlassOf(t) == L.nkItemStack) {
                    int tmp[16], n = 0; uintptr_t cur = t;
                    while (par.count(cur) && par[cur].first && n < 16) { tmp[n++] = par[cur].second; cur = par[cur].first; }
                    outN = 0; for (int i = n - 1; i >= 0 && outN < 8; --i) out[outN++] = tmp[i];
                    return true;
                }
                if (SizeOf(NarrowKlassOf(t)) >= 300) continue;
                nx.push_back(t);
            }
        }
        fr.swap(nx);
    }
    return false;
}

// ------------------------------------------------------------------ главный дискавери
inline bool DiscoverLayout(Layout& L, const ObjectIndex& idx) {
    L.strValue = 12; L.charArrLength = 12; L.charArrData = 16; L.alSize = 16; L.alElementData = 20; L.arrLength = 12; L.arrData = 16;
    auto H = idx.Histogram();
    auto keys = [&] { std::vector<uint32_t> v; for (auto& kv : H) v.push_back(kv.first); return v; }();

    // --- 1. JDK-классы ---
    // Массив-подложка String: char[] (клиент A, UTF-16) ИЛИ byte[] (клиент B, Java 9+ compact strings) — по автодетекту.
    uint32_t nkStrArr = 0, nkString = 0, nkArrayList = 0, nkClass = 0;
    const bool compact = Codec().compactStr;
    for (uint32_t k : keys) {
        const bool isBack = compact ? IsByteArray(k) : IsCharArray(k);
        if (isBack && (!nkStrArr || H[k] > H[nkStrArr])) nkStrArr = k;
    }
    if (!nkStrArr) return false;
    if (compact) {
        // Java 9+: oop-поле value лежит НЕ на +12 (примитивы hash/coder идут первыми) -> ищем (класс, оффсет value)
        // совместно: instance-класс, у которого поле на некотором смещении у большинства экземпляров ведёт в byte[]-подложку.
        uint32_t bestK = 0; int bestOff = 12; double bestSc = 0.0;
        for (uint32_t k : keys) {
            const int ksz = (int)SizeOf(k);
            if (ksz < 16 || ksz > 40 || H[k] < 10000) continue;
            auto s = idx.Instances(k, 60); if (s.size() < 12) continue;
            for (int off = 12; off + 3 < ksz; off += 4) {
                int ok = 0; for (uintptr_t o : s) if (NarrowKlassOf(Oop(o, off)) == nkStrArr) ++ok;
                const double sc = (double)ok / s.size();
                if (sc > bestSc) { bestSc = sc; bestK = k; bestOff = off; }
            }
        }
        if (bestSc < 0.8 || !bestK) return false;
        nkString = bestK; L.strValue = bestOff;             // strCoderOff остаётся 0 -> Latin1 (ники/имена предметов ASCII)
    } else {
        auto strScore = [&](uint32_t k) {
            auto s = idx.Instances(k, 40); if (s.empty()) return 0.0;
            int ok = 0; for (uintptr_t o : s) if (NarrowKlassOf(Oop(o, 12)) == nkStrArr) ++ok;
            return (double)ok / s.size();
        };
        for (uint32_t k : keys) if (SizeOf(k) >= 16 && SizeOf(k) <= 32 && H[k] > 50000 && (!nkString || strScore(k) > strScore(nkString))) nkString = k;
        if (!nkString || strScore(nkString) < 0.8) return false;
    }
    L.nkString = nkString;
    auto alScore = [&](uint32_t k) {
        auto s = idx.Instances(k, 80); if (s.empty()) return 0.0;
        int ok = 0;
        for (uintptr_t o : s) {
            uintptr_t arr = Oop(o, 20); uint32_t szraw = 0; int32_t sz = 0; SafeRead(o + 16, szraw); SafeRead(o + 16, sz);
            uint32_t cap = 0; if (arr) SafeRead(arr + 12, cap);
            if (arr && IsObjArray(NarrowKlassOf(arr)) && !(szraw >= OOP_NLO && szraw < OOP_NHI) && sz >= 0 && (uint32_t)sz <= cap) ++ok;
        }
        return (double)ok / s.size();
    };
    for (uint32_t k : keys) if (SizeOf(k) >= 24 && SizeOf(k) <= 32 && H[k] > 10000 && alScore(k) > 0.9 && (!nkArrayList || H[k] > H[nkArrayList])) nkArrayList = k;
    if (!nkArrayList) return false;
    { uint64_t mir = 0; if (SafeRead(KlassAddr(nkString) + KL_MIRROR, mir)) nkClass = NarrowKlassOf((uintptr_t)mir); }
    L.nkClass = nkClass ? nkClass : L.nkClass;

    auto jstr = [&](uintptr_t o) -> std::wstring { std::wstring s; ReadJavaString(L, o, s, 48); return s; };

    // --- 4. реестр Item -> ITEM_KS + Item.unlocalizedName (список с разнообразными классами элементов) ---
    struct RegCand { int distinct, unloc; std::unordered_set<uint32_t> ks; uintptr_t list; };
    std::vector<RegCand> regs;
    for (uintptr_t o : idx.Instances(nkArrayList)) {
        int32_t sz = 0; if (!SafeRead(o + 16, sz) || sz < 1500 || sz > 6000) continue;
        uintptr_t arr = Oop(o, 20); if (!arr) continue;
        std::vector<uintptr_t> nn; std::unordered_set<uint32_t> ks;
        for (int i = 0; i < sz && i < 500; ++i) { uintptr_t e = Oop(arr, 16 + 4 * i); if (e) { nn.push_back(e); ks.insert(NarrowKlassOf(e)); } }
        ks.erase(0);
        if (nn.size() < 50 || ks.size() < 5) continue;
        for (int u = 12; u < 80; u += 4) {
            int hit = 0; for (uintptr_t e : nn) if (IsIdent(jstr(Oop(e, u)))) ++hit;
            if (hit >= (int)(0.6 * nn.size())) { regs.push_back({ (int)ks.size(), u, ks, o }); break; }
        }
    }
    std::sort(regs.begin(), regs.end(), [](const RegCand& a, const RegCand& b) { return a.distinct > b.distinct; });

    // --- 3. ItemStack: 40..64Б, поле на 90% ведёт в ITEM_KS (проверка отсекает реестр блоков) ---
    uint32_t nkItemStack = 0; int stackItem = 0, itemUnloc = 0; std::unordered_set<uint32_t> ITEM_KS; uintptr_t itemReg = 0;
    for (auto& rc : regs) {
        uint32_t bestNk = 0; int bestOff = 0; uint32_t bestCnt = 0;
        for (uint32_t k : keys) {
            if (SizeOf(k) < 40 || SizeOf(k) > 64 || H[k] < 1000) continue;
            auto s = idx.Instances(k, 200); if (s.empty()) continue;
            for (int off = 12; off + 3 < (int)SizeOf(k); off += 4) {
                int hit = 0; for (uintptr_t o : s) if (rc.ks.count(NarrowKlassOf(Oop(o, off)))) ++hit;
                if (hit >= (int)(0.9 * s.size())) { if (!bestNk || H[k] > bestCnt) { bestNk = k; bestOff = off; bestCnt = H[k]; } break; }
            }
        }
        if (bestNk) { nkItemStack = bestNk; stackItem = bestOff; itemUnloc = rc.unloc; ITEM_KS = rc.ks; itemReg = rc.list; break; }
    }
    if (!nkItemStack) return false;
    L.nkItemStack = nkItemStack; L.stackItem = stackItem; L.itemUnlocalizedName = itemUnloc; L.itemRegistry = itemReg;
    // stackSize / itemDamage по статистике int-полей
    auto stacks = idx.Instances(nkItemStack);
    {
        std::vector<uintptr_t> smp(stacks.begin(), stacks.begin() + std::min<size_t>(stacks.size(), 4000));
        int bestSz = 20; long bestScore = -1; int dmg = 0;
        for (int off = 12; off + 3 < (int)SizeOf(nkItemStack); off += 4) {
            long in164 = 0; bool has32767 = false;
            for (uintptr_t o : smp) { int32_t v = 0; SafeRead(o + off, v); if (v >= 1 && v <= 64) ++in164; if (v == 32767) has32767 = true; }
            if (has32767) dmg = off;
            long score = in164 - (has32767 ? 1000000000L : 0);
            if (score > bestScore) { bestScore = score; bestSz = off; }
        }
        L.stackSize = bestSz; L.stackDamage = dmg ? dmg : L.stackDamage;
    }

    // --- 4. EntityItem + цепочка: ПРЯМОЙ BFS от сущности до ItemStack; настоящий EntityItem — путь длины 5,
    //         проходящий через индекс массива +56 (= слот 10 DataWatcher). Детерминированно, без обратных обходов кучи.
    //         БЕЗ верхнего лимита экземпляров: на людной карте EntityItem-ов тысячи (именно тогда радар и нужен).
    //         Из всех совпавших по сигнатуре берём САМЫЙ населённый класс (страхует от случайного одиночного совпадения). ---
    uintptr_t refEI = 0;   // один заведомо настоящий EntityItem (прошёл слот-10 BFS) — для сверки смещения worldObj
    {
        uint32_t bestNk = 0, bestCnt = 0; int bDw = 0, bDws = 0, bWra = 0, bWo = 0; uintptr_t bE = 0;
        for (uint32_t k : keys) {
            if (SizeOf(k) < 300 || SizeOf(k) > 700 || H[k] <= bestCnt) continue;   // тестируем только более населённых кандидатов
            for (uintptr_t e : idx.Instances(k, 6)) {
                if (IsForwarded(e)) continue;
                int path[8], np = 0;
                if (BfsPathToItemStack(L, e, path, np) && np == 5 && path[3] == 16 + 4 * Layout::ITEM_SLOT) {
                    bestNk = k; bestCnt = H[k]; bDw = path[0]; bDws = path[1]; bWra = path[2]; bWo = path[4]; bE = e;
                }
                break;   // достаточно первого живого экземпляра класса
            }
        }
        if (bestNk) { L.nkEntityItem = bestNk; L.entDataWatcher = bDw; L.dwSlots = bDws; L.wrapArray = bWra; L.woValue = bWo; refEI = bE; }
    }
    // nkEntityItem НЕ обязателен: если на карте нет лежащих предметов — идём дальше. Неймтеги/маньяк/дистанция работают
    // без него (координаты берём с локального игрока ниже). Item ESP включится, когда предметы появятся + Rescan.

    // --- 5. Minecraft + theWorld + worldObj + класс мира ОДНОЙ сигнатурой (без голосования по мусорным EntityItem,
    //         которых на людной карте тысячи и первые по адресу могут быть мёртвыми): единственный экземпляр M
    //         (ninst==1, 300..2000Б) с полем woff -> W (объект-мир 150..600Б) и полем poff -> P (сущность >=500Б,
    //         thePlayer), у которого ЕСТЬ поле wo с P.[wo]==W, ПРИЧЁМ то же смещение wo у настоящего EntityItem ведёт
    //         в объект того же класса (worldObj — поле базового Entity, общее для игрока и предмета). Итог: minecraft=M,
    //         theWorld=W(@woff), worldObj=wo, nkWorld=class(W). Это groundtruth-мир игрока, не зависит от состояния EntityItem. ---
    for (uint32_t k : keys) {
        if (H[k] > 3 || SizeOf(k) < 300 || SizeOf(k) > 2000) continue;
        auto in1 = idx.Instances(k, 1); if (in1.empty()) continue;
        const uintptr_t M = in1[0]; const int msz = (int)SizeOf(k);
        for (int woff = 12; woff + 3 < msz && !L.minecraft; woff += 4) {
            const uintptr_t W = ReadOop(M, woff);
            const uint32_t wnk = W ? NarrowKlassOf(W) : 0; const int wsz = (int)SizeOf(wnk);
            if (wsz < 150 || wsz > 600) continue;                        // кандидат в theWorld
            for (int poff = 12; poff + 3 < msz && !L.minecraft; poff += 4) {
                const uintptr_t P = ReadOop(M, poff);
                if (!P || (int)SizeOf(NarrowKlassOf(P)) < 500) continue;  // кандидат в thePlayer (сущность)
                const int psz = (int)SizeOf(NarrowKlassOf(P));
                for (int wo = 12; wo + 3 < psz; wo += 4) {
                    if (ReadOop(P, wo) != W) continue;                    // P.[wo] -> W
                    if (refEI && NarrowKlassOf(ReadOop(refEI, wo)) != wnk) continue;   // то же смещение у EntityItem -> мир того же класса
                    L.minecraft = M; L.mcTheWorld = woff; L.mcThePlayer = poff; L.nkMinecraft = k; L.entWorldObj = wo; L.nkWorld = wnk; break;
                }
            }
        }
        if (L.minecraft) break;
    }
    if (!L.minecraft || !L.entWorldObj) return false;
    L.nkArrayList = nkArrayList;

    // Стабильный Klass(Minecraft) + слот статика theMinecraft — чтобы CurrentMinecraft переживал переезд синглтона GC.
    L.klassMinecraft = KlassAddr(L.nkMinecraft); L.mcStaticSlot = 0;   // Klass* через кодек (A: nk<<3; B: base+nk)
    { uint64_t mir = 0;
      if (SafeRead(L.klassMinecraft + KL_MIRROR, mir) && IsHeapAddr((uintptr_t)mir))
          for (int off = L.classStatics; off < L.classStatics + 4 * 160; off += 4) {
              const uintptr_t o = ReadOop((uintptr_t)mir, off);
              if (o && NarrowKlassOf(o) == L.nkMinecraft) { L.mcStaticSlot = off; break; }
          }
    }

    // --- 6b. игроки (для радара игроков): смещение ника + классы игроков в мире ---
    {
        const uintptr_t thePlayer = ReadOop(L.minecraft, L.mcThePlayer);
        const uintptr_t theWorld  = ReadOop(L.minecraft, L.mcTheWorld);
        if (thePlayer) {
            // entName = поле локального игрока -> String с реальным ником (базовое поле, одинаково у всех классов игроков)
            const int psz = (int)SizeOf(NarrowKlassOf(thePlayer));
            for (int off = 12; off + 3 < psz; off += 4) {
                const uintptr_t s = ReadOop(thePlayer, off);
                if (s && NarrowKlassOf(s) == L.nkString) { std::wstring nm; ReadJavaString(L, s, nm, 24); if (IsUserName(nm)) { L.entName = off; break; } }
            }
            L.nkPlayers[L.nPlayers++] = NarrowKlassOf(thePlayer);   // локальный игрок — первый класс
        }
        // классы чужих игроков: класс размера 700..1300, у БОЛЬШИНСТВА экземпляров которого на entName лежит
        // строка-ник/хендл (мир не важен — экземпляры раскиданы по загруженным мирам; фильтр мира — уже в BuildPlayers)
        (void)theWorld;
        if (L.entName)
            for (uint32_t k : keys) {
                if (L.nPlayers >= 4 || SizeOf(k) < 700 || SizeOf(k) > 1300 || (int)SizeOf(k) <= L.entName + 3) continue;
                bool have = false; for (int i = 0; i < L.nPlayers; ++i) if (L.nkPlayers[i] == k) have = true; if (have) continue;
                int hits = 0, tried = 0;
                for (uintptr_t e : idx.Instances(k, 40)) {
                    if (IsForwarded(e)) continue; ++tried;
                    const uintptr_t s = ReadOop(e, L.entName);
                    if (s && NarrowKlassOf(s) == L.nkString) { std::wstring nm; ReadJavaString(L, s, nm, 24); if (IsPlayerName(nm)) ++hits; }
                }
                if (hits >= 1 && hits * 2 >= tried) L.nkPlayers[L.nPlayers++] = k;   // ник есть у большинства экземпляров класса
            }
    }

    // --- 7. координаты (AABB): истинные БАЗОВЫЕ смещения Entity.posX/Y/Z (общие для дропов и игроков) даёт только
    //         ПОЗИЦИОНИРОВАННЫЙ дроп — служебные дропы в (0,0,0) дают неоднозначные (ложные) оси, а игрок даёт
    //         player-специфичные (не совпадают с базовыми). Перебираем EntityItem, выводим оси и принимаем ПЕРВЫЙ,
    //         у кого координаты самого дропа вменяемы (не ноль, Y∈[-64,320]) И те же смещения дают вменяемого игрока. ---
    {
        const uintptr_t tp = ReadOop(L.minecraft, L.mcThePlayer);
        auto sane = [&](uintptr_t e, const Layout& t) -> bool {
            double x = 0, y = 0, z = 0; SafeRead(e + t.entPosX, x); SafeRead(e + t.entPosY, y); SafeRead(e + t.entPosZ, z);
            return x == x && z == z && fabs(x) < 3e7 && fabs(z) < 3e7 && y >= -64.0 && y <= 320.0 && !(x == 0.0 && y == 0.0 && z == 0.0);
        };
        int tried = 0; bool got = false;
        for (uintptr_t e : idx.Instances(L.nkEntityItem)) {
            if (++tried > 6000) break;
            uint64_t m = 0; if (SafeRead(e, m) && (m & 3) == 3) continue;
            Layout t = L; if (!DeriveCoords(t, e)) continue;
            if (sane(e, t) && (!tp || sane(tp, t))) { L.entPosX = t.entPosX; L.entPosY = t.entPosY; L.entPosZ = t.entPosZ; got = true; break; }
        }
        // ФОЛБЭК без дропов: якорь на ЛОКАЛЬНОМ игроке (есть всегда/позиционирован). posX/Y/Z наследуются от Entity —
        // смещения базовые, те же, что у дропов. Иначе в лобби/старте (дропов нет) координаты остаются невыведенными.
        if (!got && tp) {
            Layout t = L;
            if (DeriveCoords(t, tp) && sane(tp, t)) { L.entPosX = t.entPosX; L.entPosY = t.entPosY; L.entPosZ = t.entPosZ; got = true; }
        }
    }

    // список игроков мира (playerEntities) — источник правды вместо скана кучи (best-effort; 0 => фолбэк на скан)
    // + путь к инвентарю для O(1) чтения руки (best-effort; 0 => чтение руки через BFS)
    {
        const uintptr_t w = ReadOop(L.minecraft, L.mcTheWorld);
        const uintptr_t lp = ReadOop(L.minecraft, L.mcThePlayer);
        if (w && lp) DiscoverPlayerList(L, w, lp);
        if (lp) DiscoverInventory(L, lp);
    }

    // CORE найден: Minecraft + мир + worldObj + ItemStack + класс дропа. Координаты доуточнятся с живым предметом.
    return L.minecraft && L.nkWorld && L.entWorldObj && L.nkItemStack;
}

// Текущий адрес синглтона Minecraft из СТАБИЛЬНОГО Klass: Klass -> _java_mirror(+192) -> статик theMinecraft.
// Метапространство (Klass) не двигается GC весь запуск; mirror — корень, GC его обновляет; статик всегда указывает
// на живой синглтон. Поэтому переживает и полный GC, и смену карты БЕЗ пересканирования. 0 = не удалось.
// Пытается освежить синглтон через СТАБИЛЬНЫЙ Klass (переживает переезд объекта при GC), НО если на этой
// сборке синглтон не лежит статиком в своём классе (mcStaticSlot не нашёлся) — откатывается на сырой L.minecraft,
// который сам по себе стабилен (объект в old-gen). Никогда не возвращает 0, если синглтон уже найден.
inline uintptr_t CurrentMinecraft(const Layout& L) {
    if (L.klassMinecraft && L.mcStaticSlot) {
        uint64_t mir = 0;
        if (SafeRead(L.klassMinecraft + KL_MIRROR, mir) && IsHeapAddr((uintptr_t)mir)) {
            const uint32_t nk = L.nkMinecraft;             // narrow klass берём из Layout (реверс klassMinecraft>>3 неверен для клиента B)
            const uintptr_t o = ReadOop((uintptr_t)mir, L.mcStaticSlot);
            if (o && NarrowKlassOf(o) == nk) return o;
        }
    }
    return L.minecraft;                                        // фолбэк: сырой адрес синглтона (стабилен в old-gen)
}

// Fallback-вывод базовых Entity.posX/Y/Z по ЛОКАЛЬНОМУ игроку (всегда есть/позиционирован) — без дропов.
// true + смещения, если AABB игрока дал вменяемые оси. Для рантайм-самолечения координат (когда матч рвётся).
inline bool DeriveCoordsFromLocal(const Layout& L, int& ox, int& oy, int& oz) {
    const uintptr_t mc = CurrentMinecraft(L);
    const uintptr_t lp = (mc && L.mcThePlayer) ? ReadOop(mc, L.mcThePlayer) : 0;
    if (!lp) return false;
    Layout t = L;
    if (!DeriveCoords(t, lp)) return false;
    double x = 0, y = 0, z = 0;
    SafeRead(lp + t.entPosX, x); SafeRead(lp + t.entPosY, y); SafeRead(lp + t.entPosZ, z);
    if (!(x == x && z == z && fabs(x) < 3e7 && fabs(z) < 3e7 && y >= -64.0 && y <= 320.0 && !(x == 0.0 && y == 0.0 && z == 0.0)))
        return false;
    ox = t.entPosX; oy = t.entPosY; oz = t.entPosZ; return true;
}

// Живые сущности из списка игроков мира (L.worldPlayerList) — источник правды игры. Без скана кучи.
// Возвращает указатели элементов ArrayList (Collect отфильтрует по классу игрока). Пусто, если список не найден/невалиден.
inline std::vector<uintptr_t> PlayersFromWorldList(const Layout& L) {
    std::vector<uintptr_t> out;
    if (!L.worldPlayerList) return out;
    const uintptr_t mc = CurrentMinecraft(L);
    const uintptr_t world = IsHeapAddr(mc) ? ReadOop(mc, L.mcTheWorld) : 0;
    const uintptr_t lst = world ? ReadOop(world, L.worldPlayerList) : 0;
    if (!IsHeapAddr(lst) || NarrowKlassOf(lst) != L.nkArrayList) return out;     // мир сменился/список переехал -> пусто
    const uintptr_t arr = ReadOop(lst, L.alElementData);
    if (!IsHeapAddr(arr)) return out;
    int32_t sz = 0; SafeRead(lst + L.alSize, sz);
    uint32_t len = 0; SafeRead(arr + L.arrLength, len);
    uint32_t n = (sz > 0 && (uint32_t)sz < len) ? (uint32_t)sz : len; if (n > 8192) n = 8192;
    out.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        const uintptr_t e = ReadOop(arr, L.arrData + 4 * i);
        if (e && IsHeapAddr(e)) out.push_back(e);
    }
    return out;
}

// (B) Скан региона [lo,hi) ПРЯМО в памяти, без копирования: пишет адреса объектов, чей narrow klass ∈ nks[0..nn),
// в dst (ёмкость cap). Только POD-локали + SEH — не требует раскрутки C++ (безопасно с __try). nn мал (<=8).
inline size_t ScanRegionRaw(const uint32_t* nks, int nn, uintptr_t lo, uintptr_t hi, uintptr_t* dst, size_t cap) noexcept {
    size_t n = 0;
    __try {
        for (uintptr_t o = lo; o + 12 <= hi && n < cap; o += 8) {
            const uint32_t k = *(const volatile uint32_t*)(o + 8);
            for (int i = 0; i < nn; ++i) if (k == nks[i]) { dst[n++] = o; break; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}
// добавляет совпадения региона в v (растит ёмкость и пересканивает регион, если не влезло — потерь нет)
inline void AppendRegionMatches(const uint32_t* nks, int nn, uintptr_t lo, uintptr_t hi, std::vector<uintptr_t>& v, size_t& n) {
    for (;;) {
        if (v.size() < n + 4096) v.resize((v.size() + 4096) * 2);
        const size_t capLeft = v.size() - n;
        const size_t got = ScanRegionRaw(nks, nn, lo, hi, v.data() + n, capLeft);
        if (got < capLeft) { n += got; return; }                 // всё поместилось
        v.resize(v.size() * 2);                                  // возможно усекли — растём и пересканиваем регион
    }
}

// (B) Полный проход по куче: адреса всех объектов классов nks[0..nn) (дропы+игроки в один проход). Скан на месте.
inline std::vector<uintptr_t> ScanInstancesSet(const uint32_t* nks, int nn) {
    std::vector<uintptr_t> v; if (nn <= 0) return v; v.resize(1u << 16); size_t n = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t p = HEAP_LO; p < HEAP_HI; ) {
        if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) break;
        const uintptr_t base = (uintptr_t)mbi.BaseAddress, next = base + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            AppendRegionMatches(nks, nn, base < HEAP_LO ? HEAP_LO : base, next > HEAP_HI ? HEAP_HI : next, v, n);
        p = next > p ? next : p + 0x1000;
    }
    v.resize(n); return v;
}
inline std::vector<uintptr_t> ScanInstances(uint32_t nk) { return nk ? ScanInstancesSet(&nk, 1) : std::vector<uintptr_t>{}; }

// (C) Сужение: помним регионы, где в прошлый ПОЛНЫЙ проход были экземпляры. Большинство проходов сканируем
// только их (дёшево), полный проход — раз в fullEvery (и пока горячих нет), он же обновляет список. Корректность
// гарантирует периодический полный проход: объект в новом регионе всплывёт максимум через fullEvery проходов.
struct ScanNarrow { std::vector<std::pair<uintptr_t, uintptr_t>> hot; int pass = 0; };
inline std::vector<uintptr_t> ScanInstancesSmart(const uint32_t* nks, int nn, ScanNarrow& s, int fullEvery = 5) {
    std::vector<uintptr_t> v; if (nn <= 0) return v; v.resize(1u << 16); size_t n = 0;
    const bool full = s.hot.empty() || (s.pass % fullEvery) == 0; ++s.pass;
    MEMORY_BASIC_INFORMATION mbi{};
    if (full) {
        std::vector<std::pair<uintptr_t, uintptr_t>> newHot;
        for (uintptr_t p = HEAP_LO; p < HEAP_HI; ) {
            if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) break;
            const uintptr_t base = (uintptr_t)mbi.BaseAddress, next = base + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                const uintptr_t lo = base < HEAP_LO ? HEAP_LO : base, hi = next > HEAP_HI ? HEAP_HI : next;
                const size_t before = n; AppendRegionMatches(nks, nn, lo, hi, v, n);
                if (n > before) newHot.push_back({ lo, hi });
            }
            p = next > p ? next : p + 0x1000;
        }
        s.hot.swap(newHot);
    } else {
        for (auto& r : s.hot) {                                  // только горячие регионы, с подтверждением коммита
            if (!VirtualQuery((LPCVOID)r.first, &mbi, sizeof(mbi))) continue;
            if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) continue;
            AppendRegionMatches(nks, nn, r.first, r.second, v, n);
        }
    }
    v.resize(n); return v;
}

// Из готового списка экземпляров EntityItem строит дропы: ЖИВЫЕ, worldObj == Minecraft.theWorld, не в (0,0,0).
// Только чтение по указателям (дёшево) — вызывается каждый кадр; сам скан кучи делает фоновый поток.
inline std::vector<DroppedItem> BuildDrops(const Layout& L, const std::vector<uintptr_t>& insts) {
    std::vector<DroppedItem> out;
    if (!L.nkEntityItem || !L.entWorldObj) return out;
    const uintptr_t mc = CurrentMinecraft(L);                        // живой синглтон (переживает переезд GC)
    const uintptr_t world = IsHeapAddr(mc) ? ReadOop(mc, L.mcTheWorld) : 0;
    if (!world) return out;
    for (uintptr_t e : insts) {
        if (IsForwarded(e) || NarrowKlassOf(e) != L.nkEntityItem) continue;
        if (ReadOop(e, L.entWorldObj) != world) continue;              // только предметы ТЕКУЩЕГО мира
        DroppedItem d; d.entity = e;
        SafeRead(e + L.entPosX, d.x); SafeRead(e + L.entPosY, d.y); SafeRead(e + L.entPosZ, d.z);
        if (d.x == 0.0 && d.y == 0.0 && d.z == 0.0) continue;          // непозиционированный/служебный EntityItem (GUI-панели и т.п.) — не лежит на земле
        SafeRead(e + L.entEntityId, d.entityId);
        const uintptr_t dw = ReadOop(e, L.entDataWatcher), wrap = dw ? ReadOop(dw, L.dwSlots) : 0, slots = wrap ? ReadOop(wrap, L.wrapArray) : 0;
        int32_t ns = 0;
        if (slots && SafeRead(slots + L.arrLength, ns) && ns > Layout::ITEM_SLOT) {
            const uintptr_t wo = ReadOop(slots, L.arrData + 4 * Layout::ITEM_SLOT);   // Object[] индекс 10 -> WatchableObject слота 10
            const uintptr_t stack = wo ? ReadOop(wo, L.woValue) : 0;                  // .value == ItemStack (dataValueId тут не выучен — проверяем по классу)
            if (stack && !IsForwarded(stack) && NarrowKlassOf(stack) == L.nkItemStack) {
                d.itemStack = stack;
                SafeRead(stack + L.stackSize, d.count);
                SafeRead(stack + L.stackDamage, d.damage);
                d.hasNbt = ReadOop(stack, L.stackNbt) != 0;
                d.item = ReadOop(stack, L.stackItem);
                if (d.item) d.complete = ReadJavaString(L, ReadOop(d.item, L.itemUnlocalizedName), d.name);
            }
        }
        out.push_back(std::move(d));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------- игроки
struct PlayerInfo {
    uintptr_t    entity = 0;
    std::wstring name;
    double       x = 0, y = 0, z = 0, dist = 0;
    bool         isLocal = false, resolving = false;   // resolving = ник ещё временный хендл '-XXXX' (профиль не подгрузился)
    std::wstring held;                                 // имя предмета в руке ("" если рука пуста)
    uintptr_t    heldPtr = 0;                          // Item* предмета в руке (для матча по указателю)
    int          heldCount = 0;
    bool         isTarget = false;                     // held ∈ целей модуля (напр. swordIron/bow)
    std::vector<std::pair<std::wstring, int>> items;   // (совместимость) экипировка/инвентарь, если собирали
};

// мелкий BFS от игрока: собрать до maxItems достижимых ItemStack (имя+кол-во) — предмет в руке/экипировка/инвентарь.
// Не заходит в крупные объекты (>=300Б) и другие сущности; ближайшие (по BFS) = держимые/надетые.
inline void CollectHeld(const Layout& L, uintptr_t root, std::vector<std::pair<std::wstring, int>>& out, int maxItems = 6) {
    if (!L.nkItemStack) return;
    std::vector<uintptr_t> fr{ root }; std::unordered_set<uintptr_t> seen; seen.insert(root); int vis = 0;
    for (int d = 0; d < 5 && vis < 1200 && (int)out.size() < maxItems; ++d) {
        std::vector<uintptr_t> nx;
        for (uintptr_t o : fr) {
            if (++vis > 1200) break;
            const uint32_t nk = NarrowKlassOf(o); int lo, hi;
            if (LayoutHelper(nk) < 0) { if (!IsObjArray(nk)) continue; uint32_t len = 0; SafeRead(o + 12, len); if (len > 64) len = 64; lo = 16; hi = 16 + (int)len * 4; }
            else { int sz = (int)SizeOf(nk); if (sz <= 0 || sz > 4096) sz = 64; lo = 12; hi = sz; }
            for (int off = lo; off + 3 < hi; off += 4) {
                const uintptr_t t = ReadOop(o, off); if (!t || !seen.insert(t).second) continue;
                const uint32_t tnk = NarrowKlassOf(t);
                if (tnk == L.nkItemStack) {
                    int cnt = 0; SafeRead(t + L.stackSize, cnt); const uintptr_t it = ReadOop(t, L.stackItem);
                    std::wstring nm; if (it) ReadJavaString(L, ReadOop(it, L.itemUnlocalizedName), nm);
                    out.push_back({ nm, cnt }); if ((int)out.size() >= maxItems) return; continue;
                }
                if (SizeOf(tnk) >= 300) continue;                 // не уходим в другие сущности/мир
                nx.push_back(t);
            }
        }
        fr.swap(nx);
    }
}

// Из готового списка экземпляров строит список игроков ТЕКУЩЕГО мира: ник (@entName), координаты, дистанция от
// локального игрока, предмет(ы) в руке. isLocal помечает тебя; resolving — ник ещё как временный хендл.
inline std::vector<PlayerInfo> BuildPlayers(const Layout& L, const std::vector<uintptr_t>& insts) {
    std::vector<PlayerInfo> out;
    if (!L.nPlayers || !L.entName || !L.entWorldObj) return out;
    const uintptr_t mc = CurrentMinecraft(L);
    const uintptr_t world = IsHeapAddr(mc) ? ReadOop(mc, L.mcTheWorld) : 0;
    if (!world) return out;
    const uintptr_t localP = L.mcThePlayer ? ReadOop(mc, L.mcThePlayer) : 0;
    double lx = 0, ly = 0, lz = 0; bool haveLocal = false;
    if (localP) { SafeRead(localP + L.entPosX, lx); SafeRead(localP + L.entPosY, ly); SafeRead(localP + L.entPosZ, lz); haveLocal = true; }
    for (uintptr_t e : insts) {
        if (IsForwarded(e)) continue;
        const uint32_t nk = NarrowKlassOf(e);
        bool isP = false; for (int i = 0; i < L.nPlayers; ++i) if (nk == L.nkPlayers[i]) { isP = true; break; }
        if (!isP) continue;
        if (ReadOop(e, L.entWorldObj) != world) continue;              // только игроки ТЕКУЩЕГО мира
        PlayerInfo p; p.entity = e; p.isLocal = (e == localP);
        const uintptr_t s = ReadOop(e, L.entName);
        if (s && NarrowKlassOf(s) == L.nkString) ReadJavaString(L, s, p.name);
        p.resolving = (!p.name.empty() && p.name[0] == L'-');       // временный хендл VimeWorld
        SafeRead(e + L.entPosX, p.x); SafeRead(e + L.entPosY, p.y); SafeRead(e + L.entPosZ, p.z);
        if (p.x == 0.0 && p.y == 0.0 && p.z == 0.0) continue;          // непозиционированный
        p.dist = haveLocal ? std::sqrt((p.x - lx) * (p.x - lx) + (p.y - ly) * (p.y - ly) + (p.z - lz) * (p.z - lz)) : 0.0;
        if (!p.isLocal) CollectHeld(L, e, p.items);                    // свой инвентарь не тащим (шумно) — только чужих
        out.push_back(std::move(p));
    }
    return out;
}

// Синхронный вариант (для офлайн-теста и фолбэка): throttle-скан по классу + BuildDrops. В DLL перечисление
// делает фоновый поток, а кадр зовёт BuildDrops напрямую по снимку.
inline std::vector<DroppedItem> ScanDroppedItems(const Layout& L) {
    static std::vector<uintptr_t> cache; static ULONGLONG lastScan = 0; static uint32_t lastNk = 0;
    if (L.nkEntityItem && (lastNk != L.nkEntityItem || GetTickCount64() - lastScan > 2000)) {
        cache = ScanInstances(L.nkEntityItem); lastScan = GetTickCount64(); lastNk = L.nkEntityItem;
    }
    return BuildDrops(L, cache);
}

// ── Автодетект модели памяти (narrow-KLASS base+shift + compact strings). Ставит Codec() под текущий клиент. ──
// Клиент A (VimeWorld): base 0, shift 3. Клиент B (Genesis/Lunar, Java 9+): base = класс-спейс (per-launch ASLR), shift 0.
// Работает по структуре, ничего не хардкодит: сэмплит реальные объекты через oop-дереф (oop-кодек фиксирован),
// перебирает (base,shift) по committed-регионам и берёт тот, где layout_helper вменяем у большинства объектов.
inline bool AutoDetectKlassModel() {
    KlassCodec& C = Codec();
    // 1) выборка реальных объектов: значения в oop-диапазоне -> дереф -> narrow klass в obj+8 (не зависит от klass-кодека)
    std::vector<uint32_t> nks; nks.reserve(4096);
    std::vector<uint8_t> buf(4u << 20);
    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t p = HEAP_LO; p < HEAP_HI && nks.size() < 3000; ) {
        if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) break;
        const uintptr_t base = (uintptr_t)mbi.BaseAddress, next = base + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            const uintptr_t lo = base < HEAP_LO ? HEAP_LO : base, hi = next > HEAP_HI ? HEAP_HI : next;
            for (uintptr_t c = lo; c < hi && nks.size() < 3000; c += buf.size()) {
                const size_t n = (size_t)((hi - c < buf.size()) ? hi - c : buf.size());
                if (!detail::SafeReadRaw(c, buf.data(), n)) continue;
                for (size_t i = 0; i + 12 <= n && nks.size() < 3000; i += 64) {   // разреженный сэмпл
                    uint32_t v; std::memcpy(&v, buf.data() + i, 4);
                    if (v >= OOP_NLO && v < OOP_NHI) {                            // похоже на narrow oop
                        const uintptr_t o = (uintptr_t)v << OOP_SHIFT;
                        uint32_t k = 0;
                        if (SafeRead(o + 8, k) && k && !(k & 7)) nks.push_back(k);
                    }
                }
            }
        }
        p = next > p ? next : p + 0x1000;
    }
    if (nks.size() < 100) return false;

    auto score = [&](uintptr_t kbase, unsigned kshift) -> int {
        C.base = kbase; C.shift = kshift; int ok = 0, tested = 0;
        for (uint32_t k : nks) { if (!k || (k & 7)) continue; if (++tested > 1500) break; if (KlassLhSane(k)) ++ok; }
        return tested ? ok * 1000 / tested : 0;
    };

    // 2) кандидаты: сперва клиент A (0/3); если не уверен — committed-регионы выше кучи как база (shift 0 и 3)
    int bestScore = score(0, 3); uintptr_t bestBase = 0; unsigned bestShift = 3;
    if (bestScore < 950) {
        for (uintptr_t p = HEAP_HI; p < 0x800000000000ull; ) {
            if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) break;
            const uintptr_t rbase = (uintptr_t)mbi.BaseAddress, rnext = rbase + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                mbi.RegionSize >= (1u << 20) && mbi.RegionSize <= (1ull << 31)) {
                for (unsigned sh : {0u, 3u}) { int s = score(rbase, sh); if (s > bestScore) { bestScore = s; bestBase = rbase; bestShift = sh; } }
            }
            p = rnext > p ? rnext : p + 0x1000;
            if (bestScore >= 980) break;
        }
    }

    // 3) применяем лучшую модель. Порог снижен до 500 — на Lunar score колеблется 500-980,
    //    и даже marginal detection лучше чем fallback на client A (который там точно не подходит).
    if (bestScore < 500) { C.base = 0; C.shift = 3; C.classLo = CLASS_LO; C.classHi = CLASS_HI;
                           C.nkLo = (uint32_t)(CLASS_LO >> 3); C.nkHi = (uint32_t)(CLASS_HI >> 3);
                           C.validateLh = false; C.compactStr = false; C.detected = false; return false; }
    C.base = bestBase; C.shift = bestShift;
    if (bestBase == 0 && bestShift == 3) {                        // клиент A
        C.classLo = CLASS_LO; C.classHi = CLASS_HI; C.validateLh = false;
    } else {                                                      // клиент B: класс-спейс committed НЕСКОЛЬКИМИ кусками
        // с reserved-разрывами внутри одной ~1ГБ резервации. VirtualQuery(base) даёт только ПЕРВЫЙ кусок ->
        // склеиваем весь committed-экстент (мостим малые разрывы ≤16МБ; большой reserved-хвост резервации завершает).
        C.classLo = bestBase;
        uintptr_t hi = bestBase;
        for (uintptr_t p = bestBase; p < bestBase + (512ull << 20); ) {
            if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) break;
            const uintptr_t rb = (uintptr_t)mbi.BaseAddress, rn = rb + mbi.RegionSize;
            const bool committed = (mbi.State == MEM_COMMIT) && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
            if (committed) hi = rn;                               // расширяем до конца committed-куска
            else if ((rn - rb) > (16ull << 20)) break;           // крупный не-committed разрыв -> конец класс-спейса
            p = rn > p ? rn : p + 0x1000;                        // малый разрыв — мостим, идём выше
        }
        C.classHi = hi > bestBase ? hi : bestBase + (64ull << 20);
        C.validateLh = true;
    }
    C.nkLo = (uint32_t)((C.classLo - C.base) >> C.shift); if (C.nkLo == 0) C.nkLo = 8;
    C.nkHi = (uint32_t)((C.classHi - C.base) >> C.shift);

    // 4) compact strings? считаем char[] c текстом против byte[] c текстом среди сэмпла массивов
    int charTxt = 0, byteTxt = 0;
    {
        // пройдём по тем же oop-объектам ещё раз, но теперь классифицируем массивы
        int seen = 0;
        for (uintptr_t p = HEAP_LO; p < HEAP_HI && seen < 4000; ) {
            if (!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) break;
            const uintptr_t base = (uintptr_t)mbi.BaseAddress, next = base + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                const uintptr_t lo = base < HEAP_LO ? HEAP_LO : base, hi = next > HEAP_HI ? HEAP_HI : next;
                for (uintptr_t c = lo; c < hi && seen < 4000; c += buf.size()) {
                    const size_t n = (size_t)((hi - c < buf.size()) ? hi - c : buf.size());
                    if (!detail::SafeReadRaw(c, buf.data(), n)) continue;
                    for (size_t i = 0; i + 20 <= n && seen < 4000; i += 64) {
                        uint32_t v; std::memcpy(&v, buf.data() + i, 4);
                        if (v < OOP_NLO || v >= OOP_NHI) continue;
                        const uintptr_t o = (uintptr_t)v << OOP_SHIFT;
                        const uint32_t k = NarrowKlassOf(o); if (!k || (k & 7)) continue;
                        const int et = ArrTypeByte(k); if (et != 5 && et != 8) continue;
                        uint32_t ln = 0; if (!SafeRead(o + 12, ln) || ln < 3 || ln > 64) continue;
                        // содержимое: ASCII?
                        bool ascii = true; const int step = (et == 5) ? 2 : 1; int cnt = 0;
                        for (uint32_t q = 0; q < ln && cnt < 12; ++q, ++cnt) { uint8_t b = 0; SafeRead(o + 16 + (uintptr_t)q * step, b); if (b < 0x20 || b > 0x7e) { ascii = false; break; } }
                        if (!ascii) continue;
                        if (et == 5) ++charTxt; else ++byteTxt; ++seen;
                    }
                }
            }
            p = next > p ? next : p + 0x1000;
        }
    }
    C.compactStr = (byteTxt > 20 && byteTxt > charTxt * 4);      // byte[]-строки резко доминируют -> Java 9+ compact
    C.detected = true;
    return true;
}

// строит индекс и запускает дискавери; true = CORE найден (синглтон/мир/классы). Item-цепочка — best-effort.
inline bool DiscoverAll(Layout& L) {
    if (!Codec().detected) AutoDetectKlassModel();   // модель памяти клиента фиксирована за запуск -> детектим один раз (иначе кэш)
    ObjectIndex idx; idx.Build();
    if (idx.addr.empty()) return false;
    return DiscoverLayout(L, idx);
}

} // namespace mc
