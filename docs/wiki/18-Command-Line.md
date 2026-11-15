# 18. Command Line

`moonlightweb` (`MoonlightWeb.exe` on Windows) takes its options on the command line, parsed by `QCommandLineParser` in `backend/src/main.cpp`. They fall in three groups:

- **Run options** change how this run of the server behaves (ports, logs, an isolated instance, booth mode).
- **Commands** ask the server that is **already running** on this machine for something, print the answer and exit. They reach it through its local channel ([§3.7](03-Backend.md#37-headless-operator-cli)), a named pipe or a Unix socket of this install, never through the network: they work **on this machine only**, and only against a running server (otherwise they say so and exit with `1`).
- **Tools** do one job on their own (a benchmark, a probe) and exit.

`moonlightweb --help` lists the public options, `--help-all` adds Qt's own. Options that take a value accept both `--name value` and `--name=value`.

## 18.1 Run options

| Option | Effect | `settings.json` |
|---|---|---|
| `--port <port>` | HTTP listener port (it redirects to HTTPS). Default `80`, `48080` with `--dev`. | `http_port` (the option wins) |
| `--ws-port <port>` | Base of the WebRTC signaling ports. Default `48001`, `48501` with `--dev`. The control channel and the per-slot worker ports are counted from it. | — |
| `--log <path>` | Write the log to this file instead of the default log folder. Read before anything else, so the first line lands there. | — |
| `--config <path>` | Read and write the settings from this file instead of `settings.json` in the data folder. Created if absent. | the whole file |
| `--dev` | An isolated development instance: its own data folder (`…-dev`), its own ports (HTTP `48080`, HTTPS `48443`, signaling `48501`), no single-instance lock, so it runs beside an installed server. | — |
| `--debug` | Debug mode for this run: the options of a debug build, and the browsers' console sent to the client log ([§3.8](03-Backend.md#38-diagnostics-debug-mode-verbose-logs-the-logs-archive)). | `debug_mode` (either turns it on) |
| `--verbose` | Verbose logs for this run: DEBUG lines, libdatachannel and Qt network diagnostics, the browsers' pipeline diagnostics. | `verbose_logs` (either turns it on) |
| `--autostart` | Started at login: no browser tab is opened, and a second launch does not surface the admin page. Used by the login item. | — |
| `--noadmin` | Booth mode: the hosts already set up are streamed, with no admin page, no host change and no new device, even from this machine, until a restart without the flag. | `share_in_booth_mode` (sharing while in booth mode) |

## 18.2 Commands (this machine only)

Each one talks to the running server and exits: `0` when it was done, `1` otherwise (no server running, a refused value, a refused request).

| Command | Effect | `settings.json` |
|---|---|---|
| `--status` | Prints the server's local and LAN URLs, the remote entry link and whether its line is up, the access PIN, the Sunshine state, and while Internet Access is off whether a UPnP router answered. | — (read only) |
| `--new-pin` | Issues a new access PIN for a remote browser and prints it. Sessions already signed in stay signed in. | — |
| `--set-admin-password` | Sets the password another computer on the network uses to open the admin page. Read from standard input (typed twice on a terminal, or piped), never from the command line. | `admin_password` |
| `--clear-admin-password` | Removes that password: only this machine opens the admin page, and the machines that had unlocked it are signed out of it. Asks for confirmation, or `--yes`. | `admin_password` |
| `--enable-internet` | Turns the Internet link on: prints the consent text and asks for `yes` on the terminal, or `--yes`. Then says whether a UPnP router answered, or what to forward by hand. Refused on a LAN-only instance (`MW_LAN_ONLY`). | `internet_access_enabled`, `internet_consent` |
| `--yes` | With `--enable-internet` or `--clear-admin-password`: agree without a question, for a script or an unattended install. | — |
| `--open-link` | Opens the server's page in the browser, signed in as this machine's owner. | — |
| `--print-link` | Prints the link `--open-link` would open instead. It works once, for ten minutes: send it through a pipe, never into a file. | — |
| `--max-allowed-frame-rate <fps>` | The administrator's frame rate: no stream goes above it, whatever each viewer's Settings say. `0` lifts it. | `max_allowed_frame_rate` |
| `--max-allowed-bitrate <mbps>` | The administrator's bitrate, in Mbps. `0` lifts it. | `max_allowed_bitrate` |
| `--max-allowed-stream-resolution <WxH>` | The administrator's box: `1920x1080`, `1920` (a square box), `0x1440` (the height only), `0` lifts it. | `max_allowed_stream_resolution` |
| `--allow-viewer-microphone on\|off` | Whether this host takes a viewer's microphone. `off` refuses it to every viewer, whatever their Settings say, and closes the ones of the streams already running. Default `on`. Also on the admin page, *Accept viewers' microphones*. | `viewer_mic_allowed` |
| `--local-request "<METHOD> <path>"` | One request to the running server, for a script of this machine (a bench, a health check). The answer's body goes to standard output, its status to standard error; a body to send is read from standard input, never from the command line. Only a few exact routes are accepted (`LocalRequest::allowed`): the server's status reads, its native engine and virtual display, the streaming settings, a new PIN, restart and quit. The credentials, the Internet link and ending someone's stream are not. | — |

The three `--max-allowed-*` options can be given together. Each command prints what the server holds once it is done, read back from it.

## 18.3 Tools

| Option | Effect |
|---|---|
| `--native-bench <spec>` | Benchmarks the native capture and encode engine on this machine, without a browser. `spec` is a `key=value` list: `display=<id>,seconds=10,codec=hevc\|h264\|av1,fps=0,bitrate=20000,width=0,height=0,yuv444=0,intra=0,gpu=<id>,preset=1..7,tuning=ull\|ll,multipass=off\|quarter\|full,aq=0\|1,taq=0\|1,preanalysis=0\|1,quality=speed\|balanced\|quality,tu=1..7,vbv=<frames>,out=<csv>`. Without `display=` it lists the displays and GPUs. Writes one CSV row per frame and prints a summary. |
| `--native-probe` | Prints what the native engine can do on this desktop, as one JSON object on standard output. The service uses it to ask the console session. |
| `--help`, `-h`, `-?` | The public options. `--help-all` adds Qt's. |
| `--version`, `-v` | The version. |

## 18.4 Internal, not for operators

These are started by the server itself, its service, its scheduled tasks or its installer, with arguments they build. They are listed so that a process list or a log line can be read; running one by hand is not supported.

| Option | Started by |
|---|---|
| `--stream-worker`, `--worker-pipe`, `--worker-data-dir`, `--signin-screen` | The server, for each stream's worker process. |
| `--worker-service`, `--worker-service-install`, `--worker-service-remove` | The installer and Windows, for the stream-worker launcher service. |
| `--owner-record` | The installer, to record the account the installation is made by. |
| `--vdisplay-apply` (with `--stage`, `--vdisplay-dir`), `--vdisplay-guard` | The server and its elevated task, for the virtual display. |
| `--vhid-apply` (with `--vhid-dir`) | The installer, for the HID passthrough driver. |
| `--update-apply` | The elevated task that runs a staged update. |
| `--open-on-desktop` | The service, to open a native host app on the console user's desktop. |
| `--wait-link` | The installer, to wait until `--open-link` has a link to open. |
| `--quit-server` | The installer and the uninstaller, to ask the running server to quit (Windows). |
