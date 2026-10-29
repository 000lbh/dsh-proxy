#define _GNU_SOURCE

#include "supervisor.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "net.h"
#include "util.h"

extern char **environ;

struct dsp_supervisor {
	const dsp_config *config;
	dsp_upstream *upstream;

	pid_t pid;
	int out_fd;
	int in_fd;
	char **child_env;

	int stopping;
	int warned_token_timeout;
	int warned_prompt;

	uint64_t spawn_ms;
	uint64_t last_answer_ms;
	uint64_t next_restart_ms;
	int restart_delay_ms;

	dsp_buf output;
	char prompt_scan[2048];
	size_t prompt_scan_len;
	char last_token_url[2048];
};

static void close_child_fds(dsp_supervisor *supervisor)
{
	if (supervisor->out_fd >= 0) {
		close(supervisor->out_fd);
		supervisor->out_fd = -1;
	}
	if (supervisor->in_fd >= 0) {
		close(supervisor->in_fd);
		supervisor->in_fd = -1;
	}
}

static const char *describe_status(int status)
{
	static char text[128];

	if (WIFEXITED(status)) {
		snprintf(text, sizeof(text), "exit code %d",
			 WEXITSTATUS(status));
	} else if (WIFSIGNALED(status)) {
		snprintf(text, sizeof(text), "signal %d (%s)",
			 WTERMSIG(status), strsignal(WTERMSIG(status)));
	} else {
		snprintf(text, sizeof(text), "status 0x%x", status);
	}
	return text;
}

static uint64_t monotonic_ms(void)
{
	return dsp_now_ms();
}

static int write_prompt_answer(dsp_supervisor *supervisor)
{
	ssize_t n;

	if (supervisor->in_fd < 0)
		return -1;
	do {
		n = write(supervisor->in_fd, "y\n", 2);
	} while (n < 0 && errno == EINTR);
	if (n != 2) {
		log_warn("could not answer dsh prompt: %s", strerror(errno));
		return -1;
	}
	return 0;
}

static int command_has_port(const char *command)
{
	const char *cursor = command;

	while ((cursor = strstr(cursor, "--port")) != NULL) {
		int before_ok = cursor == command ||
				isspace((unsigned char)cursor[-1]);
		char after = cursor[6];
		int after_ok = after == '\0' || after == '=' ||
			       isspace((unsigned char)after);

		if (before_ok && after_ok)
			return 1;
		cursor += 6;
	}
	return 0;
}

static int build_dsh_command(const dsp_config *config, char *out,
			     size_t out_cap)
{
	int written;

	if (command_has_port(config->dsh_command))
		written = snprintf(out, out_cap, "%s", config->dsh_command);
	else
		written = snprintf(out, out_cap, "%s --port %d",
				   config->dsh_command, config->dsh_port);
	if (written < 0 || (size_t)written >= out_cap)
		return -1;
	return 0;
}


static const char *const scrubbed_environment_names[] = {
	"DISPLAY",
	"WAYLAND_DISPLAY",
	"WAYLAND_SOCKET",
	"XAUTHORITY",
	"XDG_SESSION_TYPE",
	"XDG_SESSION_ID",
	"XDG_SESSION_DESKTOP",
	"XDG_SESSION_PATH",
	"XDG_SESSION_CLASS",
	"XDG_SEAT",
	"XDG_SEAT_PATH",
	"XDG_CURRENT_DESKTOP",
	"XDG_RUNTIME_DIR",
	"DESKTOP_SESSION",
	"GDMSESSION",
	"GNOME_DESKTOP_SESSION_ID",
	"GNOME_SHELL_SESSION_MODE",
	"KDE_FULL_SESSION",
	"KDE_SESSION_VERSION",
	"KDE_SESSION_UID",
	"DBUS_SESSION_BUS_ADDRESS",
	"DBUS_SYSTEM_BUS_ADDRESS",
	"DBUS_STARTER_ADDRESS",
	"DBUS_STARTER_BUS_TYPE",
	"AT_SPI_BUS_ADDRESS",
	"AT_SPI_BUS",
	"XDG_ACTIVATION_TOKEN",
	"DESKTOP_STARTUP_ID",
	"QT_QPA_PLATFORM",
	"QT_QPA_PLATFORMTHEME",
	"GDK_BACKEND",
	"BROWSER",
	"PULSE_SERVER",
	"PIPEWIRE_REMOTE",
};

