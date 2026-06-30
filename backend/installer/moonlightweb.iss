; ===========================================================================
;  MoonlightWeb — interactive Windows installer (Inno Setup 6).
;
;  Produces a stepped wizard (MoonlightWeb-installer-win-<arch>.exe) that:
;    1. installs the app + Start-Menu shortcuts (app, admin page, uninstaller),
;    2. asks the user to authorize the Internet link (named public domain),
;    3. detects Sunshine and optionally installs it silently, collecting the
;       Sunshine username/password (default admin/admin),
;    4. drops a provisioning.json the server consumes on first run (enable
;       Internet Access, auto-pair the local Sunshine via its REST API),
;    5. creates a Desktop shortcut to the admin page and opens it at the end.
;
;  DNS/ACME secrets are NOT shipped with the installer: they are compiled into
;  MoonlightWeb.exe at build time (CMake, from CI secrets). Nothing on the user's
;  machine carries or can edit them.
;
;  Build:
;    iscc backend\installer\moonlightweb.iss /DSourceDir=<staged-dist> [/DMyArch=x64]
;  where <staged-dist> holds MoonlightWeb.exe + Qt runtime + frontend\ (the
;  output of `cmake --install` + windeployqt, see .github/workflows/release.yml).
; ===========================================================================

#ifndef MyArch
  #define MyArch "x64"
#endif
#ifndef SourceDir
  #define SourceDir "dist"
#endif
#ifndef MyAppVersion
  #define MyAppVersion "0.1.0"
#endif
; Latest Sunshine Windows installer (NSIS, supports /S silent). Overridable.
#ifndef SunshineUrl
  #define SunshineUrl "https://github.com/LizardByte/Sunshine/releases/latest/download/sunshine-windows-installer.exe"
#endif

#define MyAppName "MoonlightWeb"
#define MyAppExe "MoonlightWeb.exe"
; Provisional admin URL written before first launch. The server rewrites this
; Desktop shortcut on startup with the real HTTPS port / public domain.
#ifndef AdminUrl
  #define AdminUrl "https://localhost/admin"
#endif

