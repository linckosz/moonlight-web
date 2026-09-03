/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * MoonlightWeb — Streaming Settings view
 *
 * Streaming-related user preferences (video codec, resolution, bitrate, etc.).
 * Settings are stored in localStorage (per-browser) with default values
 * fetched from the server on first visit.
 *
 * Layout:
 *   - Video section: resolution, FPS, HDR, bitrate
 *   - Advanced section: video enhancement, codec, mute host, tearing,
 *     performance stats, gaming mode
 */
import { BackendClient } from '../api/BackendClient.js';
import { Toast } from './Toast.js';
import { t, getLanguage, setLanguage, AVAILABLE_LANGUAGES } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';
import { noticeDetailsHtml, plainText } from './PrivacyNotice.js';
import {
    SUPPORTS_CANVAS_TEARING,
    IS_MOBILE_OR_TABLET,
    resolveTearing,
} from '../util/BrowserDetect.js';
import { aspectToNumber, computeAutoBitrate } from '../util/AutoBitrate.js';
import { ASPECT_VALUES, SCREEN_ASPECTS } from '../util/AspectRatio.js';

/** True when the browser supports touch events (mobile/tablet, or touchscreen laptop). */
const IS_TOUCH_DEVICE =
    'ontouchstart' in window ||
    (typeof navigator.maxTouchPoints !== 'undefined' && navigator.maxTouchPoints > 0);

const STORAGE_KEY = 'mw-streaming-settings';

/**
 * Enhancer choices, in menu order after 'auto': Canvas2D resampling, then each
 * upscaler on WebGL2 and on WebGPU. Listed here rather than imported so the
 * settings page does not pull the renderers in. `gpu` marks the ones that need
 * WebGPU (grayed out without).
 */
const ENHANCER_CHOICES = [
    { value: 'smooth2d', key: 'settings.algoPerfSmooth2d', gpu: false },
    { value: 'gl-sgsr', key: 'settings.algoPerfGl', gpu: false },
    { value: 'sgsr', key: 'settings.algoPerfGpu', gpu: true },
    { value: 'gl-nis', key: 'settings.algoBalGl', gpu: false },
    { value: 'nis', key: 'settings.algoBalGpu', gpu: true },
    { value: 'gl-fsr1', key: 'settings.algoQualGl', gpu: false },
    { value: 'fsr1', key: 'settings.algoQualGpu', gpu: true },
];

export class SettingsView {
    constructor(container, onClose) {
        this.container = container;
        this.onClose = onClose || (() => {});

        this._videoCodec = 'hevc';
        this._gamingMode = true;
        this._showPerformanceStats = false;
        this._streamBitrateMbps = 20;
        this._streamHeight = 1080;
        this._streamAspect = 'auto';
        this._streamFps = 60;
        this._hdrEnabled = false;
        this._chroma444 = false;
        this._muteHostAudio = true;
        this._touchSensitivity = 2.2;
        // Mobile only: direct touch-screen input (absolute) instead of the
        // relative trackpad model. Off by default.
        this._touchScreen = false;
        // Allow tearing (default ON = present on decode). Only meaningful on
        // Chromium desktop (desynchronized canvas); forced off elsewhere, where
        // the render loop keeps its VSync pacing.
        this._tearing = SUPPORTS_CANVAS_TEARING;
        // Worker decode mode: 'auto' (= off, default), 'on' (explicit opt-in) or 'off'.
        this._videoWorker = 'auto';
        this._mediaTrackOnlyH264 = false;
        // Video enhancement (upscale/sharpen): 'off'|'on' + algo selector, see
        // ENHANCER_CHOICES ('auto' picks by platform).
        this._videoEnhancement = 'off';
        this._videoEnhancementAlgo = 'auto';
        this._debugBuild = false;
        // Which virtual controller the host presents: 'auto' follows the pad we
        // detect, 'x360'/'ds4' force one. Shown only in debug builds — in
        // production the right behaviour is to guess correctly, and a visible
        // switch turns a detection bug into a question asked of the user.
        this._gamepadProfile = 'auto';
        // Click-to-photon latency flag: a host-side switch (the host raises a
        // flag on every click, the stream measures the delay). Shown only when
        // the server can honour it — debug builds on Windows — and read from
        // the server, never from this device's copy: it is the host's state.
        this._latencyFlag = false;
        this._latencyFlagSupported = false;

        // Power Saving mode (mobile only): forces the lightest pipeline.
        // _powerSaveBackup holds the values present before enabling, so unchecking
        // can restore the ones the user didn't manually change in the meantime.
        this._powerSave = false;
        this._powerSaveBackup = null;

        // Per-codec browser support map: { h264:bool, hevc:bool, av1:bool } or null
        this._codecSupport = null;

        // True when this browser holds a session worth ending — see
        // _checkSession(). False for the host machine's own browser, whichever
        // address it reached us under.
        this._canLogout = false;

        // Debounce timer to avoid rapid repeated saves
        this._saveTimer = null;

        // Privacy section. Everyone signed in sees what this machine reports;
        // only the machine itself may change the answer, so the switch and the
        // disclosure are two separate things. All false until _loadStatsConsent
        // runs, so a backend that will not answer yields no section at all
        // rather than one claiming the wrong state.
        this._statsLoaded = false;
        this._statsAvailable = false;
        this._statsGranted = false;
        this._statsWritable = false;
    }

    async start() {
        await this._loadState();
        this._codecSupport = await this._checkCodecSupport();
        this._webgpuUsable = await this._checkWebGpuSupport();
        this._canLogout = await this._checkSession();
        await this._loadStatsConsent();
        this.render();
        this.bindEvents();
    }

    /**
     * What this machine reports, and whether this browser may change it.
     *
     * Readable by any signed-in user — it is their streams being counted too,
     * and a disclosure only the owner can find is not a disclosure. Writable
     * only from the machine itself: the answer speaks for the machine, and a
     * guest on the LAN does not get to answer for its owner.
     *
     * Best-effort: no answer means no section, which matches a backend that is
     * reporting nothing anyway.
     */
    async _loadStatsConsent() {
        try {
            const state = await BackendClient.getMetricsConsent();
            this._statsLoaded = !!state;
            this._statsAvailable = !!(state && state.available);
            this._statsGranted = !!(state && state.decision === 'granted');
            this._statsWritable = !!(state && state.writable);
        } catch (err) {
            console.warn('[Settings] statistics consent unavailable:', err);
            this._statsLoaded = false;
        }
    }

