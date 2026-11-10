/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * Shared sockets, our patch on libjuice (third_party/patches/
 * libjuice-ice-mux.patch): an agent that sends on UDP sockets another process
 * holds and reads, and gets its own datagrams on a feed socket. Here the holder
 * is a thread of the test. It reads two sockets, the two ports every agent is
 * reached on, and hands each datagram to the agent it is for: a Binding request
 * by the ufrag it names, the rest by the address it comes from. A peer with a
 * socket of its own stands for the browser. Everything binds 127.0.0.1 (no
 * firewall prompt). Part of run_juice_tests, whose main() is in
 * test_ice_tcp_passive.c.
 */
#include "juice/juice.h"

#include "addr.h"
#include "conn_poll.h"
#include "ice.h"
#include "socket.h"
#include "stun.h"
#include "thread.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static void sleep_ms(int ms) { Sleep(ms); }
#else
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

#define PORTS 2
#define HOSTS_MAX 2
#define ROUTES_MAX 64
#define DATAGRAM_MAX 4096
#define PASSWORD "pw0123456789012345678901"

// ── The holder: what the main process is to its stream workers ──────────────

enum { NOBODY = -1, EVERYBODY = -2 };

typedef struct holder {
	socket_t port_sock[PORTS];
	uint16_t port[PORTS];
	socket_t out; // every feed is written from this one
	int hosts;
	socket_t feed[HOSTS_MAX];
	struct sockaddr_in feed_addr[HOSTS_MAX];
	const char *ufrag[HOSTS_MAX];
	int routes;
	struct {
		struct sockaddr_in src;
		int via;
		int host;
	} route[ROUTES_MAX];
	uint64_t clock;
	atomic(int) blocked[PORTS]; // nothing gets in through this port
	atomic(int) seen[PORTS];    // datagrams read on it
	atomic(int) strangers;      // datagrams that were for no agent
	atomic(int) stop;
	thread_t thread;
} holder_t;

static socket_t udp_loopback(uint16_t *port, struct sockaddr_in *name) {
	socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(0x7F000001);
	socklen_t len = sizeof(sin);
	if (bind(sock, (struct sockaddr *)&sin, sizeof(sin)) != 0 ||
	    getsockname(sock, (struct sockaddr *)&sin, &len) != 0) {
		closesocket(sock);
		return INVALID_SOCKET;
	}
	ctl_t nbio = 1;
	ioctlsocket(sock, FIONBIO, &nbio);
	if (port)
		*port = ntohs(sin.sin_port);
	if (name)
		*name = sin;
	return sock;
}

static int holder_known(const holder_t *h, const struct sockaddr_in *src, int via) {
	for (int i = 0; i < h->routes; ++i)
		if (h->route[i].via == via && h->route[i].src.sin_port == src->sin_port &&
		    h->route[i].src.sin_addr.s_addr == src->sin_addr.s_addr)
			return h->route[i].host;
	return NOBODY;
}

// Whose datagram this is. A Binding request names its agent, and teaches where that agent's peer
// talks from; an answer to a check the agent sent first comes from an address not yet known, and
// goes to all of them: only the one that sent the check knows its transaction.
static int holder_route(holder_t *h, char *data, size_t size, const struct sockaddr_in *src,
                        int via) {
	int known = holder_known(h, src, via);
	if (!is_stun_datagram(data, size))
		return known;

	stun_message_t msg;
	if (stun_read(data, size, &msg) < 0)
		return NOBODY;
	if (msg.msg_class != STUN_CLASS_REQUEST)
		return known != NOBODY ? known : EVERYBODY;

	for (int host = 0; host < h->hosts; ++host) {
		size_t n = strlen(h->ufrag[host]);
		if (strncmp(msg.credentials.username, h->ufrag[host], n) != 0 ||
		    msg.credentials.username[n] != ':')
			continue;
		if (known == NOBODY && h->routes < ROUTES_MAX) {
			h->route[h->routes].src = *src;
			h->route[h->routes].via = via;
			h->route[h->routes].host = host;
			++h->routes;
		}
		return host;
	}
	return NOBODY;
}

static void holder_hand(holder_t *h, int host, const char *datagram, size_t size) {
	sendto(h->out, datagram, (int)size, 0, (const struct sockaddr *)&h->feed_addr[host],
	       sizeof(h->feed_addr[host]));
}

