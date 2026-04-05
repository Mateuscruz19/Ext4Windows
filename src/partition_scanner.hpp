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
#include <vector>
#include <cstdint>

// Represents a detected partition that may contain an ext4 filesystem.
struct PartitionInfo {
    std::wstring device_path;   // e.g. L"\\\\?\\GLOBALROOT\\Device\\Harddisk1\\Partition2"
    std::wstring display_name;  // e.g. L"Disk 1, Partition 2 (50.00 GB)"
    uint64_t size_bytes;        // Partition size in bytes
    int disk_number;            // Physical disk number (0, 1, ...)
    int partition_number;       // Partition number within the disk (1, 2, ...)
};

// Scan all physical disks and return partitions that look like they could
// be ext4 (Linux partitions without a Windows drive letter).
std::vector<PartitionInfo> scan_ext4_partitions();
