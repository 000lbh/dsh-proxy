#ifndef DSP_PROXY_H
#define DSP_PROXY_H

#include <stddef.h>

#include "config.h"
#include "upstream.h"

typedef struct dsp_proxy dsp_proxy;

dsp_proxy *dsp_proxy_create(const dsp_config *config, dsp_upstream *upstream,
			    char *error, size_t error_cap);
int dsp_proxy_fd(dsp_proxy *proxy);
void dsp_proxy_accept(dsp_proxy *proxy);
void dsp_proxy_stop(dsp_proxy *proxy);
int dsp_proxy_destroy(dsp_proxy *proxy);

#endif
