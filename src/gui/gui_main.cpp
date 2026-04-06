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

// ─── What is this file? ─────────────────────────────────────────────
// This is the graphical user interface (GUI) for Ext4Windows, built
// with Microsoft WebView2 — a library that embeds a Chromium browser
// (the same engine as Microsoft Edge) inside a native Windows window.
//
// Think of it like this:
//   - The GUI is a web page (HTML + CSS + JavaScript)
//   - WebView2 is the "frame" that displays the web page
//   - The C++ code here creates the frame and handles messages from JS
//
// The HTML page sends commands like "MOUNT Z RW C:\test.img" to C++.
// C++ forwards those commands to the ext4windows server via named pipe
// (the same protocol used by the CLI), then sends the response back
// to the HTML page.
//
// Docs:
//   WebView2: https://learn.microsoft.com/en-us/microsoft-edge/webview2/
//   WRL:      https://learn.microsoft.com/en-us/cpp/cppcx/wrl/
//   COM:      https://learn.microsoft.com/en-us/windows/win32/com/
// ─────────────────────────────────────────────────────────────────────

#include "gui_main.hpp"
#include "gui_resources.hpp"       // GUI_HTML — the embedded HTML page
#include "../pipe_protocol.hpp"    // PIPE_NAME, pipe_send, pipe_recv
#include "../debug_log.hpp"        // g_debug, dbg()
#include "../partition_scanner.hpp" // scan_ext4_partitions

// Windows headers
#include <windows.h>
#include <shellapi.h>    // ShellExecuteW — open URLs/files
#include <shlobj.h>      // SHGetKnownFolderPath — AppData path
#include <commdlg.h>     // GetOpenFileNameW — file dialog
#include <knownfolders.h>

// WebView2 headers
// wrl.h provides Microsoft::WRL::Callback — a helper to create COM
// callback objects. In Python terms, it's like passing a lambda to
// an async function. COM (Component Object Model) is Windows' way
// of doing object-oriented programming across different languages.
//
// Docs: https://learn.microsoft.com/en-us/cpp/cppcx/wrl/
#include <wrl.h>
#include <WebView2.h>

#include <string>
#include <vector>
#include <functional>
#include <fstream>

// ─── Using declarations ─────────────────────────────────────────────
// "using namespace" imports names so we don't need to type the full
// path every time. In Python, this is like "from module import *".
//
// Microsoft::WRL provides Callback<> for COM event handlers.
using namespace Microsoft::WRL;

// ─── Global state ───────────────────────────────────────────────────
// These are module-level variables (like Python module globals).
// "static" means they're only visible in this file — similar to
// a leading underscore in Python (_variable = private).
//
// Docs: https://en.cppreference.com/w/cpp/language/storage_duration
static HWND g_hwnd = nullptr;                          // Main window handle
static ICoreWebView2Controller* g_controller = nullptr; // WebView controller
static ICoreWebView2* g_webview = nullptr;              // WebView instance

// ─── Forward declarations ───────────────────────────────────────────
// We declare these functions here so we can reference them before
// their full definition appears later in the file. In Python you
// don't need this because Python resolves names at runtime.
//
// Docs: https://en.cppreference.com/w/cpp/language/function
static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static void InitWebView(HWND hwnd);
static void OnWebMessageReceived(const std::wstring& message);
static std::string to_utf8(const std::wstring& wide);
static std::wstring to_wide(const std::string& utf8);

// ─── Config helpers ─────────────────────────────────────────────────
// These read/write the same config.ini as the interactive terminal
// mode, so settings are shared between GUI and TUI.
//
// Path: %APPDATA%\Ext4Windows\config.ini
// Format: key=value, one per line (like Python's configparser)

