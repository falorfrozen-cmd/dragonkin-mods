// Menu panel. Earlier versions drew into the game's own D3D12 swap chain, which
// crashed the GPU in the real game ("GPU Crash dump Triggered"). The panel is now
// a separate window with its own D3D11 device: the game's renderer is never touched.
#include "overlay.h"
#include <d3d11.h>
#include <dxgi.h>
#include <windowsx.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <vector>
#include "core.h"
#include "modloader.h"
#include "i18n.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#pragma comment(lib, "d3d11.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace overlay {
namespace {

constexpr UINT WM_MOD_TICK = WM_APP + 0x3D7;
constexpr UINT kMenuKey = VK_F7;

HWND g_game = nullptr;
HWND g_panel = nullptr;
WNDPROC g_origWndProc = nullptr;
std::atomic<bool> g_menuOpen{false};
std::atomic<bool> g_wantText{false};
std::atomic<bool> g_tickPending{false};
std::atomic<DWORD> g_tickPostedAt{0};

float g_scale = 1.0f;
int g_width = 580, g_height = 720;  // current panel size (height may shrink to fit the screen)
int g_fullHeight = 720;              // preferred height
POINT g_offset{40, 40};              // panel position relative to the game's client area (set by the user's drag)
std::atomic<bool> g_dragging{false};
std::atomic<bool> g_panelReady{false};

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;

// ---------------------------------------------------------------------------
// Game window (game thread)
// ---------------------------------------------------------------------------
LRESULT CALLBACK HookedWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_MOD_TICK) {
        g_tickPending = false;
        core::GameThreadTick();
        return 0;
    }
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && w == kMenuKey) {
        if (!(l & (1 << 30))) {  // ignore auto-repeat
            g_menuOpen = !g_menuOpen;
            Log("menu %s", g_menuOpen ? "opened" : "closed");
        }
        return 0;
    }
    if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && w == kMenuKey) return 0;
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(l & (1 << 30)) && !(g_menuOpen && g_wantText))
        modloader::OnKeyDown((unsigned)w);  // mod hotkeys (the key still reaches the game)
    // The panel never takes keyboard focus (so the game keeps running in fullscreen);
    // while a text box in the panel is active, forward typing to it.
    if (g_menuOpen && g_wantText && g_panel &&
        (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)) {
        PostMessageW(g_panel, msg, w, l);
        return 0;
    }
    return CallWindowProcW(g_origWndProc, h, msg, w, l);
}

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM param) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd)) return TRUE;
    wchar_t cls[64];
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"UnrealWindow") != 0) return TRUE;
    *reinterpret_cast<HWND*>(param) = hwnd;
    return FALSE;
}

// ---------------------------------------------------------------------------
// Panel window (panel thread)
// ---------------------------------------------------------------------------
POINT GameClientOrigin() {
    POINT p{0, 0};
    ClientToScreen(g_game, &p);
    return p;
}

