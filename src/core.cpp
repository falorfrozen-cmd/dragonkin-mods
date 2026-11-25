#include "core.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <mutex>
#include <unordered_map>
#include "common.h"
#include "devcmd.h"
#include "i18n.h"
#include "imgui.h"
#include "modloader.h"
#include "overlay.h"
#include "ue.h"

namespace core {
using i18n::T;
using i18n::TL;
namespace {

// ---------------------------------------------------------------------------
// Settings (DragonkinMod.ini, [Loader])
// ---------------------------------------------------------------------------
std::atomic<bool> g_modsEnabled{true};
std::atomic<bool> g_dirty{false};
std::atomic<int> g_forcedTab{-1};

std::wstring IniPath() { return ModDir() + L"DragonkinMod.ini"; }

// ---------------------------------------------------------------------------
// Game context
// ---------------------------------------------------------------------------
std::unordered_map<std::string, int> g_propCache;
int PropOff(const char* cls, const char* prop) {
    std::string key = std::string(cls) + "." + prop;
    auto it = g_propCache.find(key);
    if (it != g_propCache.end()) return it->second;
    int result = -1;
    if (void* c = ue::FindClass(cls))
        if (auto p = ue::FindProperty(c, prop)) result = p.offset;
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
void *g_world = nullptr, *g_pc = nullptr, *g_pawn = nullptr, *g_ps = nullptr;

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
    static void* lastWorld = nullptr;
    static void* lastPawn = nullptr;
    if (g_world != lastWorld) Log("world: %s", g_world ? ue::NameOf(g_world).c_str() : "none");
    if (g_pawn != lastPawn)
        Log("player pawn: %s (class %s)", g_pawn ? ue::NameOf(g_pawn).c_str() : "none",
            g_pawn ? ue::NameOf(ue::ClassOf(g_pawn)).c_str() : "-");
    lastWorld = g_world;
    lastPawn = g_pawn;
}

// --- Player attributes (HnSUberAttributeSet). This game's FGameplayAttributeData
// is 8 bytes; BaseValue/CurrentValue offsets come from the struct's reflection.
int g_attrSize = -1, g_baseOff = -1, g_curOff = -1;
void* g_attrClass = nullptr;
void* g_attrSet = nullptr;
int g_attrIdx = -1;
std::unordered_map<std::string, int> g_attrOffsets;

bool ResolveAttrLayout(const ue::PropInfo& p) {
    if (g_attrSize > 0) return p.elementSize == g_attrSize;
    void* strct = ue::PropertyStruct(p.field);
    if (!strct) return false;
    auto bp = ue::FindProperty(strct, "BaseValue");
    auto cp = ue::FindProperty(strct, "CurrentValue");
    if (!bp || !cp) {
        g_attrSize = 0x7FFFFFFF;
        return false;
    }
    g_baseOff = bp.offset;
    g_curOff = cp.offset;
    g_attrSize = p.elementSize;
    return true;
}

bool OwnedByPlayer(void* o) {
    for (int d = 0; o && d < 4; ++d) {
        o = ue::OuterOf(o);
        if (o && (o == g_pawn || o == g_ps || o == g_pc)) return true;
    }
    return false;
}

void FindAttrSet() {
    static DWORD lastScan = 0;
    if (ue::IsLive(g_attrSet, g_attrIdx) && OwnedByPlayer(g_attrSet)) return;
    g_attrSet = nullptr;
    if (!g_pawn || GetTickCount() - lastScan < 1000) return;
    lastScan = GetTickCount();
    if (!g_attrClass) g_attrClass = ue::FindClass("HnSUberAttributeSet");
    if (!g_attrClass) return;
    int n = ue::NumObjects();
    for (int i = 0; i < n; ++i) {
        void* o = ue::ObjectAt(i);
        if (!o || ue::IsCDO(o) || !ue::IsA(o, g_attrClass) || !OwnedByPlayer(o)) continue;
        g_attrSet = o;
        g_attrIdx = i;
        return;
    }
}

uint8_t* AttrPtr(const char* name) {
    if (!g_attrSet || !name) return nullptr;
    auto it = g_attrOffsets.find(name);
    int off;
    if (it == g_attrOffsets.end()) {
        auto p = ue::FindProperty(g_attrClass, name);
        off = (p && p.type == "StructProperty" && ResolveAttrLayout(p)) ? p.offset : -1;
        g_attrOffsets[name] = off;
    } else {
        off = it->second;
    }
    return off >= 0 ? (uint8_t*)g_attrSet + off : nullptr;
}

// After a direct write, OnRep_<Name>(Old) makes GAS re-evaluate and broadcast the
// change (derived stats such as TotalMovementSpeed follow).
void Notify(const char* name, float oldBase, float oldCur) {
    std::string fnName = std::string("OnRep_") + name;
    void* fn = ue::FindFunction(ue::ClassOf(g_attrSet), fnName.c_str());
    if (!fn) return;
    std::vector<uint8_t> buf(ue::StructSize(fn) + 16, 0);
    *reinterpret_cast<float*>(buf.data() + g_baseOff) = oldBase;
    *reinterpret_cast<float*>(buf.data() + g_curOff) = oldCur;
    ue::CallFunction(g_attrSet, fn, buf.data());
}

// --- Console ---
bool ExecConsole(const std::string& cmd) {
    static void* cdo = nullptr;
    static void* fn = nullptr;
    static ue::PropInfo pWorld, pCmd, pPlayer;
    if (!fn) {
        void* lib = ue::FindClass("KismetSystemLibrary");
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

// ---------------------------------------------------------------------------
// Class watching for mods: an incremental scan (40k objects per tick, no hitch).
// A class matches a watched name if the name is the class or one of its supers;
// the answer is memoized per class (with its index, since addresses get reused).
// ---------------------------------------------------------------------------
struct Tracked {
    void* obj;
    int idx;
    void* cls;
};
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
int g_scanCursor = 0;

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

void WatchScan() {
    if (g_watches.empty()) return;
    for (auto& w : g_watches)
        for (size_t i = 0; i < w.found.size();)
            if (!ue::IsLive(w.found[i].obj, w.found[i].idx) || ue::ClassOf(w.found[i].obj) != w.found[i].cls)
                w.found.erase(w.found.begin() + i);
            else ++i;
    int n = ue::NumObjects();
    if (g_scanCursor >= n) g_scanCursor = 0;
    int end = std::min(n, g_scanCursor + 40000);
    for (int i = g_scanCursor; i < end; ++i) {
        void* o = ue::ObjectAt(i);
        if (!o || ue::IsCDO(o)) continue;
        void* cls = ue::ClassOf(o);
        if (!cls) continue;
        for (uint16_t w : WatchesOf(cls)) {
            auto& f = g_watches[w].found;
            bool known = false;
            for (auto& t : f) known |= t.obj == o;
            if (!known) f.push_back({o, i, cls});
        }
    }
    g_scanCursor = end >= n ? 0 : end;
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------
struct HeaderStatus {
    bool reflection = false, world = false, pawn = false;
    std::string pawnName;
};
std::mutex g_statusMutex;
HeaderStatus g_header;

void TickImpl() {
    static void* lastWorld = nullptr;
    ResolveContext();
    if (g_world != lastWorld) {  // classes may have been unloaded: start the scan over
        lastWorld = g_world;
        g_watchMemo.clear();
        for (auto& w : g_watches) w.found.clear();
        g_scanCursor = 0;
    }
    FindAttrSet();
    WatchScan();
    modloader::Tick();
    dev::RunPending();
    static DWORD last = 0;
    if (GetTickCount() - last > 250) {
        last = GetTickCount();
        HeaderStatus h{true, g_world != nullptr, g_pawn != nullptr, g_pawn ? ue::NameOf(g_pawn) : std::string()};
        std::lock_guard<std::mutex> lock(g_statusMutex);
        g_header = h;
    }
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
void Note(const char* key) {
    ImGui::TextDisabled("%s", i18n::Wrap(TL(key), ImGui::GetContentRegionAvail().x).c_str());
}

void DrawSettingsTab() {
    ImGui::SeparatorText(T("Language"));
    int lang = i18n::Language();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    if (ImGui::BeginCombo("##lang", i18n::LanguageName(lang))) {
        for (int l = 0; l < i18n::LANG_COUNT; ++l)
            if (ImGui::Selectable(i18n::LanguageName(l), l == lang)) {
                i18n::SetLanguage(l);
                g_dirty = true;
            }
        ImGui::EndCombo();
    }
    ImGui::SeparatorText(T("Mods"));
    bool on = g_modsEnabled;
    if (ImGui::Checkbox(T("Enable mods"), &on)) {
        g_modsEnabled = on;
        g_dirty = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("%s", i18n::Wrap(TL("Switches every mod off at once; the game's own values come back."),
                                           ImGui::GetFontSize() * 26)
                                    .c_str());
    ImGui::SeparatorText(T("Panel"));
    if (ImGui::Button(T("Reset panel position"))) overlay::ResetPanelPosition();
    Note("Drag the panel by its title bar. It always stays inside the screen that shows the game.");
    ImGui::Spacing();
    ImGui::TextDisabled("Dragonkin Mod Loader %s", modloader::kVersion);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------
void LoadSettings() {
    wchar_t buf[32];
    GetPrivateProfileStringW(L"Loader", L"Language", L"", buf, 32, IniPath().c_str());
    if (!buf[0]) GetPrivateProfileStringW(L"Mod", L"Language", L"en", buf, 32, IniPath().c_str());  // older versions
    char c8[32];
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, c8, 32, nullptr, nullptr);
    i18n::SetLanguage(i18n::LanguageFromCode(c8));
    g_modsEnabled = GetPrivateProfileIntW(L"Loader", L"ModsEnabled", 1, IniPath().c_str()) != 0;
    Log("loader settings: language %s, mods %s", i18n::LanguageCode(i18n::Language()), g_modsEnabled ? "on" : "off");
}

void SaveSettingsIfDirty() {
    if (!g_dirty.exchange(false)) return;
    std::string code = i18n::LanguageCode(i18n::Language());
    WritePrivateProfileStringW(L"Loader", L"Language", std::wstring(code.begin(), code.end()).c_str(), IniPath().c_str());
    WritePrivateProfileStringW(L"Loader", L"ModsEnabled", g_modsEnabled ? L"1" : L"0", IniPath().c_str());
}

void OnReflectionReady() {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    g_header.reflection = true;
}

void GameThreadTick() {
    if (!ue::Ready()) return;
    static DWORD pausedUntil = 0;
    if (GetTickCount() < pausedUntil) return;
    if (SafeTick()) {
        Log("game tick: access violation, pausing 5 s");
        g_engine = nullptr;
        g_attrSet = nullptr;
        pausedUntil = GetTickCount() + 5000;
    }
}

Context GetContext() { return {g_engine, g_world, g_pc, g_pawn, g_ps, g_attrSet}; }

bool GetAttr(const char* name, float& base, float& cur) {
    uint8_t* a = AttrPtr(name);
    if (!a) return false;
    base = *(float*)(a + g_baseOff);
    cur = *(float*)(a + g_curOff);
    return true;
}

bool SetAttr(const char* name, float base, float cur) {
    uint8_t* a = AttrPtr(name);
    if (!a) return false;
    float ob = *(float*)(a + g_baseOff), oc = *(float*)(a + g_curOff);
    *(float*)(a + g_baseOff) = base;
    *(float*)(a + g_curOff) = cur;
    Notify(name, ob, oc);
    return true;
}

bool ExecConsoleCommand(const std::string& cmd) { return ExecConsole(cmd); }

int WatchClass(const std::string& name) {
    for (size_t w = 0; w < g_watches.size(); ++w)
        if (g_watches[w].name == name) return (int)w;
    g_watches.push_back({name, {}});
    g_watchMemo.clear();  // classes seen before must be checked against the new name
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

bool ModsEnabled() { return g_modsEnabled; }

void SetForcedTab(int tab) { g_forcedTab = tab; }

bool DrawMenu() {
    HeaderStatus h;
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        h = g_header;
    }
    bool open = true;
    ImGui::SetNextWindowPos({0, 0}, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
    if (!ImGui::Begin(u8"Dragonkin Mod Loader  ·  F7###panel", &open,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return open;
    }
    if (!h.reflection)
        ImGui::TextColored({0.95f, 0.75f, 0.30f, 1}, "%s", T("● Scanning game data..."));
    else if (!h.pawn)
        ImGui::TextColored({0.95f, 0.75f, 0.30f, 1}, "%s", T("● Waiting for character (enter the game)"));
    else
        ImGui::TextColored({0.45f, 0.85f, 0.45f, 1}, "%s", i18n::F("● Connected: %s", h.pawnName.c_str()).c_str());

    int forced = g_forcedTab;
    int tabIndex = 0;
    auto flags = [&]() { return forced == tabIndex++ ? ImGuiTabItemFlags_SetSelected : 0; };
    if (ImGui::BeginTabBar("tabs")) {
        int n = modloader::PluginTabCount();
        for (int i = 0; i < n; ++i) {
            ImGui::PushID(i);
            std::string label = modloader::PluginTabName(i) + "###ptab";
            if (ImGui::BeginTabItem(label.c_str(), nullptr, flags())) {
                modloader::DrawPluginTab(i);
                ImGui::EndTabItem();
            }
            ImGui::PopID();
        }
        if (ImGui::BeginTabItem((std::string(T("Mods")) + "###mods").c_str(), nullptr, flags())) {
            modloader::DrawModsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem((std::string(T("Developer")) + "###dev").c_str(), nullptr, flags())) {
            modloader::DrawDeveloperTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem((std::string(T("Settings")) + "###settings").c_str(), nullptr, flags())) {
            DrawSettingsTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
    return open;
}

}  // namespace core
