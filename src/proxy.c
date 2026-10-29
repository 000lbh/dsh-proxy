#define _GNU_SOURCE

#include "proxy.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "net.h"
#include "util.h"

#define MAX_HEADERS 128
#define MAX_HEADER_BYTES (64 * 1024)
#define MAX_CLIENTS 256
#define READER_CAP (64 * 1024)
#define BODY_BUFFER (32 * 1024)

typedef struct {
	int fd;
	char buffer[READER_CAP];
	size_t start;
	size_t end;
	int timeout_ms;
} reader;

typedef struct {
	const char *name;
	size_t name_len;
	const char *value;
	size_t value_len;
} http_header;

typedef struct {
	char method[16];
	char *target;
	char version[16];
	http_header headers[MAX_HEADERS];
	size_t header_count;
} http_request;

typedef struct {
	int chunked;
	int has_length;
	uint64_t length;
	int expect_continue;
	int upgrade;
	char upgrade_value[256];
} body_info;

typedef struct {
	int from;
	int to;
} relay_direction;

struct dsp_proxy {
	const dsp_config *config;
	dsp_upstream *upstream;
	int listen_fd;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int active;
	int stopping;
};

typedef struct {
	dsp_proxy *proxy;
	int fd;
	char peer[128];
} client_job;

static void reader_init(reader *r, int fd, int timeout_ms)
{
	r->fd = fd;
	r->start = 0;
	r->end = 0;
	r->timeout_ms = timeout_ms;
}

static int reader_fill(reader *r)
{
	if (r->start > 0) {
		memmove(r->buffer, r->buffer + r->start, r->end - r->start);
		r->end -= r->start;
		r->start = 0;
	}
	if (r->end >= sizeof(r->buffer))
		return -2;
	for (;;) {
		ssize_t n;
		int ready = dsp_wait_fd(r->fd, POLLIN, r->timeout_ms);

		if (ready == 0)
			return -3;
		if (ready < 0)
			return -1;
		n = recv(r->fd, r->buffer + r->end,
			 sizeof(r->buffer) - r->end, 0);
		if (n > 0) {
			r->end += (size_t)n;
			return 1;
		}
		if (n == 0)
			return 0;
		if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
			continue;
		return -1;
	}
}

static int reader_read(reader *r, void *buffer, size_t max)
{
	size_t available;

	if (r->start == r->end) {
		int rc = reader_fill(r);

		if (rc <= 0)
			return rc;
	}
	available = r->end - r->start;
	if (available > max)
		available = max;
	memcpy(buffer, r->buffer + r->start, available);
	r->start += available;
	return (int)available;
}

static int reader_read_exact(reader *r, void *buffer, size_t len)
{
	size_t done = 0;

	while (done < len) {
		int n = reader_read(r, (char *)buffer + done, len - done);

		if (n <= 0)
			return n;
		done += (size_t)n;
	}
	return 1;
}

static int reader_read_line(reader *r, char *line, size_t cap)
{
	for (;;) {
		char *newline;
		size_t available = r->end - r->start;
		size_t line_len;

		newline = memchr(r->buffer + r->start, '\n', available);
		if (newline != NULL) {
			line_len = (size_t)(newline - (r->buffer + r->start)) + 1;
			if (line_len + 1 > cap)
				return -2;
			memcpy(line, r->buffer + r->start, line_len);
			line[line_len] = '\0';
			r->start += line_len;
			return (int)line_len;
		}
		if (available + 1 >= cap)
			return -2;
		{
			int rc = reader_fill(r);

			if (rc <= 0)
				return rc;
		}
	}
}

static int read_headers(reader *r, dsp_buf *out, size_t max)
{
	size_t searched = 0;

	for (;;) {
		size_t len = r->end - r->start;
		size_t i;
		char *base = r->buffer + r->start;

		for (i = searched; i + 3 < len; i++) {
			if (base[i] == '\r' && base[i + 1] == '\n' &&
			    base[i + 2] == '\r' && base[i + 3] == '\n') {
				size_t header_len = i + 4;

				if (dsp_buf_append(out, base, header_len) < 0)
					return -2;
				r->start += header_len;
				return 0;
			}
		}
		if (len >= max)
			return -2;
		searched = len > 3 ? len - 3 : 0;
		{
			int rc = reader_fill(r);

			if (rc == 0)
				return -1;
			if (rc < 0)
				return rc;
		}
	}
}

