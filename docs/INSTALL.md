# Ext4Windows installation

## What the installer contains

The setup is an offline installer. It contains:

- Ext4Windows;
- the matching `winfsp-x64.dll` user-mode library;
- the official WinFsp `2.1.25156` MSI;
- the project license and the modern ext4 technical fix record
  (`modern-ext4-r8-fix-record.md`, contributed by @BakaValen).

WinFsp `2.1.25156` is the approved dependency for this release. The official
MSI SHA-256 is:

```text
073A70E00F77423E34BED98B86E600DEF93393BA5822204FAC57A29324DB9F7A
```

The release workflow downloads the MSI from the fixed official GitHub release
URL, verifies that hash, and only then builds the Ext4Windows installer. The
installer does not download executable code at installation time.

If WinFsp `2.1.25156` or a newer version is already installed, setup leaves it
alone. If WinFsp is missing or older, setup installs the embedded MSI. Windows
may require a restart after a WinFsp upgrade.

Official WinFsp release:
<https://github.com/winfsp/winfsp/releases/tag/v2.1>

## Install

1. Cleanly unmount every Ext4Windows volume.
2. Close Ext4Windows, including its tray process.
3. Run `Ext4Windows-<version>-setup.exe`.
4. Accept the Administrator prompt.
5. Leave `Start on Windows login` disabled unless you specifically want it.
6. Complete setup and restart Windows if setup requests it.
7. Launch Ext4Windows from the Start menu.

Installing over a previous Ext4Windows version upgrades it in place:

```text
C:\Program Files\Ext4Windows
```

## Mount a physical ext4 partition

Open a normal, non-Administrator PowerShell or Windows Terminal and run:

```powershell
ext4windows.exe --scan Z: --rw
```

Accept the Administrator prompts used to scan and open the physical disk. Keep
the mounting terminal open. The filesystem host remains in the normal desktop
session so the drive is visible in the normal File Explorer session.

Before rebooting, shutting down, or disconnecting the disk:

1. Close every program and Explorer window using the mounted drive.
2. Press Ctrl+C once in the mounting terminal.
3. Wait for `Unmounting... OK!`.

If read-write safety checks refuse the filesystem, use read-only mode:

```powershell
ext4windows.exe --scan Z: --ro
```

Boot Linux and let Linux perform recovery. Do not force a Windows write mount.

## Updating the pinned WinFsp release

Do not replace the dependency with an unversioned `latest` download. For a new
WinFsp release:

1. Read the official WinFsp release notes.
2. Record the exact MSI filename, version, fixed release URL, size, and SHA-256.
3. Update `scripts/install-winfsp.ps1`.
4. Update the filename and minimum file version in `installer/ext4windows.iss`.
5. Update both GitHub Actions workflows.
6. Build and test Ext4Windows against that exact version.
7. Publish a new Ext4Windows release with a new SHA-256 manifest.

This keeps old releases reproducible and prevents an installer built today from
executing a different dependency in the future.
