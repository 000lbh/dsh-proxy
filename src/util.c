#define _GNU_SOURCE

#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

void dsp_buf_init(dsp_buf *buf)
{
	buf->data = NULL;
	buf->len = 0;
	buf->cap = 0;
}

void dsp_buf_free(dsp_buf *buf)
{
	free(buf->data);
	buf->data = NULL;
	buf->len = 0;
	buf->cap = 0;
}

int dsp_buf_reserve(dsp_buf *buf, size_t extra)
{
	size_t need;
	size_t cap;
	char *next;

	if (extra > SIZE_MAX - buf->len - 1)
		return -1;
	need = buf->len + extra + 1;
	if (need <= buf->cap)
		return 0;
	cap = buf->cap == 0 ? 256 : buf->cap;
	while (cap < need) {
		if (cap > SIZE_MAX / 2) {
			cap = need;
			break;
		}
		cap *= 2;
	}
	next = realloc(buf->data, cap);
	if (next == NULL)
		return -1;
	buf->data = next;
	buf->cap = cap;
	return 0;
}

int dsp_buf_append(dsp_buf *buf, const void *data, size_t len)
{
	if (dsp_buf_reserve(buf, len) < 0)
		return -1;
	if (len > 0)
		memcpy(buf->data + buf->len, data, len);
	buf->len += len;
	buf->data[buf->len] = '\0';
	return 0;
}

int dsp_buf_append_str(dsp_buf *buf, const char *text)
{
	return dsp_buf_append(buf, text, strlen(text));
}

int dsp_buf_printf(dsp_buf *buf, const char *fmt, ...)
{
	va_list ap;
	va_list copy;
	int needed;
	size_t old_len;

	old_len = buf->len;
	va_start(ap, fmt);
	va_copy(copy, ap);
	needed = vsnprintf(NULL, 0, fmt, copy);
	va_end(copy);
	if (needed < 0) {
		va_end(ap);
		return -1;
	}
	if (dsp_buf_reserve(buf, (size_t)needed) < 0) {
		va_end(ap);
		return -1;
	}
	vsnprintf(buf->data + old_len, buf->cap - old_len, fmt, ap);
	va_end(ap);
	buf->len = old_len + (size_t)needed;
	return 0;
}

char *dsp_buf_steal(dsp_buf *buf, size_t *len_out)
{
	char *data = buf->data;

	if (data == NULL) {
		data = calloc(1, 1);
		if (data == NULL)
			return NULL;
	}
	if (len_out != NULL)
		*len_out = buf->len;
	buf->data = NULL;
	buf->len = 0;
	buf->cap = 0;
	return data;
}

char *dsp_strdup(const char *text)
{
	size_t len;
	char *copy;

	if (text == NULL)
		return NULL;
	len = strlen(text);
	copy = malloc(len + 1);
	if (copy == NULL)
		return NULL;
	memcpy(copy, text, len + 1);
	return copy;
}

char *dsp_strndup(const char *text, size_t len)
{
	char *copy = malloc(len + 1);

	if (copy == NULL)
		return NULL;
	memcpy(copy, text, len);
	copy[len] = '\0';
	return copy;
}

void dsp_trim(char *text)
{
	char *start = text;
	char *end;

	while (*start != '\0' && isspace((unsigned char)*start))
		start++;
	if (start != text)
		memmove(text, start, strlen(start) + 1);
	end = text + strlen(text);
	while (end > text && isspace((unsigned char)end[-1]))
		end--;
	*end = '\0';
}

int dsp_strcaseeq(const char *a, const char *b)
{
	size_t len_a;
	size_t len_b;

	if (a == NULL || b == NULL)
		return a == b;
	len_a = strlen(a);
	len_b = strlen(b);
	return len_a == len_b && dsp_strncaseeq(a, b, len_a);
}