static std::wstring get_config_path()
{
    // SHGetKnownFolderPath gets special Windows folders.
    // FOLDERID_RoamingAppData = %APPDATA% (e.g. C:\Users\You\AppData\Roaming)
    // In Python: os.environ['APPDATA']
    //
    // Docs: https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/
    //   nf-shlobj_core-shgetknownfolderpath
    wchar_t* appdata = nullptr;
    SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata);
    std::wstring path = std::wstring(appdata) + L"\\Ext4Windows";
    CoTaskMemFree(appdata);

    // Create directory if it doesn't exist
    CreateDirectoryW(path.c_str(), nullptr);
    return path + L"\\config.ini";
}

static std::string read_config_value(const std::string& key)
{
    std::wstring path = get_config_path();
    std::string spath = to_utf8(path);

    std::ifstream f(spath);
    std::string line;
    while (std::getline(f, line)) {
        if (line.substr(0, key.size() + 1) == key + "=") {
            return line.substr(key.size() + 1);
        }
    }
    return "";
}

static void write_config_value(const std::string& key, const std::string& value)
{
    std::wstring path = get_config_path();
    std::string spath = to_utf8(path);

    // Read all lines
    std::vector<std::string> lines;
    bool found = false;
    {
        std::ifstream f(spath);
        std::string line;
        while (std::getline(f, line)) {
            if (line.substr(0, key.size() + 1) == key + "=") {
                lines.push_back(key + "=" + value);
                found = true;
            } else {
                lines.push_back(line);
            }
        }
    }
    if (!found) lines.push_back(key + "=" + value);

    // Write back
    std::ofstream f(spath);
    for (auto& l : lines) f << l << "\n";
}

// ─── Auto-start registry helpers ────────────────────────────────────
// Same registry key as the tray icon uses.
// HKCU\Software\Microsoft\Windows\CurrentVersion\Run
//
// Docs: https://learn.microsoft.com/en-us/windows/win32/api/winreg/

static bool is_autostart_enabled()
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;

    DWORD type = 0, size = 0;
    bool exists = (RegQueryValueExW(key, L"Ext4Windows", nullptr,
                   &type, nullptr, &size) == ERROR_SUCCESS);
    RegCloseKey(key);
    return exists;
}

static void set_autostart(bool enable)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;

    if (enable) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring val = L"\"" + std::wstring(exe) + L"\" --server";
        RegSetValueExW(key, L"Ext4Windows", 0, REG_SZ,
                       (const BYTE*)val.c_str(),
                       (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"Ext4Windows");
    }
    RegCloseKey(key);
}

// ─── Pipe communication ─────────────────────────────────────────────
// Sends a command to the server via the named pipe and returns the
// response. This is the same protocol used by client.cpp.
//
// If the server isn't running, it starts it automatically.

static bool is_server_running()
{
    return WaitNamedPipeW(PIPE_NAME, 100) != 0;
}

static bool start_server()
{
    wchar_t exe_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);

    std::wstring cmd = L"\"";
    cmd += exe_path;
    cmd += L"\" --server";
    if (g_debug) cmd += L" --debug";

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    BOOL ok = CreateProcessW(exe_path, const_cast<LPWSTR>(cmd.c_str()),
                             nullptr, nullptr, FALSE,
                             DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                             nullptr, nullptr, &si, &pi);
    if (!ok) return false;

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static bool ensure_server()
{
    if (is_server_running()) return true;
    if (!start_server()) return false;

    // Wait for server to start (up to 3 seconds)
    for (int i = 0; i < 30; i++) {
        if (WaitNamedPipeW(PIPE_NAME, 100)) return true;
        Sleep(100);
    }
    return false;
}

static std::string send_pipe_command(const std::string& command)
{
    if (!ensure_server()) return "ERROR Server not running";

    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 10; attempt++) {
        pipe = CreateFileW(PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;

        if (GetLastError() == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(PIPE_NAME, 2000);
            continue;
        }
        return "ERROR Could not connect to server";
    }

    if (pipe == INVALID_HANDLE_VALUE)
        return "ERROR Server is busy";

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    if (!pipe_send(pipe, command)) {
        CloseHandle(pipe);
        return "ERROR Failed to send command";
    }

    std::string response = pipe_recv(pipe);
    CloseHandle(pipe);

    return response.empty() ? "ERROR No response" : response;
}

