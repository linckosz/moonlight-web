; ===========================================================================
;  MoonlightWeb — interactive Windows installer (Inno Setup 6).
;
;  Produces a stepped wizard (MoonlightWeb-installer-<version>-win-<arch>.exe) that:
;    1. installs the app + Start-Menu shortcuts (app, admin page, uninstaller),
;    2. asks the user to authorize the Internet link (named public domain),
;    3. detects Sunshine and adapts a Skip/Next/Cancel page to what it finds —
;       absent (install it silently with the prefilled admin/admin credentials),
;       installed but unpaired (ask for its real credentials), or already paired
;       (nothing to ask, no Skip button),
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

; Inno Setup 6.6 gave CreateCustomForm its width/height arguments (the uninstall
; "delete my configuration" dialog uses them). An older compiler would fail deep
; in [Code] with "Invalid number of parameters" — say why instead.
#if VER < EncodeVer(6,6,0)
  #error Inno Setup 6.6 or newer is required to compile this script.
#endif

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
AppPublisherURL=https://github.com/linckosz/moonlight-web
DefaultDirName={autopf}\MoonlightWeb
DefaultGroupName=MoonlightWeb
DisableProgramGroupPage=yes
OutputBaseFilename=MoonlightWeb-installer-{#MyAppVersion}-win-{#MyArch}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; NO SignTool= here, on purpose. Authenticode signing happens in release.yml,
; around this compile rather than inside it: SignPath signs remotely and the
; private key never reaches the build machine, so there is no local signtool for
; ISCC to call. The payload exe is signed before it is packaged here, and the
; installer this produces is signed afterwards.
;
; The one casualty is the uninstaller: ISCC generates it at compile time and
; SignedUninstaller=yes would need that same local signtool, so unins000.exe
; ships unsigned and the uninstall UAC prompt reads "Unknown publisher". The
; download itself — the only thing SmartScreen gates — is fully signed. Adding
; SignTool=/SignedUninstaller= back would just make ISCC abort with "Unknown
; SignTool name" unless a signing provider with a local CLI replaces SignPath.
; Branding: installer .exe icon + small wizard logo (top-right on inner pages).
; Paths are relative to this .iss; PNG wizard images need Inno Setup 6.3+.
SetupIconFile=..\..\frontend\assets\favicon.ico
WizardSmallImageFile=..\..\frontend\assets\logo-512.png
; Add/Remove Programs entry shows the app icon (embedded in the exe).
UninstallDisplayIcon={app}\{#MyAppExe}
UninstallDisplayName={#MyAppName}
PrivilegesRequired=admin
; Do NOT let Inno's Restart Manager pass deal with the running MoonlightWeb.
; That pass happens while "Preparing to Install", i.e. BEFORE ssInstall — before
; StopRunningInstance() gets its turn — and it is looking at a process that may
; be a session-0 service. In a `/VERYSILENT /SUPPRESSMSGBOXES` run (how the app
; updates itself) anything it stumbles on becomes a silent abort with nothing but
; an exit code to show for it. We stop the app/service ourselves at ssInstall and
; bring it back at ssPostInstall, which is the whole point of those two steps.
CloseApplications=no
RestartApplications=no
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
en.InternetPageBody=MoonlightWeb can allow streaming from outside your local network. Your router is asked (UPnP) to open a streaming port during each session, and whoever connects reaches this PC directly — each side of a peer-to-peer connection sees the other's public IP address. This PC holds one outgoing connection to the MoonlightWeb introduction server, which sees its public IP address and when it is online, and asks a MoonlightWeb STUN server what that public address is; if that server cannot be reached, a public one (Google, Cloudflare) is tried instead.%n%nNo public DNS record is created, no certificate is issued for this machine, and ports 80/443 stay closed. You can turn this off at any time from the Admin page.
en.InternetPageOption=Allow the Internet link (recommended)
en.SunshinePageCaption=Sunshine
en.SunshinePageDesc=Sunshine streaming server
en.SunshineInstallCheck=Install Sunshine automatically
en.SunshineInstallCheckDone=Install Sunshine automatically (already installed)
en.SunshineDetected=The installer detected that Sunshine is already installed on this machine.%nEnter its credentials to pair MoonlightWeb automatically, or click Skip to pair later from the admin page.
en.SunshineNotDetected=Sunshine was not detected. Check the box to install it automatically. The credentials below are the ones it will be created with — change them if you prefer.
en.SunshineAlreadyPaired=Sunshine is already installed on this machine and already paired with MoonlightWeb.%n%nThere is nothing to set up here — click Next to continue.
en.ButtonSkip=&Skip
en.InternetBtnSkip=&Skip
en.InternetBtnAccept=&Accept
en.SunshineUserLabel=Username
en.SunshinePassLabel=Password
en.SunshineCredsRequired=Please enter the Sunshine username and password so MoonlightWeb can pair automatically.
en.SunshineCheckWait=Checking the Sunshine credentials...
en.SunshineCredsWrong=Sunshine refused these credentials.%n%nCheck the username and password of its web interface (https://localhost:47990), or click Skip to pair later from the admin page.
en.SunshineNoAnswer=Sunshine is installed but is not answering on this machine.%n%nStart Sunshine and try again, or click Skip to pair later from the admin page.
en.RunApp=Launch MoonlightWeb
en.RunAdmin=Open the admin page
en.SunshineDownloadCaption=Downloading and installing Sunshine...
en.SunshineDownloadFail=Failed to download Sunshine:
en.SunshineLaunchFail=Could not start the Sunshine installer.
en.ProvisionPageCaption=Setting up MoonlightWeb
en.ProvisionPageDesc=Finalizing the installation
en.ProvisionWorking=Please wait while MoonlightWeb finishes setting up...
en.TaskSunshine=Install the Sunshine streaming server
en.TaskSunshineDone=Sunshine is already installed
en.TaskPairing=Pair the local Sunshine
en.TaskPairingDone=Sunshine is already paired
en.TaskArecord=Enable the Internet link
en.ButtonUpdate=&Update
en.UpdatePageCaption=Update MoonlightWeb
en.UpdatePageDesc=A newer version will replace the installed one
en.UpdateReadyMemo=MoonlightWeb %1 is installed and will be updated to %2.%n%nYour settings, the Internet link and the Sunshine pairing are kept as they are — there is nothing to configure.
en.UpdateReadyMemoFresh=MoonlightWeb will be updated to %1.%n%nYour settings, the Internet link and the Sunshine pairing are kept as they are — there is nothing to configure.
en.UninstConfigTitle=Uninstall MoonlightWeb
en.UninstConfigBody=Your configuration is kept by default: if you install MoonlightWeb again, it starts up exactly as you left it.
en.UninstConfigOption=Also delete my configuration
en.UninstConfigDetail=Settings, accounts, certificates and Sunshine pairings are erased for good.
; --- French ---
fr.AutoStartTask=Démarrer MoonlightWeb à l'ouverture de session
fr.InternetPageCaption=Lien Internet
fr.InternetPageDesc=Autoriser l'accès depuis Internet ?
fr.InternetPageBody=MoonlightWeb peut autoriser le streaming depuis l'extérieur de votre réseau local. Votre box se voit demander (UPnP) l'ouverture d'un port de streaming pendant chaque session, et celui qui se connecte joint ce PC directement — chaque côté d'une connexion pair-à-pair voit l'adresse IP publique de l'autre. Ce PC maintient une connexion sortante vers le serveur d'introduction MoonlightWeb, qui voit alors son adresse IP publique et ses périodes de présence en ligne, et demande à un serveur STUN MoonlightWeb quelle est cette adresse publique ; si ce serveur est injoignable, un serveur public (Google, Cloudflare) est essayé à la place.%n%nAucun enregistrement DNS public n'est créé, aucun certificat n'est émis pour cette machine, et les ports 80/443 restent fermés. Désactivable à tout moment depuis la page admin.
fr.InternetPageOption=Autoriser le lien Internet (recommandé)
fr.SunshinePageCaption=Sunshine
fr.SunshinePageDesc=Serveur de streaming Sunshine
fr.SunshineInstallCheck=Installer Sunshine automatiquement
fr.SunshineInstallCheckDone=Installer Sunshine automatiquement (déjà installé)
fr.SunshineDetected=L'installeur a détecté que Sunshine est déjà installé sur cette machine.%nSaisissez ses identifiants pour appairer MoonlightWeb automatiquement, ou cliquez sur Ignorer pour appairer plus tard depuis la page admin.
fr.SunshineNotDetected=Sunshine n'a pas été détecté. Cochez la case pour l'installer automatiquement. Les identifiants ci-dessous sont ceux qui lui seront attribués — modifiez-les si vous le souhaitez.
fr.SunshineAlreadyPaired=Sunshine est déjà installé sur cette machine et déjà appairé avec MoonlightWeb.%n%nIl n'y a rien à configurer ici — cliquez sur Suivant pour continuer.
fr.ButtonSkip=&Ignorer
fr.InternetBtnSkip=&Passer
fr.InternetBtnAccept=&Accepter
fr.SunshineUserLabel=Identifiant
fr.SunshinePassLabel=Mot de passe
fr.SunshineCredsRequired=Veuillez saisir l'identifiant et le mot de passe Sunshine pour que MoonlightWeb puisse appairer automatiquement.
fr.SunshineCheckWait=Vérification des identifiants Sunshine...
fr.SunshineCredsWrong=Sunshine a refusé ces identifiants.%n%nVérifiez l'identifiant et le mot de passe de son interface web (https://localhost:47990), ou cliquez sur Ignorer pour appairer plus tard depuis la page admin.
fr.SunshineNoAnswer=Sunshine est installé mais ne répond pas sur cette machine.%n%nDémarrez Sunshine puis réessayez, ou cliquez sur Ignorer pour appairer plus tard depuis la page admin.
fr.RunApp=Lancer MoonlightWeb
fr.RunAdmin=Ouvrir la page admin
fr.SunshineDownloadCaption=Téléchargement et installation de Sunshine...
fr.SunshineDownloadFail=Échec du téléchargement de Sunshine :
fr.SunshineLaunchFail=Impossible de lancer l'installeur Sunshine.
fr.ProvisionPageCaption=Configuration de MoonlightWeb
fr.ProvisionPageDesc=Finalisation de l'installation
fr.ProvisionWorking=Veuillez patienter pendant la fin de la configuration de MoonlightWeb...
fr.TaskSunshine=Installer le serveur de streaming Sunshine
fr.TaskSunshineDone=Sunshine est déjà installé
fr.TaskPairing=Appairer le Sunshine local
fr.TaskPairingDone=Sunshine est déjà appairé
fr.TaskArecord=Activer le lien Internet
fr.ButtonUpdate=&Mettre à jour
fr.UpdatePageCaption=Mise à jour de MoonlightWeb
fr.UpdatePageDesc=Une version plus récente va remplacer celle installée
fr.UpdateReadyMemo=MoonlightWeb %1 est installé et va être mis à jour vers %2.%n%nVos réglages, le lien Internet et l'appairage Sunshine sont conservés tels quels — il n'y a rien à configurer.
fr.UpdateReadyMemoFresh=MoonlightWeb va être mis à jour vers %1.%n%nVos réglages, le lien Internet et l'appairage Sunshine sont conservés tels quels — il n'y a rien à configurer.
fr.UninstConfigTitle=Désinstallation de MoonlightWeb
fr.UninstConfigBody=Votre configuration est conservée par défaut : si vous réinstallez MoonlightWeb, il redémarrera exactement dans l'état où vous l'avez laissé.
fr.UninstConfigOption=Supprimer aussi ma configuration
fr.UninstConfigDetail=Les réglages, comptes, certificats et appairages Sunshine seront définitivement effacés.
; --- Simplified Chinese ---
zh.AutoStartTask=登录时启动 MoonlightWeb
zh.InternetPageCaption=互联网链接
zh.InternetPageDesc=是否允许从互联网访问？
zh.InternetPageBody=MoonlightWeb 可以允许从本地网络之外进行串流。每次串流会话期间会通过 UPnP 请求路由器开放一个串流端口，连接方将直接连到这台电脑——点对点连接的双方都能看到对方的公网 IP 地址。这台电脑会保持一条到 MoonlightWeb 介绍服务器的出站连接，该服务器因此能看到这台电脑的公网 IP 地址及其在线时间；同时会向 MoonlightWeb STUN 服务器查询该公网地址，若该服务器无法连接，则改用公共服务器（Google、Cloudflare）。%n%n不会创建任何公开 DNS 记录，不会为这台机器签发任何证书，端口 80/443 保持关闭。可随时在管理页面关闭。
zh.InternetPageOption=允许互联网链接（推荐）
zh.SunshinePageCaption=Sunshine
zh.SunshinePageDesc=Sunshine 串流服务器
zh.SunshineInstallCheck=自动安装 Sunshine
zh.SunshineInstallCheckDone=自动安装 Sunshine（已安装）
zh.SunshineDetected=安装程序检测到此计算机上已安装 Sunshine。%n请输入其凭据以自动配对 MoonlightWeb，或点击“跳过”以稍后在管理页面配对。
zh.SunshineNotDetected=未检测到 Sunshine。勾选此框以自动安装。下方的凭据即为将要设置的凭据 — 如需更改请自行修改。
zh.SunshineAlreadyPaired=此计算机上已安装 Sunshine，并且已与 MoonlightWeb 配对。%n%n此处无需任何设置 — 点击“下一步”继续。
zh.ButtonSkip=跳过(&S)
zh.InternetBtnSkip=跳过(&S)
zh.InternetBtnAccept=接受(&A)
zh.SunshineUserLabel=用户名
zh.SunshinePassLabel=密码
zh.SunshineCredsRequired=请输入 Sunshine 的用户名和密码，以便 MoonlightWeb 自动配对。
zh.SunshineCheckWait=正在验证 Sunshine 凭据...
zh.SunshineCredsWrong=Sunshine 拒绝了这些凭据。%n%n请在其网页界面（https://localhost:47990）中核对用户名和密码，或点击“跳过”以稍后在管理页面配对。
zh.SunshineNoAnswer=此计算机上已安装 Sunshine，但没有响应。%n%n请启动 Sunshine 后重试，或点击“跳过”以稍后在管理页面配对。
zh.RunApp=启动 MoonlightWeb
zh.RunAdmin=打开管理页面
zh.SunshineDownloadCaption=正在下载并安装 Sunshine...
zh.SunshineDownloadFail=下载 Sunshine 失败：
zh.SunshineLaunchFail=无法启动 Sunshine 安装程序。
zh.ProvisionPageCaption=正在设置 MoonlightWeb
zh.ProvisionPageDesc=正在完成安装
zh.ProvisionWorking=请稍候，MoonlightWeb 正在完成设置...
zh.TaskSunshine=安装 Sunshine 串流服务器
zh.TaskSunshineDone=Sunshine 已安装
zh.TaskPairing=配对本地 Sunshine
zh.TaskPairingDone=Sunshine 已配对
zh.TaskArecord=启用互联网链接
zh.ButtonUpdate=更新(&U)
zh.UpdatePageCaption=更新 MoonlightWeb
zh.UpdatePageDesc=较新的版本将替换已安装的版本
zh.UpdateReadyMemo=已安装 MoonlightWeb %1，将更新到 %2。%n%n您的设置、互联网链接和 Sunshine 配对将保持不变 — 无需任何配置。
zh.UpdateReadyMemoFresh=MoonlightWeb 将更新到 %1。%n%n您的设置、互联网链接和 Sunshine 配对将保持不变 — 无需任何配置。
zh.UninstConfigTitle=卸载 MoonlightWeb
zh.UninstConfigBody=默认保留您的配置：如果您再次安装 MoonlightWeb，它将完全按照您离开时的状态启动。
zh.UninstConfigOption=同时删除我的配置
zh.UninstConfigDetail=设置、账户、证书和 Sunshine 配对将被永久删除。

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
// The Sunshine page has three mutually exclusive shapes (see CurPageChanged):
//   scAbsent — Sunshine not installed: offer to install it, credentials
//              prefilled with admin/admin (the ones it will be created with).
//   scUnpaired — Sunshine installed but MoonlightWeb never paired with it: ask
//              for its real credentials so first-run pairing can push the PIN.
//   scPaired — Sunshine installed AND already paired: nothing to ask, the page
//              is a plain confirmation (no Skip button either).
const
  scAbsent = 0;
  scUnpaired = 1;
  scPaired = 2;
  // Inno files its uninstall entry under "<AppId>_is1" and exposes no constant
  // for it to [Code] — KEEP THIS IN SYNC WITH [Setup] AppId above.
  UninstallKey = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{6F2C9E4A-7B3D-4E5F-9A1C-2D8E4B6F0A33}_is1';
  // Trigger-less, elevated task the running (unprivileged) app starts with
  // `schtasks /Run` to apply an update without a UAC prompt nobody could answer
  // from a remote browser. Its action is the fixed staging path SelfUpdater
  // downloads to — the two MUST agree or the task runs nothing.
  UpdateTaskName = 'MoonlightWeb Update';
  UpdateStagedExe = '%LocalAppData%\MoonlightWeb\update\MoonlightWeb-update.exe';
  UpdateSilentArgs = '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-';

var
  InternetPage: TWizardPage;
  InternetBodyLabel: TNewStaticText;
  InternetOptionLabel: TNewStaticText;
  // The Internet page asks with two buttons rather than a checkbox: "Passer"
  // takes the Back button's slot, "Accepter" is the Next button wearing another
  // caption. InternetSkipped is what tells the two apart, because Skip reaches
  // NextButtonClick through a simulated Next click (see InternetSkipClick).
  InternetSkipButton: TNewButton;
  InternetSkipped: Boolean;
  InternetAuthorized: Boolean;
  SunshinePage: TWizardPage;
  SunshineInstallCheck: TNewCheckBox;
  SunshineUserEdit: TNewEdit;
  SunshinePassEdit: TNewEdit;
  SunshineUserLabel: TNewStaticText;
  SunshinePassLabel: TNewStaticText;
  SunshineStatusLabel: TNewStaticText;
  // Shown only while Next probes an already-installed Sunshine with the typed
  // credentials (ProbeSunshineCreds).
  SunshineCheckLabel: TNewStaticText;
  SunshineDetected: Boolean;
  SunshineExePath: String;
  SunshineCase: Integer;
  SunshinePagePrepared: Boolean;
  // The Sunshine page swaps Back for a Skip button; these track that swap and
  // the user's choice to walk past the page without installing/pairing.
  SkipButton: TNewButton;
  BackButtonHidden: Boolean;
  SunshineSkipped: Boolean;
  // The password starts visible (prefilled "admin") and masks itself as soon as
  // the user edits it. SettingPass suppresses the OnChange we cause ourselves.
  SunshinePassMasked: Boolean;
  SettingPass: Boolean;
  // Live post-install checklist (Sunshine / pairing / A-record).
  ProgressPage: TOutputProgressWizardPage;
  LblSunshine: TNewStaticText;
  LblPairing: TNewStaticText;
  LblArecord: TNewStaticText;
  SunshineStepState: String;
  // Update mode: a MoonlightWeb is already installed, so this run only swaps the
  // payload. Every question was answered by the previous install — the wizard
  // collapses to a single "Update" confirmation (see ShouldSkipPage).
  UpdateMode: Boolean;
  PrevVersion: String;
  // The server runs as an NSSM service (session 0): stop/start it around the file
  // copy instead of killing and relaunching a tray process.
  ServiceInstalled: Boolean;

// --- Detection ------------------------------------------------------------

// Version of the MoonlightWeb already installed, '' when this is a first install.
function InstalledVersion(): String;
var
  v: String;
begin
  if RegQueryStringValue(HKLM, UninstallKey, 'DisplayVersion', v) then Result := v
  else Result := '';
end;

// True when a previous install is present. The registry entry is the only
// signal available this early: InitializeWizard runs before the destination
// page, so {app} is NOT initialized yet and expanding it there aborts Setup
// with "An attempt was made to expand the 'app' constant before it was
// initialized" (only visible on a machine with no previous install, where the
// registry test doesn't short-circuit first). The leftover-exe case — a
// half-removed install whose server may still be running — is handled at
// ssInstall instead, where {app} is known (see CurStepChanged).
function PreviousInstallExists(): Boolean;
begin
  Result := RegKeyExists(HKLM, UninstallKey);
end;

// True when the server is registered as a Windows service (install-service.bat).
function DetectService(): Boolean;
var
  rc: Integer;
begin
  Result := Exec(ExpandConstant('{sys}\sc.exe'), 'query MoonlightWeb', '', SW_HIDE,
                 ewWaitUntilTerminated, rc) and (rc = 0);
end;

// True when the logon task created by a previous install is still registered.
function LogonTaskExists(): Boolean;
var
  rc: Integer;
begin
  Result := Exec('schtasks.exe', '/Query /TN "MoonlightWeb"', '', SW_HIDE,
                 ewWaitUntilTerminated, rc) and (rc = 0);
end;

// True when a Desktop shortcut created by a previous install is still there.
function DesktopIconExists(): Boolean;
begin
  Result := FileExists(ExpandConstant('{autodesktop}\MoonlightWeb.lnk'))
         or FileExists(ExpandConstant('{userdesktop}\MoonlightWeb.lnk'));
end;

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

// True when the registry entry describes the Sunshine running on THIS machine:
// either the host was added by loopback address (what the installer's own
// auto-pairing does) or its advertised hostname matches the local computer name
// (what an mDNS-discovered local host looks like).
function IsLocalHostEntry(const key: String): Boolean;
var
  v: String;
begin
  Result := False;
  if RegQueryStringValue(HKCU, key, 'manualaddress', v)
     and ((v = '127.0.0.1') or (v = '::1') or (Lowercase(v) = 'localhost')) then begin
    Result := True;
    Exit;
  end;
  if RegQueryStringValue(HKCU, key, 'hostname', v) and (v <> '')
     and (CompareText(v, GetComputerNameString()) = 0) then
    Result := True;
end;

// True when a previous MoonlightWeb run already paired with the local Sunshine.
// The server persists its host list through QSettings, which on Windows is the
// registry (HKCU\Software\<org>\<app>\hosts\<n>\...; see ComputerManager::
// saveHosts / NvComputer::serialize). Absent key / no local paired host → False,
// so an unreadable hive degrades to the normal "ask for credentials" page.
function LocalSunshinePaired(): Boolean;
var
  root, key, state: String;
  names: TArrayOfString;
  i: Integer;
begin
  Result := False;
  root := 'Software\MoonlightWeb\MoonlightWeb\hosts';
  if not RegGetSubkeyNames(HKCU, root, names) then Exit;
  for i := 0 to GetArrayLength(names) - 1 do begin
    key := root + '\' + names[i];
    if RegQueryStringValue(HKCU, key, 'pairState', state)
       and (Lowercase(state) = 'paired') and IsLocalHostEntry(key) then begin
      Result := True;
      Exit;
    end;
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

// The prefilled default ("admin") is shown in clear so the user can read what
// will be applied; the field turns into a real password box on the first edit.
procedure SunshinePassChange(Sender: TObject);
begin
  if SettingPass or SunshinePassMasked then Exit;
  SunshinePassMasked := True;
  SunshinePassEdit.PasswordChar := '*';
end;

procedure SetSunshinePassword(const value: String; masked: Boolean);
begin
  SettingPass := True;
  SunshinePassEdit.Text := value;
  SettingPass := False;
  SunshinePassMasked := masked;
  if masked then SunshinePassEdit.PasswordChar := '*'
  else SunshinePassEdit.PasswordChar := #0;
end;

// "Skip": walk past the Sunshine page without installing or pairing anything.
// Clearing both fields is what makes CurStepChanged write auto_pair=false, and
// SunshineSkipped keeps PrepareToInstall from downloading Sunshine. The user can
// still install/pair later from the admin page.
procedure SkipButtonClick(Sender: TObject);
begin
  SunshineSkipped := True;
  SunshineUserEdit.Text := '';
  SetSunshinePassword('', True);
  // Simulate a click on Next — Inno has no API to advance the wizard directly.
  WizardForm.NextButton.OnClick(WizardForm.NextButton);
end;

// "Passer": decline the Internet link and move on. Inno has no API to advance
// the wizard, so this simulates Next — which means NextButtonClick fires for
// this page either way, and InternetSkipped is the only thing that tells a
// declined page from an accepted one.
procedure InternetSkipClick(Sender: TObject);
begin
  InternetSkipped := True;
  WizardForm.NextButton.OnClick(WizardForm.NextButton);
end;

// Reads a top-level boolean from the server's settings.json. Returns True only
// when the key is present with the JSON value `true`. The file is written by
// QJsonDocument (indented) as `"key": true`, so we locate `"key":` and test the
// short token that follows the colon. Missing file / missing key → False.
function SettingsBoolEnabled(const key: String): Boolean;
var
  raw: AnsiString; // LoadStringFromFile requires an AnsiString out-param.
  content, seg: String;
  p: Integer;
begin
  Result := False;
  if not LoadStringFromFile(
       ExpandConstant('{userappdata}\MoonlightWeb\MoonlightWeb\settings.json'), raw) then
    Exit;
  content := raw;
  p := Pos('"' + key + '":', content);
  if p = 0 then Exit;
  p := p + Length(key) + 3; // skip the opening quote, key, closing quote and ':'
  seg := Lowercase(Copy(content, p, 8)); // e.g. ' true,' / ' false'
  Result := Pos('true', seg) > 0;
end;

// Whether the server ever recorded a value for `key`. This is what makes a
// wizard page "already answered" in update mode: a page whose setting is absent
// is a NEW question and must still be shown, everything else is skipped. Any
// page added in a future version follows the same rule for free.
function SettingsHasKey(const key: String): Boolean;
var
  raw: AnsiString;
  content: String;
begin
  Result := False;
  if not LoadStringFromFile(
       ExpandConstant('{userappdata}\MoonlightWeb\MoonlightWeb\settings.json'), raw) then
    Exit;
  content := raw;
  Result := Pos('"' + key + '":', content) > 0;
end;

procedure InitializeWizard();
var
  tasks: String;
begin
  // Decide the wizard's shape before any page is built: an existing install
  // turns this into an update.
  PrevVersion := InstalledVersion();
  UpdateMode := PreviousInstallExists();

  // Step: Internet link authorization. Two buttons, not a checkbox: the page
  // cannot be walked past without answering it, and neither answer is
  // pre-selected — a checkbox that is merely left alone reads as an answer
  // nobody gave. There is nothing to remember from a previous install either:
  // ShouldSkipPage hides the page entirely once settings.json holds the key.
  InternetPage := CreateCustomPage(wpSelectTasks,
    ExpandConstant('{cm:InternetPageCaption}'), ExpandConstant('{cm:InternetPageDesc}'));

  InternetBodyLabel := TNewStaticText.Create(WizardForm);
  InternetBodyLabel.Parent := InternetPage.Surface;
  InternetBodyLabel.Left := 0;
  InternetBodyLabel.Top := 0;
  InternetBodyLabel.Width := InternetPage.SurfaceWidth;
  // WordWrap + AutoSize, so the height follows the text instead of a number
  // guessed here. The wording has already grown once (it now names the
  // introduction and STUN servers) and a fixed height would have clipped the
  // last sentence — which is the one saying what is NOT done.
  InternetBodyLabel.WordWrap := True;
  InternetBodyLabel.AutoSize := True;
  InternetBodyLabel.Caption := ExpandConstant('{cm:InternetPageBody}');

  // The sentence the Accept button agrees to, kept as its own line so the
  // consent record below can still quote it verbatim.
  // TColor is $00BBGGRR: $43A02E = RGB(46,160,67), a discreet positive green.
  InternetOptionLabel := TNewStaticText.Create(WizardForm);
  InternetOptionLabel.Parent := InternetPage.Surface;
  InternetOptionLabel.Left := 0;
  InternetOptionLabel.Top := InternetBodyLabel.Top + InternetBodyLabel.Height + ScaleY(12);
  InternetOptionLabel.Width := InternetPage.SurfaceWidth;
  InternetOptionLabel.WordWrap := True;
  InternetOptionLabel.Font.Style := [fsBold];
  InternetOptionLabel.Font.Color := $43A02E;
  InternetOptionLabel.Caption := ExpandConstant('{cm:InternetPageOption}');

  // "Passer" in the slot the Back button gives up on this page, so the pair
  // reads Passer / Accepter left to right — the order a Windows user expects,
  // with the affirmative one on the right. Same construction as SkipButton on
  // the Sunshine page.
  InternetSkipButton := TNewButton.Create(WizardForm);
  InternetSkipButton.Parent := WizardForm.BackButton.Parent;
  InternetSkipButton.Left := WizardForm.BackButton.Left;
  InternetSkipButton.Top := WizardForm.BackButton.Top;
  InternetSkipButton.Width := WizardForm.BackButton.Width;
  InternetSkipButton.Height := WizardForm.BackButton.Height;
  InternetSkipButton.Caption := ExpandConstant('{cm:InternetBtnSkip}');
  InternetSkipButton.OnClick := @InternetSkipClick;
  InternetSkipButton.Visible := False;

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
  SunshinePassEdit.OnChange := @SunshinePassChange;
  SunshinePassMasked := True;

  // Waiting line for the credential probe, under the two fields.
  SunshineCheckLabel := TNewStaticText.Create(WizardForm);
  SunshineCheckLabel.Parent := SunshinePage.Surface;
  SunshineCheckLabel.Top := ScaleY(136);
  SunshineCheckLabel.Width := SunshinePage.SurfaceWidth;
  SunshineCheckLabel.Visible := False;

  // "Skip" — shown only on the Sunshine page, in the slot the Back button
  // occupies (which is hidden there): the page must offer Skip / Next / Cancel.
  SkipButton := TNewButton.Create(WizardForm);
  SkipButton.Parent := WizardForm.BackButton.Parent;
  SkipButton.Left := WizardForm.BackButton.Left;
  SkipButton.Top := WizardForm.BackButton.Top;
  SkipButton.Width := WizardForm.BackButton.Width;
  SkipButton.Height := WizardForm.BackButton.Height;
  SkipButton.Caption := ExpandConstant('{cm:ButtonSkip}');
  SkipButton.OnClick := @SkipButtonClick;
  SkipButton.Visible := False;

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

  // The tasks page is hidden in update mode, so its checkboxes would fall back
  // to their defaults (both ticked) and silently re-add a Desktop icon or an
  // autostart the user had removed. Mirror what is actually on the machine.
  // One call with the complete list: WizardSelectTasks deselects everything the
  // list does not name, so two calls would undo each other.
  if UpdateMode then begin
    tasks := '';
    if DesktopIconExists() then tasks := 'desktopicon';
    if LogonTaskExists() then begin
      if tasks <> '' then tasks := tasks + ',';
      tasks := tasks + 'autostart';
    end;
    WizardSelectTasks(tasks);
  end;
end;

// Update mode collapses the wizard to the Ready page: the destination, the
// shortcuts and Sunshine were all settled by the previous install, and re-asking
// is exactly what makes updates feel heavy. Only a question that was never
// answered still deserves a page.
function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if not UpdateMode then Exit;
  if (PageID = wpSelectDir) or (PageID = wpSelectProgramGroup) or (PageID = wpSelectTasks) then
    Result := True
  else if (SunshinePage <> nil) and (PageID = SunshinePage.ID) then
    // Nothing to install or pair on an update: the admin page owns pairing from
    // here on, and asking for credentials again would be pure noise.
    Result := True
  else if (InternetPage <> nil) and (PageID = InternetPage.ID) then
    Result := SettingsHasKey('internet_access_enabled');
end;

// Content of the Ready page's memo. In update mode it says what is about to
// happen in one sentence; otherwise it reproduces Inno's own composition (there
// is no way to fall through to the default once this function is defined).
function UpdateReadyMemo(const Space, NewLine, MemoUserInfoInfo, MemoDirInfo,
  MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
var
  s: String;
begin
  if UpdateMode then begin
    // Keep the argument array on the same line as the call: ISCC reads a line
    // whose first non-blank character is '[' as a section tag, even inside [Code].
    if PrevVersion <> '' then
      Result := FmtMessage(ExpandConstant('{cm:UpdateReadyMemo}'), [PrevVersion, '{#MyAppVersion}'])
    else
      Result := FmtMessage(ExpandConstant('{cm:UpdateReadyMemoFresh}'), ['{#MyAppVersion}']);
    Exit;
  end;

  s := '';
  if MemoUserInfoInfo <> '' then s := s + MemoUserInfoInfo + NewLine + NewLine;
  if MemoDirInfo <> '' then s := s + MemoDirInfo + NewLine + NewLine;
  if MemoTypeInfo <> '' then s := s + MemoTypeInfo + NewLine + NewLine;
  if MemoComponentsInfo <> '' then s := s + MemoComponentsInfo + NewLine + NewLine;
  if MemoGroupInfo <> '' then s := s + MemoGroupInfo + NewLine + NewLine;
  if MemoTasksInfo <> '' then s := s + MemoTasksInfo + NewLine + NewLine;
  Result := s;
end;

// Build the Sunshine page for the case detected on this machine. Runs once (the
// page is entered once — Back is hidden there) so a user who edited the fields
// and came back through another route never sees their input reset.
procedure PrepareSunshinePage();
var
  showFields: Boolean;
begin
  if SunshinePagePrepared then Exit;
  SunshinePagePrepared := True;

  SunshineDetected := DetectSunshine();
  if not SunshineDetected then SunshineCase := scAbsent
  else if LocalSunshinePaired() then SunshineCase := scPaired
  else SunshineCase := scUnpaired;

  showFields := SunshineCase <> scPaired;
  SunshineInstallCheck.Visible := showFields;
  SunshineUserLabel.Visible := showFields;
  SunshinePassLabel.Visible := showFields;
  SunshineUserEdit.Visible := showFields;
  SunshinePassEdit.Visible := showFields;

  case SunshineCase of
    scPaired:
      begin
        // Nothing to install, nothing to pair: a plain confirmation. The empty
        // credentials make CurStepChanged write auto_pair=false, and the message
        // gets the whole surface since no control sits under it.
        SunshineStatusLabel.Caption := ExpandConstant('{cm:SunshineAlreadyPaired}');
        SunshineStatusLabel.Height := SunshinePage.SurfaceHeight;
        SunshineUserEdit.Text := '';
        SetSunshinePassword('', True);
      end;
    scUnpaired:
      begin
        SunshineStatusLabel.Caption := ExpandConstant('{cm:SunshineDetected}');
        // Already installed: show the box ticked + "(already installed)" and keep
        // it disabled. PrepareToInstall skips the download when SunshineDetected,
        // so a ticked box here never triggers a reinstall.
        SunshineInstallCheck.Caption := ExpandConstant('{cm:SunshineInstallCheckDone}');
        SunshineInstallCheck.Checked := True;
        SunshineInstallCheck.Enabled := False;
        // Do NOT prefill: wrong (default) credentials make the REST PIN push fail,
        // leaving a pending pairing request and an unpaired host. The user types
        // Sunshine's real username/password to pair — or clicks Skip to pair later
        // (auto_pair is then written false; see CurStepChanged).
        SunshineUserEdit.Text := '';
        SetSunshinePassword('', True);
        UpdateSunshineFieldsEnabled();
      end;
  else
    begin
      SunshineStatusLabel.Caption := ExpandConstant('{cm:SunshineNotDetected}');
      SunshineInstallCheck.Caption := ExpandConstant('{cm:SunshineInstallCheck}');
      SunshineInstallCheck.Enabled := True;
      SunshineInstallCheck.Checked := True;
      // Fresh install: the silent installer applies these via --creds, so both
      // default to "admin". The password shows in clear until the user edits it
      // (SunshinePassChange) — it is a value to read, not a secret to hide, until
      // it becomes one.
      SunshineUserEdit.Text := 'admin';
      SetSunshinePassword('admin', False);
      UpdateSunshineFieldsEnabled();
    end;
  end;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  // In update mode every other page is skipped, so this IS the whole wizard: an
  // "Update" button instead of "Install" (the memo itself is built by
  // UpdateReadyMemo, the only supported way to rewrite that page's content).
  if (CurPageID = wpReady) and UpdateMode then begin
    WizardForm.PageNameLabel.Caption := ExpandConstant('{cm:UpdatePageCaption}');
    WizardForm.PageDescriptionLabel.Caption := ExpandConstant('{cm:UpdatePageDesc}');
    WizardForm.ReadyLabel.Caption := '';
    WizardForm.NextButton.Caption := ExpandConstant('{cm:ButtonUpdate}');
  end;

  if (InternetPage <> nil) and (CurPageID = InternetPage.ID) then begin
    // Defensive: nothing can currently re-enter this page, because the Sunshine
    // page after it hides Back as well. If that ever changes, a second visit
    // must not inherit the first visit's Passer.
    InternetSkipped := False;
    WizardForm.BackButton.Visible := False;
    BackButtonHidden := True;
    InternetSkipButton.Visible := True;
    // Nothing restores this on the way out, and nothing needs to: Inno resets
    // the Next caption on every page change before calling us — which is why
    // the Update caption above is re-applied here rather than set once.
    WizardForm.NextButton.Caption := ExpandConstant('{cm:InternetBtnAccept}');
  end else begin
    InternetSkipButton.Visible := False;
  end;

  if (SunshinePage <> nil) and (CurPageID = SunshinePage.ID) then begin
    PrepareSunshinePage();
    // Re-entering the page (Back from the "ready to install" page) cancels an
    // earlier Skip: whatever the user leaves in the fields now is what counts,
    // so a skipped fresh install gets its admin/admin defaults back.
    if SunshineSkipped then begin
      SunshineSkipped := False;
      if SunshineCase = scAbsent then begin
        SunshineUserEdit.Text := 'admin';
        SetSunshinePassword('admin', False);
      end;
    end;
    // Skip / Next / Cancel: the Back button gives up its slot to Skip. When
    // Sunshine is already paired there is nothing to skip, so the page keeps
    // just Next / Cancel.
    WizardForm.BackButton.Visible := False;
    BackButtonHidden := True;
    SkipButton.Visible := SunshineCase <> scPaired;
  end else begin
    SkipButton.Visible := False;
    // Only ever restore what we hid, and never on a page that is itself hiding
    // Back (the Internet page runs through this branch too).
    if BackButtonHidden and not ((InternetPage <> nil) and (CurPageID = InternetPage.ID)) then begin
      WizardForm.BackButton.Visible := True;
      BackButtonHidden := False;
    end;
  end;
end;

// Base64 of the UTF-8 bytes of `s`, for the probe's Basic-Auth header. Inno's
// strings are UTF-16 and its AnsiString conversions go through the system
// codepage, so the UTF-8 encoding is spelled out here (BMP only — a credential
// made of astral-plane characters is not a case worth carrying).
function Base64OfUtf8(const s: String): String;
var
  alphabet: String;
  bytes: array of Integer;
  i, n, c, acc: Integer;
begin
  alphabet := 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  SetArrayLength(bytes, Length(s) * 3);
  n := 0;
  for i := 1 to Length(s) do begin
    c := Ord(s[i]);
    if c < $80 then begin
      bytes[n] := c;
      n := n + 1;
    end else if c < $800 then begin
      bytes[n] := $C0 or (c shr 6);
      bytes[n + 1] := $80 or (c and $3F);
      n := n + 2;
    end else begin
      bytes[n] := $E0 or (c shr 12);
      bytes[n + 1] := $80 or ((c shr 6) and $3F);
      bytes[n + 2] := $80 or (c and $3F);
      n := n + 3;
    end;
  end;

  Result := '';
  i := 0;
  while i < n do begin
    acc := bytes[i] shl 16;
    if i + 1 < n then acc := acc or (bytes[i + 1] shl 8);
    if i + 2 < n then acc := acc or bytes[i + 2];
    Result := Result + alphabet[((acc shr 18) and 63) + 1] + alphabet[((acc shr 12) and 63) + 1];
    if i + 1 < n then Result := Result + alphabet[((acc shr 6) and 63) + 1]
    else Result := Result + '=';
    if i + 2 < n then Result := Result + alphabet[(acc and 63) + 1]
    else Result := Result + '=';
    i := i + 3;
  end;
end;

// Try the typed credentials against the Sunshine already installed here: a
// Basic-Auth GET on its web-UI port (47990). Returns False when the probe could
// not run at all (no MSXML) — the wizard then lets the user through rather than
// blocking on its own failure. Otherwise `httpStatus` is what Sunshine answered
// with, or 0 when it did not answer.
//
// Only 401/403 means "wrong credentials": any other answer proves the header got
// past the guard, so a Sunshine whose API differs is never mistaken for a bad
// password.
function ProbeSunshineCreds(const user, pass: String; var httpStatus: Integer): Boolean;
var
  req: Variant;
begin
  Result := False;
  httpStatus := 0;
  try
    req := CreateOleObject('MSXML2.ServerXMLHTTP.6.0');
  except
    Exit;
  end;
  Result := True;

  // The request below is synchronous — MSXML's asynchronous mode never leaves
  // readyState 1 inside Setup's apartment — so the wait indicator has to be on
  // screen BEFORE it starts. Update() forces the repaint that the blocked
  // message loop would otherwise only get around to once the answer is in.
  SunshineCheckLabel.Caption := ExpandConstant('{cm:SunshineCheckWait}');
  SunshineCheckLabel.Visible := True;
  WizardForm.NextButton.Enabled := False;
  SkipButton.Enabled := False;
  WizardForm.Cursor := crHourglass;
  WizardForm.Update;
  try
    try
      req.open('GET', 'https://127.0.0.1:47990/api/apps', False);
      // Loopback: a Sunshine that is up answers in milliseconds. These caps are
      // what turns "not running" into an error the user reads rather than a
      // wizard that hangs.
      req.setTimeouts(2000, 2000, 2000, 4000);
      // Sunshine self-signs its web UI: ignore every server-certificate error.
      req.setOption(2, 13056);
      // SXH_PROXY_SET_DIRECT — a configured proxy must never see loopback.
      req.setProxy(1);
      req.setRequestHeader('Authorization', 'Basic ' + Base64OfUtf8(user + ':' + pass));
      req.send('');
      httpStatus := req.status;
    except
      // Refused connection, TLS failure, timeout: nothing answered, and reading
      // .status would raise in turn. 0 is the "no answer" verdict.
      httpStatus := 0;
    end;
  finally
    WizardForm.Cursor := crDefault;
    SunshineCheckLabel.Visible := False;
    WizardForm.NextButton.Enabled := True;
    SkipButton.Enabled := True;
  end;
end;

// Credentials are only MANDATORY for a fresh auto-install: the silent installer
// sets Sunshine's username/password via --creds, so both must be provided (they
// come prefilled with admin/admin, so this only fires if the user empties them).
// When Sunshine is ALREADY installed the credentials only drive optional
// auto-pairing — Skip walks past the page and pairs later from the admin page.
// If Sunshine is absent and install is declined, the grayed-out fields are
// irrelevant.
function NextButtonClick(CurPageID: Integer): Boolean;
var
  user, pass: String;
  httpStatus: Integer;
begin
  Result := True;

  // The Internet page: "Accepter" is Next itself, "Passer" is Next reached
  // through InternetSkipClick — so the flag, not the button, is the answer.
  if (InternetPage <> nil) and (CurPageID = InternetPage.ID) then begin
    InternetAuthorized := not InternetSkipped;
    Exit;
  end;

  if (SunshinePage = nil) or (CurPageID <> SunshinePage.ID) or SunshineSkipped then Exit;

  // Probed and written exactly as typed: trimming here would validate one pair
  // of credentials and hand another to provisioning.json.
  user := SunshineUserEdit.Text;
  pass := SunshinePassEdit.Text;

  if (not SunshineDetected) and SunshineInstallCheck.Checked
     and ((Trim(user) = '') or (Trim(pass) = '')) then begin
    MsgBox(ExpandConstant('{cm:SunshineCredsRequired}'), mbError, MB_OK);
    Result := False;
    Exit;
  end;

  // Sunshine was already on this machine, so its credentials are the user's to
  // know — and a wrong pair fails silently much later (a pending pairing request
  // in Sunshine, an unpaired host). Settle it here, while the page is still up
  // and Skip is one click away. A fresh install is never probed: those
  // credentials are the ones the installer is about to create.
  if (SunshineCase = scUnpaired) and (Trim(user) <> '') and (Trim(pass) <> '') then begin
    if not ProbeSunshineCreds(user, pass, httpStatus) then Exit;
    if (httpStatus = 401) or (httpStatus = 403) then begin
      MsgBox(ExpandConstant('{cm:SunshineCredsWrong}'), mbError, MB_OK);
      Result := False;
    end else if httpStatus = 0 then begin
      MsgBox(ExpandConstant('{cm:SunshineNoAnswer}'), mbError, MB_OK);
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
  // disabled purely as an "already installed" indicator), when the user clicked
  // Skip, when they declined it, or on an update (the Sunshine page never ran).
  // Only a ticked box on a machine WITHOUT Sunshine triggers a real install.
  if (SunshinePage = nil) or UpdateMode or SunshineDetected or SunshineSkipped
     or (not SunshineInstallCheck.Checked) then begin
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

// --- One-click update from the web app: the elevated launcher --------------
//
// The server runs unprivileged, so launching an installer from it would raise a
// UAC consent dialog on the host's physical desktop — unanswerable when the
// update was triggered from a phone on the other side of the planet. This
// installer runs elevated, so it registers a trigger-less task with
// RunLevel=HighestAvailable: `schtasks /Run` on it later elevates silently.
//
// The task's action is a FIXED path under %LocalAppData% (expanded by Task
// Scheduler in the user's own context) that SelfUpdater downloads the new
// installer to. Per-user on purpose: a machine-wide staging directory would let
// any local account drop an executable that we then run elevated.
procedure RegisterUpdateTask();
var
  user, xml, xmlPath: String;
  rc: Integer;
begin
  user := TaskXmlEscape(GetEnv('USERDOMAIN') + '\' + GetEnv('USERNAME'));
  // No <?xml?> declaration and pure ASCII, for the same reason as the logon task.
  xml :=
    '<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">' + #13#10 +
    '  <RegistrationInfo><Author>MoonlightWeb</Author></RegistrationInfo>' + #13#10 +
    '  <Principals><Principal id="Author">' +
    '<UserId>' + user + '</UserId><LogonType>InteractiveToken</LogonType>' +
    '<RunLevel>HighestAvailable</RunLevel>' +
    '</Principal></Principals>' + #13#10 +
    // Element order follows the Task Scheduler schema (schtasks /XML is strict).
    '  <Settings>' +
    '<AllowStartOnDemand>true</AllowStartOnDemand>' +
    '<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>' +
    '<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>' +
    '<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>' +
    '<ExecutionTimeLimit>PT0S</ExecutionTimeLimit>' +
    '</Settings>' + #13#10 +
    // No <Triggers>: the task exists purely to be started on demand.
    '  <Actions Context="Author"><Exec>' +
    '<Command>' + UpdateStagedExe + '</Command>' +
    '<Arguments>' + UpdateSilentArgs + '</Arguments>' +
    '</Exec></Actions>' + #13#10 +
    '</Task>' + #13#10;
  xmlPath := ExpandConstant('{tmp}\mw-update-task.xml');
  if SaveStringToFile(xmlPath, xml, False) then
    Exec('schtasks.exe', '/Create /TN "' + UpdateTaskName + '" /XML "' + xmlPath + '" /F',
         '', SW_HIDE, ewWaitUntilTerminated, rc);
end;

// Release the files under {app} before the copy: a running server holds its exe
// and Qt DLLs open and Inno would fail (or defer to a reboot) on every one.
procedure StopRunningInstance();
var
  rc: Integer;
begin
  if ServiceInstalled then
    Exec(ExpandConstant('{sys}\net.exe'), 'stop MoonlightWeb', '', SW_HIDE,
         ewWaitUntilTerminated, rc);
  // End the logon task first so its supervisor cannot relaunch what we kill.
  Exec('schtasks.exe', '/End /TN "MoonlightWeb"', '', SW_HIDE, ewWaitUntilTerminated, rc);
  Exec('taskkill.exe', '/IM "{#MyAppExe}" /F', '', SW_HIDE, ewWaitUntilTerminated, rc);
  // taskkill returns before Windows has actually torn the process down.
  Sleep(2000);
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

// This step's contribution to the overall bar. A terminal step is finished
// whatever its outcome, so skipped/failed count as a full share — the bar must
// only ever move forward, and must be able to reach 100%.
function BarPercent(const state: String; pct: Integer): Integer;
begin
  if IsTerminal(state) then Result := 100 else Result := pct;
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
  statusPath, content, ps, psDisp, ar, spin: String;
  sunLabel, pairLabel: String;
  raw: AnsiString; // LoadStringFromFile requires an AnsiString out-param.
  i, rc: Integer;
  pctSun, pctPair, pctAr, itSun, itPair, itAr: Integer;
  prevSun, prevPair, prevAr: String;
  holdDone, alreadyPaired: Boolean;
begin
  // Bring the server back up. A service install owns its own lifecycle (session
  // 0, no tray) — restarting it is what StopRunningInstance stopped.
  if ServiceInstalled then
    Exec(ExpandConstant('{sys}\net.exe'), 'start MoonlightWeb', '', SW_HIDE,
         ewWaitUntilTerminated, rc)
  // On an update this setup is itself running elevated (started by the app
  // through the "MoonlightWeb Update" task), so Exec'ing the exe here would
  // leave the server running as administrator until the next logon. Going
  // through the logon task instead brings it back at its normal, least
  // privileged level.
  else if UpdateMode and LogonTaskExists() then
    Exec('schtasks.exe', '/Run /TN "MoonlightWeb"', '', SW_HIDE, ewWaitUntilTerminated, rc)
  else
    // Start the windowless tray server now so provisioning.json is consumed and
    // pairing + A-record run. ewNoWait: it keeps running after setup exits.
    // --autostart: an automatic launch must not open the browser itself — the
    // [Run] "open the admin page" checkbox owns that.
    Exec(ExpandConstant('{app}\{#MyAppExe}'), '--autostart', ExpandConstant('{app}'),
         SW_HIDE, ewNoWait, rc);

  // Silent installs have no UI to drive; an update has nothing to provision.
  // Either way the server just restarted with the settings it already had.
  if WizardSilent or UpdateMode then Exit;

  // Qt AppDataLocation on Windows: %AppData%\<Org>\<App> = MoonlightWeb\MoonlightWeb.
  statusPath := ExpandConstant('{userappdata}\MoonlightWeb\MoonlightWeb\provisioning.status.json');

  // A step the installer had nothing to do for must read as a statement of fact
  // ("Sunshine is already installed"), not as an instruction it never carried
  // out — the latter reads as a lie next to a 100% bar.
  sunLabel := ExpandConstant('{cm:TaskSunshine}');
  if SunshineDetected then sunLabel := ExpandConstant('{cm:TaskSunshineDone}');

  // scPaired: a previous MoonlightWeb already paired this machine, so the wizard
  // asked for no credentials and provisioning.json carries auto_pair=false — the
  // backend reports the step as "skipped". Note that wiping %AppData% does NOT
  // undo that pairing: the client identity and the host list are QSettings, i.e.
  // HKCU\Software\MoonlightWeb on Windows. The step really is complete, so show
  // it complete rather than with the "[--]" that means "not done".
  alreadyPaired := SunshinePagePrepared and (SunshineCase = scPaired);
  pairLabel := ExpandConstant('{cm:TaskPairing}');
  if alreadyPaired then pairLabel := ExpandConstant('{cm:TaskPairingDone}');

  ProgressPage.SetText(ExpandConstant('{cm:ProvisionWorking}'), '');
  ProgressPage.Show;
  itSun := 0; itPair := 0; itAr := 0;
  prevSun := ''; prevPair := ''; prevAr := '';
  try
    // ~3min budget (600 * 300ms). On a fresh install the internet step closes
    // in seconds (no DNS, no certificate); the budget is sized for a legacy
    // upgrade whose step still waits for the ACME certificate (up to 30s of DNS
    // propagation retries, then the order itself) — a 90s budget used to expire
    // mid-issuance and the wizard moved on while the domain was still served
    // with the self-signed fallback.
    for i := 0 to 600 do begin
      ps := ''; ar := '';
      if LoadStringFromFile(statusPath, raw) then begin
        content := raw; // implicit AnsiString -> String (Unicode Inno)
        ps := StatusValue(content, 'pairing');
        ar := StatusValue(content, 'arecord');
      end;
      spin := SpinChar(i);
      psDisp := ps;
      if alreadyPaired and (psDisp = 'skipped') then psDisp := 'done';

      // Pseudo-progress: climb towards 95 while a task runs, then snap to 100 on
      // done. Gives a long step visible movement instead of a spinner that looks
      // frozen. Each step climbs at its own rate, sized on how long it actually
      // takes, so none of them parks at 95% for the rest of the install:
      //   Sunshine  ~1%/300ms  (download + silent install)
      //   pairing   ~1%/600ms  (up to ~65s: PIN push + the 5-stage handshake)
      //   A-record  ~1%/1.5s   (DNS propagation, then the ACME order)
      if SunshineStepState = 'done' then pctSun := 100
      else if IsTerminal(SunshineStepState) then pctSun := 0
      else begin itSun := itSun + 1; pctSun := itSun; if pctSun > 95 then pctSun := 95; end;

      if psDisp = 'done' then pctPair := 100
      else if IsTerminal(psDisp) then pctPair := 0
      else begin itPair := itPair + 1; pctPair := itPair div 2; if pctPair > 95 then pctPair := 95; end;

      if ar = 'done' then pctAr := 100
      else if IsTerminal(ar) then pctAr := 0
      else begin itAr := itAr + 1; pctAr := itAr div 5; if pctAr > 95 then pctAr := 95; end;

      LblSunshine.Caption := StepGlyph(SunshineStepState, spin) + ' ' + PadRight(StepPercent(SunshineStepState, pctSun), 6) + sunLabel;
      LblPairing.Caption  := StepGlyph(psDisp, spin) + ' ' + PadRight(StepPercent(psDisp, pctPair), 6) + pairLabel;
      LblArecord.Caption  := StepGlyph(ar, spin) + ' ' + PadRight(StepPercent(ar, pctAr), 6) + ExpandConstant('{cm:TaskArecord}');

      // Smooth overall bar driven by the three pseudo-percentages (max 300). A
      // skipped/failed step counts as complete here (its label already says so):
      // it is finished, and scoring it 0 would both drag the bar backwards when
      // it resolves and stop it ever reaching 100%.
      ProgressPage.SetProgress(BarPercent(SunshineStepState, pctSun) + BarPercent(psDisp, pctPair)
                               + BarPercent(ar, pctAr), 300);

      // When a task has just turned [OK], pause 1s so the user registers it.
      holdDone := ((SunshineStepState = 'done') and (prevSun <> 'done'))
               or ((psDisp = 'done') and (prevPair <> 'done'))
               or ((ar = 'done') and (prevAr <> 'done'));
      prevSun := SunshineStepState; prevPair := psDisp; prevAr := ar;

      if IsTerminal(SunshineStepState) and IsTerminal(psDisp) and IsTerminal(ar) then begin
        // Everything settled. SetProgress above already pumped the message queue,
        // so the full bar and the final [OK] lines are on screen: hold them long
        // enough to be read instead of blinking straight to the next page.
        Sleep(900);
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
  // Before the file copy: nothing under {app} can be replaced while the server
  // is holding it open.
  if CurStep = ssInstall then begin
    ServiceInstalled := DetectService();
    // The FileExists arm covers a half-removed install (no uninstall entry, so
    // UpdateMode is False) whose exe is still there and possibly running. {app}
    // is initialized by now, which is why the test lives here and not in
    // PreviousInstallExists.
    if UpdateMode or ServiceInstalled
       or FileExists(ExpandConstant('{app}\{#MyAppExe}')) then StopRunningInstance();
    Exit;
  end;

  if CurStep <> ssPostInstall then Exit;

  // Register the elevated on-demand task that lets the (unprivileged) server
  // apply the next update by itself, with no UAC prompt. Refreshed on every
  // install so the action always matches the current staging convention.
  RegisterUpdateTask();

  // provisioning.json — consumed and removed by the server on first run.
  // An update skips it: the server is already provisioned, and replaying a
  // first-run provisioning would re-answer settled questions. The exception is a
  // question this build asks for the FIRST time — ShouldSkipPage let its page
  // through, so its answer still has to reach the server.
  if (not UpdateMode) or (not SettingsHasKey('internet_access_enabled')) then begin
    // Written as UTF-8 (no BOM): the consent text is localized (accents) and the
    // server parses this file with a strict UTF-8 JSON parser.
    if InternetAuthorized then internet := 'true' else internet := 'false';
    // Exact agreement text the user read on the Internet page — recorded by the
    // server in its DNS registration audit log (legal traceability).
    consent := ExpandConstant('{cm:InternetPageBody}') + ' / '
             + ExpandConstant('{cm:InternetPageOption}');
    StringChangeEx(consent, '%n', ' ', True);
    // Only ask the server to auto-pair when the user actually supplied credentials.
    // Blank creds mean "no pairing to do": the user clicked Skip, or MoonlightWeb
    // is already paired with the local Sunshine (scPaired clears both fields).
    // Pairing with empty creds would fail and leave a pending request, so mark it
    // skipped instead.
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
  end;

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

// --- Uninstall: optional removal of the user's configuration ---------------
//
// Everything MoonlightWeb persists lives OUTSIDE {app}, so a plain uninstall
// leaves it all behind on purpose — reinstalling then finds the machine exactly
// as the user left it (that is also what the one-click update relies on). This
// dialog is the opt-in that wipes it instead:
//   %AppData%\MoonlightWeb\MoonlightWeb  settings.json, sessions.json, the ACME
//                                        certificate, logs and crash dumps
//   HKCU\Software\MoonlightWeb           the QSettings hive: paired Sunshine
//                                        hosts and the Moonlight client identity
// Unchecked by default: losing the pairing certificate means re-pairing every
// host, which must never happen by accident.
function AskRemoveConfig(): Boolean;
var
  frm: TSetupForm;
  lbl, detail: TNewStaticText;
  chk: TNewCheckBox;
  btn: TNewButton;
begin
  Result := False;
  // A silent uninstall has nobody to ask — keep the configuration.
  if UninstallSilent then Exit;

  // Fixed size: nothing on this form can grow, so let it keep the size we give
  // it (last two arguments) instead of being resized around its controls.
  frm := CreateCustomForm(ScaleX(420), ScaleY(150), True, True);
  try
    frm.Caption := ExpandConstant('{cm:UninstConfigTitle}');

    lbl := TNewStaticText.Create(frm);
    lbl.Parent := frm;
    lbl.Left := ScaleX(12);
    lbl.Top := ScaleY(12);
    lbl.Width := frm.ClientWidth - ScaleX(24);
    lbl.AutoSize := False;
    lbl.Height := ScaleY(36);
    lbl.WordWrap := True;
    lbl.Caption := ExpandConstant('{cm:UninstConfigBody}');

    // TNewCheckBox cannot wrap its caption, so the box gets a short label and
    // the consequences go in the static line below it.
    chk := TNewCheckBox.Create(frm);
    chk.Parent := frm;
    chk.Left := ScaleX(12);
    chk.Top := ScaleY(56);
    chk.Width := frm.ClientWidth - ScaleX(24);
    chk.Height := ScaleY(17);
    chk.Checked := False;
    chk.Caption := ExpandConstant('{cm:UninstConfigOption}');

    detail := TNewStaticText.Create(frm);
    detail.Parent := frm;
    // Indented under the checkbox caption, past the box itself.
    detail.Left := ScaleX(28);
    detail.Top := ScaleY(78);
    detail.Width := frm.ClientWidth - ScaleX(40);
    detail.AutoSize := False;
    detail.Height := ScaleY(30);
    detail.WordWrap := True;
    detail.Caption := ExpandConstant('{cm:UninstConfigDetail}');

    btn := TNewButton.Create(frm);
    btn.Parent := frm;
    btn.Height := ScaleY(23);
    btn.Caption := SetupMessage(msgButtonOK);
    // Localized captions differ in length; size the button for the one in use.
    btn.Width := frm.CalculateButtonWidth([btn.Caption]);
    btn.Left := frm.ClientWidth - btn.Width - ScaleX(12);
    btn.Top := frm.ClientHeight - ScaleY(23 + 12);
    btn.ModalResult := mrOk;
    btn.Default := True;

    // Focus the button, not the checkbox: Enter/Space on an unfocused checkbox
    // cannot tick it by accident, and the safe answer is one keypress away.
    frm.ActiveControl := btn;
    frm.FlipAndCenterIfNeeded(True, UninstallProgressForm, False);
    // Closing the window (mrCancel) means "keep it" — same as leaving the box
    // unticked. The uninstall itself is already confirmed and goes ahead.
    if frm.ShowModal() = mrOk then Result := chk.Checked;
  finally
    frm.Free();
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  rc: Integer;
  removeConfig: Boolean;
begin
  if CurUninstallStep = usUninstall then begin
    // Asked before anything is torn down, so the wizard is not left idling on a
    // half-removed install while the dialog waits for an answer.
    removeConfig := AskRemoveConfig();

    // Stop the running server first: remove the logon task (so it cannot be
    // relaunched), end any task-started instance, then force-kill the tray
    // process. Otherwise MoonlightWeb.exe keeps running and locks {app} files.
    Exec('schtasks.exe', '/End /TN "MoonlightWeb"', '', SW_HIDE,
         ewWaitUntilTerminated, rc);
    Exec('schtasks.exe', '/Delete /TN "MoonlightWeb" /F', '', SW_HIDE,
         ewWaitUntilTerminated, rc);
    // The elevated on-demand update launcher goes too — leaving it behind would
    // keep an elevated task pointing at a path the user can still write to.
    Exec('schtasks.exe', '/Delete /TN "' + UpdateTaskName + '" /F', '', SW_HIDE,
         ewWaitUntilTerminated, rc);
    DelTree(ExpandConstant('{localappdata}\MoonlightWeb\update'), True, True, True);
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

    // Opt-in wipe. Runs after the server has been killed above: its lock file,
    // logs and settings are only closed once the process is really gone.
    if removeConfig then begin
      DelTree(ExpandConstant('{userappdata}\MoonlightWeb\MoonlightWeb'), True, True, True);
      // Leave the parent MoonlightWeb\ behind only if something else lives in it.
      RemoveDir(ExpandConstant('{userappdata}\MoonlightWeb'));
      // QSettings' hive: hosts\<n>\pairState, the client certificate/key, window
      // state. Org key and app subkey go together.
      RegDeleteKeyIncludingSubkeys(HKCU, 'Software\MoonlightWeb');
    end;
  end;
end;