static void holder_read(holder_t *h, int via) {
	for (int left = 64; left > 0; --left) {
		char buffer[JUICE_FEED_HEADER_SIZE + DATAGRAM_MAX];
		char *data = buffer + JUICE_FEED_HEADER_SIZE;
		struct sockaddr_in src;
		socklen_t src_len = sizeof(src);
		int len = recvfrom(h->port_sock[via], data, DATAGRAM_MAX, 0, (struct sockaddr *)&src,
		                   &src_len);
		if (len <= 0)
			return;

		atomic_store(&h->seen[via], atomic_load(&h->seen[via]) + 1);
		if (atomic_load(&h->blocked[via]))
			continue;

		int host = holder_route(h, data, (size_t)len, &src, via);
		if (host == NOBODY) {
			atomic_store(&h->strangers, atomic_load(&h->strangers) + 1);
			continue;
		}

		juice_feed_write_header(buffer, JUICE_FEED_HEADER_SIZE, 4, &src.sin_addr,
		                        ntohs(src.sin_port), 0, via, ++h->clock);
		size_t size = JUICE_FEED_HEADER_SIZE + (size_t)len;
		if (host == EVERYBODY) {
			for (int i = 0; i < h->hosts; ++i)
				holder_hand(h, i, buffer, size);
		} else {
			holder_hand(h, host, buffer, size);
		}
	}
}

static thread_return_t THREAD_CALL holder_run(void *arg) {
	holder_t *h = arg;
	while (!atomic_load(&h->stop)) {
		struct pollfd pfds[PORTS];
		for (int via = 0; via < PORTS; ++via) {
			pfds[via].fd = h->port_sock[via];
			pfds[via].events = POLLIN;
			pfds[via].revents = 0;
		}
		if (poll(pfds, PORTS, 20) <= 0)
			continue;
		for (int via = 0; via < PORTS; ++via)
			if (pfds[via].revents & POLLIN)
				holder_read(h, via);
	}
	return (thread_return_t)0;
}

// The ports, and a feed for each agent named by its ufrag, connected to the socket the holder
// writes them from: nothing else can put a datagram in.
static bool holder_start(holder_t *h, const char *ufrag_a, const char *ufrag_b) {
	memset(h, 0, sizeof(*h));
	struct sockaddr_in out_addr;
	h->out = udp_loopback(NULL, &out_addr);
	bool ok = h->out != INVALID_SOCKET;
	for (int via = 0; via < PORTS; ++via) {
		h->port_sock[via] = udp_loopback(&h->port[via], NULL);
		ok = ok && h->port_sock[via] != INVALID_SOCKET;
	}
	const char *ufrags[HOSTS_MAX] = {ufrag_a, ufrag_b};
	for (int host = 0; host < HOSTS_MAX && ufrags[host]; ++host) {
		h->ufrag[host] = ufrags[host];
		h->feed[host] = udp_loopback(NULL, &h->feed_addr[host]);
		ok = ok && h->feed[host] != INVALID_SOCKET &&
		     connect(h->feed[host], (struct sockaddr *)&out_addr, sizeof(out_addr)) == 0;
		++h->hosts;
	}
	return ok && thread_init(&h->thread, holder_run, h) == 0;
}

static void holder_stop(holder_t *h) {
	atomic_store(&h->stop, 1);
	thread_join(h->thread, NULL);
	closesocket(h->out);
	for (int via = 0; via < PORTS; ++via)
		closesocket(h->port_sock[via]);
	for (int host = 0; host < h->hosts; ++host)
		closesocket(h->feed[host]);
}

// ── Agents ──────────────────────────────────────────────────────────────────

typedef struct end {
	juice_agent_t *agent;
	struct end *peer; // its candidates go there, if set
	mutex_t lock;     // over what the connections thread writes below
	int received;
	char data[64];
} end_t;

static void on_candidate(juice_agent_t *agent, const char *sdp, void *user_ptr) {
	(void)agent;
	end_t *end = user_ptr;
	if (end->peer)
		juice_add_remote_candidate(end->peer->agent, sdp);
}