// ─── Wide string ↔ UTF-8 conversion ─────────────────────────────────
// WebView2 uses wide strings (wchar_t / UTF-16), but our pipe protocol
// uses narrow strings (char / UTF-8). These helpers convert between them.
//
// In Python, all strings are Unicode — you don't need this. In C/C++
// on Windows, you constantly convert between "wide" (2 bytes per char)
// and "narrow" (1+ bytes per char, UTF-8).
//
// Docs: https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/
//   nf-stringapiset-widechartomultibyte

static std::string to_utf8(const std::wstring& wide)
{
    if (wide.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(),
                                   nullptr, 0, nullptr, nullptr);
    std::string result(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(),
                         result.data(), len, nullptr, nullptr);
    return result;
}

static std::wstring to_wide(const std::string& utf8)
{
    if (utf8.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(),
                                   nullptr, 0);
    std::wstring result(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(),
                         result.data(), len);
    return result;
}

// ─── Simple JSON helpers ─────────────────────────────────────────────
// We don't want a full JSON library (like nlohmann/json) — that would
// be overkill. These helpers build JSON strings manually.
// They're only used for the WebView2 message channel.

// Escape a string for JSON (handle quotes, backslashes, newlines)
static std::wstring json_escape(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size() + 10);
    for (wchar_t c : s) {
        switch (c) {
            case L'"':  out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n";  break;
            case L'\r': out += L"\\r";  break;
            case L'\t': out += L"\\t";  break;
            default:    out += c;       break;
        }
    }
    return out;
}

// Send a JSON message to the WebView (JavaScript side)
static void post_to_js(const std::wstring& json)
{
    if (g_webview) {
        g_webview->PostWebMessageAsJson(json.c_str());
    }
}

// ─── Get available drive letters ─────────────────────────────────────
static std::vector<char> get_free_drives()
{
    DWORD used = GetLogicalDrives();
    std::vector<char> drives;
    for (char c = 'Z'; c >= 'D'; c--) {
        if (!(used & (1 << (c - 'A'))))
            drives.push_back(c);
    }
    return drives;
}

// ─── Handle messages from JavaScript ─────────────────────────────────
// When the HTML page calls window.chrome.webview.postMessage({...}),
// this function receives the message. It parses the JSON (simple
// substring matching — no full parser needed) and dispatches the
// appropriate action.
//
// This is like a Flask route handler:
//   @app.route('/api/command', methods=['POST'])
//   def handle_command():
//       data = request.json
//       ...

// Extract a JSON string value by key (simple parser — no library needed)
static std::wstring json_get_string(const std::wstring& json, const std::wstring& key)
{
    std::wstring needle = L"\"" + key + L"\"";
    size_t pos = json.find(needle);
    if (pos == std::wstring::npos) return L"";

    // Find the colon after the key
    pos = json.find(L':', pos + needle.size());
    if (pos == std::wstring::npos) return L"";

    // Find the opening quote of the value
    pos = json.find(L'"', pos + 1);
    if (pos == std::wstring::npos) return L"";
    pos++; // skip the quote

    // Find the closing quote (handling escaped quotes)
    std::wstring value;
    while (pos < json.size() && json[pos] != L'"') {
        if (json[pos] == L'\\' && pos + 1 < json.size()) {
            pos++;
            switch (json[pos]) {
                case L'n': value += L'\n'; break;
                case L't': value += L'\t'; break;
                default:   value += json[pos]; break;
            }
        } else {
            value += json[pos];
        }
        pos++;
    }
    return value;
}

// Extract a JSON boolean value by key
static bool json_get_bool(const std::wstring& json, const std::wstring& key)
{
    std::wstring needle = L"\"" + key + L"\"";
    size_t pos = json.find(needle);
    if (pos == std::wstring::npos) return false;
    pos = json.find(L':', pos);
    if (pos == std::wstring::npos) return false;
    return json.find(L"true", pos) < json.find(L"}", pos);
}

