/**
 * Minimal fMP4 muxer for H.264 video.
 * Converts H.264 Annex B (NAL start codes) to fMP4 segments (AVCC + moof/mdat)
 * suitable for MediaSource Extensions (MSE).
 */
export class Mp4Muxer {
    constructor(width = 1920, height = 1080, fps = 60) {
        this.width = width;
        this.height = height;
        this.sequenceNumber = 0;
        this.sps = null;
        this.pps = null;
        this.initSegment = null;
    }

    /**
     * Feed a raw H.264 Annex B frame.
     * @param {Uint8Array} annexB
     * @param {boolean} isKeyframe
     * @returns {{init: Uint8Array|null, media: Uint8Array|null}}
     */
    processFrame(annexB, isKeyframe) {
        const result = { init: null, media: null };

        if (isKeyframe) {
            const nals = extractNalUnits(annexB);
            for (const nalu of nals) {
                const type = nalu[0] & 0x1F;
                if (type === 7) this.sps = nalu;
                else if (type === 8) this.pps = nalu;
            }
            if (this.sps && this.pps && !this.initSegment) {
                this.initSegment = buildInitSegment(this.width, this.height, this.sps, this.pps);
                result.init = this.initSegment;
            }
        }

        const avcc = annexBtoAvcc(annexB);
        this.sequenceNumber++;
        result.media = buildMediaSegment(avcc, isKeyframe, this.sequenceNumber);
        return result;
    }
}

// --- Standalone helpers ---

/**
 * Find all NAL units in an Annex B buffer.
 * Start codes: 0x00 0x00 0x01 or 0x00 0x00 0x00 0x01
 */
function extractNalUnits(data) {
    const nals = [];
    let i = 0;

    while (i < data.length - 3) {
        // Look for 3-byte start code: 00 00 01
        if (data[i] === 0x00 && data[i+1] === 0x00) {
            let startLen = 0;
            if (data[i+2] === 0x01) {
                startLen = 3;
            } else if (i + 3 < data.length && data[i+2] === 0x00 && data[i+3] === 0x01) {
                startLen = 4;
            }

            if (startLen > 0) {
                const naluStart = i + startLen;
                // Find next start code
                let naluEnd = data.length;
                for (let j = naluStart; j < data.length - 3; j++) {
                    if (data[j] === 0x00 && data[j+1] === 0x00) {
                        if (data[j+2] === 0x01 || (j+3 < data.length && data[j+2] === 0x00 && data[j+3] === 0x01)) {
                            naluEnd = j;
                            break;
                        }
                    }
                }
                nals.push(data.slice(naluStart, naluEnd));
                i = naluEnd;
                continue;
            }
        }
        i++;
    }
    return nals;
}

/**
 * Convert Annex B (start codes) to AVCC (4-byte length prefix).
 */
function annexBtoAvcc(data) {
    const nals = extractNalUnits(data);
    const parts = [];
    for (const nalu of nals) {
        const len = nalu.length;
        parts.push((len >> 24) & 0xFF, (len >> 16) & 0xFF, (len >> 8) & 0xFF, len & 0xFF);
        parts.push(...nalu);
    }
    return new Uint8Array(parts);
}

// --- MP4 box builders ---

/**
 * Build the fMP4 init segment: ftyp + moov with avcC.
 */
function buildInitSegment(width, height, sps, pps) {
    const avcC = buildAvcC(sps, pps);
    const avc1Box = buildAvc1Box(width, height, avcC);

    return concatBytes(
        // ftyp: isom, avc1
        boxHeader(24, 'ftyp'),
        u32(0x69736F6D), u32(0), u32(0x69736F6D), u32(0x61766331),
        // moov
        boxHeader(0, 'moov', /* use placeholder */ true),
        //   mvhd
        box(108, 'mvhd',
            u32(0), u32(0),  // creation, modification time
            u32(1000),        // timescale
            u32(0), u32(0),  // duration (0 for fragmented)
            u32(0x00010000), // rate
            u16(0x0100),     // volume
            u16(0),          // reserved
            u32(0), u32(0), u32(0), u32(0), u32(0), u32(0), u32(0), // matrix
            u32(0), u32(0), u32(0), u32(0), u32(0), u32(0), // pre-defined
            u32(2)           // next track id
        ),
        //   trak
        box(0, 'trak', /* placeHolder */ true,
            // tkhd
            box(92, 'tkhd',
                u32(7),        // flags: track enabled + in movie + in preview
                u32(0), u32(0), // creation, modification
                u32(1),        // track id
                u32(0),        // reserved
                u32(0),        // duration
                u32(0), u32(0), // reserved
                u32(0),        // layer, alt group
                u16(0x0100),   // volume
                u16(0),        // reserved
                u32(0x00010000), u32(0), u32(0), u32(0), u32(0x00010000), u32(0), u32(0), u32(0), u32(0x40000000), // matrix
                u32(width << 16), u32(height << 16) // width, height
            ),
            // mdia
            box(0, 'mdia', /* placeHolder */ true,
                // mdhd
                box(32, 'mdhd',
                    u32(0), u32(0), u32(0), u32(1000), u32(0), u32(0)
                ),
                // hdlr
                box(33, 'hdlr',
                    u32(0), u32(0),
                    u8s('vide'),
                    u32(0), u32(0), u32(0)
                ),
                // minf
                box(0, 'minf', /* placeHolder */ true,
                    // vmhd
                    box(20, 'vmhd', u32(1), u32(0), u32(0), u32(0)),
                    // dinf
                    box(36, 'dinf',
                        // dref
                        box(28, 'dref', u32(0), u32(1),
                            // url
                            box(12, 'url ', u32(1))
                        )
                    ),
                    // stbl
                    box(0, 'stbl', /* placeHolder */ true,
                        // stsd
                        box(0, 'stsd', true,
                            u32(0), u32(1), // version, entry count
                            avc1Box
                        ),
                        // stts
                        box(16, 'stts', u32(0), u32(0)),
                        // stsc
                        box(16, 'stsc', u32(0), u32(0)),
                        // stsz
                        box(20, 'stsz', u32(0), u32(0), u32(0)),
                        // stco
                        box(16, 'stco', u32(0), u32(0))
                    )
                )
            )
        )
    );
}

