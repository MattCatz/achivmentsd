#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <libubox/blobmsg_json.h>

#include "config.h"
#include "util.h"

struct config cfg;

/* Backs the config strings for the life of the process. */
static struct blob_buf cfg_blob;

enum {
	CFG_PROXY_HOST,
	CFG_PROXY_PORT,
	CFG_UPSTREAM_HOST,
	CFG_STATE_DIR,
	CFG_CA_FILE,
	CFG_INSECURE_TLS,
	CFG_RETROARCH_CFG,
	__CFG_MAX
};

static const struct blobmsg_policy cfg_policy[__CFG_MAX] = {
	[CFG_PROXY_HOST] = { "proxy_host", BLOBMSG_TYPE_STRING },
	[CFG_PROXY_PORT] = { "proxy_port", BLOBMSG_CAST_INT64 },
	[CFG_UPSTREAM_HOST] = { "upstream_host", BLOBMSG_TYPE_STRING },
	[CFG_STATE_DIR] = { "state_dir", BLOBMSG_TYPE_STRING },
	[CFG_CA_FILE] = { "ca_file", BLOBMSG_TYPE_STRING },
	[CFG_INSECURE_TLS] = { "insecure_tls", BLOBMSG_TYPE_BOOL },
	[CFG_RETROARCH_CFG] = { "retroarch_cfg", BLOBMSG_TYPE_STRING },
};

void config_defaults(void)
{
	cfg = (struct config){
		.proxy_host = "127.0.0.1",
		.proxy_port = 8080,
		.upstream_host = "https://retroachievements.org",
		.state_dir = "/var/lib/achivmentsd",
	};
}

int config_load(const char *path, bool required)
{
	struct blob_attr *tb[__CFG_MAX];

	if (access(path, R_OK) && errno == ENOENT && !required)
		return 0;

	blob_buf_init(&cfg_blob, 0);
	if (!blobmsg_add_json_from_file(&cfg_blob, path)) {
		ULOG_ERR("config %s is missing or not a JSON object\n", path);
		return -1;
	}
	blobmsg_parse(cfg_policy, __CFG_MAX, tb, blob_data(cfg_blob.head), blob_len(cfg_blob.head));

	if (tb[CFG_PROXY_HOST])
		cfg.proxy_host = blobmsg_get_string(tb[CFG_PROXY_HOST]);
	if (tb[CFG_PROXY_PORT])
		cfg.proxy_port = (int)blobmsg_cast_s64(tb[CFG_PROXY_PORT]);
	if (tb[CFG_UPSTREAM_HOST]) {
		char *u = blobmsg_get_string(tb[CFG_UPSTREAM_HOST]);
		size_t len = strlen(u);

		while (len && u[len - 1] == '/')
			u[--len] = 0;
		cfg.upstream_host = u;
	}
	if (tb[CFG_STATE_DIR])
		cfg.state_dir = blobmsg_get_string(tb[CFG_STATE_DIR]);
	if (tb[CFG_CA_FILE])
		cfg.ca_file = blobmsg_get_string(tb[CFG_CA_FILE]);
	if (tb[CFG_INSECURE_TLS])
		cfg.insecure_tls = blobmsg_get_bool(tb[CFG_INSECURE_TLS]);
	if (tb[CFG_RETROARCH_CFG])
		cfg.retroarch_cfg = blobmsg_get_string(tb[CFG_RETROARCH_CFG]);
	return 0;
}
