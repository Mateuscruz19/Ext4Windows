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

    // Load default application icon (placeholder — can be replaced
    // with a custom .ico embedded via resource file later)
    icon_ = LoadIconW(nullptr, IDI_APPLICATION);

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
