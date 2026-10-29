#define _GNU_SOURCE

#include "upstream.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "net.h"
#include "util.h"

struct dsp_upstream {
	pthread_mutex_t lock;
	pthread_cond_t cond;

	int shutting_down;
	int ready;
	int refreshing;
	uint64_t generation;
	time_t cookie_time;
	time_t last_attempt;

	char host[256];
	char port[16];
	char authority[300];
	char token[512];
	char cookie[2048];

	int has_override;
	char override_host[256];
	char override_port[16];
	char override_authority[300];
};

struct exchange_job {
	dsp_upstream *upstream;
	uint64_t generation;
	char host[256];
	char port[16];
	char authority[300];
	char token[512];
};

static int exchange_cookie(const char *host, const char *port,
			   const char *authority, const char *token,
			   char *cookie, size_t cookie_cap)
{
	dsp_buf request;
	dsp_buf response;
	char error[256];
	const char *read_error = NULL;
	int fd = -1;
	int result = -1;
	size_t header_end = 0;
	char *headers = NULL;
	char *line;
	char *save = NULL;

	dsp_buf_init(&request);
	dsp_buf_init(&response);
	if (dsp_buf_printf(&request,
			   "GET /?token=%s HTTP/1.1\r\n"
			   "Host: %s\r\n"
			   "User-Agent: dsh-proxy/1.0\r\n"
			   "Accept: */*\r\n"
			   "Connection: close\r\n"
			   "\r\n",
			   token, authority) < 0)
		goto done;

	error[0] = '\0';
	fd = dsp_connect_host(host, port, 10000, error, sizeof(error));
	if (fd < 0) {
		log_warn("cookie exchange: %s", error);
		goto done;
	}
	if (dsp_write_full(fd, request.data, request.len, 10000,
			   &read_error) < 0) {
		log_warn("cookie exchange: write failed: %s",
			 read_error == NULL ? "unknown" : read_error);
		goto done;
	}

	for (;;) {
		char chunk[4096];
		ssize_t n;
		int ready = dsp_wait_fd(fd, POLLIN, 10000);

		if (ready == 0) {
			log_warn("cookie exchange: timed out reading response");
			goto done;
		}
		if (ready < 0) {
			log_warn("cookie exchange: poll failed: %s",
				 strerror(errno));
			goto done;
		}
		n = recv(fd, chunk, sizeof(chunk), 0);
		if (n == 0)
			break;
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN ||
			    errno == EWOULDBLOCK)
				continue;
			log_warn("cookie exchange: read failed: %s",
				 strerror(errno));
			goto done;
		}
		if (dsp_buf_append(&response, chunk, (size_t)n) < 0 ||
		    response.len > 65536)
			break;
		if (strstr(response.data, "\r\n\r\n") != NULL)
			break;
	}

	header_end = 0;
	if (response.data != NULL) {
		size_t i;

		for (i = 0; i + 3 < response.len; i++) {
			if (response.data[i] == '\r' &&
			    response.data[i + 1] == '\n' &&
			    response.data[i + 2] == '\r' &&
			    response.data[i + 3] == '\n') {
				header_end = i + 4;
				break;
			}
		}
	}
	if (header_end == 0) {
		log_warn("cookie exchange: malformed or empty HTTP response");
		goto done;
	}
	headers = dsp_strndup(response.data, header_end);
	if (headers == NULL)
		goto done;

	line = strtok_r(headers, "\r\n", &save);
	if (line == NULL || !dsp_has_prefix_ci(line, "HTTP/1.")) {
		log_warn("cookie exchange: invalid status line");
		goto done;
	}
	{
		int status = 0;
		const char *space = strchr(line, ' ');
		const char *space2;

		if (space != NULL) {
			status = atoi(space + 1);
			space2 = strchr(space + 1, ' ');
			if (space2 != NULL)
				log_debug("cookie exchange: HTTP status %d", status);
		}
		if (status == 401) {
			log_warn("cookie exchange: dsh rejected the token (401)");
			goto done;
		}
	}

	while ((line = strtok_r(NULL, "\r\n", &save)) != NULL) {
		const char *value;
		const char *end;
		size_t len;

		if (!dsp_has_prefix_ci(line, "Set-Cookie:"))
			continue;
		value = line + strlen("Set-Cookie:");
		while (*value == ' ' || *value == '\t')
			value++;
		if (!dsp_has_prefix_ci(value, "dsh-auth-"))
			continue;
		end = value;
		while (*end != '\0' && *end != ';')
			end++;
		len = (size_t)(end - value);
		while (len > 0 && (value[len - 1] == ' ' || value[len - 1] == '\t'))
			len--;
		if (len == 0 || len + 1 > cookie_cap ||
		    memchr(value, '\r', len) != NULL ||
		    memchr(value, '\n', len) != NULL)
			continue;
		memcpy(cookie, value, len);
		cookie[len] = '\0';
		if (strchr(cookie, '=') == NULL)
			continue;
		result = 0;
		break;
	}

	if (result < 0)
		log_warn("cookie exchange: no dsh-auth Set-Cookie in response");

