/*
WII_LOOPBACK_NET.C

HWI-016D: the sockets under the Xbox Winsock and XNet layer (the Linux
port's port/linux/src/xnet.c, reused unchanged), on the Wii: an in-process
loopback. A split-screen game is a network game whose host and client are
the same machine: the host listens and the client joins it through
127.0.0.1 (xnet.c), over a stream connection and datagrams. The Wii has no
network stack the build uses yet (system link and internet play are later
work, HWI-030s), so every socket here reaches only the other sockets of
this process:

- datagrams go to the socket bound to the destination port (any address:
  127.0.0.1, this machine's, or a broadcast), and come from 127.0.0.1;
- a stream connect to a port with a listening socket is accepted at once
  (a new socket waits in the listener's queue for accept); bytes sent on
  one end are received on the other in order; closing one end is the
  other's end of stream;
- select answers at once (readable: bytes, a connection to accept, a
  datagram, or the end; writeable: connected); no call ever blocks.

This machine's local network address is none (xnet.c then reports no
ethernet link: no system link play), names do not resolve, and the random
bytes XNet asks for are a fixed sequence, so every launch (and the i686
host) makes the same keys and nonces. Internet play (p2p) is not on the Wii:
its entry points answer that no address is a peer's.

The calls follow port/linux/src/posix.h: -1 on failure with the Winsock
error from posix_socket_last_error(). Compiled into engine.dol and the i686
host reference alike (C only).
*/

#include "platform.h"
#include "posix.h"
#include "p2p.h"
#include "wii_platform.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* ---------- constants */

enum
{
	LOOPBACK_SOCKETS = 64,
	LOOPBACK_FIRST_DESCRIPTOR = 100,
	LOOPBACK_STREAM_LIMIT = 1024 * 1024,
	LOOPBACK_DATAGRAM_QUEUE = 256,
	LOOPBACK_DATAGRAM_LIMIT = 64 * 1024,
	LOOPBACK_ACCEPT_QUEUE = 8,
	LOOPBACK_FIRST_EPHEMERAL_PORT = 49152,
	/* Winsock's (the XDK headers' SOCK_* and AF_INET) */
	LOOPBACK_SOCK_STREAM = 1,
	LOOPBACK_SOCK_DGRAM = 2,
	LOOPBACK_AF_INET = 2,
};

/* Winsock's sockaddr_in: the family in this machine's order, the port and
address in network order (kept as their bytes) */
struct loopback_address
{
	short family;
	unsigned short port;
	unsigned char address[4];
	unsigned char zero[8];
};

struct loopback_datagram
{
	unsigned short from_port;
	int size;
	unsigned char *data;
};

struct loopback_socket
{
	int used;
	int type;
	int nonblocking;
	unsigned short port;          /* bound (network order), 0 if not */
	int listening;
	int connected;
	int peer;                     /* the other end's slot, or -1 */
	int peer_closed;
	unsigned short peer_port;
	/* stream: bytes received, not yet read */
	unsigned char *bytes;
	int byte_count, byte_capacity;
	/* listening: connections waiting for accept (slots) */
	int accept_queue[LOOPBACK_ACCEPT_QUEUE];
	int accept_count;
	/* datagrams received, not yet read */
	struct loopback_datagram datagrams[LOOPBACK_DATAGRAM_QUEUE];
	int datagram_first, datagram_count;
};

/* ---------- globals */

static pthread_mutex_t loopback_lock = PTHREAD_MUTEX_INITIALIZER;
static struct loopback_socket loopback_sockets[LOOPBACK_SOCKETS];
static int loopback_last_error;
static unsigned short loopback_next_port = LOOPBACK_FIRST_EPHEMERAL_PORT;
static unsigned long loopback_random_state = 0x48574931u;
static unsigned long loopback_counts[4]; /* sockets made, streams connected, datagrams, stream bytes */

/* ---------- private code */

static unsigned short network_port(
	unsigned short host_order)
{
	unsigned char bytes[2] = { (unsigned char)(host_order >> 8), (unsigned char)host_order };
	unsigned short port;

	memcpy(&port, bytes, 2);
	return port;
}

static int fail(
	int error)
{
	loopback_last_error = error;
	return -1;
}

static int succeed(
	int result)
{
	loopback_last_error = 0;
	return result;
}

static struct loopback_socket *socket_at(
	int descriptor)
{
	int slot = descriptor - LOOPBACK_FIRST_DESCRIPTOR;

	if (slot < 0 || slot >= LOOPBACK_SOCKETS || !loopback_sockets[slot].used)
		return NULL;
	return &loopback_sockets[slot];
}

