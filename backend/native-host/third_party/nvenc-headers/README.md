# nvenc-headers

`ffnvcodec/nvEncodeAPI.h` — the interface to NVIDIA's hardware encoder (NVENC).

## Provenance

Taken from **[nv-codec-headers](https://github.com/FFmpeg/nv-codec-headers)**, the
FFmpeg project's redistribution of NVIDIA's codec headers. This copy is tag
`n12.0.16.2`, corresponding to **Video Codec SDK 12.0**
(`NVENCAPI_MAJOR_VERSION 12`, `NVENCAPI_MINOR_VERSION 0`).

Minimum driver: **520.56** (Linux) / **522.25** (Windows), October 2022.

## Why not the newest SDK

Deliberately not the latest. NVENC's API is backward compatible in one
direction only: a driver accepts the struct versions of its own API generation
or older, and **rejects anything newer**. So the header version is not a
"how new can we be" choice — it is a floor on the driver every user must have.

This was not theoretical. The first attempt vendored SDK 13.1, and the
development bench — an RTX 5060 Ti — reported API version 208 (SDK 13.0)
against the header's 209 and refused every encode session. A brand-new GPU was
locked out by a header chosen for no better reason than being current.

SDK 12.0 carries everything this engine actually uses:

| Needed | In 12.0 |
|---|---|
| H.264, HEVC, AV1 encode | yes — AV1 arrived in 12.0 |
| `NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY` | yes |
| Intra-refresh | yes |
| Reference-picture invalidation | yes |
| 10-bit encode (HDR) | yes |
| D3D11 input surfaces (zero-copy) | yes |

Moving up would cost driver compatibility and buy nothing. Revisit only when a
feature this engine genuinely needs exists solely in a later SDK.

## Licence

The header carries its own permission notice — an MIT-style grant from NVIDIA
Corporation that applies **to the header file only**:

> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software […] to deal in the Software without restriction, including
> without limitation the rights to use, copy, modify, merge, publish,
> distribute, sublicense, and/or sell copies […]

Which means, for MoonlightWeb's purposes:

| | |
|---|---|
| Open-source impact | none — no copyleft, no reciprocal obligation |
| Commercial use | permitted, including in a proprietary build |
| Redistribution | permitted; the notice must travel with the file |

That is why this source was chosen over NVIDIA's own SDK download: the terms are
plainly permissive and require nothing beyond keeping the notice, which suits
the relicensing constraint this whole module is built around (see
`../../LICENSE.md`).

## Why vendored rather than fetched

A build must not need the network. Fetching this at configure time would make
an offline build fail and put a third-party host in the critical path of CI —
for a single header that changes a few times a year.

## Updating

Copy the newer `include/ffnvcodec/nvEncodeAPI.h` from nv-codec-headers, keeping
the notice intact, and record the new SDK version above. Nothing else here is
generated.

**This is not NVIDIA's SDK.** Only the API header is present: the encoder
itself lives in `nvEncodeAPI64.dll`, which ships with the display driver and is
loaded at runtime. Nothing from the SDK is redistributed with MoonlightWeb.
