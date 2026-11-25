# Dragonkin Mod Loader 1.0.0

A mod loader and in-game mod menu for **Dragonkin: The Banished**.
Press **F7** in game to open the panel.

* One file (`dwmapi.dll`). No other tools, no game files are changed.
* Mods are folders in `Binaries\Win64\Mods\`. Switch them on and off in the **Mods** tab;
  their settings appear right there.
* Most mods are a single `mod.json` (no programming): sliders, toggles, buttons,
  hotkeys, and changes to player stats or game data. Advanced mods can add a native
  plugin with their own panel tabs.
* Mods never overwrite each other: every change goes through one shared registry, and
  switching a mod off gives the game its own value back.
* A crashing mod is switched off instead of the game crashing.
* The panel never takes the game's focus and stays on the game's screen, windowed or
  fullscreen.
* English, Türkçe, Français, Deutsch, العربية, 繁體中文, 简体中文 (Settings tab).
* Developer tab: a live inspector of the running game for mod makers.

## Install
Extract the archive into the game folder `Dragonkin - The Banished` (so that
`DragonkinTheBanished\Binaries\Win64\dwmapi.dll` exists), or copy `dwmapi.dll` and the
`Mods` folder into `DragonkinTheBanished\Binaries\Win64\` by hand.

## Install a mod
Put the mod's folder into `DragonkinTheBanished\Binaries\Win64\Mods\`, start the game
(or press **Reload mods**) and switch it on in the **Mods** tab. A mod that contains a
program file (plugin DLL) also needs **Allow native code** — tick it only for mods you
trust.

## Uninstall
Delete `dwmapi.dll` from `DragonkinTheBanished\Binaries\Win64\` (and the `Mods` and
`DragonkinModLoader` folders, `DragonkinMod.ini`, `DragonkinMod.log` if you like).

## Make mods
`docs\MODDING.md` explains the mod.json format and the plugin API (`sdk\dml_plugin.h`).
Three examples are included in `Mods` (switched off): basics, game data, native plugin.

## Notes
* Use mods offline / solo. Online cities and co-op are checked by the server.
* Problems: `Binaries\Win64\DragonkinMod.log` records every step.
* Includes Dear ImGui (MIT), see `THIRD_PARTY_NOTICES.txt`.
