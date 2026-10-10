# DSCP and Wi-Fi WMM — the bench's probes

Plan « le son et la priorité des paquets (DSCP/WMM) », piste 2, phase D0:
prove what a packet's DSCP becomes on the wire and on the air before any
product code marks anything. Findings go to
`docs/design/network-latency-findings.md`.

## Tools

| File | Where | What |
|---|---|---|
| `win_dscp_probe.py` | Windows sender | tries each way a process can mark (plain, `IP_TOS`, `WSASendMsg` with an `IP_TOS` control message per packet, qWAVE traffic type, qWAVE exact value), each with its own payload size |
| `tosparse.py` | Linux receiver | DSCP per payload size in a `tcpdump -n -v -l` text capture |
| `pcapng_tos.py` | any | the same from a pcapng (`pktmon etl2pcap` on a Windows receiver, Wi-Fi frames included) |
| `udptos.py` | Linux sender | marked UDP, one class at a time (DF, CS1, AF11, AF41, CS5, VA, EF, CS6, CS7), each with its own size and time window; prints the windows |
| `mon-on.sh`, `mon-off.sh`, `mon-cap.sh` | Linux, Intel AX210 | monitor mode on channel 44 / 80 MHz (the Freebox's 5 GHz), back to managed, a capture of N seconds |
| `tidstat.py` | Linux | per receiver and transmitter, the 802.11 QoS data frames by TID (bad-FCS frames left out) |
| `tidtime.py` | Linux | the TIDs of the frames to one station from one source, per `udptos.py` window: the access point's DSCP → Wi-Fi queue table |
| `ap-table.sh`, `ap_table.py` | Linux, Wi-Fi + wired | the same table read from the station's own per-TID counters, no monitor mode: one BSSID per run |
| `load.py` | a wired server + a second Wi-Fi station | D1: a reproducible load beside the stream (a video's bursts, a bulk transfer, a call), marked as asked, and what it costs the house (its own throughput and round trips) |

TID → queue: 1-2 BK, 0 and 3 BE, 4-5 VI, 6-7 VO.

### A load beside the stream (D1)

```
python3 load.py serve                                   # a wired machine
python3 load.py run --server <wired-ip> --bind <wifi-ip> --profile video --secs 300 --out v.json
python3 load.py run --server <wired-ip> --profile call --tos EF --secs 300   # a video call
```

The generator runs on a second Wi-Fi station of the stream client's access
point; `--dir down` (the default) fills the access point's queues toward it,
as the stream's own way down does, `up` the air from it. `video` is an
iPhone watching a video: 11 Mbit/s on average, fetched in 4-second segments
over TCP as fast as the link goes, since bursts are what the stream feels
(a paced flow is not). `bulk` is one long TCP transfer, `call` paced UDP both
ways (3 Mbit/s each way, 30 frames a second). `--tos` marks every socket on
both sides (DF, AF41, EF, or 0-63); the server marks what it sends back as
asked. Every run pings the server 50 times a second in the same class, from
a process of its own so the load's loops never stretch its round trips: that
and the segments' Mbit/s are the harm done to the house.

For a mark to count, the station and the stream's client must hang off an
access point that reads it. On the bench (06/10) the switch hangs off
ec:68:b4, which puts everything in BE: a Wi-Fi client on that same access
point sees no effect of any mark. Behind the Wi-Fi 7 link (the router
07:1d:00) the precedence rule holds (EF and AF41 in VI, CS6 in VO).

### The access point's table, from the station's counters

```
MW_WIFI_PSK=<key> sudo --preserve-env=MW_WIFI_PSK ./ap-table.sh <bssid>
```

The machine's Wi-Fi joins one access point (BSSID pinned, no IP address), and
`ap_table.py` sends raw marked frames from its own wired port to its own
Wi-Fi MAC. mac80211 counts what the station receives per TID
(`iw dev <wifi> station dump -v`): each class's frames land on the TID the
access point gave them. Frames sent the other way, out of the Wi-Fi, show
this kernel's own table (RFC 8325 since Linux 6.8), a known answer that
checks the counters first. With the access point the wired port hangs off,
nothing crosses the mesh link between repeaters. The key never touches a
disk (in-memory profile, secret flagged "not saved", handed over by a pipe);
the profile is deleted on exit. A Windows station cannot do this: its Wi-Fi
stack hands frames up as plain Data, without the QoS field.

### Windows marking probe

```
# receiver (Linux, wired): what arrives
sudo tcpdump -n -v -l -i enp2s0 udp port 9 and src <windows-ip> > probe.txt
# sender (Windows)
python win_dscp_probe.py <linux-ip> 46 20 9
# then
python3 tosparse.py probe.txt
```

### The access point's table, on the air

```
./mon-on.sh                      # AX210 in monitor mode, channel 44 / 80 MHz
./mon-cap.sh 60 /tmp/mon.pcap &  # capture while the classes go out
python3 udptos.py <wifi-station-ip> 100 5 2 > windows.txt   # from a wired host
python3 tidtime.py /tmp/mon.pcap <station-mac> <wired-host-mac> windows.txt
./mon-off.sh                     # back to managed, handed to NetworkManager
```

The station needs no listener: the access point sends the frames anyway. The
802.11 header (and its QoS field) is not encrypted, only the payload. A far
access point decodes poorly: most frames come with a bad FCS and are left out.

## Readings

### 05/10/2026 — D0, first readings (session moonlight-web-da)

**The Freebox's WMM parameters** (`iw dev wlp3s0 scan` on the UM790Pro): the
box and every repeater (SSID `OctoPowerWifi7`, 5 GHz channel 44 at 80 MHz,
2.4 GHz channel 6) advertise the standard client parameters, no admission
control: VO CW 3-7 AIFSN 2 TXOP 1504 µs; VI CW 7-15 AIFSN 2 TXOP 3008 µs; BE CW
15-1023 AIFSN 3; BK CW 15-1023 AIFSN 7. No 6 GHz BSS seen.

**On the air, passive** (UM790Pro's AX210 in monitor mode, during the Wi-Fi
plan's W4 series): the N95 (`78:8a:86:09:57:5c`) sits on the far repeater
(`3a:07:16:ec:68:b4`, -68 dBm from the UM790Pro). The stream from DualRTX
(Windows native host, DSCP 0) reaches it in TID 0 (best effort): 146 of 146
decodable frames. A household iPhone on the same repeater sends and receives
in TID 6 (voice).

**Windows marks after all** (`win_dscp_probe.py` on DualRTX, Windows 11
26200.9457, not elevated, no QoS policy or registry setting; received on the
UM790Pro through two Freebox repeaters and their Wi-Fi 7 link):

| Method | Asked EF 46 | Asked AF41 34 | Asked CS6 48 |
|---|---|---|---|
| plain | 0 | 0 | 0 |
| `setsockopt(IP_TOS)` | **46** | **34** | refused (WSAEACCES) |
| `WSASendMsg` + `IP_TOS` per packet | **46** | **34** | refused |
| qWAVE `QOSAddSocketToFlow` (Voice) | 56 | 56 | 56 |
| qWAVE `QOSSetFlow(OutgoingDSCPValue)` | refused (error 5) | refused | refused |

- libjuice's Windows `udp_set_diffserv` gives up without trying
  (« IP_TOS has been intentionally broken on Windows… »): on this Windows the
  plain socket option works for a user process, below CS6.
- The DSCP crossed DualRTX → repeater → Wi-Fi 7 link → repeater → UM790Pro
  unchanged.

**Per packet and dual stack** (05/10/2026, later; the same probe, received by
the N95 in Wi-Fi with `pktmon start --capture --comp nics`, then
`pktmon etl2pcap` and `pcapng_tos.py`):
- one socket whose `IP_TOS` changes before every packet (EF, AF41, AF11, 0 in
  turn, as libjuice does): every packet carries its own value;
- an IPv6 dual-stack socket (libjuice's) sending to an IPv4 peer through its
  v4-mapped address: `IPV6_TCLASS` alone is ignored (0), `IP_TOS` alone marks
  (46), both mark (46);
- the marks arrive intact at a Wi-Fi client of the Freebox.

So a Windows build of libjuice that sets `IP_TOS` (and `IPV6_TCLASS`, harmless)
on its socket marks every stream packet as Linux and macOS already do.

### 06/10/2026 — D0.4, the access point's table (session moonlight-web-da)

`ap-table.sh` on the UM790Pro (Ubuntu 24.04, kernel 7.0, AX210), 05:10-05:15,
16 classes × 300 frames, 300 of 300 on one TID for every class. Up (this
kernel): exactly RFC 8325. Down (the access point's choice):

| DSCP | near repeater `3a:07:16:07:1d:00` (-26 dBm, the wired port's own box) | far repeaters `3a:07:16:ec:68:b4`, `3a:07:16:e4:6f:44` (-61/-63 dBm, behind the Wi-Fi 7 link) |
|---|---|---|
| DF, LE | 0 | 0 |
| CS1, AF11 | 0 | 1 (BK) |
| CS2, AF21 | 0 | 2 (BK) |
| CS3, AF31 | 0 | 3 |
| CS4, AF41, AF42 | 0 | 4 (VI) |
| CS5, VA, EF | 0 | 5 (VI) |
| CS6 | 0 | 6 (VO) |
| CS7 | 0 | 7 (VO) |

Same box: the DSCP is ignored. Behind the mesh link: precedence (TID =
DSCP >> 3), so EF rides VI with the video and libdatachannel's AF11 (all
SCTP) rides BK. The Wi-Fi was left disconnected, managed, with no profile.

Again on 06/10, 11:19, from the bench switch (the UM790Pro moved onto it;
interfaces `enp1s0`, `wlp2s0`): `3a:07:16:ec:68:b4`, the access point nearest
the switch (-21 dBm), put every class in TID 0, so the switch hangs off it;
`3a:07:16:07:1d:00`, the Freebox router behind the Wi-Fi 7 link, gave the
precedence table again; `3a:07:16:e4:6f:40` was too weak to stay associated.
From a wired port to a Wi-Fi client of the same box the DSCP is ignored;
across the mesh link, precedence.
