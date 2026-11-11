/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * Passive ICE-TCP, our patch on libjuice (third_party/patches/
 * libjuice-ice-tcp-passive.patch, docs/design/ice-tcp-passive.md): two agents
 * connect over TCP alone, the listener's gate closes everything that is not an
 * authenticated Binding request, and the listener is gone once the checks are
 * over. Plain C against libjuice itself, so a runner of its own. Everything
 * binds 127.0.0.1 (no firewall prompt), but one wildcard listener off Windows.
 */
#include "juice/juice.h"

#include "socket.h"
#include "stun.h"
#include "thread.h"
#include "udp.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static void sleep_ms(int ms) { Sleep(ms); }
#else
#include <sys/time.h>
#include <unistd.h>
static void sleep_ms(int ms) { usleep(ms * 1000); }
#endif

static int failures = 0;
#define CHECK(cond)                                                                                \
	do {                                                                                           \
		if (cond) {                                                                                \
			printf("  ok   %s\n", #cond);                                                          \
		} else {                                                                                   \
			printf("  FAIL %s (line %d)\n", #cond, __LINE__);                                      \
			++failures;                                                                            \
		}                                                                                          \
	} while (0)

// ── Raw TCP client ──────────────────────────────────────────────────────────

// From 127.0.0.<from> (another source address on the same loopback; 1 by default).
static socket_t dial_from(int from, int port) {
	socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	if (from != 1) {
		sin.sin_addr.s_addr = htonl(0x7F000000 | (uint32_t)from);
		if (bind(sock, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
			closesocket(sock);
			return INVALID_SOCKET;
		}
	}
	sin.sin_port = htons((uint16_t)port);
	sin.sin_addr.s_addr = htonl(0x7F000001);
	if (connect(sock, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
		closesocket(sock);
		return INVALID_SOCKET;
	}
	return sock;
}

static socket_t dial(int port) { return dial_from(1, port); }

static void set_timeout(socket_t sock, int ms) {
#ifdef _WIN32
	DWORD tv = (DWORD)ms;
#else
	struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
#endif
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
}

// True when the other end closed (or reset) within `ms`; false when it stayed silent.
static bool closed_within(socket_t sock, int ms) {
	set_timeout(sock, ms);
	char c;
	int len = recv(sock, &c, 1, 0);
	if (len == 0)
		return true;
	if (len > 0)
		return false; // it talked: not closed
#ifdef _WIN32
	return WSAGetLastError() != WSAETIMEDOUT;
#else
	return errno != EAGAIN && errno != EWOULDBLOCK;
#endif
}

static void send_frame(socket_t sock, const char *data, uint16_t announced, size_t len) {
	uint16_t header = htons(announced);
	send(sock, (const char *)&header, 2, 0);
	if (len)
		send(sock, data, (int)len, 0);
}

static int binding_request(char *buf, size_t size, const char *username, const char *password) {
	stun_message_t msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_class = STUN_CLASS_REQUEST;
	msg.msg_method = STUN_METHOD_BINDING;
	for (int i = 0; i < STUN_TRANSACTION_ID_SIZE; ++i)
		msg.transaction_id[i] = (uint8_t)rand();
	snprintf(msg.credentials.username, STUN_MAX_USERNAME_LEN, "%s", username);
	msg.priority = 1845501695;
	msg.ice_controlling = 0x0123456789ABCDEFull;
	msg.use_candidate = true; // nominates the pair: the agent connects on it
	return stun_write(buf, size, &msg, password);
}

// One RFC 4571 frame, or -1 when none came within `ms`.
static int read_frame(socket_t sock, char *buf, int size, int ms) {
	set_timeout(sock, ms);
	uint16_t header = 0;
	if (recv(sock, (char *)&header, 2, MSG_WAITALL) != 2)
		return -1;
	int len = ntohs(header);
	if (len > size)
		return -1;
	if (len > 0 && recv(sock, buf, len, MSG_WAITALL) != len)
		return -1;
	return len;
}

// ── Agents ──────────────────────────────────────────────────────────────────

typedef struct side {
	juice_agent_t *agent;
	struct side *peer;   // passive candidates go there, if set
	const char *bind;    // 127.0.0.1 if NULL
	const char *rewrite; // the address the peer gets them with, like the tunnel's public one
	uint16_t port;       // the UDP socket's, and so the listener's; 0 leaves it to the system
	char passive_addr[64];
	atomic(int) passive_port;
	atomic(int) received;
	char data[64];
} side_t;

static void on_candidate(juice_agent_t *agent, const char *sdp, void *user_ptr) {
	(void)agent;
	side_t *side = user_ptr;
	if (!strstr(sdp, "tcptype passive"))
		return;
	char addr[64];
	int port = 0;
	int addr_begin = 0, addr_end = 0;
	if (sscanf(sdp, "a=candidate:%*s %*d %*s %*u %n%63s%n %d", &addr_begin, addr, &addr_end,
	           &port) == 2) {
		snprintf(side->passive_addr, sizeof(side->passive_addr), "%s", addr);
		atomic_store(&side->passive_port, port);
	}
	if (!side->peer)
		return;
	if (side->rewrite && addr_end > addr_begin) {
		char rewritten[JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
		snprintf(rewritten, sizeof(rewritten), "%.*s%s%s", addr_begin, sdp, side->rewrite,
		         sdp + addr_end);
		juice_add_remote_candidate(side->peer->agent, rewritten);
	} else {
		juice_add_remote_candidate(side->peer->agent, sdp);
	}
}

static void on_recv(juice_agent_t *agent, const char *data, size_t size, void *user_ptr) {
	(void)agent;
	side_t *side = user_ptr;
	size_t n = size < sizeof(side->data) - 1 ? size : sizeof(side->data) - 1;
	memcpy(side->data, data, n);
	side->data[n] = '\0';
	atomic_store(&side->received, 1);
}

static void make(side_t *side, juice_ice_tcp_mode_t mode) {
	juice_config_t config;
	memset(&config, 0, sizeof(config));
	config.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
	config.bind_address = side->bind ? side->bind : "127.0.0.1";
	config.local_port_range_begin = side->port;
	config.local_port_range_end = side->port;
	config.cb_candidate = on_candidate;
	config.cb_recv = on_recv;
	config.user_ptr = side;
	side->agent = juice_create(&config);
	juice_set_ice_tcp_mode(side->agent, mode);
}

// A port free here in TCP and in UDP, for a host agent: its listener takes the number of its UDP
// socket, and an ephemeral one may not be free in TCP. Windows reserves blocks of 100 ports or
// more for TCP alone or UDP alone (Hyper-V, WSL: `netsh int ipv4 show excludedportrange`, 10013),
// and hands UDP ports out in sequence, so the agent after a refused one lands in the same block:
// 1 run in 15 failed on one machine, retrying 5 times did not always get out. Each system choice
// here skips what its own protocol reserves; the other one is tried, and the next taken if it
// fails. With `held`, the TCP socket stays open, listening, for the caller.
static uint16_t free_port(socket_t *held) {
	for (int attempt = 0; attempt < 2000; ++attempt) {
		struct sockaddr_in sin;
		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		sin.sin_addr.s_addr = htonl(0x7F000001);
		socklen_t sin_len = sizeof(sin);
		socket_t tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		bool ok = bind(tcp, (struct sockaddr *)&sin, sizeof(sin)) == 0 && listen(tcp, 1) == 0 &&
		          getsockname(tcp, (struct sockaddr *)&sin, &sin_len) == 0;
		socket_t udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		ok = ok && bind(udp, (struct sockaddr *)&sin, sizeof(sin)) == 0;
		closesocket(udp);
		if (ok) {
			if (held)
				*held = tcp;
			else
				closesocket(tcp);
			return ntohs(sin.sin_port);
		}
		closesocket(tcp);
	}
	return 0;
}

static bool wait_state(juice_agent_t *agent, juice_state_t state, int ms) {
	for (int t = 0; t < ms; t += 50) {
		if (juice_get_state(agent) == state)
			return true;
		sleep_ms(50);
	}
	return juice_get_state(agent) == state;
}

static void read_attr(const char *sdp, const char *key, char *out, size_t size) {
	const char *p = strstr(sdp, key);
	out[0] = '\0';
	if (!p)
		return;
	p += strlen(key);
	size_t n = strcspn(p, "\r\n");
	if (n >= size)
		n = size - 1;
	memcpy(out, p, n);
	out[n] = '\0';
}

// ── Tests ───────────────────────────────────────────────────────────────────

// The host (a) and a peer that dials it like a browser (b), gathering. If another program took the
// host's port since free_port() (no listener, no passive candidate, as designed): another pair.
static void connecting_pair(side_t *a, side_t *b, const char *bind, const char *rewrite) {
	for (int attempt = 0; attempt < 5 && !a->passive_port; ++attempt) {
		if (a->agent) {
			juice_destroy(a->agent);
			juice_destroy(b->agent);
		}
		memset(a, 0, sizeof(*a));
		memset(b, 0, sizeof(*b));
		a->bind = bind;
		a->rewrite = rewrite;
		a->port = free_port(NULL);
		make(a, JUICE_ICE_TCP_MODE_ACTIVE_PASSIVE);
		make(b, JUICE_ICE_TCP_MODE_ACTIVE);
		a->peer = b; // only a's passive candidates travel; nothing of b's does

		char sdp[JUICE_MAX_SDP_STRING_LEN];
		juice_get_local_description(a->agent, sdp, sizeof(sdp));
		juice_set_remote_description(b->agent, sdp);
		juice_get_local_description(b->agent, sdp, sizeof(sdp));
		juice_set_remote_description(a->agent, sdp);

		juice_gather_candidates(a->agent);
		juice_gather_candidates(b->agent);
		for (int t = 0; t < 1000 && !a->passive_port; t += 50)
			sleep_ms(50);
	}
}

static void test_connect_over_tcp_only(void) {
	printf("Two agents connect over passive TCP alone\n");
	side_t a = {0}, b = {0};
	connecting_pair(&a, &b, NULL, NULL);

	CHECK(wait_state(a.agent, JUICE_STATE_COMPLETED, 10000));
	CHECK(wait_state(b.agent, JUICE_STATE_COMPLETED, 2000));
	CHECK(a.passive_port > 0);

	char local[JUICE_MAX_CANDIDATE_SDP_STRING_LEN], remote[JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
	if (juice_get_selected_candidates(a.agent, local, sizeof(local), remote, sizeof(remote)) == 0) {
		printf("  host selected: %s -> %s\n", local, remote);
		CHECK(strstr(remote, "TCP") != NULL);
		CHECK(strstr(local, "tcptype passive") != NULL); // what the pair runs on, not the 1st one
	} else {
		CHECK(!"selected candidates");
	}

	CHECK(juice_send(b.agent, "from the browser", 16) == 0);
	CHECK(juice_send(a.agent, "from the host", 13) == 0);
	for (int t = 0; t < 2000 && (!a.received || !b.received); t += 50)
		sleep_ms(50);
	CHECK(a.received && strcmp(a.data, "from the browser") == 0);
	CHECK(b.received && strcmp(b.data, "from the host") == 0);

	// Connected: the listener is gone, its port takes nobody.
	socket_t late = dial(a.passive_port);
	CHECK(late == INVALID_SOCKET);
	if (late != INVALID_SOCKET)
		closesocket(late);

	juice_destroy(a.agent);
	juice_destroy(b.agent);
}

static void test_selected_local_is_the_address_reached(void) {
	printf("The selected pair's local side is the address the peer reached\n");
#ifdef _WIN32
	// A listener on every interface would raise the firewall's prompt; the CI runs it on Linux
	printf("  skipped on Windows\n");
#else
	// The host listens on every interface but offers its first address only, which the peer
	// gets rewritten (here to 127.0.0.1, the tunnel's public address in production). Before,
	// the log named the address offered, not the one that accepted.
	side_t a = {0}, b = {0};
	connecting_pair(&a, &b, "0.0.0.0", "127.0.0.1");

	CHECK(wait_state(a.agent, JUICE_STATE_COMPLETED, 10000));
	printf("  offered %s, reached 127.0.0.1%s\n", a.passive_addr,
	       strcmp(a.passive_addr, "127.0.0.1") == 0 ? " (the same: no other interface)" : "");

	char local[JUICE_MAX_CANDIDATE_SDP_STRING_LEN], remote[JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
	if (juice_get_selected_candidates(a.agent, local, sizeof(local), remote, sizeof(remote)) == 0) {
		printf("  host selected: %s -> %s\n", local, remote);
		CHECK(strstr(local, "tcptype passive") != NULL);
		CHECK(strstr(local, " 127.0.0.1 ") != NULL);
	} else {
		CHECK(!"selected candidates");
	}

	juice_destroy(a.agent);
	juice_destroy(b.agent);
#endif
}

// A host agent whose remote is a raw peer ("peer" and `peer_pwd`), listening. If another program
// took its port since free_port(): another agent.
static void listening_host(side_t *h, const char *peer_pwd) {
	char sdp[JUICE_MAX_SDP_STRING_LEN];
	snprintf(sdp, sizeof(sdp), "a=ice-ufrag:peer\r\na=ice-pwd:%s\r\n", peer_pwd);
	for (int attempt = 0; attempt < 5 && !h->passive_port; ++attempt) {
		if (h->agent)
			juice_destroy(h->agent);
		memset(h, 0, sizeof(*h));
		h->port = free_port(NULL);
		make(h, JUICE_ICE_TCP_MODE_ACTIVE_PASSIVE);
		juice_set_remote_description(h->agent, sdp);
		juice_gather_candidates(h->agent);
		for (int t = 0; t < 1000 && !h->passive_port; t += 50)
			sleep_ms(50);
	}
}

static void host_credentials(side_t *h, char ufrag[64], char pwd[64]) {
	char local_sdp[JUICE_MAX_SDP_STRING_LEN];
	juice_get_local_description(h->agent, local_sdp, sizeof(local_sdp));
	read_attr(local_sdp, "a=ice-ufrag:", ufrag, 64);
	read_attr(local_sdp, "a=ice-pwd:", pwd, 64);
}

// A raw peer, connected to the host's listener, sends its nominating check, then answers the
// host's own checks until the host connects. True if it did within 5 s.
static bool peer_connects(side_t *h, socket_t peer, const char *peer_pwd) {
	char ufrag[64], pwd[64], user[160];
	host_credentials(h, ufrag, pwd);
	snprintf(user, sizeof(user), "%s:peer", ufrag);

	char buf[2048];
	int len = binding_request(buf, sizeof(buf), user, pwd);
	send_frame(peer, buf, (uint16_t)len, (size_t)len);
	for (int t = 0; t < 5000 && juice_get_state(h->agent) != JUICE_STATE_CONNECTED &&
	                juice_get_state(h->agent) != JUICE_STATE_COMPLETED;
	     t += 50) {
		len = read_frame(peer, buf, sizeof(buf), 50);
		stun_message_t in;
		memset(&in, 0, sizeof(in));
		if (len <= 0 || stun_read(buf, (size_t)len, &in) < 0 ||
		    in.msg_class != STUN_CLASS_REQUEST || in.msg_method != STUN_METHOD_BINDING)
			continue;
		stun_message_t resp;
		memset(&resp, 0, sizeof(resp));
		resp.msg_class = STUN_CLASS_RESP_SUCCESS;
		resp.msg_method = STUN_METHOD_BINDING;
		memcpy(resp.transaction_id, in.transaction_id, STUN_TRANSACTION_ID_SIZE);
		resp.mapped.len = sizeof(resp.mapped.addr);
		getpeername(peer, (struct sockaddr *)&resp.mapped.addr, &resp.mapped.len);
		len = stun_write(buf, sizeof(buf), &resp, peer_pwd);
		send_frame(peer, buf, (uint16_t)len, (size_t)len);
	}
	juice_state_t state = juice_get_state(h->agent);
	return state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED;
}

#define BURST_SIZE 1200 // an SCTP packet over libdatachannel
#define BURST_MAX 8000  // 9.6 MB: past what the sockets and the queue hold
#define QUEUE_AT_LEAST (128 * 1024) // half the patch's TCP_SEND_QUEUE_MAX: socket sizes vary

// How many bytes an accepted socket of this machine takes, toward a loopback peer that has a
// small receive buffer and does not read, before it would block: what the host's socket alone
// holds in the test below.
static size_t socket_capacity(void) {
	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(0x7F000001);
	socklen_t sin_len = sizeof(sin);
	socket_t lst = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	bind(lst, (struct sockaddr *)&sin, sizeof(sin));
	listen(lst, 1);
	getsockname(lst, (struct sockaddr *)&sin, &sin_len);

	socket_t cli = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	const int rcvbuf = 4096;
	setsockopt(cli, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));
	connect(cli, (struct sockaddr *)&sin, sizeof(sin));
	socket_t srv = accept(lst, NULL, NULL);
	ctl_t nbio = 1;
	ioctlsocket(srv, FIONBIO, &nbio);

	char frame[2 + BURST_SIZE];
	memset(frame, 'x', sizeof(frame));
	size_t total = 0;
	for (int i = 0; i < BURST_MAX; ++i) {
		int len = send(srv, frame, (int)sizeof(frame), 0);
		if (len <= 0)
			break;
		total += (size_t)len;
		if (len < (int)sizeof(frame))
			break;
	}
	closesocket(srv);
	closesocket(cli);
	closesocket(lst);
	return total;
}

static void test_burst_over_tcp(void) {
	printf("A burst the socket cannot take at once arrives whole and in order\n");
	// Upstream kept one datagram aside when the socket would block and dropped the rest: over a
	// long round trip SCTP lost one each time the socket filled, and a first page load stalled.
	// The peer here is a raw connection with a small receive buffer that stops reading once the
	// host is connected, which is what a far browser looks like from the host's socket.
	const char *peer_pwd = "pw01234567890123456789";
	side_t h = {0};
	listening_host(&h, peer_pwd);
	CHECK(h.passive_port > 0);

	socket_t peer = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	const int rcvbuf = 4096; // before connect, so the window it offers stays small
	setsockopt(peer, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));
	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons((uint16_t)h.passive_port);
	sin.sin_addr.s_addr = htonl(0x7F000001);
	CHECK(connect(peer, (struct sockaddr *)&sin, sizeof(sin)) == 0);
	CHECK(peer_connects(&h, peer, peer_pwd));
	char buf[2048];
	int len;

	// Nothing reads now: the burst goes on until the host refuses one.
	size_t capacity = socket_capacity();
	char datagram[BURST_SIZE];
	memset(datagram, 'x', sizeof(datagram));
	int accepted = 0;
	for (uint32_t seq = 0; seq < BURST_MAX; ++seq) {
		memcpy(datagram, &seq, sizeof(seq));
		if (juice_send(h.agent, datagram, sizeof(datagram)) != 0)
			break;
		++accepted;
	}
	size_t accepted_bytes = (size_t)accepted * (2 + BURST_SIZE);
	printf("  a socket alone holds %zu KB, the host took %zu KB before refusing one\n",
	       capacity / 1024, accepted_bytes / 1024);
	CHECK(accepted < BURST_MAX); // it did fill up
	// Where the socket stopped, the queue took over. Upstream refused the second datagram the
	// socket could not take.
	CHECK(accepted_bytes >= capacity + QUEUE_AT_LEAST);

	// What was accepted arrives, all of it and in order.
	int received = 0, disorder = 0, misses = 0;
	while (received < accepted && misses < 10) {
		len = read_frame(peer, buf, sizeof(buf), 300);
		if (len < 0) {
			++misses;
			continue;
		}
		if (len != BURST_SIZE || is_stun_datagram(buf, (size_t)len))
			continue; // the host's own checks and keepalives
		uint32_t seq;
		memcpy(&seq, buf, sizeof(seq));
		if (seq != (uint32_t)received)
			++disorder;
		++received;
	}
	printf("  accepted %d, received %d, out of order %d\n", accepted, received, disorder);
	CHECK(received == accepted);
	CHECK(disorder == 0);

	closesocket(peer);
	juice_destroy(h.agent);
}

static void test_gate(void) {
	printf("The gate closes what is not an authenticated Binding request\n");
	side_t h = {0};
	const char *peer_pwd = "pw01234567890123456789";
	listening_host(&h, peer_pwd);
	CHECK(h.passive_port > 0);

	char ufrag[64], pwd[64];
	host_credentials(&h, ufrag, pwd);
	char good_user[160], bad_user[160];
	snprintf(good_user, sizeof(good_user), "%s:peer", ufrag);
	snprintf(bad_user, sizeof(bad_user), "zz%s:peer", ufrag);
	char buf[512];

	// Silent: kept a little, then closed by the first-frame deadline (2 s).
	socket_t idle = dial(h.passive_port);
	CHECK(idle != INVALID_SOCKET);
	CHECK(!closed_within(idle, 1000));
	CHECK(closed_within(idle, 2500));
	closesocket(idle);

	// Announced length out of bounds, either way.
	socket_t big = dial(h.passive_port);
	send_frame(big, NULL, 5000, 0);
	CHECK(closed_within(big, 1000));
	closesocket(big);
	socket_t tiny = dial(h.passive_port);
	send_frame(tiny, "0123456789", 10, 10);
	CHECK(closed_within(tiny, 1000));
	closesocket(tiny);

	// Not STUN.
	memset(buf, 'A', 100);
	socket_t junk = dial(h.passive_port);
	send_frame(junk, buf, 100, 100);
	CHECK(closed_within(junk, 1000));
	closesocket(junk);

	// STUN, but the wrong password, then the wrong ufrag.
	int len = binding_request(buf, sizeof(buf), good_user, "not-the-password-at-all");
	socket_t wrong_pwd = dial(h.passive_port);
	send_frame(wrong_pwd, buf, (uint16_t)len, (size_t)len);
	CHECK(closed_within(wrong_pwd, 1000));
	closesocket(wrong_pwd);
	len = binding_request(buf, sizeof(buf), bad_user, pwd);
	socket_t wrong_ufrag = dial(h.passive_port);
	send_frame(wrong_ufrag, buf, (uint16_t)len, (size_t)len);
	CHECK(closed_within(wrong_ufrag, 1000));
	closesocket(wrong_ufrag);

	// One address keeps two places: its third connection takes its first one's.
	socket_t mine[3];
	for (int i = 0; i < 3; ++i) {
		mine[i] = dial(h.passive_port);
		sleep_ms(100);
	}
	CHECK(closed_within(mine[0], 500));
	CHECK(!closed_within(mine[1], 100) && !closed_within(mine[2], 100));
	for (int i = 0; i < 3; ++i)
		closesocket(mine[i]);
	sleep_ms(300);

	// Four places in all, from four addresses (127.0.0.2 to .5)
	socket_t probe = dial_from(2, h.passive_port);
	if (probe == INVALID_SOCKET) {
		printf("  no other loopback address here: the four-address cases are skipped\n");
	} else {
		closesocket(probe);
		sleep_ms(300);

		// Full of silent connections: a fifth source takes the oldest one's place.
		socket_t waiting[4];
		for (int i = 0; i < 4; ++i) {
			waiting[i] = dial_from(2 + i, h.passive_port);
			sleep_ms(100);
		}
		socket_t fifth = dial_from(6, h.passive_port);
		CHECK(fifth != INVALID_SOCKET && !closed_within(fifth, 300));
		CHECK(closed_within(waiting[0], 500));
		bool others_open = true;
		for (int i = 1; i < 4; ++i)
			others_open = others_open && !closed_within(waiting[i], 100);
		CHECK(others_open);
		for (int i = 0; i < 4; ++i)
			closesocket(waiting[i]);
		if (fifth != INVALID_SOCKET)
			closesocket(fifth);
		sleep_ms(300);

		// Full of connections that have each sent a byte: the same, a byte buys no place.
		for (int i = 0; i < 4; ++i) {
			waiting[i] = dial_from(2 + i, h.passive_port);
			send(waiting[i], "\0", 1, 0); // half of a frame header
			sleep_ms(100);
		}
		fifth = dial_from(6, h.passive_port);
		CHECK(fifth != INVALID_SOCKET && !closed_within(fifth, 300));
		CHECK(closed_within(waiting[0], 500));
		others_open = true;
		for (int i = 1; i < 4; ++i)
			others_open = others_open && !closed_within(waiting[i], 100);
		CHECK(others_open);
		for (int i = 0; i < 4; ++i)
			closesocket(waiting[i]);
		if (fifth != INVALID_SOCKET)
			closesocket(fifth);
		sleep_ms(300);
	}

	// The real thing: admitted, and answered on the same connection.
	len = binding_request(buf, sizeof(buf), good_user, pwd);
	socket_t good = dial(h.passive_port);
	send_frame(good, buf, (uint16_t)len, (size_t)len);
	set_timeout(good, 2000);
	uint16_t header = 0;
	int got = recv(good, (char *)&header, 2, MSG_WAITALL);
	CHECK(got == 2);
	int resp_len = ntohs(header);
	got = resp_len > 0 && resp_len <= (int)sizeof(buf) ? recv(good, buf, resp_len, MSG_WAITALL) : -1;
	CHECK(got == resp_len);
	stun_message_t resp;
	memset(&resp, 0, sizeof(resp));
	CHECK(got > 0 && stun_read(buf, (size_t)got, &resp) >= 0 &&
	      resp.msg_class == STUN_CLASS_RESP_SUCCESS && resp.msg_method == STUN_METHOD_BINDING);

	// Admitted: the listener is closed.
	sleep_ms(200);
	socket_t after = dial(h.passive_port);
	CHECK(after == INVALID_SOCKET);
	if (after != INVALID_SOCKET)
		closesocket(after);
	closesocket(good);

	juice_destroy(h.agent);
}

static void test_half_frames_do_not_keep_the_peer_out(void) {
	printf("Four connections holding half a frame header do not keep the peer out\n");
	const char *peer_pwd = "pw01234567890123456789";
	side_t h = {0};
	listening_host(&h, peer_pwd);
	CHECK(h.passive_port > 0);
	socket_t probe = dial_from(2, h.passive_port);
	if (probe == INVALID_SOCKET) {
		printf("  no other loopback address here: skipped\n");
		juice_destroy(h.agent);
		return;
	}
	closesocket(probe);
	sleep_ms(300);

	socket_t squat[4];
	for (int i = 0; i < 4; ++i) {
		squat[i] = dial_from(2 + i, h.passive_port);
		send(squat[i], "\0", 1, 0);
		sleep_ms(50);
	}
	socket_t peer = dial(h.passive_port);
	CHECK(peer != INVALID_SOCKET && peer_connects(&h, peer, peer_pwd));

	for (int i = 0; i < 4; ++i)
		closesocket(squat[i]);
	if (peer != INVALID_SOCKET)
		closesocket(peer);
	juice_destroy(h.agent);
}

static void test_announced_active_takes_no_place(void) {
	printf("An active candidate the peer announces takes no place from the one that dials\n");
	// Before, an announced "tcptype active" made a TCP pair nobody dials, which took the single
	// TCP place: the connection that did come in then had no pair, and the host never connected.
	const char *peer_pwd = "pw01234567890123456789";
	side_t h = {0};
	listening_host(&h, peer_pwd);
	CHECK(h.passive_port > 0);
	CHECK(juice_add_remote_candidate(
	          h.agent, "a=candidate:1 1 TCP 1518280447 127.0.0.1 9 typ host tcptype active") == 0);

	socket_t peer = dial(h.passive_port);
	CHECK(peer != INVALID_SOCKET);
	CHECK(peer != INVALID_SOCKET && peer_connects(&h, peer, peer_pwd));

	char local[JUICE_MAX_CANDIDATE_SDP_STRING_LEN], remote[JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
	if (juice_get_selected_candidates(h.agent, local, sizeof(local), remote, sizeof(remote)) == 0) {
		printf("  host selected: %s -> %s\n", local, remote);
		CHECK(strstr(remote, "prflx") != NULL);
	} else {
		CHECK(!"selected candidates");
	}

	if (peer != INVALID_SOCKET)
		closesocket(peer);
	juice_destroy(h.agent);
}

static void test_taken_tcp_port_means_no_candidate(void) {
	printf("A TCP port another socket holds: no listener, no candidate; free again, both\n");
	socket_t holder = INVALID_SOCKET;
	const uint16_t port = free_port(&holder);
	CHECK(port > 0);

	const char *remote = "a=ice-ufrag:peer\r\na=ice-pwd:pw01234567890123456789\r\n";
	for (int free_again = 0; free_again < 2; ++free_again) {
		if (free_again)
			closesocket(holder);
		side_t s = {0};
		s.port = port;
		make(&s, JUICE_ICE_TCP_MODE_ACTIVE_PASSIVE);
		juice_set_remote_description(s.agent, remote);
		CHECK(juice_gather_candidates(s.agent) == 0);
		for (int t = 0; t < 500 && !s.passive_port; t += 50)
			sleep_ms(50);
		if (free_again)
			CHECK(s.passive_port == port);
		else
			CHECK(s.passive_port == 0);
		juice_destroy(s.agent);
	}
}

static void test_active_only_does_not_listen(void) {
	printf("ACTIVE alone listens on nothing and offers no passive candidate\n");
	side_t s = {0};
	make(&s, JUICE_ICE_TCP_MODE_ACTIVE);
	juice_set_remote_description(s.agent, "a=ice-ufrag:peer\r\na=ice-pwd:pw01234567890123456789\r\n");
	juice_gather_candidates(s.agent);
	sleep_ms(500);
	CHECK(s.passive_port == 0);
	juice_destroy(s.agent);
}

// Not of the passive TCP, but of the same runner: libjuice-udp-long-datagram.patch
static void test_long_datagram_is_dropped(void) {
	printf("A datagram longer than the buffer is dropped, the next one is read\n");
	socket_t rx = socket(AF_INET, SOCK_DGRAM, 0);
	socket_t tx = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t sin_len = sizeof(sin);
	CHECK(bind(rx, (struct sockaddr *)&sin, sizeof(sin)) == 0);
	CHECK(getsockname(rx, (struct sockaddr *)&sin, &sin_len) == 0);
	set_timeout(rx, 2000);

	static char big[5000], next[1200];
	memset(big, 'b', sizeof(big));
	memset(next, 's', sizeof(next));
	CHECK(sendto(tx, big, sizeof(big), 0, (struct sockaddr *)&sin, sizeof(sin)) ==
	      (int)sizeof(big));
	CHECK(sendto(tx, next, sizeof(next), 0, (struct sockaddr *)&sin, sizeof(sin)) ==
	      (int)sizeof(next));

	char buffer[4096];
	addr_record_t src;
	int len = udp_recvfrom(rx, buffer, sizeof(buffer), &src);
	if (len == (int)sizeof(buffer) && buffer[0] == 'b') // cut short, where the system does that
		len = udp_recvfrom(rx, buffer, sizeof(buffer), &src);
	CHECK(len == (int)sizeof(next) && buffer[0] == 's');
	closesocket(rx);
	closesocket(tx);
}

int main(void) {
#ifdef _WIN32
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
	juice_set_log_level(getenv("JUICE_DEBUG") ? JUICE_LOG_LEVEL_DEBUG : JUICE_LOG_LEVEL_WARN);
	test_connect_over_tcp_only();
	test_selected_local_is_the_address_reached();
	test_burst_over_tcp();
	test_gate();
	test_half_frames_do_not_keep_the_peer_out();
	test_announced_active_takes_no_place();
	test_taken_tcp_port_means_no_candidate();
	test_active_only_does_not_listen();
	test_long_datagram_is_dropped();
	printf(failures ? "\n%d FAILURE(S)\n" : "\nall passed\n", failures);
	return failures ? 1 : 0;
}
