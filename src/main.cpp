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

#include <windows.h>
#include <shellapi.h>
#include <winfsp/winfsp.h>

#include "ext4_filesystem.hpp"
#include "blockdev_file.hpp"
#include "blockdev_partition.hpp"
#include "partition_scanner.hpp"
#include "debug_log.hpp"
#include "server.hpp"
#include "client.hpp"
#include "interactive.hpp"
#include "gui/gui_main.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Global event used to signal the main thread to unmount and exit.
// The Ctrl+C handler sets this event so WaitForSingleObject returns.
static HANDLE g_stop_event = nullptr;

static BOOL WINAPI console_ctrl_handler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        if (g_stop_event)
            SetEvent(g_stop_event);
        return TRUE;
    }
    return FALSE;
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
    printf("  Mount ext4 Linux drives on Windows\n");
    printf("  github.com/Mateuscruz19/Ext4Windows\n");
    printf("\n");
}

static void print_welcome()
{
    print_banner();
    printf("  HOW TO USE:\n");
    printf("  1. Choose an ext4 image file (.img)\n");
    printf("  2. Pick a drive letter (or let us pick one)\n");
    printf("  3. Access your Linux files in File Explorer!\n");
    printf("\n");
    printf("  TIP: You can drag and drop a file into this window.\n");
    printf("\n");
    printf("  --------------------------------------------------------\n");
    printf("\n");
}

static void print_usage()
{
    print_banner();
    printf("  USAGE:\n");
    printf("    ext4windows mount <path> [drive:] [--rw]   Mount via background server\n");
    printf("    ext4windows unmount <drive:>                Unmount a drive\n");
    printf("    ext4windows status                         Show active mounts\n");
    printf("    ext4windows quit                           Stop server and unmount all\n");
    printf("\n");
    printf("  LEGACY (blocking, no server):\n");
    printf("    ext4windows <image-file> [drive:] [--rw]   Mount and block terminal\n");
    printf("    ext4windows --scan [drive:]                Detect ext4 partitions\n");
    printf("\n");
    printf("  EXAMPLES:\n");
    printf("    ext4windows mount C:\\linux.img Z: --rw    Mount image (background)\n");
    printf("    ext4windows status                        Show what's mounted\n");
    printf("    ext4windows unmount Z:                    Unmount Z:\n");
    printf("    ext4windows C:\\linux.img Z:               Legacy blocking mode\n");
    printf("\n");
    printf("  OPTIONS:\n");
    printf("    --rw      Mount with read-write access (default: read-only)\n");
    printf("    --debug   Print detailed debug log to stderr\n");
    printf("    --scan    List ext4 partitions (legacy mode)\n");
    printf("\n");
    printf("  Run without arguments for interactive mode.\n");
    printf("\n");
}

// Find the first available drive letter, starting from Z: going down.
// Returns L'\0' if none found.
static wchar_t find_free_drive_letter()
{
    DWORD used = GetLogicalDrives();
    for (wchar_t letter = L'Z'; letter >= L'D'; letter--) {
        int bit = letter - L'A';
        if (!(used & (1 << bit)))
            return letter;
    }
    return L'\0';
}

