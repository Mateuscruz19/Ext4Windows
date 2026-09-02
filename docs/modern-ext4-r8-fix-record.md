# Ext4Windows modern-ext4 r8: complete technical fix record

## 1. Scope and result

This record describes the exact source changes made to the original GitHub checkout, why each change was required, how the fixes evolved through r4 to r8, how the Windows release was assembled, and what was done on the physical Kali ext4 drive to prove that r8 worked.

The final result was:

- the 451.40 GiB Kali ext4 partition mounted at `Z:` read-write;
- the drive was visible in the normal Windows Explorer session;
- create, write, 64 MiB copy, readback, SHA256, rename, recursive delete, and timestamps passed;
- a Windows-created marker survived a cross-OS reboot with identical content and SHA256;
- the marker appeared in Kali as UID 1000, GID 1000, mode 0600;
- a foreground r8 mount and Ctrl+C unmount both completed with `OK!`;
- an inherited raw-disk handle remained valid across the read-only safety probe and read-write remount.

This is not a Microsoft-native ext4 driver. It is a portable, unsigned, user-mode filesystem built from Ext4Windows, a patched bundled lwext4, and WinFsp. WinFsp supplies the Windows filesystem bridge and its installed driver.

## 2. Exact baselines

The retained original checkout used these revisions:

| Component | Exact revision | Commit subject |
|---|---|---|
| Ext4Windows | `658008c8fa365766e675acaf015d4e3285e14065` | `Add 8-language GUI translations, keyboard shortcuts, and fix write permissions` |
| Bundled `external/lwext4` | `764b6834384de8cfbf8f86642a8a443fc675af42` | `Fix 32-bit truncation in fread/fwrite/truncate for large files` |

The Ext4Windows project was the checkout of `github.com/Mateuscruz19/Ext4Windows`. The bundled lwext4 remote was `github.com/Mateuscruz19/lwext4`.

The final upstream-to-r8 delta changes 24 files with 1,232 insertions and 61 deletions. The exact unified patch is supplied in the push-ready support bundle:

- file: `Ext4Windows-r8-upstream-to-r8.patch`
- SHA256: `6882cf80b3429159c50e6dabb3c0895849ab40e20a1decc621b01ff5f7ab6a20`
- size: 55,066 bytes, 1,466 lines
- verification: `patch --dry-run -p1` succeeded against the exact baseline above

The retained source snapshots begin at r4. Earlier binaries were experimental iterations. The original-to-r4 section below is therefore cumulative. The r4-to-r8 boundaries are source-diff verified.

The maintained fork records the tested work as these commits:

| Repository | Commit | Purpose |
|---|---|---|
| `BakaValen/lwext4` | `f3c75515f07187394ba838fa5f35e17cfc6e7332` | Modern ext4 checksum-seed support |
| `BakaValen/lwext4` | `6a3f8c0b8ead58328d4c3abde2655c4fb8f4826e` | Sparse growth and out-of-order writes |
| `BakaValen/lwext4` | `bee14736ac426610c0ee3ca002052ff3dc57fdd6` | Safe orphan-file validation |
| `BakaValen/Ext4Windows` | `8d7bf88` | Modern ext4 and Windows operation fixes |
| `BakaValen/Ext4Windows` | `9b426ce` | Windows sparse-copy handling |
| `BakaValen/Ext4Windows` | `8f4cd22` | Linux metadata inheritance and write-mount preflight |
| `BakaValen/Ext4Windows` | `a870752` | Elevated raw-handle retention across remount |

The Ext4Windows commit IDs above are abbreviated because the final documentation commit is created afterward. The gitlinks stored in each Ext4Windows commit use the complete lwext4 SHAs shown in the table.

## 3. Original failures and their causes

