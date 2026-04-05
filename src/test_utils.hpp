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

// Testable utility functions extracted from the codebase.
// These are pure-logic functions with no Windows API dependencies
// (except WideToUtf8 which uses WideCharToMultiByte).
//
// This header allows the test suite to call these functions directly
// without needing to instantiate Ext4FileSystem, MountManager, etc.

#include <windows.h>
#include <string>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cstdio>

namespace ext4windows {

// --- Drive Letter Validation ---
// Returns true if c is a valid drive letter (A-Z or a-z).
inline bool is_valid_drive_letter(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// --- Path Traversal Detection ---
// Returns true if the path contains dangerous sequences like ".." or null bytes.
inline bool path_contains_traversal(const std::string& path)
{
    if (path.find("..") != std::string::npos) return true;
    if (path.find('\0') != std::string::npos) return true;
    return false;
}

// --- Wide String to UTF-8 Conversion ---
inline std::string wide_to_utf8(const wchar_t* wide)
{
    if (!wide || !*wide)
        return "";

    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return "";

    std::string result(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), len, nullptr, nullptr);
    return result;
}

// --- UTF-8 to Wide String Conversion ---
inline std::wstring utf8_to_wide(const char* utf8)
{
    if (!utf8 || !*utf8)
        return L"";

    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 0)
        return L"";

    std::wstring result(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, result.data(), len);
    return result;
}

// --- Windows Path to ext4 Path ---
// Converts a Windows-style path (\dir\file) to an ext4 mount path (/mnt_X/dir/file).
inline std::string to_ext4_path(const wchar_t* win_path, const std::string& mount_point)
{
    std::string path = wide_to_utf8(win_path);

    // Replace backslashes with forward slashes
    std::replace(path.begin(), path.end(), '\\', '/');

    // Ensure leading slash
    if (path.empty() || path[0] != '/')
        path = "/" + path;

    // For root, just return mount point
    if (path == "/")
        return mount_point;

    // Strip leading "/" since mount_point already ends with "/"
    if (path[0] == '/')
        path = path.substr(1);
    return mount_point + path;
}

// --- Format Byte Size ---
// Converts a byte count to a human-readable string (e.g. "1.50 GB").
inline std::wstring format_size(uint64_t bytes)
{
    wchar_t buf[64];
    if (bytes >= 1099511627776ULL)
        swprintf(buf, 64, L"%.2f TB", (double)bytes / 1099511627776.0);
    else if (bytes >= 1073741824ULL)
        swprintf(buf, 64, L"%.2f GB", (double)bytes / 1073741824.0);
    else if (bytes >= 1048576ULL)
        swprintf(buf, 64, L"%.2f MB", (double)bytes / 1048576.0);
    else
        swprintf(buf, 64, L"%llu bytes", bytes);
    return buf;
}

// --- GUID Comparison ---
inline bool guid_equal(const GUID& a, const GUID& b)
{
    return memcmp(&a, &b, sizeof(GUID)) == 0;
}

// --- POSIX Timestamp to Windows FILETIME ---
// Windows FILETIME = 100-nanosecond intervals since 1601-01-01
// POSIX time = seconds since 1970-01-01
// Difference = 11644473600 seconds (in 100ns units: 116444736000000000)
inline uint64_t posix_to_windows_filetime(uint32_t posix_time)
{
    const uint64_t EPOCH_DIFF = 116444736000000000ULL;
    return static_cast<uint64_t>(posix_time) * 10000000ULL + EPOCH_DIFF;
}

// --- Windows FILETIME to POSIX Timestamp ---
inline uint32_t windows_filetime_to_posix(uint64_t win_time)
{
    const uint64_t EPOCH_DIFF = 116444736000000000ULL;
    if (win_time <= EPOCH_DIFF)
        return 0;
    return static_cast<uint32_t>((win_time - EPOCH_DIFF) / 10000000ULL);
}

// --- Permission Mapping: ext4 mode → Windows attributes ---
// Returns FILE_ATTRIBUTE flags based on ext4 mode bits and filename.
inline uint32_t ext4_mode_to_win_attributes(uint32_t mode, bool is_directory,
                                             const char* basename)
{
    uint32_t attrs = 0;

    if (is_directory) {
        attrs |= FILE_ATTRIBUTE_DIRECTORY;
    } else {
        attrs |= FILE_ATTRIBUTE_NORMAL;
    }

    // Check owner write permission: bit 7 (0200 in octal)
    bool owner_can_write = (mode & 0200) != 0;
    if (!owner_can_write && !is_directory) {
        attrs |= FILE_ATTRIBUTE_READONLY;
    }

    // Hidden files: Linux files starting with '.' are hidden
    if (basename && basename[0] == '.') {
        attrs |= FILE_ATTRIBUTE_HIDDEN;
    }

    // When READONLY or HIDDEN is set, NORMAL must be cleared
    if (attrs & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_DIRECTORY)) {
        attrs &= ~FILE_ATTRIBUTE_NORMAL;
    }

    return attrs;
}

// --- Parse Pipe Command ---
// Extracts the action (first word) from a pipe command string.
inline std::string parse_command_action(const std::string& cmd)
{
    size_t end = cmd.find(' ');
    if (end == std::string::npos)
        return cmd;
    return cmd.substr(0, end);
}

// --- Normalize Drive Letter ---
// Converts lowercase drive letter to uppercase.
inline wchar_t normalize_drive_letter(wchar_t c)
{
    if (c >= L'a' && c <= L'z')
        return c - 32;
    return c;
}

// --- Validate Mount Command ---
// Returns an error string if the MOUNT command is invalid, or empty if valid.
inline std::string validate_mount_command(const std::string& source,
                                           const std::string& drive_str)
{
    if (source.empty() || drive_str.empty())
        return "ERROR Usage: MOUNT <source_path> <drive_letter> [RW]";

    if (drive_str.size() < 1 || !is_valid_drive_letter(drive_str[0]))
        return "ERROR Invalid drive letter";

    if (path_contains_traversal(source))
        return "ERROR Path contains invalid sequences";

    return "";  // Valid
}

// --- Integer Overflow Guard ---
// Returns true if blk_cnt * block_size would overflow a DWORD.
inline bool would_overflow_dword(uint32_t blk_cnt, uint32_t block_size)
{
    return blk_cnt > 0xFFFFFFFFULL / block_size;
}

} // namespace ext4windows
