# vpl-headers

Intel's **oneVPL** (Video Processing Library) public headers — the interface to
the hardware encoder on Intel GPUs.

## Provenance

From [intel/libvpl](https://github.com/intel/libvpl), `api/vpl`. This copy is
API version **2.16** (`MFX_VERSION_MAJOR 2`, `MFX_VERSION_MINOR 16`).

Only the headers are vendored. The runtime is the oneVPL **dispatcher**
(`libvpl.dll`), which ships with the Intel graphics driver and is resolved at
run time — nothing is linked, and nothing of Intel's SDK is redistributed with
MoonlightWeb.

## Licence

**MIT**, per `LICENSE` alongside these headers:

> Copyright (c) 2020 Intel Corporation

| | |
|---|---|
| Open-source impact | none — no copyleft |
| Commercial use | permitted, including in a proprietary build |
| Redistribution | permitted; the notice must travel with the files |

The same codec-patent point recorded for AMF applies here and is unchanged by
which vendor's encoder does the work — see the licence section of
`docs/design/native-capture-encoder.md`.

## Why the 2.x dispatcher, and not the legacy Media SDK

Intel ships two runtimes:

- **oneVPL** (`libvpl.dll`), the current one, entered through `MFXLoad` and a
  config/enumeration API;
- the **legacy Media SDK** (`libmfxhw64.dll`), entered through `MFXInit`.

Only the first is implemented. The legacy runtime reaches back to hardware this
engine does not target anyway (§8 of the mission: modern and limited rather than
universal and slow), and supporting both would double a code path that cannot be
tested on the same machine.

A machine with only the legacy runtime therefore reports no Intel encoder, and
falls back exactly as any other unsupported configuration does.

## Updating

Copy `api/vpl/*.h` and `LICENSE` from a newer libvpl release, keeping the
notices intact, and record the API version above.
