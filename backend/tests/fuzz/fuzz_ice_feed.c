/*
 * MoonlightWeb — fuzz target. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The first thing an agent on shared sockets reads of every datagram of its
 * feed: the header that says where the datagram came from and which socket it
 * came in on (third_party/patches/libjuice-ice-mux.patch,
 * conn_poll_feed_read_header). Whatever the bytes, it either refuses them or
 * gives an address the rest of libjuice can use as it is.
 *
 * libFuzzer, built by no CMake file of the repository. With clang, from a
 * build of libjuice (juice-static) compiled with
 * -fsanitize=fuzzer-no-link,address,undefined:
 *   clang -g -O1 -fsanitize=fuzzer,address,undefined -I<libjuice>/src \
 *         -I<libjuice>/include fuzz_ice_feed.c libjuice-static.a -o fuzz_ice_feed
 *   ./fuzz_ice_feed -max_len=128 -max_total_time=60
 */
#include "juice/juice.h"

#include "addr.h"
#include "conn_poll.h"
#include "socket.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	if (size < 1)
		return 0;

	// The first byte picks how many shared sockets the agent has
	const int via_count = 1 + data[0] % JUICE_SHARED_SOCKETS_MAX;
	const char *datagram = (const char *)data + 1;
	const size_t datagram_size = size - 1;

	addr_record_t src;
	memset(&src, 0xA5, sizeof(src));
	uint64_t timestamp = 0;
	const int header =
	    conn_poll_feed_read_header(datagram, datagram_size, via_count, &src, &timestamp);
	if (header < 0)
		return 0;

	// Accepted: a whole header, of the one version, naming a socket the agent has
	if (header != JUICE_FEED_HEADER_SIZE || datagram_size < (size_t)header)
		abort();
	if ((uint8_t)datagram[0] != JUICE_FEED_VERSION)
		abort();
	if (src.via < 0 || src.via >= via_count || src.via != (uint8_t)datagram[1])
		abort();

	// and an address of a family libjuice knows, of that family's size
	if (src.socktype != SOCK_DGRAM)
		abort();
	if (src.addr.ss_family == AF_INET) {
		if (src.len != sizeof(struct sockaddr_in))
			abort();
	} else if (src.addr.ss_family == AF_INET6) {
		if (src.len != sizeof(struct sockaddr_in6))
			abort();
	} else {
		abort();
	}

	// which the library's own functions take
	char text[ADDR_MAX_STRING_LEN];
	addr_record_to_string(&src, text, sizeof(text));
	addr_record_t same = src;
	if (!addr_record_is_equal(&src, &same, true))
		abort();
	(void)addr_record_hash(&src, true);
	(void)addr_get_port((const struct sockaddr *)&src.addr);
	return 0;
}
