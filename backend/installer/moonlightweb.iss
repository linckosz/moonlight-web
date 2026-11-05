; ===========================================================================
;  MoonlightWeb — interactive Windows installer (Inno Setup 6).
;
;  Produces a stepped wizard (MoonlightWeb-installer-<version>-win-<arch>.exe) that:
;    1. installs the app + Start-Menu shortcuts (app, admin page, uninstaller),
;    2. asks the user to authorize the Internet link (no DNS record, no
;       certificate, no open web port — see InternetPageBody),
;    3. installs the ViGEmBus gamepad driver silently, best-effort,
;    4. drops a provisioning.json the server consumes on first run (enable
;       Internet Access),
;    5. creates a Desktop shortcut to the admin page and opens it at the end.
;
;  It does NOT install Sunshine. Since September 2026 the app captures and
;  encodes this machine itself (the native host, run in the console session by
;  the service — see docs/design/native-capture-encoder.md §15), so a Windows
;  install needs no streaming server beside it. Sunshine remains fully supported
;  as a HOST: a machine that already runs it is discovered and paired from the
;  hosts page like any other, and SunshineInstaller stays for macOS/Linux, where
;  the native host does not exist yet.
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
; ViGEmBus — the virtual gamepad bus driver the native host needs to present a
; controller to Windows. One asset covers x64, x86 and ARM64. Installed silently
; and best-effort: a machine without it streams fine, just without a gamepad, so
; nothing here may ever fail the installation.
#ifndef VigemBusUrl
  #define VigemBusUrl "https://github.com/nefarius/ViGEmBus/releases/download/v1.22.0/ViGEmBus_1.22.0_x64_x86_arm64.exe"
#endif

; Edition. /DDevEdition builds the DEV installer (release.yml, every run that
; is not a v* tag): MoonlightWebDev, a pre-release that installs BESIDE the
; production app instead of over it. Everything Windows knows an install by is
; derived from MyAppName below — the AppId (hence the uninstall entry), the
; folder, the exe, the service, the scheduled tasks, the firewall rule, the
; shortcuts and the per-user state under %AppData%\MoonlightWeb\<name> — so the
; two editions never share one of them. See backend/src/common/Edition.h.
#ifdef DevEdition
  #define MyAppName "MoonlightWebDev"
  #define MyAppId "{5B1E7A8C-8D0E-4F53-8D6F-E1373C70AFE6}"
  #define MyIconFile "..\..\frontend\assets\dev\favicon.ico"
  ; The DEV edition listens on the DEV ports (Edition.h), clear of 443.
  #define MyHttpsSuffix ":48443"
#else
  #define MyAppName "MoonlightWeb"
  #define MyAppId "{6F2C9E4A-7B3D-4E5F-9A1C-2D8E4B6F0A33}"
  #define MyIconFile "..\..\frontend\assets\favicon.ico"
  #define MyHttpsSuffix ""
#endif
#define MyAppExe MyAppName + ".exe"
; Provisional admin URL written before first launch. The server rewrites this
; Desktop shortcut on startup with the real HTTPS port / public domain.
#ifndef AdminUrl
  #define AdminUrl "https://localhost" + MyHttpsSuffix + "/admin"
#endif