    /**
     * The checkbox IS the consent record: what it says is stored together with
     * the text shown around it, in the language it was read in, so the record
     * always names what was agreed to. Reverted on failure rather than left
     * showing a state the backend does not hold.
     */
    async _setStatsConsent(checkbox) {
        const granted = checkbox.checked;
        checkbox.disabled = true;
        try {
            await BackendClient.setMetricsConsent(
                granted,
                plainText(['sectionTitle', 'toggle', 'toggleDesc']),
                'settings',
            );
            this._statsGranted = granted;
            Toast.success(granted ? t('stats.turnedOn') : t('stats.turnedOff'));
        } catch (err) {
            console.warn('[Settings] could not save the statistics choice:', err);
            checkbox.checked = !granted;
            Toast.error(t('stats.choiceSaveFailed'));
        } finally {
            checkbox.disabled = false;
        }
    }

    /**
     * The privacy block: what leaves this machine, and the switch that stops
     * it. Absent only when the backend would not say — claiming "nothing is
     * sent" without having asked would be the one unacceptable answer.
     */
    _renderPrivacySection() {
        if (!this._statsLoaded) return '';

        // A build with no reporting credentials (anything self-compiled) sends
        // nothing whatever anyone ticks, so it gets the statement instead of a
        // switch that would be a lie.
        const control = !this._statsAvailable
            ? `<p class="setting-desc">${t('stats.unavailable')}</p>`
            : `
                    <div class="settings-field">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="settings-stats-consent"
                                   ${this._statsGranted ? 'checked' : ''}
                                   ${this._statsWritable ? '' : 'disabled'} />
                            <span class="settings-checkbox-text">${t('stats.toggle')}</span>
                        </label>
                        <p class="setting-desc">${t('stats.toggleDesc')}</p>
                        ${this._statsWritable ? '' : `<p class="setting-desc">${t('stats.ownerOnly')}</p>`}
                    </div>`;

        return `
                <div class="settings-section" id="settings-section-privacy">
                    <h3 class="settings-section-title">${t('stats.settingsTitle')}</h3>
                    ${control}
                    ${noticeDetailsHtml()}
                </div>`;
    }

    /**
     * Does this browser hold a session it has any reason to end?
     *
     * Two conditions, and the second is the one that is easy to get wrong. The
     * browser must hold a session cookie at all (`has_session` — the host's own
     * loopback page is authenticated by its address and has nothing to end), and
     * it must not be the host machine's own browser (`is_host_machine`, true for
     * the loopback origin *and* for a host-key session reached through the public
     * domain). Signing the host out protects nothing — the credential is the
     * machine itself — and costs the admin access being used to click, dropping
     * it onto a PIN prompt on the very machine that owns the server. A LAN device
     * that unlocked admin with the password is not the host and keeps the button:
     * it is somebody else's computer, which is the whole point.
     *
     * Best-effort: on any failure the Log out section is simply not offered.
     */
    async _checkSession() {
        try {
            const status = await BackendClient.getAuthStatus();
            return status.has_session === true && status.is_host_machine !== true;
        } catch (err) {
            console.warn('[Settings] Could not read the auth status:', err);
            return false;
        }
    }

    async _loadState() {
        // 1. Try localStorage first (per-browser preferences)
        const stored = localStorage.getItem(STORAGE_KEY);
        if (stored) {
            try {
                const data = JSON.parse(stored);
                this._applySettings(data);
            } catch (err) {
                console.warn('[Settings] Failed to parse localStorage, falling back to server');
            }
        }

        // 2. Fetch server settings.
        //    media_track_only_h264 is a server-enforced constraint, not a user preference.
        //    Always use the server's value, overriding localStorage.
        //    On first visit (no localStorage), also load all defaults from the server.
        try {
            const data = await BackendClient.getStreamingSettings();
            this._mediaTrackOnlyH264 = data.media_track_only_h264 === true;
            this._debugBuild = data.debug_build === true;
            this._latencyFlagSupported = data.latency_flag_supported === true;
            this._latencyFlag = this._latencyFlagSupported && data.latency_flag_enabled === true;

            if (!stored) {
                this._applySettings(data);
                await this._saveToStorage();
            }
        } catch (err) {
            if (!stored) {
                console.warn('[Settings] Failed to load streaming settings:', err);
            } else {
                console.warn('[Settings] Failed to fetch media_track_only_h264 from server:', err);
            }
        }
    }

    /** Apply a settings object to internal state (normalises field names). */
    _applySettings(data) {
        this._videoCodec = data.video_codec || 'hevc';
        this._mediaTrackOnlyH264 = data.media_track_only_h264 === true;
        this._gamingMode = data.gaming_mode !== false;
        this._showPerformanceStats = data.show_performance_stats === true;
        const kbps = data.stream_bitrate || 20000;
        this._streamBitrateMbps = Math.round(kbps / 1000);
        this._streamHeight = data.stream_height || 1080;
        this._streamAspect = ASPECT_VALUES.includes(data.stream_aspect)
            ? data.stream_aspect
            : 'auto';
        this._streamFps = data.stream_fps || 60;
        this._hdrEnabled = data.hdr_enabled === true;
        this._chroma444 = data.chroma_444_enabled === true;
        this._muteHostAudio = data.mute_host_audio !== false;
        this._touchSensitivity =
            typeof data.touch_sensitivity === 'number' && data.touch_sensitivity > 0
                ? data.touch_sensitivity
                : 2.2;
        this._touchScreen = data.touch_screen === true;
        // Allow tearing (default on): resolveTearing() owns the default, the
        // legacy keys and the "cannot tear anyway" gate — see BrowserDetect.
        this._tearing = resolveTearing(data);
        // Back-compat: older saves stored a boolean; map it onto the tri-state.
        const vw = data.video_worker;
        this._videoWorker =
            vw === true || vw === 'on' ? 'on' : vw === false || vw === 'off' ? 'off' : 'auto';
        this._videoEnhancement = data.video_enhancement === 'on' ? 'on' : 'off';
        const profile = data.gamepad_profile;
        this._gamepadProfile = profile === 'x360' || profile === 'ds4' ? profile : 'auto';
        const algo = data.video_enhancement_algo;
        this._videoEnhancementAlgo = ENHANCER_CHOICES.some((c) => c.value === algo) ? algo : 'auto';
        this._powerSave = data.power_save === true;
        this._powerSaveBackup =
            data.power_save_backup && typeof data.power_save_backup === 'object'
                ? data.power_save_backup
                : null;
    }

    /** See util/AutoBitrate.js (shared with the congestion-degradation ladder). */
    _aspectToNumber(aspect) {
        return aspectToNumber(aspect);
    }

    _computeAutoBitrate(height, fps, aspect, chroma444, hdr) {
        return computeAutoBitrate(height, fps, aspect, chroma444, hdr);
    }

