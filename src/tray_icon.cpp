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

#include "tray_icon.hpp"
#include "server.hpp"
#include "debug_log.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <sstream>

// Window class name for our hidden message window
static const wchar_t* TRAY_WND_CLASS = L"Ext4WindowsTrayClass";

// Store pointer to TrayIcon instance for the static WndProc
static TrayIcon* g_tray_instance = nullptr;

TrayIcon::TrayIcon(MountManager& manager)
    : manager_(manager)
{
    g_tray_instance = this;

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TrayIcon::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = TRAY_WND_CLASS;
    RegisterClassExW(&wc);

    // Create a message-only window (invisible, just receives messages)
    hwnd_ = CreateWindowExW(
        0, TRAY_WND_CLASS, L"Ext4Windows Tray",
        0, 0, 0, 0, 0,
        HWND_MESSAGE,   // Message-only window — no visible window
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);

    if (!hwnd_) {
        dbg("TrayIcon: CreateWindowEx failed err=%lu", GetLastError());
        return;
    }

    // Load our custom icon from the embedded resource (ext4windows.rc).
    // IDI_APPICON (1) is defined in the .rc file.
    // LoadIconW with GetModuleHandleW(nullptr) loads from OUR exe,
    // not from the system icons.
    icon_ = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    if (!icon_) {
        // Fallback to generic icon if resource not found (e.g. during dev)
        dbg("TrayIcon: custom icon not found, using default");
        icon_ = LoadIconW(nullptr, IDI_APPLICATION);
    }

    CreateTrayIcon();
    dbg("TrayIcon: created");
}

TrayIcon::~TrayIcon()
{
    RemoveTrayIcon();
    if (hwnd_)
        DestroyWindow(hwnd_);
    UnregisterClassW(TRAY_WND_CLASS, GetModuleHandleW(nullptr));
    g_tray_instance = nullptr;
    dbg("TrayIcon: destroyed");
}

void TrayIcon::CreateTrayIcon()
{
    std::memset(&nid_, 0, sizeof(nid_));
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = WM_TRAYICON;
    nid_.hIcon = icon_;
    wcscpy_s(nid_.szTip, L"Ext4Windows - No active mounts");

    Shell_NotifyIconW(NIM_ADD, &nid_);
}

void TrayIcon::RemoveTrayIcon()
{
    Shell_NotifyIconW(NIM_DELETE, &nid_);
}

void TrayIcon::Update()
{
    size_t count = manager_.ActiveCount();
    if (count == 0)
        wcscpy_s(nid_.szTip, L"Ext4Windows - No active mounts");
    else
        swprintf_s(nid_.szTip, L"Ext4Windows - %zu mount(s) active", count);

    nid_.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void TrayIcon::ShowContextMenu()
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    // Add header (disabled, just for display)
    AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED,
                0, L"Ext4Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // Add active mounts
    std::string status = manager_.Status();
    // Parse status response to get mount lines
    // Format: "OK N mount(s) active\nZ: path (mode)\n..."
    size_t count = manager_.ActiveCount();
    if (count > 0) {
        // Parse each line after the first
        std::istringstream iss(status);
        std::string line;
        std::getline(iss, line); // skip "OK N mount(s) active"

        UINT idx = 0;
        while (std::getline(iss, line)) {
            if (line.empty()) continue;

            // Convert to wide string for the menu
            int wlen = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1,
                                            nullptr, 0);
            std::wstring wline(wlen - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1,
                                 wline.data(), wlen);

            // Create a submenu with "Unmount" option
            HMENU sub = CreatePopupMenu();
            AppendMenuW(sub, MF_STRING, IDM_UNMOUNT_BASE + idx, L"Unmount");
            AppendMenuW(menu, MF_STRING | MF_POPUP,
                        reinterpret_cast<UINT_PTR>(sub), wline.c_str());
            idx++;
        }
    } else {
        AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED,
                    0, L"No active mounts");
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // Auto-start toggle: adds/removes a Windows Registry "Run" key
    // so the server starts automatically on login.
    // MF_CHECKED shows a checkmark next to the item if enabled.
    bool autostart = IsAutoStartEnabled();
    AppendMenuW(menu, MF_STRING | (autostart ? MF_CHECKED : 0),
                IDM_AUTOSTART, L"Start on login");

    AppendMenuW(menu, MF_STRING, IDM_QUIT, L"Quit");

    // Show the menu at the cursor position
    POINT pt;
    GetCursorPos(&pt);

    // Required for the menu to disappear when clicking elsewhere
    SetForegroundWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN,
                   pt.x, pt.y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);

    DestroyMenu(menu);
}

