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

#include "interactive.hpp"
#include "pipe_protocol.hpp"
#include "partition_scanner.hpp"
#include "blockdev_partition.hpp"
#include "ext4_filesystem.hpp"
#include "debug_log.hpp"

#include <windows.h>
#include <shellapi.h>
#include <winfsp/winfsp.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>

// ═══════════════════════════════════════════════════════════
// Colors for the Windows console (SetConsoleTextAttribute).
//
// The Windows console uses a bitmask for colors:
//   Bits 0-3 = foreground, Bits 4-7 = background.
//
// We define our brand colors:
//   "Orange" = bright yellow (red + green + intensity)
//   "Blue"   = bright blue (blue + intensity)
//   "White"  = all foreground bits + intensity
//   "Gray"   = just intensity (dark white)
//
// Docs: https://learn.microsoft.com/en-us/windows/console/
//       console-screen-buffers#character-attributes
// ═══════════════════════════════════════════════════════════

static HANDLE g_console = INVALID_HANDLE_VALUE;
static WORD   g_default_attr = 0x07;

static const WORD CLR_ORANGE = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
static const WORD CLR_BLUE   = FOREGROUND_BLUE | FOREGROUND_INTENSITY;
static const WORD CLR_WHITE  = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
static const WORD CLR_GRAY   = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
static const WORD CLR_GREEN  = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
static const WORD CLR_RED    = FOREGROUND_RED | FOREGROUND_INTENSITY;

static void set_color(WORD color)
{
    SetConsoleTextAttribute(g_console, color);
}

static void reset_color()
{
    SetConsoleTextAttribute(g_console, g_default_attr);
}

static void init_console()
{
    g_console = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi = {};
    if (GetConsoleScreenBufferInfo(g_console, &csbi))
        g_default_attr = csbi.wAttributes;
    SetConsoleTitleW(L"Ext4Windows");
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);

    // Set the console window icon to our app icon (embedded in the .exe).
    // GetConsoleWindow() returns the HWND of this console window.
    // LoadIconW with MAKEINTRESOURCEW(1) loads IDI_APPICON (ID=1) from ext4windows.rc.
    // WM_SETICON tells the window to use our icon instead of the default terminal icon.
    // ICON_BIG = taskbar/alt-tab icon, ICON_SMALL = title bar icon.
    // Docs: https://learn.microsoft.com/en-us/windows/console/getconsolewindow
    //       https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-seticon
    HWND hwndConsole = GetConsoleWindow();
    if (hwndConsole) {
        HICON hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
        if (hIcon) {
            SendMessageW(hwndConsole, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
            SendMessageW(hwndConsole, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
        }
    }
}

// ═══════════════════════════════════════════════════════════
// Internationalization (i18n)
//
// Supports 8 languages:
//   0 = English      1 = Portugues    2 = Espanol
//   3 = Deutsch      4 = Francais     5 = Chinese
//   6 = Japanese     7 = Russian
//
// tr() takes one string per language and returns the active one.
// ═══════════════════════════════════════════════════════════

static int g_lang = 0;
static int g_default_mode = 0;  // 0 = read-only, 1 = read-write

// ── Config file (%APPDATA%\Ext4Windows\config.ini) ──────
// Simple key=value file for persisting user preferences.

static std::string get_config_path()
{
    char* appdata = nullptr;
    size_t len = 0;
    if (_dupenv_s(&appdata, &len, "APPDATA") != 0 || !appdata)
        return "";
    std::string path(appdata);
    free(appdata);
    path += "\\Ext4Windows";
    CreateDirectoryA(path.c_str(), nullptr);
    path += "\\config.ini";
    return path;
}

static void load_config()
{
    std::string path = get_config_path();
    if (path.empty()) return;

    FILE* f = fopen(path.c_str(), "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64] = {}, val[64] = {};
        if (sscanf(line, "%63[^=]=%63s", key, val) == 2) {
            if (strcmp(key, "language") == 0)
                g_lang = atoi(val);
            else if (strcmp(key, "default_mode") == 0)
                g_default_mode = atoi(val);
            else if (strcmp(key, "debug") == 0)
                g_debug = atoi(val);
        }
    }
    fclose(f);

    // Validate ranges
    if (g_lang < 0 || g_lang > 7) g_lang = 0;
    if (g_default_mode < 0 || g_default_mode > 1) g_default_mode = 0;
}

static void save_config()
{
    std::string path = get_config_path();
    if (path.empty()) return;

    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;

    fprintf(f, "language=%d\n", g_lang);
    fprintf(f, "default_mode=%d\n", g_default_mode);
    fprintf(f, "debug=%d\n", g_debug ? 1 : 0);
    fclose(f);
}

// ── Auto-start registry helpers (same key as tray_icon.cpp) ──

static const wchar_t* AUTOSTART_RUN_KEY =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* AUTOSTART_RUN_VALUE = L"Ext4Windows";

static bool is_autostart_enabled()
{
    DWORD size = 0;
    LONG result = RegGetValueW(
        HKEY_CURRENT_USER, AUTOSTART_RUN_KEY, AUTOSTART_RUN_VALUE,
        RRF_RT_REG_SZ, nullptr, nullptr, &size);
    return result == ERROR_SUCCESS;
}

static void toggle_autostart()
{
    if (is_autostart_enabled()) {
        HKEY key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, AUTOSTART_RUN_KEY, 0,
                          KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
            RegDeleteValueW(key, AUTOSTART_RUN_VALUE);
            RegCloseKey(key);
        }
    } else {
        wchar_t exe_path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        std::wstring cmd = L"\"";
        cmd += exe_path;
        cmd += L"\" --server";
        HKEY key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, AUTOSTART_RUN_KEY, 0,
                          KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
            RegSetValueExW(key, AUTOSTART_RUN_VALUE, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(cmd.c_str()),
                static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(key);
        }
    }
}

static const char* tr(const char* en, const char* pt,
                      const char* es, const char* de,
                      const char* fr, const char* zh,
                      const char* ja, const char* ru = nullptr)
{
    const char* t[] = {en, pt, es, de, fr, zh, ja, ru};
    // Fallback to English if translation is missing (nullptr)
    return t[g_lang] ? t[g_lang] : en;
}

static bool read_line(char* buf, int max_chars)
{
    if (!fgets(buf, max_chars, stdin))
        return false;
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n')
        buf[len - 1] = '\0';
    return true;
}

static void strip_quotes(char* s)
{
    size_t len = strlen(s);
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') {
        memmove(s, s + 1, len - 2);
        s[len - 2] = '\0';
    }
}

// ═══════════════════════════════════════════════════════════
// Server communication
// ═══════════════════════════════════════════════════════════

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

    BOOL ok = CreateProcessW(nullptr, const_cast<LPWSTR>(cmd.c_str()),
        nullptr, nullptr, FALSE,
        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
        nullptr, nullptr, &si, &pi);

    if (!ok) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static bool wait_for_server()
{
    for (int i = 0; i < 30; i++) {
        if (WaitNamedPipeW(PIPE_NAME, 100)) return true;
        Sleep(100);
    }
    return false;
}

static bool ensure_server()
{
    if (is_server_running()) return true;

    set_color(CLR_GRAY);
    printf("  %s", tr(
        "Starting server...",
        "Iniciando servidor...",
        "Iniciando servidor...",
        "Server wird gestartet...",
        "Demarrage du serveur...",
        "\xe6\xad\xa3\xe5\x9c\xa8\xe5\x90\xaf\xe5\x8a\xa8\xe6\x9c\x8d\xe5\x8a\xa1\xe5\x99\xa8...",
        "\xe3\x82\xb5\xe3\x83\xbc\xe3\x83\x90\xe3\x83\xbc\xe3\x82\x92\xe8\xb5\xb7\xe5\x8b\x95\xe4\xb8\xad...",
        "\xd0\x97\xd0\xb0\xd0\xbf\xd1\x83\xd1\x81\xd0\xba \xd1\x81\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80\xd0\xb0..."));
    reset_color();

    if (!start_server()) {
        set_color(CLR_RED);
        printf(" %s\n", tr("failed!", "falhou!", "fallo!",
            "fehlgeschlagen!", "echoue!", "\xe5\xa4\xb1\xe8\xb4\xa5!", "\xe5\xa4\xb1\xe6\x95\x97!",
        "\xd0\xbe\xd1\x88\xd0\xb8\xd0\xb1\xd0\xba\xd0\xb0!"));
        reset_color();
        return false;
    }

    if (!wait_for_server()) {
        set_color(CLR_RED);
        printf(" timeout!\n");
        reset_color();
        return false;
    }

    set_color(CLR_GREEN);
    printf(" OK\n");
    reset_color();
    return true;
}

static std::string send_command(const std::string& command)
{
    if (!ensure_server()) return "";

    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 10; attempt++) {
        pipe = CreateFileW(PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(PIPE_NAME, 2000);
            continue;
        }
        return "";
    }
    if (pipe == INVALID_HANDLE_VALUE) return "";

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    if (!pipe_send(pipe, command)) {
        CloseHandle(pipe);
        return "";
    }

    std::string response = pipe_recv(pipe);
    CloseHandle(pipe);
    return response;
}

// ═══════════════════════════════════════════════════════════
// UI Drawing
// ═══════════════════════════════════════════════════════════