static void on_recv(juice_agent_t *agent, const char *data, size_t size, void *user_ptr) {
	(void)agent;
	end_t *end = user_ptr;
	size_t n = size < sizeof(end->data) - 1 ? size : sizeof(end->data) - 1;
	mutex_lock(&end->lock);
	memcpy(end->data, data, n);
	end->data[n] = '\0';
	++end->received;
	mutex_unlock(&end->lock);
}

// Whether `count` datagrams came, the last one `text` (NULL: whatever it was), within `ms`
static bool got(end_t *end, int count, const char *text, int ms) {
	for (int t = 0;; t += 20) {
		mutex_lock(&end->lock);
		const bool yes = end->received == count && (!text || strcmp(end->data, text) == 0);
		const bool late = end->received > count;
		mutex_unlock(&end->lock);
		if (yes || late || t >= ms)
			return yes;
		sleep_ms(20);
	}
}

static juice_agent_t *make_agent(end_t *end, const char *bind) {
	juice_config_t config;
	memset(&config, 0, sizeof(config));
	config.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
	config.bind_address = bind;
	config.cb_candidate = on_candidate;
	config.cb_recv = on_recv;
	config.user_ptr = end;
	mutex_init(&end->lock, 0);
	end->agent = juice_create(&config);
	return end->agent;
}

static void destroy_agent(end_t *end) {
	juice_destroy(end->agent);
	mutex_destroy(&end->lock);
}

static juice_shared_sockets_t shared_of(const holder_t *h, int host) {
	juice_shared_sockets_t shared;
	memset(&shared, 0, sizeof(shared));
	shared.feed = (uintptr_t)h->feed[host];
	for (int via = 0; via < PORTS; ++via)
		shared.send[via] = (uintptr_t)h->port_sock[via];
	shared.send_count = PORTS;
	return shared;
}

// The host, on the holder's ports, makes the offer and so leads the checks, as the product's
// does. `announced`: the peer's candidates travel to it; without, it learns the peer from the
// checks alone, as it does of a browser whose candidates it cannot resolve.
static void connect_pair(holder_t *h, int index, end_t *host, end_t *peer, bool announced) {
	memset(host, 0, sizeof(*host));
	memset(peer, 0, sizeof(*peer));
	make_agent(host, NULL);
	make_agent(peer, "127.0.0.1");
	host->peer = peer;
	peer->peer = announced ? host : NULL;

	juice_set_local_ice_attributes(host->agent, h->ufrag[index], PASSWORD);
	juice_shared_sockets_t shared = shared_of(h, index);
	CHECK(juice_set_shared_sockets(host->agent, &shared) == JUICE_ERR_SUCCESS);

	char sdp[JUICE_MAX_SDP_STRING_LEN];
	juice_get_local_description(host->agent, sdp, sizeof(sdp));
	juice_set_remote_description(peer->agent, sdp);
	juice_get_local_description(peer->agent, sdp, sizeof(sdp));
	juice_set_remote_description(host->agent, sdp);

	CHECK(juice_gather_candidates(host->agent) == 0);
	juice_gather_candidates(peer->agent);
}

static bool wait_state(juice_agent_t *agent, juice_state_t state, int ms) {
	for (int t = 0; t < ms; t += 20) {
		if (juice_get_state(agent) == state)
			return true;
		sleep_ms(20);
	}
	return juice_get_state(agent) == state;
}

// The port of the local side of the selected pair: which shared socket the session runs on.
static int selected_local_port(juice_agent_t *agent) {
	char local[JUICE_MAX_ADDRESS_STRING_LEN], remote[JUICE_MAX_ADDRESS_STRING_LEN];
	if (juice_get_selected_addresses(agent, local, sizeof(local), remote, sizeof(remote)) != 0)
		return -1;
	const char *colon = strrchr(local, ':');
	return colon ? atoi(colon + 1) : -1;
}

static void exchange(end_t *host, end_t *peer, const char *to_host, const char *to_peer) {
	CHECK(juice_send(peer->agent, to_host, strlen(to_host)) == 0);
	CHECK(juice_send(host->agent, to_peer, strlen(to_peer)) == 0);
	CHECK(got(host, 1, to_host, 2000));
	CHECK(got(peer, 1, to_peer, 2000));
}

// ── Tests ───────────────────────────────────────────────────────────────────

static atomic(int) timestamps;
static atomic(int) timestamp_zero;

