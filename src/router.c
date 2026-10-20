#include <stdio.h>
#include <string.h>
#include <time.h>

#include "awards.h"
#include "config.h"
#include "creds.h"
#include "db.h"
#include "http_server.h"
#include "msg.h"
#include "netstate.h"
#include "params.h"
#include "refresh.h"
#include "router.h"
#include "upstream.h"

#define AWARD_REFRESH_DELAY_MS 3000

enum route {
	ROUTE_PASS = 1,  /* not a dorequest.php call: relay untouched */
	ROUTE_API,       /* dorequest.php: relay and cache */
	ROUTE_AWARD,     /* award/leaderboard submit: relay, queue if unreachable */
};

static bool streq(const char *a, const char *b)
{
	return a && b && !strcmp(a, b);
}

static bool is_award(const char *action)
{
	return streq(action, "awardachievement") || streq(action, "submitlbentry");
}

static bool is_login(const char *action)
{
	return streq(action, "login") || streq(action, "login2");
}

static uint32_t parse_u32(const char *s)
{
	unsigned long v = s ? strtoul(s, NULL, 10) : 0;

	return v > UINT32_MAX ? 0 : (uint32_t)v;
}

static char *game_param(const struct request *r)
{
	char *g = req_param(r, "g");

	return g ? g : req_param(r, "i");
}

/* Query parameters followed by the body, as one form. */
static char *request_form(const struct request *r)
{
	_autobuf_ struct buf b = { 0 };

	if (*r->query)
		buf_puts(&b, r->query);
	if (r->body && *r->body) {
		if (b.len)
			buf_append(&b, "&", 1);
		buf_puts(&b, r->body);
	}
	return buf_steal(&b);
}

/* A request's cache key; owns the parameter strings it points at. */
struct req_key {
	struct cache_key key;
	char *game;
	char *hash;
	char *user;
	char *h;
};

static void req_key_free(struct req_key *k)
{
	free(k->game);
	free(k->hash);
	free(k->user);
	free(k->h);
}
#define _autoreqkey_ _cleanup_(req_key_free)

static void req_key_init(struct req_key *k, const struct request *r, const char *action)
{
	k->game = game_param(r);
	k->hash = req_param(r, "m");
	k->user = req_param(r, "u");
	k->h = req_param(r, "h");
	k->key = (struct cache_key){ .action = action, .scope = k->game, .user = k->user, .hardcore = streq(k->h, "1") };

	if (is_login(action))
		k->key = cache_key_login(k->user);
	else if (streq(action, "gameid"))
		k->key = (struct cache_key){ .action = action, .scope = k->hash };
	else if (streq(action, "achievementsets") && k->hash && *k->hash)
		k->key.scope = k->hash;
	else if (streq(action, "startsession"))
		k->key.hardcore = false;
}

static struct cache_key session_key(const char *game, const char *user)
{
	return (struct cache_key){ .action = "startsession", .scope = game, .user = user };
}

static struct cache_key unlocks_key(const char *game, const char *user, bool hardcore)
{
	return (struct cache_key){ .action = "unlocks", .scope = game, .user = user, .hardcore = hardcore };
}

static void respond_blob(struct client *c, int status, struct blob_buf *b)
{
	_autofree_ char *json = msg_to_json(b);

	client_respond_json(c, status, json ? json : "{}");
}

static void respond_error(struct client *c, int status, const char *msg)
{
	_autoblob_ struct blob_buf b = { 0 };

	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "Success", false);
	blobmsg_add_string(&b, "Error", msg);
	respond_blob(c, status, &b);
}

/* Loads a cached RA response into b; false if absent or unparsable. */
static bool load_cached(struct cache_key key, struct blob_buf *b)
{
	_autofree_ char *body = db_cache_get(&key, NULL);

	return msg_parse(b, body);
}

static void collect_cached_unlocks(const char *game, const char *user, int hardcore, struct idset *ids)
{
	_autoblob_ struct blob_buf b = { 0 };

	if (load_cached(unlocks_key(game, user, hardcore), &b) && msg_success(msg_root(&b)))
		msg_collect_ids(msg_attr(msg_root(&b), "UserUnlocks", BLOBMSG_TYPE_ARRAY), ids);
}

static void add_id_array(struct blob_buf *b, const char *name, const struct idset *ids)
{
	void *arr = blobmsg_open_array(b, name);

	for (size_t i = 0; i < ids->n; i++)
		blobmsg_add_u32(b, NULL, ids->v[i]);
	blobmsg_close_array(b, arr);
}