static void clear_screen()
{
    DWORD mode = 0;
    GetConsoleMode(g_console, &mode);
    SetConsoleMode(g_console, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    printf("\033[2J\033[H");
}

static void print_banner()
{
    printf("\n");
    printf("  ___________         __     _____  __      __.__            .___\n");
    printf("  \\_   _____/__  ____/  |_  /  |  |/  \\    /  \\__| ____    __| _/______  _  ________\n");
    printf("   |    __)_\\  \\/  /\\   __\\/   |  |\\   \\/\\/   /  |/    \\  / __ |/  _ \\ \\/ \\/ /  ___/\n");
    printf("   |        \\>    <  |  | /    ^   /\\        /|  |   |  \\/ /_/ (  <_> )     /\\___ \\\n");
    printf("  /_______  /__/\\_ \\ |__| \\____   |  \\__/\\  / |__|___|  /\\____ |\\____/ \\/\\_//____  >\n");
    printf("          \\/      \\/           |__|       \\/          \\/      \\/                 \\/\n");
    printf("\n");
    set_color(CLR_WHITE);
    printf("  %s\n", tr(
        "Mount ext4 Linux drives on Windows",
        "Monte drives ext4 do Linux no Windows",
        "Monta unidades ext4 de Linux en Windows",
        "Ext4-Linux-Laufwerke unter Windows mounten",
        "Montez des disques ext4 Linux sous Windows",
        "\xe5\x9c\xa8Windows\xe4\xb8\x8a\xe6\x8c\x82\xe8\xbd\xbdLinux ext4\xe9\xa9\xb1\xe5\x8a\xa8\xe5\x99\xa8",
        "Windows\xe3\x81\xa7Linux ext4\xe3\x83\x89\xe3\x83\xa9\xe3\x82\xa4\xe3\x83\x96\xe3\x82\x92\xe3\x83\x9e\xe3\x82\xa6\xe3\x83\xb3\xe3\x83\x88",
        "\xd0\x9c\xd0\xbe\xd0\xbd\xd1\x82\xd0\xb8\xd1\x80\xd1\x83\xd0\xb9\xd1\x82\xd0\xb5 \xd0\xb4\xd0\xb8\xd1\x81\xd0\xba\xd0\xb8 ext4 Linux \xd0\xb2 Windows"));
    set_color(CLR_GRAY);
    printf("  github.com/Mateuscruz19/Ext4Windows\n");
    reset_color();
    printf("\n");
}

static void print_divider()
{
    set_color(CLR_BLUE);
    printf("  ");
    for (int i = 0; i < 56; i++) printf("=");
    printf("\n");
    reset_color();
}

static void print_main_menu()
{
    printf("\n");
    print_divider();
    printf("\n");

    set_color(CLR_WHITE);
    printf("   %s\n", tr(
        "MAIN MENU", "MENU PRINCIPAL", "MENU PRINCIPAL",
        "HAUPTMENU", "MENU PRINCIPAL",
        "\xe4\xb8\xbb\xe8\x8f\x9c\xe5\x8d\x95",
        "\xe3\x83\xa1\xe3\x82\xa4\xe3\x83\xb3\xe3\x83\xa1\xe3\x83\x8b\xe3\x83\xa5\xe3\x83\xbc",
        "\xd0\x93\xd0\x9b\xd0\x90\xd0\x92\xd0\x9d\xd0\x9e\xd0\x95 \xd0\x9c\xd0\x95\xd0\x9d\xd0\xae"));
    reset_color();
    printf("\n");

    set_color(CLR_ORANGE); printf("    [1] "); set_color(CLR_WHITE);
    printf("%s\n", tr(
        "Mount image file (.img)",
        "Montar arquivo de imagem (.img)",
        "Montar archivo de imagen (.img)",
        "Image-Datei mounten (.img)",
        "Monter un fichier image (.img)",
        "\xe6\x8c\x82\xe8\xbd\xbd\xe9\x95\x9c\xe5\x83\x8f\xe6\x96\x87\xe4\xbb\xb6 (.img)",
        "\xe3\x82\xa4\xe3\x83\xa1\xe3\x83\xbc\xe3\x82\xb8\xe3\x83\x95\xe3\x82\xa1\xe3\x82\xa4\xe3\x83\xab\xe3\x82\x92\xe3\x83\x9e\xe3\x82\xa6\xe3\x83\xb3\xe3\x83\x88 (.img)",
        "\xd0\xa1\xd0\xbc\xd0\xbe\xd0\xbd\xd1\x82\xd0\xb8\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\xd1\x82\xd1\x8c \xd0\xbe\xd0\xb1\xd1\x80\xd0\xb0\xd0\xb7 (.img)"));

    set_color(CLR_ORANGE); printf("    [2] "); set_color(CLR_WHITE);
    printf("%s\n", tr(
        "Scan for Linux partitions",
        "Buscar particoes Linux",
        "Buscar particiones Linux",
        "Nach Linux-Partitionen suchen",
        "Rechercher des partitions Linux",
        "\xe6\x89\xab\xe6\x8f\x8fLinux\xe5\x88\x86\xe5\x8c\xba",
        "Linux\xe3\x83\x91\xe3\x83\xbc\xe3\x83\x86\xe3\x82\xa3\xe3\x82\xb7\xe3\x83\xa7\xe3\x83\xb3\xe3\x82\x92\xe3\x82\xb9\xe3\x82\xad\xe3\x83\xa3\xe3\x83\xb3",
        "\xd0\x9f\xd0\xbe\xd0\xb8\xd1\x81\xd0\xba \xd1\x80\xd0\xb0\xd0\xb7\xd0\xb4\xd0\xb5\xd0\xbb\xd0\xbe\xd0\xb2 Linux"));

    set_color(CLR_ORANGE); printf("    [3] "); set_color(CLR_WHITE);
    printf("%s\n", tr(
        "Status (active mounts)",
        "Status (mounts ativos)",
        "Estado (montajes activos)",
        "Status (aktive Mounts)",
        "Statut (montages actifs)",
        "\xe7\x8a\xb6\xe6\x80\x81 (\xe6\xb4\xbb\xe5\x8a\xa8\xe6\x8c\x82\xe8\xbd\xbd)",
        "\xe3\x82\xb9\xe3\x83\x86\xe3\x83\xbc\xe3\x82\xbf\xe3\x82\xb9 (\xe3\x82\xa2\xe3\x82\xaf\xe3\x83\x86\xe3\x82\xa3\xe3\x83\x96\xe3\x83\x9e\xe3\x82\xa6\xe3\x83\xb3\xe3\x83\x88)",
        "\xd0\xa1\xd1\x82\xd0\xb0\xd1\x82\xd1\x83\xd1\x81 (\xd0\xb0\xd0\xba\xd1\x82\xd0\xb8\xd0\xb2\xd0\xbd\xd1\x8b\xd0\xb5 \xd0\xbc\xd0\xbe\xd0\xbd\xd1\x82\xd0\xb8\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\xd0\xbd\xd0\xb8\xd1\x8f)"));

    set_color(CLR_ORANGE); printf("    [4] "); set_color(CLR_WHITE);
    printf("%s\n", tr(
        "Unmount a drive",
        "Desmontar um drive",
        "Desmontar una unidad",
        "Laufwerk unmounten",
        "Demonter un lecteur",
        "\xe5\x8d\xb8\xe8\xbd\xbd\xe9\xa9\xb1\xe5\x8a\xa8\xe5\x99\xa8",
        "\xe3\x83\x89\xe3\x83\xa9\xe3\x82\xa4\xe3\x83\x96\xe3\x82\x92\xe3\x82\xa2\xe3\x83\xb3\xe3\x83\x9e\xe3\x82\xa6\xe3\x83\xb3\xe3\x83\x88",
        "\xd0\xa0\xd0\xb0\xd0\xb7\xd0\xbc\xd0\xbe\xd0\xbd\xd1\x82\xd0\xb8\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\xd1\x82\xd1\x8c \xd0\xb4\xd0\xb8\xd1\x81\xd0\xba"));

    set_color(CLR_ORANGE); printf("    [5] "); set_color(CLR_WHITE);
    printf("%s\n", tr("Settings", "Configuracoes", "Configuracion",
        "Einstellungen", "Parametres",
        "\xe8\xae\xbe\xe7\xbd\xae",
        "\xe8\xa8\xad\xe5\xae\x9a",
        "\xd0\x9d\xd0\xb0\xd1\x81\xd1\x82\xd1\x80\xd0\xbe\xd0\xb9\xd0\xba\xd0\xb8"));

    set_color(CLR_ORANGE); printf("    [6] "); set_color(CLR_WHITE);
    printf("%s\n", tr("Help", "Ajuda", "Ayuda", "Hilfe", "Aide",
        "\xe5\xb8\xae\xe5\x8a\xa9", "\xe3\x83\x98\xe3\x83\xab\xe3\x83\x97",
        "\xd0\x9f\xd0\xbe\xd0\xbc\xd0\xbe\xd1\x89\xd1\x8c"));

    set_color(CLR_ORANGE); printf("    [7] "); set_color(CLR_WHITE);
    printf("%s\n", tr("Quit", "Sair", "Salir", "Beenden", "Quitter",
        "\xe9\x80\x80\xe5\x87\xba", "\xe7\xb5\x82\xe4\xba\x86",
        "\xd0\x92\xd1\x8b\xd1\x85\xd0\xbe\xd0\xb4"));

    printf("\n");
    print_divider();
    printf("\n");
}

static void print_prompt()
{
    set_color(CLR_BLUE);
    printf("  ext4windows");
    set_color(CLR_ORANGE);
    printf("> ");
    reset_color();
}

static void print_result(const std::string& response)
{
    if (response.empty()) {
        set_color(CLR_RED);
        printf("\n  %s\n", tr(
            "Error: could not communicate with server.",
            "Erro: nao foi possivel comunicar com o servidor.",
            "Error: no se pudo comunicar con el servidor.",
            "Fehler: Kommunikation mit Server fehlgeschlagen.",
            "Erreur: impossible de communiquer avec le serveur.",
            "\xe9\x94\x99\xe8\xaf\xaf: \xe6\x97\xa0\xe6\xb3\x95\xe4\xb8\x8e\xe6\x9c\x8d\xe5\x8a\xa1\xe5\x99\xa8\xe9\x80\x9a\xe4\xbf\xa1\xe3\x80\x82",
            "\xe3\x82\xa8\xe3\x83\xa9\xe3\x83\xbc: \xe3\x82\xb5\xe3\x83\xbc\xe3\x83\x90\xe3\x83\xbc\xe3\x81\xa8\xe9\x80\x9a\xe4\xbf\xa1\xe3\x81\xa7\xe3\x81\x8d\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x80\x82",
        "\xd0\x9e\xd1\x88\xd0\xb8\xd0\xb1\xd0\xba\xd0\xb0: \xd0\xbd\xd0\xb5 \xd1\x83\xd0\xb4\xd0\xb0\xd0\xbb\xd0\xbe\xd1\x81\xd1\x8c \xd1\x81\xd0\xb2\xd1\x8f\xd0\xb7\xd0\xb0\xd1\x82\xd1\x8c\xd1\x81\xd1\x8f \xd1\x81 \xd1\x81\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80\xd0\xbe\xd0\xbc."));
        reset_color();
        return;
    }
    if (response.substr(0, 2) == "OK")
        set_color(CLR_GREEN);
    else
        set_color(CLR_RED);
    printf("\n  %s\n", response.c_str());
    reset_color();
}

// ═══════════════════════════════════════════════════════════
// Menu Actions
// ═══════════════════════════════════════════════════════════

static char find_free_drive()
{
    DWORD used = GetLogicalDrives();
    for (char letter = 'Z'; letter >= 'D'; letter--) {
        if (!(used & (1 << (letter - 'A'))))
            return letter;
    }
    return '\0';
}

static void do_mount_image()
{
    printf("\n");
    set_color(CLR_WHITE);
    printf("  %s\n", tr(
        "MOUNT IMAGE FILE", "MONTAR ARQUIVO DE IMAGEM",
        "MONTAR ARCHIVO DE IMAGEN", "IMAGE-DATEI MOUNTEN",
        "MONTER UN FICHIER IMAGE",
        "\xe6\x8c\x82\xe8\xbd\xbd\xe9\x95\x9c\xe5\x83\x8f\xe6\x96\x87\xe4\xbb\xb6",
        "\xe3\x82\xa4\xe3\x83\xa1\xe3\x83\xbc\xe3\x82\xb8\xe3\x83\x95\xe3\x82\xa1\xe3\x82\xa4\xe3\x83\xab\xe3\x82\x92\xe3\x83\x9e\xe3\x82\xa6\xe3\x83\xb3\xe3\x83\x88",
        "\xd0\xa1\xd0\x9c\xd0\x9e\xd0\x9d\xd0\xa2\xd0\x98\xd0\xa0\xd0\x9e\xd0\x92\xd0\x90\xd0\xa2\xd0\xac \xd0\x9e\xd0\x91\xd0\xa0\xd0\x90\xd0\x97"));
    reset_color();
    printf("\n");

    set_color(CLR_GRAY);
    printf("  %s\n", tr(
        "Enter the path to your .img file (or drag and drop):",
        "Digite o caminho do arquivo .img (ou arraste e solte):",
        "Ingrese la ruta del archivo .img (o arrastre y suelte):",
        "Pfad zur .img-Datei eingeben (oder Drag & Drop):",
        "Entrez le chemin du fichier .img (ou glissez-deposez):",
        "\xe8\xaf\xb7\xe8\xbe\x93\xe5\x85\xa5.img\xe6\x96\x87\xe4\xbb\xb6\xe8\xb7\xaf\xe5\xbe\x84\xef\xbc\x88\xe6\x88\x96\xe6\x8b\x96\xe6\x94\xbe\xef\xbc\x89:",
        ".img\xe3\x83\x95\xe3\x82\xa1\xe3\x82\xa4\xe3\x83\xab\xe3\x81\xae\xe3\x83\x91\xe3\x82\xb9\xe3\x82\x92\xe5\x85\xa5\xe5\x8a\x9b\xef\xbc\x88\xe3\x83\x89\xe3\x83\xa9\xe3\x83\x83\xe3\x82\xb0&\xe3\x83\x89\xe3\x83\xad\xe3\x83\x83\xe3\x83\x97\xe5\x8f\xaf\xef\xbc\x89:",
        "\xd0\x92\xd0\xb2\xd0\xb5\xd0\xb4\xd0\xb8\xd1\x82\xd0\xb5 \xd0\xbf\xd1\x83\xd1\x82\xd1\x8c \xd0\xba \xd1\x84\xd0\xb0\xd0\xb9\xd0\xbb\xd1\x83 .img (\xd0\xb8\xd0\xbb\xd0\xb8 \xd0\xbf\xd0\xb5\xd1\x80\xd0\xb5\xd1\x82\xd0\xb0\xd1\x89\xd0\xb8\xd1\x82\xd0\xb5):"));
    reset_color();
    printf("\n");
    print_prompt();

    char path[512] = {};
    if (!read_line(path, sizeof(path)) || path[0] == '\0') {
        set_color(CLR_RED);
        printf("\n  %s\n", tr(
            "No file specified. Going back to menu.",
            "Nenhum arquivo informado. Voltando ao menu.",
            "Ningun archivo especificado. Volviendo al menu.",
            "Keine Datei angegeben. Zuruck zum Menu.",
            "Aucun fichier specifie. Retour au menu.",
            "\xe6\x9c\xaa\xe6\x8c\x87\xe5\xae\x9a\xe6\x96\x87\xe4\xbb\xb6\xe3\x80\x82\xe8\xbf\x94\xe5\x9b\x9e\xe8\x8f\x9c\xe5\x8d\x95\xe3\x80\x82",
            "\xe3\x83\x95\xe3\x82\xa1\xe3\x82\xa4\xe3\x83\xab\xe6\x9c\xaa\xe6\x8c\x87\xe5\xae\x9a\xe3\x80\x82\xe3\x83\xa1\xe3\x83\x8b\xe3\x83\xa5\xe3\x83\xbc\xe3\x81\xab\xe6\x88\xbb\xe3\x82\x8a\xe3\x81\xbe\xe3\x81\x99\xe3\x80\x82",
        "\xd0\xa4\xd0\xb0\xd0\xb9\xd0\xbb \xd0\xbd\xd0\xb5 \xd1\x83\xd0\xba\xd0\xb0\xd0\xb7\xd0\xb0\xd0\xbd. \xd0\x92\xd0\xbe\xd0\xb7\xd0\xb2\xd1\x80\xd0\xb0\xd1\x82 \xd0\xb2 \xd0\xbc\xd0\xb5\xd0\xbd\xd1\x8e."));
        reset_color();
        return;
    }
    strip_quotes(path);

    char abs_path[MAX_PATH] = {};
    GetFullPathNameA(path, MAX_PATH, abs_path, nullptr);

    // Drive letter
    char free = find_free_drive();
    printf("\n");
    set_color(CLR_GRAY);
    if (free) {
        printf("  %s [%c]:\n", tr(
            "Drive letter? Press Enter for auto",
            "Letra do drive? Enter para auto",
            "Letra de unidad? Enter para auto",
            "Laufwerksbuchstabe? Enter fur auto",
            "Lettre de lecteur? Entree pour auto",
            "\xe9\xa9\xb1\xe5\x8a\xa8\xe5\x99\xa8\xe5\x8f\xb7? \xe5\x9b\x9e\xe8\xbd\xa6\xe8\x87\xaa\xe5\x8a\xa8\xe9\x80\x89\xe6\x8b\xa9",
            "\xe3\x83\x89\xe3\x83\xa9\xe3\x82\xa4\xe3\x83\x96\xe6\x96\x87\xe5\xad\x97? Enter\xe3\x81\xa7\xe8\x87\xaa\xe5\x8b\x95",
        "\xd0\x91\xd1\x83\xd0\xba\xd0\xb2\xd0\xb0 \xd0\xb4\xd0\xb8\xd1\x81\xd0\xba\xd0\xb0? Enter \xd0\xb4\xd0\xbb\xd1\x8f \xd0\xb0\xd0\xb2\xd1\x82\xd0\xbe"), free);
    } else {
        printf("  %s:\n", tr(
            "Drive letter?", "Letra do drive?",
            "Letra de unidad?", "Laufwerksbuchstabe?",
            "Lettre de lecteur?",
            "\xe9\xa9\xb1\xe5\x8a\xa8\xe5\x99\xa8\xe5\x8f\xb7?",
            "\xe3\x83\x89\xe3\x83\xa9\xe3\x82\xa4\xe3\x83\x96\xe6\x96\x87\xe5\xad\x97?",
        "\xd0\x91\xd1\x83\xd0\xba\xd0\xb2\xd0\xb0 \xd0\xb4\xd0\xb8\xd1\x81\xd0\xba\xd0\xb0?"));
    }
    reset_color();
    print_prompt();

    char drive_input[64] = {};
    read_line(drive_input, sizeof(drive_input));

    char drive_letter;
    if (drive_input[0] == '\0' && free) {
        drive_letter = free;
    } else if (drive_input[0] >= 'A' && drive_input[0] <= 'Z') {
        drive_letter = drive_input[0];
    } else if (drive_input[0] >= 'a' && drive_input[0] <= 'z') {
        drive_letter = drive_input[0] - 32;
    } else {
        set_color(CLR_RED);
        printf("\n  %s\n", tr(
            "Invalid drive letter.",
            "Letra de drive invalida.",
            "Letra de unidad invalida.",
            "Ungultiger Laufwerksbuchstabe.",
            "Lettre de lecteur invalide.",
            "\xe6\x97\xa0\xe6\x95\x88\xe7\x9a\x84\xe9\xa9\xb1\xe5\x8a\xa8\xe5\x99\xa8\xe5\x8f\xb7\xe3\x80\x82",
            "\xe7\x84\xa1\xe5\x8a\xb9\xe3\x81\xaa\xe3\x83\x89\xe3\x83\xa9\xe3\x82\xa4\xe3\x83\x96\xe6\x96\x87\xe5\xad\x97\xe3\x80\x82",
        "\xd0\x9d\xd0\xb5\xd0\xb2\xd0\xb5\xd1\x80\xd0\xbd\xd0\xb0\xd1\x8f \xd0\xb1\xd1\x83\xd0\xba\xd0\xb2\xd0\xb0 \xd0\xb4\xd0\xb8\xd1\x81\xd0\xba\xd0\xb0."));
        reset_color();
        return;
    }

    // Access mode
    printf("\n");
    set_color(CLR_WHITE);
    printf("  %s\n", tr(
        "ACCESS MODE", "MODO DE ACESSO", "MODO DE ACCESO",
        "ZUGRIFFSMODUS", "MODE D'ACCES",
        "\xe8\xae\xbf\xe9\x97\xae\xe6\xa8\xa1\xe5\xbc\x8f",
        "\xe3\x82\xa2\xe3\x82\xaf\xe3\x82\xbb\xe3\x82\xb9\xe3\x83\xa2\xe3\x83\xbc\xe3\x83\x89",
        "\xd0\xa0\xd0\x95\xd0\x96\xd0\x98\xd0\x9c \xd0\x94\xd0\x9e\xd0\xa1\xd0\xa2\xd0\xa3\xd0\x9f\xd0\x90"));
    reset_color();
    set_color(CLR_ORANGE); printf("    [1] "); set_color(CLR_WHITE);
    printf("%s\n", tr(
        "Read-only  (safe, cannot modify files)",
        "Somente leitura  (seguro, nao modifica)",
        "Solo lectura  (seguro, no modifica archivos)",
        "Nur lesen  (sicher, keine Anderungen)",
        "Lecture seule  (sur, pas de modification)",
        "\xe5\x8f\xaa\xe8\xaf\xbb  (\xe5\xae\x89\xe5\x85\xa8\xef\xbc\x8c\xe4\xb8\x8d\xe8\x83\xbd\xe4\xbf\xae\xe6\x94\xb9\xe6\x96\x87\xe4\xbb\xb6)",
        "\xe8\xaa\xad\xe3\x81\xbf\xe5\x8f\x96\xe3\x82\x8a\xe5\xb0\x82\xe7\x94\xa8  (\xe5\xae\x89\xe5\x85\xa8\xe3\x80\x81\xe3\x83\x95\xe3\x82\xa1\xe3\x82\xa4\xe3\x83\xab\xe5\xa4\x89\xe6\x9b\xb4\xe4\xb8\x8d\xe5\x8f\xaf)",
        "\xd0\xa2\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xba\xd0\xbe \xd1\x87\xd1\x82\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5  (\xd0\xb1\xd0\xb5\xd0\xb7\xd0\xbe\xd0\xbf\xd0\xb0\xd1\x81\xd0\xbd\xd0\xbe, \xd1\x84\xd0\xb0\xd0\xb9\xd0\xbb\xd1\x8b \xd0\xbd\xd0\xb5 \xd0\xb8\xd0\xb7\xd0\xbc\xd0\xb5\xd0\xbd\xd1\x8f\xd1\x8e\xd1\x82\xd1\x81\xd1\x8f)"));
    set_color(CLR_ORANGE); printf("    [2] "); set_color(CLR_WHITE);
    printf("%s\n", tr(
        "Read-write (can create, edit, delete)",
        "Leitura e escrita (pode criar, editar, deletar)",
        "Lectura y escritura (puede crear, editar, eliminar)",
        "Lesen/Schreiben (erstellen, bearbeiten, loschen)",
        "Lecture/ecriture (creer, modifier, supprimer)",
        "\xe8\xaf\xbb\xe5\x86\x99  (\xe5\x8f\xaf\xe5\x88\x9b\xe5\xbb\xba\xe3\x80\x81\xe7\xbc\x96\xe8\xbe\x91\xe3\x80\x81\xe5\x88\xa0\xe9\x99\xa4)",
        "\xe8\xaa\xad\xe3\x81\xbf\xe6\x9b\xb8\xe3\x81\x8d  (\xe4\xbd\x9c\xe6\x88\x90\xe3\x80\x81\xe7\xb7\xa8\xe9\x9b\x86\xe3\x80\x81\xe5\x89\x8a\xe9\x99\xa4\xe5\x8f\xaf\xe8\x83\xbd)",
        "\xd0\xa7\xd1\x82\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5/\xd0\xb7\xd0\xb0\xd0\xbf\xd0\xb8\xd1\x81\xd1\x8c (\xd1\x81\xd0\xbe\xd0\xb7\xd0\xb4\xd0\xb0\xd0\xb2\xd0\xb0\xd1\x82\xd1\x8c, \xd1\x80\xd0\xb5\xd0\xb4\xd0\xb0\xd0\xba\xd1\x82\xd0\xb8\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\xd1\x82\xd1\x8c, \xd1\x83\xd0\xb4\xd0\xb0\xd0\xbb\xd1\x8f\xd1\x82\xd1\x8c)"));
    reset_color();
    set_color(CLR_GRAY);
    printf("  [Enter = %s]\n", g_default_mode ? "2" : "1");
    reset_color();
    print_prompt();

    char rw_input[64] = {};
    read_line(rw_input, sizeof(rw_input));
    bool read_write = (rw_input[0] == '\0')
        ? (g_default_mode == 1) : (rw_input[0] == '2');

    printf("\n");
    set_color(CLR_GRAY);
    printf("  %s", tr(
        "Mounting...", "Montando...", "Montando...",
        "Wird gemountet...", "Montage en cours...",
        "\xe6\xad\xa3\xe5\x9c\xa8\xe6\x8c\x82\xe8\xbd\xbd...",
        "\xe3\x83\x9e\xe3\x82\xa6\xe3\x83\xb3\xe3\x83\x88\xe4\xb8\xad...",
        "\xd0\x9c\xd0\xbe\xd0\xbd\xd1\x82\xd0\xb8\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\xd0\xbd\xd0\xb8\xd0\xb5..."));
    reset_color();

    std::string cmd = "MOUNT ";
    cmd += abs_path;
    cmd += " ";
    cmd += drive_letter;
    if (read_write) cmd += " RW";

    std::string response = send_command(cmd);
    print_result(response);
}

// ═══════════════════════════════════════════════════════════
// Partition scan (requires admin)
// ═══════════════════════════════════════════════════════════

static bool is_elevated()
{
    BOOL elevated = FALSE;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elev = {};
        DWORD size = sizeof(elev);
        if (GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &size))
            elevated = elev.TokenIsElevated;
        CloseHandle(token);
    }
    return elevated != FALSE;
}

