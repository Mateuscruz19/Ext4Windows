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

#include "ext4_filesystem.hpp"
#include "debug_log.hpp"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>
#include <sddl.h>

// Global mutex shared by ALL Ext4FileSystem instances.
// lwext4 uses global internal state (mount table, device registry),
// so concurrent access from multiple instances would corrupt it.
std::mutex& Ext4FileSystem::global_ext4_mutex()
{
    static std::mutex mtx;
    return mtx;
}

// Convert wide string (UTF-16) to UTF-8
std::string Ext4FileSystem::WideToUtf8(const wchar_t* wide)
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

// Convert Windows path (backslashes) to ext4 path (forward slashes).
// Prepends the per-instance lwext4 mount point so that "/hello.txt"
// becomes "/mnt_Z/hello.txt" (where Z is the instance ID).
std::string Ext4FileSystem::ToExt4Path(const wchar_t* win_path)
{
    std::string path = WideToUtf8(win_path);

    // Replace backslashes with forward slashes
    std::replace(path.begin(), path.end(), '\\', '/');

    // Ensure leading slash
    if (path.empty() || path[0] != '/')
        path = "/" + path;

    // Prepend mount point: "/mnt_Z/" + "hello.txt" = "/mnt_Z/hello.txt"
    // For root: just return "/mnt_Z/"
    // mount_point_ already ends with "/" (e.g. "/mnt_Z/")
    if (path == "/")
        return mount_point_;

    // Strip leading "/" from path since mount_point_ already ends with "/"
    if (path[0] == '/')
        path = path.substr(1);
    return mount_point_ + path;
}

Ext4FileSystem* Ext4FileSystem::GetSelf(FSP_FILE_SYSTEM* FileSystem)
{
    return static_cast<Ext4FileSystem*>(FileSystem->UserContext);
}

Ext4FileSystem::Ext4FileSystem()
{
    std::memset(&iface_, 0, sizeof(iface_));

    // Wire up callbacks — tell WinFsp which function to call for each operation
    iface_.GetVolumeInfo = &Ext4FileSystem::OnGetVolumeInfo;
    iface_.GetSecurityByName = &Ext4FileSystem::OnGetSecurityByName;
    iface_.Create = &Ext4FileSystem::OnCreate;
    iface_.Open = &Ext4FileSystem::OnOpen;
    iface_.Overwrite = &Ext4FileSystem::OnOverwrite;
    iface_.Close = &Ext4FileSystem::OnClose;
    iface_.Read = &Ext4FileSystem::OnRead;
    iface_.Write = &Ext4FileSystem::OnWrite;
    iface_.ReadDirectory = &Ext4FileSystem::OnReadDirectory;
    iface_.GetFileInfo = &Ext4FileSystem::OnGetFileInfo;
    iface_.SetBasicInfo = &Ext4FileSystem::OnSetBasicInfo;
    iface_.SetFileSize = &Ext4FileSystem::OnSetFileSize;
    iface_.CanDelete = &Ext4FileSystem::OnCanDelete;
    iface_.Rename = &Ext4FileSystem::OnRename;
    iface_.Flush = &Ext4FileSystem::OnFlush;
    iface_.Cleanup = &Ext4FileSystem::OnCleanup;
}

Ext4FileSystem::~Ext4FileSystem()
{
    Unmount();
}