    /** Recompute the bitrate from current selects and sync the slider UI. */
    _applyAutoBitrate() {
        const height = parseInt(this.container.querySelector('#settings-stream-height')?.value, 10);
        const fps = parseInt(this.container.querySelector('#settings-stream-fps')?.value, 10);
        const chroma444 = this.container.querySelector('#settings-chroma-444')?.checked === true;
        // "auto" resolves to the host's own format at launch, which is unknown
        // here — aspectToNumber() reads it as the 16:9 baseline, which is what
        // the estimate wants.
        const aspect =
            this.container.querySelector('#settings-stream-aspect')?.value || this._streamAspect;
        const hdr = this.container.querySelector('#settings-hdr')?.checked ?? this._hdrEnabled;
        const mbps = this._computeAutoBitrate(
            isNaN(height) ? this._streamHeight : height,
            isNaN(fps) ? this._streamFps : fps,
            aspect,
            chroma444,
            hdr,
        );

        const slider = this.container.querySelector('#settings-stream-bitrate');
        const label = this.container.querySelector('#settings-bitrate-value');
        if (slider) slider.value = mbps;
        if (label) label.textContent = mbps;
        this._streamBitrateMbps = mbps;
    }

    /** Persist current internal state to localStorage (all clients)
     *  and to the server (localhost only — updates defaults for all browsers). */
    async _saveToStorage() {
        const settings = {
            video_codec: this._videoCodec,
            gaming_mode: this._gamingMode,
            show_performance_stats: this._showPerformanceStats,
            stream_bitrate: this._streamBitrateMbps * 1000,
            stream_height: this._streamHeight,
            stream_aspect: this._streamAspect,
            stream_fps: this._streamFps,
            hdr_enabled: this._hdrEnabled,
            chroma_444_enabled: this._chroma444,
            mute_host_audio: this._muteHostAudio,
            seamless_switching: true,
            touch_sensitivity: this._touchSensitivity,
            touch_screen: this._touchScreen,
            tearing_enabled: this._tearing,
            // Marks the value above as a real choice rather than the old OFF
            // default (see resolveTearing). Only a browser that can actually
            // tear stamps it: elsewhere tearing_enabled is a forced false, not
            // a preference worth propagating to the other devices through the
            // server-side defaults.
            ...(SUPPORTS_CANVAS_TEARING ? { tearing_default_v2: true } : {}),
            video_worker: this._videoWorker,
            video_enhancement: this._videoEnhancement,
            video_enhancement_algo: this._videoEnhancementAlgo,
            // Per-device only (server ignores these unknown fields).
            power_save: this._powerSave,
            power_save_backup: this._powerSaveBackup,
            gamepad_profile: this._gamepadProfile,
            // Host-side switch: the server ignores it unless it can honour it.
            latency_flag_enabled: this._latencyFlag,
        };
        localStorage.setItem(STORAGE_KEY, JSON.stringify(settings));

        // Persist to server when on localhost, so settings.json is updated
        // and all other clients get these defaults on first visit.
        const host = window.location.hostname;
        if (host === 'localhost' || host === '127.0.0.1') {
            try {
                await BackendClient.saveStreamingSettings(settings);
            } catch (err) {
                console.warn('[Settings] Failed to save to server:', err);
            }
        }
    }

    /**
     * Test browser codec support via VideoDecoder.isConfigSupported() for H.264,
     * HEVC and AV1. Uses minimal codec strings (no bitstream description needed).
     *
     * Each codec is tested with a list of fallback codec strings. The codec is
     * marked supported if ANY string in its list returns supported=true.
     *
     * If VideoDecoder.isConfigSupported is not available (old browser), all
     * codecs are assumed supported (graceful fallback).
     */
    async _checkCodecSupport() {
        const support = { h264: false, hevc: false, av1: false };

        if (typeof VideoDecoder?.isConfigSupported !== 'function') {
            console.warn(
                '[Settings] VideoDecoder.isConfigSupported not available — ' +
                    'assuming all codecs supported',
            );
            support.h264 = true;
            support.hevc = true;
            support.av1 = true;
            return support;
        }

        // Test a list of codec strings, return true if ANY is supported.
        const testCodec = async (codecs) => {
            for (const codec of codecs) {
                try {
                    const result = await VideoDecoder.isConfigSupported({ codec });
                    if (result?.supported) return true;
                } catch (_) {
                    // Individual codec string rejected — try next fallback
                }
            }
            return false;
        };

        support.h264 = await testCodec([
            'avc1.64002A', // High 4.2
            'avc1.42001E', // Baseline 3.0
            'avc1.64001E', // High 3.0
        ]);
        support.hevc = await testCodec([
            'hev1.1.6.L153.B0', // hev1 used for actual streaming (Annex B fmt)
            'hvc1.1.6.L153.B0', // Main, High tier, Level 5.1
            'hvc1.1.6.L120.B0', // Main, High tier, Level 4.0
            'hvc1.1.2.L153.B0', // Main, Main tier, Level 5.1
        ]);
        support.av1 = await testCodec([
            'av01.0.08M.08', // Main, 1080p, 8-bit
            'av01.0.04M.08', // Main, 720p, 8-bit
            'av01.0.01M.08', // Main, 480p, 8-bit
        ]);

        console.log('[Settings] Browser codec support:', JSON.stringify(support));

        if (!support.h264) {
            console.error('[Settings] CRITICAL: H.264 not supported by this browser');
        }

        return support;
    }

    /**
     * Probe WebGPU usability for the Video Enhancement feature (consumed by the
     * UI graying in a later commit). Probes the adapter, NOT the device — the
     * device is created by the renderer at launch (Canvas2D fallback on failure).
     */
    async _checkWebGpuSupport() {
        try {
            if (!navigator.gpu) return false;
            const adapter = await navigator.gpu.requestAdapter();
            return !!adapter;
        } catch (_) {
            return false;
        }
    }

    /**
     * Return the effective codec to display in the dropdown, considering:
     * 1. MediaTrack transport forces H.264
     * 2. Browser codec support (preferred codec may be unsupported)
     * 3. Fallback chain: h264 > hevc > av1
     *
     * Does NOT modify this._videoCodec (user preference remains in storage).
     */
    _getEffectiveCodec() {
        if (this._mediaTrackOnlyH264) return 'h264';
        // HDR has no H.264 path (H.264 HDR isn't decodable in practice): when HDR
        // is on, H.264 is excluded from selection / fallback entirely.
        const hdr = this._hdrEnabled;
        if (!this._codecSupport) {
            return hdr && this._videoCodec === 'h264' ? 'hevc' : this._videoCodec;
        }
        if (this._codecSupport[this._videoCodec] && !(hdr && this._videoCodec === 'h264')) {
            return this._videoCodec;
        }
        if (hdr) {
            if (this._codecSupport.hevc) return 'hevc';
            if (this._codecSupport.av1) return 'av1';
            return 'hevc';
        }
        if (this._codecSupport.h264) return 'h264';
        if (this._codecSupport.hevc) return 'hevc';
        if (this._codecSupport.av1) return 'av1';
        return this._videoCodec;
    }

