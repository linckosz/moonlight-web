; ===========================================================================
;  MoonlightWeb — interactive Windows installer (Inno Setup 6).
;
;  Produces a stepped wizard (MoonlightWeb-installer-<version>-win-<arch>.exe) that:
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
  #define MyAppVersion "0.1.2"
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
OutputBaseFilename=MoonlightWeb-installer-{#MyAppVersion}-win-{#MyArch}
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
; Simplified Chinese is not shipped with Inno Setup, so the translation is
; vendored next to this script (UTF-8 with BOM). Path is relative to the .iss.
Name: "zh"; MessagesFile: "ChineseSimplified.isl"

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
en.SunshineDetected=The installer detected that Sunshine is already installed on this machine.%nEnter its credentials to pair MoonlightWeb automatically, or leave them blank to skip and pair later from the admin page.
en.SunshineNotDetected=Sunshine was not detected. Check the box to install it automatically, then set its credentials.
en.SunshineUserLabel=Username
en.SunshinePassLabel=Password
en.SunshineCredsRequired=Please enter the Sunshine username and password so MoonlightWeb can pair automatically.
en.RunApp=Launch MoonlightWeb
en.RunAdmin=Open the admin page
en.SunshineDownloadCaption=Downloading and installing Sunshine...
en.SunshineDownloadFail=Failed to download Sunshine:
en.SunshineLaunchFail=Could not start the Sunshine installer.
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
fr.SunshineDetected=L'installeur a détecté que Sunshine est déjà installé sur cette machine.%nSaisissez ses identifiants pour appairer MoonlightWeb automatiquement, ou laissez-les vides pour ignorer et appairer plus tard depuis la page admin.
fr.SunshineNotDetected=Sunshine n'a pas été détecté. Cochez la case pour l'installer automatiquement, puis définissez ses identifiants.
fr.SunshineUserLabel=Identifiant
fr.SunshinePassLabel=Mot de passe
fr.SunshineCredsRequired=Veuillez saisir l'identifiant et le mot de passe Sunshine pour que MoonlightWeb puisse appairer automatiquement.
fr.RunApp=Lancer MoonlightWeb
fr.RunAdmin=Ouvrir la page admin
fr.SunshineDownloadCaption=Téléchargement et installation de Sunshine...
fr.SunshineDownloadFail=Échec du téléchargement de Sunshine :
fr.SunshineLaunchFail=Impossible de lancer l'installeur Sunshine.
fr.ProvisionPageCaption=Configuration de MoonlightWeb
fr.ProvisionPageDesc=Finalisation de l'installation
fr.ProvisionWorking=Veuillez patienter pendant la fin de la configuration de MoonlightWeb...
fr.TaskSunshine=Installer le serveur de streaming Sunshine
fr.TaskPairing=Appairer le Sunshine local
fr.TaskArecord=Publier l'adresse Internet sécurisée
; --- Simplified Chinese ---
zh.AutoStartTask=登录时启动 MoonlightWeb
zh.InternetPageCaption=互联网链接
zh.InternetPageDesc=是否允许从互联网访问？
zh.InternetPageBody=MoonlightWeb 可以发布一个安全的公共链接（例如 https://a1b2c3d4.moonlightweb.top），以便在本地网络之外进行串流。%n%n此操作将在应用程序安装完成后进行。
zh.InternetPageOption=允许互联网链接（推荐）
zh.SunshinePageCaption=Sunshine
zh.SunshinePageDesc=Sunshine 串流服务器
zh.SunshineInstallCheck=自动安装 Sunshine
zh.SunshineInstallCheckDone=自动安装 Sunshine（已安装）
zh.SunshineDetected=安装程序检测到此计算机上已安装 Sunshine。%n请输入其凭据以自动配对 MoonlightWeb，或留空以跳过并稍后在管理页面配对。
zh.SunshineNotDetected=未检测到 Sunshine。勾选此框以自动安装，然后设置其凭据。
zh.SunshineUserLabel=用户名
zh.SunshinePassLabel=密码
zh.SunshineCredsRequired=请输入 Sunshine 的用户名和密码，以便 MoonlightWeb 自动配对。
zh.RunApp=启动 MoonlightWeb
zh.RunAdmin=打开管理页面
zh.SunshineDownloadCaption=正在下载并安装 Sunshine...
zh.SunshineDownloadFail=下载 Sunshine 失败：
zh.SunshineLaunchFail=无法启动 Sunshine 安装程序。
zh.ProvisionPageCaption=正在设置 MoonlightWeb
zh.ProvisionPageDesc=正在完成安装
zh.ProvisionWorking=请稍候，MoonlightWeb 正在完成设置...
zh.TaskSunshine=安装 Sunshine 串流服务器
zh.TaskPairing=配对本地 Sunshine
zh.TaskArecord=发布安全的互联网地址

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "autostart"; Description: "{cm:AutoStartTask}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
; Start-Menu group + Desktop: a single "MoonlightWeb" entry, a .lnk shortcut that
; LAUNCHES THE EXE (not a URL). The windowless app starts when it is down or, when
; already running, surfaces the admin page via its single-instance logic — so one
; click always lands on the admin page, launching the app first if needed. The
; uninstaller entry lives in the group too.
Name: "{group}\MoonlightWeb"; Filename: "{app}\{#MyAppExe}"; WorkingDir: "{app}"
Name: "{autodesktop}\MoonlightWeb"; Filename: "{app}\{#MyAppExe}"; WorkingDir: "{app}"; Tasks: desktopicon
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
  // Step: Internet link authorization. Unchecked by default: opening the
  // machine to the Internet (UPnP + public DNS record) requires an explicit
  // opt-in click from the user. The option is highlighted in a positive green
  // tint instead of being pre-ticked.
  InternetPage := CreateInputOptionPage(wpSelectTasks,
    ExpandConstant('{cm:InternetPageCaption}'), ExpandConstant('{cm:InternetPageDesc}'),
    ExpandConstant('{cm:InternetPageBody}'),
    False, False);
  InternetPage.Add(ExpandConstant('{cm:InternetPageOption}'));
  InternetPage.Values[0] := False;
  // TColor is $00BBGGRR: $43A02E = RGB(46,160,67), a discreet positive green.
  InternetPage.CheckListBox.Font.Color := $43A02E;

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
      // leaving a pending pairing request and an unpaired host. The user types
      // Sunshine's real username/password to pair — or leaves them blank to skip
      // pairing (auto_pair is then written false; see CurStepChanged).
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

// Credentials are only MANDATORY for a fresh auto-install: the silent installer
// sets Sunshine's username/password via --creds, so both must be provided. When
// Sunshine is ALREADY installed the credentials only drive optional auto-pairing
// — the user may leave them blank to skip pairing and continue (they can pair
// later from the admin page). If Sunshine is absent and install is declined, the
// grayed-out fields are irrelevant.
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (SunshinePage <> nil) and (CurPageID = SunshinePage.ID) then begin
    if (not SunshineDetected) and SunshineInstallCheck.Checked
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
    '  <Actions Context="Author"><Exec><Command>"' + exePath + '"</Command>' +
    '<Arguments>--autostart</Arguments></Exec></Actions>' + #13#10 +
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

// Allow the server through Windows Defender Firewall. Program-scoped (not port-
// scoped) on purpose: the app may listen on 443, on a per-instance parity port
// (e.g. 44729 when another device already forwards 443 on the router), or on a
// startup fallback port — a single program rule covers every port it ever picks,
// so the inbound listener is never silently blocked (the windowless tray/logon
// process would otherwise never get the "allow" prompt). Idempotent: the old
// rule (if any) is deleted first, so re-installs don't stack duplicates.
procedure AddFirewallRule();
var
  rc: Integer;
  exePath: String;
begin
  exePath := ExpandConstant('{app}\{#MyAppExe}');
  Exec(ExpandConstant('{sys}\netsh.exe'),
       'advfirewall firewall delete rule name="MoonlightWeb"',
       '', SW_HIDE, ewWaitUntilTerminated, rc);
  Exec(ExpandConstant('{sys}\netsh.exe'),
       'advfirewall firewall add rule name="MoonlightWeb" dir=in action=allow'
       + ' program="' + exePath + '" enable=yes profile=any',
       '', SW_HIDE, ewWaitUntilTerminated, rc);
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
  // --autostart: an automatic launch must not open the browser itself — the
  // [Run] "open the admin page" checkbox owns that.
  Exec(ExpandConstant('{app}\{#MyAppExe}'), '--autostart', ExpandConstant('{app}'),
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
  lines: TArrayOfString;
  internet, consent, autoPair: String;
begin
  if CurStep <> ssPostInstall then Exit;

  // provisioning.json — consumed and removed by the server on first run.
  // Written as UTF-8 (no BOM): the consent text is localized (accents) and the
  // server parses this file with a strict UTF-8 JSON parser.
  if InternetPage.Values[0] then internet := 'true' else internet := 'false';
  // Exact agreement text the user read on the Internet page — recorded by the
  // server in its DNS registration audit log (legal traceability).
  consent := ExpandConstant('{cm:InternetPageBody}') + ' / '
           + ExpandConstant('{cm:InternetPageOption}');
  StringChangeEx(consent, '%n', ' ', True);
  // Only ask the server to auto-pair when the user actually supplied credentials.
  // Blank creds mean "skip pairing" (Sunshine already installed, user chose not
  // to pair now): pairing with empty creds would fail and leave a pending
  // request, so mark it skipped instead.
  if (Trim(SunshineUserEdit.Text) <> '') and (Trim(SunshinePassEdit.Text) <> '') then
    autoPair := 'true'
  else
    autoPair := 'false';
  SetArrayLength(lines, 9);
  lines[0] := '{';
  lines[1] := '  "internet_access_authorized": ' + internet + ',';
  lines[2] := '  "consent_message": "' + JsonEscape(consent) + '",';
  lines[3] := '  "sunshine": {';
  lines[4] := '    "auto_pair": ' + autoPair + ',';
  lines[5] := '    "username": "' + JsonEscape(SunshineUserEdit.Text) + '",';
  lines[6] := '    "password": "' + JsonEscape(SunshinePassEdit.Text) + '"';
  lines[7] := '  }';
  lines[8] := '}';
  SaveStringsToUTF8FileWithoutBOM(ExpandConstant('{app}\provisioning.json'), lines, False);

  // The Desktop / Start-Menu shortcuts are now .lnk files that launch the exe
  // (created in [Icons]); clean up stale .url shortcuts left by pre-2026-07
  // versions (the server used to self-heal a Desktop/Start-Menu .url pointing
  // at the admin page, which did not start the app).
  DeleteFile(ExpandConstant('{group}\MoonlightWeb Admin.url'));
  DeleteFile(ExpandConstant('{autodesktop}\MoonlightWeb Admin.url'));
  DeleteFile(ExpandConstant('{userdesktop}\MoonlightWeb Admin.url'));

  // Open the firewall for the server before it first binds a listener, so the
  // inbound HTTPS port (443 / parity / fallback) is reachable from the LAN and
  // the router's forwarded port — never silently dropped.
  AddFirewallRule();

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
    // Remove the firewall rule added at install time.
    Exec(ExpandConstant('{sys}\netsh.exe'),
         'advfirewall firewall delete rule name="MoonlightWeb"', '', SW_HIDE,
         ewWaitUntilTerminated, rc);
    DeleteFile(ExpandConstant('{group}\MoonlightWeb Admin.url'));
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
