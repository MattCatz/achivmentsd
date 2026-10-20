#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include <libubox/usock.h>

#include "http_server.h"
#include "msg.h"
#include "params.h"
#include "router.h"
#include "upstream.h"

#define MAX_HEADER_BYTES 16384
#define MAX_BODY_BYTES (1 << 20)
#define CLIENT_TIMEOUT_MS 30000
#define LINGER_MS 2000
#define MAX_CLIENTS 32

static struct uloop_fd listen_fd = { .fd = -1 };
static int nclients;

static const char *reason_phrase(int status)
{
	switch (status) {
	case 100: return "Continue";
	case 200: return "OK";
	case 204: return "No Content";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 413: return "Payload Too Large";
	case 422: return "Unprocessable Content";
	case 429: return "Too Many Requests";
	case 431: return "Request Header Fields Too Large";
	case 500: return "Internal Server Error";
	case 501: return "Not Implemented";
	case 502: return "Bad Gateway";
	case 503: return "Service Unavailable";
	default: return status < 300 ? "OK" : status < 500 ? "Client Error" : "Server Error";
	}
}

static void free_cb(struct uloop_timeout *t)
{
	struct client *c = container_of(t, struct client, timer);

	ustream_free(&c->sfd.stream);
	close(c->sfd.fd.fd);
	buf_free(&c->in);
	free(c->req.path);
	free(c->req.body);
	free(c->req.user_agent);
	free(c->req.content_type);
	free(c);
	nclients--;
}

/* Freeing is deferred so ustream and uclient callbacks never see a dead client. */
static void client_close(struct client *c)
{
	if (c->up) {
		up_free(c->up);
		c->up = NULL;
	}
	c->timer.cb = free_cb;
	uloop_timeout_set(&c->timer, 0);
}

static void timeout_cb(struct uloop_timeout *t)
{
	client_close(container_of(t, struct client, timer));
}

static void finish_write(struct client *c)
{
	/* FIN rather than RST, so a client still reading gets the whole body. */
	shutdown(c->sfd.fd.fd, SHUT_WR);
	uloop_timeout_set(&c->timer, LINGER_MS);
}

void client_respond(struct client *c, int status, const char *content_type, const char *body, size_t len)
{
	struct ustream *s = &c->sfd.stream;

	if (c->responded)
		return;
	c->responded = true;
	ustream_printf(s,
		       "HTTP/1.1 %d %s\r\n"
		       "Content-Type: %s\r\n"
		       "Content-Length: %zu\r\n"
		       "Connection: close\r\n\r\n",
		       status, reason_phrase(status), content_type ? content_type : "application/octet-stream", len);
	if (len)
		ustream_write(s, body, (int)len, false);
	if (!ustream_pending_data(s, true))
		finish_write(c);
}

void client_respond_json(struct client *c, int status, const char *json)
{
	client_respond(c, status, "application/json", json, strlen(json));
}

static void respond_error(struct client *c, int status, const char *msg)
{
	_autoblob_ struct blob_buf b = { 0 };
	_autofree_ char *json = NULL;

	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "Success", false);
	blobmsg_add_string(&b, "Error", msg);
	json = msg_to_json(&b);
	client_respond_json(c, status, json ? json : "{}");
}

char *req_param(const struct request *r, const char *name)
{
	char *v = form_get(r->query, name);

	return v ? v : form_get(r->body, name);
}

static bool is_loopback(int fd)
{
	struct sockaddr_storage ss;
	socklen_t sl = sizeof(ss);

	if (getpeername(fd, (struct sockaddr *)&ss, &sl))
		return false;
	if (ss.ss_family == AF_INET)
		return (ntohl(((struct sockaddr_in *)&ss)->sin_addr.s_addr) >> 24) == 127;
	if (ss.ss_family == AF_INET6) {
		const struct in6_addr *a = &((struct sockaddr_in6 *)&ss)->sin6_addr;

		return IN6_IS_ADDR_LOOPBACK(a) || (IN6_IS_ADDR_V4MAPPED(a) && a->s6_addr[12] == 127);
	}
	return false;
}

