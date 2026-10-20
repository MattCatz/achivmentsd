#ifndef ACHD_UTIL_H
#define ACHD_UTIL_H

#include <dirent.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <libubox/blob.h>
#include <libubox/ulog.h>
#include <libubox/utils.h>

#define ULOG_DBG(fmt, ...) ulog(LOG_DEBUG, fmt, ##__VA_ARGS__)

/* Scope-bound cleanup. TAKE_PTR() hands a pointer out of a guarded variable. */
#define _cleanup_(f) __attribute__((cleanup(f)))
#define _autofree_ _cleanup_(freep)
#define _autoblob_ _cleanup_(blob_buf_freep)
#define _autoclose_ _cleanup_(closep)
#define _autoclosedir_ _cleanup_(closedirp)
#define _autobuf_ _cleanup_(buf_freep)
#define _autoidset_ _cleanup_(idset_freep)

#define TAKE_PTR(p)                         \
	({                                  \
		__typeof__(p) _ptr = (p);   \
		(p) = NULL;                 \
		_ptr;                       \
	})

#define TAKE_FD(fd)                         \
	({                                  \
		int _fd = (fd);             \
		(fd) = -1;                  \
		_fd;                        \
	})

static inline void freep(void *p)
{
	free(*(void **)p);
}


static inline void blob_buf_freep(struct blob_buf *b)
{
	blob_buf_free(b);
}

static inline void closep(int *fd)
{
	if (*fd >= 0)
		close(*fd);
}

static inline void closedirp(DIR **d)
{
	if (*d)
		closedir(*d);
}

/* Growable string buffer; data is NUL-terminated once anything was appended. */
struct buf {
	char *data;
	size_t len;
	size_t cap;
};

int buf_append(struct buf *b, const void *d, size_t n);
int buf_puts(struct buf *b, const char *s);
int buf_printf(struct buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void buf_free(struct buf *b);
/* Returns the data (never NULL) and leaves the buffer empty. */
char *buf_steal(struct buf *b);

static inline void buf_freep(struct buf *b)
{
	buf_free(b);
}

/* Small unordered set of achievement IDs; 0 is never stored. */
struct idset {
	uint32_t *v;
	size_t n;
	size_t cap;
};

bool idset_has(const struct idset *s, uint32_t id);
int idset_add(struct idset *s, uint32_t id);
void idset_free(struct idset *s);

static inline void idset_freep(struct idset *s)
{
	idset_free(s);
}

/* Whole file, NUL-terminated (malloc'd), or NULL with errno set. */
char *read_file(const char *path, size_t *len);
/* mkdir -p with mode 0700. */
int make_dirs(const char *path);

/* asprintf() that returns NULL on failure. */
char *xasprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *strdup_lower(const char *s);
int64_t mono_ms(void);

#endif
