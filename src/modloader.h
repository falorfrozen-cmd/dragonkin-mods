#pragma once
// Dragonkin Mod Loader: loads mods from Binaries\Win64\Mods\<folder>\mod.json.
// A mod declares controls (sliders, toggles, buttons), effects (player attributes,
// object properties, locks, console commands), hotkeys and optionally a native plugin.
// See docs\MODDING.md for the format.
#include <string>

namespace modloader {

constexpr const char* kVersion = "1.0.0";
constexpr int kSchema = 1;

void Init();  // worker thread, after settings are loaded: reads the mods folder
void Tick();  // game thread: applies effects, runs queued actions, saves settings
void Shutdown();

// Game thread, from the game window: hotkeys of enabled mods.
void OnKeyDown(unsigned vk);

// Worker thread, right after reflection is ready: loads the plugins of enabled, allowed
// mods as early as possible (some need to change game data before the game uses it).
void LoadEnabledPlugins();

// Panel thread, inside the ImGui frame.
void DrawModsTab();
void DrawDeveloperTab();
int PluginTabCount();             // tabs added by enabled plugin mods
std::string PluginTabName(int i);
void DrawPluginTab(int i);

// Game thread: developer commands that a plugin handles (OnCommand).
bool PluginCommand(const std::string& line, std::string& out);

// Testing / developer channel (game thread).
std::string Report();
bool SetModEnabled(const std::string& id, bool on);
bool SetControl(const std::string& id, const std::string& control, double value);
bool PressButton(const std::string& id, const std::string& control);
void RequestReload();
void SetTrustNative(const std::string& id, bool on);

}  // namespace modloader
