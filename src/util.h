#ifndef DSP_UTIL_H
#define DSP_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct {
	char *data;
	size_t len;
	size_t cap;
} dsp_buf;

void dsp_buf_init(dsp_buf *buf);
void dsp_buf_free(dsp_buf *buf);
int dsp_buf_reserve(dsp_buf *buf, size_t extra);
int dsp_buf_append(dsp_buf *buf, const void *data, size_t len);
int dsp_buf_append_str(dsp_buf *buf, const char *text);
int dsp_buf_printf(dsp_buf *buf, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
char *dsp_buf_steal(dsp_buf *buf, size_t *len_out);

char *dsp_strdup(const char *text);
char *dsp_strndup(const char *text, size_t len);
void dsp_trim(char *text);
int dsp_strcaseeq(const char *a, const char *b);
int dsp_strncaseeq(const char *a, const char *b, size_t n);
int dsp_has_prefix_ci(const char *text, const char *prefix);
int dsp_has_token_ci(const char *value, const char *token);

int dsp_parse_hostport(const char *text, char *host, size_t host_cap,
		       char *port, size_t port_cap);
int dsp_format_authority(const char *host, const char *port,
			 const char *scheme, char *out, size_t out_cap);
int dsp_parse_http_url(const char *url, char *scheme, size_t scheme_cap,
		       char *host, size_t host_cap, char *port, size_t port_cap,
		       char *path, size_t path_cap);
int dsp_token_from_url(const char *url, char *token, size_t token_cap);

int dsp_wait_fd(int fd, short events, int timeout_ms);
int dsp_read_full(int fd, void *buffer, size_t len, int timeout_ms,
		  const char **error);
int dsp_write_full(int fd, const void *buffer, size_t len, int timeout_ms,
		   const char **error);

uint64_t dsp_now_ms(void);
void dsp_strip_ansi(char *text);
void dsp_redact_token(const char *line, char *out, size_t out_cap);

#endif
