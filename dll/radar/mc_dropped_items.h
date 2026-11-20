// mc_dropped_items.h — чтение сущностей и предметов из памяти JVM Minecraft 1.8.9
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#ifdef MC_DUMP_BACKED
extern "C" bool mc_dump_read(uintptr_t src, void* dst, size_t n);   // офлайн-тест по дампу: подмена доступа к памяти (в DLL не компилируется)
#endif

namespace mc {

// ---------------------------------------------------------------------------------------------- 1. константы JVM
constexpr uintptr_t HEAP_LO   = 0x6C0000000ull;
constexpr uintptr_t HEAP_HI   = 0x7C0000000ull;
constexpr uintptr_t CLASS_LO  = 0x7C0000000ull;   // дефолт клиента A (VimeWorld): метаспейс сразу над кучей
constexpr uintptr_t CLASS_HI  = 0x7C1000000ull;
constexpr unsigned  OOP_SHIFT = 3;                // narrow OOP: base 0, shift 3 (для ОБОИХ клиентов — куча < 32ГБ)

// ── Кодек narrow-KLASS (декод указателя класса). ОТЛИЧАЕТСЯ между клиентами и МЕНЯЕТСЯ каждый запуск (ASLR) ──
// Клиент A (VimeWorld): Klass* = nk << 3 (base 0, shift 3), narrow klass в [0xF8000000, 0xF8200000).
// Клиент B (Genesis/Lunar, Java 9+): Klass* = base + nk (shift 0), base рандомен per-launch; строки compact (byte[]+coder).
// Автодетект (mc_discover.h: AutoDetectKlassModel) выставляет это при старте. Дефолт = клиент A (совместимость).
struct KlassCodec {
    uintptr_t base       = 0;                     // A: 0
    unsigned  shift      = OOP_SHIFT;             // A: 3 ; B: 0
    uintptr_t classLo    = CLASS_LO;              // диапазон валидных Klass* (для распознавания объектов)
    uintptr_t classHi    = CLASS_HI;
    uint32_t  nkLo       = (uint32_t)(CLASS_LO >> OOP_SHIFT);   // диапазон narrow klass (быстрый фильтр в дискавери)
    uint32_t  nkHi       = (uint32_t)(CLASS_HI >> OOP_SHIFT);
    bool      compactStr = false;                 // true = String.value это byte[]+coder (Java 9+), а не UTF-16 char[]
    int       strCoderOff = 0;                    // смещение байта coder в String (0 = ещё не выучено -> Latin1 для ASCII-имён)
    bool      detected   = false;                 // автодетект отработал
    bool      validateLh = false;                 // клиент B: узкие klass лежат в «шумном» низком диапазоне -> валидировать layout_helper при индексации
};
inline KlassCodec& Codec() { static KlassCodec c; return c; }   // единственный экземпляр (C++14-safe, без inline-переменных)
inline uintptr_t KlassAddr(uint32_t nk) { const KlassCodec& c = Codec(); return nk ? c.base + ((uintptr_t)nk << c.shift) : 0; }
inline bool KlassNkInRange(uint32_t nk) { return nk >= Codec().nkLo && nk < Codec().nkHi; }

// ---------------------------------------------------------------------------------------------- 2. раскладка
struct Layout {
    uintptr_t minecraft    = 0;         // адрес синглтона (стабилен: old-gen); при возможности освежается из klassMinecraft
    uint32_t  nkEntityItem = 0;          // 0 = ещё не выучен (нужен живой дроп); НЕ хардкодить (меняется каждый запуск)
    uint32_t  nkPlayer     = 0;
    uint32_t  nkItemStack  = 0;
    uint32_t  nkString     = 0xF800033A;
    uint32_t  nkClass      = 0xF8000461;

    // --- Minecraft ---
    uintptr_t klassMinecraft = 0;       // (устар.) Klass(Minecraft)
    int mcStaticSlot = 0;               // (устар.)
    uint32_t nkMinecraft = 0;           // narrow klass объекта Minecraft (для валидации кэша)
    int mcTheWorld = 380, mcThePlayer = 0;