static std::vector<PartitionInfo> scan_with_elevation()
{
    std::vector<PartitionInfo> results;
    wchar_t temp_dir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp_dir);
    std::wstring temp_file = std::wstring(temp_dir) + L"ext4windows_scan.txt";
    DeleteFileW(temp_file.c_str());

    wchar_t exe_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    std::wstring args = L"--scan-save \"" + temp_file + L"\"";

    set_color(CLR_GRAY);
    printf("  %s\n", tr(
        "Requesting Administrator access...",
        "Solicitando acesso de Administrador...",
        "Solicitando acceso de Administrador...",
        "Administratorzugriff wird angefordert...",
        "Demande d'acces Administrateur...",
        "\xe8\xaf\xb7\xe6\xb1\x82\xe7\xae\xa1\xe7\x90\x86\xe5\x91\x98\xe6\x9d\x83\xe9\x99\x90...",
        "\xe7\xae\xa1\xe7\x90\x86\xe8\x80\x85\xe6\xa8\xa9\xe9\x99\x90\xe3\x82\x92\xe8\xa6\x81\xe6\xb1\x82\xe4\xb8\xad...",
        "\xd0\x97\xd0\xb0\xd0\xbf\xd1\x80\xd0\xbe\xd1\x81 \xd0\xbf\xd1\x80\xd0\xb0\xd0\xb2 \xd0\x90\xd0\xb4\xd0\xbc\xd0\xb8\xd0\xbd\xd0\xb8\xd1\x81\xd1\x82\xd1\x80\xd0\xb0\xd1\x82\xd0\xbe\xd1\x80\xd0\xb0..."));
    reset_color();

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe_path;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
        set_color(CLR_RED);
        printf("  %s\n", tr(
            "Administrator access denied.",
            "Acesso de Administrador negado.",
            "Acceso de Administrador denegado.",
            "Administratorzugriff verweigert.",
            "Acces Administrateur refuse.",
            "\xe7\xae\xa1\xe7\x90\x86\xe5\x91\x98\xe6\x9d\x83\xe9\x99\x90\xe8\xa2\xab\xe6\x8b\x92\xe7\xbb\x9d\xe3\x80\x82",
            "\xe7\xae\xa1\xe7\x90\x86\xe8\x80\x85\xe6\xa8\xa9\xe9\x99\x90\xe3\x81\x8c\xe6\x8b\x92\xe5\x90\xa6\xe3\x81\x95\xe3\x82\x8c\xe3\x81\xbe\xe3\x81\x97\xe3\x81\x9f\xe3\x80\x82",
        "\xd0\x94\xd0\xbe\xd1\x81\xd1\x82\xd1\x83\xd0\xbf \xd0\x90\xd0\xb4\xd0\xbc\xd0\xb8\xd0\xbd\xd0\xb8\xd1\x81\xd1\x82\xd1\x80\xd0\xb0\xd1\x82\xd0\xbe\xd1\x80\xd0\xb0 \xd0\xbe\xd1\x82\xd0\xba\xd0\xbb\xd0\xbe\xd0\xbd\xd1\x91\xd0\xbd."));
        reset_color();
        return results;
    }

    WaitForSingleObject(sei.hProcess, 30000);
    CloseHandle(sei.hProcess);

    FILE* f = _wfopen(temp_file.c_str(), L"r, ccs=UTF-8");
    if (!f) return results;

    wchar_t line[512];
    while (fgetws(line, 512, f)) {
        size_t len = wcslen(line);
        if (len > 0 && line[len - 1] == L'\n') line[len - 1] = L'\0';
        wchar_t* ctx = nullptr;
        wchar_t* dev = wcstok(line, L"|", &ctx);
        wchar_t* disp = wcstok(nullptr, L"|", &ctx);
        wchar_t* sz = wcstok(nullptr, L"|", &ctx);
        wchar_t* dk = wcstok(nullptr, L"|", &ctx);
        wchar_t* pn = wcstok(nullptr, L"|", &ctx);
        if (dev && disp && sz && dk && pn) {
            PartitionInfo info;
            info.device_path = dev;
            info.display_name = disp;
            info.size_bytes = _wcstoui64(sz, nullptr, 10);
            info.disk_number = _wtoi(dk);
            info.partition_number = _wtoi(pn);
            results.push_back(info);
        }
    }
    fclose(f);
    DeleteFileW(temp_file.c_str());
    return results;
}

