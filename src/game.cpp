#include "game.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <cmath>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "common.h"
#include "imgui.h"
#include "ue.h"
#include "i18n.h"
#include "plus_host.h"
#include <cstdarg>

namespace game {
using i18n::F;
using i18n::T;
using i18n::TL;
namespace {

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
struct MultDef {
    const char* attr;      // UHnSUberAttributeSet property
    const char* category;  // UI group
    const char* label;
    const char* tip;
    float minV, maxV;
    bool log;  // logarithmic slider
    float value = 1.0f;
};

MultDef g_mults[] = {
    {"XPMultiplier", "Progression", "XP gain", "Experience earned by the character.", 0.1f, 50.f, true},
    {"WyrmlingXPMultiplier", "Progression", "Wyrmling XP", "Experience earned by the Wyrmling companion.", 0.1f, 50.f, true},

    {"ExtraGoldMultiplier", "Gold and loot", "Gold", "Amount of gold dropped.", 0.1f, 50.f, true},
    {"EquipmentDropMultiplier", "Gold and loot", "Equipment drops", "Number of equipment drops.", 0.1f, 20.f, true},
    {"EquipmentQualityMultiplier", "Gold and loot", "Equipment quality", "Chance of rarer equipment.", 0.1f, 10.f, true},
    {"LootQualityMultiplier", "Gold and loot", "Loot quality", "Overall loot rarity.", 0.1f, 10.f, true},
    {"T4QualityMultiplier", "Gold and loot", "T4 quality", "Chance of top tier (T4) items.", 0.1f, 10.f, true},
    {"LootEfficiencyMultiplier", "Gold and loot", "Loot efficiency", "The game's loot efficiency stat.", 0.1f, 10.f, true},
    {"MaterialsDropMultiplier", "Gold and loot", "Material drops", "Crafting materials.", 0.1f, 20.f, true},
    {"EggDropMultiplier", "Gold and loot", "Egg drops", "Wyrmling egg drop rate.", 0.1f, 20.f, true},
    {"SkillFragmentDropMultiplier", "Gold and loot", "Skill fragment drops", "Skill fragment drop rate.", 0.1f, 20.f, true},
    {"FragmentQualityMultiplier", "Gold and loot", "Fragment quality", "Fragment rarity.", 0.1f, 10.f, true},
    {"WyrmHatchChanceMultiplier", "Gold and loot", "Hatch chance", "Wyrm egg hatch chance.", 0.1f, 10.f, true},
    {"LootRange", "Gold and loot", "Loot pickup range", "Radius in which items are picked up automatically.", 0.5f, 10.f, true},

    {"FinalDamageMultiplier", "Combat", "Damage dealt", "Final multiplier of all damage.", 0.1f, 20.f, true},
    {"CriticalDamageMultiplier", "Combat", "Critical damage", "Critical hit damage.", 0.1f, 10.f, true},
    {"HealthMaxMultiplier", "Combat", "Maximum health", "Maximum health multiplier.", 0.5f, 10.f, true},
    {"HealingReceivedMultiplier", "Combat", "Healing received", "Potion and skill healing.", 0.1f, 10.f, true},
    {"CooldownDurationMultiplier", "Combat", "Cooldown duration", "Lower value = shorter cooldowns.", 0.05f, 2.f, true},
    {"CostMultiplier", "Combat", "Skill cost", "0 = skills cost no mana.", 0.0f, 2.f, false},
    {"ResourceGainMultiplier", "Combat", "Resource gain", "Refill speed of resources such as mana or rage.", 0.1f, 10.f, true},

    {"MovementSpeedMultiplier", "Movement", "Movement speed", "Running speed.", 0.5f, 3.f, true},
    {"AnimSpeedMultiplier", "Movement", "Attack / animation speed", "Attack and skill animation speed.", 0.5f, 3.f, true},
    {"DodgeDistanceMultiplier", "Movement", "Dodge distance", "Distance of the dodge dash.", 0.5f, 3.f, true},
};
constexpr int kMultCount = sizeof(g_mults) / sizeof(g_mults[0]);

struct LockDef {
    const char* key;
    const char* label;
    const char* tip;
    const char* attrs[3][2];  // {value, max} pairs
    bool on = false;
};
LockDef g_locks[] = {
    {"God", "God mode (health lock)", "Health and shield are kept at maximum. Against one-shot attacks also enable Invincibility in the Console tab.",
     {{"Health", "HealthMax"}, {"Shield", "ShieldMax"}, {nullptr, nullptr}}},
    {"Mana", "Unlimited mana / flame", "Mana and flame always full.", {{"Mana", "ManaMax"}, {"Flame", "FlameMax"}, {nullptr, nullptr}}},
    {"Potions", "Unlimited potions", "Potion charges never run out.", {{"PotionCharges", "PotionChargesMax"}, {nullptr, nullptr}, {nullptr, nullptr}}},
    {"Dodge", "Unlimited dodge", "Dodge charges never run out.", {{"DodgeCharges", "DodgeChargesMax"}, {nullptr, nullptr}, {nullptr, nullptr}}},
    {"Ultimate", "Ultimate always full", "Ultimate gauge always full.", {{"UltimateSkillJauge", "UltimateSkillJaugeMax"}, {nullptr, nullptr}, {nullptr, nullptr}}},
};
constexpr int kLockCount = sizeof(g_locks) / sizeof(g_locks[0]);

struct CvarDef {
    const char* cvar;
    const char* label;
    const char* tip;
    bool on = false;
};
CvarDef g_cvars[] = {
    {"hns.DebugCommands.Invincibility", "Developer invincibility", "The game's own debug command. May not work in the release build."},
    {"hns.DebugCommands.IgnoreCooldownsForPlayer", "No cooldowns", "The game's own debug command."},
    {"hns.DebugCommands.IgnoreCostsForPlayer", "No skill cost", "The game's own debug command."},
    {"HnS.UI.AlwaysShowHealthBars", "Always show health bars", "Health bars of all enemies are visible."},
    {"hns.Loot.SmoothLootRarityProgression", "Smooth loot rarity progression", "The game's loot setting."},
};
constexpr int kCvarCount = sizeof(g_cvars) / sizeof(g_cvars[0]);

bool g_enabled = true;
float g_hubSpeed = 1.0f;  // extra movement speed in the city
bool InCity();
bool g_speedOn = false;
float g_gameSpeed = 1.0f;
int g_npcLevel = 0;
bool g_dirty = false;

std::wstring IniPath() { return ModDir() + L"DragonkinPlus.ini"; }

std::wstring W(const char* s) {
    wchar_t buf[256];
    MultiByteToWideChar(CP_UTF8, 0, s, -1, buf, 256);
    return buf;
}
float IniFloat(const wchar_t* sec, const char* key, float def) {
    wchar_t buf[64];
    GetPrivateProfileStringW(sec, W(key).c_str(), L"", buf, 64, IniPath().c_str());
    return buf[0] ? (float)_wtof(buf) : def;
}
void IniWrite(const wchar_t* sec, const char* key, float v) {
    wchar_t buf[64];
    swprintf_s(buf, L"%g", v);
    WritePrivateProfileStringW(sec, W(key).c_str(), buf, IniPath().c_str());
}

// ---------------------------------------------------------------------------
// Commands from the UI, executed on the game thread
// ---------------------------------------------------------------------------
enum class Cmd {
    Console,
    AddAttributePoints,
    Dump,
    UnlockWaypoints,
    OpenTeleportMap,
    CountWaypoints,
    Unstuck,
    SavePos,
    LoadPos,
    SellJunk,
    ResetDps,
    DrinkPotion
};
struct Command {
    Cmd type;
    std::string text;
    float amount = 0;
};
std::mutex g_cmdMutex;
std::deque<Command> g_cmds;

void Push(Cmd t, std::string text = {}, float amount = 0) {
    std::lock_guard<std::mutex> lock(g_cmdMutex);
    g_cmds.push_back({t, std::move(text), amount});
}

// ---------------------------------------------------------------------------
// Status shown in the UI (written on game thread, read on render thread)
// ---------------------------------------------------------------------------
struct AttrRow {
    std::string name;
    float base, cur;
};
struct Status {
    bool reflection = false, world = false, pawn = false, attrSet = false, canCall = false;
    std::string pawnName, attrSetPath, lastMessage;
    std::vector<AttrRow> attrs;
    int cityTables = 0, cityValues = 0;
    float citySampleOrig = 0, citySampleNow = 0;
    // QoL
    std::string worldName;
    bool inCity = false;
    bool dpsOk = false;
    float dmgTotal = 0, dpsAvg = 0, dpsLast = 0, dmgRecv = 0, healRecv = 0;
    struct DpsLine {
        std::string name;
        float total, dps;
    };
    std::vector<DpsLine> skills;
    int enemiesTracked = 0, enemiesScaled = 0;
    int recipeValues = 0;
    int levelUpWidgets = 0, levelUpHidden = 0;
    bool potionAbility = false;
    float camLen = 0;
    bool savedPos[3] = {};
};
std::mutex g_statusMutex;
Status g_status;

void SetMessage(const std::string& m) {
    Log("%s", m.c_str());
    std::lock_guard<std::mutex> lock(g_statusMutex);
    g_status.lastMessage = m;
}

// ---------------------------------------------------------------------------
// Game context
// ---------------------------------------------------------------------------
// Attribute value struct. Standard UE is FGameplayAttributeData {vtable, Base, Current}
// (16 bytes), but this game uses an 8-byte struct, so the BaseValue/CurrentValue
// offsets are read from the struct's own reflection data.
struct AttrData;
int g_attrSize = -1, g_baseOff = -1, g_curOff = -1;
inline float& B(AttrData* a) { return *reinterpret_cast<float*>((uint8_t*)a + g_baseOff); }
inline float& C(AttrData* a) { return *reinterpret_cast<float*>((uint8_t*)a + g_curOff); }

bool ResolveAttrLayout(const ue::PropInfo& p) {
    if (g_attrSize > 0) return p.elementSize == g_attrSize;
    void* strct = ue::PropertyStruct(p.field);
    if (!strct) return false;
    auto bp = ue::FindProperty(strct, "BaseValue");
    auto cp = ue::FindProperty(strct, "CurrentValue");
    if (bp && cp) {
        g_baseOff = bp.offset;
        g_curOff = cp.offset;
    } else if (p.elementSize == 16) {
        g_baseOff = 8;
        g_curOff = 12;
    } else {
        Log("attribute struct %s (size %d) has no BaseValue/CurrentValue; attributes disabled", ue::NameOf(strct).c_str(),
            p.elementSize);
        g_attrSize = 0x7FFFFFFF;  // never matches again
        return false;
    }
    g_attrSize = p.elementSize;
    Log("attribute struct %s: size %d, BaseValue +%d, CurrentValue +%d", ue::NameOf(strct).c_str(), g_attrSize, g_baseOff,
        g_curOff);
    return true;
}

std::unordered_map<std::string, int> g_propCache;
int PropOff(const char* cls, const char* prop) {
    std::string key = std::string(cls) + "." + prop;
    auto it = g_propCache.find(key);
    if (it != g_propCache.end()) return it->second;
    int result = -1;
    if (void* c = ue::FindClass(cls))
        if (auto p = ue::FindProperty(c, prop)) result = p.offset;
    if (result < 0) Log("property %s not found", key.c_str());
    g_propCache[key] = result;
    return result;
}

void* GetObj(void* o, const char* cls, const char* prop) {
    if (!o) return nullptr;
    int off = PropOff(cls, prop);
    void* v = nullptr;
    if (off < 0 || !Read((uint8_t*)o + off, v) || !LooksLikePtr(v)) return nullptr;
    return v;
}

void* g_engine = nullptr;
int g_engineIdx = -1;
void* g_world = nullptr;
void* g_pc = nullptr;
void* g_pawn = nullptr;
void* g_ps = nullptr;

void* g_attrClass = nullptr;
void* g_attrSet = nullptr;
int g_attrIdx = -1;
DWORD g_lastAttrScan = 0;
std::unordered_map<std::string, int> g_attrOffsets;

struct MultState {
    float orig = 0, ref = 1, last = NAN;
};
MultState g_multState[kMultCount];
bool g_speedApplied = false;
bool g_cvarsSynced = false;

void ResolveContext() {
    static DWORD lastEngineScan = 0;
    if (!ue::IsLive(g_engine, g_engineIdx)) g_engine = nullptr;
    if (!g_engine && GetTickCount() - lastEngineScan > 2000) {
        lastEngineScan = GetTickCount();
        void* cls = ue::FindClass("GameEngine");
        if (cls) g_engine = ue::FindFirstInstance(cls);
        if (g_engine) g_engineIdx = ue::IndexOf(g_engine);
    }
    g_world = g_pc = g_pawn = g_ps = nullptr;
    void* viewport = GetObj(g_engine, "Engine", "GameViewport");
    g_world = GetObj(viewport, "GameViewportClient", "World");
    void* gi = GetObj(g_world, "World", "OwningGameInstance");
    if (gi) {
        int off = PropOff("GameInstance", "LocalPlayers");
        ue::TArrayRaw arr{};
        if (off >= 0 && Read((uint8_t*)gi + off, arr) && arr.num > 0 && LooksLikePtr(arr.data)) {
            void* lp = ReadOr<void*>(arr.data, nullptr);
            g_pc = GetObj(lp, "Player", "PlayerController");
        }
    }
    g_pawn = GetObj(g_pc, "Controller", "Pawn");
    g_ps = GetObj(g_pc, "Controller", "PlayerState");

    static void* lastEngine = nullptr;
    static void* lastWorld = nullptr;
    static void* lastPawn = nullptr;
    if (g_engine != lastEngine) Log("engine: %s", g_engine ? ue::PathOf(g_engine).c_str() : "none");
    if (g_world != lastWorld) Log("world: %s", g_world ? ue::NameOf(g_world).c_str() : "none");
    if (g_pawn != lastPawn)
        Log("player pawn: %s (class %s)", g_pawn ? ue::NameOf(g_pawn).c_str() : "none",
            g_pawn ? ue::NameOf(ue::ClassOf(g_pawn)).c_str() : "-");
    lastEngine = g_engine;
    lastWorld = g_world;
    lastPawn = g_pawn;
}

bool OwnedByPlayer(void* o) {
    for (int d = 0; o && d < 4; ++d) {
        o = ue::OuterOf(o);
        if (o && (o == g_pawn || o == g_ps || o == g_pc)) return true;
    }
    return false;
}

void ResetAttrState() {
    for (auto& s : g_multState) s = MultState{};
}

void FindAttrSet() {
    if (ue::IsLive(g_attrSet, g_attrIdx) && OwnedByPlayer(g_attrSet)) return;
    if (g_attrSet) {
        g_attrSet = nullptr;
        ResetAttrState();
    }
    if (!g_pawn || GetTickCount() - g_lastAttrScan < 1000) return;
    g_lastAttrScan = GetTickCount();
    if (!g_attrClass) g_attrClass = ue::FindClass("HnSUberAttributeSet");
    if (!g_attrClass) return;
    int n = ue::NumObjects();
    for (int i = 0; i < n; ++i) {
        void* o = ue::ObjectAt(i);
        if (!o || ue::IsCDO(o) || !ue::IsA(o, g_attrClass) || !OwnedByPlayer(o)) continue;
        g_attrSet = o;
        g_attrIdx = i;
        ResetAttrState();
        Log("attribute set found: %s", ue::PathOf(o).c_str());
        return;
    }
}

AttrData* Attr(const char* name) {
    if (!g_attrSet || !name) return nullptr;
    auto it = g_attrOffsets.find(name);
    int off;
    if (it == g_attrOffsets.end()) {
        auto p = ue::FindProperty(g_attrClass, name);
        off = (p && p.type == "StructProperty" && ResolveAttrLayout(p)) ? p.offset : -1;
        if (off < 0) Log("attribute %s not usable (found=%d type=%s size=%d)", name, (bool)p, p.type.c_str(), p.elementSize);
        g_attrOffsets[name] = off;
    } else {
        off = it->second;
    }
    return off >= 0 ? reinterpret_cast<AttrData*>((uint8_t*)g_attrSet + off) : nullptr;
}

// After writing an attribute directly, call the attribute set's OnRep_<Name>(Old) like
// replication would: GAS then re-evaluates the value and broadcasts the change, so
// derived stats (TotalMovementSpeed, TotalAnimSpeedMultiplier, ...) and listeners
// update immediately. Verified in game: without it TotalMovementSpeed stayed stale.
void Notify(const char* name, float oldBase, float oldCur) {
    if (!g_attrSet) return;
    std::string fnName = std::string("OnRep_") + name;
    void* fn = ue::FindFunction(ue::ClassOf(g_attrSet), fnName.c_str());
    if (!fn) return;
    std::vector<uint8_t> buf(ue::StructSize(fn) + 16, 0);
    uint8_t old[16] = {};
    *reinterpret_cast<float*>(old + g_baseOff) = oldBase;
    *reinterpret_cast<float*>(old + g_curOff) = oldCur;
    memcpy(buf.data(), old, g_attrSize > 0 && g_attrSize <= 16 ? g_attrSize : 8);  // single parameter at offset 0
    ue::CallFunction(g_attrSet, fn, buf.data());
}

// Writes base/current and notifies GAS.
void WriteAttr(const char* name, AttrData* a, float base, float cur) {
    float ob = B(a), oc = C(a);
    B(a) = base;
    C(a) = cur;
    Notify(name, ob, oc);
}

// ---------------------------------------------------------------------------
// Features
// ---------------------------------------------------------------------------
void ApplyMultipliers() {
    for (int i = 0; i < kMultCount; ++i) {
        AttrData* a = Attr(g_mults[i].attr);
        if (!a) continue;
        MultState& s = g_multState[i];
        float want = g_enabled ? g_mults[i].value * HostFactor(g_mults[i].attr) : 1.0f;
        if (g_enabled && InCity() && !strcmp(g_mults[i].attr, "MovementSpeedMultiplier")) want *= g_hubSpeed;
        bool managing = !std::isnan(s.last);
        if (managing && fabsf(B(a) - s.last) > 1e-4f) {  // the game rewrote the base value
            managing = false;
            s.last = NAN;
        }
        if (!managing) {
            s.orig = B(a);
            // Scale relative to the base value; if the base is 0 the attribute is fed by
            // gameplay effects, so scale relative to the current value instead.
            s.ref = fabsf(B(a)) > 1e-4f ? B(a) : (fabsf(C(a)) > 1e-4f ? C(a) : 1.0f);
        }
        if (fabsf(want - 1.0f) < 1e-4f) {
            if (managing) {
                float d = s.orig - B(a);
                WriteAttr(g_mults[i].attr, a, s.orig, C(a) + d);
                s.last = NAN;
            }
            continue;
        }
        float target = s.orig + (want - 1.0f) * s.ref;
        if (fabsf(B(a) - target) > 1e-5f) {
            float d = target - B(a);
            WriteAttr(g_mults[i].attr, a, target, C(a) + d);
        }
        s.last = target;
    }
}

void ApplyLocks() {
    if (!g_enabled) return;
    for (auto& l : g_locks) {
        if (!l.on) continue;
        for (auto& pair : l.attrs) {
            AttrData* v = Attr(pair[0]);
            AttrData* m = Attr(pair[1]);
            if (v && m && C(m) > 0 && C(v) < C(m)) WriteAttr(pair[0], v, C(m), C(m));
        }
    }
}

// ---------------------------------------------------------------------------
// City XP. The city level is not a player attribute: XP per event comes from the
// m_XpGains table of the city rules data assets. We scale every gain value in memory
// (keeping the originals) so the game awards and levels up the city on its own.
// ---------------------------------------------------------------------------
float g_cityXp = 1.0f;

struct ScaledFloat {
    float* p;
    float orig;
    float last;
};
struct CityAsset {
    void* obj;
    int idx;
    std::vector<ScaledFloat> vals;
};
std::vector<CityAsset> g_cityAssets;

struct SparseRaw {  // TSparseArray (the first 0x38 bytes of TSet/TMap)
    uint8_t* data;
    int32_t num, max;
    uint32_t inlineBits[4];
    void* secondaryBits;
    int32_t numBits, maxBits;
    int32_t firstFree, numFree;
};
static_assert(sizeof(SparseRaw) == 0x38, "TSparseArray layout");

bool PlausibleGain(float v) { return std::isfinite(v) && v >= 0.0f && v <= 1e6f; }

bool CollectCityGains(void* obj, std::vector<ScaledFloat>& out) {
    void* gainStruct = ue::FindObject("HnSShared_City_XpGain");
    void* rangeStruct = ue::FindObject("HnSShared_City_XpGain_Range");
    auto pMap = ue::FindProperty(ue::ClassOf(obj), "m_XpGains");
    if (!gainStruct || !rangeStruct || !pMap) return false;
    auto pGain = ue::FindProperty(gainStruct, "m_fGain");
    auto pRarity = ue::FindProperty(gainStruct, "m_GainByRarity");
    auto pRange = ue::FindProperty(gainStruct, "m_GainByRange");
    auto pRangeValue = ue::FindProperty(rangeStruct, "m_fValue");
    if (!pGain || !pRarity || !pRange || !pRangeValue) return false;
    int gainSize = ue::StructSize(gainStruct), rangeSize = ue::StructSize(rangeStruct);

    SparseRaw map{};
    if (!Read((uint8_t*)obj + pMap.offset, map) || map.numFree != 0 || map.num <= 0 || map.num > 64 || !LooksLikePtr(map.data))
        return false;
    // TSetElement<TPair<uint8 enum, FHnSShared_City_XpGain>>: value 8-aligned after the key, then 2 hash ints.
    const int valueOff = 8, stride = ((valueOff + gainSize + 7) & ~7) + 8;
    for (int i = 0; i < map.num; ++i) {
        uint8_t* v = map.data + i * stride + valueOff;
        float g;
        if (!Read(v + pGain.offset, g) || !PlausibleGain(g)) return false;
        out.push_back({(float*)(v + pGain.offset), g, g});

        SparseRaw rar{};  // TMap<enum, float>: pair {key, float@4} + 2 hash ints = 16 bytes
        if (Read(v + pRarity.offset, rar) && rar.num > 0 && rar.num <= 32 && rar.numFree == 0 && LooksLikePtr(rar.data))
            for (int k = 0; k < rar.num; ++k) {
                float* f = (float*)(rar.data + k * 16 + 4);
                float x;
                if (Read(f, x) && PlausibleGain(x)) out.push_back({f, x, x});
            }
        ue::TArrayRaw ranges{};
        if (Read(v + pRange.offset, ranges) && ranges.num > 0 && ranges.num <= 64 && LooksLikePtr(ranges.data))
            for (int k = 0; k < ranges.num; ++k) {
                float* f = (float*)((uint8_t*)ranges.data + k * rangeSize + pRangeValue.offset);
                float x;
                if (Read(f, x) && PlausibleGain(x)) out.push_back({f, x, x});
            }
    }
    return !out.empty();
}

// ---------------------------------------------------------------------------
// Incremental object scan. Instead of walking the whole object array (~500k
// objects) in one go, every tick looks at the next slice, so there is no hitch.
// Objects are sorted into categories by their class (memoized per class).
// ---------------------------------------------------------------------------
enum Cat : uint8_t { CatNone = 0, CatCity, CatRecipe, CatLevelUp, CatPotionGA, CatCount };
struct Tracked {
    void* obj;
    int idx;
    void* cls;  // class at the time it was found; a reused slot gets a different class
};
std::vector<Tracked> g_found[CatCount];
struct CatMemo {
    uint8_t cat;
    int idx;  // object index of the class, to notice when it was unloaded and the address reused
};
std::unordered_map<void*, CatMemo> g_catMemo;
int g_scanCursor = 0;
int g_scanPasses = 0;

uint8_t ClassCategory(void* cls) {
    auto it = g_catMemo.find(cls);
    if (it != g_catMemo.end()) {
        if (ue::ObjectAt(it->second.idx) == cls) return it->second.cat;
        g_catMemo.erase(it);  // class was unloaded and something else lives at that address now
    }
    int clsIdx = ReadOr<int32_t>((uint8_t*)cls + ue::off.ObjIndex, -1);
    if (clsIdx < 0 || ue::ObjectAt(clsIdx) != cls) return CatNone;  // not a live class object
    static void* city1 = ue::FindClass("HnSShared_GameData_CityRules");
    static void* city2 = ue::FindClass("HnSCityHubRulesDataAsset");
    static void* recipeGen = ue::FindClass("HnSRecipeGenerator");
    uint8_t c = CatNone;
    std::string n = ue::NameOf(cls);
    if ((city1 && ue::IsChildOf(cls, city1)) || (city2 && ue::IsChildOf(cls, city2)))
        c = CatCity;
    else if (recipeGen && ue::IsChildOf(cls, recipeGen))
        c = CatRecipe;
    else if (n.rfind("WBP_LevelUpNotification", 0) == 0)
        c = CatLevelUp;
    else if (n.rfind("GA_POTION_BASE_", 0) == 0)
        c = CatPotionGA;
    g_catMemo[cls] = {c, clsIdx};
    return c;
}

void AddFound(int cat, void* o) {
    for (auto& t : g_found[cat])
        if (t.obj == o) return;
    g_found[cat].push_back({o, ue::IndexOf(o), ue::ClassOf(o)});
}

// Classes watched by mods (by name, including subclasses). Matching is done once per
// class by walking its super chain, so no expensive FindClass calls are needed.
struct Watch {
    std::string name;
    std::vector<Tracked> found;
};
std::vector<Watch> g_watches;
struct WatchMemo {
    int clsIdx;
    std::vector<uint16_t> watches;
};
std::unordered_map<void*, WatchMemo> g_watchMemo;

const std::vector<uint16_t>& WatchesOf(void* cls) {
    static const std::vector<uint16_t> none;
    auto it = g_watchMemo.find(cls);
    if (it != g_watchMemo.end()) {
        if (ue::ObjectAt(it->second.clsIdx) == cls) return it->second.watches;
        g_watchMemo.erase(it);
    }
    int clsIdx = ReadOr<int32_t>((uint8_t*)cls + ue::off.ObjIndex, -1);
    if (clsIdx < 0 || ue::ObjectAt(clsIdx) != cls) return none;
    WatchMemo m{clsIdx, {}};
    int depth = 0;
    for (void* c = cls; c && depth < 32; c = ue::SuperOf(c), ++depth) {
        std::string n = ue::NameOf(c);
        for (size_t w = 0; w < g_watches.size(); ++w)
            if (g_watches[w].name == n) m.watches.push_back((uint16_t)w);
    }
    return g_watchMemo.emplace(cls, std::move(m)).first->second.watches;
}

void IncrementalScan() {
    for (auto& w : g_watches)
        for (size_t i = 0; i < w.found.size();)
            if (!ue::IsLive(w.found[i].obj, w.found[i].idx) || ue::ClassOf(w.found[i].obj) != w.found[i].cls)
                w.found.erase(w.found.begin() + i);
            else ++i;
    for (auto& v : g_found)
        for (size_t i = 0; i < v.size();)
            if (!ue::IsLive(v[i].obj, v[i].idx) || ue::ClassOf(v[i].obj) != v[i].cls) v.erase(v.begin() + i);
            else ++i;
    int n = ue::NumObjects();
    if (g_scanCursor >= n) g_scanCursor = 0;
    int end = std::min(n, g_scanCursor + 40000);
    for (int i = g_scanCursor; i < end; ++i) {
        void* o = ue::ObjectAt(i);
        if (!o) continue;
        void* cls = ue::ClassOf(o);
        if (!cls) continue;
        if (!g_watches.empty() && !ue::IsCDO(o))
            for (uint16_t w : WatchesOf(cls)) {
                auto& f = g_watches[w].found;
                bool known = false;
                for (auto& t : f) known |= t.obj == o;
                if (!known) f.push_back({o, i, cls});
            }
        uint8_t cat = ClassCategory(cls);
        if (!cat) continue;
        if (cat == CatPotionGA) {
            if (ue::IsCDO(o)) AddFound(cat, cls);  // we want the ability class itself
        } else if (!ue::IsCDO(o)) {
            AddFound(cat, o);
        }
    }
    g_scanCursor = end >= n ? 0 : end;
    if (g_scanCursor == 0) ++g_scanPasses;
}

void ApplyCityXp() {
    // Drop assets that were unloaded.
    for (size_t i = 0; i < g_cityAssets.size();)
        if (!ue::IsLive(g_cityAssets[i].obj, g_cityAssets[i].idx)) g_cityAssets.erase(g_cityAssets.begin() + i);
        else ++i;
    {
        for (auto& t : g_found[CatCity]) {
            void* o = t.obj;
            int i = t.idx;
            bool known = false;
            for (auto& a : g_cityAssets) known |= a.obj == o;
            if (known) continue;
            CityAsset a{o, i, {}};
            if (CollectCityGains(o, a.vals)) {
                Log("city XP table found: %s (%zu gain values, first = %g)", ue::PathOf(o).c_str(), a.vals.size(),
                    a.vals[0].orig);
                g_cityAssets.push_back(std::move(a));
            } else {
                Log("city XP table not readable: %s", ue::PathOf(o).c_str());
                g_cityAssets.push_back({o, i, {}});  // remember it so we don't retry every scan
            }
        }
    }
    // Scaled through the shared registry (mods may change the same table).
    float want = g_enabled ? g_cityXp : 1.0f;
    if (fabsf(want - 1.0f) > 1e-4f)
        for (auto& a : g_cityAssets) {
            if (a.vals.empty()) continue;
            for (const char* path : {"m_XpGains.*.m_fGain", "m_XpGains.*.m_GainByRarity.*", "m_XpGains.*.m_GainByRange.*.m_fValue"})
                HostContribute(a.obj, path, DML_OP_MULTIPLY, want, "Dragonkin Plus: City XP");
        }
}

// ---------------------------------------------------------------------------
// Waypoints (teleporters). The full list of teleporter tags lives in the shared
// game data asset; activation goes through the game's own teleportation subsystem.
// ---------------------------------------------------------------------------
void* TeleportSubsystem() {
    static void* sub = nullptr;
    static int idx = -1;
    if (!ue::IsLive(sub, idx)) {
        void* cls = ue::FindClass("HnSTeleportationSubsystem");
        sub = cls ? ue::FindFirstInstance(cls) : nullptr;
        idx = sub ? ue::IndexOf(sub) : -1;
    }
    return sub;
}

std::vector<uint64_t> AllTeleporterTags() {
    std::vector<uint64_t> tags;
    void* cls = ue::FindClass("HnSShared_GameData_Teleporters");
    void* data = cls ? ue::FindFirstInstance(cls) : nullptr;
    auto p = data ? ue::FindProperty(cls, "m_AllTeleporters") : ue::PropInfo{};
    if (!p) return tags;
    ue::ForEachSparseElement(
        (uint8_t*)data + p.offset, ue::SetElementStride(8),
        [](uint8_t* e, void* u) { static_cast<std::vector<uint64_t>*>(u)->push_back(*reinterpret_cast<uint64_t*>(e)); }, &tags);
    return tags;
}

bool CallReturningBool(void* obj, const char* fn, uint64_t tag, bool& result) {
    std::vector<uint8_t> out;
    if (!ue::CallByName(obj, fn, {{"TeleportationTag", &tag}}, &out)) return false;
    auto r = ue::FindProperty(ue::FindFunction(ue::ClassOf(obj), fn), "ReturnValue");
    result = r && out[r.offset] != 0;
    return (bool)r;
}

struct WaypointCount {
    int total = 0, active = 0;
};
WaypointCount g_waypoints;

WaypointCount CountWaypoints() {
    WaypointCount c;
    void* sub = TeleportSubsystem();
    for (uint64_t tag : AllTeleporterTags()) {
        bool on = false;
        ++c.total;
        if (sub && CallReturningBool(sub, "IsTeleporterActivated_TagOnly", tag, on) && on) ++c.active;
    }
    return c;
}

void UnlockAllWaypoints() {
    void* sub = TeleportSubsystem();
    if (!sub) return SetMessage(T("Teleport system not found (enter the game)."));
    int added = 0;
    for (uint64_t tag : AllTeleporterTags()) {
        bool on = false;
        if (CallReturningBool(sub, "IsTeleporterActivated_TagOnly", tag, on) && on) continue;
        if (ue::CallByName(sub, "TryActivateTeleporter", {{"TeleportationTag", &tag}})) ++added;
    }
    g_waypoints = CountWaypoints();
    SetMessage(F("Waypoints: %d/%d unlocked (%d newly tried)", g_waypoints.active, g_waypoints.total, added));
}

void OpenTeleportMap() {
    if (!g_pc || !ue::CallByName(g_pc, "ToggleTeleportWorldMap", {}))
        SetMessage(T("Could not open the teleport map (may not be supported on this screen)."));
}

std::atomic<int> g_forcedTab{-1};

void ApplyGameSpeed() {
    void* level = GetObj(g_world, "World", "PersistentLevel");
    void* ws = GetObj(level, "Level", "WorldSettings");
    int off = PropOff("WorldSettings", "TimeDilation");
    if (!ws || off < 0) return;
    float& td = ue::At<float>(ws, off);
    if (g_enabled && g_speedOn) {
        td = g_gameSpeed;
        g_speedApplied = true;
    } else if (g_speedApplied) {
        td = 1.0f;
        g_speedApplied = false;
    }
}

// ---------------------------------------------------------------------------
// QoL features
// ---------------------------------------------------------------------------
float g_camZoom = 1.0f;     // camera distance multiplier
float g_enemyHp = 1.0f;     // enemy max health multiplier
float g_enemyDmg = 1.0f;    // enemy damage multiplier
bool g_autoPotion = false;  // drink a potion below g_autoPotionPct health
float g_autoPotionPct = 35.0f;
bool g_hideLevelUp = false;  // hide the level-up notification widgets
float g_craftGold = 1.0f;    // gold cost of crafting recipes
int g_invMain = 0;           // player bag size (0 = game default)
int g_invChest = 0;          // city/account chest size (0 = game default)

void SetStatusFlag(const std::function<void(Status&)>& f) {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    f(g_status);
}

bool InCity() {
    static void* lastWorld = nullptr;
    static bool city = false;
    if (g_world != lastWorld) {
        lastWorld = g_world;
        city = g_world && ue::NameOf(g_world).find("City") != std::string::npos;
    }
    return city;
}

std::string WorldName() { return g_world ? ue::NameOf(g_world) : std::string(); }

// Calls a function whose out-parameters allocate memory (TArray, structs with
// arrays) with a buffer that lives as long as the caller wants: the function
// assigns into the previous result, which frees/reuses the old allocation
// instead of leaking a new one on every call.
struct PersistentCall {
    void* fn = nullptr;
    void* fnClass = nullptr;
    std::vector<uint8_t> buf;
    bool Prepare(void* obj, const char* name) {
        void* cls = ue::ClassOf(obj);
        if (fn && fnClass == cls) return true;
        if (fn && fnClass != cls) return false;  // only ever used with one class
        fn = ue::FindFunction(cls, name);
        fnClass = cls;
        if (!fn) return false;
        buf.assign(ue::StructSize(fn) + 64, 0);
        return true;
    }
    ue::PropInfo Param(const char* name) { return fn ? ue::FindProperty(fn, name) : ue::PropInfo{}; }
};

bool ActorVec(void* actor, const char* fn, double out[3]) {
    std::vector<uint8_t> o;
    if (!actor || !ue::CallByName(actor, fn, {}, &o)) return false;
    auto r = ue::FindProperty(ue::FindFunction(ue::ClassOf(actor), fn), "ReturnValue");
    if (!r) return false;
    memcpy(out, o.data() + r.offset, 24);
    return true;
}

bool TeleportPawn(const double p[3]) {
    double rot[3] = {};
    ActorVec(g_pawn, "K2_GetActorRotation", rot);
    std::vector<uint8_t> o;
    return g_pawn && ue::CallByName(g_pawn, "K2_TeleportTo", {{"DestLocation", p}, {"DestRotation", rot}}, &o);
}

// --- Camera ----------------------------------------------------------------
void ApplyCamera() {
    static void* view = nullptr;
    static int viewIdx = -1;
    static DWORD lastFind = 0;
    if (!ue::IsLive(view, viewIdx)) {
        view = nullptr;
        if (GetTickCount() - lastFind < 2000) return;
        lastFind = GetTickCount();
        static void* cls = ue::FindClass("HnSCameraView");
        view = cls ? ue::FindFirstInstance(cls) : nullptr;
        viewIdx = view ? ue::IndexOf(view) : -1;
        if (!view) return;
    }
    void* boom = GetObj(view, "HnSCameraView", "m_pCameraBoom");
    int off = PropOff("SpringArmComponent", "TargetArmLength");
    if (!boom || off < 0) return;
    // Written through the shared registry (also used by mods), which keeps the game's
    // own distance and restores it at 1x.
    float want = g_enabled ? std::clamp(g_camZoom, 0.25f, 4.0f) : 1.0f;
    if (fabsf(want - 1.0f) > 1e-3f)
        HostContribute(boom, "TargetArmLength", DML_OP_MULTIPLY, want, "Dragonkin Plus: Camera distance");
    float len = ue::At<float>(boom, off);
    SetStatusFlag([&](Status& s) { s.camLen = len; });
}

// --- Gameplay attributes through the ability system --------------------------
// FGameplayAttribute values (0x38 bytes: name, field path, owner) copied from the
// game's own list, used to call ServerModifyAttributeValue / GetFloatAttribute.
std::unordered_map<std::string, std::vector<uint8_t>> g_gaAttrs;

void* PawnASC(void* pawn) { return GetObj(pawn, "HnSCharacterBase", "m_pAbilitySystemComponent"); }

bool LoadGameplayAttributes(void* asc) {
    if (!g_gaAttrs.empty()) return true;
    static DWORD lastTry = 0;
    if (!asc || !g_attrClass || GetTickCount() - lastTry < 3000) return false;
    lastTry = GetTickCount();
    // FProperty* -> attribute name, for every attribute of the uber attribute set.
    std::unordered_map<void*, std::string> byField;
    for (void* c = g_attrClass; c; c = ue::SuperOf(c)) {
        ue::ForEachProperty(
            c, [](void* f, void* u) { (*static_cast<std::unordered_map<void*, std::string>*>(u))[f] = ue::FieldName(f); }, &byField);
        if (ue::NameOf(c) == "AttributeSet") break;
    }
    std::vector<uint8_t> out;  // the returned array is kept forever (one-time, small)
    if (!ue::CallByName(asc, "GetGameplayAttributes", {}, &out)) {
        Log("GetGameplayAttributes failed");
        return false;
    }
    auto p = ue::FindProperty(ue::FindFunction(ue::ClassOf(asc), "GetGameplayAttributes"), "OutGameplayAttributes");
    void* inner = p ? ue::ArrayInner(p.field) : nullptr;
    int esz = inner ? ue::DescribeProperty(inner).elementSize : 0;
    ue::TArrayRaw a = p ? ue::At<ue::TArrayRaw>(out.data(), p.offset) : ue::TArrayRaw{};
    if (esz < 0x10 || esz > 0x100 || a.num <= 0 || a.num > 4000 || !LooksLikePtr(a.data)) {
        Log("GetGameplayAttributes: unexpected result (element size %d, count %d)", esz, a.num);
        return false;
    }
    for (int i = 0; i < a.num; ++i) {
        uint8_t* e = (uint8_t*)a.data + i * esz;
        for (int o = 0; o + 8 <= esz; o += 8) {
            auto it = byField.find(ReadOr<void*>(e + o, nullptr));
            if (it == byField.end()) continue;
            g_gaAttrs[it->second] = std::vector<uint8_t>(e, e + esz);
            break;
        }
    }
    Log("gameplay attributes: %zu of %d matched (element size 0x%X)", g_gaAttrs.size(), a.num, esz);
    return !g_gaAttrs.empty();
}

enum ModOp : uint8_t { OpAdd = 0, OpMultiply = 1, OpDivide = 2, OpOverride = 3 };

bool ModifyAttr(void* asc, const char* name, float value, ModOp op) {
    auto it = g_gaAttrs.find(name);
    if (!asc || it == g_gaAttrs.end()) return false;
    void* fn = ue::FindFunction(ue::ClassOf(asc), "ServerModifyAttributeValue");
    if (!fn) return false;
    auto pA = ue::FindProperty(fn, "Attribute"), pV = ue::FindProperty(fn, "Value"), pO = ue::FindProperty(fn, "ModifierOp");
    if (!pA || !pV || !pO) return false;
    std::vector<uint8_t> buf(ue::StructSize(fn) + 16, 0);
    memcpy(buf.data() + pA.offset, it->second.data(), std::min<size_t>(it->second.size(), pA.elementSize));
    ue::At<float>(buf.data(), pV.offset) = value;
    buf[pO.offset] = op;
    return ue::CallFunction(asc, fn, buf.data());
}

bool ReadAttr(void* asc, const char* name, float& v) {
    auto it = g_gaAttrs.find(name);
    if (!asc || it == g_gaAttrs.end()) return false;
    void* fn = ue::FindFunction(ue::ClassOf(asc), "GetFloatAttribute_OrDefault");
    if (!fn) return false;
    auto pA = ue::FindProperty(fn, "Attribute"), pR = ue::FindProperty(fn, "ReturnValue");
    std::vector<uint8_t> buf(ue::StructSize(fn) + 16, 0);
    memcpy(buf.data() + pA.offset, it->second.data(), std::min<size_t>(it->second.size(), pA.elementSize));
    if (!ue::CallFunction(asc, fn, buf.data())) return false;
    v = ue::At<float>(buf.data(), pR.offset);
    return true;
}

// --- Enemies -----------------------------------------------------------------
struct EnemyRec {
    int ascIdx;
    bool hostile;
    float hp = 1.0f, dmg = 1.0f;          // factors currently applied
    float baseHp = 1.0f, baseDmg = 1.0f;  // the game's own base values (elites/bosses may differ from 1)
};

// Base value of an attribute straight from the actor's attribute set.
bool ReadBase(void* asc, const char* name, float& out) {
    uint8_t asserts = 0;
    std::vector<uint8_t> o;
    if (!ue::CallByName(asc, "GetUberAttributeSet", {{"AssertsEnabled", &asserts}}, &o)) return false;
    auto r = ue::FindProperty(ue::FindFunction(ue::ClassOf(asc), "GetUberAttributeSet"), "ReturnValue");
    void* set = r ? ue::At<void*>(o.data(), r.offset) : nullptr;
    auto p = set && g_attrClass ? ue::FindProperty(g_attrClass, name) : ue::PropInfo{};
    if (!p || g_baseOff < 0) return false;
    return Read((uint8_t*)set + p.offset + g_baseOff, out);
}
std::unordered_map<void*, EnemyRec> g_enemies;  // key: ability system component
PersistentCall g_allActorsCall;

std::vector<void*> AllActorsOf(void* cls) {
    std::vector<void*> r;
    static void* gs = nullptr;
    static int gsIdx = -1;
    if (!ue::IsLive(gs, gsIdx)) {
        gs = ue::FindObject("Default__GameplayStatics");
        gsIdx = gs ? ue::IndexOf(gs) : -1;
    }
    if (!gs || !cls || !g_world || !g_allActorsCall.Prepare(gs, "GetAllActorsOfClass")) return r;
    auto pW = g_allActorsCall.Param("WorldContextObject"), pC = g_allActorsCall.Param("ActorClass"),
         pO = g_allActorsCall.Param("OutActors");
    if (!pW || !pC || !pO) return r;
    uint8_t* b = g_allActorsCall.buf.data();
    ue::At<void*>(b, pW.offset) = g_world;
    ue::At<void*>(b, pC.offset) = cls;
    if (!ue::CallFunction(gs, g_allActorsCall.fn, b)) return r;
    ue::TArrayRaw a = ue::At<ue::TArrayRaw>(b, pO.offset);
    if (a.num <= 0 || a.num > 20000 || !LooksLikePtr(a.data)) return r;
    r.assign((void**)a.data, (void**)a.data + a.num);
    return r;
}

bool CallBool(void* obj, const char* fn, bool& result) {
    std::vector<uint8_t> o;
    if (!ue::CallByName(obj, fn, {}, &o)) return false;
    auto r = ue::FindProperty(ue::FindFunction(ue::ClassOf(obj), fn), "ReturnValue");
    if (!r) return false;
    result = o[r.offset] != 0;
    return true;
}

void ApplyEnemies() {
    static DWORD last = 0;
    if (GetTickCount() - last < 600) return;
    last = GetTickCount();
    float wantHp = g_enabled ? g_enemyHp : 1.0f, wantDmg = g_enabled ? g_enemyDmg : 1.0f;
    bool active = fabsf(wantHp - 1.0f) > 1e-3f || fabsf(wantDmg - 1.0f) > 1e-3f;
    bool anyApplied = false;
    for (auto& e : g_enemies) anyApplied |= e.second.hp != 1.0f || e.second.dmg != 1.0f;
    if (!active && !anyApplied) {
        SetStatusFlag([](Status& s) { s.enemiesTracked = s.enemiesScaled = 0; });
        return;
    }
    if (!LoadGameplayAttributes(PawnASC(g_pawn))) return;
    static void* npcClass = ue::FindClass("HnSNPCBase");
    std::unordered_map<void*, EnemyRec> next;
    int scaled = 0;
    for (void* actor : AllActorsOf(npcClass)) {
        void* asc = PawnASC(actor);
        if (!asc) continue;
        EnemyRec rec;
        auto it = g_enemies.find(asc);
        if (it != g_enemies.end() && ue::IsLive(asc, it->second.ascIdx)) {
            rec = it->second;
        } else {
            bool hostile = false, dead = false;
            CallBool(actor, "IsHostile", hostile);
            CallBool(actor, "IsDead", dead);
            if (dead) continue;
            rec = EnemyRec{ue::IndexOf(asc), hostile};
            if (hostile && (!ReadBase(asc, "HealthMaxMultiplier", rec.baseHp) || !ReadBase(asc, "FinalDamageMultiplier", rec.baseDmg)))
                continue;  // not ready yet
        }
        if (rec.hostile) {
            if (fabsf(rec.hp - wantHp) > 1e-4f) {
                float hp = 0, maxBefore = 0;
                ReadAttr(asc, "Health", hp);
                ReadAttr(asc, "TotalMaxHealth", maxBefore);
                // The game's "multiply" op overwrites in this build, so write base x factor
                // (keeps elite/boss multipliers; 1x writes the original back).
                if (ModifyAttr(asc, "HealthMaxMultiplier", rec.baseHp * wantHp, OpOverride)) {
                    float maxAfter = 0;
                    ReadAttr(asc, "TotalMaxHealth", maxAfter);
                    if (maxBefore > 0 && maxAfter > 0 && hp > 0)
                        ModifyAttr(asc, "Health", std::min(maxAfter, hp / maxBefore * maxAfter), OpOverride);
                    rec.hp = wantHp;
                }
            }
            if (fabsf(rec.dmg - wantDmg) > 1e-4f && ModifyAttr(asc, "FinalDamageMultiplier", rec.baseDmg * wantDmg, OpOverride))
                rec.dmg = wantDmg;
            if (rec.hp != 1.0f || rec.dmg != 1.0f) ++scaled;
        }
        next[asc] = rec;
    }
    g_enemies.swap(next);
    int tracked = (int)g_enemies.size();
    SetStatusFlag([&](Status& s) {
        s.enemiesTracked = tracked;
        s.enemiesScaled = scaled;
    });
}

// --- Positions: unstuck and saved spots --------------------------------------
struct PosSample {
    double p[3];
    DWORD t;
};
std::deque<PosSample> g_posHist;
struct SavedPos {
    bool set = false;
    double p[3] = {};
};
std::unordered_map<std::string, std::array<SavedPos, 3>> g_savedPos;  // per map

double Dist(const double a[3], const double b[3]) {
    double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrt(dx * dx + dy * dy + dz * dz);
}

void TrackPosition() {
    static DWORD last = 0;
    static void* lastWorld = nullptr;
    if (g_world != lastWorld) {
        lastWorld = g_world;
        g_posHist.clear();
    }
    if (!g_pawn || GetTickCount() - last < 1000) return;
    last = GetTickCount();
    double p[3];
    if (!ActorVec(g_pawn, "K2_GetActorLocation", p)) return;
    if (!g_posHist.empty() && Dist(p, g_posHist.back().p) < 60) return;  // standing still
    g_posHist.push_back({{p[0], p[1], p[2]}, GetTickCount()});
    if (g_posHist.size() > 60) g_posHist.pop_front();
}

void Unstuck() {
    double cur[3];
    if (!ActorVec(g_pawn, "K2_GetActorLocation", cur)) return SetMessage(T("Character not found."));
    // Newest recorded spot that is clearly away from where we are now.
    for (auto it = g_posHist.rbegin(); it != g_posHist.rend(); ++it) {
        if (Dist(it->p, cur) < 250) continue;
        if (TeleportPawn(it->p)) {
            int ago = (int)((GetTickCount() - it->t) / 1000);
            return SetMessage(F("Unstuck: returned to the spot from %d s ago.", ago));
        }
    }
    double up[3] = {cur[0], cur[1], cur[2] + 300};
    SetMessage(TeleportPawn(up) ? T("Unstuck: character moved up a little.") : T("Teleport failed."));
}

std::wstring PosKey(int slot) {
    std::string w = WorldName();
    return std::wstring(w.begin(), w.end()) + L"_" + std::to_wstring(slot + 1);
}

std::array<SavedPos, 3>& SlotsForWorld() {
    std::string w = WorldName();
    auto it = g_savedPos.find(w);
    if (it != g_savedPos.end()) return it->second;
    auto& slots = g_savedPos[w];
    for (int i = 0; i < 3; ++i) {
        wchar_t buf[128];
        GetPrivateProfileStringW(L"Positions", PosKey(i).c_str(), L"", buf, 128, IniPath().c_str());
        if (buf[0] && swscanf_s(buf, L"%lf %lf %lf", &slots[i].p[0], &slots[i].p[1], &slots[i].p[2]) == 3) slots[i].set = true;
    }
    return slots;
}

void SavePosition(int slot) {
    double p[3];
    if (slot < 0 || slot > 2 || !ActorVec(g_pawn, "K2_GetActorLocation", p)) return SetMessage(T("Could not save the position."));
    auto& s = SlotsForWorld()[slot];
    s.set = true;
    memcpy(s.p, p, sizeof(p));
    wchar_t buf[128];
    swprintf_s(buf, L"%.1f %.1f %.1f", p[0], p[1], p[2]);
    WritePrivateProfileStringW(L"Positions", PosKey(slot).c_str(), buf, IniPath().c_str());
    SetMessage(F("Position %d saved (%s).", slot + 1, WorldName().c_str()));
}

void LoadPosition(int slot) {
    if (slot < 0 || slot > 2) return;
    auto& s = SlotsForWorld()[slot];
    if (!s.set) return SetMessage(F("Position %d is not saved on this map.", slot + 1));
    SetMessage(TeleportPawn(s.p) ? F("Teleported to position %d.", slot + 1) : std::string(T("Teleport failed.")));
}

// --- Inventory ---------------------------------------------------------------
void* PlayerInventory() {
    if (!g_ps) return nullptr;
    auto p = ue::FindProperty(ue::ClassOf(g_ps), "m_PlayerInventory");
    void* inv = nullptr;
    if (!p || !Read((uint8_t*)g_ps + p.offset, inv) || !LooksLikePtr(inv)) return nullptr;
    return inv;
}

int GoldCount(void* inv) {
    static uint32_t goldName = 0;
    if (!goldName && !ue::FNameFromString("Items.Currencies.Gold", goldName)) return -1;
    uint32_t id[2] = {goldName, 0};
    std::vector<uint8_t> o;
    if (!ue::CallByName(inv, "GetCountableCount", {{"CountableID", id}}, &o)) return -1;
    auto r = ue::FindProperty(ue::FindFunction(ue::ClassOf(inv), "GetCountableCount"), "ReturnValue");
    return r ? ue::At<int32_t>(o.data(), r.offset) : -1;
}

void SellJunk() {
    void* inv = PlayerInventory();
    void* lib = ue::FindObject("Default__HnSItemSellLibrary");
    if (!inv || !lib) return SetMessage(T("Inventory not found (enter the game)."));
    int gold0 = GoldCount(inv), sold = 0;
    for (const char* getter : {"GetAllSellableEquipment_MarkedAsJunk", "GetAllSellableFragments_MarkedAsJunk"}) {
        std::vector<uint8_t> o;
        if (!ue::CallByName(lib, getter, {{"pInventoryComponent", &inv}}, &o)) continue;
        auto r = ue::FindProperty(ue::FindFunction(ue::ClassOf(lib), getter), "ReturnValue");
        if (!r) continue;
        ue::TArrayRaw items = ue::At<ue::TArrayRaw>(o.data(), r.offset);
        if (items.num <= 0) continue;
        if (ue::CallByName(lib, "SellItems", {{"Items", &items}, {"pInventoryComponent", &inv}})) sold += items.num;
    }
    int gold1 = GoldCount(inv);
    SetMessage(sold ? F("Sold junk: %d items, +%d gold.", sold, gold1 - gold0)
                    : std::string(T("No junk-marked items (mark items as junk in the inventory).")));
}

// --- Crafting gold cost ------------------------------------------------------
// The craft rule assets are loaded from disk at the moment a recipe is built and
// dropped right after, so scaling them does nothing. Instead the finished recipe
// of every live recipe generator (the open craft menu) is scaled: its gold
// requirement is what the craft checks and takes.
struct CraftVal {
    int32_t orig, last;
};
std::unordered_map<int32_t*, CraftVal> g_craftVals;

void ApplyCraftCost() {
    static DWORD last = 0;
    if (GetTickCount() - last < 100) return;
    last = GetTickCount();
    static uint32_t goldName = 0;
    if (!goldName) ue::FNameFromString("Items.Currencies.Gold", goldName);
    static void* reqGroupS = ue::FindObject("HnSRecipeRequirementGroup");
    static void* countReqS = ue::FindObject("HnSCountableRecipeRequirement");
    static int oRecipe = PropOff("HnSRecipeGenerator", "m_GeneratedRecipe");
    static int oCountables = reqGroupS ? ue::FindProperty(reqGroupS, "m_CountableRequirements").offset : -1;
    static int oId = countReqS ? ue::FindProperty(countReqS, "m_CountableID").offset : -1;
    static int oAmount = countReqS ? ue::FindProperty(countReqS, "m_nNeededAmount").offset : -1;
    static int elemSize = countReqS ? ue::StructSize(countReqS) : 0;
    if (!goldName || oRecipe < 0 || oCountables < 0 || oId < 0 || oAmount < 0 || elemSize <= 0) return;
    float want = g_enabled ? g_craftGold : 1.0f;
    std::unordered_map<int32_t*, CraftVal> seen;
    int count = 0;
    for (auto& t : g_found[CatRecipe]) {
        // m_GeneratedRecipe (HnSRecipe) starts with m_RequirementGroup.
        ue::TArrayRaw arr{};
        if (!Read((uint8_t*)t.obj + oRecipe + oCountables, arr) || arr.num <= 0 || arr.num > 16 || !LooksLikePtr(arr.data)) continue;
        for (int i = 0; i < arr.num; ++i) {
            uint8_t* e = (uint8_t*)arr.data + i * elemSize;
            if (ReadOr<uint32_t>(e + oId, 0) != goldName) continue;
            int32_t* amount = (int32_t*)(e + oAmount);
            int32_t cur;
            if (!Read(amount, cur)) continue;
            CraftVal v{cur, cur};
            auto it = g_craftVals.find(amount);
            if (it != g_craftVals.end() && it->second.last == cur) v = it->second;  // still our value
            int32_t target = (int32_t)std::lround(v.orig * want);
            if (cur != target) *amount = target;
            v.last = target;
            seen[amount] = v;
            ++count;
        }
    }
    g_craftVals.swap(seen);
    SetStatusFlag([&](Status& s) { s.recipeValues = count; });
}

// Developer helpers for testing.
std::string CastSlot(int n) {
    void* asc = PawnASC(g_pawn);
    if (!asc) return "no asc";
    std::vector<uint8_t> o;
    if (!ue::CallByName(asc, "GetAllSlottableAbilities", {}, &o)) return "call failed";
    auto p = ue::FindProperty(ue::FindFunction(ue::ClassOf(asc), "GetAllSlottableAbilities"), "Handles");
    ue::TArrayRaw a = ue::At<ue::TArrayRaw>(o.data(), p.offset);
    if (n < 0 || n >= a.num) return "only " + std::to_string(a.num) + " abilities";
    int32_t handle = ((int32_t*)a.data)[n];
    bool remote = false;
    std::vector<uint8_t> o2;
    ue::CallByName(asc, "TryActivateAbilityWithSpecHandle", {{"AbilityToActivate", &handle}, {"bAllowRemoteActivation", &remote}}, &o2);
    auto r = ue::FindProperty(ue::FindFunction(ue::ClassOf(asc), "TryActivateAbilityWithSpecHandle"), "ReturnValue");
    return std::string("handle ") + std::to_string(handle) + (o2[r.offset] ? " activated" : " not activated");
}

// Asks the game's own validator whether each open craft recipe is affordable.
// --- Inventory size ----------------------------------------------------------
// Container sizes come from HnSGlobalConfig (a DeveloperSettings CDO): per-container
// TMap<EInvType, int>. We change the "Main" (bag) entries and keep the originals.
struct SizeRef {
    int32_t* p;
    int32_t orig;
    bool chest;
};
std::vector<SizeRef> g_sizeRefs;
int g_invMainDefault = 0, g_invChestDefault = 0;
bool g_sizeCollectChest = false;

void ApplyInventorySize() {
    static bool collected = false;
    if (!collected) {
        void* cdo = ue::FindObject("Default__HnSGlobalConfig");
        void* cls = cdo ? ue::ClassOf(cdo) : nullptr;
        void* capS = ue::FindObject("HnSInventoryCapacityProperties");
        if (!cdo || !cls || !capS) return;
        collected = true;
        int oSizes = ue::FindProperty(capS, "m_InventorySizes").offset;
        struct Src {
            const char* prop;
            bool chest;
        } srcs[] = {{"m_PlayerInventoryCapacityProperties", false},
                    {"m_AccountChestInventoryCapacityProperties", true},
                    {"m_CityChestInventoryCapacityProperties", true}};
        for (auto& src : srcs) {
            auto pp = ue::FindProperty(cls, src.prop);
            if (!pp || oSizes < 0) continue;
            g_sizeCollectChest = src.chest;
            ue::ForEachSparseElement(
                (uint8_t*)cdo + pp.offset + oSizes, ue::SetElementStride(8),
                [](uint8_t* e, void*) {
                    if (e[0] != 0) return;  // EInvType Main
                    int32_t* v = (int32_t*)(e + 4);
                    g_sizeRefs.push_back({v, *v, g_sizeCollectChest});
                    (g_sizeCollectChest ? g_invChestDefault : g_invMainDefault) = *v;
                },
                nullptr);
        }
        Log("inventory sizes: bag %d, chest %d (%zu entries)", g_invMainDefault, g_invChestDefault, g_sizeRefs.size());
    }
    // The game sizes its item arrays from this config only while loading. Changing it
    // during play would let the game use slots that do not exist yet, so the new
    // size is written only while there is no player character (start-up, loading).
    if (g_pawn) return;
    for (auto& r : g_sizeRefs) {
        int want = r.chest ? g_invChest : g_invMain;
        int32_t target = (g_enabled && want > 0) ? want : r.orig;
        if (*r.p != target) *r.p = target;
    }
}

// The bag's items: menu index, tag, junk flag, replication id (candidate item handle).
struct BagItem {
    int menuIndex, replicationId, arrayIndex;
    std::string tag;
    bool junk;
    uint8_t handle[8];  // HnSItemSpec::m_Handle (HnSItemSpecHandle)
};
std::vector<BagItem> BagItems() {
    std::vector<BagItem> r;
    void* inv = PlayerInventory();
    static void* contS = ue::FindObject("HnSItemSpecContainer");
    static void* itemS = ue::FindObject("HnSItemSpecContainerItem");
    static void* specS = ue::FindObject("HnSItemSpec");
    if (!inv || !contS || !itemS || !specS) return r;
    auto pMap = ue::FindProperty(ue::ClassOf(inv), "m_ItemContainers");
    int oItems = ue::FindProperty(contS, "Items").offset, oSpec = ue::FindProperty(itemS, "m_Spec").offset;
    int oTag = ue::FindProperty(specS, "m_ItemTag").offset, oIdx = ue::FindProperty(specS, "m_nIndexInMenu").offset;
    int oJunk = ue::FindProperty(specS, "m_bMarkedAsJunk").offset;
    int oHandle = ue::FindProperty(specS, "m_Handle").offset;
    int contSize = ue::StructSize(contS), itemSize = ue::StructSize(itemS);
    if (!pMap || oItems < 0 || oSpec < 0 || oHandle < 0 || itemSize <= 0) return r;
    struct Ctx {
        int oItems, oSpec, oTag, oIdx, oJunk, oHandle, itemSize;
        std::vector<BagItem>* r;
    } ctx{oItems, oSpec, oTag, oIdx, oJunk, oHandle, itemSize, &r};
    ue::ForEachSparseElement(
        (uint8_t*)inv + pMap.offset, ue::SetElementStride(8 + contSize),
        [](uint8_t* e, void* u) {
            auto* x = static_cast<Ctx*>(u);
            if (e[0] != 0) return;  // Main
            ue::TArrayRaw a = ue::At<ue::TArrayRaw>(e + 8, x->oItems);
            if (a.num <= 0 || a.num > 1000 || !LooksLikePtr(a.data)) return;
            for (int i = 0; i < a.num; ++i) {
                uint8_t* it = (uint8_t*)a.data + i * x->itemSize;
                uint8_t* spec = it + x->oSpec;
                std::string tag = ue::NameFromFName(spec + x->oTag);
                if (tag == "None") continue;
                BagItem b{ue::At<int32_t>(spec, x->oIdx), ue::At<int32_t>(it, 0), i, tag, spec[x->oJunk] != 0, {}};
                memcpy(b.handle, spec + x->oHandle, 8);
                x->r->push_back(b);
            }
        },
        &ctx);
    return r;
}

std::string BagReport() {
    auto items = BagItems();
    std::string r = std::to_string(items.size()) + " items";
    for (auto& b : items)
        r += " | [" + std::to_string(b.menuIndex) + "] id=" + std::to_string(b.replicationId) + " " + b.tag + (b.junk ? " JUNK" : "");
    return r;
}

// Marks the item at a bag menu index as junk (testing). Tries the replication id, then the array index, as handle.
std::string MarkJunkAt(int menuIndex) {
    void* inv = PlayerInventory();
    for (auto& b : BagItems()) {
        if (b.menuIndex != menuIndex) continue;
        bool junk = true, fav = false;
        std::vector<uint8_t> o;
        ue::CallByName(inv, "ChangeItemJunkState", {{"Handle", b.handle}, {"bMarkAsJunk", &junk}, {"bMarkAsFavorite", &fav}}, &o);
        for (auto& c : BagItems())
            if (c.menuIndex == menuIndex && c.junk) return "marked " + b.tag + " as junk";
        return "could not mark " + b.tag + " (handle " + std::to_string(*(int32_t*)(b.handle + 4)) + ")";
    }
    return "no item at that index";
}

std::string ValidateCraft() {
    void* lib = ue::FindObject("Default__HnSRecipeBlueprintLibrary");
    void* inv = PlayerInventory();
    if (!lib || !inv) return "no library/inventory";
    std::string r;
    for (auto& t : g_found[CatRecipe]) {
        std::vector<uint8_t> o;
        if (!ue::CallByName(t.obj, "GetGeneratedRecipe", {}, &o)) continue;
        auto rr = ue::FindProperty(ue::FindFunction(ue::ClassOf(t.obj), "GetGeneratedRecipe"), "ReturnValue");
        std::vector<uint8_t> o2;
        if (!ue::CallByName(lib, "ValidateAllRequirements", {{"Recipe", o.data() + rr.offset}, {"pInventoryComponent", &inv}}, &o2))
            continue;
        auto vr = ue::FindProperty(ue::FindFunction(ue::ClassOf(lib), "ValidateAllRequirements"), "ReturnValue");
        r += " " + ue::NameOf(ue::ClassOf(t.obj)) + "=" + (o2[vr.offset] ? "affordable" : "not affordable");
    }
    return r.empty() ? "no open recipes" : r;
}

// Developer helper: travel to a waypoint by tag through the game's own teleport flow.
std::string TravelTo(const std::string& tagName) {
    void* sub = TeleportSubsystem();
    uint32_t tag = 0;
    if (!sub || !g_pc || !ue::FNameFromString(tagName, tag)) return "no subsystem / unknown tag";
    void* fFind = ue::FindFunction(ue::ClassOf(sub), "FindTeleporterInfo");
    void* fGo = ue::FindFunction(ue::ClassOf(sub), "StartTeleportToLevel");
    if (!fFind || !fGo) return "functions not found";
    std::vector<uint8_t> a(ue::StructSize(fFind) + 64, 0), b(ue::StructSize(fGo) + 64, 0);
    auto fTag = ue::FindProperty(fFind, "TeleportationTag"), fLevel = ue::FindProperty(fFind, "pLevelInfo"),
         fRet = ue::FindProperty(fFind, "ReturnValue");
    ue::At<uint32_t>(a.data(), fTag.offset) = tag;
    if (!ue::CallFunction(sub, fFind, a.data()) || !a[fRet.offset]) return "teleporter not found";
    auto gPc = ue::FindProperty(fGo, "pController"), gLevel = ue::FindProperty(fGo, "pLevelInfo"),
         gTag = ue::FindProperty(fGo, "TeleportationTag"), gRet = ue::FindProperty(fGo, "ReturnValue");
    ue::At<void*>(b.data(), gPc.offset) = g_pc;
    memcpy(b.data() + gLevel.offset, a.data() + fLevel.offset, std::min(fLevel.elementSize, gLevel.elementSize));
    ue::At<uint32_t>(b.data(), gTag.offset) = tag;
    if (!ue::CallFunction(sub, fGo, b.data())) return "call failed";
    return b[gRet.offset] ? "teleport started" : "teleport refused";
}

// --- Potions -------------------------------------------------------------------
void DrinkPotion(bool quiet) {
    void* asc = PawnASC(g_pawn);
    if (!asc) return quiet ? void() : SetMessage(T("Character not found."));
    for (auto& t : g_found[CatPotionGA]) {
        void* cls = t.obj;
        void* none = nullptr;
        std::vector<uint8_t> o;
        if (!ue::CallByName(asc, "FindAbilitySpecHandleForClass", {{"AbilityClass", &cls}, {"OptionalSourceObject", &none}}, &o))
            continue;
        auto r = ue::FindProperty(ue::FindFunction(ue::ClassOf(asc), "FindAbilitySpecHandleForClass"), "ReturnValue");
        int32_t handle = r ? ue::At<int32_t>(o.data(), r.offset) : -1;
        if (handle == -1) continue;
        SetStatusFlag([](Status& s) { s.potionAbility = true; });
        bool allowRemote = false;
        std::vector<uint8_t> o2;
        ue::CallByName(asc, "TryActivateAbilityWithSpecHandle", {{"AbilityToActivate", &handle}, {"bAllowRemoteActivation", &allowRemote}},
                       &o2);
        auto r2 = ue::FindProperty(ue::FindFunction(ue::ClassOf(asc), "TryActivateAbilityWithSpecHandle"), "ReturnValue");
        bool ok = r2 && o2[r2.offset] != 0;
        if (!quiet) SetMessage(ok ? T("Potion used.") : T("Can't drink a potion right now (no charge or on cooldown)."));
        else if (ok) Log("auto potion: drank (%s)", ue::NameOf(cls).c_str());
        return;
    }
    if (!quiet) SetMessage(T("Potion ability not found (enter the game, wait a few seconds)."));
}

void ApplyAutoPotion() {
    static DWORD last = 0, lastDrink = 0;
    if (!g_enabled || !g_autoPotion || !g_attrSet || GetTickCount() - last < 200) return;
    last = GetTickCount();
    AttrData* h = Attr("Health");
    AttrData* m = Attr("TotalMaxHealth");
    if (!m) m = Attr("HealthMax");
    AttrData* c = Attr("PotionCharges");
    if (!h || !m || !c || C(m) <= 0 || C(h) <= 0) return;  // dead or not ready
    if (C(h) / C(m) * 100.0f >= g_autoPotionPct || C(c) < 1.0f) return;
    if (GetTickCount() - lastDrink < 1500) return;
    lastDrink = GetTickCount();
    DrinkPotion(true);
}

// --- Level-up notification ---------------------------------------------------
void ApplyHideLevelUp() {
    int total = (int)g_found[CatLevelUp].size(), hidden = 0;
    int off = PropOff("Widget", "Visibility");
    for (auto& t : g_found[CatLevelUp]) {
        uint8_t vis = off >= 0 ? ue::At<uint8_t>(t.obj, off) : 0;
        if (vis == 1 /*Collapsed*/) {
            ++hidden;
            continue;
        }
        if (!g_enabled || !g_hideLevelUp) continue;
        uint8_t collapsed = 1;
        if (ue::CallByName(t.obj, "SetVisibility", {{"InVisibility", &collapsed}})) ++hidden;
    }
    SetStatusFlag([&](Status& s) {
        s.levelUpWidgets = total;
        s.levelUpHidden = hidden;
    });
}

// --- DPS meter -----------------------------------------------------------------
PersistentCall g_dpsCall;

void UpdateDps(Status& s) {
    static void* sub = nullptr;
    static int subIdx = -1;
    static DWORD lastFind = 0;
    if (!ue::IsLive(sub, subIdx)) {
        sub = nullptr;
        g_dpsCall = PersistentCall{};  // the old result belonged to the old subsystem's world; drop (tiny leak)
        if (GetTickCount() - lastFind < 3000) return;
        lastFind = GetTickCount();
        static void* cls = ue::FindClass("HnSBattleLogSubsystem");
        sub = cls ? ue::FindFirstInstance(cls) : nullptr;
        subIdx = sub ? ue::IndexOf(sub) : -1;
        if (!sub) return;
    }
    if (!g_pc || !g_dpsCall.Prepare(sub, "GetDpsMeterDisplayDataFor")) return;
    auto pOut = g_dpsCall.Param("pOutDisplayData"), pPc = g_dpsCall.Param("pPlayerController");
    if (!pOut || !pPc) return;
    uint8_t* b = g_dpsCall.buf.data();
    ue::At<void*>(b, pPc.offset) = g_pc;
    if (!ue::CallFunction(sub, g_dpsCall.fn, b)) return;
    static void* dataS = ue::FindObject("HnSBattleLogSubsystem_DPSMeterDisplayData");
    static void* lineS = ue::FindObject("HnSBattleLogSubsystem_DPSMeterDisplayDataSkillLine");
    static void* dmgS = ue::FindObject("HnSBattleLogSubsystem_DamageValueWithElements");
    if (!dataS || !lineS || !dmgS) return;
    static int oSkills = ue::FindProperty(dataS, "m_PlayerSkillToDisplay").offset;
    static int oDragon = ue::FindProperty(dataS, "m_DragonSkillToDisplay").offset;
    static int oTotal = ue::FindProperty(dataS, "m_TotalDamageDealt").offset;
    static int oRecv = ue::FindProperty(dataS, "m_TotalDamageReceived").offset;
    static int oHeal = ue::FindProperty(dataS, "m_HealReceived").offset;
    static int lName = ue::FindProperty(lineS, "m_SkillName").offset;
    static int lHeal = ue::FindProperty(lineS, "m_fHeal").offset;
    static int lTotal = ue::FindProperty(lineS, "m_TotalDamage").offset;
    static int lLast = ue::FindProperty(lineS, "m_LastSecondDPS").offset;
    static int dTotal = ue::FindProperty(dmgS, "m_fTotalDamage").offset;
    static int dDps = ue::FindProperty(dmgS, "m_fComputedDPS").offset;
    static int lineSize = ue::StructSize(lineS);
    if (oSkills < 0 || oTotal < 0 || lTotal < 0 || dTotal < 0 || dDps < 0 || lineSize <= 0) return;
    uint8_t* d = b + pOut.offset;
    s.dpsOk = true;
    s.dmgTotal = ue::At<float>(d, oTotal + lTotal + dTotal);
    s.dpsAvg = ue::At<float>(d, oTotal + lTotal + dDps);
    s.dpsLast = ue::At<float>(d, oTotal + lLast + dTotal);
    s.dmgRecv = ue::At<float>(d, oRecv + lTotal + dTotal);
    s.healRecv = ue::At<float>(d, oHeal + lHeal);
    for (int arrOff : {oSkills, oDragon}) {
        if (arrOff < 0) continue;
        ue::TArrayRaw a = ue::At<ue::TArrayRaw>(d, arrOff);
        if (a.num <= 0 || a.num > 64 || !LooksLikePtr(a.data)) continue;
        for (int i = 0; i < a.num; ++i) {
            uint8_t* l = (uint8_t*)a.data + i * lineSize;
            s.skills.push_back({ue::NameFromFName(l + lName), ue::At<float>(l, lTotal + dTotal), ue::At<float>(l, lTotal + dDps)});
        }
    }
    std::sort(s.skills.begin(), s.skills.end(), [](auto& x, auto& y) { return x.total > y.total; });
}

void ResetDps() {
    static void* cls = ue::FindClass("HnSBattleLogSubsystem");
    void* sub = cls ? ue::FindFirstInstance(cls) : nullptr;
    SetMessage(sub && ue::CallByName(sub, "ResetDPSMeter", {}) ? T("DPS meter reset.") : T("DPS meter not found."));
}

// Runs one feature; an access violation disables only that feature for a while
// instead of stopping the whole mod.
struct FeatureGuard {
    const char* name;
    void (*fn)();
    DWORD pausedUntil = 0;
    int faults = 0;
};
int GuardedCall(void (*fn)()) {
    __try {
        fn();
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}
void RunGuarded(FeatureGuard& g) {
    if (GetTickCount() < g.pausedUntil) return;
    if (GuardedCall(g.fn)) {
        ++g.faults;
        g.pausedUntil = GetTickCount() + 5000;
        Log("feature %s: access violation #%d, paused 5 s", g.name, g.faults);
    }
}
FeatureGuard g_features[] = {
    {"scan", IncrementalScan},      {"cityxp", ApplyCityXp},          {"camera", ApplyCamera},
    {"enemies", ApplyEnemies},      {"position", TrackPosition},      {"craft", ApplyCraftCost},
    {"autopotion", ApplyAutoPotion}, {"levelup", ApplyHideLevelUp}, {"invsize", ApplyInventorySize},
};

void ResetScanState() {
    for (auto& v : g_found) v.clear();
    g_catMemo.clear();
    g_scanCursor = 0;
    g_watchMemo.clear();
    for (auto& w : g_watches) w.found.clear();
    // g_cityAssets is kept: those assets survive level changes and hold our scaled values
    // together with the originals (re-reading them would scale twice).
}

void TickQol() {
    static void* lastWorld = nullptr;
    if (g_world != lastWorld) {
        lastWorld = g_world;
        ResetScanState();
        g_enemies.clear();
    }
    for (auto& f : g_features) RunGuarded(f);
}

bool ExecConsole(const std::string& cmd) {
    static void* lib = nullptr;
    static void* cdo = nullptr;
    static void* fn = nullptr;
    static ue::PropInfo pWorld, pCmd, pPlayer;
    if (!fn) {
        lib = ue::FindClass("KismetSystemLibrary");
        cdo = lib ? ue::FindObject("Default__KismetSystemLibrary") : nullptr;
        fn = lib ? ue::FindFunction(lib, "ExecuteConsoleCommand") : nullptr;
        if (!fn) return false;
        pWorld = ue::FindProperty(fn, "WorldContextObject");
        pCmd = ue::FindProperty(fn, "Command");
        pPlayer = ue::FindProperty(fn, "SpecificPlayer");
    }
    if (!cdo || !g_world || !pWorld || !pCmd) return false;
    std::vector<uint8_t> params(ue::StructSize(fn) + 16, 0);
    std::wstring wcmd(cmd.begin(), cmd.end());
    ue::At<void*>(params.data(), pWorld.offset) = g_world;
    ue::At<ue::FStringView>(params.data(), pCmd.offset) = {wcmd.c_str(), (int32_t)wcmd.size() + 1, (int32_t)wcmd.size() + 1};
    if (pPlayer) ue::At<void*>(params.data(), pPlayer.offset) = g_pc;
    return ue::CallFunction(cdo, fn, params.data());
}

void RunCommands() {
    std::deque<Command> cmds;
    {
        std::lock_guard<std::mutex> lock(g_cmdMutex);
        cmds.swap(g_cmds);
    }
    for (auto& c : cmds) {
        switch (c.type) {
        case Cmd::Console:
            SetMessage(F(ExecConsole(c.text) ? "Command sent: %s" : "Command failed: %s", c.text.c_str()));
            break;
        case Cmd::AddAttributePoints:
            if (AttrData* a = Attr("AttributePoints")) {
                WriteAttr("AttributePoints", a, B(a) + c.amount, C(a) + c.amount);
                SetMessage(F("Attribute points added: %d", (int)c.amount));
            } else {
                SetMessage(T("Could not add attribute points (character not found)"));
            }
            break;
        case Cmd::UnlockWaypoints:
            UnlockAllWaypoints();
            break;
        case Cmd::OpenTeleportMap:
            OpenTeleportMap();
            break;
        case Cmd::CountWaypoints:
            g_waypoints = CountWaypoints();
            break;
        case Cmd::Unstuck:
            Unstuck();
            break;
        case Cmd::SavePos:
            SavePosition((int)c.amount);
            break;
        case Cmd::LoadPos:
            LoadPosition((int)c.amount);
            break;
        case Cmd::SellJunk:
            SellJunk();
            break;
        case Cmd::ResetDps:
            ResetDps();
            break;
        case Cmd::DrinkPotion:
            DrinkPotion(false);
            break;
        case Cmd::Dump: {
            ue::DumpPackage("/Script/HnS", ModDir() + L"DragonkinMod_dump_HnS.txt");
            FILE* f = nullptr;
            // Live objects related to the city / XP, to see where game data actually lives.
            if (!_wfopen_s(&f, (ModDir() + L"DragonkinMod_dump_objects.txt").c_str(), L"w") && f) {
                int n = ue::NumObjects(), lines = 0;
                for (int i = 0; i < n && lines < 20000; ++i) {
                    void* o = ue::ObjectAt(i);
                    if (!o) continue;
                    std::string cls = ue::NameOf(ue::ClassOf(o)), name = ue::NameOf(o);
                    auto has = [](const std::string& s) {
                        return s.find("City") != std::string::npos || s.find("Xp") != std::string::npos ||
                               s.find("XP") != std::string::npos || s.find("Prosperity") != std::string::npos;
                    };
                    if (!has(cls) && !has(name)) continue;
                    fprintf(f, "%s%-50s %s\n", ue::IsCDO(o) ? "[CDO] " : "", cls.c_str(), ue::PathOf(o).c_str());
                    ++lines;
                }
                fclose(f);
                f = nullptr;
            }
            if (g_attrSet && !_wfopen_s(&f, (ModDir() + L"DragonkinMod_attributes.txt").c_str(), L"w") && f) {
                std::lock_guard<std::mutex> lock(g_statusMutex);
                for (auto& r : g_status.attrs) fprintf(f, "%-48s base=%-12g current=%g\n", r.name.c_str(), r.base, r.cur);
                fclose(f);
            }
            SetMessage(T("Dump files written to the mod folder."));
            break;
        }
        }
    }
}

void SyncCvarsOnce() {
    if (g_cvarsSynced || !g_world || !g_pawn) return;
    g_cvarsSynced = true;
    for (auto& c : g_cvars)
        if (c.on) Push(Cmd::Console, std::string(c.cvar) + " 1");
    if (g_npcLevel > 0) Push(Cmd::Console, "HnS.NPC.ForcedLevel " + std::to_string(g_npcLevel));
}

struct AttrCollect {
    std::vector<AttrRow>* rows;
};
void CollectAttr(void* field, void* user) {
    auto p = ue::DescribeProperty(field);
    if (p.type != "StructProperty" || !ResolveAttrLayout(p)) return;
    auto* a = reinterpret_cast<AttrData*>((uint8_t*)g_attrSet + p.offset);
    static_cast<AttrCollect*>(user)->rows->push_back({ue::NameFromFName((uint8_t*)field + ue::off.FFieldName), B(a), C(a)});
}

void UpdateStatus() {
    static DWORD last = 0;
    if (GetTickCount() - last < 250) return;
    last = GetTickCount();
    Status s;
    s.reflection = true;
    s.canCall = ue::off.ProcessEventIndex >= 0;
    s.world = g_world != nullptr;
    s.pawn = g_pawn != nullptr;
    s.attrSet = g_attrSet != nullptr;
    if (g_pawn) s.pawnName = ue::NameOf(g_pawn);
    for (auto& a : g_cityAssets) {
        if (a.vals.empty()) continue;
        if (!s.cityTables) {
            s.citySampleOrig = a.vals[0].orig;
            s.citySampleNow = *a.vals[0].p;
        }
        ++s.cityTables;
        s.cityValues += (int)a.vals.size();
    }
    if (g_attrSet) {
        s.attrSetPath = ue::PathOf(g_attrSet);
        AttrCollect ctx{&s.attrs};
        for (void* c = ue::ClassOf(g_attrSet); c; c = ue::SuperOf(c)) {
            ue::ForEachProperty(c, CollectAttr, &ctx);
            if (c == g_attrClass) break;
        }
    }
    s.worldName = WorldName();
    s.inCity = InCity();
    if (g_world) {
        auto& slots = SlotsForWorld();
        for (int i = 0; i < 3; ++i) s.savedPos[i] = slots[i].set;
    }
    UpdateDps(s);
    std::lock_guard<std::mutex> lock(g_statusMutex);
    // Fields maintained by the feature functions themselves.
    const Status& o = g_status;
    s.lastMessage = o.lastMessage;
    s.camLen = o.camLen;
    s.enemiesTracked = o.enemiesTracked;
    s.enemiesScaled = o.enemiesScaled;
    s.recipeValues = o.recipeValues;
    s.levelUpWidgets = o.levelUpWidgets;
    s.levelUpHidden = o.levelUpHidden;
    s.potionAbility = o.potionAbility;
    g_status = std::move(s);
}

void TickImpl() {
    ResolveContext();
    FindAttrSet();
    SyncCvarsOnce();
    RunCommands();
    if (g_attrSet) {
        ApplyMultipliers();
        ApplyLocks();
    }
    ApplyGameSpeed();
    TickQol();
    UpdateStatus();
}

int SafeTick() {
    __try {
        TickImpl();
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

// ---------------------------------------------------------------------------
// UI helpers
// ---------------------------------------------------------------------------
// Tooltip for the last item; `key` is the English text (translated and wrapped here).
void Tip(const char* key) {
    if (!key || !*key || !ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
    ImGui::SetTooltip("%s", i18n::Wrap(TL(key), ImGui::GetFontSize() * 26.0f).c_str());
}

ImVec4 Good() { return {0.45f, 0.85f, 0.45f, 1}; }
ImVec4 Warn() { return {0.95f, 0.75f, 0.30f, 1}; }

// Text whose format is translated first, then formatted, then shaped (correct for Arabic).
std::string VF(const char* key, va_list ap) {
    char buf[2048];
    vsnprintf(buf, sizeof(buf), TL(key), ap);
    return i18n::Shape(buf);
}
// Greyed, translated note wrapped to the available width.
void Note(const char* key) {
    ImGui::TextDisabled("%s", i18n::Wrap(TL(key), ImGui::GetContentRegionAvail().x).c_str());
}
void TextF(const char* key, ...) {
    va_list ap;
    va_start(ap, key);
    ImGui::TextUnformatted(VF(key, ap).c_str());
    va_end(ap);
}
void TextDisabledF(const char* key, ...) {
    va_list ap;
    va_start(ap, key);
    ImGui::TextDisabled("%s", VF(key, ap).c_str());
    va_end(ap);
}
void TextColoredF(const ImVec4& col, const char* key, ...) {
    va_list ap;
    va_start(ap, key);
    ImGui::TextColored(col, "%s", VF(key, ap).c_str());
    va_end(ap);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------
void LoadSettings() {
    const wchar_t* M = L"Multipliers";
    for (auto& m : g_mults) m.value = IniFloat(M, m.attr, 1.0f);
    for (auto& l : g_locks) l.on = IniFloat(L"Cheats", l.key, 0) != 0;
    for (auto& c : g_cvars) c.on = IniFloat(L"Console", c.cvar, 0) != 0;
    g_enabled = IniFloat(L"Mod", "Enabled", 1) != 0;
    g_speedOn = IniFloat(L"Cheats", "GameSpeedOn", 0) != 0;
    g_gameSpeed = IniFloat(L"Cheats", "GameSpeed", 1.0f);
    g_npcLevel = (int)IniFloat(L"Console", "NpcForcedLevel", 0);
    g_cityXp = IniFloat(L"Multipliers", "CityXp", 1.0f);
    const wchar_t* Q = L"QoL";
    g_camZoom = IniFloat(Q, "CameraZoom", 1.0f);
    g_enemyHp = IniFloat(Q, "EnemyHealth", 1.0f);
    g_enemyDmg = IniFloat(Q, "EnemyDamage", 1.0f);
    g_hubSpeed = IniFloat(Q, "CitySpeed", 1.0f);
    g_autoPotion = IniFloat(Q, "AutoPotion", 0) != 0;
    g_autoPotionPct = IniFloat(Q, "AutoPotionPercent", 35.0f);
    g_hideLevelUp = IniFloat(Q, "HideLevelUp", 0) != 0;
    g_craftGold = IniFloat(Q, "CraftGold", 1.0f);
    g_invMain = (int)IniFloat(Q, "BagSize", 0);
    g_invChest = (int)IniFloat(Q, "ChestSize", 0);
    Log("settings loaded from DragonkinMod.ini");
}

void SaveSettingsIfDirty() {
    if (!g_dirty) return;
    g_dirty = false;
    for (auto& m : g_mults) IniWrite(L"Multipliers", m.attr, m.value);
    IniWrite(L"Multipliers", "CityXp", g_cityXp);
    for (auto& l : g_locks) IniWrite(L"Cheats", l.key, l.on ? 1.f : 0.f);
    for (auto& c : g_cvars) IniWrite(L"Console", c.cvar, c.on ? 1.f : 0.f);
    IniWrite(L"Mod", "Enabled", g_enabled ? 1.f : 0.f);
    IniWrite(L"Cheats", "GameSpeedOn", g_speedOn ? 1.f : 0.f);
    IniWrite(L"Cheats", "GameSpeed", g_gameSpeed);
    IniWrite(L"Console", "NpcForcedLevel", (float)g_npcLevel);
    const wchar_t* Q = L"QoL";
    IniWrite(Q, "CameraZoom", g_camZoom);
    IniWrite(Q, "EnemyHealth", g_enemyHp);
    IniWrite(Q, "EnemyDamage", g_enemyDmg);
    IniWrite(Q, "CitySpeed", g_hubSpeed);
    IniWrite(Q, "AutoPotion", g_autoPotion ? 1.f : 0.f);
    IniWrite(Q, "AutoPotionPercent", g_autoPotionPct);
    IniWrite(Q, "HideLevelUp", g_hideLevelUp ? 1.f : 0.f);
    IniWrite(Q, "CraftGold", g_craftGold);
    IniWrite(Q, "BagSize", (float)g_invMain);
    IniWrite(Q, "ChestSize", (float)g_invChest);
}

Context GetContext() { return {g_engine, g_world, g_pc, g_pawn, g_ps, g_attrSet}; }

bool GetAttr(const char* name, float& base, float& cur) {
    AttrData* a = Attr(name);
    if (!a) return false;
    base = B(a);
    cur = C(a);
    return true;
}

bool SetAttr(const char* name, float base, float cur) {
    AttrData* a = Attr(name);
    if (!a) return false;
    WriteAttr(name, a, base, cur);
    return true;
}

bool SetSetting(const std::string& key, float value) {
    g_dirty = true;
    for (auto& m : g_mults)
        if (key == m.attr) return m.value = value, true;
    for (auto& l : g_locks)
        if (key == l.key) return l.on = value != 0, true;
    if (key == "CityXp") return g_cityXp = value, true;
    if (key == "GameSpeed") return g_gameSpeed = value, true;
    if (key == "GameSpeedOn") return g_speedOn = value != 0, true;
    if (key == "Enabled") return g_enabled = value != 0, true;
    if (key == "CameraZoom") return g_camZoom = value, true;
    if (key == "EnemyHealth") return g_enemyHp = value, true;
    if (key == "EnemyDamage") return g_enemyDmg = value, true;
    if (key == "CitySpeed") return g_hubSpeed = value, true;
    if (key == "AutoPotion") return g_autoPotion = value != 0, true;
    if (key == "AutoPotionPercent") return g_autoPotionPct = value, true;
    if (key == "HideLevelUp") return g_hideLevelUp = value != 0, true;
    if (key == "CraftGold") return g_craftGold = value, true;
    if (key == "BagSize") return g_invMain = (int)value, true;
    if (key == "ChestSize") return g_invChest = (int)value, true;
    g_dirty = false;
    return false;
}

std::string SettingsSummary() {
    std::string s = std::string("Enabled=") + (g_enabled ? "1" : "0");
    for (auto& m : g_mults)
        if (fabsf(m.value - 1.f) > 1e-4f) s += " " + std::string(m.attr) + "=" + std::to_string(m.value);
    for (auto& l : g_locks)
        if (l.on) s += " " + std::string(l.key) + "=1";
    if (fabsf(g_cityXp - 1.f) > 1e-4f) s += " CityXp=" + std::to_string(g_cityXp);
    if (g_speedOn) s += " GameSpeed=" + std::to_string(g_gameSpeed);
    auto f = [&](const char* k, float v, float def) {
        if (fabsf(v - def) > 1e-4f) s += " " + std::string(k) + "=" + std::to_string(v);
    };
    f("CameraZoom", g_camZoom, 1);
    f("EnemyHealth", g_enemyHp, 1);
    f("EnemyDamage", g_enemyDmg, 1);
    f("CitySpeed", g_hubSpeed, 1);
    f("AutoPotion", g_autoPotion, 0);
    f("HideLevelUp", g_hideLevelUp, 0);
    f("CraftGold", g_craftGold, 1);
    f("BagSize", (float)g_invMain, 0);
    f("ChestSize", (float)g_invChest, 0);
    return s;
}

bool ExecConsoleCommand(const std::string& cmd) { return ExecConsole(cmd); }

int WatchClass(const std::string& name) {
    for (size_t w = 0; w < g_watches.size(); ++w)
        if (g_watches[w].name == name) return (int)w;
    g_watches.push_back({name, {}});
    g_watchMemo.clear();  // classes seen before must be re-checked against the new name
    g_scanCursor = 0;
    return (int)g_watches.size() - 1;
}

std::vector<void*> WatchedInstances(int watch) {
    std::vector<void*> r;
    if (watch < 0 || watch >= (int)g_watches.size()) return r;
    for (auto& t : g_watches[watch].found)
        if (ue::IsLive(t.obj, t.idx)) r.push_back(t.obj);
    return r;
}

bool IsBuiltinMultiplier(const std::string& attr) {
    for (auto& m : g_mults)
        if (attr == m.attr) return true;
    return false;
}

bool ModsEnabled() { return g_enabled; }

std::string RunAction(const std::string& name, int arg) {
    if (name == "unstuck") Unstuck();
    else if (name == "savepos") SavePosition(arg);
    else if (name == "loadpos") LoadPosition(arg);
    else if (name == "selljunk") SellJunk();
    else if (name == "resetdps") ResetDps();
    else if (name == "potion") DrinkPotion(false);
    else if (name.rfind("travel:", 0) == 0) return TravelTo(name.substr(7));
    else if (name == "cast") return CastSlot(arg);
    else if (name == "validate") return ValidateCraft();
    else if (name == "bag") return BagReport();
    else if (name == "markjunk") return MarkJunkAt(arg);
    else if (name == "invsize") {
        void* inv = PlayerInventory();
        uint8_t t = 0;
        std::vector<uint8_t> o;
        if (!inv || !ue::CallByName(inv, "GetInventoryContainerSize", {{"eInvType", &t}}, &o)) return "no inventory";
        auto r = ue::FindProperty(ue::FindFunction(ue::ClassOf(inv), "GetInventoryContainerSize"), "ReturnValue");
        return "bag size " + std::to_string(ue::At<int32_t>(o.data(), r.offset)) + " (config default " + std::to_string(g_invMainDefault) + ")";
    }
    else return "unknown action";
    std::lock_guard<std::mutex> lock(g_statusMutex);
    return g_status.lastMessage;
}

std::string QolReport() {
    Status s;
    UpdateDps(s);
    Status st;
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        st = g_status;
    }
    char buf[512];
    snprintf(buf, sizeof(buf),
             "world=%s city=%d camLen=%.0f enemies tracked=%d scaled=%d recipeGold=%d levelUpWidgets=%d hidden=%d "
             "potionAbility=%d potionClasses=%zu gaAttrs=%zu scanPasses=%d posHist=%zu | "
             "dps ok=%d total=%.0f avg=%.1f last=%.1f recv=%.0f heal=%.0f skills=%zu",
             WorldName().c_str(), InCity(), st.camLen, st.enemiesTracked, st.enemiesScaled, st.recipeValues, st.levelUpWidgets,
             st.levelUpHidden, st.potionAbility, g_found[CatPotionGA].size(), g_gaAttrs.size(), g_scanPasses, g_posHist.size(),
             s.dpsOk, s.dmgTotal, s.dpsAvg, s.dpsLast, s.dmgRecv, s.healRecv, s.skills.size());
    std::string r = buf;
    for (auto& v : g_craftVals) r += " | craftGold " + std::to_string(v.second.orig) + "->" + std::to_string(*v.first);
    for (auto& l : s.skills) r += " | " + l.name + " total=" + std::to_string(l.total) + " dps=" + std::to_string(l.dps);
    return r;
}

std::string EnemyAttrReport(void* asc) {
    LoadGameplayAttributes(PawnASC(g_pawn));
    std::string r;
    for (const char* n : {"Health", "HealthMax", "HealthMaxMultiplier", "TotalMaxHealth", "FinalDamageMultiplier"}) {
        float v = NAN;
        ReadAttr(asc, n, v);
        r += std::string(n) + "=" + std::to_string(v) + " ";
    }
    return r;
}

bool ModifyAttribute(void* asc, const std::string& name, float value, int op) {
    LoadGameplayAttributes(PawnASC(g_pawn));
    return ModifyAttr(asc, name.c_str(), value, (ModOp)op);
}

std::string WaypointReport(bool unlock) {
    if (unlock) UnlockAllWaypoints();
    void* sub = TeleportSubsystem();
    std::string r;
    int active = 0, total = 0;
    for (uint64_t tag : AllTeleporterTags()) {
        bool on = false;
        CallReturningBool(sub, "IsTeleporterActivated_TagOnly", tag, on);
        ++total;
        active += on;
        r += "  " + std::string(on ? "[x] " : "[ ] ") + ue::NameFromFName(&tag) + "\n";
    }
    return r + "  " + std::to_string(active) + "/" + std::to_string(total) + " active";
}

void OnReflectionReady() {
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        g_status.reflection = true;
    }
    // Container sizes must be in the config before the game creates the chest
    // inventories (during start-up, before the game thread runs our ticks).
    if (GuardedCall(ApplyInventorySize)) Log("inventory size: early apply failed");
}

void GameThreadTick() {
    if (!ue::Ready()) return;
    static DWORD pausedUntil = 0;
    if (GetTickCount() < pausedUntil) return;
    if (SafeTick()) {
        Log("game tick: access violation, pausing 5 s");
        g_attrSet = nullptr;
        g_engine = nullptr;
        ResetAttrState();
        pausedUntil = GetTickCount() + 5000;
    }
}

void SetEnabled(bool on) { g_enabled = on; }

void ClaimAttributes() {
    // The Multipliers tab manages these attributes; other mods' factors are folded in.
    if (g_host && g_host->apiVersion >= 2)
        for (auto& m : g_mults) g_host->ClaimAttribute(m.attr);
}

bool DevCommand(const std::string& line, std::string& out) {
    std::vector<std::string> a;
    for (size_t i = 0; i < line.size();) {
        while (i < line.size() && line[i] == ' ') ++i;
        size_t j = line.find(' ', i);
        if (j == std::string::npos) j = line.size();
        if (j > i) a.push_back(line.substr(i, j - i));
        i = j;
    }
    if (a.empty()) return false;
    const std::string& cmd = a[0];
    if (cmd == "qol") out = "  " + QolReport();
    else if (cmd == "modstate") out = "  " + SettingsSummary();
    else if (cmd == "waypoints") out = WaypointReport(a.size() > 1 && a[1] == "unlock");
    else if (cmd == "action" && a.size() > 1) out = "  " + RunAction(a[1], a.size() > 2 ? atoi(a[2].c_str()) : 0);
    else if (cmd == "mod" && a.size() > 2) out = SetSetting(a[1], (float)atof(a[2].c_str())) ? "  ok" : "  ! unknown setting";
    else return false;
    return true;
}

int TabCount() { return 5; }

const char* TabName(int i) {
    static const char* names[] = {"Multipliers", "Cheats", "Convenience", "Console", "Status"};
    return i >= 0 && i < 5 ? T(names[i]) : "";
}

void DrawTab(int index) {
    Status st;
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        st = g_status;
    }
    if (!st.reflection) ImGui::TextColored(Warn(), "%s", T("● Scanning game data..."));
    {
        if (index == 0) {
            const float sliderWidth = ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 13.0f;
            ImGui::SeparatorText(T("City"));
            ImGui::SetNextItemWidth(sliderWidth);
            if (ImGui::SliderFloat("##cityxp", &g_cityXp, 0.1f, 50.f, "%.2fx", ImGuiSliderFlags_Logarithmic)) g_dirty = true;
            Tip("XP earned for the city's level bar. Scales every value of the game's city XP table. Only works in the offline city.");
            ImGui::SameLine();
            if (ImGui::SmallButton("1x##city")) {
                g_cityXp = 1.0f;
                g_dirty = true;
            }
            ImGui::SameLine();
            if (fabsf(g_cityXp - 1.0f) > 1e-4f)
                ImGui::TextColored(Good(), T("City XP"));
            else
                ImGui::TextUnformatted(T("City XP"));
            const char* cat = nullptr;
            for (int i = 0; i < kMultCount; ++i) {
                MultDef& m = g_mults[i];
                if (!cat || strcmp(cat, m.category) != 0) {
                    cat = m.category;
                    ImGui::SeparatorText(T(cat));
                }
                ImGui::PushID(i);
                ImGui::SetNextItemWidth(sliderWidth);
                if (ImGui::SliderFloat("##v", &m.value, m.minV, m.maxV, "%.2fx",
                                       m.log ? ImGuiSliderFlags_Logarithmic : ImGuiSliderFlags_None))
                    g_dirty = true;
                Tip(m.tip);
                ImGui::SameLine();
                if (ImGui::SmallButton("1x")) {
                    m.value = 1.0f;
                    g_dirty = true;
                }
                ImGui::SameLine();
                if (fabsf(m.value - 1.0f) > 1e-4f)
                    ImGui::TextColored(Good(), "%s", T(m.label));
                else
                    ImGui::TextUnformatted(T(m.label));
                ImGui::PopID();
            }
            ImGui::Spacing();
            if (ImGui::Button(T("Reset all multipliers"))) {
                for (auto& m : g_mults) m.value = 1.0f;
                g_cityXp = 1.0f;
                g_dirty = true;
            }
            Note("Ctrl+click a slider to type a value.");
        }

        if (index == 1) {
            ImGui::SeparatorText(T("Locks"));
            for (auto& l : g_locks) {
                if (ImGui::Checkbox(T(l.label), &l.on)) g_dirty = true;
                Tip(l.tip);
            }
            ImGui::SeparatorText(T("Game speed"));
            if (ImGui::Checkbox(T("Change game speed"), &g_speedOn)) g_dirty = true;
            ImGui::SetNextItemWidth(260);
            if (ImGui::SliderFloat("##speed", &g_gameSpeed, 0.25f, 5.0f, "%.2fx", ImGuiSliderFlags_Logarithmic)) g_dirty = true;
            ImGui::SameLine();
            if (ImGui::SmallButton("1x##speed")) {
                g_gameSpeed = 1.0f;
                g_dirty = true;
            }
            ImGui::SeparatorText(T("World"));
            if (ImGui::Button(T("Unlock all waypoints"))) Push(Cmd::UnlockWaypoints);
            Tip("Activates every teleport point through the game's own system. Areas you have not reached in the story open too.");
            ImGui::SameLine();
            if (ImGui::Button(T("Teleport map"))) Push(Cmd::OpenTeleportMap);
            Tip("Opens the fast-travel map from where you are, without going to a waypoint.");
            if (g_waypoints.total)
                TextDisabledF("Waypoints: %d / %d unlocked", g_waypoints.active, g_waypoints.total);
            ImGui::SeparatorText(T("Attribute points (experimental)"));
            if (ImGui::Button("+1")) Push(Cmd::AddAttributePoints, {}, 1);
            ImGui::SameLine();
            if (ImGui::Button("+5")) Push(Cmd::AddAttributePoints, {}, 5);
            ImGui::SameLine();
            if (ImGui::Button("+20")) Push(Cmd::AddAttributePoints, {}, 20);
            Note("Points may not be written to the save; spend them and check.");
        }

        if (index == 2) {
            const float w = ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 13.0f;
            auto slider = [&](const char* id, float* v, float lo, float hi, const char* label, const char* tip, float def = 1.0f,
                              const char* fmt = "%.2fx") {
                ImGui::PushID(id);
                ImGui::SetNextItemWidth(w);
                if (ImGui::SliderFloat("##s", v, lo, hi, fmt, ImGuiSliderFlags_Logarithmic)) g_dirty = true;
                Tip(tip);
                ImGui::SameLine();
                if (ImGui::SmallButton(def == 1.0f ? "1x" : T("reset"))) {
                    *v = def;
                    g_dirty = true;
                }
                ImGui::SameLine();
                if (fabsf(*v - def) > 1e-4f)
                    ImGui::TextColored(Good(), "%s", label);
                else
                    ImGui::TextUnformatted(label);
                ImGui::PopID();
            };

            ImGui::SeparatorText(T("Camera"));
            slider("cam", &g_camZoom, 0.5f, 3.0f, T("Camera distance"),
                   T("Moves the camera away from the character (1x = game default). Re-applied when the game resets the camera (death, new area)."));
            if (st.camLen > 0) TextDisabledF("Current camera distance: %.0f", st.camLen);

            ImGui::SeparatorText(T("Enemies"));
            slider("ehp", &g_enemyHp, 0.1f, 10.0f, T("Enemy health"),
                   T("Maximum health of enemies. The game's elite/boss multipliers are kept. Applies only to hostile NPCs."));
            slider("edmg", &g_enemyDmg, 0.1f, 10.0f, T("Enemy damage"), T("Damage dealt by enemies."));
            if (st.enemiesTracked) TextDisabledF("%d NPCs in this area, applied to %d enemies", st.enemiesTracked, st.enemiesScaled);

            ImGui::SeparatorText(T("Movement and position"));
            slider("city", &g_hubSpeed, 1.0f, 3.0f, T("Extra speed in the city"),
                   T("Multiplier added to movement speed only while in the city (on top of the movement speed in the Multipliers tab). The game caps total movement at 3x."));
            if (ImGui::Button(T("I'm stuck!"))) Push(Cmd::Unstuck);
            Tip("When stuck in a wall or terrain, takes the character back to a safe spot it passed a few seconds ago.");
            ImGui::SameLine();
            TextDisabledF("Map: %s", st.worldName.c_str());
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(T("Position:"));
            for (int i = 0; i < 3; ++i) {
                ImGui::PushID(100 + i);
                ImGui::SameLine(0, i ? 22.0f : 10.0f);
                ImGui::Text("%d", i + 1);
                ImGui::SameLine();
                if (ImGui::Button(T("Save"))) Push(Cmd::SavePos, {}, (float)i);
                ImGui::SameLine();
                ImGui::BeginDisabled(!st.savedPos[i]);
                if (ImGui::Button(T("Go"))) Push(Cmd::LoadPos, {}, (float)i);
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            Note("Positions are stored per map; Go is greyed out for an empty slot.");

            ImGui::SeparatorText(T("Inventory and crafting"));
            {
                int defBag = g_invMainDefault > 0 ? g_invMainDefault : 36, defChest = g_invChestDefault > 0 ? g_invChestDefault : 150;
                int bag = g_invMain > 0 ? g_invMain : defBag, chest = g_invChest > 0 ? g_invChest : defChest;
                ImGui::SetNextItemWidth(w);
                if (ImGui::SliderInt("##bag", &bag, defBag, defBag * 4, T("%d slots"))) {
                    g_invMain = bag == defBag ? 0 : bag;
                    g_dirty = true;
                }
                Tip("Bag capacity. Applied on the next area load. If you shrink it or remove the mod, items in the extra slots are lost.");
                ImGui::SameLine();
                ImGui::TextUnformatted(T("Bag size"));
                ImGui::SetNextItemWidth(w);
                if (ImGui::SliderInt("##chest", &chest, defChest, defChest * 4, T("%d slots"))) {
                    g_invChest = chest == defChest ? 0 : chest;
                    g_dirty = true;
                }
                Tip("City and account chest capacity. Applied when the game restarts. If you shrink it or remove the mod, items in the extra slots are lost.");
                ImGui::SameLine();
                ImGui::TextUnformatted(T("Chest size"));
            }
            if (ImGui::Button(T("Sell junk-marked items"))) Push(Cmd::SellJunk);
            Tip("Sells equipment and fragments you marked as junk in the inventory, without visiting a merchant.");
            slider("craft", &g_craftGold, 0.0f, 2.0f, T("Crafting gold cost"),
                   T("Gold cost of the recipe prepared in the crafting menu (potions, rings, equipment, reroll). Applied while the menu is open."));
            if (st.recipeValues) TextDisabledF("Adjusted %d gold costs in the open crafting menu", st.recipeValues);

            ImGui::SeparatorText(T("Potion"));
            if (ImGui::Checkbox(T("Auto potion"), &g_autoPotion)) g_dirty = true;
            Tip("When health drops below the set percentage and you have a charge, uses the game's own potion ability.");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
            if (ImGui::SliderFloat("##potpct", &g_autoPotionPct, 10.0f, 80.0f, T("health < %%%.0f"))) g_dirty = true;
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Drink now"))) Push(Cmd::DrinkPotion);

            ImGui::SeparatorText(T("Interface"));
            if (ImGui::Checkbox(T("Hide level-up notification"), &g_hideLevelUp)) g_dirty = true;
            Tip("Hides the level-up notifications of the character, wyrmling and ancestors.");

            ImGui::SeparatorText(T("DPS meter"));
            if (!st.dpsOk) {
                Note("The game's combat log was not found (enter the game).");
            } else {
                TextF("Total damage: %.0f   Average DPS: %.1f   Last second: %.1f", st.dmgTotal, st.dpsAvg, st.dpsLast);
                TextF("Damage taken: %.0f   Healing received: %.0f", st.dmgRecv, st.healRecv);
                if (!st.skills.empty() &&
                    ImGui::BeginTable("dps", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn(T("Ability"));
                    ImGui::TableSetupColumn(T("Total"));
                    ImGui::TableSetupColumn("DPS");
                    ImGui::TableHeadersRow();
                    for (auto& l : st.skills) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(l.name.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%.0f", l.total);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.1f", l.dps);
                    }
                    ImGui::EndTable();
                }
                if (ImGui::Button(T("Reset DPS meter"))) Push(Cmd::ResetDps);
            }
        }


        if (index == 3) {
            if (!st.canCall) ImGui::TextColored(Warn(), T("ProcessEvent not found; console commands are unavailable."));
            Note("Developer commands left in the game. Some may do nothing in the release build.");
            ImGui::SeparatorText(T("On / off"));
            for (auto& c : g_cvars) {
                if (ImGui::Checkbox(T(c.label), &c.on)) {
                    g_dirty = true;
                    Push(Cmd::Console, std::string(c.cvar) + (c.on ? " 1" : " 0"));
                }
                Tip(c.tip);
            }
            ImGui::SeparatorText(T("Enemy level"));
            ImGui::SetNextItemWidth(260);
            if (ImGui::SliderInt("##npclvl", &g_npcLevel, 0, 100, g_npcLevel ? "%d" : T("off"))) g_dirty = true;
            ImGui::SameLine();
            if (ImGui::Button(T("Apply"))) Push(Cmd::Console, "HnS.NPC.ForcedLevel " + std::to_string(g_npcLevel));
            Tip("HnS.NPC.ForcedLevel — fixes the level of newly spawned enemies. 0 = off.");
            ImGui::SeparatorText(T("Free command"));
            static char cmd[256] = "";
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90);
            bool enter = ImGui::InputText("##cmd", cmd, sizeof(cmd), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if ((ImGui::Button(T("Run")) || enter) && cmd[0]) Push(Cmd::Console, cmd);
        }

        if (index == 4) {
            TextF("Reflection: %s   ProcessEvent: %s", TL(st.reflection ? "ready" : "waiting"), TL(st.canCall ? "yes" : "no"));
            TextF("World: %s   Character: %s", TL(st.world ? "yes" : "no"), st.pawn ? st.pawnName.c_str() : TL("no"));
            TextF("Attribute set: %s", st.attrSet ? st.attrSetPath.c_str() : TL("not found"));
            if (st.cityTables)
                TextF("City XP table: %d tables, %d values (sample: %.1f → %.1f)", st.cityTables, st.cityValues,
                      st.citySampleOrig, st.citySampleNow);
            else
                ImGui::TextUnformatted(T("City XP table: not found"));
            if (ImGui::Button(T("Write dump"))) Push(Cmd::Dump);
            Tip("Writes DragonkinMod_dump_HnS.txt and DragonkinMod_attributes.txt.");
            ImGui::SameLine();
            if (ImGui::Button(T("Save settings now"))) {
                g_dirty = true;
                SaveSettingsIfDirty();
            }
            static ImGuiTextFilter filter;
            filter.Draw(T("Search"), 200);
            if (ImGui::BeginTable("attrs", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV,
                                  {0, ImGui::GetContentRegionAvail().y - 30})) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Attribute");
                ImGui::TableSetupColumn("Base", ImGuiTableColumnFlags_WidthFixed, 90);
                ImGui::TableSetupColumn(T("Current"), ImGuiTableColumnFlags_WidthFixed, 90);
                ImGui::TableHeadersRow();
                for (auto& r : st.attrs) {
                    if (!filter.PassFilter(r.name.c_str())) continue;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(r.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", r.base);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", r.cur);
                }
                ImGui::EndTable();
            }
        }

    }

    ImGui::Separator();
    if (!st.lastMessage.empty()) ImGui::TextDisabled("%s", st.lastMessage.c_str());
    Note("Use only in the offline city / solo. Settings are saved automatically.");
}

}  // namespace game
