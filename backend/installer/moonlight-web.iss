; ===========================================================================
;  Moonlight-Web — interactive Windows installer (Inno Setup 6).
;
;  Produces a stepped wizard (moonlight-web-installer-win-<arch>.exe) that:
;    1. installs the app + a Start-Menu shortcut,
;    2. asks the user to authorize the Internet link (named public domain),
;    3. detects Sunshine and optionally installs it silently, collecting the
;       Sunshine username/password (default admin/admin),
;    4. drops a provisioning.json the server consumes on first run (enable
;       Internet Access, auto-pair the local Sunshine via its REST API),
;    5. creates a Desktop shortcut to the admin page and opens it at the end.
;
;  Build:
;    iscc backend\installer\moonlight-web.iss /DSourceDir=<staged-dist> [/DMyArch=x64]
;  where <staged-dist> holds moonlight-web.exe + Qt runtime + frontend\ (the
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

#define MyAppName "Moonlight-Web"
; Provisional admin URL written before first launch. The server rewrites this
; Desktop shortcut on startup with the real HTTPS port / public domain.
#ifndef AdminUrl
  #define AdminUrl "https://localhost/admin"
#endif

[Setup]
AppId={{6F2C9E4A-7B3D-4E5F-9A1C-2D8E4B6F0A33}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=Moonlight-Web
AppPublisherURL=https://github.com/moonlight-stream/moonlight-web
DefaultDirName={autopf}\Moonlight-Web
DefaultGroupName=Moonlight-Web
DisableProgramGroupPage=yes
OutputBaseFilename=moonlight-web-installer-win-{#MyArch}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
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

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "autostart"; Description: "Démarrer Moonlight-Web à l'ouverture de session (relance auto)"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\Moonlight-Web"; Filename: "{app}\moonlight-web.exe"
Name: "{group}\{cm:UninstallProgram,Moonlight-Web}"; Filename: "{uninstallexe}"

[Run]
; Launch the tray server so the admin page is reachable, then open it.
Filename: "{app}\moonlight-web.exe"; Description: "Lancer Moonlight-Web"; Flags: nowait postinstall skipifsilent
Filename: "{#AdminUrl}"; Description: "Ouvrir la page admin"; Flags: shellexec postinstall skipifsilent

[Code]
var
  InternetPage: TInputOptionWizardPage;
  SunshinePage: TWizardPage;
  SunshineInstallCheck: TNewCheckBox;
  SunshineUserEdit: TNewEdit;
  SunshinePassEdit: TNewEdit;
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
    'Lien Internet', 'Autoriser l''accès depuis Internet ?',
    'Moonlight-Web peut publier un lien public sécurisé (ex. ' +
    'https://a1b2c3d4.moonlightweb.top) pour streamer hors de votre réseau local.' + #13#10 +
    'Cette opération sera effectuée une fois l''application installée.',
    False, False);
  InternetPage.Add('Autoriser le lien Internet (recommandé)');
  InternetPage.Values[0] := True;

  // Step: Sunshine.
  SunshinePage := CreateCustomPage(InternetPage.ID, 'Sunshine',
    'Serveur de streaming Sunshine');

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
  SunshineInstallCheck.Caption := 'Installer Sunshine automatiquement (silencieux)';

  SunshineUserEdit := TNewEdit.Create(WizardForm);
  SunshineUserEdit.Parent := SunshinePage.Surface;
  SunshineUserEdit.Top := ScaleY(84);
  SunshineUserEdit.Width := ScaleX(180);
  SunshineUserEdit.Text := 'admin';

  SunshinePassEdit := TNewEdit.Create(WizardForm);
  SunshinePassEdit.Parent := SunshinePage.Surface;
  SunshinePassEdit.Top := ScaleY(84);
  SunshinePassEdit.Left := ScaleX(200);
  SunshinePassEdit.Width := ScaleX(180);
  SunshinePassEdit.Text := 'admin';
  SunshinePassEdit.PasswordChar := '*';
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (SunshinePage <> nil) and (CurPageID = SunshinePage.ID) then begin
    SunshineDetected := DetectSunshine();
    if SunshineDetected then begin
      SunshineStatusLabel.Caption :=
        'Sunshine est détecté sur cette machine. Entrez ses identifiants pour ' +
        'appairer Moonlight-Web (par défaut admin / admin).';
      SunshineInstallCheck.Checked := False;
      SunshineInstallCheck.Enabled := False;
    end else begin
      SunshineStatusLabel.Caption :=
        'Sunshine n''a pas été détecté. Cochez la case pour l''installer ' +
        'automatiquement, puis définissez ses identifiants.';
      SunshineInstallCheck.Enabled := True;
      SunshineInstallCheck.Checked := True;
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

  DownloadPage := CreateDownloadPage('Sunshine', 'Téléchargement et installation de Sunshine…', nil);
  DownloadPage.Clear;
  DownloadPage.Add('{#SunshineUrl}', 'sunshine-installer.exe', '');
  DownloadPage.Show;
  try
    try
      DownloadPage.Download;
    except
      Result := 'Échec du téléchargement de Sunshine : ' + GetExceptionMessage;
      Exit;
    end;
    tmp := ExpandConstant('{tmp}\sunshine-installer.exe');
    if not Exec(tmp, '/S', '', SW_HIDE, ewWaitUntilTerminated, rc) then begin
      Result := 'Impossible de lancer l''installeur Sunshine.';
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
  exePath := ExpandConstant('{app}\moonlight-web.exe');
  xml :=
    '<?xml version="1.0" encoding="UTF-8"?>' + #13#10 +
    '<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">' + #13#10 +
    '  <RegistrationInfo><Author>Moonlight-Web</Author></RegistrationInfo>' + #13#10 +
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
    Exec('schtasks.exe', '/Create /TN "Moonlight-Web" /XML "' + xmlPath + '" /F',
         '', SW_HIDE, ewWaitUntilTerminated, rc);
end;

// --- Provisioning + desktop shortcut --------------------------------------
function JsonEscape(const s: String): String;
begin
  Result := s;
  StringChangeEx(Result, '\', '\\', True);
  StringChangeEx(Result, '"', '\"', True);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  json, internet, autopair: String;
  shortcutPath: String;
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

  // Provisional Desktop Internet shortcut (.url). The server rewrites it on
  // startup with the real HTTPS port / public domain.
  if WizardIsTaskSelected('desktopicon') then begin
    shortcutPath := ExpandConstant('{autodesktop}\Moonlight-Web Admin.url');
    SaveStringToFile(shortcutPath,
      '[InternetShortcut]' + #13#10 + 'URL={#AdminUrl}' + #13#10, False);
  end;

  // Auto-start at logon (relaunches on crash, keeps the tray icon).
  if WizardIsTaskSelected('autostart') then
    RegisterLogonTask();
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  rc: Integer;
begin
  if CurUninstallStep = usUninstall then begin
    Exec('schtasks.exe', '/Delete /TN "Moonlight-Web" /F', '', SW_HIDE,
         ewWaitUntilTerminated, rc);
    DeleteFile(ExpandConstant('{autodesktop}\Moonlight-Web Admin.url'));
    DeleteFile(ExpandConstant('{app}\provisioning.json'));
    DeleteFile(ExpandConstant('{app}\provisioning.consumed.json'));
  end;
end;