static int slot_of(
	struct loopback_socket const *socket)
{
	return (int)(socket - loopback_sockets);
}

static int bound_to(
	int type,
	unsigned short port,
	int listening)
{
	int slot;

	for (slot = 0; slot < LOOPBACK_SOCKETS; slot++)
	{
		struct loopback_socket const *socket = &loopback_sockets[slot];

		if (socket->used && socket->type == type && socket->port == port && (!listening || socket->listening))
			return slot;
	}
	return -1;
}

static unsigned short ephemeral_port(
	int type)
{
	int tries;

	for (tries = 0; tries < 16384; tries++)
	{
		unsigned short port = network_port(loopback_next_port);

		loopback_next_port = loopback_next_port >= 65535 ? LOOPBACK_FIRST_EPHEMERAL_PORT : loopback_next_port + 1;
		if (bound_to(type, port, FALSE) < 0)
			return port;
	}
	return 0;
}

static void write_address(
	void *address,
	int *address_length,
	unsigned short port)
{
	struct loopback_address value;

	if (!address || !address_length || *address_length < (int)sizeof(value))
		return;
	memset(&value, 0, sizeof(value));
	value.family = LOOPBACK_AF_INET;
	value.port = port;
	value.address[0] = 127;
	value.address[3] = 1;
	memcpy(address, &value, sizeof(value));
	*address_length = (int)sizeof(value);
}

static int read_port(
	const void *address,
	int address_length,
	unsigned short *port)
{
	struct loopback_address value;

	if (!address || address_length < (int)sizeof(value))
		return 0;
	memcpy(&value, address, sizeof(value));
	if (value.family != LOOPBACK_AF_INET)
		return 0;
	*port = value.port;
	return 1;
}

static int stream_append(
	struct loopback_socket *socket,
	const void *data,
	int size)
{
	if (socket->byte_count + size > LOOPBACK_STREAM_LIMIT)
		return 0;
	if (socket->byte_count + size > socket->byte_capacity)
	{
		int capacity = socket->byte_capacity ? socket->byte_capacity : 4096;
		unsigned char *bytes;

		while (capacity < socket->byte_count + size)
			capacity *= 2;
		bytes = realloc(socket->bytes, (size_t)capacity);
		if (!bytes)
			return 0;
		socket->bytes = bytes;
		socket->byte_capacity = capacity;
	}
	memcpy(socket->bytes + socket->byte_count, data, (size_t)size);
	socket->byte_count += size;
	return 1;
}

static void socket_release(
	struct loopback_socket *socket)
{
	int index;

	free(socket->bytes);
	for (index = 0; index < socket->datagram_count; index++)
		free(socket->datagrams[(socket->datagram_first + index) % LOOPBACK_DATAGRAM_QUEUE].data);
	memset(socket, 0, sizeof(*socket));
	socket->peer = -1;
}

static int readable(
	struct loopback_socket const *socket)
{
	if (socket->type == LOOPBACK_SOCK_DGRAM)
		return socket->datagram_count > 0;
	if (socket->listening)
		return socket->accept_count > 0;
	return socket->byte_count > 0 || socket->peer_closed;
}

/* ---------- posix.h: sockets */

int posix_socket_last_error(
	void)
{
	return loopback_last_error;
}

int posix_socket(
	int family,
	int type,
	int protocol)
{
	int slot;
	int result = -1;

	(void)protocol;
	if (family != LOOPBACK_AF_INET || (type != LOOPBACK_SOCK_STREAM && type != LOOPBACK_SOCK_DGRAM))
		return fail(WSAEAFNOSUPPORT);
	pthread_mutex_lock(&loopback_lock);
	for (slot = 0; slot < LOOPBACK_SOCKETS; slot++)
	{
		if (!loopback_sockets[slot].used)
		{
			socket_release(&loopback_sockets[slot]);
			loopback_sockets[slot].used = 1;
			loopback_sockets[slot].type = type;
			loopback_counts[0]++;
			result = LOOPBACK_FIRST_DESCRIPTOR + slot;
			break;
		}
	}
	pthread_mutex_unlock(&loopback_lock);
	return result < 0 ? fail(WSAEMFILE) : succeed(result);
}

