#include <windows.h>
#include <winfsp/winfsp.h>

#include "ext4_filesystem.hpp"
#include "blockdev_file.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// Global event used to signal the main thread to unmount and exit.
// The Ctrl+C handler sets this event so WaitForSingleObject returns.
static HANDLE g_stop_event = nullptr;

static BOOL WINAPI console_ctrl_handler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        if (g_stop_event)
            SetEvent(g_stop_event);
        return TRUE;
    }
    return FALSE;
}

static void print_banner()
{
    printf("\n");
    printf("  ====================================\n");
    printf("  Ext4Windows  —  ext4 driver for Windows\n");
    printf("  ====================================\n");
    printf("\n");
}

static void print_usage()
{
    printf("Usage: ext4windows <image-file> [drive-letter]\n");
    printf("\n");
    printf("  image-file    Path to an ext4 .img file\n");
    printf("  drive-letter  Letter to mount (e.g. Z:) — optional\n");
    printf("\n");
    printf("Examples:\n");
    printf("  ext4windows C:\\linux.img Z:\n");
    printf("  ext4windows C:\\linux.img\n");
    printf("\n");
    printf("If no arguments are given, interactive mode starts.\n");
    printf("\n");
}

// Find the first available drive letter, starting from Z: going down.
// Returns L'\0' if none found.
static wchar_t find_free_drive_letter()
{
    DWORD used = GetLogicalDrives();
    for (wchar_t letter = L'Z'; letter >= L'D'; letter--) {
        int bit = letter - L'A';
        if (!(used & (1 << bit)))
            return letter;
    }
    return L'\0';
}