static void on_timestamp(uint64_t timestamp) {
	atomic_store(&timestamps, atomic_load(&timestamps) + 1);
	if (timestamp == 0)
		atomic_store(&timestamp_zero, 1);
}

static void test_header(void) {
	printf("The header of a feed datagram reads back, and a wrong one is refused\n");
	char buffer[JUICE_FEED_HEADER_SIZE + 4];
	addr_record_t src;
	uint64_t timestamp = 0;

	const uint8_t v4[4] = {203, 0, 113, 7};
	CHECK(juice_feed_write_header(buffer, sizeof(buffer), 4, v4, 48550, 0, 1,
	                              0x0102030405060708ull) == JUICE_FEED_HEADER_SIZE);
	memset(&src, 0xAA, sizeof(src));
	CHECK(conn_poll_feed_read_header(buffer, sizeof(buffer), 2, &src, &timestamp) ==
	      JUICE_FEED_HEADER_SIZE);
	const struct sockaddr_in *sin = (const struct sockaddr_in *)&src.addr;
	CHECK(src.addr.ss_family == AF_INET && src.len == sizeof(struct sockaddr_in));
	CHECK(ntohs(sin->sin_port) == 48550 && memcmp(&sin->sin_addr, v4, 4) == 0);
	CHECK(src.via == 1 && src.socktype == SOCK_DGRAM);
	CHECK(timestamp == 0x0102030405060708ull);

	uint8_t v6[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x2a};
	CHECK(juice_feed_write_header(buffer, sizeof(buffer), 6, v6, 3478, 5, 0, 0) ==
	      JUICE_FEED_HEADER_SIZE);
	CHECK(conn_poll_feed_read_header(buffer, sizeof(buffer), 2, &src, NULL) ==
	      JUICE_FEED_HEADER_SIZE);
	const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)&src.addr;
	CHECK(src.addr.ss_family == AF_INET6 && src.len == sizeof(struct sockaddr_in6));
	CHECK(ntohs(sin6->sin6_port) == 3478 && memcmp(&sin6->sin6_addr, v6, 16) == 0);
	CHECK(sin6->sin6_scope_id == 5 && src.via == 0);

	// An IPv4 peer of a dual-stack socket is the IPv4 address, as recvfrom()'s is made
	uint8_t mapped[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 203, 0, 113, 7};
	juice_feed_write_header(buffer, sizeof(buffer), 6, mapped, 3478, 0, 0, 0);
	CHECK(conn_poll_feed_read_header(buffer, sizeof(buffer), 2, &src, NULL) ==
	      JUICE_FEED_HEADER_SIZE);
	CHECK(src.addr.ss_family == AF_INET && memcmp(&sin->sin_addr, v4, 4) == 0);

	// Refused: too short, another version, no such family, a socket the agent does not have
	juice_feed_write_header(buffer, sizeof(buffer), 4, v4, 48550, 0, 1, 0);
	CHECK(conn_poll_feed_read_header(buffer, JUICE_FEED_HEADER_SIZE - 1, 2, &src, NULL) == -1);
	CHECK(conn_poll_feed_read_header(buffer, sizeof(buffer), 1, &src, NULL) == -1);
	buffer[0] = JUICE_FEED_VERSION + 1;
	CHECK(conn_poll_feed_read_header(buffer, sizeof(buffer), 2, &src, NULL) == -1);
	buffer[0] = JUICE_FEED_VERSION;
	buffer[2] = 5;
	CHECK(conn_poll_feed_read_header(buffer, sizeof(buffer), 2, &src, NULL) == -1);

	CHECK(juice_feed_write_header(buffer, JUICE_FEED_HEADER_SIZE - 1, 4, v4, 1, 0, 0, 0) == -1);
	CHECK(juice_feed_write_header(buffer, sizeof(buffer), 5, v4, 1, 0, 0, 0) == -1);
	CHECK(juice_feed_write_header(buffer, sizeof(buffer), 4, v4, 1, 0, JUICE_SHARED_SOCKETS_MAX,
	                              0) == -1);
	CHECK(juice_feed_write_header(buffer, sizeof(buffer), 4, NULL, 1, 0, 0, 0) == -1);
}

