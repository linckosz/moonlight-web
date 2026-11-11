/**
 * CodecAuto — « Auto », the first codec of the Settings menu and its default.
 *
 * Auto streams in HEVC, as « HEVC (recommended) » did before it, and in AV1
 * where HEVC cannot do the job: an HDR stream with the enhancer asked for,
 * from a host that encodes AV1 10-bit, in a browser that decodes it in
 * software (BrowserDetect.hdrLaunchCodec). The launch resolves it to one of
 * the two before asking the host: the host never sees « auto ». A codec
 * chosen in the menu is kept as chosen.
 *
 * A stored HEVC cannot say whether it was chosen or only left as the default,
 * so it becomes Auto once per browser (codecAfterAutoMigration). Auto is HEVC
 * but where HEVC fails: nothing is lost, and a HEVC picked afterwards stays.
 *
 * Where HEVC failed, Auto remembers it (hevcFailedHere) and asks AV1 from the
 * next launch on, of a host that encodes it, instead of failing HEVC first at
 * every launch; H.264 where AV1 failed too (av1FailedHere), or from a host
 * without AV1. AV1 rather than H.264, by Bruno's decision (07/11/2026): at
 * the bitrate Auto picks, AV1 was faster from click to photon than H.264 in
 * every case measured (3 to 10 ms, Firefox and Chrome, 1080p and 1440p),
 * under 2 client cores, and as good or better in VMAF NEG
 * (docs/design/moonlight-web-booster.md, §16).
 *
 * That was measured on strong clients. On a weak one an AV1 decoded in
 * software does not keep up, and fails without an error: a N95 decoding
 * 1440p60 AV1 in software tops at 62 frames a second with half a second of
 * delay, where its hardware H.264 takes 1 ms a frame (10/11/2026). So AV1 is
 * asked only where it is decoded by the hardware (av1HardwareHere), or, in
 * Firefox, where an AV1 stream already ran smoothly (av1ProvenHere); H.264
 * otherwise. And an AV1 that does not keep up is a failure like the others
 * (Av1Watch): H.264 from the next launch on.
 */

export const CODEC_AUTO = 'auto';

/** Set once this browser's stored codec has been through the move to Auto. */
const MIGRATED_KEY = 'mw_codec_auto';

/** localStorage, or null where reading it throws (sandboxed frame, privacy mode). */
function store() {
    try {
        return typeof localStorage !== 'undefined' ? localStorage : null;
    } catch (e) {
        return null;
    }
}

/**
 * The codec setting after the one move of a stored HEVC to Auto. A missing
 * codec is left missing, and leaves the move for the first one stored (the
 * server's default, on a first visit to the Settings page).
 * @param {string|undefined} codec the codec as stored
 * @param {Storage|null} [s] where the move is remembered
 * @returns {string|undefined}
 */
export function codecAfterAutoMigration(codec, s = store()) {
    if (typeof codec !== 'string' || !codec) return codec;
    try {
        if (!s || s.getItem(MIGRATED_KEY) === '1') return codec;
        s.setItem(MIGRATED_KEY, '1');
    } catch (e) {
        // Neither read nor kept: the codec as it is, asked again next time.
        return codec;
    }
    return codec === 'hevc' ? CODEC_AUTO : codec;
}

/**
 * The same move on the settings this browser stored (`mw-streaming-settings`),
 * written back when it changed them. Run once, at start-up, before anything
 * reads them.
 * @param {Storage|null} [s]
 */
export function migrateStoredCodecToAuto(s = store()) {
    if (!s) return;
    try {
        const raw = s.getItem('mw-streaming-settings');
        if (!raw) return;
        const settings = JSON.parse(raw);
        if (!settings || typeof settings !== 'object') return;
        const codec = codecAfterAutoMigration(settings.video_codec, s);
        if (codec !== settings.video_codec) {
            settings.video_codec = codec;
            s.setItem('mw-streaming-settings', JSON.stringify(settings));
            console.log('[MW] Codec: the HEVC stored here is Auto from now on');
        }
    } catch (e) {
        // Unreadable settings are the Settings page's to repair, not this.
    }
}