// Check if a file exists at the given path.
static bool file_exists(const wchar_t* path)
{
    DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES &&
           !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// Read a line from stdin into a wide string buffer. Returns false on EOF.
static bool read_line(wchar_t* buf, int max_chars)
{
    if (!fgetws(buf, max_chars, stdin))
        return false;

    // Remove trailing newline
    size_t len = wcslen(buf);
    if (len > 0 && buf[len - 1] == L'\n')
        buf[len - 1] = L'\0';

    return true;
}

// Remove surrounding quotes from a path (drag-and-drop adds them).
static void strip_quotes(wchar_t* path)
{
    size_t len = wcslen(path);
    if (len >= 2 && path[0] == L'"' && path[len - 1] == L'"') {
        memmove(path, path + 1, (len - 2) * sizeof(wchar_t));
        path[len - 2] = L'\0';
    }
}

// --- Internationalization (i18n) ---
// 0 = English, 1 = Portuguese
static int g_lang = 0;

static const char* tr(const char* en, const char* pt)
{
    return g_lang == 1 ? pt : en;
}

static const wchar_t* tr(const wchar_t* en, const wchar_t* pt)
{
    return g_lang == 1 ? pt : en;
}

static int ask_language()
{
    printf("  Select language / Selecione o idioma:\n");
    printf("\n");
    printf("    [1] English\n");
    printf("    [2] Portugues\n");
    printf("\n");
    printf("  > ");
    fflush(stdout);

    wchar_t input[64] = {};
    if (!read_line(input, 64))
        return 0;

    if (input[0] == L'2')
        return 1;
    return 0;
}

static void print_help_text()
{
    printf("\n");
    printf("  --------------------------------------------------------\n");
    printf("\n");
    printf("  %s\n", tr(
        "WHAT IS THIS?",
        "O QUE E ISSO?"));
    printf("\n");
    printf("  %s\n", tr(
        "  Ext4Windows lets you read Linux disks and partitions",
        "  Ext4Windows permite ler discos e particoes Linux"));
    printf("  %s\n", tr(
        "  directly on Windows — no Linux installation needed.",
        "  direto no Windows — sem precisar instalar Linux."));
    printf("\n");
    printf("  %s\n", tr(
        "  ext4 is the default filesystem of Linux (like NTFS is",
        "  ext4 e o sistema de arquivos padrao do Linux (assim como"));
    printf("  %s\n", tr(
        "  for Windows). Windows can't read ext4 natively, but",
        "  NTFS e para Windows). Windows nao le ext4 nativamente,"));
    printf("  %s\n", tr(
        "  this tool makes it possible.",
        "  mas essa ferramenta torna isso possivel."));
    printf("\n");
    printf("  %s\n", tr(
        "  HOW IT WORKS:",
        "  COMO FUNCIONA:"));
    printf("  %s\n", tr(
        "  1. You provide an ext4 image file (.img)",
        "  1. Voce fornece um arquivo de imagem ext4 (.img)"));
    printf("  %s\n", tr(
        "  2. We mount it as a Windows drive (e.g. Z:)",
        "  2. Nos montamos como um drive Windows (ex: Z:)"));
    printf("  %s\n", tr(
        "  3. Browse your Linux files in File Explorer!",
        "  3. Navegue seus arquivos Linux no Explorador!"));
    printf("\n");
    printf("  %s\n", tr(
        "  Supports read-only and read-write modes.",
        "  Suporta modos somente leitura e leitura/escrita."));
    printf("\n");
    printf("  --------------------------------------------------------\n");
    printf("\n");
}

// Ask the user for the image path interactively.
static bool ask_image_path(wchar_t* out, int max_chars)
{
    printf("  %s\n", tr(
        "IMAGE FILE",
        "ARQUIVO DE IMAGEM"));
    printf("  %s\n", tr(
        "Enter the path to your ext4 image (.img):",
        "Digite o caminho do seu arquivo ext4 (.img):"));
    printf("  %s\n", tr(
        "(TIP: you can drag and drop the file here)",
        "(DICA: voce pode arrastar e soltar o arquivo aqui)"));
    printf("\n");
    printf("  > ");
    fflush(stdout);

    if (!read_line(out, max_chars))
        return false;

    strip_quotes(out);
    return true;
}

// Ask the user for the drive letter, or auto-pick one.
static bool ask_drive_letter(wchar_t* out, int max_chars)
{
    wchar_t free_letter = find_free_drive_letter();

    printf("\n");
    printf("  %s\n", tr(
        "DRIVE LETTER",
        "LETRA DO DRIVE"));

    if (free_letter) {
        printf("  %s (%c:):\n", tr(
            "Choose a letter or press Enter for auto",
            "Escolha uma letra ou pressione Enter para auto"),
            (char)free_letter);
    } else {
        printf("  %s:\n", tr(
            "Choose a drive letter (A-Z)",
            "Escolha uma letra de drive (A-Z)"));
    }
    printf("\n");
    printf("  > ");
    fflush(stdout);

    wchar_t input[64] = {};
    if (!read_line(input, 64))
        return false;

    if (input[0] == L'\0' && free_letter) {
        swprintf(out, max_chars, L"%c:", free_letter);
    } else if (input[0] != L'\0') {
        wchar_t letter = towupper(input[0]);
        if (letter < L'A' || letter > L'Z') {
            printf("\n  %s '%c'.\n", tr(
                "Error: invalid drive letter:",
                "Erro: letra de drive invalida:"),
                (char)input[0]);
            return false;
        }
        swprintf(out, max_chars, L"%c:", letter);
    } else {
        printf("\n  %s\n", tr(
            "Error: no free drive letters available.",
            "Erro: nenhuma letra de drive disponivel."));
        return false;
    }

    return true;
}

// Ask whether to mount read-only or read-write.
static bool ask_read_write()
{
    printf("\n");
    printf("  %s\n", tr(
        "ACCESS MODE",
        "MODO DE ACESSO"));
    printf("  %s\n", tr(
        "  [1] Read-only  (safe, cannot modify files)",
        "  [1] Somente leitura  (seguro, nao modifica arquivos)"));
    printf("  %s\n", tr(
        "  [2] Read-write (can create, edit and delete files)",
        "  [2] Leitura e escrita (pode criar, editar e deletar arquivos)"));
    printf("\n");
    printf("  > ");
    fflush(stdout);

    wchar_t input[64] = {};
    if (!read_line(input, 64))
        return true; // default: read-only
    if (input[0] == L'2')
        return false; // read-write
    return true; // read-only
}

// Wait for user to press Enter (keeps the window open after errors).
static void pause_before_exit()
{
    printf("\n%s", tr("Press Enter to exit...", "Pressione Enter para sair..."));
    fflush(stdout);
    getwchar();
}

// Shared function: once we have a blockdev, mount it and wait for exit.
// source_label is what we display (image path or partition name).
// destroy_fn is the cleanup function for the blockdev.
static int mount_and_wait(struct ext4_blockdev* bdev,
                          const wchar_t* mount_point,
                          const wchar_t* source_label,
                          bool interactive, bool read_only,
                          void (*destroy_fn)(struct ext4_blockdev*))
{
    // Mount the ext4 filesystem via WinFsp
    Ext4FileSystem fs;
    // Extract drive letter as instance ID for lwext4 unique naming.
    // mount_point is like "Z:" — take the first character.
    char instance_id = (mount_point && mount_point[0]) ?
        static_cast<char>(mount_point[0]) : 'A';
    NTSTATUS status = fs.Mount(bdev, mount_point, read_only, instance_id);
    if (!NT_SUCCESS(status)) {
        printf("\n\n");
        if (status == 0xC0000035) {
            wprintf(L"  %s %s\n", tr(
                L"Error: drive already in use:",
                L"Erro: drive ja esta em uso:"),
                mount_point);
            printf("  %s\n", tr(
                "Pick a different letter.",
                "Escolha outra letra."));
        } else if (status == 0xC0000022) {
            wprintf(L"  %s %s\n", tr(
                L"Error: access denied for drive:",
                L"Erro: acesso negado ao drive:"),
                mount_point);
            printf("  %s\n", tr(
                "Try a different letter (like Z: or Y:).",
                "Tente outra letra (como Z: ou Y:)."));
        } else {
            printf("  %s (0x%08lX)\n", tr(
                "Error: failed to mount",
                "Erro: falha ao montar"),
                status);
        }
        destroy_fn(bdev);
        if (interactive) pause_before_exit();
        return 1;
    }

    printf(" OK!\n");
    printf("\n");
    wprintf(L"  %s %s\\ (%s)\n",
        tr(L"Mounted at", L"Montado em"),
        mount_point,
        read_only
            ? tr(L"read-only", L"somente leitura")
            : tr(L"read-write", L"leitura e escrita"));
    printf("\n");

    if (interactive) {
        printf("  %s\n", tr(
            "Your Linux files are now in File Explorer!",
            "Seus arquivos Linux estao no Explorador de Arquivos!"));
        printf("  %s\n", tr(
            "Press Enter here to unmount and exit.",
            "Pressione Enter aqui para desmontar e sair."));
        printf("\n");

        g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

        HANDLE stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
        HANDLE handles[] = { stdin_handle, g_stop_event };
        WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    } else {
        printf("  %s\n", tr(
            "Press Ctrl+C to unmount.",
            "Pressione Ctrl+C para desmontar."));
        printf("\n");

        g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
        WaitForSingleObject(g_stop_event, INFINITE);
    }

    // Cleanup
    printf("  %s", tr("Unmounting...", "Desmontando..."));
    fflush(stdout);
    fs.Unmount();
    destroy_fn(bdev);
    printf(" OK!\n");

    if (interactive) {
        printf("\n");
        pause_before_exit();
    }

    return 0;
}

// Mount an .img file and run until Ctrl+C or the stop event is signaled.
static int run(const wchar_t* image_path, const wchar_t* mount_point,
               bool interactive, bool read_only)
{
    // Convert image path to UTF-8 for lwext4
    char image_path_utf8[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, image_path, -1,
                        image_path_utf8, sizeof(image_path_utf8),
                        nullptr, nullptr);

    // Validate the image file exists
    if (!file_exists(image_path)) {
        printf("\n  %s %s\n", tr(
            "Error: file not found:",
            "Erro: arquivo nao encontrado:"),
            image_path_utf8);
        if (interactive) pause_before_exit();
        return 1;
    }

    printf("\n");
    printf("  --------------------------------------------------------\n");
    printf("\n");
    printf("  %s %s\n", tr("Image:", "Imagem:"), image_path_utf8);
    wprintf(L"  %s %s\\\n", tr(L"Mount:", L"Drive:"), mount_point);
    printf("\n");

    // Create block device backed by the image file
    printf("  %s", tr("Mounting...", "Montando..."));
    fflush(stdout);

    struct ext4_blockdev* bdev = create_file_blockdev(image_path_utf8);
    if (!bdev) {
        printf("\n\n  %s\n", tr(
            "Error: could not open the image file.",
            "Erro: nao foi possivel abrir o arquivo de imagem."));
        printf("  %s\n", tr(
            "Is it a valid ext4 image?",
            "E uma imagem ext4 valida?"));
        if (interactive) pause_before_exit();
        return 1;
    }

    return mount_and_wait(bdev, mount_point, image_path, interactive,
                          read_only, destroy_file_blockdev);
}

// Forward declarations for functions defined later in this file
static bool is_elevated();
static HANDLE open_device_elevated(const wchar_t* device_path, bool read_only);

// Mount a real partition and run until Ctrl+C or the stop event is signaled.
static int run_partition(const wchar_t* device_path,
                         const wchar_t* display_name,
                         const wchar_t* mount_point,
                         bool interactive, bool read_only)
{
    printf("\n");
    printf("  --------------------------------------------------------\n");
    printf("\n");
    wprintf(L"  %s %s\n", tr(L"Partition:", L"Particao:"), display_name);
    wprintf(L"  %s %s\\\n", tr(L"Mount:", L"Drive:"), mount_point);
    if (read_only) {
        printf("  %s\n", tr("Mode: read-only (safe)",
                             "Modo: somente leitura (seguro)"));
    } else {
        printf("  %s\n", tr("Mode: read-write",
                             "Modo: leitura e escrita"));
    }
    printf("\n");

    printf("  %s", tr("Mounting...", "Montando..."));
    fflush(stdout);

    struct ext4_blockdev* bdev = nullptr;

    if (is_elevated()) {
        // Already admin: open directly
        bdev = create_partition_blockdev(device_path, read_only);
    } else {
        // Not admin: get handle via elevated subprocess
        HANDLE h = open_device_elevated(device_path, read_only);
        if (h != INVALID_HANDLE_VALUE) {
            bdev = create_partition_blockdev_from_handle(h, read_only);
        }
    }

    if (!bdev) {
        printf("\n\n  %s\n", tr(
            "Error: could not open the partition.",
            "Erro: nao foi possivel abrir a particao."));
        printf("  %s\n", tr(
            "Make sure you approved the Administrator prompt.",
            "Certifique-se de aprovar o prompt de Administrador."));
        if (interactive) pause_before_exit();
        return 1;
    }

    return mount_and_wait(bdev, mount_point, display_name, interactive,
                          read_only, destroy_partition_blockdev);
}

// Check if a path looks like a Windows device path (starts with \\ or //)
static bool is_device_path(const wchar_t* path)
{
    return path && wcslen(path) > 4 &&
           (wcsncmp(path, L"\\\\", 2) == 0 || wcsncmp(path, L"//", 2) == 0);
}

// Check if the current process is running as Administrator.
static bool is_elevated()
{
    BOOL elevated = FALSE;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elev = {};
        DWORD size = sizeof(elev);
        if (GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &size)) {
            elevated = elev.TokenIsElevated;
        }
        CloseHandle(token);
    }
    return elevated != FALSE;
}

