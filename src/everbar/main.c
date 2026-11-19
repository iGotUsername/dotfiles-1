/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "everbar.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int
fail(const char *what)
{
	fprintf(stderr, "everbar: %s: %s\n", what, strerror(errno));
	return 1;
}

/* Only explicit power-menu actions spawn programs; never the status daemon. */
static bool
run(char *const argv[], const char *input, char *output, size_t capacity, unsigned timeout)
{
	int in[2] = {-1, -1}, out[2] = {-1, -1};
	if (pipe2(in, O_CLOEXEC)) return false;
	if (pipe2(out, O_CLOEXEC | O_NONBLOCK)) {
		close(in[0]); close(in[1]);
		return false;
	}
	/* The child writes in blocking mode; only the parent reader is nonblocking. */
	fcntl(out[1], F_SETFL, 0);
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
	posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
	pid_t pid;
	int error = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&actions);
	close(in[0]); close(out[1]);
	if (error) {
		close(in[1]); close(out[0]);
		fprintf(stderr, "everbar: %s: %s\n", argv[0], strerror(error));
		return false;
	}
	if (input) {
		size_t left = strlen(input);
		while (left) {
			ssize_t n = write(in[1], input, left);
			if (n < 0 && errno == EINTR) continue;
			if (n <= 0) break;
			input += n; left -= (size_t)n;
		}
	}
	close(in[1]);
	size_t used = 0;
	bool overflow = false, exited = false;
	int status = 0;
	uint64_t deadline = monotime_ms() + timeout;
	for (;;) {
		char buffer[256];
		ssize_t n;
		while ((n = read(out[0], buffer, sizeof buffer)) > 0) {
			if (capacity && (size_t)n < capacity - used) {
				memcpy(output + used, buffer, (size_t)n); used += (size_t)n;
			} else overflow = true;
		}
		pid_t result = waitpid(pid, &status, WNOHANG);
		if (result == pid) { exited = true; break; }
		if (result < 0 && errno != EINTR) break;
		uint64_t now = monotime_ms();
		if (now >= deadline) break;
		struct pollfd p = {out[0], POLLIN, 0};
		/* EOF can precede child exit: do not spin on POLLHUP. */
		if (n == 0) { p.fd = -1; }
		int delay = (int)(deadline - now);
		if (delay > 20) delay = 20;
		poll(&p, 1, delay);
	}
	if (!exited) {
		kill(pid, SIGKILL);
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
		fprintf(stderr, "everbar: %s failed or timed out\n", argv[0]);
	}
	/* Reap happened after the read; drain any final bytes written before exit. */
	char buffer[256];
	ssize_t n;
	while ((n = read(out[0], buffer, sizeof buffer)) > 0) {
		if (capacity && (size_t)n < capacity - used) {
			memcpy(output + used, buffer, (size_t)n); used += (size_t)n;
		} else overflow = true;
	}
	close(out[0]);
	if (capacity) output[used] = 0;
	return exited && !overflow && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int
power_menu(void)
{
	char choice[256];
	char *menu[] = {"fuzzel", "--dmenu", "--prompt", "Power > ", "--lines", "4", NULL};
	if (!run(menu, "Cancel\nPower off\nReboot\nHibernate\n", choice, sizeof choice, 120000)) return 0;
	size_t len = strlen(choice);
	while (len && isspace((unsigned char)choice[len - 1])) choice[--len] = 0;
	char *selected = choice;
	while (isspace((unsigned char)*selected)) selected++;
	const char *verb = !strcmp(selected, "Power off") ? "poweroff" :
	    !strcmp(selected, "Reboot") ? "reboot" : !strcmp(selected, "Hibernate") ? "hibernate" : NULL;
	if (!verb) return 0;
	char *command[] = {"loginctl", (char *)verb, NULL};
	char result[1024];
	return run(command, NULL, result, sizeof result, 5000) ? 0 : 1;
}

