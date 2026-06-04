#pragma once
#include <Windows.h>
#include <string>
#include <functional>

class Playlist;

// Modal configuration dialog – shows the playlist, allows add/remove/reorder.
class ConfigDialog {
public:
    explicit ConfigDialog(HINSTANCE hInst, HWND parent, Playlist* playlist);

    // Returns true if user confirmed changes.
    bool Show();

    std::function<void()> onPlaylistChanged;

private:
    static INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp);
    INT_PTR HandleMessage(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp);

    void PopulateList(HWND hList);
    void AddFiles(HWND hDlg);
    void RemoveSelected(HWND hList);
    void MoveSelected(HWND hList, int delta);
    void SaveAndApply(HWND hDlg);

    HINSTANCE m_hInst{};
    HWND      m_parent{};
    Playlist* m_playlist{};
    bool      m_changed{false};

    std::string m_playlistPath{"playlist.json"};
};
