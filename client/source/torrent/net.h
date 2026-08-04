#pragma once
// Adapted from pipensx (https://github.com/i3sey/pipensx,
// src/core/net.c/.h, GPL-3.0) - see ../../THIRD_PARTY_NOTICES.md.
//
// Non-blocking BSD socket helpers shared by tracker.c, peer.c, and dht.c -
// every socket this returns is already non-blocking, since the whole
// torrent engine is driven by torrent_step() polling many of them at once
// (poll()) rather than blocking on any single one, the same "caller drives
// it forward a little at a time" shape mtp_step/ftp_step already use, just
// multiplexed across several sockets instead of one.
#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

typedef int socket_t;
#define INVALID_SOCK (-1)
#define NET_TCP_RECEIVE_BUFFER_SIZE (256 * 1024)
#define NET_TCP_RECEIVE_BUFFER_FALLBACK_SIZE (128 * 1024)

// Resolves a hostname to an IPv4 address. Returns 1 on success.
int net_resolve(const char *host, uint16_t port, struct sockaddr_in *out);

// Creates a non-blocking TCP socket and starts connecting - completion is
// detected the usual non-blocking-connect way (poll for writable, then
// check SO_ERROR), not by this call itself.
socket_t net_tcp_connect(const struct sockaddr_in *addr);

// Requests a larger TCP receive buffer once a peer connection's handshake
// succeeds - best-effort, retrying once with a smaller fallback size if the
// full request exhausts the socket-buffer pool. Failure leaves the socket
// perfectly usable at the stock buffer size.
int net_set_tcp_receive_buffer(socket_t fd);

// Creates a non-blocking UDP socket, optionally bound to a fixed local port
// (0 = ephemeral, picked by the OS).
socket_t net_udp_socket(uint16_t local_port);

// Creates a non-blocking listening TCP socket bound to `port` (0 = ephemeral).
socket_t net_tcp_listen(uint16_t port, int backlog);

// Accepts one pending connection from a non-blocking listener. Returns a
// non-blocking socket, or INVALID_SOCK when none is pending or on error.
// `peer` may be NULL.
socket_t net_accept(socket_t listener, struct sockaddr_in *peer);

// The local port a socket is bound to (0 on error) - for reading back what
// port an ephemeral (port-0) listener actually landed on.
uint16_t net_local_port(socket_t fd);

int net_set_nonblock(socket_t fd);
void net_close(socket_t fd);

// Portable send/recv wrappers (retry on EINTR). net_send writes as much as
// the socket currently accepts without blocking - returns bytes sent
// (possibly 0 if the send buffer is full right now), or -1 on a hard
// error. The caller owns queuing whatever tail didn't fit.
ssize_t net_send(socket_t fd, const uint8_t *buf, size_t len);
int net_recv(socket_t fd, uint8_t *buf, size_t len);