done:
	if (fd >= 0)
		close(fd);
	free(headers);
	dsp_buf_free(&request);
	dsp_buf_free(&response);
	return result;
}

static void *exchange_thread(void *argument)
{
	struct exchange_job *job = argument;
	dsp_upstream *upstream = job->upstream;
	char cookie[2048];
	int rc;

	memset(cookie, 0, sizeof(cookie));
	rc = exchange_cookie(job->host, job->port, job->authority, job->token,
			     cookie, sizeof(cookie));

	pthread_mutex_lock(&upstream->lock);
	if (job->generation == upstream->generation) {
		if (rc == 0) {
			snprintf(upstream->cookie, sizeof(upstream->cookie), "%s",
				 cookie);
			upstream->cookie_time = time(NULL);
			upstream->ready = 1;
			log_info("session cookie acquired (upstream=%s)",
				 upstream->authority);
		} else {
			if (upstream->cookie[0] == '\0')
				upstream->ready = 0;
			log_warn("cookie exchange failed; upstream=%s",
				 upstream->authority);
		}
	} else {
		log_debug("discarding stale cookie exchange result");
	}
	upstream->refreshing--;
	pthread_cond_broadcast(&upstream->cond);
	pthread_mutex_unlock(&upstream->lock);

	free(job);
	return NULL;
}

static void start_exchange_locked(dsp_upstream *upstream)
{
	struct exchange_job *job;
	pthread_t thread;
	int rc;

	if (upstream->shutting_down || upstream->token[0] == '\0')
		return;
	job = calloc(1, sizeof(*job));
	if (job == NULL) {
		log_error("cookie exchange: out of memory");
		return;
	}
	job->upstream = upstream;
	job->generation = upstream->generation;
	snprintf(job->host, sizeof(job->host), "%s", upstream->host);
	snprintf(job->port, sizeof(job->port), "%s", upstream->port);
	snprintf(job->authority, sizeof(job->authority), "%s",
		 upstream->authority);
	snprintf(job->token, sizeof(job->token), "%s", upstream->token);
	upstream->refreshing++;
	upstream->last_attempt = time(NULL);
	rc = pthread_create(&thread, NULL, exchange_thread, job);
	if (rc != 0) {
		upstream->refreshing--;
		free(job);
		log_error("cookie exchange: pthread_create failed: %s",
			  strerror(rc));
		return;
	}
	pthread_detach(thread);
}

void dsp_upstream_refresh_if_due(dsp_upstream *upstream, int max_age_seconds)
{
	time_t now;

	if (upstream == NULL)
		return;
	now = time(NULL);
	pthread_mutex_lock(&upstream->lock);
	if (upstream->shutting_down || upstream->refreshing > 0 ||
	    upstream->token[0] == '\0') {
		pthread_mutex_unlock(&upstream->lock);
		return;
	}
	if (upstream->ready) {
		if (upstream->cookie_time != 0 &&
		    now - upstream->cookie_time >= max_age_seconds)
			start_exchange_locked(upstream);
	} else if (now - upstream->last_attempt >= 5) {
		start_exchange_locked(upstream);
	}
	pthread_mutex_unlock(&upstream->lock);
}

dsp_upstream *dsp_upstream_create(const char *override, char *error,
				  size_t error_cap)
{
	dsp_upstream *upstream = calloc(1, sizeof(*upstream));

	if (upstream == NULL) {
		snprintf(error, error_cap, "out of memory");
		return NULL;
	}
	pthread_mutex_init(&upstream->lock, NULL);
	pthread_cond_init(&upstream->cond, NULL);
	if (override != NULL && override[0] != '\0') {
		if (dsp_parse_hostport(override, upstream->override_host,
				       sizeof(upstream->override_host),
				       upstream->override_port,
				       sizeof(upstream->override_port)) < 0 ||
		    upstream->override_port[0] == '\0') {
			snprintf(error, error_cap,
				 "invalid UPSTREAM %s (expected HOST:PORT)", override);
			pthread_mutex_destroy(&upstream->lock);
			pthread_cond_destroy(&upstream->cond);
			free(upstream);
			return NULL;
		}
		if (dsp_format_authority(upstream->override_host,
					 upstream->override_port, "http",
					 upstream->override_authority,
					 sizeof(upstream->override_authority)) < 0) {
			snprintf(error, error_cap, "invalid UPSTREAM authority");
			pthread_mutex_destroy(&upstream->lock);
			pthread_cond_destroy(&upstream->cond);
			free(upstream);
			return NULL;
		}
		upstream->has_override = 1;
	}
	return upstream;
}

void dsp_upstream_destroy(dsp_upstream *upstream)
{
	if (upstream == NULL)
		return;
	pthread_mutex_destroy(&upstream->lock);
	pthread_cond_destroy(&upstream->cond);
	free(upstream);
}

