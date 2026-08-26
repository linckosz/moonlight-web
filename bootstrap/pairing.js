/*
 * MoonlightWeb — bootstrap. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * MW-BIND-v1, the browser half, as the rendezvous needs it.
 *
 * The protocol is the one in docs/design/pairing-signature.md. Two things about
 * this copy differ from the one the frontend uses on a direct connection, and
 * both come from the same fact: on the rendezvous every host is reached through
 * ONE origin.
 *
 *  1. Keys are stored per host identifier, not per origin. The frontend's copy
 *     keeps a single record because each host had its own name; here that record
 *     would be shared by every machine a person reaches, which would hand each
 *     of them the same key and make the second one look like a substitution of
 *     the first.
 *
 *  2. The host's key is trusted on first sight, the way SSH does it. There is no
 *     earlier channel to learn it on: the login screen itself arrives through
 *     the connection being bound. From then on a different key is refused, so
 *     the exposure is the first connection to a given host and nothing after it.
 *     The host prints its key fingerprint in its log and on its local admin page
 *     for anyone who wants to check that first one by hand.
 *
 * The private key is generated with `extractable: false`: this page can ask it
 * for a signature and can never read it out. That does not stop code running
 * here from USING it while it runs — see §8 of the design.
 *
 * The byte layout below must stay identical to backend/src/common/PairingCrypto.cpp
 * and to frontend/js/util/pairingCrypto.js.
 */

const DB_NAME = 'mw-pairing';
const DB_VERSION = 1;
const STORE = 'keys';

const PROTOCOL = 'MW-BIND-v1';
const ALGORITHM = { name: 'ECDSA', namedCurve: 'P-256' };
const SIGN_PARAMS = { name: 'ECDSA', hash: { name: 'SHA-256' } };

export function bytesToBase64(bytes) {
    const arr = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    let binary = '';
    for (let i = 0; i < arr.length; i++) binary += String.fromCharCode(arr[i]);
    return btoa(binary);
}

export function base64ToBytes(b64) {
    const binary = atob(b64);
    const out = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i++) out[i] = binary.charCodeAt(i);
    return out;
}

function base64Url(bytes) {
    return bytesToBase64(bytes).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}

/**
 * Length-prefixed concatenation: every field preceded by its byte length as a
 * 4-byte big-endian integer, so no field boundary is ambiguous. Plain
 * concatenation would let bytes move across a boundary — ('ab','c') and
 * ('a','bc') would hash the same — and one of these fields is a fingerprint.
 */
function buildSignedMessage(fields) {
    const parts = fields.map((f) => (typeof f === 'string' ? new TextEncoder().encode(f) : f));
    let total = 0;
    for (const p of parts) total += 4 + p.length;

    const out = new Uint8Array(total);
    const view = new DataView(out.buffer);
    let offset = 0;
    for (const p of parts) {
        view.setUint32(offset, p.length, false);
        offset += 4;
        out.set(p, offset);
        offset += p.length;
    }
    return out;
}

function hostDigestInput(hostId, browserKeyId, nonceB, fingerprintHost) {
    return buildSignedMessage([`${PROTOCOL}|host`, hostId, browserKeyId, nonceB, fingerprintHost]);
}

function browserDigestInput(hostId, nonceH, fingerprintHost, fingerprintBrowser) {
    return buildSignedMessage([
        `${PROTOCOL}|browser`,
        hostId,
        nonceH,
        fingerprintHost,
        fingerprintBrowser,
    ]);
}

// ── Storage ────────────────────────────────────────────────────────────────

function openDb() {
    return new Promise((resolve, reject) => {
        const req = indexedDB.open(DB_NAME, DB_VERSION);
        req.onupgradeneeded = () => {
            if (!req.result.objectStoreNames.contains(STORE)) req.result.createObjectStore(STORE);
        };
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
    });
}

function dbGet(db, key) {
    return new Promise((resolve, reject) => {
        const req = db.transaction(STORE, 'readonly').objectStore(STORE).get(key);
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
    });
}

function dbPut(db, key, value) {
    return new Promise((resolve, reject) => {
        const tx = db.transaction(STORE, 'readwrite');
        tx.objectStore(STORE).put(value, key);
        tx.oncomplete = () => resolve();
        tx.onerror = () => reject(tx.error);
    });
}

/** One record per host reached through this origin. */
function recordId(hostId) {
    return `mw-bind-v1:${hostId}`;
}

// ── The identity ───────────────────────────────────────────────────────────

/**
 * This browser's identity towards @p hostId, created on first use.
 *
 * Returns null when the browser gives us no safe way to do it — no WebCrypto
 * (an insecure origin) or no IndexedDB (private mode in some browsers). A caller
 * must read null as "cannot bind", never as "binding trivially succeeded".
 */
export async function loadIdentity(hostId) {
    if (!globalThis.crypto?.subtle || !globalThis.indexedDB) return null;

    const db = await openDb();
    let record = await dbGet(db, recordId(hostId));

    if (!record?.privateKey || !record?.publicKeySpki) {
        const pair = await crypto.subtle.generateKey(ALGORITHM, false, ['sign', 'verify']);
        const spki = new Uint8Array(await crypto.subtle.exportKey('spki', pair.publicKey));
        // `false` above is the whole point: the public half exports, the private
        // half never does. CryptoKey is structured-cloneable, so it survives in
        // IndexedDB without ever existing as bytes this page could read.
        record = {
            privateKey: pair.privateKey,
            publicKeySpki: spki,
            hostPublicKey: null,
        };
        await dbPut(db, recordId(hostId), record);
    }

    const keyIdBytes = new Uint8Array(await crypto.subtle.digest('SHA-256', record.publicKeySpki));

    return {
        hostId,
        privateKey: record.privateKey,
        publicKeySpki: record.publicKeySpki,
        publicKeyBase64: bytesToBase64(record.publicKeySpki),
        keyId: base64Url(keyIdBytes),
        hostPublicKey: record.hostPublicKey || null,
        nonceB: null,
    };
}

