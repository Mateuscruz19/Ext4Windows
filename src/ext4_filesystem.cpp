#include "ext4_filesystem.hpp"

#include <cstring>
#include <algorithm>
#include <vector>

const char* Ext4FileSystem::MOUNT_POINT = "/";

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

// Convert Windows path (backslashes) to ext4 path (forward slashes)
std::string Ext4FileSystem::ToExt4Path(const wchar_t* win_path)
{
    std::string path = WideToUtf8(win_path);

    // Replace backslashes with forward slashes
    std::replace(path.begin(), path.end(), '\\', '/');

    // Ensure leading slash
    if (path.empty() || path[0] != '/')
        path = "/" + path;

    return path;
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
    iface_.ReadDirectory = &Ext4FileSystem::OnReadDirectory;
    iface_.GetFileInfo = &Ext4FileSystem::OnGetFileInfo;
    iface_.Cleanup = &Ext4FileSystem::OnCleanup;
}

Ext4FileSystem::~Ext4FileSystem()
{
    Unmount();
}

NTSTATUS Ext4FileSystem::Mount(struct ext4_blockdev* bdev, const wchar_t* mount_point,
                                bool read_only)
{
    bdev_ = bdev;
    read_only_ = read_only;

    // Register and mount via lwext4
    int rc = ext4_device_register(bdev_, "ext4dev");
    if (rc != EOK)
        return STATUS_UNSUCCESSFUL;

    rc = ext4_mount("ext4dev", MOUNT_POINT, read_only);
    if (rc != EOK) {
        ext4_device_unregister("ext4dev");
        return STATUS_UNSUCCESSFUL;
    }

    // Create WinFsp filesystem
    FSP_FSCTL_VOLUME_PARAMS volume_params;
    std::memset(&volume_params, 0, sizeof(volume_params));
    volume_params.Version = sizeof(FSP_FSCTL_VOLUME_PARAMS);
    volume_params.SectorSize = 512;
    volume_params.SectorsPerAllocationUnit = 8;  // 4KB allocation unit
    volume_params.MaxComponentLength = 255;
    volume_params.FileInfoTimeout = 1000;
    volume_params.CaseSensitiveSearch = FALSE;
    volume_params.CasePreservedNames = TRUE;
    volume_params.UnicodeOnDisk = TRUE;
    volume_params.PersistentAcls = TRUE;
    volume_params.ReadOnlyVolume = read_only ? TRUE : FALSE;
    volume_params.PostCleanupWhenModifiedOnly = TRUE;
    volume_params.PostDispositionWhenNecessaryOnly = TRUE;
    volume_params.RejectIrpPriorToTransact0 = TRUE;
    wcscpy_s(volume_params.FileSystemName,
             sizeof(volume_params.FileSystemName) / sizeof(WCHAR), L"ext4");

    NTSTATUS status = FspFileSystemCreate(
        const_cast<PWSTR>(L"" FSP_FSCTL_DISK_DEVICE_NAME),
        &volume_params,
        &iface_,
        &fs_);

    if (!NT_SUCCESS(status)) {
        ext4_umount(MOUNT_POINT);
        ext4_device_unregister("ext4dev");
        return status;
    }

    fs_->UserContext = this;

    status = FspFileSystemSetMountPoint(fs_, const_cast<PWSTR>(mount_point));
    if (!NT_SUCCESS(status)) {
        FspFileSystemDelete(fs_);
        fs_ = nullptr;
        ext4_umount(MOUNT_POINT);
        ext4_device_unregister("ext4dev");
        return status;
    }

    status = FspFileSystemStartDispatcher(fs_, 0);
    if (!NT_SUCCESS(status)) {
        FspFileSystemDelete(fs_);
        fs_ = nullptr;
        ext4_umount(MOUNT_POINT);
        ext4_device_unregister("ext4dev");
        return status;
    }

    return STATUS_SUCCESS;
}

