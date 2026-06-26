dummy read check

## License

Moonlight-Web is free software, licensed under the **GNU General Public License
v3.0** (GPL-3.0). You are free to use, study, modify, fork and redistribute it,
provided you keep it open-source under the same license and **preserve the
copyright notice and credit the original author**.

> Copyright (C) 2026 Bruno Martin &lt;brunoocto@gmail.com&gt;

See [LICENSE](LICENSE) for the full license text and [COPYRIGHT](COPYRIGHT) for
the copyright notice and third-party component licenses.

## Configuration

Most options are set from the in-app Settings/Admin UI and stored server-side in
`settings.json` (under the OS app-data folder, e.g. on Windows
`%APPDATA%/Moonlight-Web/Moonlight-Web/settings.json`).

### File-only options (edit `settings.json` directly, no UI)

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `audio_time_stretch` | bool | `false` | Pitch-preserving audio time-stretch (WSOLA) in the browser. Set to `true` to improve audio smoothness on an unstable network: it absorbs clock drift and jitter by slightly stretching/compressing the sound without changing its pitch — at the cost of a small amount of time-domain distortion and slightly higher CPU usage (the corrections run on the audio thread and only fire during instability, so the energy impact on mobile is minor). `false` keeps the plain adaptive jitter buffer. |

Restart the server (or relaunch the stream) after editing the file.
