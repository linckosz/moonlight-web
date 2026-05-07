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
