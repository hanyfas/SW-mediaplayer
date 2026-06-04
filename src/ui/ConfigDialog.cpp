#include "ConfigDialog.h"
#include "playlist/Playlist.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shlwapi.h>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shlwapi.lib")

// Control IDs (inline – no resource file needed)
#define IDC_LIST_ITEMS   1001
#define IDC_BTN_ADD      1002
#define IDC_BTN_REMOVE   1003
#define IDC_BTN_UP       1004
#define IDC_BTN_DOWN     1005
#define IDC_BTN_OK       1006
#define IDC_BTN_CANCEL   1007
#define IDC_SPIN_FADE    1008
#define IDC_EDIT_FADE    1009

static ConfigDialog* g_current = nullptr;

ConfigDialog::ConfigDialog(HINSTANCE hInst, HWND parent, Playlist* playlist)
    : m_hInst(hInst), m_parent(parent), m_playlist(playlist)
{}

bool ConfigDialog::Show()
{
    g_current = this;
    m_changed = false;

    // Build dialog template in memory – no resource file dependency
    struct DlgTemplate {
        DLGTEMPLATE templ;
        WORD menu, windowClass, title;
    } tmpl{};
    tmpl.templ.style      = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER;
    tmpl.templ.cx         = 400;
    tmpl.templ.cy         = 300;
    tmpl.templ.cdit       = 0;

    INT_PTR ret = DialogBoxIndirectW(m_hInst,
                                      reinterpret_cast<LPCDLGTEMPLATEW>(&tmpl),
                                      m_parent, DlgProc);
    g_current = nullptr;
    return ret == IDOK;
}

INT_PTR CALLBACK ConfigDialog::DlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_current) return g_current->HandleMessage(hDlg, msg, wp, lp);
    return FALSE;
}

INT_PTR ConfigDialog::HandleMessage(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowTextW(hDlg, L"Signage Player – Configuration");

        // Create controls programmatically
        RECT rc; GetClientRect(hDlg, &rc);
        int W = rc.right, H = rc.bottom;

        // Listbox
        CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            10, 10, W - 120, H - 50, hDlg, (HMENU)IDC_LIST_ITEMS, m_hInst, nullptr);

        int bx = W - 105, by = 10, bw = 95, bh = 26, gap = 6;
        auto btn = [&](const wchar_t* txt, int id) {
            CreateWindowW(L"BUTTON", txt, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                          bx, by, bw, bh, hDlg, (HMENU)(UINT_PTR)id, m_hInst, nullptr);
            by += bh + gap;
        };
        btn(L"Add Files…",    IDC_BTN_ADD);
        btn(L"Remove",        IDC_BTN_REMOVE);
        btn(L"Move Up",       IDC_BTN_UP);
        btn(L"Move Down",     IDC_BTN_DOWN);

        // OK / Cancel
        CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                      W - 210, H - 35, 90, 26, hDlg, (HMENU)IDC_BTN_OK,   m_hInst, nullptr);
        CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      W - 110, H - 35, 90, 26, hDlg, (HMENU)IDC_BTN_CANCEL, m_hInst, nullptr);

        PopulateList(GetDlgItem(hDlg, IDC_LIST_ITEMS));
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BTN_ADD:
            AddFiles(hDlg);
            PopulateList(GetDlgItem(hDlg, IDC_LIST_ITEMS));
            break;
        case IDC_BTN_REMOVE:
            RemoveSelected(GetDlgItem(hDlg, IDC_LIST_ITEMS));
            break;
        case IDC_BTN_UP:
            MoveSelected(GetDlgItem(hDlg, IDC_LIST_ITEMS), -1);
            break;
        case IDC_BTN_DOWN:
            MoveSelected(GetDlgItem(hDlg, IDC_LIST_ITEMS), 1);
            break;
        case IDC_BTN_OK:
            SaveAndApply(hDlg);
            EndDialog(hDlg, IDOK);
            break;
        case IDC_BTN_CANCEL:
            EndDialog(hDlg, IDCANCEL);
            break;
        }
        return TRUE;
    case WM_CLOSE:
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

void ConfigDialog::PopulateList(HWND hList)
{
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < m_playlist->Size(); ++i) {
        const auto& item = m_playlist->GetItem(i);
        int n = MultiByteToWideChar(CP_UTF8, 0, item.path.c_str(), -1, nullptr, 0);
        std::wstring ws(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, item.path.c_str(), -1, ws.data(), n);
        // Show only filename in the list
        const wchar_t* name = PathFindFileNameW(ws.c_str());
        SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)name);
    }
}

void ConfigDialog::AddFiles(HWND hDlg)
{
    wchar_t buf[32768]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = hDlg;
    ofn.lpstrFilter  = L"Media Files\0*.mp4;*.mov;*.avi;*.mkv;*.jpg;*.jpeg;*.png;*.bmp\0All\0*.*\0";
    ofn.lpstrFile    = buf;
    ofn.nMaxFile     = sizeof(buf) / sizeof(wchar_t);
    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;

    if (!GetOpenFileNameW(&ofn)) return;

    // Parse multi-select result (directory + null-separated filenames)
    wchar_t dir[MAX_PATH]{};
    wcsncpy_s(dir, buf, MAX_PATH);

    wchar_t* p = buf + wcslen(buf) + 1;
    if (*p == L'\0') {
        // Single file selected
        char path[MAX_PATH * 2]{};
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, path, sizeof(path), nullptr, nullptr);
        PlaylistItem item;
        item.path = path;
        m_playlist->AddItem(std::move(item));
    } else {
        while (*p) {
            wchar_t full[MAX_PATH]{};
            PathCombineW(full, dir, p);
            char path[MAX_PATH * 2]{};
            WideCharToMultiByte(CP_UTF8, 0, full, -1, path, sizeof(path), nullptr, nullptr);
            PlaylistItem item;
            item.path = path;
            m_playlist->AddItem(std::move(item));
            p += wcslen(p) + 1;
        }
    }
}

void ConfigDialog::RemoveSelected(HWND hList)
{
    int idx = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
    if (idx == LB_ERR) return;
    m_playlist->RemoveItem(idx);
    PopulateList(hList);
    int newSel = std::min(idx, m_playlist->Size() - 1);
    if (newSel >= 0) SendMessageW(hList, LB_SETCURSEL, newSel, 0);
}

void ConfigDialog::MoveSelected(HWND hList, int delta)
{
    int idx = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
    if (idx == LB_ERR) return;
    int newIdx = idx + delta;
    if (newIdx < 0 || newIdx >= m_playlist->Size()) return;
    m_playlist->MoveItem(idx, newIdx);
    PopulateList(hList);
    SendMessageW(hList, LB_SETCURSEL, newIdx, 0);
}

void ConfigDialog::SaveAndApply(HWND)
{
    m_playlist->SaveToFile(m_playlistPath);
    m_changed = true;
    if (onPlaylistChanged) onPlaylistChanged();
}
