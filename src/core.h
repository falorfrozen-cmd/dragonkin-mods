#pragma once
// Dragonkin Mod Loader core: the game context (engine, world, player), player
// attributes, the console, class watching for mods, loader settings and the panel shell.
#include <string>
#include <vector>

namespace core {

struct Context {
    void* engine;
    void* world;
    void* pc;
    void* pawn;
    void* ps;
    void* attrSet;
};

void LoadSettings();        // worker thread, before anything else
void SaveSettingsIfDirty();  // worker thread, every few seconds
void OnReflectionReady();   // worker thread
void GameThreadTick();      // game thread (window message)

// --- game thread ---
Context GetContext();
bool GetAttr(const char* name, float& base, float& cur);
bool SetAttr(const char* name, float base, float cur);  // writes and notifies GAS (OnRep_<name>)
bool ExecConsoleCommand(const std::string& cmd);
int WatchClass(const std::string& className);            // live instances of a class and its subclasses,
std::vector<void*> WatchedInstances(int watch);          // collected by a background scan
bool ModsEnabled();                                       // global switch in the Settings tab

// --- panel thread ---
bool DrawMenu();             // false when the user closed the window
void SetForcedTab(int tab);  // testing: select the n-th tab on the next frames (-1 = off)

}  // namespace core