static int parse_request(dsp_buf *header, http_request *request)
{
	char *text = header->data;
	char *line_end;
	char *method;
	char *target;
	char *version;
	char *next;

	memset(request, 0, sizeof(*request));
	line_end = strstr(text, "\r\n");
	if (line_end == NULL)
		return -1;
	*line_end = '\0';
	method = text;
	target = strchr(method, ' ');
	if (target == NULL)
		return -1;
	*target++ = '\0';
	version = strchr(target, ' ');
	if (version == NULL)
		return -1;
	*version++ = '\0';
	if (method[0] == '\0' || target[0] == '\0' ||
	    !dsp_has_prefix_ci(version, "HTTP/1.") ||
	    strpbrk(version, " \t") != NULL)
		return -1;
	if (strlen(method) + 1 > sizeof(request->method))
		return -1;
	strcpy(request->method, method);
	request->target = target;
	if (strlen(version) + 1 > sizeof(request->version))
		return -1;
	strcpy(request->version, version);

	next = line_end + 2;
	while (next < text + header->len) {
		char *end = strstr(next, "\r\n");
		char *colon;

		if (end == NULL)
			return -1;
		if (end == next)
			break;
		*end = '\0';
		colon = strchr(next, ':');
		if (colon == NULL)
			return -1;
		*colon = '\0';
		if (request->header_count >= MAX_HEADERS)
			return -2;
		request->headers[request->header_count].name = next;
		request->headers[request->header_count].value = colon + 1;
		dsp_trim((char *)request->headers[request->header_count].name);
		dsp_trim((char *)request->headers[request->header_count].value);
		if (request->headers[request->header_count].name[0] == '\0')
			return -1;
		request->headers[request->header_count].name_len =
			strlen(request->headers[request->header_count].name);
		request->headers[request->header_count].value_len =
			strlen(request->headers[request->header_count].value);
		request->header_count++;
		next = end + 2;
	}
	return 0;
}

static const http_header *find_header(const http_request *request,
				      const char *name)
{
	size_t i;

	for (i = 0; i < request->header_count; i++) {
		if (dsp_strcaseeq(request->headers[i].name, name))
			return &request->headers[i];
	}
	return NULL;
}

static int header_value_has_token(const http_request *request,
				  const char *name, const char *token)
{
	size_t i;

	for (i = 0; i < request->header_count; i++) {
		if (dsp_strcaseeq(request->headers[i].name, name) &&
		    dsp_has_token_ci(request->headers[i].value, token))
			return 1;
	}
	return 0;
}

static int transfer_encoding_is_chunked(const char *value)
{
	const char *p = value;
	int found = 0;

	while (*p != '\0') {
		const char *start;
		const char *end;

		while (*p == ',' || isspace((unsigned char)*p))
			p++;
		if (*p == '\0')
			break;
		start = p;
		while (*p != '\0' && *p != ',')
			p++;
		end = p;
		while (end > start && isspace((unsigned char)end[-1]))
			end--;
		if ((size_t)(end - start) != 7 ||
		    !dsp_strncaseeq(start, "chunked", 7))
			return 0;
		found = 1;
	}
	return found;
}

static int parse_content_length(const char *value, uint64_t *out)
{
	uint64_t result = 0;
	const char *p = value;

	if (*p == '\0')
		return -1;
	while (*p != '\0') {
		if (!isdigit((unsigned char)*p))
			return -1;
		if (result > (UINT64_MAX - 9) / 10)
			return -1;
		result = result * 10 + (uint64_t)(*p - '0');
		p++;
	}
	*out = result;
	return 0;
}

static int analyze_body(const http_request *request, body_info *body)
{
	size_t i;
	int content_length_seen = 0;

	memset(body, 0, sizeof(*body));
	for (i = 0; i < request->header_count; i++) {
		const http_header *header = &request->headers[i];

		if (dsp_strcaseeq(header->name, "transfer-encoding")) {
			if (!transfer_encoding_is_chunked(header->value))
				return -1;
			body->chunked = 1;
		} else if (dsp_strcaseeq(header->name, "content-length")) {
			uint64_t length = 0;

			if (parse_content_length(header->value, &length) < 0)
				return -1;
			if (content_length_seen && body->length != length)
				return -1;
			content_length_seen = 1;
			body->has_length = 1;
			body->length = length;
		} else if (dsp_strcaseeq(header->name, "expect")) {
			if (strcasestr(header->value, "100-continue") != NULL)
				body->expect_continue = 1;
		} else if (dsp_strcaseeq(header->name, "upgrade")) {
			if (header->value_len + 1 > sizeof(body->upgrade_value))
				return -1;
			strcpy(body->upgrade_value, header->value);
		}
	}
	if (body->chunked && body->has_length)
		return -1;
	body->upgrade = header_value_has_token(request, "connection", "upgrade") &&
			body->upgrade_value[0] != '\0';
	if (body->upgrade && (body->chunked ||
			      (body->has_length && body->length > 0)))
		return -1;
	return 0;
}

static int append_char_checked(dsp_buf *out, char c)
{
	if ((unsigned char)c <= 0x20 || (unsigned char)c == 0x7f || c == '#')
		return -1;
	return dsp_buf_append(out, &c, 1);
}

