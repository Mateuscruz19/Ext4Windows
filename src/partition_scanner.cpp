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

#include "partition_scanner.hpp"
#include "debug_log.hpp"

#include <cstdio>
#include <cstring>

// We detect Linux/ext4 partitions by:
// 1. Enumerating physical disks (\\.\PhysicalDrive0, 1, 2, ...)
// 2. Reading each disk's partition table via DeviceIoControl
// 3. Filtering partitions whose type is Linux (MBR type 0x83) or
//    Linux filesystem GUID (GPT type 0FC63DAF-...)
// 4. Skipping partitions that Windows has already mounted (have a drive letter)
//
// Then for each candidate, we try to read the ext4 superblock at offset
// 1024 bytes. If the magic number 0xEF53 matches, it's ext4.

// GPT partition type GUID for Linux filesystem:
// 0FC63DAF-8483-4772-8E79-3D69D8477DE4
static const GUID LINUX_FS_GUID = {
    0x0FC63DAF, 0x8483, 0x4772,
    { 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 }
};

// ext4 superblock magic number at offset 0x38 within the superblock
static const uint16_t EXT4_SUPER_MAGIC = 0xEF53;

// Format a byte count as a human-readable size string (e.g. "50.00 GB")
static std::wstring format_size(uint64_t bytes)
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

// Check if two GUIDs are equal
static bool guid_equal(const GUID& a, const GUID& b)
{
    return memcmp(&a, &b, sizeof(GUID)) == 0;
}

// Try to read the ext4 superblock from a partition and check the magic number.
// The superblock is at byte offset 1024 from the start of the partition.
static bool check_ext4_superblock(const wchar_t* device_path)
{
    HANDLE h = CreateFileW(device_path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        dbg("  check_ext4_superblock: cannot open '%ls' (err=%lu)",
            device_path, GetLastError());
        return false;
    }

    // The ext4 superblock starts at byte 1024. We need to read at least
    // up to offset 0x38+2 = 58 bytes into the superblock, so 1024+58 = 1082.
    // But disk reads must be sector-aligned (512 bytes), so read 2 sectors
    // (1024 bytes) starting from offset 0, then read 1 more sector from
    // offset 1024. Actually, just read 4096 bytes from offset 0 to keep
    // it simple — superblock is at bytes 1024..2047 within that.
    uint8_t buf[4096] = {};
    DWORD bytes_read = 0;

    // Seek and read — for raw disk handles, offset must be sector-aligned
    LARGE_INTEGER offset;
    offset.QuadPart = 0;
    SetFilePointerEx(h, offset, nullptr, FILE_BEGIN);

    BOOL ok = ReadFile(h, buf, sizeof(buf), &bytes_read, nullptr);
    CloseHandle(h);

    if (!ok || bytes_read < 2048) {
        dbg("  check_ext4_superblock: read failed for '%ls'", device_path);
        return false;
    }

    // Superblock starts at byte 1024. Magic is at offset 0x38 within the
    // superblock, so absolute offset 1024 + 0x38 = 1080.
    uint16_t magic = 0;
    memcpy(&magic, &buf[1024 + 0x38], 2);

    dbg("  check_ext4_superblock: magic=0x%04X (expected 0x%04X)",
        magic, EXT4_SUPER_MAGIC);

    return magic == EXT4_SUPER_MAGIC;
}

std::vector<PartitionInfo> scan_ext4_partitions()
{
    std::vector<PartitionInfo> results;

    // Try up to 16 physical disks
    for (int disk = 0; disk < 16; disk++) {
        wchar_t disk_path[64];
        swprintf(disk_path, 64, L"\\\\.\\PhysicalDrive%d", disk);

        HANDLE h = CreateFileW(disk_path, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            continue;

        dbg("Scanning disk %d", disk);

        // Get partition layout. We allocate a generous buffer because
        // DRIVE_LAYOUT_INFORMATION_EX is variable-size.
        uint8_t layout_buf[16384] = {};
        DWORD returned = 0;

        BOOL ok = DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
            nullptr, 0, layout_buf, sizeof(layout_buf), &returned, nullptr);
        CloseHandle(h);

        if (!ok) {
            dbg("  Could not get partition layout for disk %d (err=%lu)",
                disk, GetLastError());
            continue;
        }

        auto* layout = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(layout_buf);
        dbg("  Partition style=%d, count=%lu",
            (int)layout->PartitionStyle, layout->PartitionCount);

        for (DWORD i = 0; i < layout->PartitionCount; i++) {
            auto& p = layout->PartitionEntry[i];

            // Skip empty/unused entries
            if (p.PartitionLength.QuadPart == 0)
                continue;

            bool is_linux = false;

            if (layout->PartitionStyle == PARTITION_STYLE_MBR) {
                // MBR: Linux partition type is 0x83
                dbg("  Partition %lu: MBR type=0x%02X size=%llu",
                    i, (int)p.Mbr.PartitionType,
                    (uint64_t)p.PartitionLength.QuadPart);
                is_linux = (p.Mbr.PartitionType == 0x83);
            } else if (layout->PartitionStyle == PARTITION_STYLE_GPT) {
                // GPT: check for Linux filesystem GUID
                dbg("  Partition %lu: GPT size=%llu",
                    i, (uint64_t)p.PartitionLength.QuadPart);
                is_linux = guid_equal(p.Gpt.PartitionType, LINUX_FS_GUID);
            }

            if (!is_linux) {
                dbg("  Partition %lu: not Linux, skipping", i);
                continue;
            }

            dbg("  Partition %lu: Linux partition detected!", i);

            // Build the device path for this partition.
            // Windows uses 1-based partition numbers.
            int part_num = p.PartitionNumber;
            if (part_num <= 0)
                part_num = static_cast<int>(i + 1);

            wchar_t part_path[128];
            swprintf(part_path, 128,
                L"\\\\?\\GLOBALROOT\\Device\\Harddisk%d\\Partition%d",
                disk, part_num);

            // Verify it's actually ext4 by checking the superblock magic
            if (!check_ext4_superblock(part_path)) {
                dbg("  Partition %d: no ext4 superblock, skipping", part_num);
                continue;
            }

            PartitionInfo info;
            info.device_path = part_path;
            info.size_bytes = static_cast<uint64_t>(p.PartitionLength.QuadPart);
            info.disk_number = disk;
            info.partition_number = part_num;

            wchar_t display[256];
            std::wstring size_str = format_size(info.size_bytes);
            swprintf(display, 256, L"Disk %d, Partition %d (%s)",
                disk, part_num, size_str.c_str());
            info.display_name = display;

            results.push_back(info);
            dbg("  Found ext4: %ls", info.display_name.c_str());
        }
    }

    return results;
}