// Launch an elevated subprocess to scan for partitions.
// The subprocess writes results to a temp file, which we read back.
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

    printf("  %s\n", tr(
        "Requesting Administrator access to scan disks...",
        "Solicitando acesso de Administrador para escanear discos..."));
    printf("\n");

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe_path;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
        DWORD err = GetLastError();
        if (err == ERROR_CANCELLED) {
            printf("  %s\n", tr(
                "Administrator access was denied.",
                "Acesso de Administrador foi negado."));
        }
        return results;
    }

    WaitForSingleObject(sei.hProcess, 30000);
    CloseHandle(sei.hProcess);

    FILE* f = _wfopen(temp_file.c_str(), L"r, ccs=UTF-8");
    if (!f)
        return results;

    wchar_t line[512];
    while (fgetws(line, 512, f)) {
        size_t len = wcslen(line);
        if (len > 0 && line[len - 1] == L'\n')
            line[len - 1] = L'\0';

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

// Launch an elevated subprocess to open a device handle and duplicate it
// into this (non-admin) process. Returns the duplicated HANDLE or
// INVALID_HANDLE_VALUE on failure.
static HANDLE open_device_elevated(const wchar_t* device_path, bool read_only)
{
    wchar_t temp_dir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp_dir);
    std::wstring temp_file = std::wstring(temp_dir) + L"ext4windows_handle.txt";
    DeleteFileW(temp_file.c_str());

    wchar_t exe_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);

    // Pass: --open-device <device_path> <parent_pid> <temp_file> [--rw]
    wchar_t pid_str[32];
    swprintf(pid_str, 32, L"%lu", GetCurrentProcessId());

    std::wstring args = L"--open-device \"";
    args += device_path;
    args += L"\" ";
    args += pid_str;
    args += L" \"";
    args += temp_file;
    args += L"\"";
    if (!read_only)
        args += L" --rw";

    printf("  %s\n", tr(
        "Requesting Administrator access to open partition...",
        "Solicitando acesso de Administrador para abrir particao..."));

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe_path;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
        printf("  %s\n", tr(
            "Administrator access was denied.",
            "Acesso de Administrador foi negado."));
        return INVALID_HANDLE_VALUE;
    }

    WaitForSingleObject(sei.hProcess, 15000);
    CloseHandle(sei.hProcess);

    // Read the duplicated handle value from the temp file
    FILE* f = _wfopen(temp_file.c_str(), L"r");
    if (!f)
        return INVALID_HANDLE_VALUE;

    unsigned long long handle_val = 0;
    if (fscanf(f, "%llu", &handle_val) != 1)
        handle_val = 0;
    fclose(f);
    DeleteFileW(temp_file.c_str());

    if (handle_val == 0)
        return INVALID_HANDLE_VALUE;

    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle_val));
}

