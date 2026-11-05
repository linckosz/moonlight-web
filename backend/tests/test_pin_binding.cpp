/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * MW-PIN-v1 and MW-TLS-v1 (docs/design/tls-key-proof.md): the PIN proved and
 * never sent, bound to the channel it was typed on; a TLS key vouched for by the
 * host key.
 *
 * The scenario throughout is a machine in the middle of a run: it shows the
 * client a TLS key of its own, and both rounds go through it. Each check below
 * is one way of using that position, and each must fail closed. The SPAKE2 core
 * is pinned by RFC 9382's own vectors, and the MW-PIN-v1 framing by a run the
 * Android TV app's tests compute with the same inputs (moonlight-web-android-tv,
 * PinProofTest.aKnownRunGivesTheBytesTheHostChecks). The routes themselves are
 * driven in test_auth_manager.cpp.
 */

#include "test_framework.h"

#include "common/PairingCrypto.h"
#include "common/PinBinding.h"

#include <QCryptographicHash>
#include <QString>
#include <QStringList>

#include <cstring>
#include <utility>

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/x509.h>

namespace {

struct Vector
{
    const char* idA;
    const char* idB;
    const char* w;
    const char* x;
    const char* pA;
    const char* y;
    const char* pB;
    const char* k;
    const char* tt;
    const char* ke;
    const char* ka;
    const char* kcA;
    const char* kcB;
    const char* confirmA;
    const char* confirmB;
};

// RFC 9382, Appendix B (P256-SHA256-HKDF-HMAC), joined from the RFC's text.
const Vector kVectors[] = {
    {"server", "client", "2ee57912099d31560b3a44b1184b9b4866e904c49d12ac5042c97dca461b1a5f",
     "43dd0fd7215bdcb482879fca3220c6a968e66d70b1356cac18bb26c84a78d729",
     "04a56fa807caaa53a4d28dbb9853b9815c61a411118a6fe516a8798434751470f9010153"
     "ac33d0d5f2047ffdb1a3e42c9b4e6be662766e1eeb4116988ede5f912c",
     "dcb60106f276b02606d8ef0a328c02e4b629f84f89786af5befb0bc75b6e66be",
     "0406557e482bd03097ad0cbaa5df82115460d951e3451962f1eaf4367a420676d09857cc"
     "bc522686c83d1852abfa8ed6e4a1155cf8f1543ceca528afb591a1e0b7",
     "0412af7e89717850671913e6b469ace67bd90a4df8ce45c2af19010175e37eed69f75897"
     "996d539356e2fa6a406d528501f907e04d97515fbe83db277b715d3325",
     "06000000000000007365727665720600000000000000636c69656e744100000000000000"
     "04a56fa807caaa53a4d28dbb9853b9815c61a411118a6fe516a8798434751470f9010153"
     "ac33d0d5f2047ffdb1a3e42c9b4e6be662766e1eeb4116988ede5f912c41000000000000"
     "000406557e482bd03097ad0cbaa5df82115460d951e3451962f1eaf4367a420676d09857"
     "ccbc522686c83d1852abfa8ed6e4a1155cf8f1543ceca528afb591a1e0b7410000000000"
     "00000412af7e89717850671913e6b469ace67bd90a4df8ce45c2af19010175e37eed69f7"
     "5897996d539356e2fa6a406d528501f907e04d97515fbe83db277b715d33252000000000"
     "0000002ee57912099d31560b3a44b1184b9b4866e904c49d12ac5042c97dca461b1a5f",
     "0e0672dc86f8e45565d338b0540abe69", "15bdf72e2b35b5c9e5663168e960a91b",
     "00c12546835755c86d8c0db7851ae86f", "a9fa3406c3b781b93d804485430ca27a",
     "58ad4aa88e0b60d5061eb6b5dd93e80d9c4f00d127c65b3b35b1b5281fee38f0",
     "d3e2e547f1ae04f2dbdbf0fc4b79f8ecff2dff314b5d32fe9fcef2fb26dc459b"},
    {"", "client", "0548d8729f730589e579b0475a582c1608138ddf7054b73b5381c7e883e2efae",
     "403abbe3b1b4b9ba17e3032849759d723939a27a27b9d921c500edde18ed654b",
     "04a897b769e681c62ac1c2357319a3d363f610839c4477720d24cbe32f5fd85f44fb92ba"
     "966578c1b712be6962498834078262caa5b441ecfa9d4a9485720e918a",
     "903023b6598908936ea7c929bd761af6039577a9c3f9581064187c3049d87065",
     "04e0f816fd1c35e22065d5556215c097e799390d16661c386e0ecc84593974a61b881a8c"
     "82327687d0501862970c64565560cb5671f696048050ca66ca5f8cc7fc",
     "048f83ec9f6e4f87cc6f9dc740bdc2769725f923364f01c84148c049a39a735ebda82eac"
     "03e00112fd6a5710682767cff5361f7e819e53d8d3c3a2922e0d837aa6",
     "00000000000000000600000000000000636c69656e74410000000000000004a897b769e6"
     "81c62ac1c2357319a3d363f610839c4477720d24cbe32f5fd85f44fb92ba966578c1b712"
     "be6962498834078262caa5b441ecfa9d4a9485720e918a410000000000000004e0f816fd"
     "1c35e22065d5556215c097e799390d16661c386e0ecc84593974a61b881a8c82327687d0"
     "501862970c64565560cb5671f696048050ca66ca5f8cc7fc4100000000000000048f83ec"
     "9f6e4f87cc6f9dc740bdc2769725f923364f01c84148c049a39a735ebda82eac03e00112"
     "fd6a5710682767cff5361f7e819e53d8d3c3a2922e0d837aa620000000000000000548d8"
     "729f730589e579b0475a582c1608138ddf7054b73b5381c7e883e2efae",
     "642f05c473c2cd79909f9a841e2f30a7", "0bf89b18180af97353ba198789c2b963",
     "c6be376fc7cd1301fd0a13adf3e7bffd", "b7243f4ae60440a49b3f8cab3c1fba07",
     "47d29e6666af1b7dd450d571233085d7a9866e4d49d2645e2df975489521232b",
     "3313c5cefc361d27fb16847a91c2a73b766ffa90a4839122a9b70a2f6bd1d6df"},
    {"server", "", "626e0cdc7b14c9db3e52a0b1b3a768c98e37852d5db30febe0497b14eae8c254",
     "07adb3db6bc623d3399726bfdbfd3d15a58ea776ab8a308b00392621291f9633",
     "04f88fb71c99bfffaea370966b7eb99cd4be0ff1a7d335caac4211c4afd855e2e15a873b"
     "298503ad8ba1d9cbb9a392d2ba309b48bfd7879aefd0f2cea6009763b0",
     "b6a4fc8dbb629d4ba51d6f91ed1532cf87adec98f25dd153a75accafafedec16",
     "040c269d6be017dccb15182ac6bfcd9e2a14de019dd587eaf4bdfd353f031101e7cca177"
     "f8eb362a6e83e7d5e729c0732e1b528879c086f39ba0f31a9661bd34db",
     "0445ee233b8ecb51ebd6e7da3f307e88a1616bae2166121221fdc0dadb986afaf3ec8a98"
     "8dc9c626fa3b99f58a7ca7c9b844bb3e8dd9554aafc5b53813504c1cbe",
     "06000000000000007365727665720000000000000000410000000000000004f88fb71c99"
     "bfffaea370966b7eb99cd4be0ff1a7d335caac4211c4afd855e2e15a873b298503ad8ba1"
     "d9cbb9a392d2ba309b48bfd7879aefd0f2cea6009763b04100000000000000040c269d6b"
     "e017dccb15182ac6bfcd9e2a14de019dd587eaf4bdfd353f031101e7cca177f8eb362a6e"
     "83e7d5e729c0732e1b528879c086f39ba0f31a9661bd34db41000000000000000445ee23"
     "3b8ecb51ebd6e7da3f307e88a1616bae2166121221fdc0dadb986afaf3ec8a988dc9c626"
     "fa3b99f58a7ca7c9b844bb3e8dd9554aafc5b53813504c1cbe2000000000000000626e0c"
     "dc7b14c9db3e52a0b1b3a768c98e37852d5db30febe0497b14eae8c254",
     "005184ff460da2ce59062c87733c299c", "3521297d736598fc0a1127600efa1afb",
     "f3da53604f0aeecea5a33be7bddf6edf", "9e3f86848736f159bd92b6e107ec6799",
     "bc9f9bbe99f26d0b2260e6456e05a86196a3307ec6663a18bf6ac825736533b2",
     "c2370e1bf813b086dff0d834e74425a06e6390f48f5411900276dcccc5a297ec"},
    {"", "", "7bf46c454b4c1b25799527d896508afd5fc62ef4ec59db1efb49113063d70cca",
     "8cef65df64bb2d0f83540c53632de911b5b24b3eab6cc74a97609fd659e95473",
     "04a65b367a3f613cf9f0654b1b28a1e3a8a40387956c8ba6063e8658563890f46ca1ef6a"
     "676598889fc28de2950ab8120b79a5ef1ea4c9f44bc98f585634b46d66",
     "d7a66f64074a84652d8d623a92e20c9675c61cb5b4f6a0063e4648a2fdc02d53",
     "04589f13218822710d98d8b2123a079041052d9941b9cf88c6617ddb2fcc0494662eea8b"
     "a6b64692dc318250030c6af045cb738bc81ba35b043c3dcb46adf6f58d",
     "041a3c03d51b452537ca2a1fea6110353c6d5ed483c4f0f86f4492ca3f378d40a994b447"
     "7f93c64d928edbbcd3e85a7c709b7ea73ee97986ce3d1438e135543772",
     "00000000000000000000000000000000410000000000000004a65b367a3f613cf9f0654b"
     "1b28a1e3a8a40387956c8ba6063e8658563890f46ca1ef6a676598889fc28de2950ab812"
     "0b79a5ef1ea4c9f44bc98f585634b46d66410000000000000004589f13218822710d98d8"
     "b2123a079041052d9941b9cf88c6617ddb2fcc0494662eea8ba6b64692dc318250030c6a"
     "f045cb738bc81ba35b043c3dcb46adf6f58d4100000000000000041a3c03d51b452537ca"
     "2a1fea6110353c6d5ed483c4f0f86f4492ca3f378d40a994b4477f93c64d928edbbcd3e8"
     "5a7c709b7ea73ee97986ce3d1438e13554377220000000000000007bf46c454b4c1b2579"
     "9527d896508afd5fc62ef4ec59db1efb49113063d70cca",
     "fc6374762ba5cf11f4b2caa08b2cd1b9", "907ae0e26e8d6234318d91583cd74c86",
     "5dbd2f477166b7fb6d61febbd77a5563", "7689b4654407a5faeffdc8f18359d8a3",
     "dfb4db8d48ae5a675963ea5e6c19d98d4ea028d8e898dad96ea19a80ade95dca",
     "d0f0609d1613138d354f7e95f19fb556bf52d751947241e8c7118df5ef0ae175"},
};

QByteArray hex(const char* s)
{
    return QByteArray::fromHex(QByteArray(s));
}

QString hexOf(const QByteArray& b)
{
    return QString::fromLatin1(b.toHex());
}

/// P-256's base point as a public key (SPKI): a valid key whose private half is
/// 1, the host identity of the known run.
const char kBasePointKey[] = "3059301306072a8648ce3d020106082a8648ce3d03010703420004"
                             "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
                             "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5";

/// What the TV's test found for the known run (PinProofTest.KNOWN_RUN): PIN
/// "123456", the RFC's first vector's x and y, client identity "MW-PIN-v1
/// vector client", host identity kBasePointKey, a TLS channel whose key hashes to
/// 32 bytes of 0x07.
const char kKnownRun[] =
    "w=6778573e40a8614d75e6fa68e482b50a0ad926c0977bad3475bf90204ae3ac54\n"
    "pA=04bead5eb2f29dbd797deef9618804db7b80da83713c2f37f5fa045c1e79432b"
    "1ce4ca633cc320915cb868c58fb2c12a8daaf8e6eb44c4255cc6ae319dd0f3c993\n"
    "pB=04a04c503b1c98b93c226d08945917fb336de87722fbf0d103396b10a960a407"
    "64f1abf562ab779c5d32815312da8543730715c3eac2ba29268982ec802bb17f4b\n"
    "confirmA=28d970361fd49d210a71fb87a20136c12b5a9ff95729a27ed406d6fc81e0e824\n"
    "confirmB=628d87e445ced1d309c8a83cde3e8526bdd56b6210295f7c84e3c231bf742dcd";

/// A self-signed P-256 certificate, DER, and the SPKI of its key.
std::pair<QByteArray, QByteArray> selfSignedCertificate()
{
    std::pair<QByteArray, QByteArray> out;
    EVP_PKEY* key = EVP_EC_gen("P-256");
    X509* cert = X509_new();
    if (key && cert) {
        X509_set_version(cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert), 0);
        X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
        X509_set_pubkey(cert, key);
        X509_NAME* name = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("MoonlightWeb"), -1, -1,
                                   0);
        X509_set_issuer_name(cert, name);
        if (X509_sign(cert, key, EVP_sha256()) > 0) {
            unsigned char* der = nullptr;
            int len = i2d_X509(cert, &der);
            if (len > 0) out.first = QByteArray(reinterpret_cast<const char*>(der), len);
            OPENSSL_free(der);
            der = nullptr;
            len = i2d_PUBKEY(key, &der);
            if (len > 0) out.second = QByteArray(reinterpret_cast<const char*>(der), len);
            OPENSSL_free(der);
        }
    }
    X509_free(cert);
    EVP_PKEY_free(key);
    return out;
}