static HANDLE open_device_elevated(const wchar_t* device_path, bool read_only)
{
    wchar_t temp_dir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp_dir);
    std::wstring temp_file = std::wstring(temp_dir) + L"ext4windows_handle.txt";
    DeleteFileW(temp_file.c_str());

    wchar_t exe_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    wchar_t pid_str[32];
    swprintf(pid_str, 32, L"%lu", GetCurrentProcessId());

    std::wstring args = L"--open-device \"";
    args += device_path;
    args += L"\" ";
    args += pid_str;
    args += L" \"";
    args += temp_file;
    args += L"\"";
    if (!read_only) args += L" --rw";

    set_color(CLR_GRAY);
    printf("  %s\n", tr(
        "Requesting Administrator access...",
        "Solicitando acesso de Administrador...",
        "Solicitando acceso de Administrador...",
        "Administratorzugriff wird angefordert...",
        "Demande d'acces Administrateur...",
        "\xe8\xaf\xb7\xe6\xb1\x82\xe7\xae\xa1\xe7\x90\x86\xe5\x91\x98\xe6\x9d\x83\xe9\x99\x90...",
        "\xe7\xae\xa1\xe7\x90\x86\xe8\x80\x85\xe6\xa8\xa9\xe9\x99\x90\xe3\x82\x92\xe8\xa6\x81\xe6\xb1\x82\xe4\xb8\xad...",
        "\xd0\x97\xd0\xb0\xd0\xbf\xd1\x80\xd0\xbe\xd1\x81 \xd0\xbf\xd1\x80\xd0\xb0\xd0\xb2 \xd0\x90\xd0\xb4\xd0\xbc\xd0\xb8\xd0\xbd\xd0\xb8\xd1\x81\xd1\x82\xd1\x80\xd0\xb0\xd1\x82\xd0\xbe\xd1\x80\xd0\xb0..."));
    reset_color();

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe_path;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei)) return INVALID_HANDLE_VALUE;
    WaitForSingleObject(sei.hProcess, 15000);
    CloseHandle(sei.hProcess);

    FILE* f = _wfopen(temp_file.c_str(), L"r");
    if (!f) return INVALID_HANDLE_VALUE;
    unsigned long long handle_val = 0;
    if (fscanf(f, "%llu", &handle_val) != 1) handle_val = 0;
    fclose(f);
    DeleteFileW(temp_file.c_str());
    if (handle_val == 0) return INVALID_HANDLE_VALUE;
    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle_val));
}

static void do_scan_partitions()
{
    printf("\n");
    set_color(CLR_WHITE);
    printf("  %s\n", tr(
        "SCAN FOR LINUX PARTITIONS",
        "BUSCAR PARTICOES LINUX",
        "BUSCAR PARTICIONES LINUX",
        "NACH LINUX-PARTITIONEN SUCHEN",
        "RECHERCHER DES PARTITIONS LINUX",
        "\xe6\x89\xab\xe6\x8f\x8fLINUX\xe5\x88\x86\xe5\x8c\xba",
        "LINUX\xe3\x83\x91\xe3\x83\xbc\xe3\x83\x86\xe3\x82\xa3\xe3\x82\xb7\xe3\x83\xa7\xe3\x83\xb3\xe3\x82\x92\xe3\x82\xb9\xe3\x82\xad\xe3\x83\xa3\xe3\x83\xb3",
        "\xd0\x9f\xd0\x9e\xd0\x98\xd0\xa1\xd0\x9a \xd0\xa0\xd0\x90\xd0\x97\xd0\x94\xd0\x95\xd0\x9b\xd0\x9e\xd0\x92 LINUX"));
    reset_color();
    printf("\n");

    set_color(CLR_GRAY);
    printf("  %s\n", tr(
        "Scanning disks...", "Escaneando discos...",
        "Escaneando discos...", "Festplatten werden gescannt...",
        "Analyse des disques...",
        "\xe6\xad\xa3\xe5\x9c\xa8\xe6\x89\xab\xe6\x8f\x8f\xe7\xa3\x81\xe7\x9b\x98...",
        "\xe3\x83\x87\xe3\x82\xa3\xe3\x82\xb9\xe3\x82\xaf\xe3\x82\x92\xe3\x82\xb9\xe3\x82\xad\xe3\x83\xa3\xe3\x83\xb3\xe4\xb8\xad...",
        "\xd0\xa1\xd0\xba\xd0\xb0\xd0\xbd\xd0\xb8\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\xd0\xbd\xd0\xb8\xd0\xb5 \xd0\xb4\xd0\xb8\xd1\x81\xd0\xba\xd0\xbe\xd0\xb2..."));
    reset_color();

    std::vector<PartitionInfo> partitions;
    if (is_elevated())
        partitions = scan_ext4_partitions();
    else
        partitions = scan_with_elevation();

    if (partitions.empty()) {
        set_color(CLR_RED);
        printf("\n  %s\n", tr(
            "No ext4 partitions found.",
            "Nenhuma particao ext4 encontrada.",
            "No se encontraron particiones ext4.",
            "Keine ext4-Partitionen gefunden.",
            "Aucune partition ext4 trouvee.",
            "\xe6\x9c\xaa\xe6\x89\xbe\xe5\x88\xb0\x65\x78\x74\x34\xe5\x88\x86\xe5\x8c\xba\xe3\x80\x82",
            "ext4\xe3\x83\x91\xe3\x83\xbc\xe3\x83\x86\xe3\x82\xa3\xe3\x82\xb7\xe3\x83\xa7\xe3\x83\xb3\xe3\x81\x8c\xe8\xa6\x8b\xe3\x81\xa4\xe3\x81\x8b\xe3\x82\x8a\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x80\x82",
        "\xd0\xa0\xd0\xb0\xd0\xb7\xd0\xb4\xd0\xb5\xd0\xbb\xd1\x8b ext4 \xd0\xbd\xd0\xb5 \xd0\xbd\xd0\xb0\xd0\xb9\xd0\xb4\xd0\xb5\xd0\xbd\xd1\x8b."));
        reset_color();
        return;
    }

    printf("\n");
    set_color(CLR_GREEN);
    printf("  %s %zu %s\n",
        tr("Found", "Encontradas", "Encontradas", "Gefunden",
           "Trouvees", "\xe6\x89\xbe\xe5\x88\xb0", "\xe7\x99\xba\xe8\xa6\x8b",
        "\xd0\x9d\xd0\xb0\xd0\xb9\xd0\xb4\xd0\xb5\xd0\xbd\xd0\xbe"),
        partitions.size(),
        tr("ext4 partition(s):", "particao(oes) ext4:",
           "particion(es) ext4:", "ext4-Partition(en):",
           "partition(s) ext4:",
           "\xe4\xb8\xaa\x65\x78\x74\x34\xe5\x88\x86\xe5\x8c\xba:",
           "\xe5\x80\x8b\xe3\x81\xae" "ext4\xe3\x83\x91\xe3\x83\xbc\xe3\x83\x86\xe3\x82\xa3\xe3\x82\xb7\xe3\x83\xa7\xe3\x83\xb3:"));
    reset_color();
    printf("\n");

    for (size_t i = 0; i < partitions.size(); i++) {
        char display[256] = {};
        WideCharToMultiByte(CP_UTF8, 0,
            partitions[i].display_name.c_str(), -1,
            display, sizeof(display), nullptr, nullptr);
        set_color(CLR_ORANGE);
        printf("    [%zu] ", i + 1);
        set_color(CLR_WHITE);
        printf("%s\n", display);
    }

    printf("\n");
    set_color(CLR_GRAY);
    printf("  %s\n", tr(
        "Choose a partition (0 to cancel):",
        "Escolha uma particao (0 para cancelar):",
        "Elija una particion (0 para cancelar):",
        "Partition wahlen (0 zum Abbrechen):",
        "Choisissez une partition (0 pour annuler):",
        "\xe9\x80\x89\xe6\x8b\xa9\xe5\x88\x86\xe5\x8c\xba (0\xe5\x8f\x96\xe6\xb6\x88):",
        "\xe3\x83\x91\xe3\x83\xbc\xe3\x83\x86\xe3\x82\xa3\xe3\x82\xb7\xe3\x83\xa7\xe3\x83\xb3\xe3\x82\x92\xe9\x81\xb8\xe6\x8a\x9e (0\xe3\x81\xa7\xe3\x82\xad\xe3\x83\xa3\xe3\x83\xb3\xe3\x82\xbb\xe3\x83\xab):",
        "\xd0\x92\xd1\x8b\xd0\xb1\xd0\xb5\xd1\x80\xd0\xb8\xd1\x82\xd0\xb5 \xd1\x80\xd0\xb0\xd0\xb7\xd0\xb4\xd0\xb5\xd0\xbb (0 \xd0\xb4\xd0\xbb\xd1\x8f \xd0\xbe\xd1\x82\xd0\xbc\xd0\xb5\xd0\xbd\xd1\x8b):"));
    reset_color();
    print_prompt();

    char input[64] = {};
    if (!read_line(input, sizeof(input))) return;
    int choice = atoi(input);
    if (choice <= 0 || choice > (int)partitions.size()) return;
    auto& part = partitions[choice - 1];

    // Ask drive letter
    char free = find_free_drive();
    printf("\n");
    set_color(CLR_GRAY);
    if (free) {
        printf("  %s [%c]:\n", tr(
            "Drive letter? Press Enter for auto",
            "Letra do drive? Enter para auto",
            "Letra de unidad? Enter para auto",
            "Laufwerksbuchstabe? Enter fur auto",
            "Lettre de lecteur? Entree pour auto",
            "\xe9\xa9\xb1\xe5\x8a\xa8\xe5\x99\xa8\xe5\x8f\xb7? \xe5\x9b\x9e\xe8\xbd\xa6\xe8\x87\xaa\xe5\x8a\xa8",
            "\xe3\x83\x89\xe3\x83\xa9\xe3\x82\xa4\xe3\x83\x96\xe6\x96\x87\xe5\xad\x97? Enter\xe3\x81\xa7\xe8\x87\xaa\xe5\x8b\x95",
        "\xd0\x91\xd1\x83\xd0\xba\xd0\xb2\xd0\xb0 \xd0\xb4\xd0\xb8\xd1\x81\xd0\xba\xd0\xb0? Enter \xd0\xb4\xd0\xbb\xd1\x8f \xd0\xb0\xd0\xb2\xd1\x82\xd0\xbe"), free);
    } else {
        printf("  %s:\n", tr(
            "Drive letter?", "Letra do drive?",
            "Letra de unidad?", "Laufwerksbuchstabe?",
            "Lettre de lecteur?",
            "\xe9\xa9\xb1\xe5\x8a\xa8\xe5\x99\xa8\xe5\x8f\xb7?",
            "\xe3\x83\x89\xe3\x83\xa9\xe3\x82\xa4\xe3\x83\x96\xe6\x96\x87\xe5\xad\x97?",
        "\xd0\x91\xd1\x83\xd0\xba\xd0\xb2\xd0\xb0 \xd0\xb4\xd0\xb8\xd1\x81\xd0\xba\xd0\xb0?"));
    }
    reset_color();
    print_prompt();

    char drive_input[64] = {};
    read_line(drive_input, sizeof(drive_input));
    char drive_letter;
    if (drive_input[0] == '\0' && free)
        drive_letter = free;
    else if (drive_input[0] >= 'a' && drive_input[0] <= 'z')
        drive_letter = drive_input[0] - 32;
    else if (drive_input[0] >= 'A' && drive_input[0] <= 'Z')
        drive_letter = drive_input[0];
    else {
        set_color(CLR_RED);
        printf("\n  %s\n", tr(
            "Invalid drive letter.", "Letra invalida.",
            "Letra invalida.", "Ungultiger Buchstabe.",
            "Lettre invalide.",
            "\xe6\x97\xa0\xe6\x95\x88\xe5\xad\x97\xe6\xaf\x8d\xe3\x80\x82",
            "\xe7\x84\xa1\xe5\x8a\xb9\xe3\x81\xaa\xe6\x96\x87\xe5\xad\x97\xe3\x80\x82",
        "\xd0\x9d\xd0\xb5\xd0\xb2\xd0\xb5\xd1\x80\xd0\xbd\xd0\xb0\xd1\x8f \xd0\xb1\xd1\x83\xd0\xba\xd0\xb2\xd0\xb0 \xd0\xb4\xd0\xb8\xd1\x81\xd0\xba\xd0\xb0."));
        reset_color();
        return;
    }

    // Access mode
    printf("\n");
    set_color(CLR_WHITE);
    printf("  %s\n", tr(
        "ACCESS MODE", "MODO DE ACESSO", "MODO DE ACCESO",
        "ZUGRIFFSMODUS", "MODE D'ACCES",
        "\xe8\xae\xbf\xe9\x97\xae\xe6\xa8\xa1\xe5\xbc\x8f",
        "\xe3\x82\xa2\xe3\x82\xaf\xe3\x82\xbb\xe3\x82\xb9\xe3\x83\xa2\xe3\x83\xbc\xe3\x83\x89",
        "\xd0\xa0\xd0\x95\xd0\x96\xd0\x98\xd0\x9c \xd0\x94\xd0\x9e\xd0\xa1\xd0\xa2\xd0\xa3\xd0\x9f\xd0\x90"));
    reset_color();
    set_color(CLR_ORANGE); printf("    [1] "); set_color(CLR_WHITE);
    printf("%s\n", tr(
        "Read-only  (safe)", "Somente leitura  (seguro)",
        "Solo lectura  (seguro)", "Nur lesen  (sicher)",
        "Lecture seule  (sur)",
        "\xe5\x8f\xaa\xe8\xaf\xbb  (\xe5\xae\x89\xe5\x85\xa8)",
        "\xe8\xaa\xad\xe3\x81\xbf\xe5\x8f\x96\xe3\x82\x8a\xe5\xb0\x82\xe7\x94\xa8  (\xe5\xae\x89\xe5\x85\xa8)",
        "\xd0\xa2\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xba\xd0\xbe \xd1\x87\xd1\x82\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5  (\xd0\xb1\xd0\xb5\xd0\xb7\xd0\xbe\xd0\xbf\xd0\xb0\xd1\x81\xd0\xbd\xd0\xbe)"));
    set_color(CLR_ORANGE); printf("    [2] "); set_color(CLR_WHITE);
    printf("%s\n", tr(
        "Read-write", "Leitura e escrita",
        "Lectura y escritura", "Lesen/Schreiben",
        "Lecture/ecriture",
        "\xe8\xaf\xbb\xe5\x86\x99",
        "\xe8\xaa\xad\xe3\x81\xbf\xe6\x9b\xb8\xe3\x81\x8d",
        "\xd0\xa7\xd1\x82\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5/\xd0\xb7\xd0\xb0\xd0\xbf\xd0\xb8\xd1\x81\xd1\x8c"));
    reset_color();
    set_color(CLR_GRAY);
    printf("  [Enter = %s]\n", g_default_mode ? "2" : "1");
    reset_color();
    print_prompt();

    char rw_input[64] = {};
    read_line(rw_input, sizeof(rw_input));
    bool read_write = (rw_input[0] == '\0')
        ? (g_default_mode == 1) : (rw_input[0] == '2');
    bool read_only = !read_write;

    // Mount via server pipe (MOUNT_PARTITION command) — the server keeps the
    // filesystem alive in the background, so closing this window won't unmount.
    printf("\n");
    set_color(CLR_GRAY);
    printf("  %s", tr(
        "Mounting...", "Montando...", "Montando...",
        "Wird gemountet...", "Montage en cours...",
        "\xe6\xad\xa3\xe5\x9c\xa8\xe6\x8c\x82\xe8\xbd\xbd...",
        "\xe3\x83\x9e\xe3\x82\xa6\xe3\x83\xb3\xe3\x83\x88\xe4\xb8\xad...",
        "\xd0\x9c\xd0\xbe\xd0\xbd\xd1\x82\xd0\xb8\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\xd0\xbd\xd0\xb8\xd0\xb5..."));
    reset_color();

    // Convert device path to UTF-8 for the pipe command
    char devpath_utf8[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, part.device_path.c_str(), -1,
                         devpath_utf8, sizeof(devpath_utf8), nullptr, nullptr);

    // Build pipe command: MOUNT_PARTITION <drive> <RW|RO> <device_path>
    std::string pipe_cmd = "MOUNT_PARTITION ";
    pipe_cmd += drive_letter;
    pipe_cmd += read_write ? " RW " : " RO ";
    pipe_cmd += devpath_utf8;

    std::string response = send_command(pipe_cmd);

    if (response.empty()) {
        set_color(CLR_RED);
        printf("\n\n  %s\n", tr(
            "Error: no response from server.",
            "Erro: sem resposta do servidor.",
            "Error: sin respuesta del servidor.",
            "Fehler: Keine Antwort vom Server.",
            "Erreur: pas de reponse du serveur.",
            "\xe9\x94\x99\xe8\xaf\xaf: \xe6\x9c\x8d\xe5\x8a\xa1\xe5\x99\xa8\xe6\x97\xa0\xe5\x93\x8d\xe5\xba\x94\xe3\x80\x82",
            "\xe3\x82\xa8\xe3\x83\xa9\xe3\x83\xbc: \xe3\x82\xb5\xe3\x83\xbc\xe3\x83\x90\xe3\x83\xbc\xe3\x81\x8b\xe3\x82\x89\xe5\xbf\x9c\xe7\xad\x94\xe3\x81\x8c\xe3\x81\x82\xe3\x82\x8a\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x80\x82",
        "\xd0\x9e\xd1\x88\xd0\xb8\xd0\xb1\xd0\xba\xd0\xb0: \xd0\xbd\xd0\xb5\xd1\x82 \xd0\xbe\xd1\x82\xd0\xb2\xd0\xb5\xd1\x82\xd0\xb0 \xd0\xbe\xd1\x82 \xd1\x81\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80\xd0\xb0."));
        reset_color();
        return;
    }

    if (response.substr(0, 2) == "OK") {
        set_color(CLR_GREEN);
        printf("\n\n  %s\n", response.c_str());
        reset_color();
    } else {
        set_color(CLR_RED);
        printf("\n\n  %s\n", response.c_str());
        reset_color();
    }
}