int posix_socket_close(
	int descriptor)
{
	struct loopback_socket *socket;
	int index;

	pthread_mutex_lock(&loopback_lock);
	socket = socket_at(descriptor);
	if (!socket)
	{
		pthread_mutex_unlock(&loopback_lock);
		return fail(WSAENOTSOCK);
	}
	if (socket->peer >= 0 && loopback_sockets[socket->peer].used)
		loopback_sockets[socket->peer].peer_closed = 1;
	/* connections never accepted are closed with their listener */
	for (index = 0; index < socket->accept_count; index++)
		socket_release(&loopback_sockets[socket->accept_queue[index]]);
	socket_release(socket);
	pthread_mutex_unlock(&loopback_lock);
	return succeed(0);
}

int posix_socket_bind(
	int descriptor,
	const void *address,
	int address_length)
{
	struct loopback_socket *socket;
	unsigned short port;
	int result;

	if (!read_port(address, address_length, &port))
		return fail(WSAEFAULT);
	pthread_mutex_lock(&loopback_lock);
	socket = socket_at(descriptor);
	if (!socket)
		result = fail(WSAENOTSOCK);
	else if (socket->port)
		result = fail(WSAEINVAL);
	else
	{
		if (!port)
			port = ephemeral_port(socket->type);
		if (!port || bound_to(socket->type, port, FALSE) >= 0)
			result = fail(WSAEADDRINUSE);
		else
		{
			socket->port = port;
			result = succeed(0);
		}
	}
	pthread_mutex_unlock(&loopback_lock);
	return result;
}

int posix_socket_connect(
	int descriptor,
	const void *address,
	int address_length)
{
	struct loopback_socket *socket;
	unsigned short port;
	int listener, slot;
	int result;

	if (!read_port(address, address_length, &port))
		return fail(WSAEFAULT);
	pthread_mutex_lock(&loopback_lock);
	socket = socket_at(descriptor);
	if (!socket)
		result = fail(WSAENOTSOCK);
	else if (socket->type == LOOPBACK_SOCK_DGRAM)
	{
		/* a datagram socket's default destination */
		if (!socket->port)
			socket->port = ephemeral_port(socket->type);
		socket->peer_port = port;
		socket->connected = 1;
		result = succeed(0);
	}
	else if (socket->connected)
		result = fail(WSAEISCONN);
	else if ((listener = bound_to(LOOPBACK_SOCK_STREAM, port, TRUE)) < 0 ||
		loopback_sockets[listener].accept_count >= LOOPBACK_ACCEPT_QUEUE)
	{
		result = fail(WSAECONNREFUSED);
	}
	else
	{
		/* the listener's end, waiting for accept */
		for (slot = 0; slot < LOOPBACK_SOCKETS && loopback_sockets[slot].used; slot++)
			;
		if (slot == LOOPBACK_SOCKETS)
			result = fail(WSAENOBUFS);
		else
		{
			struct loopback_socket *other = &loopback_sockets[slot];
			struct loopback_socket *server = &loopback_sockets[listener];

			socket_release(other);
			if (!socket->port)
				socket->port = ephemeral_port(LOOPBACK_SOCK_STREAM);
			other->used = 1;
			other->type = LOOPBACK_SOCK_STREAM;
			other->port = server->port;
			other->connected = 1;
			other->peer = slot_of(socket);
			other->peer_port = socket->port;
			socket->connected = 1;
			socket->peer = slot;
			socket->peer_port = port;
			server->accept_queue[server->accept_count++] = slot;
			loopback_counts[1]++;
			result = succeed(0);
		}
	}
	pthread_mutex_unlock(&loopback_lock);
	return result;
}

int posix_socket_listen(
	int descriptor,
	int backlog)
{
	struct loopback_socket *socket;
	int result;

	(void)backlog;
	pthread_mutex_lock(&loopback_lock);
	socket = socket_at(descriptor);
	if (!socket || socket->type != LOOPBACK_SOCK_STREAM)
		result = fail(socket ? WSAEOPNOTSUPP : WSAENOTSOCK);
	else
	{
		if (!socket->port)
			socket->port = ephemeral_port(LOOPBACK_SOCK_STREAM);
		socket->listening = 1;
		result = succeed(0);
	}
	pthread_mutex_unlock(&loopback_lock);
	return result;
}

