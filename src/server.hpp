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
#include <map>
#include <mutex>
#include <memory>
#include <functional>

#include "ext4_filesystem.hpp"

extern "C" {
#include <ext4_blockdev.h>
}

// Represents a single mounted ext4 filesystem.
struct MountEntry {
    wchar_t drive_letter;
    std::wstring source_path;
    bool read_only;
    struct ext4_blockdev* bdev;
    void (*destroy_fn)(struct ext4_blockdev*);
    std::unique_ptr<Ext4FileSystem> fs;
};

// Forward declaration for TrayIcon (defined in tray_icon.hpp)
class TrayIcon;

// Manages multiple mounted ext4 filesystems.
// Thread-safe: all methods lock an internal mutex.
class MountManager {
public:
    MountManager();
    ~MountManager();

    // Mount an image file on the given drive letter.
    // Returns "OK ..." or "ERROR ..." message.
    std::string MountImage(const std::wstring& image_path,
                           wchar_t drive_letter, bool read_write);

    // Mount a raw disk partition on the given drive letter.
    // device_path is like "\\?\GLOBALROOT\Device\Harddisk1\Partition2".
    std::string MountPartition(const std::wstring& device_path,
                               wchar_t drive_letter, bool read_write);

    // Unmount the filesystem on the given drive letter.
    std::string Unmount(wchar_t drive_letter);

    // Get status of all active mounts.
    std::string Status();

    // Unmount all and signal quit.
    std::string Quit();

    // Get number of active mounts.
    size_t ActiveCount();

    // Set the tray icon (so we can update it on mount/unmount).
    void SetTrayIcon(TrayIcon* tray) { tray_ = tray; }

    // Check if quit was requested.
    bool QuitRequested() const { return quit_requested_; }

private:
    std::recursive_mutex mutex_;
    std::map<wchar_t, std::unique_ptr<MountEntry>> mounts_;
    TrayIcon* tray_ = nullptr;
    bool quit_requested_ = false;

    void NotifyTray();
};

// Run the server: listen on named pipe, manage mounts, show tray icon.
// This function does NOT return until the server is shut down.
int run_server();
