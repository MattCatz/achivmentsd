#ifndef ACHD_MSG_H
#define ACHD_MSG_H

#include <stdbool.h>
#include <stdint.h>

#include <libubox/blobmsg.h>
#include <libubox/blobmsg_json.h>

#include "util.h"

/*
 * RetroAchievements JSON handled as blobmsg. JSON integers arrive as INT32 or
 * INT64 depending on magnitude, so integer fields use BLOBMSG_CAST_INT64.
 */

/* RetroAchievements' "unsupported emulator" pseudo-achievement. */
#define RA_CLIENT_WARNING_ID 101000001u

/* The members of a table: a blob_buf root or a nested blobmsg table. */
struct msg_table {
	void *data;
	size_t len;
};

static inline struct msg_table msg_root(struct blob_buf *b)
{
	return (struct msg_table){ blob_data(b->head), blob_len(b->head) };
}

static inline struct msg_table msg_sub(struct blob_attr *a)
{
	return (struct msg_table){ blobmsg_data(a), blobmsg_data_len(a) };
}

/* (Re)initialises b with the members of a JSON object; false for anything else. */
bool msg_parse(struct blob_buf *b, const char *json);
struct blob_attr *msg_attr(struct msg_table t, const char *name, int type);
bool msg_success(struct msg_table t);
int64_t msg_int(struct msg_table t, const char *name, int64_t def);
const char *msg_str(struct msg_table t, const char *name);
/* Adds every ID from an array of integers or of tables carrying "ID". */
void msg_collect_ids(struct blob_attr *array, struct idset *out);
/* JSON text of a blob_buf's root table (malloc'd). */
char *msg_to_json(struct blob_buf *b);

#endif