static int sanitize_target(const char *input, char *out, size_t out_cap)
{
	char parsed_path[4096];
	const char *src = input;
	const char *path_end;
	const char *query = NULL;
	const char *fragment;
	dsp_buf result;
	size_t i;

	if (strcmp(input, "*") == 0) {
		if (out_cap < 2)
			return -1;
		strcpy(out, "*");
		return 0;
	}
	if (dsp_has_prefix_ci(input, "http://") ||
	    dsp_has_prefix_ci(input, "https://")) {
		char scheme[8];
		char host[256];
		char port[16];

		if (dsp_parse_http_url(input, scheme, sizeof(scheme), host,
				       sizeof(host), port, sizeof(port),
				       parsed_path, sizeof(parsed_path)) < 0)
			return -1;
		src = parsed_path;
	}
	if (src[0] != '/')
		return -1;

	fragment = strchr(src, '#');
	query = strchr(src, '?');
	if (query != NULL && fragment != NULL && query > fragment)
		query = NULL;
	path_end = query != NULL ? query : (fragment != NULL ? fragment
							    : src + strlen(src));
	dsp_buf_init(&result);
	if (path_end == src) {
		if (dsp_buf_append_str(&result, "/") < 0)
			goto fail;
	} else {
		for (i = 0; src + i < path_end; i++) {
			if (append_char_checked(&result, src[i]) < 0)
				goto fail;
		}
	}
	if (query != NULL) {
		const char *end = fragment != NULL && fragment > query
					 ? fragment
					 : src + strlen(src);
		const char *p = query + 1;
		int first = 1;

		while (p < end) {
			const char *pair_end = memchr(p, '&', (size_t)(end - p));
			const char *key_end;
			int keep = 1;

			if (pair_end == NULL)
				pair_end = end;
			key_end = memchr(p, '=', (size_t)(pair_end - p));
			if (key_end == NULL)
				key_end = pair_end;
			if ((size_t)(key_end - p) == 5 &&
			    strncmp(p, "token", 5) == 0)
				keep = 0;
			if (keep) {
				if (first) {
					if (dsp_buf_append_str(&result, "?") < 0)
						goto fail;
					first = 0;
				} else if (dsp_buf_append_str(&result, "&") < 0) {
					goto fail;
				}
				for (i = 0; p + i < pair_end; i++) {
					if (append_char_checked(&result,
								p[i]) < 0)
						goto fail;
				}
			}
			p = pair_end;
			if (p < end)
				p++;
		}
	}
	if (result.len + 1 > out_cap)
		goto fail;
	memcpy(out, result.data != NULL ? result.data : "", result.len);
	out[result.len] = '\0';
	dsp_buf_free(&result);
	return 0;

fail:
	dsp_buf_free(&result);
	return -1;
}

static int skip_forwarded_header(const char *name)
{
	static const char *skip[] = {
		"host",
		"connection",
		"keep-alive",
		"proxy-connection",
		"te",
		"trailer",
		"cookie",
		"origin",
		"referer",
		"expect",
		"transfer-encoding",
		"content-length",
		"upgrade",
		"sec-fetch-site",
		"x-forwarded-for",
		"x-forwarded-proto",
		"x-forwarded-host",
	};
	size_t i;

	for (i = 0; i < sizeof(skip) / sizeof(skip[0]); i++) {
		if (dsp_strcaseeq(name, skip[i]))
			return 1;
	}
	return 0;
}

static int append_referer(dsp_buf *out, const char *value,
			  const char *authority)
{
	char scheme[8];
	char host[256];
	char port[16];
	char path[2048];

	if (dsp_parse_http_url(value, scheme, sizeof(scheme), host,
			       sizeof(host), port, sizeof(port), path,
			       sizeof(path)) < 0)
		return 0;
	return dsp_buf_printf(out, "Referer: http://%s%s\r\n", authority, path);
}