int posix_socket_accept(
	int descriptor,
	void *address,
	int *address_length)
{
	struct loopback_socket *socket;
	int result;

	pthread_mutex_lock(&loopback_lock);
	socket = socket_at(descriptor);
	if (!socket || !socket->listening)
		result = fail(socket ? WSAEINVAL : WSAENOTSOCK);
	else if (!socket->accept_count)
		result = fail(WSAEWOULDBLOCK);
	else
	{
		int slot = socket->accept_queue[0];

		memmove(socket->accept_queue, socket->accept_queue + 1, sizeof(int) * (size_t)(--socket->accept_count));
		write_address(address, address_length, loopback_sockets[slot].peer_port);
		result = succeed(LOOPBACK_FIRST_DESCRIPTOR + slot);
	}
	pthread_mutex_unlock(&loopback_lock);
	return result;
}

static int datagram_deliver(
	unsigned short from_port,
	unsigned short to_port,
	const void *buffer,
	int length)
{
	int slot = bound_to(LOOPBACK_SOCK_DGRAM, to_port, FALSE);
	struct loopback_socket *target;
	struct loopback_datagram *datagram;

	/* (no socket there: lost, as a datagram may be) */
	if (slot < 0)
		return length;
	target = &loopback_sockets[slot];
	if (target->datagram_count == LOOPBACK_DATAGRAM_QUEUE || length > LOOPBACK_DATAGRAM_LIMIT)
		return length;
	datagram = &target->datagrams[(target->datagram_first + target->datagram_count) % LOOPBACK_DATAGRAM_QUEUE];
	datagram->data = malloc(length > 0 ? (size_t)length : 1);
	if (!datagram->data)
		return length;
	memcpy(datagram->data, buffer, (size_t)length);
	datagram->size = length;
	datagram->from_port = from_port;
	target->datagram_count++;
	loopback_counts[2]++;
	return length;
}

int posix_socket_send(
	int descriptor,
	const void *buffer,
	int length,
	int flags)
{
	struct loopback_socket *socket;
	int result;

	(void)flags;
	pthread_mutex_lock(&loopback_lock);
	socket = socket_at(descriptor);
	if (!socket)
		result = fail(WSAENOTSOCK);
	else if (!socket->connected)
		result = fail(WSAENOTCONN);
	else if (socket->type == LOOPBACK_SOCK_DGRAM)
		result = succeed(datagram_deliver(socket->port, socket->peer_port, buffer, length));
	else if (socket->peer_closed || socket->peer < 0 || !loopback_sockets[socket->peer].used)
		result = fail(WSAECONNRESET);
	else if (!stream_append(&loopback_sockets[socket->peer], buffer, length))
		result = fail(WSAEWOULDBLOCK);
	else
	{
		loopback_counts[3] += (unsigned long)length;
		result = succeed(length);
	}
	pthread_mutex_unlock(&loopback_lock);
	return result;
}

int posix_socket_sendto(
	int descriptor,
	const void *buffer,
	int length,
	int flags,
	const void *address,
	int address_length)
{
	struct loopback_socket *socket;
	unsigned short port;
	int result;

	(void)flags;
	if (!read_port(address, address_length, &port))
		return fail(WSAEFAULT);
	pthread_mutex_lock(&loopback_lock);
	socket = socket_at(descriptor);
	if (!socket || socket->type != LOOPBACK_SOCK_DGRAM)
		result = fail(socket ? WSAEOPNOTSUPP : WSAENOTSOCK);
	else
	{
		if (!socket->port)
			socket->port = ephemeral_port(LOOPBACK_SOCK_DGRAM);
		result = succeed(datagram_deliver(socket->port, port, buffer, length));
	}
	pthread_mutex_unlock(&loopback_lock);
	return result;
}

int posix_socket_recv(
	int descriptor,
	void *buffer,
	int length,
	int flags)
{
	struct loopback_socket *socket;
	int result;

	(void)flags;
	pthread_mutex_lock(&loopback_lock);
	socket = socket_at(descriptor);
	if (!socket)
		result = fail(WSAENOTSOCK);
	else if (socket->type == LOOPBACK_SOCK_DGRAM)
	{
		pthread_mutex_unlock(&loopback_lock);
		return posix_socket_recvfrom(descriptor, buffer, length, flags, NULL, NULL);
	}
	else if (socket->byte_count)
	{
		int count = length < socket->byte_count ? length : socket->byte_count;

		memcpy(buffer, socket->bytes, (size_t)count);
		memmove(socket->bytes, socket->bytes + count, (size_t)(socket->byte_count - count));
		socket->byte_count -= count;
		result = succeed(count);
	}
	else if (socket->peer_closed)
		result = succeed(0);
	else
		result = fail(socket->connected ? WSAEWOULDBLOCK : WSAENOTCONN);
	pthread_mutex_unlock(&loopback_lock);
	return result;
}

