# Ext4Windows

A lightweight ext4 filesystem driver for Windows. Mount your Linux ext4 partitions as native Windows drive letters — no VM, no WSL, no hassle.

Built with **C++**, **WinFsp**, and **lwext4**.

## Why?

Dual-booting Linux and Windows is common, but accessing your Linux files from Windows is painful:

- Windows has **zero** native ext4 support
- Existing tools are either **abandoned** (Ext2Fsd), **paid** (Paragon), or **read-only** (DiskInternals)
- WSL2's `--mount` runs inside a VM and requires admin — not a real drive letter

**Ext4Windows** aims to be the free, open-source, actively maintained solution.

## Features (Planned)

- [ ] Mount ext4 partitions as Windows drive letters (E:, F:, etc.)
- [ ] Read support for ext4 files and directories
- [ ] Write support (create, modify, delete files)
- [ ] Auto-detect ext4 partitions on startup
- [ ] System tray GUI for mounting/unmounting
- [ ] Support for ext4 features: extents, 64-bit, metadata checksums
- [ ] Journaling support for crash safety

## Tech Stack

- **C++** — Core application
- **[WinFsp](https://winfsp.dev)** — Windows FUSE framework (filesystem in userspace)
- **[lwext4](https://github.com/gkostka/lwext4)** — Portable ext4 filesystem implementation in C
- **Qt 6** — GUI (system tray application)

## Roadmap

### Phase 1 — Read-Only Mount
- Parse ext4 superblock and block groups
- Read files and directories
- Mount as Windows drive letter via WinFsp

### Phase 2 — Write Support
- Create, modify, and delete files
- Directory operations (mkdir, rmdir, rename)
- Proper error handling and fsck validation

### Phase 3 — Stability & Features
- Journaling support
- Large file support
- Permission mapping (Linux → Windows)
- Auto-detection of ext4 partitions

### Phase 4 — GUI & Polish
- System tray application
- Mount/unmount with one click
- Settings and configuration

## Building

> Coming soon — project is in early development.

## Contributing

Contributions are welcome! This project is in its early stages, so feel free to open issues for discussion.

## License

GPL-2.0 — See [LICENSE](LICENSE) for details.