static int build_request(const http_request *request, const body_info *body,
			 const dsp_upstream_view *view, const char *target,
			 const char *client_host, const char *peer_ip,
			 dsp_buf *out)
{
	size_t i;
	int expects_body;

	if (view->cookie[0] == '\0')
		return -1;
	if (dsp_buf_printf(out, "%s %s HTTP/1.1\r\n", request->method,
			   target) < 0)
		return -1;
	for (i = 0; i < request->header_count; i++) {
		const http_header *header = &request->headers[i];

		if (skip_forwarded_header(header->name))
			continue;
		if (dsp_buf_printf(out, "%s: %s\r\n", header->name,
				   header->value) < 0)
			return -1;
	}
	if (dsp_buf_printf(out, "Host: %s\r\nCookie: %s\r\n", view->authority,
			   view->cookie) < 0)
		return -1;
	if (body->upgrade) {
		if (dsp_buf_append_str(out, "Connection: Upgrade\r\n") < 0 ||
		    dsp_buf_printf(out, "Upgrade: %s\r\n",
				   body->upgrade_value) < 0)
			return -1;
	} else if (dsp_buf_append_str(out, "Connection: close\r\n") < 0) {
		return -1;
	}

	{
		const http_header *origin = find_header(request, "origin");
		const http_header *referer = find_header(request, "referer");
		const http_header *fetch_site = find_header(request, "sec-fetch-site");

		if (origin != NULL &&
		    dsp_buf_printf(out, "Origin: http://%s\r\n",
				   view->authority) < 0)
			return -1;
		if (referer != NULL &&
		    append_referer(out, referer->value, view->authority) < 0)
			return -1;
		if (fetch_site != NULL &&
		    dsp_buf_append_str(out, "Sec-Fetch-Site: same-origin\r\n") < 0)
			return -1;
	}
	if (dsp_buf_printf(out, "X-Forwarded-For: %s\r\n"
				"X-Forwarded-Proto: http\r\n",
			   peer_ip) < 0)
		return -1;
	if (client_host[0] != '\0' &&
	    dsp_buf_printf(out, "X-Forwarded-Host: %s\r\n", client_host) < 0)
		return -1;

	expects_body = strcmp(request->method, "POST") == 0 ||
		       strcmp(request->method, "PUT") == 0 ||
		       strcmp(request->method, "PATCH") == 0;
	if (body->chunked) {
		if (dsp_buf_append_str(out, "Transfer-Encoding: chunked\r\n") < 0)
			return -1;
	} else if (body->has_length) {
		if (dsp_buf_printf(out, "Content-Length: %llu\r\n",
				   (unsigned long long)body->length) < 0)
			return -1;
	} else if (expects_body) {
		if (dsp_buf_append_str(out, "Content-Length: 0\r\n") < 0)
			return -1;
	}
	if (dsp_buf_append_str(out, "\r\n") < 0)
		return -1;
	return 0;
}

static int append_location(dsp_buf *out, const char *value,
			   const char *upstream_authority,
			   const char *client_host)
{
	char scheme[8];
	char host[256];
	char port[16];
	char path[2048];
	char parsed_authority[300];

	if (value[0] == '/')
		return dsp_buf_printf(out, "Location: %s\r\n", value);
	if (dsp_parse_http_url(value, scheme, sizeof(scheme), host,
			       sizeof(host), port, sizeof(port), path,
			       sizeof(path)) == 0 &&
	    dsp_format_authority(host, port, scheme, parsed_authority,
				 sizeof(parsed_authority)) == 0 &&
	    strcmp(parsed_authority, upstream_authority) == 0 &&
	    client_host[0] != '\0')
		return dsp_buf_printf(out, "Location: http://%s%s\r\n",
				      client_host, path);
	return dsp_buf_printf(out, "Location: %s\r\n", value);
}

static int build_response(const char *raw, int upgrade,
			  const char *upstream_authority,
			  const char *client_host, dsp_buf *out)
{
	const char *p = raw;
	const char *end = raw + strlen(raw);
	const char *line_end = strstr(p, "\r\n");

	if (line_end == NULL)
		return -1;
	if (dsp_buf_append(out, p, (size_t)(line_end - p) + 2) < 0)
		return -1;
	p = line_end + 2;
	while (p < end) {
		const char *colon;
		size_t name_len;

		line_end = strstr(p, "\r\n");
		if (line_end == NULL)
			return -1;
		if (line_end == p)
			break;
		colon = memchr(p, ':', (size_t)(line_end - p));
		name_len = colon == NULL ? (size_t)(line_end - p)
					 : (size_t)(colon - p);
		if (!upgrade &&
		    ((name_len == 10 && dsp_strncaseeq(p, "connection", 10)) ||
		     (name_len == 10 && dsp_strncaseeq(p, "keep-alive", 10)) ||
		     (name_len == 16 &&
		      dsp_strncaseeq(p, "proxy-connection", 16)))) {
			p = line_end + 2;
			continue;
		}
		if (upgrade && name_len == 16 &&
		    dsp_strncaseeq(p, "proxy-connection", 16)) {
			p = line_end + 2;
			continue;
		}
		if (name_len == 8 && dsp_strncaseeq(p, "location", 8) &&
		    colon != NULL) {
			const char *value = colon + 1;
			char *copy;
			size_t value_len;

			while (value < line_end &&
			       (*value == ' ' || *value == '\t'))
				value++;
			value_len = (size_t)(line_end - value);
			copy = dsp_strndup(value, value_len);
			if (copy == NULL)
				return -1;
			if (append_location(out, copy, upstream_authority,
					    client_host) < 0) {
				free(copy);
				return -1;
			}
			free(copy);
			p = line_end + 2;
			continue;
		}
		if (dsp_buf_append(out, p, (size_t)(line_end - p) + 2) < 0)
			return -1;
		p = line_end + 2;
	}
	if (!upgrade &&
	    dsp_buf_append_str(out, "Connection: close\r\n") < 0)
		return -1;
	if (dsp_buf_append_str(out, "\r\n") < 0)
		return -1;
	return 0;
}

