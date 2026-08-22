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
 * MW-BIND-v1 — the browser half.
 *
 * Signaling will one day be relayed by an introduction server. That server must
 * not be able to swap the DTLS fingerprints, terminate DTLS itself and inject
 * keyboard and mouse events into the host's desktop. So each side signs the
 * fingerprint it commits to, with a key the server has never seen.
 *
 * The private key is generated with `extractable: false`. JavaScript running on
 * this page — including a malicious bootstrap — can ask it for a signature but
 * can never read it out. That does not stop such code from *using* the key while
 * it runs; see §8 of docs/design/pairing-signature.md for what this does and
 * does not cover.
 *
 * The byte layout below must stay identical to backend/src/common/PairingCrypto.cpp.
 */

const DB_NAME = 'mw-pairing';
const DB_VERSION = 1;
const STORE = 'keys';
/** One record per host origin, so two hosts reached from one browser never share a key. */
const RECORD_ID = 'mw-bind-v1';

const PROTOCOL = 'MW-BIND-v1';
const ALGORITHM = { name: 'ECDSA', namedCurve: 'P-256' };
const SIGN_PARAMS = { name: 'ECDSA', hash: { name: 'SHA-256' } };

// ── Encoding helpers ───────────────────────────────────────────────────────

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
 * Length-prefixed concatenation: each field is preceded by its byte length as a
 * 4-byte big-endian integer.
 *
 * Plain concatenation would leave field boundaries ambiguous — ('ab','c') and
 * ('a','bc') would produce the same bytes — and one of these fields is a
 * fingerprint. The backend builds the same framing in
 * PairingCrypto::buildSignedMessage(); the two must not drift apart.
 */
function buildSignedMessage(fields) {
    const parts = fields.map((f) => (typeof f === 'string' ? new TextEncoder().encode(f) : f));
    let total = 0;
    for (const p of parts) total += 4 + p.length;

    const out = new Uint8Array(total);
    const view = new DataView(out.buffer);
    let offset = 0;
    for (const p of parts) {
        view.setUint32(offset, p.length, false); // big-endian
        offset += 4;
        out.set(p, offset);
        offset += p.length;
    }
    return out;
}

/** The exact bytes the host signs (design §4 step 1). */
function hostDigestInput(hostId, browserKeyId, nonceB, fingerprintHost) {
    return buildSignedMessage([`${PROTOCOL}|host`, hostId, browserKeyId, nonceB, fingerprintHost]);
}

/** The exact bytes this browser signs (design §4 step 3). */
function browserDigestInput(hostId, nonceH, fingerprintHost, fingerprintBrowser) {
    return buildSignedMessage([
        `${PROTOCOL}|browser`,
        hostId,
        nonceH,
        fingerprintHost,
        fingerprintBrowser,
    ]);
}

// ── IndexedDB ──────────────────────────────────────────────────────────────

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

// ── The pairing identity ───────────────────────────────────────────────────

/**
 * This browser's pairing identity for the current host, creating the key pair on
 * first use.
 *
 * CryptoKey objects are structured-cloneable, so the non-extractable private key
 * is stored in IndexedDB as-is: it survives reloads without ever existing as
 * bytes this page could read or send anywhere.
 *
 * Returns null when the browser gives us no way to do this safely — no
 * WebCrypto (an insecure origin), or no IndexedDB (private mode in some
 * browsers). Callers must treat null as "cannot bind", never as "binding
 * succeeded trivially".
 */
export async function loadOrCreateIdentity() {
    if (!globalThis.crypto?.subtle || !globalThis.indexedDB) {
        console.warn('[MW-BIND] WebCrypto or IndexedDB unavailable — cannot bind this browser');
        return null;
    }

    try {
        const db = await openDb();
        let record = await dbGet(db, RECORD_ID);

        if (!record?.privateKey || !record?.publicKeySpki) {
            const pair = await crypto.subtle.generateKey(ALGORITHM, false, ['sign', 'verify']);
            const spki = new Uint8Array(await crypto.subtle.exportKey('spki', pair.publicKey));
            // `false` above is the whole point: the public half exports, the
            // private half never does.
            record = { privateKey: pair.privateKey, publicKeySpki: spki, hostPublicKey: null };
            await dbPut(db, RECORD_ID, record);
            console.log('[MW-BIND] Generated a new pairing key for this browser');
        }

        const keyIdBytes = new Uint8Array(
            await crypto.subtle.digest('SHA-256', record.publicKeySpki)
        );

        return {
            privateKey: record.privateKey,
            publicKeySpki: record.publicKeySpki,
            publicKeyBase64: bytesToBase64(record.publicKeySpki),
            keyId: base64Url(keyIdBytes),
            hostPublicKey: record.hostPublicKey || null,
            hostId: record.hostId || null,
        };
    } catch (e) {
        console.warn('[MW-BIND] Could not open the pairing key store:', e.message);
        return null;
    }
}

/**
 * Remember the host's public key alongside our own, after pairing.
 *
 * Trust-on-first-use: the first host identity we see for this origin is the one
 * we keep. A *different* one arriving later is refused rather than accepted —
 * that is either a host that regenerated its key (the user re-pairs with a PIN,
 * which clears this record) or exactly the substitution this protocol exists to
 * catch. Returns false when the identity was refused.
 */