NTSTATUS Ext4FileSystem::Mount(struct ext4_blockdev* bdev, const wchar_t* mount_point,
                                bool read_only, char instance_id)
{
    bdev_ = bdev;
    read_only_ = read_only;

    // Generate unique lwext4 names for this instance so multiple
    // filesystems can be mounted simultaneously without collisions.
    device_name_ = std::string("ext4dev_") + instance_id;
    mount_point_ = std::string("/mnt_") + instance_id + "/";

    dbg("Mount: read_only=%d device=%s lwext4_mount=%s",
        (int)read_only, device_name_.c_str(), mount_point_.c_str());

    // Register and mount via lwext4
    dbg("Mount: calling ext4_device_register('%s')", device_name_.c_str());
    int rc = ext4_device_register(bdev_, device_name_.c_str());
    if (rc != EOK) {
        dbg("Mount: ext4_device_register failed rc=%d", rc);
        bdev_ = nullptr;
        return STATUS_UNSUCCESSFUL;
    }
    dbg("Mount: device registered OK");

    dbg("Mount: calling ext4_mount('%s', '%s', %d)",
        device_name_.c_str(), mount_point_.c_str(), (int)read_only);
    rc = ext4_mount(device_name_.c_str(), mount_point_.c_str(), read_only);
    if (rc != EOK) {
        dbg("Mount: ext4_mount failed rc=%d", rc);
        ext4_device_unregister(device_name_.c_str());
        bdev_ = nullptr;
        return STATUS_UNSUCCESSFUL;
    }
    dbg("Mount: ext4_mount OK");

    // Journal recovery: replay any pending transactions from a previous
    // unclean shutdown (e.g. power loss, crash). This is safe to call even
    // if the filesystem has no journal — lwext4 simply returns EOK.
    rc = ext4_recover(mount_point_.c_str());
    if (rc != EOK && rc != ENOTSUP) {
        dbg("Mount: ext4_recover failed rc=%d (non-fatal)", rc);
    } else {
        dbg("Mount: ext4_recover OK");
    }

    // Start journaling: wraps all future write operations in transactions
    // so they can be rolled back if the system crashes mid-write.
    // Transparent — does nothing if the filesystem has no journal feature.
    if (!read_only) {
        rc = ext4_journal_start(mount_point_.c_str());
        if (rc != EOK) {
            dbg("Mount: ext4_journal_start failed rc=%d (non-fatal)", rc);
        } else {
            dbg("Mount: ext4_journal_start OK");
        }
    }

    // Create WinFsp filesystem
    FSP_FSCTL_VOLUME_PARAMS volume_params;
    std::memset(&volume_params, 0, sizeof(volume_params));
    volume_params.Version = sizeof(FSP_FSCTL_VOLUME_PARAMS);
    volume_params.SectorSize = 512;
    volume_params.SectorsPerAllocationUnit = 8;  // 4KB allocation unit
    volume_params.MaxComponentLength = 255;
    volume_params.FileInfoTimeout = 1000;  // cache metadata for 1 second
    volume_params.CaseSensitiveSearch = FALSE;
    volume_params.CasePreservedNames = TRUE;
    volume_params.UnicodeOnDisk = TRUE;
    volume_params.PersistentAcls = TRUE;
    volume_params.ReadOnlyVolume = read_only ? TRUE : FALSE;
    // Removed PostCleanupWhenModifiedOnly and PostDispositionWhenNecessaryOnly
    // as they caused duplicate OnClose calls from WinFsp internal threads.
    wcscpy_s(volume_params.FileSystemName,
             sizeof(volume_params.FileSystemName) / sizeof(WCHAR), L"ext4");

    dbg("Mount: calling FspFileSystemCreate");
    NTSTATUS status = FspFileSystemCreate(
        const_cast<PWSTR>(L"" FSP_FSCTL_DISK_DEVICE_NAME),
        &volume_params,
        &iface_,
        &fs_);

    if (!NT_SUCCESS(status)) {
        dbg("Mount: FspFileSystemCreate failed status=0x%08lX", status);
        ext4_umount(mount_point_.c_str());
        ext4_device_unregister(device_name_.c_str());
        bdev_ = nullptr;
        return status;
    }
    dbg("Mount: FspFileSystemCreate OK");

    fs_->UserContext = this;

    dbg("Mount: calling FspFileSystemSetMountPoint('%ls')", mount_point);
    status = FspFileSystemSetMountPoint(fs_, const_cast<PWSTR>(mount_point));
    if (!NT_SUCCESS(status)) {
        dbg("Mount: FspFileSystemSetMountPoint failed status=0x%08lX", status);
        FspFileSystemDelete(fs_);
        fs_ = nullptr;
        ext4_umount(mount_point_.c_str());
        ext4_device_unregister(device_name_.c_str());
        bdev_ = nullptr;
        return status;
    }
    dbg("Mount: FspFileSystemSetMountPoint OK");

    // Use 1 dispatcher thread. lwext4 is not thread-safe, so concurrent
    // callbacks corrupt its internal state. A global mutex protects all calls.
    dbg("Mount: calling FspFileSystemStartDispatcher");
    status = FspFileSystemStartDispatcher(fs_, 1);
    if (!NT_SUCCESS(status)) {
        dbg("Mount: FspFileSystemStartDispatcher failed status=0x%08lX", status);
        FspFileSystemDelete(fs_);
        fs_ = nullptr;
        ext4_umount(mount_point_.c_str());
        ext4_device_unregister(device_name_.c_str());
        bdev_ = nullptr;
        return status;
    }
    dbg("Mount: FspFileSystemStartDispatcher OK");

    return STATUS_SUCCESS;
}

void Ext4FileSystem::Unmount()
{
    if (fs_) {
        FspFileSystemStopDispatcher(fs_);
        FspFileSystemDelete(fs_);
        fs_ = nullptr;
    }

    // Free any deferred contexts
    for (auto* p : deferred_delete_)
        delete static_cast<Ext4FileContext*>(p);
    deferred_delete_.clear();

    if (bdev_) {
        // Stop journaling before unmount — flushes all pending transactions
        // to disk, ensuring the filesystem is in a consistent state.
        if (!read_only_) {
            int rc = ext4_journal_stop(mount_point_.c_str());
            dbg("Unmount: ext4_journal_stop rc=%d", rc);
        }

        ext4_cache_flush(mount_point_.c_str());
        ext4_umount(mount_point_.c_str());
        ext4_device_unregister(device_name_.c_str());
        bdev_ = nullptr;
    }
}

// --- WinFsp Callbacks ---

NTSTATUS NTAPI Ext4FileSystem::OnGetVolumeInfo(FSP_FILE_SYSTEM* FileSystem,
    FSP_FSCTL_VOLUME_INFO* VolumeInfo)
{
    auto* self = GetSelf(FileSystem);
    std::lock_guard<std::mutex> lock(Ext4FileSystem::global_ext4_mutex());
    dbg("GetVolumeInfo");
    struct ext4_mount_stats stats;
    std::memset(&stats, 0, sizeof(stats));

    int rc = ext4_mount_point_stats(self->mount_point_.c_str(), &stats);
    if (rc != EOK)
        return STATUS_UNSUCCESSFUL;

    VolumeInfo->TotalSize = stats.blocks_count * stats.block_size;
    VolumeInfo->FreeSize = stats.free_blocks_count * stats.block_size;

    // Volume label — read from the ext4 superblock.
    // The volume_name field is set when the filesystem is created
    // (e.g. `mkfs.ext4 -L "arch_root" /dev/sda2`). It's stored in
    // the superblock as a 16-byte UTF-8 string.
    //
    // If the volume has a label, we show it (e.g. "arch_root").
    // If it's empty, we show "Ext4 Volume" as a friendly default
    // instead of just "Local Disk".
    //
    // stats.volume_name is a char[16] from ext4_mount_point_stats().
    // Docs: https://github.com/gkostka/lwext4 (ext4.h, ext4_mount_stats)
    const char* label = stats.volume_name;
    wchar_t wlabel[64] = {};

    if (label[0] != '\0') {
        // Convert UTF-8 volume name to wide string for Windows
        MultiByteToWideChar(CP_UTF8, 0, label, -1, wlabel, 64);
    } else {
        wcscpy_s(wlabel, L"Ext4 Volume");
    }

    dbg("GetVolumeInfo: label='%s' total=%llu free=%llu",
        label, VolumeInfo->TotalSize, VolumeInfo->FreeSize);

    wcscpy_s(VolumeInfo->VolumeLabel,
             sizeof(VolumeInfo->VolumeLabel) / sizeof(WCHAR), wlabel);
    VolumeInfo->VolumeLabelLength =
        static_cast<UINT16>(wcslen(VolumeInfo->VolumeLabel) * sizeof(WCHAR));

    return STATUS_SUCCESS;
}

