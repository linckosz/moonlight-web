# OpenSSL on Windows — where the DLLs come from, and why not from the CI

> The Windows packages ship OpenSSL as two DLLs next to the executable
> (`libssl-3-<arch>.dll`, `libcrypto-3-<arch>.dll`). They are **committed**,
> under `backend/libs/windows/lib/<arch>/` with their import libraries and
> `backend/libs/windows/include/<arch>/`, and `backend/CMakeLists.txt` links
> and installs them. The CI never builds OpenSSL.

## 1. Where each set comes from

| Arch  | Version | Built by | Record |
|-------|---------|----------|--------|
| x64   | 3.6.2   | the moonlight-stream dependency build (`VC-WIN64A-masm`), committed in June 2026 | the DLLs' own `OpenSSL_version(OPENSSL_CFLAGS)` string |
| arm64 | 3.6.4   | `scripts/build-openssl-windows.ps1`, MSVC 14.51 | `lib/arm64/BUILD-INFO.txt` (Configure line, compiler, SHA-256 of every file) |

Refreshing either is one command, from a machine with Visual Studio (or its
Build Tools) and the C++ toolset of the target:

```powershell
scripts\build-openssl-windows.ps1 -Arch arm64 -Install   # needs a clang-cl for the ARM64 assembly
scripts\build-openssl-windows.ps1 -Arch x64 -Install
```

The script downloads the OpenSSL release tarball and checks it against a
pinned SHA-256 (add the new one to `$OpenSslSha256` when moving version), as
well as Perl and NASM. ARM64 assembly goes through `clang-cl`: Visual
Studio's "C++ Clang tools" component, an LLVM install, or `-ClangCl <path>`.
Review `BUILD-INFO.txt` in the diff, then run the ARM64 installer from the CI
on the Snapdragon bench before merging (the CI smoke test below catches a dead
TLS stack, not a subtle one).

## 2. Why not build it in the CI — September 2026

Until then the ARM64 job ran `vcpkg install openssl:arm64-windows`, so
OpenSSL was compiled on every run by whatever MSVC the `windows-11-arm` image
carried. In September 2026 the image moved from Visual Studio 2022 (MSVC
14.44) to Visual Studio 18 (MSVC 14.51), and the same OpenSSL 3.6.4 source
came out crashing: the app died with an access violation in
`libssl-3-arm64.dll` about a second after `Server ready`, on every start —
the first TLS handshake it made. Nothing in the version numbers had changed.

### What the compiler does

OpenSSL's Windows targets compile with `/Gs0`: a stack probe (`__chkstk`) in
every function, however small its frame. MSVC 14.51 for ARM64 sometimes
places that call **before the return address is saved**. In a function it
shrink-wraps — the register saves pushed below an early test — the prologue
comes out as:

```
tls_parse_all_extensions:
    sub   sp,sp,#0x60
    mov   x15,#1
    bl    __chkstk          ; LR := the next instruction
    sub   sp,sp,x15,lsl #4
    ...
    str   lr,[sp,#0x60]     ; saves that address, not the caller's
```

`bl` writes its own return address into LR, so the LR saved afterwards points
back into the prologue. When the function returns, it lands on `sub
sp,sp,x15,lsl #4` with its return value (1, success) in `x0`, runs its body
again with `x0` taken for the `SSL_CONNECTION *`, and faults on
`ldr x8,[x0,#0x878]`. A debugger shows it as an endless recursion of the same
frame. In OpenSSL 3.6.4 three functions have that prologue:
`tls_parse_all_extensions` (every handshake), `DES_pcbc_encrypt` and
`IDEA_cfb64_encrypt`. MSVC 14.44 saves LR before the call.

It is a compiler defect, not an OpenSSL one: `docs/design/msvc-arm64-gs0-repro.c` is
a self-contained program (OpenSSL's `DES_pcbc_encrypt`, macros inlined) that
prints "ok" built with `cl /O2`, and dies with `0xC0000005` built with
`cl /O2 /Gs0`, on ARM64 with MSVC 19.51.36248.

### What we do about it

- **ARM64 OpenSSL is built with `-Gs4096`** (after OpenSSL's `/Gs0`; the last
  one wins): the compiler's default threshold, one page. Only frames that could
  step over the guard page are probed — which is the whole purpose of a probe
  — and libssl goes from 687 probes to 4, libcrypto from 2,734 to 19.
- **`scripts/check-arm64-chkstk.ps1`** disassembles a binary (its PDB beside
  it) and fails on any function that calls `__chkstk` before saving LR. The
  build script runs it on both DLLs; the release workflow runs it on the ARM64
  executable after every build, since a function of ours with a frame over
  4 KB is exposed to the same defect and the compiler is whatever the image
  carries. The executable built by MSVC 14.51 in September 2026 was clean (132
  probes, all after LR).
- **The release workflow starts the packaged app** (both architectures):
  health check over HTTPS, then `--status`, which is an HTTPS client of the
  running server through the same OpenSSL, then checks that the server is
  still alive ten seconds later. That is the test that would have caught the
  September build before it reached a machine.

## 3. Reading an ARM64 crash in these DLLs

Windows Error Reporting keeps dumps in `%LOCALAPPDATA%\CrashDumps` on the
bench. `cdb.exe` from the x64 Windows SDK reads ARM64 dumps; point `-y` at a
folder holding the matching DLLs and PDBs (the PDBs stay in the build folder of
`build-openssl-windows.ps1`, they are not committed). On a fault in a prologue,
`k` walks LR and shows nonsense; read `dps @sp` instead.
