; Stripewake installer (Inno Setup 6). Build with build.cmd, or: ISCC installer\Stripewake.iss
; The first page asks "Just me" (no admin, per-user folder, starts at your sign-in)
; or "All users" (admin, Program Files, starts at every user's sign-in, one copy per session).

#ifndef AppVer
  #define AppVer "1.0.0"
#endif

[Setup]
AppId={{AC2973C3-A737-47EE-8D45-734642C3A2E5}
AppName=Stripewake
AppVersion={#AppVer}
AppVerName=Stripewake {#AppVer}
AppPublisher=Arifi Labs
DefaultDirName={autopf}\Stripewake
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir=..\build
OutputBaseFilename=Stripewake-{#AppVer}-setup
SetupIconFile=..\art\stripewake-on.ico
UninstallDisplayIcon={app}\Stripewake.exe
UninstallDisplayName=Stripewake
LicenseFile=..\LICENSE
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
DisableWelcomePage=no
WizardImageFile=..\art\wizard\side-164x314.bmp,..\art\wizard\side-192x386.bmp,..\art\wizard\side-246x459.bmp,..\art\wizard\side-273x520.bmp,..\art\wizard\side-328x628.bmp,..\art\wizard\side-355x666.bmp,..\art\wizard\side-410x797.bmp
WizardSmallImageFile=..\art\wizard\corner-55.bmp,..\art\wizard\corner-64.bmp,..\art\wizard\corner-69.bmp,..\art\wizard\corner-83.bmp,..\art\wizard\corner-92.bmp,..\art\wizard\corner-110.bmp,..\art\wizard\corner-119.bmp,..\art\wizard\corner-138.bmp
CloseApplications=yes
RestartApplications=no
UsedUserAreasWarning=no

[Tasks]
Name: startatlogon; Description: "Start Stripewake at sign-in"

[Files]
Source: "..\build\Stripewake.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"

[Icons]
Name: "{autoprograms}\Stripewake"; Filename: "{app}\Stripewake.exe"

[Registry]
; HKA = HKCU for "Just me", HKLM for "All users".
Root: HKA; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Stripewake"; ValueData: """{app}\Stripewake.exe"""; Flags: uninsdeletevalue; Tasks: startatlogon
; The app's own "Start at logon" menu item writes HKCU; remove it on uninstall too.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "Stripewake"; Flags: uninsdeletevalue dontcreatekey
; The app's saved "Lid closed" choice.
Root: HKCU; Subkey: "Software\Stripewake"; Flags: uninsdeletekey dontcreatekey

[Run]
Filename: "{app}\Stripewake.exe"; Description: "Start Stripewake now"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
; Ask it to close (it puts its settings back), force it if needed, then put back any setting a forced stop left.
Filename: "{sys}\taskkill.exe"; Parameters: "/im Stripewake.exe"; Flags: runhidden; RunOnceId: "CloseStripewake"
Filename: "{sys}\taskkill.exe"; Parameters: "/f /im Stripewake.exe"; Flags: runhidden; RunOnceId: "StopStripewake"
Filename: "{app}\Stripewake.exe"; Parameters: "/restore"; Flags: runhidden; RunOnceId: "RestoreSettings"

[UninstallDelete]
Type: filesandordirs; Name: "{localappdata}\Stripewake"

[Code]
const
  UninstKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{AC2973C3-A737-47EE-8D45-734642C3A2E5}_is1';

var
  Existing: Boolean;   { already installed in the scope being installed now }
  Verb: String;        { 'Update' or 'Reinstall' }

{ Installed for this user ("Just me") or for all users? The uninstall record OR the program file counts. }
function InstalledPerUser: Boolean;
begin
  Result := RegKeyExists(HKCU, UninstKey) or FileExists(ExpandConstant('{localappdata}\Programs\Stripewake\Stripewake.exe'));
end;

function InstalledAllUsers: Boolean;
begin
  Result := RegKeyExists(HKLM64, UninstKey) or RegKeyExists(HKLM32, UninstKey) or
            FileExists(ExpandConstant('{commonpf64}\Stripewake\Stripewake.exe'));
end;

function InstalledVersion: String;
begin
  Result := '';
  if IsAdminInstallMode then
  begin
    if not RegQueryStringValue(HKLM64, UninstKey, 'DisplayVersion', Result) then
      RegQueryStringValue(HKLM32, UninstKey, 'DisplayVersion', Result);
  end else
    RegQueryStringValue(HKCU, UninstKey, 'DisplayVersion', Result);
end;

{ One copy only: never install in the other scope on top of an existing install. }
function InitializeSetup: Boolean;
begin
  Result := True;
  if IsAdminInstallMode and InstalledPerUser then
  begin
    SuppressibleMsgBox('Stripewake is already installed for you only ("Just me").' + #13#10 + #13#10 +
           'Installing it for all users would make a second copy. Run setup again and choose "Just me" to update it,' +
           ' or uninstall it first (Settings > Apps).', mbError, MB_OK, IDOK);
    Result := False;
  end else if (not IsAdminInstallMode) and InstalledAllUsers then
  begin
    SuppressibleMsgBox('Stripewake is already installed for all users.' + #13#10 + #13#10 +
           'Installing it for you only would make a second copy. Run setup again and choose "All users" to update it,' +
           ' or uninstall it first (Settings > Apps).', mbError, MB_OK, IDOK);
    Result := False;
  end;
  if IsAdminInstallMode then Existing := InstalledAllUsers else Existing := InstalledPerUser;
  if Existing and (InstalledVersion = '{#AppVer}') then Verb := 'Reinstall' else Verb := 'Update';
end;

procedure InitializeWizard;
begin
  if Existing then WizardForm.Caption := Verb + ' - Stripewake {#AppVer}';
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := Existing and (Verb = 'Reinstall') and (PageID = wpLicense);  { an update shows it again }
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if not Existing then Exit;
  if CurPageID = wpWelcome then
  begin
    WizardForm.WelcomeLabel1.Caption := Verb + ' Stripewake';
    WizardForm.WelcomeLabel2.Caption := 'Stripewake is already installed on this PC.' + #13#10 + #13#10 +
      'Setup will ' + Lowercase(Verb) + ' it to version {#AppVer} in the same folder and keep your settings.' +
      ' If Stripewake is running, Setup closes it first and starts it again at the end.';
  end else if CurPageID = wpReady then
  begin
    WizardForm.ReadyLabel.Caption := 'Setup is ready to ' + Lowercase(Verb) + ' Stripewake.';
    WizardForm.NextButton.Caption := Verb;
  end else if CurPageID = wpFinished then
  begin
    WizardForm.FinishedHeadingLabel.Caption := 'Stripewake is ' + Lowercase(Verb) + 'd';
  end;
end;

{ Close a running Stripewake before files are replaced: ask first (it puts its settings back), force after 10 s. }
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code, I: Integer;
begin
  Result := '';
  if FindWindowByClassName('StripewakeTray') = 0 then Exit;
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/im Stripewake.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
  I := 0;
  while (FindWindowByClassName('StripewakeTray') <> 0) and (I < 40) do
  begin
    Sleep(250);
    I := I + 1;
  end;
  if FindWindowByClassName('StripewakeTray') <> 0 then
  begin
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /im Stripewake.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
    Sleep(500);
    { a forced stop cannot put settings back itself; the new copy does it at its first start }
  end;
  if FindWindowByClassName('StripewakeTray') <> 0 then
    Result := 'Stripewake is still running and could not be closed. Exit it from its tray menu, then click Back and Next.';
end;