[Setup]
AppId={{6F2C9E4A-7B3D-4E5F-9A1C-2D8E4B6F0A33}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=MoonlightWeb
AppPublisherURL=https://github.com/moonlight-stream/moonlight-web
DefaultDirName={autopf}\MoonlightWeb
DefaultGroupName=MoonlightWeb
DisableProgramGroupPage=yes
OutputBaseFilename=MoonlightWeb-installer-win-{#MyArch}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; Branding: installer .exe icon + small wizard logo (top-right on inner pages).
; Paths are relative to this .iss; PNG wizard images need Inno Setup 6.3+.
SetupIconFile=..\..\frontend\assets\favicon.ico
WizardSmallImageFile=..\..\frontend\assets\logo-512.png
; Add/Remove Programs entry shows the app icon (embedded in the exe).
UninstallDisplayIcon={app}\{#MyAppExe}
UninstallDisplayName={#MyAppName}
PrivilegesRequired=admin
#if MyArch == "arm64"
ArchitecturesInstallIn64BitMode=arm64
ArchitecturesAllowed=arm64
#else
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
#endif

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "fr"; MessagesFile: "compiler:Languages\French.isl"

[CustomMessages]
; --- English ---
en.AutoStartTask=Start MoonlightWeb at logon
en.InternetPageCaption=Internet Link
en.InternetPageDesc=Allow access from the Internet?
en.InternetPageBody=MoonlightWeb can publish a secure public link (e.g. https://a1b2c3d4.moonlightweb.top) to stream outside your local network.%n%nThis is done once the application is installed.
en.InternetPageOption=Allow the Internet link (recommended)
en.SunshinePageCaption=Sunshine
en.SunshinePageDesc=Sunshine streaming server
en.SunshineInstallCheck=Install Sunshine automatically
en.SunshineDetected=The installer detected that Sunshine is already installed on this machine.%nEnter its credentials to pair MoonlightWeb.
en.SunshineNotDetected=Sunshine was not detected. Check the box to install it automatically, then set its credentials.
en.SunshineUserLabel=Username
en.SunshinePassLabel=Password
en.SunshineCredsRequired=Please enter the Sunshine username and password so MoonlightWeb can pair automatically.
en.RunApp=Launch MoonlightWeb
en.RunAdmin=Open the admin page
en.SunshineDownloadCaption=Downloading and installing Sunshine...
en.SunshineDownloadFail=Failed to download Sunshine:
en.SunshineLaunchFail=Could not start the Sunshine installer.
en.AdminShortcut=MoonlightWeb Admin
; --- French ---
fr.AutoStartTask=Démarrer MoonlightWeb à l'ouverture de session
fr.InternetPageCaption=Lien Internet
fr.InternetPageDesc=Autoriser l'accès depuis Internet ?
fr.InternetPageBody=MoonlightWeb peut publier un lien public sécurisé (ex. https://a1b2c3d4.moonlightweb.top) pour streamer hors de votre réseau local.%n%nCette opération est effectuée une fois l'application installée.
fr.InternetPageOption=Autoriser le lien Internet (recommandé)
fr.SunshinePageCaption=Sunshine
fr.SunshinePageDesc=Serveur de streaming Sunshine
fr.SunshineInstallCheck=Installer Sunshine automatiquement
fr.SunshineDetected=L'installeur a détecté que Sunshine est déjà installé sur cette machine.%nEntrez ses identifiants pour appairer MoonlightWeb.
fr.SunshineNotDetected=Sunshine n'a pas été détecté. Cochez la case pour l'installer automatiquement, puis définissez ses identifiants.
fr.SunshineUserLabel=Identifiant
fr.SunshinePassLabel=Mot de passe
fr.SunshineCredsRequired=Veuillez saisir l'identifiant et le mot de passe Sunshine pour que MoonlightWeb puisse appairer automatiquement.
fr.RunApp=Lancer MoonlightWeb
fr.RunAdmin=Ouvrir la page admin
fr.SunshineDownloadCaption=Téléchargement et installation de Sunshine...
fr.SunshineDownloadFail=Échec du téléchargement de Sunshine :
fr.SunshineLaunchFail=Impossible de lancer l'installeur Sunshine.
fr.AdminShortcut=Administration MoonlightWeb

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "autostart"; Description: "{cm:AutoStartTask}"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
; Start-Menu group: application (embedded exe icon), admin page (.url created in
; [Code]) and the uninstaller. The admin .url is added by CurStepChanged.
Name: "{group}\MoonlightWeb"; Filename: "{app}\{#MyAppExe}"
Name: "{group}\{cm:UninstallProgram,MoonlightWeb}"; Filename: "{uninstallexe}"

[Run]
; Launch the tray server so the admin page is reachable, then open it.
Filename: "{app}\{#MyAppExe}"; Description: "{cm:RunApp}"; Flags: nowait postinstall skipifsilent
Filename: "{#AdminUrl}"; Description: "{cm:RunAdmin}"; Flags: shellexec postinstall skipifsilent

[Code]
var
  InternetPage: TInputOptionWizardPage;
  SunshinePage: TWizardPage;
  SunshineInstallCheck: TNewCheckBox;
  SunshineUserEdit: TNewEdit;
  SunshinePassEdit: TNewEdit;
  SunshineUserLabel: TNewStaticText;
  SunshinePassLabel: TNewStaticText;
  SunshineStatusLabel: TNewStaticText;
  SunshineDetected: Boolean;
  SunshineExePath: String;

// --- Detection ------------------------------------------------------------
function DetectSunshine(): Boolean;
var
  p: String;
begin
  Result := False;
  SunshineExePath := '';
  p := ExpandConstant('{autopf}\Sunshine\sunshine.exe');
  if FileExists(p) then begin SunshineExePath := p; Result := True; Exit; end;
  p := ExpandConstant('{commonpf64}\Sunshine\sunshine.exe');
  if FileExists(p) then begin SunshineExePath := p; Result := True; Exit; end;
  // Registry uninstall entry (covers non-default install dirs).
  if RegQueryStringValue(HKLM, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Sunshine',
                         'InstallLocation', p) and (p <> '') then begin
    SunshineExePath := AddBackslash(p) + 'sunshine.exe';
    Result := FileExists(SunshineExePath);
  end;
end;

procedure InitializeWizard();
begin
  // Step: Internet link authorization.
  InternetPage := CreateInputOptionPage(wpSelectTasks,
    ExpandConstant('{cm:InternetPageCaption}'), ExpandConstant('{cm:InternetPageDesc}'),
    ExpandConstant('{cm:InternetPageBody}'),
    False, False);
  InternetPage.Add(ExpandConstant('{cm:InternetPageOption}'));
  InternetPage.Values[0] := True;

  // Step: Sunshine.
  SunshinePage := CreateCustomPage(InternetPage.ID,
    ExpandConstant('{cm:SunshinePageCaption}'), ExpandConstant('{cm:SunshinePageDesc}'));

  SunshineStatusLabel := TNewStaticText.Create(WizardForm);
  SunshineStatusLabel.Parent := SunshinePage.Surface;
  SunshineStatusLabel.Left := 0;
  SunshineStatusLabel.Top := 0;
  SunshineStatusLabel.Width := SunshinePage.SurfaceWidth;
  SunshineStatusLabel.AutoSize := False;
  SunshineStatusLabel.Height := ScaleY(40);
  SunshineStatusLabel.WordWrap := True;

  SunshineInstallCheck := TNewCheckBox.Create(WizardForm);
  SunshineInstallCheck.Parent := SunshinePage.Surface;
  SunshineInstallCheck.Top := ScaleY(48);
  SunshineInstallCheck.Width := SunshinePage.SurfaceWidth;
  SunshineInstallCheck.Caption := ExpandConstant('{cm:SunshineInstallCheck}');

  SunshineUserLabel := TNewStaticText.Create(WizardForm);
  SunshineUserLabel.Parent := SunshinePage.Surface;
  SunshineUserLabel.Top := ScaleY(80);
  SunshineUserLabel.Caption := ExpandConstant('{cm:SunshineUserLabel}');

  SunshinePassLabel := TNewStaticText.Create(WizardForm);
  SunshinePassLabel.Parent := SunshinePage.Surface;
  SunshinePassLabel.Left := ScaleX(200);
  SunshinePassLabel.Top := ScaleY(80);
  SunshinePassLabel.Caption := ExpandConstant('{cm:SunshinePassLabel}');

  SunshineUserEdit := TNewEdit.Create(WizardForm);
  SunshineUserEdit.Parent := SunshinePage.Surface;
  SunshineUserEdit.Top := ScaleY(98);
  SunshineUserEdit.Width := ScaleX(180);

  SunshinePassEdit := TNewEdit.Create(WizardForm);
  SunshinePassEdit.Parent := SunshinePage.Surface;
  SunshinePassEdit.Top := ScaleY(98);
  SunshinePassEdit.Left := ScaleX(200);
  SunshinePassEdit.Width := ScaleX(180);
  SunshinePassEdit.PasswordChar := '*';
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (SunshinePage <> nil) and (CurPageID = SunshinePage.ID) then begin
    SunshineDetected := DetectSunshine();
    if SunshineDetected then begin
      SunshineStatusLabel.Caption := ExpandConstant('{cm:SunshineDetected}');
      SunshineInstallCheck.Checked := False;
      SunshineInstallCheck.Enabled := False;
      // Already installed: do NOT prefill. Wrong (default) credentials make the
      // REST PIN push fail, leaving a pending pairing request and an unpaired
      // host. Force the user to type Sunshine's real username/password.
      SunshineUserEdit.Text := '';
      SunshinePassEdit.Text := '';
    end else begin
      SunshineStatusLabel.Caption := ExpandConstant('{cm:SunshineNotDetected}');
      SunshineInstallCheck.Enabled := True;
      SunshineInstallCheck.Checked := True;
      // Fresh install: the silent installer sets these via --creds, so the
      // admin/admin default is what will actually be configured.
      if SunshineUserEdit.Text = '' then SunshineUserEdit.Text := 'admin';
      if SunshinePassEdit.Text = '' then SunshinePassEdit.Text := 'admin';
    end;
  end;
end;

// Require Sunshine credentials before leaving the page so auto-pairing can work.
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (SunshinePage <> nil) and (CurPageID = SunshinePage.ID) then begin
    if (Trim(SunshineUserEdit.Text) = '') or (Trim(SunshinePassEdit.Text) = '') then begin
      MsgBox(ExpandConstant('{cm:SunshineCredsRequired}'), mbError, MB_OK);
      Result := False;
    end;
  end;
end;

// --- Silent Sunshine install (download + /S + set credentials) ------------
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  DownloadPage: TDownloadWizardPage;
  tmp: String;
  rc: Integer;
begin
  Result := '';
  if (SunshinePage = nil) or (not SunshineInstallCheck.Checked) then Exit;

  DownloadPage := CreateDownloadPage(ExpandConstant('{cm:SunshinePageCaption}'),
    ExpandConstant('{cm:SunshineDownloadCaption}'), nil);
  DownloadPage.Clear;
  DownloadPage.Add('{#SunshineUrl}', 'sunshine-installer.exe', '');
  DownloadPage.Show;
  try
    try
      DownloadPage.Download;
    except
      Result := ExpandConstant('{cm:SunshineDownloadFail}') + ' ' + GetExceptionMessage;
      Exit;
    end;
    tmp := ExpandConstant('{tmp}\sunshine-installer.exe');
    if not Exec(tmp, '/S', '', SW_HIDE, ewWaitUntilTerminated, rc) then begin
      Result := ExpandConstant('{cm:SunshineLaunchFail}');
      Exit;
    end;
    // Re-detect to get the installed path, then set credentials via the CLI.
    if DetectSunshine() then
      Exec(SunshineExePath, '--creds ' + SunshineUserEdit.Text + ' ' + SunshinePassEdit.Text,
           '', SW_HIDE, ewWaitUntilTerminated, rc);
  finally
    DownloadPage.Hide;
  end;
end;

// --- Auto-start: logon scheduled task (keeps the tray icon, native) -------
procedure RegisterLogonTask();
var
  user, xml, xmlPath, exePath: String;
  rc: Integer;
begin
  user := GetEnv('USERDOMAIN') + '\' + GetEnv('USERNAME');
  exePath := ExpandConstant('{app}\{#MyAppExe}');
  xml :=
    '<?xml version="1.0" encoding="UTF-8"?>' + #13#10 +
    '<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">' + #13#10 +
    '  <RegistrationInfo><Author>MoonlightWeb</Author></RegistrationInfo>' + #13#10 +
    '  <Triggers><LogonTrigger><Enabled>true</Enabled><UserId>' + user + '</UserId></LogonTrigger></Triggers>' + #13#10 +
    '  <Principals><Principal id="Author">' +
    '<UserId>' + user + '</UserId><LogonType>InteractiveToken</LogonType><RunLevel>LeastPrivilege</RunLevel>' +
    '</Principal></Principals>' + #13#10 +
    // Element order follows the Task Scheduler schema (schtasks /XML is strict).
    '  <Settings>' +
    '<RestartOnFailure><Interval>PT1M</Interval><Count>3</Count></RestartOnFailure>' +
    '<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>' +
    '<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>' +
    '<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>' +
    '<ExecutionTimeLimit>PT0S</ExecutionTimeLimit>' +
    '</Settings>' + #13#10 +
    '  <Actions Context="Author"><Exec><Command>"' + exePath + '"</Command></Exec></Actions>' + #13#10 +
    '</Task>' + #13#10;
  xmlPath := ExpandConstant('{tmp}\mw-task.xml');
  if SaveStringToFile(xmlPath, xml, False) then
    Exec('schtasks.exe', '/Create /TN "MoonlightWeb" /XML "' + xmlPath + '" /F',
         '', SW_HIDE, ewWaitUntilTerminated, rc);
end;

// --- Provisioning + shortcuts ---------------------------------------------
function JsonEscape(const s: String): String;
begin
  Result := s;
  StringChangeEx(Result, '\', '\\', True);
  StringChangeEx(Result, '"', '\"', True);
end;

// Write an admin-page .url internet shortcut (logo icon) at the given path.
procedure WriteAdminShortcut(const path: String);
begin
  SaveStringToFile(path,
    '[InternetShortcut]' + #13#10 +
    'URL={#AdminUrl}' + #13#10 +
    'IconFile=' + ExpandConstant('{app}\frontend\assets\favicon.ico') + #13#10 +
    'IconIndex=0' + #13#10, False);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  json, internet, autopair: String;
begin
  if CurStep <> ssPostInstall then Exit;

  // provisioning.json — consumed and removed by the server on first run.
  if InternetPage.Values[0] then internet := 'true' else internet := 'false';
  autopair := 'true';
  json :=
    '{' + #13#10 +
    '  "internet_access_authorized": ' + internet + ',' + #13#10 +
    '  "sunshine": {' + #13#10 +
    '    "auto_pair": ' + autopair + ',' + #13#10 +
    '    "username": "' + JsonEscape(SunshineUserEdit.Text) + '",' + #13#10 +
    '    "password": "' + JsonEscape(SunshinePassEdit.Text) + '"' + #13#10 +
    '  }' + #13#10 +
    '}' + #13#10;
  SaveStringToFile(ExpandConstant('{app}\provisioning.json'), json, False);

  // Start-Menu admin shortcut (in the group folder created by [Icons]).
  WriteAdminShortcut(ExpandConstant('{group}\{cm:AdminShortcut}.url'));

  // Provisional Desktop admin shortcut. The server rewrites it on startup with
  // the real HTTPS port / public domain.
  if WizardIsTaskSelected('desktopicon') then
    WriteAdminShortcut(ExpandConstant('{autodesktop}\MoonlightWeb Admin.url'));

  // Auto-start at logon (relaunches on crash, keeps the tray icon).
  if WizardIsTaskSelected('autostart') then
    RegisterLogonTask();
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  rc: Integer;
begin
  if CurUninstallStep = usUninstall then begin
    Exec('schtasks.exe', '/Delete /TN "MoonlightWeb" /F', '', SW_HIDE,
         ewWaitUntilTerminated, rc);
    DeleteFile(ExpandConstant('{group}\{cm:AdminShortcut}.url'));
    DeleteFile(ExpandConstant('{autodesktop}\MoonlightWeb Admin.url'));
    DeleteFile(ExpandConstant('{app}\provisioning.json'));
    DeleteFile(ExpandConstant('{app}\provisioning.consumed.json'));
  end;
end;
