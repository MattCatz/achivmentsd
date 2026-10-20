#include <stdio.h>
#include <string.h>

#include <libubox/blobmsg.h>
#include <libubox/list.h>
#include <libubox/uclient.h>
#include <libubox/uloop.h>
#include <libubox/ustream-ssl.h>

#include "config.h"
#include "netstate.h"
#include "upstream.h"

#define MIN_START_INTERVAL_MS 300
#define CONNECT_TIMEOUT_MS 3000
#define REQUEST_TIMEOUT_MS 15000
#define MAX_RESPONSE_BYTES (16 << 20)
#define RATE_LIMIT_PAUSE_MS (10 * 60 * 1000)
#define MAX_REDIRECTS 3

/* Lives in the same allocation as its up_req, strings included. */
struct up_priv {
	struct up_req *r;
	struct list_head list;
	struct uclient *cl;
	struct uloop_timeout finish;
	const char *url;
	const char *method;
	const char *post;    /* NULL unless POST */
	const char *ua;
	const char *ctype;   /* NULL: form encoding */
	unsigned flags;
	int redirects;
	bool queued;
	bool finished;
	up_cb cb;
	void *cb_priv;
};

static const struct ustream_ssl_ops *ssl_ops;
static struct ustream_ssl_ctx *ssl_ctx;
static LIST_HEAD(queue);
static struct uloop_timeout pump_timer;
static int64_t last_start;
static int64_t paused_until;

bool up_background_paused(void)
{
	return paused_until > mono_ms();
}

static const char *meta_get(struct uclient *cl, const char *name)
{
	const struct blobmsg_policy policy = { .name = name, .type = BLOBMSG_TYPE_STRING };
	struct blob_attr *attr = NULL;

	if (cl->meta)
		blobmsg_parse(&policy, 1, &attr, blob_data(cl->meta), blob_len(cl->meta));
	return attr ? blobmsg_get_string(attr) : NULL;
}

static void finish_cb(struct uloop_timeout *t)
{
	struct up_priv *p = container_of(t, struct up_priv, finish);
	struct up_req *r = p->r;

	if (p->cl)
		uclient_free(TAKE_PTR(p->cl));
	net_set_online(r->result != UP_NET_ERROR, r->error);
	p->cb(r, p->cb_priv);
}

/* Deferred so callers never see their callback before up_send() returns. */
static void finish(struct up_priv *p, enum up_result result, const char *error)
{
	if (p->finished)
		return;
	p->finished = true;
	p->r->result = result;
	p->r->error = xstrdup(error);
	p->finish.cb = finish_cb;
	uloop_timeout_set(&p->finish, 0);
}

static void header_done_cb(struct uclient *cl)
{
	struct up_priv *p = cl->priv;
	const char *v;

	if (p->finished)
		return;
	if (uclient_http_status_redirect(cl) && !strcmp(p->method, "GET") && p->redirects < MAX_REDIRECTS) {
		p->redirects++;
		if (uclient_http_redirect(cl) > 0)
			return;
	}
	p->r->status = cl->status_code;
	p->r->content_type = xstrdup(meta_get(cl, "content-type"));
	if (cl->status_code == 429) {
		int64_t pause = RATE_LIMIT_PAUSE_MS;

		if ((v = meta_get(cl, "retry-after")) && atoll(v) * 1000 > pause)
			pause = atoll(v) * 1000;
		paused_until = mono_ms() + pause;
		ULOG_WARN("rate limited by upstream; background work paused for %llds\n", (long long)pause / 1000);
	}
}

static void data_read_cb(struct uclient *cl)
{
	struct up_priv *p = cl->priv;
	char chunk[4096];
	int n;

	while ((n = uclient_read(cl, chunk, sizeof(chunk))) > 0) {
		if (p->finished)
			continue;
		if (p->r->body.len + n > MAX_RESPONSE_BYTES)
			finish(p, UP_NET_ERROR, "response too large");
		else
			buf_append(&p->r->body, chunk, n);
	}
}

static void data_eof_cb(struct uclient *cl)
{
	struct up_priv *p = cl->priv;
	int s = p->r->status;

	if (!s)
		finish(p, UP_NET_ERROR, "connection closed before a response");
	else
		finish(p, s >= 200 && s < 300 ? UP_OK : UP_HTTP_ERROR, NULL);
}

static void error_cb(struct uclient *cl, int code)
{
	finish(cl->priv, UP_NET_ERROR, uclient_strerror(code));
}

static void log_cb(struct uclient *cl, enum uclient_log_type type, const char *msg)
{
	ULOG_DBG("uclient: %s\n", msg);
}

static const struct uclient_cb client_cb = {
	.header_done = header_done_cb,
	.data_read = data_read_cb,
	.data_eof = data_eof_cb,
	.error = error_cb,
	.log_msg = log_cb,
};

