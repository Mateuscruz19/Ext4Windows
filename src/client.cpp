#include "client.hpp"
#include "pipe_protocol.hpp"
#include "partition_scanner.hpp"
#include "debug_log.hpp"

#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <sstream>
#include <vector>

// Check if the server is already running by checking if the pipe exists.
// Uses WaitNamedPipeW instead of CreateFileW to avoid consuming the
// pipe connection (the server only has 1 instance at a time).
static bool is_server_running()
{
    return WaitNamedPipeW(PIPE_NAME, 100) != 0;
}

// Start the server process in background (detached, no console).
// The server is the same exe launched with "--server".
static bool start_server()
{
    // Get our own exe path
    wchar_t exe_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);

    // Build command line: "ext4windows.exe --server"
    // If debug is enabled, pass --debug too
    std::wstring cmd = L"\"";
    cmd += exe_path;
    cmd += L"\" --server";
    if (g_debug)
        cmd += L" --debug";

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    BOOL ok = CreateProcessW(
        nullptr,
        const_cast<LPWSTR>(cmd.c_str()),
        nullptr, nullptr,
        FALSE,
        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
        nullptr, nullptr,
        &si, &pi);

    if (!ok) {
        dbg("start_server: CreateProcessW failed err=%lu", GetLastError());
        return false;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// Wait for the server pipe to appear (up to ~3 seconds).
static bool wait_for_server()
{
    for (int i = 0; i < 30; i++) {
        if (WaitNamedPipeW(PIPE_NAME, 100))
            return true;
        Sleep(100);
    }
    return false;
}

// Send a command to the server and print the response.
static int send_and_print(const std::string& command)
{
    // Ensure server is running
    if (!is_server_running()) {
        printf("  Starting server...\n");
        if (!start_server()) {
            fprintf(stderr, "  Error: could not start server\n");
            return 1;
        }
        if (!wait_for_server()) {
            fprintf(stderr, "  Error: server did not start in time\n");
            return 1;
        }
    }

    // Connect to the pipe (retry if busy — the server handles
    // one client at a time, so we may need to wait)
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 10; attempt++) {
        pipe = CreateFileW(
            PIPE_NAME,
            GENERIC_READ | GENERIC_WRITE,
            0, nullptr,
            OPEN_EXISTING,
            0, nullptr);

        if (pipe != INVALID_HANDLE_VALUE)
            break;

        DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            // Pipe exists but server is handling another client — wait
            WaitNamedPipeW(PIPE_NAME, 2000);
            continue;
        }

        // Any other error — give up
        fprintf(stderr, "  Error: could not connect to server (err=%lu)\n", err);
        return 1;
    }

    if (pipe == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "  Error: server is busy, try again\n");
        return 1;
    }

    // Set pipe to message mode
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    // Send command
    if (!pipe_send(pipe, command)) {
        fprintf(stderr, "  Error: failed to send command\n");
        CloseHandle(pipe);
        return 1;
    }

    // Read response
    std::string response = pipe_recv(pipe);
    CloseHandle(pipe);

    if (response.empty()) {
        fprintf(stderr, "  Error: no response from server\n");
        return 1;
    }

    // Print the response
    printf("  %s\n", response.c_str());

    // Return 0 if response starts with "OK", 1 otherwise
    return (response.substr(0, 2) == "OK") ? 0 : 1;
}

// Convert wide string to UTF-8
static std::string to_utf8(const wchar_t* wide)
{
    if (!wide || !*wide) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1,
                                   nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string result(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), len,
                         nullptr, nullptr);
    return result;
}

