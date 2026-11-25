# Dragonkin Mod Loader — making mods

The loader is `Binaries\Win64\dwmapi.dll`. It loads every folder in
`Binaries\Win64\Mods\` that contains a `mod.json`. Players switch mods on and
off in the **Mods** tab of the F7 panel; values are saved in `DragonkinMod.ini`.

Most mods need no programming: a mod is one JSON file that declares
**controls** (what the player sees), **effects** (what changes in the game) and
**hotkeys**. For anything else a mod can ship a small **native plugin** (C ABI).

Start by copying one of the examples (`Mods\ExampleBasics`, `Mods\ExampleProperties`,
`Mods\ExamplePlugin`), change it, and press **Reload mods** — the game does not need
to restart. Errors are shown in red under the mod, with the line number.

## 1. Design

* **Nothing is patched on disk.** Everything happens in memory while the game runs.
  Switch a mod off (or remove it) and the game's own values come back.
* **Mods cannot fight.** Every change goes through one registry. If two mods (or a
  mod and a built-in feature) change the same value, both changes are applied in
  order (built-in features first, then mods in folder order) and the Mods tab shows
  "shares values with …". A value that the game keeps rewriting is left alone for
  30 seconds instead of being stacked on.
* **Crashes are contained.** Effects and plugins run inside a fault guard. A mod that
  crashes three times is switched off and shows the reason; the game keeps running.
* **Safe memory access.** Game objects are found by name through Unreal's reflection
  (no fixed addresses), memory is only touched while its object is alive, and value
  addresses are looked up again every tick (arrays may move).
* **Offline only.** Online cities and co-op are server-checked; use mods solo.

## 2. mod.json

Comments (`//`, `/* */`) and trailing commas are allowed.

```jsonc
{
  "schema": 1,                       // format version (this loader: 1)
  "id": "my-mod",                    // required: letters, digits, - _ . (unique)
  "name": { "en": "My mod", "tr": "Modum" },   // or just "My mod"
  "version": "1.0.0",
  "author": "you",
  "description": { "en": "What it does." },
  "controls": [ ... ],
  "effects":  [ ... ],
  "hotkeys":  [ ... ],
  "plugin": "MyPlugin.dll"           // optional native plugin in the same folder
}
```

**Texts** (`name`, `description`, `label`, `tooltip`) are a string or an object of
languages: `en`, `tr`, `fr`, `de`, `ar`, `zh-Hant`, `zh-Hans`. English is the
fallback. Arabic is shaped and drawn right-to-left automatically.

### 2.1 Controls

| type     | fields                                                         | value |
|----------|----------------------------------------------------------------|-------|
| `slider` | `min`, `max`, `default`, `format` (`"%.2fx"`), `log` (bool)    | number |
| `int`    | `min`, `max`, `default`, `format` (`"%d"`)                     | whole number |
| `toggle` | `default` (true/false)                                         | 0 / 1 |
| `button` | `actions` (see 2.4)                                            | — |

Every control has an `id` and a `label`, and may have a `tooltip`.

### 2.2 Effects