// ═══════════════════════════════════════════════════════════
// Status, Unmount, Help
// ═══════════════════════════════════════════════════════════

static void do_status()
{
    printf("\n");
    set_color(CLR_WHITE);
    printf("  %s\n", tr(
        "MOUNT STATUS", "STATUS DOS MOUNTS", "ESTADO DE MONTAJES",
        "MOUNT-STATUS", "STATUT DES MONTAGES",
        "\xe6\x8c\x82\xe8\xbd\xbd\xe7\x8a\xb6\xe6\x80\x81",
        "\xe3\x83\x9e\xe3\x82\xa6\xe3\x83\xb3\xe3\x83\x88\xe3\x82\xb9\xe3\x83\x86\xe3\x83\xbc\xe3\x82\xbf\xe3\x82\xb9",
        "\xd0\xa1\xd0\xa2\xd0\x90\xd0\xa2\xd0\xa3\xd0\xa1 \xd0\x9c\xd0\x9e\xd0\x9d\xd0\xa2\xd0\x98\xd0\xa0\xd0\x9e\xd0\x92\xd0\x90\xd0\x9d\xd0\x98\xd0\xaf"));
    reset_color();
    std::string response = send_command("STATUS");
    print_result(response);
}

static void do_unmount()
{
    printf("\n");
    set_color(CLR_WHITE);
    printf("  %s\n", tr(
        "UNMOUNT DRIVE", "DESMONTAR DRIVE",
        "DESMONTAR UNIDAD", "LAUFWERK UNMOUNTEN",
        "DEMONTER UN LECTEUR",
        "\xe5\x8d\xb8\xe8\xbd\xbd\xe9\xa9\xb1\xe5\x8a\xa8\xe5\x99\xa8",
        "\xe3\x83\x89\xe3\x83\xa9\xe3\x82\xa4\xe3\x83\x96\xe3\x82\x92\xe3\x82\xa2\xe3\x83\xb3\xe3\x83\x9e\xe3\x82\xa6\xe3\x83\xb3\xe3\x83\x88",
        "\xd0\xa0\xd0\x90\xd0\x97\xd0\x9c\xd0\x9e\xd0\x9d\xd0\xa2\xd0\x98\xd0\xa0\xd0\x9e\xd0\x92\xd0\x90\xd0\xa2\xd0\xac \xd0\x94\xd0\x98\xd0\xa1\xd0\x9a"));
    reset_color();
    printf("\n");

    std::string status = send_command("STATUS");
    if (status.empty() || status.find("No active") != std::string::npos) {
        set_color(CLR_GRAY);
        printf("  %s\n", tr(
            "Nothing is mounted.",
            "Nada esta montado.",
            "No hay nada montado.",
            "Nichts ist gemountet.",
            "Rien n'est monte.",
            "\xe6\xb2\xa1\xe6\x9c\x89\xe6\x8c\x82\xe8\xbd\xbd\xe4\xbb\xbb\xe4\xbd\x95\xe5\x86\x85\xe5\xae\xb9\xe3\x80\x82",
            "\xe4\xbd\x95\xe3\x82\x82\xe3\x83\x9e\xe3\x82\xa6\xe3\x83\xb3\xe3\x83\x88\xe3\x81\x95\xe3\x82\x8c\xe3\x81\xa6\xe3\x81\x84\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x80\x82",
        "\xd0\x9d\xd0\xb8\xd1\x87\xd0\xb5\xd0\xb3\xd0\xbe \xd0\xbd\xd0\xb5 \xd1\x81\xd0\xbc\xd0\xbe\xd0\xbd\xd1\x82\xd0\xb8\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\xd0\xbd\xd0\xbe."));
        reset_color();
        return;
    }

    set_color(CLR_GRAY);
    printf("  %s\n", status.c_str());
    reset_color();
    printf("\n");

    set_color(CLR_GRAY);
    printf("  %s\n", tr(
        "Which drive letter to unmount? (e.g. Z)",
        "Qual letra desmontar? (ex: Z)",
        "Que letra desmontar? (ej: Z)",
        "Welchen Buchstaben unmounten? (z.B. Z)",
        "Quelle lettre demonter? (ex: Z)",
        "\xe8\xa6\x81\xe5\x8d\xb8\xe8\xbd\xbd\xe5\x93\xaa\xe4\xb8\xaa\xe9\xa9\xb1\xe5\x8a\xa8\xe5\x99\xa8? (\xe4\xbe\x8b: Z)",
        "\xe3\x81\xa9\xe3\x81\xae\xe3\x83\x89\xe3\x83\xa9\xe3\x82\xa4\xe3\x83\x96\xe3\x82\x92\xe3\x82\xa2\xe3\x83\xb3\xe3\x83\x9e\xe3\x82\xa6\xe3\x83\xb3\xe3\x83\x88? (\xe4\xbe\x8b: Z)",
        "\xd0\x9a\xd0\xb0\xd0\xba\xd0\xbe\xd0\xb9 \xd0\xb4\xd0\xb8\xd1\x81\xd0\xba \xd1\x80\xd0\xb0\xd0\xb7\xd0\xbc\xd0\xbe\xd0\xbd\xd1\x82\xd0\xb8\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\xd1\x82\xd1\x8c? (\xd0\xbd\xd0\xb0\xd0\xbf\xd1\x80. Z)"));
    reset_color();
    print_prompt();

    char input[64] = {};
    if (!read_line(input, sizeof(input)) || input[0] == '\0') return;
    char letter = input[0];
    if (letter >= 'a' && letter <= 'z') letter -= 32;
    if (letter < 'A' || letter > 'Z') {
        set_color(CLR_RED);
        printf("\n  %s\n", tr(
            "Invalid letter.", "Letra invalida.",
            "Letra invalida.", "Ungultiger Buchstabe.",
            "Lettre invalide.",
            "\xe6\x97\xa0\xe6\x95\x88\xe5\xad\x97\xe6\xaf\x8d\xe3\x80\x82",
            "\xe7\x84\xa1\xe5\x8a\xb9\xe3\x81\xaa\xe6\x96\x87\xe5\xad\x97\xe3\x80\x82",
        "\xd0\x9d\xd0\xb5\xd0\xb2\xd0\xb5\xd1\x80\xd0\xbd\xd0\xb0\xd1\x8f \xd0\xb1\xd1\x83\xd0\xba\xd0\xb2\xd0\xb0."));
        reset_color();
        return;
    }

    std::string cmd = "UNMOUNT ";
    cmd += letter;
    std::string response = send_command(cmd);
    print_result(response);
}

