#ifndef SHADY_MCP_IPC_H
#define SHADY_MCP_IPC_H

#include <stddef.h>

#include "json.h"

/* The Shady IPC socket: `override`, else $SHADY_SOCKET, else
 * $XDG_RUNTIME_DIR/shady-$WAYLAND_DISPLAY.sock, else the only
 * $XDG_RUNTIME_DIR/shady-*.sock there is. False with a reason in `error`. */
bool ipc_socket_path(const char *override, char *out, size_t size,
	char *error, size_t error_size);

/* Send one request object (JSON text, one line) and return Shady's answer:
 * {"ok": …, "data": …} or {"ok": false, "error": …}. NULL with a reason
 * in `error` when Shady cannot be reached or stays silent `timeout_ms`. */
struct json *ipc_call(const char *socket_path, const char *request, int timeout_ms,
	char *error, size_t error_size);

#endif
