/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "InputCrypto.h"

#include <QDebug>
#include <QtEndian>

extern "C" {
#include <openssl/evp.h>
#include <openssl/err.h>
}

InputCrypto::InputCrypto(const QByteArray& aesKey, uint32_t rikeyid)
{
    // Key = raw 16 bytes (AES-128)
    Q_ASSERT(aesKey.size() == 16);
    std::memcpy(m_Key, aesKey.constData(), 16);

    // IV = rikeyid as BE uint32 in first 4 bytes, rest zeroed
    std::memset(m_Iv, 0, sizeof(m_Iv));
    qToBigEndian(rikeyid, m_Iv);
}

InputCrypto::~InputCrypto() = default;

QByteArray InputCrypto::encrypt(const QByteArray& plaintext)
{
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        qWarning() << "[InputCrypto] EVP_CIPHER_CTX_new failed";
        return {};
    }

    if (EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr) != 1) {
        qWarning() << "[InputCrypto] EVP_EncryptInit_ex(algo) failed";
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    // GCM IV length = 16 bytes (matching moonlight-qt)
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 16, nullptr) != 1) {
        qWarning() << "[InputCrypto] SET_IVLEN failed";
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, m_Key, m_Iv) != 1) {
        qWarning() << "[InputCrypto] EVP_EncryptInit_ex(key/iv) failed";
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    // Encrypt: output = [tag(16)][ciphertext(N)]  (tag first, matching moonlight-qt)
    int plainLen = plaintext.size();
    QByteArray out(16 + plainLen, '\0');

    int outLen = 0;
    if (EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(out.data()) + 16, &outLen,
                          reinterpret_cast<const unsigned char*>(plaintext.constData()),
                          plainLen) != 1) {
        qWarning() << "[InputCrypto] EVP_EncryptUpdate failed";
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    int finalLen = 0;
    if (EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(out.data()) + 16 + outLen,
                            &finalLen) != 1) {
        qWarning() << "[InputCrypto] EVP_EncryptFinal_ex failed";
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    // Get GCM tag (first 16 bytes of output)
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, out.data()) != 1) {
        qWarning() << "[InputCrypto] GET_TAG failed";
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    EVP_CIPHER_CTX_free(ctx);

    // IV rotation: last 16 bytes of [tag][ciphertext] → next IV
    // Matches moonlight-common-c InputStream.c and Sunshine stream.cpp
    std::memcpy(m_Iv, out.constData() + out.size() - 16, 16);

    return out;
}

QByteArray InputCrypto::wrapAndEncrypt(const QByteArray& plaintext)
{
    QByteArray taggedCipher = encrypt(plaintext);
    if (taggedCipher.isEmpty()) return {};

    // Prepend BE length of tagged_cipher
    uint32_t len = static_cast<uint32_t>(taggedCipher.size());
    QByteArray result(4 + taggedCipher.size(), '\0');
    qToBigEndian(len, result.data());
    std::memcpy(result.data() + 4, taggedCipher.constData(), taggedCipher.size());

    return result;
}