// Where the panel should be: the user's offset from the game's client area, kept
// fully inside the monitor that shows the game. The offset itself is never changed
// here, so going back from fullscreen to a window puts the panel back where it was.
RECT ComputePanelRect() {
    POINT o = GameClientOrigin();
    MONITORINFO mi{sizeof(mi)};
    HMONITOR mon = MonitorFromWindow(g_game, MONITOR_DEFAULTTONEAREST);
    RECT area{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (mon && GetMonitorInfoW(mon, &mi)) area = mi.rcMonitor;
    int margin = (int)(8 * g_scale);
    int maxH = (area.bottom - area.top) - 2 * margin;
    int maxW = (area.right - area.left) - 2 * margin;
    int h = std::min(g_fullHeight, std::max(maxH, (int)(200 * g_scale)));
    int w = std::min(g_width, std::max(maxW, (int)(200 * g_scale)));
    int x = o.x + g_offset.x, y = o.y + g_offset.y;
    x = std::max((int)area.left + margin, std::min(x, (int)area.right - margin - w));
    y = std::max((int)area.top + margin, std::min(y, (int)area.bottom - margin - h));
    return {x, y, x + w, y + h};
}

void ResizeTargets(int w, int h) {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
    g_swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

// Moves/resizes the native window to ComputePanelRect() when it differs.
void PlacePanel(bool show) {
    RECT want = ComputePanelRect(), cur;
    GetWindowRect(g_panel, &cur);
    int w = want.right - want.left, h = want.bottom - want.top;
    bool resize = (cur.right - cur.left) != w || (cur.bottom - cur.top) != h;
    if (!show && !resize && cur.left == want.left && cur.top == want.top) return;
    SetWindowPos(g_panel, HWND_TOPMOST, want.left, want.top, w, h,
                 SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : 0) | (resize ? 0 : SWP_NOSIZE));
    if (resize) {
        g_height = h;
        ResizeTargets(w, h);
        Log("panel resized to %dx%d to fit the screen", w, h);
    }
}

LRESULT CALLBACK PanelProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(h, msg, w, l)) return 1;
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;  // clicking the panel must not take focus from the game
    case WM_NCHITTEST: {
        // The ImGui title bar drags the native window (except the close button).
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(h, &p);
        if (p.y >= 0 && p.y < (int)(30 * g_scale) && p.x < g_width - (int)(40 * g_scale)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_ENTERSIZEMOVE:
        g_dragging = true;
        break;
    case WM_EXITSIZEMOVE:
    case WM_MOVE:
        // Only a drag by the user changes the saved position. Moves made by us or by
        // the system (e.g. while the game switches to fullscreen) must not, otherwise a
        // half-finished game window position gets saved and the panel ends up off-screen.
        if (g_dragging) {
            RECT r;
            GetWindowRect(h, &r);
            POINT o = GameClientOrigin();
            g_offset = {r.left - o.x, r.top - o.y};
        }
        if (msg == WM_EXITSIZEMOVE) {
            g_dragging = false;
            PlacePanel(false);  // keep it on screen if it was dragged past the edge
        }
        return 0;
    case WM_KEYDOWN:
        if (w == kMenuKey) {
            g_menuOpen = false;
            return 0;
        }
        break;
    case WM_CLOSE:
        g_menuOpen = false;
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

bool CreateDevice() {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = g_width;
    sd.BufferDesc.Height = g_height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_panel;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                               &sd, &g_swap, &g_dev, &fl, &g_ctx);
    if (FAILED(hr))  // no usable GPU path: fall back to the software rasterizer
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd,
                                           &g_swap, &g_dev, &fl, &g_ctx);
    if (FAILED(hr)) {
        Log("panel: D3D11 device creation failed 0x%08lX", (unsigned long)hr);
        return false;
    }
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
    return true;
}

void SetupStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 0;
    s.FrameRounding = 5;
    s.GrabRounding = 5;
    s.TabRounding = 5;
    s.WindowPadding = {14, 12};
    s.ItemSpacing = {8, 7};
    s.WindowBorderSize = 1;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = {0.07f, 0.06f, 0.07f, 1.0f};
    c[ImGuiCol_Border] = {0.62f, 0.22f, 0.15f, 1};
    c[ImGuiCol_TitleBgActive] = {0.45f, 0.12f, 0.10f, 1};
    c[ImGuiCol_TitleBg] = {0.45f, 0.12f, 0.10f, 1};
    c[ImGuiCol_Tab] = {0.30f, 0.10f, 0.09f, 1};
    c[ImGuiCol_TabSelected] = {0.62f, 0.20f, 0.14f, 1};
    c[ImGuiCol_TabHovered] = {0.75f, 0.28f, 0.18f, 1};
    c[ImGuiCol_SliderGrab] = {0.90f, 0.45f, 0.20f, 1};
    c[ImGuiCol_SliderGrabActive] = {1.00f, 0.60f, 0.30f, 1};
    c[ImGuiCol_CheckMark] = {1.00f, 0.60f, 0.30f, 1};
    c[ImGuiCol_FrameBg] = {0.18f, 0.12f, 0.11f, 1};
    c[ImGuiCol_FrameBgHovered] = {0.30f, 0.17f, 0.14f, 1};
    c[ImGuiCol_FrameBgActive] = {0.40f, 0.20f, 0.16f, 1};
    c[ImGuiCol_Button] = {0.40f, 0.14f, 0.11f, 1};
    c[ImGuiCol_ButtonHovered] = {0.62f, 0.22f, 0.15f, 1};
    c[ImGuiCol_ButtonActive] = {0.75f, 0.30f, 0.18f, 1};
    c[ImGuiCol_Header] = {0.40f, 0.14f, 0.11f, 1};
    c[ImGuiCol_HeaderHovered] = {0.62f, 0.22f, 0.15f, 1};
    c[ImGuiCol_Separator] = {0.62f, 0.22f, 0.15f, 1};
    s.ScaleAllSizes(g_scale);
}