[Setup]
; "{{" is Inno's escape for a literal brace.
AppId={#StringChange(MyAppId, "{", "{{")}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=MoonlightWeb
AppPublisherURL=https://github.com/linckosz/moonlight-web
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputBaseFilename={#MyAppName}-installer-{#MyAppVersion}-win-{#MyArch}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; Taller than the default, for one page: the Internet consent is four paragraphs
; and its label is AutoSize, so at the default height the last one (the sentence
; saying it can be turned off again) would be pushed off the surface in the
; longer translations. Height only — the width is what makes the wizard look
; like every other Windows installer, and nothing here needs more of it.
WizardSizePercent=100,115
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
SetupIconFile={#MyIconFile}
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
; InternetPageBody keeps the literal product name in both editions: it is the
; consent the app's own setup wizard shows word for word, and the text recorded
; as agreed to (frontend/test/InternetConsentText.test.js holds the two together).
; --- English ---
en.AutoStartTask=Start {#MyAppName} at logon
en.InternetPageCaption=Internet Link
en.InternetPageDesc=Allow access from the Internet?
en.InternetPageBody=MoonlightWeb can allow streaming from outside your local network, in a highly secure way.%n%nStreaming is direct, from this PC to the browser you invited. While a session runs, your router is asked (UPnP) to open a single port, closed again when the session ends. It carries nothing but the encrypted stream, and every connection on it has to authenticate first.%n%nA rendezvous server introduces the two sides, so nothing about this PC is published. No public DNS record, no certificate for this machine, and ports 80/443 stay closed. Your public IP address is listed nowhere: only that server and whoever holds the link you sent ever see it. To learn its own public address, this PC asks a MoonlightWeb STUN server, or a public one (Google, Cloudflare) as a fallback.%n%nYou can turn this off at any time from the Admin page.
en.InternetPageOption=Allow the Internet link (recommended)
en.InternetBtnSkip=&Skip
en.InternetBtnAccept=&Accept
en.VDisplayPageCaption=Virtual Display
en.VDisplayPageDesc=Install "MoonlightWeb Virtual Display"?
en.VDisplayPageBody=MoonlightWeb can give this PC a screen of its own to stream: "MoonlightWeb Virtual Display", a virtual monitor that appears as a card on the hosts page.%n%nIt is off by default. Opening its card turns it on, makes it the primary display and streams it; when the last stream ends it is turned off again and your previous primary display gets its role back. A PC with no monitor attached — installed through a TV, then left in a cupboard — streams through it.%n%nThe driver is the open-source Virtual Display Driver (MIT, signed), installed silently and in addition to any virtual display you already have. Only uninstalling MoonlightWeb removes it.
en.VDisplayPageOption=Install "MoonlightWeb Virtual Display" (recommended)
en.RunApp=Launch {#MyAppName}
en.RunAdmin=Open the admin page
en.ProvisionPageCaption=Setting up {#MyAppName}
en.ProvisionPageDesc=Finalizing the installation
en.ProvisionWorking=Please wait while {#MyAppName} finishes setting up...
en.ResolveLinkWait=Preparing the link to the admin page...
en.TaskArecord=Enable the Internet link
en.ButtonUpdate=&Update
en.UpdatePageCaption=Update {#MyAppName}
en.UpdatePageDesc=A newer version will replace the installed one
en.UpdateReadyMemo={#MyAppName} %1 is installed and will be updated to %2.%n%nYour settings, the Internet link and your paired hosts are kept as they are. There is nothing to configure.
en.UpdateReadyMemoFresh={#MyAppName} will be updated to %1.%n%nYour settings, the Internet link and your paired hosts are kept as they are. There is nothing to configure.
en.UninstConfigTitle=Uninstall {#MyAppName}
en.UninstConfigBody=Your configuration is kept by default: if you install {#MyAppName} again, it starts up exactly as you left it.
en.UninstConfigOption=Also delete my configuration
en.UninstConfigDetail=Settings, accounts, certificates and host pairings are erased for good.
; --- French ---
fr.AutoStartTask=Démarrer {#MyAppName} à l'ouverture de session
fr.InternetPageCaption=Lien Internet
fr.InternetPageDesc=Autoriser l'accès depuis Internet ?
fr.InternetPageBody=MoonlightWeb peut autoriser le streaming depuis l'extérieur de votre réseau local, de façon hautement sécurisée.%n%nLe streaming est direct, de ce PC vers le navigateur que vous avez invité. Pendant une session, votre box se voit demander (UPnP) l'ouverture d'un seul port, refermé à la fin de la session. Il ne transporte rien d'autre que le flux chiffré, et toute connexion dessus doit d'abord s'authentifier.%n%nUn serveur de rendez-vous met les deux côtés en relation : rien de ce PC n'est publié. Aucun enregistrement DNS public, aucun certificat pour cette machine, et les ports 80/443 restent fermés. Votre adresse IP publique n'est listée nulle part : seuls ce serveur et la personne à qui vous avez donné le lien la voient. Pour connaître sa propre adresse publique, ce PC interroge un serveur STUN MoonlightWeb, ou un serveur public (Google, Cloudflare) en secours.%n%nDésactivable à tout moment depuis la page admin.
fr.InternetPageOption=Autoriser le lien Internet (recommandé)
fr.InternetBtnSkip=&Passer
fr.InternetBtnAccept=&Accepter
fr.VDisplayPageCaption=Écran virtuel
fr.VDisplayPageDesc=Installer « MoonlightWeb Virtual Display » ?
fr.VDisplayPageBody=MoonlightWeb peut donner à ce PC un écran à lui, à diffuser : « MoonlightWeb Virtual Display », un moniteur virtuel qui apparaît comme une carte sur la page des hôtes.%n%nIl est éteint par défaut. Ouvrir sa carte l'allume, en fait l'écran principal et le diffuse ; à la fin du dernier stream il s'éteint et votre écran principal précédent retrouve son rôle. Un PC sans moniteur — installé depuis une TV, puis rangé dans un placard — diffuse grâce à lui.%n%nLe pilote est le Virtual Display Driver open source (MIT, signé), installé silencieusement et en plus de tout écran virtuel déjà présent. Seule la désinstallation de MoonlightWeb le retire.
fr.VDisplayPageOption=Installer « MoonlightWeb Virtual Display » (recommandé)
fr.RunApp=Lancer {#MyAppName}
fr.RunAdmin=Ouvrir la page admin
fr.ProvisionPageCaption=Configuration de {#MyAppName}
fr.ProvisionPageDesc=Finalisation de l'installation
fr.ProvisionWorking=Veuillez patienter pendant la fin de la configuration de {#MyAppName}...
fr.ResolveLinkWait=Préparation du lien vers la page admin...
fr.TaskArecord=Activer le lien Internet
fr.ButtonUpdate=&Mettre à jour
fr.UpdatePageCaption=Mise à jour de {#MyAppName}
fr.UpdatePageDesc=Une version plus récente va remplacer celle installée
fr.UpdateReadyMemo={#MyAppName} %1 est installé et va être mis à jour vers %2.%n%nVos réglages, le lien Internet et vos hôtes appairés sont conservés tels quels. Il n'y a rien à configurer.
fr.UpdateReadyMemoFresh={#MyAppName} va être mis à jour vers %1.%n%nVos réglages, le lien Internet et vos hôtes appairés sont conservés tels quels. Il n'y a rien à configurer.
fr.UninstConfigTitle=Désinstallation de {#MyAppName}
fr.UninstConfigBody=Votre configuration est conservée par défaut : si vous réinstallez {#MyAppName}, il redémarrera exactement dans l'état où vous l'avez laissé.
fr.UninstConfigOption=Supprimer aussi ma configuration
fr.UninstConfigDetail=Les réglages, comptes, certificats et appairages d'hôtes seront définitivement effacés.
; --- Simplified Chinese ---
zh.AutoStartTask=登录时启动 {#MyAppName}
zh.InternetPageCaption=互联网链接
zh.InternetPageDesc=是否允许从互联网访问？
zh.InternetPageBody=MoonlightWeb 可以以高度安全的方式，允许从本地网络之外进行串流。%n%n串流是点对点直连的，从这台电脑直接到您邀请的浏览器。会话进行期间，会通过 UPnP 请求路由器只开放一个端口，会话结束后即被关闭。该端口只承载加密后的串流，任何连接都必须先通过身份验证。%n%n由一台会合服务器为双方牵线，这台电脑无需公开任何信息。不会创建公开 DNS 记录，不会为这台机器签发证书，端口 80/443 保持关闭。您的公网 IP 地址不会被列在任何地方：只有该服务器以及持有您所发送链接的人才能看到。为了得知自己的公网地址，这台电脑会向 MoonlightWeb STUN 服务器查询，无法连接时改用公共服务器（Google、Cloudflare）。%n%n可随时在管理页面关闭。
zh.InternetPageOption=允许互联网链接（推荐）
zh.InternetBtnSkip=跳过(&S)
zh.InternetBtnAccept=接受(&A)
zh.VDisplayPageCaption=虚拟显示器
zh.VDisplayPageDesc=是否安装“MoonlightWeb Virtual Display”？
zh.VDisplayPageBody=MoonlightWeb 可以为这台电脑提供一个专属的串流屏幕：“MoonlightWeb Virtual Display”，一个在主机页面上显示为卡片的虚拟显示器。%n%n它默认处于关闭状态。打开其卡片即可开启它、将其设为主显示器并进行串流；最后一个串流结束后它会再次关闭，您之前的主显示器恢复原来的角色。没有连接显示器的电脑——通过电视安装后放进柜子里——就靠它来串流。%n%n驱动程序是开源的 Virtual Display Driver（MIT 许可，已签名），静默安装，且与您已有的任何虚拟显示器并存。只有卸载 MoonlightWeb 才会将其移除。
zh.VDisplayPageOption=安装“MoonlightWeb Virtual Display”（推荐）
zh.RunApp=启动 {#MyAppName}
zh.RunAdmin=打开管理页面
zh.ProvisionPageCaption=正在设置 {#MyAppName}
zh.ProvisionPageDesc=正在完成安装
zh.ProvisionWorking=请稍候，{#MyAppName} 正在完成设置...
zh.ResolveLinkWait=正在准备通往管理页面的链接...
zh.TaskArecord=启用互联网链接
zh.ButtonUpdate=更新(&U)
zh.UpdatePageCaption=更新 {#MyAppName}
zh.UpdatePageDesc=较新的版本将替换已安装的版本
zh.UpdateReadyMemo=已安装 {#MyAppName} %1，将更新到 %2。%n%n您的设置、互联网链接和已配对的主机将保持不变，无需任何配置。
zh.UpdateReadyMemoFresh={#MyAppName} 将更新到 %1。%n%n您的设置、互联网链接和已配对的主机将保持不变，无需任何配置。
zh.UninstConfigTitle=卸载 {#MyAppName}
zh.UninstConfigBody=默认保留您的配置：如果您再次安装 {#MyAppName}，它将完全按照您离开时的状态启动。
zh.UninstConfigOption=同时删除我的配置
zh.UninstConfigDetail=设置、账户、证书和主机配对将被永久删除。

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
; No "autostart" task here: start-at-logon is a checkbox on the Finished page
; (see AutostartCheck in [Code]), ticked by default and still within reach at
; the very last click — and, on an update, showing what the machine already does.

#if MyArch == "arm64"
[InstallDelete]
; The x64 virtual display driver an earlier ARM64 installer laid down: this
; one bundles none (see the Virtual Display page in [Code]).
Type: filesandordirs; Name: "{app}\drivers\vdd"

#endif
[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
; Start-Menu group + Desktop: a single "{#MyAppName}" entry, a .lnk shortcut that
; LAUNCHES THE EXE (not a URL). The windowless app starts when it is down or, when
; already running, surfaces the admin page via its single-instance logic — so one
; click always lands on the admin page, launching the app first if needed. The
; uninstaller entry lives in the group too.
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"; WorkingDir: "{app}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"; WorkingDir: "{app}"; Tasks: desktopicon
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"

[Run]
; The tray server is already launched during the provisioning checklist (see
; RunProvisionChecklist in [Code]); only offer to open the admin page here.
; GetAdminUrl hands out what ResolveAdminUrl settled while the server was coming
; up: the rendezvous address when the internet link is on and answering, the
; published loopback URL (real HTTPS port) otherwise.
Filename: "{code:GetAdminUrl}"; Description: "{cm:RunAdmin}"; Flags: shellexec postinstall skipifsilent

[Code]
const
  // Inno files its uninstall entry under "<AppId>_is1" and exposes no constant
  // for it to [Code]; MyAppId is the one source for both.
  UninstallKey = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{#MyAppId}_is1';
  // Trigger-less, elevated task the running (unprivileged) app starts with
  // `schtasks /Run` to apply an update without a UAC prompt nobody could answer
  // from a remote browser. Its action is our exe under {app} with
  // --update-apply, which verifies what SelfUpdater staged before running it.
  UpdateTaskName = '{#MyAppName} Update';
  UpdateApplyArgs = '--update-apply';
  // Trigger-less, elevated task the running (unprivileged) app starts to add
  // or remove a virtual display on a headless machine — a driver install that
  // would otherwise raise a UAC prompt on a desktop with no monitor. Its
  // action is OUR exe under {app} with a fixed argument: the request it acts
  // on is a parameters-only file under %LocalAppData% (VirtualDisplay.h).
  VDisplayTaskName = '{#MyAppName} Virtual Display';
  VDisplayArgs = '--vdisplay-apply';
  // Trigger-less, elevated task the running (unprivileged) app starts for each
  // native stream, so that its worker can type into windows that run as
  // administrator (UIPI shuts an unelevated one out of them, and out of every
  // window while one has the focus). $(Arg0) is the name of the pipes the
  // worker connects back to; the app passes it and checks who connected.
  WorkerTaskName = '{#MyAppName} Stream Worker';
  WorkerArgs = '--stream-worker --worker-pipe $(Arg0)';
  // Where this installer remembers the answer to its own Virtual Display
  // page, so an update does not ask again (the device node itself is the
  // truth about what is installed; the helper never creates a second one).
  VDisplaySetupKey = 'SOFTWARE\{#MyAppName}\Setup';
  // Ctrl+Alt+Suppr from the stream (design §31.7): Windows takes SendSAS from
  // the launcher service only under this machine policy — 1 lets services
  // press it, 3 services and Ease of Access. Set when nothing has set it. Each
  // edition relying on a value one of our installers set is named under
  // SasOwnersKey, and the value goes when the last of them is uninstalled.
  SasPolicyKey = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System';
  SasPolicyValue = 'SoftwareSASGeneration';
  SasOwnersKey = 'SOFTWARE\MoonlightWeb\SoftwareSASGeneration';

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
  // The Virtual Display page: the same two buttons (the Skip button is
  // shared, its handler swapped per page) and the same flag pair.
  VDisplayPage: TWizardPage;
  VDisplayBodyLabel: TNewStaticText;
  VDisplayOptionLabel: TNewStaticText;
  VDisplaySkipped: Boolean;
  VDisplayAccepted: Boolean;
  BackButtonHidden: Boolean;
  // Live post-install checklist. One row since September 2026: the app captures
  // and encodes this machine itself, so there is no streaming server to install
  // and no local host to pair — only the Internet link is provisioned here.
  ProgressPage: TOutputProgressWizardPage;
  LblArecord: TNewStaticText;
  // Update mode: a MoonlightWeb is already installed, so this run only swaps the
  // payload. Every question was answered by the previous install — the wizard
  // collapses to a single "Update" confirmation (see ShouldSkipPage).
  UpdateMode: Boolean;
  PrevVersion: String;
  // The server runs as an NSSM service (session 0): stop/start it around the file
  // copy instead of killing and relaunching a tray process.
  ServiceInstalled: Boolean;
  // Where the Finish page's "open the admin page" leads, settled by
  // ResolveAdminUrl at the end of the post-install step and read back by
  // GetAdminUrl when the [Run] entry fires.
  ResolvedAdminUrl: String;
  // "Start MoonlightWeb at logon" on the Finished page, above the "open the
  // admin page" row. Ticked by default on a first install; on an update it
  // shows whether the logon task is there right now — the tray menu and the
  // admin page drive that same task, so the previous install's box is not the
  // last word on it. Applied when Finish is clicked (NextButtonClick), or from
  // its default when there is no Finished page to click through (silent).
  AutostartCheck: TNewCheckBox;
  WantAutostart: Boolean;

// Not a Pascal Script builtin — it has to be declared as an import, same as
// any other Win32 API call, or the compiler stops with "Unknown identifier
// 'GetTickCount'".
function GetTickCount: Cardinal;
  external 'GetTickCount@kernel32.dll stdcall';

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
  Result := Exec(ExpandConstant('{sys}\sc.exe'), 'query {#MyAppName}', '', SW_HIDE,
                 ewWaitUntilTerminated, rc) and (rc = 0);
end;

// True when the logon task created by a previous install is still registered.
function LogonTaskExists(): Boolean;
var
  rc: Integer;
begin
  Result := Exec('schtasks.exe', '/Query /TN "{#MyAppName}"', '', SW_HIDE,
                 ewWaitUntilTerminated, rc) and (rc = 0);
end;

// What the user last ASKED FOR, which is not the same question as what the
// machine currently has.
//
// An update used to read the logon task alone and mirror it, which is right
// when the task is missing because somebody unticked the box, and wrong when it
// is missing because registering it failed. It failed for everyone who
// installed over a network logon before the TaskPrincipal fix: USERDOMAIN is
// WORKGROUP there, schtasks maps it to no SID, and the task was never created.
// Mirroring that state means the fix ships and those machines stay broken,
// because the update faithfully preserves the damage.
//
// So the answer is recorded, every time it is applied, and an update prefers
// the record over the symptom. Returns False when nothing was ever recorded —
// every install from before this build.
function AutostartChoiceRecorded(var want: Boolean): Boolean;
var
  v: Cardinal;
begin
  Result := RegQueryDWordValue(HKEY_LOCAL_MACHINE, VDisplaySetupKey,
                               'AutostartWanted', v);
  if Result then want := v <> 0;
end;

// The policy Ctrl+Alt+Suppr needs (see SasPolicyKey). Left alone when anyone
// else set it — an administrator, a group policy, another remote desktop — to
// whatever value: their answer stands, and nothing of ours ever removes it.
procedure ClaimSasPolicy();
var
  v: Cardinal;
begin
  if not RegQueryDWordValue(HKEY_LOCAL_MACHINE, SasPolicyKey, SasPolicyValue, v) then begin
    if RegWriteDWordValue(HKEY_LOCAL_MACHINE, SasPolicyKey, SasPolicyValue, 1) then
      RegWriteDWordValue(HKEY_LOCAL_MACHINE, SasOwnersKey, '{#MyAppName}', 1);
  end else if RegKeyExists(HKEY_LOCAL_MACHINE, SasOwnersKey) then
    // One of our installers set it, for the other edition: this one relies on
    // it too.
    RegWriteDWordValue(HKEY_LOCAL_MACHINE, SasOwnersKey, '{#MyAppName}', 1);
end;

// Uninstall: the value goes only if one of our installers set it, no other
// edition still relies on it, and it still says what we wrote.
procedure ReleaseSasPolicy();
var
  names: TArrayOfString;
  v: Cardinal;
begin
  if not RegKeyExists(HKEY_LOCAL_MACHINE, SasOwnersKey) then Exit;
  RegDeleteValue(HKEY_LOCAL_MACHINE, SasOwnersKey, '{#MyAppName}');
  if RegGetValueNames(HKEY_LOCAL_MACHINE, SasOwnersKey, names) and
     (GetArrayLength(names) > 0) then Exit;
  RegDeleteKeyIncludingSubkeys(HKEY_LOCAL_MACHINE, SasOwnersKey);
  RegDeleteKeyIfEmpty(HKEY_LOCAL_MACHINE, 'SOFTWARE\MoonlightWeb');
  if RegQueryDWordValue(HKEY_LOCAL_MACHINE, SasPolicyKey, SasPolicyValue, v) and (v = 1) then
    RegDeleteValue(HKEY_LOCAL_MACHINE, SasPolicyKey, SasPolicyValue);
end;

// True when a Desktop shortcut created by a previous install is still there.
function DesktopIconExists(): Boolean;
begin
  Result := FileExists(ExpandConstant('{autodesktop}\{#MyAppName}.lnk'))
         or FileExists(ExpandConstant('{userdesktop}\{#MyAppName}.lnk'));
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

procedure VDisplaySkipClick(Sender: TObject);
begin
  VDisplaySkipped := True;
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
       ExpandConstant('{userappdata}\MoonlightWeb\{#MyAppName}\settings.json'), raw) then
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
       ExpandConstant('{userappdata}\MoonlightWeb\{#MyAppName}\settings.json'), raw) then
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
  // guessed here. AutoSize does not mean it always fits: the page surface does
  // not scroll, so when the wording grew it pushed the green consent sentence
  // below the button row and out of sight. The four paragraphs were tightened
  // on 2026-09-12 to fit again, with about three lines to spare in French, the
  // longest of the three languages. Measure before adding a sentence here: see
  // WizardSizePercent in [Setup] for the vertical room available.
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
  // with the affirmative one on the right.
  InternetSkipButton := TNewButton.Create(WizardForm);
  InternetSkipButton.Parent := WizardForm.BackButton.Parent;
  InternetSkipButton.Left := WizardForm.BackButton.Left;
  InternetSkipButton.Top := WizardForm.BackButton.Top;
  InternetSkipButton.Width := WizardForm.BackButton.Width;
  InternetSkipButton.Height := WizardForm.BackButton.Height;
  InternetSkipButton.Caption := ExpandConstant('{cm:InternetBtnSkip}');
  InternetSkipButton.OnClick := @InternetSkipClick;
  InternetSkipButton.Visible := False;

  // Step: "MoonlightWeb Virtual Display". Same shape as the Internet page:
  // a question with two answers and no default, asked once. Not on ARM64:
  // the driver bundled is x64 only, and Windows on ARM takes a driver package
  // signed by Microsoft only (0x800b0109, 20/10/2026), so there is nothing to
  // offer — VDisplayPage stays nil and every use of it below checks.
#if MyArch != "arm64"
  VDisplayPage := CreateCustomPage(InternetPage.ID,
    ExpandConstant('{cm:VDisplayPageCaption}'), ExpandConstant('{cm:VDisplayPageDesc}'));

  VDisplayBodyLabel := TNewStaticText.Create(WizardForm);
  VDisplayBodyLabel.Parent := VDisplayPage.Surface;
  VDisplayBodyLabel.Left := 0;
  VDisplayBodyLabel.Top := 0;
  VDisplayBodyLabel.Width := VDisplayPage.SurfaceWidth;
  VDisplayBodyLabel.WordWrap := True;
  VDisplayBodyLabel.AutoSize := True;
  VDisplayBodyLabel.Caption := ExpandConstant('{cm:VDisplayPageBody}');

  VDisplayOptionLabel := TNewStaticText.Create(WizardForm);
  VDisplayOptionLabel.Parent := VDisplayPage.Surface;
  VDisplayOptionLabel.Left := 0;
  VDisplayOptionLabel.Top := VDisplayBodyLabel.Top + VDisplayBodyLabel.Height + ScaleY(12);
  VDisplayOptionLabel.Width := VDisplayPage.SurfaceWidth;
  VDisplayOptionLabel.WordWrap := True;
  VDisplayOptionLabel.Font.Style := [fsBold];
  VDisplayOptionLabel.Font.Color := $43A02E;
  VDisplayOptionLabel.Caption := ExpandConstant('{cm:VDisplayPageOption}');
#endif

  // Live checklist shown during post-install (driven in RunProvisionChecklist).
  ProgressPage := CreateOutputProgressPage(
    ExpandConstant('{cm:ProvisionPageCaption}'), ExpandConstant('{cm:ProvisionPageDesc}'));

  // Header (Msg1Label, fed via SetText): let it wrap so the full sentence shows
  // instead of being clipped, and reserve vertical space above the checklist.
  ProgressPage.Msg1Label.AutoSize := False;
  ProgressPage.Msg1Label.WordWrap := True;
  ProgressPage.Msg1Label.Width := ProgressPage.SurfaceWidth;
  ProgressPage.Msg1Label.Top := ScaleY(0);
  ProgressPage.Msg1Label.Height := ScaleY(34);

  LblArecord := TNewStaticText.Create(WizardForm);
  LblArecord.Parent := ProgressPage.Surface;
  LblArecord.Top := ScaleY(44);
  LblArecord.Width := ProgressPage.SurfaceWidth;
  LblArecord.Font.Name := 'Consolas';
  LblArecord.Font.Size := 10;

  // Progress bar below the checklist (not overlapping it), with the secondary
  // message under the bar. Without this the bar sits on top of the label.
  ProgressPage.ProgressBar.Top := ScaleY(76);
  ProgressPage.ProgressBar.Width := ProgressPage.SurfaceWidth;
  ProgressPage.Msg2Label.Top := ScaleY(102);

  // The tasks page is hidden in update mode, so its checkbox would fall back to
  // its default and silently re-add a Desktop icon the user had removed. Mirror
  // what is actually on the machine. (WizardSelectTasks deselects everything the
  // list does not name — one call with the complete list.)
  if UpdateMode then begin
    tasks := '';
    if DesktopIconExists() then tasks := 'desktopicon';
    WizardSelectTasks(tasks);
  end;

  // Start at logon: on by default. Placed on the Finished page in
  // CurPageChanged, once Inno has laid that page out (RunList's position is
  // only known then).
  // On an update: the task if it is there (unambiguous), else the recorded
  // answer, else on. That last default is the repair — an install old enough to
  // have no record, whose task is missing, is overwhelmingly one the WORKGROUP
  // bug silently robbed, and turning it back on is what the user asked for the
  // day they installed. Someone who had genuinely turned it off gets it once,
  // on a page where the box is visible and unticking it is now remembered.
  if UpdateMode then begin
    if LogonTaskExists() then WantAutostart := True
    else if not AutostartChoiceRecorded(WantAutostart) then WantAutostart := True;
  end
  else WantAutostart := True;
  AutostartCheck := TNewCheckBox.Create(WizardForm);
  AutostartCheck.Parent := WizardForm.FinishedPage;
  AutostartCheck.Left := WizardForm.FinishedLabel.Left;
  AutostartCheck.Width := WizardForm.FinishedLabel.Width;
  AutostartCheck.Height := ScaleY(17);
  AutostartCheck.Caption := ExpandConstant('{cm:AutoStartTask}');
  AutostartCheck.Checked := WantAutostart;
end;

// Update mode collapses the wizard to the Ready page: the destination and the
// shortcuts were settled by the previous install, and re-asking is exactly what
// makes updates feel heavy. Only a question that was never answered still
// deserves a page.
function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if not UpdateMode then Exit;
  if (PageID = wpSelectDir) or (PageID = wpSelectProgramGroup) or (PageID = wpSelectTasks) then
    Result := True
  else if (InternetPage <> nil) and (PageID = InternetPage.ID) then
    Result := SettingsHasKey('internet_access_enabled')
  else if (VDisplayPage <> nil) and (PageID = VDisplayPage.ID) then
    // A silent walk reads as "Accepter", and an update from the web app is
    // always silent: a page that was skipped stays skipped rather than be
    // answered by nobody. A silent first install does install it.
    Result := WizardSilent or
              RegValueExists(HKEY_LOCAL_MACHINE, VDisplaySetupKey, 'VirtualDisplayAccepted');
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

  // Finished page: the start-at-logon box goes right above the "open the admin
  // page" row, which moves down to make room. That row is Inno's RunList; it is
  // laid out just before this hook runs, which is why the box is placed here
  // rather than when it was created. With no RunList (nothing to run, a silent
  // install never gets here at all), the box sits under the closing text.
  if CurPageID = wpFinished then begin
    if WizardForm.RunList.Visible then begin
      // RunList draws its check glyph Offset pixels in from its own left edge;
      // match it so both boxes stand in one column.
      AutostartCheck.Left := WizardForm.RunList.Left + WizardForm.RunList.Offset;
      AutostartCheck.Width := WizardForm.RunList.Width - WizardForm.RunList.Offset;
      AutostartCheck.Top := WizardForm.RunList.Top;
      WizardForm.RunList.Top := AutostartCheck.Top + AutostartCheck.Height + ScaleY(8);
    end else
      AutostartCheck.Top := WizardForm.FinishedLabel.Top + WizardForm.FinishedLabel.Height + ScaleY(16);
  end;

  if (InternetPage <> nil) and (CurPageID = InternetPage.ID) then begin
    // Defensive: a second visit must not inherit the first visit's Passer.
    InternetSkipped := False;
    WizardForm.BackButton.Visible := False;
    BackButtonHidden := True;
    InternetSkipButton.Visible := True;
    // Nothing restores this on the way out, and nothing needs to: Inno resets
    // the Next caption on every page change before calling us — which is why
    // the Update caption above is re-applied here rather than set once.
    WizardForm.NextButton.Caption := ExpandConstant('{cm:InternetBtnAccept}');
  end else if (VDisplayPage <> nil) and (CurPageID = VDisplayPage.ID) then begin
    VDisplaySkipped := False;
    WizardForm.BackButton.Visible := False;
    BackButtonHidden := True;
    InternetSkipButton.OnClick := @VDisplaySkipClick;
    InternetSkipButton.Visible := True;
    WizardForm.NextButton.Caption := ExpandConstant('{cm:InternetBtnAccept}');
  end else begin
    InternetSkipButton.Visible := False;
    InternetSkipButton.OnClick := @InternetSkipClick;
    // Only ever restore what we hid. The Internet page is the one page that
    // hides Back, and it takes the branch above rather than this one.
    if BackButtonHidden then begin
      WizardForm.BackButton.Visible := True;
      BackButtonHidden := False;
    end;
  end;
end;

// Defined with the task helpers further down; declared here because the Finish
// click below is the one place it is called from before that.
procedure ApplyAutostartChoice(want: Boolean); forward;

// The Internet answer of a silent install: only an explicit /INTERNET=yes (or
// on, true, 1) on the command line opens it. Anything else keeps it closed,
// and the log says why.
function SilentInternetWanted(): Boolean;
var
  value: String;
begin
  value := Lowercase(Trim(ExpandConstant('{param:INTERNET|}')));
  Result := (value = 'yes') or (value = 'on') or (value = 'true') or (value = '1');
  if Result then
    Log('Internet: opened by /INTERNET=' + value + ' (silent install)')
  else if value = '' then
    Log('Internet: kept closed (silent install, no /INTERNET=yes)')
  else
    Log('Internet: kept closed (silent install, /INTERNET=' + value + ' is not yes)');
end;

// The Internet page is the only one that answers a question, and it answers it
// through this hook: "Accepter" is Next itself, "Passer" is Next reached through
// InternetSkipClick — so the flag, not the button, is the answer.
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (InternetPage <> nil) and (CurPageID = InternetPage.ID) then
    InternetAuthorized := not InternetSkipped;
  if (VDisplayPage <> nil) and (CurPageID = VDisplayPage.ID) then
    VDisplayAccepted := not VDisplaySkipped;
  // Finish: the last click is what the start-at-logon box means. Done before
  // Inno runs the [Run] rows, so the admin page it opens next already sees the
  // task the way the box left it.
  if CurPageID = wpFinished then
    ApplyAutostartChoice(AutostartCheck.Checked);
end;

// --- ViGEmBus: the virtual gamepad bus ------------------------------------
//
// Keyboard and mouse can be injected with a plain API call; a gamepad cannot.
// Games do not read input events, they enumerate HID devices, so a real device
// has to exist and only a kernel driver can create one.
//
// Everything here is best-effort by design. A failure to download, a failure to
// install, an offline machine — none of them may block the installation, and
// none of them is even reported to the user: what they lose is controller
// support on a product whose main use is a desktop. It is retried on the next
// update, since the check runs every time.

function VigemBusInstalled(): Boolean;
begin
  // The driver registers a service under its own name. Cheaper and more
  // reliable than hunting for the device interface, and true as soon as the
  // driver is staged even if no pad has ever been plugged in.
  Result := RegKeyExists(HKEY_LOCAL_MACHINE, 'SYSTEM\CurrentControlSet\Services\ViGEmBus');
end;

procedure InstallVigemBus();
var
  tmp: String;
  rc: Integer;
begin
  if VigemBusInstalled() then Exit;

  // Swallowed on purpose: no network, a moved asset, a proxy in the way. None
  // of those is worth a word to someone installing a screen-sharing tool.
  try
    DownloadTemporaryFile('{#VigemBusUrl}', 'vigembus-setup.exe', '', nil);
  except
    Exit;
  end;

  tmp := ExpandConstant('{tmp}\vigembus-setup.exe');
  if not FileExists(tmp) then Exit;

  // The setup is a WiX bundle: /quiet /norestart is its unattended form. A
  // driver install can legitimately want a reboot; we never ask for one, and
  // the pad simply appears after the user's next restart.
  Exec(tmp, '/quiet /norestart', '', SW_HIDE, ewWaitUntilTerminated, rc);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  // The gamepad driver is wanted on an update as much as on a fresh install,
  // and it is the only thing this installer fetches from the network.
  InstallVigemBus();
end;

// --- Visual C++ runtime ----------------------------------------------------
//
// MoonlightWeb.exe links against Microsoft's C++ runtime (VCRUNTIME140.dll,
// MSVCP140.dll, MSVCP140_1.dll). windeployqt puts Microsoft's own
// vc_redist.<arch>.exe in the payload but installs nothing, and on a Windows
// that never received the runtime from anything else — a fresh install, a VM —
// the exe dies at load with "VCRUNTIME140.dll was not found": the server, and
// every helper this installer runs through it (worker service, virtual
// display). Seen on a fresh Windows 11 in October 2026; every bench machine
// already had the runtime, and so does the CI runner whose smoke test starts
// the exe.
//
// The redistributable's file version is the runtime version it installs, and
// it records that version under Runtimes\<arch>. Installed only when missing
// or older than ours: an exe built with a newer toolset than the runtime it
// finds can still load and then fail, so "present" alone is not enough.

function VCRuntimeAtLeast(Wanted: Int64): Boolean;
var
  installed, major, minor, bld: Cardinal;
  key: String;
begin
  Result := False;
  key := 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\{#MyArch}';
  if not RegQueryDWordValue(HKLM64, key, 'Installed', installed) or (installed <> 1) then Exit;
  if not RegQueryDWordValue(HKLM64, key, 'Major', major) then Exit;
  if not RegQueryDWordValue(HKLM64, key, 'Minor', minor) then Exit;
  if not RegQueryDWordValue(HKLM64, key, 'Bld', bld) then Exit;
  Result := ComparePackedVersion(PackVersionComponents(major, minor, bld, 0), Wanted) >= 0;
end;

procedure InstallVCRuntime();
var
  redist: String;
  wanted: Int64;
  rc: Integer;
begin
  redist := ExpandConstant('{app}\vc_redist.{#MyArch}.exe');
  if not FileExists(redist) then Exit;
  if not GetPackedVersion(redist, wanted) then wanted := 0;
  if VCRuntimeAtLeast(wanted) then Exit;
  // /norestart: the DLLs are usable at once on a machine that had none, which
  // is the case this is for. Exit codes (3010 = reboot wanted, 1638 = newer
  // already there) change nothing for us, so none is reported.
  Exec(redist, '/install /quiet /norestart', '', SW_HIDE, ewWaitUntilTerminated, rc);
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

// Who the scheduled tasks run as, written the way schtasks can resolve it.
//
// USERDOMAIN alone is not that. An interactive logon on a machine that is not
// domain-joined sets it to the computer name, which resolves; a NETWORK logon —
// SSH, WinRM, an RMM or MDM agent, anything that deploys a fleet without a
// human at the keyboard — sets it to "WORKGROUP", and "WORKGROUP\someone" maps
// to no SID at all. schtasks then fails with "No mapping between account names
// and security IDs was done", the installer has no UI to say so, and the
// machine ends up installed, green, and with no autostart, no update task and
// no virtual display task. A domain account keeps its domain: that one is a
// real authority.
function TaskPrincipal(): String;
var
  domain: String;
begin
  domain := GetEnv('USERDOMAIN');
  if (domain = '') or (Uppercase(domain) = 'WORKGROUP') then
    domain := GetEnv('COMPUTERNAME');
  if domain = '' then
    Result := TaskXmlEscape(GetEnv('USERNAME'))
  else
    Result := TaskXmlEscape(domain + '\' + GetEnv('USERNAME'));
end;

procedure RegisterLogonTask();
var
  user, xml, xmlPath, exePath: String;
  rc: Integer;
begin
  user := TaskPrincipal();
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
    Exec('schtasks.exe', '/Create /TN "{#MyAppName}" /XML "' + xmlPath + '" /F',
         '', SW_HIDE, ewWaitUntilTerminated, rc);
end;

// The start-at-logon box, made true on the machine: register the task (over
// any existing one, so an update also refreshes its exe path) or drop it. The
// same task the app's own switch drives afterwards (tray menu, admin page).
// Dropping it leaves a running instance alone: /Delete unregisters, /End is
// what would kill it, and the user asked about the next logon, not this one.
procedure ApplyAutostartChoice(want: Boolean);
var
  rc: Integer;
begin
  if want then
    RegisterLogonTask()
  else if LogonTaskExists() then
    Exec('schtasks.exe', '/Delete /TN "{#MyAppName}" /F', '', SW_HIDE,
         ewWaitUntilTerminated, rc);
  // Recorded AFTER the attempt and regardless of whether it succeeded: this is
  // the intent, and the next update needs it precisely in the case where
  // registering failed. HKLM, like the rest of this installer's own state —
  // it is written elevated and read elevated.
  if want then
    RegWriteDWordValue(HKEY_LOCAL_MACHINE, VDisplaySetupKey, 'AutostartWanted', 1)
  else
    RegWriteDWordValue(HKEY_LOCAL_MACHINE, VDisplaySetupKey, 'AutostartWanted', 0);
end;

// --- One-click update from the web app: the elevated launcher --------------
//
// The server runs unprivileged, so launching an installer from it would raise a
// UAC consent dialog on the host's physical desktop — unanswerable when the
// update was triggered from a phone on the other side of the planet. This
// installer runs elevated, so it registers a trigger-less task with
// RunLevel=HighestAvailable: `schtasks /Run` on it later elevates silently.
//
// The task's action is our exe under {app} (admin-writable only), never the
// installer SelfUpdater downloads: that one sits under %LocalAppData%, which
// the user's own unelevated programs can write — any of them could drop an
// executable there and start the task. --update-apply copies the download to
// {app}\update-staging (Administrators and SYSTEM only), checks it against
// GitHub's sha256 for the latest release (and against our signer, when this
// build is signed) and only then runs it (src/network/UpdateApply.h).
procedure RegisterUpdateTask();
var
  user, xml, xmlPath, exePath: String;
  rc: Integer;
begin
  user := TaskPrincipal();
  exePath := TaskXmlEscape(ExpandConstant('{app}\{#MyAppExe}'));
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
    '<Command>"' + exePath + '"</Command>' +
    '<Arguments>' + UpdateApplyArgs + '</Arguments>' +
    '</Exec></Actions>' + #13#10 +
    '</Task>' + #13#10;
  xmlPath := ExpandConstant('{tmp}\mw-update-task.xml');
  if SaveStringToFile(xmlPath, xml, False) then
    Exec('schtasks.exe', '/Create /TN "' + UpdateTaskName + '" /XML "' + xmlPath + '" /F',
         '', SW_HIDE, ewWaitUntilTerminated, rc);
end;

// --- Virtual display: the elevated helper task -----------------------------
//
// Same mechanism as the update task (trigger-less, RunLevel=HighestAvailable,
// started on demand by the unprivileged server), for the driver install that
// turning "MoonlightWeb Virtual Display" on and off needs. Unlike the update task the
// command is a fixed path under {app} — admin-writable only — so nothing the
// user's account can write is ever what runs elevated. Hidden: it runs on a
// desktop that may have no monitor, and it never has anything to show.
procedure RegisterVirtualDisplayTask();
var
  user, xml, xmlPath, exePath: String;
  rc: Integer;
begin
  user := TaskPrincipal();
  exePath := TaskXmlEscape(ExpandConstant('{app}\{#MyAppExe}'));
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
    '<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>' +
    '<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>' +
    '<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>' +
    '<AllowStartOnDemand>true</AllowStartOnDemand>' +
    '<Hidden>true</Hidden>' +
    '<ExecutionTimeLimit>PT0S</ExecutionTimeLimit>' +
    '</Settings>' + #13#10 +
    // No <Triggers>: the task exists purely to be started on demand.
    '  <Actions Context="Author"><Exec><Command>"' + exePath + '"</Command>' +
    '<Arguments>' + VDisplayArgs + '</Arguments></Exec></Actions>' + #13#10 +
    '</Task>' + #13#10;
  xmlPath := ExpandConstant('{tmp}\mw-vdisplay-task.xml');
  if SaveStringToFile(xmlPath, xml, False) then
    Exec('schtasks.exe', '/Create /TN "' + VDisplayTaskName + '" /XML "' + xmlPath + '" /F',
         '', SW_HIDE, ewWaitUntilTerminated, rc);
end;

// --- Native stream worker: the elevated launcher --------------------------
//
// Same mechanism again, for the process that injects a remote viewer's keyboard
// and mouse. Only the worker is raised — the server listening on the network,
// the tray and the browser it opens stay at the user's own level. The command
// is our exe under {app} (admin-writable only); the argument is a pipe name,
// and the worker refuses any pipe not served by this same exe. Parallel: one
// worker per native session. Hidden, like the display helper.
procedure RegisterStreamWorkerTask();
var
  user, xml, xmlPath, exePath: String;
  rc: Integer;
begin
  user := TaskPrincipal();
  exePath := TaskXmlEscape(ExpandConstant('{app}\{#MyAppExe}'));
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
    '<MultipleInstancesPolicy>Parallel</MultipleInstancesPolicy>' +
    '<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>' +
    '<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>' +
    '<AllowStartOnDemand>true</AllowStartOnDemand>' +
    '<Hidden>true</Hidden>' +
    '<ExecutionTimeLimit>PT0S</ExecutionTimeLimit>' +
    '<Priority>4</Priority>' +
    '</Settings>' + #13#10 +
    // No <Triggers>: the task exists purely to be started on demand.
    '  <Actions Context="Author"><Exec><Command>"' + exePath + '"</Command>' +
    '<Arguments>' + WorkerArgs + '</Arguments></Exec></Actions>' + #13#10 +
    '</Task>' + #13#10;
  xmlPath := ExpandConstant('{tmp}\mw-worker-task.xml');
  if SaveStringToFile(xmlPath, xml, False) then
    Exec('schtasks.exe', '/Create /TN "' + WorkerTaskName + '" /XML "' + xmlPath + '" /F',
         '', SW_HIDE, ewWaitUntilTerminated, rc);
end;

// --- The SYSTEM worker launcher service ------------------------------------
//
// Level 2 of the elevated-worker story: a LocalSystem service whose only job is
// to start the stream worker on the console desktop with a SYSTEM token, so a
// remote viewer can answer a UAC prompt, unlock the screen and press
// Ctrl+Alt+Suppr — the secure desktop, which the elevated task above cannot
// reach. The exe registers and removes itself (WorkerService.cpp); the
// installer only has to run it elevated, which it is.
//
// Refreshed on every install so the service's image follows {app}.
procedure RegisterWorkerService();
var
  rc: Integer;
begin
  Exec(ExpandConstant('{app}\{#MyAppExe}'), '--worker-service-install', '', SW_HIDE,
       ewWaitUntilTerminated, rc);
end;

procedure UnregisterWorkerService();
var
  rc: Integer;
begin
  Exec(ExpandConstant('{app}\{#MyAppExe}'), '--worker-service-remove', '', SW_HIDE,
       ewWaitUntilTerminated, rc);
end;

// Run the app's own elevated helper for one verb on "MoonlightWeb Virtual
// Display": `install` creates the driver's device node from the files under
// {app}\drivers\vdd, named, disabled, and creates none when one is there;
// `uninstall` removes it (and the driver package, unless another node still
// uses it). The request is a file holding the verb and nothing else; the
// helper re-hashes the driver against the constants compiled into it before
// SetupAPI sees a byte. Exit 0 or 3010 (reboot wanted) both mean done.
procedure RunVirtualDisplayHelper(verb: String);
var
  dir: String;
  rc: Integer;
begin
#if MyArch == "arm64"
  // No driver on ARM64 (see the page above): install and uninstall alike
  // have nothing to run.
  Log(Format('virtual display helper (%s): skipped, no driver on ARM64', [verb]));
  Exit;
#endif
  dir := ExpandConstant('{tmp}\vdisplay');
  ForceDirectories(dir);
  if not SaveStringToFile(dir + '\request.json', '{"action":"' + verb + '"}', False) then Exit;
  Exec(ExpandConstant('{app}\{#MyAppExe}'),
       VDisplayArgs + ' --stage=driver --vdisplay-dir "' + dir + '"',
       ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, rc);
  Log(Format('virtual display helper (%s): exit %d', [verb, rc]));
end;

// "MoonlightWeb Virtual HID", the HID passthrough's driver: the app's elevated
// helper again. `install` re-hashes the package under {app}\drivers\vhid
// against the constants compiled into the exe, then creates or updates the
// driver's node; `uninstall` removes it once no MoonlightWeb edition that
// installed it is left. Best-effort like ViGEmBus: without the driver the
// passthrough is simply unavailable, so nothing here may fail the setup. The
// package is signed by MoonlightWeb's own certificate, which the helper trusts
// for the install alone (Root and Trusted Publishers, out of both again
// before it returns). A build that bundles no package (Windows on ARM: it
// takes a driver signed by Microsoft only, 0x800b0109 on 20/10/2026) skips it
// entirely.
// Through it the stream also gives a host with no pointing device a mouse of
// its own (SystemMouse.h), and one with no keyboard a keyboard of its own for
// gaming mode (SystemKeyboard.h). Exit 3010 asks for no restart: a game
// still held a device made through the old driver, which stays loaded until
// Windows next restarts, and the app works with either. An update that brings
// the same driver installs nothing, so it never gets there.
procedure RunVirtualHidHelper(verb: String);
var
  rc: Integer;
begin
  Exec(ExpandConstant('{app}\{#MyAppExe}'), '--vhid-apply ' + verb,
       ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, rc);
  Log(Format('virtual HID helper (%s): exit %d', [verb, rc]));
end;

// Release the files under {app} before the copy: a running server holds its exe
// and Qt DLLs open and Inno would fail (or defer to a reboot) on every one.
procedure StopRunningInstance();
var
  rc: Integer;
begin
  if ServiceInstalled then
    Exec(ExpandConstant('{sys}\net.exe'), 'stop {#MyAppName}', '', SW_HIDE,
         ewWaitUntilTerminated, rc);
  // End the logon task first so its supervisor cannot relaunch what we kill.
  Exec('schtasks.exe', '/End /TN "{#MyAppName}"', '', SW_HIDE, ewWaitUntilTerminated, rc);
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
       'advfirewall firewall delete rule name="{#MyAppName}"',
       '', SW_HIDE, ewWaitUntilTerminated, rc);
  Exec(ExpandConstant('{sys}\netsh.exe'),
       'advfirewall firewall add rule name="{#MyAppName}" dir=in action=allow'
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

// The loopback admin URL: the one the server wrote into provisioning.status.json
// at startup (it knows the real HTTPS port, which may be a fallback), falling
// back to the provisional compile-time default if the server never started.
function LoopbackAdminUrl(): String;
var
  raw: AnsiString;
  url: String;
begin
  Result := '{#AdminUrl}';
  if LoadStringFromFile(
       ExpandConstant('{userappdata}\MoonlightWeb\{#MyAppName}\provisioning.status.json'), raw) then
  begin
    url := StatusValue(raw, 'admin_url');
    if url <> '' then Result := url;
  end;
end;

// Origin to talk to the local server on, derived from the URL it published:
// same port, but 127.0.0.1 rather than the "localhost" that URL names. The
// server only answers this route to a local caller, and 127.0.0.1 is the one
// spelling of "local" that cannot come out as ::1 on the way.
function LoopbackOrigin(const adminUrl: String): String;
var
  rest: String;
  i: Integer;
begin
  Result := 'https://127.0.0.1';
  rest := adminUrl;
  i := Pos('://', rest);
  if i > 0 then rest := Copy(rest, i + 3, Length(rest));
  // Host[:port], without the path.
  i := Pos('/', rest);
  if i > 0 then rest := Copy(rest, 1, i - 1);
  i := Pos(':', rest);
  if i > 0 then Result := Result + Copy(rest, i, Length(rest));
end;

// Ask the running server for the internet way into its own admin page: the
// rendezvous address, with a single-use host key in the fragment.
//
// Loopback, self-signed certificate, no proxy. Empty on anything unexpected, so
// the caller falls back. `answered` says whether the server replied at all (it
// may still be restarting), `pending` whether its empty url only means "not yet".
//
// The question can only be put to the server, because only the server knows
// whether the line is up AND whether the introduction server answers when asked
// — it probes that itself. An empty url without `pending` is its considered
// answer, not a failure: it means loopback is the honest address here.
function AskRemoteAdminLink(const origin: String; var answered, pending: Boolean): String;
var
  req: Variant;
  body: String;
begin
  Result := '';
  answered := False;
  pending := False;
  try
    req := CreateOleObject('MSXML2.ServerXMLHTTP.6.0');
  except
    Exit;
  end;
  try
    req.open('GET', origin + '/api/server/remote-link?p=/admin', False);
    // Loopback: an answer takes milliseconds. The server may still be probing
    // the entry page though, so allow the receive a little room.
    req.setTimeouts(2000, 2000, 2000, 6000);
    // The loopback listener serves MoonlightWeb's own self-signed certificate.
    req.setOption(2, 13056);
    // SXH_PROXY_SET_DIRECT — a configured proxy must never see loopback.
    req.setProxy(1);
    req.send('');
    if req.status <> 200 then Exit;
    body := req.responseText;
  except
    Exit;
  end;
  answered := True;
  // Compact JSON: {"pending":false,"url":"https://.../xxxx#k=...&p=/admin"}.
  // A server older than `pending` never sends it, and is read as settled.
  pending := Pos('"pending":true', body) > 0;
  Result := StatusValue(body, 'url');
end;

// Decide, once, where the Finish page's "open the admin page" leads.
//
// Loopback is always true from this machine, and it is what this handed out
// unconditionally until now — including to the user who had just accepted the
// internet link on the page before, which read as the installer not believing
// its own question. Through the rendezvous the same page arrives under a
// certificate the browser already trusts, so there is no interstitial to accept
// first; loopback is served by the self-signed one. The server picks between
// them, and answers with loopback whenever the internet way in is off, not up,
// or not answering.
//
// Asked HERE rather than at the click on Finish, and this is the whole reason
// the answer is cached in a variable: the line needs a few seconds to come up
// after the server starts, and those are seconds this page is already spending.
// A wizard that freezes on its last button is worse than one that waits where
// it is already waiting.
//
// How long: until the server has a verdict, not a fixed budget. It used to be
// six seconds, and that is why an update landed on loopback: on an update
// nothing is provisioned, so this starts the instant the server is relaunched,
// and a Snapdragon 7c needs about eleven seconds from there to line up and
// check the entry page (13/09/2026). A plain empty url is a verdict and ends
// the wait. While the server is still restarting (no answer) or says `pending`,
// the wait goes on, up to a ceiling; the server itself stops saying `pending`
// twenty seconds after its start. Then loopback, silently.
procedure ResolveAdminUrl();
var
  origin, link: String;
  answered, pending: Boolean;
  // Cardinal, like GetTickCount itself: the difference then stays correct
  // across the 49-day wrap instead of going through a signed overflow.
  started, elapsed: Cardinal;
begin
  ResolvedAdminUrl := LoopbackAdminUrl();
  // Nothing to prefer, or nobody to show it to.
  if WizardSilent then Exit;
  if not (InternetAuthorized or SettingsBoolEnabled('internet_access_enabled')) then Exit;

  origin := LoopbackOrigin(ResolvedAdminUrl);
  ProgressPage.SetText(ExpandConstant('{cm:ResolveLinkWait}'), '');
  // The bar is full from the checklist above, and this wait has no measurable
  // progress to show: it ends the moment the server has a verdict, or at the
  // ceiling. So the bar simply stays full. Neither a refilling bar
  // (100% dropping to a slowly refilling 50% read as the install going
  // backwards) nor a marquee (switching style recreates the control, which
  // restarts empty with its block at the left edge: a flash of 0%). The
  // SetProgress calls below keep it at 100% and pump the message queue.
  ProgressPage.Show;
  ProgressPage.SetProgress(100, 100);
  try
    // Bounded on the CLOCK, not on a number of attempts: a server that has
    // wedged answers each attempt with a timeout rather than a refusal, and
    // fifteen of those would hold the wizard for a minute and a half.
    started := GetTickCount;
    repeat
      link := AskRemoteAdminLink(origin, answered, pending);
      if link <> '' then begin
        ResolvedAdminUrl := link;
        Break;
      end;
      elapsed := GetTickCount - started;
      // SetProgress pumps the message queue, which is what keeps the page
      // painted across the Sleep below.
      ProgressPage.SetProgress(100, 100);
      // The server just installed is the one answering, so it knows `pending`:
      // an empty url without it is settled.
      if answered and not pending then Break;
      if elapsed >= 30000 then Break;
      // Still refusing after fifteen seconds: it is not coming back up.
      if not answered and (elapsed >= 15000) then Break;
      Sleep(400);
    until False;
  finally
    ProgressPage.Hide;
  end;
end;

// Filename of the [Run] entry. ResolveAdminUrl has already settled this; the
// fallback covers the paths that never call it (a silent install, an aborted
// post-install), where loopback is the only answer anyway.
function GetAdminUrl(Param: String): String;
begin
  if ResolvedAdminUrl <> '' then Result := ResolvedAdminUrl
  else Result := LoopbackAdminUrl();
end;

// Launch the server (kicks off first-run provisioning) and poll its status file,
// driving the on-screen checklist until the task is terminal or it times out.
procedure RunProvisionChecklist();
var
  statusPath, content, ar, spin: String;
  raw: AnsiString; // LoadStringFromFile requires an AnsiString out-param.
  i, j, rc: Integer;
  pctAr, itAr: Integer;
  prevAr: String;
  holdDone: Boolean;
begin
  // Bring the server back up. A service install owns its own lifecycle (session
  // 0, no tray) — restarting it is what StopRunningInstance stopped.
  if ServiceInstalled then
    Exec(ExpandConstant('{sys}\net.exe'), 'start {#MyAppName}', '', SW_HIDE,
         ewWaitUntilTerminated, rc)
  // On an update this setup is itself running elevated (started by the app
  // through the "MoonlightWeb Update" task), so Exec'ing the exe here would
  // leave the server running as administrator until the next logon. Going
  // through the logon task instead brings it back at its normal, least
  // privileged level.
  else if UpdateMode and LogonTaskExists() then
    Exec('schtasks.exe', '/Run /TN "{#MyAppName}"', '', SW_HIDE, ewWaitUntilTerminated, rc)
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

  // Qt AppDataLocation on Windows: %AppData%\<Org>\<App> = MoonlightWeb\<MyAppName>.
  statusPath := ExpandConstant('{userappdata}\MoonlightWeb\{#MyAppName}\provisioning.status.json');

  ProgressPage.SetText(ExpandConstant('{cm:ProvisionWorking}'), '');
  ProgressPage.Show;
  itAr := 0;
  prevAr := '';
  try
    // ~3min budget (600 * 300ms). On a fresh install the internet step closes
    // in seconds (no DNS, no certificate); the budget is sized for a legacy
    // upgrade whose step still waits for the ACME certificate (up to 30s of DNS
    // propagation retries, then the order itself) — a 90s budget used to expire
    // mid-issuance and the wizard moved on while the domain was still served
    // with the self-signed fallback.
    for i := 0 to 600 do begin
      ar := '';
      if LoadStringFromFile(statusPath, raw) then begin
        content := raw; // implicit AnsiString -> String (Unicode Inno)
        ar := StatusValue(content, 'arecord');
      end;
      spin := SpinChar(i);

      // Pseudo-progress: climb towards 95 while the task runs, then snap to 100
      // on done. Gives the step visible movement instead of a spinner that
      // looks frozen. The curve is 95·n/(n+7) over 300 ms ticks: fast at first
      // (40% after 1.5 s, 56% after 3 s), then ever slower, so it neither parks
      // at 95% nor crawls. The old linear 1%/1.5 s was sized on DNS propagation
      // plus an ACME order; since the rendezvous era the step closes in a few
      // seconds, and the bar barely left 1% before snapping to 100.
      if ar = 'done' then pctAr := 100
      else if IsTerminal(ar) then pctAr := 0
      else begin itAr := itAr + 1; pctAr := (95 * itAr) div (itAr + 7); end;

      LblArecord.Caption := StepGlyph(ar, spin) + ' ' + PadRight(StepPercent(ar, pctAr), 6) + ExpandConstant('{cm:TaskArecord}');

      // A skipped/failed step counts as complete on the bar (its label already
      // says so): it is finished, and scoring it 0 would stop the bar ever
      // reaching 100%.
      ProgressPage.SetProgress(BarPercent(ar, pctAr), 100);

      // When the task has just turned [OK], pause 1s so the user registers it.
      holdDone := (ar = 'done') and (prevAr <> 'done');
      prevAr := ar;

      if IsTerminal(ar) then begin
        // Settled: hold the full bar and the final [OK] line long enough to be
        // read instead of blinking straight to the next page. Held in short
        // slices that each pump the message queue, not one Sleep: a themed
        // progress bar animates its fill on timer messages, and a single
        // Sleep(900) froze it around 70% under a label already reading 100%.
        for j := 1 to 9 do begin
          ProgressPage.SetProgress(BarPercent(ar, pctAr), 100);
          Sleep(100);
        end;
        Break;
      end;
      if holdDone then Sleep(1000) else Sleep(300);
    end;
  finally
    ProgressPage.Hide;
  end;
end;

// The rights Program Files gives, on a folder chosen outside it: the elevated
// tasks run our exe from {app} and count on only administrators writing there.
function InstallDirUnderProgramFiles(): Boolean;
var
  dir: String;
begin
  dir := Lowercase(ExpandConstant('{app}')) + '\';
  Result := Pos(Lowercase(ExpandConstant('{commonpf32}')) + '\', dir) = 1;
  if (not Result) and IsWin64 then
    Result := Pos(Lowercase(ExpandConstant('{commonpf64}')) + '\', dir) = 1;
end;

procedure ProtectInstallDir();
var
  dir, icacls: String;
  rc: Integer;
begin
  if InstallDirUnderProgramFiles() then Exit;
  dir := ExpandConstant('{app}');
  icacls := ExpandConstant('{sys}\icacls.exe');
  ForceDirectories(dir);
  // Well-known SIDs, not names: those are translated. Administrators own it, so
  // nobody else keeps the owner's right to change these rules.
  Exec(icacls, '"' + dir + '" /setowner *S-1-5-32-544 /T /C /Q', '', SW_HIDE,
       ewWaitUntilTerminated, rc);
  if not Exec(icacls, '"' + dir + '" /inheritance:r /remove:g *S-1-1-0 *S-1-5-11 ' +
              '/grant:r *S-1-5-18:(OI)(CI)F *S-1-5-32-544:(OI)(CI)F *S-1-5-32-545:(OI)(CI)RX ' +
              '/T /C /Q', '', SW_HIDE, ewWaitUntilTerminated, rc) or (rc <> 0) then
    Log(Format('install folder rights not set (icacls: %d)', [rc]));
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  lines: TArrayOfString;
  internet, consent: String;
#if MyArch == "arm64"
  rc: Integer;
#endif
begin
  // Before the file copy: nothing under {app} can be replaced while the server
  // is holding it open.
  if CurStep = ssInstall then begin
    ProtectInstallDir();
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

  // First: everything below that runs MoonlightWeb.exe needs its runtime.
  InstallVCRuntime();

  // Register the elevated on-demand task that lets the (unprivileged) server
  // apply the next update by itself, with no UAC prompt. Refreshed on every
  // install so the action always matches the current staging convention.
#ifndef DevEdition
  RegisterUpdateTask();
#endif
  // The virtual display helper task: every edition, since a DEV build can
  // stream a headless machine just as well. Refreshed on every install so
  // the action follows {app}. Not on ARM64, which has no driver to apply;
  // the task an earlier installer may have left there goes.
#if MyArch != "arm64"
  RegisterVirtualDisplayTask();
#else
  Exec('schtasks.exe', '/Delete /TN "' + VDisplayTaskName + '" /F', '', SW_HIDE,
       ewWaitUntilTerminated, rc);
#endif
  // The elevated stream worker task: every edition, refreshed so the action
  // follows {app}.
  RegisterStreamWorkerTask();
  // And the launcher service above it, which the worker prefers when it is
  // there. Both are kept: the task is the fallback if the service is stopped.
  RegisterWorkerService();
  // The policy under which that service may press Ctrl+Alt+Suppr.
  ClaimSasPolicy();
  // "MoonlightWeb Virtual Display" itself: on Accept, and again on every
  // update once accepted (the helper finds the node and creates no second
  // one — this is what makes an update a no-op and a reinstall a repair).
  if VDisplayAccepted then
    RegWriteDWordValue(HKEY_LOCAL_MACHINE, VDisplaySetupKey, 'VirtualDisplayAccepted', 1);
  if RegValueExists(HKEY_LOCAL_MACHINE, VDisplaySetupKey, 'VirtualDisplayAccepted') then
    RunVirtualDisplayHelper('install');
  // The HID passthrough's driver, with the native host it serves; again on
  // every update, which is how a newer driver arrives.
  if FileExists(ExpandConstant('{app}\drivers\vhid\MoonlightWebVHid.inf')) then
    RunVirtualHidHelper('install');

  // provisioning.json — consumed and removed by the server on first run.
  // An update skips it: the server is already provisioned, and replaying a
  // first-run provisioning would re-answer settled questions. The exception is a
  // question this build asks for the FIRST time — ShouldSkipPage let its page
  // through, so its answer still has to reach the server.
  if (not UpdateMode) or (not SettingsHasKey('internet_access_enabled')) then begin
    // Written as UTF-8 (no BOM): the consent text is localized (accents) and the
    // server parses this file with a strict UTF-8 JSON parser.
    // A silent install shows no page, and Inno walks it as if Next had been
    // pressed — which used to read as "Accepter". Nobody answered there, so
    // Internet stays closed unless the command line says otherwise.
    if WizardSilent then InternetAuthorized := SilentInternetWanted();
    if InternetAuthorized then internet := 'true' else internet := 'false';
    // Exact agreement text the user read on the Internet page — recorded by the
    // server in its DNS registration audit log (legal traceability). From a
    // silent install, the record says the page was not read.
    consent := ExpandConstant('{cm:InternetPageBody}') + ' / '
             + ExpandConstant('{cm:InternetPageOption}');
    StringChangeEx(consent, '%n', ' ', True);
    if WizardSilent and InternetAuthorized then
      consent := 'Silent install, /INTERNET=yes on the command line (page not shown): ' + consent;
    // No "sunshine" object any more: this installer installs no streaming server
    // and pairs no local host, so there is nothing to auto-pair. Provisioning
    // reads a missing object as auto_pair=false and marks the step skipped, so
    // an older server given this file behaves exactly as if Skip had been
    // clicked — and no plaintext credential is ever written to disk.
    SetArrayLength(lines, 4);
    lines[0] := '{';
    lines[1] := '  "internet_access_authorized": ' + internet + ',';
    lines[2] := '  "consent_message": "' + JsonEscape(consent) + '"';
    lines[3] := '}';
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

  // Auto-start at logon (relaunches on crash, keeps the tray icon). Interactive
  // installs decide this on the Finished page, at the last click; a silent one
  // has no such page and takes what InitializeWizard worked out — on for a
  // first install, and for an update the machine's own task, else the recorded
  // answer, else on. That last arm is what repairs a machine whose task the
  // WORKGROUP bug destroyed, and it has to work here above all: an update
  // triggered from the web app is always silent.
  if WizardSilent then
    ApplyAutostartChoice(WantAutostart);

  // Launch the server and show the live checklist (the Internet link) while
  // first-run provisioning completes.
  RunProvisionChecklist();

  // With the server up, settle the address the Finish page will offer: the
  // internet way in when there is one, loopback otherwise.
  ResolveAdminUrl();
end;

// --- Uninstall: optional removal of the user's configuration ---------------
//
// Everything MoonlightWeb persists lives OUTSIDE {app}, so a plain uninstall
// leaves it all behind on purpose — reinstalling then finds the machine exactly
// as the user left it (that is also what the one-click update relies on). This
// dialog is the opt-in that wipes it instead:
//   %AppData%\MoonlightWeb\<name>       settings.json, sessions.json, the ACME
//                                        certificate, logs and crash dumps
//   HKCU\Software\MoonlightWeb\<name>   the QSettings hive: paired hosts and
//                                        the Moonlight client identity
// <name> is this edition's (MyAppName). The parent keys are shared with the
// other edition and with --dev instances, whose pairings must survive this.
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
    Exec('schtasks.exe', '/End /TN "{#MyAppName}"', '', SW_HIDE,
         ewWaitUntilTerminated, rc);
    Exec('schtasks.exe', '/Delete /TN "{#MyAppName}" /F', '', SW_HIDE,
         ewWaitUntilTerminated, rc);
    // The elevated on-demand update launcher goes too — leaving it behind would
    // keep an elevated task pointing at an exe that is no longer there.
    Exec('schtasks.exe', '/Delete /TN "' + UpdateTaskName + '" /F', '', SW_HIDE,
         ewWaitUntilTerminated, rc);
    DelTree(ExpandConstant('{localappdata}\{#MyAppName}\update'), True, True, True);
    // Where --update-apply verified and ran the last installer (~60 MB).
    DelTree(ExpandConstant('{app}\update-staging'), True, True, True);
    // "MoonlightWeb Virtual Display" goes with the app — this is the one
    // place it is ever removed from (the app offers no such button). Run
    // while the exe and the driver files are still under {app}.
    RunVirtualDisplayHelper('uninstall');
    RegDeleteKeyIncludingSubkeys(HKEY_LOCAL_MACHINE, VDisplaySetupKey);
    // The HID passthrough's driver, unless another edition still uses it.
    RunVirtualHidHelper('uninstall');
    // The virtual display helper task and its staging directory.
    Exec('schtasks.exe', '/Delete /TN "' + VDisplayTaskName + '" /F', '', SW_HIDE,
         ewWaitUntilTerminated, rc);
    DelTree(ExpandConstant('{localappdata}\{#MyAppName}\vdisplay'), True, True, True);
    // The elevated stream worker task.
    Exec('schtasks.exe', '/Delete /TN "' + WorkerTaskName + '" /F', '', SW_HIDE,
         ewWaitUntilTerminated, rc);
    // The launcher service, while the exe that knows how to remove it is still
    // under {app} — and before the taskkill below, which would take it too.
    UnregisterWorkerService();
    // And the policy it pressed Ctrl+Alt+Suppr under, if ours alone.
    ReleaseSasPolicy();
    Exec('taskkill.exe', '/IM "{#MyAppExe}" /F', '', SW_HIDE,
         ewWaitUntilTerminated, rc);
    // Remove the firewall rule added at install time.
    Exec(ExpandConstant('{sys}\netsh.exe'),
         'advfirewall firewall delete rule name="{#MyAppName}"', '', SW_HIDE,
         ewWaitUntilTerminated, rc);
    DeleteFile(ExpandConstant('{group}\MoonlightWeb Admin.url'));
    // Both desktops: the installer wrote the provisional shortcut to the common
    // desktop ({autodesktop} elevated); the server self-heals one on the USER
    // desktop at every startup (see writeAdminShortcut in main.cpp).
    DeleteFile(ExpandConstant('{autodesktop}\MoonlightWeb Admin.url'));
    DeleteFile(ExpandConstant('{userdesktop}\MoonlightWeb Admin.url'));
    DeleteFile(ExpandConstant('{app}\provisioning.json'));
    DeleteFile(ExpandConstant('{app}\provisioning.consumed.json'));
    DeleteFile(ExpandConstant('{userappdata}\MoonlightWeb\{#MyAppName}\provisioning.status.json'));

    // Opt-in wipe. Runs after the server has been killed above: its lock file,
    // logs and settings are only closed once the process is really gone.
    if removeConfig then begin
      DelTree(ExpandConstant('{userappdata}\MoonlightWeb\{#MyAppName}'), True, True, True);
      // Leave the parent MoonlightWeb\ behind only if something else lives in it.
      RemoveDir(ExpandConstant('{userappdata}\MoonlightWeb'));
      // QSettings' hive: hosts\<n>\pairState, the client certificate/key, window
      // state. This edition's subkey only — the org key also holds the other
      // edition's and every --dev instance's pairings — then the org key
      // itself if nothing else is left in it.
      RegDeleteKeyIncludingSubkeys(HKCU, 'Software\MoonlightWeb\{#MyAppName}');
      RegDeleteKeyIfEmpty(HKCU, 'Software\MoonlightWeb');
    end;
  end;
end;