static int environment_name_equals(const char *entry, const char *name)
{
	size_t len = strlen(name);

	return strncmp(entry, name, len) == 0 && entry[len] == '=';
}

static int environment_name_is_scrubbed(const char *entry)
{
	size_t i;

	for (i = 0; i < sizeof(scrubbed_environment_names) /
				sizeof(scrubbed_environment_names[0]);
	     i++) {
		if (environment_name_equals(entry,
					    scrubbed_environment_names[i]))
			return 1;
	}
	return 0;
}

static void free_child_environment(char **environment)
{
	size_t i;

	if (environment == NULL)
		return;
	for (i = 0; environment[i] != NULL; i++)
		free(environment[i]);
	free(environment);
}

static char **build_child_environment(const dsp_config *config,
				      size_t *count_out)
{
	size_t total = 0;
	size_t out = 0;
	size_t i;
	char **environment;
	int override_home = config->dsh_home[0] != '\0';

	if (environ != NULL) {
		while (environ[total] != NULL)
			total++;
	}
	environment = calloc(total + 2, sizeof(*environment));
	if (environment == NULL)
		return NULL;

	for (i = 0; i < total; i++) {
		const char *entry = environ[i];

		if (environment_name_is_scrubbed(entry))
			continue;
		if (override_home && environment_name_equals(entry, "DSH_HOME"))
			continue;
		environment[out] = dsp_strdup(entry);
		if (environment[out] == NULL) {
			free_child_environment(environment);
			return NULL;
		}
		out++;
	}
	if (override_home) {
		size_t length = strlen("DSH_HOME=") +
				strlen(config->dsh_home) + 1;

		environment[out] = malloc(length);
		if (environment[out] == NULL) {
			free_child_environment(environment);
			return NULL;
		}
		snprintf(environment[out], length, "DSH_HOME=%s",
			 config->dsh_home);
		out++;
	}
	environment[out] = NULL;
	*count_out = out;
	return environment;
}