/* Returns 0 when headers are complete, 1 when more data is needed, else an HTTP status. */
static int parse_headers(struct client *c)
{
	char *end, *line, *next, *sp;
	bool chunked = false, expect_continue = false;

	end = memmem(c->in.data, c->in.len, "\r\n\r\n", 4);
	if (!end)
		return c->in.len > MAX_HEADER_BYTES ? 431 : 1;
	c->header_len = end - c->in.data + 4;
	*end = 0;

	line = c->in.data;
	next = strstr(line, "\r\n");
	if (next)
		*next = 0;
	sp = strchr(line, ' ');
	if (!sp || sp - line >= (int)sizeof(c->req.method))
		return 400;
	memcpy(c->req.method, line, sp - line);
	line = sp + 1;
	sp = strchr(line, ' ');
	if (!sp)
		return 400;
	c->req.path = xstrndup(line, sp - line);
	c->req.query = strchr(c->req.path, '?');
	c->req.query = c->req.query ? c->req.query + 1 : "";

	for (line = next ? next + 2 : NULL; line && *line; line = next) {
		char *val;

		next = strstr(line, "\r\n");
		if (next) {
			*next = 0;
			next += 2;
		}
		val = strchr(line, ':');
		if (!val)
			continue;
		*val++ = 0;
		while (*val == ' ' || *val == '\t')
			val++;
		if (!strcasecmp(line, "Content-Length")) {
			char *e;
			unsigned long long n = strtoull(val, &e, 10);

			if (*e || n > MAX_BODY_BYTES)
				return 413;
			c->content_length = n;
		} else if (!strcasecmp(line, "User-Agent")) {
			free(c->req.user_agent);
			c->req.user_agent = xstrdup(val);
		} else if (!strcasecmp(line, "Content-Type")) {
			free(c->req.content_type);
			c->req.content_type = xstrdup(val);
		} else if (!strcasecmp(line, "Transfer-Encoding")) {
			chunked = strcasestr(val, "chunked") != NULL;
		} else if (!strcasecmp(line, "Expect")) {
			expect_continue = !strcasecmp(val, "100-continue");
		}
	}
	if (chunked)
		return 501;
	if (expect_continue && c->content_length)
		ustream_printf(&c->sfd.stream, "HTTP/1.1 100 Continue\r\n\r\n");
	return 0;
}

static void try_dispatch(struct client *c)
{
	int ret;

	if (!c->header_len) {
		ret = parse_headers(c);
		if (ret == 1)
			return;
		if (ret) {
			c->dispatched = true;
			respond_error(c, ret, reason_phrase(ret));
			return;
		}
	}
	if (c->in.len - c->header_len < c->content_length)
		return;

	c->dispatched = true;
	c->req.body = xstrndup(c->in.data + c->header_len, c->content_length);
	c->req.body_len = c->content_length;

	if (!is_loopback(c->sfd.fd.fd)) {
		respond_error(c, 403, "loopback_only");
		return;
	}
	router_handle(c);
}

static void notify_read(struct ustream *s, int bytes)
{
	struct client *c = container_of(s, struct client, sfd.stream);
	char *data;
	int len;

	while ((data = ustream_get_read_buf(s, &len)) && len > 0) {
		if (!c->dispatched)
			buf_append(&c->in, data, len);
		ustream_consume(s, len);
	}
	if (!c->dispatched)
		try_dispatch(c);
}

static void notify_write(struct ustream *s, int bytes)
{
	struct client *c = container_of(s, struct client, sfd.stream);

	if (c->responded && !ustream_pending_data(s, true))
		finish_write(c);
}

static void notify_state(struct ustream *s)
{
	struct client *c = container_of(s, struct client, sfd.stream);

	/* A client that half-closes after sending its request still gets the answer. */
	if (s->write_error || (s->eof && (!c->dispatched || c->responded)))
		client_close(c);
}

static void listen_cb(struct uloop_fd *u, unsigned int events)
{
	for (;;) {
		struct client *c;
		int fd = accept(u->fd, NULL, NULL);

		if (fd < 0)
			return;
		if (nclients >= MAX_CLIENTS) {
			ULOG_WARN("too many clients, dropping connection\n");
			close(fd);
			continue;
		}
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
		fcntl(fd, F_SETFD, FD_CLOEXEC);

		c = calloc(1, sizeof(*c));
		if (!c) {
			close(fd);
			continue;
		}
		nclients++;
		c->sfd.stream.notify_read = notify_read;
		c->sfd.stream.notify_write = notify_write;
		c->sfd.stream.notify_state = notify_state;
		ustream_fd_init(&c->sfd, fd);
		c->timer.cb = timeout_cb;
		uloop_timeout_set(&c->timer, CLIENT_TIMEOUT_MS);
	}
}

int http_server_init(const char *host, int port)
{
	char portstr[8];

	snprintf(portstr, sizeof(portstr), "%d", port);
	listen_fd.fd = usock(USOCK_TCP | USOCK_SERVER | USOCK_NONBLOCK, host, portstr);
	if (listen_fd.fd < 0) {
		ULOG_ERR("cannot listen on %s:%d: %s\n", host, port, strerror(errno));
		return -1;
	}
	listen_fd.cb = listen_cb;
	uloop_fd_add(&listen_fd, ULOOP_READ);
	ULOG_INFO("listening on %s:%d\n", host, port);
	return 0;
}

void http_server_shutdown(void)
{
	if (listen_fd.fd < 0)
		return;
	uloop_fd_delete(&listen_fd);
	close(listen_fd.fd);
	listen_fd.fd = -1;
}