// Subprocess: open device as admin, duplicate handle into parent process.
static int do_open_device(const wchar_t* device_path, DWORD parent_pid,
                          const wchar_t* output_path, bool read_only)
{
    DWORD access = read_only ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);

    HANDLE h = CreateFileW(device_path, access,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr);

    if (h == INVALID_HANDLE_VALUE)
        return 1;

    // Open the parent process so we can duplicate the handle into it
    HANDLE parent = OpenProcess(PROCESS_DUP_HANDLE, FALSE, parent_pid);
    if (!parent) {
        CloseHandle(h);
        return 1;
    }

    HANDLE dup_handle = INVALID_HANDLE_VALUE;
    BOOL ok = DuplicateHandle(
        GetCurrentProcess(), h,       // source
        parent, &dup_handle,           // target
        0, FALSE, DUPLICATE_SAME_ACCESS);

    CloseHandle(h);
    CloseHandle(parent);

    if (!ok)
        return 1;

    // Write the handle value (as seen by the parent process) to the file
    FILE* f = _wfopen(output_path, L"w");
    if (!f)
        return 1;

    fprintf(f, "%llu",
            static_cast<unsigned long long>(
                reinterpret_cast<uintptr_t>(dup_handle)));
    fclose(f);

    return 0;
}

// Save scan results to a file (used by --scan-save subprocess).
static int do_scan_save(const wchar_t* output_path)
{
    auto partitions = scan_ext4_partitions();

    FILE* f = _wfopen(output_path, L"w, ccs=UTF-8");
    if (!f)
        return 1;

    for (auto& p : partitions) {
        fwprintf(f, L"%s|%s|%llu|%d|%d\n",
                 p.device_path.c_str(),
                 p.display_name.c_str(),
                 p.size_bytes,
                 p.disk_number,
                 p.partition_number);
    }

    fclose(f);
    return 0;
}

