#define _GNU_SOURCE
#include "ipc.h"

#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

bool ipc_socket_path(const char *override, char *out, size_t size,
		char *error, size_t error_size) {
	const char *env = getenv("SHADY_SOCKET");
	const char *chosen = override && *override ? override : env && *env ? env : NULL;
	if (chosen) {
		snprintf(out, size, "%s", chosen);
		return true;
	}
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (!runtime || !*runtime) {
		snprintf(error, error_size, "no Shady socket: set SHADY_SOCKET or XDG_RUNTIME_DIR");
		return false;
	}
	const char *display = getenv("WAYLAND_DISPLAY");
	if (display && *display && !strchr(display, '/')) {
		snprintf(out, size, "%s/shady-%s.sock", runtime, display);
		if (access(out, F_OK) == 0) return true;
	}
	/* Not started from inside the session: the only Shady running. */
	DIR *dir = opendir(runtime);
	int found = 0;
	if (dir) {
		struct dirent *e;
		while ((e = readdir(dir))) {
			size_t n = strlen(e->d_name);
			if (n > 11 && !strncmp(e->d_name, "shady-", 6) &&
					!strcmp(e->d_name + n - 5, ".sock")) {
				if (!found) snprintf(out, size, "%s/%s", runtime, e->d_name);
				found++;
			}
		}
		closedir(dir);
	}
	if (found == 1) return true;
	snprintf(error, error_size, found ? "several Shady sockets in %s: set SHADY_SOCKET" :
		"Shady is not running (no shady-*.sock in %s)", runtime);
	return false;
}

static long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

struct json *ipc_call(const char *socket_path, const char *request, int timeout_ms,
		char *error, size_t error_size) {
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	if (strlen(socket_path) >= sizeof(addr.sun_path)) {
		snprintf(error, error_size, "socket path too long: %s", socket_path);
		return NULL;
	}
	strcpy(addr.sun_path, socket_path);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		snprintf(error, error_size, "cannot reach Shady at %s: %s", socket_path, strerror(errno));
		if (fd >= 0) close(fd);
		return NULL;
	}

	size_t len = strlen(request), sent = 0;
	while (sent < len + 1) {
		const char *p = sent < len ? request + sent : "\n";
		size_t n = sent < len ? len - sent : 1;
		ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
		if (w < 0 && errno == EINTR) continue;
		if (w <= 0) {
			snprintf(error, error_size, "sending to Shady failed: %s", strerror(errno));
			close(fd);
			return NULL;
		}
		sent += (size_t)w;
	}

	/* Lines until the answer; events (subscribe) are not ours. */
	struct buf in = {0};
	struct json *answer = NULL;
	long long deadline = now_ms() + timeout_ms;
	size_t scanned = 0;
	for (;;) {
		char *nl = in.len > scanned ? memchr(in.data + scanned, '\n', in.len - scanned) : NULL;
		if (nl) {
			size_t line_len = (size_t)(nl - in.data);
			const char *perr = NULL;
			struct json *msg = json_parse(in.data, line_len, &perr);
			memmove(in.data, nl + 1, in.len - line_len - 1);
			in.len -= line_len + 1;
			scanned = 0;
			if (!msg) {
				snprintf(error, error_size, "Shady answered malformed JSON: %s", perr);
				break;
			}
			if (json_has(msg, "event")) {
				json_free(msg);
				continue;
			}
			answer = msg;
			break;
		}
		scanned = in.len;
		long long left = deadline - now_ms();
		if (left <= 0) {
			snprintf(error, error_size, "Shady did not answer within %d ms", timeout_ms);
			break;
		}
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int ready = poll(&pfd, 1, (int)left);
		if (ready < 0 && errno == EINTR) continue;
		if (ready <= 0) continue;
		char chunk[65536];
		ssize_t r = recv(fd, chunk, sizeof(chunk), 0);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) {
			snprintf(error, error_size, "Shady closed the connection");
			break;
		}
		buf_append(&in, chunk, (size_t)r);
		if (in.failed) {
			snprintf(error, error_size, "out of memory");
			break;
		}
	}
	buf_free(&in);
	close(fd);
	return answer;
}