int posix_socket_recvfrom(
	int descriptor,
	void *buffer,
	int length,
	int flags,
	void *address,
	int *address_length)
{
	struct loopback_socket *socket;
	int result;

	(void)flags;
	pthread_mutex_lock(&loopback_lock);
	socket = socket_at(descriptor);
	if (!socket)
		result = fail(WSAENOTSOCK);
	else if (socket->type != LOOPBACK_SOCK_DGRAM)
	{
		pthread_mutex_unlock(&loopback_lock);
		result = posix_socket_recv(descriptor, buffer, length, flags);
		if (result >= 0 && socket)
			write_address(address, address_length, socket->peer_port);
		return result;
	}
	else if (!socket->datagram_count)
		result = fail(WSAEWOULDBLOCK);
	else
	{
		struct loopback_datagram *datagram = &socket->datagrams[socket->datagram_first];
		int truncated = datagram->size > length;

		memcpy(buffer, datagram->data, (size_t)(truncated ? length : datagram->size));
		write_address(address, address_length, datagram->from_port);
		result = truncated ? fail(WSAEMSGSIZE) : succeed(datagram->size);
		free(datagram->data);
		datagram->data = NULL;
		socket->datagram_first = (socket->datagram_first + 1) % LOOPBACK_DATAGRAM_QUEUE;
		socket->datagram_count--;
	}
	pthread_mutex_unlock(&loopback_lock);
	return result;
}

int posix_socket_shutdown(
	int descriptor,
	int how)
{
	struct loopback_socket *socket;

	(void)how;
	pthread_mutex_lock(&loopback_lock);
	socket = socket_at(descriptor);
	if (socket && socket->peer >= 0 && loopback_sockets[socket->peer].used)
		loopback_sockets[socket->peer].peer_closed = 1;
	pthread_mutex_unlock(&loopback_lock);
	return socket ? succeed(0) : fail(WSAENOTSOCK);
}

int posix_socket_set_nonblocking(
	int descriptor,
	int nonblocking)
{
	struct loopback_socket *socket;

	pthread_mutex_lock(&loopback_lock);
	socket = socket_at(descriptor);
	if (socket)
		socket->nonblocking = nonblocking;
	pthread_mutex_unlock(&loopback_lock);
	return socket ? succeed(0) : fail(WSAENOTSOCK);
}

int posix_socket_set_nodelay(
	int descriptor)
{
	return socket_at(descriptor) ? succeed(0) : fail(WSAENOTSOCK);
}

int posix_socket_bytes_available(
	int descriptor,
	posix_ulong *count)
{
	struct loopback_socket *socket;

	pthread_mutex_lock(&loopback_lock);
	socket = socket_at(descriptor);
	if (socket)
	{
		*count = socket->type == LOOPBACK_SOCK_DGRAM ?
			(socket->datagram_count ? (posix_ulong)socket->datagrams[socket->datagram_first].size : 0) :
			(posix_ulong)socket->byte_count;
	}
	pthread_mutex_unlock(&loopback_lock);
	return socket ? succeed(0) : fail(WSAENOTSOCK);
}

int posix_socket_setsockopt(
	int descriptor,
	int level,
	int name,
	const void *value,
	int length)
{
	(void)level;
	(void)name;
	(void)value;
	(void)length;
	return socket_at(descriptor) ? succeed(0) : fail(WSAENOTSOCK);
}

int posix_socket_getsockopt(
	int descriptor,
	int level,
	int name,
	void *value,
	int *length)
{
	struct loopback_socket *socket = socket_at(descriptor);
	int answer = 0;

	(void)level;
	if (!socket)
		return fail(WSAENOTSOCK);
	if (!value || !length || *length < (int)sizeof(int))
		return fail(WSAEFAULT);
	switch (name)
	{
	case 0x1001: /* SO_SNDBUF */
	case 0x1002: /* SO_RCVBUF */
		answer = LOOPBACK_STREAM_LIMIT;
		break;
	case 0x1008: /* SO_TYPE */
		answer = socket->type;
		break;
	default: /* SO_ERROR and the rest: none */
		answer = 0;
		break;
	}
	memcpy(value, &answer, sizeof(answer));
	*length = (int)sizeof(answer);
	return succeed(0);
}

int posix_socket_getsockname(
	int descriptor,
	void *address,
	int *address_length)
{
	struct loopback_socket *socket = socket_at(descriptor);

	if (!socket)
		return fail(WSAENOTSOCK);
	write_address(address, address_length, socket->port);
	return succeed(0);
}

