; Inno Setup Script for Ext4Windows
; https://jrsoftware.org/isinfo.php
;
; To compile this installer:
;   1. Install Inno Setup from https://jrsoftware.org/isdl.php
;   2. Open this file in Inno Setup Compiler
;   3. Click Build > Compile (or Ctrl+F9)
;
; The installer will be created in the "Output" folder.

#define MyAppName "Ext4Windows"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "Ext4Windows"
#define MyAppURL "https://github.com/Mateuscruz19/Ext4Windows"
#define MyAppExeName "ext4windows.exe"

[Setup]
AppId={{E4W-2026-EXT4-WINDOWS-DRIVER}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
OutputBaseFilename=Ext4Windows-{#MyAppVersion}-setup
SetupIconFile=..\assets\ext4windows.ico
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
UninstallDisplayIcon={app}\{#MyAppExeName}
LicenseFile=..\LICENSE

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "portuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "german"; MessagesFile: "compiler:Languages\German.isl"
Name: "french"; MessagesFile: "compiler:Languages\French.isl"
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "addtopath"; Description: "Add to system PATH"; GroupDescription: "System integration:"
Name: "autostart"; Description: "Start on Windows login"; GroupDescription: "System integration:"

[Files]
; Main executable
Source: "..\build\ext4windows.exe"; DestDir: "{app}"; Flags: ignoreversion

; WinFsp runtime DLL
Source: "..\build\winfsp-x64.dll"; DestDir: "{app}"; Flags: ignoreversion

; License
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
; Start Menu
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"

; Desktop (optional)
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; Auto-start on login (optional)
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
    ValueType: string; ValueName: "Ext4Windows"; \
    ValueData: """{app}\{#MyAppExeName}"" --server"; \
    Tasks: autostart; Flags: uninsdeletevalue

; Add to PATH (optional)
Root: HKLM; Subkey: "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"; \
    ValueType: expandsz; ValueName: "Path"; \
    ValueData: "{olddata};{app}"; \
    Tasks: addtopath; Check: NeedsAddPath(ExpandConstant('{app}'))

[Run]
; Launch after install
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; \
    Flags: nowait postinstall skipifsilent

[UninstallRun]
; Stop server before uninstall
Filename: "{app}\{#MyAppExeName}"; Parameters: "quit"; \
    Flags: runhidden waituntilterminated; RunOnceId: "StopServer"

[UninstallDelete]
; Clean up debug logs
Type: files; Name: "{app}\debug.log"
Type: files; Name: "{app}\*.log"

[Code]
// WinFsp MSI download URL (latest stable release)
const
  WinFspMsiUrl = 'https://github.com/winfsp/winfsp/releases/download/v2.1/winfsp-2.1.25156.msi';

// Check if WinFsp is installed (required dependency)
function IsWinFspInstalled: Boolean;
var
  InstallDir: String;
begin
  Result := RegQueryStringValue(HKLM, 'SOFTWARE\WinFsp', 'InstallDir', InstallDir)
         or RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\WinFsp', 'InstallDir', InstallDir);
end;

// Check if path already contains our directory
function NeedsAddPath(Param: string): boolean;
var
  OrigPath: string;
begin
  if not RegQueryStringValue(HKLM,
    'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
    'Path', OrigPath)
  then begin
    Result := True;
    exit;
  end;
  Result := Pos(';' + Param + ';', ';' + OrigPath + ';') = 0;
end;

// Download a file from URL to a local path using PowerShell
function DownloadFile(const Url, DestPath: String): Boolean;
var
  ExitCode: Integer;
begin
  Result := Exec('powershell.exe',
    '-NoProfile -ExecutionPolicy Bypass -Command ' +
    '"[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; ' +
    'Invoke-WebRequest -Uri ''' + Url + ''' -OutFile ''' + DestPath + '''"',
    '', SW_HIDE, ewWaitUntilTerminated, ExitCode);
  Result := Result and (ExitCode = 0) and FileExists(DestPath);
end;

// Install WinFsp silently from a downloaded MSI
function InstallWinFsp(const MsiPath: String): Boolean;
var
  ExitCode: Integer;
begin
  Result := Exec('msiexec.exe',
    '/i "' + MsiPath + '" /qn /norestart',
    '', SW_HIDE, ewWaitUntilTerminated, ExitCode);
  Result := Result and (ExitCode = 0);
end;

// Before installation: check for WinFsp and auto-install if missing
function InitializeSetup(): Boolean;
var
  TmpMsi: String;
begin
  Result := True;

  if IsWinFspInstalled then
    Exit;

  // WinFsp not found — ask user to auto-install
  if MsgBox(
    'WinFsp is required but not installed.' + #13#10 + #13#10 +
    'WinFsp is a free, open-source framework that allows Ext4Windows ' +
    'to create virtual drives on Windows. Without it, the application cannot work.' + #13#10 + #13#10 +
    'Download and install WinFsp automatically?',
    mbConfirmation, MB_YESNO) = IDNO
  then begin
    // User declined — warn and abort
    MsgBox(
      'Ext4Windows cannot work without WinFsp.' + #13#10 +
      'You can install it manually from https://winfsp.dev/rel/' + #13#10 + #13#10 +
      'Installation will now exit.',
      mbError, MB_OK);
    Result := False;
    Exit;
  end;

  // Download WinFsp MSI to temp folder
  TmpMsi := ExpandConstant('{tmp}\winfsp-setup.msi');

  if not DownloadFile(WinFspMsiUrl, TmpMsi) then begin
    MsgBox(
      'Failed to download WinFsp.' + #13#10 + #13#10 +
      'Please check your internet connection and try again, ' +
      'or install WinFsp manually from https://winfsp.dev/rel/',
      mbError, MB_OK);
    Result := False;
    Exit;
  end;

  // Install WinFsp silently
  if not InstallWinFsp(TmpMsi) then begin
    MsgBox(
      'WinFsp installation failed.' + #13#10 + #13#10 +
      'Please install it manually from https://winfsp.dev/rel/' + #13#10 +
      'Then run the Ext4Windows installer again.',
      mbError, MB_OK);
    Result := False;
    Exit;
  end;

  // Verify installation succeeded
  if not IsWinFspInstalled then begin
    MsgBox(
      'WinFsp installation did not complete correctly.' + #13#10 + #13#10 +
      'Please install it manually from https://winfsp.dev/rel/',
      mbError, MB_OK);
    Result := False;
    Exit;
  end;

  MsgBox('WinFsp installed successfully!', mbInformation, MB_OK);
end;

// Remove from PATH on uninstall
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Path: string;
  AppDir: string;
  P: Integer;
begin
  if CurUninstallStep = usPostUninstall then begin
    AppDir := ExpandConstant('{app}');
    if RegQueryStringValue(HKLM,
      'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
      'Path', Path) then begin
      P := Pos(';' + AppDir, Path);
      if P > 0 then begin
        Delete(Path, P, Length(';' + AppDir));
        RegWriteExpandStringValue(HKLM,
          'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
          'Path', Path);
      end;
    end;
  end;
end;