static void test_via_is_part_of_an_address(void) {
	printf("An address heard on another shared socket is another address, less preferred\n");
	addr_record_t a, b;
	memset(&a, 0, sizeof(a));
	struct sockaddr_in *sin = (struct sockaddr_in *)&a.addr;
	sin->sin_family = AF_INET;
	sin->sin_port = htons(50000);
	sin->sin_addr.s_addr = htonl(0x7F000001);
	a.len = sizeof(*sin);
	a.socktype = SOCK_DGRAM;
	b = a;
	CHECK(addr_record_is_equal(&a, &b, true));
	b.via = 1;
	CHECK(!addr_record_is_equal(&a, &b, true));

	// What a browser sends for a peer-reflexive candidate, here through the second socket:
	// under any candidate through the first one (the least is server-reflexive), above TCP
	const uint32_t prflx = ice_compute_priority(ICE_CANDIDATE_TYPE_PEER_REFLEXIVE, AF_INET, 1, 0,
	                                            ICE_CANDIDATE_TRANSPORT_UDP);
	const uint32_t srflx = ice_compute_priority(ICE_CANDIDATE_TYPE_SERVER_REFLEXIVE, AF_INET, 1,
	                                            4095, ICE_CANDIDATE_TRANSPORT_UDP);
	const uint32_t tcp = ice_compute_priority(ICE_CANDIDATE_TYPE_HOST, AF_INET6, 1, 0,
	                                          ICE_CANDIDATE_TRANSPORT_TCP_TYPE_ACTIVE);
	CHECK(ice_penalize_priority_via(prflx, 0) == prflx);
	CHECK(ice_penalize_priority_via(prflx, 1) < srflx);
	CHECK(ice_penalize_priority_via(prflx, 1) > tcp);
	CHECK(ice_penalize_priority_via(0x05000000u, 1) == 0); // never below zero
}

static void test_refused_sockets(void) {
	printf("Sockets that are not what they must be are refused, and so is another mode\n");
	holder_t h;
	CHECK(holder_start(&h, "hostAAAA", NULL));

	end_t e = {0};
	juice_config_t config;
	memset(&config, 0, sizeof(config));
	config.concurrency_mode = JUICE_CONCURRENCY_MODE_THREAD;
	juice_agent_t *threaded = juice_create(&config);
	juice_shared_sockets_t shared = shared_of(&h, 0);
	CHECK(juice_set_shared_sockets(threaded, &shared) == JUICE_ERR_INVALID);
	juice_destroy(threaded);

	make_agent(&e, NULL);
	CHECK(juice_set_shared_sockets(NULL, &shared) == JUICE_ERR_INVALID);
	CHECK(juice_set_shared_sockets(e.agent, NULL) == JUICE_ERR_INVALID);
	shared.send_count = 0;
	CHECK(juice_set_shared_sockets(e.agent, &shared) == JUICE_ERR_INVALID);
	shared.send_count = JUICE_SHARED_SOCKETS_MAX + 1;
	CHECK(juice_set_shared_sockets(e.agent, &shared) == JUICE_ERR_INVALID);

	// A stream socket for a feed
	socket_t stream = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	shared = shared_of(&h, 0);
	shared.feed = (uintptr_t)stream;
	CHECK(juice_set_shared_sockets(e.agent, &shared) == JUICE_ERR_SUCCESS);
	CHECK(juice_gather_candidates(e.agent) != 0);
	destroy_agent(&e);

	// A socket bound to nothing to send on
	socket_t unbound = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	make_agent(&e, NULL);
	shared = shared_of(&h, 0);
	shared.send[1] = (uintptr_t)unbound;
	CHECK(juice_set_shared_sockets(e.agent, &shared) == JUICE_ERR_SUCCESS);
	CHECK(juice_gather_candidates(e.agent) != 0);
	destroy_agent(&e);

	// Too late once it gathers
	make_agent(&e, NULL);
	shared = shared_of(&h, 0);
	CHECK(juice_set_shared_sockets(e.agent, &shared) == JUICE_ERR_SUCCESS);
	CHECK(juice_gather_candidates(e.agent) == 0);
	CHECK(juice_set_shared_sockets(e.agent, &shared) == JUICE_ERR_FAILED);
	destroy_agent(&e);

	closesocket(stream);
	closesocket(unbound);
	holder_stop(&h);
}