NTSTATUS Ext4FileSystem::FillFileInfo(const char* path,
    FSP_FSCTL_FILE_INFO* FileInfo)
{
    std::memset(FileInfo, 0, sizeof(*FileInfo));

    // Try to open as file first
    ext4_file f;
    std::memset(&f, 0, sizeof(f));

    bool is_directory = false;
    int rc = ext4_fopen(&f, path, "rb");
    if (rc == EOK) {
        // It's a file
        FileInfo->FileAttributes = FILE_ATTRIBUTE_NORMAL;
        FileInfo->FileSize = ext4_fsize(&f);
        FileInfo->AllocationSize =
            (FileInfo->FileSize + 4095) & ~4095ULL; // Round up to 4KB
        ext4_fclose(&f);
    } else {
        // Try as directory
        ext4_dir d;
        std::memset(&d, 0, sizeof(d));

        rc = ext4_dir_open(&d, path);
        if (rc != EOK)
            return STATUS_OBJECT_NAME_NOT_FOUND;

        FileInfo->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
        FileInfo->FileSize = 0;
        FileInfo->AllocationSize = 0;
        ext4_dir_close(&d);
        is_directory = true;
    }

    // ── Linux permission mapping ──────────────────────────────
    // Read the ext4 inode mode bits (standard POSIX format):
    //   Bits 0-2: others (rwx)
    //   Bits 3-5: group  (rwx)
    //   Bits 6-8: owner  (rwx)
    //   Bits 12-15: file type (regular, directory, symlink, etc.)
    //
    // In Python terms: os.stat(file).st_mode & 0o777 gives the
    // permission bits. 0o755 = rwxr-xr-x, 0o644 = rw-r--r--.
    //
    // We map these to Windows file attributes:
    //   - If owner has no write permission → FILE_ATTRIBUTE_READONLY
    //   - Hidden files (name starts with '.') → FILE_ATTRIBUTE_HIDDEN
    //
    // Docs: https://en.cppreference.com/w/cpp/filesystem/perms
    //       https://learn.microsoft.com/en-us/windows/win32/fileio/file-attribute-constants
    uint32_t mode = 0;
    if (ext4_mode_get(path, &mode) == EOK) {
        // Check owner write permission: bit 7 (0200 in octal)
        // This is like checking: (st_mode & stat.S_IWUSR) in Python
        bool owner_can_write = (mode & 0200) != 0;
        if (!owner_can_write && !is_directory) {
            FileInfo->FileAttributes |= FILE_ATTRIBUTE_READONLY;
        }
    }

    // Hidden files: in Linux, files starting with '.' are hidden.
    // Map this to FILE_ATTRIBUTE_HIDDEN on Windows so they don't
    // clutter the Explorer view (just like Linux file managers hide them).
    const char* basename = strrchr(path, '/');
    if (basename && basename[1] == '.') {
        FileInfo->FileAttributes |= FILE_ATTRIBUTE_HIDDEN;
    }

    // Read timestamps from ext4 (POSIX seconds → Windows FILETIME)
    // Windows FILETIME = 100-nanosecond intervals since 1601-01-01
    // POSIX time = seconds since 1970-01-01
    // Difference = 11644473600 seconds
    const uint64_t EPOCH_DIFF = 116444736000000000ULL; // in 100ns units

    uint32_t atime = 0, mtime = 0, ctime = 0, crtime = 0;
    ext4_atime_get(path, &atime);
    ext4_mtime_get(path, &mtime);
    ext4_ctime_get(path, &ctime);
    ext4_crtime_get(path, &crtime);  // real creation time (ext4 extended inode)

    // If crtime is not available (old filesystem), fall back to ctime
    if (crtime == 0)
        crtime = ctime;

    uint64_t atime_win  = static_cast<uint64_t>(atime)  * 10000000ULL + EPOCH_DIFF;
    uint64_t mtime_win  = static_cast<uint64_t>(mtime)  * 10000000ULL + EPOCH_DIFF;
    uint64_t ctime_win  = static_cast<uint64_t>(ctime)  * 10000000ULL + EPOCH_DIFF;
    uint64_t crtime_win = static_cast<uint64_t>(crtime) * 10000000ULL + EPOCH_DIFF;

    FileInfo->CreationTime   = crtime_win;  // ext4 crtime → Windows creation time
    FileInfo->LastAccessTime = atime_win;
    FileInfo->LastWriteTime  = mtime_win;
    FileInfo->ChangeTime     = ctime_win;   // ext4 ctime → Windows change time
    FileInfo->IndexNumber = 0;
    FileInfo->HardLinks = 1;
    FileInfo->ReparseTag = 0;

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI Ext4FileSystem::OnGetSecurityByName(FSP_FILE_SYSTEM* FileSystem,
    PWSTR FileName, PUINT32 PFileAttributes,
    PSECURITY_DESCRIPTOR SecurityDescriptor,
    SIZE_T* PSecurityDescriptorSize)
{

    auto* self = GetSelf(FileSystem);
    std::lock_guard<std::mutex> lock(Ext4FileSystem::global_ext4_mutex());
    std::string path = self->ToExt4Path(FileName);
    dbg("GetSecurityByName: '%s'", path.c_str());
    // Check if the file/directory exists and get attributes
    FSP_FSCTL_FILE_INFO file_info;
    std::memset(&file_info, 0, sizeof(file_info));

    NTSTATUS status = self->FillFileInfo(path.c_str(), &file_info);
    if (!NT_SUCCESS(status)) {

        return status;
    }

    if (PFileAttributes)
        *PFileAttributes = file_info.FileAttributes;

    // ── Security descriptor from ext4 permissions ──────────────
    // Build a Windows security descriptor based on the ext4 inode
    // mode bits. This tells Windows who can read/write/execute.
    //
    // SDDL (Security Descriptor Definition Language) is a string
    // format for security descriptors. Think of it like a mini-language
    // for expressing "who can do what" — similar to chmod in Linux.
    //
    // Format: "O:owner G:group D:(ACE)(ACE)..."
    //   O:BA = Owner is BUILTIN\Administrators
    //   G:BA = Group is BUILTIN\Administrators
    //   D:   = DACL (Discretionary Access Control List)
    //   A    = Allow
    //   GA   = Generic All (full access)
    //   GR   = Generic Read
    //   GX   = Generic Execute
    //   GW   = Generic Write
    //   WD   = World (Everyone)
    //   BA   = BUILTIN\Administrators
    //
    // We map ext4 permissions like this:
    //   Owner rwx → Administrators get corresponding access
    //   Others rwx → Everyone gets corresponding access
    //
    // Docs: https://learn.microsoft.com/en-us/windows/win32/secauthz/
    //       security-descriptor-definition-language
    if (PSecurityDescriptorSize) {
        // Read ext4 mode bits for this file
        uint32_t mode = 0;
        int mode_rc = ext4_mode_get(path.c_str(), &mode);
        dbg("  mode_get('%s') rc=%d mode=0%03o", path.c_str(), mode_rc, mode);

        // Build a Windows security descriptor from ext4 permissions.
        //
        // We use SDDL (Security Descriptor Definition Language) — a
        // string format that describes "who can do what". Think of it
        // like chmod notation but for Windows.
        //
        // We use NUMERIC access masks (hex) instead of symbolic rights
        // (like GR, GW) because concatenated symbolic rights (GRGWGX)
        // are unreliable across Windows versions.
        //
        // The access mask bits we care about:
        //   0x00120089 = FILE_GENERIC_READ (read files, list dirs)
        //   0x00120116 = FILE_GENERIC_WRITE (write files, create)
        //   0x001200A0 = FILE_GENERIC_EXECUTE (traverse dirs)
        //   0x001F01FF = FILE_ALL_ACCESS (full control)
        //
        // Docs: https://learn.microsoft.com/en-us/windows/win32/secauthz/
        //       security-descriptor-definition-language
        // Docs: https://learn.microsoft.com/en-us/windows/win32/fileio/
        //       file-access-rights-constants

        // Map ext4 owner bits (6-8) to a Windows access mask
        DWORD owner_mask = 0;
        if (mode & 0400) owner_mask |= 0x00120089;  // read
        if (mode & 0200) owner_mask |= 0x00120116;  // write
        if (mode & 0100) owner_mask |= 0x001200A0;  // execute
        if (owner_mask == 0) owner_mask = 0x00120089; // fallback: read

        // Map ext4 others bits (0-2) to a Windows access mask
        DWORD other_mask = 0;
        if (mode & 0004) other_mask |= 0x00120089;  // read
        if (mode & 0002) other_mask |= 0x00120116;  // write
        if (mode & 0001) other_mask |= 0x001200A0;  // execute
        if (other_mask == 0) other_mask = 0x00120089; // fallback: read

        // Build SDDL string:
        //   O:WD   = Owner is Everyone (WD = World)
        //   G:WD   = Group is Everyone
        //   D:P    = Protected DACL (won't inherit from parent)
        //   (A;;mask;;;BA) = Allow Administrators the owner-level access
        //   (A;;mask;;;WD) = Allow Everyone the others-level access
        wchar_t sddl[256];
        swprintf(sddl, 256,
            L"O:WDG:WDD:P(A;;0x%08lx;;;BA)(A;;0x%08lx;;;WD)",
            owner_mask, other_mask);

        dbg("  SDDL: '%ls'", sddl);

        PSECURITY_DESCRIPTOR sd = nullptr;
        ULONG sd_size = 0;

        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl, SDDL_REVISION_1, &sd, &sd_size))
        {
            dbg("  SDDL parse FAILED err=%lu, using full-access fallback",
                GetLastError());
            // Fallback: full access to Everyone
            ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"O:WDG:WDD:P(A;;0x001f01ff;;;WD)", SDDL_REVISION_1,
                &sd, &sd_size);
        }

        if (*PSecurityDescriptorSize < sd_size) {
            *PSecurityDescriptorSize = sd_size;
            LocalFree(sd);
            return STATUS_BUFFER_OVERFLOW;
        }

        *PSecurityDescriptorSize = sd_size;
        if (SecurityDescriptor)
            memcpy(SecurityDescriptor, sd, sd_size);

        LocalFree(sd);
    }


    return STATUS_SUCCESS;
}