static bool
address(struct sockaddr_un *addr, const char *path)
{
	memset(addr, 0, sizeof *addr);
	addr->sun_family = AF_UNIX;
	if (strlen(path) >= sizeof addr->sun_path) { errno = ENAMETOOLONG; return false; }
	strcpy(addr->sun_path, path);
	return true;
}

static bool
send_action(const char *path, const char *action)
{
	struct sockaddr_un addr;
	if (!address(&addr, path)) return false;
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) return false;
	bool ok = sendto(fd, action, strlen(action), MSG_NOSIGNAL, (struct sockaddr *)&addr, sizeof addr) == (ssize_t)strlen(action);
	int error = errno;
	close(fd);
	errno = error;
	return ok;
}

struct peer {
	char name[256];
	ino_t ino;
	struct timespec ctime;
	uint64_t status_version, title_version;
	bool seen;
};

static bool
send_text(const char *path, const char *kind, const char *text)
{
	struct sockaddr_un addr;
	if (!address(&addr, path)) return false;
	char buffer[TEXT_CAP];
	int n = snprintf(buffer, sizeof buffer, "all %s %s", kind, text);
	if (n < 0 || (size_t)n >= sizeof buffer) return false;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) return false;
	bool ok = connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0 &&
	    send(fd, buffer, (size_t)n, MSG_NOSIGNAL) == n;
	close(fd);
	return ok;
}

static void
publish(struct peer *peers, size_t count, const char *runtime, const char *status, uint64_t sv, const char *title, uint64_t tv)
{
	char directory[PATH_MAX];
	if (snprintf(directory, sizeof directory, "%s/dwlb", runtime) >= (int)sizeof directory) return;
	DIR *dir = opendir(directory);
	if (!dir) return;
	for (size_t i = 0; i < count; i++) peers[i].seen = false;
	struct dirent *de;
	while ((de = readdir(dir))) {
		if (strncmp(de->d_name, "dwlb-", 5)) continue;
		struct stat st;
		if (fstatat(dirfd(dir), de->d_name, &st, AT_SYMLINK_NOFOLLOW) ||
		    !S_ISSOCK(st.st_mode) || st.st_uid != getuid()) continue;
		struct peer *p = NULL;
		for (size_t i = 0; i < count; i++) if (!strcmp(peers[i].name, de->d_name)) { p = &peers[i]; break; }
		if (!p) for (size_t i = 0; i < count; i++) if (!peers[i].name[0]) { p = &peers[i]; break; }
		if (!p) continue;
		if (strcmp(p->name, de->d_name) || p->ino != st.st_ino ||
		    p->ctime.tv_sec != st.st_ctim.tv_sec || p->ctime.tv_nsec != st.st_ctim.tv_nsec) {
			memset(p, 0, sizeof *p);
			snprintf(p->name, sizeof p->name, "%s", de->d_name);
			p->ino = st.st_ino;
			p->ctime = st.st_ctim;
		}
		p->seen = true;
		char path[PATH_MAX];
		if (snprintf(path, sizeof path, "%s/%s", directory, de->d_name) >= (int)sizeof path) continue;
		if (p->title_version != tv && send_text(path, "title", title)) p->title_version = tv;
		if (p->status_version != sv && send_text(path, "status", status)) p->status_version = sv;
	}
	closedir(dir);
	for (size_t i = 0; i < count; i++) if (!peers[i].seen) memset(&peers[i], 0, sizeof peers[i]);
}

static bool
private_runtime(const char *runtime)
{
	struct stat st;
	if (mkdir(runtime, 0700) && errno != EEXIST) return false;
	if (lstat(runtime, &st)) return false;
	if (!S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 0022)) { errno = EACCES; return false; }
	return true;
}