int dsp_strncaseeq(const char *a, const char *b, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		unsigned char ca = (unsigned char)a[i];
		unsigned char cb = (unsigned char)b[i];

		if (tolower(ca) != tolower(cb))
			return 0;
		if (ca == '\0' || cb == '\0')
			break;
	}
	return 1;
}

int dsp_has_prefix_ci(const char *text, const char *prefix)
{
	return dsp_strncaseeq(text, prefix, strlen(prefix));
}

int dsp_has_token_ci(const char *value, const char *token)
{
	const char *start = value;
	size_t token_len = strlen(token);

	while (*start != '\0') {
		const char *end;
		const char *p;

		while (*start == ',' || isspace((unsigned char)*start))
			start++;
		end = start;
		while (*end != '\0' && *end != ',')
			end++;
		p = end;
		while (p > start && isspace((unsigned char)p[-1]))
			p--;
		if ((size_t)(p - start) == token_len &&
		    dsp_strncaseeq(start, token, token_len))
			return 1;
		start = end;
		if (*start == ',')
			start++;
	}
	return 0;
}

int dsp_parse_hostport(const char *text, char *host, size_t host_cap,
		       char *port, size_t port_cap)
{
	const char *start;
	const char *colon;
	size_t host_len;
	size_t port_len;

	if (text == NULL || host == NULL || port == NULL)
		return -1;
	while (isspace((unsigned char)*text))
		text++;
	start = text;
	if (*start == '[') {
		const char *close = strchr(start + 1, ']');

		if (close == NULL)
			return -1;
		host_len = (size_t)(close - (start + 1));
		if (host_len == 0 || host_len + 1 > host_cap)
			return -1;
		memcpy(host, start + 1, host_len);
		host[host_len] = '\0';
		if (close[1] == '\0') {
			port[0] = '\0';
			return 0;
		}
		if (close[1] != ':')
			return -1;
		port_len = strlen(close + 2);
		if (port_len + 1 > port_cap)
			return -1;
		memcpy(port, close + 2, port_len + 1);
	} else {
		colon = strrchr(start, ':');
		if (colon == NULL) {
			host_len = strlen(start);
			if (host_len + 1 > host_cap)
				return -1;
			memcpy(host, start, host_len + 1);
			port[0] = '\0';
		} else {
			const char *first = strchr(start, ':');

			if (first != colon) {
				/* Bare IPv6 literal without a port. */
				host_len = strlen(start);
				if (host_len + 1 > host_cap)
					return -1;
				memcpy(host, start, host_len + 1);
				port[0] = '\0';
			} else {
				host_len = (size_t)(colon - start);
				if (host_len + 1 > host_cap)
					return -1;
				memcpy(host, start, host_len);
				host[host_len] = '\0';
				port_len = strlen(colon + 1);
				if (port_len + 1 > port_cap)
					return -1;
				memcpy(port, colon + 1, port_len + 1);
			}
		}
	}
	{
		char *host_end = host + strlen(host);
		char *port_end = port + strlen(port);

		while (host_end > host && isspace((unsigned char)host_end[-1]))
			*--host_end = '\0';
		while (port_end > port && isspace((unsigned char)port_end[-1]))
			*--port_end = '\0';
	}
	if (port[0] != '\0') {
		char *end = NULL;
		long value = strtol(port, &end, 10);

		if (end == port || *end != '\0' || value < 1 || value > 65535)
			return -1;
	}
	return 0;
}

int dsp_format_authority(const char *host, const char *port,
			 const char *scheme, char *out, size_t out_cap)
{
	int needs_brackets;
	int default_port;
	int written;

	if (host == NULL || out == NULL || out_cap == 0)
		return -1;
	needs_brackets = strchr(host, ':') != NULL && host[0] != '[';
	if (dsp_strcaseeq(scheme, "https"))
		default_port = 443;
	else
		default_port = 80;
	if (port == NULL || port[0] == '\0' ||
	    atoi(port) == default_port)
		written = snprintf(out, out_cap, needs_brackets ? "[%s]" : "%s",
				   host);
	else
		written = snprintf(out, out_cap,
				   needs_brackets ? "[%s]:%s" : "%s:%s",
				   host, port);
	if (written < 0 || (size_t)written >= out_cap)
		return -1;
	return 0;
}