    destroy() {
        if (this._saveTimer) {
            clearTimeout(this._saveTimer);
            this._saveTimer = null;
        }
    }

    // --- Auto-save ---

    _autoSave() {
        if (this._saveTimer) {
            clearTimeout(this._saveTimer);
        }

        this._saveTimer = setTimeout(async () => {
            this._saveTimer = null;

            // When MediaTrack transport is forced, only H.264 is available
            const codec = this._mediaTrackOnlyH264
                ? 'h264'
                : this.container.querySelector('#settings-video-codec')?.value || this._videoCodec;
            const gamingMode =
                this.container.querySelector('#settings-gaming-mode')?.checked ?? this._gamingMode;
            const showPerf =
                this.container.querySelector('#settings-show-perf-stats')?.checked ??
                this._showPerformanceStats;
            const bitrateMbps =
                parseInt(this.container.querySelector('#settings-stream-bitrate')?.value, 10) ||
                this._streamBitrateMbps;
            const heightRaw = this.container.querySelector('#settings-stream-height')?.value;
            const height = heightRaw !== undefined ? parseInt(heightRaw, 10) : this._streamHeight;
            const aspect =
                this.container.querySelector('#settings-stream-aspect')?.value ||
                this._streamAspect;
            const fps =
                parseInt(this.container.querySelector('#settings-stream-fps')?.value, 10) ||
                this._streamFps;
            const hdr = this.container.querySelector('#settings-hdr')?.checked ?? this._hdrEnabled;
            const chroma444 =
                this.container.querySelector('#settings-chroma-444')?.checked ?? this._chroma444;
            const sensRaw = parseFloat(
                this.container.querySelector('#settings-sensitivity')?.value,
            );
            const sensitivity = isNaN(sensRaw) ? this._touchSensitivity : sensRaw;
            const touchScreen =
                this.container.querySelector('#settings-touch-screen')?.checked ??
                this._touchScreen;
            const tearing =
                this.container.querySelector('#settings-tearing')?.checked ?? this._tearing;
            const videoWorker =
                this.container.querySelector('#settings-video-worker')?.value ?? this._videoWorker;
            const veCheck = this.container.querySelector('#settings-video-enhancement');
            const videoEnhancement = veCheck
                ? veCheck.checked
                    ? 'on'
                    : 'off'
                : this._videoEnhancement;
            // Algo dropdown is always available; defaults to 'auto'.
            const veAlgoEl = this.container.querySelector('#settings-video-enhancement-algo');
            const videoEnhancementAlgo = veAlgoEl ? veAlgoEl.value : this._videoEnhancementAlgo;
            // Absent outside debug builds, where it keeps whatever it had —
            // which is 'auto' unless someone once ran a debug build here.
            const gpProfileEl = this.container.querySelector('#settings-gamepad-profile');
            const gamepadProfile = gpProfileEl ? gpProfileEl.value : this._gamepadProfile;
            const latencyFlag =
                this.container.querySelector('#settings-latency-flag')?.checked ??
                this._latencyFlag;

            // Update internal state
            this._videoCodec = codec;
            this._gamingMode = gamingMode;
            this._showPerformanceStats = showPerf;
            this._streamBitrateMbps = bitrateMbps;
            this._streamHeight = isNaN(height) ? this._streamHeight : height;
            this._streamAspect = aspect;
            this._streamFps = fps;
            this._hdrEnabled = hdr;
            this._chroma444 = chroma444;
            this._touchSensitivity = sensitivity;
            this._touchScreen = touchScreen;
            this._tearing = tearing;
            this._videoWorker = videoWorker;
            this._videoEnhancement = videoEnhancement;
            this._videoEnhancementAlgo = videoEnhancementAlgo;
            this._gamepadProfile = gamepadProfile;
            this._latencyFlag = latencyFlag;

            // Save to localStorage and server (if localhost)
            await this._saveToStorage();

            Toast.success(t('settings.saved'));
        }, 300);
    }

    /** Reset all streaming preferences to their default values. */
    async _resetDefaults() {
        this._videoCodec = 'hevc';
        // Default preset leaves mouse gaming mode OFF (opt-in per user request).
        this._gamingMode = false;
        this._showPerformanceStats = false;
        this._streamHeight = 1080;
        this._streamAspect = 'auto';
        this._streamFps = 60;
        this._hdrEnabled = false;
        this._chroma444 = false;
        this._muteHostAudio = true;
        this._touchSensitivity = 2.2;
        this._touchScreen = false;
        this._tearing = SUPPORTS_CANVAS_TEARING;
        this._videoWorker = 'auto';
        // Off: the plain Canvas2D path is the fastest first impression (see
        // StreamView's renderer selection); the Enhancer is an opt-in.
        this._videoEnhancement = 'off';
        this._videoEnhancementAlgo = 'auto';
        this._powerSave = false;
        this._powerSaveBackup = null;
        // Bitrate follows the 1080p60 SDR 16:9 reference
        this._streamBitrateMbps = this._computeAutoBitrate(1080, 60, '16:9', false, false);

        await this._saveToStorage();

        // Re-render with defaults and re-bind the controls
        this.render();
        this.bindEvents();
        Toast.success(t('settings.settingsReset'));
    }

    /**
     * Bitrate used by Power Saving: the 720p60 SDR auto estimate for the
     * current aspect, reduced by 30% (rounded down) and floored at 2 Mbps.
     */
    _powerSaveBitrate() {
        const base = this._computeAutoBitrate(720, 60, this._streamAspect, false, false);
        return Math.max(2, Math.floor(base * 0.7));
    }