void Ext4FileSystem::Unmount()
{
    if (fs_) {
        FspFileSystemStopDispatcher(fs_);
        FspFileSystemDelete(fs_);
        fs_ = nullptr;
    }

    if (bdev_) {
        ext4_umount(MOUNT_POINT);
        ext4_device_unregister("ext4dev");
        bdev_ = nullptr;
    }
}

// --- WinFsp Callbacks ---

NTSTATUS NTAPI Ext4FileSystem::OnGetVolumeInfo(FSP_FILE_SYSTEM* FileSystem,
    FSP_FSCTL_VOLUME_INFO* VolumeInfo)
{
    struct ext4_mount_stats stats;
    std::memset(&stats, 0, sizeof(stats));

    int rc = ext4_mount_point_stats(MOUNT_POINT, &stats);
    if (rc != EOK)
        return STATUS_UNSUCCESSFUL;

    VolumeInfo->TotalSize = stats.blocks_count * stats.block_size;
    VolumeInfo->FreeSize = stats.free_blocks_count * stats.block_size;

    // Volume label
    wcscpy_s(VolumeInfo->VolumeLabel,
             sizeof(VolumeInfo->VolumeLabel) / sizeof(WCHAR), L"ext4");
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
    }

    // Read timestamps from ext4 (POSIX seconds → Windows FILETIME)
    // Windows FILETIME = 100-nanosecond intervals since 1601-01-01
    // POSIX time = seconds since 1970-01-01
    // Difference = 11644473600 seconds
    const uint64_t EPOCH_DIFF = 116444736000000000ULL; // in 100ns units

    uint32_t atime = 0, mtime = 0, ctime = 0;
    ext4_atime_get(path, &atime);
    ext4_mtime_get(path, &mtime);
    ext4_ctime_get(path, &ctime);

    uint64_t atime_win = static_cast<uint64_t>(atime) * 10000000ULL + EPOCH_DIFF;
    uint64_t mtime_win = static_cast<uint64_t>(mtime) * 10000000ULL + EPOCH_DIFF;
    uint64_t ctime_win = static_cast<uint64_t>(ctime) * 10000000ULL + EPOCH_DIFF;

    FileInfo->CreationTime = ctime_win;
    FileInfo->LastAccessTime = atime_win;
    FileInfo->LastWriteTime = mtime_win;
    FileInfo->ChangeTime = mtime_win;
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
    std::string path = self->ToExt4Path(FileName);

    // Check if the file/directory exists and get attributes
    FSP_FSCTL_FILE_INFO file_info;
    std::memset(&file_info, 0, sizeof(file_info));

    NTSTATUS status = self->FillFileInfo(path.c_str(), &file_info);
    if (!NT_SUCCESS(status))
        return status;

    if (PFileAttributes)
        *PFileAttributes = file_info.FileAttributes;

    // Build a security descriptor that grants everyone full access.
    // We use SDDL: D:P(A;;GA;;;WD) meaning "Allow Generic All to World (Everyone)"
    if (PSecurityDescriptorSize) {
        // Create the SD from SDDL string
        PSECURITY_DESCRIPTOR sd = nullptr;
        ULONG sd_size = 0;

        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"O:BAG:BAD:(A;;0x1f01ff;;;WD)", SDDL_REVISION_1, &sd, &sd_size))
            return STATUS_UNSUCCESSFUL;

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
    // Read-only filesystem: cannot create new files
    return STATUS_ACCESS_DENIED;
}

NTSTATUS NTAPI Ext4FileSystem::OnOverwrite(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, UINT32 FileAttributes, BOOLEAN ReplaceFileAttributes,
    UINT64 AllocationSize, FSP_FSCTL_FILE_INFO* FileInfo)
{
    // Read-only filesystem: cannot overwrite files
    return STATUS_ACCESS_DENIED;
}

