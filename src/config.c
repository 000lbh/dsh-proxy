#define _GNU_SOURCE

#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define DSP_PROXY_VERSION "1.0.0"

static int parse_bool(const char *text, int *out)
{
	if (dsp_strcaseeq(text, "1") || dsp_strcaseeq(text, "yes") ||
	    dsp_strcaseeq(text, "true") || dsp_strcaseeq(text, "on")) {
		*out = 1;
		return 0;
	}
	if (dsp_strcaseeq(text, "0") || dsp_strcaseeq(text, "no") ||
	    dsp_strcaseeq(text, "false") || dsp_strcaseeq(text, "off")) {
		*out = 0;
		return 0;
	}
	return -1;
}

static int parse_int_range(const char *text, int min, int max, int *out)
{
	char *end = NULL;
	long value;

	errno = 0;
	value = strtol(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' || value < min ||
	    value > max)
		return -1;
	*out = (int)value;
	return 0;
}

static int parse_log_level(const char *text, dsp_log_level *out)
{
	if (dsp_strcaseeq(text, "error"))
		*out = DSP_LOG_ERROR;
	else if (dsp_strcaseeq(text, "warn") || dsp_strcaseeq(text, "warning"))
		*out = DSP_LOG_WARN;
	else if (dsp_strcaseeq(text, "info"))
		*out = DSP_LOG_INFO;
	else if (dsp_strcaseeq(text, "debug"))
		*out = DSP_LOG_DEBUG;
	else
		return -1;
	return 0;
}

static int set_listen(dsp_config *config, const char *value, char *error,
		      size_t error_cap)
{
	char host[sizeof(config->listen_host)];
	char port[sizeof(config->listen_port)];

	if (dsp_parse_hostport(value, host, sizeof(host), port, sizeof(port)) < 0 ||
	    port[0] == '\0') {
		snprintf(error, error_cap,
			 "invalid LISTEN value %s (expected IP:PORT, e.g. 0.0.0.0:8080 or [::]:8080)",
			 value);
		return -1;
	}
	snprintf(config->listen_host, sizeof(config->listen_host), "%s", host);
	snprintf(config->listen_port, sizeof(config->listen_port), "%s", port);
	return 0;
}

void dsp_config_defaults(dsp_config *config)
{
	memset(config, 0, sizeof(*config));
	strcpy(config->listen_host, "127.0.0.1");
	strcpy(config->listen_port, "8080");
	strcpy(config->dsh_command, "dsh web --no-open");
	config->dsh_port = 0;
	config->token_timeout = 180;
	config->restart_delay = 5;
	config->cookie_refresh = 21600;
	config->auto_yes = 1;
	config->log_level = DSP_LOG_INFO;
}

static void unquote(char *text)
{
	size_t len = strlen(text);

	if (len < 2)
		return;
	if ((text[0] == '"' && text[len - 1] == '"') ||
	    (text[0] == '\'' && text[len - 1] == '\'')) {
		memmove(text, text + 1, len - 2);
		text[len - 2] = '\0';
	}
}

