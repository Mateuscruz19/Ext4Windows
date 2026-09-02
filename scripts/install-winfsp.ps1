[CmdletBinding()]
param(
    [Parameter()]
    [string] $Destination = (Join-Path $env:TEMP 'winfsp-2.1.25156.msi'),

    [Parameter()]
    [switch] $Install
)

$ErrorActionPreference = 'Stop'

$winFspVersion = '2.1.25156'
$winFspUrl = 'https://github.com/winfsp/winfsp/releases/download/v2.1/winfsp-2.1.25156.msi'
$winFspSha256 = '073A70E00F77423E34BED98B86E600DEF93393BA5822204FAC57A29324DB9F7A'

$destinationPath = [IO.Path]::GetFullPath($Destination)
$destinationDirectory = Split-Path -Parent $destinationPath

if (-not (Test-Path -LiteralPath $destinationDirectory)) {
    New-Item -ItemType Directory -Path $destinationDirectory | Out-Null
}

Write-Host "Downloading WinFsp $winFspVersion from the official release..."
Invoke-WebRequest -Uri $winFspUrl -OutFile $destinationPath

$actualSha256 = (Get-FileHash -LiteralPath $destinationPath -Algorithm SHA256).Hash
if ($actualSha256 -ne $winFspSha256) {
    throw "WinFsp SHA-256 mismatch. Expected $winFspSha256 but received $actualSha256."
}

Write-Host "Verified WinFsp SHA-256: $actualSha256"

if ($Install) {
    Write-Host "Installing WinFsp $winFspVersion..."
    $arguments = @(
        '/i',
        "`"$destinationPath`"",
        'ADDLOCAL=F.Main,F.User,F.Developer',
        '/qn',
        '/norestart'
    )
    $process = Start-Process -FilePath 'msiexec.exe' -ArgumentList $arguments -Wait -PassThru

    if ($process.ExitCode -notin @(0, 1641, 3010)) {
        throw "WinFsp installation failed with Windows Installer exit code $($process.ExitCode)."
    }

    if ($process.ExitCode -in @(1641, 3010)) {
        Write-Warning 'WinFsp installed successfully, but Windows must be restarted.'
    }
}

[PSCustomObject]@{
    Version = $winFspVersion
    Path = $destinationPath
    SHA256 = $actualSha256
    Installed = [bool] $Install
}
