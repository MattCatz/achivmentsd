#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <libubox/md5.h>
#include <libubox/uloop.h>

#include "awards.h"
#include "config.h"
#include "creds.h"
#include "db.h"
#include "http_server.h"
#include "msg.h"
#include "netstate.h"
#include "params.h"
#include "refresh.h"
#include "upstream.h"

#define RETRY_INTERVAL_MS (5 * 60 * 1000)
#define REFRESH_DELAY_MS 3000
#define REPLAY_PATH "/dorequest.php"

enum outcome {
	OUT_DONE,     /* accepted, or already unlocked: delete */
	OUT_FAILED,   /* refused for good: keep, marked failed */
	OUT_RETRY,    /* transient: stop the round, try again later */
	OUT_AUTH,     /* token refused: stop until the emulator logs in again */
};

static struct uloop_interval retry_timer;
static struct up_req *inflight;
static struct db_award *inflight_award;
static char *inflight_token;
static char *blocked_token;
static struct idset round_games;
static char *round_user;
static bool initialized;

static bool is_award_action(const char *action)
{
	return !strcmp(action, "awardachievement");
}

static char *md5_hex(const char *s)
{
	static const char hex[] = "0123456789abcdef";
	char *out = malloc(33);
	uint8_t d[16];
	md5_ctx_t ctx;

	if (!out)
		return NULL;
	md5_begin(&ctx);
	md5_hash(s, strlen(s), &ctx);
	md5_end(d, &ctx);
	for (int i = 0; i < 16; i++) {
		out[i * 2] = hex[d[i] >> 4];
		out[i * 2 + 1] = hex[d[i] & 15];
	}
	out[32] = 0;
	return out;
}

static void replace(char **form, const char *name, const char *value)
{
	char *n = form_set(*form, name, value);

	free(*form);
	*form = n;
}

/* The string RA hashes into v: id, user, then h (awards) or score (leaderboards), then o. */
static char *validation_string(const char *form, const char *action, const char *offset)
{
	_autofree_ char *user = form_get(form, "u");
	_autofree_ char *id = form_get(form, is_award_action(action) ? "a" : "i");
	_autofree_ char *extra = form_get(form, is_award_action(action) ? "h" : "s");
	unsigned long nid = strtoul(id ? id : "0", NULL, 10);

	if (is_award_action(action))
		return xasprintf("%lu%s%d%lu%s", nid, user ? user : "", extra && !strcmp(extra, "1"), nid, offset);
	return xasprintf("%lu%s%lld%s", nid, user ? user : "", strtoll(extra ? extra : "0", NULL, 10), offset);
}

char *award_replay_body(const char *form, const char *action, int64_t queued_at, int64_t now, const char *token)
{
	_autofree_ char *o_str = form_get(form, "o"), *sig = NULL, *v = NULL;
	char *body = xstrdup(form);
	int64_t offset;
	char num[32];

	if (token && *token)
		replace(&body, "t", token);
	if (queued_at < ACHD_BUILD_EPOCH || now <= queued_at)
		return body;

	offset = (o_str ? strtoll(o_str, NULL, 10) : 0) + (now - queued_at);
	if (offset < 0)
		offset = 0;
	if (offset > MAX_OFFSET_SECONDS)
		offset = MAX_OFFSET_SECONDS;
	snprintf(num, sizeof(num), "%" PRId64, offset);
	replace(&body, "o", num);

	sig = validation_string(form, action, num);
	if (sig && (v = md5_hex(sig)))
		replace(&body, "v", v);
	return body;
}

static void retry_cb(struct uloop_interval *t)
{
	awards_kick();
}

void awards_init(void)
{
	unsigned n = db_award_count();

	initialized = true;
	retry_timer.cb = retry_cb;
	uloop_interval_set(&retry_timer, RETRY_INTERVAL_MS);
	if (n)
		ULOG_INFO("%u queued award(s) waiting for upstream\n", n);
}

int awards_queue(const struct request *r, const char *action)
{
	_autofree_ char *id = req_param(r, is_award_action(action) ? "a" : "i");
	_autofree_ char *user = req_param(r, "u"), *h = req_param(r, "h");
	_autobuf_ struct buf form = { 0 };
	int64_t now = time(NULL);
	struct db_award a = {
		.action = action,
		.ua = r->user_agent ? r->user_agent : "",
		.user = user ? user : "",
		.id = id ? (uint32_t)strtoul(id, NULL, 10) : 0,
		.hardcore = h && !strcmp(h, "1"),
		.queued_at = now >= ACHD_BUILD_EPOCH ? now : 0,
	};

	/* The server ignores the warning ID; replaying it would only spend a request. */
	if (is_award_action(action) &&
	    (a.id == RA_CLIENT_WARNING_ID || db_award_pending(action, a.id, a.hardcore, a.user)))
		return 0;

	/* Replays always POST to /dorequest.php, so fold any query parameters into the body. */
	if (*r->query)
		buf_puts(&form, r->query);
	if (r->body && *r->body) {
		if (form.len)
			buf_append(&form, "&", 1);
		buf_puts(&form, r->body);
	}
	a.body = form.data ? form.data : "";
	if (db_award_add(&a))
		return -1;

	ULOG_INFO("queued %s id=%" PRIu32 " hardcore=%d (%u pending)\n", action, a.id, a.hardcore, db_award_count());
	if (!a.queued_at)
		ULOG_WARN("system clock is before the build date; this award will replay without a backdate\n");
	return 0;
}

