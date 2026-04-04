#include <windows.h>
#include <winfsp/winfsp.h>

#include "ext4_filesystem.hpp"
#include "blockdev_file.hpp"

#include <cstdio>
#include <cstdlib>

static void usage()
{
    fprintf(stderr,
        "Usage: ext4windows <image-file> <mount-point>\n"
        "\n"
        "  image-file   Path to an ext4 .img file\n"
        "  mount-point  Drive letter to mount (e.g. Z:)\n"
        "\n"
        "Example:\n"
        "  ext4windows C:\\linux.img Z:\n"
    );
}

int wmain(int argc, wchar_t* argv[])
{
    if (argc < 3) {
        usage();
        return 1;
    }

    const wchar_t* image_path = argv[1];
    const wchar_t* mount_point = argv[2];

    // Convert image path to UTF-8 for lwext4
    char image_path_utf8[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, image_path, -1,
                        image_path_utf8, sizeof(image_path_utf8),
                        nullptr, nullptr);

    printf("Ext4Windows — ext4 filesystem driver for Windows\n");
    printf("Image: %s\n", image_path_utf8);
    wprintf(L"Mount: %s\n", mount_point);

    // Create block device backed by the image file
    struct ext4_blockdev* bdev = create_file_blockdev(image_path_utf8);
    if (!bdev) {
        fprintf(stderr, "Error: failed to create block device for '%s'\n",
                image_path_utf8);
        return 1;
    }

    // Mount the ext4 filesystem via WinFsp
    Ext4FileSystem fs;
    NTSTATUS status = fs.Mount(bdev, mount_point, true /* read-only */);
    if (!NT_SUCCESS(status)) {
        fprintf(stderr, "Error: failed to mount filesystem (NTSTATUS 0x%08lX)\n",
                status);
        destroy_file_blockdev(bdev);
        return 1;
    }

    wprintf(L"Filesystem mounted at %s (read-only). Press Ctrl+C to unmount.\n",
            mount_point);

    // Wait for Ctrl+C
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler([](DWORD type) -> BOOL {
        return type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT;
    }, TRUE);
    WaitForSingleObject(event, INFINITE);

    // Cleanup
    fs.Unmount();
    destroy_file_blockdev(bdev);

    printf("Filesystem unmounted.\n");
    return 0;
}