// The panel is visible while the game (or any window of the game process) is in front,
// so alt-tabbing to another program hides it.
const char* HiddenReason() {
    if (!g_game || IsIconic(g_game)) return "game minimized";
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) return "another program is in front";
    return nullptr;
}

std::atomic<int> g_snapshotTab{-1};

// Segoe UI for Latin text, Arial merged in for Arabic presentation forms, and a CJK
// font (JhengHei first for Traditional Chinese, YaHei otherwise). Rebuilt when the
// language changes; must run outside NewFrame/Render.
void EnsureFonts() {
    static int loadedFor = -2;
    int lang = i18n::Language();
    bool tc = lang == i18n::ZH_HANT;
    int key = tc ? 1 : 0;
    if (key == loadedFor) return;
    loadedFor = key;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    char dir[MAX_PATH];
    GetWindowsDirectoryA(dir, MAX_PATH);
    auto path = [&](const char* f) { return std::string(dir) + "\\Fonts\\" + f; };
    auto exists = [&](const char* f) { return GetFileAttributesA(path(f).c_str()) != INVALID_FILE_ATTRIBUTES; };
    float size = 18.0f * g_scale;
    if (exists("segoeui.ttf"))
        io.Fonts->AddFontFromFileTTF(path("segoeui.ttf").c_str(), size);
    else
        io.Fonts->AddFontDefault();
    ImFontConfig merge;
    merge.MergeMode = true;
    const char* extra[] = {"arial.ttf", tc ? "msjh.ttc" : "msyh.ttc", tc ? "msyh.ttc" : nullptr};
    for (const char* f : extra)
        if (f && exists(f)) io.Fonts->AddFontFromFileTTF(path(f).c_str(), size, &merge);
    Log("fonts loaded for %s", i18n::LanguageCode(lang));
}

void RenderSnapshot(int tab) {
    core::SetForcedTab(tab);
    EnsureFonts();
    const float clear[4] = {0.07f, 0.06f, 0.07f, 1.0f};
    for (int frame = 0; frame < 4; ++frame) {  // a few frames so tabs and layout settle
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        core::DrawMenu();
        ImGui::Render();
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    core::SetForcedTab(-1);
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    D3D11_TEXTURE2D_DESC d;
    back->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, &staging))) {
        g_ctx->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            std::wstring path = ModDir() + L"DragonkinMod_snap_" + std::to_wstring(tab) + L".bmp";
            FILE* f = nullptr;
            if (!_wfopen_s(&f, path.c_str(), L"wb") && f) {
                BITMAPFILEHEADER fh{};
                BITMAPINFOHEADER ih{};
                ih.biSize = sizeof(ih);
                ih.biWidth = (LONG)d.Width;
                ih.biHeight = -(LONG)d.Height;  // top-down
                ih.biPlanes = 1;
                ih.biBitCount = 32;
                fh.bfType = 0x4D42;
                fh.bfOffBits = sizeof(fh) + sizeof(ih);
                fh.bfSize = fh.bfOffBits + d.Width * d.Height * 4;
                fwrite(&fh, sizeof(fh), 1, f);
                fwrite(&ih, sizeof(ih), 1, f);
                std::vector<uint8_t> row(d.Width * 4);
                for (UINT y = 0; y < d.Height; ++y) {
                    const uint8_t* src = (const uint8_t*)m.pData + y * m.RowPitch;
                    for (UINT x = 0; x < d.Width; ++x) {  // RGBA -> BGRA
                        row[x * 4 + 0] = src[x * 4 + 2];
                        row[x * 4 + 1] = src[x * 4 + 1];
                        row[x * 4 + 2] = src[x * 4 + 0];
                        row[x * 4 + 3] = 255;
                    }
                    fwrite(row.data(), row.size(), 1, f);
                }
                fclose(f);
                Log("snapshot of tab %d written", tab);
            }
            g_ctx->Unmap(staging, 0);
        }
        staging->Release();
    }
    back->Release();
}

