#pragma once

#include <windows.h>
#include <sddl.h>
#include <winfsp/winfsp.h>

extern "C" {
#include <ext4.h>
#include <ext4_blockdev.h>
}

#include <string>
#include <mutex>
#include <set>
#include <vector>

// Context stored per open file/directory handle.
struct Ext4FileContext {
    std::wstring path;
    bool is_directory;
    bool closed;         // Set by OnCleanup+delete; OnClose just frees memory
    ext4_file file;      // Used for files
    ext4_dir dir;        // Used for directories
};

class Ext4FileSystem {
public:
    Ext4FileSystem();
    ~Ext4FileSystem();

    NTSTATUS Mount(struct ext4_blockdev* bdev, const wchar_t* mount_point,
                   bool read_only);
    void Unmount();

private:
    // WinFsp callbacks (prefixed with "On" to avoid name clash with
    // FSP_FILE_SYSTEM_INTERFACE field names)
    static NTSTATUS NTAPI OnGetVolumeInfo(FSP_FILE_SYSTEM* FileSystem,
        FSP_FSCTL_VOLUME_INFO* VolumeInfo);

    static NTSTATUS NTAPI OnGetSecurityByName(FSP_FILE_SYSTEM* FileSystem,
        PWSTR FileName, PUINT32 PFileAttributes,
        PSECURITY_DESCRIPTOR SecurityDescriptor,
        SIZE_T* PSecurityDescriptorSize);

    static NTSTATUS NTAPI OnCreate(FSP_FILE_SYSTEM* FileSystem,
        PWSTR FileName, UINT32 CreateOptions, UINT32 GrantedAccess,
        UINT32 FileAttributes, PSECURITY_DESCRIPTOR SecurityDescriptor,
        UINT64 AllocationSize,
        PVOID* PFileContext, FSP_FSCTL_FILE_INFO* FileInfo);

    static NTSTATUS NTAPI OnOpen(FSP_FILE_SYSTEM* FileSystem,
        PWSTR FileName, UINT32 CreateOptions, UINT32 GrantedAccess,
        PVOID* PFileContext, FSP_FSCTL_FILE_INFO* FileInfo);

    static NTSTATUS NTAPI OnOverwrite(FSP_FILE_SYSTEM* FileSystem,
        PVOID FileContext, UINT32 FileAttributes, BOOLEAN ReplaceFileAttributes,
        UINT64 AllocationSize, FSP_FSCTL_FILE_INFO* FileInfo);

    static VOID NTAPI OnClose(FSP_FILE_SYSTEM* FileSystem, PVOID FileContext);

    static NTSTATUS NTAPI OnRead(FSP_FILE_SYSTEM* FileSystem,
        PVOID FileContext, PVOID Buffer, UINT64 Offset, ULONG Length,
        PULONG PBytesTransferred);

    static NTSTATUS NTAPI OnReadDirectory(FSP_FILE_SYSTEM* FileSystem,
        PVOID FileContext, PWSTR Pattern, PWSTR Marker,
        PVOID Buffer, ULONG Length, PULONG PBytesTransferred);

    static NTSTATUS NTAPI OnGetFileInfo(FSP_FILE_SYSTEM* FileSystem,
        PVOID FileContext, FSP_FSCTL_FILE_INFO* FileInfo);

    static NTSTATUS NTAPI OnWrite(FSP_FILE_SYSTEM* FileSystem,
        PVOID FileContext, PVOID Buffer, UINT64 Offset, ULONG Length,
        BOOLEAN WriteToEndOfFile, BOOLEAN ConstrainedIo,
        PULONG PBytesTransferred, FSP_FSCTL_FILE_INFO* FileInfo);

    static NTSTATUS NTAPI OnSetBasicInfo(FSP_FILE_SYSTEM* FileSystem,
        PVOID FileContext, UINT32 FileAttributes,
        UINT64 CreationTime, UINT64 LastAccessTime, UINT64 LastWriteTime,
        UINT64 ChangeTime, FSP_FSCTL_FILE_INFO* FileInfo);

    static NTSTATUS NTAPI OnSetFileSize(FSP_FILE_SYSTEM* FileSystem,
        PVOID FileContext, UINT64 NewSize, BOOLEAN SetAllocationSize,
        FSP_FSCTL_FILE_INFO* FileInfo);

    static NTSTATUS NTAPI OnCanDelete(FSP_FILE_SYSTEM* FileSystem,
        PVOID FileContext, PWSTR FileName);

    static NTSTATUS NTAPI OnRename(FSP_FILE_SYSTEM* FileSystem,
        PVOID FileContext, PWSTR FileName, PWSTR NewFileName,
        BOOLEAN ReplaceIfExists);

    static NTSTATUS NTAPI OnFlush(FSP_FILE_SYSTEM* FileSystem,
        PVOID FileContext, FSP_FSCTL_FILE_INFO* FileInfo);

    static VOID NTAPI OnCleanup(FSP_FILE_SYSTEM* FileSystem,
        PVOID FileContext, PWSTR FileName, ULONG Flags);

    // Helpers
    static Ext4FileSystem* GetSelf(FSP_FILE_SYSTEM* FileSystem);
    NTSTATUS FillFileInfo(const char* path, FSP_FSCTL_FILE_INFO* FileInfo);
    static std::string WideToUtf8(const wchar_t* wide);
    std::string ToExt4Path(const wchar_t* win_path);

    FSP_FILE_SYSTEM* fs_ = nullptr;
    FSP_FILE_SYSTEM_INTERFACE iface_ = {};
    struct ext4_blockdev* bdev_ = nullptr;
    bool read_only_ = true;
    std::mutex ext4_mutex_;
    std::set<void*> valid_contexts_;        // Track open contexts to detect double-close
    std::vector<void*> deferred_delete_;    // Contexts waiting to be freed safely

    static const char* MOUNT_POINT;
};
