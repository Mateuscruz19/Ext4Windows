#include "server.hpp"
#include "pipe_protocol.hpp"
#include "tray_icon.hpp"
#include "blockdev_file.hpp"
#include "blockdev_partition.hpp"
#include "debug_log.hpp"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>

// --- MountManager ---

MountManager::MountManager() {}

MountManager::~MountManager()
{
    // Unmount everything on destruction
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (auto& [letter, entry] : mounts_) {
        try {
            if (entry->fs)
                entry->fs->Unmount();
        } catch (...) {}
        try {
            if (entry->bdev && entry->destroy_fn)
                entry->destroy_fn(entry->bdev);
        } catch (...) {}
    }
    mounts_.clear();
}

std::string MountManager::MountImage(const std::wstring& image_path,
                                      wchar_t drive_letter, bool read_write)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Check if drive letter is already in use
    if (mounts_.count(drive_letter)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "ERROR Drive %c: is already in use",
                 static_cast<char>(drive_letter));
        return msg;
    }

    // Convert wide path to UTF-8 for blockdev_file
    int len = WideCharToMultiByte(CP_UTF8, 0, image_path.c_str(), -1,
                                   nullptr, 0, nullptr, nullptr);
    std::string utf8_path(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, image_path.c_str(), -1,
                         utf8_path.data(), len, nullptr, nullptr);

    // Check if file exists
    DWORD attrs = GetFileAttributesW(image_path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return "ERROR File not found: " + utf8_path;
    }

    // Create block device from image file
    struct ext4_blockdev* bdev = create_file_blockdev(utf8_path.c_str());
    if (!bdev) {
        return "ERROR Failed to create block device for: " + utf8_path;
    }

    // Create filesystem and mount
    auto fs = std::make_unique<Ext4FileSystem>();
    wchar_t mount_point[4] = { drive_letter, L':', L'\0' };
    char instance_id = static_cast<char>(drive_letter);

    NTSTATUS status = fs->Mount(bdev, mount_point, !read_write, instance_id);
    if (!NT_SUCCESS(status)) {
        destroy_file_blockdev(bdev);
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "ERROR Mount failed on %c: (status=0x%08lX)",
                 static_cast<char>(drive_letter), status);
        return msg;
    }

    // Store the mount entry
    auto entry = std::make_unique<MountEntry>();
    entry->drive_letter = drive_letter;
    entry->source_path = image_path;
    entry->read_only = !read_write;
    entry->bdev = bdev;
    entry->destroy_fn = destroy_file_blockdev;
    entry->fs = std::move(fs);

    mounts_[drive_letter] = std::move(entry);
    NotifyTray();

    char msg[512];
    snprintf(msg, sizeof(msg), "OK Mounted %s on %c: (%s)",
             utf8_path.c_str(), static_cast<char>(drive_letter),
             read_write ? "read-write" : "read-only");
    return msg;
}

std::string MountManager::Unmount(wchar_t drive_letter)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto it = mounts_.find(drive_letter);
    if (it == mounts_.end()) {
        char msg[128];
        snprintf(msg, sizeof(msg), "ERROR Drive %c: is not mounted",
                 static_cast<char>(drive_letter));
        return msg;
    }

    auto& entry = it->second;
    // Use try/catch: if the filesystem was already ejected by Windows,
    // the WinFsp handles may be invalid and Unmount could throw.
    try {
        if (entry->fs)
            entry->fs->Unmount();
    } catch (...) {
        dbg("Unmount: exception during fs->Unmount(), ignoring");
    }
    try {
        if (entry->bdev && entry->destroy_fn)
            entry->destroy_fn(entry->bdev);
    } catch (...) {
        dbg("Unmount: exception during destroy_fn(), ignoring");
    }

    mounts_.erase(it);
    NotifyTray();

    char msg[128];
    snprintf(msg, sizeof(msg), "OK Unmounted %c:",
             static_cast<char>(drive_letter));
    return msg;
}

std::string MountManager::Status()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Clean up ejected mounts: if the drive letter no longer exists
    // in Windows, the user ejected it via Explorer. Remove the ghost.
    DWORD drives = GetLogicalDrives();
    std::vector<wchar_t> to_remove;
    for (auto& [letter, entry] : mounts_) {
        int bit = letter - L'A';
        if (!(drives & (1 << bit))) {
            dbg("Status: drive %c: was ejected, cleaning up", (char)letter);
            to_remove.push_back(letter);
        }
    }
    for (wchar_t letter : to_remove) {
        auto& entry = mounts_[letter];
        try { if (entry->bdev && entry->destroy_fn) entry->destroy_fn(entry->bdev); }
        catch (...) {}
        mounts_.erase(letter);
    }
    if (!to_remove.empty())
        NotifyTray();

    if (mounts_.empty())
        return "OK No active mounts";

    std::ostringstream oss;
    oss << "OK " << mounts_.size() << " mount(s) active";

    for (auto& [letter, entry] : mounts_) {
        // Convert source path to UTF-8 for display
        int len = WideCharToMultiByte(CP_UTF8, 0, entry->source_path.c_str(),
                                       -1, nullptr, 0, nullptr, nullptr);
        std::string src(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, entry->source_path.c_str(), -1,
                             src.data(), len, nullptr, nullptr);

        oss << "\n" << static_cast<char>(letter) << ": "
            << src << " (" << (entry->read_only ? "read-only" : "read-write")
            << ")";
    }

    return oss.str();
}