NTSTATUS NTAPI Ext4FileSystem::OnCreate(FSP_FILE_SYSTEM* FileSystem,
    PWSTR FileName, UINT32 CreateOptions, UINT32 GrantedAccess,
    UINT32 FileAttributes, PSECURITY_DESCRIPTOR SecurityDescriptor,
    UINT64 AllocationSize,
    PVOID* PFileContext, FSP_FSCTL_FILE_INFO* FileInfo)
{
    auto* self = GetSelf(FileSystem);
    std::lock_guard<std::mutex> lock(Ext4FileSystem::global_ext4_mutex());

    // NOTE: We intentionally do NOT free deferred_delete_ contexts here.
    // Freeing memory allows 'new' to reuse the same address, which causes
    // the ABA problem: a stale WinFsp Close call targets the new context
    // at the recycled address. By never freeing, addresses are unique and
    // stale Closes are always caught by the valid_contexts_ check.
    // Memory is freed on Unmount. ~100 bytes per Open, negligible.

    std::string path = self->ToExt4Path(FileName);
    bool is_directory = (CreateOptions & FILE_DIRECTORY_FILE) != 0;
    dbg("Create: '%s' is_dir=%d", path.c_str(), (int)is_directory);

    if (is_directory) {
        // Create directory on the ext4 filesystem
        int rc = ext4_dir_mk(path.c_str());
        if (rc != EOK)
            return STATUS_UNSUCCESSFUL;
    } else {
        // Create file by opening in "wb" mode (truncate/create)
        ext4_file f;
        std::memset(&f, 0, sizeof(f));

        int rc = ext4_fopen(&f, path.c_str(), "wb");
        if (rc != EOK)
            return STATUS_UNSUCCESSFUL;
        ext4_fclose(&f);
    }

    // Don't keep ext4 handles open — each callback opens its own.
    NTSTATUS status = self->FillFileInfo(path.c_str(), FileInfo);
    if (!NT_SUCCESS(status))
        return status;

    auto* ctx = new Ext4FileContext{};
    ctx->path.assign(FileName);
    ctx->is_directory = is_directory;
    std::memset(&ctx->file, 0, sizeof(ctx->file));
    std::memset(&ctx->dir, 0, sizeof(ctx->dir));

    *PFileContext = ctx;
    self->valid_contexts_.insert(ctx);

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI Ext4FileSystem::OnOverwrite(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, UINT32 FileAttributes, BOOLEAN ReplaceFileAttributes,
    UINT64 AllocationSize, FSP_FSCTL_FILE_INFO* FileInfo)
{
    // Overwrite: truncate existing file to 0 bytes and reopen for writing.
    auto* self = GetSelf(FileSystem);
    std::lock_guard<std::mutex> lock(Ext4FileSystem::global_ext4_mutex());
    auto* ctx = static_cast<Ext4FileContext*>(FileContext);
    if (!ctx || ctx->is_directory)
        return STATUS_INVALID_DEVICE_REQUEST;

    std::string path = self->ToExt4Path(ctx->path.c_str());
    dbg("Overwrite: '%s'", path.c_str());

    // Truncate file to 0 by opening in "wb" mode (write-only, truncate)
    ext4_file f;
    std::memset(&f, 0, sizeof(f));

    int rc = ext4_fopen(&f, path.c_str(), "wb");
    if (rc != EOK)
        return STATUS_UNSUCCESSFUL;
    ext4_fclose(&f);

    return self->FillFileInfo(path.c_str(), FileInfo);
}

NTSTATUS NTAPI Ext4FileSystem::OnOpen(FSP_FILE_SYSTEM* FileSystem,
    PWSTR FileName, UINT32 CreateOptions, UINT32 GrantedAccess,
    PVOID* PFileContext, FSP_FSCTL_FILE_INFO* FileInfo)
{

    auto* self = GetSelf(FileSystem);
    std::lock_guard<std::mutex> lock(Ext4FileSystem::global_ext4_mutex());

    // NOTE: We intentionally do NOT free deferred_delete_ contexts here.
    // See OnCreate comment for the ABA problem explanation.

    std::string path = self->ToExt4Path(FileName);
    dbg("Open: '%s'", path.c_str());

    // Check if the path exists by trying to open as directory, then as file.
    // We don't keep the handle open — handles are opened per-operation
    // in OnRead/OnWrite/OnReadDirectory to avoid leak from WinFsp
    // sending mismatched Close calls.
    bool is_dir = false;
    {
        ext4_dir d;
        std::memset(&d, 0, sizeof(d));
        if (ext4_dir_open(&d, path.c_str()) == EOK) {
            is_dir = true;
            ext4_dir_close(&d);
        } else {
            ext4_file f;
            std::memset(&f, 0, sizeof(f));
            int rc = ext4_fopen(&f, path.c_str(), "rb");
            if (rc != EOK) {
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }
            ext4_fclose(&f);
        }
    }

    NTSTATUS status = self->FillFileInfo(path.c_str(), FileInfo);
    if (!NT_SUCCESS(status))
        return status;

    auto* ctx = new Ext4FileContext{};
    ctx->path.assign(FileName);
    ctx->is_directory = is_dir;
    std::memset(&ctx->file, 0, sizeof(ctx->file));
    std::memset(&ctx->dir, 0, sizeof(ctx->dir));

    *PFileContext = ctx;
    self->valid_contexts_.insert(ctx);

    return STATUS_SUCCESS;
}

VOID NTAPI Ext4FileSystem::OnClose(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext)
{
    auto* self = GetSelf(FileSystem);
    std::lock_guard<std::mutex> lock(Ext4FileSystem::global_ext4_mutex());
    auto* ctx = static_cast<Ext4FileContext*>(FileContext);
    if (!ctx) return;

    // Guard against double-close: WinFsp internal threads may call
    // OnClose twice for the same context pointer.
    if (self->valid_contexts_.find(ctx) == self->valid_contexts_.end()) {
        dbg("Close: skipping double-close ctx=%p", (void*)ctx);
        return;
    }
    self->valid_contexts_.erase(ctx);

    dbg("Close: '%ls' is_dir=%d closed=%d",
        ctx->path.c_str(), (int)ctx->is_directory, (int)ctx->closed);

    // No ext4 handles to close — OnOpen doesn't keep them open.
    // Defer freeing the memory so stale Close calls from WinFsp
    // don't hit a reused address.
    self->deferred_delete_.push_back(ctx);
}

NTSTATUS NTAPI Ext4FileSystem::OnRead(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, PVOID Buffer, UINT64 Offset, ULONG Length,
    PULONG PBytesTransferred)
{

    auto* self = GetSelf(FileSystem);
    std::lock_guard<std::mutex> lock(Ext4FileSystem::global_ext4_mutex());
    auto* ctx = static_cast<Ext4FileContext*>(FileContext);
    if (!ctx || ctx->is_directory)
        return STATUS_INVALID_DEVICE_REQUEST;
    std::string path = self->ToExt4Path(ctx->path.c_str());
    dbg("Read: '%s' offset=%llu len=%lu", path.c_str(), Offset, Length);

    // Reopen for reading at the right offset
    ext4_file f;
    std::memset(&f, 0, sizeof(f));

    int rc = ext4_fopen(&f, path.c_str(), "rb");
    if (rc != EOK) {
        dbg("Read: fopen failed rc=%d", rc);
        return STATUS_UNSUCCESSFUL;
    }

    rc = ext4_fseek(&f, Offset, SEEK_SET);
    if (rc != EOK) {
        dbg("Read: fseek failed rc=%d", rc);
        ext4_fclose(&f);
        return STATUS_UNSUCCESSFUL;
    }

    size_t bytes_read = 0;
    rc = ext4_fread(&f, Buffer, Length, &bytes_read);
    dbg("Read: fread rc=%d bytes_read=%zu", rc, bytes_read);
    ext4_fclose(&f);

    if (rc != EOK && rc != ENODATA)
        return STATUS_UNSUCCESSFUL;

    *PBytesTransferred = static_cast<ULONG>(bytes_read);

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI Ext4FileSystem::OnWrite(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, PVOID Buffer, UINT64 Offset, ULONG Length,
    BOOLEAN WriteToEndOfFile, BOOLEAN ConstrainedIo,
    PULONG PBytesTransferred, FSP_FSCTL_FILE_INFO* FileInfo)
{
    auto* self = GetSelf(FileSystem);
    std::lock_guard<std::mutex> lock(Ext4FileSystem::global_ext4_mutex());
    auto* ctx = static_cast<Ext4FileContext*>(FileContext);
    if (!ctx || ctx->is_directory)
        return STATUS_INVALID_DEVICE_REQUEST;

    std::string path = self->ToExt4Path(ctx->path.c_str());
    dbg("Write: '%s' offset=%llu len=%lu eof=%d constrained=%d",
        path.c_str(), Offset, Length, (int)WriteToEndOfFile, (int)ConstrainedIo);

    // Reopen file for each write (lwext4 seek is unreliable on open handles)
    ext4_file f;
    std::memset(&f, 0, sizeof(f));

    int rc = ext4_fopen(&f, path.c_str(), "r+b");
    if (rc != EOK)
        return STATUS_UNSUCCESSFUL;

    uint64_t file_size = ext4_fsize(&f);
    uint64_t write_offset = WriteToEndOfFile ? file_size : Offset;
    ULONG write_length = Length;

    // ConstrainedIo: cannot write beyond current file size
    if (ConstrainedIo) {
        if (write_offset >= file_size) {
            ext4_fclose(&f);
            *PBytesTransferred = 0;
            return STATUS_SUCCESS;
        }
        if (write_offset + write_length > file_size)
            write_length = static_cast<ULONG>(file_size - write_offset);
    }

    rc = ext4_fseek(&f, write_offset, SEEK_SET);
    if (rc != EOK) {
        ext4_fclose(&f);
        return STATUS_UNSUCCESSFUL;
    }

    size_t bytes_written = 0;
    rc = ext4_fwrite(&f, Buffer, write_length, &bytes_written);
    ext4_fclose(&f);

    if (rc != EOK)
        return STATUS_UNSUCCESSFUL;

    *PBytesTransferred = static_cast<ULONG>(bytes_written);

    return self->FillFileInfo(path.c_str(), FileInfo);
}

NTSTATUS NTAPI Ext4FileSystem::OnReadDirectory(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, PWSTR Pattern, PWSTR Marker,
    PVOID Buffer, ULONG Length, PULONG PBytesTransferred)
{

    auto* self = GetSelf(FileSystem);
    std::lock_guard<std::mutex> lock(Ext4FileSystem::global_ext4_mutex());
    auto* ctx = static_cast<Ext4FileContext*>(FileContext);
    if (!ctx || !ctx->is_directory)
        return STATUS_INVALID_DEVICE_REQUEST;
    std::string dir_path = self->ToExt4Path(ctx->path.c_str());
    dbg("ReadDirectory: '%s'", dir_path.c_str());

    // Open directory locally for enumeration — we don't keep handles
    // open in the context to avoid leaks from WinFsp mismatched Close calls.
    ext4_dir d;
    std::memset(&d, 0, sizeof(d));

    int rc = ext4_dir_open(&d, dir_path.c_str());
    if (rc != EOK)
        return STATUS_UNSUCCESSFUL;

    bool past_marker = (Marker == nullptr);
    const ext4_direntry* entry;

    while ((entry = ext4_dir_entry_next(&d)) != nullptr) {
        // Skip . and ..
        if (entry->name_length == 1 && entry->name[0] == '.')
            continue;
        if (entry->name_length == 2 && entry->name[0] == '.' && entry->name[1] == '.')
            continue;

        // Convert entry name to wide string
        char name_buf[256] = {};
        std::memcpy(name_buf, entry->name, entry->name_length);
        name_buf[entry->name_length] = '\0';

        wchar_t wide_name[256] = {};
        MultiByteToWideChar(CP_UTF8, 0, name_buf, -1, wide_name, 256);

        // Skip entries before the marker
        if (!past_marker) {
            if (wcscmp(wide_name, Marker) == 0)
                past_marker = true;
            continue;
        }

        // Build full path for stat
        std::string entry_path = dir_path;
        if (entry_path.back() != '/')
            entry_path += '/';
        entry_path += name_buf;

        // Get file info
        FSP_FSCTL_FILE_INFO file_info;
        std::memset(&file_info, 0, sizeof(file_info));
        self->FillFileInfo(entry_path.c_str(), &file_info);

        // Build dir info entry
        union {
            UINT8 bytes[sizeof(FSP_FSCTL_DIR_INFO) + 256 * sizeof(WCHAR)];
            FSP_FSCTL_DIR_INFO info;
        } dir_info_buf;

        std::memset(&dir_info_buf, 0, sizeof(dir_info_buf));
        dir_info_buf.info.Size = static_cast<UINT16>(
            offsetof(FSP_FSCTL_DIR_INFO, FileNameBuf) +
            wcslen(wide_name) * sizeof(WCHAR));
        dir_info_buf.info.FileInfo = file_info;
        wcscpy_s(dir_info_buf.info.FileNameBuf, 256, wide_name);

        if (!FspFileSystemAddDirInfo(&dir_info_buf.info, Buffer, Length,
                                      PBytesTransferred))
            break;
    }

    // Signal end of directory
    FspFileSystemAddDirInfo(nullptr, Buffer, Length, PBytesTransferred);

    ext4_dir_close(&d);

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI Ext4FileSystem::OnGetFileInfo(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, FSP_FSCTL_FILE_INFO* FileInfo)
{

    auto* self = GetSelf(FileSystem);
    std::lock_guard<std::mutex> lock(Ext4FileSystem::global_ext4_mutex());
    auto* ctx = static_cast<Ext4FileContext*>(FileContext);
    if (!ctx) return STATUS_INVALID_HANDLE;
    std::string path = self->ToExt4Path(ctx->path.c_str());
    dbg("GetFileInfo: '%s'", path.c_str());

    NTSTATUS r = self->FillFileInfo(path.c_str(), FileInfo);

    return r;
}

NTSTATUS NTAPI Ext4FileSystem::OnSetBasicInfo(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, UINT32 FileAttributes,
    UINT64 CreationTime, UINT64 LastAccessTime, UINT64 LastWriteTime,
    UINT64 ChangeTime, FSP_FSCTL_FILE_INFO* FileInfo)
{

    auto* self = GetSelf(FileSystem);
    std::lock_guard<std::mutex> lock(Ext4FileSystem::global_ext4_mutex());

    auto* ctx = static_cast<Ext4FileContext*>(FileContext);
    if (!ctx) return STATUS_INVALID_HANDLE;

    std::string path = self->ToExt4Path(ctx->path.c_str());
    dbg("SetBasicInfo: '%s'", path.c_str());

    if (self->read_only_)
        return self->FillFileInfo(path.c_str(), FileInfo);

    // Windows-to-POSIX epoch difference
    const uint64_t EPOCH_DIFF = 116444736000000000ULL;

    // Convert FILETIME to POSIX seconds
    if (LastAccessTime != 0) {
        uint32_t atime = static_cast<uint32_t>((LastAccessTime - EPOCH_DIFF) / 10000000ULL);
        ext4_atime_set(path.c_str(), atime);
    }

    if (LastWriteTime != 0) {
        uint32_t mtime = static_cast<uint32_t>((LastWriteTime - EPOCH_DIFF) / 10000000ULL);
        ext4_mtime_set(path.c_str(), mtime);
    }

    if (CreationTime != 0) {
        uint32_t crtime = static_cast<uint32_t>((CreationTime - EPOCH_DIFF) / 10000000ULL);
        ext4_crtime_set(path.c_str(), crtime);  // write to ext4 crtime, not ctime
    }

    if (ChangeTime != 0) {
        uint32_t ctime = static_cast<uint32_t>((ChangeTime - EPOCH_DIFF) / 10000000ULL);
        ext4_ctime_set(path.c_str(), ctime);
    }

    NTSTATUS r2 = self->FillFileInfo(path.c_str(), FileInfo);

    return r2;
}

NTSTATUS NTAPI Ext4FileSystem::OnSetFileSize(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, UINT64 NewSize, BOOLEAN SetAllocationSize,
    FSP_FSCTL_FILE_INFO* FileInfo)
{

    auto* self = GetSelf(FileSystem);
    std::lock_guard<std::mutex> lock(Ext4FileSystem::global_ext4_mutex());

    auto* ctx = static_cast<Ext4FileContext*>(FileContext);
    if (!ctx || ctx->is_directory)
        return STATUS_INVALID_DEVICE_REQUEST;

    dbg("SetFileSize: '%ls' new_size=%llu alloc=%d",
        ctx->path.c_str(), NewSize, (int)SetAllocationSize);

    if (SetAllocationSize)
        return self->FillFileInfo(
            self->ToExt4Path(ctx->path.c_str()).c_str(), FileInfo);
    std::string path = self->ToExt4Path(ctx->path.c_str());

    ext4_file f;
    std::memset(&f, 0, sizeof(f));

    int rc = ext4_fopen(&f, path.c_str(), "r+b");
    if (rc != EOK)
        return STATUS_UNSUCCESSFUL;

    rc = ext4_ftruncate(&f, NewSize);
    ext4_fclose(&f);

    if (rc != EOK)
        return STATUS_UNSUCCESSFUL;

    return self->FillFileInfo(path.c_str(), FileInfo);
}

NTSTATUS NTAPI Ext4FileSystem::OnCanDelete(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, PWSTR FileName)
{
    // Check if a file/directory can be deleted (directories must be empty).
    auto* self = GetSelf(FileSystem);
    std::lock_guard<std::mutex> lock(Ext4FileSystem::global_ext4_mutex());
    auto* ctx = static_cast<Ext4FileContext*>(FileContext);
    if (!ctx) return STATUS_INVALID_HANDLE;
    dbg("CanDelete: '%ls' is_dir=%d", ctx->path.c_str(), (int)ctx->is_directory);

    if (ctx->is_directory) {
        std::string path = self->ToExt4Path(ctx->path.c_str());

        // Check if directory is empty
        ext4_dir d;
        std::memset(&d, 0, sizeof(d));

        int rc = ext4_dir_open(&d, path.c_str());
        if (rc != EOK)
            return STATUS_UNSUCCESSFUL;

        const ext4_direntry* entry;
        bool has_entries = false;

        while ((entry = ext4_dir_entry_next(&d)) != nullptr) {
            // Skip . and ..
            if (entry->name_length == 1 && entry->name[0] == '.')
                continue;
            if (entry->name_length == 2 && entry->name[0] == '.' && entry->name[1] == '.')
                continue;

            has_entries = true;
            break;
        }

        ext4_dir_close(&d);

        if (has_entries)
            return STATUS_DIRECTORY_NOT_EMPTY;
    }

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI Ext4FileSystem::OnRename(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, PWSTR FileName, PWSTR NewFileName,
    BOOLEAN ReplaceIfExists)
{
    // Rename or move a file/directory.
    // lwext4 cannot rename a file while it has an open handle, so we must
    // close the handle first, perform the rename, then reopen with the new path.
    auto* self = GetSelf(FileSystem);
    std::lock_guard<std::mutex> lock(Ext4FileSystem::global_ext4_mutex());
    auto* ctx = static_cast<Ext4FileContext*>(FileContext);

    // Use ctx->path for the old path — WinFsp may pass FileName in
    // uppercase, but ext4 is case-sensitive and needs the original case.
    std::string old_path = ctx ? self->ToExt4Path(ctx->path.c_str())
                               : self->ToExt4Path(FileName);
    std::string new_path = self->ToExt4Path(NewFileName);
    dbg("Rename: '%s' -> '%s' replace=%d",
        old_path.c_str(), new_path.c_str(), (int)ReplaceIfExists);

    // Check if destination already exists
    if (!ReplaceIfExists) {
        ext4_file f;
        std::memset(&f, 0, sizeof(f));

        if (ext4_fopen(&f, new_path.c_str(), "rb") == EOK) {
            ext4_fclose(&f);
            return STATUS_OBJECT_NAME_COLLISION;
        }

        ext4_dir d;
        std::memset(&d, 0, sizeof(d));

        if (ext4_dir_open(&d, new_path.c_str()) == EOK) {
            ext4_dir_close(&d);
            return STATUS_OBJECT_NAME_COLLISION;
        }
    }

    // No ext4 handles to close — we don't keep them open.
    int rc = ext4_frename(old_path.c_str(), new_path.c_str());
    if (rc != EOK)
        return STATUS_UNSUCCESSFUL;

    // Update the stored path so future callbacks use the new name
    if (ctx)
        ctx->path.assign(NewFileName);

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI Ext4FileSystem::OnFlush(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, FSP_FSCTL_FILE_INFO* FileInfo)
{

    auto* self = GetSelf(FileSystem);
    std::lock_guard<std::mutex> lock(Ext4FileSystem::global_ext4_mutex());
    dbg("Flush");
    if (!self->read_only_)
        ext4_cache_flush(self->mount_point_.c_str());

    if (FileContext && FileInfo) {
        auto* ctx = static_cast<Ext4FileContext*>(FileContext);
        std::string path = self->ToExt4Path(ctx->path.c_str());
        return self->FillFileInfo(path.c_str(), FileInfo);
    }

    return STATUS_SUCCESS;
}

VOID NTAPI Ext4FileSystem::OnCleanup(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, PWSTR FileName, ULONG Flags)
{

    if (!FileContext || !FileName)
        return;

    auto* self = GetSelf(FileSystem);
    std::lock_guard<std::mutex> lock(Ext4FileSystem::global_ext4_mutex());
    auto* ctx = static_cast<Ext4FileContext*>(FileContext);
    std::string path = self->ToExt4Path(ctx->path.c_str());
    dbg("Cleanup: '%s' flags=0x%lx delete=%d",
        path.c_str(), Flags, (int)((Flags & FspCleanupDelete) != 0));

    // If marked for deletion, remove from disk.
    // No ext4 handles to close — we don't keep them open.
    // Do NOT delete ctx here — OnClose will be called next by WinFsp.
    if (Flags & FspCleanupDelete) {
        if (ctx->is_directory)
            ext4_dir_rm(path.c_str());
        else
            ext4_fremove(path.c_str());

        ext4_cache_flush(self->mount_point_.c_str());
        ctx->closed = true;
    }

}