int dsp_config_load_file(dsp_config *config, const char *path, int required,
			 char *error, size_t error_cap)
{
	FILE *file;
	char *line = NULL;
	size_t line_cap = 0;
	ssize_t line_len;
	int line_number = 0;
	int result = 0;

	file = fopen(path, "re");
	if (file == NULL) {
		if (required) {
			snprintf(error, error_cap, "cannot read config %s: %s",
				 path, strerror(errno));
			return -1;
		}
		return 0;
	}

	while ((line_len = getline(&line, &line_cap, file)) >= 0) {
		char *eq;
		char *key;
		char *value;

		line_number++;
		while (line_len > 0 &&
		       (line[line_len - 1] == '\n' || line[line_len - 1] == '\r'))
			line[--line_len] = '\0';
		dsp_trim(line);
		if (line[0] == '\0' || line[0] == '#')
			continue;
		eq = strchr(line, '=');
		if (eq == NULL) {
			log_warn("config %s:%d: ignoring line without '='",
				 path, line_number);
			continue;
		}
		*eq = '\0';
		key = line;
		value = eq + 1;
		dsp_trim(key);
		dsp_trim(value);
		unquote(value);

		if (dsp_strcaseeq(key, "LISTEN")) {
			if (set_listen(config, value, error, error_cap) < 0)
				result = -1;
		} else if (dsp_strcaseeq(key, "DSH_CMD")) {
			if (value[0] == '\0' ||
			    strlen(value) >= sizeof(config->dsh_command)) {
				snprintf(error, error_cap,
					 "config %s:%d: invalid DSH_CMD",
					 path, line_number);
				result = -1;
			} else {
				strcpy(config->dsh_command, value);
			}
		} else if (dsp_strcaseeq(key, "DSH_PORT")) {
			if (parse_int_range(value, 0, 65535,
					    &config->dsh_port) < 0) {
				snprintf(error, error_cap,
					 "config %s:%d: DSH_PORT must be 0..65535",
					 path, line_number);
				result = -1;
			}
		} else if (dsp_strcaseeq(key, "DSH_HOME")) {
			if (strlen(value) >= sizeof(config->dsh_home)) {
				snprintf(error, error_cap,
					 "config %s:%d: DSH_HOME is too long",
					 path, line_number);
				result = -1;
			} else {
				strcpy(config->dsh_home, value);
			}
		} else if (dsp_strcaseeq(key, "UPSTREAM")) {
			if (strlen(value) >= sizeof(config->upstream)) {
				snprintf(error, error_cap,
					 "config %s:%d: UPSTREAM is too long",
					 path, line_number);
				result = -1;
			} else {
				strcpy(config->upstream, value);
			}
		} else if (dsp_strcaseeq(key, "TOKEN_TIMEOUT")) {
			if (parse_int_range(value, 1, 86400,
					    &config->token_timeout) < 0) {
				snprintf(error, error_cap,
					 "config %s:%d: TOKEN_TIMEOUT must be 1..86400",
					 path, line_number);
				result = -1;
			}
		} else if (dsp_strcaseeq(key, "RESTART_DELAY")) {
			if (parse_int_range(value, 1, 3600,
					    &config->restart_delay) < 0) {
				snprintf(error, error_cap,
					 "config %s:%d: RESTART_DELAY must be 1..3600",
					 path, line_number);
				result = -1;
			}
		} else if (dsp_strcaseeq(key, "COOKIE_REFRESH")) {
			if (parse_int_range(value, 60, 2592000,
					    &config->cookie_refresh) < 0) {
				snprintf(error, error_cap,
					 "config %s:%d: COOKIE_REFRESH must be 60..2592000",
					 path, line_number);
				result = -1;
			}
		} else if (dsp_strcaseeq(key, "AUTO_YES")) {
			if (parse_bool(value, &config->auto_yes) < 0) {
				snprintf(error, error_cap,
					 "config %s:%d: AUTO_YES must be 0 or 1",
					 path, line_number);
				result = -1;
			}
		} else if (dsp_strcaseeq(key, "LOG_LEVEL")) {
			if (parse_log_level(value, &config->log_level) < 0) {
				snprintf(error, error_cap,
					 "config %s:%d: LOG_LEVEL must be error, warn, info, or debug",
					 path, line_number);
				result = -1;
			}
		} else {
			log_warn("config %s:%d: unknown key %s", path,
				 line_number, key);
		}
		if (result < 0)
			break;
	}

	free(line);
	fclose(file);
	return result;
}

static int copy_option(char *dest, size_t cap, const char *value,
		       const char *name, char *error, size_t error_cap)
{
	if (strlen(value) >= cap) {
		snprintf(error, error_cap, "%s value is too long", name);
		return -1;
	}
	strcpy(dest, value);
	return 0;
}

