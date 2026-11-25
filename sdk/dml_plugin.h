/*
 * Dragonkin Mod Loader - native plugin API (C ABI).
 *
 * A mod folder may contain a plugin DLL next to its mod.json ("plugin": "MyPlugin.dll").
 * The loader only loads it after the player ticks "Allow native code" for that mod.
 * Callbacks run inside a fault guard: a crash in a plugin switches that mod off
 * instead of crashing the game.
 *
 * Threads: DmlInit may run on a loader worker thread (plugins of enabled mods are
 * loaded as early as possible at start-up); OnEnable, OnDisable, OnTick, OnAction and
 * OnCommand run on the game thread; GetTabCount/GetTabName/DrawTab run on the panel
 * thread inside an ImGui frame.
 *
 * Version 1: Log, Exec, Console, controls, attributes; OnEnable/OnDisable/OnTick/OnAction.
 * Version 2 adds: panel tabs drawn with Dear ImGui (same version as the loader, with the
 * loader's context and allocator), the shared value registry, attribute claiming,
 * the current language, developer commands and the plugin's folder.
 * Fields are only ever appended; check apiVersion before using newer ones.
 */
#ifndef DML_PLUGIN_H
#define DML_PLUGIN_H
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DML_API_VERSION 2

/* Operations for ContributeProperty */
#define DML_OP_MULTIPLY 0
#define DML_OP_SET 1
#define DML_OP_ADD 2

typedef struct DmlHost {
    int apiVersion; /* DML_API_VERSION of the loader */
    /* --- version 1 --- */
    /* Writes a line to the loader log and to the mod's log in the Mods tab. */
    void (*Log)(const char* text);
    /* Runs a developer command (see the Developer tab). Returns its output; the text
       stays valid until the next Exec call on the same thread. */
    const char* (*Exec)(const char* commandLine);
    /* Runs a game console command. Returns non-zero on success. */
    int (*Console)(const char* command);
    /* Values of this mod's controls declared in mod.json (toggles are 0/1). */
    double (*GetControl)(const char* controlId);
    void (*SetControl)(const char* controlId, double value);
    /* Player attributes of HnSUberAttributeSet ("Health", "XPMultiplier", ...).
       Return non-zero on success. SetAttribute notifies the game like replication does. */
    int (*GetAttribute)(const char* name, float* baseValue, float* currentValue);
    int (*SetAttribute)(const char* name, float baseValue, float currentValue);

    /* --- version 2 --- */
    /* Dear ImGui: build the plugin with the same ImGui version (imguiVersion). Before your
       UI code runs (GetTabName/DrawTab) call ImGui::SetAllocatorFunctions(imguiAlloc,
       imguiFree, imguiAllocUser) and ImGui::SetCurrentContext(imguiContext). Read these
       fields there, not in DmlInit: the panel (and its context) may not exist yet when
       plugins of enabled mods are loaded at start-up. */
    const char* imguiVersion;
    void* imguiContext;
    void* (*imguiAlloc)(size_t size, void* user);
    void (*imguiFree)(void* ptr, void* user);
    void* imguiAllocUser;
    /* UI language: 0 en, 1 tr, 2 fr, 3 de, 4 ar, 5 zh-Hant, 6 zh-Hans. */
    int (*GetLanguage)(void);
    /* Shared value registry (game thread). Call every tick while you want a change of a
       float/double/int/bool field reached by `path` from `owner` (a UObject*); stop calling
       and the game's own value comes back. Changes by several mods are combined instead of
       overwriting each other. Path syntax as in mod.json ("m_XpGains.*.m_fGain"). Returns
       the number of values found. */
    int (*ContributeProperty)(void* owner, const char* path, int op, double value, const char* sourceName);
    /* Attributes this plugin manages itself (for example its own multiplier sliders). Other
       mods' "multiply" effects on a claimed attribute are then not applied by the loader but
       reported through GetAttributeFactor, which the plugin multiplies into its own value. */
    void (*ClaimAttribute)(const char* name);
    float (*GetAttributeFactor)(const char* name);
    /* Folder of this mod, with a trailing backslash (UTF-8). */
    const char* (*ModFolder)(void);
    /* The global "Enable mods" switch of the Settings tab. */
    int (*ModsEnabled)(void);
} DmlHost;

typedef struct DmlPlugin {
    int apiVersion;                       /* set to DML_API_VERSION you were built with */
    /* --- version 1 --- */
    void (*OnEnable)(void);               /* mod switched on (also at start-up if it was on) */
    void (*OnDisable)(void);              /* mod switched off, reloaded, or the loader stops */
    void (*OnTick)(float seconds);        /* game thread, while enabled */
    void (*OnAction)(const char* action); /* {"plugin": "action"} steps of buttons/hotkeys */
    /* --- version 2 --- */
    /* Panel tabs, shown while the mod is enabled. Names are display text (UTF-8). */
    int (*GetTabCount)(void);
    const char* (*GetTabName)(int index);
    void (*DrawTab)(int index);
    /* Developer commands: return non-zero if `line` was yours and write the reply to `out`. */
    int (*OnCommand)(const char* line, char* out, int outSize);
} DmlPlugin;

/* Exported by the plugin DLL. Fill `plugin` and return 1 to load, 0 to refuse. */
typedef int (*DmlInitFn)(const DmlHost* host, DmlPlugin* plugin);
#define DML_INIT_EXPORT "DmlInit"

#ifdef __cplusplus
}
#endif
#endif
