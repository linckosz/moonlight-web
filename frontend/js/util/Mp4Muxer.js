/**
 * Minimal fMP4 muxer for H.264 video via MSE.
 *
 * Uses a pre-validated init segment template patched with the correct avcC
 * (extracted from SPS/PPS), plus moof+mdat media segments per frame.
 */

const FTYP_BOX = new Uint8Array([
    0x00, 0x00, 0x00, 0x18, // size=24
    0x66, 0x74, 0x79, 0x70, // ftyp
    0x69, 0x73, 0x6F, 0x6D, // major=isom
    0x00, 0x00, 0x00, 0x00, // minor=0
    0x69, 0x73, 0x6F, 0x6D, // compat[0]=isom
    0x61, 0x76, 0x63, 0x31, // compat[1]=avc1
]);

// Minimal pre-built moov for H.264 video, with placeholder avcC.
// We build it properly using the box() helpers below.
// Width/height fields are patched at runtime.

export class Mp4Muxer {
    constructor(width = 1920, height = 1080) {
        this.width = width;
        this.height = height;
        this.seq = 0;
        this.sps = null;
        this.pps = null;
        this.initReady = false;
    }

    processFrame(annexB, isKeyframe) {
        const nals = splitNals(annexB);
        if (nals.length === 0) return { init: null, media: null };

        let sps = null, pps = null;
        const dataNals = [];

        for (const n of nals) {
            const type = n[0] & 0x1F;
            if (type === 7) sps = n;
            else if (type === 8) pps = n;
            else if (type !== 6 && type !== 9 && type !== 10 && type !== 11) {
                dataNals.push(n);
            }
        }

        if (sps && pps) {
            this.sps = sps;
            this.pps = pps;
        }

        if (!this.sps && isKeyframe) {
            this.sps = GENERIC_SPS_1080p;
            this.pps = GENERIC_PPS_1080p;
        }

        let init = null;
        if (isKeyframe && this.sps && this.pps && !this.initReady) {
            init = buildInitSegment(this.width, this.height, this.sps, this.pps);
            this.initReady = true;
        }

        const avcc = toAvcc(dataNals);
        this.seq++;
        const media = buildMediaSegment(avcc, isKeyframe, this.seq);

        return { init, media };
    }
}

// === NAL parsing ===

function splitNals(annexB) {
    const nals = [];
    let i = 0, len = annexB.length;
    while (i < len - 3) {
        if (annexB[i] === 0 && annexB[i+1] === 0) {
            let sc = 0;
            if (annexB[i+2] === 1) sc = 3;
            else if (i+3 < len && annexB[i+2] === 0 && annexB[i+3] === 1) sc = 4;
            if (sc) {
                const start = i + sc;
                let end = len;
                for (let j = start; j < len - 3; j++) {
                    if (annexB[j] === 0 && annexB[j+1] === 0) {
                        if (annexB[j+2] === 1 ||
                            (j+3 < len && annexB[j+2] === 0 && annexB[j+3] === 1)) {
                            end = j; break;
                        }
                    }
                }
                nals.push(annexB.slice(start, end));
                i = end;
                continue;
            }
        }
        i++;
    }
    return nals;
}

function toAvcc(nals) {
    const parts = [];
    for (const n of nals) {
        const l = n.length;
        parts.push((l>>24)&0xFF, (l>>16)&0xFF, (l>>8)&0xFF, l&0xFF);
        for (let i = 0; i < l; i++) parts.push(n[i]);
    }
    return new Uint8Array(parts);
}

// === MP4 box helpers ===

function box(type, data) {
    const sz = 8 + data.length;
    const b = new Uint8Array(sz);
    b[0]=(sz>>24)&0xFF; b[1]=(sz>>16)&0xFF; b[2]=(sz>>8)&0xFF; b[3]=sz&0xFF;
    b[4]=type.charCodeAt(0); b[5]=type.charCodeAt(1); b[6]=type.charCodeAt(2); b[7]=type.charCodeAt(3);
    b.set(data, 8);
    return b;
}

function fbox(type, ver, flags, data) {
    const inner = new Uint8Array(4 + data.length);
    inner[0]=ver; inner[1]=(flags>>16)&0xFF; inner[2]=(flags>>8)&0xFF; inner[3]=flags&0xFF;
    inner.set(data, 4);
    return box(type, inner);
}

function concat(...arrs) {
    let t = 0; for (const a of arrs) if (a) t += a.length;
    const r = new Uint8Array(t); let o = 0;
    for (const a of arrs) { if (a) { r.set(a, o); o += a.length; } }
    return r;
}

function u32(v) { return new Uint8Array([(v>>24)&0xFF,(v>>16)&0xFF,(v>>8)&0xFF,v&0xFF]); }
function u16(v) { return new Uint8Array([(v>>8)&0xFF,v&0xFF]); }
function u64(hi,lo) { return concat(u32(hi), u32(lo)); }
function pad(n) { return new Uint8Array(n); }
function str4(s) { const a=new Uint8Array(4); for(let i=0;i<4;i++)a[i]=s.charCodeAt(i); return a; }

