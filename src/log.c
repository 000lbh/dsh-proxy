#define _GNU_SOURCE

#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static dsp_log_level g_level = DSP_LOG_INFO;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

void dsp_log_set_level(dsp_log_level level)
{
	g_level = level;
}

dsp_log_level dsp_log_get_level(void)
{
	return g_level;
}

const char *dsp_log_level_name(dsp_log_level level)
{
	switch (level) {
	case DSP_LOG_ERROR:
		return "error";
	case DSP_LOG_WARN:
		return "warn";
	case DSP_LOG_INFO:
		return "info";
	case DSP_LOG_DEBUG:
		return "debug";
	}
	return "?";
}

void dsp_log(dsp_log_level level, const char *fmt, ...)
{
	char message[4096];
	char stamp[32];
	struct timespec ts;
	struct tm tm;
	va_list ap;
	int off;
	int n;

	if (level > g_level)
		return;

	clock_gettime(CLOCK_REALTIME, &ts);
	localtime_r(&ts.tv_sec, &tm);
	strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);

	va_start(ap, fmt);
	n = vsnprintf(message, sizeof(message), fmt, ap);
	va_end(ap);
	if (n < 0)
		message[0] = '\0';
	else if ((size_t)n >= sizeof(message))
		message[sizeof(message) - 1] = '\0';

	off = snprintf(stamp + strlen(stamp), sizeof(stamp) - strlen(stamp),
		       ".%03ld", ts.tv_nsec / 1000000L);
	(void)off;

	pthread_mutex_lock(&g_log_lock);
	fprintf(stderr, "%s [%s] %s\n", stamp, dsp_log_level_name(level), message);
	fflush(stderr);
	pthread_mutex_unlock(&g_log_lock);
}