int dsp_parse_http_url(const char *url, char *scheme, size_t scheme_cap,
		       char *host, size_t host_cap, char *port, size_t port_cap,
		       char *path, size_t path_cap)
{
	const char *sep;
	const char *authority;
	const char *authority_end;
	const char *p;
	char authority_text[512];
	char default_port[8];
	size_t scheme_len;
	size_t len;

	if (url == NULL)
		return -1;
	sep = strstr(url, "://");
	if (sep == NULL)
		return -1;
	scheme_len = (size_t)(sep - url);
	if ((scheme_len != 4 || !dsp_strncaseeq(url, "http", 4)) &&
	    (scheme_len != 5 || !dsp_strncaseeq(url, "https", 5)))
		return -1;
	if (scheme_len + 1 > scheme_cap)
		return -1;
	memcpy(scheme, url, scheme_len);
	scheme[scheme_len] = '\0';
	if (dsp_strcaseeq(scheme, "https"))
		strcpy(default_port, "443");
	else
		strcpy(default_port, "80");

	authority = sep + 3;
	authority_end = authority;
	while (*authority_end != '\0' && *authority_end != '/' &&
	       *authority_end != '?' && *authority_end != '#')
		authority_end++;
	len = (size_t)(authority_end - authority);
	if (len == 0 || len >= sizeof(authority_text))
		return -1;
	memcpy(authority_text, authority, len);
	authority_text[len] = '\0';
	if (strchr(authority_text, '@') != NULL)
		return -1;
	if (dsp_parse_hostport(authority_text, host, host_cap, port, port_cap) < 0)
		return -1;
	if (host[0] == '\0')
		return -1;
	if (port[0] == '\0') {
		if (strlen(default_port) + 1 > port_cap)
			return -1;
		strcpy(port, default_port);
	}

	p = authority_end;
	if (*p == '/') {
		len = strlen(p);
		if (len + 1 > path_cap)
			return -1;
		p = authority_end;
		len = 0;
		while (*p != '\0' && *p != '#') {
			if (len + 1 >= path_cap)
				return -1;
			path[len++] = *p++;
		}
		path[len] = '\0';
	} else {
		len = 0;
		if (*p == '?') {
			if (len + 1 >= path_cap)
				return -1;
			path[len++] = '/';
			while (*p != '\0' && *p != '#') {
				if (len + 1 >= path_cap)
					return -1;
				path[len++] = *p++;
			}
		} else {
			if (len + 1 >= path_cap)
				return -1;
			path[len++] = '/';
		}
		path[len] = '\0';
	}
	return 0;
}

int dsp_token_from_url(const char *url, char *token, size_t token_cap)
{
	const char *p;

	if (url == NULL || token == NULL || token_cap == 0)
		return -1;
	p = strchr(url, '?');
	if (p == NULL)
		return -1;
	p++;
	while (*p != '\0') {
		const char *eq;
		const char *end;

		if (dsp_strncaseeq(p, "token=", 6)) {
			p += 6;
			end = p;
			while (*end != '\0' && *end != '&' && *end != '#' &&
			       !isspace((unsigned char)*end) && *end != '"' &&
			       *end != '\'')
				end++;
			if (end == p || (size_t)(end - p) + 1 > token_cap)
				return -1;
			memcpy(token, p, (size_t)(end - p));
			token[end - p] = '\0';
			return 0;
		}
		eq = strchr(p, '&');
		if (eq == NULL)
			break;
		p = eq + 1;
	}
	return -1;
}

int dsp_wait_fd(int fd, short events, int timeout_ms)
{
	struct pollfd pfd;
	int rc;

	pfd.fd = fd;
	pfd.events = events;
	pfd.revents = 0;
	do {
		rc = poll(&pfd, 1, timeout_ms);
	} while (rc < 0 && errno == EINTR);
	if (rc == 0)
		return 0;
	if (rc < 0)
		return -1;
	if (pfd.revents & (POLLERR | POLLNVAL))
		return -1;
	if (pfd.revents & (events | POLLHUP))
		return 1;
	return 0;
}

