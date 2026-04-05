#include "blockdev_partition.hpp"
#include "debug_log.hpp"

#include <windows.h>
#include <cstdio>
#include <cstring>

extern "C" {
#include <ext4_errno.h>
}

static constexpr uint32_t BLOCK_SIZE = 512;

// Internal data for a partition-backed block device.
// Uses Windows HANDLE (CreateFile/ReadFile/WriteFile) for raw disk I/O.
struct partition_blockdev_data {
    HANDLE handle;
    bool read_only;
    wchar_t path[256];
    uint8_t block_buf[BLOCK_SIZE];
    ext4_blockdev_iface iface;
    ext4_blockdev bdev;
};

// Helper: get partition size from an open handle
static bool get_partition_size(HANDLE h, LARGE_INTEGER* out_size)
{
    if (GetFileSizeEx(h, out_size))
        return true;

    // For raw devices, GetFileSizeEx may not work.
    // Try IOCTL_DISK_GET_LENGTH_INFO instead.
    GET_LENGTH_INFORMATION len_info = {};
    DWORD returned = 0;
    if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO,
            nullptr, 0, &len_info, sizeof(len_info), &returned, nullptr)) {
        out_size->QuadPart = len_info.Length.QuadPart;
        return true;
    }

    return false;
}

static int part_open(struct ext4_blockdev* bdev)
{
    auto* data = static_cast<partition_blockdev_data*>(bdev->bdif->p_user);

    // If handle is already open (from create_partition_blockdev_from_handle),
    // just get the size and return.
    if (data->handle != INVALID_HANDLE_VALUE) {
        dbg("part_open: using pre-opened handle");
    } else {
        DWORD access = data->read_only ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);

        data->handle = CreateFileW(data->path, access,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr);

        if (data->handle == INVALID_HANDLE_VALUE) {
            dbg("part_open: CreateFileW failed for '%ls' (err=%lu)",
                data->path, GetLastError());
            return EIO;
        }
    }

    // Get partition size
    LARGE_INTEGER size;
    if (!get_partition_size(data->handle, &size)) {
        dbg("part_open: cannot get size (err=%lu)", GetLastError());
        CloseHandle(data->handle);
        data->handle = INVALID_HANDLE_VALUE;
        return EIO;
    }

    bdev->bdif->ph_bcnt = static_cast<uint64_t>(size.QuadPart) / BLOCK_SIZE;
    bdev->part_offset = 0;
    bdev->part_size = static_cast<uint64_t>(size.QuadPart);

    dbg("part_open: size=%llu blocks=%llu",
        bdev->part_size, bdev->bdif->ph_bcnt);

    return EOK;
}

static int part_bread(struct ext4_blockdev* bdev, void* buf, uint64_t blk_id,
                       uint32_t blk_cnt)
{
    auto* data = static_cast<partition_blockdev_data*>(bdev->bdif->p_user);

    LARGE_INTEGER offset;
    offset.QuadPart = static_cast<int64_t>(blk_id) * BLOCK_SIZE;

    if (!SetFilePointerEx(data->handle, offset, nullptr, FILE_BEGIN)) {
        dbg("part_bread: SetFilePointerEx failed blk=%llu err=%lu",
            blk_id, GetLastError());
        return EIO;
    }

    DWORD bytes_to_read = static_cast<DWORD>(blk_cnt) * BLOCK_SIZE;
    DWORD bytes_read = 0;

    // FILE_FLAG_NO_BUFFERING requires the read buffer to be aligned to the
    // disk sector size. lwext4 may pass unaligned buffers, so we use an
    // aligned intermediate buffer and copy the data.
    void* aligned_buf = _aligned_malloc(bytes_to_read, BLOCK_SIZE);
    if (!aligned_buf)
        return ENOMEM;

    if (!ReadFile(data->handle, aligned_buf, bytes_to_read, &bytes_read, nullptr)) {
        dbg("part_bread: ReadFile failed blk=%llu cnt=%u err=%lu",
            blk_id, blk_cnt, GetLastError());
        _aligned_free(aligned_buf);
        return EIO;
    }

    if (bytes_read != bytes_to_read) {
        dbg("part_bread: short read blk=%llu expected=%lu got=%lu",
            blk_id, bytes_to_read, bytes_read);
        _aligned_free(aligned_buf);
        return EIO;
    }

    memcpy(buf, aligned_buf, bytes_to_read);
    _aligned_free(aligned_buf);

    return EOK;
}