// Check if a file exists at the given path.
static bool file_exists(const wchar_t* path)
{
    DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES &&
           !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// Read a line from stdin into a wide string buffer. Returns false on EOF.
static bool read_line(wchar_t* buf, int max_chars)
{
    if (!fgetws(buf, max_chars, stdin))
        return false;

    // Remove trailing newline
    size_t len = wcslen(buf);
    if (len > 0 && buf[len - 1] == L'\n')
        buf[len - 1] = L'\0';

    return true;
}

// Remove surrounding quotes from a path (drag-and-drop adds them).
static void strip_quotes(wchar_t* path)
{
    size_t len = wcslen(path);
    if (len >= 2 && path[0] == L'"' && path[len - 1] == L'"') {
        memmove(path, path + 1, (len - 2) * sizeof(wchar_t));
        path[len - 2] = L'\0';
    }
}

// Ask the user for the image path interactively.
static bool ask_image_path(wchar_t* out, int max_chars)
{
    printf("Enter the path to an ext4 image file:\n");
    printf("  (you can drag and drop the file here)\n");
    printf("\n");
    printf("  Path: ");
    fflush(stdout);

    if (!read_line(out, max_chars))
        return false;

    strip_quotes(out);
    return true;
}

// Ask the user for the drive letter, or auto-pick one.
static bool ask_drive_letter(wchar_t* out, int max_chars)
{
    wchar_t free_letter = find_free_drive_letter();

    printf("\n");
    if (free_letter) {
        printf("Enter drive letter to mount, or press Enter for auto (%c:):\n",
               (char)free_letter);
    } else {
        printf("Enter drive letter to mount:\n");
    }
    printf("\n");
    printf("  Drive: ");
    fflush(stdout);

    wchar_t input[64] = {};
    if (!read_line(input, 64))
        return false;

    if (input[0] == L'\0' && free_letter) {
        // User pressed Enter — use auto
        swprintf(out, max_chars, L"%c:", free_letter);
    } else if (input[0] != L'\0') {
        // User typed something — normalize to "X:" format
        wchar_t letter = towupper(input[0]);
        if (letter < L'A' || letter > L'Z') {
            printf("\n  Error: '%c' is not a valid drive letter (A-Z).\n",
                   (char)input[0]);
            return false;
        }
        swprintf(out, max_chars, L"%c:", letter);
    } else {
        printf("\n  Error: no free drive letters available.\n");
        return false;
    }

    return true;
}

// Wait for user to press Enter (keeps the window open after errors).
static void pause_before_exit()
{
    printf("\nPress Enter to exit...");
    fflush(stdout);
    getwchar();
}

// Mount and run until Ctrl+C or the stop event is signaled.
static int run(const wchar_t* image_path, const wchar_t* mount_point,
               bool interactive)
{
    // Convert image path to UTF-8 for lwext4
    char image_path_utf8[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, image_path, -1,
                        image_path_utf8, sizeof(image_path_utf8),
                        nullptr, nullptr);

    // Validate the image file exists
    if (!file_exists(image_path)) {
        printf("  Error: file not found: %s\n", image_path_utf8);
        if (interactive) pause_before_exit();
        return 1;
    }

    printf("\n");
    printf("  Image:  %s\n", image_path_utf8);
    wprintf(L"  Mount:  %s\\\n", mount_point);
    printf("\n");

    // Create block device backed by the image file
    struct ext4_blockdev* bdev = create_file_blockdev(image_path_utf8);
    if (!bdev) {
        printf("  Error: could not open '%s'.\n", image_path_utf8);
        printf("         Is the file a valid ext4 image?\n");
        if (interactive) pause_before_exit();
        return 1;
    }

    // Mount the ext4 filesystem via WinFsp
    Ext4FileSystem fs;
    NTSTATUS status = fs.Mount(bdev, mount_point, true /* read-only */);
    if (!NT_SUCCESS(status)) {
        if (status == 0xC0000035) {
            wprintf(L"  Error: drive %s is already in use.\n", mount_point);
            printf("         Pick a different letter.\n");
        } else {
            printf("  Error: failed to mount (NTSTATUS 0x%08lX).\n", status);
            printf("         The file might not be a valid ext4 image.\n");
        }
        destroy_file_blockdev(bdev);
        if (interactive) pause_before_exit();
        return 1;
    }

    wprintf(L"  Mounted at %s (read-only).\n", mount_point);
    printf("\n");

    if (interactive) {
        printf("  The drive is now accessible in File Explorer.\n");
        printf("  Press Enter here to unmount and exit.\n");
        printf("\n");

        // In interactive mode, wait for Enter OR Ctrl+C
        g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

        // Read stdin in a loop, checking both Enter and the stop event
        HANDLE stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
        HANDLE handles[] = { stdin_handle, g_stop_event };
        WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    } else {
        printf("  Press Ctrl+C to unmount.\n");
        printf("\n");

        g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
        WaitForSingleObject(g_stop_event, INFINITE);
    }

    // Cleanup
    printf("  Unmounting...\n");
    fs.Unmount();
    destroy_file_blockdev(bdev);
    printf("  Done.\n");

    if (interactive) {
        printf("\n");
        pause_before_exit();
    }

    return 0;
}

int wmain(int argc, wchar_t* argv[])
{
    // CLI mode: ext4windows <image> [drive]
    if (argc >= 2) {
        // Check for --help / -h
        if (wcscmp(argv[1], L"--help") == 0 || wcscmp(argv[1], L"-h") == 0) {
            print_banner();
            print_usage();
            return 0;
        }

        const wchar_t* image_path = argv[1];
        wchar_t mount_buf[8] = {};

        if (argc >= 3) {
            // Drive letter provided
            wcsncpy(mount_buf, argv[2], 7);
        } else {
            // Auto-pick a drive letter
            wchar_t letter = find_free_drive_letter();
            if (!letter) {
                printf("  Error: no free drive letters available.\n");
                return 1;
            }
            swprintf(mount_buf, 8, L"%c:", letter);
        }

        print_banner();
        return run(image_path, mount_buf, false);
    }

    // Interactive mode: no arguments
    print_banner();

    wchar_t image_path[512] = {};
    if (!ask_image_path(image_path, 512)) {
        pause_before_exit();
        return 1;
    }

    wchar_t mount_point[8] = {};
    if (!ask_drive_letter(mount_point, 8)) {
        pause_before_exit();
        return 1;
    }

    return run(image_path, mount_point, true);
}
