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
; Asset names are arch-specific: Sunshine-Windows-{AMD64,ARM64}-installer.exe.
#ifndef SunshineUrl
  #if MyArch == "arm64"
    #define SunshineUrl "https://github.com/LizardByte/Sunshine/releases/latest/download/Sunshine-Windows-ARM64-installer.exe"
  #else
    #define SunshineUrl "https://github.com/LizardByte/Sunshine/releases/latest/download/Sunshine-Windows-AMD64-installer.exe"
  #endif
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
en.SunshineInstallCheckDone=Install Sunshine automatically (already installed)
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
en.ProvisionPageCaption=Setting up MoonlightWeb
en.ProvisionPageDesc=Finalizing the installation
en.ProvisionWorking=Please wait while MoonlightWeb finishes setting up...
en.TaskSunshine=Install the Sunshine streaming server
en.TaskPairing=Pair the local Sunshine
en.TaskArecord=Publish the secure Internet address
; --- French ---
fr.AutoStartTask=Démarrer MoonlightWeb à l'ouverture de session
fr.InternetPageCaption=Lien Internet
fr.InternetPageDesc=Autoriser l'accès depuis Internet ?
fr.InternetPageBody=MoonlightWeb peut publier un lien public sécurisé (ex. https://a1b2c3d4.moonlightweb.top) pour streamer hors de votre réseau local.%n%nCette opération est effectuée une fois l'application installée.
fr.InternetPageOption=Autoriser le lien Internet (recommandé)
fr.SunshinePageCaption=Sunshine
fr.SunshinePageDesc=Serveur de streaming Sunshine
fr.SunshineInstallCheck=Installer Sunshine automatiquement
fr.SunshineInstallCheckDone=Installer Sunshine automatiquement (déjà installé)
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
fr.ProvisionPageCaption=Configuration de MoonlightWeb
fr.ProvisionPageDesc=Finalisation de l'installation
fr.ProvisionWorking=Veuillez patienter pendant la fin de la configuration de MoonlightWeb...
fr.TaskSunshine=Installer le serveur de streaming Sunshine
fr.TaskPairing=Appairer le Sunshine local
fr.TaskArecord=Publier l'adresse Internet sécurisée

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "autostart"; Description: "{cm:AutoStartTask}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
; Start-Menu group: application (embedded exe icon), admin page (.url created in
; [Code]) and the uninstaller. The admin .url is added by CurStepChanged.
Name: "{group}\MoonlightWeb"; Filename: "{app}\{#MyAppExe}"
Name: "{group}\{cm:UninstallProgram,MoonlightWeb}"; Filename: "{uninstallexe}"

[Run]
; The tray server is already launched during the provisioning checklist (see
; RunProvisionChecklist in [Code]); only offer to open the admin page here.
; GetAdminUrl reads the URL the server published (real HTTPS port / public
; domain) and falls back to the provisional one if the server did not start.
Filename: "{code:GetAdminUrl}"; Description: "{cm:RunAdmin}"; Flags: shellexec postinstall skipifsilent

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
  // Live post-install checklist (Sunshine / pairing / A-record).
  ProgressPage: TOutputProgressWizardPage;
  LblSunshine: TNewStaticText;
  LblPairing: TNewStaticText;
  LblArecord: TNewStaticText;
  SunshineStepState: String;

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

// Gray out the credential fields unless they are actually needed: when Sunshine
// is already installed (user must supply its real creds) or the "install"
// checkbox is ticked (creds for the fresh install). Disabled when Sunshine is
// absent and installation is declined.
procedure UpdateSunshineFieldsEnabled();
var
  enabled: Boolean;
begin
  enabled := SunshineDetected or SunshineInstallCheck.Checked;
  SunshineUserEdit.Enabled := enabled;
  SunshinePassEdit.Enabled := enabled;
  SunshineUserLabel.Enabled := enabled;
  SunshinePassLabel.Enabled := enabled;
end;

procedure SunshineInstallCheckClick(Sender: TObject);
begin
  UpdateSunshineFieldsEnabled();
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
  SunshineInstallCheck.OnClick := @SunshineInstallCheckClick;

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

  // Live checklist shown during post-install (driven in RunProvisionChecklist).
  // Default each task to "skipped"; PrepareToInstall / the backend status file
  // promote them to running/done/failed.
  SunshineStepState := 'skipped';
  ProgressPage := CreateOutputProgressPage(
    ExpandConstant('{cm:ProvisionPageCaption}'), ExpandConstant('{cm:ProvisionPageDesc}'));

  // Header (Msg1Label, fed via SetText): let it wrap so the full sentence shows
  // instead of being clipped, and reserve vertical space above the checklist.
  ProgressPage.Msg1Label.AutoSize := False;
  ProgressPage.Msg1Label.WordWrap := True;
  ProgressPage.Msg1Label.Width := ProgressPage.SurfaceWidth;
  ProgressPage.Msg1Label.Top := ScaleY(0);
  ProgressPage.Msg1Label.Height := ScaleY(34);

  LblSunshine := TNewStaticText.Create(WizardForm);
  LblSunshine.Parent := ProgressPage.Surface;
  LblSunshine.Top := ScaleY(44);
  LblSunshine.Width := ProgressPage.SurfaceWidth;
  LblSunshine.Font.Name := 'Consolas';
  LblSunshine.Font.Size := 10;

  LblPairing := TNewStaticText.Create(WizardForm);
  LblPairing.Parent := ProgressPage.Surface;
  LblPairing.Top := ScaleY(68);
  LblPairing.Width := ProgressPage.SurfaceWidth;
  LblPairing.Font.Name := 'Consolas';
  LblPairing.Font.Size := 10;

  LblArecord := TNewStaticText.Create(WizardForm);
  LblArecord.Parent := ProgressPage.Surface;
  LblArecord.Top := ScaleY(92);
  LblArecord.Width := ProgressPage.SurfaceWidth;
  LblArecord.Font.Name := 'Consolas';
  LblArecord.Font.Size := 10;

  // Progress bar below the checklist (not overlapping it), with the secondary
  // message under the bar. Without this the bar sits on top of the labels.
  ProgressPage.ProgressBar.Top := ScaleY(124);
  ProgressPage.ProgressBar.Width := ProgressPage.SurfaceWidth;
  ProgressPage.Msg2Label.Top := ScaleY(150);
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (SunshinePage <> nil) and (CurPageID = SunshinePage.ID) then begin
    SunshineDetected := DetectSunshine();
    if SunshineDetected then begin
      SunshineStatusLabel.Caption := ExpandConstant('{cm:SunshineDetected}');
      // Already installed: show the box ticked + "(already installed)" and keep
      // it disabled. PrepareToInstall skips the download when SunshineDetected,
      // so a ticked box here never triggers a reinstall.
      SunshineInstallCheck.Caption := ExpandConstant('{cm:SunshineInstallCheckDone}');
      SunshineInstallCheck.Checked := True;
      SunshineInstallCheck.Enabled := False;
      // Do NOT prefill: wrong (default) credentials make the REST PIN push fail,
      // leaving a pending pairing request and an unpaired host. Force the user
      // to type Sunshine's real username/password.
      SunshineUserEdit.Text := '';
      SunshinePassEdit.Text := '';
    end else begin
      SunshineStatusLabel.Caption := ExpandConstant('{cm:SunshineNotDetected}');
      SunshineInstallCheck.Caption := ExpandConstant('{cm:SunshineInstallCheck}');
      SunshineInstallCheck.Enabled := True;
      SunshineInstallCheck.Checked := True;
      // Fresh install: prefill the username (the silent installer sets it via
      // --creds) but leave the password blank so the user picks one they know.
      if SunshineUserEdit.Text = '' then SunshineUserEdit.Text := 'admin';
      SunshinePassEdit.Text := '';
    end;
    UpdateSunshineFieldsEnabled();
  end;
end;

// Require Sunshine credentials before leaving the page so auto-pairing can work.
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (SunshinePage <> nil) and (CurPageID = SunshinePage.ID) then begin
    // Credentials are only required when they will actually be used: Sunshine
    // already installed (pair with its real creds) or a fresh install requested.
    // If Sunshine is absent and install is declined, the grayed-out fields are
    // irrelevant — don't block.
    if (SunshineDetected or SunshineInstallCheck.Checked)
       and ((Trim(SunshineUserEdit.Text) = '') or (Trim(SunshinePassEdit.Text) = '')) then begin
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
  // Skip the download when Sunshine is already present (the box is ticked but
  // disabled purely as an "already installed" indicator) or the user declined
  // it. Only a ticked box on a machine WITHOUT Sunshine triggers a real install.
  if (SunshinePage = nil) or SunshineDetected or (not SunshineInstallCheck.Checked) then begin
    if SunshineDetected then SunshineStepState := 'done';
    Exit;
  end;

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
    // Quote both values: spaces/special characters must reach Sunshine intact.
    if DetectSunshine() then
      Exec(SunshineExePath, '--creds "' + SunshineUserEdit.Text + '" "' + SunshinePassEdit.Text + '"',
           '', SW_HIDE, ewWaitUntilTerminated, rc);
    SunshineStepState := 'done';
  finally
    DownloadPage.Hide;
  end;
end;

// --- Auto-start: logon scheduled task (keeps the tray icon, native) -------

// Escape a value for embedding in the task XML. SaveStringToFile writes ANSI
// and the file carries no <?xml?> declaration (see RegisterLogonTask), so the
// output must stay pure ASCII: non-ASCII characters (accented usernames) are
// emitted as numeric character references.
function TaskXmlEscape(const s: String): String;
var
  i: Integer;
begin
  Result := '';
  for i := 1 to Length(s) do begin
    if s[i] = '&' then Result := Result + '&amp;'
    else if s[i] = '<' then Result := Result + '&lt;'
    else if s[i] = '>' then Result := Result + '&gt;'
    else if Ord(s[i]) > 126 then Result := Result + '&#' + IntToStr(Ord(s[i])) + ';'
    else Result := Result + s[i];
  end;
end;

procedure RegisterLogonTask();
var
  user, xml, xmlPath, exePath: String;
  rc: Integer;
begin
  user := TaskXmlEscape(GetEnv('USERDOMAIN') + '\' + GetEnv('USERNAME'));
  exePath := TaskXmlEscape(ExpandConstant('{app}\{#MyAppExe}'));
  // No <?xml?> declaration: schtasks' MSXML rejects a declared encoding when
  // the file bytes don't match it exactly ("unable to switch the encoding" —
  // SaveStringToFile writes ANSI, not UTF-8/UTF-16). Without a declaration a
  // pure-ASCII file always parses; TaskXmlEscape keeps it pure ASCII.
  xml :=
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

// --- Live provisioning checklist ------------------------------------------
function SpinChar(i: Integer): String;
begin
  case (i mod 4) of
    0: Result := '|';
    1: Result := '/';
    2: Result := '-';
  else Result := '\';
  end;
end;

// Glyph shown left of each task: animated spinner while running, terminal marks
// otherwise. Monospace font keeps the labels aligned.
function StepGlyph(const state, spin: String): String;
begin
  if state = 'done' then Result := '[OK]'
  else if state = 'failed' then Result := '[!!]'
  else if state = 'skipped' then Result := '[--]'
  else Result := '[' + spin + ' ]';
end;

function IsTerminal(const state: String): Boolean;
begin
  Result := (state = 'done') or (state = 'failed') or (state = 'skipped');
end;

function CountDone(const state: String): Integer;
begin
  if (state = 'done') or (state = 'skipped') then Result := 1 else Result := 0;
end;

// Right-pad to a fixed width so the percentage column stays aligned (monospace).
function PadRight(const s: String; n: Integer): String;
begin
  Result := s;
  while Length(Result) < n do Result := Result + ' ';
end;

// Percentage shown right of the glyph. Blank for skipped/failed (the glyph
// already says so); a live "NN%" while running; "100%" once done.
function StepPercent(const state: String; pct: Integer): String;
begin
  if (state = 'skipped') or (state = 'failed') then Result := ''
  else Result := IntToStr(pct) + '%';
end;

// Extract "<key>":"<value>" from the backend's compact provisioning.status.json.
function StatusValue(const content, key: String): String;
var
  p, q: Integer;
  pat: String;
begin
  Result := '';
  pat := '"' + key + '":"';
  p := Pos(pat, content);
  if p = 0 then Exit;
  p := p + Length(pat);
  q := p;
  while (q <= Length(content)) and (content[q] <> '"') do Inc(q);
  Result := Copy(content, p, q - p);
end;

// Admin URL for the post-install "open the admin page" action: prefer the URL
// the server wrote into provisioning.status.json at startup (real HTTPS port,
// public domain once ready) over the provisional compile-time default.
function GetAdminUrl(Param: String): String;
var
  raw: AnsiString;
  url: String;
begin
  Result := '{#AdminUrl}';
  if LoadStringFromFile(
       ExpandConstant('{userappdata}\MoonlightWeb\MoonlightWeb\provisioning.status.json'), raw) then
  begin
    url := StatusValue(raw, 'admin_url');
    if url <> '' then Result := url;
  end;
end;

// Launch the server (kicks off first-run provisioning) and poll its status file,
// driving the on-screen checklist until every task is terminal or it times out.
procedure RunProvisionChecklist();
var
  statusPath, content, ps, ar, spin: String;
  raw: AnsiString; // LoadStringFromFile requires an AnsiString out-param.
  i, rc: Integer;
  pctSun, pctPair, pctAr, itSun, itPair, itAr: Integer;
  prevSun, prevPair, prevAr: String;
  holdDone: Boolean;
begin
  // Start the windowless tray server now so provisioning.json is consumed and
  // pairing + A-record run. ewNoWait: it keeps running after setup exits.
  Exec(ExpandConstant('{app}\{#MyAppExe}'), '', ExpandConstant('{app}'),
       SW_HIDE, ewNoWait, rc);

  // Silent installs have no UI to drive; the server still provisions in the
  // background.
  if WizardSilent then Exit;

  // Qt AppDataLocation on Windows: %AppData%\<Org>\<App> = MoonlightWeb\MoonlightWeb.
  statusPath := ExpandConstant('{userappdata}\MoonlightWeb\MoonlightWeb\provisioning.status.json');

  ProgressPage.SetText(ExpandConstant('{cm:ProvisionWorking}'), '');
  ProgressPage.Show;
  itSun := 0; itPair := 0; itAr := 0;
  prevSun := ''; prevPair := ''; prevAr := '';
  try
    // ~90s budget (300 * 300ms); the A-record may still finalize afterwards.
    for i := 0 to 300 do begin
      ps := ''; ar := '';
      if LoadStringFromFile(statusPath, raw) then begin
        content := raw; // implicit AnsiString -> String (Unicode Inno)
        ps := StatusValue(content, 'pairing');
        ar := StatusValue(content, 'arecord');
      end;
      spin := SpinChar(i);

      // Pseudo-progress: climb ~1%/300ms while a task runs (capped at 95), then
      // snap to 100 on done. Gives the long A-record/ACME step visible movement
      // instead of a spinner that looks frozen.
      if SunshineStepState = 'done' then pctSun := 100
      else if IsTerminal(SunshineStepState) then pctSun := 0
      else begin itSun := itSun + 1; pctSun := itSun; if pctSun > 95 then pctSun := 95; end;

      if ps = 'done' then pctPair := 100
      else if IsTerminal(ps) then pctPair := 0
      else begin itPair := itPair + 1; pctPair := itPair; if pctPair > 95 then pctPair := 95; end;

      if ar = 'done' then pctAr := 100
      else if IsTerminal(ar) then pctAr := 0
      else begin itAr := itAr + 1; pctAr := itAr; if pctAr > 95 then pctAr := 95; end;

      LblSunshine.Caption := StepGlyph(SunshineStepState, spin) + ' ' + PadRight(StepPercent(SunshineStepState, pctSun), 6) + ExpandConstant('{cm:TaskSunshine}');
      LblPairing.Caption  := StepGlyph(ps, spin) + ' ' + PadRight(StepPercent(ps, pctPair), 6) + ExpandConstant('{cm:TaskPairing}');
      LblArecord.Caption  := StepGlyph(ar, spin) + ' ' + PadRight(StepPercent(ar, pctAr), 6) + ExpandConstant('{cm:TaskArecord}');

      // Smooth overall bar driven by the three pseudo-percentages (max 300).
      ProgressPage.SetProgress(pctSun + pctPair + pctAr, 300);

      // When a task has just turned [OK], pause 1s so the user registers it.
      holdDone := ((SunshineStepState = 'done') and (prevSun <> 'done'))
               or ((ps = 'done') and (prevPair <> 'done'))
               or ((ar = 'done') and (prevAr <> 'done'));
      prevSun := SunshineStepState; prevPair := ps; prevAr := ar;

      if IsTerminal(SunshineStepState) and IsTerminal(ps) and IsTerminal(ar) then begin
        if holdDone then Sleep(1000);
        Break;
      end;
      if holdDone then Sleep(1000) else Sleep(300);
    end;
  finally
    ProgressPage.Hide;
  end;
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

  // Launch the server and show the live checklist (Sunshine / pairing / A-record)
  // while first-run provisioning completes.
  RunProvisionChecklist();
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  rc: Integer;
begin
  if CurUninstallStep = usUninstall then begin
    // Stop the running server first: remove the logon task (so it cannot be
    // relaunched), end any task-started instance, then force-kill the tray
    // process. Otherwise MoonlightWeb.exe keeps running and locks {app} files.
    Exec('schtasks.exe', '/End /TN "MoonlightWeb"', '', SW_HIDE,
         ewWaitUntilTerminated, rc);
    Exec('schtasks.exe', '/Delete /TN "MoonlightWeb" /F', '', SW_HIDE,
         ewWaitUntilTerminated, rc);
    Exec('taskkill.exe', '/IM "{#MyAppExe}" /F', '', SW_HIDE,
         ewWaitUntilTerminated, rc);
    DeleteFile(ExpandConstant('{group}\{cm:AdminShortcut}.url'));
    // Both desktops: the installer wrote the provisional shortcut to the common
    // desktop ({autodesktop} elevated); the server self-heals one on the USER
    // desktop at every startup (see writeAdminShortcut in main.cpp).
    DeleteFile(ExpandConstant('{autodesktop}\MoonlightWeb Admin.url'));
    DeleteFile(ExpandConstant('{userdesktop}\MoonlightWeb Admin.url'));
    DeleteFile(ExpandConstant('{app}\provisioning.json'));
    DeleteFile(ExpandConstant('{app}\provisioning.consumed.json'));
    DeleteFile(ExpandConstant('{userappdata}\MoonlightWeb\MoonlightWeb\provisioning.status.json'));
  end;
end;