DWORD WINAPI PanelThread(LPVOID) {
    UINT dpi = GetDpiForWindow(g_game);
    g_scale = dpi ? dpi / 96.0f : 1.0f;
    g_width = (int)(580 * g_scale);
    g_height = (int)(720 * g_scale);
    g_fullHeight = g_height;
    g_offset = {(int)(40 * g_scale), (int)(40 * g_scale)};

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = PanelProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DragonkinModPanel";
    RegisterClassExW(&wc);
    g_panel = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"Dragonkin Mod", WS_POPUP,
                              0, 0, g_width, g_height, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_panel || !CreateDevice()) {
        Log("panel: window/device creation failed, menu unavailable");
        return 0;
    }

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    EnsureFonts();
    SetupStyle();
    ImGui_ImplWin32_Init(g_panel);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    Log("panel ready (%dx%d, scale %.2f)", g_width, g_height, g_scale);
    g_panelReady = true;

    bool shown = false;
    for (;;) {
        EnsureFonts();
        int snap = g_snapshotTab.exchange(-1);
        if (snap >= 0) RenderSnapshot(snap);
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        const char* reason = g_menuOpen ? HiddenReason() : "menu closed";
        bool want = reason == nullptr;
        if (want != shown) {
            if (want) {
                PlacePanel(true);
            } else {
                ShowWindow(g_panel, SW_HIDE);
            }
            shown = want;
            Log("panel %s%s%s", shown ? "shown" : "hidden", reason ? " â€” " : "", reason ? reason : "");
        }
        if (!shown) {
            Sleep(50);
            continue;
        }

        // Follow the game window (moves, fullscreen switches), staying on screen.
        if (!g_dragging) PlacePanel(false);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        if (!core::DrawMenu()) g_menuOpen = false;
        g_wantText = io.WantTextInput;
        ImGui::Render();
        const float clear[4] = {0.07f, 0.06f, 0.07f, 1.0f};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }
}

}  // namespace

HWND FindGameWindow() {
    HWND found = nullptr;
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&found));
    return found;
}

void InstallWndProc(HWND hwnd) {
    g_game = hwnd;
    g_origWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc)));
    Log("window procedure hooked (F7 = menu)");
}

void StartPanel() {
    if (HANDLE t = CreateThread(nullptr, 0, PanelThread, nullptr, 0, nullptr)) CloseHandle(t);
}

void RequestSnapshot(int tab) { g_snapshotTab = tab; }

bool PanelReady() { return g_panelReady; }

std::string PanelInfo() {
    RECT cur{}, want = ComputePanelRect(), game{};
    if (g_panel) GetWindowRect(g_panel, &cur);
    GetClientRect(g_game, &game);
    POINT o = GameClientOrigin();
    char b[400];
    snprintf(b, sizeof(b), "game client %ldx%ld at (%ld,%ld) | offset (%ld,%ld) | panel now (%ld,%ld %ldx%ld) | target (%ld,%ld %ldx%ld) | visible=%d",
             game.right, game.bottom, o.x, o.y, g_offset.x, g_offset.y, cur.left, cur.top, cur.right - cur.left, cur.bottom - cur.top,
             want.left, want.top, want.right - want.left, want.bottom - want.top, g_panel ? (int)IsWindowVisible(g_panel) : -1);
    return b;
}

void SetPanelOffset(int x, int y) { g_offset = {x, y}; }

void ResetPanelPosition() { g_offset = {(int)(40 * g_scale), (int)(40 * g_scale)}; }

void RequestGameThreadTick() {
    if (!g_game) return;
    if (g_tickPending && GetTickCount() - g_tickPostedAt < 3000) return;  // still waiting for the last one
    g_tickPending = true;
    g_tickPostedAt = GetTickCount();
    if (!PostMessageW(g_game, WM_MOD_TICK, 0, 0)) g_tickPending = false;
}

}  // namespace overlay