    // --- World / WorldClient ---
    uint32_t  nkArrayList = 0;          // java.util.ArrayList (для выбора списка сущностей в мире на лету)
    uint32_t  nkWorld = 0;              // WorldClient
    int worldLoadedEntityList = 112;
    int worldPlayerList = 0;           // ДИСКАВЕРИТСЯ: offset ArrayList игроков мира (playerEntities); 0 = не найден -> фолбэк на скан кучи

    // --- java.util.ArrayList и массивы ---
    int alSize = 12, alElementData = 20, arrLength = 12, arrData = 16;

    // --- Entity (координаты X, Y, Z) ---
    int entPosX = 40, entPosY = 48, entPosZ = 56;
    int entEntityId = 332, entDataWatcher = 444;
    int entWorldObj = 0;                // Entity.worldObj -> WorldClient (для фильтра дропов по текущему миру)

    // --- DataWatcher ---
    int dwSlots = 16, wrapArray = 12;
    int woDataValueId = 12, woType = 16, woValue = 24;
    static constexpr int32_t ITEM_SLOT = 10, ITEM_TYPE = 5;

    // --- ItemStack, Item ---
    int stackItem = 40, stackSize = 20, stackDamage = 16, stackNbt = 32;
    int itemUnlocalizedName = 36;
    uintptr_t itemRegistry = 0;         // ArrayList всех Item по id (для резолва «имя предмета -> Item*» один раз)

    // --- java.lang.String ---
    int strValue = 12, charArrLength = 12, charArrData = 16;
    int className = 20, classKlassPtr = 72, classStatics = 104;

    // --- EntityPlayer / InventoryPlayer (ДИСКАВЕРЯТСЯ; 0 = не найдено -> чтение руки через BFS) ---
    int playerInventory = 0, invCurrentItem = 0, invMainInventory = 0;