std::string MountManager::Quit()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // Unmount all
    for (auto& [letter, entry] : mounts_) {
        if (entry->fs)
            entry->fs->Unmount();
        if (entry->bdev && entry->destroy_fn)
            entry->destroy_fn(entry->bdev);
    }
    mounts_.clear();
    quit_requested_ = true;

    // Post WM_QUIT to break the message loop
    PostQuitMessage(0);

    return "OK Server shutting down";
}

size_t MountManager::ActiveCount()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return mounts_.size();
}

void MountManager::NotifyTray()
{
    if (tray_)
        tray_->Update();
}

// --- Command Dispatcher ---

static std::string dispatch_command(MountManager& manager,
                                     const std::string& cmd)
{
    dbg("Server received command: '%s'", cmd.c_str());

    // Parse the command (first word determines the action)
    std::istringstream iss(cmd);
    std::string action;
    iss >> action;

    if (action == "MOUNT") {
        std::string source;
        std::string drive_str;
        std::string rw_flag;
        iss >> source >> drive_str >> rw_flag;

        if (source.empty() || drive_str.empty())
            return "ERROR Usage: MOUNT <source_path> <drive_letter> [RW]";

        wchar_t drive_letter = static_cast<wchar_t>(drive_str[0]);
        bool read_write = (rw_flag == "RW");

        // Convert source path to wide string
        int wlen = MultiByteToWideChar(CP_UTF8, 0, source.c_str(), -1,
                                        nullptr, 0);
        std::wstring wsource(wlen - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, source.c_str(), -1,
                             wsource.data(), wlen);

        return manager.MountImage(wsource, drive_letter, read_write);
    }

    if (action == "UNMOUNT") {
        std::string drive_str;
        iss >> drive_str;
        if (drive_str.empty())
            return "ERROR Usage: UNMOUNT <drive_letter>";
        wchar_t drive_letter = static_cast<wchar_t>(drive_str[0]);
        return manager.Unmount(drive_letter);
    }

    if (action == "STATUS") {
        return manager.Status();
    }

    if (action == "QUIT") {
        return manager.Quit();
    }

    return "ERROR Unknown command: " + action;
}

// --- Pipe Server Thread ---

static void pipe_server_thread(MountManager& manager)
{
    dbg("Pipe server thread started");

    while (!manager.QuitRequested()) {
        // Create a named pipe instance and wait for a client
        HANDLE pipe = CreateNamedPipeW(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1,                  // Only 1 instance (one client at a time)
            PIPE_BUFFER_SIZE,
            PIPE_BUFFER_SIZE,
            0,                  // Default timeout
            nullptr);

        if (pipe == INVALID_HANDLE_VALUE) {
            dbg("CreateNamedPipe failed: %lu", GetLastError());
            Sleep(1000);
            continue;
        }

        // Wait for a client to connect
        BOOL connected = ConnectNamedPipe(pipe, nullptr)
                         || (GetLastError() == ERROR_PIPE_CONNECTED);

        if (!connected) {
            dbg("ConnectNamedPipe failed: %lu", GetLastError());
            CloseHandle(pipe);
            continue;
        }

        // Read command from client
        std::string cmd = pipe_recv(pipe);
        if (!cmd.empty()) {
            // Dispatch and send response
            std::string response = dispatch_command(manager, cmd);
            dbg("Pipe: sending response: '%s'", response.c_str());
            pipe_send(pipe, response);
            dbg("Pipe: response sent OK");
        }

        // Disconnect and close pipe for this client
        dbg("Pipe: disconnecting client");
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        dbg("Pipe: client disconnected, waiting for next");
    }

    dbg("Pipe server thread exiting");
}

// --- Server Entry Point ---

int run_server()
{
    // Detach from console (server runs in background)
    FreeConsole();

    // If debug is enabled, log to a file since we have no console
    if (g_debug) {
        wchar_t temp_path[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp_path);
        wcscat_s(temp_path, L"ext4windows_server.log");
        g_debug_file = _wfopen(temp_path, L"w");
    }

    dbg("Server starting");

    MountManager manager;

    // Start pipe listener on a separate thread
    std::thread pipe_thread(pipe_server_thread, std::ref(manager));
    pipe_thread.detach();

    // Create system tray icon and run Win32 message loop
    TrayIcon tray(manager);
    manager.SetTrayIcon(&tray);

    // Message loop — required for the tray icon to receive events.
    // This blocks until PostQuitMessage(0) is called (by QUIT command
    // or tray menu "Quit").
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    dbg("Server shutting down");

    if (g_debug_file) {
        fclose(g_debug_file);
        g_debug_file = nullptr;
    }

    return 0;
}
