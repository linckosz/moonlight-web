/*
 * MoonlightWeb — self-hosted web client for Moonlight game streaming.
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

#include "HostUploadCap.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace mw::hostcap {

std::string capName(unsigned long pid)
{
    return "MoonlightWeb host cap " + std::to_string(pid);
}

std::string streamName(unsigned long pid)
{
    return "MoonlightWeb stream " + std::to_string(pid);
}

std::string psQuote(const std::string& s)
{
    std::string out = "'";
    for (const char c : s) {
        out += c;
        if (c == '\'') out += '\'';
    }
    out += '\'';
    return out;
}

std::string onScript(unsigned long pid, const std::string& exePath, unsigned mbps, unsigned dscp)
{
    const std::string cap = psQuote(capName(pid));
    const std::string stream = psQuote(streamName(pid));
    const unsigned long long bits = static_cast<unsigned long long>(mbps) * 1000000ull;
    std::string s;
    s += "$ErrorActionPreference = 'Stop'\n";
    // A worker that died with its cap on: its process id runs no more.
    s += "Get-NetQosPolicy -PolicyStore ActiveStore | Where-Object { $_.Name -match "
         "'^MoonlightWeb (host cap|stream) (\\d+)$' -and -not (Get-Process -Id ([int]$Matches[2]) "
         "-ErrorAction SilentlyContinue) } | ForEach-Object { Remove-NetQosPolicy -Name $_.Name "
         "-PolicyStore ActiveStore -Confirm:$false; \"swept $($_.Name)\" }\n";
    s += "foreach ($n in " + cap + ", " + stream +
         ") { if (Get-NetQosPolicy -PolicyStore "
         "ActiveStore | Where-Object Name -eq $n) { Remove-NetQosPolicy -Name $n -PolicyStore "
         "ActiveStore -Confirm:$false } }\n";
    // The highest precedence: at the default 127, the cap wins over it (S1 bis).
    s += "New-NetQosPolicy -Name " + stream +
         " -PolicyStore ActiveStore -AppPathNameMatchCondition " + psQuote(exePath) +
         " -IPProtocolMatchCondition Both -DSCPAction " + std::to_string(dscp) +
         " -Precedence 255 | Out-Null\n";
    s += "New-NetQosPolicy -Name " + cap +
         " -PolicyStore ActiveStore -IPProtocolMatchCondition Both "
         "-ThrottleRateActionBitsPerSecond " +
         std::to_string(bits) + " | Out-Null\n";
    s += "'set'\n";
    return s;
}

std::string offScript(unsigned long pid)
{
    std::string s;
    s += "$names = " + psQuote(capName(pid)) + ", " + psQuote(streamName(pid)) + "\n";
    s += "foreach ($round in 1, 2) {\n";
    s += "  foreach ($n in $names) { Get-NetQosPolicy -PolicyStore ActiveStore | Where-Object Name "
         "-eq $n | ForEach-Object { Remove-NetQosPolicy -Name $n -PolicyStore ActiveStore "
         "-Confirm:$false } }\n";
    s += "  if ($round -eq 1) { Start-Sleep -Seconds 5 }\n";
    s += "}\n";
    return s;
}

std::string encodedCommand(const std::string& script)
{
    const QString text = QString::fromUtf8(script.data(), static_cast<qsizetype>(script.size()));
    QByteArray utf16le;
    utf16le.reserve(text.size() * 2);
    for (const QChar c : text) {
        const char16_t u = c.unicode();
        utf16le.append(static_cast<char>(u & 0xff));
        utf16le.append(static_cast<char>(u >> 8));
    }
    return utf16le.toBase64().toStdString();
}

bool hasNetAdmin(const std::string& procSelfStatus)
{
    std::istringstream in(procSelfStatus);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("CapEff:", 0) != 0) continue;
        const std::string hex = line.substr(line.find_first_not_of(" \t", 7));
        if (hex.empty() || hex.size() > 16 ||
            hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            return false;
        return (std::stoull(hex, nullptr, 16) >> 12) & 1u;
    }
    return false;
}

std::vector<std::string> defaultRouteDevices(const std::string& procNetRoute,
                                             const std::string& procNetIpv6Route)
{
    constexpr unsigned long kRtfUp = 0x1;
    std::vector<std::string> out;
    const auto add = [&out](const std::string& dev) {
        if (dev.empty() || dev == "lo") return;
        for (const std::string& d : out)
            if (d == dev) return;
        out.push_back(dev);
    };
    const auto flagsUp = [](const std::string& hex) {
        if (hex.empty() || hex.size() > 8 ||
            hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            return false;
        return (std::stoul(hex, nullptr, 16) & kRtfUp) != 0;
    };
    // Iface Destination Gateway Flags RefCnt Use Metric Mask ...: a header,
    // then one route a line, the addresses in hex.
    {
        std::istringstream in(procNetRoute);
        std::string line;
        std::getline(in, line);
        while (std::getline(in, line)) {
            std::istringstream f(line);
            std::string dev, dest, gateway, flags, refcnt, use, metric, mask;
            if (!(f >> dev >> dest >> gateway >> flags >> refcnt >> use >> metric >> mask))
                continue;
            if (dest == "00000000" && mask == "00000000" && flagsUp(flags)) add(dev);
        }
    }
    // Destination, its prefix length, source, its length, next hop, metric,
    // refcnt, use, flags, device; no header.
    {
        std::istringstream in(procNetIpv6Route);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream f(line);
            std::string dest, destLen, src, srcLen, hop, metric, refcnt, use, flags, dev;
            if (!(f >> dest >> destLen >> src >> srcLen >> hop >> metric >> refcnt >> use >>
                  flags >> dev))
                continue;
            if (dest == std::string(32, '0') && destLen == "00" && flagsUp(flags)) add(dev);
        }
    }
    return out;
}

bool safeDeviceName(const std::string& dev)
{
    if (dev.empty() || dev.size() > 15) return false;
    for (const char c : dev) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-' || c == ':' ||
                        c == '@';
        if (!ok) return false;
    }
    return true;
}

Root rootQdisc(const std::string& tcShow, std::string* kind)
{
    // "qdisc fq_codel 0: root refcnt 2 limit 10240p ...", children after it
    // ("parent 1:"), one a line.
    std::istringstream in(tcShow);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream f(line);
        std::string word, k, handle, where;
        if (!(f >> word >> k >> handle >> where) || word != "qdisc" || where != "root") continue;
        if (kind) *kind = k;
        if (handle == "0:") return Root::Default;
        char ours[16];
        std::snprintf(ours, sizeof ours, "%x:", kTcHandle);
        if (handle == ours) return Root::Ours;
        return Root::Foreign;
    }
    return Root::Foreign;
}

std::vector<unsigned> exemptDscps(const std::array<unsigned int, 3>& dscp)
{
    std::vector<unsigned> out;
    for (const unsigned d : dscp) {
        if (d == 0 || d > 63) continue;
        bool seen = false;
        for (const unsigned o : out)
            seen = seen || o == d;
        if (!seen) out.push_back(d);
    }
    return out;
}

std::string tcOnBatch(const std::string& dev, unsigned mbps, const std::vector<unsigned>& dscps)
{
    char h[16];
    std::snprintf(h, sizeof h, "%x", kTcHandle);
    const std::string root = std::string(h) + ":";
    const std::string at = " dev " + dev + " ";
    // HTB's ids are hex: "default 20" is class :20. The stream's class (:10)
    // goes as fast as the device; the rest (:20) is held at the rate. A
    // quantum of our own keeps the kernel from warning of the one it derives.
    std::string s;
    s += "qdisc replace" + at + "root handle " + root + " htb default 20\n";
    s += "class replace" + at + "parent " + root + " classid " + root +
         "10 htb rate 10gbit quantum 60000\n";
    s += "class replace" + at + "parent " + root + " classid " + root + "20 htb rate " +
         std::to_string(mbps) + "mbit ceil " + std::to_string(mbps) + "mbit quantum 60000\n";
    s += "qdisc replace" + at + "parent " + root + "20 fq_codel\n";
    // The DSCP is the top six bits of IPv4's TOS byte and of IPv6's traffic
    // class.
    unsigned prio = 1;
    for (const unsigned d : dscps) {
        char tos[8];
        std::snprintf(tos, sizeof tos, "0x%02x", (d & 63u) << 2);
        s += "filter add" + at + "parent " + root + " protocol ip prio " + std::to_string(prio++) +
             " u32 match ip dsfield " + tos + " 0xfc flowid " + root + "10\n";
        s += "filter add" + at + "parent " + root + " protocol ipv6 prio " +
             std::to_string(prio++) + " u32 match ip6 priority " + tos + " 0xfc flowid " + root +
             "10\n";
    }
    return s;
}

std::string tcOffBatch(const std::string& dev)
{
    return "qdisc del dev " + dev + " root\n";
}

#ifdef _WIN32
namespace {

/// The worker's token may set QoS policies: an administrator's, elevated, or
/// SYSTEM's.
bool elevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD size = 0;
    bool yes = GetTokenInformation(token, TokenElevation, &e, sizeof e, &size) && e.TokenIsElevated;
    if (!yes) {
        // SYSTEM, whatever its token says of elevation.
        alignas(TOKEN_USER) unsigned char buf[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE] = {};
        BYTE system[SECURITY_MAX_SID_SIZE] = {};
        DWORD systemSize = sizeof system;
        if (GetTokenInformation(token, TokenUser, buf, sizeof buf, &size) &&
            CreateWellKnownSid(WinLocalSystemSid, nullptr, system, &systemSize))
            yes = EqualSid(reinterpret_cast<TOKEN_USER*>(buf)->User.Sid, system) != FALSE;
    }
    CloseHandle(token);
    return yes;
}

/// Windows PowerShell by its full path: a SYSTEM process does not look it up
/// on the PATH.
QString powershell()
{
    wchar_t dir[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return QString::fromWCharArray(dir, static_cast<int>(n)) +
           QStringLiteral("\\WindowsPowerShell\\v1.0\\powershell.exe");
}

QStringList argumentsFor(const std::string& script)
{
    return {QStringLiteral("-NoProfile"),       QStringLiteral("-NonInteractive"),
            QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
            QStringLiteral("-EncodedCommand"),  QString::fromStdString(encodedCommand(script))};
}

} // namespace
#elif defined(__linux__)
namespace {

std::string readText(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll().toStdString();
}

/// tc by its full path: the service's PATH may not hold /usr/sbin.
QString tcProgram()
{
    for (const char* p : {"/usr/sbin/tc", "/sbin/tc", "/usr/bin/tc", "/bin/tc"})
        if (QFileInfo(QString::fromLatin1(p)).isExecutable()) return QString::fromLatin1(p);
    return {};
}

/// tc with @p args, @p input on its standard input; true when it exits 0.
bool runTc(const QString& tc, const QStringList& args, const std::string& input = {},
           QString* out = nullptr)
{
    QProcess p;
    p.start(tc, args);
    if (!p.waitForStarted(3000)) return false;
    if (!input.empty()) p.write(input.data(), static_cast<qint64>(input.size()));
    p.closeWriteChannel();
    const bool done = p.waitForFinished(5000);
    if (!done) p.kill();
    const QString err = QString::fromLocal8Bit(p.readAllStandardError()).trimmed();
    if (out) *out = QString::fromLocal8Bit(p.readAllStandardOutput());
    const bool ok = done && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
    if (!ok && !err.isEmpty())
        qWarning().noquote() << "[HostCap] tc:" << err.left(400).simplified();
    return ok;
}

Root rootOf(const QString& tc, const std::string& dev, std::string* kind)
{
    QString show;
    if (!runTc(tc,
               {QStringLiteral("qdisc"), QStringLiteral("show"), QStringLiteral("dev"),
                QString::fromStdString(dev)},
               {}, &show))
        return Root::Foreign;
    return rootQdisc(show.toStdString(), kind);
}

const QStringList kBatch = {QStringLiteral("-batch"), QStringLiteral("-")};

} // namespace
#endif

void Cap::start(unsigned mbps, const std::array<unsigned int, 3>& dscp)
{
    if (mbps == 0) return;
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Started) return;
#ifdef _WIN32
    if (!elevated()) {
        qWarning().noquote() << QStringLiteral(
                                    "[HostCap] hostcap=%1 asked for, but this worker has no "
                                    "administrator rights (the launcher service's SYSTEM worker "
                                    "has them): nothing capped")
                                    .arg(mbps);
        return;
    }
    const QString program = powershell();
    if (program.isEmpty()) return;
    const unsigned long pid = GetCurrentProcessId();
    const std::string exe =
        QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toStdString();
    const QStringList args = argumentsFor(onScript(pid, exe, mbps, dscp[1]));
    m_Started = true;
    qInfo().noquote() << QStringLiteral("[HostCap] capping every other upload of this PC at %1 "
                                        "Mbit/s during the stream (hostcap=; ActiveStore policies "
                                        "\"%2\", \"%3\" for %4, DSCP %5)")
                             .arg(mbps)
                             .arg(QString::fromStdString(capName(pid)),
                                  QString::fromStdString(streamName(pid)),
                                  QString::fromStdString(exe))
                             .arg(dscp[1]);
    // Off the caller's thread: PowerShell and its NetQos module take a second
    // or two to start. The thread holds nothing of this object.
    std::thread([program, args]() {
        QProcess p;
        p.start(program, args);
        const bool done = p.waitForFinished(30000);
        const QString out = QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed();
        const QString err = QString::fromLocal8Bit(p.readAllStandardError()).trimmed();
        if (done && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0 &&
            out.endsWith(QLatin1String("set"))) {
            qInfo().noquote() << "[HostCap] cap set:" << out.simplified();
        } else {
            qWarning().noquote() << "[HostCap] cap NOT set (exit" << p.exitCode()
                                 << (done ? "" : ", timed out")
                                 << "):" << err.left(400).simplified()
                                 << out.left(200).simplified();
        }
    }).detach();
#elif defined(__linux__)
    if (!hasNetAdmin(readText(QStringLiteral("/proc/self/status")))) {
        qWarning().noquote() << QStringLiteral(
                                    "[HostCap] hostcap=%1 asked for, but this worker may not "
                                    "change the network's queues (CAP_NET_ADMIN: the headless "
                                    "service, as root, has it; the desktop install does not): "
                                    "nothing capped")
                                    .arg(mbps);
        return;
    }
    const std::vector<unsigned> dscps = exemptDscps(dscp);
    if (dscps.empty()) {
        qWarning().noquote() << QStringLiteral("[HostCap] hostcap=%1 asked for, but the stream "
                                               "leaves unmarked (packet_marking: false), so it "
                                               "cannot be exempt: nothing capped")
                                    .arg(mbps);
        return;
    }
    const QString tc = tcProgram();
    if (tc.isEmpty()) {
        qWarning().noquote() << QStringLiteral("[HostCap] hostcap=%1 asked for, but tc "
                                               "(iproute2) is not installed: nothing capped")
                                    .arg(mbps);
        return;
    }
    const std::vector<std::string> devices =
        defaultRouteDevices(readText(QStringLiteral("/proc/net/route")),
                            readText(QStringLiteral("/proc/net/ipv6_route")));
    if (devices.empty()) qWarning().noquote() << "[HostCap] no default route: nothing capped";
    QStringList marks;
    for (const unsigned d : dscps)
        marks << QString::number(d);
    for (const std::string& dev : devices) {
        const QString name = QString::fromStdString(dev);
        if (!safeDeviceName(dev)) {
            qWarning().noquote() << "[HostCap] a device name tc is not given, hex:"
                                 << QByteArray::fromStdString(dev).toHex();
            continue;
        }
        std::string kind;
        const Root root = rootOf(tc, dev, &kind);
        if (root == Root::Foreign) {
            qWarning().noquote() << QStringLiteral("[HostCap] %1 has a root qdisc of its own "
                                                   "(%2), left as it is: not capped")
                                        .arg(name, kind.empty() ? QStringLiteral("unread")
                                                                : QString::fromStdString(kind));
            continue;
        }
        // A cap of ours already there: a dead worker's, or a sibling stream's.
        const std::string batch =
            (root == Root::Ours ? tcOffBatch(dev) : std::string()) + tcOnBatch(dev, mbps, dscps);
        if (runTc(tc, kBatch, batch)) {
            m_Devices.push_back(dev);
            qInfo().noquote() << QStringLiteral("[HostCap] capping every other upload of %1 at "
                                                "%2 Mbit/s during the stream (hostcap=; tc htb "
                                                "%3:, DSCP %4 exempt%5)")
                                     .arg(name)
                                     .arg(mbps)
                                     .arg(kTcHandle, 0, 16)
                                     .arg(marks.join(QLatin1Char(',')),
                                          root == Root::Ours
                                              ? QStringLiteral("; replaced a cap left by another "
                                                               "worker")
                                              : QString());
        } else {
            // Half set, it could cap the stream itself: the default back.
            runTc(tc, kBatch, tcOffBatch(dev));
            qWarning().noquote() << "[HostCap] cap NOT set on" << name
                                 << "(tc failed): its root qdisc taken off again";
        }
    }
    m_Started = !m_Devices.empty();
#else
    (void)dscp;
    qWarning().noquote() << QStringLiteral("[HostCap] hostcap=%1 asked for: Windows and Linux "
                                           "only, nothing capped")
                                .arg(mbps);
#endif
}

void Cap::stop()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Started) return;
    m_Started = false;
#ifdef _WIN32
    // Detached: it finishes after this worker has gone.
    const unsigned long pid = GetCurrentProcessId();
    if (QProcess::startDetached(powershell(), argumentsFor(offScript(pid))))
        qInfo().noquote() << "[HostCap] removing the cap (a detached PowerShell, twice 5 s apart)";
    else
        qWarning().noquote() << QStringLiteral("[HostCap] could not start the removal: the "
                                               "policies \"%1\" and \"%2\" stay until the next "
                                               "worker sweeps them, or a reboot")
                                    .arg(QString::fromStdString(capName(pid)),
                                         QString::fromStdString(streamName(pid)));
#elif defined(__linux__)
    const QString tc = tcProgram();
    for (const std::string& dev : m_Devices) {
        // Only ours: a sibling stream may have replaced it, never someone else.
        if (tc.isEmpty() || rootOf(tc, dev, nullptr) != Root::Ours) continue;
        if (runTc(tc, kBatch, tcOffBatch(dev)))
            qInfo().noquote() << "[HostCap] cap removed from" << QString::fromStdString(dev);
        else
            qWarning().noquote() << QStringLiteral("[HostCap] could not remove the cap from %1: "
                                                   "it stays until the next worker replaces it, "
                                                   "or a reboot")
                                        .arg(QString::fromStdString(dev));
    }
    m_Devices.clear();
#endif
}

} // namespace mw::hostcap
