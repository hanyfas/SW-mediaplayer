#pragma once
#include <Windows.h>
#include <shellapi.h>
#include <functional>
#include <string>

class TrayIcon {
public:
    TrayIcon() = default;
    ~TrayIcon();

    bool Create(HWND hwnd, HINSTANCE hInst, const std::wstring& tooltip);
    void Destroy();

    // Callbacks
    std::function<void()> onOpenConfig;
    std::function<void()> onExit;

    // Call from WndProc when msg == WM_APP+1
    void HandleTrayMessage(LPARAM lp);

private:
    void ShowContextMenu();

    HWND           m_hwnd{};
    NOTIFYICONDATAW m_nid{};
    bool           m_created{false};
};
