/*
 * Moonlight-Web — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "streaming/InputCrypto.h"

#include <QtEndian>

void run_input_crypto_tests()
{
    SECTION("InputCrypto");

    QByteArray key(16, char(0x42));

    // encrypt(): output = [tag(16)][ciphertext(N)].
    {
        InputCrypto c(key, 0x01020304u);
        QByteArray plain("hello world", 11);
        QByteArray out = c.encrypt(plain);
        CHECK_EQ(out.size(), 16 + plain.size());
    }

    // Empty plaintext still yields a 16-byte GCM tag.
    {
        InputCrypto c(key, 0);
        CHECK_EQ(c.encrypt(QByteArray()).size(), 16);
    }

    // IV rotates after each call → identical plaintext encrypts differently.
    {
        InputCrypto c(key, 7);
        QByteArray p("packet", 6);
        QByteArray a = c.encrypt(p);
        QByteArray b = c.encrypt(p);
        CHECK(a != b);
    }

    // wrapAndEncrypt prepends a 4-byte big-endian length of [tag][cipher].
    {
        InputCrypto c(key, 9);
        QByteArray p("XY", 2);
        QByteArray wrapped = c.wrapAndEncrypt(p);
        CHECK_EQ(wrapped.size(), 4 + 16 + p.size());
        quint32 len = qFromBigEndian<quint32>(wrapped.constData());
        CHECK_EQ(len, static_cast<quint32>(16 + p.size()));
    }
}