Effects are active while the mod is on and, if given, while `"when": "$toggleId"`
(or `"!$toggleId"`) is true. `value` is a number, `true`/`false`, or
`"$controlId"` (the control's current value). `mode` is `multiply` (default),
`set` or `add`.

**attribute** — a player attribute of `HnSUberAttributeSet` (see the Status tab or
`attrs` in the Developer tab for all names):

```json
{ "type": "attribute", "attribute": "XPMultiplier", "value": "$xp" }
{ "type": "attribute", "attribute": "HealthRegen", "mode": "add", "value": 10 }
```

The multipliers of the Multipliers tab (XPMultiplier, ExtraGoldMultiplier, …) only
accept `multiply`; your factor is combined with the player's slider.

**property** — any float / double / int / bool field of a game object:

```json
{ "type": "property", "target": "cdo:HnSGlobalConfig",
  "path": "m_PlayerInventoryCapacityProperties.m_InventorySizes.[MainBuyback]",
  "mode": "set", "value": 64 }
{ "type": "property", "class": "HnSCameraView",
  "path": "m_pCameraBoom.TargetArmLength", "value": 1.5 }
{ "type": "property", "class": "HnSShared_GameData_CityRules",
  "path": "m_XpGains.*.m_fGain", "value": 2 }
```

* `target` — one object: `pawn`, `pc`, `ps`, `world`, `engine`, `gi`, `attrset`,
  `cdo:Class` (class defaults/config), `first:Class`, `near:Class`, or a dotted path
  from one of those (`ps.m_PlayerInventory`).
* `class` — every live instance of the class (and subclasses); new instances are
  found in the background as the game loads them.
* `path` — property names separated by dots. Object references are followed
  (`m_pCameraBoom.TargetArmLength`). For arrays and maps: `*` = every element,
  `[3]` = one array element, `[Key]` = one map entry (enum, name or number key).

**lock** — keeps an attribute at its maximum: `{ "type": "lock", "attribute": "Mana", "max": "ManaMax" }`

**console** — runs a game console command when the effect turns on/off:
`{ "type": "console", "when": "$bars", "on": "HnS.UI.AlwaysShowHealthBars 1", "off": "HnS.UI.AlwaysShowHealthBars 0" }`

### 2.3 Hotkeys

```json
{ "key": "F8", "toggle": "$bars" }
{ "key": "Ctrl+Shift+F9", "actions": [ { "command": "call pc ToggleTeleportWorldMap" } ] }
```

Keys: F1–F24, A–Z, 0–9, Numpad0–9, Insert, Delete, Home, End, PageUp, PageDown,
Pause, Space, Tab, Up/Down/Left/Right, Backspace, Enter, with `Ctrl+`, `Shift+`,
`Alt+`. F7 is reserved for the panel. Hotkeys work while the game window is active.

### 2.4 Actions (buttons and hotkeys)

| action | does |
|--------|------|
| `{ "command": "..." }` | a developer command (section 4); `${controlId}` is replaced by the control's value |
| `{ "console": "..." }` | a game console command |
| `{ "set": "controlId", "value": 1 }` | changes one of the mod's controls |
| `{ "plugin": "name" }` | calls the plugin's `OnAction("name")` |

## 3. Native plugins

Build a DLL with `sdk\dml_plugin.h` and export `DmlInit`. See
`sdk\example\ExamplePlugin.c` (API 1) and the Dragonkin Plus mod (API 2: its whole UI
is five panel tabs).

```c
__declspec(dllexport) int DmlInit(const DmlHost* host, DmlPlugin* plugin) {
    plugin->apiVersion = DML_API_VERSION;
    plugin->OnTick = MyTick;          // game thread, while the mod is on
    plugin->OnAction = MyAction;      // {"plugin": "..."} actions of buttons/hotkeys
    plugin->GetTabCount = MyTabCount; // API 2: tabs in the F7 panel
    plugin->GetTabName = MyTabName;
    plugin->DrawTab = MyDrawTab;      // panel thread, Dear ImGui
    return 1;
}
```

| Host function | |
|---|---|
| `Log`, `Exec`, `Console` | log line, developer command (section 4), game console |
| `GetControl` / `SetControl` | the controls declared in your mod.json |
| `GetAttribute` / `SetAttribute` | player attributes (GAS is notified) |
| `ContributeProperty` (2) | change a game value through the shared registry (combined with other mods, restored when you stop) |
| `ClaimAttribute` / `GetAttributeFactor` (2) | attributes you manage yourself; other mods' factors are handed to you |
| `GetLanguage`, `ModFolder`, `ModsEnabled` (2) | UI language, your folder, the global switch |
| `imguiContext`, `imguiAlloc`, `imguiFree` (2) | draw your tabs with Dear ImGui of the same version |

Rules:
* A plugin is only loaded after the player ticks **Allow native code** for that mod.
  Plugins of mods that are on are loaded at game start, before the game builds most of
  its data, so `DmlInit` may run on a worker thread and before the panel exists.
* `OnEnable`, `OnDisable`, `OnTick`, `OnAction`, `OnCommand` run on the game thread;
  `GetTabCount`, `GetTabName`, `DrawTab` on the panel thread. Every call is fault-guarded:
  a plugin that crashes three times is switched off.
* In `OnDisable` give every value back to the game (one more tick with your features off
  is the simplest way).
* For UI: before ImGui calls, `ImGui::SetAllocatorFunctions(host->imguiAlloc,
  host->imguiFree, host->imguiAllocUser)` and `ImGui::SetCurrentContext(host->imguiContext)`.
  Build with the ImGui version in `host->imguiVersion`.
* Build with `/MD` and a version resource (see `sdk\example\ExamplePlugin.rc`): small
  static-CRT DLLs without version info are often flagged by antivirus heuristics.

## 4. Finding what to change (Developer tab)

The Developer tab runs commands against the live game (the same commands can be
written to `Binaries\Win64\DragonkinMod_cmd.txt`; output goes to `DragonkinMod_out.txt`):

| command | example |
|---------|---------|
| `names <text>` | `names Potion` — names that exist in the game |
| `find <Class> [max]` | `find HnSCameraView` — live objects |
| `props <target> [filter]` | `props cdo:HnSGlobalConfig Camera` — fields and values |
| `get` / `set <target> <prop> [value]` | `get pawn m_pAbilitySystemComponent` |
| `sig <Class> <Function>` | parameters of a function |
| `call <target> <Function> [p=v ...]` | `call pc ToggleTeleportWorldMap` |
| `attrs [filter]` / `setattr <name> <v>` | player attributes |
| `pos`, `tp x y z`, `exec <console>` | position, teleport, console |
| `mods`, `modreload`, `modon <id> 0/1`, `modset <id> <control> <v>` | mod loader |

`Status → Write dump` writes every class of the game module with its fields to
`DragonkinMod_dump_HnS.txt` — the best map of what exists.

## 5. Checklist before publishing

* `id` is unique and stays the same between versions (settings are stored by id).
* Every slider has sensible `min`/`max`; the default does what the description says.
* Test: switch the mod on, off, and press Reload mods — values must come back.
* Give at least an English `name` and `description`.
* If you ship a plugin DLL, say so on your mod page and publish its source.
