#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <libubox/avl.h>
#include <libubox/runqueue.h>
#include <libubox/uloop.h>

#include "config.h"
#include "creds.h"
#include "db.h"
#include "msg.h"
#include "netstate.h"
#include "params.h"
#include "refresh.h"
#include "upstream.h"

#define RECENT_PLAY_SECONDS (7 * 24 * 60 * 60)
#define STALE_AFTER_SECONDS (24 * 60 * 60)

/* One allocation (calloc_a) holding the job and its strings. */
struct job {
	struct runqueue_task task;
	struct avl_node node;         /* in `jobs`, keyed by `key` */
	struct uloop_timeout delay;
	struct up_req *up;
	struct cache_key key;
	struct cache_meta meta;       /* form is the request without t */
};

static struct runqueue rq;
static struct avl_tree jobs;
static bool initialized;

static void job_done(struct up_req *r, void *priv)
{
	struct job *j = priv;
	_autoblob_ struct blob_buf b = { 0 };

	j->up = NULL;
	if (r->result == UP_OK && msg_parse(&b, r->body.data) && msg_success(msg_root(&b))) {
		db_cache_put(&j->key, r->body.data, r->body.len, &j->meta);
		db_learn(j->key.action, msg_root(&b));
		ULOG_DBG("refreshed %s for game %" PRIu32 "\n", j->key.action, j->meta.game);
	} else {
		ULOG_DBG("refresh of %s for game %" PRIu32 " failed (HTTP %d)\n", j->key.action, j->meta.game, r->status);
	}
	up_free(r);
	runqueue_task_complete(&j->task);
}

static void job_run(struct runqueue *q, struct runqueue_task *t)
{
	struct job *j = container_of(t, struct job, task);
	_autofree_ char *token = up_background_paused() ? NULL : creds_token(j->key.user);
	_autofree_ char *body = token ? form_set(j->meta.form, "t", token) : NULL;

	j->up = body ? up_send("POST", "/dorequest.php", body, ACHD_UA_TOKEN, NULL, UP_BACKGROUND, job_done, j) : NULL;
	if (!j->up)
		runqueue_task_complete(t);
}

static void job_kill(struct runqueue *q, struct runqueue_task *t)
{
	struct job *j = container_of(t, struct job, task);

	up_free(TAKE_PTR(j->up));
}

static void job_cancel(struct runqueue *q, struct runqueue_task *t, int type)
{
	job_kill(q, t);
	runqueue_task_complete(t);
}

static void job_free(struct job *j)
{
	avl_delete(&jobs, &j->node);
	uloop_timeout_cancel(&j->delay);
	up_free(j->up);
	free(j);
}

static void job_complete(struct runqueue *q, struct runqueue_task *t)
{
	job_free(container_of(t, struct job, task));
}

static const struct runqueue_task_type job_type = {
	.name = "refresh",
	.run = job_run,
	.cancel = job_cancel,
	.kill = job_kill,
};

static void delay_cb(struct uloop_timeout *t)
{
	struct job *j = container_of(t, struct job, delay);

	runqueue_task_add(&rq, &j->task, false);
}

/* Orders jobs by cache key, matching scope and user case-insensitively like the cache does. */
static int job_cmp(const void *a, const void *b, void *ptr)
{
	const struct cache_key *x = a, *y = b;
	int r = strcmp(x->action, y->action);

	if (!r)
		r = strcasecmp(x->scope, y->scope);
	if (!r)
		r = strcasecmp(x->user, y->user);
	return r ? r : x->hardcore - y->hardcore;
}

static void enqueue(const struct cache_key *key, const struct cache_meta *meta, int delay_ms)
{
	struct job *j;
	char *a, *sc, *u, *f;

	if (avl_find(&jobs, key))
		return;
	j = calloc_a(sizeof(*j), &a, strlen(key->action) + 1, &sc, strlen(key->scope) + 1, &u, strlen(key->user) + 1,
		     &f, strlen(meta->form) + 1);
	if (!j)
		return;
	j->key = (struct cache_key){
		.action = strcpy(a, key->action),
		.scope = strcpy(sc, key->scope),
		.user = strcpy(u, key->user),
		.hardcore = key->hardcore,
	};
	j->meta = (struct cache_meta){ .form = strcpy(f, meta->form), .game = meta->game };
	j->task.type = &job_type;
	j->task.complete = job_complete;
	j->node.key = &j->key;
	avl_insert(&jobs, &j->node);

	j->delay.cb = delay_cb;
	uloop_timeout_set(&j->delay, delay_ms);
}

void refresh_unlocks(uint32_t game, const char *user, int delay_ms)
{
	_autofree_ char *u = NULL;
	char g[16];

	if (!game || !user || !*user)
		return;
	snprintf(g, sizeof(g), "%" PRIu32, game);
	u = url_encode(user);
	for (int h = 0; h <= 1; h++) {
		_autofree_ char *form = xasprintf("r=unlocks&u=%s&g=%s&h=%d", u, g, h);
		const struct cache_key key = { .action = "unlocks", .scope = g, .user = user, .hardcore = h };
		const struct cache_meta meta = { .form = form, .game = game };

		if (!form)
			return;
		enqueue(&key, &meta, delay_ms);
	}
}

struct scan_ctx {
	struct idset games;
	char *user;
};

static void scan_ctx_free(struct scan_ctx *s)
{
	idset_free(&s->games);
	free(s->user);
}

static void scan_entry(const struct cache_key *key, const struct cache_meta *meta, void *priv)
{
	struct scan_ctx *s = priv;

	enqueue(key, meta, 0);
	idset_add(&s->games, meta->game);
	if (!s->user)
		s->user = xstrdup(key->user);
}

void refresh_scan(void)
{
	_cleanup_(scan_ctx_free) struct scan_ctx s = { 0 };
	int64_t now = time(NULL);

	db_cache_stale(now - RECENT_PLAY_SECONDS, now - STALE_AFTER_SECONDS, scan_entry, &s);
	for (size_t i = 0; i < s.games.n; i++)
		refresh_unlocks(s.games.v[i], s.user, 0);
	ULOG_INFO("refreshing cached data for %zu recently played game(s)%s\n", s.games.n,
		  s.games.n && !net_online() ? " once upstream is reachable" : "");
}

void refresh_set_online(bool online)
{
	if (online)
		runqueue_resume(&rq);
	else
		runqueue_stop(&rq);
}

void refresh_init(void)
{
	initialized = true;
	runqueue_init(&rq);
	rq.max_running_tasks = 1;
	runqueue_stop(&rq);  /* until the first request reaches upstream */
	avl_init(&jobs, job_cmp, false, NULL);
}

void refresh_shutdown(void)
{
	struct job *j, *tmp;

	if (!initialized)
		return;
	initialized = false;
	runqueue_kill(&rq);
	/* Jobs still waiting out their delay never reached the runqueue. */
	avl_remove_all_elements(&jobs, j, node, tmp) {
		uloop_timeout_cancel(&j->delay);
		free(j);
	}
}
