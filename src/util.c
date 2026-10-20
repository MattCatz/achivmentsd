#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "util.h"

static int buf_reserve(struct buf *b, size_t extra)
{
	size_t need = b->len + extra + 1, cap;
	char *d;

	if (need <= b->cap)
		return 0;
	for (cap = b->cap ? b->cap : 256; cap < need; cap *= 2)
		;
	d = realloc(b->data, cap);
	if (!d)
		return -1;
	b->data = d;
	b->cap = cap;
	return 0;
}

int buf_append(struct buf *b, const void *d, size_t n)
{
	if (buf_reserve(b, n))
		return -1;
	memcpy(b->data + b->len, d, n);
	b->len += n;
	b->data[b->len] = 0;
	return 0;
}

int buf_puts(struct buf *b, const char *s)
{
	return buf_append(b, s, strlen(s));
}

int buf_printf(struct buf *b, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0 || buf_reserve(b, n))
		return -1;
	va_start(ap, fmt);
	vsnprintf(b->data + b->len, n + 1, fmt, ap);
	va_end(ap);
	b->len += n;
	return 0;
}

void buf_free(struct buf *b)
{
	free(b->data);
	memset(b, 0, sizeof(*b));
}

char *buf_steal(struct buf *b)
{
	char *d = b->data ? b->data : calloc(1, 1);

	memset(b, 0, sizeof(*b));
	return d;
}

bool idset_has(const struct idset *s, uint32_t id)
{
	for (size_t i = 0; i < s->n; i++)
		if (s->v[i] == id)
			return true;
	return false;
}

int idset_add(struct idset *s, uint32_t id)
{
	if (!id || idset_has(s, id))
		return 0;
	if (s->n == s->cap) {
		size_t cap = s->cap ? s->cap * 2 : 32;
		uint32_t *v = realloc(s->v, cap * sizeof(*v));

		if (!v)
			return -1;
		s->v = v;
		s->cap = cap;
	}
	s->v[s->n++] = id;
	return 0;
}

void idset_free(struct idset *s)
{
	free(s->v);
	memset(s, 0, sizeof(*s));
}

char *xasprintf(const char *fmt, ...)
{
	va_list ap;
	char *s;
	int n;

	va_start(ap, fmt);
	n = vasprintf(&s, fmt, ap);
	va_end(ap);
	return n < 0 ? NULL : s;
}

char *xstrdup(const char *s)
{
	return s ? strdup(s) : NULL;
}

char *xstrndup(const char *s, size_t n)
{
	char *d = malloc(n + 1);

	if (d) {
		memcpy(d, s, n);
		d[n] = 0;
	}
	return d;
}

char *strdup_lower(const char *s)
{
	char *d = xstrdup(s);

	for (char *p = d; p && *p; p++)
		*p = tolower((unsigned char)*p);
	return d;
}

int64_t mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

char *read_file(const char *path, size_t *len)
{
	_autoclose_ int fd = open(path, O_RDONLY);
	_autobuf_ struct buf b = { 0 };
	char chunk[8192];
	ssize_t n;

	if (fd < 0)
		return NULL;
	while ((n = read(fd, chunk, sizeof(chunk))) > 0)
		buf_append(&b, chunk, n);
	if (n < 0)
		return NULL;
	if (len)
		*len = b.len;
	return buf_steal(&b);
}

static int mkdir_one(const char *path)
{
	if (mkdir(path, 0700) && errno != EEXIST) {
		ULOG_ERR("mkdir %s: %s\n", path, strerror(errno));
		return -1;
	}
	return 0;
}

int make_dirs(const char *path)
{
	_autofree_ char *p = xstrdup(path);

	for (char *s = p + 1; *s; s++) {
		if (*s != '/')
			continue;
		*s = 0;
		if (mkdir_one(p))
			return -1;
		*s = '/';
	}
	return mkdir_one(p);
}