/** The browsers whose HEVC failed to decode: user agent → when (ms). */
const HEVC_FAILED_KEY = 'mw_hevc_failed';
/** The same for AV1, asked by Auto where HEVC had failed. */
const AV1_FAILED_KEY = 'mw_av1_failed';

/** The browsers where an AV1 stream ran smoothly: user agent → when (ms). */
const AV1_PROVEN_KEY = 'mw_av1_proven';

/** How long such a verdict holds. Past it, Auto asks HEVC again. */
export const HEVC_FAILED_DAYS = 30;
const HEVC_FAILED_MS = HEVC_FAILED_DAYS * 24 * 3600 * 1000;

const userAgent = () => (typeof navigator !== 'undefined' && navigator.userAgent) || '';

/** @param {Storage|null} s @param {string} key @returns {Object<string, number>} */
function verdicts(s, key) {
    try {
        const kept = s && JSON.parse(s.getItem(key) || '{}');
        return kept && typeof kept === 'object' ? kept : {};
    } catch (e) {
        return {};
    }
}

/**
 * Whether this browser's HEVC was seen failing: the decoder refused every
 * configuration, or gave no picture, and the stream fell back to H.264.
 * Firefox on the UM790Pro asked HEVC under Auto, failed it in 0.8 s and came
 * back in H.264, at every launch (28/10/2026). The verdict is kept under the
 * user agent (a new version of the browser is asked again) and for
 * HEVC_FAILED_DAYS at most.
 * @param {string} [ua]
 * @param {Storage|null} [s]
 * @param {number} [now] ms since the epoch
 * @returns {boolean}
 */
export function hevcFailedHere(ua = userAgent(), s = store(), now = Date.now()) {
    return failedHere(HEVC_FAILED_KEY, ua, s, now);
}

/**
 * Whether this browser's AV1 was seen failing in the same way (hevcFailedHere),
 * kept as long.
 * @param {string} [ua]
 * @param {Storage|null} [s]
 * @param {number} [now]
 * @returns {boolean}
 */
export function av1FailedHere(ua = userAgent(), s = store(), now = Date.now()) {
    return failedHere(AV1_FAILED_KEY, ua, s, now);
}

function failedHere(key, ua, s, now) {
    const seen = verdicts(s, key)[ua];
    // A date ahead of the clock (the clock set back) holds no longer either.
    return typeof seen === 'number' && now >= seen && now - seen < HEVC_FAILED_MS;
}

/**
 * Remember it (hevcFailedHere), dropping the verdicts past their time.
 * @param {string} [ua]
 * @param {Storage|null} [s]
 * @param {number} [now]
 */
export function noteHevcFailed(ua = userAgent(), s = store(), now = Date.now()) {
    noteFailed(HEVC_FAILED_KEY, ua, s, now);
}

/**
 * Remember it for AV1 (av1FailedHere). A stream that ran smoothly here before
 * (av1ProvenHere) is no longer the last word.
 * @param {string} [ua]
 * @param {Storage|null} [s]
 * @param {number} [now]
 */
export function noteAv1Failed(ua = userAgent(), s = store(), now = Date.now()) {
    noteFailed(AV1_FAILED_KEY, ua, s, now);
    forget(AV1_PROVEN_KEY, ua, s);
}

/**
 * Whether an 8-bit AV1 stream ran smoothly in this browser (Av1Watch), kept as
 * long as a failure and in the same way.
 * @param {string} [ua]
 * @param {Storage|null} [s]
 * @param {number} [now]
 * @returns {boolean}
 */
export function av1ProvenHere(ua = userAgent(), s = store(), now = Date.now()) {
    return failedHere(AV1_PROVEN_KEY, ua, s, now);
}

/**
 * Remember it (av1ProvenHere). An earlier failure is no longer the last word.
 * @param {string} [ua]
 * @param {Storage|null} [s]
 * @param {number} [now]
 */
export function noteAv1Proven(ua = userAgent(), s = store(), now = Date.now()) {
    noteFailed(AV1_PROVEN_KEY, ua, s, now);
    forget(AV1_FAILED_KEY, ua, s);
}