| Observed failure | Direct evidence | Cause in the original source | Final fix |
|---|---|---|---|
| Kali ext4 was detected but did not mount | Magic `0xEF53`; masks `Compat=0x0000103C`, `Incompat=0x000022C2`, `ROCompat=0x0000046B`; `ext4_mount` returned 129 | The bundled lwext4 did not recognize `metadata_csum_seed`, had an old superblock tail layout, and derived metadata checksum seeds only from the UUID | Corrected the superblock layout, added `CSUM_SEED` and `ORPHAN_FILE`, and routed every metadata checksum through the correct stored-or-derived seed |
| A successful mount was not visible in normal Explorer | Initial foreground test was run from an Administrator context, then Ctrl+C immediately unmounted it | Raw disk access required elevation, but keeping the filesystem host elevated can put the drive mapping in the elevated logon context | Keep the filesystem process non-elevated; launch a short elevated helper only to open the raw partition and duplicate its handle back |
| `Rename-Item` returned access denied or `A device attached to the system is not functioning` | Debug showed destination `/mnt_Z/HOME/VALEN/EXT4WINDOWS-RW-TEST/renamed.txt` | WinFsp was told the filesystem was case-insensitive even though ext4 is case-sensitive; Windows also needed delete rights that were not represented in the generated ACL | Advertise case-sensitive search, preserve the original context path, and map POSIX parent write+execute permission to Windows `DELETE` and `FILE_DELETE_CHILD` |
| Delete initially failed | `Remove-Item` could not remove the file, then the directory remained non-empty | Same missing Windows delete rights and rename/path handling problem | ACL and case handling changes above |
| New directories showed 1 January 1970 | New directory `CreationTime` and `LastWriteTime` were Unix epoch | lwext4 leaves timestamp policy to its caller; Ext4Windows did not initialize creation or modification times | Convert current Windows `FILETIME` to Unix seconds and set atime, mtime, ctime, and crtime on creation; update mtime and ctime on writes, overwrites, and size changes |
| A 64 MiB `Copy-Item` failed and left no destination | Debug showed `SetFileSize` to 67,108,864 followed by out-of-order writes beginning at offset 2 MiB | lwext4 growth truncate did not update the inode size, sparse holes were mishandled, and block allocation/removal assumed contiguous writes | Implement logical sparse growth, zero reads from holes, allocation at the requested logical block, and correct extent traversal across holes |
| Windows-created files were root-owned in Kali | r5 marker was UID 0, GID 0, mode 0666 in `/home/valen`; actual user was `valen001`, UID/GID 1000, home mode 0700 | New lwext4 entries defaulted to root and no Windows-to-POSIX identity mapping existed | Inherit UID/GID from the parent directory and derive child mode from the parent's access classes |
| A write mount needed orphan safety | Kali showed `orphan_file` and `orphan_present`; lwext4 cannot perform Linux orphan-file recovery | The original mount path could proceed without a specific, fail-closed orphan-file check | Add a read-only preflight; refuse unsafe states; validate the complete checksummed orphan file; clear only a verified-empty stale `ORPHAN_PRESENT` flag |
| r7 passed orphan validation but then failed with `err=2`, `rc=5` | `part_open: CreateFileW failed for '(inherited handle)'` | Probe unmount called the block-device close callback. That closed the only elevated raw handle. The non-elevated process then tried to reopen the diagnostic label as a pathname | r8 retains duplicated handles across internal probe/remount cycles and closes the handle only when the owning block-device object is destroyed |

## 4. Original to r4: modern ext4 plus Windows filesystem semantics

### 4.1 Correct the modern ext4 superblock layout

Changed `external/lwext4/include/ext4_types.h` so `struct ext4_sblock` matches the modern on-disk layout through the checksum at byte `0x3FC`:

- split the old 16-bit reserved field into `encryption_level` and `reserved_pad`;
- added `prj_quota_inum`;
- added the stored `checksum_seed` at offset `0x270`;
- added the high timestamp bytes and error-code bytes;
- added encoding fields;
- added `orphan_file_inum` at offset `0x280`;
- reduced tail padding from 100 to 94 32-bit words;
- preserved the total superblock size of 1,024 bytes;
- preserved the superblock checksum at offset `0x3FC`.

Added feature constants:

```text
EXT4_FCOM_ORPHAN_FILE = 0x00001000
EXT4_FINCOM_CSUM_SEED = 0x00002000
```

`EXT4_FINCOM_CSUM_SEED` was added to the supported incompatible-feature mask. Unit tests enforce the exact size and offsets with compile-time assertions.

### 4.2 Use the correct metadata checksum seed everywhere