static int
daemon_main(const char *runtime, const char *socket_path, const char *action)
{
	if (!private_runtime(runtime)) return fail("unsafe or inaccessible runtime directory");
	char lock_path[PATH_MAX];
	if (snprintf(lock_path, sizeof lock_path, "%s/everbar-refresh.lock", runtime) >= (int)sizeof lock_path) {
		errno = ENAMETOOLONG; return fail("lock path");
	}
	int lock = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
	if (lock < 0) return fail("open lock");
	struct stat st;
	if (fstat(lock, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 0077)) {
		close(lock); errno = EACCES; return fail("lock must be a private regular file owned by you");
	}
	if (flock(lock, LOCK_EX | LOCK_NB)) {
		int error = errno;
		close(lock);
		if (error == EWOULDBLOCK) return send_action(socket_path, "refresh") ? 0 : fail("wake existing instance");
		errno = error; return fail("lock");
	}
	struct sockaddr_un addr;
	if (!address(&addr, socket_path)) { close(lock); return fail("refresh socket path"); }
	if (!lstat(socket_path, &st)) {
		if (!S_ISSOCK(st.st_mode) || st.st_uid != getuid()) {
			close(lock); errno = EACCES; return fail("refusing to replace non-owned/non-socket path");
		}
		if (unlink(socket_path)) { close(lock); return fail("remove stale socket"); }
	} else if (errno != ENOENT) { close(lock); return fail("inspect socket"); }
	int sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (sock < 0) { close(lock); return fail("socket"); }
	int enabled = 1;
	mode_t old_mask = umask(0077);
	int result = setsockopt(sock, SOL_SOCKET, SO_PASSCRED, &enabled, sizeof enabled);
	if (!result) result = bind(sock, (struct sockaddr *)&addr, sizeof addr);
	if (!result) result = chmod(socket_path, 0600);
	umask(old_mask);
	if (result) { close(sock); close(lock); return fail("bind refresh socket"); }
	struct stat bound;
	if (lstat(socket_path, &bound)) { close(sock); close(lock); return fail("inspect bound socket"); }
	sigset_t signals;
	sigemptyset(&signals);
	sigaddset(&signals, SIGINT); sigaddset(&signals, SIGTERM); sigaddset(&signals, SIGHUP);
	sigprocmask(SIG_BLOCK, &signals, NULL);
	int sigfd = signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK);
	struct status s = {.cpu = -1, .temp = -1, .battery = -1, .volume = -1, .ram = "--"};
	struct services *services = services_new(&s);
	if (sigfd < 0 || !services) {
		fprintf(stderr, "everbar: cannot initialize event loop (check PipeWire client configuration)\n");
		result = 1;
		goto cleanup;
	}
	struct peer peers[64] = {0};
	char status[TEXT_CAP] = "", title[128] = "";
	uint64_t sv = 0, tv = 0, next_sample = 0, next_battery = 0;
	struct cpu_sample previous = read_cpu("");
	bool running = true, force = true;
	while (running) {
		uint64_t now = monotime_ms();
		services_step(services, now);
		now = monotime_ms();
		bool sample_due = now >= next_sample;
		if (sample_due) {
			struct cpu_sample current = read_cpu("");
			s.cpu = cpu_percent(previous, current);
			previous = current;
			read_sensors(&s, "", now >= next_battery);
			if (now >= next_battery) next_battery = now + 10000;
			next_sample = now + 2000;
		}
		char new_status[TEXT_CAP], new_title[128];
		if (!render_status(new_status, sizeof new_status - 12, &s, action)) {
			fprintf(stderr, "everbar: status exceeds dwlb protocol limit\n"); result = 1; break;
		}
		time_t wall = time(NULL);
		render_clock(new_title, sizeof new_title, wall);
		bool changed = false;
		if (strcmp(status, new_status)) { strcpy(status, new_status); sv++; changed = true; }
		if (strcmp(title, new_title)) { strcpy(title, new_title); tv++; changed = true; }
		if (changed || sample_due || force) publish(peers, 64, runtime, status, sv, title, tv);
		force = false;
		uint64_t deadline = services_deadline(services);
		if (next_sample < deadline) deadline = next_sample;
		uint64_t minute = now + (uint64_t)(60 - wall % 60) * 1000;
		if (minute < deadline) deadline = minute;
		now = monotime_ms();
		int timeout = deadline <= now ? 0 : deadline - now > INT_MAX ? INT_MAX : (int)(deadline - now);
		struct pollfd fds[] = {{sock, POLLIN, 0}, {sigfd, POLLIN, 0}, {services_fd(services), POLLIN, 0}};
		if (poll(fds, 3, timeout) < 0) {
			if (errno == EINTR) continue;
			result = fail("poll"); break;
		}
		if (fds[1].revents) running = false;
		if (fds[0].revents & POLLIN) {
			/* Bound work per turn so a click storm cannot starve signals/audio. */
			for (unsigned i = 0; i < 64; i++) {
				char command[32], control[CMSG_SPACE(sizeof(struct ucred))];
				struct iovec iov = {command, sizeof command - 1};
				struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof control};
				ssize_t n = recvmsg(sock, &msg, MSG_DONTWAIT);
				if (n < 0) break;
				if (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) continue;
				bool owned = false;
				for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
					if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_CREDENTIALS && c->cmsg_len >= CMSG_LEN(sizeof(struct ucred))) {
						struct ucred cred;
						memcpy(&cred, CMSG_DATA(c), sizeof cred);
						owned = cred.uid == getuid();
					}
				}
				if (!owned) continue;
				command[n] = 0;
				if (!strcmp(command, "refresh")) { next_sample = 0; next_battery = 0; force = true; }
				else services_action(services, command);
			}
		}
	}