static void OnWebMessageReceived(const std::wstring& message)
{
    std::wstring cmd = json_get_string(message, L"command");
    std::wstring id = json_get_string(message, L"id");

    dbg("GUI message: cmd=%ls", cmd.c_str());

    // ── Pipe commands (MOUNT, UNMOUNT, STATUS, etc.) ──
    // These are forwarded to the server via the named pipe.
    if (cmd.substr(0, 5) == L"MOUNT" ||
        cmd.substr(0, 7) == L"UNMOUNT" ||
        cmd == L"STATUS" ||
        cmd == L"QUIT") {

        std::string response = send_pipe_command(to_utf8(cmd));
        std::wstring wresp = to_wide(response);

        // Send response back to JS with the request ID
        std::wstring json = L"{\"id\":\"" + json_escape(id) +
                            L"\",\"response\":\"" + json_escape(wresp) + L"\"}";
        post_to_js(json);
        return;
    }

    // ── Browse for file ──
    if (cmd == L"GUI_BROWSE") {
        // GetOpenFileNameW shows the standard Windows "Open File" dialog.
        // In Python: tkinter.filedialog.askopenfilename()
        //
        // Docs: https://learn.microsoft.com/en-us/windows/win32/api/commdlg/
        //   nf-commdlg-getopenfilenamew
        wchar_t file[MAX_PATH] = {};
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = g_hwnd;
        ofn.lpstrFilter = L"Disk Images (*.img;*.vhd;*.raw)\0*.img;*.vhd;*.raw\0"
                          L"All Files (*.*)\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

        if (GetOpenFileNameW(&ofn)) {
            std::wstring json = L"{\"type\":\"file_selected\",\"path\":\"" +
                                json_escape(file) + L"\"}";
            post_to_js(json);
        }
        return;
    }

    // ── Scan for partitions ──
    if (cmd == L"GUI_SCAN") {
        // Scanning requires admin. We use the same --scan-save
        // mechanism as the interactive mode: launch an elevated
        // subprocess that writes results to a temp file.

        // Check if we're already elevated
        BOOL elevated = FALSE;
        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            TOKEN_ELEVATION elev = {};
            DWORD size = sizeof(elev);
            if (GetTokenInformation(token, TokenElevation, &elev,
                                     sizeof(elev), &size))
                elevated = elev.TokenIsElevated;
            CloseHandle(token);
        }

        // Create temp file for results
        wchar_t temp_dir[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp_dir);
        std::wstring temp_file = std::wstring(temp_dir) + L"ext4w_scan.tmp";

        // Get our exe path
        wchar_t exe_path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);

        if (!elevated) {
            // Launch elevated subprocess with --scan-save
            std::wstring args = L"--scan-save \"" + temp_file + L"\"";

            SHELLEXECUTEINFOW sei = {};
            sei.cbSize = sizeof(sei);
            sei.fMask = SEE_MASK_NOCLOSEPROCESS;
            sei.lpVerb = L"runas";
            sei.lpFile = exe_path;
            sei.lpParameters = args.c_str();
            sei.nShow = SW_HIDE;

            if (!ShellExecuteExW(&sei)) {
                post_to_js(L"{\"type\":\"scan_results\",\"partitions\":[]}");
                return;
            }

            WaitForSingleObject(sei.hProcess, 30000);
            CloseHandle(sei.hProcess);
        } else {
            // Already elevated — scan directly
            auto partitions = scan_ext4_partitions();
            // Write to temp file in the same format as --scan-save
            std::ofstream f(to_utf8(temp_file));
            for (auto& p : partitions) {
                f << to_utf8(p.device_path)
                  << "|" << to_utf8(p.display_name) << "\n";
            }
            f.close();
        }

        // Read results from temp file
        std::wstring json = L"{\"type\":\"scan_results\",\"partitions\":[";
        {
            std::string tpath = to_utf8(temp_file);
            std::ifstream f(tpath);
            std::string line;
            bool first = true;
            while (std::getline(f, line)) {
                if (line.empty()) continue;

                // Parse "device_path|display_name" format
                size_t sep = line.find('|');
                if (sep == std::string::npos) continue;

                std::string path = line.substr(0, sep);
                std::string name = line.substr(sep + 1);

                if (!first) json += L",";
                first = false;

                json += L"{\"path\":\"" + json_escape(to_wide(path)) +
                        L"\",\"name\":\"" + json_escape(to_wide(name)) + L"\"}";
            }
            f.close();
            DeleteFileW(temp_file.c_str());
        }
        json += L"]}";
        post_to_js(json);
        return;
    }

    // ── Refresh drive letters ──
    if (cmd == L"GUI_REFRESH_DRIVES") {
        auto drives = get_free_drives();
        std::wstring json = L"{\"type\":\"init\",\"drives\":[";
        for (size_t i = 0; i < drives.size(); i++) {
            if (i > 0) json += L",";
            json += L"\"";
            json += (wchar_t)(drives[i]);
            json += L"\"";
        }
        json += L"]}";
        post_to_js(json);
        return;
    }

    // ── Settings: auto-start ──
    if (cmd == L"GUI_SET_AUTOSTART") {
        bool on = json_get_bool(message, L"value");
        set_autostart(on);
        return;
    }

    // ── Settings: default mode ──
    if (cmd == L"GUI_SET_MODE") {
        std::wstring mode = json_get_string(message, L"value");
        write_config_value("default_mode", mode == L"RW" ? "1" : "0");
        return;
    }

    // ── Settings: debug ──
    if (cmd == L"GUI_SET_DEBUG") {
        bool on = json_get_bool(message, L"value");
        write_config_value("debug", on ? "1" : "0");
        return;
    }

    // ── Settings: language ──
    // JS sends: { command: "GUI_SET_LANGUAGE", value: 3 }
    // The value is a bare number (not a string), so we find
    // "value": and parse the digits after the colon.
    if (cmd == L"GUI_SET_LANGUAGE") {
        size_t vpos = message.find(L"\"value\"");
        if (vpos != std::wstring::npos) {
            vpos = message.find(L':', vpos);
            if (vpos != std::wstring::npos) {
                // Skip whitespace after colon
                vpos++;
                while (vpos < message.size() && message[vpos] == L' ') vpos++;
                int v = 0;
                if (vpos < message.size() && message[vpos] >= L'0' && message[vpos] <= L'9')
                    v = message[vpos] - L'0';
                if (v >= 0 && v <= 7)
                    write_config_value("language", std::to_string(v));
            }
        }
        return;
    }

    // ── View logs ──
    if (cmd == L"GUI_VIEW_LOGS") {
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        std::wstring log_path = std::wstring(temp) + L"ext4windows_server.log";
        ShellExecuteW(nullptr, L"open", log_path.c_str(),
                       nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }

    // ── Open drive in Explorer ──
    if (cmd == L"GUI_OPEN_DRIVE") {
        std::wstring drive = json_get_string(message, L"drive");
        if (!drive.empty()) {
            std::wstring path = drive + L":\\";
            ShellExecuteW(nullptr, L"open", path.c_str(),
                           nullptr, nullptr, SW_SHOWNORMAL);
        }
        return;
    }

    // ── Open URL ──
    if (cmd == L"GUI_OPEN_URL") {
        std::wstring url = json_get_string(message, L"url");
        ShellExecuteW(nullptr, L"open", url.c_str(),
                       nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
}

// ─── Initialize WebView2 ────────────────────────────────────────────
// This is where the WebView2 browser engine is created. It's async
// because initializing Chromium takes a moment. The pattern is:
//
//   1. CreateCoreWebView2Environment() → callback with environment
//   2. env->CreateCoreWebView2Controller() → callback with controller
//   3. controller->get_CoreWebView2() → the actual webview
//   4. webview->NavigateToString() → load our HTML
//
// Each step is a callback (like a JavaScript Promise chain):
//   createEnvironment()
//     .then(env => env.createController(window))
//     .then(controller => controller.getWebView())
//     .then(webview => webview.navigate(html))
//
// Docs: https://learn.microsoft.com/en-us/microsoft-edge/webview2/
//   get-started/win32

static void InitWebView(HWND hwnd)
{
    // Initialize COM (Component Object Model) — Windows' inter-process
    // communication system. WebView2 uses COM internally.
    // In Python, you'd never deal with this — it's a Windows-only thing.
    //
    // COINIT_APARTMENTTHREADED means "this thread owns its COM objects"
    // (as opposed to COINIT_MULTITHREADED where any thread can call them).
    //
    // Docs: https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/
    //   nf-combaseapi-coinitializeex
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Step 1: Create the WebView2 environment.
    // The environment is like a browser profile — it stores cookies,
    // cache, etc. We pass nullptr for everything to use defaults.
    //
    // Callback<...> creates a COM callback object. It's like passing
    // a lambda to an async function in Python:
    //   asyncio.ensure_future(create_env()).add_done_callback(lambda result: ...)
    //
    // The HRESULT type is Windows' way of reporting errors. S_OK = success.
    // Docs: https://learn.microsoft.com/en-us/windows/win32/com/error-handling-in-com

    // Use a temp folder for WebView2 user data to avoid cluttering AppData
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    std::wstring user_data = std::wstring(temp) + L"Ext4Windows_WebView2";

    CreateCoreWebView2EnvironmentWithOptions(
        nullptr,                   // browserExecutableFolder (nullptr = auto-detect Edge)
        user_data.c_str(),         // userDataFolder for WebView2 profile
        nullptr,                   // environmentOptions (nullptr = defaults)
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd](HRESULT result, ICoreWebView2Environment* env) -> HRESULT
            {
                if (FAILED(result) || !env) {
                    MessageBoxW(hwnd,
                        L"Failed to initialize WebView2.\n\n"
                        L"Make sure Microsoft Edge (or WebView2 Runtime) is installed.\n"
                        L"Download: https://developer.microsoft.com/en-us/microsoft-edge/webview2/",
                        L"Ext4Windows", MB_ICONERROR | MB_OK);
                    PostQuitMessage(1);
                    return result;
                }

                // Step 2: Create the controller (the browser widget inside our window).
                env->CreateCoreWebView2Controller(
                    hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hwnd](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT
                        {
                            if (FAILED(result) || !controller) {
                                PostQuitMessage(1);
                                return result;
                            }

                            g_controller = controller;
                            controller->AddRef();  // prevent premature destruction

                            // Step 3: Get the actual WebView2 instance
                            controller->get_CoreWebView2(&g_webview);
                            g_webview->AddRef();

                            // Resize the browser to fill the window
                            RECT bounds;
                            GetClientRect(hwnd, &bounds);
                            controller->put_Bounds(bounds);

                            // ── Configure the WebView ───────────────
                            // Disable the default context menu and DevTools
                            // (we don't want users right-clicking to "Inspect")
                            ICoreWebView2Settings* settings = nullptr;
                            g_webview->get_Settings(&settings);
                            if (settings) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(g_debug ? TRUE : FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_IsZoomControlEnabled(FALSE);
                                settings->Release();
                            }

                            // ── Listen for messages from JavaScript ──
                            // When JS calls window.chrome.webview.postMessage(),
                            // this callback fires. It's our "API endpoint".
                            g_webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2* sender,
                                       ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT
                                    {
                                        LPWSTR raw = nullptr;
                                        args->get_WebMessageAsJson(&raw);
                                        if (raw) {
                                            OnWebMessageReceived(raw);
                                            CoTaskMemFree(raw);
                                        }
                                        return S_OK;
                                    }).Get(),
                                nullptr);

                            // ── Load the HTML page ──
                            // NavigateToString() loads HTML directly from a
                            // string — no file or URL needed. The entire GUI
                            // is embedded in gui_resources.hpp.
                            //
                            // Docs: https://learn.microsoft.com/en-us/microsoft-edge/
                            //   webview2/reference/win32/icorewebview2#navigatetostring
                            std::wstring html = load_gui_html();
                            g_webview->NavigateToString(html.c_str());

                            // ── Send initial data to JS after navigation ──
                            // We listen for the NavigationCompleted event
                            // to know when the page has loaded.
                            g_webview->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [](ICoreWebView2* sender,
                                       ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT
                                    {
                                        // Send version, drive letters, and config
                                        auto drives = get_free_drives();
                                        std::wstring json = L"{\"type\":\"init\"";

                                        // Version
                                        #ifdef EXT4W_VERSION
                                        json += L",\"version\":\"";
                                        json += to_wide(EXT4W_VERSION);
                                        json += L"\"";
                                        #endif

                                        // Drive letters
                                        json += L",\"drives\":[";
                                        for (size_t i = 0; i < drives.size(); i++) {
                                            if (i > 0) json += L",";
                                            json += L"\"";
                                            json += (wchar_t)(drives[i]);
                                            json += L"\"";
                                        }
                                        json += L"]";

                                        // Config
                                        json += L",\"config\":{";
                                        json += L"\"autostart\":";
                                        json += is_autostart_enabled() ? L"true" : L"false";

                                        std::string mode_val = read_config_value("default_mode");
                                        json += L",\"default_mode\":\"";
                                        json += (mode_val == "1") ? L"RW" : L"RO";
                                        json += L"\"";

                                        std::string debug_val = read_config_value("debug");
                                        json += L",\"debug\":";
                                        json += (debug_val == "1") ? L"true" : L"false";

                                        json += L"}";

                                        // Language setting
                                        std::string lang_val = read_config_value("language");
                                        int lang = 0;
                                        if (!lang_val.empty()) {
                                            lang = std::stoi(lang_val);
                                            if (lang < 0 || lang > 7) lang = 0;
                                        }
                                        json += L",\"language\":";
                                        json += std::to_wstring(lang);

                                        // Show language picker on first launch
                                        // (no config file exists yet)
                                        std::string cfg_path_check = read_config_value("language");
                                        if (cfg_path_check.empty()) {
                                            json += L",\"first_launch\":true";
                                        }

                                        json += L"}";
                                        post_to_js(json);

                                        return S_OK;
                                    }).Get(),
                                nullptr);

                            return S_OK;
                        }).Get()
                );

                return S_OK;
            }).Get()
    );
}