int posix_socket_getpeername(
	int descriptor,
	void *address,
	int *address_length)
{
	struct loopback_socket *socket = socket_at(descriptor);

	if (!socket)
		return fail(WSAENOTSOCK);
	if (!socket->connected)
		return fail(WSAENOTCONN);
	write_address(address, address_length, socket->peer_port);
	return succeed(0);
}

/* each list keeps only its ready descriptors; never waits */
static int select_list(
	int *list,
	int *count,
	int which)
{
	int kept = 0;
	int index;

	if (!list || !count)
		return 0;
	for (index = 0; index < *count; index++)
	{
		struct loopback_socket const *socket = socket_at(list[index]);
		int ready = 0;

		if (socket)
		{
			if (which == 0)
				ready = readable(socket);
			else if (which == 1)
				ready = socket->type == LOOPBACK_SOCK_DGRAM || socket->connected;
		}
		if (ready)
			list[kept++] = list[index];
	}
	*count = kept;
	return kept;
}

int posix_socket_select(
	int *read,
	int *read_count,
	int *write,
	int *write_count,
	int *error,
	int *error_count,
	posix_long timeout_seconds,
	posix_long timeout_microseconds,
	int infinite)
{
	int ready;

	(void)timeout_seconds;
	(void)timeout_microseconds;
	(void)infinite;
	pthread_mutex_lock(&loopback_lock);
	ready = select_list(read, read_count, 0) + select_list(write, write_count, 1) +
		select_list(error, error_count, 2);
	pthread_mutex_unlock(&loopback_lock);
	return succeed(ready);
}

posix_ulong posix_local_ipv4_address(
	void)
{
	/* no local network: system link reports no ethernet link */
	return 0;
}

void posix_random_bytes(
	void *buffer,
	posix_ulong size)
{
	unsigned char *bytes = buffer;
	posix_ulong index;

	/* a fixed sequence (xorshift): XNet's keys and nonces never leave this
	machine here, and every launch, and the i686 host, makes the same */
	for (index = 0; index < size; index++)
	{
		loopback_random_state ^= loopback_random_state << 13;
		loopback_random_state ^= loopback_random_state >> 17;
		loopback_random_state ^= loopback_random_state << 5;
		loopback_random_state &= 0xFFFFFFFFul;
		bytes[index] = (unsigned char)(loopback_random_state >> 24);
	}
}

posix_ulong posix_resolve_ipv4(
	const char *host)
{
	(void)host;
	return 0;
}

void wii_loopback_net_report(
	unsigned long counts[4])
{
	memcpy(counts, loopback_counts, sizeof(loopback_counts));
}

/* ---------- p2p.h: internet play is not on the Wii (no address is a peer's) */

void p2p_initialize(
	unsigned long local_address)
{
	(void)local_address;
	wii_unsupported("p2p", "p2p_initialize (internet play)");
}

const unsigned char *p2p_identifier(
	void)
{
	static const unsigned char none[6] = { 0 };

	return none;
}

int p2p_outgoing(
	int stream,
	int socket,
	unsigned long *address,
	unsigned short *port)
{
	(void)stream;
	(void)socket;
	(void)address;
	(void)port;
	return 0;
}

int p2p_incoming(
	int stream,
	unsigned long *address,
	unsigned short *port)
{
	(void)stream;
	(void)address;
	(void)port;
	return 0;
}

int p2p_broadcast_targets(
	unsigned short port,
	unsigned long *addresses,
	unsigned short *ports,
	int maximum_count)
{
	(void)port;
	(void)addresses;
	(void)ports;
	(void)maximum_count;
	return 0;
}

int p2p_send_datagram(
	unsigned short source_port,
	unsigned long address,
	unsigned short port,
	const void *data,
	int size)
{
	(void)source_port;
	(void)address;
	(void)port;
	(void)data;
	(void)size;
	return 0;
}

int p2p_broadcast_datagram(
	unsigned short source_port,
	unsigned short port,
	const void *data,
	int size)
{
	(void)source_port;
	(void)port;
	(void)data;
	(void)size;
	return 0;
}

void p2p_socket_port(
	int socket,
	int stream,
	int listening,
	unsigned short port)
{
	(void)socket;
	(void)stream;
	(void)listening;
	(void)port;
}

void p2p_port_taken(
	int stream,
	unsigned short port)
{
	(void)stream;
	(void)port;
}

void p2p_socket_closed(
	int socket,
	unsigned short datagram_port)
{
	(void)socket;
	(void)datagram_port;
}
