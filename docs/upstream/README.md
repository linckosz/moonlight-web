# Fixes that belong upstream

Patches kept here are complete and ready to send — not sketches. Each one names
the project, the commit it was written against, and what MoonlightWeb has to do
locally for as long as it is not merged and deployed. When one lands and reaches
the releases people actually run, the local workaround it excuses can go.

Apply with `git am` from a clean checkout of the target project.

## `inputtino-scroll-remainder.patch`

- **Project:** [games-on-whales/inputtino](https://github.com/games-on-whales/inputtino)
- **Written against:** `d28ec79` (2026-07-13)
- **Verified:** applies cleanly with `git apply --check` on that commit
- **Status:** not submitted

`Mouse::vertical_scroll()` turns the client's high-resolution amount into
`REL_WHEEL = high_res_distance / 120` and keeps no remainder, so every amount
below one detent floors to zero and is gone. A mouse wheel is unaffected — one
notch is exactly 120 — but a trackpad, a free-spinning wheel or a touch drag
report far finer than that, and on those the host scrolls by nothing at all.
`REL_WHEEL_HI_RES` is emitted alongside with the exact amount, which saves
clients that consume it; X11 sessions routinely do not.

The patch keeps the leftover per axis and emits detents as they add up.

**Who is affected.** inputtino is the input stack of Wolf, and of every Sunshine
release up to and including `v2026.516.143833`. Sunshine moved to LizardByte's
`libvirtualhid` in `v2026.830.44125`, whose uinput backend already does exactly
this (`accumulated_legacy_scroll()`, `src/platform/linux/uhid_backend.cpp`) —
so the fix below is the behaviour LizardByte has already settled on, which is
worth saying in the PR.

**What MoonlightWeb does meanwhile.** `MoonlightShim::quantizeScroll()` holds
sub-notch amounts back and sends whole notches, for hosts we believe run Linux
(`HostOsProbe`). That workaround costs those hosts smooth scrolling and cannot
be removed until the fix is in the Wolf and Sunshine builds people run — which
is a long way off, so do not treat merging it as the end of the story.
