/*
 * MoonlightWeb — the UDP that failed toward a host, remembered.
 * Copyright (C) 2026 Bruno Martin. GPLv3 — see repository LICENSE.
 */

/**
 * Where the UDP rungs of the transport chain failed toward a host and a later
 * rung carried the stream, the next launches to that host start past them.
 *
 * Issue #48: a car on LTE whose UDP answers the ICE checks, then carries
 * nothing. The STUN probe (api/UdpProbe.js) found UDP getting out, so every
 * launch spent its UDP attempts, about fourteen seconds, before ICE-TCP
 * streamed — and the same again at the next launch.
 *
 * What is kept, in this browser only (localStorage): the host's identifier,
 * when, how many launches have skipped UDP since, and the kind of network the
 * browser says it is on (navigator.connection.type: wifi, cellular… or
 * nothing where it does not say). No address and no candidate. UDP is tried
 * again after UDP_MEMORY_SKIPS launches or UDP_MEMORY_MAX_AGE_MS, whichever
 * comes first: a network changes, and one failed attempt then is the whole
 * cost of finding it mended.
 */

const KEY = 'mw_udp_failed';

/** How long a failure is held against UDP toward a host. */
export const UDP_MEMORY_MAX_AGE_MS = 24 * 60 * 60 * 1000;

/** How many launches skip UDP before it is tried again. */
export const UDP_MEMORY_SKIPS = 5;

/** Hosts remembered at most; the oldest failure goes first. */
const MAX_HOSTS = 16;

/**
 * The end of a transport's failure reason when ICE connected and was lost
 * before a single data channel opened (WebRtcDataChannel).
 */
export const ICE_LOST_BEFORE_CHANNELS = ' before the channels opened';

/** ICE connected, then went before any channel opened. */
export function iceLostBeforeChannels(reason) {
    return String(reason).endsWith(ICE_LOST_BEFORE_CHANNELS);
}

/**
 * The failure is the path's: ICE ran out of time, or failed, or went before
 * the stream showed. A refused key, a decoder or a host that did not start the
 * stream say nothing about the network.
 */
export function pathFailure(reason) {
    return /^ICE (timeout|disconnected|failed)\b/.test(String(reason));
}

/** localStorage, or null where reading it throws (sandboxed frame, privacy mode). */
function store() {
    try {
        return typeof localStorage !== 'undefined' ? localStorage : null;
    } catch (_e) {
        return null;
    }
}

/**
 * A word for the kind of network this browser is on, never an address: what
 * navigator.connection says (wifi, cellular, ethernet…), '' where it says
 * nothing — most desktop browsers, and the car's.
 */
export function networkKind(nav = globalThis.navigator) {
    try {
        const type = nav && nav.connection && nav.connection.type;
        return typeof type === 'string' && /^[a-z]{1,12}$/.test(type) ? type : '';
    } catch (_e) {
        return '';
    }
}

function validHost(id) {
    return typeof id === 'string' && /^[\w-]{1,64}$/.test(id);
}

function validEntry(e) {
    return (
        !!e &&
        typeof e === 'object' &&
        Number.isFinite(e.at) &&
        Number.isInteger(e.skips) &&
        e.skips >= 0 &&
        typeof e.net === 'string'
    );
}

function readAll(s) {
    try {
        const all = JSON.parse(s.getItem(KEY) || '{}');
        return all && typeof all === 'object' && !Array.isArray(all) ? all : {};
    } catch (_e) {
        return {};
    }
}

function writeAll(s, all) {
    try {
        if (Object.keys(all).length === 0) s.removeItem(KEY);
        else s.setItem(KEY, JSON.stringify(all));
    } catch (_e) {
        // Not kept: UDP is tried at the next launch, as without this memory.
    }
}

/**
 * Whether this launch to @p hostId skips UDP, and if so one skip is spent.
 * A failure past its age or its skips is forgotten here, and UDP is tried.
 *
 * @returns {{skip: number, of: number}|null} the skip this launch is (1-based)
 *   out of how many, or null when UDP is tried.
 */
export function skipUdpFor(hostId, { now = Date.now(), net = networkKind(), s = store() } = {}) {
    if (!s || !validHost(hostId)) return null;
    const all = readAll(s);
    const e = all[hostId];
    if (e === undefined) return null;
    if (
        !validEntry(e) ||
        now < e.at ||
        now - e.at >= UDP_MEMORY_MAX_AGE_MS ||
        e.skips >= UDP_MEMORY_SKIPS
    ) {
        delete all[hostId];
        writeAll(s, all);
        return null;
    }
    // Failed on another kind of network: this one has not been tried.
    if (e.net !== net) return null;
    e.skips += 1;
    writeAll(s, all);
    return { skip: e.skips, of: UDP_MEMORY_SKIPS };
}

/** UDP failed toward @p hostId and a later rung carried the stream. */
export function rememberUdpFailed(
    hostId,
    { now = Date.now(), net = networkKind(), s = store() } = {},
) {
    if (!s || !validHost(hostId)) return;
    const all = readAll(s);
    for (const id of Object.keys(all)) if (!validHost(id) || !validEntry(all[id])) delete all[id];
    all[hostId] = { at: now, skips: 0, net };
    const ids = Object.keys(all).sort((a, b) => all[b].at - all[a].at);
    for (const id of ids.slice(MAX_HOSTS)) delete all[id];
    writeAll(s, all);
}

/** UDP carried a stream to @p hostId on this kind of network: nothing held against it. */
export function forgetUdpFailed(hostId, { net = networkKind(), s = store() } = {}) {
    if (!s || !validHost(hostId)) return;
    const all = readAll(s);
    const e = all[hostId];
    if (e === undefined || (validEntry(e) && e.net !== net)) return;
    delete all[hostId];
    writeAll(s, all);
}
