# amf-headers

AMD's **Advanced Media Framework** (AMF) public headers — the interface to the
hardware encoder on Radeon GPUs.

## Provenance

From [GPUOpen-LibrariesAndSDKs/AMF](https://github.com/GPUOpen-LibrariesAndSDKs/AMF),
`amf/public/include` — the `core/` and `components/` trees only. Nothing else of
the SDK is needed: the runtime lives in `amfrt64.dll`, which ships with the
Radeon driver and is loaded at run time.

## Licence

**MIT**, per the notice at the head of every file:

> Copyright (c) 2018 Advanced Micro Devices, Inc. All rights reserved.
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction […]

| | |
|---|---|
| Open-source impact | none — no copyleft |
| Commercial use | permitted, including in a proprietary build |
| Redistribution | permitted; the notice must travel with the files |

### The standards notice — worth reading once

AMD prefixes these headers with a clause that is **not** a software-licence
restriction but a disclaimer about codec patents:

> AMD does not provide a license or sublicense to any Intellectual Property
> Rights relating to any standards, including […] AVC/H.264; HEVC/H.265 […]
> you will pay any royalties due for such third party technologies.

This changes nothing about MoonlightWeb's position and adds no obligation the
project did not already have: encoding H.264 or HEVC in a commercial product is
a matter for the AVC and HEVC patent pools regardless of which vendor's encoder
does the work. It is the same point recorded in the licence section of
`docs/design/native-capture-encoder.md`, and the same reason AV1 (royalty-free,
AOMedia) is preferred when both ends support it.

## Updating

Copy `amf/public/include/{core,components}` from a newer AMF release, keeping
the notices intact. Nothing here is generated or patched.