static void test_both_ports(bool announced) {
	printf("Both ports open, the peer %s: the session runs on the first\n",
	       announced ? "announced" : "learnt from its checks");
	holder_t h;
	CHECK(holder_start(&h, "hostAAAA", NULL));
	atomic_store(&timestamps, 0);
	atomic_store(&timestamp_zero, 0);
	juice_set_feed_timestamp_handler(on_timestamp);

	// What the feed drops, and goes on: datagrams no header fits, a header with no datagram
	// behind it, and a datagram larger than any the agent reads
	char junk[JUICE_FEED_HEADER_SIZE + DATAGRAM_MAX + 1000];
	memset(junk, 0x5A, sizeof(junk));
	holder_hand(&h, 0, junk, 7);
	holder_hand(&h, 0, junk, sizeof(junk));
	const uint8_t nowhere[4] = {192, 0, 2, 1};
	juice_feed_write_header(junk, sizeof(junk), 4, nowhere, 9, 0, 0, 1);
	holder_hand(&h, 0, junk, JUICE_FEED_HEADER_SIZE);
	holder_hand(&h, 0, junk, sizeof(junk));

	end_t host, peer;
	connect_pair(&h, 0, &host, &peer, announced);
	CHECK(wait_state(host.agent, JUICE_STATE_COMPLETED, 10000));
	CHECK(wait_state(peer.agent, JUICE_STATE_COMPLETED, 5000));
	CHECK(selected_local_port(host.agent) == h.port[0]);
	exchange(&host, &peer, "from the browser", "from the host");

	// The feed delivered every datagram with the holder's timestamp
	CHECK(atomic_load(&timestamps) > 0 && !atomic_load(&timestamp_zero));
	juice_set_feed_timestamp_handler(NULL);

#ifndef _WIN32
	// A class asked for a datagram is set on the socket the session runs on, and on that one
	// only (Windows takes it with another patch, which a test tree may not have)
	CHECK(juice_send_diffserv(host.agent, "ef", 2, 0xB8) == 0);
	int tos[PORTS];
	for (int via = 0; via < PORTS; ++via) {
		socklen_t tos_len = sizeof(tos[via]);
		tos[via] = -1;
		getsockopt(h.port_sock[via], IPPROTO_IP, IP_TOS, (char *)&tos[via], &tos_len);
	}
	CHECK(tos[0] == 0xB8 && tos[1] == 0);
#endif

	// The agent never reads the shared sockets: every datagram that comes in on a port is the
	// holder's to read, here 100 on each from a stranger
	socket_t stranger = udp_loopback(NULL, NULL);
	const int before = atomic_load(&h.strangers);
	for (int i = 0; i < 100; ++i) {
		for (int via = 0; via < PORTS; ++via) {
			struct sockaddr_in to;
			memset(&to, 0, sizeof(to));
			to.sin_family = AF_INET;
			to.sin_addr.s_addr = htonl(0x7F000001);
			to.sin_port = htons(h.port[via]);
			const char rtp_like[12] = {(char)0x80, 96};
			sendto(stranger, rtp_like, sizeof(rtp_like), 0, (struct sockaddr *)&to, sizeof(to));
		}
		if (i % 10 == 9)
			sleep_ms(2);
	}
	for (int t = 0; t < 2000 && atomic_load(&h.strangers) - before < 200; t += 20)
		sleep_ms(20);
	CHECK(atomic_load(&h.strangers) - before == 200);
	CHECK(juice_get_state(host.agent) == JUICE_STATE_COMPLETED);
	closesocket(stranger);

	destroy_agent(&host);
	destroy_agent(&peer);

	// Borrowed: the agent is gone, the sockets are not
	struct sockaddr_in to;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = htonl(0x7F000001);
	to.sin_port = htons(h.port[1]);
	for (int via = 0; via < PORTS; ++via)
		CHECK(sendto(h.port_sock[via], "x", 1, 0, (struct sockaddr *)&to, sizeof(to)) == 1);
	int type = 0;
	socklen_t type_len = sizeof(type);
	CHECK(getsockopt(h.feed[0], SOL_SOCKET, SO_TYPE, (char *)&type, &type_len) == 0);
	holder_stop(&h);
}