/// One run between a client that typed @p typed on @p clientChannel and this host
/// showing @p shown on @p hostChannel: whether the host takes the client's
/// confirmation, and the client this host's.
std::pair<bool, bool> run(const QString& typed, const QString& shown,
                          const QByteArray& clientChannel, const QByteArray& hostChannel)
{
    const QByteArray client = QByteArrayLiteral("client key");
    const QByteArray host = hex(kBasePointKey);
    const QByteArray wClient = PinBinding::pinScalar(typed);
    const QByteArray wHost = PinBinding::pinScalar(shown);
    const auto a = PinBinding::shareA(wClient);
    const auto b = PinBinding::shareB(wHost);
    if (!a || !b) return {false, false};
    const auto keysA = PinBinding::finishA(wClient, *a, b->element, client, host,
                                           PinBinding::associatedData(clientChannel));
    const auto keysB = PinBinding::finishB(wHost, *b, a->element, client, host,
                                           PinBinding::associatedData(hostChannel));
    if (!keysA || !keysB) return {false, false};
    return {PinBinding::sameBytes(keysA->confirmA, keysB->confirmA),
            PinBinding::sameBytes(keysA->confirmB, keysB->confirmB)};
}

QByteArray tlsChannelOfDigest(const QByteArray& digest)
{
    return PairingCrypto::buildSignedMessage({QByteArrayLiteral("tls"), digest});
}

