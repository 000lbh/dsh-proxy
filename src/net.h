#ifndef DSP_NET_H
#define DSP_NET_H

#include <stddef.h>

int dsp_connect_host(const char *host, const char *port, int timeout_ms,
		     char *error, size_t error_cap);
int dsp_listen(const char *host, const char *port, int backlog,
	       char *error, size_t error_cap);
void dsp_set_nonblocking(int fd, int enabled);
void dsp_set_cloexec(int fd);
int dsp_get_peer_name(int fd, char *out, size_t out_cap);
int dsp_get_local_name(int fd, char *out, size_t out_cap);

#endif