function noteFailed(key, ua, s, now) {
    if (!s) return;
    try {
        const kept = {};
        for (const [k, seen] of Object.entries(verdicts(s, key))) {
            if (typeof seen === 'number' && now >= seen && now - seen < HEVC_FAILED_MS)
                kept[k] = seen;
        }
        kept[ua] = now;
        s.setItem(key, JSON.stringify(kept));
    } catch (e) {
        // Not kept: the next launch finds out again, a relaunch later.
    }
}

function forget(key, ua, s) {
    if (!s) return;
    try {
        const kept = verdicts(s, key);
        if (!(ua in kept)) return;
        delete kept[ua];
        s.setItem(key, JSON.stringify(kept));
    } catch (e) {
        // Left as it is: both verdicts lapse by themselves.
    }
}

/** A host's codec mode bits for AV1, any profile (moonlight-common-c, SCM_MASK_AV1). */
export const SCM_MASK_AV1 = 0xf0000;

/** The stream the hardware is asked about: what Auto launches most. */
const AV1_PROBE = { codec: 'av01.0.08M.08', width: 1920, height: 1080, fps: 60, bitrate: 10e6 };
/** How long a launch waits for the browser's answer. */
const AV1_PROBE_MS = 1000;

let av1Hardware = null;

/** Test hook: ask the browser again. */
export function resetAv1HardwareHere() {
    av1Hardware = null;
}

/**
 * Whether this browser says it decodes AV1 with the hardware, asked once per
 * page.
 *
 * Chrome answers `VideoDecoder.isConfigSupported` with `prefer-hardware`
 * truthfully: no with hardware decoding off, yes on a machine that has it.
 * Firefox says yes to that whatever it has, so it is asked through
 * `mediaCapabilities.decodingInfo` instead, whose `powerEfficient` follows its
 * hardware decoding (true by default, false with
 * media.hardware-video-decoding.enabled off; Firefox 159, 11/11/2026). A
 * browser without WebCodecs is asked the same way. No answer, an error or an
 * answer that comes too late: no.
 * @param {{gecko?: boolean, decoder?: any, capabilities?: any, timeoutMs?: number}} [opts]
 * @returns {Promise<boolean>}
 */
export function av1HardwareHere(opts = {}) {
    if (!av1Hardware) av1Hardware = askAv1Hardware(opts).catch(() => false);
    return av1Hardware;
}

async function askAv1Hardware({ gecko = false, decoder, capabilities, timeoutMs } = {}) {
    const vd =
        decoder !== undefined ? decoder : typeof VideoDecoder !== 'undefined' && VideoDecoder;
    const mc =
        capabilities !== undefined
            ? capabilities
            : typeof navigator !== 'undefined' && /** @type {any} */ (navigator).mediaCapabilities;
    let asked;
    if (!gecko && vd && typeof vd.isConfigSupported === 'function') {
        asked = vd
            .isConfigSupported({
                codec: AV1_PROBE.codec,
                codedWidth: AV1_PROBE.width,
                codedHeight: AV1_PROBE.height,
                hardwareAcceleration: 'prefer-hardware',
            })
            .then((r) => !!(r && r.supported));
    } else if (mc && typeof mc.decodingInfo === 'function') {
        asked = mc
            .decodingInfo({
                type: 'webrtc',
                video: {
                    contentType: 'video/AV1',
                    width: AV1_PROBE.width,
                    height: AV1_PROBE.height,
                    bitrate: AV1_PROBE.bitrate,
                    framerate: AV1_PROBE.fps,
                },
            })
            .then((r) => !!(r && r.supported && r.powerEfficient));
    } else {
        return false;
    }
    let timer = null;
    const late = new Promise((resolve) => {
        timer = setTimeout(() => resolve(false), timeoutMs || AV1_PROBE_MS);
    });
    try {
        return await Promise.race([asked, late]);
    } finally {
        clearTimeout(timer);
    }
}

