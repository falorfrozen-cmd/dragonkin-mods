#pragma once
// Dragonkin Plus (a plugin of the Dragonkin Mod Loader): multipliers, cheats,
// convenience features, console variables and a status page, drawn as panel tabs.
#include <string>
#include <vector>

namespace game {

void LoadSettings();
void SaveSettingsIfDirty();
void OnReflectionReady();
void SetEnabled(bool on);  // the mod's switch in the loader's Mods tab
void ClaimAttributes();    // tell the loader which attributes the Multipliers tab manages

// Game thread.
void GameThreadTick();
bool DevCommand(const std::string& line, std::string& out);  // developer commands of this mod

// Panel thread, inside the loader's ImGui frame.
int TabCount();
const char* TabName(int index);
void DrawTab(int index);

// --- used by the developer commands (game thread) ---
struct Context {
    void* engine;
    void* world;
    void* pc;
    void* pawn;
    void* ps;
    void* attrSet;
};
Context GetContext();
bool GetAttr(const char* name, float& base, float& cur);
bool SetAttr(const char* name, float base, float cur);
bool SetSetting(const std::string& key, float value);
std::string SettingsSummary();
bool ExecConsoleCommand(const std::string& cmd);
std::string WaypointReport(bool unlock);
std::string RunAction(const std::string& name, int arg);
std::string QolReport();
int WatchClass(const std::string& className);
std::vector<void*> WatchedInstances(int watch);
bool IsBuiltinMultiplier(const std::string& attr);
bool ModsEnabled();
std::string EnemyAttrReport(void* asc);
bool ModifyAttribute(void* asc, const std::string& name, float value, int op);

}  // namespace game
