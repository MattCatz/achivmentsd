#ifndef ACHD_HTTP_SERVER_H
#define ACHD_HTTP_SERVER_H

#include <stdbool.h>
#include <stddef.h>

#include <libubox/uloop.h>
#include <libubox/ustream.h>

#include "util.h"

struct up_req;

struct request {
	char method[8];
	char *path;         /* request target as sent */
	const char *query;  /* points into path after '?', or "" */
	char *body;
	size_t body_len;
	char *user_agent;
	char *content_type;
};

struct client {
	struct ustream_fd sfd;
	struct uloop_timeout timer;
	struct buf in;
	size_t header_len;
	size_t content_length;
	bool dispatched;
	bool responded;
	struct request req;
	/* router state */
	struct up_req *up;
	int route_kind;
};

int http_server_init(const char *host, int port);
void http_server_shutdown(void);

/* Exactly one response per client; Connection: close is always sent. */
void client_respond(struct client *c, int status, const char *content_type, const char *body, size_t len);
void client_respond_json(struct client *c, int status, const char *json);

/* Request parameter from the query string, else the form body (malloc'd). */
char *req_param(const struct request *r, const char *name);

#endif
