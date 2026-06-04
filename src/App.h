#pragma once
#include <Windows.h>
#include <memory>
#include <string>

class D3D11Renderer;
class WASAPIRenderer;
class Playlist;
class PlaybackEngine;
class TrayIcon;
class ConfigDialog;

#define WM_TRAY_MSG (WM_APP + 1)

class App {
public:
    explicit App(HINSTANCE hInst);
    ~App();

    int  Run();
    void RequestExit();

    static App* Get() { return s_instance; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    bool CreateAppWindow();
    bool InitSubsystems();
    void MainLoop();
    void RenderFrame();

    void OpenConfig();

    static App* s_instance;
    HINSTANCE   m_hInst{};
    HWND        m_hwnd{};
    bool        m_running{true};

    std::unique_ptr<D3D11Renderer>  m_renderer;
    std::unique_ptr<WASAPIRenderer> m_audio;
    std::unique_ptr<Playlist>       m_playlist;
    std::unique_ptr<PlaybackEngine> m_engine;
    std::unique_ptr<TrayIcon>       m_tray;

    static constexpr UINT kTimerFPS = 1;
};