/// w·M, uncompressed: the share for which, with the right w, the host's K falls
/// on the identity (RFC 9382 §7). M as the RFC gives it.
QByteArray wTimesM(const QByteArray& w)
{
    const QByteArray m = hex("02886e2f97ace46e55ba9dd7242579f2993b64e16ef3dcab95afd497333d8fa12f");
    QByteArray out;
    EC_GROUP* group = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
    EC_POINT* point = group ? EC_POINT_new(group) : nullptr;
    EC_POINT* product = group ? EC_POINT_new(group) : nullptr;
    BIGNUM* scalar = BN_bin2bn(reinterpret_cast<const unsigned char*>(w.constData()),
                               static_cast<int>(w.size()), nullptr);
    unsigned char buf[65];
    if (point && product && scalar &&
        EC_POINT_oct2point(group, point, reinterpret_cast<const unsigned char*>(m.constData()),
                           static_cast<size_t>(m.size()), nullptr) == 1 &&
        EC_POINT_mul(group, product, nullptr, point, scalar, nullptr) == 1 &&
        EC_POINT_point2oct(group, product, POINT_CONVERSION_UNCOMPRESSED, buf, sizeof buf,
                           nullptr) == sizeof buf)
        out = QByteArray(reinterpret_cast<const char*>(buf), sizeof buf);
    BN_free(scalar);
    EC_POINT_free(product);
    EC_POINT_free(point);
    EC_GROUP_free(group);
    return out;
}

} // namespace

