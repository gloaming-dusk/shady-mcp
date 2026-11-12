#define _POSIX_C_SOURCE 200809L
#include "json.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 64

/* ---- writer ----------------------------------------------------------- */

void buf_free(struct buf *b) {
	free(b->data);
	*b = (struct buf){0};
}

static bool reserve(struct buf *b, size_t extra) {
	if (b->failed) return false;
	if (b->len + extra + 1 <= b->cap) return true;
	size_t cap = b->cap ? b->cap : 256;
	while (cap < b->len + extra + 1) cap *= 2;
	char *data = realloc(b->data, cap);
	if (!data) {
		b->failed = true;
		return false;
	}
	b->data = data;
	b->cap = cap;
	return true;
}

void buf_append(struct buf *b, const char *text, size_t len) {
	if (!reserve(b, len)) return;
	memcpy(b->data + b->len, text, len);
	b->len += len;
	b->data[b->len] = '\0';
}

void buf_puts(struct buf *b, const char *text) {
	buf_append(b, text, strlen(text));
}

void buf_printf(struct buf *b, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	va_list copy;
	va_copy(copy, ap);
	int n = vsnprintf(NULL, 0, fmt, copy);
	va_end(copy);
	if (n >= 0 && reserve(b, (size_t)n)) {
		vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
		b->len += (size_t)n;
	}
	va_end(ap);
}

void buf_string(struct buf *b, const char *text, size_t len) {
	buf_append(b, "\"", 1);
	const unsigned char *p = (const unsigned char *)(text ? text : "");
	if (!text) len = 0;
	size_t start = 0;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = p[i];
		const char *esc = NULL;
		char hex[8];
		switch (c) {
		case '"': esc = "\\\""; break;
		case '\\': esc = "\\\\"; break;
		case '\n': esc = "\\n"; break;
		case '\r': esc = "\\r"; break;
		case '\t': esc = "\\t"; break;
		default:
			if (c < 0x20) {
				snprintf(hex, sizeof(hex), "\\u%04x", c);
				esc = hex;
			}
		}
		if (!esc) continue;
		buf_append(b, (const char *)p + start, i - start);
		buf_puts(b, esc);
		start = i + 1;
	}
	buf_append(b, (const char *)p + start, len - start);
	buf_append(b, "\"", 1);
}

void buf_cstring(struct buf *b, const char *text) {
	buf_string(b, text, text ? strlen(text) : 0);
}

void buf_number(struct buf *b, double number) {
	if (!isfinite(number)) buf_puts(b, "null");
	else if (number == floor(number) && fabs(number) < 1e15) buf_printf(b, "%.0f", number);
	else buf_printf(b, "%.17g", number);
}

void buf_json(struct buf *b, const struct json *v) {
	if (!v) {
		buf_puts(b, "null");
		return;
	}
	switch (v->type) {
	case JSON_NULL: buf_puts(b, "null"); break;
	case JSON_BOOL: buf_puts(b, v->boolean ? "true" : "false"); break;
	case JSON_NUMBER:
		if (v->raw) buf_puts(b, v->raw);
		else buf_number(b, v->number);
		break;
	case JSON_STRING: buf_string(b, v->string, v->len); break;
	case JSON_ARRAY:
	case JSON_OBJECT:
		buf_puts(b, v->type == JSON_ARRAY ? "[" : "{");
		for (size_t i = 0; i < v->count; i++) {
			if (i) buf_puts(b, ",");
			if (v->type == JSON_OBJECT) {
				buf_cstring(b, v->keys[i]);
				buf_puts(b, ":");
			}
			buf_json(b, v->items[i]);
		}
		buf_puts(b, v->type == JSON_ARRAY ? "]" : "}");
		break;
	}
}

/* ---- reader ----------------------------------------------------------- */

struct parser {
	const char *p, *end;
	const char *error;
};

static void skip_space(struct parser *ps) {
	while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' ||
			*ps->p == '\n' || *ps->p == '\r'))
		ps->p++;
}

static struct json *fail(struct parser *ps, const char *message) {
	if (!ps->error) ps->error = message;
	return NULL;
}

static struct json *new_value(enum json_type type) {
	struct json *v = calloc(1, sizeof(*v));
	if (v) v->type = type;
	return v;
}