static void store_unlocks(const char *game, const char *user, int hardcore, const struct idset *ids)
{
	_autoblob_ struct blob_buf b = { 0 };
	const struct cache_key key = unlocks_key(game, user, hardcore);
	const struct cache_meta meta = { .game = parse_u32(game) };
	_autofree_ char *json = NULL;

	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "Success", true);
	add_id_array(&b, "UserUnlocks", ids);
	json = msg_to_json(&b);
	if (json)
		db_cache_put(&key, json, strlen(json), &meta);
}

/* Writes field as the cached entries (keeping their When) plus every other known unlock. */
static void add_unlock_list(struct blob_buf *out, const char *field, struct blob_attr *cached,
			    const struct idset *ids, int64_t now)
{
	_autoidset_ struct idset have = { 0 };
	void *arr = blobmsg_open_array(out, field);
	struct blob_attr *cur;
	size_t rem;

	if (cached) {
		blobmsg_for_each_attr(cur, cached, rem) {
			int64_t id = blobmsg_type(cur) == BLOBMSG_TYPE_TABLE ? msg_int(msg_sub(cur), "ID", 0) : 0;

			if (id <= 0 || idset_has(&have, (uint32_t)id))
				continue;
			idset_add(&have, (uint32_t)id);
			blobmsg_add_blob(out, cur);
		}
	}
	for (size_t i = 0; i < ids->n; i++) {
		void *entry;

		if (idset_has(&have, ids->v[i]))
			continue;
		entry = blobmsg_open_table(out, NULL);
		blobmsg_add_u32(out, "ID", ids->v[i]);
		blobmsg_add_u64(out, "When", now);
		blobmsg_close_table(out, entry);
	}
	blobmsg_close_array(out, arr);
}

static void offline_startsession(struct client *c)
{
	_autofree_ char *game = req_param(&c->req, "g"), *user = req_param(&c->req, "u");
	_autoblob_ struct blob_buf cached = { 0 }, out = { 0 };
	_autoidset_ struct idset soft = { 0 }, hard = { 0 };
	struct msg_table root;
	struct blob_attr *cur;
	int64_t now = time(NULL);
	bool have;
	size_t rem;

	if (!game || !user) {
		respond_error(c, 400, "bad request");
		return;
	}
	have = load_cached(session_key(game, user), &cached);
	root = have ? msg_root(&cached) : (struct msg_table){ 0 };

	collect_cached_unlocks(game, user, 0, &soft);
	awards_pending_ids(parse_u32(game), user, false, &soft);
	collect_cached_unlocks(game, user, 1, &hard);
	awards_pending_ids(parse_u32(game), user, true, &hard);

	blob_buf_init(&out, 0);
	if (have) {
		blob_for_each_attr(cur, cached.head, rem) {
			const char *name = blobmsg_name(cur);

			if (strcmp(name, "Success") && strcmp(name, "Unlocks") && strcmp(name, "HardcoreUnlocks") &&
			    strcmp(name, "ServerNow"))
				blobmsg_add_blob(&out, cur);
		}
	}
	blobmsg_add_u8(&out, "Success", true);
	add_unlock_list(&out, "Unlocks", msg_attr(root, "Unlocks", BLOBMSG_TYPE_ARRAY), &soft, now);
	add_unlock_list(&out, "HardcoreUnlocks", msg_attr(root, "HardcoreUnlocks", BLOBMSG_TYPE_ARRAY), &hard, now);
	blobmsg_add_u64(&out, "ServerNow", now);
	respond_blob(c, 200, &out);
}

static void offline_unlocks(struct client *c)
{
	_autofree_ char *game = req_param(&c->req, "g"), *user = req_param(&c->req, "u");
	_autofree_ char *h = req_param(&c->req, "h");
	_autoblob_ struct blob_buf session = { 0 }, out = { 0 };
	_autoidset_ struct idset ids = { 0 };
	int hardcore = streq(h, "1");

	if (!game || !user) {
		respond_error(c, 400, "bad request");
		return;
	}
	collect_cached_unlocks(game, user, hardcore, &ids);
	if (load_cached(session_key(game, user), &session))
		msg_collect_ids(msg_attr(msg_root(&session), hardcore ? "HardcoreUnlocks" : "Unlocks",
					 BLOBMSG_TYPE_ARRAY), &ids);
	awards_pending_ids(parse_u32(game), user, hardcore, &ids);

	blob_buf_init(&out, 0);
	blobmsg_add_u8(&out, "Success", true);
	add_id_array(&out, "UserUnlocks", &ids);
	blobmsg_add_u32(&out, "GameID", parse_u32(game));
	blobmsg_add_u8(&out, "HardcoreMode", hardcore);
	respond_blob(c, 200, &out);
}