/* Returns an error message, or NULL once the request is on the wire. */
static const char *start(struct up_priv *p)
{
	char len[24];

	p->cl = uclient_new(p->url, NULL, &client_cb);
	if (!p->cl)
		return "bad upstream url";
	p->cl->priv = p;
	if (!strncmp(p->url, "https://", 8)) {
		if (!ssl_ctx)
			return "TLS unavailable";
		uclient_http_set_ssl_ctx(p->cl, ssl_ops, ssl_ctx, !cfg.insecure_tls);
	}
	/*
	 * uclient resolves and connects synchronously, stalling the event loop
	 * for as long as the connect timeout; keep that short and use the longer
	 * timeout only for the response.
	 */
	uclient_set_timeout(p->cl, CONNECT_TIMEOUT_MS);
	if (uclient_connect(p->cl))
		return "connect failed";
	uclient_set_timeout(p->cl, REQUEST_TIMEOUT_MS);
	uclient_http_set_request_type(p->cl, p->method);
	uclient_http_reset_headers(p->cl);
	uclient_http_set_header(p->cl, "User-Agent", p->ua);
	if (p->post) {
		snprintf(len, sizeof(len), "%zu", strlen(p->post));
		uclient_http_set_header(p->cl, "Content-Type", p->ctype ? p->ctype : "application/x-www-form-urlencoded");
		/* Without Content-Length uclient chunks the body, which RA's PHP backend drops. */
		uclient_http_set_header(p->cl, "Content-Length", len);
		uclient_write(p->cl, p->post, strlen(p->post));
	}
	return uclient_request(p->cl) ? "request failed" : NULL;
}

static void pump(struct uloop_timeout *t)
{
	while (!list_empty(&queue)) {
		int64_t wait = last_start + MIN_START_INTERVAL_MS - mono_ms();
		struct up_priv *p;
		const char *err;

		if (wait > 0) {
			uloop_timeout_set(&pump_timer, (int)wait);
			return;
		}
		p = list_first_entry(&queue, struct up_priv, list);
		list_del(&p->list);
		p->queued = false;
		last_start = mono_ms();

		if ((p->flags & UP_BACKGROUND) && up_background_paused()) {
			p->r->status = 429;
			finish(p, UP_HTTP_ERROR, "rate limited");
		} else if ((err = start(p))) {
			finish(p, UP_NET_ERROR, err);
		}
	}
}

static char *copy_str(char **dst, const char *src)
{
	return src ? strcpy(*dst, src) : NULL;
}

struct up_req *up_send(const char *method, const char *path, const char *body, const char *user_agent,
		       const char *content_type, unsigned flags, up_cb cb, void *priv)
{
	const char *post = strcmp(method, "POST") ? NULL : (body ? body : "");
	const char *ua = user_agent && *user_agent ? user_agent : ACHD_UA_TOKEN;
	size_t url_len = strlen(cfg.upstream_host) + strlen(path) + 1;
	char *url, *m, *b, *u, *ct;
	struct up_priv *p;
	struct up_req *r;

	r = calloc_a(sizeof(*r), &p, sizeof(*p),
		     &url, url_len,
		     &m, strlen(method) + 1,
		     &b, post ? strlen(post) + 1 : 1,
		     &u, strlen(ua) + 1,
		     &ct, content_type ? strlen(content_type) + 1 : 1);
	if (!r)
		return NULL;
	snprintf(url, url_len, "%s%s", cfg.upstream_host, path);
	r->p = p;
	p->r = r;
	p->url = url;
	p->method = strcpy(m, method);
	p->post = copy_str(&b, post);
	p->ua = strcpy(u, ua);
	p->ctype = copy_str(&ct, content_type);
	p->flags = flags;
	p->cb = cb;
	p->cb_priv = priv;
	p->queued = true;
	list_add_tail(&p->list, &queue);
	pump(NULL);
	return r;
}

void up_free(struct up_req *r)
{
	struct up_priv *p;

	if (!r)
		return;
	p = r->p;
	if (p->queued)
		list_del(&p->list);
	uloop_timeout_cancel(&p->finish);
	if (p->cl)
		uclient_free(p->cl);
	free(r->content_type);
	free(r->error);
	buf_free(&r->body);
	free(r);
}

int up_init(void)
{
	static const char *const bundles[] = {
		"/etc/ssl/certs/ca-certificates.crt",
		"/etc/ssl/cert.pem",
		"/etc/pki/tls/certs/ca-bundle.crt",
		"/etc/ssl/ca-bundle.pem",
		"/etc/ssl/certs/ca-bundle.crt",
	};
	const char *ca = cfg.ca_file;

	pump_timer.cb = pump;

#ifdef ACHD_DLOPEN_SSL
	ssl_ctx = uclient_new_ssl_context(&ssl_ops);
#else
	ssl_ops = &ustream_ssl_ops;
	ssl_ctx = ssl_ops->context_new(false);
#endif
	if (!ssl_ctx) {
		ULOG_ERR("cannot create TLS context; only http:// upstreams will work\n");
		return 0;
	}

	for (size_t i = 0; !ca && i < ARRAY_SIZE(bundles); i++)
		if (!access(bundles[i], R_OK))
			ca = bundles[i];
	if (ca && !ssl_ops->context_add_ca_crt_file(ssl_ctx, ca))
		ULOG_INFO("CA bundle: %s\n", ca);
	else if (cfg.insecure_tls)
		ULOG_WARN("no CA bundle; upstream certificates are NOT verified (insecure_tls)\n");
	else
		ULOG_ERR("no usable CA bundle%s%s; HTTPS upstream requests will fail until ca_file is set\n",
			 ca ? " at " : "", ca ? ca : "");
	return 0;
}

void up_shutdown(void)
{
	struct up_priv *p, *tmp;

	list_for_each_entry_safe(p, tmp, &queue, list)
		up_free(p->r);
	uloop_timeout_cancel(&pump_timer);
	if (ssl_ctx)
		ssl_ops->context_free(TAKE_PTR(ssl_ctx));
}
