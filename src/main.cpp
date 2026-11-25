// Dragonkin Mod Loader — loaded by the game as "dwmapi.dll".
// All real dwmapi exports are forwarded to C:\Windows\System32\dwmapi.dll (exports.h).
#include "common.h"
#include "devcmd.h"
#include "exports.h"
#include "core.h"
#include "modloader.h"
#include "overlay.h"
#include "ue.h"

static HMODULE g_self = nullptr;

static bool IsGameProcess() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"DragonkinTheBanished-Win64-Shipping.exe") == 0;
}

static DWORD WINAPI Worker(LPVOID) {
    Log("Dragonkin Mod Loader %s starting (build %s %s)", modloader::kVersion, __DATE__, __TIME__);
    InstallCrashLogger();
    core::LoadSettings();
    modloader::Init();

    // 1) Unreal reflection, as early as possible (it does not need the window). Retry
    //    until the engine has created its first objects.
    for (int attempt = 1; !ue::Init(); ++attempt) {
        if (attempt % 50 == 1) Log("Unreal reflection not ready yet (attempt %d)", attempt);
        Sleep(100);
    }
    core::OnReflectionReady();
    // 2) Plugins of enabled mods load right away, before the game creates its inventories
    //    (a plugin may change their configured sizes). They get the ImGui context when
    //    they first draw.
    modloader::LoadEnabledPlugins();

    // 3) Wait for the game window, then input (F7) + game-thread tick channel and the panel.
    HWND hwnd = nullptr;
    while (!(hwnd = overlay::FindGameWindow())) Sleep(500);
    Log("Game window found: %p", hwnd);
    Sleep(3000);  // let the renderer finish creating its device and swap chain
    overlay::InstallWndProc(hwnd);
    overlay::StartPanel();

    // 4) Main loop: request a game-thread tick every 50 ms, save settings when changed.
    DWORD lastSave = GetTickCount();
    for (;;) {
        dev::PollCommandFile();
        overlay::RequestGameThreadTick();
        if (GetTickCount() - lastSave > 2000) {
            core::SaveSettingsIfDirty();
            lastSave = GetTickCount();
        }
        Sleep(50);
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
        if (!IsGameProcess()) return TRUE;  // plain dwmapi forwarder anywhere else
        LogInit(module);
        if (HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
