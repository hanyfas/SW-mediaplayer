#include "TrayIcon.h"
#include "resource.h"

#define WM_TRAY (WM_APP + 1)
#define ID_TRAY_OPEN_CONFIG  4001
#define ID_TRAY_EXIT         4002

bool TrayIcon::Create(HWND hwnd, HINSTANCE hInst, const std::wstring& tooltip)
{
    m_hwnd = hwnd;
    ZeroMemory(&m_nid, sizeof(m_nid));
    m_nid.cbSize           = sizeof(m_nid);
    m_nid.hWnd             = hwnd;
    m_nid.uID              = 1;
    m_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    m_nid.uCallbackMessage = WM_TRAY;
    m_nid.hIcon            = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPLICATION));
    wcsncpy_s(m_nid.szTip, tooltip.c_str(), 127);

    m_created = Shell_NotifyIconW(NIM_ADD, &m_nid) != FALSE;

    // Use Version 4 for balloon tips and modern behavior
    m_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &m_nid);
    return m_created;
}

void TrayIcon::HandleTrayMessage(LPARAM lp)
{
    UINT msg = LOWORD(lp);
    if (msg == WM_RBUTTONUP || msg == WM_CONTEXTMENU) {
        ShowContextMenu();
    } else if (msg == WM_LBUTTONDBLCLK) {
        if (onOpenConfig) onOpenConfig();
    }
}

void TrayIcon::ShowContextMenu()
{
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN_CONFIG, L"Open Configuration");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(m_hwnd);

    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                              pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);

    if (cmd == ID_TRAY_OPEN_CONFIG && onOpenConfig) onOpenConfig();
    else if (cmd == ID_TRAY_EXIT   && onExit)       onExit();
}

void TrayIcon::Destroy()
{
    if (m_created) {
        Shell_NotifyIconW(NIM_DELETE, &m_nid);
        m_created = false;
    }
}

TrayIcon::~TrayIcon()
{
    Destroy();
}
