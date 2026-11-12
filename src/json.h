#ifndef SHADY_MCP_JSON_H
#define SHADY_MCP_JSON_H

/*
 * A small JSON reader and writer: the whole of JSON, for MCP messages and
 * Shady's IPC answers. Parsed values are a tree freed with json_free().
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

enum json_type { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT };

struct json {
	enum json_type type;
	bool boolean;
	double number;
	char *string;       /* JSON_STRING (NUL-terminated, may hold NULs: len) */
	size_t len;
	char *raw;          /* JSON_NUMBER: its text, to echo ids exactly */
	struct json **items; /* JSON_ARRAY and JSON_OBJECT values */
	char **keys;        /* JSON_OBJECT keys */
	size_t count;
};

/* NULL and *error on malformed input or trailing garbage. */
struct json *json_parse(const char *text, size_t len, const char **error);
void json_free(struct json *value);
const struct json *json_get(const struct json *object, const char *key);
const char *json_get_string(const struct json *object, const char *key);
/* The number at `key`, or `fallback` when it is absent or not a number. */
double json_get_number(const struct json *object, const char *key, double fallback);
bool json_has(const struct json *object, const char *key);

struct buf {
	char *data;
	size_t len, cap;
	bool failed;
};

void buf_free(struct buf *b);
void buf_append(struct buf *b, const char *text, size_t len);
void buf_puts(struct buf *b, const char *text);
void buf_printf(struct buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* A quoted, escaped JSON string. */
void buf_string(struct buf *b, const char *text, size_t len);
void buf_cstring(struct buf *b, const char *text);
/* A finite number as JSON (integers without a fraction). */
void buf_number(struct buf *b, double number);
/* Any parsed value, written back as JSON. */
void buf_json(struct buf *b, const struct json *value);

#endif
