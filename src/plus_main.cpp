// Dragonkin Plus — a plugin of the Dragonkin Mod Loader (Mods\DragonkinPlus\DragonkinPlus.dll).
#include <cstring>
#include "common.h"
#include "game.h"
#include "i18n.h"
#include "imgui.h"
#include "plus_host.h"
#include "ue.h"

const DmlHost* g_host = nullptr;

namespace {

HMODULE g_self = nullptr;
DWORD g_lastSave = 0;

// Use the loader's ImGui context and allocator (the context may only exist once the
// panel is up, so this runs before every UI call rather than once in DmlInit).
void EnsureImGui() {
    if (!g_host || !g_host->imguiContext) return;
    auto* ctx = static_cast<ImGuiContext*>(g_host->imguiContext);
    if (ImGui::GetCurrentContext() == ctx) return;
    ImGui::SetAllocatorFunctions(g_host->imguiAlloc, g_host->imguiFree, g_host->imguiAllocUser);
    ImGui::SetCurrentContext(ctx);
}

void SyncLanguage() {
    if (g_host && g_host->apiVersion >= 2) i18n::SetLanguage(g_host->GetLanguage());
}

// Settings used to live next to the loader (DragonkinMod.ini); keep them on first run.
void MigrateSettings() {
    std::wstring mine = ModDir() + L"DragonkinPlus.ini";
    if (GetFileAttributesW(mine.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    std::wstring old = ModDir() + L"..\\..\\DragonkinMod.ini";
    if (GetFileAttributesW(old.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    if (CopyFileW(old.c_str(), mine.c_str(), TRUE)) Log("settings copied from DragonkinMod.ini");
}

void OnEnable() {
    game::SetEnabled(true);
    Log("enabled");
}

void OnDisable() {
    game::SetEnabled(false);
    if (ue::Ready()) game::GameThreadTick();  // one more tick gives every value back to the game
    game::SaveSettingsIfDirty();
    Log("disabled");
}

void OnTick(float) {
    SyncLanguage();
    if (!ue::Ready()) {
        static DWORD lastTry = 0;  // reflection did not come up in DmlInit: keep trying
        if (GetTickCount() - lastTry > 1000) {
            lastTry = GetTickCount();
            if (ue::Init()) game::OnReflectionReady();
        }
        return;
    }
    game::GameThreadTick();
    if (GetTickCount() - g_lastSave > 2000) {
        g_lastSave = GetTickCount();
        game::SaveSettingsIfDirty();
    }
}

int GetTabCount() { return game::TabCount(); }
const char* GetTabName(int i) {
    EnsureImGui();
    SyncLanguage();
    return game::TabName(i);
}
void DrawTab(int i) {
    EnsureImGui();
    SyncLanguage();
    game::DrawTab(i);
}

int OnCommand(const char* line, char* out, int outSize) {
    std::string reply;
    if (!line || !game::DevCommand(line, reply)) return 0;
    strncpy_s(out, outSize, reply.c_str(), _TRUNCATE);
    return 1;
}

}  // namespace

extern "C" __declspec(dllexport) int DmlInit(const DmlHost* host, DmlPlugin* plugin) {
    if (!host || host->apiVersion < 2) return 0;  // needs panel tabs and the value registry
    if (!host->imguiVersion || strcmp(host->imguiVersion, IMGUI_VERSION) != 0) return 0;
    g_host = host;

    LogInit(g_self, L"DragonkinPlus.log");
    Log("Dragonkin Plus starting (build %s %s), loader API %d", __DATE__, __TIME__, host->apiVersion);
    MigrateSettings();
    game::LoadSettings();
    game::SetEnabled(false);  // until the loader calls OnEnable
    game::ClaimAttributes();
    SyncLanguage();
    // Usually instant (the loader found the same structures); applies the configured bag
    // and chest sizes before the game creates its inventories.
    for (int attempt = 0; attempt < 20 && !ue::Init(); ++attempt) Sleep(100);
    // The loader loads a plugin this early only for an enabled mod, and OnEnable follows on
    // the first game tick; the bag/chest sizes must be in place before that, so this first
    // application runs as enabled.
    game::SetEnabled(true);
    if (ue::Ready()) game::OnReflectionReady();
    game::SetEnabled(false);

    plugin->apiVersion = DML_API_VERSION;
    plugin->OnEnable = OnEnable;
    plugin->OnDisable = OnDisable;
    plugin->OnTick = OnTick;
    plugin->GetTabCount = GetTabCount;
    plugin->GetTabName = GetTabName;
    plugin->DrawTab = DrawTab;
    plugin->OnCommand = OnCommand;
    return 1;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
