// Ext4Windows — Mount ext4 partitions as Windows drive letters
// Copyright (C) 2026 Mateus Cruz
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, see <https://www.gnu.org/licenses/>.

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

    // Show a balloon (toast) notification above the tray icon.
    // Public so the update checker thread can call it.
    void ShowBalloon(const wchar_t* title, const wchar_t* message);

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
    size_t prev_mount_count_ = 0;  // Track mount count for notifications

    // Check if auto-start is currently enabled (registry Run key)
    bool IsAutoStartEnabled();
    // Toggle auto-start on/off
    void ToggleAutoStart();

    // Launch the interactive terminal (ext4windows.exe with no args)
    void LaunchInteractive();

    // Open the server log file in the default text editor
    void ViewLogs();

    static const UINT WM_TRAYICON = WM_APP + 1;
    static const UINT IDM_OPEN = 39999;
    static const UINT IDM_QUIT = 40000;
    static const UINT IDM_AUTOSTART = 40001;
    static const UINT IDM_VIEWLOGS = 40002;
    static const UINT IDM_UNMOUNT_BASE = 41000;  // 41000 + index
};