static int part_bwrite(struct ext4_blockdev* bdev, const void* buf,
                        uint64_t blk_id, uint32_t blk_cnt)
{
    auto* data = static_cast<partition_blockdev_data*>(bdev->bdif->p_user);

    if (data->read_only)
        return EPERM;

    LARGE_INTEGER offset;
    offset.QuadPart = static_cast<int64_t>(blk_id) * BLOCK_SIZE;

    if (!SetFilePointerEx(data->handle, offset, nullptr, FILE_BEGIN))
        return EIO;

    DWORD bytes_to_write = static_cast<DWORD>(blk_cnt) * BLOCK_SIZE;
    DWORD bytes_written = 0;

    // Use aligned buffer for FILE_FLAG_NO_BUFFERING compatibility
    void* aligned_buf = _aligned_malloc(bytes_to_write, BLOCK_SIZE);
    if (!aligned_buf)
        return ENOMEM;

    memcpy(aligned_buf, buf, bytes_to_write);

    if (!WriteFile(data->handle, aligned_buf, bytes_to_write, &bytes_written, nullptr)) {
        _aligned_free(aligned_buf);
        return EIO;
    }

    _aligned_free(aligned_buf);

    if (bytes_written != bytes_to_write)
        return EIO;

    return EOK;
}

static int part_close(struct ext4_blockdev* bdev)
{
    auto* data = static_cast<partition_blockdev_data*>(bdev->bdif->p_user);

    if (data->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(data->handle);
        data->handle = INVALID_HANDLE_VALUE;
    }
    return EOK;
}

struct ext4_blockdev* create_partition_blockdev(const wchar_t* device_path,
                                                bool read_only)
{
    auto* data = new partition_blockdev_data{};

    wcsncpy(data->path, device_path, 255);
    data->path[255] = L'\0';
    data->handle = INVALID_HANDLE_VALUE;
    data->read_only = read_only;

    // Set up the interface
    std::memset(&data->iface, 0, sizeof(data->iface));
    data->iface.open = part_open;
    data->iface.bread = part_bread;
    data->iface.bwrite = part_bwrite;
    data->iface.close = part_close;
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

struct ext4_blockdev* create_partition_blockdev_from_handle(void* handle,
                                                            bool read_only)
{
    auto* data = new partition_blockdev_data{};

    wcscpy(data->path, L"(inherited handle)");
    data->handle = static_cast<HANDLE>(handle);
    data->read_only = read_only;

    // Set up the interface
    std::memset(&data->iface, 0, sizeof(data->iface));
    data->iface.open = part_open;
    data->iface.bread = part_bread;
    data->iface.bwrite = part_bwrite;
    data->iface.close = part_close;
    data->iface.ph_bsize = BLOCK_SIZE;
    data->iface.ph_bcnt = 0;
    data->iface.ph_bbuf = data->block_buf;
    data->iface.p_user = data;

    // Set up the block device
    std::memset(&data->bdev, 0, sizeof(data->bdev));
    data->bdev.bdif = &data->iface;
    data->bdev.part_offset = 0;
    data->bdev.part_size = 0;

    return &data->bdev;
}

void destroy_partition_blockdev(struct ext4_blockdev* bdev)
{
    if (!bdev) return;

    auto* data = reinterpret_cast<partition_blockdev_data*>(
        reinterpret_cast<char*>(bdev) - offsetof(partition_blockdev_data, bdev));

    if (data->handle != INVALID_HANDLE_VALUE)
        CloseHandle(data->handle);

    delete data;
}