void run_pin_binding_tests()
{
    using namespace PinBinding;

    SECTION("MW-PIN-v1 — RFC 9382's vectors, byte for byte");

    for (const Vector& v : kVectors) {
        const QByteArray w = hex(v.w);
        const auto a = shareA(w, hex(v.x));
        const auto b = shareB(w, hex(v.y));
        CHECK(a.has_value() && b.has_value());
        if (!a || !b) continue;
        CHECK_EQ(hexOf(a->element), QString::fromLatin1(v.pA));
        CHECK_EQ(hexOf(b->element), QString::fromLatin1(v.pB));
        const auto keysA = finishA(w, *a, b->element, QByteArray(v.idA), QByteArray(v.idB), {});
        const auto keysB = finishB(w, *b, a->element, QByteArray(v.idA), QByteArray(v.idB), {});
        CHECK(keysA.has_value() && keysB.has_value());
        if (!keysA || !keysB) continue;
        for (const Keys* keys : {&*keysA, &*keysB}) {
            CHECK_EQ(hexOf(keys->transcript), QString::fromLatin1(v.tt));
            CHECK_EQ(hexOf(keys->ke), QString::fromLatin1(v.ke));
            CHECK_EQ(hexOf(keys->ka), QString::fromLatin1(v.ka));
            CHECK_EQ(hexOf(keys->kcA), QString::fromLatin1(v.kcA));
            CHECK_EQ(hexOf(keys->kcB), QString::fromLatin1(v.kcB));
            CHECK_EQ(hexOf(keys->confirmA), QString::fromLatin1(v.confirmA));
            CHECK_EQ(hexOf(keys->confirmB), QString::fromLatin1(v.confirmB));
        }
        // K sits in the transcript after the two elements, each behind its length.
        const int at = 8 + int(qstrlen(v.idA)) + 8 + int(qstrlen(v.idB)) + (8 + 65) * 2 + 8;
        CHECK_EQ(hexOf(keysA->transcript.mid(at, 65)), QString::fromLatin1(v.k));
    }

    SECTION("MW-PIN-v1 — framed as the TV frames it");

    {
        const QByteArray sevens(32, '\x07');
        CHECK_EQ(hexOf(tlsChannelOfDigest(sevens)),
                 QStringLiteral("00000003746c7300000020") + hexOf(sevens));
        const QByteArray spki = hex(kBasePointKey);
        CHECK_EQ(tlsChannel(spki),
                 tlsChannelOfDigest(QCryptographicHash::hash(spki, QCryptographicHash::Sha256)));
        CHECK_EQ(hexOf(lineChannel(spki)),
                 QStringLiteral("000000046c696e6500000020") +
                     hexOf(QCryptographicHash::hash(spki, QCryptographicHash::Sha256)));
        CHECK(tlsChannel(QByteArray()).isEmpty());
        CHECK_EQ(hexOf(associatedData(tlsChannelOfDigest(sevens))),
                 QStringLiteral("000000094d572d50494e2d76310000002b") +
                     hexOf(tlsChannelOfDigest(sevens)));
        // w is the framed PIN hashed with SHA-512 and reduced: 32 bytes, and two
        // PINs a digit apart are two w.
        CHECK_EQ(pinScalar(QStringLiteral("123456")).size(), 32);
        CHECK(pinScalar(QStringLiteral("123456")) != pinScalar(QStringLiteral("123457")));
    }

    SECTION("MW-PIN-v1 — the run the TV computes, computed here");

    {
        const QByteArray w = pinScalar(QStringLiteral("123456"));
        const auto a = shareA(w, hex(kVectors[0].x));
        const auto b = shareB(w, hex(kVectors[0].y));
        CHECK(a.has_value() && b.has_value());
        if (a && b) {
            const QByteArray client = QByteArrayLiteral("MW-PIN-v1 vector client");
            const QByteArray aad = associatedData(tlsChannelOfDigest(QByteArray(32, '\x07')));
            const auto keys = finishB(w, *b, a->element, client, hex(kBasePointKey), aad);
            CHECK(keys.has_value());
            if (keys) {
                const QString got =
                    QStringLiteral("w=") + hexOf(w) + QStringLiteral("\npA=") + hexOf(a->element) +
                    QStringLiteral("\npB=") + hexOf(b->element) + QStringLiteral("\nconfirmA=") +
                    hexOf(keys->confirmA) + QStringLiteral("\nconfirmB=") + hexOf(keys->confirmB);
                CHECK_EQ(got, QString::fromLatin1(kKnownRun));
            }
        }
    }

    SECTION("MW-PIN-v1 — what a machine in the middle can do");

    {
        const QByteArray seen = tlsChannelOfDigest(QByteArray(32, '\x07'));
        const QByteArray served = tlsChannelOfDigest(QByteArray(32, '\x09'));
        // The honest run: both ends confirm.
        const auto honest = run(QStringLiteral("123456"), QStringLiteral("123456"), seen, seen);
        CHECK(honest.first && honest.second);
        // A wrong PIN: neither does.
        const auto wrong = run(QStringLiteral("123456"), QStringLiteral("654321"), seen, seen);
        CHECK(!wrong.first && !wrong.second);
        // THE case: the right PIN, relayed between two TLS keys. Same exchange,
        // and still no confirmation in either direction.
        const auto relayed = run(QStringLiteral("123456"), QStringLiteral("123456"), seen, served);
        CHECK(!relayed.first && !relayed.second);
    }

    SECTION("MW-PIN-v1 — only a point of the group is taken from the wire");

    {
        const QByteArray w = pinScalar(QStringLiteral("123456"));
        const auto a = shareA(w);
        const auto b = shareB(w);
        CHECK(a.has_value() && b.has_value());
        if (a && b) {
            const QByteArray good = b->element;
            auto finish = [&](const QByteArray& e) {
                return finishA(w, *a, e, {}, {}, {}).has_value();
            };
            CHECK(finish(good));
            CHECK(!finish(good.left(64)));
            CHECK(!finish(good + QByteArray(1, '\0')));
            CHECK(!finish(QByteArray(1, '\x02') + good.mid(1, 32)));
            QByteArray offCurve = good;
            offCurve[64] = char(offCurve[64] ^ 1);
            CHECK(!finish(offCurve));
            CHECK(!finish(QByteArray(1, '\0')));
            CHECK(!finish(QByteArray()));
            // The other share's scalar out of range is refused, not reduced.
            CHECK(!shareA(w, QByteArray(32, '\0')).has_value());
            CHECK(!shareA(w, QByteArray(32, '\xff')).has_value());
            // What the route may refuse up front, for it does not depend on the PIN.
            CHECK(isElement(good));
            CHECK(!isElement(good.left(64)));
            CHECK(!isElement(QByteArray(1, '\x02') + good.mid(1, 32)));
            CHECK(!isElement(offCurve));
            CHECK(!isElement(QByteArray(1, '\0')));
            CHECK(!isElement(QByteArray()));
            // A refused point leaves the thread's OpenSSL error queue empty, as a
            // refused signature does (test_pairing_binding.cpp).
            const QByteArray refused[] = {good.left(64), offCurve,
                                          QByteArray(1, '\x02') + good.mid(1, 32)};
            int left = 0;
            for (const QByteArray& e : refused) {
                ERR_clear_error();
                CHECK(!isElement(e));
                if (ERR_peek_error() != 0) ++left;
                ERR_clear_error();
                CHECK(!finish(e));
                if (ERR_peek_error() != 0) ++left;
            }
            CHECK_EQ(left, 0);
            ERR_clear_error();
            CHECK(finish(good));
            CHECK_EQ(ERR_peek_error(), 0ul);
        }
    }

    SECTION("MW-PIN-v1 — the share that would name the PIN");

    {
        // pA = w·M is a point like any other, so the route cannot refuse it up
        // front; with the right w the host's K is the identity and finishB() has no
        // keys to give. POST /api/auth/pin/start answers that run like any other,
        // and /finish never takes it, or a million uncounted requests would name
        // the PIN.
        const QByteArray w = pinScalar(QStringLiteral("123456"));
        const QByteArray probe = wTimesM(w);
        CHECK_EQ(probe.size(), 65);
        CHECK(isElement(probe));
        const auto b = shareB(w);
        CHECK(b.has_value());
        if (b) CHECK(!finishB(w, *b, probe, {}, {}, {}).has_value());
        // Under another PIN, the same share is an ordinary one.
        const QByteArray other = pinScalar(QStringLiteral("654321"));
        const auto b2 = shareB(other);
        CHECK(b2.has_value());
        if (b2) CHECK(finishB(other, *b2, probe, {}, {}, {}).has_value());
        // A run is provable unless the route says otherwise.
        CHECK(Exchanges::Pending().provable);
    }

    SECTION("MW-PIN-v1 — runs are single use, short-lived and capped");

    {
        Exchanges runs;
        Exchanges::Pending r;
        r.bucket = QStringLiteral("198.51.100.7");
        r.pin = QStringLiteral("123456");
        const QString id = runs.add(r, 1000);
        CHECK(!id.isEmpty());
        // Another caller cannot spend it, and it is still there for its own.
        CHECK(!runs.take(id, QStringLiteral("203.0.113.1"), 1001).has_value());
        CHECK(runs.take(id, r.bucket, 1001).has_value());
        CHECK(!runs.take(id, r.bucket, 1002).has_value()); // a second /finish finds nothing

        const QString late = runs.add(r, 1000);
        CHECK(!runs.take(late, r.bucket, 1000 + Exchanges::RUN_TTL_SECS).has_value());

        // One caller keeps a few, the oldest giving way.
        QString first;
        for (int i = 0; i < Exchanges::MAX_RUNS_PER_BUCKET + 1; ++i) {
            const QString added = runs.add(r, 2000);
            if (i == 0) first = added;
        }
        CHECK_EQ(runs.size(), Exchanges::MAX_RUNS_PER_BUCKET);
        CHECK(!runs.take(first, r.bucket, 2001).has_value());

        // And all callers together a few more: full, the table takes no new
        // caller's run, and every run it holds stays.
        r.bucket = QStringLiteral("203.0.113.0");
        const QString early = runs.add(r, 3000);
        int refused = 0;
        for (int i = 1; i < Exchanges::MAX_RUNS * 2; ++i) {
            r.bucket = QStringLiteral("203.0.113.%1").arg(i);
            if (runs.add(r, 3000).isEmpty()) ++refused;
        }
        CHECK_EQ(runs.size(), Exchanges::MAX_RUNS);
        CHECK_EQ(refused, Exchanges::MAX_RUNS);
        CHECK(!runs.hasRoom(QStringLiteral("198.51.100.8"), 3001));
        CHECK(runs.take(early, QStringLiteral("203.0.113.0"), 3001).has_value());
        CHECK(runs.hasRoom(QStringLiteral("198.51.100.8"), 3001)); // one ended: room for one
    }

    {
        // Full, a caller that holds its share of runs still starts one, in place
        // of its own oldest and of no other caller's.
        Exchanges runs;
        Exchanges::Pending r;
        r.pin = QStringLiteral("123456");
        r.bucket = QStringLiteral("198.51.100.9");
        QStringList own;
        for (int i = 0; i < Exchanges::MAX_RUNS_PER_BUCKET; ++i)
            own << runs.add(r, 4000);
        for (int i = 0; i < Exchanges::MAX_RUNS && runs.size() < Exchanges::MAX_RUNS; ++i) {
            r.bucket = QStringLiteral("203.0.113.%1").arg(i);
            runs.add(r, 4000);
        }
        CHECK_EQ(runs.size(), Exchanges::MAX_RUNS);
        CHECK(!runs.hasRoom(QStringLiteral("198.51.100.10"), 4001));
        CHECK(runs.hasRoom(QStringLiteral("198.51.100.9"), 4001));
        r.bucket = QStringLiteral("198.51.100.9");
        CHECK(!runs.add(r, 4001).isEmpty());
        CHECK_EQ(runs.size(), Exchanges::MAX_RUNS);
        CHECK(!runs.take(own.first(), r.bucket, 4002).has_value());
        CHECK(runs.take(own.last(), r.bucket, 4002).has_value());
        // Expired, every run makes room.
        CHECK(runs.hasRoom(QStringLiteral("198.51.100.10"), 4001 + Exchanges::RUN_TTL_SECS));
    }

    SECTION("MW-PIN-v1 — constant-time comparison");

    CHECK(sameBytes(QByteArray("abc"), QByteArray("abc")));
    CHECK(!sameBytes(QByteArray("abc"), QByteArray("abd")));
    CHECK(!sameBytes(QByteArray("abc"), QByteArray("abcd")));
    CHECK(!sameBytes(QByteArray(), QByteArray("a")));
    // Inputs of different lengths, the longer one padded with NULs.
    CHECK(!sameBytes(QByteArray(), QByteArray(256, '\0')));
    CHECK(!sameBytes(QByteArray("abc"), QByteArray("abc") + QByteArray(256, '\0')));

    SECTION("MW-TLS-v1 — a few proofs per caller and per minute");

    {
        CallBudget budget(3, 60);
        const QString caller = QStringLiteral("198.51.100.7");
        CHECK(budget.spend(caller, 1000));
        CHECK(budget.spend(caller, 1010));
        CHECK(budget.spend(caller, 1020));
        CHECK(!budget.spend(caller, 1030));                       // a fourth in the same minute
        CHECK(budget.spend(QStringLiteral("203.0.113.9"), 1030)); // another caller's own
        CHECK(budget.spend(caller, 1060));                        // the next minute
        // A flood of callers fills the table, then nobody new until the window has
        // passed; the next one starts from an empty table.
        for (int i = 0; i < CallBudget::MAX_CALLERS; ++i)
            budget.spend(QStringLiteral("2001:db8:%1::/64").arg(i, 0, 16), 1070);
        CHECK_EQ(budget.size(), CallBudget::MAX_CALLERS);
        CHECK(!budget.spend(QStringLiteral("198.51.100.8"), 1071));
        CHECK(budget.spend(QStringLiteral("198.51.100.8"), 1200));
        CHECK_EQ(budget.size(), 1);
    }

    SECTION("MW-TLS-v1 — framed as the TV frames it, under a label of its own");

    {
        const QByteArray framed = PairingCrypto::tlsProofInput(
            QStringLiteral("H"), QByteArray(16, '\x01'), QByteArray(32, '\x02'));
        CHECK_EQ(hexOf(framed), QStringLiteral("0000000e4d572d544c532d76317c686f7374") +
                                    QStringLiteral("0000000148") + QStringLiteral("00000010") +
                                    hexOf(QByteArray(16, '\x01')) + QStringLiteral("00000020") +
                                    hexOf(QByteArray(32, '\x02')));
        // Never the bytes of an MW-BIND host signature, whatever the fields.
        CHECK(framed != PairingCrypto::hostDigestInput(QStringLiteral("H"), QString(),
                                                       QByteArray(16, '\x01'), QString()));
    }

    SECTION("MW-TLS-v1 — a proof holds for this key, this nonce, this host only");

    {
        const QByteArray pem = PairingCrypto::generatePrivateKeyPem();
        const QByteArray hostKey = PairingCrypto::publicKeySpkiFromPrivatePem(pem);
        const QString hostId = PairingCrypto::keyId(hostKey);
        const QByteArray served(32, '\x02');
        const QByteArray nonce = PairingCrypto::generateNonce();
        const QByteArray sig =
            PairingCrypto::sign(pem, PairingCrypto::tlsProofInput(hostId, nonce, served));
        CHECK(PairingCrypto::verify(hostKey, PairingCrypto::tlsProofInput(hostId, nonce, served),
                                    sig));
        // The key of the machine in the middle, which the client is shown.
        CHECK(!PairingCrypto::verify(
            hostKey, PairingCrypto::tlsProofInput(hostId, nonce, QByteArray(32, '\x05')), sig));
        // Replayed to another client, which sent another nonce.
        CHECK(!PairingCrypto::verify(
            hostKey, PairingCrypto::tlsProofInput(hostId, PairingCrypto::generateNonce(), served),
            sig));
        // Another installation's.
        CHECK(!PairingCrypto::verify(
            hostKey, PairingCrypto::tlsProofInput(QStringLiteral("other"), nonce, served), sig));
    }

    SECTION("MW-TLS-v1 — the key a certificate carries, as a client reads it");

    {
        const auto [certDer, spki] = selfSignedCertificate();
        CHECK(!certDer.isEmpty() && !spki.isEmpty());
        CHECK_EQ(PairingCrypto::certificateSpki(certDer), spki);
        ERR_clear_error();
        CHECK(PairingCrypto::certificateSpki(QByteArray("not a certificate")).isEmpty());
        CHECK_EQ(ERR_peek_error(), 0ul);
        CHECK(PairingCrypto::certificateSpki(QByteArray()).isEmpty());
    }
}
