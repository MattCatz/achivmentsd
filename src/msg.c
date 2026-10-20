#include "msg.h"

bool msg_parse(struct blob_buf *b, const char *json)
{
	blob_buf_init(b, 0);
	return json && blobmsg_add_json_from_string(b, json);
}

struct blob_attr *msg_attr(struct msg_table t, const char *name, int type)
{
	const struct blobmsg_policy policy = { .name = name, .type = type };
	struct blob_attr *attr = NULL;

	if (t.data)
		blobmsg_parse(&policy, 1, &attr, t.data, t.len);
	return attr;
}

bool msg_success(struct msg_table t)
{
	struct blob_attr *a = msg_attr(t, "Success", BLOBMSG_TYPE_BOOL);

	return a && blobmsg_get_bool(a);
}

int64_t msg_int(struct msg_table t, const char *name, int64_t def)
{
	struct blob_attr *a = msg_attr(t, name, BLOBMSG_CAST_INT64);

	return a ? blobmsg_cast_s64(a) : def;
}

const char *msg_str(struct msg_table t, const char *name)
{
	struct blob_attr *a = msg_attr(t, name, BLOBMSG_TYPE_STRING);

	return a ? blobmsg_get_string(a) : NULL;
}

void msg_collect_ids(struct blob_attr *array, struct idset *out)
{
	struct blob_attr *cur;
	size_t rem;

	if (!array || blobmsg_type(array) != BLOBMSG_TYPE_ARRAY)
		return;
	blobmsg_for_each_attr(cur, array, rem) {
		int64_t id = 0;

		if (blobmsg_type(cur) == BLOBMSG_TYPE_TABLE)
			id = msg_int(msg_sub(cur), "ID", 0);
		else if (blobmsg_type(cur) == BLOBMSG_TYPE_INT32 || blobmsg_type(cur) == BLOBMSG_TYPE_INT64)
			id = blobmsg_cast_s64(cur);
		if (id > 0 && id <= UINT32_MAX)
			idset_add(out, (uint32_t)id);
	}
}

char *msg_to_json(struct blob_buf *b)
{
	return blobmsg_format_json(b->head, true);
}