static void do_help()
{
    printf("\n");
    print_divider();
    printf("\n");

    set_color(CLR_WHITE);
    printf("  %s\n", tr(
        "WHAT IS EXT4WINDOWS?",
        "O QUE E O EXT4WINDOWS?",
        "QUE ES EXT4WINDOWS?",
        "WAS IST EXT4WINDOWS?",
        "QU'EST-CE QUE EXT4WINDOWS?",
        "\xe4\xbb\x80\xe4\xb9\x88\xe6\x98\xaf" "EXT4WINDOWS?",
        "EXT4WINDOWS\xe3\x81\xa8\xe3\x81\xaf?"));
    reset_color();
    printf("\n");

    set_color(CLR_GRAY);
    printf("  %s\n", tr(
        "  Ext4Windows lets you access Linux disks and partitions",
        "  Ext4Windows permite acessar discos e particoes Linux",
        "  Ext4Windows le permite acceder a discos y particiones Linux",
        "  Ext4Windows ermoglicht den Zugriff auf Linux-Festplatten",
        "  Ext4Windows vous permet d'acceder aux disques Linux",
        "  Ext4Windows\xe8\xae\xa9\xe6\x82\xa8\xe5\x8f\xaf\xe4\xbb\xa5\xe8\xae\xbf\xe9\x97\xaeLinux\xe7\xa3\x81\xe7\x9b\x98\xe5\x92\x8c\xe5\x88\x86\xe5\x8c\xba",
        "  Ext4Windows\xe3\x81\xa7Linux\xe3\x83\x87\xe3\x82\xa3\xe3\x82\xb9\xe3\x82\xaf\xe3\x81\xa8\xe3\x83\x91\xe3\x83\xbc\xe3\x83\x86\xe3\x82\xa3\xe3\x82\xb7\xe3\x83\xa7\xe3\x83\xb3\xe3\x81\xab\xe3\x82\xa2\xe3\x82\xaf\xe3\x82\xbb\xe3\x82\xb9",
        "  Ext4Windows \xd0\xbf\xd0\xbe\xd0\xb7\xd0\xb2\xd0\xbe\xd0\xbb\xd1\x8f\xd0\xb5\xd1\x82 \xd0\xbf\xd0\xbe\xd0\xbb\xd1\x83\xd1\x87\xd0\xb8\xd1\x82\xd1\x8c \xd0\xb4\xd0\xbe\xd1\x81\xd1\x82\xd1\x83\xd0\xbf \xd0\xba \xd0\xb4\xd0\xb8\xd1\x81\xd0\xba\xd0\xb0\xd0\xbc Linux"));
    printf("  %s\n", tr(
        "  directly on Windows - no Linux installation needed.",
        "  direto no Windows - sem precisar instalar Linux.",
        "  directamente en Windows - sin necesidad de instalar Linux.",
        "  und Partitionen direkt unter Windows.",
        "  et partitions directement sous Windows.",
        "  \xe6\x97\xa0\xe9\x9c\x80\xe5\xae\x89\xe8\xa3\x85Linux\xe3\x80\x82",
        "  Linux\xe3\x81\xae\xe3\x82\xa4\xe3\x83\xb3\xe3\x82\xb9\xe3\x83\x88\xe3\x83\xbc\xe3\x83\xab\xe4\xb8\x8d\xe8\xa6\x81\xe3\x80\x82",
        "  \xd0\xbf\xd1\x80\xd1\x8f\xd0\xbc\xd0\xbe \xd0\xb8\xd0\xb7 Windows - \xd1\x83\xd1\x81\xd1\x82\xd0\xb0\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xba\xd0\xb0 Linux \xd0\xbd\xd0\xb5 \xd0\xbd\xd1\x83\xd0\xb6\xd0\xbd\xd0\xb0."));
    printf("\n");
    printf("  %s\n", tr(
        "  ext4 is the default filesystem of Linux (like NTFS is",
        "  ext4 e o sistema de arquivos padrao do Linux (assim como",
        "  ext4 es el sistema de archivos predeterminado de Linux (como",
        "  ext4 ist das Standard-Dateisystem von Linux (wie NTFS",
        "  ext4 est le systeme de fichiers par defaut de Linux (comme",
        "  ext4\xe6\x98\xafLinux\xe7\x9a\x84\xe9\xbb\x98\xe8\xae\xa4\xe6\x96\x87\xe4\xbb\xb6\xe7\xb3\xbb\xe7\xbb\x9f",
        "  ext4\xe3\x81\xafLinux\xe3\x81\xae\xe3\x83\x87\xe3\x83\x95\xe3\x82\xa9\xe3\x83\xab\xe3\x83\x88\xe3\x83\x95\xe3\x82\xa1\xe3\x82\xa4\xe3\x83\xab\xe3\x82\xb7\xe3\x82\xb9\xe3\x83\x86\xe3\x83\xa0\xe3\x81\xa7\xe3\x81\x99",
        "  ext4 - \xd1\x84\xd0\xb0\xd0\xb9\xd0\xbb\xd0\xbe\xd0\xb2\xd0\xb0\xd1\x8f \xd1\x81\xd0\xb8\xd1\x81\xd1\x82\xd0\xb5\xd0\xbc\xd0\xb0 Linux \xd0\xbf\xd0\xbe \xd1\x83\xd0\xbc\xd0\xbe\xd0\xbb\xd1\x87\xd0\xb0\xd0\xbd\xd0\xb8\xd1\x8e (\xd0\xba\xd0\xb0\xd0\xba NTFS"));
    printf("  %s\n", tr(
        "  for Windows). Windows can't read ext4 natively, but",
        "  NTFS e para Windows). Windows nao le ext4 nativamente,",
        "  NTFS es para Windows). Windows no lee ext4 nativamente,",
        "  fur Windows ist). Windows kann ext4 nicht nativ lesen,",
        "  NTFS pour Windows). Windows ne peut pas lire ext4,",
        "  (\xe5\xb0\xb1\xe5\x83\x8fNTFS\xe4\xb9\x8b\xe4\xba\x8e" "Windows)\xe3\x80\x82Windows\xe6\x97\xa0\xe6\xb3\x95\xe5\x8e\x9f\xe7\x94\x9f\xe8\xaf\xbb\xe5\x8f\x96" "ext4,",
        "  (Windows\xe3\x81\xab\xe3\x81\xa8\xe3\x81\xa3\xe3\x81\xa6\xe3\x81\xae" "NTFS\xe3\x81\xae\xe3\x82\x88\xe3\x81\x86\xe3\x81\xaa\xe3\x82\x82\xe3\x81\xae)\xe3\x80\x82Windows\xe3\x81\xaf\xe3\x83\x8d\xe3\x82\xa4\xe3\x83\x86\xe3\x82\xa3\xe3\x83\x96\xe3\x81\xa7" "ext4\xe3\x82\x92\xe8\xaa\xad\xe3\x82\x81\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x81\x8c,"));
    printf("  %s\n", tr(
        "  this tool makes it possible.",
        "  mas essa ferramenta torna isso possivel.",
        "  pero esta herramienta lo hace posible.",
        "  aber dieses Tool macht es moglich.",
        "  mais cet outil le rend possible.",
        "  \xe4\xbd\x86\xe6\xad\xa4\xe5\xb7\xa5\xe5\x85\xb7\xe5\x8f\xaf\xe4\xbb\xa5\xe5\xae\x9e\xe7\x8e\xb0\xe3\x80\x82",
        "  \xe3\x81\x93\xe3\x81\xae\xe3\x83\x84\xe3\x83\xbc\xe3\x83\xab\xe3\x81\x8c\xe3\x81\x9d\xe3\x82\x8c\xe3\x82\x92\xe5\x8f\xaf\xe8\x83\xbd\xe3\x81\xab\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x99\xe3\x80\x82",
        "  \xd1\x8d\xd1\x82\xd0\xbe\xd1\x82 \xd0\xb8\xd0\xbd\xd1\x81\xd1\x82\xd1\x80\xd1\x83\xd0\xbc\xd0\xb5\xd0\xbd\xd1\x82 \xd0\xb4\xd0\xb5\xd0\xbb\xd0\xb0\xd0\xb5\xd1\x82 \xd1\x8d\xd1\x82\xd0\xbe \xd0\xb2\xd0\xbe\xd0\xb7\xd0\xbc\xd0\xbe\xd0\xb6\xd0\xbd\xd1\x8b\xd0\xbc."));
    printf("\n");

    set_color(CLR_WHITE);
    printf("  %s\n", tr(
        "HOW TO USE:", "COMO USAR:", "COMO USAR:",
        "ANLEITUNG:", "COMMENT UTILISER:",
        "\xe4\xbd\xbf\xe7\x94\xa8\xe6\x96\xb9\xe6\xb3\x95:",
        "\xe4\xbd\xbf\xe3\x81\x84\xe6\x96\xb9:",
        "\xd0\x9a\xd0\x90\xd0\x9a \xd0\x98\xd0\xa1\xd0\x9f\xd0\x9e\xd0\x9b\xd0\xac\xd0\x97\xd0\x9e\xd0\x92\xd0\x90\xd0\xa2\xd0\xac:"));
    reset_color();
    printf("\n");
    set_color(CLR_GRAY);
    printf("  %s\n", tr(
        "  1. Choose [1] to mount an image, or [2] to scan partitions.",
        "  1. Escolha [1] para montar imagem, ou [2] para buscar particoes.",
        "  1. Elija [1] para montar imagen, o [2] para buscar particiones.",
        "  1. Wahlen Sie [1] fur Image, oder [2] fur Partitionen.",
        "  1. Choisissez [1] pour une image, ou [2] pour les partitions.",
        "  1. \xe9\x80\x89\xe6\x8b\xa9[1]\xe6\x8c\x82\xe8\xbd\xbd\xe9\x95\x9c\xe5\x83\x8f\xef\xbc\x8c\xe6\x88\x96[2]\xe6\x89\xab\xe6\x8f\x8f\xe5\x88\x86\xe5\x8c\xba\xe3\x80\x82",
        "  1. [1]\xe3\x81\xa7\xe3\x82\xa4\xe3\x83\xa1\xe3\x83\xbc\xe3\x82\xb8\xe3\x80\x81[2]\xe3\x81\xa7\xe3\x83\x91\xe3\x83\xbc\xe3\x83\x86\xe3\x82\xa3\xe3\x82\xb7\xe3\x83\xa7\xe3\x83\xb3\xe3\x82\xb9\xe3\x82\xad\xe3\x83\xa3\xe3\x83\xb3\xe3\x80\x82",
        "  1. \xd0\x92\xd1\x8b\xd0\xb1\xd0\xb5\xd1\x80\xd0\xb8\xd1\x82\xd0\xb5 [1] \xd0\xb4\xd0\xbb\xd1\x8f \xd0\xbe\xd0\xb1\xd1\x80\xd0\xb0\xd0\xb7\xd0\xb0 \xd0\xb8\xd0\xbb\xd0\xb8 [2] \xd0\xb4\xd0\xbb\xd1\x8f \xd0\xbf\xd0\xbe\xd0\xb8\xd1\x81\xd0\xba\xd0\xb0 \xd1\x80\xd0\xb0\xd0\xb7\xd0\xb4\xd0\xb5\xd0\xbb\xd0\xbe\xd0\xb2."));
    printf("  %s\n", tr(
        "  2. Pick a drive letter (or auto-select).",
        "  2. Escolha uma letra (ou auto).",
        "  2. Elija una letra (o automatico).",
        "  2. Laufwerksbuchstabe wahlen (oder automatisch).",
        "  2. Choisissez une lettre (ou automatique).",
        "  2. \xe9\x80\x89\xe6\x8b\xa9\xe9\xa9\xb1\xe5\x8a\xa8\xe5\x99\xa8\xe5\x8f\xb7(\xe6\x88\x96\xe8\x87\xaa\xe5\x8a\xa8)\xe3\x80\x82",
        "  2. \xe3\x83\x89\xe3\x83\xa9\xe3\x82\xa4\xe3\x83\x96\xe6\x96\x87\xe5\xad\x97\xe3\x82\x92\xe9\x81\xb8\xe6\x8a\x9e(\xe3\x81\xbe\xe3\x81\x9f\xe3\x81\xaf\xe8\x87\xaa\xe5\x8b\x95)\xe3\x80\x82",
        "  2. \xd0\x92\xd1\x8b\xd0\xb1\xd0\xb5\xd1\x80\xd0\xb8\xd1\x82\xd0\xb5 \xd0\xb1\xd1\x83\xd0\xba\xd0\xb2\xd1\x83 \xd0\xb4\xd0\xb8\xd1\x81\xd0\xba\xd0\xb0 (\xd0\xb8\xd0\xbb\xd0\xb8 \xd0\xb0\xd0\xb2\xd1\x82\xd0\xbe)."));
    printf("  %s\n", tr(
        "  3. Your Linux files appear in File Explorer!",
        "  3. Seus arquivos Linux aparecem no Explorador!",
        "  3. Sus archivos Linux aparecen en el Explorador!",
        "  3. Ihre Linux-Dateien erscheinen im Explorer!",
        "  3. Vos fichiers Linux apparaissent dans l'Explorateur!",
        "  3. \xe6\x82\xa8\xe7\x9a\x84Linux\xe6\x96\x87\xe4\xbb\xb6\xe5\x87\xba\xe7\x8e\xb0\xe5\x9c\xa8\xe8\xb5\x84\xe6\xba\x90\xe7\xae\xa1\xe7\x90\x86\xe5\x99\xa8\xe4\xb8\xad!",
        "  3. Linux\xe3\x83\x95\xe3\x82\xa1\xe3\x82\xa4\xe3\x83\xab\xe3\x81\x8c\xe3\x82\xa8\xe3\x82\xaf\xe3\x82\xb9\xe3\x83\x97\xe3\x83\xad\xe3\x83\xbc\xe3\x83\xa9\xe3\x83\xbc\xe3\x81\xab\xe8\xa1\xa8\xe7\xa4\xba!",
        "  3. \xd0\x92\xd0\xb0\xd1\x88\xd0\xb8 \xd1\x84\xd0\xb0\xd0\xb9\xd0\xbb\xd1\x8b Linux \xd0\xbf\xd0\xbe\xd1\x8f\xd0\xb2\xd1\x8f\xd1\x82\xd1\x81\xd1\x8f \xd0\xb2 \xd0\x9f\xd1\x80\xd0\xbe\xd0\xb2\xd0\xbe\xd0\xb4\xd0\xbd\xd0\xb8\xd0\xba\xd0\xb5!"));
    printf("\n");

    set_color(CLR_WHITE);
    printf("  %s\n", tr("TIPS:", "DICAS:", "CONSEJOS:", "TIPPS:", "ASTUCES:",
        "\xe6\x8f\x90\xe7\xa4\xba:", "\xe3\x83\x92\xe3\x83\xb3\xe3\x83\x88:",
        "\xd0\xa1\xd0\x9e\xd0\x92\xd0\x95\xd0\xa2\xd0\xab:"));
    reset_color();
    printf("\n");
    set_color(CLR_GRAY);
    printf("  %s\n", tr(
        "  - You can drag and drop an .img file into this window.",
        "  - Voce pode arrastar e soltar um arquivo .img nesta janela.",
        "  - Puede arrastrar y soltar un archivo .img en esta ventana.",
        "  - Sie konnen eine .img-Datei in dieses Fenster ziehen.",
        "  - Vous pouvez glisser-deposer un fichier .img dans cette fenetre.",
        "  - \xe6\x82\xa8\xe5\x8f\xaf\xe4\xbb\xa5\xe5\xb0\x86.img\xe6\x96\x87\xe4\xbb\xb6\xe6\x8b\x96\xe6\x94\xbe\xe5\x88\xb0\xe6\xad\xa4\xe7\xaa\x97\xe5\x8f\xa3\xe3\x80\x82",
        "  - .img\xe3\x83\x95\xe3\x82\xa1\xe3\x82\xa4\xe3\x83\xab\xe3\x82\x92\xe3\x81\x93\xe3\x81\xae\xe3\x82\xa6\xe3\x82\xa3\xe3\x83\xb3\xe3\x83\x89\xe3\x82\xa6\xe3\x81\xab\xe3\x83\x89\xe3\x83\xa9\xe3\x83\x83\xe3\x82\xb0&\xe3\x83\x89\xe3\x83\xad\xe3\x83\x83\xe3\x83\x97\xe3\x81\xa7\xe3\x81\x8d\xe3\x81\xbe\xe3\x81\x99\xe3\x80\x82",
        "  - \xd0\x92\xd1\x8b \xd0\xbc\xd0\xbe\xd0\xb6\xd0\xb5\xd1\x82\xd0\xb5 \xd0\xbf\xd0\xb5\xd1\x80\xd0\xb5\xd1\x82\xd0\xb0\xd1\x89\xd0\xb8\xd1\x82\xd1\x8c .img \xd1\x84\xd0\xb0\xd0\xb9\xd0\xbb \xd0\xb2 \xd1\x8d\xd1\x82\xd0\xbe \xd0\xbe\xd0\xba\xd0\xbd\xd0\xbe."));
    printf("  %s\n", tr(
        "  - Mounts persist even if you close this window (server stays).",
        "  - Mounts persistem mesmo fechando esta janela (servidor continua).",
        "  - Los montajes persisten al cerrar esta ventana (servidor sigue).",
        "  - Mounts bleiben erhalten wenn Sie dieses Fenster schliessen.",
        "  - Les montages persistent meme si vous fermez cette fenetre.",
        "  - \xe5\x85\xb3\xe9\x97\xad\xe6\xad\xa4\xe7\xaa\x97\xe5\x8f\xa3\xe5\x90\x8e\xe6\x8c\x82\xe8\xbd\xbd\xe4\xbb\x8d\xe7\x84\xb6\xe6\x9c\x89\xe6\x95\x88(\xe6\x9c\x8d\xe5\x8a\xa1\xe5\x99\xa8\xe7\xbb\xa7\xe7\xbb\xad\xe8\xbf\x90\xe8\xa1\x8c)\xe3\x80\x82",
        "  - \xe3\x82\xa6\xe3\x82\xa3\xe3\x83\xb3\xe3\x83\x89\xe3\x82\xa6\xe3\x82\x92\xe9\x96\x89\xe3\x81\x98\xe3\x81\xa6\xe3\x82\x82\xe3\x83\x9e\xe3\x82\xa6\xe3\x83\xb3\xe3\x83\x88\xe3\x81\xaf\xe7\xb6\xad\xe6\x8c\x81\xe3\x81\x95\xe3\x82\x8c\xe3\x81\xbe\xe3\x81\x99\xe3\x80\x82",
        "  - \xd0\x9c\xd0\xbe\xd0\xbd\xd1\x82\xd0\xb8\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\xd0\xbd\xd0\xb8\xd1\x8f \xd1\x81\xd0\xbe\xd1\x85\xd1\x80\xd0\xb0\xd0\xbd\xd1\x8f\xd1\x8e\xd1\x82\xd1\x81\xd1\x8f \xd0\xbf\xd1\x80\xd0\xb8 \xd0\xb7\xd0\xb0\xd0\xba\xd1\x80\xd1\x8b\xd1\x82\xd0\xb8\xd0\xb8 \xd0\xbe\xd0\xba\xd0\xbd\xd0\xb0."));
    printf("  %s\n", tr(
        "  - Use the tray icon (notification area) to manage mounts.",
        "  - Use o icone na bandeja (area de notificacao) para gerenciar.",
        "  - Use el icono en la bandeja para administrar montajes.",
        "  - Verwenden Sie das Tray-Symbol zur Verwaltung.",
        "  - Utilisez l'icone de la barre pour gerer les montages.",
        "  - \xe4\xbd\xbf\xe7\x94\xa8\xe6\x89\x98\xe7\x9b\x98\xe5\x9b\xbe\xe6\xa0\x87(\xe9\x80\x9a\xe7\x9f\xa5\xe5\x8c\xba\xe5\x9f\x9f)\xe7\xae\xa1\xe7\x90\x86\xe6\x8c\x82\xe8\xbd\xbd\xe3\x80\x82",
        "  - \xe3\x83\x88\xe3\x83\xac\xe3\x82\xa4\xe3\x82\xa2\xe3\x82\xa4\xe3\x82\xb3\xe3\x83\xb3\xe3\x81\xa7\xe3\x83\x9e\xe3\x82\xa6\xe3\x83\xb3\xe3\x83\x88\xe3\x82\x92\xe7\xae\xa1\xe7\x90\x86\xe3\x81\xa7\xe3\x81\x8d\xe3\x81\xbe\xe3\x81\x99\xe3\x80\x82",
        "  - \xd0\x98\xd1\x81\xd0\xbf\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xb7\xd1\x83\xd0\xb9\xd1\x82\xd0\xb5 \xd0\xb7\xd0\xbd\xd0\xb0\xd1\x87\xd0\xbe\xd0\xba \xd0\xb2 \xd1\x82\xd1\x80\xd0\xb5\xd0\xb5 \xd0\xb4\xd0\xbb\xd1\x8f \xd1\x83\xd0\xbf\xd1\x80\xd0\xb0\xd0\xb2\xd0\xbb\xd0\xb5\xd0\xbd\xd0\xb8\xd1\x8f."));
    reset_color();
    printf("\n");
    print_divider();
}

