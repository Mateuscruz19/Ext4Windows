# Learning Roadmap — Ext4Windows

What you need to learn to build this project, in order.

## What You Already Know
- C++ and Qt 6 (from BatTorrent)
- Building desktop apps on Windows
- Working with third-party C/C++ libraries
- CMake build system

---

## Phase 0 — Foundations (1-2 weeks)

### 0.1 — How Filesystems Work (Conceptual)
Learn what a filesystem actually does at the disk level.
- What are inodes, blocks, superblocks, block groups
- How a file path like `/home/user/photo.jpg` is resolved step by step
- Difference between metadata and data
- What journaling is and why it matters

**Resources:**
- [Operating Systems: Three Easy Pieces — Chapter on Filesystems](https://pages.cs.wisc.edu/~remzi/OSTEP/)
  - Chapter 39: Files and Directories
  - Chapter 40: File System Implementation
  - Chapter 42: Journaling
- These 3 chapters are short, free, and give you the mental model you need

### 0.2 — ext4 On-Disk Format
Understand how ext4 specifically organizes data on disk.
- Superblock structure and what each field means
- Block groups and the block group descriptor table
- Inode structure (permissions, timestamps, size, block pointers)
- Extent trees (how ext4 maps file data to disk blocks)
- Directory entries (how filenames map to inodes)
- HTree (indexed directories for fast lookup)

**Resources:**
- [ext4 Kernel Wiki — Disk Layout](https://ext4.wiki.kernel.org/index.php/Ext4_Disk_Layout) — THE reference
- Read the lwext4 source code alongside the wiki to see how concepts map to code

### 0.3 — Calling C from C++
Since lwext4 is C and your project is C++, learn how to bridge them.
- `extern "C"` blocks
- Linking C libraries in CMake with `target_link_libraries`
- C struct usage in C++ code
- You probably already know most of this from libtorrent, but review it

**Practice:** Build a tiny C++ program that calls a C library function.

---

## Phase 1 — Core Tools (2-3 weeks)

### 1.1 — WinFsp API
This is your bridge to Windows. Learn how WinFsp lets you create a virtual drive.
- Install WinFsp SDK
- Read the [WinFsp Tutorial](https://winfsp.dev/doc/WinFsp-Tutorial/) — walks you through building a memory filesystem
- Understand the FUSE callback model: `getattr`, `readdir`, `open`, `read`, `write`, `create`, `unlink`
- Build the tutorial "passthrough" filesystem (mirrors a real folder as a drive letter)

**Key milestone:** You can mount a folder as Z: drive on Windows using WinFsp.

### 1.2 — lwext4 Library
This does the heavy lifting of reading/writing ext4.
- Clone [lwext4](https://github.com/gkostka/lwext4)
- Build it on Windows (it has CMake support)
- Understand the `ext4_blockdev` interface — this is how you tell lwext4 where to read/write raw bytes
- Use it to open a `.img` file and list files inside it

**Key milestone:** A C++ program that opens `test.img` and prints the list of files in it.

### 1.3 — Raw Disk I/O on Windows
How to read a physical disk/partition from Windows userspace.
- `CreateFile` on `\\.\PhysicalDriveN` and `\\.\HarddiskVolumeN`
- `DeviceIoControl` for partition info
- `ReadFile` / `WriteFile` with sector-aligned buffers
- How to enumerate partitions and detect which ones are ext4 (check superblock magic number)

**Resources:**
- [Microsoft Docs — CreateFile for physical drives](https://learn.microsoft.com/en-us/windows/win32/fileio/creating-and-opening-files)
- Look at how Ext4Fsd and WinBtrfs discover partitions

**Key milestone:** A program that reads the first 2048 bytes of a partition and prints them as hex.

---

## Phase 2 — Connect the Pieces (2-4 weeks)

### 2.1 — lwext4 + Image File
Connect lwext4's `ext4_blockdev` to a `.img` file on disk.
- Implement `blockdev_open`, `blockdev_read`, `blockdev_write`, `blockdev_close`
- These just call `fopen`/`fread`/`fwrite` on the .img file
- Mount the ext4 image through lwext4 and list/read files

**Key milestone:** Read any file from a test.img using lwext4 in your C++ code.

### 2.2 — WinFsp + lwext4 (Read-Only)
Wire WinFsp callbacks to lwext4 operations.
- `getattr` → `ext4_fstat`
- `readdir` → `ext4_dir_open` + `ext4_dir_entry_next`
- `read` → `ext4_fopen` + `ext4_fread`
- `open` → `ext4_fopen`
- Map Linux permissions to Windows (simplified)

**Key milestone:** Mount test.img as Z: drive. Open it in Explorer. See files. Open a text file. THIS IS YOUR MVP.

### 2.3 — Test with Real Partition
Replace the .img file backend with raw disk I/O.
- Swap `fread`/`fwrite` for `ReadFile`/`WriteFile` on `\\.\HarddiskVolumeN`
- Test with a real ext4 partition (read-only first!)

**Key milestone:** Your real Linux partition appears as Z: drive in Windows Explorer.

---

## Phase 3 — Write Support (4-6 weeks)

### 3.1 — Write Operations
Add write callbacks to WinFsp.
- `write` → `ext4_fwrite`
- `create` → `ext4_fopen` with create flags
- `unlink` → `ext4_fremove`
- `mkdir` → `ext4_dir_mk`
- `rmdir` → `ext4_dir_rm`
- `rename` → `ext4_frename`
- `truncate` → `ext4_ftruncate`

**Test everything on .img files first. Validate with `fsck.ext4` after every test.**

### 3.2 — Journaling
Ensure crash safety — if the PC loses power mid-write, the filesystem shouldn't corrupt.
- lwext4 has journaling support (jbd2)
- Enable it and test by killing the process mid-write
- Validate with fsck

### 3.3 — Edge Cases
- Large files (>4GB)
- Symlinks
- Hard links
- Special characters in filenames
- Unicode filenames
- Sparse files
- File locking

---

## Phase 4 — GUI & Polish (2-3 weeks)

### 4.1 — System Tray App
- Qt 6 system tray icon
- List detected ext4 partitions
- One-click mount/unmount
- Choose drive letter
- Settings (auto-mount on startup, default drive letters)

### 4.2 — Installer & Distribution
- NSIS or WiX installer
- Bundle WinFsp runtime
- Code signing (optional but nice)

---

## Total Estimated Timeline
- **Phase 0:** 1-2 weeks (study)
- **Phase 1:** 2-3 weeks (learn the tools)
- **Phase 2:** 2-4 weeks (build read-only MVP)
- **Phase 3:** 4-6 weeks (write support)
- **Phase 4:** 2-3 weeks (GUI)
- **Total: ~3-4 months**

## Reference Projects to Study
- [WinBtrfs](https://github.com/maharmstone/btrfs) — same idea for Btrfs, gold standard reference
- [Ext4Fsd](https://github.com/bobranten/Ext4Fsd) — kernel-mode ext4 driver, useful for understanding ext4 edge cases
- [fuse-ext2](https://github.com/alperakcan/fuse-ext2) — FUSE ext4 implementation, reference for callback structure
- [lwext4](https://github.com/gkostka/lwext4) — your core library, read the source
- [WinFsp samples](https://github.com/winfsp/winfsp/tree/master/tst) — official examples
