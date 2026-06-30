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

#pragma once

#include <QByteArray>
#include <cstdint>

// AES-128-GCM encryption for GameStream input packets.
// Matches moonlight-common-c encryptData() / Sunshine's IDX_INPUT_DATA handler.
class InputCrypto
{
public:
    InputCrypto(const QByteArray& aesKey, uint32_t rikeyid);
    ~InputCrypto();

    // Encrypt plaintext → [GCM tag 16B][ciphertext] + update IV
    QByteArray encrypt(const QByteArray& plaintext);

    // Encrypt + prepend BE length → [BE(tag+cipher_len)][tag][ciphertext]
    QByteArray wrapAndEncrypt(const QByteArray& plaintext);

private:
    unsigned char m_Key[16];
    unsigned char m_Iv[16];
};
