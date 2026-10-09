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
#define MyAppVersion "1.3.0"
#define MyAppPublisher "Ext4Windows"
#define MyAppURL "https://github.com/Mateuscruz19/Ext4Windows"
#define MyAppExeName "ext4windows.exe"
#define WinFspMsiName "winfsp-2.1.25156.msi"

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
WizardImageFile=wizard_large.bmp
WizardSmallImageFile=wizard_small.bmp
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
AppMutex=Local\Ext4WindowsServer
UninstallDisplayIcon={app}\{#MyAppExeName}
LicenseFile=..\LICENSE
VersionInfoVersion=1.3.0.0

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
Name: "autostart"; Description: "Start on Windows login"; GroupDescription: "System integration:"; Flags: unchecked

[Files]
; Main executable
Source: "..\build\ext4windows.exe"; DestDir: "{app}"; Flags: ignoreversion

; WinFsp runtime DLL
Source: "..\build\winfsp-x64.dll"; DestDir: "{app}"; Flags: ignoreversion

; Exact WinFsp system installer, downloaded and SHA-256 verified by CI.
; dontcopy embeds it without leaving it in the application directory.
Source: "deps\{#WinFspMsiName}"; \
    Hash: "073a70e00f77423e34bed98b86e600def93393ba5822204fac57a29324db9f7a"; \
    Flags: dontcopy noencryption

; Icon file (used for custom drive icons in Explorer)
Source: "..\assets\ext4windows.ico"; DestDir: "{app}"; Flags: ignoreversion

; License
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

; Installation, dependency, and safety notes
Source: "..\docs\INSTALL.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\docs\modern-ext4-r8-fix-record.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
; Start Menu
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Installation and safety notes"; Filename: "{app}\INSTALL.md"
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
    Flags: nowait postinstall skipifsilent runasoriginaluser; \
    Check: CanLaunchExt4Windows

[UninstallRun]
; Stop server before uninstall
Filename: "{app}\{#MyAppExeName}"; Parameters: "quit"; \
    Flags: runhidden waituntilterminated; RunOnceId: "StopServer"

[UninstallDelete]
; Clean up debug logs
Type: files; Name: "{app}\debug.log"
Type: files; Name: "{app}\*.log"

[Code]
var
  WinFspRestartRequired: Boolean;

const
  // Required file version: 2.1.25156.0
  WinFspRequiredVersionMS = $00020001;
  WinFspRequiredVersionLS = $62440000;

function GetWinFspInstallDir(var InstallDir: String): Boolean;
begin
  Result := RegQueryStringValue(
    HKLM, 'SOFTWARE\WinFsp', 'InstallDir', InstallDir)
    or RegQueryStringValue(
      HKLM, 'SOFTWARE\WOW6432Node\WinFsp', 'InstallDir', InstallDir);
end;

function IsWinFspCurrent: Boolean;
var
  InstallDir: String;
  DllPath: String;
  VersionMS: Cardinal;
  VersionLS: Cardinal;
begin
  Result := False;
  if not GetWinFspInstallDir(InstallDir) then
    Exit;

  DllPath := AddBackslash(InstallDir) + 'bin\winfsp-x64.dll';
  if not GetVersionNumbers(DllPath, VersionMS, VersionLS) then
    Exit;

  Result := (VersionMS > WinFspRequiredVersionMS)
    or ((VersionMS = WinFspRequiredVersionMS)
      and (VersionLS >= WinFspRequiredVersionLS));
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

// Install the exact WinFsp MSI embedded in this setup. The release workflow
// downloads it from the official fixed-version URL and verifies its SHA-256
// before Inno Setup is allowed to package it.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  MsiPath: String;
  ExitCode: Integer;
begin
  Result := '';

  if IsWinFspCurrent then begin
    Log('WinFsp 2.1.25156 or newer is already installed.');
    Exit;
  end;

  ExtractTemporaryFile('{#WinFspMsiName}');
  MsiPath := ExpandConstant('{tmp}\{#WinFspMsiName}');

  if not Exec(
    ExpandConstant('{sys}\msiexec.exe'),
    '/i "' + MsiPath + '" /qn /norestart',
    '', SW_HIDE, ewWaitUntilTerminated, ExitCode)
  then begin
    Result := 'Windows could not start the bundled WinFsp installer.';
    Exit;
  end;

  if (ExitCode <> 0) and (ExitCode <> 1641) and (ExitCode <> 3010) then begin
    Result := Format('The bundled WinFsp installer failed with Windows Installer exit code %d.', [ExitCode]);
    Exit;
  end;

  if (ExitCode = 1641) or (ExitCode = 3010) then begin
    WinFspRestartRequired := True;
    Log('WinFsp installation requested a Windows restart.');
  end;

  if (not WinFspRestartRequired) and (not IsWinFspCurrent) then
    Result := 'WinFsp installation completed, but version 2.1.25156 or newer was not detected.';
end;

function CanLaunchExt4Windows: Boolean;
begin
  Result := not WinFspRestartRequired;
end;

function NeedRestart: Boolean;
begin
  Result := WinFspRestartRequired;
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