export async function rememberHostIdentity(hostPublicKeyBase64, hostId) {
    if (!hostPublicKeyBase64 || !globalThis.indexedDB) return false;

    try {
        const db = await openDb();
        const record = await dbGet(db, RECORD_ID);
        if (!record) return false;

        if (record.hostPublicKey && record.hostPublicKey !== hostPublicKeyBase64) {
            console.error('[MW-BIND] This host presented a different identity key than the one '
                + 'this browser paired with — refusing to overwrite it');
            return false;
        }

        record.hostPublicKey = hostPublicKeyBase64;
        record.hostId = hostId || record.hostId || null;
        await dbPut(db, RECORD_ID, record);
        return true;
    } catch (e) {
        console.warn('[MW-BIND] Could not store the host identity:', e.message);
        return false;
    }
}

/** Forget everything, so the next visit pairs from scratch. */
export async function clearIdentity() {
    if (!globalThis.indexedDB) return;
    try {
        const db = await openDb();
        await dbPut(db, RECORD_ID, undefined);
    } catch {
        /* nothing to clear */
    }
}

// ── Per-connection operations ──────────────────────────────────────────────

/**
 * Start a connection's MW-BIND-v1 exchange: load this browser's identity and
 * draw the nonce the host's signature will have to cover.
 *
 * Returns null when this browser has no pairing key at all, which is the
 * pre-MW-BIND-v1 case; the caller then runs the old unsigned exchange.
 */
export async function beginHandshake() {
    const identity = await loadOrCreateIdentity();
    if (!identity) return null;
    identity.nonceB = generateNonce();
    return identity;
}

/**
 * The hello that opens the exchange (design §4 step 0).
 *
 * The host holds its SDP offer back until this arrives, because its signature
 * covers our nonce — that ordering is what lets us verify the host *before*
 * touching setRemoteDescription.
 */
export function helloMessage(identity) {
    return {
        type: 'hello',
        protocol: PROTOCOL,
        key_id: identity.keyId,
        nonce: bytesToBase64(identity.nonceB),
    };
}

/** A fresh 32-byte nonce. */
export function generateNonce() {
    const bytes = new Uint8Array(32);
    crypto.getRandomValues(bytes);
    return bytes;
}

/**
 * Verify the host's signature over its own SDP offer.
 *
 * Must be called BEFORE setRemoteDescription: the guarantee this protocol
 * offers is that when verification fails, no DTLS state was ever created.
 *
 * Returns false on anything unexpected — missing signature, unusable key,
 * mismatched host id. There is no "could not check, assume fine" branch.
 */
export async function verifyHostSignature(identity, message) {
    if (!identity?.hostPublicKey) return false;
    if (!message?.sig || !message?.nonce) {
        console.error('[MW-BIND] The SDP offer carries no signature');
        return false;
    }
    if (identity.hostId && message.host_id && identity.hostId !== message.host_id) {
        console.error('[MW-BIND] The SDP offer claims a different host id than the paired one');
        return false;
    }

    const fingerprint = extractFingerprint(message.sdp);
    if (!fingerprint) {
        console.error('[MW-BIND] The SDP offer does not commit to one sha-256 fingerprint');
        return false;
    }

    try {
        const hostKey = await crypto.subtle.importKey(
            'spki',
            base64ToBytes(identity.hostPublicKey),
            ALGORITHM,
            false,
            ['verify']
        );
        const signed = hostDigestInput(
            message.host_id || identity.hostId || '',
            identity.keyId,
            identity.nonceB,
            fingerprint
        );
        return await crypto.subtle.verify(
            SIGN_PARAMS,
            hostKey,
            base64ToBytes(message.sig),
            signed
        );
    } catch (e) {
        console.error('[MW-BIND] Host signature verification failed:', e.message);
        return false;
    }
}

/** Sign our answer's fingerprint. Returns the base64 signature, or null. */
export async function signAnswer(identity, hostId, nonceHBase64, fingerprintHost, answerSdp) {
    const fingerprintBrowser = extractFingerprint(answerSdp);
    if (!fingerprintBrowser) {
        console.error('[MW-BIND] Our own SDP answer has no single sha-256 fingerprint');
        return null;
    }

    try {
        const signed = browserDigestInput(
            hostId,
            base64ToBytes(nonceHBase64),
            fingerprintHost,
            fingerprintBrowser
        );
        const sig = await crypto.subtle.sign(SIGN_PARAMS, identity.privateKey, signed);
        return bytesToBase64(new Uint8Array(sig));
    } catch (e) {
        console.error('[MW-BIND] Could not sign the SDP answer:', e.message);
        return null;
    }
}

/**
 * The single sha-256 fingerprint an SDP commits to, uppercase hex with colons,
 * or null.
 *
 * Null whenever the SDP does not commit to exactly one: no fingerprint line, a
 * hash other than sha-256, a malformed value, or two lines that disagree. That
 * last case is the one worth spelling out — an SDP may repeat the same
 * fingerprint per m-line, but two *different* values would let whoever reads it
 * pick which one to trust, including an attacker who added the second.
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

        const value = body.slice(sep + 1).trim().toUpperCase();
        if (!/^([0-9A-F]{2}:){31}[0-9A-F]{2}$/.test(value)) return null;

        if (found === null) found = value;
        else if (found !== value) return null;
    }
    return found;
}
