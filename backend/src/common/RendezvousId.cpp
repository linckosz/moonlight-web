#include "RendezvousId.h"

#include <QRandomGenerator>

namespace {

// Crockford base32, lower case. The four ambiguous letters (i, l, o, u) are
// absent by design — see the header.
constexpr char kAlphabet[] = "0123456789abcdefghjkmnpqrstvwxyz";
constexpr int kAlphabetSize = 32; // sizeof(kAlphabet) - 1

} // namespace

namespace RendezvousId {

QString generate()
{
    // QRandomGenerator::system() is the OS CSPRNG. The global generator is NOT
    // acceptable here: it is seeded once and deterministic given its state, and
    // this value is the only thing standing between an instance and someone
    // else claiming its address first.
    //
    // 26 characters of a 32-symbol alphabet is 130 bits of space carrying 128
    // bits of entropy. Drawing symbol by symbol keeps the distribution uniform
    // without the modulo bias that slicing a 128-bit integer by hand invites.
    QString out;
    out.reserve(kLength);
    for (int i = 0; i < kLength; ++i) {
        const quint32 pick = QRandomGenerator::system()->bounded(kAlphabetSize);
        out.append(QLatin1Char(kAlphabet[pick]));
    }
    return out;
}

QString normalise(const QString& raw)
{
    QString out;
    out.reserve(raw.size());
    for (const QChar& c : raw.trimmed().toLower()) {
        switch (c.unicode()) {
        case u'-':
        case u' ': continue; // grouping only, never part of the value
        case u'i':
        case u'l': out.append(QLatin1Char('1')); break;
        case u'o': out.append(QLatin1Char('0')); break;
        case u'u': out.append(QLatin1Char('v')); break;
        default: out.append(c); break;
        }
    }
    return out;
}

bool isValid(const QString& id)
{
    if (id.size() != kLength) return false;
    for (const QChar& c : id) {
        const char16_t u = c.unicode();
        const bool digit = (u >= u'0' && u <= u'9');
        // The alphabet is a-z minus i, l, o and u.
        const bool letter =
            (u >= u'a' && u <= u'z') && u != u'i' && u != u'l' && u != u'o' && u != u'u';
        if (!digit && !letter) return false;
    }
    return true;
}

} // namespace RendezvousId
