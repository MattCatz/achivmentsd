#ifndef ACHD_CONFIG_H
#define ACHD_CONFIG_H

#include <stdbool.h>

#define ACHD_UA_TOKEN "achivmentsd/" ACHD_VERSION

/* Strings point at literals, argv, or the parsed config, all of which live until exit. */
struct config {
	const char *proxy_host;
	int proxy_port;
	const char *upstream_host;   /* scheme://host[:port], no trailing slash */
	const char *state_dir;
	const char *ca_file;         /* NULL: first well-known bundle that exists */
	bool insecure_tls;
	const char *retroarch_cfg;   /* credential fallback; NULL disables */
};

extern struct config cfg;

void config_defaults(void);
/* A missing file is only an error when required is set. */
int config_load(const char *path, bool required);

#endif