static bool read_hex4(struct parser *ps, uint32_t *out) {
	if (ps->end - ps->p < 4) return false;
	uint32_t v = 0;
	for (int i = 0; i < 4; i++) {
		char c = ps->p[i];
		v <<= 4;
		if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
		else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
		else return false;
	}
	ps->p += 4;
	*out = v;
	return true;
}

static void put_utf8(struct buf *b, uint32_t cp) {
	char out[4];
	size_t n;
	if (cp < 0x80) { out[0] = (char)cp; n = 1; }
	else if (cp < 0x800) { out[0] = (char)(0xc0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3f)); n = 2; }
	else if (cp < 0x10000) {
		out[0] = (char)(0xe0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		out[2] = (char)(0x80 | (cp & 0x3f)); n = 3;
	} else {
		out[0] = (char)(0xf0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
		out[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); out[3] = (char)(0x80 | (cp & 0x3f)); n = 4;
	}
	buf_append(b, out, n);
}

/* A string after its opening quote, into `out`. */
static bool parse_string_into(struct parser *ps, struct buf *out) {
	buf_append(out, "", 0);
	while (ps->p < ps->end) {
		unsigned char c = (unsigned char)*ps->p++;
		if (c == '"') return !out->failed;
		if (c < 0x20) return fail(ps, "control character in a string"), false;
		if (c != '\\') {
			buf_append(out, (const char *)&c, 1);
			continue;
		}
		if (ps->p >= ps->end) break;
		char e = *ps->p++;
		switch (e) {
		case '"': buf_append(out, "\"", 1); break;
		case '\\': buf_append(out, "\\", 1); break;
		case '/': buf_append(out, "/", 1); break;
		case 'b': buf_append(out, "\b", 1); break;
		case 'f': buf_append(out, "\f", 1); break;
		case 'n': buf_append(out, "\n", 1); break;
		case 'r': buf_append(out, "\r", 1); break;
		case 't': buf_append(out, "\t", 1); break;
		case 'u': {
			uint32_t cp;
			if (!read_hex4(ps, &cp)) return fail(ps, "bad \\u escape"), false;
			if (cp >= 0xd800 && cp <= 0xdbff) {
				uint32_t lo;
				if (ps->end - ps->p < 2 || ps->p[0] != '\\' || ps->p[1] != 'u')
					return fail(ps, "lone surrogate"), false;
				ps->p += 2;
				if (!read_hex4(ps, &lo) || lo < 0xdc00 || lo > 0xdfff)
					return fail(ps, "bad surrogate pair"), false;
				cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
			} else if (cp >= 0xdc00 && cp <= 0xdfff) {
				return fail(ps, "lone surrogate"), false;
			}
			put_utf8(out, cp);
			break;
		}
		default:
			return fail(ps, "bad escape"), false;
		}
	}
	return fail(ps, "unterminated string"), false;
}

static struct json *parse_value(struct parser *ps, int depth);

static bool push(struct json *v, struct json *item, char *key) {
	if ((v->count & (v->count - 1)) == 0) { /* 0, 1, 2, 4, ...: grow */
		size_t cap = v->count ? v->count * 2 : 1;
		struct json **items = realloc(v->items, cap * sizeof(*items));
		if (!items) return false;
		v->items = items;
		if (v->type == JSON_OBJECT) {
			char **keys = realloc(v->keys, cap * sizeof(*keys));
			if (!keys) return false;
			v->keys = keys;
		}
	}
	v->items[v->count] = item;
	if (v->type == JSON_OBJECT) v->keys[v->count] = key;
	v->count++;
	return true;
}

static struct json *parse_container(struct parser *ps, int depth, bool object) {
	struct json *v = new_value(object ? JSON_OBJECT : JSON_ARRAY);
	if (!v) return fail(ps, "out of memory");
	char close = object ? '}' : ']';
	skip_space(ps);
	if (ps->p < ps->end && *ps->p == close) {
		ps->p++;
		return v;
	}
	for (;;) {
		char *key = NULL;
		if (object) {
			skip_space(ps);
			if (ps->p >= ps->end || *ps->p != '"') goto bad;
			ps->p++;
			struct buf k = {0};
			if (!parse_string_into(ps, &k)) {
				buf_free(&k);
				goto bad;
			}
			key = k.data;
			skip_space(ps);
			if (ps->p >= ps->end || *ps->p != ':') {
				free(key);
				goto bad;
			}
			ps->p++;
		}
		struct json *item = parse_value(ps, depth + 1);
		if (!item || !push(v, item, key)) {
			json_free(item);
			free(key);
			goto bad;
		}
		skip_space(ps);
		if (ps->p < ps->end && *ps->p == ',') {
			ps->p++;
			continue;
		}
		if (ps->p < ps->end && *ps->p == close) {
			ps->p++;
			return v;
		}
		goto bad;
	}
bad:
	json_free(v);
	return fail(ps, object ? "malformed object" : "malformed array");
}

static struct json *parse_value(struct parser *ps, int depth) {
	if (depth > MAX_DEPTH) return fail(ps, "nested too deeply");
	skip_space(ps);
	if (ps->p >= ps->end) return fail(ps, "unexpected end");
	char c = *ps->p;
	if (c == '{' || c == '[') {
		ps->p++;
		return parse_container(ps, depth, c == '{');
	}
	if (c == '"') {
		ps->p++;
		struct buf s = {0};
		if (!parse_string_into(ps, &s)) {
			buf_free(&s);
			return NULL;
		}
		struct json *v = new_value(JSON_STRING);
		if (!v) {
			buf_free(&s);
			return fail(ps, "out of memory");
		}
		v->string = s.data;
		v->len = s.len;
		return v;
	}
	static const struct { const char *word; enum json_type type; bool value; } words[] = {
		{ "true", JSON_BOOL, true }, { "false", JSON_BOOL, false }, { "null", JSON_NULL, false },
	};
	for (size_t i = 0; i < 3; i++) {
		size_t n = strlen(words[i].word);
		if ((size_t)(ps->end - ps->p) >= n && !memcmp(ps->p, words[i].word, n)) {
			ps->p += n;
			struct json *v = new_value(words[i].type);
			if (v) v->boolean = words[i].value;
			return v ? v : fail(ps, "out of memory");
		}
	}
	/* A number, by JSON's grammar. */
	const char *start = ps->p;
	if (ps->p < ps->end && *ps->p == '-') ps->p++;
	if (ps->p < ps->end && *ps->p == '0') ps->p++;
	else if (ps->p < ps->end && *ps->p >= '1' && *ps->p <= '9')
		while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ps->p++;
	else return fail(ps, "unexpected character");
	if (ps->p < ps->end && *ps->p == '.') {
		ps->p++;
		if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9') return fail(ps, "bad number");
		while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ps->p++;
	}
	if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
		ps->p++;
		if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-')) ps->p++;
		if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9') return fail(ps, "bad number");
		while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ps->p++;
	}
	struct json *v = new_value(JSON_NUMBER);
	if (!v) return fail(ps, "out of memory");
	size_t n = (size_t)(ps->p - start);
	v->raw = malloc(n + 1);
	if (!v->raw) {
		free(v);
		return fail(ps, "out of memory");
	}
	memcpy(v->raw, start, n);
	v->raw[n] = '\0';
	v->number = strtod(v->raw, NULL);
	return v;
}

