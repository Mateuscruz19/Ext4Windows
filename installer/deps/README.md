# Installer dependencies

The WinFsp MSI is not committed to this repository.

Before compiling `installer/ext4windows.iss` locally, run this from the
repository root in PowerShell:

```powershell
.\scripts\install-winfsp.ps1 -Destination installer\deps\winfsp-2.1.25156.msi
```

The script downloads the fixed official release and refuses a SHA-256
mismatch. GitHub Actions runs the same script before building a release.