// Scan for ext4 partitions and list them. Handles elevation automatically.
static int do_scan(const wchar_t* mount_point, bool interactive,
                   bool read_only)
{
    printf("\n");
    printf("  %s\n", tr(
        "Scanning for ext4 partitions...",
        "Buscando particoes ext4..."));
    printf("\n");

    // Scan for partitions — elevates automatically if needed
    std::vector<PartitionInfo> partitions;
    if (is_elevated()) {
        partitions = scan_ext4_partitions();
    } else {
        partitions = scan_with_elevation();
    }

    if (partitions.empty()) {
        printf("  %s\n", tr(
            "No ext4 partitions found.",
            "Nenhuma particao ext4 encontrada."));
        if (interactive) pause_before_exit();
        return 1;
    }

    printf("  %s\n", tr(
        "Found ext4 partitions:",
        "Particoes ext4 encontradas:"));
    printf("\n");

    for (size_t i = 0; i < partitions.size(); i++) {
        wprintf(L"    [%zu] %s\n", i + 1, partitions[i].display_name.c_str());
    }
    printf("\n");

    if (!interactive) {
        // CLI --scan mode: auto-mount first partition
        if (partitions.size() > 1) {
            wprintf(L"  %s %s\n", tr(
                L"Multiple partitions found. Using first:",
                L"Multiplas particoes encontradas. Usando a primeira:"),
                partitions[0].display_name.c_str());
        }
        return run_partition(partitions[0].device_path.c_str(),
                             partitions[0].display_name.c_str(),
                             mount_point, false, read_only);
    }

    // Interactive mode: let user pick
    printf("  %s\n", tr(
        "Choose a partition (or 0 to cancel):",
        "Escolha uma particao (ou 0 para cancelar):"));
    printf("\n");
    printf("  > ");
    fflush(stdout);

    wchar_t input[64] = {};
    if (!read_line(input, 64))
        return -1;

    int num = _wtoi(input);
    if (num <= 0 || num > (int)partitions.size()) {
        printf("\n  %s\n", tr("Cancelled.", "Cancelado."));
        return -1;
    }

    size_t choice = (size_t)(num - 1);
    auto& part = partitions[choice];

    wprintf(L"\n  %s %s\n", tr(L"Selected:", L"Selecionado:"),
            part.display_name.c_str());

    return run_partition(part.device_path.c_str(),
                         part.display_name.c_str(),
                         mount_point, interactive, read_only);
}