Added `ext4_sb_get_csum_seed()` in `external/lwext4/src/ext4_super.c` and declared it in `include/ext4_super.h`.

Its exact rule is:

```text
if metadata_csum_seed is present:
    use the 32-bit seed stored in the superblock
else:
    derive CRC32C from the 16-byte filesystem UUID
```

Replaced the old UUID-only seed initialization in all affected checksum paths:

- block allocation bitmap: `src/ext4_balloc.c`;
- directory block: `src/ext4_dir.c`;
- indexed directory: `src/ext4_dir_idx.c`;
- extent block: `src/ext4_extent.c`;
- block-group descriptor: `src/ext4_fs.c`;
- inode: `src/ext4_fs.c`;
- inode allocation bitmap: `src/ext4_ialloc.c`;
- external xattr block: `src/ext4_xattr.c`.

The feature debug output was also updated to print `csum_seed` and `orphan_file` by their real names.

This fixed the original mount refusal on the Kali mask `0x000022C2`, whose `0x2000` bit is `metadata_csum_seed`.

### 4.3 Fix Windows timestamps

In `src/ext4_filesystem.cpp`:

- added `current_unix_time()` using `GetSystemTimeAsFileTime()`;
- subtracts `116444736000000000` 100-nanosecond ticks between the Windows and Unix epochs;
- divides by 10,000,000 to produce whole Unix seconds;
- clamps values above `UINT32_MAX`;
- added `set_created_timestamps()` for atime, mtime, ctime, and crtime;
- added `set_modified_timestamps()` for mtime and ctime;
- creation calls the first helper;
- overwrite, write, and real EOF size changes call the second helper.

The APIs used by this lwext4 revision store whole seconds, so sub-second Windows precision is not retained.

### 4.4 Fix rename case corruption

The original WinFsp volume parameters declared `CaseSensitiveSearch = FALSE`. That allowed Windows/WinFsp to normalize path components to uppercase. The physical debug log showed the exact bad destination:

```text
/mnt_Z/HOME/VALEN/EXT4WINDOWS-RW-TEST/renamed.txt
```

The fixes were:

- set `CaseSensitiveSearch = TRUE`;
- retain `CasePreservedNames = TRUE`;
- make `OnRename` prefer the original path stored in the open-file context instead of a possibly normalized `FileName` argument;
- log the exact lwext4 `ext4_frename` return code.

### 4.5 Fix Windows delete authorization

POSIX removal permission belongs to the parent directory, while Windows asks for delete access on the object. `OnGetSecurityByName` was changed to bridge that semantic difference:

- if the parent directory has owner write and execute bits (`0300`), add Windows `DELETE` to the object's generated access mask;
- for a writable/searchable directory, add `FILE_DELETE_CHILD`;
- retain read-only restrictions when the volume is mounted read-only.

This removed the Windows-side access-denied block on rename and recursive deletion.

### 4.6 Keep Explorer in the normal desktop session

Raw `\\.\PhysicalDrive...` access needs Administrator rights. The filesystem host should still run as the desktop user so its drive letter is visible to the normal Explorer session.

The server-side `MountPartition` flow was changed to:

1. Detect whether the server is already elevated.
2. If elevated, open the partition directly.
3. If not elevated, launch the same executable with the internal `--open-device` command through `ShellExecuteExW(..., "runas", ...)`.
4. The helper opens the raw device with `CreateFileW` and the requested read-only or read-write access.
5. The helper opens the parent process with `PROCESS_DUP_HANDLE`.
6. It calls `DuplicateHandle(..., DUPLICATE_SAME_ACCESS)` into the non-elevated parent.
7. It writes the target-process handle value to a unique temporary file.
8. The normal process reads that value, removes the temporary file, and builds the partition block device from the already-open handle.

The helper has a 60-second wait in the background-server path. The foreground `--scan` path already uses the same general elevated-handle design with a 15-second wait.

For the portable cross-build, `src/gui/gui_stub.cpp` replaces the WebView2 GUI entry point with `interactive_main()`. The normal MSVC/CMake source build still references `gui_main.cpp`; the portable executable was linked with the stub explicitly.

## 5. r4 to r5: make ordinary Windows copies work