int dsp_read_full(int fd, void *buffer, size_t len, int timeout_ms,
		  const char **error)
{
	size_t done = 0;
	char *p = buffer;

	while (done < len) {
		ssize_t n;
		int ready = dsp_wait_fd(fd, POLLIN, timeout_ms);

		if (ready == 0) {
			if (error != NULL)
				*error = "timed out while reading";
			return -1;
		}
		if (ready < 0) {
			if (error != NULL)
				*error = strerror(errno);
			return -1;
		}
		n = recv(fd, p + done, len - done, 0);
		if (n > 0) {
			done += (size_t)n;
			continue;
		}
		if (n == 0) {
			if (error != NULL)
				*error = "connection closed while reading";
			return -1;
		}
		if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
			continue;
		if (error != NULL)
			*error = strerror(errno);
		return -1;
	}
	return 0;
}

int dsp_write_full(int fd, const void *buffer, size_t len, int timeout_ms,
		   const char **error)
{
	size_t done = 0;
	const char *p = buffer;

	while (done < len) {
		ssize_t n;
		int ready = dsp_wait_fd(fd, POLLOUT, timeout_ms);

		if (ready == 0) {
			if (error != NULL)
				*error = "timed out while writing";
			return -1;
		}
		if (ready < 0) {
			if (error != NULL)
				*error = strerror(errno);
			return -1;
		}
		n = send(fd, p + done, len - done, MSG_NOSIGNAL);
		if (n > 0) {
			done += (size_t)n;
			continue;
		}
		if (n == 0)
			continue;
		if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
			continue;
		if (error != NULL)
			*error = strerror(errno);
		return -1;
	}
	return 0;
}

uint64_t dsp_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

void dsp_strip_ansi(char *text)
{
	char *src = text;
	char *dst = text;

	while (*src != '\0') {
		if ((unsigned char)*src == 0x1b) {
			src++;
			if (*src == '[') {
				src++;
				while (*src != '\0' &&
				       !((unsigned char)*src >= 0x40 &&
					 (unsigned char)*src <= 0x7e))
					src++;
				if (*src != '\0')
					src++;
				continue;
			}
			if (*src == ']') {
				src++;
				while (*src != '\0' && (unsigned char)*src != 0x07) {
					if ((unsigned char)*src == 0x1b &&
					    src[1] == '\\') {
						src += 2;
						break;
					}
					src++;
				}
				if (*src == 0x07)
					src++;
				continue;
			}
			if (*src != '\0')
				src++;
			continue;
		}
		if (*src == '\r') {
			src++;
			continue;
		}
		*dst++ = *src++;
	}
	*dst = '\0';
}

void dsp_redact_token(const char *line, char *out, size_t out_cap)
{
	const char *p;
	const char *value_end;
	size_t prefix;
	size_t suffix;

	if (out_cap == 0)
		return;
	p = strstr(line, "token=");
	if (p == NULL) {
		snprintf(out, out_cap, "%s", line);
		return;
	}
	value_end = p + 6;
	while (*value_end != '\0' && *value_end != '&' && *value_end != '#' &&
	       !isspace((unsigned char)*value_end) && *value_end != '"' &&
	       *value_end != '\'')
		value_end++;
	prefix = (size_t)(p - line) + 6;
	if (prefix >= out_cap)
		prefix = out_cap - 1;
	memcpy(out, line, prefix);
	out[prefix] = '\0';
	suffix = strlen(value_end);
	if (prefix + 3 + suffix + 1 <= out_cap) {
		memcpy(out + prefix, "***", 3);
		memcpy(out + prefix + 3, value_end, suffix + 1);
	} else {
		out[out_cap - 1] = '\0';
	}
}
