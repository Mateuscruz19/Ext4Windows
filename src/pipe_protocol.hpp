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
#include <string>

// Named pipe path used for IPC between client and server.
// The client sends text commands, the server responds with text.
static const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\ext4windows";

// Maximum size for a single pipe message (command or response).
static const DWORD PIPE_BUFFER_SIZE = 4096;

// --- Command format (client -> server) ---
// Each command is a single line of text:
//   MOUNT <source_path> <drive_letter> [RW]
//   MOUNT_PARTITION <drive_letter> <RW|RO> <device_path>
//   UNMOUNT <drive_letter>
//   STATUS
//   SCAN
//   QUIT
//
// --- Response format (server -> client) ---
// Each response is one or more lines, starting with OK or ERROR:
//   OK Mounted C:\linux.img on Z: (read-only)\n
//   ERROR Drive Z: is already in use\n
//   OK 2 mounts active\nZ: C:\linux.img (read-only)\nY: ... (read-write)\n

// Helper: send a string over a pipe handle.
// Returns true on success.
inline bool pipe_send(HANDLE pipe, const std::string& msg)
{
    DWORD written = 0;
    return WriteFile(pipe, msg.c_str(), static_cast<DWORD>(msg.size()),
                     &written, nullptr) && written == msg.size();
}

// Helper: read a string from a pipe handle.
// Returns the received string, or empty on failure.
inline std::string pipe_recv(HANDLE pipe)
{
    char buf[PIPE_BUFFER_SIZE] = {};
    DWORD bytes_read = 0;
    if (!ReadFile(pipe, buf, sizeof(buf) - 1, &bytes_read, nullptr))
        return "";
    buf[bytes_read] = '\0';
    return std::string(buf);
}