static void offline_respond(struct client *c, const char *action)
{
	_autoreqkey_ struct req_key k = { 0 };
	_autofree_ char *body = NULL;
	size_t len;

	if (streq(action, "ping") || streq(action, "postactivity")) {
		client_respond_json(c, 200, "{\"Success\":true}");
		return;
	}
	if (streq(action, "startsession")) {
		offline_startsession(c);
		return;
	}
	if (streq(action, "unlocks")) {
		offline_unlocks(c);
		return;
	}
	if (!action) {
		respond_error(c, 503, "offline");
		return;
	}

	req_key_init(&k, &c->req, action);
	body = db_cache_get(&k.key, &len);
	if (body) {
		ULOG_INFO("r=%s served from cache\n", action);
		client_respond(c, 200, "application/json", body, len);
	} else if (streq(action, "gameid")) {
		ULOG_INFO("r=gameid cache miss (hash %s)\n", k.key.scope ? k.key.scope : "-");
		client_respond_json(c, 200,
				    "{\"Success\":false,\"Error\":\"Game not cached. Launch this game while online first.\","
				    "\"GameID\":0}");
	} else {
		ULOG_INFO("r=%s cache miss (scope %s)\n", action, k.key.scope ? k.key.scope : "-");
		respond_error(c, 503, "no cached response");
	}
}

static void respond_queued(struct client *c, const char *action)
{
	_autofree_ char *user = req_param(&c->req, "u");
	_autoblob_ struct blob_buf login = { 0 }, out = { 0 };
	struct msg_table lr;

	if (awards_queue(&c->req, action)) {
		respond_error(c, 500, "award_queue_failed");
		return;
	}
	lr = load_cached(cache_key_login(user), &login) ? msg_root(&login) : (struct msg_table){ 0 };

	blob_buf_init(&out, 0);
	blobmsg_add_u8(&out, "Success", true);
	if (streq(action, "awardachievement")) {
		_autofree_ char *a = req_param(&c->req, "a");

		blobmsg_add_u64(&out, "Score", msg_int(lr, "Score", 0));
		blobmsg_add_u64(&out, "SoftcoreScore", msg_int(lr, "SoftcoreScore", 0));
		blobmsg_add_u32(&out, "AchievementID", parse_u32(a));
	} else {
		/* rcheevos requires every one of these fields, NumEntries as a string. */
		_autofree_ char *s = req_param(&c->req, "s");
		int64_t score = s ? strtoll(s, NULL, 10) : 0;
		void *resp = blobmsg_open_table(&out, "Response"), *rank, *top;

		blobmsg_add_u64(&out, "Score", score);
		blobmsg_add_u64(&out, "BestScore", score);
		rank = blobmsg_open_table(&out, "RankInfo");
		blobmsg_add_u32(&out, "Rank", 1);
		blobmsg_add_string(&out, "NumEntries", "1");
		blobmsg_close_table(&out, rank);
		top = blobmsg_open_array(&out, "TopEntries");
		blobmsg_close_array(&out, top);
		blobmsg_close_table(&out, resp);
	}
	respond_blob(c, 200, &out);
}

static uint32_t response_game_id(const char *action, struct msg_table root, const char *game)
{
	struct blob_attr *pd;

	if (streq(action, "patch") && (pd = msg_attr(root, "PatchData", BLOBMSG_TYPE_TABLE)))
		return (uint32_t)msg_int(msg_sub(pd), "ID", 0);
	if (streq(action, "achievementsets"))
		return (uint32_t)msg_int(root, "GameId", 0);
	if (streq(action, "gameid"))
		return (uint32_t)msg_int(root, "GameID", 0);
	return parse_u32(game);
}

static void cache_startsession(const struct request *r, struct msg_table root, const char *body, size_t len)
{
	_autofree_ char *user = req_param(r, "u"), *game = req_param(r, "g");
	struct cache_key key = session_key(game, user);
	struct cache_meta meta = { .game = parse_u32(game) };

	if (!game || !user)
		return;
	db_cache_put(&key, body, len, &meta);
	for (int h = 0; h <= 1; h++) {
		struct blob_attr *arr = msg_attr(root, h ? "HardcoreUnlocks" : "Unlocks", BLOBMSG_TYPE_ARRAY);
		_autoidset_ struct idset ids = { 0 };

		if (!arr)
			continue;
		msg_collect_ids(arr, &ids);
		store_unlocks(game, user, h, &ids);
	}
}