function buildAvc1Box(width, height, avcC) {
    return box(0, 'avc1', /* placeHolder */ true,
        u32(0), u16(0), u16(1), // reserved + data ref index
        u16(0), u16(0), u16(0), u16(0), u16(0), u16(0), u16(0), u16(0), // pre-defined
        u16(width), u16(height),
        u32(0x00480000), u32(0x00480000), // 72 dpi
        u32(0), u32(1), // reserved + frame count
        u8s(''), // compressor name (32 bytes - we pad)
        u16(0x0018), u16(0xFFFF), // depth + pre-defined
        // avcC as a sub-box (not really a box, just size + 'avcC' + data...
        // actually avcC is inline in the avc1, not a box. So we just write size + data.
        u32(8 + avcC.length), u8s('avcC'),
        avcC
    );
}

function buildAvcC(sps, pps) {
    const data = [0x01]; // configurationVersion
    data.push(sps[1]);    // AVCProfileIndication
    data.push(sps[2]);    // profile_compatibility
    data.push(sps[3]);    // AVCLevelIndication
    data.push(0xFF);      // lengthSizeMinusOne (4 bytes) = 3 encoded as 0b11 + 0b111111 (6 bits reserved)
    data.push(0xE1);      // numOfSequenceParameterSets (1) in low 5 bits
    data.push((sps.length >> 8) & 0xFF);
    data.push(sps.length & 0xFF);
    data.push(...sps);
    data.push(0x01); // numOfPictureParameterSets (1)
    data.push((pps.length >> 8) & 0xFF);
    data.push(pps.length & 0xFF);
    data.push(...pps);
    return new Uint8Array(data);
}

/**
 * Build a moof+mdat media segment.
 */
function buildMediaSegment(nalus, isKeyframe, sequenceNumber) {
    const sampleSize = nalus.length;
    const sampleFlags = isKeyframe ? 0x02000000 : 0x01010000;

    return concatBytes(
        // moof
        box(0, 'moof', /* placeHolder */ true,
            box(16, 'mfhd', u32(sequenceNumber)),
            box(0, 'traf', true,
                box(0, 'tfhd', true,
                    u32(1),    // track id
                    u32(0)     // base data offset (moof-relative)
                ),
                box(0, 'tfdt', true,
                    u32(0)     // baseMediaDecodeTime (we use 0 since we're not doing proper timing yet)
                ),
                box(0, 'trun', true,
                    u32(1),        // sample count
                    u32(0),        // data offset (mdat-relative)
                    u32(sampleFlags),
                    u32(sampleSize),
                    u32(0)         // sample duration
                )
            )
        ),
        // mdat
        box(sampleSize, 'mdat', nalus)
    );
}

// --- Low-level MP4 helpers ---

function boxHeader(size, type, placeholder) {
    if (placeholder) return { placeholder: true, type, size }; // size will be filled later
    return null; // handled by box()
}

function box(size, type, ...children) {
    const isPlaceholder = (size === 0);
    const parts = [];
    // Gather all children into a flat array
    for (const child of children) {
        if (child === null || child === undefined) continue;
        if (child.placeholder) {
            // Recurse into placeholder boxes
            parts.push(child.resolved);
        } else if (child instanceof Uint8Array) {
            parts.push(child);
        } else if (Array.isArray(child)) {
            parts.push(new Uint8Array(child));
        } else {
            // Single number or string — handled by concat
            parts.push(new Uint8Array([child]));
        }
    }

    const data = concatBytes(...parts);
    const totalSize = isPlaceholder ? data.length + 8 : size;
    const header = new Uint8Array(8);
    header[0] = (totalSize >> 24) & 0xFF;
    header[1] = (totalSize >> 16) & 0xFF;
    header[2] = (totalSize >> 8) & 0xFF;
    header[3] = totalSize & 0xFF;
    header[4] = type.charCodeAt(0);
    header[5] = type.charCodeAt(1);
    header[6] = type.charCodeAt(2);
    header[7] = type.charCodeAt(3);

    const result = new Uint8Array(totalSize);
    result.set(header, 0);
    result.set(data, 8);

    if (isPlaceholder) {
        return { placeholder: true, type, size: totalSize, resolved: result };
    }
    return result;
}

function u32(v) { return new Uint8Array([(v>>24)&0xFF, (v>>16)&0xFF, (v>>8)&0xFF, v&0xFF]); }
function u16(v) { return new Uint8Array([(v>>8)&0xFF, v&0xFF]); }
function u8s(s) {
    const arr = new Uint8Array(s.length);
    for (let i = 0; i < s.length; i++) arr[i] = s.charCodeAt(i);
    return arr;
}

function concatBytes(...arrays) {
    let totalLen = 0;
    for (const a of arrays) {
        if (a instanceof Uint8Array) totalLen += a.length;
        else if (Array.isArray(a)) totalLen += a.length;
    }
    const result = new Uint8Array(totalLen);
    let off = 0;
    for (const a of arrays) {
        if (a instanceof Uint8Array) {
            result.set(a, off);
            off += a.length;
        } else if (Array.isArray(a)) {
            result.set(new Uint8Array(a), off);
            off += a.length;
        }
    }
    return result;
}