// ═══════════════════════════════════════════════════════════
// Settings
// ═══════════════════════════════════════════════════════════

static void do_settings()
{
    const char* lang_names[] = {
        "English", "Portugues", "Espanol", "Deutsch",
        "Francais",
        "\xe4\xb8\xad\xe6\x96\x87",
        "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e",
        "\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9"
    };

    while (true) {
        printf("\n");
        print_divider();
        printf("\n");

        set_color(CLR_WHITE);
        printf("   %s\n", tr(
            "SETTINGS", "CONFIGURACOES", "CONFIGURACION",
            "EINSTELLUNGEN", "PARAMETRES",
            "\xe8\xae\xbe\xe7\xbd\xae",
            "\xe8\xa8\xad\xe5\xae\x9a",
            "\xd0\x9d\xd0\x90\xd0\xa1\xd0\xa2\xd0\xa0\xd0\x9e\xd0\x99\xd0\x9a\xd0\x98"));
        reset_color();
        printf("\n");

        // [1] Language
        set_color(CLR_ORANGE); printf("    [1] "); set_color(CLR_WHITE);
        printf("%s: ", tr("Language", "Idioma", "Idioma",
            "Sprache", "Langue",
            "\xe8\xaf\xad\xe8\xa8\x80", "\xe8\xa8\x80\xe8\xaa\x9e",
            "\xd0\xaf\xd0\xb7\xd1\x8b\xd0\xba"));
        set_color(CLR_GREEN);
        printf("%s\n", lang_names[g_lang]);
        reset_color();

        // [2] Default mode
        set_color(CLR_ORANGE); printf("    [2] "); set_color(CLR_WHITE);
        printf("%s: ", tr("Default mode", "Modo padrao", "Modo predeterminado",
            "Standardmodus", "Mode par defaut",
            "\xe9\xbb\x98\xe8\xae\xa4\xe6\xa8\xa1\xe5\xbc\x8f",
            "\xe3\x83\x87\xe3\x83\x95\xe3\x82\xa9\xe3\x83\xab\xe3\x83\x88\xe3\x83\xa2\xe3\x83\xbc\xe3\x83\x89",
            "\xd0\xa0\xd0\xb5\xd0\xb6\xd0\xb8\xd0\xbc \xd0\xbf\xd0\xbe \xd1\x83\xd0\xbc\xd0\xbe\xd0\xbb\xd1\x87\xd0\xb0\xd0\xbd\xd0\xb8\xd1\x8e"));
        set_color(CLR_GREEN);
        printf("%s\n", g_default_mode
            ? tr("Read-write", "Leitura e escrita",
                 "Lectura y escritura", "Lesen/Schreiben",
                 "Lecture/ecriture",
                 "\xe8\xaf\xbb\xe5\x86\x99",
                 "\xe8\xaa\xad\xe3\x81\xbf\xe6\x9b\xb8\xe3\x81\x8d",
                 "\xd0\xa7\xd1\x82\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5/\xd0\xb7\xd0\xb0\xd0\xbf\xd0\xb8\xd1\x81\xd1\x8c")
            : tr("Read-only", "Somente leitura",
                 "Solo lectura", "Nur lesen",
                 "Lecture seule",
                 "\xe5\x8f\xaa\xe8\xaf\xbb",
                 "\xe8\xaa\xad\xe3\x81\xbf\xe5\x8f\x96\xe3\x82\x8a\xe5\xb0\x82\xe7\x94\xa8",
                 "\xd0\xa2\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xba\xd0\xbe \xd1\x87\xd1\x82\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5"));
        reset_color();

        // [3] Auto-start
        set_color(CLR_ORANGE); printf("    [3] "); set_color(CLR_WHITE);
        printf("%s: ", tr("Start on login", "Iniciar no login",
            "Iniciar al login", "Beim Login starten",
            "Demarrer a la connexion",
            "\xe7\x99\xbb\xe5\xbd\x95\xe6\x97\xb6\xe5\x90\xaf\xe5\x8a\xa8",
            "\xe3\x83\xad\xe3\x82\xb0\xe3\x82\xa4\xe3\x83\xb3\xe6\x99\x82\xe3\x81\xab\xe8\xb5\xb7\xe5\x8b\x95",
            "\xd0\x97\xd0\xb0\xd0\xbf\xd1\x83\xd1\x81\xd0\xba \xd0\xbf\xd1\x80\xd0\xb8 \xd0\xb2\xd1\x85\xd0\xbe\xd0\xb4\xd0\xb5"));
        set_color(is_autostart_enabled() ? CLR_GREEN : CLR_RED);
        printf("%s\n", is_autostart_enabled()
            ? tr("ON", "LIGADO", "ACTIVADO", "AN", "ACTIVE",
                 "\xe5\xbc\x80", "\xe3\x82\xaa\xe3\x83\xb3",
                 "\xd0\x92\xd0\x9a\xd0\x9b")
            : tr("OFF", "DESLIGADO", "DESACTIVADO", "AUS", "DESACTIVE",
                 "\xe5\x85\xb3", "\xe3\x82\xaa\xe3\x83\x95",
                 "\xd0\x92\xd0\xab\xd0\x9a\xd0\x9b"));
        reset_color();

        // [4] Debug
        set_color(CLR_ORANGE); printf("    [4] "); set_color(CLR_WHITE);
        printf("Debug: ");
        set_color(g_debug ? CLR_GREEN : CLR_RED);
        printf("%s\n", g_debug
            ? tr("ON", "LIGADO", "ACTIVADO", "AN", "ACTIVE",
                 "\xe5\xbc\x80", "\xe3\x82\xaa\xe3\x83\xb3",
                 "\xd0\x92\xd0\x9a\xd0\x9b")
            : tr("OFF", "DESLIGADO", "DESACTIVADO", "AUS", "DESACTIVE",
                 "\xe5\x85\xb3", "\xe3\x82\xaa\xe3\x83\x95",
                 "\xd0\x92\xd0\xab\xd0\x9a\xd0\x9b"));
        reset_color();

        printf("\n");
        set_color(CLR_ORANGE); printf("    [0] "); set_color(CLR_WHITE);
        printf("%s\n", tr("Back", "Voltar", "Volver", "Zuruck", "Retour",
            "\xe8\xbf\x94\xe5\x9b\x9e", "\xe6\x88\xbb\xe3\x82\x8b",
            "\xd0\x9d\xd0\xb0\xd0\xb7\xd0\xb0\xd0\xb4"));

        printf("\n");
        print_divider();
        printf("\n");
        print_prompt();

        char input[64] = {};
        if (!read_line(input, sizeof(input))) return;

        switch (input[0]) {
            case '1': {
                // Change language
                const char* langs[] = {
                    "English", "Portugues", "Espanol", "Deutsch",
                    "Francais",
                    "\xe4\xb8\xad\xe6\x96\x87",
                    "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e",
                    "\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9"
                };
                printf("\n");
                for (int i = 0; i < 8; i++) {
                    set_color(CLR_ORANGE);
                    printf("    [%d] ", i + 1);
                    set_color(g_lang == i ? CLR_GREEN : CLR_WHITE);
                    printf("%s%s\n", langs[i],
                           g_lang == i ? " *" : "");
                }
                reset_color();
                print_prompt();
                char lang_input[64] = {};
                if (read_line(lang_input, sizeof(lang_input))) {
                    int choice = atoi(lang_input);
                    if (choice >= 1 && choice <= 8) {
                        g_lang = choice - 1;
                        save_config();
                    }
                }
                break;
            }
            case '2':
                g_default_mode = g_default_mode ? 0 : 1;
                save_config();
                break;
            case '3':
                toggle_autostart();
                break;
            case '4':
                g_debug = !g_debug;
                save_config();
                break;
            case '0':
            case 'b':
            case 'B':
                return;
            default:
                break;
        }
    }
}