static void cache_response(const struct request *r, const char *action, const char *body, size_t len)
{
	_autoblob_ struct blob_buf b = { 0 };
	_autoreqkey_ struct req_key k = { 0 };
	_autofree_ char *form = NULL, *tokenless = NULL;
	struct cache_meta meta = { 0 };
	struct cache_key login;
	struct msg_table root;

	if (!action || !msg_parse(&b, body) || !msg_success(msg_root(&b)))
		return;
	root = msg_root(&b);
	req_key_init(&k, r, action);

	if (is_login(action)) {
		login = cache_key_login(msg_str(root, "User") ? msg_str(root, "User") : k.user);
		db_cache_put(&login, body, len, NULL);
		creds_note(login.user, msg_str(root, "Token"));
	} else if (streq(action, "startsession")) {
		cache_startsession(r, root, body, len);
	} else if (!streq(action, "ping") && !streq(action, "postactivity") && !is_award(action)) {
		/* Stored without the token so refresh can resend it with whichever is current. */
		form = request_form(r);
		tokenless = form_set(form, "t", "");
		meta.form = tokenless;
		meta.game = response_game_id(action, root, k.game);
		db_cache_put(&k.key, body, len, &meta);
		if (streq(action, "patch") || streq(action, "achievementsets"))
			db_learn(action, root);
	}
}

static void award_relayed(const struct request *r, struct up_req *up)
{
	_autoblob_ struct blob_buf b = { 0 };
	_autofree_ char *a = req_param(r, "a"), *user = req_param(r, "u");
	uint32_t game = db_game_for(parse_u32(a));

	if (game && msg_parse(&b, up->body.data) && msg_success(msg_root(&b)))
		refresh_unlocks(game, user, AWARD_REFRESH_DELAY_MS);
}

static void forward_done(struct up_req *up, void *priv)
{
	struct client *c = priv;
	_autofree_ char *action = req_param(&c->req, "r");

	c->up = NULL;
	if (up->result == UP_NET_ERROR) {
		ULOG_INFO("r=%s upstream failed (%s); answering offline\n", action ? action : "-",
			  up->error ? up->error : "?");
		if (c->route_kind == ROUTE_AWARD)
			respond_queued(c, action);
		else if (c->route_kind == ROUTE_API)
			offline_respond(c, action);
		else
			respond_error(c, 503, "upstream unavailable");
	} else {
		if (up->result == UP_OK && c->route_kind == ROUTE_API)
			cache_response(&c->req, action, up->body.data, up->body.len);
		else if (up->result == UP_OK && c->route_kind == ROUTE_AWARD)
			award_relayed(&c->req, up);
		ULOG_DBG("r=%s upstream HTTP %d\n", action ? action : "-", up->status);
		client_respond(c, up->status, up->content_type ? up->content_type : "application/json", up->body.data,
			       up->body.len);
	}
	up_free(up);
}

static void forward(struct client *c, enum route kind)
{
	struct request *r = &c->req;

	c->route_kind = kind;
	c->up = up_send(r->method, r->path, r->body, r->user_agent, r->content_type, 0, forward_done, c);
	if (!c->up)
		respond_error(c, 500, "out of memory");
}

void router_handle(struct client *c)
{
	struct request *r = &c->req;
	_autofree_ char *action = NULL, *user = NULL, *token = NULL;

	if (strcmp(r->method, "GET") && strcmp(r->method, "POST")) {
		respond_error(c, 405, "method not allowed");
		return;
	}
	if (strncmp(r->path, "/dorequest.php", 14)) {
		forward(c, ROUTE_PASS);
		return;
	}

	action = req_param(r, "r");
	user = req_param(r, "u");
	token = req_param(r, "t");
	creds_note(user, token);
	if (streq(action, "startsession")) {
		_autofree_ char *game = req_param(r, "g");

		db_note_played(parse_u32(game));
	}
	ULOG_DBG("r=%s %s\n", action ? action : "-", net_online() ? "online" : "offline");
	/* Always tried: a failure here is the only way offline is detected or left. */
	forward(c, is_award(action) ? ROUTE_AWARD : ROUTE_API);
}
