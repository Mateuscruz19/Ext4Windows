# Changelog

All notable changes to Ext4Windows will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-04-05

First stable release.

### Features

- Mount ext4 image files (`.img`) as Windows drive letters
- Mount raw ext4 partitions from physical disks
- Full read and write support (files, directories, symlinks)
- Client-server architecture with background daemon
- System tray icon with context menu (unmount, quit)
- CLI client (`mount`, `unmount`, `status`, `scan`, `quit`)
- Auto-detect ext4 partitions with `scan` command
- Multiple simultaneous mounts (Z:, Y:, X:, ...)
- Auto-start server on first mount command
- Auto-start on Windows login (optional, via Registry)
- Ghost mount detection and auto-cleanup on eject
- Named Pipe IPC with SDDL ACL security
- ext4 journaling support (recovery + write transactions)
- Large file support (>4GB with 64-bit block calculations)
- 512KB block cache + WinFsp metadata caching
- File timestamps (ext4 crtime/atime/mtime/ctime mapped to Windows)
- Linux permission mapping (ext4 mode bits to Windows attributes)
- Path traversal and drive letter validation
- Integer overflow guards on block read/write
- Debug logging (console + file)
- Custom application icon
- Interactive mode with 8-language support (EN, PT, ES, DE, FR, ZH, JA, RU)
- Installer with automatic WinFsp dependency installation
- Portable release (.zip)

### Security

- Audited with AddressSanitizer (0 errors)
- Audited with MSVC `/analyze` (0 vulnerabilities)
- Audited with CppCheck 2.20 (0 bugs)
- CRT Debug Heap memory leak check (0 leaks)
- 110 tests / 1359 assertions — all passing

[1.0.0]: https://github.com/Mateuscruz19/Ext4Windows/releases/tag/v1.0.0
