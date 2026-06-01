---
name: hevc-avcc-keyframe-first-nall-type
description: Fix Chrome/Edge HEVC ecran noir "wasn't a key frame" — AUD/SEI entre PPS et IDR dans toAvcc()
metadata:
  type: feedback
---

# HEVC AVCC Keyframe First NAL Type Fix

**Rule**: Chrome/Edge WebCodecs require the first NAL unit in HEVC AVCC keyframe data to be an IRAP type (16-21: BLA, IDR, CRA). Non-IRAP NAL units (AUD=35, SEI=39, filler data=38) before the first IRAP cause decode rejection with "wasn't a key frame" error.

**Why**: Sunshine's HEVC encoder may insert AUD, SEI, or other non-slice NAL units between PPS and the IDR slice in keyframes. Safari is permissive and accepts any first NAL unit; Chrome/Edge validate strictly.

**Fix**: In `toAvcc()` (`Mp4Muxer.js`), when `stripParams=true` and `codec=CODEC_HEVC`, skip all NAL units before the first IRAP (types 16-21), not just VPS/SPS/PPS. The `hevcFoundIrap` flag tracks whether the first IRAP has been seen.

**Affected browsers**: Chrome, Edge, Opera, Brave — any Chromium-based browser. Safari (WebKit) is unaffected.

**How to apply**: This fix is already in `Mp4Muxer.js`. If adding similar validation elsewhere (e.g. sending HEVC data over another transport), ensure only IRAP NAL units precede the slice data in keyframe AVCC chunks.

See also: [[hevc-isconfigsupported-rejected]] [[hevc-green-image-emulation-fix]] [[phase5b-webcodecs-fix]]