// === Init segment (ftyp + moov) ===

function buildInitSegment(w, h, sps, pps) {
    const avcc = buildAvcc(sps, pps);

    // avc1 box
    const avc1Box = box('avc1', concat(
        pad(6), u16(1),          // reserved + data-ref-index
        pad(16),                 // pre-defined
        u16(w), u16(h),          // width, height
        u32(0x00480000),         // h-res 72dpi
        u32(0x00480000),         // v-res 72dpi
        pad(4),                  // reserved
        u16(1),                  // frame count
        pad(32),                 // compressor name
        u16(0x0018),             // depth 24-bit
        u16(0xFFFF),             // pre-defined -1
        box('avcC', avcc)
    ));

    // stsd
    const stsd = fbox('stsd', 0, 0, concat(u32(1), avc1Box));

    // stbl
    const stbl = box('stbl', concat(
        stsd,
        fbox('stts', 0, 0, u32(0)),
        fbox('stsc', 0, 0, u32(0)),
        fbox('stsz', 0, 0, concat(u32(0), u32(0))),
        fbox('stco', 0, 0, u32(0))
    ));

    // minf
    const vmhd = fbox('vmhd', 0, 1, concat(u32(0), u32(0), u32(0), u32(0)));
    const dref = fbox('dref', 0, 0, concat(u32(1), fbox('url ', 0, 1, new Uint8Array(0))));
    const dinf = box('dinf', box('dref', dref));
    const minf = box('minf', concat(vmhd, dinf, stbl));

    // mdia
    const mdhd = fbox('mdhd', 0, 0, concat(u32(0), u32(0), u32(1000), u32(0), u32(0)));
    const hdlr = fbox('hdlr', 0, 0, concat(u32(0), str4('vide'), pad(12)));
    const mdia = box('mdia', concat(mdhd, hdlr, minf));

    // tkhd: track header
    const matrix = concat(
        u32(0x00010000), u32(0), u32(0), u32(0),
        u32(0x00010000), u32(0), u32(0), u32(0),
        u32(0x40000000)
    );
    const tkhd = fbox('tkhd', 0, 7, concat(
        u32(0), u32(0), u32(1),       // creation, modification, track ID
        pad(4), u32(0),                // reserved + duration
        pad(8),                        // reserved
        u16(0), u16(0),                // layer, alt group
        u16(0x0100), u16(0),           // volume, reserved
        matrix,                        // 9 u32 matrix
        u32(w << 16), u32(h << 16)     // width, height
    ));
    const trak = box('trak', concat(tkhd, mdia));

    // mvhd
    const mvhd = fbox('mvhd', 0, 0, concat(
        u32(0), u32(0), u32(1000), u32(0),
        u32(0), u32(0x00010000),
        u16(0x0100), u16(0),
        pad(8),                       // reserved
        pad(36),                       // matrix (9 x u32)
        pad(24),                       // pre-defined
        u32(2)                         // next track ID
    ));

    const moov = box('moov', concat(mvhd, trak));
    return concat(FTYP_BOX, moov);
}

function buildAvcc(sps, pps) {
    return concat(
        new Uint8Array([0x01, sps[0], sps[1], sps[2], 0xFF, 0xE1]),
        u16(sps.length), sps,
        new Uint8Array([0x01]),
        u16(pps.length), pps
    );
}

// === Media segment (moof + mdat) ===

function buildMediaSegment(nalus, isKeyframe, seq) {
    const sampleSize = nalus.length;
    const sampleFlags = isKeyframe ? 0x02000000 : 0x01010000;

    const mfhd = fbox('mfhd', 0, 0, u32(seq));
    const tfhd = fbox('tfhd', 0, 0x020000, concat(u32(1), u64(0,0), u32(1), u32(0), u32(0)));
    const tfdt = fbox('tfdt', 1, 0, u64(0, 0));
    // trun: count=1, data-offset, first-sample-flags, sample-duration, sample-size
    const trun = fbox('trun', 0, 0x000701, concat(
        u32(1),            // sample count
        u32(0),            // data offset (mdat-relative)
        u32(sampleFlags),
        u32(0),            // duration
        u32(sampleSize)    // size
    ));
    const traf = box('traf', concat(tfhd, tfdt, trun));
    const moof = box('moof', concat(mfhd, traf));
    const mdat = box('mdat', nalus);

    return concat(moof, mdat);
}

// === Fallback SPS/PPS for H.264 High Profile, Level 4.2, 1080p ===

const GENERIC_SPS_1080p = new Uint8Array([
    0x67, 0x64, 0x00, 0x2A, 0xAD, 0x84, 0x01, 0x0C, 0x20, 0x08, 0x61,
    0x00, 0x43, 0x08, 0x02, 0x18, 0x40, 0x10, 0xC2, 0x00, 0x84, 0x2B, 0x50
]);

const GENERIC_PPS_1080p = new Uint8Array([0x68, 0xEE, 0x3C, 0x80]);