/**
 * The codec an Auto launch asks the host for: HEVC; in a browser whose HEVC
 * failed (hevcFailedHere), AV1 of a host that encodes it (SCM_MASK_AV1), where
 * the hardware decodes it (av1HardwareHere) or, in Firefox, where it already
 * ran smoothly (av1ProvenHere); H.264 otherwise, where AV1 failed
 * (av1FailedHere), or from a host without AV1. The HDR route may still turn
 * HEVC into AV1 (BrowserDetect.hdrLaunchCodec).
 * @param {number} [hostCodecs] the host's serverCodecModeSupport
 * @param {string} [ua]
 * @param {Storage|null} [s]
 * @param {number} [now]
 * @param {{hardware?: boolean, gecko?: boolean}} [av1] what av1HardwareHere
 *   answered, and whether this is Firefox
 * @returns {'hevc'|'av1'|'h264'}
 */
export function autoLaunchCodec(hostCodecs = 0, ua, s, now, av1 = {}) {
    if (!hevcFailedHere(ua, s, now)) return 'hevc';
    if ((hostCodecs & SCM_MASK_AV1) === 0 || av1FailedHere(ua, s, now)) return 'h264';
    if (av1.hardware === true) return 'av1';
    if (av1.gecko === true && av1ProvenHere(ua, s, now)) return 'av1';
    return 'h264';
}

/** A decoder queue this full on average is one that does not keep up: the
 *  stream stops feeding it at 8 (StreamView, DECODE_QUEUE_MAX), and a decoder
 *  that keeps up holds 0 or 1. */
export const AV1_WATCH_QUEUE = 4;
/** Pictures a second from which a stream is moving. A desktop that stands
 *  still sends two, and decoding those says nothing of the decoder. */
export const AV1_WATCH_MOVING_FPS = 24;
/** That much time spent not keeping up, within the first minute of moving
 *  pictures, is a failure. */
export const AV1_WATCH_FAIL_MS = 5000;
export const AV1_WATCH_FIRST_MS = 60 * 1000;
/** That long in moving pictures without it ran smoothly. */
export const AV1_WATCH_PROVEN_MS = 120 * 1000;

/**
 * Whether an AV1 stream keeps up, read now and then from the decoder's queue
 * (PipelineDiag, decodeQueueAvg), the pictures arriving, and the decode-rate
 * governor. An AV1 decoded in software on a client too weak for it raises no
 * error: its queue fills and the stream falls behind for as long as it lasts
 * (a N95 on 1440p60: 39 pictures decoded of the 60 a second, a thousand
 * waiting after 50 s — 11/11/2026). Where a native host was asked for fewer
 * pictures the queue empties again: that is no decoder keeping up either.
 *
 * Time counts only while pictures move (AV1_WATCH_MOVING_FPS): two minutes of
 * a still desktop are not two minutes of decoding. One verdict per stream:
 * 'failed' once the decoder did not keep up for AV1_WATCH_FAIL_MS within the
 * first minute of moving pictures, or 'proven' after AV1_WATCH_PROVEN_MS of
 * them that never added up to that.
 */
export class Av1Watch {
    constructor() {
        this._last = null;
        this._movingMs = 0;
        this._behindMs = 0;
        this._over = false;
    }

    /**
     * @param {{queue?: number, fps?: number, capped?: boolean}} reading since
     *   the last one: the decoder queue's average, the pictures arriving a
     *   second, and whether the host is being asked for fewer of them
     * @param {number} now ms, any monotonic clock
     * @returns {'failed'|'proven'|null} the verdict, the one time it is reached
     */
    sample(reading, now) {
        if (this._over) return null;
        if (this._last === null) {
            this._last = now;
            return null;
        }
        // A page put away reads late: no more than two seconds a reading.
        const step = Math.min(Math.max(now - this._last, 0), 2000);
        this._last = now;
        const r = reading || {};
        const behind = r.capped === true || (r.queue || 0) >= AV1_WATCH_QUEUE;
        if (behind) this._behindMs += step;
        // A decoder behind is at work, however few pictures arrive.
        if (behind || (r.fps || 0) >= AV1_WATCH_MOVING_FPS) this._movingMs += step;
        if (this._behindMs >= AV1_WATCH_FAIL_MS) {
            // Past the first minute it is no failure, and no proof either.
            this._over = true;
            return this._movingMs <= AV1_WATCH_FIRST_MS ? 'failed' : null;
        }
        if (this._movingMs >= AV1_WATCH_PROVEN_MS) {
            this._over = true;
            return 'proven';
        }
        return null;
    }
}
