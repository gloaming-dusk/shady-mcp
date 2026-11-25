/*
 * shady-mcp: a Model Context Protocol server (stdio) for computer use on
 * Shady. It turns MCP tool calls into Shady IPC requests: screenshots come
 * back as images, and clicks, drags, scrolls and moves take coordinates in
 * pixels of the latest screenshot, so an agent clicks where it looked.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ipc.h"
#include "json.h"

#ifndef SHADY_MCP_VERSION
#define SHADY_MCP_VERSION "0.0.0"
#endif

#define DEFAULT_TIMEOUT_MS 10000

static struct {
	const char *socket_override;
	char shot_dir[4096];
	unsigned shot_seq;
	/* How pixels of the latest screenshot map to layout coordinates. */
	struct {
		bool valid;
		double x, y, scale;
		int width, height;
	} shot;
} S;

/* ---- tool results ----------------------------------------------------- */

struct result {
	struct buf content; /* the items of the "content" array */
	bool error;
};

static void result_item_sep(struct result *r) {
	if (r->content.len) buf_puts(&r->content, ",");
}

static void result_text(struct result *r, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
static void result_text(struct result *r, const char *fmt, ...) {
	char *text = NULL;
	va_list ap;
	va_start(ap, fmt);
	int n = vasprintf(&text, fmt, ap);
	va_end(ap);
	if (n < 0) return;
	result_item_sep(r);
	buf_puts(&r->content, "{\"type\":\"text\",\"text\":");
	buf_string(&r->content, text, (size_t)n);
	buf_puts(&r->content, "}");
	free(text);
}

static bool result_fail(struct result *r, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
static bool result_fail(struct result *r, const char *fmt, ...) {
	char *text = NULL;
	va_list ap;
	va_start(ap, fmt);
	int n = vasprintf(&text, fmt, ap);
	va_end(ap);
	r->error = true;
	if (n >= 0) {
		result_item_sep(r);
		buf_puts(&r->content, "{\"type\":\"text\",\"text\":");
		buf_string(&r->content, text, (size_t)n);
		buf_puts(&r->content, "}");
		free(text);
	}
	return false;
}

static void base64(struct buf *out, const unsigned char *data, size_t len) {
	static const char table[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	char quad[4];
	for (size_t i = 0; i < len; i += 3) {
		uint32_t v = (uint32_t)data[i] << 16;
		if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
		if (i + 2 < len) v |= data[i + 2];
		quad[0] = table[(v >> 18) & 63];
		quad[1] = table[(v >> 12) & 63];
		quad[2] = i + 1 < len ? table[(v >> 6) & 63] : '=';
		quad[3] = i + 2 < len ? table[v & 63] : '=';
		buf_append(out, quad, 4);
	}
}

/* ---- talking to Shady ------------------------------------------------- */

/* A request under way: {"cmd": …, fields}. */
static void req_begin(struct buf *b, const char *cmd) {
	buf_puts(b, "{\"cmd\":");
	buf_cstring(b, cmd);
}

static void req_num(struct buf *b, const char *key, double value) {
	buf_puts(b, ",");
	buf_cstring(b, key);
	buf_puts(b, ":");
	buf_number(b, value);
}

static void req_str(struct buf *b, const char *key, const char *value) {
	buf_puts(b, ",");
	buf_cstring(b, key);
	buf_puts(b, ":");
	buf_cstring(b, value);
}

static void req_bool(struct buf *b, const char *key, bool value) {
	buf_puts(b, ",");
	buf_cstring(b, key);
	buf_puts(b, value ? ":true" : ":false");
}

/* Send it; the answer's "data" stays in *answer (free it). False, with the
 * reason in the result, when Shady says no or cannot be reached. */
static bool shady(struct result *r, struct buf *req, int timeout_ms, struct json **answer) {
	buf_puts(req, "}");
	*answer = NULL;
	char path[4096], error[512];
	if (!ipc_socket_path(S.socket_override, path, sizeof(path), error, sizeof(error)))
		return result_fail(r, "%s", error);
	struct json *reply = ipc_call(path, req->data, timeout_ms, error, sizeof(error));
	if (!reply) return result_fail(r, "%s", error);
	const struct json *ok = json_get(reply, "ok");
	if (!ok || ok->type != JSON_BOOL || !ok->boolean) {
		const char *message = json_get_string(reply, "error");
		/* An older Shady: the commands this uses came with 0.4.0. */
		bool old = message && !strcmp(message, "unknown command");
		result_fail(r, "Shady: %s%s", message ? message : "request failed",
			old ? " (this Shady is too old: shady-mcp needs Shady 0.4.0 or later)" : "");
		json_free(reply);
		return false;
	}
	*answer = reply;
	return true;
}

static const struct json *data_of(const struct json *answer) {
	return json_get(answer, "data");
}

/* ---- arguments -------------------------------------------------------- */

static bool arg_number(struct result *r, const struct json *args, const char *key,
		bool required, double *out) {
	const struct json *v = json_get(args, key);
	if (!v || v->type == JSON_NULL) {
		if (required) return result_fail(r, "missing number argument: %s", key);
		return true;
	}
	if (v->type != JSON_NUMBER || !isfinite(v->number))
		return result_fail(r, "%s must be a number", key);
	*out = v->number;
	return true;
}

/* Pixels of the latest screenshot to layout coordinates. */
static void to_layout(double px, double py, double *lx, double *ly) {
	if (!S.shot.valid) {
		*lx = px;
		*ly = py;
		return;
	}
	*lx = S.shot.x + px / S.shot.scale;
	*ly = S.shot.y + py / S.shot.scale;
}

static bool arg_point(struct result *r, const struct json *args, const char *xkey,
		const char *ykey, bool required, double *lx, double *ly, bool *given) {
	bool has_x = json_has(args, xkey), has_y = json_has(args, ykey);
	if (given) *given = has_x && has_y;
	if (has_x != has_y || (required && !has_x))
		return result_fail(r, "give both %s and %s", xkey, ykey);
	if (!has_x) return true;
	double px = 0, py = 0;
	if (!arg_number(r, args, xkey, true, &px) || !arg_number(r, args, ykey, true, &py))
		return false;
	to_layout(px, py, lx, ly);
	return true;
}

static void copy_button(struct buf *req, const struct json *args) {
	const struct json *b = json_get(args, "button");
	if (b && b->type == JSON_STRING) req_str(req, "button", b->string);
}

/* ---- tools ------------------------------------------------------------ */

static bool read_file(const char *path, struct buf *out) {
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	char chunk[65536];
	size_t n;
	while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) buf_append(out, chunk, n);
	bool ok = !ferror(f) && !out->failed;
	fclose(f);
	return ok;
}

struct shot_options {
	double max_w, max_h, window;
	bool settle;
	bool region; /* lx, ly, lw, lh: a part of the layout, to zoom in */
	double lx, ly, lw, lh;
	const char *output;
};

static bool take_screenshot(const struct shot_options *o, struct result *r) {
	struct json *answer = NULL;
	if (o->settle) {
		/* What a person would see: once the windows (or this one) stop
		 * changing. A bar's clock does not count. */
		struct buf req = {0};
		req_begin(&req, "wait");
		req_num(&req, "idle", 150);
		req_num(&req, "timeout", 1500);
		if (o->window > 0) req_num(&req, "window", o->window);
		bool ok = shady(r, &req, 5000, &answer);
		buf_free(&req);
		json_free(answer);
		if (!ok) return false;
	}
	char path[4200];
	snprintf(path, sizeof(path), "%s/shot-%u.png", S.shot_dir, ++S.shot_seq);
	struct buf req = {0};
	req_begin(&req, "screenshot");
	req_str(&req, "path", path);
	req_bool(&req, "wait", true);
	if (o->max_w > 0) req_num(&req, "max_width", floor(o->max_w));
	if (o->max_h > 0) req_num(&req, "max_height", floor(o->max_h));
	if (o->window > 0) req_num(&req, "window", o->window);
	if (o->output) req_str(&req, "output", o->output);
	if (o->region) {
		req_num(&req, "x", floor(o->lx));
		req_num(&req, "y", floor(o->ly));
		req_num(&req, "width", ceil(o->lw));
		req_num(&req, "height", ceil(o->lh));
		/* Larger, to read: a Shady without upscale ignores it and gives the
		 * region at its own size. */
		req_bool(&req, "upscale", true);
	}
	bool ok = shady(r, &req, 15000, &answer);
	buf_free(&req);
	if (!ok) return false;
	const struct json *d = data_of(answer);
	struct buf png = {0};
	if (!read_file(path, &png)) {
		json_free(answer);
		unlink(path);
		return result_fail(r, "cannot read the screenshot %s: %s", path, strerror(errno));
	}
	unlink(path);
	S.shot.valid = true;
	S.shot.x = json_get_number(d, "x", 0);
	S.shot.y = json_get_number(d, "y", 0);
	S.shot.scale = json_get_number(d, "scale", 1);
	if (!(S.shot.scale > 0)) S.shot.scale = 1;
	S.shot.width = (int)json_get_number(d, "width", 0);
	S.shot.height = (int)json_get_number(d, "height", 0);

	result_item_sep(r);
	buf_puts(&r->content, "{\"type\":\"image\",\"mimeType\":\"image/png\",\"data\":\"");
	base64(&r->content, (const unsigned char *)png.data, png.len);
	buf_puts(&r->content, "\"}");
	buf_free(&png);
	const char *output = json_get_string(d, "output");
	result_text(r, "%dx%d screenshot of %s%s, covering layout (%g, %g) %gx%g. "
		"Coordinates for click, move, drag and scroll are now pixels of this image.",
		S.shot.width, S.shot.height,
		o->window > 0 ? "a window on " : o->region ? "a region of " : "",
		output ? output : "the screen", S.shot.x, S.shot.y,
		json_get_number(d, "logical_width", 0), json_get_number(d, "logical_height", 0));
	json_free(answer);
	return true;
}

static bool tool_screenshot(const struct json *args, struct result *r) {
	struct shot_options o = { .max_w = 1280, .max_h = 800 };
	if (!arg_number(r, args, "max_width", false, &o.max_w) ||
			!arg_number(r, args, "max_height", false, &o.max_h) ||
			!arg_number(r, args, "window", false, &o.window))
		return false;
	const struct json *settle = json_get(args, "settle");
	o.settle = !settle || settle->type != JSON_BOOL || settle->boolean;
	o.output = json_get_string(args, "output");

	/* A region, in pixels of the latest screenshot like every other point,
	 * to see it closer: small text, a crowded toolbar. */
	bool has_x = json_has(args, "x"), has_y = json_has(args, "y");
	bool has_w = json_has(args, "width"), has_h = json_has(args, "height");
	if (has_x || has_y || has_w || has_h) {
		if (!(has_x && has_y && has_w && has_h))
			return result_fail(r, "a region needs x, y, width and height");
		if (o.window > 0) return result_fail(r, "give a window or a region, not both");
		double x = 0, y = 0, w = 0, h = 0;
		if (!arg_number(r, args, "x", true, &x) || !arg_number(r, args, "y", true, &y) ||
				!arg_number(r, args, "width", true, &w) ||
				!arg_number(r, args, "height", true, &h))
			return false;
		if (w < 1 || h < 1) return result_fail(r, "width and height must be at least 1");
		double scale = S.shot.valid ? S.shot.scale : 1;
		to_layout(x, y, &o.lx, &o.ly);
		o.lw = w / scale;
		o.lh = h / scale;
		o.region = true;
	}
	return take_screenshot(&o, r);
}

/* An action asked to show what it did: a settled screenshot of the whole
 * screen after it, so look-act-look takes one call. */
static bool after_action(const struct json *args, struct result *r, bool ok) {
	const struct json *want = json_get(args, "screenshot");
	if (!ok || !want || want->type != JSON_BOOL || !want->boolean) return ok;
	struct shot_options o = { .max_w = 1280, .max_h = 800, .settle = true };
	return take_screenshot(&o, r);
}

static bool tool_click(const struct json *args, struct result *r) {
	double x = 0, y = 0, count = 1;
	if (!arg_point(r, args, "x", "y", true, &x, &y, NULL) ||
			!arg_number(r, args, "count", false, &count))
		return false;
	if (count < 1 || count > 3) return result_fail(r, "count is 1, 2 or 3");
	struct buf req = {0};
	req_begin(&req, "pointer.click");
	req_num(&req, "x", x);
	req_num(&req, "y", y);
	req_num(&req, "count", floor(count));
	copy_button(&req, args);
	struct json *answer;
	bool ok = shady(r, &req, DEFAULT_TIMEOUT_MS, &answer);
	buf_free(&req);
	json_free(answer);
	if (ok) result_text(r, "clicked at layout (%g, %g)", x, y);
	return after_action(args, r, ok);
}

static bool tool_move(const struct json *args, struct result *r) {
	double x = 0, y = 0;
	if (!arg_point(r, args, "x", "y", true, &x, &y, NULL)) return false;
	struct buf req = {0};
	req_begin(&req, "pointer.move");
	req_num(&req, "x", x);
	req_num(&req, "y", y);
	struct json *answer;
	bool ok = shady(r, &req, DEFAULT_TIMEOUT_MS, &answer);
	buf_free(&req);
	json_free(answer);
	if (ok) result_text(r, "pointer at layout (%g, %g)", x, y);
	return after_action(args, r, ok);
}

static bool tool_drag(const struct json *args, struct result *r) {
	double x1 = 0, y1 = 0, x2 = 0, y2 = 0, duration = 400;
	if (!arg_point(r, args, "x1", "y1", true, &x1, &y1, NULL) ||
			!arg_point(r, args, "x2", "y2", true, &x2, &y2, NULL) ||
			!arg_number(r, args, "duration", false, &duration))
		return false;
	if (duration < 0 || duration > 60000) return result_fail(r, "duration is 0 to 60000 ms");
	struct buf req = {0};
	req_begin(&req, "pointer.drag");
	req_num(&req, "x1", x1);
	req_num(&req, "y1", y1);
	req_num(&req, "x2", x2);
	req_num(&req, "y2", y2);
	req_num(&req, "duration", floor(duration));
	copy_button(&req, args);
	struct json *answer;
	bool ok = shady(r, &req, (int)duration + DEFAULT_TIMEOUT_MS, &answer);
	buf_free(&req);
	json_free(answer);
	if (ok) result_text(r, "dragged from layout (%g, %g) to (%g, %g)", x1, y1, x2, y2);
	return after_action(args, r, ok);
}

static bool tool_scroll(const struct json *args, struct result *r) {
	double x = 0, y = 0, dx = 0, dy = 0;
	bool given = false;
	if (!arg_point(r, args, "x", "y", false, &x, &y, &given) ||
			!arg_number(r, args, "dx", false, &dx) || !arg_number(r, args, "dy", false, &dy))
		return false;
	struct buf req = {0};
	req_begin(&req, "pointer.scroll");
	if (given) {
		req_num(&req, "x", x);
		req_num(&req, "y", y);
	}
	req_num(&req, "dx", dx);
	req_num(&req, "dy", dy);
	struct json *answer;
	bool ok = shady(r, &req, DEFAULT_TIMEOUT_MS, &answer);
	buf_free(&req);
	json_free(answer);
	if (ok) result_text(r, "scrolled %g, %g wheel clicks", dx, dy);
	return after_action(args, r, ok);
}

static bool tool_type(const struct json *args, struct result *r) {
	const char *text = json_get_string(args, "text");
	if (!text) return result_fail(r, "missing string argument: text");
	struct buf req = {0};
	req_begin(&req, "type");
	req_str(&req, "text", text);
	struct json *answer;
	bool ok = shady(r, &req, DEFAULT_TIMEOUT_MS, &answer);
	buf_free(&req);
	if (ok) result_text(r, "typed %g characters",
		json_get_number(data_of(answer), "typed", 0));
	json_free(answer);
	return after_action(args, r, ok);
}

static bool tool_key(const struct json *args, struct result *r) {
	const char *keys = json_get_string(args, "keys");
	if (!keys) return result_fail(r, "missing string argument: keys");
	const char *action = json_get_string(args, "action");
	const char *cmd = "key";
	if (action && !strcmp(action, "down")) cmd = "key.down";
	else if (action && !strcmp(action, "up")) cmd = "key.up";
	else if (action && strcmp(action, "press") != 0)
		return result_fail(r, "action is press, down or up");
	struct buf req = {0};
	req_begin(&req, cmd);
	req_str(&req, "keys", keys);
	struct json *answer;
	bool ok = shady(r, &req, DEFAULT_TIMEOUT_MS, &answer);
	buf_free(&req);
	if (ok) {
		const struct json *d = data_of(answer);
		const struct json *handled = json_get(d, "handled");
		const struct json *sent = json_get(d, "sent");
		bool h = handled && handled->type == JSON_BOOL && handled->boolean;
		bool s = sent && sent->type == JSON_BOOL && sent->boolean;
		if (!h && !sent) {
			/* Before 0.4.0 an unbound key reached no window, and said nothing. */
			result_fail(r, "%s %s: no shortcut took it, and this Shady is too old to send "
				"keys to windows (shady-mcp needs Shady 0.4.0 or later)", cmd, keys);
		} else {
			result_text(r, "%s %s: %s", cmd, keys,
				h ? "a compositor shortcut took it" :
				s ? "sent to the focused window" : "nothing took it (no such key in the keymap)");
		}
	}
	json_free(answer);
	return after_action(args, r, ok && !r->error);
}

static bool tool_wait(const struct json *args, struct result *r) {
	double ms = 0, idle = 0, timeout = 5000, window = 0;
	if (!arg_number(r, args, "ms", false, &ms) || !arg_number(r, args, "idle", false, &idle) ||
			!arg_number(r, args, "timeout", false, &timeout) ||
			!arg_number(r, args, "window", false, &window))
		return false;
	const char *scope = json_get_string(args, "scope");
	if (ms <= 0 && idle <= 0) idle = 200;
	struct buf req = {0};
	req_begin(&req, "wait");
	req_num(&req, "ms", floor(ms));
	req_num(&req, "idle", floor(idle));
	req_num(&req, "timeout", floor(timeout));
	if (window > 0) req_num(&req, "window", window);
	if (scope) req_str(&req, "scope", scope);
	struct json *answer;
	bool ok = shady(r, &req, (int)(ms > timeout ? ms : timeout) + DEFAULT_TIMEOUT_MS, &answer);
	buf_free(&req);
	if (ok) {
		const struct json *settled = json_get(data_of(answer), "settled");
		const char *active = json_get_string(data_of(answer), "active");
		if (settled && settled->type == JSON_BOOL && settled->boolean)
			result_text(r, "settled after %g ms", json_get_number(data_of(answer), "elapsed", 0));
		else
			result_text(r, "still changing after %g ms%s%s", json_get_number(data_of(answer), "elapsed", 0),
				active ? ": the last change was " : "", active ? active : "");
	}
	json_free(answer);
	return ok;
}

static bool tool_windows(const struct json *args, struct result *r) {
	(void)args;
	struct buf req = {0};
	req_begin(&req, "windows");
	struct json *answer;
	bool ok = shady(r, &req, DEFAULT_TIMEOUT_MS, &answer);
	buf_free(&req);
	if (!ok) return false;
	const struct json *list = data_of(answer);
	struct buf text = {0};
	for (size_t i = 0; list && list->type == JSON_ARRAY && i < list->count; i++) {
		const struct json *w = list->items[i];
		double x = json_get_number(w, "x", 0), y = json_get_number(w, "y", 0);
		double width = json_get_number(w, "width", 0), height = json_get_number(w, "height", 0);
		const struct json *focused = json_get(w, "focused");
		buf_printf(&text, "window %g: ", json_get_number(w, "id", 0));
		buf_cstring(&text, json_get_string(w, "app_id"));
		buf_puts(&text, " ");
		buf_cstring(&text, json_get_string(w, "title"));
		buf_printf(&text, " on workspace %s%s, layout (%g, %g) %gx%g",
			json_get_string(w, "workspace") ? json_get_string(w, "workspace") : "?",
			focused && focused->type == JSON_BOOL && focused->boolean ? ", focused" : "",
			x, y, width, height);
		if (S.shot.valid)
			buf_printf(&text, ", in the latest screenshot (%.0f, %.0f) %.0fx%.0f",
				(x - S.shot.x) * S.shot.scale, (y - S.shot.y) * S.shot.scale,
				width * S.shot.scale, height * S.shot.scale);
		buf_puts(&text, "\n");
	}
	result_text(r, "%s", text.len ? text.data : "no windows");
	buf_free(&text);
	json_free(answer);
	return true;
}

static bool tool_focus_window(const struct json *args, struct result *r) {
	double window = 0;
	if (!arg_number(r, args, "window", true, &window)) return false;
	struct buf req = {0};
	req_begin(&req, "window.focus");
	req_num(&req, "window", window);
	struct json *answer;
	bool ok = shady(r, &req, DEFAULT_TIMEOUT_MS, &answer);
	buf_free(&req);
	json_free(answer);
	if (ok) result_text(r, "window %g focused", window);
	return ok;
}

/* text as a Lua string literal: quotes, backslashes and control bytes
 * escaped; UTF-8 passes through. */
static void lua_string(struct buf *b, const char *text) {
	buf_puts(b, "\"");
	for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
		if (*p == '"' || *p == '\\') buf_printf(b, "\\%c", *p);
		else if (*p < 0x20 || *p == 0x7f) buf_printf(b, "\\%03u", *p);
		else buf_append(b, (const char *)p, 1);
	}
	buf_puts(b, "\"");
}

/* The windows Shady has now (free the answer). */
static bool list_windows(struct result *r, struct json **answer) {
	struct buf req = {0};
	req_begin(&req, "windows");
	bool ok = shady(r, &req, DEFAULT_TIMEOUT_MS, answer);
	buf_free(&req);
	return ok;
}

static bool has_window(const struct json *list, double id) {
	for (size_t i = 0; list && list->type == JSON_ARRAY && i < list->count; i++)
		if (json_get_number(list->items[i], "id", -1) == id) return true;
	return false;
}

static double now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* text as one single-quoted shell word. */
static void sh_quote(struct buf *b, const char *text) {
	buf_puts(b, "'");
	for (const char *p = text; *p; p++) {
		if (*p == '\'') buf_puts(b, "'\\''");
		else buf_append(b, p, 1);
	}
	buf_puts(b, "'");
}

/* The exit status the launch shell left in path, or -1 while it runs. */
static int read_status(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	int status = -1;
	if (fscanf(f, "%d", &status) != 1) status = -1;
	fclose(f);
	return status;
}

/* Start a program as Super+Return would, and wait for its window. The
 * shell leaves the command's exit status in a file, so a command that is
 * not there, or that fails before it has a window, says so at once. */
static bool tool_launch(const struct json *args, struct result *r) {
	const char *command = json_get_string(args, "command");
	if (!command || !*command) return result_fail(r, "missing string argument: command");
	double timeout = 5000;
	if (!arg_number(r, args, "timeout", false, &timeout)) return false;
	if (timeout < 0 || timeout > 60000) return result_fail(r, "timeout is 0 to 60000 ms");

	struct json *before = NULL;
	if (!list_windows(r, &before)) return false;

	char status_path[4200];
	snprintf(status_path, sizeof(status_path), "%s/launch-%u.status", S.shot_dir, ++S.shot_seq);
	/* The command on lines of its own, so a trailing comment or & stays
	 * its own; the status written quietly, in case we have gone. */
	struct buf script = {0};
	buf_puts(&script, command);
	buf_puts(&script, "\n{ printf '%s' \"$?\" >");
	sh_quote(&script, status_path);
	buf_puts(&script, "; } 2>/dev/null");

	struct buf code = {0};
	buf_puts(&code, "return shady.spawn(");
	lua_string(&code, script.data);
	buf_puts(&code, ")");
	buf_free(&script);
	struct buf req = {0};
	req_begin(&req, "lua");
	buf_puts(&req, ",\"name\":\"=shady-mcp launch\",\"code\":");
	buf_string(&req, code.data, code.len);
	buf_free(&code);
	struct json *answer = NULL;
	bool ok = shady(r, &req, DEFAULT_TIMEOUT_MS, &answer);
	buf_free(&req);
	if (!ok) {
		json_free(before);
		return false;
	}
	const struct json *ret = data_of(answer);
	const struct json *spawned = ret && ret->type == JSON_ARRAY && ret->count ? ret->items[0] : NULL;
	bool started = spawned && spawned->type == JSON_BOOL && spawned->boolean;
	json_free(answer);
	if (!started) {
		json_free(before);
		return result_fail(r, "Shady could not start: %s", command);
	}

	/* A new window, mapped: the program is up. Polled, since one IPC
	 * request is one connection here. A command that finished well may
	 * have handed its window to a program already running (a browser
	 * opening a tab): a moment more for that. */
	const struct json *found = NULL;
	struct json *now = NULL;
	int status = -1;
	double end = now_ms() + timeout;
	while (!found) {
		json_free(now);
		now = NULL;
		if (!list_windows(r, &now)) {
			json_free(before);
			return false;
		}
		const struct json *list = data_of(now);
		for (size_t i = 0; list && list->type == JSON_ARRAY && i < list->count; i++) {
			double id = json_get_number(list->items[i], "id", -1);
			if (!has_window(data_of(before), id)) {
				found = list->items[i];
				break;
			}
		}
		if (found || now_ms() >= end) break;
		if (status < 0 && (status = read_status(status_path)) >= 0) {
			if (status != 0) break;
			if (end > now_ms() + 1000) end = now_ms() + 1000;
		}
		struct timespec pause = { 0, 100 * 1000000L };
		nanosleep(&pause, NULL);
	}
	if (status < 0) status = read_status(status_path);
	if (status >= 0) unlink(status_path);
	json_free(before);

	bool shown = true;
	if (found) {
		const char *app = json_get_string(found, "app_id");
		const char *title = json_get_string(found, "title");
		result_text(r, "started %s: window %g, %s \"%s\"", command,
			json_get_number(found, "id", 0), app ? app : "?", title ? title : "");
	} else if (status == 127) {
		shown = result_fail(r, "%s: command not found (the shell's status 127)", command);
	} else if (status == 126) {
		shown = result_fail(r, "%s: found but cannot be run (the shell's status 126)", command);
	} else if (status > 0) {
		shown = result_fail(r, "%s exited with status %d before it opened a window", command, status);
	} else if (status == 0) {
		result_text(r, "%s finished (status 0) and no new window came within a second of it; "
			"anything it started in the background may still open one", command);
	} else {
		result_text(r, "started %s; still running, with no new window within %g ms "
			"(it may still be starting, or have no window)", command, timeout);
	}
	json_free(now);
	return after_action(args, r, shown);
}

/* Any IPC command, for what the other tools do not cover. */
static bool tool_ipc(const struct json *args, struct result *r) {
	const char *cmd = json_get_string(args, "cmd");
	if (!cmd) return result_fail(r, "missing string argument: cmd");
	const struct json *fields = json_get(args, "args");
	if (fields && fields->type != JSON_OBJECT && fields->type != JSON_NULL)
		return result_fail(r, "args must be an object");
	struct buf req = {0};
	req_begin(&req, cmd);
	for (size_t i = 0; fields && fields->type == JSON_OBJECT && i < fields->count; i++) {
		if (!strcmp(fields->keys[i], "cmd") || !strcmp(fields->keys[i], "id")) continue;
		buf_puts(&req, ",");
		buf_cstring(&req, fields->keys[i]);
		buf_puts(&req, ":");
		buf_json(&req, fields->items[i]);
	}
	struct json *answer;
	bool ok = shady(r, &req, 30000, &answer);
	buf_free(&req);
	if (!ok) return false;
	struct buf text = {0};
	const struct json *d = data_of(answer);
	if (d) buf_json(&text, d);
	else buf_puts(&text, "ok");
	result_text(r, "%s", text.data);
	buf_free(&text);
	json_free(answer);
	return true;
}

#define POINT_DOC "Pixels of the latest screenshot (layout coordinates before any)."
#define BUTTON_SCHEMA "\"button\":{\"type\":\"string\",\"enum\":[\"left\",\"right\",\"middle\",\"side\",\"extra\",\"back\",\"forward\"],\"description\":\"Default left.\"}"
#define SHOT_AFTER "\"screenshot\":{\"type\":\"boolean\",\"description\":\"Then take a screenshot of the screen, once it settles, to see what happened. Default false.\"}"
#define POINT_PROP(name) "\"" name "\":{\"type\":\"number\",\"description\":\"" POINT_DOC "\"}"

/* MCP tool annotations: hints for a client deciding what to ask about. */
#define LOOKS(title) "{\"title\":\"" title "\",\"readOnlyHint\":true,\"openWorldHint\":false}"
#define ACTS(title) "{\"title\":\"" title "\",\"readOnlyHint\":false,\"destructiveHint\":true,\"openWorldHint\":false}"
#define HARMLESS(title) "{\"title\":\"" title "\",\"readOnlyHint\":false,\"destructiveHint\":false,\"idempotentHint\":true,\"openWorldHint\":false}"

static const struct tool {
	const char *name;
	const char *description;
	const char *schema;
	const char *annotations;
	bool (*run)(const struct json *args, struct result *r);
} tools[] = {
	{ "screenshot",
	  "See the screen, after it stops changing. Later click/move/drag/scroll coordinates are pixels of this image. "
	  "To read small text, zoom in: x, y, width, height pick a region of the latest screenshot, captured closer.",
	  "{\"type\":\"object\",\"properties\":{"
	  "\"window\":{\"type\":\"integer\",\"description\":\"Only this window (an id from windows).\"},"
	  "\"x\":{\"type\":\"number\",\"description\":\"Region to zoom into: its left, in pixels of the latest screenshot.\"},"
	  "\"y\":{\"type\":\"number\",\"description\":\"Region top, in pixels of the latest screenshot.\"},"
	  "\"width\":{\"type\":\"number\",\"description\":\"Region width, in pixels of the latest screenshot.\"},"
	  "\"height\":{\"type\":\"number\",\"description\":\"Region height, in pixels of the latest screenshot.\"},"
	  "\"output\":{\"type\":\"string\",\"description\":\"Which output (monitor), by name as shady_ipc outputs gives it. Default the first to draw.\"},"
	  "\"max_width\":{\"type\":\"integer\",\"description\":\"Scale down to at most this wide. Default 1280.\"},"
	  "\"max_height\":{\"type\":\"integer\",\"description\":\"Scale down to at most this tall. Default 800.\"},"
	  "\"settle\":{\"type\":\"boolean\",\"description\":\"Wait until nothing changes first. Default true.\"}}}",
	  LOOKS("Screenshot"), tool_screenshot },
	{ "click", "Move the pointer to a point and click there.",
	  "{\"type\":\"object\",\"properties\":{"
	  POINT_PROP("x") "," POINT_PROP("y") "," BUTTON_SCHEMA ","
	  "\"count\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":3,\"description\":\"2 for a double click, 3 for a triple. Default 1.\"},"
	  SHOT_AFTER "},"
	  "\"required\":[\"x\",\"y\"]}",
	  ACTS("Click"), tool_click },
	{ "move", "Move the pointer, to hover.",
	  "{\"type\":\"object\",\"properties\":{"
	  POINT_PROP("x") "," POINT_PROP("y") "," SHOT_AFTER "},\"required\":[\"x\",\"y\"]}",
	  HARMLESS("Move pointer"), tool_move },
	{ "drag", "Press at one point, move to another over a while, release: select text, move a window by its title bar, drag a slider.",
	  "{\"type\":\"object\",\"properties\":{"
	  POINT_PROP("x1") "," POINT_PROP("y1") "," POINT_PROP("x2") "," POINT_PROP("y2") ","
	  BUTTON_SCHEMA ","
	  "\"duration\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":60000,\"description\":\"Milliseconds. Default 400.\"},"
	  SHOT_AFTER "},"
	  "\"required\":[\"x1\",\"y1\",\"x2\",\"y2\"]}",
	  ACTS("Drag"), tool_drag },
	{ "scroll", "Scroll by wheel clicks, at a point or where the pointer is.",
	  "{\"type\":\"object\",\"properties\":{"
	  POINT_PROP("x") "," POINT_PROP("y") ","
	  "\"dy\":{\"type\":\"number\",\"description\":\"Wheel clicks; positive scrolls down.\"},"
	  "\"dx\":{\"type\":\"number\",\"description\":\"Wheel clicks; positive scrolls right.\"},"
	  SHOT_AFTER "}}",
	  ACTS("Scroll"), tool_scroll },
	{ "type", "Type text into the focused window. Any characters, Hangul and emoji included; \\n is Return.",
	  "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}," SHOT_AFTER "},\"required\":[\"text\"]}",
	  ACTS("Type text"), tool_type },
	{ "key", "Press a key or shortcut: Return, Escape, Tab, BackSpace, Up, ctrl+l, Ctrl+Shift+t, Super+Return. Compositor shortcuts run; other keys go to the focused window.",
	  "{\"type\":\"object\",\"properties\":{"
	  "\"keys\":{\"type\":\"string\",\"description\":\"Modifiers (Ctrl, Alt, Shift, Super) and one XKB key name, joined by +.\"},"
	  "\"action\":{\"type\":\"string\",\"enum\":[\"press\",\"down\",\"up\"],\"description\":\"down/up hold and release it. Default press.\"},"
	  SHOT_AFTER "},"
	  "\"required\":[\"keys\"]}",
	  ACTS("Press key"), tool_key },
	{ "wait", "Wait: at least ms, then until the windows have not changed for idle ms (default 200), up to timeout. A bar's clock or an animated wallpaper does not count. Says what kept changing if it does not settle.",
	  "{\"type\":\"object\",\"properties\":{"
	  "\"ms\":{\"type\":\"integer\",\"minimum\":0},\"idle\":{\"type\":\"integer\",\"minimum\":0},"
	  "\"timeout\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"Default 5000.\"},"
	  "\"window\":{\"type\":\"integer\",\"description\":\"Only this window (and windows moving), for a busy desktop.\"},"
	  "\"scope\":{\"type\":\"string\",\"enum\":[\"windows\",\"screen\"],\"description\":\"screen: every frame drawn. Default windows.\"}}}",
	  LOOKS("Wait"), tool_wait },
	{ "windows", "List the windows: id, app id, title, workspace, focus and where they are, also in the latest screenshot.",
	  "{\"type\":\"object\",\"properties\":{}}",
	  LOOKS("List windows"), tool_windows },
	{ "focus_window", "Focus a window, switching to its workspace.",
	  "{\"type\":\"object\",\"properties\":{\"window\":{\"type\":\"integer\"}},\"required\":[\"window\"]}",
	  HARMLESS("Focus window"), tool_focus_window },
	{ "launch", "Start a program, as a launcher would: a shell command such as foot, firefox or 'foot -e htop'. Waits for its window and says which it is, or why it did not start.",
	  "{\"type\":\"object\",\"properties\":{"
	  "\"command\":{\"type\":\"string\",\"description\":\"Run by /bin/sh -lc in the session.\"},"
	  "\"timeout\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":60000,\"description\":\"How long to wait for a new window, ms. 0 to not wait. Default 5000.\"},"
	  SHOT_AFTER "},"
	  "\"required\":[\"command\"]}",
	  "{\"title\":\"Launch program\",\"readOnlyHint\":false,\"destructiveHint\":true,\"openWorldHint\":true}",
	  tool_launch },
	{ "shady_ipc", "Any Shady IPC command, such as workspaces, workspace.switch, window.close, window.maximize, outputs or lua. See Shady's docs/IPC_API.md. Coordinates here are layout coordinates.",
	  "{\"type\":\"object\",\"properties\":{"
	  "\"cmd\":{\"type\":\"string\"},"
	  "\"args\":{\"type\":\"object\",\"description\":\"The command's arguments: strings, numbers, booleans or arrays of strings.\"}},"
	  "\"required\":[\"cmd\"]}",
	  "{\"title\":\"Shady IPC\",\"readOnlyHint\":false,\"destructiveHint\":true,\"openWorldHint\":true}",
	  tool_ipc },
};
#define TOOL_COUNT (sizeof(tools) / sizeof(tools[0]))

/* ---- MCP -------------------------------------------------------------- */

static const char *instructions =
	"Shady is a Wayland desktop. Work it as a person does: take a screenshot, "
	"act (click, type, key, drag, scroll), then take another screenshot to see "
	"what changed. Coordinates are pixels of the latest screenshot. Type text "
	"with `type`; press Return, Tab or shortcuts with `key`. Start programs "
	"with `launch`. To read small text, zoom: `screenshot` with a region. "
	"Actions take `screenshot: true` to show what they did in the same call. "
	"`windows` lists the windows with their ids; `shady_ipc` reaches anything else.";

static void send_message(struct buf *msg) {
	buf_puts(msg, "\n");
	fwrite(msg->data, 1, msg->len, stdout);
	fflush(stdout);
}

static void reply_result(const struct json *id, const char *result) {
	struct buf msg = {0};
	buf_puts(&msg, "{\"jsonrpc\":\"2.0\",\"id\":");
	buf_json(&msg, id);
	buf_puts(&msg, ",\"result\":");
	buf_puts(&msg, result);
	buf_puts(&msg, "}");
	send_message(&msg);
	buf_free(&msg);
}

static void reply_error(const struct json *id, int code, const char *message) {
	struct buf msg = {0};
	buf_puts(&msg, "{\"jsonrpc\":\"2.0\",\"id\":");
	buf_json(&msg, id);
	buf_printf(&msg, ",\"error\":{\"code\":%d,\"message\":", code);
	buf_cstring(&msg, message);
	buf_puts(&msg, "}}");
	send_message(&msg);
	buf_free(&msg);
}

static void handle_initialize(const struct json *id, const struct json *params) {
	static const char *known[] = { "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05" };
	const char *asked = json_get_string(params, "protocolVersion");
	const char *version = known[1];
	for (size_t i = 0; asked && i < sizeof(known) / sizeof(known[0]); i++)
		if (!strcmp(asked, known[i])) version = known[i];
	struct buf res = {0};
	buf_puts(&res, "{\"protocolVersion\":");
	buf_cstring(&res, version);
	buf_puts(&res, ",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"shady-mcp\","
		"\"version\":\"" SHADY_MCP_VERSION "\"},\"instructions\":");
	buf_cstring(&res, instructions);
	buf_puts(&res, "}");
	reply_result(id, res.data);
	buf_free(&res);
}

static void handle_tools_list(const struct json *id) {
	struct buf res = {0};
	buf_puts(&res, "{\"tools\":[");
	for (size_t i = 0; i < TOOL_COUNT; i++) {
		if (i) buf_puts(&res, ",");
		buf_puts(&res, "{\"name\":");
		buf_cstring(&res, tools[i].name);
		buf_puts(&res, ",\"description\":");
		buf_cstring(&res, tools[i].description);
		buf_puts(&res, ",\"inputSchema\":");
		buf_puts(&res, tools[i].schema);
		buf_puts(&res, ",\"annotations\":");
		buf_puts(&res, tools[i].annotations);
		buf_puts(&res, "}");
	}
	buf_puts(&res, "]}");
	reply_result(id, res.data);
	buf_free(&res);
}

static void handle_tools_call(const struct json *id, const struct json *params) {
	const char *name = json_get_string(params, "name");
	const struct json *args = json_get(params, "arguments");
	const struct tool *tool = NULL;
	for (size_t i = 0; name && i < TOOL_COUNT; i++)
		if (!strcmp(tools[i].name, name)) tool = &tools[i];
	if (!tool) {
		reply_error(id, -32602, "unknown tool");
		return;
	}
	if (args && args->type != JSON_OBJECT && args->type != JSON_NULL) {
		reply_error(id, -32602, "arguments must be an object");
		return;
	}
	struct result r = {0};
	tool->run(args, &r);
	struct buf res = {0};
	buf_puts(&res, "{\"content\":[");
	if (r.content.len) buf_append(&res, r.content.data, r.content.len);
	buf_printf(&res, "],\"isError\":%s}", r.error ? "true" : "false");
	if (res.failed) reply_error(id, -32603, "out of memory");
	else reply_result(id, res.data);
	buf_free(&res);
	buf_free(&r.content);
}

static void handle_message(const char *line, size_t len) {
	const char *error = NULL;
	struct json *msg = json_parse(line, len, &error);
	if (!msg || msg->type != JSON_OBJECT) {
		reply_error(NULL, -32700, msg ? "expected a JSON-RPC object" : error);
		json_free(msg);
		return;
	}
	const char *method = json_get_string(msg, "method");
	const struct json *id = json_get(msg, "id");
	const struct json *params = json_get(msg, "params");
	if (!method) {
		/* A response to us: we send no requests. */
		json_free(msg);
		return;
	}
	if (!id) { /* notifications: initialized, cancelled; nothing to do */
		json_free(msg);
		return;
	}
	if (!strcmp(method, "initialize")) handle_initialize(id, params);
	else if (!strcmp(method, "ping")) reply_result(id, "{}");
	else if (!strcmp(method, "tools/list")) handle_tools_list(id);
	else if (!strcmp(method, "tools/call")) handle_tools_call(id, params);
	else reply_error(id, -32601, "method not found");
	json_free(msg);
}

static void usage(FILE *out) {
	fprintf(out,
		"usage: shady-mcp [--socket PATH]\n"
		"An MCP server on stdin/stdout for computer use on the Shady compositor.\n"
		"The socket is PATH, else $SHADY_SOCKET, else\n"
		"$XDG_RUNTIME_DIR/shady-$WAYLAND_DISPLAY.sock, else the only Shady running.\n");
}

static volatile sig_atomic_t quitting;

static void on_quit_signal(int sig) {
	(void)sig;
	quitting = 1;
}

/* The screenshot directory and anything still in it. */
static void remove_shot_dir(void) {
	DIR *dir = opendir(S.shot_dir);
	if (dir) {
		struct dirent *e;
		while ((e = readdir(dir))) {
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
			char path[4400];
			snprintf(path, sizeof(path), "%s/%s", S.shot_dir, e->d_name);
			unlink(path);
		}
		closedir(dir);
	}
	rmdir(S.shot_dir);
}

int main(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--socket") && i + 1 < argc) {
			S.socket_override = argv[++i];
		} else if (!strcmp(argv[i], "--version")) {
			printf("shady-mcp %s\n", SHADY_MCP_VERSION);
			return 0;
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(stdout);
			return 0;
		} else {
			usage(stderr);
			return 2;
		}
	}
	signal(SIGPIPE, SIG_IGN);
	/* An MCP client stops its server with a signal as often as by closing
	 * stdin. Without SA_RESTART the read in getline gives up, and we tidy. */
	struct sigaction quit = { .sa_handler = on_quit_signal };
	sigemptyset(&quit.sa_mask);
	sigaction(SIGTERM, &quit, NULL);
	sigaction(SIGINT, &quit, NULL);
	sigaction(SIGHUP, &quit, NULL);

	/* Shady writes screenshots here and we read them back: private, and
	 * where a sandboxed /tmp would not hide it from the compositor. */
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	snprintf(S.shot_dir, sizeof(S.shot_dir), "%s/shady-mcp-XXXXXX",
		runtime && *runtime ? runtime : "/tmp");
	if (!mkdtemp(S.shot_dir)) {
		fprintf(stderr, "shady-mcp: cannot make %s: %s\n", S.shot_dir, strerror(errno));
		return 1;
	}

	char *line = NULL;
	size_t cap = 0;
	ssize_t n;
	while (!quitting && (n = getline(&line, &cap, stdin)) >= 0) {
		while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
		if (n == 0) continue;
		handle_message(line, (size_t)n);
	}
	free(line);
	remove_shot_dir();
	return 0;
}