The physical r4 build mounted, created small files, and had corrected rename/delete behavior, but a 64 MiB `Copy-Item` still failed.

The diagnostic request sequence was decisive:

```text
SetFileSize new_size=67108864 alloc=0
Write offset=2097152 len=1048576
Write offset=0       len=1048576
Write offset=1048576 len=1048576
Write offset=3145728 len=1048576
...
```

Windows first set the final EOF and then wrote 1 MiB chunks out of order. Four lwext4 paths were corrected.

### 5.1 Sparse growth in `ext4_ftruncate_no_lock`

When `new_size` exceeds the current size, the function now:

- updates the inode's logical size;
- marks the inode reference dirty;
- updates the open file's cached size;
- leaves the new range as a sparse hole;
- returns success without allocating every intervening block.

Equal-size truncation is a no-op. Shrink still uses the normal truncate path.

### 5.2 Zero-fill sparse reads in `ext4_fread`

For an unmapped logical block (`physical block == 0`), reads now fill the caller's buffer with zeros. Contiguous physical reads are coalesced only while the physical block sequence is actually adjacent. Partial leading and trailing hole reads are handled as zero data as well.

### 5.3 Allocate the requested logical block

`ext4_fs_init_inode_dblk_idx()` now handles a non-extent sparse hole by:

- finding an allocation goal;
- allocating a physical block;
- attaching it to the exact requested logical index;
- freeing it again if attaching the mapping fails.

This prevents an out-of-order write from targeting physical block zero.

### 5.4 Traverse extents across holes during removal

`ext4_extent_remove_space()` now advances from a preceding extent or exhausted leaf to the first extent at or after the requested removal range. This makes truncation and deletion of sparse, out-of-order-written files remove all relevant extents correctly.

### 5.5 Detect short writes at the WinFsp boundary

`OnWrite` now treats either a nonzero lwext4 return code or `bytes_written != requested_length` as failure. `OnWrite` and `OnSetFileSize` received detailed offset, length, old-size, new-size, and return-code debug output.

The expanded image test exactly reproduces the Windows pattern: grow to 64 MiB, read an unwritten hole as zero, write chunk order 2, 0, 1, 3 through 63, read every byte back, delete, stop the journal, and unmount.

## 6. r5 to r6: Linux ownership plus fail-closed read-write safety

### 6.1 Parent metadata inheritance

Added `inherit_created_metadata()` in `src/ext4_filesystem.cpp`.

For every new file or directory it:

1. Finds the parent ext4 path.
2. Reads the parent's UID, GID, and mode.
3. Assigns the child the same UID and GID.
4. Derives child access bits from the parent:

| Parent | New file | New directory |
|---:|---:|---:|
| 0700 | 0600 | 0700 |
| 0755 | 0644 | 0755 |
| 0777 | 0666 | 0777 |

5. Copies the setgid bit to a new child directory when the parent has setgid.
6. Rolls the newly created object back if owner or mode assignment fails.

This is inheritance, not Windows SID-to-Linux UID mapping. It was chosen because creating inside `/home/valen001` should produce objects owned by that directory's Linux owner rather than root.

### 6.2 Read-only preflight before every requested write mount

Added two Ext4Windows-specific status values:

```text
0xC0E40001  Linux orphan recovery required
0xC0E40002  unsupported feature for safe read-write exposure
```

Added `EXT4_FRO_COM_ORPHAN_PRESENT = 0x00010000` to the modern superblock feature definitions.

Before a requested write mount, `Ext4FileSystem::Mount` now performs a logical read-only lwext4 mount and reads:

- `s_last_orphan`;
- read-only-compatible feature bits;
- superblock state.

It refuses read-write if:

- `s_last_orphan` is nonzero;
- `EXT4_SUPERBLOCK_STATE_ORPHAN_FS` is set;
- any remaining read-only-compatible feature is outside `CONFIG_SUPPORTED_FRO_COM`.

The explicit unsupported-RO check is important. Upstream lwext4 can internally downgrade itself to read-only for an unknown RO-compatible feature; without this guard, WinFsp could still be told that the Windows volume was writable.

The CLI and server translate both status codes into actionable messages and preserve read-only mounting as a fallback.