    // --- Игроки (радар игроков: ник + координаты + предмет в руке) ---
    uint32_t nkPlayers[4] = { 0, 0, 0, 0 };   // классы игроков в мире (локальный + чужие); 0 = ещё не выучены
    int nPlayers = 0;                          // сколько классов игроков найдено
    int entName = 0;                           // смещение строки-ника в сущности игрока (общее базовое поле игрока)
};

// ---------------------------------------------------------------------------------------------- 3. безопасное чтение
namespace detail {
inline bool SafeReadRaw(uintptr_t src, void* dst, size_t n) noexcept {
#ifdef MC_DUMP_BACKED
    return ::mc_dump_read(src, dst, n);                 // офлайн-тест: читаем из дампа вместо живой памяти
#else
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#endif
}
} // namespace detail

template <class T>
inline bool SafeRead(uintptr_t addr, T& out) noexcept {
    static_assert(std::is_trivially_copyable<T>::value, "SafeRead: only POD types");   // ::value — совместимо и с C++14, и с C++17
    return addr != 0 && detail::SafeReadRaw(addr, &out, sizeof(T));
}

inline bool      IsHeapAddr(uintptr_t a) noexcept { return a >= HEAP_LO && a < HEAP_HI; }
inline uintptr_t DecodeOop(uint32_t narrow) noexcept { return static_cast<uintptr_t>(narrow) << OOP_SHIFT; }

inline uintptr_t ReadOop(uintptr_t obj, int off) noexcept {
    uint32_t n = 0;
    if (!obj || !SafeRead(obj + off, n) || n == 0) return 0;
    const uintptr_t a = DecodeOop(n);
    return IsHeapAddr(a) ? a : 0;
}

inline uint32_t NarrowKlassOf(uintptr_t obj) noexcept {
    uint32_t nk = 0;
    return SafeRead(obj + 8, nk) ? nk : 0;
}

inline bool IsForwarded(uintptr_t obj) noexcept {
    uint64_t m = 0;
    return !SafeRead(obj, m) || (m & 3) == 3;
}

// ---------------------------------------------------------------------------------------------- 4. java.lang.String
inline bool ReadJavaString(const Layout& L, uintptr_t str, std::wstring& out, size_t maxChars = 512) {
    out.clear();
    if (!str) return false;
    if (L.nkString && NarrowKlassOf(str) != L.nkString) return false;
    const uintptr_t arr = ReadOop(str, L.strValue);       // value: char[] (A) или byte[] (B, compact)
    int32_t alen = 0;                                     // длина массива в ЭЛЕМЕНТАХ (char у A; байт у B)
    if (!arr || !SafeRead(arr + L.charArrLength, alen) || alen < 0) return false;

    const KlassCodec& cc = Codec();
    if (cc.compactStr) {                                  // Java 9+: byte[] + coder (0=Latin1 1Б/симв, 1=UTF16 2Б/симв)
        int coder = 0;
        if (cc.strCoderOff > 0) { uint8_t cb = 0; if (SafeRead(str + cc.strCoderOff, cb)) coder = cb & 1; }
        if (coder == 0) {                                 // LATIN1 (ники/имена предметов — всегда сюда)
            size_t n = (size_t)alen; if (n > maxChars) n = maxChars;
            if (!n) return true;
            std::vector<uint8_t> tmp(n);
            if (!detail::SafeReadRaw(arr + L.charArrData, tmp.data(), n)) return false;
            out.resize(n); for (size_t i = 0; i < n; ++i) out[i] = (wchar_t)tmp[i];
            return true;
        }
        size_t n = (size_t)(alen / 2); if (n > maxChars) n = maxChars;   // UTF16
        out.resize(n);
        if (n && !detail::SafeReadRaw(arr + L.charArrData, &out[0], n * sizeof(wchar_t))) { out.clear(); return false; }
        return true;
    }

    // Клиент A (VimeWorld): UTF-16 char[]
    int32_t len = alen;
    if (static_cast<size_t>(len) > maxChars) len = static_cast<int32_t>(maxChars);
    out.resize(static_cast<size_t>(len));
    if (len && !detail::SafeReadRaw(arr + L.charArrData, &out[0], static_cast<size_t>(len) * sizeof(wchar_t))) {   // &out[0] пишем (data() до C++17 const)
        out.clear();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------- 5. сущности и предметы
struct DroppedItem {
    uintptr_t    entity = 0, itemStack = 0, item = 0;
    int32_t      entityId = 0, count = 0, damage = 0;
    double       x = 0, y = 0, z = 0;
    bool         hasNbt = false;
    bool         complete = false;
    std::wstring name;
};

inline bool CollectDroppedItems(Layout& L, std::vector<DroppedItem>& out, uintptr_t* thePlayerOut = nullptr) {
    out.clear();
    const uintptr_t mc = L.minecraft;
    if (!IsHeapAddr(mc)) return false;

    const uintptr_t world  = ReadOop(mc, L.mcTheWorld);
    if (!world) return false;

    const uintptr_t list = ReadOop(world, L.worldLoadedEntityList);
    if (!list) return false;
    int32_t size = 0, capacity = 0;
    const uintptr_t data = ReadOop(list, L.alElementData);
    if (!data || !SafeRead(list + L.alSize, size) || !SafeRead(data + L.arrLength, capacity)) return false;
    if (size <= 0 && SafeRead(list + 24, size) == false) return false;
    if (size < 0 || size > 50000) return false;

    for (int32_t i = 0; i < size; ++i) {
        const uintptr_t e = ReadOop(data, L.arrData + 4 * i);
        if (!e || IsForwarded(e)) continue;

        uint32_t nk = NarrowKlassOf(e);
        if (thePlayerOut && nk == L.nkPlayer) {
            *thePlayerOut = e;
        }

        if (nk != L.nkEntityItem) continue;

        DroppedItem d;
        d.entity = e;
        SafeRead(e + L.entPosX, d.x);
        SafeRead(e + L.entPosY, d.y);
        SafeRead(e + L.entPosZ, d.z);
        SafeRead(e + L.entEntityId, d.entityId);

        // DataWatcher -> обёртка -> Object[] -> WatchableObject[слот 10] -> ItemStack, по НАЙДЕННЫМ смещениям.
        const uintptr_t dw    = ReadOop(e, L.entDataWatcher);
        const uintptr_t wrap  = dw   ? ReadOop(dw, L.dwSlots)     : 0;
        const uintptr_t slots = wrap ? ReadOop(wrap, L.wrapArray) : 0;
        int32_t ns = 0;
        if (slots && SafeRead(slots + L.arrLength, ns) && ns > Layout::ITEM_SLOT) {
            const uintptr_t wo = ReadOop(slots, L.arrData + 4 * Layout::ITEM_SLOT);
            int32_t id = -1;
            if (wo && SafeRead(wo + L.woDataValueId, id) && id == Layout::ITEM_SLOT) {
                const uintptr_t stack = ReadOop(wo, L.woValue);
                if (stack && !IsForwarded(stack)) {
                    d.itemStack = stack;
                    SafeRead(stack + L.stackSize, d.count);
                    SafeRead(stack + L.stackDamage, d.damage);
                    d.hasNbt = ReadOop(stack, L.stackNbt) != 0;
                    d.item = ReadOop(stack, L.stackItem);
                    if (d.item) d.complete = ReadJavaString(L, ReadOop(d.item, L.itemUnlocalizedName), d.name);
                }
            }
        }

        out.push_back(std::move(d));
    }
    return true;
}

inline std::vector<DroppedItem> GetDroppedItems(Layout& L, int attempts = 3, uintptr_t* thePlayerOut = nullptr) {
    std::vector<DroppedItem> v;
    for (int a = 0; a < attempts; ++a) {
        if (CollectDroppedItems(L, v, thePlayerOut)) return v;
        Sleep(1);
    }
    v.clear();
    return v;
}

// ---------------------------------------------------------------------------------------------- 6. резолвинг кучи
inline bool ResolveByHeapScan(Layout& L, const wchar_t* entityItemClass = L"tsa", const wchar_t* minecraftClass = L"lcA") {
    uintptr_t mcMirror = 0, eiMirror = 0;
    std::vector<uint8_t> buf(1u << 20);
    MEMORY_BASIC_INFORMATION mbi{};
    for (uintptr_t p = HEAP_LO; p < HEAP_HI && (!mcMirror || !eiMirror); p = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi))) break;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) continue;
        const uintptr_t rb = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t re = (rb + mbi.RegionSize < HEAP_HI) ? rb + mbi.RegionSize : HEAP_HI;
        for (uintptr_t chunk = rb; chunk < re; chunk += buf.size()) {
            const size_t n = static_cast<size_t>((re - chunk < buf.size()) ? re - chunk : buf.size());
            if (!detail::SafeReadRaw(chunk, buf.data(), n)) continue;
            for (size_t i = 8; i + 4 <= n; i += 8) {
                uint32_t nk; std::memcpy(&nk, buf.data() + i, 4);
                if (nk != L.nkClass) continue;
                const uintptr_t mirror = chunk + i - 8;
                std::wstring name;
                if (!ReadJavaString(L, ReadOop(mirror, L.className), name, 64)) continue;
                if (!mcMirror && name == minecraftClass) mcMirror = mirror;
                if (!eiMirror && name == entityItemClass) eiMirror = mirror;
            }
        }
    }
    uintptr_t k = 0;
    bool ok = true;
    if (eiMirror && SafeRead(eiMirror + L.classKlassPtr, k) && k >= CLASS_LO && k < CLASS_HI) L.nkEntityItem = static_cast<uint32_t>(k >> OOP_SHIFT); else ok = false;
    uint32_t nkMc = 0;
    if (mcMirror && SafeRead(mcMirror + L.classKlassPtr, k) && k >= CLASS_LO && k < CLASS_HI) nkMc = static_cast<uint32_t>(k >> OOP_SHIFT);
    uintptr_t found = 0;
    for (int off = L.classStatics; nkMc && off < L.classStatics + 4 * 64 && !found; off += 4) {
        const uintptr_t o = ReadOop(mcMirror, off);
        if (o && NarrowKlassOf(o) == nkMc) found = o;
    }
    if (found) L.minecraft = found; else ok = false;
    return ok;
}

} // namespace mc
