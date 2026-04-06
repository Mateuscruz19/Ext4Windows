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
        exe_path,       // Use explicit exe path (not NULL) to prevent
        const_cast<LPWSTR>(cmd.c_str()),  // PATH search hijacking
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
    // Find the subcommand index — skip flags like --debug that may
    // appear before the subcommand. This way both of these work:
    //   ext4windows --debug mount test.img
    //   ext4windows mount --debug test.img
    //
    // In Python terms, this is like using argparse with a global
    // --debug flag that can appear anywhere in the command line.
    int sub_idx = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != L'-') {
            sub_idx = i;
            break;
        }
    }

    if (sub_idx == 0) {
        fprintf(stderr, "  Error: no subcommand\n");
        return 1;
    }

    std::wstring subcmd = argv[sub_idx];

    // --- mount ---
    // Supports multiple images at once:
    //   ext4windows mount a.img b.img c.img
    //   ext4windows mount a.img Z: --ro
    //
    // Each image gets the next free drive letter (Z, Y, X, ...).
    // If only one image, a specific drive letter can be given.
    if (subcmd == L"mount") {
        bool rw = true;
        std::string explicit_drive = "";
        std::vector<std::wstring> paths;

        // Collect paths, flags, and optional drive letter
        for (int i = sub_idx + 1; i < argc; i++) {
            if (wcscmp(argv[i], L"--rw") == 0) { rw = true; continue; }
            if (wcscmp(argv[i], L"--ro") == 0) { rw = false; continue; }
            if (wcscmp(argv[i], L"--debug") == 0) continue;

            // Check if it looks like a drive letter (1-2 chars, A-Z)
            size_t len = wcslen(argv[i]);
            wchar_t first = towupper(argv[i][0]);
            if (len <= 2 && first >= L'A' && first <= L'Z' &&
                (len == 1 || argv[i][1] == L':')) {
                explicit_drive = std::string(1, static_cast<char>(first));
                continue;
            }

            // It's a path — convert to absolute
            wchar_t abs_path[MAX_PATH] = {};
            GetFullPathNameW(argv[i], MAX_PATH, abs_path, nullptr);
            paths.push_back(abs_path);
        }

        if (paths.empty()) {
            fprintf(stderr, "  Usage: ext4windows mount <path> [path2 ...] [drive:] [--rw|--ro]\n");
            return 1;
        }

        // For each path, pick a drive letter and send MOUNT
        int errors = 0;
        DWORD used = GetLogicalDrives();

        for (size_t pi = 0; pi < paths.size(); pi++) {
            std::string source = to_utf8(paths[pi].c_str());

            // Use explicit drive only for single-image mount
            std::string drive = "";
            if (paths.size() == 1 && !explicit_drive.empty()) {
                drive = explicit_drive;
            } else {
                // Auto-select next free letter (Z down to D)
                for (char letter = 'Z'; letter >= 'D'; letter--) {
                    int bit = letter - 'A';
                    if (!(used & (1 << bit))) {
                        drive = std::string(1, letter);
                        used |= (1 << bit); // Mark as used for next iteration
                        break;
                    }
                }
                if (drive.empty()) {
                    fprintf(stderr, "  Error: no free drive letter for %s\n",
                            source.c_str());
                    errors++;
                    continue;
                }
            }

            // Protocol: "MOUNT <drive> [RW|RO] <source>"
            std::string cmd = "MOUNT " + drive;
            if (rw) cmd += " RW";
            else    cmd += " RO";
            cmd += " " + source;

            int rc = send_and_print(cmd);
            if (rc != 0) errors++;
        }

        return errors > 0 ? 1 : 0;
    }

    // --- unmount ---
    if (subcmd == L"unmount") {
        // Find drive letter arg (first non-flag after subcommand)
        int drive_idx = 0;
        for (int i = sub_idx + 1; i < argc; i++) {
            if (argv[i][0] != L'-') { drive_idx = i; break; }
        }
        if (drive_idx == 0) {
            fprintf(stderr, "  Usage: ext4windows unmount <drive:>\n");
            return 1;
        }
        char letter = static_cast<char>(argv[drive_idx][0]);
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
        // Parse flags — default to read-write like mount
        bool rw = true;
        std::string drive = "";
        for (int i = sub_idx + 1; i < argc; i++) {
            if (wcscmp(argv[i], L"--rw") == 0) {
                rw = true;
            } else if (wcscmp(argv[i], L"--ro") == 0) {
                rw = false;
            } else if (wcslen(argv[i]) >= 1 && wcslen(argv[i]) <= 2
                       && ((argv[i][0] >= L'A' && argv[i][0] <= L'Z')
                        || (argv[i][0] >= L'a' && argv[i][0] <= L'z'))) {
                char c = static_cast<char>(argv[i][0]);
                if (c >= 'a') c -= 32;
                drive = std::string(1, c);
            }
        }

        // Scan runs LOCALLY in the client process (not via server pipe).
        // Why? Because scan needs admin privileges to open \\.\PhysicalDrive
        // devices. The server is a background process that may have been
        // started without admin. The client, on the other hand, runs in the
        // user's terminal — if they launched it as admin, we have the
        // privileges we need right here.
        //
        // In Python terms, this is like calling the function directly
        // instead of sending an RPC to a worker process that might not
        // have the right permissions.

        // Check if we have admin privileges. If not, re-launch ourselves
        // as admin using ShellExecuteExW with "runas" verb. This triggers
        // a UAC (User Account Control) prompt — that Windows dialog that
        // asks "Do you want to allow this app to make changes?"
        //
        // ShellExecuteExW docs:
        // https://learn.microsoft.com/en-us/windows/win32/api/shellapi/
        //   nf-shellapi-shellexecuteexw
        // "runas" verb docs:
        // https://learn.microsoft.com/en-us/windows/win32/api/shellapi/
        //   ns-shellapi-shellexecuteinfow
        {
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

            if (!elevated) {
                // Not admin — re-launch this exact command line as admin.
                // GetCommandLineW() returns the full command line string
                // that Windows used to launch this process (including exe
                // path and all arguments).
                //
                // In Python, this would be like:
                //   subprocess.run(sys.argv, shell=True, runas='admin')
                //
                // Docs: https://learn.microsoft.com/en-us/windows/win32/
                //   api/processenv/nf-processenv-getcommandlinew
                wchar_t exe_path[MAX_PATH] = {};
                GetModuleFileNameW(nullptr, exe_path, MAX_PATH);

                // Rebuild args: "scan [drive] [--rw]"
                std::wstring args = L"scan";
                for (int i = 2; i < argc; i++) {
                    args += L" ";
                    args += argv[i];
                }

                SHELLEXECUTEINFOW sei = {};
                sei.cbSize = sizeof(sei);
                sei.fMask = SEE_MASK_NOCLOSEPROCESS;
                sei.lpVerb = L"runas";      // Triggers UAC elevation
                sei.lpFile = exe_path;
                sei.lpParameters = args.c_str();
                sei.nShow = SW_SHOWNORMAL;  // Show the elevated console

                if (!ShellExecuteExW(&sei)) {
                    fprintf(stderr, "  Error: Administrator access required for scanning.\n");
                    fprintf(stderr, "  Run this command from an elevated prompt, or accept the UAC dialog.\n");
                    return 1;
                }

                // Wait for the elevated process to finish
                WaitForSingleObject(sei.hProcess, INFINITE);
                DWORD exit_code = 0;
                GetExitCodeProcess(sei.hProcess, &exit_code);
                CloseHandle(sei.hProcess);
                return static_cast<int>(exit_code);
            }
        }

        auto partitions = scan_ext4_partitions();

        if (partitions.empty()) {
            printf("  No ext4 partitions found.\n");
            printf("  Make sure your Linux disk is connected.\n");
            return 0;
        }

        // Build results list from the scan
        struct ScanResult {
            std::string display;
            std::string device_path;
        };
        std::vector<ScanResult> results;

        for (auto& p : partitions) {
            ScanResult r;
            r.display = to_utf8(p.display_name.c_str());
            r.device_path = to_utf8(p.device_path.c_str());
            results.push_back(r);
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