static void test_one_port_blocked(int blocked, bool announced) {
	printf("The %s port lets nothing in, the peer %s: the session runs on the other\n",
	       blocked == 0 ? "first" : "second", announced ? "announced" : "learnt from its checks");
	holder_t h;
	CHECK(holder_start(&h, "hostAAAA", NULL));
	atomic_store(&h.blocked[blocked], 1);

	end_t host, peer;
	connect_pair(&h, 0, &host, &peer, announced);
	// On the second port the host first waits for the pair it prefers: its nomination timeout
	CHECK(wait_state(host.agent, JUICE_STATE_COMPLETED, 15000));
	CHECK(wait_state(peer.agent, JUICE_STATE_COMPLETED, 5000));
	CHECK(selected_local_port(host.agent) == h.port[1 - blocked]);
	for (int t = 0; t < 2000 && !atomic_load(&h.seen[blocked]); t += 20)
		sleep_ms(20);
	CHECK(atomic_load(&h.seen[blocked]) > 0); // the peer did try it
	exchange(&host, &peer, "from the browser", "from the host");

	destroy_agent(&host);
	destroy_agent(&peer);
	holder_stop(&h);
}

static void test_first_port_late(void) {
	printf("The first port only answers later: the session moves to it before it settles\n");
	holder_t h;
	CHECK(holder_start(&h, "hostAAAA", NULL));
	atomic_store(&h.blocked[0], 1);

	// The peer is learnt from its checks, so nothing is left to check once the second port
	// answers: the host must still wait before it nominates that pair
	end_t host, peer;
	connect_pair(&h, 0, &host, &peer, false);
	CHECK(wait_state(host.agent, JUICE_STATE_CONNECTED, 5000));
	CHECK(selected_local_port(host.agent) == h.port[1]);
	exchange(&host, &peer, "on the second port", "from the host");

	atomic_store(&h.blocked[0], 0);
	CHECK(wait_state(host.agent, JUICE_STATE_COMPLETED, 10000));
	CHECK(wait_state(peer.agent, JUICE_STATE_COMPLETED, 5000));
	CHECK(selected_local_port(host.agent) == h.port[0]);
	CHECK(juice_send(peer.agent, "on the first port", 17) == 0);
	CHECK(got(&host, 2, "on the first port", 2000));

	destroy_agent(&host);
	destroy_agent(&peer);
	holder_stop(&h);
}

static void test_two_hosts(void) {
	printf("Two agents on the same two ports, each with its peer\n");
	holder_t h;
	CHECK(holder_start(&h, "hostAAAA", "hostBBBB"));

	end_t host_a, peer_a, host_b, peer_b;
	connect_pair(&h, 0, &host_a, &peer_a, true);
	connect_pair(&h, 1, &host_b, &peer_b, false);
	CHECK(wait_state(host_a.agent, JUICE_STATE_COMPLETED, 10000));
	CHECK(wait_state(host_b.agent, JUICE_STATE_COMPLETED, 10000));
	CHECK(wait_state(peer_a.agent, JUICE_STATE_COMPLETED, 5000));
	CHECK(wait_state(peer_b.agent, JUICE_STATE_COMPLETED, 5000));
	CHECK(selected_local_port(host_a.agent) == h.port[0]);
	CHECK(selected_local_port(host_b.agent) == h.port[0]);

	// Each got its own peer's datagram, and only that one
	exchange(&host_a, &peer_a, "to the first host", "from the first host");
	exchange(&host_b, &peer_b, "to the second host", "from the second host");
	sleep_ms(200);
	CHECK(got(&host_a, 1, NULL, 0) && got(&host_b, 1, NULL, 0));

	// One ends; the other goes on
	destroy_agent(&host_a);
	destroy_agent(&peer_a);
	CHECK(juice_send(peer_b.agent, "again", 5) == 0);
	CHECK(got(&host_b, 2, "again", 2000));

	destroy_agent(&host_b);
	destroy_agent(&peer_b);
	holder_stop(&h);
}

int ice_mux_tests(void) {
	test_header();
	test_via_is_part_of_an_address();
	test_refused_sockets();
	test_both_ports(true);
	test_both_ports(false);
	test_one_port_blocked(0, true);
	test_one_port_blocked(0, false);
	test_one_port_blocked(1, true);
	test_first_port_late();
	test_two_hosts();
	return failures;
}