static int parse_status(const char *headers)
{
	const char *space;
	int status;

	if (!dsp_has_prefix_ci(headers, "HTTP/1."))
		return -1;
	space = strchr(headers, ' ');
	if (space == NULL)
		return -1;
	status = atoi(space + 1);
	if (status < 100 || status > 599)
		return -1;
	return status;
}

static void send_simple(int fd, int status, const char *reason,
			const char *extra, const char *body)
{
	dsp_buf response;
	const char *error = NULL;

	dsp_buf_init(&response);
	if (dsp_buf_printf(&response,
			   "HTTP/1.1 %d %s\r\n"
			   "Connection: close\r\n"
			   "Content-Type: text/plain; charset=utf-8\r\n"
			   "%s"
			   "Content-Length: %zu\r\n"
			   "\r\n"
			   "%s",
			   status, reason, extra, body == NULL ? 0 : strlen(body),
			   body == NULL ? "" : body) < 0) {
		dsp_buf_free(&response);
		return;
	}
	(void)dsp_write_full(fd, response.data, response.len, 5000, &error);
	dsp_buf_free(&response);
}

static int forward_body(reader *r, int upstream_fd, const body_info *body)
{
	const char *error = NULL;

	r->timeout_ms = 300000;
	if (body->has_length) {
		uint64_t left = body->length;
		char buffer[BODY_BUFFER];

		while (left > 0) {
			size_t want = left > sizeof(buffer) ? sizeof(buffer)
							    : (size_t)left;
			int n = reader_read(r, buffer, want);

			if (n <= 0)
				return -1;
			if (dsp_write_full(upstream_fd, buffer, (size_t)n,
					   300000, &error) < 0)
				return -1;
			left -= (uint64_t)n;
		}
		return 0;
	}
	if (body->chunked) {
		for (;;) {
			char line[8192];
			char *end = NULL;
			unsigned long long size;
			uint64_t left;
			int n;

			n = reader_read_line(r, line, sizeof(line));
			if (n <= 0)
				return -1;
			if (dsp_write_full(upstream_fd, line, (size_t)n,
					   300000, &error) < 0)
				return -1;
			size = strtoull(line, &end, 16);
			if (end == line)
				return -1;
			if (size == 0) {
				for (;;) {
					char trailer[8192];
					int m = reader_read_line(r, trailer,
								 sizeof(trailer));

					if (m <= 0)
						return -1;
					if (dsp_write_full(upstream_fd, trailer,
							   (size_t)m, 300000,
							   &error) < 0)
						return -1;
					if ((m == 2 && trailer[0] == '\r' &&
					     trailer[1] == '\n') ||
					    (m == 1 && trailer[0] == '\n'))
						break;
				}
				break;
			}
			left = size;
			while (left > 0) {
				char buffer[BODY_BUFFER];
				size_t want = left > sizeof(buffer)
						      ? sizeof(buffer)
						      : (size_t)left;

				n = reader_read(r, buffer, want);
				if (n <= 0)
					return -1;
				if (dsp_write_full(upstream_fd, buffer,
						   (size_t)n, 300000, &error) < 0)
					return -1;
				left -= (uint64_t)n;
			}
			{
				char crlf[2];
				int m = reader_read_exact(r, crlf, 2);

				if (m <= 0 ||
				    dsp_write_full(upstream_fd, crlf, 2, 300000,
						   &error) < 0)
					return -1;
			}
		}
	}
	return 0;
}

static int relay_response(reader *r, int client_fd, int timeout_ms)
{
	char buffer[BODY_BUFFER];
	const char *error = NULL;

	r->timeout_ms = timeout_ms;
	for (;;) {
		int n = reader_read(r, buffer, sizeof(buffer));

		if (n == 0)
			return 0;
		if (n < 0)
			return -1;
		if (dsp_write_full(client_fd, buffer, (size_t)n, timeout_ms,
				   &error) < 0)
			return -1;
	}
}

static void relay_loop(relay_direction *direction)
{
	char buffer[BODY_BUFFER];
	const char *error = NULL;

	for (;;) {
		ssize_t n = recv(direction->from, buffer, sizeof(buffer), 0);

		if (n > 0) {
			if (dsp_write_full(direction->to, buffer, (size_t)n, -1,
					   &error) < 0)
				break;
			continue;
		}
		if (n == 0)
			break;
		if (errno == EINTR)
			continue;
		break;
	}
	shutdown(direction->from, SHUT_RDWR);
	shutdown(direction->to, SHUT_RDWR);
}

static void *relay_thread(void *argument)
{
	relay_direction *direction = argument;

	relay_loop(direction);
	return NULL;
}