    /**
     * Toggle Power Saving mode (mobile only). Enabling forces the lightest
     * pipeline (native video / UDP-first transport, H.264, no HDR, no
     * enhancement, no tearing, 720p / 60fps, reduced bitrate) and grays out
     * the forced controls. Resolution / FPS / bitrate stay editable.
     *
     * The pre-enable values are snapshotted; disabling restores each one ONLY
     * if it is still at its power-save default (i.e. the user didn't change it).
     */
    async _togglePowerSave(enabled) {
        if (enabled) {
            this._powerSaveBackup = {
                video_codec: this._videoCodec,
                hdr_enabled: this._hdrEnabled,
                chroma_444_enabled: this._chroma444,
                video_enhancement: this._videoEnhancement,
                tearing_enabled: this._tearing,
                stream_height: this._streamHeight,
                stream_fps: this._streamFps,
                stream_bitrate_mbps: this._streamBitrateMbps,
            };
            this._videoCodec = 'h264';
            this._hdrEnabled = false;
            this._chroma444 = false;
            this._videoEnhancement = 'off';
            this._tearing = false;
            this._streamHeight = 720;
            this._streamFps = 60;
            // Power-save default bitrate: the 720p60 SDR auto value cut by 30%
            // (floored) with a 2 Mbps floor, to spare mobile battery / data.
            this._streamBitrateMbps = this._powerSaveBitrate();
            this._powerSave = true;
        } else {
            const b = this._powerSaveBackup;
            if (b) {
                const psBitrate = this._powerSaveBitrate();
                // Restore each value only if untouched (still at the power-save default).
                if (this._videoCodec === 'h264') this._videoCodec = b.video_codec;
                if (this._hdrEnabled === false) this._hdrEnabled = b.hdr_enabled;
                if (this._chroma444 === false) this._chroma444 = b.chroma_444_enabled;
                if (this._videoEnhancement === 'off') this._videoEnhancement = b.video_enhancement;
                // Old backups stored the inverted vsync_enabled key — with
                // nothing to restore, fall back to the default (on wherever the
                // platform can tear) instead of leaving a stale off behind.
                if (this._tearing === false)
                    this._tearing =
                        b.tearing_enabled === undefined
                            ? SUPPORTS_CANVAS_TEARING
                            : b.tearing_enabled === true && SUPPORTS_CANVAS_TEARING;
                if (this._streamHeight === 720) this._streamHeight = b.stream_height;
                if (this._streamFps === 60) this._streamFps = b.stream_fps;
                if (this._streamBitrateMbps === psBitrate)
                    this._streamBitrateMbps = b.stream_bitrate_mbps;
            }
            this._powerSave = false;
            this._powerSaveBackup = null;
        }

        await this._saveToStorage();
        this.render();
        this.bindEvents();
        Toast.success(t('settings.saved'));
    }

    // --- Rendering ---