LRESULT CALLBACK TrayIcon::WndProc(HWND hwnd, UINT msg,
                                    WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_TRAYICON && g_tray_instance) {
        // lParam contains the mouse message
        if (LOWORD(lParam) == WM_RBUTTONUP ||
            LOWORD(lParam) == WM_CONTEXTMENU) {
            g_tray_instance->ShowContextMenu();
            return 0;
        }
    }

    if (msg == WM_COMMAND && g_tray_instance) {
        UINT cmd_id = LOWORD(wParam);

        if (cmd_id == IDM_QUIT) {
            g_tray_instance->manager_.Quit();
            return 0;
        }

        if (cmd_id == IDM_AUTOSTART) {
            g_tray_instance->ToggleAutoStart();
            return 0;
        }

        // Unmount commands: IDM_UNMOUNT_BASE + index
        if (cmd_id >= IDM_UNMOUNT_BASE && cmd_id < IDM_UNMOUNT_BASE + 26) {
            UINT idx = cmd_id - IDM_UNMOUNT_BASE;
            // Find the drive letter at this index
            std::string status = g_tray_instance->manager_.Status();
            std::istringstream iss(status);
            std::string line;
            std::getline(iss, line); // skip header

            UINT current = 0;
            while (std::getline(iss, line)) {
                if (line.empty()) continue;
                if (current == idx) {
                    // First char is the drive letter
                    wchar_t drive = static_cast<wchar_t>(line[0]);
                    g_tray_instance->manager_.Unmount(drive);
                    g_tray_instance->Update();
                    break;
                }
                current++;
            }
            return 0;
        }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ── Auto-start on login ─────────────────────────────────
// Uses the Windows Registry "Run" key to start the server
// automatically when the user logs in.
//
// The registry key is:
//   HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run
//
// This is the same place where apps like Steam, Discord, etc.
// register themselves to start on login. Each value in this key
// is a program path that Windows runs automatically.
//
// HKEY_CURRENT_USER means it only applies to THIS user (no admin
// needed). HKEY_LOCAL_MACHINE\...\Run would apply to all users
// but requires admin.
//
// In Python, this would be like:
//   import winreg
//   key = winreg.OpenKey(winreg.HKEY_CURRENT_USER,
//         r"Software\Microsoft\Windows\CurrentVersion\Run",
//         0, winreg.KEY_SET_VALUE)
//   winreg.SetValueEx(key, "Ext4Windows", 0, winreg.REG_SZ, exe_path)
//
// Docs: https://learn.microsoft.com/en-us/windows/win32/api/winreg/
//       nf-winreg-regsetvalueexw
// Docs: https://learn.microsoft.com/en-us/windows/win32/setupapi/
//       run-and-runonce-registry-keys

// Registry path and value name
static const wchar_t* RUN_KEY =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* RUN_VALUE = L"Ext4Windows";

bool TrayIcon::IsAutoStartEnabled()
{
    // Try to read the registry value. If it exists, auto-start is enabled.
    // RegGetValueW is a simpler wrapper around RegQueryValueExW.
    // Docs: https://learn.microsoft.com/en-us/windows/win32/api/winreg/
    //       nf-winreg-reggetvaluew
    DWORD size = 0;
    LONG result = RegGetValueW(
        HKEY_CURRENT_USER, RUN_KEY, RUN_VALUE,
        RRF_RT_REG_SZ,    // Only accept REG_SZ (string) type
        nullptr,           // Don't need the type
        nullptr,           // Don't need the data (just checking existence)
        &size);            // Size of the data

    return result == ERROR_SUCCESS;
}

void TrayIcon::ToggleAutoStart()
{
    if (IsAutoStartEnabled()) {
        // Remove the registry value → disable auto-start
        // RegDeleteKeyValueW deletes a specific value from a key.
        // Docs: https://learn.microsoft.com/en-us/windows/win32/api/winreg/
        //       nf-winreg-regdeletekeyvaluew
        HKEY key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0,
                          KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
            RegDeleteValueW(key, RUN_VALUE);
            RegCloseKey(key);
            dbg("AutoStart: disabled");
        }
    } else {
        // Add the registry value → enable auto-start
        // Get the path to our own exe
        wchar_t exe_path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);

        // The value is: "path\to\ext4windows.exe" --server
        // This tells Windows to run our server on login.
        std::wstring cmd = L"\"";
        cmd += exe_path;
        cmd += L"\" --server";

        HKEY key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0,
                          KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
            // RegSetValueExW sets a registry value.
            // REG_SZ = null-terminated string type.
            // Docs: https://learn.microsoft.com/en-us/windows/win32/api/
            //       winreg/nf-winreg-regsetvalueexw
            RegSetValueExW(key, RUN_VALUE, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(cmd.c_str()),
                static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(key);
            dbg("AutoStart: enabled → %ls", cmd.c_str());
        }
    }
}
