#pragma once

#include <windows.h>
#include <shellapi.h>

// Forward declaration
class MountManager;

// System tray icon using pure Win32 API (no Qt).
// Shows an icon in the notification area with a right-click context menu
// listing active mounts, unmount options, and quit.
class TrayIcon {
public:
    TrayIcon(MountManager& manager);
    ~TrayIcon();

    // Update the tray icon tooltip (e.g. after mount/unmount).
    void Update();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg,
                                     WPARAM wParam, LPARAM lParam);
    void ShowContextMenu();
    void CreateTrayIcon();
    void RemoveTrayIcon();

    HWND hwnd_ = nullptr;
    NOTIFYICONDATAW nid_ = {};
    MountManager& manager_;
    HICON icon_ = nullptr;

    // Check if auto-start is currently enabled (registry Run key)
    bool IsAutoStartEnabled();
    // Toggle auto-start on/off
    void ToggleAutoStart();

    static const UINT WM_TRAYICON = WM_APP + 1;
    static const UINT IDM_QUIT = 40000;
    static const UINT IDM_AUTOSTART = 40001;
    static const UINT IDM_UNMOUNT_BASE = 41000;  // 41000 + index
};
