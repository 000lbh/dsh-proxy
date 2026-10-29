#ifndef DSP_UPSTREAM_H
#define DSP_UPSTREAM_H

#include <stddef.h>

typedef struct dsp_upstream dsp_upstream;

typedef struct {
	int ready;
	char host[256];
	char port[16];
	char authority[300];
	char cookie[2048];
} dsp_upstream_view;

dsp_upstream *dsp_upstream_create(const char *override,
				  char *error, size_t error_cap);
void dsp_upstream_destroy(dsp_upstream *upstream);
void dsp_upstream_set_token(dsp_upstream *upstream, const char *token_url);
void dsp_upstream_snapshot(dsp_upstream *upstream, dsp_upstream_view *view);
void dsp_upstream_refresh_if_due(dsp_upstream *upstream, int max_age_seconds);
void dsp_upstream_note_unauthorized(dsp_upstream *upstream);
void dsp_upstream_child_lost(dsp_upstream *upstream);
void dsp_upstream_shutdown(dsp_upstream *upstream);

#endif
