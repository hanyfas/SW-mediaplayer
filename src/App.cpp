#include "App.h"
#include "renderer/D3D11Renderer.h"
#include "audio/WASAPIRenderer.h"
#include "playlist/Playlist.h"
#include "playback/PlaybackEngine.h"
#include "ui/TrayIcon.h"
#include "ui/ConfigDialog.h"
#include <stdexcept>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

App* App::s_instance = nullptr;

App::App(HINSTANCE hInst) : m_hInst(hInst)
{
    s_instance = this;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Enable per-monitor DPI awareness
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Init common controls for the config dialog
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);
}

App::~App()
{
    m_engine.reset();
    m_audio.reset();
    m_renderer.reset();
    m_tray.reset();
    CoUninitialize();
    s_instance = nullptr;
}

// ─── Window ───────────────────────────────────────────────────────────────────
bool App::CreateAppWindow()
{
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = m_hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"SignagePlayerWnd";
    RegisterClassExW(&wc);

    // Get primary monitor resolution
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    m_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP,
        L"SignagePlayerWnd", L"Signage Player",
        WS_POPUP,
        0, 0, sw, sh,
        nullptr, nullptr, m_hInst, nullptr);

    if (!m_hwnd) return false;

    // Hide the cursor in fullscreen mode
    ShowCursor(FALSE);

    // Dark title bar (Windows 11)
    BOOL dark = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
    return true;
}

// ─── Init ─────────────────────────────────────────────────────────────────────
bool App::InitSubsystems()
{
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    UINT w = rc.right, h = rc.bottom;

    // Renderer
    m_renderer = std::make_unique<D3D11Renderer>();
    if (!m_renderer->Init(m_hwnd, w, h)) {
        MessageBoxW(nullptr, L"Failed to initialize Direct3D 11.", L"Error", MB_ICONERROR);
        return false;
    }

    // Audio
    m_audio = std::make_unique<WASAPIRenderer>();
    m_audio->Init(); // non-fatal if audio fails

    // Playlist
    m_playlist = std::make_unique<Playlist>();
    m_playlist->LoadFromFile("playlist.json");

    // Playback engine
    m_engine = std::make_unique<PlaybackEngine>(m_renderer.get(),
                                                 m_audio.get(),
                                                 m_playlist.get());
    if (m_playlist->Size() > 0) m_engine->Start();

    // Tray icon
    m_tray = std::make_unique<TrayIcon>();
    m_tray->Create(m_hwnd, m_hInst, L"Signage Player");
    m_tray->onOpenConfig = [this] { OpenConfig(); };
    m_tray->onExit       = [this] { RequestExit(); };

    return true;
}

// ─── Run ──────────────────────────────────────────────────────────────────────
int App::Run()
{
    if (!CreateAppWindow()) return 1;
    if (!InitSubsystems())  return 1;
    MainLoop();
    return 0;
}

void App::MainLoop()
{
    // Use a high-resolution timer for the render loop.
    // The swap chain waitable object gates the actual frame rate.
    while (m_running) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { m_running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!m_running) break;

        RenderFrame();
    }
}

void App::RenderFrame()
{
    if (!m_engine || !m_renderer) return;
    m_engine->Tick();
    m_renderer->Present(); // blocks on waitable swap chain (vsync)
}

// ─── Config ───────────────────────────────────────────────────────────────────
void App::OpenConfig()
{
    ShowCursor(TRUE);

    ConfigDialog dlg(m_hInst, m_hwnd, m_playlist.get());
    dlg.onPlaylistChanged = [this] {
        m_engine->Stop();
        m_engine = std::make_unique<PlaybackEngine>(m_renderer.get(),
                                                     m_audio.get(),
                                                     m_playlist.get());
        if (m_playlist->Size() > 0) m_engine->Start();
    };
    dlg.Show();

    ShowCursor(FALSE);
}

void App::RequestExit()
{
    m_running = false;
    PostQuitMessage(0);
}

// ─── WndProc ─────────────────────────────────────────────────────────────────
LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    App* app = App::Get();

    switch (msg) {
    case WM_TRAY_MSG:
        if (app && app->m_tray)
            app->m_tray->HandleTrayMessage(lp);
        return 0;

    case WM_SIZE:
        if (app && app->m_renderer)
            app->m_renderer->Resize(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE || wp == VK_F4) {
            if (app) app->OpenConfig();
        }
        return 0;

    case WM_DESTROY:
        if (app) app->RequestExit();
        return 0;

    case WM_ERASEBKGND:
        // Prevent GDI from painting black flashes behind the D3D surface
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
