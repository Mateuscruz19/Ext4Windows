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

#include <cstdint>

extern "C" {
#include <ext4_blockdev.h>
}

// Creates an ext4_blockdev backed by a raw disk partition.
// device_path is a Windows device path like "\\?\GLOBALROOT\Device\Harddisk1\Partition2".
// read_only controls whether the partition is opened for writing.
// The caller takes ownership and must call destroy_partition_blockdev() when done.
struct ext4_blockdev* create_partition_blockdev(const wchar_t* device_path,
                                                bool read_only);

// Creates an ext4_blockdev using an already-opened HANDLE.
// The blockdev takes ownership of the handle (will close it on destroy).
struct ext4_blockdev* create_partition_blockdev_from_handle(void* handle,
                                                            bool read_only);

void destroy_partition_blockdev(struct ext4_blockdev* bdev);
