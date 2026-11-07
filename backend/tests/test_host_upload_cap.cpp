/*
 * MoonlightWeb — Backend TNR. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * The bench's cap on the host's other uploads (HostUploadCap.h, hostcap=).
 *
 * Nothing here sets a policy: the scripts are only built and read. What
 * matters is that the stream's exemption is set before the cap and removed
 * after it, that the executable's path cannot end the quoted string it sits
 * in, that leftovers of dead workers are swept, and that PowerShell receives
 * exactly the script built.
 *
 * Linux: the kernel's text read back (the right, the default routes, the root
 * qdisc) and tc's batch built, from text copied off Ubuntu 24.04. Never run
 * against tc here.
 */

#include "../src/streaming/HostUploadCap.h"

#include "mw/native/EncoderTuning.h"

#include "test_framework.h"

#include <QByteArray>
#include <QString>

using namespace mw::hostcap;

namespace {

bool has(const std::string& s, const std::string& part)
{
    return s.find(part) != std::string::npos;
}

std::string decoded(const std::string& base64)
{
    const QByteArray raw = QByteArray::fromBase64(QByteArray::fromStdString(base64));
    QString text;
    for (int i = 0; i + 1 < raw.size(); i += 2) {
        const char16_t u = static_cast<char16_t>(static_cast<unsigned char>(raw[i]) |
                                                 (static_cast<unsigned char>(raw[i + 1]) << 8));
        text.append(QChar(u));
    }
    return text.toStdString();
}

} // namespace