void awards_pending_ids(uint32_t game, const char *user, bool hardcore_only, struct idset *out)
{
	db_award_pending_ids(game, user, hardcore_only, out);
}

static void end_round(void)
{
	for (size_t i = 0; i < round_games.n; i++)
		refresh_unlocks(round_games.v[i], round_user, REFRESH_DELAY_MS);
	idset_free(&round_games);
	free(TAKE_PTR(round_user));
}

static enum outcome classify(struct up_req *r, struct msg_table body, bool parsed)
{
	const char *err = msg_str(body, "Error");

	if (r->result == UP_NET_ERROR || r->status == 429 || r->status >= 500 || !parsed)
		return OUT_RETRY;
	if (r->status == 401 || (err && strstr(err, "Invalid user/token")))
		return OUT_AUTH;
	if (msg_success(body) || (err && strstr(err, "already has")))
		return OUT_DONE;
	return OUT_FAILED;
}

static void replay_next(void);

static void replay_done(struct up_req *r, void *priv)
{
	_autofree_ struct db_award *a = TAKE_PTR(inflight_award);
	_autofree_ char *token = TAKE_PTR(inflight_token);
	_autoblob_ struct blob_buf b = { 0 };
	bool parsed = msg_parse(&b, r->body.data);
	const char *err = msg_str(msg_root(&b), "Error");
	const char *why = r->error ? r->error : err ? err : "HTTP error";
	enum outcome out = classify(r, msg_root(&b), parsed);
	uint32_t game;

	inflight = NULL;
	switch (out) {
	case OUT_DONE:
		ULOG_INFO("replayed %s id=%" PRIu32 "%s\n", a->action, a->id,
			  msg_success(msg_root(&b)) ? "" : " (already unlocked)");
		game = is_award_action(a->action) ? db_game_for(a->id) : 0;
		if (game) {
			idset_add(&round_games, game);
			if (!round_user)
				round_user = xstrdup(a->user);
		}
		db_award_delete(a->seq);
		break;
	case OUT_FAILED:
		ULOG_WARN("upstream refused %s id=%" PRIu32 " (HTTP %d: %s); kept as failed\n", a->action, a->id,
			  r->status, err ? err : "no error text");
		db_award_fail(a->seq, err ? err : "refused");
		break;
	case OUT_AUTH:
		free(blocked_token);
		blocked_token = TAKE_PTR(token);
		ULOG_WARN("upstream rejected the API token; replay paused until the emulator logs in again\n");
		/* fall through */
	case OUT_RETRY:
		db_award_retry(a->seq, why);
		ULOG_INFO("replay of %s id=%" PRIu32 " deferred (attempt %" PRIu32 "): %s\n", a->action, a->id,
			  a->retries + 1, why);
		break;
	}
	up_free(r);

	if (out == OUT_DONE || out == OUT_FAILED)
		replay_next();
	else
		end_round();
}

static void replay_next(void)
{
	_autofree_ struct db_award *a = NULL;
	_autofree_ char *token = NULL, *body = NULL, *ua = NULL;

	if (!net_online() || up_background_paused() || !(a = db_award_next())) {
		end_round();
		return;
	}
	token = creds_token(a->user);
	if (token && blocked_token && !strcmp(token, blocked_token)) {
		end_round();
		return;
	}
	body = award_replay_body(a->body, a->action, a->queued_at, time(NULL), token);
	if (strstr(a->ua, ACHD_UA_TOKEN))
		ua = xstrdup(a->ua);
	else
		ua = xasprintf("%s%s%s", a->ua, *a->ua ? " " : "", ACHD_UA_TOKEN);

	inflight = up_send("POST", REPLAY_PATH, body, ua, "application/x-www-form-urlencoded", UP_BACKGROUND,
			   replay_done, NULL);
	if (!inflight) {
		end_round();
		return;
	}
	inflight_award = TAKE_PTR(a);
	inflight_token = TAKE_PTR(token);
}

void awards_kick(void)
{
	unsigned n;

	if (inflight || !net_online() || up_background_paused() || !(n = db_award_count()))
		return;
	ULOG_INFO("replaying %u queued award(s)\n", n);
	replay_next();
}

void awards_shutdown(void)
{
	if (!initialized)
		return;
	initialized = false;
	up_free(TAKE_PTR(inflight));
	uloop_interval_cancel(&retry_timer);
	idset_free(&round_games);
	free(TAKE_PTR(round_user));
	free(TAKE_PTR(inflight_award));
	free(TAKE_PTR(inflight_token));
	free(TAKE_PTR(blocked_token));
}