// ═══════════════════════════════════════════════════════════
// Language Selection & Main Loop
// ═══════════════════════════════════════════════════════════

static int ask_language()
{
    printf("\n");
    set_color(CLR_WHITE);
    printf("  Select language:\n");
    reset_color();
    printf("\n");

    const char* langs[] = {
        "English", "Portugues", "Espanol",
        "Deutsch", "Francais",
        "\xe4\xb8\xad\xe6\x96\x87",
        "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e",
        "\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9"
    };

    for (int i = 0; i < 8; i++) {
        set_color(CLR_ORANGE);
        printf("    [%d] ", i + 1);
        set_color(CLR_WHITE);
        printf("%s\n", langs[i]);
    }

    reset_color();
    printf("\n");
    print_prompt();

    char input[64] = {};
    if (!read_line(input, sizeof(input))) return 0;
    int choice = atoi(input);
    if (choice >= 1 && choice <= 8) return choice - 1;
    return 0;  // default English
}

int interactive_main()
{
    init_console();
    clear_screen();
    print_banner();

    // Load saved settings (language, default mode, debug).
    // If a config file exists, skip the language selection screen.
    load_config();
    std::string cfg = get_config_path();
    FILE* cfg_test = fopen(cfg.c_str(), "r");
    if (cfg_test) {
        fclose(cfg_test);
        // Config exists — use saved language
    } else {
        g_lang = ask_language();
        save_config();
    }

    ensure_server();

    // Background thread: monitors the server every 2 seconds.
    // If the server dies externally (e.g. user clicked Quit in tray),
    // this thread terminates the entire process so the terminal closes.
    std::thread server_monitor([]() {
        // Give the server a moment to fully start
        Sleep(3000);
        while (true) {
            Sleep(2000);
            if (!is_server_running()) {
                // Server died — exit the process
                ExitProcess(0);
            }
        }
    });
    server_monitor.detach();

    while (true) {

        print_main_menu();
        print_prompt();

        char input[256] = {};
        if (!read_line(input, sizeof(input))) break;

        switch (input[0]) {
            case '1': do_mount_image(); break;
            case '2': do_scan_partitions(); break;
            case '3': do_status(); break;
            case '4': do_unmount(); break;
            case '5': do_settings(); break;
            case '6': do_help(); break;
            case '7':
            case 'q':
            case 'Q': {
                printf("\n");
                set_color(CLR_GRAY);
                printf("  %s\n", tr(
                    "Close this window only? Active mounts stay via tray icon.",
                    "Fechar so esta janela? Mounts ativos continuam no tray.",
                    "Cerrar solo esta ventana? Los montajes siguen en la bandeja.",
                    "Nur dieses Fenster schliessen? Mounts bleiben im Tray.",
                    "Fermer cette fenetre? Les montages restent dans la barre.",
                    "\xe5\x8f\xaa\xe5\x85\xb3\xe9\x97\xad\xe6\xad\xa4\xe7\xaa\x97\xe5\x8f\xa3? \xe6\xb4\xbb\xe5\x8a\xa8\xe6\x8c\x82\xe8\xbd\xbd\xe7\xbb\xa7\xe7\xbb\xad\xe9\x80\x9a\xe8\xbf\x87\xe6\x89\x98\xe7\x9b\x98\xe5\x9b\xbe\xe6\xa0\x87\xe8\xbf\x90\xe8\xa1\x8c\xe3\x80\x82",
                    "\xe3\x81\x93\xe3\x81\xae\xe3\x82\xa6\xe3\x82\xa3\xe3\x83\xb3\xe3\x83\x89\xe3\x82\xa6\xe3\x81\xa0\xe3\x81\x91\xe9\x96\x89\xe3\x81\x98\xe3\x82\x8b? \xe3\x83\x9e\xe3\x82\xa6\xe3\x83\xb3\xe3\x83\x88\xe3\x81\xaf\xe3\x83\x88\xe3\x83\xac\xe3\x82\xa4\xe3\x81\xa7\xe7\xb6\xad\xe6\x8c\x81\xe3\x80\x82",
        "\xd0\x97\xd0\xb0\xd0\xba\xd1\x80\xd1\x8b\xd1\x82\xd1\x8c \xd1\x82\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xba\xd0\xbe \xd0\xbe\xd0\xba\xd0\xbd\xd0\xbe? \xd0\x9c\xd0\xbe\xd0\xbd\xd1\x82\xd0\xb8\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\xd0\xbd\xd0\xb8\xd1\x8f \xd0\xbe\xd1\x81\xd1\x82\xd0\xb0\xd1\x8e\xd1\x82\xd1\x81\xd1\x8f \xd1\x87\xd0\xb5\xd1\x80\xd0\xb5\xd0\xb7 \xd1\x82\xd1\x80\xd0\xb5\xd0\xb9."));
                reset_color();
                printf("\n");

                set_color(CLR_ORANGE); printf("    [1] "); set_color(CLR_WHITE);
                printf("%s\n", tr(
                    "Close window (mounts stay)",
                    "Fechar janela (mounts continuam)",
                    "Cerrar ventana (montajes continuan)",
                    "Fenster schliessen (Mounts bleiben)",
                    "Fermer fenetre (montages restent)",
                    "\xe5\x85\xb3\xe9\x97\xad\xe7\xaa\x97\xe5\x8f\xa3 (\xe6\x8c\x82\xe8\xbd\xbd\xe4\xbf\x9d\xe6\x8c\x81)",
                    "\xe3\x82\xa6\xe3\x82\xa3\xe3\x83\xb3\xe3\x83\x89\xe3\x82\xa6\xe3\x82\x92\xe9\x96\x89\xe3\x81\x98\xe3\x82\x8b (\xe3\x83\x9e\xe3\x82\xa6\xe3\x83\xb3\xe3\x83\x88\xe7\xb6\xad\xe6\x8c\x81)",
        "\xd0\x97\xd0\xb0\xd0\xba\xd1\x80\xd1\x8b\xd1\x82\xd1\x8c \xd0\xbe\xd0\xba\xd0\xbd\xd0\xbe (\xd0\xbc\xd0\xbe\xd0\xbd\xd1\x82\xd0\xb8\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\xd0\xbd\xd0\xb8\xd1\x8f \xd0\xbe\xd1\x81\xd1\x82\xd0\xb0\xd1\x8e\xd1\x82\xd1\x81\xd1\x8f)"));
                set_color(CLR_ORANGE); printf("    [2] "); set_color(CLR_WHITE);
                printf("%s\n", tr(
                    "Quit everything (unmount all)",
                    "Sair de tudo (desmontar tudo)",
                    "Salir de todo (desmontar todo)",
                    "Alles beenden (alles unmounten)",
                    "Tout quitter (tout demonter)",
                    "\xe5\x85\xa8\xe9\x83\xa8\xe9\x80\x80\xe5\x87\xba (\xe5\x8d\xb8\xe8\xbd\xbd\xe5\x85\xa8\xe9\x83\xa8)",
                    "\xe3\x81\x99\xe3\x81\xb9\xe3\x81\xa6\xe7\xb5\x82\xe4\xba\x86 (\xe5\x85\xa8\xe3\x81\xa6\xe3\x82\xa2\xe3\x83\xb3\xe3\x83\x9e\xe3\x82\xa6\xe3\x83\xb3\xe3\x83\x88)",
        "\xd0\x92\xd1\x8b\xd0\xb9\xd1\x82\xd0\xb8 \xd0\xbf\xd0\xbe\xd0\xbb\xd0\xbd\xd0\xbe\xd1\x81\xd1\x82\xd1\x8c\xd1\x8e (\xd1\x80\xd0\xb0\xd0\xb7\xd0\xbc\xd0\xbe\xd0\xbd\xd1\x82\xd0\xb8\xd1\x80\xd0\xbe\xd0\xb2\xd0\xb0\xd1\x82\xd1\x8c \xd0\xb2\xd1\x81\xd1\x91)"));
                reset_color();
                print_prompt();

                char quit_input[64] = {};
                read_line(quit_input, sizeof(quit_input));
                if (quit_input[0] == '2') {
                    send_command("QUIT");
                    set_color(CLR_GREEN);
                    printf("\n  %s\n", tr(
                        "Server stopped. Goodbye!",
                        "Servidor parado. Ate mais!",
                        "Servidor detenido. Hasta luego!",
                        "Server gestoppt. Auf Wiedersehen!",
                        "Serveur arrete. Au revoir!",
                        "\xe6\x9c\x8d\xe5\x8a\xa1\xe5\x99\xa8\xe5\xb7\xb2\xe5\x81\x9c\xe6\xad\xa2\xe3\x80\x82\xe5\x86\x8d\xe8\xa7\x81!",
                        "\xe3\x82\xb5\xe3\x83\xbc\xe3\x83\x90\xe3\x83\xbc\xe5\x81\x9c\xe6\xad\xa2\xe3\x80\x82\xe3\x81\x95\xe3\x82\x88\xe3\x81\x86\xe3\x81\xaa\xe3\x82\x89!",
        "\xd0\xa1\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80 \xd0\xbe\xd1\x81\xd1\x82\xd0\xb0\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xbb\xd0\xb5\xd0\xbd. \xd0\x94\xd0\xbe \xd1\x81\xd0\xb2\xd0\xb8\xd0\xb4\xd0\xb0\xd0\xbd\xd0\xb8\xd1\x8f!"));
                    reset_color();
                } else {
                    set_color(CLR_GREEN);
                    printf("\n  %s\n", tr(
                        "Window closed. Server continues in tray.",
                        "Janela fechada. Servidor continua no tray.",
                        "Ventana cerrada. Servidor continua en la bandeja.",
                        "Fenster geschlossen. Server lauft im Tray weiter.",
                        "Fenetre fermee. Serveur continue dans la barre.",
                        "\xe7\xaa\x97\xe5\x8f\xa3\xe5\xb7\xb2\xe5\x85\xb3\xe9\x97\xad\xe3\x80\x82\xe6\x9c\x8d\xe5\x8a\xa1\xe5\x99\xa8\xe7\xbb\xa7\xe7\xbb\xad\xe5\x9c\xa8\xe6\x89\x98\xe7\x9b\x98\xe8\xbf\x90\xe8\xa1\x8c\xe3\x80\x82",
                        "\xe3\x82\xa6\xe3\x82\xa3\xe3\x83\xb3\xe3\x83\x89\xe3\x82\xa6\xe3\x82\x92\xe9\x96\x89\xe3\x81\x98\xe3\x81\xbe\xe3\x81\x97\xe3\x81\x9f\xe3\x80\x82\xe3\x82\xb5\xe3\x83\xbc\xe3\x83\x90\xe3\x83\xbc\xe3\x81\xaf\xe3\x83\x88\xe3\x83\xac\xe3\x82\xa4\xe3\x81\xa7\xe7\xb6\x99\xe7\xb6\x9a\xe3\x80\x82",
        "\xd0\x9e\xd0\xba\xd0\xbd\xd0\xbe \xd0\xb7\xd0\xb0\xd0\xba\xd1\x80\xd1\x8b\xd1\x82\xd0\xbe. \xd0\xa1\xd0\xb5\xd1\x80\xd0\xb2\xd0\xb5\xd1\x80 \xd1\x80\xd0\xb0\xd0\xb1\xd0\xbe\xd1\x82\xd0\xb0\xd0\xb5\xd1\x82 \xd0\xb2 \xd1\x82\xd1\x80\xd0\xb5\xd0\xb5."));
                    reset_color();
                }
                return 0;
            }

            default:
                set_color(CLR_RED);
                printf("\n  %s\n", tr(
                    "Invalid option. Choose 1-6.",
                    "Opcao invalida. Escolha 1-6.",
                    "Opcion invalida. Elija 1-6.",
                    "Ungultige Option. Wahlen Sie 1-6.",
                    "Option invalide. Choisissez 1-6.",
                    "\xe6\x97\xa0\xe6\x95\x88\xe9\x80\x89\xe9\xa1\xb9\xe3\x80\x82\xe8\xaf\xb7\xe9\x80\x89\xe6\x8b\xa9 1-6\xe3\x80\x82",
                    "\xe7\x84\xa1\xe5\x8a\xb9\xe3\x81\xaa\xe3\x82\xaa\xe3\x83\x97\xe3\x82\xb7\xe3\x83\xa7\xe3\x83\xb3\xe3\x80\x82" "1-6\xe3\x82\x92\xe9\x81\xb8\xe6\x8a\x9e\xe3\x80\x82"));
                reset_color();
                break;
        }

        printf("\n");
        set_color(CLR_GRAY);
        printf("  %s", tr(
            "Press Enter to continue...",
            "Pressione Enter para continuar...",
            "Presione Enter para continuar...",
            "Enter drucken zum Fortfahren...",
            "Appuyez sur Entree pour continuer...",
            "\xe6\x8c\x89" "Enter\xe7\xbb\xa7\xe7\xbb\xad...",
            "Enter\xe3\x82\xad\xe3\x83\xbc\xe3\x82\x92\xe6\x8a\xbc\xe3\x81\x97\xe3\x81\xa6\xe7\xb6\x9a\xe8\xa1\x8c..."));
        reset_color();
        char dummy[64] = {};
        read_line(dummy, sizeof(dummy));
    }

    return 0;
}
