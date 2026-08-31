# nvenc-headers

`ffnvcodec/nvEncodeAPI.h` — the interface to NVIDIA's hardware encoder (NVENC).

## Provenance

Taken from **[nv-codec-headers](https://github.com/FFmpeg/nv-codec-headers)**, the
FFmpeg project's redistribution of NVIDIA's codec headers. This copy corresponds
to **Video Codec SDK 13.1.15** (`NVENCAPI_MAJOR_VERSION 13`,
`NVENCAPI_MINOR_VERSION 1`).

Minimum driver: 610.0 or newer, on both Windows and Linux.

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