static int tunnel(int client_fd, int upstream_fd, reader *client_reader,
		  reader *upstream_reader)
{
	relay_direction upstream_to_client = { upstream_fd, client_fd };
	relay_direction client_to_upstream = { client_fd, upstream_fd };
	pthread_t thread;
	int created;
	const char *error = NULL;

	if (upstream_reader->end > upstream_reader->start) {
		size_t len = upstream_reader->end - upstream_reader->start;

		if (dsp_write_full(client_fd,
				   upstream_reader->buffer + upstream_reader->start,
				   len, 10000, &error) < 0)
			return -1;
		upstream_reader->start = upstream_reader->end;
	}
	if (client_reader->end > client_reader->start) {
		size_t len = client_reader->end - client_reader->start;

		if (dsp_write_full(upstream_fd,
				   client_reader->buffer + client_reader->start,
				   len, 10000, &error) < 0)
			return -1;
		client_reader->start = client_reader->end;
	}

	created = pthread_create(&thread, NULL, relay_thread,
				 &upstream_to_client) == 0;
	if (!created)
		log_warn("could not create WebSocket relay thread; closing tunnel");
	relay_loop(&client_to_upstream);
	shutdown(client_fd, SHUT_RDWR);
	shutdown(upstream_fd, SHUT_RDWR);
	if (created)
		pthread_join(thread, NULL);
	return 0;
}

static void peer_ip(const char *peer, char *out, size_t out_cap)
{
	const char *colon;

	if (peer[0] == '[') {
		const char *close = strchr(peer, ']');

		if (close != NULL) {
			size_t len = (size_t)(close - peer - 1);

			if (len + 1 > out_cap)
				len = out_cap - 1;
			memcpy(out, peer + 1, len);
			out[len] = '\0';
			return;
		}
	}
	colon = strrchr(peer, ':');
	if (colon == NULL) {
		snprintf(out, out_cap, "%s", peer);
		return;
	}
	{
		size_t len = (size_t)(colon - peer);

		if (len + 1 > out_cap)
			len = out_cap - 1;
		memcpy(out, peer, len);
		out[len] = '\0';
	}
}

static int response_is_event_stream(const char *raw)
{
	return strcasestr(raw, "content-type: text/event-stream") != NULL;
}

