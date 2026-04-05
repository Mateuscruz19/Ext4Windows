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
#define MyAppURL "https://github.com/user/ext4windows"
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
// Check if WinFsp is installed (required dependency)
function IsWinFspInstalled: Boolean;
var
  InstallDir: String;
begin
  Result := RegQueryStringValue(HKLM, 'SOFTWARE\WinFsp', 'InstallDir', InstallDir);
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

// Warn if WinFsp is not installed
function InitializeSetup(): Boolean;
begin
  Result := True;
  if not IsWinFspInstalled then begin
    if MsgBox(
      'WinFsp is required but not installed.' + #13#10 +
      'Download it from https://winfsp.dev/rel/' + #13#10 + #13#10 +
      'Continue installation anyway?',
      mbConfirmation, MB_YESNO) = IDNO
    then
      Result := False;
  end;
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