// ─── Window procedure ────────────────────────────────────────────────
// The WndProc handles Windows messages for our main window.
// Every window in Windows has a "window procedure" that receives
// messages (events) like WM_SIZE (window resized), WM_CLOSE
// (user clicked X), etc.
//
// In Python/tkinter terms:
//   root.bind("<Configure>", on_resize)
//   root.protocol("WM_DELETE_WINDOW", on_close)
//
// Docs: https://learn.microsoft.com/en-us/windows/win32/winmsg/
//   window-procedures

// Timer ID for server health check
static const UINT_PTR TIMER_SERVER_CHECK = 1;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_SIZE:
        // When the window is resized, resize the WebView to match.
        if (g_controller) {
            RECT bounds;
            GetClientRect(hwnd, &bounds);
            g_controller->put_Bounds(bounds);
        }
        return 0;

    case WM_CREATE:
        // Start a timer that fires every 3 seconds to check if the
        // server is still running. If it dies (user quit from tray),
        // we update the status bar in the GUI.
        //
        // SetTimer: https://learn.microsoft.com/en-us/windows/win32/api/
        //   winuser/nf-winuser-settimer
        SetTimer(hwnd, TIMER_SERVER_CHECK, 3000, nullptr);
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_SERVER_CHECK && g_webview) {
            bool running = is_server_running();
            // Tell JS to update the server status indicator
            std::wstring json = L"{\"type\":\"server_status\",\"running\":";
            json += running ? L"true" : L"false";
            json += L"}";
            post_to_js(json);

            // If server died, try to restart it
            if (!running) {
                ensure_server();
            }
        }
        return 0;

    case WM_CLOSE:
        KillTimer(hwnd, TIMER_SERVER_CHECK);
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        // Clean up WebView2 resources
        if (g_controller) {
            g_controller->Close();
            g_controller->Release();
            g_controller = nullptr;
        }
        if (g_webview) {
            g_webview->Release();
            g_webview = nullptr;
        }
        PostQuitMessage(0);
        return 0;

    case WM_GETMINMAXINFO: {
        // Set minimum window size (600x400)
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = 700;
        mmi->ptMinTrackSize.y = 500;
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ─── GUI entry point ─────────────────────────────────────────────────
// Called from main.cpp when the user launches ext4windows without
// arguments (and Qt is not being used). Creates the main window,
// initializes WebView2, and runs the message loop.
//
// This replaces interactive_main() as the default mode.

int gui_main()
{
    // ── Hide the console window ──
    // The exe is built as a console application (so CLI commands work).
    // When launching the GUI, we don't want the black terminal window
    // sitting behind it. FreeConsole() detaches the console.
    //
    // In Python: there's no equivalent — Python scripts don't have
    // a console by default unless you run them from cmd.
    //
    // Docs: https://learn.microsoft.com/en-us/windows/console/freeconsole
    FreeConsole();

    // ── Register window class ──
    // Every window in Windows needs a "class" that defines its
    // appearance and behavior. This is NOT a C++ class — it's a
    // Win32 concept. Think of it as a template for windows.
    //
    // Docs: https://learn.microsoft.com/en-us/windows/win32/api/winuser/
    //   ns-winuser-wndclassexw

    HINSTANCE hInst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(27, 27, 27));  // Match --bg (#1b1b1b)
    wc.lpszClassName = L"Ext4WindowsGUI";

    // Load the app icon from resources (same icon as the tray)
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));  // IDI_APPICON = 1
    wc.hIconSm = wc.hIcon;

    RegisterClassExW(&wc);

    // ── Calculate centered window position ──
    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    int win_w = 900;
    int win_h = 620;
    int x = (screen_w - win_w) / 2;
    int y = (screen_h - win_h) / 2;

    // ── Create the window ──
    g_hwnd = CreateWindowExW(
        0,                          // No extended styles
        L"Ext4WindowsGUI",         // Window class name
        L"Ext4Windows",            // Window title
        WS_OVERLAPPEDWINDOW,       // Standard window with title bar, resize, etc.
        x, y, win_w, win_h,       // Position and size
        nullptr,                    // No parent window
        nullptr,                    // No menu
        hInst,
        nullptr);

    if (!g_hwnd) {
        MessageBoxW(nullptr, L"Failed to create window", L"Error", MB_ICONERROR);
        return 1;
    }

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    // ── Initialize WebView2 ──
    // This is async — the actual browser will appear after a moment.
    InitWebView(g_hwnd);

    // ── Ensure server is running ──
    // Start the background server if it's not already running,
    // so the GUI can send commands immediately.
    ensure_server();

    // ── Message loop ──
    // This is the heart of every Windows application. GetMessage()
    // waits for the next event (mouse click, key press, timer, etc.),
    // TranslateMessage() converts key codes to characters, and
    // DispatchMessage() calls the appropriate WndProc.
    //
    // In Python/tkinter: root.mainloop()
    //
    // Docs: https://learn.microsoft.com/en-us/windows/win32/winmsg/
    //   using-messages-and-message-queues
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CoUninitialize();
    return (int)msg.wParam;
}
