#include "ext4_filesystem.hpp"

#include <cstdio>
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
                                bool read_only)
{
    bdev_ = bdev;
    read_only_ = read_only;

    // Register and mount via lwext4
    int rc = ext4_device_register(bdev_, "ext4dev");
    if (rc != EOK) {
        bdev_ = nullptr;
        return STATUS_UNSUCCESSFUL;
    }

    rc = ext4_mount("ext4dev", MOUNT_POINT, read_only);
    if (rc != EOK) {
        ext4_device_unregister("ext4dev");
        bdev_ = nullptr;
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
    // Removed PostCleanupWhenModifiedOnly and PostDispositionWhenNecessaryOnly
    // as they caused duplicate OnClose calls from WinFsp internal threads.
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
        bdev_ = nullptr;
        return status;
    }

    fs_->UserContext = this;

    status = FspFileSystemSetMountPoint(fs_, const_cast<PWSTR>(mount_point));
    if (!NT_SUCCESS(status)) {
        FspFileSystemDelete(fs_);
        fs_ = nullptr;
        ext4_umount(MOUNT_POINT);
        ext4_device_unregister("ext4dev");
        bdev_ = nullptr;
        return status;
    }

    // Use 1 dispatcher thread. lwext4 is not thread-safe, so concurrent
    // callbacks corrupt its internal state. A mutex protects all calls.
    status = FspFileSystemStartDispatcher(fs_, 1);
    if (!NT_SUCCESS(status)) {
        FspFileSystemDelete(fs_);
        fs_ = nullptr;
        ext4_umount(MOUNT_POINT);
        ext4_device_unregister("ext4dev");
        bdev_ = nullptr;
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
    auto* self = GetSelf(FileSystem);
    std::lock_guard<std::mutex> lock(self->ext4_mutex_);
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
    std::lock_guard<std::mutex> lock(self->ext4_mutex_);
    std::string path = self->ToExt4Path(FileName);


    // Check if the file/directory exists and get attributes
    FSP_FSCTL_FILE_INFO file_info;
    std::memset(&file_info, 0, sizeof(file_info));

    NTSTATUS status = self->FillFileInfo(path.c_str(), &file_info);
    if (!NT_SUCCESS(status)) {

        return status;
    }

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
    auto* self = GetSelf(FileSystem);
    std::lock_guard<std::mutex> lock(self->ext4_mutex_);
    std::string path = self->ToExt4Path(FileName);

    bool is_directory = (CreateOptions & FILE_DIRECTORY_FILE) != 0;

    if (is_directory) {
        // Create directory on the ext4 filesystem
        int rc = ext4_dir_mk(path.c_str());
        if (rc != EOK)
            return STATUS_UNSUCCESSFUL;

        // Open the newly created directory to return a handle to WinFsp
        auto* ctx = new Ext4FileContext{};
        ctx->path.assign(FileName);
        ctx->is_directory = true;
        std::memset(&ctx->dir, 0, sizeof(ctx->dir));

        rc = ext4_dir_open(&ctx->dir, path.c_str());
        if (rc != EOK) {
            delete ctx;
            return STATUS_UNSUCCESSFUL;
        }

        NTSTATUS status = self->FillFileInfo(path.c_str(), FileInfo);
        if (!NT_SUCCESS(status)) {
            ext4_dir_close(&ctx->dir);
            delete ctx;
            return status;
        }

        *PFileContext = ctx;
        self->valid_contexts_.insert(ctx);
    } else {
        ext4_file f;
        std::memset(&f, 0, sizeof(f));

        int rc = ext4_fopen(&f, path.c_str(), "wb");
        if (rc != EOK)
            return STATUS_UNSUCCESSFUL;
        ext4_fclose(&f);

        // Reopen for read+write
        auto* ctx = new Ext4FileContext{};
        ctx->path.assign(FileName);
        ctx->is_directory = false;
        std::memset(&ctx->file, 0, sizeof(ctx->file));

        rc = ext4_fopen(&ctx->file, path.c_str(), "r+b");
        if (rc != EOK) {
            delete ctx;
            return STATUS_UNSUCCESSFUL;
        }

        NTSTATUS status = self->FillFileInfo(path.c_str(), FileInfo);
        if (!NT_SUCCESS(status)) {
            ext4_fclose(&ctx->file);
            delete ctx;
            return status;
        }

        *PFileContext = ctx;
        self->valid_contexts_.insert(ctx);
    }

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI Ext4FileSystem::OnOverwrite(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, UINT32 FileAttributes, BOOLEAN ReplaceFileAttributes,
    UINT64 AllocationSize, FSP_FSCTL_FILE_INFO* FileInfo)
{
    // Overwrite: truncate existing file to 0 bytes and reopen for writing.

    auto* self = GetSelf(FileSystem);
    std::lock_guard<std::mutex> lock(self->ext4_mutex_);
    auto* ctx = static_cast<Ext4FileContext*>(FileContext);
    if (!ctx || ctx->is_directory)
        return STATUS_INVALID_DEVICE_REQUEST;

    std::string path = self->ToExt4Path(ctx->path.c_str());
    ext4_fclose(&ctx->file);

    ext4_file f;
    std::memset(&f, 0, sizeof(f));

    int rc = ext4_fopen(&f, path.c_str(), "wb");
    if (rc != EOK) {
        // Try to reopen in old mode to keep ctx valid
        ext4_fopen(&ctx->file, path.c_str(), "r+b");
        return STATUS_UNSUCCESSFUL;
    }
    ext4_fclose(&f);

    // Reabrir para leitura+escrita
    std::memset(&ctx->file, 0, sizeof(ctx->file));
    rc = ext4_fopen(&ctx->file, path.c_str(), "r+b");
    if (rc != EOK)
        return STATUS_UNSUCCESSFUL;

    return self->FillFileInfo(path.c_str(), FileInfo);
}

NTSTATUS NTAPI Ext4FileSystem::OnOpen(FSP_FILE_SYSTEM* FileSystem,
    PWSTR FileName, UINT32 CreateOptions, UINT32 GrantedAccess,
    PVOID* PFileContext, FSP_FSCTL_FILE_INFO* FileInfo)
{

    auto* self = GetSelf(FileSystem);
    std::lock_guard<std::mutex> lock(self->ext4_mutex_);
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

        // Open read+write when volume is writable, read-only otherwise.
        const char* mode = self->read_only_ ? "rb" : "r+b";
        int rc = ext4_fopen(&ctx->file, path.c_str(), mode);
        if (rc != EOK) {
            // Fallback to read-only if r+b fails
            rc = ext4_fopen(&ctx->file, path.c_str(), "rb");
            if (rc != EOK) {
                delete ctx;
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }
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
    self->valid_contexts_.insert(ctx);

    return STATUS_SUCCESS;
}

VOID NTAPI Ext4FileSystem::OnClose(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext)
{
    auto* self = GetSelf(FileSystem);
    std::lock_guard<std::mutex> lock(self->ext4_mutex_);
    auto* ctx = static_cast<Ext4FileContext*>(FileContext);
    if (!ctx) return;

    // Guard against double-close: WinFsp internal threads may call
    // OnClose twice for the same context pointer.
    if (self->valid_contexts_.find(ctx) == self->valid_contexts_.end())
        return;
    self->valid_contexts_.erase(ctx);

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

    auto* self = GetSelf(FileSystem);
    std::lock_guard<std::mutex> lock(self->ext4_mutex_);
    auto* ctx = static_cast<Ext4FileContext*>(FileContext);
    if (!ctx || ctx->is_directory)
        return STATUS_INVALID_DEVICE_REQUEST;
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

NTSTATUS NTAPI Ext4FileSystem::OnWrite(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, PVOID Buffer, UINT64 Offset, ULONG Length,
    BOOLEAN WriteToEndOfFile, BOOLEAN ConstrainedIo,
    PULONG PBytesTransferred, FSP_FSCTL_FILE_INFO* FileInfo)
{
    auto* self = GetSelf(FileSystem);
    std::lock_guard<std::mutex> lock(self->ext4_mutex_);
    auto* ctx = static_cast<Ext4FileContext*>(FileContext);
    if (!ctx || ctx->is_directory)
        return STATUS_INVALID_DEVICE_REQUEST;

    std::string path = self->ToExt4Path(ctx->path.c_str());

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
    std::lock_guard<std::mutex> lock(self->ext4_mutex_);
    auto* ctx = static_cast<Ext4FileContext*>(FileContext);
    if (!ctx || !ctx->is_directory)
        return STATUS_INVALID_DEVICE_REQUEST;
    std::string dir_path = self->ToExt4Path(ctx->path.c_str());

    // Close and reopen directory to rewind the iterator.
    // IMPORTANT: never open a second handle to the same directory —
    // lwext4 corrupts internal state when two handles share the same inode.
    ext4_dir_close(&ctx->dir);
    std::memset(&ctx->dir, 0, sizeof(ctx->dir));

    int rc = ext4_dir_open(&ctx->dir, dir_path.c_str());
    if (rc != EOK)
        return STATUS_UNSUCCESSFUL;

    bool past_marker = (Marker == nullptr);
    const ext4_direntry* entry;

    while ((entry = ext4_dir_entry_next(&ctx->dir)) != nullptr) {
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


    return STATUS_SUCCESS;
}

NTSTATUS NTAPI Ext4FileSystem::OnGetFileInfo(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, FSP_FSCTL_FILE_INFO* FileInfo)
{

    auto* self = GetSelf(FileSystem);
    std::lock_guard<std::mutex> lock(self->ext4_mutex_);
    auto* ctx = static_cast<Ext4FileContext*>(FileContext);
    if (!ctx) return STATUS_INVALID_HANDLE;
    std::string path = self->ToExt4Path(ctx->path.c_str());

    NTSTATUS r = self->FillFileInfo(path.c_str(), FileInfo);

    return r;
}

NTSTATUS NTAPI Ext4FileSystem::OnSetBasicInfo(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, UINT32 FileAttributes,
    UINT64 CreationTime, UINT64 LastAccessTime, UINT64 LastWriteTime,
    UINT64 ChangeTime, FSP_FSCTL_FILE_INFO* FileInfo)
{

    auto* self = GetSelf(FileSystem);
    std::lock_guard<std::mutex> lock(self->ext4_mutex_);

    auto* ctx = static_cast<Ext4FileContext*>(FileContext);
    if (!ctx) return STATUS_INVALID_HANDLE;

    std::string path = self->ToExt4Path(ctx->path.c_str());

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
        uint32_t ctime = static_cast<uint32_t>((CreationTime - EPOCH_DIFF) / 10000000ULL);
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
    std::lock_guard<std::mutex> lock(self->ext4_mutex_);

    auto* ctx = static_cast<Ext4FileContext*>(FileContext);
    if (!ctx || ctx->is_directory)
        return STATUS_INVALID_DEVICE_REQUEST;

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
    std::lock_guard<std::mutex> lock(self->ext4_mutex_);
    auto* ctx = static_cast<Ext4FileContext*>(FileContext);
    if (!ctx) return STATUS_INVALID_HANDLE;

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
    //
    auto* self = GetSelf(FileSystem);
    std::lock_guard<std::mutex> lock(self->ext4_mutex_);
    std::string old_path = self->ToExt4Path(FileName);
    std::string new_path = self->ToExt4Path(NewFileName);

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

    int rc = ext4_frename(old_path.c_str(), new_path.c_str());
    if (rc != EOK)
        return STATUS_UNSUCCESSFUL;

    // Update the stored path in the context
    auto* ctx = static_cast<Ext4FileContext*>(FileContext);
    if (ctx)
        ctx->path.assign(NewFileName);

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI Ext4FileSystem::OnFlush(FSP_FILE_SYSTEM* FileSystem,
    PVOID FileContext, FSP_FSCTL_FILE_INFO* FileInfo)
{

    auto* self = GetSelf(FileSystem);
    std::lock_guard<std::mutex> lock(self->ext4_mutex_);
    if (!self->read_only_)
        ext4_cache_flush(MOUNT_POINT);

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
    std::lock_guard<std::mutex> lock(self->ext4_mutex_);
    std::string path = self->ToExt4Path(FileName);
    auto* ctx = static_cast<Ext4FileContext*>(FileContext);

    // If marked for deletion, close the handle and remove from disk
    if (Flags & FspCleanupDelete) {
        if (ctx->is_directory) {
            ext4_dir_close(&ctx->dir);
            ext4_dir_rm(path.c_str());
            std::memset(&ctx->dir, 0, sizeof(ctx->dir));
        } else {
            ext4_fclose(&ctx->file);
            ext4_fremove(path.c_str());
            std::memset(&ctx->file, 0, sizeof(ctx->file));
        }
    }

}
