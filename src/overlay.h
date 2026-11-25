#pragma once
#include "common.h"
#include <string>

namespace overlay {

HWND FindGameWindow();

// Subclasses the game window: F7 toggles the menu, and a private window message
// gives us a safe hook on the game thread. The game's rendering is never touched.
void InstallWndProc(HWND hwnd);

// Starts the menu panel: a separate borderless window that floats over the game,
// rendered on its own thread with its own small D3D11 device.
void StartPanel();

// Posts one tick message to the game thread (coalesced).
void RequestGameThreadTick();

// Testing: renders the menu off-screen with the given tab selected and saves it as
// DragonkinMod_snap_<tab>.bmp next to the DLL (the panel window stays hidden).
void RequestSnapshot(int tab);
std::string PanelInfo();             // testing: game/panel geometry
void SetPanelOffset(int x, int y);   // testing: simulate a stored position
void ResetPanelPosition();
bool PanelReady();  // the panel window and its ImGui context exist

}  // namespace overlay