## 7. r6 to r7: validate, then safely clear a stale empty orphan file

r6 treated any `ORPHAN_PRESENT` bit as a hard refusal. The physical Kali filesystem had the bit set, but its orphan file was valid and empty. r7 added a fail-closed validator rather than blindly clearing the flag.

### 7.1 New lwext4 API

Added:

```c
int ext4_orphan_file_check_empty(const char *mount_point, bool *empty);
```

The implementation in `external/lwext4/src/ext4.c` performs all of these checks under the mount lock:

1. `orphan_file` compatible feature must be present.
2. `orphan_file_inum` must be nonzero and no greater than the filesystem inode count.
3. The orphan inode must load successfully.
4. It must be a regular file.
5. Its size must be nonzero and exactly divisible by the filesystem block size.
6. It is limited defensively to 512 blocks.
7. Every logical orphan-file block must map to a nonzero physical block. Holes are rejected.
8. Every block tail must contain magic `0x0B10CA04`.
9. With `metadata_csum`, the per-inode checksum seed is calculated from the filesystem checksum seed, little-endian orphan inode number, and inode generation.
10. Each block checksum is CRC32C over the little-endian physical block number followed by every orphan inode slot.
11. The stored checksum must match.
12. Every inode slot must be zero for the file to be reported empty.

Any malformed block, bad checksum, missing block, invalid inode, or nonzero entry fails closed.

### 7.2 Safe flag-clear rule

If and only if all of the following are true:

- `s_last_orphan == 0`;
- `ORPHAN_FS` state is clear;
- `ORPHAN_PRESENT` is set;
- the entire orphan file validates;
- every orphan slot is zero;

r7 clears only `EXT4_FRO_COM_ORPHAN_PRESENT` in the superblock and writes the superblock through `ext4_sb_write()`, which updates its checksum. It does not delete or rewrite the orphan file. A real orphan entry or validation error causes a write-mount refusal.

### 7.3 Test-only tools

Added:

- `tests/orphan_file_probe_image.c` for inspect, refusal, and clear-if-empty behavior;
- `tests/orphan_file_fixture_image.c` to insert one valid checksummed entry into an explicit image, physical block, and inode generation;
- refusal tests for a valid nonempty orphan file and a deliberately corrupted orphan block.

The fixture takes an ordinary image filename, not a device path discovered at runtime. This prevents the test helper from accidentally selecting a live disk.

## 8. r7 to r8: preserve the elevated raw handle across probe/remount

The physical r7 output proved the orphan logic itself worked:

```text
probe last_orphan=0 ro_features=0x0001046B state=0x0001
orphan-file validation rc=0 empty=1
verified orphan file is empty; cleared stale ORPHAN_PRESENT
```

The next mount then failed:

```text
part_open: CreateFileW failed for '(inherited handle)' (err=2)
ext4_mount failed rc=5
```

The read-only probe's `ext4_umount()` called the partition interface's `part_close()`. For a handle duplicated from the elevated helper, closing it was terminal: the normal process did not have permission to reopen the raw partition, and `(inherited handle)` was only a diagnostic label.

r8 made the following exact changes in `src/blockdev_partition.cpp` and `.hpp`:

- added `retain_handle_between_mounts` to `partition_blockdev_data`;
- set it `false` for block devices created from a pathname;
- set it `true` for block devices created from an inherited handle;
- make `part_close()` return success without closing when retention is enabled;
- keep using the already-open handle in the next `part_open()`;
- retain ownership in the block-device object;
- close the real handle in `destroy_partition_blockdev()` on final failure or final unmount.

This is the only source difference between retained r7 and r8.

## 9. Exact changed-file inventory

### Ext4Windows layer

- `src/blockdev_partition.cpp`
- `src/blockdev_partition.hpp`
- `src/ext4_filesystem.cpp`
- `src/ext4_filesystem.hpp`
- `src/main.cpp`
- `src/server.cpp`
- new `src/gui/gui_stub.cpp`
- `tests/CMakeLists.txt`
- new `tests/test_modern_ext4.cpp`
- new `tests/modern_ext4_rw_image.c`
- new `tests/orphan_file_probe_image.c`
- new `tests/orphan_file_fixture_image.c`