struct json *json_parse(const char *text, size_t len, const char **error) {
	struct parser ps = { .p = text, .end = text + len };
	struct json *v = parse_value(&ps, 0);
	if (v) {
		skip_space(&ps);
		if (ps.p != ps.end) {
			json_free(v);
			v = NULL;
			ps.error = "trailing characters";
		}
	}
	if (!v && error) *error = ps.error ? ps.error : "malformed JSON";
	return v;
}

void json_free(struct json *v) {
	if (!v) return;
	for (size_t i = 0; i < v->count; i++) {
		json_free(v->items[i]);
		if (v->keys) free(v->keys[i]);
	}
	free(v->items);
	free(v->keys);
	free(v->string);
	free(v->raw);
	free(v);
}

const struct json *json_get(const struct json *object, const char *key) {
	if (!object || object->type != JSON_OBJECT) return NULL;
	for (size_t i = 0; i < object->count; i++)
		if (!strcmp(object->keys[i], key)) return object->items[i];
	return NULL;
}

const char *json_get_string(const struct json *object, const char *key) {
	const struct json *v = json_get(object, key);
	return v && v->type == JSON_STRING ? v->string : NULL;
}

double json_get_number(const struct json *object, const char *key, double fallback) {
	const struct json *v = json_get(object, key);
	return v && v->type == JSON_NUMBER ? v->number : fallback;
}

bool json_has(const struct json *object, const char *key) {
	const struct json *v = json_get(object, key);
	return v && v->type != JSON_NULL;
}
