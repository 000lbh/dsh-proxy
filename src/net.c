#define _GNU_SOURCE

#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

void dsp_set_nonblocking(int fd, int enabled)
{
	int flags = fcntl(fd, F_GETFL, 0);

	if (flags < 0)
		return;
	if (enabled)
		flags |= O_NONBLOCK;
	else
		flags &= ~O_NONBLOCK;
	(void)fcntl(fd, F_SETFL, flags);
}

void dsp_set_cloexec(int fd)
{
	int flags = fcntl(fd, F_GETFD, 0);

	if (flags >= 0)
		(void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

int dsp_connect_host(const char *host, const char *port, int timeout_ms,
		     char *error, size_t error_cap)
{
	struct addrinfo hints;
	struct addrinfo *result = NULL;
	struct addrinfo *item;
	int rc;

	if (error != NULL && error_cap > 0)
		error[0] = '\0';
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	rc = getaddrinfo(host, port, &hints, &result);
	if (rc != 0) {
		if (error != NULL)
			snprintf(error, error_cap, "resolve %s:%s: %s", host, port,
				 gai_strerror(rc));
		return -1;
	}

	for (item = result; item != NULL; item = item->ai_next) {
		int fd = socket(item->ai_family,
				item->ai_socktype | SOCK_CLOEXEC,
				item->ai_protocol);
		int one = 1;
		int so_error = 0;
		socklen_t so_len = sizeof(so_error);

		if (fd < 0)
			continue;
		(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		(void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
		dsp_set_nonblocking(fd, 1);
		if (connect(fd, item->ai_addr, item->ai_addrlen) == 0) {
			/* Connected immediately. */
		} else if (errno == EINPROGRESS) {
			struct pollfd pfd;
			int ready;

			pfd.fd = fd;
			pfd.events = POLLOUT;
			pfd.revents = 0;
			do {
				ready = poll(&pfd, 1, timeout_ms);
			} while (ready < 0 && errno == EINTR);
			if (ready <= 0 ||
			    getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error,
				       &so_len) < 0 ||
			    so_error != 0) {
				if (ready == 0)
					so_error = ETIMEDOUT;
				else if (so_error == 0)
					so_error = errno;
				close(fd);
				if (error != NULL)
					snprintf(error, error_cap,
						 "connect %s:%s: %s", host, port,
						 strerror(so_error));
				continue;
			}
		} else {
			int saved = errno;

			close(fd);
			if (error != NULL)
				snprintf(error, error_cap, "connect %s:%s: %s",
					 host, port, strerror(saved));
			continue;
		}
		dsp_set_nonblocking(fd, 0);
		freeaddrinfo(result);
		return fd;
	}

	freeaddrinfo(result);
	if (error != NULL && error[0] == '\0')
		snprintf(error, error_cap, "connect %s:%s failed", host, port);
	return -1;
}

int dsp_listen(const char *host, const char *port, int backlog,
	       char *error, size_t error_cap)
{
	struct addrinfo hints;
	struct addrinfo *result = NULL;
	struct addrinfo *item;
	int rc;
	int last_error = 0;

	if (error != NULL && error_cap > 0)
		error[0] = '\0';
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	rc = getaddrinfo(host == NULL || host[0] == '\0' || strcmp(host, "*") == 0
				 ? NULL
				 : host,
			 port, &hints, &result);
	if (rc != 0) {
		if (error != NULL)
			snprintf(error, error_cap, "resolve listen address: %s",
				 gai_strerror(rc));
		return -1;
	}

	for (item = result; item != NULL; item = item->ai_next) {
		int fd = socket(item->ai_family,
				item->ai_socktype | SOCK_CLOEXEC,
				item->ai_protocol);
		int one = 1;

		if (fd < 0) {
			last_error = errno;
			continue;
		}
		(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		if (item->ai_family == AF_INET6) {
			int zero = 0;

			(void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero,
					 sizeof(zero));
		}
		if (bind(fd, item->ai_addr, item->ai_addrlen) < 0 ||
		    listen(fd, backlog) < 0) {
			last_error = errno;
			close(fd);
			continue;
		}
		dsp_set_nonblocking(fd, 1);
		freeaddrinfo(result);
		return fd;
	}

	freeaddrinfo(result);
	if (error != NULL)
		snprintf(error, error_cap, "bind %s:%s: %s",
			 host == NULL ? "*" : host, port,
			 strerror(last_error != 0 ? last_error : EADDRNOTAVAIL));
	return -1;
}

int dsp_get_peer_name(int fd, char *out, size_t out_cap)
{
	struct sockaddr_storage storage;
	socklen_t len = sizeof(storage);
	char host[NI_MAXHOST];
	char service[NI_MAXSERV];
	int rc;

	if (getpeername(fd, (struct sockaddr *)&storage, &len) < 0)
		return -1;
	rc = getnameinfo((struct sockaddr *)&storage, len, host, sizeof(host),
			 service, sizeof(service),
			 NI_NUMERICHOST | NI_NUMERICSERV);
	if (rc != 0)
		return -1;
	if (strchr(host, ':') != NULL && host[0] != '[')
		snprintf(out, out_cap, "[%s]:%s", host, service);
	else
		snprintf(out, out_cap, "%s:%s", host, service);
	return 0;
}

int dsp_get_local_name(int fd, char *out, size_t out_cap)
{
	struct sockaddr_storage storage;
	socklen_t len = sizeof(storage);
	char host[NI_MAXHOST];
	char service[NI_MAXSERV];
	int rc;

	if (getsockname(fd, (struct sockaddr *)&storage, &len) < 0)
		return -1;
	rc = getnameinfo((struct sockaddr *)&storage, len, host, sizeof(host),
			 service, sizeof(service),
			 NI_NUMERICHOST | NI_NUMERICSERV);
	if (rc != 0)
		return -1;
	if (strchr(host, ':') != NULL && host[0] != '[')
		snprintf(out, out_cap, "[%s]:%s", host, service);
	else
		snprintf(out, out_cap, "%s:%s", host, service);
	return 0;
}
