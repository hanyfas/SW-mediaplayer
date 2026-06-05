; Inno Setup script for Signage Player
; Runs on the GitHub Actions Windows runner — no manual steps needed.

#define AppName      "Signage Player"
#define AppVersion   "1.0.0"
#define AppPublisher "hanyfas"
#define AppURL       "https://github.com/hanyfas/SW-mediaplayer"
#define AppExeName   "SignagePlayer.exe"

[Setup]
AppId={{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
AppUpdatesURL={#AppURL}

; Install to Program Files by default
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
AllowNoIcons=yes

; Output
OutputDir=Output
OutputBaseFilename=SignagePlayerSetup
SetupIconFile=

; Compression
Compression=lzma2/ultra64
SolidCompression=yes
LZMAUseSeparateProcess=yes

; Require admin for Program Files install
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog

; Minimum Windows 10
MinVersion=10.0.17763

; 64-bit only
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible

WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon";    Description: "{cm:CreateDesktopIcon}";    GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "startupicon";    Description: "Start automatically with Windows"; GroupDescription: "Startup:"; Flags: unchecked

[Files]
; Main executable (built by CMake, collected into dist\)
Source: "..\dist\{#AppExeName}";    DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\playlist.json";    DestDir: "{app}"; Flags: ignoreversion onlyifdoesntexist
Source: "..\dist\shaders\*";        DestDir: "{app}\shaders"; Flags: ignoreversion recursesubdirs

[Icons]
; Start menu
Name: "{group}\{#AppName}";              Filename: "{app}\{#AppExeName}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
; Desktop (optional, unchecked by default)
Name: "{autodesktop}\{#AppName}";        Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Registry]
; Auto-start with Windows (optional, off by default)
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
  ValueType: string; ValueName: "{#AppName}"; \
  ValueData: """{app}\{#AppExeName}"""; \
  Flags: uninsdeletevalue; Tasks: startupicon

[Run]
; Offer to launch after install
Filename: "{app}\{#AppExeName}"; \
  Description: "{cm:LaunchProgram,{#StringChange(AppName, '&', '&&')}}"; \
  Flags: nowait postinstall skipifsilent

[UninstallRun]
; Kill the player before uninstalling
Filename: "taskkill.exe"; Parameters: "/f /im {#AppExeName}"; Flags: runhidden

[Code]
// Optional: warn if installing over an older version that's currently running
function InitializeSetup(): Boolean;
begin
  Result := True;
end;