void run_host_upload_cap_tests()
{
    SECTION("HostUploadCap — the policies are named after the worker");
    {
        CHECK_EQ(capName(4242), std::string("MoonlightWeb host cap 4242"));
        CHECK_EQ(streamName(4242), std::string("MoonlightWeb stream 4242"));
    }

    SECTION("HostUploadCap — a quote in the path cannot end the literal");
    {
        CHECK_EQ(psQuote("C:\\a b\\x.exe"), std::string("'C:\\a b\\x.exe'"));
        CHECK_EQ(psQuote("C:\\Bob's\\x.exe"), std::string("'C:\\Bob''s\\x.exe'"));
        CHECK_EQ(psQuote(""), std::string("''"));
    }

    SECTION("HostUploadCap — on: sweep, then the stream's exemption, then the cap");
    {
        const std::string s =
            onScript(4242, "C:\\Program Files\\MoonlightWeb\\MoonlightWeb.exe", 20, 34);
        // Leftovers of workers no longer running.
        CHECK(has(s, "'^MoonlightWeb (host cap|stream) (\\d+)$'"));
        CHECK(has(s, "Get-Process -Id ([int]$Matches[2])"));
        // The exemption: this executable, its own DSCP, no cap, and the highest
        // precedence, or the cap wins over it.
        const size_t stream = s.find("New-NetQosPolicy -Name 'MoonlightWeb stream 4242'");
        CHECK(stream != std::string::npos);
        CHECK(has(
            s, "-AppPathNameMatchCondition 'C:\\Program Files\\MoonlightWeb\\MoonlightWeb.exe'"));
        CHECK(has(s, "-DSCPAction 34 "));
        const size_t streamEnd = s.find('\n', stream);
        const std::string streamLine = s.substr(stream, streamEnd - stream);
        CHECK(has(streamLine, " -Precedence 255 "));
        CHECK(!has(streamLine, "-ThrottleRateActionBitsPerSecond"));
        // The cap: everything, 20 Mbit/s in bits per second, ActiveStore only.
        const size_t cap = s.find("New-NetQosPolicy -Name 'MoonlightWeb host cap 4242'");
        CHECK(cap != std::string::npos);
        CHECK(stream < cap);
        CHECK(!has(s.substr(cap, s.find('\n', cap) - cap), "-Precedence"));
        CHECK(has(s, "-ThrottleRateActionBitsPerSecond 20000000 "));
        CHECK(!has(s, "PolicyStore localhost"));
        size_t at = 0;
        int newPolicies = 0;
        while ((at = s.find("New-NetQosPolicy", at)) != std::string::npos) {
            ++newPolicies;
            CHECK(s.compare(s.find("-PolicyStore", at), 25, "-PolicyStore ActiveStore ") == 0);
            at += 16;
        }
        CHECK_EQ(newPolicies, 2);
        // The line the worker reads as success comes last.
        CHECK(s.size() > 6 && s.compare(s.size() - 6, 6, "'set'\n") == 0);
        // 10 Gbit/s, the most the key takes, still fits.
        CHECK(has(onScript(1, "x.exe", kMaxMbps, 0),
                  "-ThrottleRateActionBitsPerSecond 10000000000 "));
    }

    SECTION("HostUploadCap — off: the cap first, twice, 5 s apart");
    {
        const std::string s = offScript(4242);
        CHECK(has(s, "$names = 'MoonlightWeb host cap 4242', 'MoonlightWeb stream 4242'"));
        CHECK(has(s, "foreach ($round in 1, 2)"));
        CHECK(has(s, "Start-Sleep -Seconds 5"));
        CHECK(has(s, "Remove-NetQosPolicy -Name $n -PolicyStore ActiveStore -Confirm:$false"));
        CHECK(!has(s, "New-NetQosPolicy"));
    }

    SECTION("HostUploadCap — PowerShell gets the script back, accents and all");
    {
        const std::string script =
            onScript(7, "D:\\Jeux d'été\\MoonlightWeb.exe", 50, 34) + "# ü €\n";
        CHECK_EQ(decoded(encodedCommand(script)), script);
        // UTF-16LE: 'A' is 41 00.
        CHECK_EQ(encodedCommand("A"), std::string("QQA="));
    }

    SECTION("HostUploadCap — hostcap= is a key of its own in the tuning");
    {
        mw::native::EncoderTuning t;
        CHECK(t.isDefault());
        t.hostCapMbps = 20;
        CHECK(!t.isDefault());
        CHECK(has(t.describe(), "hostcap=20"));
    }

    SECTION("HostUploadCap — Linux: CAP_NET_ADMIN is bit 12 of CapEff");
    {
        // root's, and the desktop launcher's (CAP_SYS_ADMIN, bit 21, alone).
        CHECK(hasNetAdmin("Name:\tMoonlightWeb\nCapInh:\t0000000000000000\n"
                          "CapPrm:\t000001ffffffffff\nCapEff:\t000001ffffffffff\n"));
        CHECK(!hasNetAdmin("CapPrm:\t0000000000201000\nCapEff:\t0000000000200000\n"));
        CHECK(hasNetAdmin("CapEff:\t0000000000001000\n"));
        // CapPrm is not CapEff; unreadable is no.
        CHECK(!hasNetAdmin("CapPrm:\t0000000000001000\nCapEff:\t0000000000000000\n"));
        CHECK(!hasNetAdmin(""));
        CHECK(!hasNetAdmin("CapEff:\tzz\n"));
    }

    SECTION("HostUploadCap — Linux: the devices of the default routes, each once");
    {
        const std::string v4 =
            "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
            "enp3s0\t00000000\t0101A8C0\t0003\t0\t0\t100\t00000000\t0\t0\t0\n"
            "enp3s0\t0001A8C0\t00000000\t0001\t0\t0\t100\t00FFFFFF\t0\t0\t0\n"
            "wlp2s0\t00000000\t0101A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n"
            // A default route that is down.
            "tun0\t00000000\t00000000\t0000\t0\t0\t50\t00000000\t0\t0\t0\n";
        const std::string v6 =
            "fe800000000000000000000000000000 40 00000000000000000000000000000000 00 "
            "00000000000000000000000000000000 00000100 00000001 00000000 00000001 enp3s0\n"
            "00000000000000000000000000000000 00 00000000000000000000000000000000 00 "
            "fe80000000000000021122fffe334455 00000064 00000003 00000000 00450003 enp3s0\n"
            "00000000000000000000000000000000 00 00000000000000000000000000000000 00 "
            "fe80000000000000021122fffe334455 00000400 00000001 00000000 00450003 eno1\n"
            // The kernel's own unreachable default, on lo.
            "00000000000000000000000000000000 00 00000000000000000000000000000000 00 "
            "00000000000000000000000000000000 ffffffff 00000001 00000000 00200200 lo\n";
        const std::vector<std::string> d = defaultRouteDevices(v4, v6);
        CHECK_EQ(d.size(), size_t(3));
        if (d.size() == 3) {
            CHECK_EQ(d[0], std::string("enp3s0"));
            CHECK_EQ(d[1], std::string("wlp2s0"));
            CHECK_EQ(d[2], std::string("eno1"));
        }
        CHECK(defaultRouteDevices("", "").empty());
        CHECK(defaultRouteDevices("Iface\tDestination\n", "garbage\n").empty());
    }

    SECTION("HostUploadCap — Linux: a device name cannot carry a second tc line");
    {
        CHECK(safeDeviceName("enp3s0"));
        CHECK(safeDeviceName("wlp2s0"));
        CHECK(safeDeviceName("br-1a2b3c4d5e6f"));
        CHECK(safeDeviceName("eth0.100"));
        CHECK(safeDeviceName("veth1@if2"));
        CHECK(!safeDeviceName(""));
        CHECK(!safeDeviceName("a234567890123456")); // 16: over IFNAMSIZ
        CHECK(!safeDeviceName("eth0 root"));
        CHECK(!safeDeviceName("eth0\nqdisc del dev lo root"));
        CHECK(!safeDeviceName("eth0;x"));
    }

    SECTION("HostUploadCap — Linux: only the kernel's root, or ours, is taken");
    {
        std::string kind;
        CHECK(rootQdisc("qdisc fq_codel 0: root refcnt 2 limit 10240p flows 1024 quantum 1514 "
                        "target 5ms interval 100ms memory_limit 32Mb ecn drop_batch 64 \n",
                        &kind) == Root::Default);
        CHECK_EQ(kind, std::string("fq_codel"));
        // A multi-queue card: mq at the root, its children after.
        CHECK(rootQdisc("qdisc mq 0: root \nqdisc fq_codel 0: parent :4 limit 10240p\n"
                        "qdisc fq_codel 0: parent :3 limit 10240p\n",
                        &kind) == Root::Default);
        CHECK_EQ(kind, std::string("mq"));
        CHECK(rootQdisc("qdisc noqueue 0: root refcnt 2 \n") == Root::Default);
        CHECK(rootQdisc("qdisc htb 4d57: root refcnt 2 r2q 10 default 0x20 direct_packets_stat 0 "
                        "direct_qlen 1000\nqdisc fq_codel 8001: parent 4d57:20 limit 10240p\n") ==
              Root::Ours);
        // Someone's own: a shaper, CAKE, an htb of another handle.
        CHECK(rootQdisc("qdisc cake 8001: root refcnt 2 bandwidth 40Mbit diffserv4\n", &kind) ==
              Root::Foreign);
        CHECK_EQ(kind, std::string("cake"));
        CHECK(rootQdisc("qdisc htb 1: root refcnt 2 r2q 10 default 0x30\n") == Root::Foreign);
        // A child line alone, nothing, or an error: not ours to touch.
        CHECK(rootQdisc("qdisc fq_codel 0: parent :1 limit 10240p\n") == Root::Foreign);
        CHECK(rootQdisc("") == Root::Foreign);
        CHECK(rootQdisc("Cannot find device \"eth9\"\n") == Root::Foreign);
    }

    SECTION("HostUploadCap — Linux: the stream's marks, each once, never 0");
    {
        const std::vector<unsigned> d = exemptDscps({46, 34, 34});
        CHECK_EQ(d.size(), size_t(2));
        if (d.size() == 2) {
            CHECK_EQ(d[0], 46u);
            CHECK_EQ(d[1], 34u);
        }
        CHECK(exemptDscps({0, 0, 0}).empty()); // packet_marking: false
        CHECK_EQ(exemptDscps({0, 34, 0}).size(), size_t(1));
        CHECK(exemptDscps({64, 99, 0}).empty());
    }

    SECTION("HostUploadCap — Linux: the root, the two classes, then the filters");
    {
        const std::string s = tcOnBatch("enp3s0", 20, {46, 34});
        const size_t root = s.find("qdisc replace dev enp3s0 root handle 4d57: htb default 20\n");
        CHECK_EQ(root, size_t(0));
        const size_t stream =
            s.find("class replace dev enp3s0 parent 4d57: classid 4d57:10 htb rate 10gbit "
                   "quantum 60000\n");
        const size_t cap =
            s.find("class replace dev enp3s0 parent 4d57: classid 4d57:20 htb rate 20mbit "
                   "ceil 20mbit quantum 60000\n");
        CHECK(stream != std::string::npos);
        CHECK(cap != std::string::npos);
        CHECK(has(s, "qdisc replace dev enp3s0 parent 4d57:20 fq_codel\n"));
        // EF is 0xb8 in the TOS byte, AF41 0x88; IPv4 and IPv6 for each.
        const size_t ef = s.find("filter add dev enp3s0 parent 4d57: protocol ip prio 1 u32 match "
                                 "ip dsfield 0xb8 0xfc flowid 4d57:10\n");
        CHECK(ef != std::string::npos);
        CHECK(has(s, "filter add dev enp3s0 parent 4d57: protocol ipv6 prio 2 u32 match ip6 "
                     "priority 0xb8 0xfc flowid 4d57:10\n"));
        CHECK(has(s, "filter add dev enp3s0 parent 4d57: protocol ip prio 3 u32 match ip dsfield "
                     "0x88 0xfc flowid 4d57:10\n"));
        CHECK(has(s, "filter add dev enp3s0 parent 4d57: protocol ipv6 prio 4 u32 match ip6 "
                     "priority 0x88 0xfc flowid 4d57:10\n"));
        CHECK(stream < ef && cap < ef);
        // Nothing sends to the capped class but the default; nothing deletes.
        CHECK(!has(s, "flowid 4d57:20"));
        CHECK(!has(s, " del "));
        // Each line names the device.
        size_t lines = 0, named = 0, at = 0;
        while (at < s.size()) {
            const size_t end = s.find('\n', at);
            const std::string line = s.substr(at, end - at);
            ++lines;
            if (has(line, " dev enp3s0 ")) ++named;
            at = end + 1;
        }
        CHECK_EQ(lines, size_t(8));
        CHECK_EQ(named, lines);
        // The most the key takes.
        CHECK(has(tcOnBatch("eth0", kMaxMbps, {34}), "rate 10000mbit ceil 10000mbit "));
    }

    SECTION("HostUploadCap — Linux: off takes the root away, the default comes back");
    {
        CHECK_EQ(tcOffBatch("enp3s0"), std::string("qdisc del dev enp3s0 root\n"));
    }

    SECTION("HostUploadCap — start(0) and stop() without a start do nothing");
    {
        Cap c;
        c.start(0, {46, 34, 34});
        c.stop();
        c.stop();
        CHECK(true);
    }
}
