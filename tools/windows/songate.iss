; Songate for PC: a per-user installer (no administrator prompt). tools/windows/package.sh passes the version.
#ifndef AppVersion
  #define AppVersion "0"
#endif
#ifndef OutName
  #define OutName "Songate-Windows-Setup"
#endif

[Setup]
AppId={{8F1B6C2E-5D7A-4E0B-9A63-6B7E2C4D1F01}
AppName=Songate
AppVersion={#AppVersion}
AppPublisher=AndryTheBeast
AppPublisherURL=https://github.com/AndryTheBeast/Songate
DefaultDirName={localappdata}\Programs\Songate
DefaultGroupName=Songate
DisableProgramGroupPage=yes
DisableDirPage=yes
DisableFinishedPage=yes
PrivilegesRequired=lowest
OutputDir=..\..\build-windows\release
OutputBaseFilename={#OutName}
SetupIconFile=..\..\platform\windows\songate.ico
WizardSmallImageFile=..\..\platform\windows\songate-wizard.bmp
UninstallDisplayIcon={app}\Songate.exe
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
CloseApplications=yes
RestartApplications=no

[Files]
Source: "..\..\build-windows\Songate.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\build-windows\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\build-windows\romfs\*"; DestDir: "{app}\romfs"; Flags: ignoreversion recursesubdirs
Source: "..\..\build-windows\licences\*"; DestDir: "{app}\licences"; Flags: ignoreversion recursesubdirs

[Icons]
Name: "{userprograms}\Songate"; Filename: "{app}\Songate.exe"
Name: "{userdesktop}\Songate"; Filename: "{app}\Songate.exe"

; No tick boxes anywhere: Inno Setup 6.3's list control cuts them off at high display scaling
[Run]
Filename: "{app}\Songate.exe"; Parameters: "--after"; Flags: nowait