    render() {
        // Power Saving forces (and grays out) HDR, VSync, codec and enhancement.
        // psDisabled disables the control; psLocked dims the whole field + adds a
        // lock icon so it's clearly read-only (not just subtly greyed).
        const psDisabled = this._powerSave ? ' disabled' : '';
        const psLocked = this._powerSave ? ' settings-field-locked' : '';

        const hdrDisabled = psDisabled;
        const hdrLocked = psLocked;
        const hdrChecked = this._hdrEnabled ? 'checked' : '';

        // Allow tearing: only Chromium desktop can bypass VSync (desynchronized
        // canvas swapchain) — elsewhere the field is dimmed + locked (🔒),
        // unchecked, with the "(unavailable)" suffix. Power Saving also locks it.
        const tearingUnavailable = !SUPPORTS_CANVAS_TEARING;
        const tearingDisabled = tearingUnavailable || this._powerSave ? ' disabled' : '';
        const tearingLocked = tearingUnavailable ? ' settings-field-locked' : psLocked;
        const tearingChecked = this._tearing && !tearingUnavailable ? 'checked' : '';

        // Codec options (explicit, no "Auto")
        const codecs = [
            { value: 'h264', label: t('settings.codecH264') },
            { value: 'hevc', label: t('settings.codecHevc') },
            { value: 'av1', label: t('settings.codecAv1') },
        ];
        const effectiveCodec = this._getEffectiveCodec();
        const codecOptions = codecs
            .map((c) => {
                const browserDisabled = this._codecSupport && !this._codecSupport[c.value];
                const mediaTrackDisabled =
                    this._mediaTrackOnlyH264 && (c.value === 'hevc' || c.value === 'av1');
                // H.264 has no usable HDR path: disable it while HDR is enabled.
                const hdrDisabledCodec = this._hdrEnabled && c.value === 'h264';
                const disabled = browserDisabled || mediaTrackDisabled || hdrDisabledCodec;
                const selected = c.value === effectiveCodec ? ' selected' : '';

                let label = c.label;
                if (hdrDisabledCodec) {
                    label = t('settings.codecHdrUnavailable', { codec: c.value.toUpperCase() });
                } else if (browserDisabled || mediaTrackDisabled) {
                    label = t('settings.codecUnavailable', { codec: c.value.toUpperCase() });
                }

                return `<option value="${c.value}"${selected}${disabled ? ' disabled' : ''}>${this.esc(label)}</option>`;
            })
            .join('');

        // Warning when codec preference is overridden due to browser support
        const codecChanged =
            this._codecSupport &&
            this._codecSupport[this._videoCodec] === false &&
            effectiveCodec !== this._videoCodec;
        let codecHintHtml = '';
        if (codecChanged) {
            codecHintHtml = `<div class="settings-note">
                ${this.esc(
                    t('settings.codecFallback', {
                        selected: this._videoCodec.toUpperCase(),
                        effective: effectiveCodec.toUpperCase(),
                    }),
                )}
            </div>`;
        }

        // Critical warning when no codec is supported at all
        const noCodecSupported =
            this._codecSupport &&
            !this._codecSupport.h264 &&
            !this._codecSupport.hevc &&
            !this._codecSupport.av1;
        if (noCodecSupported) {
            codecHintHtml = `<div class="settings-status settings-status-pending u-mb-2">
                <strong>${t('settings.noCodecSupportedTitle')}</strong> ${t('settings.noCodecSupportedBody')}
            </div>`;
        }

        // Resolution options (short labels: "1080p")
        const heights = [
            { value: 720, label: '720p' },
            { value: 1080, label: '1080p' },
            { value: 1440, label: '1440p' },
            { value: 2160, label: '2160p' },
        ];
        const heightOptions = heights
            .map(
                (h) =>
                    `<option value="${h.value}" ${h.value === this._streamHeight ? 'selected' : ''}>${this.esc(h.label)}</option>`,
            )
            .join('');

        // Aspect options: "Auto" (measured from the host, see AspectProbe) then
        // the real screen formats, most common first.
        const aspectOptions = [{ value: 'auto', label: t('settings.aspectAuto') }]
            .concat(SCREEN_ASPECTS.map((a) => ({ value: a.value, label: a.value })))
            .map(
                (a) =>
                    `<option value="${a.value}" ${a.value === this._streamAspect ? 'selected' : ''}>${this.esc(a.label)}</option>`,
            )
            .join('');

        // FPS options
        const fpsValues = [15, 30, 60, 75, 90, 120, 144, 165, 240];
        const fpsOptions = fpsValues
            .map(
                (f) =>
                    `<option value="${f}" ${f === this._streamFps ? 'selected' : ''}>${this.esc(t('settings.fpsSuffix', { fps: f }))}</option>`,
            )
            .join('');

        // Video Enhancement (upscale/sharpen). The WebGPU flavours are grayed
        // out when WebGPU is unavailable, like the per-codec graying; the
        // WebGL2 ones stay, so the feature is not lost with the API.
        const webgpuUnavailable = !this._webgpuUsable;
        const veAlgos = [
            { value: 'auto', label: t('settings.algoAuto'), disabled: false },
            ...ENHANCER_CHOICES.map((c) => ({
                value: c.value,
                label: t(c.key),
                disabled: c.gpu && webgpuUnavailable,
            })),
        ];
        const veAlgoOptions = veAlgos
            .map(
                (a) =>
                    `<option value="${a.value}" ${a.value === this._videoEnhancementAlgo ? 'selected' : ''}${a.disabled ? ' disabled' : ''}>${this.esc(a.label)}</option>`,
            )
            .join('');

        // Gamepad profile — debug builds only. In production the auto-detection
        // IS the behaviour: a visible switch would turn "we guessed your pad
        // wrong" into a question the user has no way to answer. In debug it is
        // the only way to tell a detection bug from a mapping bug.
        const gamepadProfileHtml = this._debugBuild
            ? `
                    <div class="settings-field">
                        <label class="settings-label" for="settings-gamepad-profile">
                            ${t('settings.gamepadProfile')}
                        </label>
                        <span class="setting-desc">${t('settings.gamepadProfileDesc')}</span>
                        <select id="settings-gamepad-profile" class="settings-select">
                            ${[
                                { value: 'auto', label: t('settings.gamepadProfileAuto') },
                                { value: 'x360', label: t('settings.gamepadProfileX360') },
                                { value: 'ds4', label: t('settings.gamepadProfileDs4') },
                            ]
                                .map(
                                    (p) =>
                                        `<option value="${p.value}" ${p.value === this._gamepadProfile ? 'selected' : ''}>${this.esc(p.label)}</option>`,
                                )
                                .join('')}
                        </select>
                    </div>`
            : '';
        // Click-to-photon latency flag — debug builds on a Windows host only.
        // A host-side switch (the overlay lives on the host's screen), saved
        // through the same form; the server takes it only from localhost.
        const latencyFlagHtml = this._latencyFlagSupported
            ? `
                    <div class="settings-field">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="settings-latency-flag"
                                ${this._latencyFlag ? 'checked' : ''} />
                            <span class="settings-checkbox-text">
                                <strong>${t('settings.latencyFlag')}</strong>
                            </span>
                        </label>
                        <span class="setting-desc">${t('settings.latencyFlagDesc')}</span>
                    </div>`
            : '';
        const veNote = webgpuUnavailable
            ? `<div class="settings-note">${t('settings.webgpuUnavailable')}</div>`
            : '';

        // HDR + Enhancer: the stream is tone-mapped HDR→SDR in the renderer's
        // Pass 0, so FSR1/SGSR run on a normal SDR canvas. Show an informational
        // note while HDR is on (the tone-map costs a software AV1 decode).
        const veLockedClass = this._powerSave ? ' settings-field-locked' : '';
        const veCheckboxDisabled = this._powerSave ? ' disabled' : '';
        const veChecked = this._videoEnhancement === 'on' ? 'checked' : '';
        const veHdrNote = this._hdrEnabled
            ? `<div class="settings-note">${t('settings.videoEnhancementHdrNote')}</div>`
            : '';

        this.container.innerHTML = `
            <div class="settings-view" id="view-settings">
                <div class="settings-header">
                    <h2>${t('settings.title')}</h2>
                    <button class="view-close-btn" id="btn-settings-close"
                            title="${this.esc(t('common.close'))}">&times;</button>
                </div>

                <!-- ── Video ─────────────────────────────────────────────── -->
                <div class="settings-section">
                    <h3 class="settings-section-title">${t('settings.video')}</h3>

                    <div class="settings-field">
                        <label class="settings-label" for="settings-stream-height">
                            ${t('settings.resolution')}
                        </label>
                        <span class="setting-desc">${t('settings.resolutionDesc')}</span>
                        <select id="settings-stream-height" class="settings-select">
                            ${heightOptions}
                        </select>
                    </div>

                    <div class="settings-field">
                        <label class="settings-label" for="settings-stream-aspect">
                            ${t('settings.streamAspect')}
                        </label>
                        <span class="setting-desc">${t('settings.streamAspectDesc')}</span>
                        <select id="settings-stream-aspect" class="settings-select">
                            ${aspectOptions}
                        </select>
                    </div>

                    <div class="settings-field">
                        <label class="settings-label" for="settings-stream-fps">
                            ${t('settings.frameRate')}
                        </label>
                        <span class="setting-desc">${t('settings.frameRateDesc')}</span>
                        <select id="settings-stream-fps" class="settings-select">
                            ${fpsOptions}
                        </select>
                    </div>

                    <!-- HDR: requires HEVC (or AV1 HDR profile) and a WebGPU-capable browser.
                         Shown when the app supports HDR; otherwise grayed out with the noWebgpu message. -->
                    <div class="settings-field${hdrLocked}">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="settings-hdr"
                                ${hdrChecked}${hdrDisabled} />
                            <span class="settings-checkbox-text">
                                <strong>${t('settings.hdr')}</strong>
                            </span>
                        </label>
                        <span class="setting-desc">${t('settings.hdrDesc')}</span>
                    </div>

                    <div class="settings-field">
                        <label class="settings-label" for="settings-stream-bitrate">
                            ${t('settings.bitrate')} <strong id="settings-bitrate-value">${this._streamBitrateMbps}</strong> ${t('settings.bitrateUnit')}
                        </label>
                        <span class="setting-desc">${t('settings.bitrateDesc')}</span>
                        <input type="range" id="settings-stream-bitrate"
                               class="settings-slider"
                               min="1" max="150" step="1"
                               value="${this._streamBitrateMbps}" />
                        <div class="settings-slider-labels">
                            <span>1 Mbps</span>
                            <span>150 Mbps</span>
                        </div>
                    </div>
                </div>

                <!-- ── Advanced ────────────────────────────────────────────── -->
                <div class="settings-section">
                    <h3 class="settings-section-title">${t('settings.advanced')}</h3>

                    <div class="settings-field${veLockedClass}">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="settings-video-enhancement"
                                ${veChecked}${veCheckboxDisabled} />
                            <span class="settings-checkbox-text">
                                <strong>${t('settings.videoEnhancement')}</strong>
                            </span>
                        </label>
                        <span class="setting-desc">${t('settings.videoEnhancementDesc')}</span>
                        <select id="settings-video-enhancement-algo" class="settings-select u-mt-2"${veCheckboxDisabled}>
                            ${veAlgoOptions}
                        </select>
                        ${veHdrNote}
                        ${veNote}
                    </div>

                    <div class="settings-field${psLocked}">
                        <label class="settings-label" for="settings-video-codec">
                            ${t('settings.videoCodec')}
                        </label>
                        <span class="setting-desc">${t('settings.videoCodecDesc')}</span>
                        <select id="settings-video-codec" class="settings-select"${psDisabled}>
                            ${codecOptions}
                        </select>
                        ${codecHintHtml}
                    </div>

                    <div class="settings-field">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="settings-mute-host"
                                ${this._muteHostAudio ? 'checked' : ''} />
                            <span class="settings-checkbox-text">
                                <strong>${t('settings.muteHost')}</strong>
                            </span>
                        </label>
                        <span class="setting-desc">${t('settings.muteHostDesc')}</span>
                    </div>

                    <!-- Allow tearing: disables VSync pacing + desynchronized
                         canvas. Dimmed + locked "(unavailable)" outside Chromium
                         desktop — Safari/Firefox ignore the flag and mobile
                         compositors always re-synchronize. -->
                    <div class="settings-field${tearingLocked}">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="settings-tearing"
                                ${tearingChecked}${tearingDisabled} />
                            <span class="settings-checkbox-text">
                                <strong>${t('settings.tearing')}${tearingUnavailable ? t('settings.unavailableSuffix') : ''}</strong>
                            </span>
                        </label>
                        <span class="setting-desc">${t('settings.tearingDesc')}</span>
                    </div>

                    <div class="settings-field${psLocked}">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="settings-chroma-444"
                                ${this._chroma444 ? 'checked' : ''}${psDisabled} />
                            <span class="settings-checkbox-text">
                                <strong>${t('settings.chroma444')}</strong>
                            </span>
                        </label>
                        <span class="setting-desc">${t('settings.chroma444Desc')}</span>
                    </div>
                    ${gamepadProfileHtml}
                    ${latencyFlagHtml}

                    ${
                        IS_TOUCH_DEVICE
                            ? ''
                            : `
                    <div class="settings-field">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="settings-gaming-mode"
                                ${this._gamingMode ? 'checked' : ''} />
                            <span class="settings-checkbox-text">
                                <strong>${t('settings.gamingMode')}</strong>
                            </span>
                        </label>
                        <span class="setting-desc">${t('settings.gamingModeDesc')}</span>
                    </div>`
                    }

                    <!-- Hidden: too technical for the average user. Kept in the DOM
                         (display:none) as a debug override; defaults to 'auto'. -->
                    <div class="settings-field u-hidden">
                        <label class="settings-label" for="settings-video-worker">Decode on worker thread</label>
                        <select id="settings-video-worker" class="settings-select">
                            <option value="auto" ${this._videoWorker === 'auto' ? 'selected' : ''}>Auto (off)</option>
                            <option value="on" ${this._videoWorker === 'on' ? 'selected' : ''}>On</option>
                            <option value="off" ${this._videoWorker === 'off' ? 'selected' : ''}>Off</option>
                        </select>
                        <span class="setting-desc">Decodes &amp; renders video off the UI thread (OffscreenCanvas). <strong>Auto</strong> keeps it off: measured slower on macOS (22 ms) and no faster elsewhere. Falls back automatically if unsupported. DataChannel/WSS transports only.</span>
                    </div>

                    <!-- Finger input, mobile/tablet only. A touchscreen laptop is
                         driven by its trackpad or a mouse: both settings only act on
                         the touch path (StreamViewTouch), so neither is offered
                         there — gate on the platform type, not on IS_TOUCH_DEVICE. -->
                    ${
                        IS_MOBILE_OR_TABLET
                            ? `
                    <div class="settings-field">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="settings-touch-screen"
                                ${this._touchScreen ? 'checked' : ''} />
                            <span class="settings-checkbox-text">
                                <strong>${t('settings.touchScreen')}</strong>
                            </span>
                        </label>
                        <span class="setting-desc">${t('settings.touchScreenDesc')}</span>
                    </div>

                    <div class="settings-field">
                        <label class="settings-label" for="settings-sensitivity">
                            ${t('settings.pointerSensitivity')} <strong id="settings-sensitivity-value">${this._touchSensitivity.toFixed(1)}</strong>×
                        </label>
                        <span class="setting-desc">${t('settings.pointerSensitivityDesc')}</span>
                        <input type="range" id="settings-sensitivity"
                               class="settings-slider"
                               min="0.5" max="5" step="0.1"
                               value="${this._touchSensitivity}" />
                        <div class="settings-slider-labels">
                            <span>0.5×</span>
                            <span>5×</span>
                        </div>
                    </div>`
                            : ''
                    }

                    <div class="settings-field">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="settings-show-perf-stats"
                                ${this._showPerformanceStats ? 'checked' : ''} />
                            <span class="settings-checkbox-text">
                                <strong>${t('settings.showPerfStats')}</strong>
                            </span>
                        </label>
                        <span class="setting-desc">${t('settings.showPerfStatsDesc')}</span>
                    </div>
                </div>

                <!-- ── Language ────────────────────────────────────────────── -->
                <div class="settings-section">
                    <h3 class="settings-section-title">${t('settings.language')}</h3>
                    <div class="settings-field">
                        <select id="settings-language" class="settings-select">
                            ${AVAILABLE_LANGUAGES.map(
                                (l) =>
                                    `<option value="${l.code}" ${l.code === getLanguage() ? 'selected' : ''}>${this.esc(l.label)}</option>`,
                            ).join('')}
                        </select>
                        <span class="setting-desc">${t('settings.languageDesc')}</span>
                    </div>
                </div>

                ${
                    IS_TOUCH_DEVICE
                        ? `
                <!-- ── Power Saving (mobile only, isolated above Reset) ──────── -->
                <div class="settings-section settings-section-powersave">
                    <h3 class="settings-section-title">${t('settings.powerSave')}</h3>
                    <div class="settings-field">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="settings-power-save"
                                ${this._powerSave ? 'checked' : ''} />
                            <span class="settings-checkbox-text">
                                <strong>${t('settings.powerSaveToggle')}</strong>
                            </span>
                        </label>
                        <span class="setting-desc">${t('settings.powerSaveDesc')}</span>
                    </div>
                </div>`
                        : ''
                }

                <!-- ── Privacy ─────────────────────────────────────────────── -->
                <!-- Here rather than in Admin: it concerns everyone who streams
                     through this machine, and Admin is a door most users never
                     open. The switch is still the owner's alone — shown to all,
                     operable from the machine itself. -->
                ${this._renderPrivacySection()}

                <!-- ── Reset ───────────────────────────────────────────────── -->
                <div class="settings-section">
                    <button class="btn btn-neutral" id="btn-settings-reset">
                        ${t('settings.resetDefaults')}
                    </button>
                    <span class="setting-desc">${t('settings.resetDefaultsDesc')}</span>
                </div>

                ${
                    this._canLogout
                        ? `
                <!-- ── Session (last: it ends the visit) ──────────────────── -->
                <div class="settings-section settings-section-logout">
                    <h3 class="settings-section-title">${t('settings.session')}</h3>
                    <button class="btn btn-danger" id="btn-settings-logout">
                        ${t('settings.logout')}
                    </button>
                    <span class="setting-desc">${t('settings.logoutDesc')}</span>
                </div>`
                        : ''
                }
            </div>
        `;

        // Live bitrate label updates on slider input
        const slider = this.container.querySelector('#settings-stream-bitrate');
        const label = this.container.querySelector('#settings-bitrate-value');
        if (slider && label) {
            slider.addEventListener('input', () => {
                label.textContent = slider.value;
            });
        }

        // Live sensitivity label updates while dragging
        const sensSlider = this.container.querySelector('#settings-sensitivity');
        const sensLabel = this.container.querySelector('#settings-sensitivity-value');
        if (sensSlider && sensLabel) {
            sensSlider.addEventListener('input', () => {
                sensLabel.textContent = parseFloat(sensSlider.value).toFixed(1);
            });
        }
    }