int dsp_config_parse_args(dsp_config *config, int argc, char **argv,
			  char *error, size_t error_cap)
{
	const char *config_path = NULL;
	int config_required = 0;
	int option;
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-c") == 0 ||
		    strcmp(argv[i], "--config") == 0) {
			if (i + 1 >= argc) {
				snprintf(error, error_cap,
					 "%s requires a path", argv[i]);
				return -1;
			}
			config_path = argv[i + 1];
			config_required = 1;
			i++;
		} else if (strncmp(argv[i], "--config=", 9) == 0) {
			config_path = argv[i] + 9;
			config_required = 1;
		}
	}

	if (dsp_config_load_file(config,
				 config_path == NULL ? DSP_CONFIG_DEFAULT_PATH
						     : config_path,
				 config_required, error, error_cap) < 0)
		return -1;

	opterr = 0;
	optind = 1;
	for (;;) {
		static const struct option long_options[] = {
			{ "help", no_argument, NULL, 'h' },
			{ "version", no_argument, NULL, 'V' },
			{ "config", required_argument, NULL, 'c' },
			{ "listen", required_argument, NULL, 'l' },
			{ "dsh-command", required_argument, NULL, 'd' },
			{ "dsh-port", required_argument, NULL, 'P' },
			{ "dsh-home", required_argument, NULL, 'H' },
			{ "upstream", required_argument, NULL, 'u' },
			{ "token-timeout", required_argument, NULL, 't' },
			{ "restart-delay", required_argument, NULL, 'r' },
			{ "cookie-refresh", required_argument, NULL, 'R' },
			{ "auto-yes", no_argument, NULL, 'y' },
			{ "no-auto-yes", no_argument, NULL, 1000 },
			{ "verbose", no_argument, NULL, 'v' },
			{ "quiet", no_argument, NULL, 'q' },
			{ NULL, 0, NULL, 0 }
		};

		option = getopt_long(argc, argv, "hVc:l:d:P:H:u:t:r:R:yvq",
				     long_options, NULL);
		if (option == -1)
			break;
		switch (option) {
		case 'h':
			dsp_config_print_help(argv[0]);
			exit(0);
		case 'V':
			printf("dsh-proxy %s\n", DSP_PROXY_VERSION);
			exit(0);
		case 'c':
			break;
		case 'l':
			if (set_listen(config, optarg, error, error_cap) < 0)
				return -1;
			break;
		case 'd':
			if (copy_option(config->dsh_command,
					sizeof(config->dsh_command), optarg,
					"--dsh-command", error, error_cap) < 0)
				return -1;
			break;
		case 'P':
			if (parse_int_range(optarg, 0, 65535,
					    &config->dsh_port) < 0) {
				snprintf(error, error_cap,
					 "--dsh-port must be 0..65535");
				return -1;
			}
			break;
		case 'H':
			if (copy_option(config->dsh_home,
					sizeof(config->dsh_home), optarg,
					"--dsh-home", error, error_cap) < 0)
				return -1;
			break;
		case 'u':
			if (copy_option(config->upstream,
					sizeof(config->upstream), optarg,
					"--upstream", error, error_cap) < 0)
				return -1;
			break;
		case 't':
			if (parse_int_range(optarg, 1, 86400,
					    &config->token_timeout) < 0) {
				snprintf(error, error_cap,
					 "--token-timeout must be 1..86400");
				return -1;
			}
			break;
		case 'r':
			if (parse_int_range(optarg, 1, 3600,
					    &config->restart_delay) < 0) {
				snprintf(error, error_cap,
					 "--restart-delay must be 1..3600");
				return -1;
			}
			break;
		case 'R':
			if (parse_int_range(optarg, 60, 2592000,
					    &config->cookie_refresh) < 0) {
				snprintf(error, error_cap,
					 "--cookie-refresh must be 60..2592000");
				return -1;
			}
			break;
		case 'y':
			config->auto_yes = 1;
			break;
		case 1000:
			config->auto_yes = 0;
			break;
		case 'v':
			config->log_level = DSP_LOG_DEBUG;
			break;
		case 'q':
			config->log_level = DSP_LOG_ERROR;
			break;
		case '?':
			snprintf(error, error_cap, "unknown or incomplete option: %s",
				 argv[optind - 1]);
			return -1;
		default:
			snprintf(error, error_cap, "invalid option");
			return -1;
		}
	}
	if (optind < argc) {
		snprintf(error, error_cap, "unexpected argument: %s",
			 argv[optind]);
		return -1;
	}
	if (config->listen_port[0] == '\0') {
		snprintf(error, error_cap, "LISTEN port is required");
		return -1;
	}
	if (config->dsh_command[0] == '\0') {
		snprintf(error, error_cap, "DSH_CMD is required");
		return -1;
	}
	return 0;
}

void dsp_config_print_help(const char *program)
{
	printf("Usage: %s [options]\n"
	       "\n"
	       "启动 dsh web，读取它打印的 token，并把本机监听端口上的 HTTP/WebSocket\n"
	       "请求带上认证信息转发给 dsh。\n"
	       "\n"
	       "Options:\n"
	       "  -c, --config FILE        配置文件 (默认 %s)\n"
	       "  -l, --listen IP:PORT     监听地址 (默认 127.0.0.1:8080)\n"
	       "  -d, --dsh-command CMD    启动 dsh 的命令 (默认 \"dsh web --no-open\")\n"
	       "  -P, --dsh-port PORT      dsh 端口 (默认 0，由系统分配空闲端口)\n"
	       "  -H, --dsh-home DIR       dsh 的 DSH_HOME (默认继承环境)\n"
	       "  -u, --upstream HOST:PORT 强制上游地址 (默认取自 dsh 打印的 URL)\n"
	       "  -t, --token-timeout SEC  等待 token 的告警时间 (默认 180)\n"
	       "  -r, --restart-delay SEC  dsh 退出后的重启间隔 (默认 5)\n"
	       "  -R, --cookie-refresh SEC 会话 cookie 刷新间隔 (默认 21600)\n"
	       "  -y, --auto-yes           自动对安装/更新提示回答 y (默认开启)\n"
	       "      --no-auto-yes        不自动回答提示\n"
	       "  -v, --verbose            调试日志\n"
	       "  -q, --quiet              只输出错误\n"
	       "  -h, --help               显示帮助\n"
	       "  -V, --version            显示版本\n"
	       "\n"
	       "配置文件使用 KEY=VALUE，支持的键：\n"
	       "  LISTEN, DSH_CMD, DSH_PORT, DSH_HOME, UPSTREAM, TOKEN_TIMEOUT,\n"
	       "  RESTART_DELAY, COOKIE_REFRESH, AUTO_YES, LOG_LEVEL\n",
	       program, DSP_CONFIG_DEFAULT_PATH);
}

void dsp_config_print(const dsp_config *config)
{
	log_info("listen=%s:%s upstream=%s", config->listen_host[0] == '\0'
						 ? "*"
						 : config->listen_host,
		 config->listen_port,
		 config->upstream[0] == '\0' ? "(from dsh output)"
					     : config->upstream);
	log_info("dsh_cmd=%s dsh_port=%d dsh_home=%s auto_yes=%d",
		 config->dsh_command, config->dsh_port,
		 config->dsh_home[0] == '\0' ? "(inherited)" : config->dsh_home,
		 config->auto_yes);
}
