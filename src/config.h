#ifndef DSP_CONFIG_H
#define DSP_CONFIG_H

#include <stddef.h>

#include "log.h"

#define DSP_CONFIG_DEFAULT_PATH "/etc/dsh-proxy/dsh-proxy.conf"

typedef struct {
	char listen_host[256];
	char listen_port[16];
	char dsh_command[2048];
	int dsh_port;
	char dsh_home[1024];
	char upstream[512];
	int token_timeout;
	int restart_delay;
	int cookie_refresh;
	int auto_yes;
	dsp_log_level log_level;
} dsp_config;

void dsp_config_defaults(dsp_config *config);
int dsp_config_load_file(dsp_config *config, const char *path, int required,
			 char *error, size_t error_cap);
int dsp_config_parse_args(dsp_config *config, int argc, char **argv,
			  char *error, size_t error_cap);
void dsp_config_print_help(const char *program);
void dsp_config_print(const dsp_config *config);

#endif