static int handle_client(dsp_proxy *proxy, int client_fd, const char *peer)
{
	reader client_reader;
	reader upstream_reader;
	dsp_buf header;
	http_request request;
	body_info body;
	char target[8192];
	char peer_address[128];
	char client_host[300];
	dsp_upstream_view view;
	dsp_buf request_headers;
	int upstream_fd = -1;
	int status = 0;
	int rc;
	int result = -1;

	dsp_buf_init(&header);
	dsp_buf_init(&request_headers);
	reader_init(&client_reader, client_fd, 15000);
	reader_init(&upstream_reader, -1, 60000);

	rc = read_headers(&client_reader, &header, MAX_HEADER_BYTES);
	if (rc < 0) {
		if (rc == -3)
			send_simple(client_fd, 408, "Request Timeout", "",
				    "request header timed out\n");
		else if (rc == -2)
			send_simple(client_fd, 431,
				    "Request Header Fields Too Large", "",
				    "request header too large\n");
		else if (rc != -1)
			send_simple(client_fd, 400, "Bad Request", "",
				    "malformed request\n");
		goto cleanup;
	}
	rc = parse_request(&header, &request);
	if (rc < 0) {
		send_simple(client_fd, 400, "Bad Request", "",
			    "malformed HTTP request\n");
		goto cleanup;
	}
	rc = analyze_body(&request, &body);
	if (rc < 0) {
		send_simple(client_fd, 400, "Bad Request", "",
			    "ambiguous or unsupported request body framing\n");
		goto cleanup;
	}
	if (sanitize_target(request.target, target, sizeof(target)) < 0) {
		send_simple(client_fd, 400, "Bad Request", "",
			    "invalid request target\n");
		goto cleanup;
	}

	dsp_upstream_snapshot(proxy->upstream, &view);
	if (!view.ready) {
		send_simple(client_fd, 503, "Service Unavailable",
			    "Retry-After: 2\r\n",
			    "dsh is still starting or renewing its session; retry shortly.\n");
		goto cleanup;
	}
	{
		const http_header *host = find_header(&request, "host");

		if (host != NULL)
			snprintf(client_host, sizeof(client_host), "%s",
				 host->value);
		else
			snprintf(client_host, sizeof(client_host), "%s",
				 view.authority);
	}
	peer_ip(peer, peer_address, sizeof(peer_address));

	log_debug("%s %s %s -> %s", peer, request.method, target,
		  view.authority);
	if (build_request(&request, &body, &view, target, client_host,
			  peer_address, &request_headers) < 0) {
		send_simple(client_fd, 500, "Internal Server Error", "",
			    "could not build upstream request\n");
		goto cleanup;
	}

	{
		char error[256];

		error[0] = '\0';
		upstream_fd = dsp_connect_host(view.host, view.port, 10000,
					       error, sizeof(error));
		if (upstream_fd < 0) {
			log_warn("upstream connect failed: %s", error);
			send_simple(client_fd, 502, "Bad Gateway", "",
				    "cannot reach dsh\n");
			goto cleanup;
		}
	}
	{
		const char *error = NULL;

		if (dsp_write_full(upstream_fd, request_headers.data,
				   request_headers.len, 15000, &error) < 0) {
			log_warn("writing upstream request failed: %s",
				 error == NULL ? "unknown" : error);
			send_simple(client_fd, 502, "Bad Gateway", "",
				    "cannot send request to dsh\n");
			goto cleanup;
		}
	}

	if (body.expect_continue &&
	    (body.chunked || (body.has_length && body.length > 0))) {
		static const char continue_response[] =
			"HTTP/1.1 100 Continue\r\n\r\n";
		const char *error = NULL;

		if (dsp_write_full(client_fd, continue_response,
				   sizeof(continue_response) - 1, 5000,
				   &error) < 0)
			goto cleanup;
	}
	if ((body.chunked || body.has_length) &&
	    forward_body(&client_reader, upstream_fd, &body) < 0) {
		log_debug("forwarding request body failed for %s", peer);
		goto cleanup;
	}

	reader_init(&upstream_reader, upstream_fd, 60000);
	{
		dsp_buf response_header;
		int attempts;

		dsp_buf_init(&response_header);
		for (attempts = 0; attempts < 4; attempts++) {
			rc = read_headers(&upstream_reader, &response_header,
					  MAX_HEADER_BYTES);
			if (rc < 0)
				break;
			status = parse_status(response_header.data);
			if (status != 100)
				break;
			response_header.len = 0;
			if (response_header.data != NULL)
				response_header.data[0] = '\0';
		}
		if (rc < 0 || status < 0 || status == 100) {
			log_warn("invalid response from dsh for %s %s", request.method,
				 target);
			dsp_buf_free(&response_header);
			send_simple(client_fd, 502, "Bad Gateway", "",
				    "invalid response from dsh\n");
			goto cleanup;
		}
		log_debug("%s %s -> %d", peer, target, status);

		if (status == 401) {
			dsp_upstream_note_unauthorized(proxy->upstream);
			send_simple(client_fd, 503, "Service Unavailable",
				    "Retry-After: 2\r\n",
				    "dsh session expired; the proxy is renewing it, retry shortly.\n");
			dsp_buf_free(&response_header);
			goto cleanup;
		}

		if (status == 101) {
			dsp_buf response;

			if (!body.upgrade) {
				dsp_buf_free(&response_header);
				send_simple(client_fd, 502, "Bad Gateway", "",
					    "unexpected protocol switch from dsh\n");
				goto cleanup;
			}
			dsp_buf_init(&response);
			if (build_response(response_header.data, 1,
					   view.authority, client_host,
					   &response) < 0) {
				dsp_buf_free(&response);
				dsp_buf_free(&response_header);
				goto cleanup;
			}
			if (dsp_write_full(client_fd, response.data,
					   response.len, 15000, NULL) < 0) {
				dsp_buf_free(&response);
				dsp_buf_free(&response_header);
				goto cleanup;
			}
			dsp_buf_free(&response);
			dsp_buf_free(&response_header);
			result = tunnel(client_fd, upstream_fd, &client_reader,
					&upstream_reader);
			goto cleanup;
		}

		{
			dsp_buf response;

			dsp_buf_init(&response);
			if (build_response(response_header.data, 0,
					   view.authority, client_host,
					   &response) < 0 ||
			    dsp_write_full(client_fd, response.data,
					   response.len, 15000, NULL) < 0) {
				dsp_buf_free(&response);
				dsp_buf_free(&response_header);
				goto cleanup;
			}
			dsp_buf_free(&response);
		}
		if (strcmp(request.method, "HEAD") != 0 && status != 204 &&
		    status != 304) {
			int timeout = response_is_event_stream(response_header.data)
						      ? 3600000
						      : 300000;

			if (relay_response(&upstream_reader, client_fd,
					   timeout) < 0)
				log_debug("response relay ended early for %s", peer);
		}
		dsp_buf_free(&response_header);
	}
	result = 0;

cleanup:
	if (upstream_fd >= 0)
		close(upstream_fd);
	dsp_buf_free(&header);
	dsp_buf_free(&request_headers);
	return result;
}

static void client_done(dsp_proxy *proxy)
{
	pthread_mutex_lock(&proxy->lock);
	if (proxy->active > 0)
		proxy->active--;
	pthread_cond_broadcast(&proxy->cond);
	pthread_mutex_unlock(&proxy->lock);
}

