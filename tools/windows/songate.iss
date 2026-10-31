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
DisableDirPage=no
DirExistsWarning=no
DisableReadyPage=yes
PrivilegesRequired=lowest
OutputDir=..\..\build-windows\release
OutputBaseFilename={#OutName}
SetupIconFile=..\..\platform\windows\songate.ico
WizardSmallImageFile=..\..\platform\windows\songate-wizard.bmp
WizardImageFile=..\..\platform\windows\songate-panel.bmp,..\..\platform\windows\songate-panel-2x.bmp
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

; (the usual text speaks of shortcuts, and there may be none)
[Messages]
FinishedLabel=[name] is installed on your computer.
FinishedLabelNoIcons=[name] is installed on your computer.

[Icons]
Name: "{userprograms}\Songate"; Filename: "{app}\Songate.exe"; Check: WantStartMenu
Name: "{userdesktop}\Songate"; Filename: "{app}\Songate.exe"; Check: WantDesktop

; An update (/SILENT) has no last page: Songate comes back by itself
[Run]
Filename: "{app}\Songate.exe"; Parameters: "--after"; Flags: nowait; Check: WizardSilent

; Plain check boxes, not [Tasks] or a postinstall [Run]: Inno Setup 6.3's list control cuts its boxes off at high display scaling.
; An update (/SILENT, no pages) or a second install keeps the shortcuts as they are: they start ticked only if they exist.
[Code]
var
  ShortcutsPage: TWizardPage;
  DesktopBox, StartMenuBox, OpenBox: TNewCheckBox;

function ShortcutFile(const Folder: String): String;
begin
  Result := ExpandConstant(Folder + '\Songate.lnk');
end;

function AddBox(Parent: TWinControl; const Caption: String; Top: Integer; Ticked: Boolean): TNewCheckBox;
begin
  Result := TNewCheckBox.Create(Parent);
  Result.Parent := Parent;
  Result.Caption := Caption;
  Result.Top := Top;
  Result.Width := Parent.ClientWidth;
  Result.Height := ScaleY(24);
  Result.Checked := Ticked;
end;

procedure InitializeWizard;
var
  Again: Boolean;
begin
  Again := WizardForm.PrevAppDir <> '';
  ShortcutsPage := CreateCustomPage(wpSelectDir, 'Shortcuts', 'Where would you like to open Songate from?');
  DesktopBox := AddBox(ShortcutsPage.Surface, 'Desktop', ScaleY(8), not Again or FileExists(ShortcutFile('{userdesktop}')));
  StartMenuBox := AddBox(ShortcutsPage.Surface, 'Start menu', ScaleY(40), not Again or FileExists(ShortcutFile('{userprograms}')));
  OpenBox := AddBox(WizardForm.FinishedPage, 'Open Songate', 0, True);
  OpenBox.Left := WizardForm.FinishedLabel.Left;
  OpenBox.Width := WizardForm.FinishedLabel.Width;
end;

function WantDesktop: Boolean;
begin
  Result := DesktopBox.Checked;
end;

function WantStartMenu: Boolean;
begin
  Result := StartMenuBox.Checked;
end;

// Installed for this user alone, so without administrator rights: a folder like Program Files would fail halfway through
function CanWrite(Dir: String): Boolean;
var
  Test: String;
begin
  while (not DirExists(Dir)) and (ExtractFileDir(Dir) <> Dir) do Dir := ExtractFileDir(Dir);
  Test := AddBackslash(Dir) + 'songate-write-test.tmp';
  Result := SaveStringToFile(Test, '', False);
  if Result then DeleteFile(Test);
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  // Next installs from here: there is no Ready page
  if CurPageID = ShortcutsPage.ID then WizardForm.NextButton.Caption := SetupMessage(msgButtonInstall);
  if CurPageID = wpFinished then OpenBox.Top := WizardForm.FinishedLabel.Top + WizardForm.FinishedLabel.Height + ScaleY(16);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Code: Integer;
begin
  Result := True;
  // (called in a silent run too, where the box was never seen: [Run] brings Songate back there)
  if (CurPageID = wpFinished) and OpenBox.Checked and not WizardSilent then
    Exec(ExpandConstant('{app}\Songate.exe'), '--after', '', SW_SHOW, ewNoWait, Code);
  if (CurPageID = wpSelectDir) and not CanWrite(WizardDirValue) then begin
    MsgBox('Songate cannot install there: Windows only lets administrators change that folder. Choose another one.', mbError, MB_OK);
    Result := False;
  end;
end;

// A box unticked on a second install takes its shortcut away
procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    if not WantDesktop then DeleteFile(ShortcutFile('{userdesktop}'));
    if not WantStartMenu then DeleteFile(ShortcutFile('{userprograms}'));
  end;
end;