static int spawn_child(dsp_supervisor *supervisor)
{
	int in_pipe[2] = { -1, -1 };
	int out_pipe[2] = { -1, -1 };
	pid_t pid;
	char command[4096];
	size_t environment_count = 0;

	if (build_dsh_command(supervisor->config, command, sizeof(command)) < 0) {
		log_error("dsh command is too long after adding --port");
		return -1;
	}
	free_child_environment(supervisor->child_env);
	supervisor->child_env = build_child_environment(supervisor->config,
							 &environment_count);
	if (supervisor->child_env == NULL) {
		log_error("cannot build a scrubbed environment for dsh");
		return -1;
	}
	log_debug("dsh environment: %zu variables (desktop, Wayland, X11, "
		  "session and D-Bus variables scrubbed)", environment_count);
	if (pipe(in_pipe) < 0) {
		log_error("cannot create dsh stdin pipe: %s", strerror(errno));
		return -1;
	}
	if (pipe(out_pipe) < 0) {
		log_error("cannot create dsh output pipe: %s", strerror(errno));
		close(in_pipe[0]);
		close(in_pipe[1]);
		return -1;
	}
	dsp_set_cloexec(in_pipe[0]);
	dsp_set_cloexec(in_pipe[1]);
	dsp_set_cloexec(out_pipe[0]);
	dsp_set_cloexec(out_pipe[1]);

	fflush(NULL);
	pid = fork();
	if (pid < 0) {
		log_error("cannot fork dsh: %s", strerror(errno));
		close(in_pipe[0]);
		close(in_pipe[1]);
		close(out_pipe[0]);
		close(out_pipe[1]);
		return -1;
	}
	if (pid == 0) {
		(void)setpgid(0, 0);
		if (dup2(in_pipe[0], STDIN_FILENO) < 0 ||
		    dup2(out_pipe[1], STDOUT_FILENO) < 0 ||
		    dup2(out_pipe[1], STDERR_FILENO) < 0)
			_exit(127);
		close(in_pipe[0]);
		close(in_pipe[1]);
		close(out_pipe[0]);
		close(out_pipe[1]);
		signal(SIGTERM, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		signal(SIGHUP, SIG_DFL);
		{
			char *const arguments[] = {
				(char *)"sh",
				(char *)"-c",
				command,
				NULL
			};

			execve("/bin/sh", arguments, supervisor->child_env);
		}
		_exit(127);
	}

	(void)setpgid(pid, pid);
	close(in_pipe[0]);
	close(out_pipe[1]);
	dsp_set_nonblocking(out_pipe[0], 1);
	dsp_set_cloexec(in_pipe[1]);
	dsp_set_cloexec(out_pipe[0]);

	supervisor->pid = pid;
	supervisor->in_fd = in_pipe[1];
	supervisor->out_fd = out_pipe[0];
	supervisor->spawn_ms = monotonic_ms();
	supervisor->warned_token_timeout = 0;
	supervisor->warned_prompt = 0;
	supervisor->last_answer_ms = 0;
	supervisor->prompt_scan_len = 0;
	supervisor->last_token_url[0] = '\0';
	supervisor->output.len = 0;
	if (supervisor->output.data != NULL)
		supervisor->output.data[0] = '\0';

	log_info("started dsh: %s (pid=%ld)", command, (long)pid);
	return 0;
}

dsp_supervisor *dsp_supervisor_create(const dsp_config *config,
				      dsp_upstream *upstream)
{
	dsp_supervisor *supervisor = calloc(1, sizeof(*supervisor));

	if (supervisor == NULL)
		return NULL;
	supervisor->config = config;
	supervisor->upstream = upstream;
	supervisor->pid = 0;
	supervisor->out_fd = -1;
	supervisor->in_fd = -1;
	supervisor->restart_delay_ms = config->restart_delay * 1000;
	dsp_buf_init(&supervisor->output);
	return supervisor;
}

void dsp_supervisor_start(dsp_supervisor *supervisor)
{
	if (supervisor == NULL || supervisor->pid > 0)
		return;
	if (spawn_child(supervisor) < 0) {
		supervisor->next_restart_ms =
			monotonic_ms() + (uint64_t)supervisor->restart_delay_ms;
	}
}

int dsp_supervisor_fd(dsp_supervisor *supervisor)
{
	return supervisor == NULL ? -1 : supervisor->out_fd;
}

static int extract_url(const char *line, char *out, size_t out_cap)
{
	const char *cursor = line;
	char fallback[2048];
	int have_fallback = 0;

	if (line == NULL)
		return -1;
	fallback[0] = '\0';
	while (cursor != NULL && *cursor != '\0') {
		const char *start = strstr(cursor, "http://");
		const char *secure = strstr(cursor, "https://");
		const char *end;
		size_t len;

		if (secure != NULL && (start == NULL || secure < start))
			start = secure;
		if (start == NULL)
			break;
		end = start;
		while (*end != '\0' && !isspace((unsigned char)*end) &&
		       *end != '"' && *end != '\'' && *end != '<' &&
		       *end != '>' && *end != ')' && *end != ']' &&
		       *end != '}')
			end++;
		while (end > start && (end[-1] == '.' || end[-1] == ',' ||
				       end[-1] == ';' || end[-1] == ':'))
			end--;
		len = (size_t)(end - start);
		if (len > 0 && len + 1 <= sizeof(fallback)) {
			char candidate[2048];

			memcpy(candidate, start, len);
			candidate[len] = '\0';
			if (strstr(candidate, "token=") != NULL &&
			    len + 1 <= out_cap) {
				memcpy(out, candidate, len + 1);
				return 0;
			}
			if (!have_fallback && len + 1 <= sizeof(fallback)) {
				memcpy(fallback, candidate, len + 1);
				have_fallback = 1;
			}
		}
		if (end <= start)
			break;
		cursor = end;
	}
	if (have_fallback && strlen(fallback) + 1 <= out_cap) {
		strcpy(out, fallback);
		return 0;
	}
	return -1;
}

static int looks_like_prompt(const char *text, size_t len)
{
	static const char *patterns[] = {
		"ok to proceed",
		"do you want to",
		"would you like to",
		"are you sure",
		"proceed?",
		"continue?",
		"update now?",
		"install now?",
		"overwrite?",
		"(y/n)",
		"[y/n]",
		"(y/n)?",
		"[y/n]?",
		"y/n)",
		"y/n]",
		"yes/no",
	};
	char cleaned[2048];
	char lower[2048];
	size_t i;
	size_t copy = len < sizeof(cleaned) - 1 ? len : sizeof(cleaned) - 1;

	memcpy(cleaned, text, copy);
	cleaned[copy] = '\0';
	dsp_strip_ansi(cleaned);
	copy = strlen(cleaned);
	if (copy >= sizeof(lower))
		copy = sizeof(lower) - 1;
	for (i = 0; i < copy; i++)
		lower[i] = (char)tolower((unsigned char)cleaned[i]);
	lower[copy] = '\0';
	for (i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
		if (strstr(lower, patterns[i]) != NULL)
			return 1;
	}
	return 0;
}

static void scan_prompt(dsp_supervisor *supervisor, const char *data,
			size_t len)
{
	size_t cap = sizeof(supervisor->prompt_scan) - 1;
	size_t room;

	if (len > cap) {
		data += len - cap;
		len = cap;
		supervisor->prompt_scan_len = 0;
	}
	if (supervisor->prompt_scan_len + len > cap) {
		size_t discard = supervisor->prompt_scan_len + len - cap;

		if (discard > supervisor->prompt_scan_len)
			discard = supervisor->prompt_scan_len;
		memmove(supervisor->prompt_scan,
			supervisor->prompt_scan + discard,
			supervisor->prompt_scan_len - discard);
		supervisor->prompt_scan_len -= discard;
	}
	room = cap - supervisor->prompt_scan_len;
	if (len > room)
		len = room;
	memcpy(supervisor->prompt_scan + supervisor->prompt_scan_len, data, len);
	supervisor->prompt_scan_len += len;
	supervisor->prompt_scan[supervisor->prompt_scan_len] = '\0';

	if (!looks_like_prompt(supervisor->prompt_scan,
			       supervisor->prompt_scan_len))
		return;
	if (!supervisor->config->auto_yes) {
		if (!supervisor->warned_prompt) {
			log_warn("dsh is waiting for an answer, but AUTO_YES=0");
			supervisor->warned_prompt = 1;
		}
		return;
	}
	{
		uint64_t now = monotonic_ms();

		if (now - supervisor->last_answer_ms < 2000)
			return;
		if (write_prompt_answer(supervisor) == 0) {
			supervisor->last_answer_ms = now;
			log_info("auto-answered a dsh install/update prompt with 'y'");
			supervisor->prompt_scan_len = 0;
			supervisor->prompt_scan[0] = '\0';
		}
	}
}

static void handle_line(dsp_supervisor *supervisor, char *line)
{
	char redacted[8192];
	char url[2048];

	dsp_strip_ansi(line);
	dsp_trim(line);
	if (line[0] == '\0')
		return;
	dsp_redact_token(line, redacted, sizeof(redacted));
	log_info("dsh: %s", redacted);
	if (strstr(line, "token=") != NULL &&
	    extract_url(line, url, sizeof(url)) == 0) {
		if (strcmp(url, supervisor->last_token_url) != 0) {
			snprintf(supervisor->last_token_url,
				 sizeof(supervisor->last_token_url), "%s", url);
			dsp_upstream_set_token(supervisor->upstream, url);
		}
	}
}

static void process_output(dsp_supervisor *supervisor)
{
	for (;;) {
		char *newline;
		size_t line_len;
		char *line;

		newline = memchr(supervisor->output.data, '\n',
				 supervisor->output.len);
		if (newline == NULL)
			break;
		line_len = (size_t)(newline - supervisor->output.data);
		line = dsp_strndup(supervisor->output.data, line_len);
		if (line == NULL)
			return;
		handle_line(supervisor, line);
		free(line);
		memmove(supervisor->output.data, newline + 1,
			supervisor->output.len - line_len - 1);
		supervisor->output.len -= line_len + 1;
		supervisor->output.data[supervisor->output.len] = '\0';
	}
	if (supervisor->output.len > 128 * 1024) {
		log_warn("dsh produced a very long line; truncating output buffer");
		supervisor->output.len = 0;
		if (supervisor->output.data != NULL)
			supervisor->output.data[0] = '\0';
	}
}

void dsp_supervisor_read(dsp_supervisor *supervisor)
{
	char buffer[8192];
	size_t total = 0;

	if (supervisor == NULL || supervisor->out_fd < 0)
		return;
	for (;;) {
		ssize_t n = read(supervisor->out_fd, buffer, sizeof(buffer));

		if (n > 0) {
			if (dsp_buf_append(&supervisor->output, buffer,
					   (size_t)n) < 0)
				return;
			scan_prompt(supervisor, buffer, (size_t)n);
			total += (size_t)n;
			if (total >= 128 * 1024)
				break;
			continue;
		}
		if (n == 0) {
			close(supervisor->out_fd);
			supervisor->out_fd = -1;
			break;
		}
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			break;
		log_warn("read from dsh failed: %s", strerror(errno));
		close(supervisor->out_fd);
		supervisor->out_fd = -1;
		break;
	}
	process_output(supervisor);
}

void dsp_supervisor_tick(dsp_supervisor *supervisor)
{
	uint64_t now;

	if (supervisor == NULL)
		return;
	now = monotonic_ms();

	if (supervisor->pid > 0) {
		int status = 0;
		pid_t result = waitpid(supervisor->pid, &status, WNOHANG);

		if (result == supervisor->pid ||
		    (result < 0 && errno == ECHILD)) {
			if (result == supervisor->pid)
				log_warn("dsh exited: %s", describe_status(status));
			else
				log_warn("dsh process disappeared");
			supervisor->pid = 0;
			close_child_fds(supervisor);
			dsp_upstream_child_lost(supervisor->upstream);
			if (!supervisor->stopping) {
				uint64_t ran = now - supervisor->spawn_ms;
				int base = supervisor->config->restart_delay * 1000;

				if (ran > 60000) {
					supervisor->restart_delay_ms = base;
				} else if (supervisor->restart_delay_ms < base) {
					supervisor->restart_delay_ms = base;
				} else if (supervisor->restart_delay_ms < 30000) {
					supervisor->restart_delay_ms *= 2;
					if (supervisor->restart_delay_ms > 30000)
						supervisor->restart_delay_ms = 30000;
				}
				supervisor->next_restart_ms =
					now + (uint64_t)supervisor->restart_delay_ms;
				log_info("restarting dsh in %.1f seconds",
					 supervisor->restart_delay_ms / 1000.0);
			}
		}
	}

	if (supervisor->pid == 0 && !supervisor->stopping &&
	    supervisor->next_restart_ms != 0 &&
	    now >= supervisor->next_restart_ms) {
		supervisor->next_restart_ms = 0;
		if (spawn_child(supervisor) < 0)
			supervisor->next_restart_ms =
				now + (uint64_t)supervisor->restart_delay_ms;
	}

	if (supervisor->pid > 0 && !supervisor->warned_token_timeout &&
	    now - supervisor->spawn_ms >
		    (uint64_t)supervisor->config->token_timeout * 1000ULL) {
		log_warn("dsh has not printed a token URL after %d seconds; "
			 "it may be installing or updating. Still waiting.",
			 supervisor->config->token_timeout);
		supervisor->warned_token_timeout = 1;
	}
}

void dsp_supervisor_stop(dsp_supervisor *supervisor)
{
	int waited = 0;

	if (supervisor == NULL)
		return;
	supervisor->stopping = 1;
	if (supervisor->pid > 0) {
		pid_t pid = supervisor->pid;

		(void)kill(-pid, SIGTERM);
		(void)kill(pid, SIGTERM);
		while (waited < 5000) {
			int status = 0;
			pid_t result = waitpid(pid, &status, WNOHANG);

			if (result == pid || (result < 0 && errno == ECHILD)) {
				supervisor->pid = 0;
				break;
			}
			usleep(100 * 1000);
			waited += 100;
		}
		if (supervisor->pid > 0) {
			log_warn("dsh did not stop in time; sending SIGKILL");
			(void)kill(-pid, SIGKILL);
			(void)kill(pid, SIGKILL);
			(void)waitpid(pid, NULL, 0);
			supervisor->pid = 0;
		}
	}
	close_child_fds(supervisor);
}

void dsp_supervisor_destroy(dsp_supervisor *supervisor)
{
	if (supervisor == NULL)
		return;
	if (supervisor->pid > 0)
		dsp_supervisor_stop(supervisor);
	free_child_environment(supervisor->child_env);
	dsp_buf_free(&supervisor->output);
	free(supervisor);
}
