#ifndef ACHD_UPSTREAM_H
#define ACHD_UPSTREAM_H

#include <stdbool.h>

#include "util.h"

enum up_result {
	UP_OK,          /* 2xx */
	UP_HTTP_ERROR,  /* any other status; body is the server's */
	UP_NET_ERROR,   /* no HTTP response at all */
};

/* Background work (replay, refresh) stops for 10 minutes after a 429. */
#define UP_BACKGROUND 1

struct up_req;
typedef void (*up_cb)(struct up_req *r, void *priv);

struct up_req {
	enum up_result result;
	int status;
	char *content_type;
	struct buf body;
	char *error;
	/* private below */
	struct up_priv *p;
};

/*
 * Starts are spaced 300 ms apart. path is appended to upstream_host. The
 * callback owns r and must up_free() it. DNS resolution and connect inside
 * uclient block the event loop, so each attempt while the network is down
 * stalls everything for up to the connect timeout or the resolver's timeout.
 */
struct up_req *up_send(const char *method, const char *path, const char *body, const char *user_agent,
		       const char *content_type, unsigned flags, up_cb cb, void *priv);
/* Releases r; an unfinished request is aborted and its callback never runs. */
void up_free(struct up_req *r);
bool up_background_paused(void);
int up_init(void);
void up_shutdown(void);

#endif
