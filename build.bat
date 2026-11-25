@echo off
rem Builds the Dragonkin Mod Loader (build\dwmapi.dll), the Dragonkin Plus plugin
rem (build\plus\DragonkinPlus.dll) and the SDK example plugin. Needs Visual Studio (C++).
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\dkmod_vsdir.txt"
set /p VSDIR=<"%TEMP%\dkmod_vsdir.txt"
del "%TEMP%\dkmod_vsdir.txt" >nul 2>&1
if not defined VSDIR (echo Visual Studio C++ tools not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cd /d "%~dp0"
for %%d in (build build\loader build\plus build\plugin) do if not exist %%d mkdir %%d
set IMGUI=vendor\imgui\imgui.cpp vendor\imgui\imgui_draw.cpp vendor\imgui\imgui_tables.cpp vendor\imgui\imgui_widgets.cpp

rem --- Dragonkin Mod Loader: dwmapi.dll proxy, panel, mod loader ---
rc /nologo /fo build\loader\loader.res res\loader.rc || exit /b 1
cl /nologo /std:c++17 /utf-8 /O2 /MT /EHa /W3 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /Isrc /Ivendor\imgui ^
   src\main.cpp src\core.cpp src\overlay.cpp src\modloader.cpp src\devcmd.cpp src\common.cpp src\ue.cpp src\i18n.cpp src\json.cpp ^
   %IMGUI% vendor\imgui\imgui_impl_dx11.cpp vendor\imgui\imgui_impl_win32.cpp ^
   /Fobuild\loader\ /Fdbuild\loader\ /LD /Febuild\dwmapi.dll ^
   /link /NOLOGO /MAP:build\dwmapi.map build\loader\loader.res user32.lib gdi32.lib shell32.lib d3d11.lib dxgi.lib d3dcompiler.lib || exit /b 1

rem --- Dragonkin Plus: a plugin of the loader (dynamic CRT + version resource) ---
rc /nologo /fo build\plus\plus.res res\plus.rc || exit /b 1
cl /nologo /std:c++17 /utf-8 /O2 /MD /EHa /W3 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /Isrc /Ivendor\imgui ^
   src\plus_main.cpp src\game.cpp src\common.cpp src\ue.cpp src\i18n.cpp %IMGUI% ^
   /Fobuild\plus\ /Fdbuild\plus\ /LD /Febuild\plus\DragonkinPlus.dll ^
   /link /NOLOGO /MAP:build\plus\DragonkinPlus.map build\plus\plus.res user32.lib || exit /b 1

rem --- SDK example plugin (examples\ExamplePlugin) ---
rem /MD (the Visual C++ runtime the game already uses) and a version resource: a tiny
rem static-CRT DLL without version info gets machine-learning false positives.
rc /nologo /fo build\plugin\ExamplePlugin.res sdk\example\ExamplePlugin.rc || exit /b 1
cl /nologo /O2 /MD /W3 /GS /guard:cf /D_CRT_SECURE_NO_WARNINGS /Isdk sdk\example\ExamplePlugin.c ^
   /Fobuild\plugin\ /LD /Febuild\plugin\ExamplePlugin.dll ^
   /link /NOLOGO /GUARD:CF /DYNAMICBASE /NXCOMPAT build\plugin\ExamplePlugin.res || exit /b 1
exit /b 0
