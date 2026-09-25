/*
 * pcbc_encrypt is DES_pcbc_encrypt from OpenSSL 3.6.4 (crypto/des/pcbc_enc.c),
 * with the macros of crypto/des/des_local.h inlined:
 *   Copyright 1995-2020 The OpenSSL Project Authors. All Rights Reserved.
 *   Licensed under the Apache License 2.0 (https://www.openssl.org/source/license.html)
 * The rest is a test harness. See docs/design/openssl-windows.md.
 */
/*
 * MSVC 19.51 (14.51) ARM64 miscompile with /Gs0: a prologue calls __chkstk
 * before LR is saved, so the function returns into itself.
 *
 *   vcvarsall.bat amd64_arm64
 *   cl /nologo /O2 /Gs0 msvc-arm64-gs0-repro.c   -> on ARM64: 0xC0000005, no "ok"
 *   cl /nologo /O2      msvc-arm64-gs0-repro.c   -> prints "ok"
 *
 * MSVC 19.51.36248 compiles the prologue of pcbc_encrypt as
 *     sub  sp,sp,#0x50
 *     mov  x15,#1
 *     bl   __chkstk        ; LR := address of the next instruction
 *     sub  sp,sp,x15,lsl #4
 *     ...
 *     str  lr,[sp,#0x50]   ; saves that address, not the caller's
 * and the epilogue's `ret` lands back on `sub sp,sp,x15,lsl #4`.
 * MSVC 14.44 saves LR before the call. Seen first as OpenSSL crashing on every
 * TLS handshake (tls_parse_all_extensions has the same prologue).
 */
#include <stdio.h>
#include <string.h>

typedef unsigned int DES_LONG;
typedef unsigned char DES_cblock[8];
typedef struct { DES_LONG ks[32]; } DES_key_schedule;

#define c2l(c, l) (l = ((DES_LONG)(*((c)++))), l |= ((DES_LONG)(*((c)++))) << 8L, \
    l |= ((DES_LONG)(*((c)++))) << 16L, l |= ((DES_LONG)(*((c)++))) << 24L)
#define c2ln(c, l1, l2, n) { c += n; l1 = l2 = 0; switch (n) { \
    case 8: l2 = ((DES_LONG)(*(--(c)))) << 24L; \
    case 7: l2 |= ((DES_LONG)(*(--(c)))) << 16L; \
    case 6: l2 |= ((DES_LONG)(*(--(c)))) << 8L; \
    case 5: l2 |= ((DES_LONG)(*(--(c)))); \
    case 4: l1 = ((DES_LONG)(*(--(c)))) << 24L; \
    case 3: l1 |= ((DES_LONG)(*(--(c)))) << 16L; \
    case 2: l1 |= ((DES_LONG)(*(--(c)))) << 8L; \
    case 1: l1 |= ((DES_LONG)(*(--(c)))); } }
#define l2c(l, c) (*((c)++) = (unsigned char)(((l)) & 0xff), \
    *((c)++) = (unsigned char)(((l) >> 8L) & 0xff), \
    *((c)++) = (unsigned char)(((l) >> 16L) & 0xff), \
    *((c)++) = (unsigned char)(((l) >> 24L) & 0xff))
#define l2cn(l1, l2, c, n) { c += n; switch (n) { \
    case 8: *(--(c)) = (unsigned char)(((l2) >> 24L) & 0xff); \
    case 7: *(--(c)) = (unsigned char)(((l2) >> 16L) & 0xff); \
    case 6: *(--(c)) = (unsigned char)(((l2) >> 8L) & 0xff); \
    case 5: *(--(c)) = (unsigned char)(((l2)) & 0xff); \
    case 4: *(--(c)) = (unsigned char)(((l1) >> 24L) & 0xff); \
    case 3: *(--(c)) = (unsigned char)(((l1) >> 16L) & 0xff); \
    case 2: *(--(c)) = (unsigned char)(((l1) >> 8L) & 0xff); \
    case 1: *(--(c)) = (unsigned char)(((l1)) & 0xff); } }

/* Stand-in for the block cipher: any out-of-line call will do. */
__declspec(noinline) void encrypt1(DES_LONG *data, DES_key_schedule *ks, int enc)
{
    data[0] ^= ks->ks[0] + (DES_LONG)enc;
    data[1] ^= ks->ks[1];
}

__declspec(noinline) void pcbc_encrypt(const unsigned char *input, unsigned char *output,
    long length, DES_key_schedule *schedule, DES_cblock *ivec, int enc)
{
    register DES_LONG sin0, sin1, xor0, xor1, tout0, tout1;
    DES_LONG tin[2];
    const unsigned char *in;
    unsigned char *out, *iv;

    in = input;
    out = output;
    iv = &(*ivec)[0];

    if (enc) {
        c2l(iv, xor0);
        c2l(iv, xor1);
        for (; length > 0; length -= 8) {
            if (length >= 8) {
                c2l(in, sin0);
                c2l(in, sin1);
            } else
                c2ln(in, sin0, sin1, length);
            tin[0] = sin0 ^ xor0;
            tin[1] = sin1 ^ xor1;
            encrypt1((DES_LONG *)tin, schedule, 1);
            tout0 = tin[0];
            tout1 = tin[1];
            xor0 = sin0 ^ tout0;
            xor1 = sin1 ^ tout1;
            l2c(tout0, out);
            l2c(tout1, out);
        }
    } else {
        c2l(iv, xor0);
        c2l(iv, xor1);
        for (; length > 0; length -= 8) {
            c2l(in, sin0);
            c2l(in, sin1);
            tin[0] = sin0;
            tin[1] = sin1;
            encrypt1((DES_LONG *)tin, schedule, 0);
            tout0 = tin[0] ^ xor0;
            tout1 = tin[1] ^ xor1;
            if (length >= 8) {
                l2c(tout0, out);
                l2c(tout1, out);
            } else
                l2cn(tout0, tout1, out, length);
            xor0 = tout0 ^ sin0;
            xor1 = tout1 ^ sin1;
        }
    }
    tin[0] = tin[1] = 0;
    sin0 = sin1 = xor0 = xor1 = tout0 = tout1 = 0;
}

int main(void)
{
    static unsigned char in[16] = "0123456789abcde", out[16];
    static DES_key_schedule ks;
    DES_cblock iv = {0};

    pcbc_encrypt(in, out, sizeof(in), &ks, &iv, 1);
    puts("ok");
    return 0;
}