/**
 * Keep the host key this identity is now bound to.
 *
 * Refuses to replace one that is already there. That refusal is the whole value
 * of trust-on-first-use: after the first connection, a host presenting a
 * different key is either a machine that regenerated its own — in which case the
 * user clears this pairing deliberately — or the substitution this protocol
 * exists to catch, and the two must not be told apart by guessing.
 */
async function rememberHostKey(hostId, hostPublicKeyBase64) {
    const db = await openDb();
    const record = await dbGet(db, recordId(hostId));
    if (!record) return false;
    if (record.hostPublicKey && record.hostPublicKey !== hostPublicKeyBase64) return false;
    record.hostPublicKey = hostPublicKeyBase64;
    await dbPut(db, recordId(hostId), record);
    return true;
}

/** Forget this host, so the next visit pairs from scratch. */
export async function forgetHost(hostId) {
    if (!globalThis.indexedDB) return;
    try {
        const db = await openDb();
        await dbPut(db, recordId(hostId), undefined);
    } catch {
        /* nothing to clear */
    }
}

export function generateNonce() {
    const bytes = new Uint8Array(32);
    crypto.getRandomValues(bytes);
    return bytes;
}

/** The hello that opens the exchange (design §4 step 0). */
export function helloMessage(identity) {
    identity.nonceB = generateNonce();
    return {
        type: 'hello',
        protocol: PROTOCOL,
        key_id: identity.keyId,
        // Present on this path and absent on the direct one: the host has no
        // session to look our key up in yet.
        public_key: identity.publicKeyBase64,
        nonce: bytesToBase64(identity.nonceB),
    };
}

/**
 * Check the host's signature over its own offer, and pin its key on the first
 * connection. MUST run before setRemoteDescription: the guarantee is that when
 * this returns false, no DTLS state was ever created.
 *
 * Returns { ok, firstContact, reason }.
 */
export async function verifyOffer(identity, message) {
    if (!message?.sig || !message?.nonce || !message?.host_key)
        return { ok: false, reason: 'the offer is not signed' };
    if (message.host_id !== identity.hostId)
        return { ok: false, reason: 'the offer names a different host' };

    const fingerprint = extractFingerprint(message.sdp);
    if (!fingerprint)
        return {
            ok: false,
            reason: 'the offer does not commit to one sha-256 fingerprint',
        };

    const firstContact = !identity.hostPublicKey;
    if (!firstContact && identity.hostPublicKey !== message.host_key) {
        return {
            ok: false,
            reason: 'this host presented a different identity key than the one this browser paired with',
        };
    }

    let verified = false;
    try {
        const hostKey = await crypto.subtle.importKey(
            'spki',
            base64ToBytes(message.host_key),
            ALGORITHM,
            false,
            ['verify'],
        );
        verified = await crypto.subtle.verify(
            SIGN_PARAMS,
            hostKey,
            base64ToBytes(message.sig),
            hostDigestInput(identity.hostId, identity.keyId, identity.nonceB, fingerprint),
        );
    } catch {
        return { ok: false, reason: 'the host key could not be used' };
    }
    if (!verified) return { ok: false, reason: 'the offer signature does not verify' };

    // Pinned only after the signature checks out, so a key that cannot sign for
    // itself is never the one we remember.
    if (firstContact && !(await rememberHostKey(identity.hostId, message.host_key)))
        return { ok: false, reason: 'the host key could not be stored' };

    identity.hostPublicKey = message.host_key;
    return { ok: true, firstContact, fingerprintHost: fingerprint };
}

/** Sign our answer's fingerprint. Returns the base64 signature, or null. */
export async function signAnswer(identity, nonceHBase64, fingerprintHost, answerSdp) {
    const fingerprintBrowser = extractFingerprint(answerSdp);
    if (!fingerprintBrowser) return null;
    try {
        const sig = await crypto.subtle.sign(
            SIGN_PARAMS,
            identity.privateKey,
            browserDigestInput(
                identity.hostId,
                base64ToBytes(nonceHBase64),
                fingerprintHost,
                fingerprintBrowser,
            ),
        );
        return bytesToBase64(new Uint8Array(sig));
    } catch {
        return null;
    }
}

/**
 * The single sha-256 fingerprint an SDP commits to, uppercase hex with colons,
 * or null.
 *
 * Null for anything ambiguous: no fingerprint line, a hash other than sha-256, a
 * malformed value, or two lines that disagree. That last case is the one worth
 * spelling out — an SDP may repeat the same fingerprint per m-line, but two
 * DIFFERENT values would let whoever reads it pick which to trust, including
 * whoever added the second.
 *
 * Mirrors backend/src/streaming/SdpFingerprint.cpp.
 */
export function extractFingerprint(sdp) {
    if (!sdp) return null;
    let found = null;

    for (const rawLine of sdp.split(/[\r\n]+/)) {
        const line = rawLine.trim();
        if (!line.toLowerCase().startsWith('a=fingerprint:')) continue;

        const body = line.slice('a=fingerprint:'.length).trim();
        const sep = body.indexOf(' ');
        if (sep <= 0) return null;
        if (body.slice(0, sep).toLowerCase() !== 'sha-256') return null;

        const value = body
            .slice(sep + 1)
            .trim()
            .toUpperCase();
        if (!/^([0-9A-F]{2}:){31}[0-9A-F]{2}$/.test(value)) return null;

        if (found === null) found = value;
        else if (found !== value) return null;
    }
    return found;
}