cleanup:
	services_free(services);
	if (sigfd >= 0) close(sigfd);
	close(sock);
	if (!lstat(socket_path, &st) && st.st_ino == bound.st_ino && st.st_dev == bound.st_dev) unlink(socket_path);
	/* Keep the lock inode: deleting it would permit split ownership. */
	close(lock);
	return result;
}

int
main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);
	setenv("TZ", EVERBAR_TIMEZONE, 1);
	tzset();
	if (argc == 2 && !strcmp(argv[1], "--help")) {
		puts("Usage: everbar [wifi|bluetooth|mute|power|refresh|--once|--help]\n"
		     "No arguments: run the status daemon. --once: print local sensors (no services).");
		return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "power")) return power_menu();
	char executable[PATH_MAX], action[TEXT_CAP];
	ssize_t length = readlink("/proc/self/exe", executable, sizeof executable - 1);
	if (length < 0 || (size_t)length >= sizeof executable - 1) return fail("resolve executable");
	executable[length] = 0;
	if (!quote_action(action, sizeof action, executable)) {
		fprintf(stderr, "everbar: executable path cannot be represented safely in dwlb markup\n"); return 1;
	}
	if (argc == 2 && !strcmp(argv[1], "--once")) {
		struct status s = {.cpu = -1, .temp = -1, .battery = -1, .volume = -1, .ram = "--"};
		char text[TEXT_CAP], clock[128];
		read_sensors(&s, "", true);
		if (!render_status(text, sizeof text, &s, action)) return 1;
		render_clock(clock, sizeof clock, time(NULL));
		printf("%s\n%s\n", clock, text);
		return 0;
	}
	if (argc > 2 || (argc == 2 && strcmp(argv[1], "wifi") && strcmp(argv[1], "bluetooth") && strcmp(argv[1], "mute") && strcmp(argv[1], "refresh"))) {
		fprintf(stderr, "everbar: unknown argument; see --help\n"); return 1;
	}
	char fallback[PATH_MAX], socket_path[PATH_MAX];
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (!runtime || !*runtime) {
		const char *home = getenv("HOME");
		if (!home || snprintf(fallback, sizeof fallback, "%s/.cache", home) >= (int)sizeof fallback) {
			fprintf(stderr, "everbar: HOME or XDG_RUNTIME_DIR is required\n"); return 1;
		}
		runtime = fallback;
	}
	if (snprintf(socket_path, sizeof socket_path, "%s/everbar-refresh.sock", runtime) >= (int)sizeof socket_path) {
		errno = ENAMETOOLONG; return fail("runtime path");
	}
	if (argc == 2) return send_action(socket_path, argv[1]) ? 0 : fail("contact running everbar");
	return daemon_main(runtime, socket_path, action);
}