void dsp_upstream_set_token(dsp_upstream *upstream, const char *token_url)
{
	char scheme[8];
	char host[256];
	char port[16];
	char path[1024];
	char token[512];
	char authority[300];
	const char *use_host;
	const char *use_port;
	int same;

	if (upstream == NULL || token_url == NULL)
		return;
	if (dsp_parse_http_url(token_url, scheme, sizeof(scheme), host,
			       sizeof(host), port, sizeof(port), path,
			       sizeof(path)) < 0) {
		log_warn("could not parse dsh token URL");
		return;
	}
	if (dsp_token_from_url(token_url, token, sizeof(token)) < 0) {
		log_debug("dsh output URL has no token parameter");
		return;
	}
	if (dsp_strcaseeq(scheme, "https") && !upstream->has_override) {
		log_error("dsh printed an https URL; this proxy only supports an http upstream. "
			  "Run dsh web without --tls-cert/--tls-key, or set UPSTREAM to the local HTTP address.");
		pthread_mutex_lock(&upstream->lock);
		upstream->generation++;
		upstream->ready = 0;
		upstream->cookie[0] = '\0';
		upstream->token[0] = '\0';
		pthread_mutex_unlock(&upstream->lock);
		return;
	}

	use_host = upstream->has_override ? upstream->override_host : host;
	use_port = upstream->has_override ? upstream->override_port : port;
	if (dsp_format_authority(use_host, use_port, "http", authority,
				 sizeof(authority)) < 0) {
		log_warn("could not format upstream authority");
		return;
	}
	{
		char base_path[1024];
		size_t base_len = strcspn(path, "?#");

		if (base_len >= sizeof(base_path))
			base_len = sizeof(base_path) - 1;
		memcpy(base_path, path, base_len);
		base_path[base_len] = '\0';
		if (strcmp(base_path, "/") != 0)
			log_debug("token URL path is %s; the local token exchange uses /",
				  base_path);
	}

	pthread_mutex_lock(&upstream->lock);
	same = upstream->ready && strcmp(upstream->token, token) == 0 &&
	       strcmp(upstream->authority, authority) == 0;
	if (same) {
		pthread_mutex_unlock(&upstream->lock);
		log_debug("dsh token unchanged; keeping the current cookie");
		return;
	}
	upstream->generation++;
	snprintf(upstream->host, sizeof(upstream->host), "%s", use_host);
	snprintf(upstream->port, sizeof(upstream->port), "%s", use_port);
	snprintf(upstream->authority, sizeof(upstream->authority), "%s",
		 authority);
	snprintf(upstream->token, sizeof(upstream->token), "%s", token);
	upstream->cookie[0] = '\0';
	upstream->cookie_time = 0;
	upstream->ready = 0;
	upstream->last_attempt = 0;
	log_info("dsh token acquired; upstream=%s", authority);
	start_exchange_locked(upstream);
	pthread_mutex_unlock(&upstream->lock);
}

void dsp_upstream_snapshot(dsp_upstream *upstream, dsp_upstream_view *view)
{
	memset(view, 0, sizeof(*view));
	if (upstream == NULL)
		return;
	pthread_mutex_lock(&upstream->lock);
	view->ready = upstream->ready;
	if (upstream->ready) {
		snprintf(view->host, sizeof(view->host), "%s", upstream->host);
		snprintf(view->port, sizeof(view->port), "%s", upstream->port);
		snprintf(view->authority, sizeof(view->authority), "%s",
			 upstream->authority);
		snprintf(view->cookie, sizeof(view->cookie), "%s",
			 upstream->cookie);
	}
	pthread_mutex_unlock(&upstream->lock);
}

void dsp_upstream_note_unauthorized(dsp_upstream *upstream)
{
	if (upstream == NULL)
		return;
	pthread_mutex_lock(&upstream->lock);
	if (upstream->shutting_down) {
		pthread_mutex_unlock(&upstream->lock);
		return;
	}
	if (upstream->ready)
		log_warn("dsh rejected the injected session cookie; refreshing it");
	upstream->ready = 0;
	upstream->last_attempt = 0;
	pthread_mutex_unlock(&upstream->lock);
	dsp_upstream_refresh_if_due(upstream, 0);
}

void dsp_upstream_child_lost(dsp_upstream *upstream)
{
	if (upstream == NULL)
		return;
	pthread_mutex_lock(&upstream->lock);
	upstream->generation++;
	upstream->ready = 0;
	upstream->cookie[0] = '\0';
	upstream->cookie_time = 0;
	upstream->token[0] = '\0';
	upstream->last_attempt = 0;
	pthread_mutex_unlock(&upstream->lock);
}

void dsp_upstream_shutdown(dsp_upstream *upstream)
{
	if (upstream == NULL)
		return;
	pthread_mutex_lock(&upstream->lock);
	upstream->shutting_down = 1;
	while (upstream->refreshing > 0)
		pthread_cond_wait(&upstream->cond, &upstream->lock);
	pthread_mutex_unlock(&upstream->lock);
}
