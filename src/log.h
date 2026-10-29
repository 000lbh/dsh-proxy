#ifndef DSP_LOG_H
#define DSP_LOG_H

#include <stdarg.h>

typedef enum {
	DSP_LOG_ERROR = 0,
	DSP_LOG_WARN,
	DSP_LOG_INFO,
	DSP_LOG_DEBUG
} dsp_log_level;

void dsp_log_set_level(dsp_log_level level);
dsp_log_level dsp_log_get_level(void);
const char *dsp_log_level_name(dsp_log_level level);
void dsp_log(dsp_log_level level, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

#define log_error(...) dsp_log(DSP_LOG_ERROR, __VA_ARGS__)
#define log_warn(...) dsp_log(DSP_LOG_WARN, __VA_ARGS__)
#define log_info(...) dsp_log(DSP_LOG_INFO, __VA_ARGS__)
#define log_debug(...) dsp_log(DSP_LOG_DEBUG, __VA_ARGS__)

#endif