    bindEvents() {
        // Anonymous statistics: consent given, or withdrawn, on the spot.
        const statsChk = this.container.querySelector('#settings-stats-consent');
        if (statsChk) {
            statsChk.addEventListener('change', () => this._setStatsConsent(statsChk));
        }

        const codecSelect = this.container.querySelector('#settings-video-codec');
        if (codecSelect) codecSelect.addEventListener('change', () => this._autoSave());

        // Resolution / FPS / HDR changes recompute the recommended bitrate
        // from the 1080p60 SDR reference before saving.
        const heightSelect = this.container.querySelector('#settings-stream-height');
        if (heightSelect)
            heightSelect.addEventListener('change', () => {
                this._applyAutoBitrate();
                this._autoSave();
            });

        // Aspect: an ultrawide frame is more pixels at the same height, so the
        // recommended bitrate follows it too.
        const aspectSelect = this.container.querySelector('#settings-stream-aspect');
        if (aspectSelect)
            aspectSelect.addEventListener('change', () => {
                this._streamAspect = aspectSelect.value;
                this._applyAutoBitrate();
                this._autoSave();
            });

        const fpsSelect = this.container.querySelector('#settings-stream-fps');
        if (fpsSelect)
            fpsSelect.addEventListener('change', () => {
                this._applyAutoBitrate();
                this._autoSave();
            });

        const hdrCheck = this.container.querySelector('#settings-hdr');
        if (hdrCheck)
            hdrCheck.addEventListener('change', () => {
                this._hdrEnabled = hdrCheck.checked;
                // Re-render so the codec options and the enhancer HDR note update.
                this._applyAutoBitrate();
                this.render();
                this.bindEvents();
                this._autoSave();
            });

        // 4:4:4 chroma bumps the recommended bitrate (higher bandwidth) before saving.
        const chroma444Check = this.container.querySelector('#settings-chroma-444');
        if (chroma444Check)
            chroma444Check.addEventListener('change', () => {
                this._applyAutoBitrate();
                this._autoSave();
            });

        const muteHostCheck = this.container.querySelector('#settings-mute-host');
        if (muteHostCheck)
            muteHostCheck.addEventListener('change', () => {
                this._muteHostAudio = muteHostCheck.checked;
                this._autoSave();
            });

        const bitrateSlider = this.container.querySelector('#settings-stream-bitrate');
        if (bitrateSlider) bitrateSlider.addEventListener('change', () => this._autoSave());

        const gamingCheck = this.container.querySelector('#settings-gaming-mode');
        if (gamingCheck) gamingCheck.addEventListener('change', () => this._autoSave());

        const touchScreenCheck = this.container.querySelector('#settings-touch-screen');
        if (touchScreenCheck) touchScreenCheck.addEventListener('change', () => this._autoSave());

        const perfCheck = this.container.querySelector('#settings-show-perf-stats');
        if (perfCheck) perfCheck.addEventListener('change', () => this._autoSave());

        const tearingCheck = this.container.querySelector('#settings-tearing');
        if (tearingCheck) tearingCheck.addEventListener('change', () => this._autoSave());

        const workerCheck = this.container.querySelector('#settings-video-worker');
        if (workerCheck) workerCheck.addEventListener('change', () => this._autoSave());

        const veSelect = this.container.querySelector('#settings-video-enhancement');
        if (veSelect) veSelect.addEventListener('change', () => this._autoSave());

        const veAlgoSelect = this.container.querySelector('#settings-video-enhancement-algo');
        if (veAlgoSelect) veAlgoSelect.addEventListener('change', () => this._autoSave());

        const sensSlider = this.container.querySelector('#settings-sensitivity');
        if (sensSlider) sensSlider.addEventListener('change', () => this._autoSave());

        // Language selector — changing it persists the choice and reloads.
        const langSelect = this.container.querySelector('#settings-language');
        if (langSelect) langSelect.addEventListener('change', () => setLanguage(langSelect.value));

        const powerSaveCheck = this.container.querySelector('#settings-power-save');
        if (powerSaveCheck)
            powerSaveCheck.addEventListener('change', () =>
                this._togglePowerSave(powerSaveCheck.checked),
            );

        const resetBtn = this.container.querySelector('#btn-settings-reset');
        if (resetBtn) resetBtn.addEventListener('click', () => this._resetDefaults());

        const logoutBtn = this.container.querySelector('#btn-settings-logout');
        if (logoutBtn) logoutBtn.addEventListener('click', () => this._logout(logoutBtn));

        const closeBtn = this.container.querySelector('#btn-settings-close');
        if (closeBtn) closeBtn.addEventListener('click', () => this.onClose());
    }

    /**
     * End this browser's session and go back to the PIN page.
     *
     * The reload is not cosmetic: the backend only checks the session cookie
     * when a WebSocket is upgraded, so a stream already running would survive
     * the logout. Reloading drops every open socket and re-enters the app
     * through _checkAuth(), which now finds no session.
     */
    async _logout(btn) {
        if (!window.confirm(t('settings.logoutConfirm'))) return;

        btn.disabled = true;
        try {
            await BackendClient.logout();
        } catch (err) {
            // The cookie may be gone anyway (expired, already revoked). Reload
            // regardless: if it is not, the login page is what comes back.
            console.warn('[Settings] Logout request failed:', err);
        }
        window.location.replace(window.location.pathname);
    }

    // --- Helpers ---

    esc(text) {
        return escapeHtml(text);
    }
}