int wmain(int argc, wchar_t* argv[])
{
    // ── Parse global flags first ─────────────────────────────
    // Scan ALL arguments for --debug before doing anything else.
    // This way "--debug" works in any position:
    //   ext4windows --debug mount test.img
    //   ext4windows mount test.img --debug
    //
    // In Python, this would be like argparse with a global flag
    // that doesn't depend on subcommand position.
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--debug") == 0)
            g_debug = true;
    }

    // ── Find the subcommand (skipping flags) ─────────────────
    // Look for the first non-flag argument to determine what mode
    // we're in. This lets flags appear before or after the subcommand:
    //   ext4windows --debug mount ...
    //   ext4windows mount --debug ...
    // Both work the same way.
    int subcmd_idx = 0;  // index of the subcommand in argv (0 = none found)
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != L'-') {
            subcmd_idx = i;
            break;
        }
        // Special case: flags that ARE the subcommand
        if (wcscmp(argv[i], L"--help") == 0 || wcscmp(argv[i], L"-h") == 0 ||
            wcscmp(argv[i], L"/?") == 0 ||
            wcscmp(argv[i], L"--server") == 0 ||
            wcscmp(argv[i], L"--server-daemon") == 0 ||
            wcscmp(argv[i], L"--scan-save") == 0 ||
            wcscmp(argv[i], L"--open-device") == 0 ||
            wcscmp(argv[i], L"--scan") == 0 ||
            wcscmp(argv[i], L"--tui") == 0) {
            subcmd_idx = i;
            break;
        }
    }

    // CLI mode: ext4windows <image> [drive] or ext4windows --scan [drive]
    if (argc >= 2 && subcmd_idx > 0) {
        const wchar_t* subcmd = argv[subcmd_idx];

        // Check for --help / -h / /?
        if (wcscmp(subcmd, L"--help") == 0 ||
            wcscmp(subcmd, L"-h") == 0 ||
            wcscmp(subcmd, L"/?") == 0) {
            print_usage();
            return 0;
        }

        // Server mode: relaunch as detached background process and exit
        if (wcscmp(subcmd, L"--server") == 0) {
            wchar_t exe_path[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exe_path, MAX_PATH);

            std::wstring cmd = L"\"" + std::wstring(exe_path) + L"\" --server-daemon";
            for (int i = 2; i < argc; i++) {
                cmd += L" ";
                cmd += argv[i];
            }

            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi = {};
            if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
                               FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS,
                               nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
            return 0;  // Exit immediately — terminal closes
        }

        // Actual server process (launched detached by --server above)
        if (wcscmp(subcmd, L"--server-daemon") == 0) {
            return run_server();
        }

        // Client subcommands: mount, unmount, status, scan, quit
        // Note: g_debug is already set at the top, no need to re-parse.
        if (wcscmp(subcmd, L"mount") == 0 ||
            wcscmp(subcmd, L"unmount") == 0 ||
            wcscmp(subcmd, L"status") == 0 ||
            wcscmp(subcmd, L"scan") == 0 ||
            wcscmp(subcmd, L"quit") == 0) {
            return client_main(argc, argv);
        }

        // Internal command: --scan-save <file> (elevated subprocess)
        if (wcscmp(subcmd, L"--scan-save") == 0 && argc >= 3) {
            return do_scan_save(argv[subcmd_idx + 1]);
        }

        // Internal command: --open-device <path> <pid> <file> [--rw]
        if (wcscmp(subcmd, L"--open-device") == 0 && argc >= 5) {
            bool rw = false;
            for (int i = 5; i < argc; i++) {
                if (wcscmp(argv[i], L"--rw") == 0) rw = true;
            }
            return do_open_device(argv[subcmd_idx + 1],
                                  static_cast<DWORD>(_wtoi(argv[subcmd_idx + 2])),
                                  argv[subcmd_idx + 3], !rw);
        }

        // Parse CLI flags for legacy mode
        bool cli_read_only = true;
        bool cli_scan = false;
        for (int i = 1; i < argc; i++) {
            if (wcscmp(argv[i], L"--rw") == 0)
                cli_read_only = false;
            if (wcscmp(argv[i], L"--scan") == 0)
                cli_scan = true;
        }

        // Find the mount point argument: first non-flag arg that looks
        // like a drive letter (e.g. "Z:" or "Z")
        wchar_t mount_buf[8] = {};
        const wchar_t* image_path = nullptr;

        for (int i = 1; i < argc; i++) {
            // Skip flags
            if (argv[i][0] == L'-')
                continue;

            // Check if it looks like a drive letter (1 or 2 chars, A-Z)
            size_t len = wcslen(argv[i]);
            wchar_t first = towupper(argv[i][0]);
            if (len <= 2 && first >= L'A' && first <= L'Z' &&
                (len == 1 || argv[i][1] == L':')) {
                swprintf(mount_buf, 8, L"%c:", first);
            } else if (!image_path) {
                // It's the image path (first non-flag, non-drive arg)
                image_path = argv[i];
            }
        }

        // Auto-pick drive letter if none specified
        if (mount_buf[0] == L'\0') {
            wchar_t letter = find_free_drive_letter();
            if (!letter) {
                printf("  %s\n", tr(
                    "Error: no free drive letters available.",
                    "Erro: nenhuma letra de drive disponivel."));
                return 1;
            }
            swprintf(mount_buf, 8, L"%c:", letter);
        }

        print_banner();

        if (cli_scan) {
            // --scan mode: auto-detect ext4 partitions
            return do_scan(mount_buf, false, cli_read_only);
        }

        if (!image_path) {
            print_usage();
            return 1;
        }

        // If the path looks like a device path, mount as partition
        if (is_device_path(image_path)) {
            return run_partition(image_path, image_path, mount_buf,
                                false, cli_read_only);
        }

        return run(image_path, mount_buf, false, cli_read_only);
    }

    // GUI mode: no arguments — launch the graphical interface.
    // This is the main user experience when someone double-clicks the exe.
    // The GUI uses WebView2 (Chromium embedded browser) with an HTML
    // interface that communicates with the server via the same pipe protocol.
    //
    // Use --tui to force the old terminal-based interactive mode:
    //   ext4windows --tui
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--tui") == 0)
            return interactive_main();
    }

    return gui_main();
}