### Bundled lwext4 layer

- `external/lwext4/include/ext4.h`
- `external/lwext4/include/ext4_super.h`
- `external/lwext4/include/ext4_types.h`
- `external/lwext4/src/ext4.c`
- `external/lwext4/src/ext4_balloc.c`
- `external/lwext4/src/ext4_dir.c`
- `external/lwext4/src/ext4_dir_idx.c`
- `external/lwext4/src/ext4_extent.c`
- `external/lwext4/src/ext4_fs.c`
- `external/lwext4/src/ext4_ialloc.c`
- `external/lwext4/src/ext4_super.c`
- `external/lwext4/src/ext4_xattr.c`

The patch beside this report is the authoritative line-by-line record.

## 10. Build and release assembly

### 10.1 Verified build facts

- compiler package: llvm-mingw `20260826`;
- compiler: LLVM/Clang `23.1.0`;
- target: `x86_64-w64-windows-gnu`;
- output: PE32+ x86-64 Windows console executable;
- subsystem version: 6.0;
- GUI choice: portable console/TUI stub, not WebView2;
- linked filesystem bridge: `winfsp-x64.dll`;
- final executable imports WinFsp plus Windows system/UCRT DLLs;
- complete patched source and GPL-2.0 license are in the release ZIP.

The portable build compiled the Ext4Windows C++ sources, all required lwext4 C sources, the resource file, the console GUI stub, and linked against a generated x64 WinFsp import library. The original CMake project still describes the normal Visual Studio/WebView2 GUI build; the portable release intentionally omits that GUI dependency.

`[Unverified]` The original one-off shell command lines and their exact optimization switches were not retained as a build script. They are not reconstructed here as if they were facts. The exact source delta, compiler identity, PE result, source archive, binaries, and hashes are retained.

### 10.2 r8 release contents and hashes

| File | SHA256 |
|---|---|
| `ext4windows.exe` | `156410c94b53fac34afddf44693e215371f34289bedb9e75d56bada397a8fe50` |
| `winfsp-x64.dll` | `08d7389b8d030770a4de108a60a86047d5cdd00b9ecafc6dd66e957bc51c2437` |
| `Ext4Windows-modern-ext4-source-r8.tar.gz` | `8c13cf24764476a11f63704d7e906b8129b8f2a14a9be13f2d7d9561c9682d6a` |
| `LICENSE.txt` | `edaef632cbb643e4e7a221717a6c441a4c1a7c918e6e4d56debc3d8739b233f6` |
| `README.txt` | `84a9fafca9e78f8533b247b1724502ee87c62004cfc909bc8af56b5d2491cac6` |
| `TEST-RESULTS.txt` | `180e401756dee85f2a0bf4bd08f5dc5a77ab75ac781cf86eaecdc3ce19976f6e` |
| `ext4windows.ico` | `8d9b26a86ffdbb9d68c7ed22c1f14427ea1e5c890f57dfdcf30c77b78a20e8fb` |
| complete `Ext4Windows-modern-ext4-x64-r8.zip` | `2d06d48f09072b8a07df12076e3c27d6ef22f17521d020f5528f86a99dc1bd8f` |

File sizes:

```text
ext4windows.exe                         1,925,632 bytes
winfsp-x64.dll                           198,376 bytes
Ext4Windows-modern-ext4-source-r8.tar.gz 24,280,716 bytes
Ext4Windows-modern-ext4-x64-r8.zip       25,165,589 bytes
```

## 11. Validation record

### 11.1 Modern and legacy image tests

The modern image carried the exact Kali feature masks:

```text
Compatible:            0x0000103C
Incompatible:          0x000022C2
Read-only-compatible:  0x0000046B
```

The native modern-superblock tests passed both cases:

- stored `metadata_csum_seed` is used when the feature is present;
- legacy `metadata_csum` derives the seed from the UUID.

The 512 MiB modern image passed:

- 1 MiB create/write/readback;
- 3,000-byte external xattr write and byte-for-byte readback;
- rename and delete;
- directory create, timestamp set/get, and delete;
- grow to a 64 MiB sparse file;
- zero read from an unwritten hole;
- 64 out-of-order 1 MiB writes;
- complete byte-for-byte readback;
- deletion, cache flush, journal stop, and clean unmount.

