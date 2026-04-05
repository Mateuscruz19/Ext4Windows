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

#include "blockdev_file.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <ext4_errno.h>
}

static constexpr uint32_t BLOCK_SIZE = 512;

struct file_blockdev_data {
    FILE* file;
    char path[512];
    uint8_t block_buf[BLOCK_SIZE];
    ext4_blockdev_iface iface;
    ext4_blockdev bdev;
};

static int file_open(struct ext4_blockdev* bdev)
{
    auto* data = static_cast<file_blockdev_data*>(bdev->bdif->p_user);

    data->file = fopen(data->path, "r+b");
    if (!data->file) {
        // Try read-only
        data->file = fopen(data->path, "rb");
        if (!data->file)
            return EIO;
    }

    // Get file size (use 64-bit seek/tell for files >2GB)
    _fseeki64(data->file, 0, SEEK_END);
    int64_t size = _ftelli64(data->file);
    _fseeki64(data->file, 0, SEEK_SET);

    if (size <= 0)
        return EIO;

    bdev->bdif->ph_bcnt = static_cast<uint64_t>(size) / BLOCK_SIZE;
    bdev->part_offset = 0;
    bdev->part_size = static_cast<uint64_t>(size);

    return EOK;
}

static int file_bread(struct ext4_blockdev* bdev, void* buf, uint64_t blk_id,
                       uint32_t blk_cnt)
{
    auto* data = static_cast<file_blockdev_data*>(bdev->bdif->p_user);

    if (_fseeki64(data->file, static_cast<int64_t>(blk_id) * BLOCK_SIZE, SEEK_SET) != 0)
        return EIO;

    size_t bytes = static_cast<size_t>(blk_cnt) * BLOCK_SIZE;
    if (fread(buf, 1, bytes, data->file) != bytes)
        return EIO;

    return EOK;
}

static int file_bwrite(struct ext4_blockdev* bdev, const void* buf,
                        uint64_t blk_id, uint32_t blk_cnt)
{
    auto* data = static_cast<file_blockdev_data*>(bdev->bdif->p_user);

    if (_fseeki64(data->file, static_cast<int64_t>(blk_id) * BLOCK_SIZE, SEEK_SET) != 0)
        return EIO;

    size_t bytes = static_cast<size_t>(blk_cnt) * BLOCK_SIZE;
    if (fwrite(buf, 1, bytes, data->file) != bytes)
        return EIO;

    fflush(data->file);
    return EOK;
}

static int file_close(struct ext4_blockdev* bdev)
{
    auto* data = static_cast<file_blockdev_data*>(bdev->bdif->p_user);

    if (data->file) {
        fclose(data->file);
        data->file = nullptr;
    }
    return EOK;
}

struct ext4_blockdev* create_file_blockdev(const char* path)
{
    auto* data = new file_blockdev_data{};

    strncpy(data->path, path, sizeof(data->path) - 1);
    data->path[sizeof(data->path) - 1] = '\0';
    data->file = nullptr;

    // Set up the interface
    std::memset(&data->iface, 0, sizeof(data->iface));
    data->iface.open = file_open;
    data->iface.bread = file_bread;
    data->iface.bwrite = file_bwrite;
    data->iface.close = file_close;
    data->iface.ph_bsize = BLOCK_SIZE;
    data->iface.ph_bcnt = 0; // Set during open
    data->iface.ph_bbuf = data->block_buf;
    data->iface.p_user = data;

    // Set up the block device
    std::memset(&data->bdev, 0, sizeof(data->bdev));
    data->bdev.bdif = &data->iface;
    data->bdev.part_offset = 0;
    data->bdev.part_size = 0; // Set during open

    return &data->bdev;
}

void destroy_file_blockdev(struct ext4_blockdev* bdev)
{
    if (!bdev) return;

    // Recover the data pointer from the container struct
    auto* data = reinterpret_cast<file_blockdev_data*>(
        reinterpret_cast<char*>(bdev) - offsetof(file_blockdev_data, bdev));

    if (data->file)
        fclose(data->file);

    delete data;
}
