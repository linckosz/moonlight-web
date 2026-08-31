# ViGEmClient — vendored

The user-mode client for **ViGEmBus**, the virtual gamepad bus driver. It is how
the native host presents an Xbox 360 pad to Windows: the driver creates a device
that games see as a real controller, and this library is the thin layer that
talks to it over `DeviceIoControl`.

| | |
|---|---|
| Upstream | https://github.com/nefarius/ViGEmClient |
| Version | **v1.16.18.0** |
| Licence | **MIT** — see `LICENSE` |
| Modified | **No.** Byte-for-byte upstream, original filenames kept |

## What is here, and what is not

`include/` and `src/ViGEmClient.cpp` only. The solution files, the CI config and
the resource script are not vendored — we build the source directly into
`mw-native-host` rather than producing `ViGEmClient.dll`.

## Why vendored rather than fetched

Same reason as the three encoder SDKs: the build must not depend on the network,
and a version bump must be a visible commit rather than something that happens
to whoever builds next. This is ~58 KB of source; the cost of carrying it is
much lower than the cost of a moving dependency.

## Why the licence matters here

MIT, so it can be relicensed with the module (see `../../LICENSE.md` §26 of the
mission). It is worth noting that this was recorded as BSD-3 in the original
design document — it is not; MIT is what upstream ships, and MIT is strictly
easier for a future commercial product.

## The driver is a separate thing

This library talks to a **kernel driver that must already be installed**.
ViGEmBus ships under BSD-3 as a signed driver package; it is installed by the
MoonlightWeb installer, not carried here, and nothing in this tree is
redistributed as part of it. With the driver absent, `vigem_connect` fails
cleanly and the native host streams without gamepad support rather than
refusing to start.

## Maintenance note

Upstream was archived in 2023. That is a real risk and it is accepted
deliberately: the gamepad is optional, its absence degrades to keyboard and
mouse, and the protocol it speaks is frozen along with the driver. If the driver
ever stops working on a future Windows, the replacement is a different driver —
not a newer version of this file.
