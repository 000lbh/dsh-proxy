#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "log.h"
#include "proxy.h"
#include "supervisor.h"
#include "upstream.h"
#include "util.h"

static volatile sig_atomic_t g_stop;
static int g_signal_pipe[2] = { -1, -1 };

static void handle_signal(int signal_number)
{
	unsigned char byte = (unsigned char)signal_number;

	g_stop = 1;
	if (g_signal_pipe[1] >= 0) {
		ssize_t ignored = write(g_signal_pipe[1], &byte, 1);

		(void)ignored;
	}
}

static int install_signals(void)
{
	struct sigaction action;

	if (pipe2(g_signal_pipe, O_CLOEXEC | O_NONBLOCK) < 0) {
		log_error("cannot create signal pipe: %s", strerror(errno));
		return -1;
	}
	memset(&action, 0, sizeof(action));
	action.sa_handler = handle_signal;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	if (sigaction(SIGTERM, &action, NULL) < 0 ||
	    sigaction(SIGINT, &action, NULL) < 0) {
		log_error("cannot install signal handlers: %s", strerror(errno));
		close(g_signal_pipe[0]);
		close(g_signal_pipe[1]);
		g_signal_pipe[0] = g_signal_pipe[1] = -1;
		return -1;
	}
	signal(SIGPIPE, SIG_IGN);
	return 0;
}

int main(int argc, char **argv)
{
	dsp_config config;
	dsp_upstream *upstream;
	dsp_proxy *proxy;
	dsp_supervisor *supervisor;
	char error[512];
	int exit_code = 0;

	dsp_config_defaults(&config);
	error[0] = '\0';
	if (dsp_config_parse_args(&config, argc, argv, error, sizeof(error)) < 0) {
		fprintf(stderr, "dsh-proxy: %s\n", error);
		fprintf(stderr, "Try '%s --help' for usage.\n", argv[0]);
		return 2;
	}
	if (geteuid() == 0) {
		fprintf(stderr,
			"dsh-proxy: 拒绝以 root 运行。dsh 能执行 shell 命令，必须以普通用户身份运行。\n"
			"请使用 systemd --user（scripts/install-user.sh），\n"
			"或 systemd system unit 并设置 User=<你的用户名>，\n"
			"不要使用 sudo 直接启动 dsh-proxy。\n");
		return 1;
	}
	dsp_log_set_level(config.log_level);
	dsp_config_print(&config);

	upstream = dsp_upstream_create(config.upstream, error, sizeof(error));
	if (upstream == NULL) {
		log_error("%s", error);
		return 1;
	}
	proxy = dsp_proxy_create(&config, upstream, error, sizeof(error));
	if (proxy == NULL) {
		log_error("%s", error);
		dsp_upstream_destroy(upstream);
		return 1;
	}
	supervisor = dsp_supervisor_create(&config, upstream);
	if (supervisor == NULL) {
		log_error("out of memory");
		dsp_proxy_stop(proxy);
		dsp_upstream_destroy(upstream);
		return 1;
	}
	if (install_signals() < 0) {
		dsp_proxy_stop(proxy);
		dsp_supervisor_destroy(supervisor);
		if (dsp_proxy_destroy(proxy))
			dsp_upstream_destroy(upstream);
		return 1;
	}

	dsp_supervisor_start(supervisor);
	log_info("dsh-proxy is running; press Ctrl+C to stop");

	while (!g_stop) {
		struct pollfd fds[3];
		int rc;

		fds[0].fd = dsp_proxy_fd(proxy);
		fds[0].events = POLLIN;
		fds[0].revents = 0;
		fds[1].fd = dsp_supervisor_fd(supervisor);
		fds[1].events = POLLIN;
		fds[1].revents = 0;
		fds[2].fd = g_signal_pipe[0];
		fds[2].events = POLLIN;
		fds[2].revents = 0;

		rc = poll(fds, 3, 500);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			log_error("poll failed: %s", strerror(errno));
			exit_code = 1;
			break;
		}
		if (fds[0].revents & POLLIN)
			dsp_proxy_accept(proxy);
		if (fds[1].revents & (POLLIN | POLLHUP))
			dsp_supervisor_read(supervisor);
		if (fds[2].revents & POLLIN) {
			char drain[32];

			while (read(g_signal_pipe[0], drain, sizeof(drain)) > 0)
				;
		}
		dsp_supervisor_tick(supervisor);
		dsp_upstream_refresh_if_due(upstream, config.cookie_refresh);
	}

	log_info("shutting down");
	dsp_proxy_stop(proxy);
	dsp_supervisor_stop(supervisor);
	dsp_upstream_shutdown(upstream);
	dsp_supervisor_destroy(supervisor);
	if (dsp_proxy_destroy(proxy))
		dsp_upstream_destroy(upstream);
	if (g_signal_pipe[0] >= 0) {
		close(g_signal_pipe[0]);
		close(g_signal_pipe[1]);
	}
	return exit_code;
}