int client_main(int argc, wchar_t* argv[])
{
    // argv[0] = "ext4windows"
    // argv[1] = subcommand ("mount", "unmount", "status", "quit")
    // argv[2..] = arguments

    if (argc < 2) {
        fprintf(stderr, "  Error: no subcommand\n");
        return 1;
    }

    std::wstring subcmd = argv[1];

    // --- mount ---
    if (subcmd == L"mount") {
        if (argc < 3) {
            fprintf(stderr, "  Usage: ext4windows mount <path> [drive:] [--rw]\n");
            return 1;
        }

        // Convert to absolute path so the server (which may have a
        // different working directory) can find the file.
        wchar_t abs_path[MAX_PATH] = {};
        GetFullPathNameW(argv[2], MAX_PATH, abs_path, nullptr);
        std::string source = to_utf8(abs_path);
        std::string drive = "";
        bool rw = false;

        // Parse remaining args
        for (int i = 3; i < argc; i++) {
            if (wcscmp(argv[i], L"--rw") == 0) {
                rw = true;
            } else if (wcslen(argv[i]) >= 1 && wcslen(argv[i]) <= 2
                       && argv[i][0] >= L'A' && argv[i][0] <= L'Z') {
                // Drive letter like "Z" or "Z:"
                drive = std::string(1, static_cast<char>(argv[i][0]));
            } else if (wcslen(argv[i]) >= 1 && wcslen(argv[i]) <= 2
                       && argv[i][0] >= L'a' && argv[i][0] <= L'z') {
                drive = std::string(1, static_cast<char>(argv[i][0] - 32));
            }
        }

        // Auto-select drive letter if not specified
        if (drive.empty()) {
            DWORD used = GetLogicalDrives();
            for (char letter = 'Z'; letter >= 'D'; letter--) {
                int bit = letter - 'A';
                if (!(used & (1 << bit))) {
                    drive = std::string(1, letter);
                    break;
                }
            }
            if (drive.empty()) {
                fprintf(stderr, "  Error: no free drive letter\n");
                return 1;
            }
        }

        std::string cmd = "MOUNT " + source + " " + drive;
        if (rw) cmd += " RW";
        return send_and_print(cmd);
    }

    // --- unmount ---
    if (subcmd == L"unmount") {
        if (argc < 3) {
            fprintf(stderr, "  Usage: ext4windows unmount <drive:>\n");
            return 1;
        }
        char letter = static_cast<char>(argv[2][0]);
        if (letter >= 'a' && letter <= 'z') letter -= 32;
        std::string cmd = "UNMOUNT " + std::string(1, letter);
        return send_and_print(cmd);
    }

    // --- status ---
    if (subcmd == L"status") {
        return send_and_print("STATUS");
    }

    // --- quit ---
    if (subcmd == L"quit") {
        return send_and_print("QUIT");
    }

    // --- scan ---
    if (subcmd == L"scan") {
        // Parse flags
        bool rw = false;
        std::string drive = "";
        for (int i = 2; i < argc; i++) {
            if (wcscmp(argv[i], L"--rw") == 0) {
                rw = true;
            } else if (wcslen(argv[i]) >= 1 && wcslen(argv[i]) <= 2
                       && ((argv[i][0] >= L'A' && argv[i][0] <= L'Z')
                        || (argv[i][0] >= L'a' && argv[i][0] <= L'z'))) {
                char c = static_cast<char>(argv[i][0]);
                if (c >= 'a') c -= 32;
                drive = std::string(1, c);
            }
        }

        // Send SCAN to server (server has admin privileges for disk access)
        // Ensure server is running first
        if (!is_server_running()) {
            printf("  Starting server...\n");
            if (!start_server() || !wait_for_server()) {
                fprintf(stderr, "  Error: could not start server\n");
                return 1;
            }
        }

        // Connect to pipe
        HANDLE pipe = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 10; attempt++) {
            pipe = CreateFileW(PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) break;
            if (GetLastError() == ERROR_PIPE_BUSY)
                WaitNamedPipeW(PIPE_NAME, 2000);
            else break;
        }
        if (pipe == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "  Error: could not connect to server\n");
            return 1;
        }
        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

        // Send SCAN command
        pipe_send(pipe, "SCAN");
        std::string response = pipe_recv(pipe);
        CloseHandle(pipe);

        if (response.empty() || response.substr(0, 5) == "ERROR") {
            fprintf(stderr, "  %s\n", response.empty()
                    ? "Error: no response from server" : response.c_str());
            return 1;
        }

        // Parse response: "OK N partition(s) found\n0|display|devpath\n..."
        std::istringstream iss(response);
        std::string header;
        std::getline(iss, header);

        struct ScanResult {
            std::string display;
            std::string device_path;
        };
        std::vector<ScanResult> results;

        std::string line;
        while (std::getline(iss, line)) {
            if (line.empty()) continue;
            // Format: index|display_name|device_path
            size_t sep1 = line.find('|');
            size_t sep2 = line.find('|', sep1 + 1);
            if (sep1 == std::string::npos || sep2 == std::string::npos) continue;
            ScanResult r;
            r.display = line.substr(sep1 + 1, sep2 - sep1 - 1);
            r.device_path = line.substr(sep2 + 1);
            results.push_back(r);
        }

        if (results.empty()) {
            printf("  No ext4 partitions found.\n");
            printf("  Make sure your Linux disk is connected and you're running as admin.\n");
            return 0;
        }

        // Display partitions
        printf("\n  ext4 partitions found:\n\n");
        for (size_t i = 0; i < results.size(); i++) {
            printf("    [%zu] %s\n", i + 1, results[i].display.c_str());
        }
        printf("\n  Select partition (1-%zu, or 0 to cancel): ",
               results.size());

        char input[16] = {};
        if (!fgets(input, sizeof(input), stdin)) return 1;
        int choice = atoi(input);
        if (choice < 1 || choice > static_cast<int>(results.size())) {
            printf("  Cancelled.\n");
            return 0;
        }

        auto& selected = results[choice - 1];

        // Auto-select drive letter if not specified
        if (drive.empty()) {
            DWORD used = GetLogicalDrives();
            for (char letter = 'Z'; letter >= 'D'; letter--) {
                int bit = letter - 'A';
                if (!(used & (1 << bit))) {
                    drive = std::string(1, letter);
                    break;
                }
            }
            if (drive.empty()) {
                fprintf(stderr, "  Error: no free drive letter\n");
                return 1;
            }
        }

        // Send MOUNT_PARTITION command
        std::string cmd = "MOUNT_PARTITION " + drive + " "
                        + (rw ? "RW" : "RO") + " " + selected.device_path;
        return send_and_print(cmd);
    }

    fprintf(stderr, "  Unknown subcommand: %ls\n", subcmd.c_str());
    fprintf(stderr, "  Available: mount, unmount, status, scan, quit\n");
    return 1;
}
