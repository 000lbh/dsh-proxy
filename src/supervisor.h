#ifndef DSP_SUPERVISOR_H
#define DSP_SUPERVISOR_H

#include "config.h"
#include "upstream.h"

typedef struct dsp_supervisor dsp_supervisor;

dsp_supervisor *dsp_supervisor_create(const dsp_config *config,
				      dsp_upstream *upstream);
void dsp_supervisor_destroy(dsp_supervisor *supervisor);
void dsp_supervisor_start(dsp_supervisor *supervisor);
int dsp_supervisor_fd(dsp_supervisor *supervisor);
void dsp_supervisor_read(dsp_supervisor *supervisor);
void dsp_supervisor_tick(dsp_supervisor *supervisor);
void dsp_supervisor_stop(dsp_supervisor *supervisor);

#endif