NTSTATUS NTAPI Ext4FileSystem::OnOpen(FSP_FILE_SYSTEM* FileSystem,
    PWSTR FileName, UINT32 CreateOptions, UINT32 GrantedAccess,
    PVOID* PFileContext, FSP_FSCTL_FILE_INFO* FileInfo)
{
    auto* self = GetSelf(FileSystem);
    std::string path = self->ToExt4Path(FileName);

    auto* ctx = new Ext4FileContext{};
    ctx->path.assign(FileName);

    // Try to determine if it's a directory or file
    ext4_dir d;
    std::memset(&d, 0, sizeof(d));

    if (ext4_dir_open(&d, path.c_str()) == EOK) {
        ctx->is_directory = true;
        ctx->dir = d;
    } else {
        ctx->is_directory = false;
        std::memset(&ctx->file, 0, sizeof(ctx->file));

        int rc = ext4_fopen(&ctx->file, path.c_str(), "rb");
        if (rc != EOK) {
            delete ctx;
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
    }

    NTSTATUS status = self->FillFileInfo(path.c_str(), FileInfo);
    if (!NT_SUCCESS(status)) {
        if (ctx->is_directory)
            ext4_dir_close(&ctx->dir);
        else
            ext4_fclose(&ctx->file);
        delete ctx;
        return status;
    }

    *PFileContext = ctx;
    return STATUS_SUCCESS;
}

VOID NTAPI Ext4FileSystem::OnClose(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext)
{
    auto* ctx = static_cast<Ext4FileContext*>(FileContext);
    if (!ctx) return;

    if (ctx->is_directory)
        ext4_dir_close(&ctx->dir);
    else
        ext4_fclose(&ctx->file);

    delete ctx;
}

NTSTATUS NTAPI Ext4FileSystem::OnRead(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, PVOID Buffer, UINT64 Offset, ULONG Length,
    PULONG PBytesTransferred)
{
    auto* ctx = static_cast<Ext4FileContext*>(FileContext);
    if (!ctx || ctx->is_directory)
        return STATUS_INVALID_DEVICE_REQUEST;

    auto* self = GetSelf(FileSystem);
    std::string path = self->ToExt4Path(ctx->path.c_str());

    // Reopen for reading at the right offset
    ext4_file f;
    std::memset(&f, 0, sizeof(f));

    int rc = ext4_fopen(&f, path.c_str(), "rb");
    if (rc != EOK)
        return STATUS_UNSUCCESSFUL;

    rc = ext4_fseek(&f, Offset, SEEK_SET);
    if (rc != EOK) {
        ext4_fclose(&f);
        return STATUS_UNSUCCESSFUL;
    }

    size_t bytes_read = 0;
    rc = ext4_fread(&f, Buffer, Length, &bytes_read);
    ext4_fclose(&f);

    if (rc != EOK && rc != ENODATA)
        return STATUS_UNSUCCESSFUL;

    *PBytesTransferred = static_cast<ULONG>(bytes_read);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI Ext4FileSystem::OnReadDirectory(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, PWSTR Pattern, PWSTR Marker,
    PVOID Buffer, ULONG Length, PULONG PBytesTransferred)
{
    auto* ctx = static_cast<Ext4FileContext*>(FileContext);
    if (!ctx || !ctx->is_directory)
        return STATUS_INVALID_DEVICE_REQUEST;

    auto* self = GetSelf(FileSystem);
    std::string dir_path = self->ToExt4Path(ctx->path.c_str());

    // Reopen directory to iterate from the beginning
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

    ext4_dir_close(&d);

    // Signal end of directory
    FspFileSystemAddDirInfo(nullptr, Buffer, Length, PBytesTransferred);

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI Ext4FileSystem::OnGetFileInfo(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, FSP_FSCTL_FILE_INFO* FileInfo)
{
    auto* ctx = static_cast<Ext4FileContext*>(FileContext);
    if (!ctx) return STATUS_INVALID_HANDLE;

    auto* self = GetSelf(FileSystem);
    std::string path = self->ToExt4Path(ctx->path.c_str());

    return self->FillFileInfo(path.c_str(), FileInfo);
}

VOID NTAPI Ext4FileSystem::OnCleanup(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, PWSTR FileName, ULONG Flags)
{
    // Nothing to clean up for read-only filesystem
}