static void *client_thread(void *argument)
{
	client_job *job = argument;
	dsp_proxy *proxy = job->proxy;
	int fd = job->fd;

	(void)handle_client(proxy, fd, job->peer);
	shutdown(fd, SHUT_RDWR);
	close(fd);
	client_done(proxy);
	free(job);
	return NULL;
}

dsp_proxy *dsp_proxy_create(const dsp_config *config, dsp_upstream *upstream,
			    char *error, size_t error_cap)
{
	dsp_proxy *proxy = calloc(1, sizeof(*proxy));
	char local[128];

	if (proxy == NULL) {
		snprintf(error, error_cap, "out of memory");
		return NULL;
	}
	proxy->config = config;
	proxy->upstream = upstream;
	proxy->listen_fd = -1;
	pthread_mutex_init(&proxy->lock, NULL);
	pthread_cond_init(&proxy->cond, NULL);
	proxy->listen_fd = dsp_listen(config->listen_host, config->listen_port,
				      128, error, error_cap);
	if (proxy->listen_fd < 0) {
		pthread_mutex_destroy(&proxy->lock);
		pthread_cond_destroy(&proxy->cond);
		free(proxy);
		return NULL;
	}
	if (dsp_get_local_name(proxy->listen_fd, local, sizeof(local)) == 0)
		log_info("proxy listening on %s", local);
	else
		log_info("proxy listening on %s:%s",
			 config->listen_host[0] == '\0' ? "*"
							: config->listen_host,
			 config->listen_port);
	return proxy;
}

int dsp_proxy_fd(dsp_proxy *proxy)
{
	return proxy == NULL ? -1 : proxy->listen_fd;
}

void dsp_proxy_accept(dsp_proxy *proxy)
{
	for (;;) {
		struct sockaddr_storage storage;
		socklen_t storage_len = sizeof(storage);
		int fd;
		int one = 1;
		client_job *job;
		pthread_t thread;
		char peer[128];

		if (proxy == NULL || proxy->stopping)
			return;
		fd = accept4(proxy->listen_fd, (struct sockaddr *)&storage,
			     &storage_len, SOCK_CLOEXEC);
		if (fd < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			if (errno == EINTR)
				continue;
			log_warn("accept failed: %s", strerror(errno));
			break;
		}
		dsp_set_nonblocking(fd, 0);
		(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		(void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
		peer[0] = '\0';
		(void)dsp_get_peer_name(fd, peer, sizeof(peer));

		pthread_mutex_lock(&proxy->lock);
		if (proxy->stopping || proxy->active >= MAX_CLIENTS) {
			pthread_mutex_unlock(&proxy->lock);
			send_simple(fd, 503, "Service Unavailable",
				    "Retry-After: 2\r\n",
				    "too many proxy connections\n");
			close(fd);
			continue;
		}
		proxy->active++;
		pthread_mutex_unlock(&proxy->lock);

		job = calloc(1, sizeof(*job));
		if (job == NULL) {
			client_done(proxy);
			close(fd);
			continue;
		}
		job->proxy = proxy;
		job->fd = fd;
		snprintf(job->peer, sizeof(job->peer), "%s", peer);
		if (pthread_create(&thread, NULL, client_thread, job) != 0) {
			client_done(proxy);
			close(fd);
			free(job);
			continue;
		}
		pthread_detach(thread);
	}
}

int dsp_proxy_destroy(dsp_proxy *proxy)
{
	int active;

	if (proxy == NULL)
		return 1;
	if (proxy->listen_fd >= 0) {
		close(proxy->listen_fd);
		proxy->listen_fd = -1;
	}
	pthread_mutex_lock(&proxy->lock);
	active = proxy->active;
	pthread_mutex_unlock(&proxy->lock);
	if (active > 0) {
		log_warn("not freeing proxy state: %d client thread(s) still active",
			 active);
		return 0;
	}
	pthread_mutex_destroy(&proxy->lock);
	pthread_cond_destroy(&proxy->cond);
	free(proxy);
	return 1;
}

void dsp_proxy_stop(dsp_proxy *proxy)
{
	if (proxy == NULL)
		return;
	pthread_mutex_lock(&proxy->lock);
	proxy->stopping = 1;
	pthread_mutex_unlock(&proxy->lock);
	if (proxy->listen_fd >= 0) {
		close(proxy->listen_fd);
		proxy->listen_fd = -1;
	}
	pthread_mutex_lock(&proxy->lock);
	{
		struct timespec deadline;

		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_sec += 5;
		while (proxy->active > 0) {
			if (pthread_cond_timedwait(&proxy->cond, &proxy->lock,
						   &deadline) != 0)
				break;
		}
	}
	pthread_mutex_unlock(&proxy->lock);
}
