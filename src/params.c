#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "params.h"
#include "util.h"

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static char *url_decode(const char *s, size_t len)
{
	char *out = malloc(len + 1), *o = out;

	if (!out)
		return NULL;
	for (size_t i = 0; i < len; i++) {
		if (s[i] == '+') {
			*o++ = ' ';
		} else if (s[i] == '%' && i + 2 < len && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
			*o++ = (char)(hexval(s[i + 1]) << 4 | hexval(s[i + 2]));
			i += 2;
		} else {
			*o++ = s[i];
		}
	}
	*o = 0;
	return out;
}

char *url_encode(const char *s)
{
	static const char hex[] = "0123456789ABCDEF";
	struct buf b = { 0 };

	for (; *s; s++) {
		unsigned char c = *s;

		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		    c == '-' || c == '_' || c == '.' || c == '~') {
			buf_append(&b, s, 1);
		} else {
			char e[3] = { '%', hex[c >> 4], hex[c & 15] };

			buf_append(&b, e, 3);
		}
	}
	return buf_steal(&b);
}

/* Calls cb for each key=value pair; stops when cb returns false. */
static void form_each(const char *form, bool (*cb)(const char *k, size_t kl, const char *v, size_t vl, void *p), void *p)
{
	const char *s = form;

	while (s && *s) {
		const char *end = strchr(s, '&');
		size_t pl = end ? (size_t)(end - s) : strlen(s);
		const char *eq = memchr(s, '=', pl);
		size_t kl = eq ? (size_t)(eq - s) : pl;

		if (pl && !cb(s, kl, eq ? eq + 1 : s + pl, eq ? pl - kl - 1 : 0, p))
			return;
		s = end ? end + 1 : NULL;
	}
}

struct get_ctx {
	const char *name;
	char *out;
};

static bool get_cb(const char *k, size_t kl, const char *v, size_t vl, void *p)
{
	struct get_ctx *g = p;

	if (strlen(g->name) != kl || memcmp(k, g->name, kl))
		return true;
	g->out = url_decode(v, vl);
	return false;
}

char *form_get(const char *form, const char *name)
{
	struct get_ctx g = { name, NULL };

	form_each(form, get_cb, &g);
	return g.out;
}

struct set_ctx {
	const char *name;
	const char *enc;
	bool done;
	struct buf b;
};

static bool set_cb(const char *k, size_t kl, const char *v, size_t vl, void *p)
{
	struct set_ctx *c = p;
	bool match = strlen(c->name) == kl && !memcmp(k, c->name, kl);

	if (match && c->done)
		return true;
	if (c->b.len)
		buf_append(&c->b, "&", 1);
	buf_append(&c->b, k, kl);
	buf_append(&c->b, "=", 1);
	if (match) {
		buf_puts(&c->b, c->enc);
		c->done = true;
	} else {
		buf_append(&c->b, v, vl);
	}
	return true;
}

char *form_set(const char *form, const char *name, const char *value)
{
	_autofree_ char *enc = url_encode(value);
	struct set_ctx c = { .name = name, .enc = enc };

	form_each(form, set_cb, &c);
	if (!c.done)
		buf_printf(&c.b, "%s%s=%s", c.b.len ? "&" : "", name, c.enc);
	return buf_steal(&c.b);
}
