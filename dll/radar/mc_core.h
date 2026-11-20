// mc_core.h — ЯДРО радара VimeWorld 1.8.9: доступ к памяти + динамический дискавери (Layout) + резолв целей.
// Ничего не захардкожено: все классы/смещения находятся по структуре в рантайме (см. mc_discover.h).
// Модули mc_gold.h / mc_players.h строятся поверх этого ядра и матчат предметы ПО УКАЗАТЕЛЮ (без чтения строк в цикле).
//
// Быстрый старт для своей DLL:
//   mc::Layout L;
//   if (mc::DiscoverAll(L)) {            // одноразовый дискавери (несколько секунд); нужен хотя бы один лежащий дроп
//       mc::GoldRadar gold;  gold.Start(L);      // из mc_gold.h
//       mc::PlayerRadar pl;  pl.Start(L);        // из mc_players.h
//       ... каждый свой кадр: auto d = gold.Get(); auto p = pl.Get(); // свежие данные, мгновенно, потокобезопасно
//   }
#pragma once
#include "mc_discover.h"   // подтягивает mc_dropped_items.h (SafeRead/ReadOop/Layout/...) + дискавери и скан-примитивы

namespace mc {

struct Vec3 { double x = 0, y = 0, z = 0; };

// Резолв «unlocalizedName предмета -> адрес Item-объекта» ОДИН раз (Item — синглтон, адрес стабилен в old-gen).
// Дальше модули сравнивают stack.item == этот указатель — без чтения строк в горячем цикле. 0 = не найден.
inline uintptr_t ResolveItem(const Layout& L, const wchar_t* unlocName) {
    if (!L.itemRegistry || !unlocName) return 0;
    const uintptr_t arr = ReadOop(L.itemRegistry, L.alElementData);
    int32_t sz = 0;
    if (!arr || !SafeRead(L.itemRegistry + L.alSize, sz) || sz <= 0 || sz > 100000) return 0;
    const std::wstring want = unlocName;
    for (int i = 0; i < sz; ++i) {
        const uintptr_t it = ReadOop(arr, L.arrData + 4 * i);
        if (!it) continue;
        std::wstring nm;
        if (ReadJavaString(L, ReadOop(it, L.itemUnlocalizedName), nm, 48) && nm == want) return it;
    }
    return 0;
}

// Предмет в руке сущности: мелкий BFS (глубина <=5, <=1200 объектов, не заходит в крупные >=300Б/чужие сущности),
// возвращает ПЕРВЫЙ достижимый ItemStack (ближайший = слот руки): его Item* + имя + количество. false, если ничего.
inline bool ReadHeld(const Layout& L, uintptr_t root, uintptr_t& itemPtr, std::wstring& name, int& count) {
    itemPtr = 0; name.clear(); count = 0;
    if (!L.nkItemStack || !root) return false;
    std::vector<uintptr_t> fr{ root }; std::unordered_set<uintptr_t> seen; seen.insert(root); int vis = 0;
    for (int d = 0; d < 5 && vis < 1200; ++d) {
        std::vector<uintptr_t> nx;
        for (uintptr_t o : fr) {
            if (++vis > 1200) break;
            const uint32_t nk = NarrowKlassOf(o); int lo, hi;
            if (LayoutHelper(nk) < 0) { if (!IsObjArray(nk)) continue; uint32_t len = 0; SafeRead(o + 12, len); if (len > 64) len = 64; lo = 16; hi = 16 + (int)len * 4; }
            else { int sz = (int)SizeOf(nk); if (sz <= 0 || sz > 4096) sz = 64; lo = 12; hi = sz; }
            for (int off = lo; off + 3 < hi; off += 4) {
                const uintptr_t t = ReadOop(o, off); if (!t || !seen.insert(t).second) continue;
                if (NarrowKlassOf(t) == L.nkItemStack) {
                    SafeRead(t + L.stackSize, count);
                    itemPtr = ReadOop(t, L.stackItem);
                    if (itemPtr) ReadJavaString(L, ReadOop(itemPtr, L.itemUnlocalizedName), name);
                    return true;
                }
                if (SizeOf(NarrowKlassOf(t)) >= 300) continue;     // не уходим в другие сущности/мир
                nx.push_back(t);
            }
        }
        fr.swap(nx);
    }
    return false;
}

// Обход НАДЕТОГО сущности (рука + броня) ОДНИМ BFS: отдаёт первый ItemStack (для показа) и, если среди ВСЕХ
// достижимых стаков нашлась цель (матч Item* по указателю) — её Item* в matchedTarget (ранний выход).
// Надёжнее «первого стака»: у чужих игроков синкается только рука+броня (инвентарь не передаётся), а броня не
// может быть мечом/луком — поэтому найденная цель = оружие В РУКЕ, независимо от порядка обхода.
inline void ReadEquip(const Layout& L, uintptr_t root, const uintptr_t* targets, int nTargets,
                      uintptr_t& firstItem, std::wstring& firstName, int& firstCount, uintptr_t& matchedTarget) {
    firstItem = 0; firstName.clear(); firstCount = 0; matchedTarget = 0;
    if (!L.nkItemStack || !root) return;
    bool haveFirst = false;
    std::vector<uintptr_t> fr{ root }; std::unordered_set<uintptr_t> seen; seen.insert(root); int vis = 0;
    for (int d = 0; d < 5 && vis < 1200; ++d) {
        std::vector<uintptr_t> nx;
        for (uintptr_t o : fr) {
            if (++vis > 1200) break;
            const uint32_t nk = NarrowKlassOf(o); int lo, hi;
            if (LayoutHelper(nk) < 0) { if (!IsObjArray(nk)) continue; uint32_t len = 0; SafeRead(o + 12, len); if (len > 64) len = 64; lo = 16; hi = 16 + (int)len * 4; }
            else { int sz = (int)SizeOf(nk); if (sz <= 0 || sz > 4096) sz = 64; lo = 12; hi = sz; }
            for (int off = lo; off + 3 < hi; off += 4) {
                const uintptr_t t = ReadOop(o, off); if (!t || !seen.insert(t).second) continue;
                if (NarrowKlassOf(t) == L.nkItemStack) {
                    const uintptr_t it = ReadOop(t, L.stackItem);
                    if (!haveFirst) { haveFirst = true; firstItem = it; SafeRead(t + L.stackSize, firstCount); if (it) ReadJavaString(L, ReadOop(it, L.itemUnlocalizedName), firstName); }
                    for (int i = 0; i < nTargets; ++i) if (it && it == targets[i]) { matchedTarget = it; return; }   // цель найдена — выходим
                    continue;                                    // не цель — внутрь ItemStack не идём
                }
                if (SizeOf(NarrowKlassOf(t)) >= 300) continue;   // не уходим в другие сущности/мир
                nx.push_back(t);
            }
        }
        fr.swap(nx);
    }
}

// Прямое чтение руки: цель среди предметов ГЛАВНОГО инвентаря игрока (у чужих там только предмет в руке).
// O(число слотов), без обхода графа объектов. Требует L.playerInventory + L.invMainInventory (иначе см. ReadEquip).
inline void ReadEquipDirect(const Layout& L, uintptr_t player, const uintptr_t* targets, int nTargets,
                            uintptr_t& firstItem, std::wstring& firstName, int& firstCount, uintptr_t& matchedTarget) {
    firstItem = 0; firstName.clear(); firstCount = 0; matchedTarget = 0;
    if (!player || !L.playerInventory || !L.invMainInventory || !L.nkItemStack) return;
    const uintptr_t inv = ReadOop(player, L.playerInventory);
    const uintptr_t arr = IsHeapAddr(inv) ? ReadOop(inv, L.invMainInventory) : 0;
    if (!IsHeapAddr(arr)) return;
    uint32_t len = 0; if (!SafeRead(arr + L.arrLength, len) || len == 0 || len > 64) return;
    bool haveFirst = false;
    for (uint32_t i = 0; i < len; ++i) {
        const uintptr_t s = ReadOop(arr, L.arrData + 4 * i);
        if (!s || NarrowKlassOf(s) != L.nkItemStack) continue;
        const uintptr_t it = ReadOop(s, L.stackItem);
        if (!haveFirst) { haveFirst = true; firstItem = it; SafeRead(s + L.stackSize, firstCount); if (it) ReadJavaString(L, ReadOop(it, L.itemUnlocalizedName), firstName); }
        for (int k = 0; k < nTargets; ++k) if (it && it == targets[k]) { matchedTarget = it; return; }
    }
}

} // namespace mc