`e2fsck -fn` completed all five passes:

```text
KALI_RW_TEST: 12/32768 files, 6385/131072 blocks
```

The same integration test passed an older `metadata_csum` image without `metadata_csum_seed` or `orphan_file`:

```text
LEGACY_TEST: 11/65536 files, 8268/65536 blocks
```

### 11.2 Orphan-file tests

The following passed:

- valid empty checksummed orphan file: `rc=0 empty=1`;
- safe `ORPHAN_PRESENT` clear on an empty file;
- clean `e2fsck -fn` after the clear;
- valid checksummed nonempty entry: refused as `empty=0`;
- corrupted orphan block: refused with validation error.

### 11.3 Physical Windows tests

The final 64 MiB test reported:

```text
Copy64MiB         : PASS
SourceLength      : 67108864
DestinationLength : 67108864
SHA256Match       : True
Rename            : PASS
Delete            : PASS
```

The source and destination SHA256 were:

```text
3B6A07D0D404FAB4E23B6D34BC6696A6A312DD92821332385E5AF7C01C421351
```

A directory timestamp read before deletion was:

```text
CreationTime  : 01/09/2026 22:50:35
LastWriteTime : 01/09/2026 22:50:35
```

An earlier combined script printed year 1601 for a directory object after that directory had already been deleted. That value was not reproduced when metadata was read before removal. The corrected validation reads timestamps before deletion.

### 11.4 Cross-OS physical proof

Windows wrote:

```text
Path:  Z:\home\valen001\ext4windows-r8-cross-os-test.txt
Size:  105
UTC:   2026-09-01T21:50:44.2796789Z
Token: d60c5042-b30e-427e-88c1-ff3ab4ec2d02
SHA256: AC525FE17FC6CB415A28B3B73DF3C63E311330DB75E38166E2F5DF5F9D9428C7
```

Kali read the same content and lowercase-equivalent hash:

```text
ac525fe17fc6cb415a28b3b73df3c63e311330db75e38166e2f5df5f9d9428c7
```

Kali metadata was:

```text
UID:      1000
GID:      1000
Mode:     600
Size:     105
Created:  2026-09-01 22:50:45 +0100
Modified: 2026-09-01 22:50:45 +0100
```

This verified both byte preservation and parent-owner inheritance inside the real `/home/valen001` directory.

### 11.5 Clean-unmount investigation

A controlled Windows run created a marker and then produced:

```text
Unmount: ext4_journal_stop rc=0
Unmounting... OK!
```

Kali still logged `orphan cleanup on readonly fs` at boot. The same message appeared after a Kali-to-Kali clean shutdown and reboot with no intervening Windows mount.

`[Inference]` Because the message was independently reproduced without Ext4Windows, that log line alone cannot be attributed to r8. It is not proof that r8 left a new orphan. The supplied checks showed a clean filesystem and did not show an EXT4 error, warning, JBD2 error, or journal abort. r8 nevertheless retains the preflight and refuses a real nonempty or invalid orphan state.

The Linux orphan-file format and cleanup mechanism are documented in the [Linux ext4 orphan-file documentation](https://docs.kernel.org/filesystems/ext4/orphan.html) and implemented in the [Linux ext4 orphan source](https://github.com/torvalds/linux/blob/master/fs/ext4/orphan.c).

### 11.6 Final r8 result

After the final r8 fix, the physical debug sequence included:

```text
probe last_orphan=0 ro_features=0x0000046B state=0x0001
part_close: retaining inherited handle for remount
part_open: using pre-opened handle
ext4_mount OK
ext4_recover OK
ext4_journal_start OK
FspFileSystemSetMountPoint OK
FspFileSystemStartDispatcher OK
```

The final normal, non-debug invocation mounted and unmounted successfully:

```text
Mounted at Z:\ (read-write)
Press Ctrl+C to unmount.
Unmounting... OK!
```

## 12. Exact procedure used to run r8 successfully

### Before leaving Kali

Shut down cleanly:

```bash
sudo poweroff
```

Wait until the machine is fully off before booting Windows. Never mount the same filesystem from Windows and Linux simultaneously.

### In Windows

1. Install WinFsp if its driver is not already installed.
2. Extract every file from the r8 ZIP into one directory. Do not run the executable from inside the ZIP.
3. Open a normal, non-Administrator PowerShell or Windows Terminal in that directory.
4. Verify the executable:

```powershell
(Get-FileHash .\ext4windows.exe -Algorithm SHA256).Hash
```

Expected:

```text
156410C94B53FAC34AFDDF44693E215371F34289BEDB9E75D56BADA397A8FE50
```

SHA256 is the correct algorithm here. MD5 was not used.

5. Mount in the foreground:

```powershell
.\ext4windows.exe --scan Z: --rw
```

6. Accept the UAC prompts for disk scanning and opening.
7. Select the ext4 partition if prompted.
8. Leave that terminal running for the entire mount.
9. Use `Z:` in normal Explorer or PowerShell.
10. Close every Explorer window and application using `Z:`.
11. Press Ctrl+C once in the mounting terminal.
12. Wait for `Unmounting... OK!` before rebooting or unplugging.

Use debug only when diagnosing a problem:

```powershell
.\ext4windows.exe --scan Z: --rw --debug
```

Debug logs every filesystem request, so hashing a large file produces a great deal of output.

`--scan` and `scan` are different modes. `--scan` is the foreground scanner/mounter used for this portable build. Bare `scan`, `quit`, `mount`, `unmount`, and `status` are client commands for the background server. The successful r8 procedure did not require starting that server or running `quit` first.

If the safety preflight refuses write access, use read-only mode:

```powershell
.\ext4windows.exe --scan Z: --ro
```

Then boot Linux for recovery rather than forcing a Windows write mount.

## 13. Supported feature boundary and remaining limitations

The final supported ext4 masks in the bundled lwext4 include:

- incompatible: `filetype`, `meta_bg`, `extents`, `flex_bg`, `64bit`, and `metadata_csum_seed`;
- read-only-compatible for read-write: `sparse_super`, `metadata_csum`, `large_file`, `gdt_csum`, `dir_nlink`, `extra_isize`, and `huge_file`;
- compatible `dir_index`, plus understood `orphan_file` safety metadata.

Important limits:

- r8 does not implement Linux orphan recovery. It only clears `ORPHAN_PRESENT` after proving that the checksummed orphan file is completely empty.
- Unknown or unsupported read-only-compatible bits cause a requested write mount to be refused.
- Encryption, casefold, verity, inline data, bigalloc, quota-dependent layouts, and external-journal arrangements are outside this tested write boundary.
- LUKS, LVM, RAID assembly, and other container layers are not provided by Ext4Windows.
- Windows SIDs are not mapped to Linux users. New objects inherit the parent UID/GID. The existing WinFsp ACL translation exposes owner-level access to the Windows user rather than enforcing a complete POSIX identity model.
- ext4 remains case-sensitive on Windows. Windows applications that assume case-insensitive semantics may behave differently.
- timestamps written through these lwext4 APIs have one-second resolution.
- the executable is unsigned and may trigger UAC or SmartScreen warnings.
- WinFsp must remain installed; the adjacent DLL alone is not the kernel driver.
- the foreground terminal is the mount lifetime. Closing it or pressing Ctrl+C unmounts the drive.
- clean unmount and clean OS shutdown remain mandatory for read-write use.

The tested claim is therefore not "every ext4 filesystem." The verified claim is that r8 safely mounted and wrote the specified ordinary Kali ext4 layout, retained compatibility with the legacy checksum image, and refused unsupported or unsafe states instead of silently exposing them as writable.

## 14. Applying the exact source patch

Starting from the exact Ext4Windows commit and initialized lwext4 submodule listed in section 2:

```bash
patch --dry-run -p1 < Ext4Windows-r8-upstream-to-r8.patch
patch -p1 < Ext4Windows-r8-upstream-to-r8.patch
```

The patch includes both top-level Ext4Windows files and files inside `external/lwext4`. It also adds the portable GUI stub and the four test sources. The r8 source tar in the release is the second independent copy of the final source state.
