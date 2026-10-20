#include <stdio.h>
#include <string.h>
#include <time.h>

#include <sqlite3.h>

#include "db.h"

#define SCHEMA_VERSION 1

enum award_status {
	AWARD_PENDING = 0,
	AWARD_FAILED = 1,
};

static const char schema[] =
	"CREATE TABLE IF NOT EXISTS cache ("
	"  action TEXT NOT NULL,"
	"  scope TEXT NOT NULL,"
	"  user TEXT NOT NULL,"
	"  hardcore INTEGER NOT NULL,"
	"  body TEXT NOT NULL,"
	"  game_id INTEGER,"
	"  form TEXT,"
	"  fetched_at INTEGER NOT NULL,"
	"  PRIMARY KEY (action, scope, user, hardcore));"
	"CREATE INDEX IF NOT EXISTS cache_game ON cache(game_id);"
	"CREATE TABLE IF NOT EXISTS achievements ("
	"  id INTEGER PRIMARY KEY,"
	"  game_id INTEGER NOT NULL);"
	"CREATE TABLE IF NOT EXISTS played ("
	"  game_id INTEGER PRIMARY KEY,"
	"  last_at INTEGER NOT NULL);"
	"CREATE TABLE IF NOT EXISTS awards ("
	"  seq INTEGER PRIMARY KEY AUTOINCREMENT,"
	"  action TEXT NOT NULL,"
	"  body TEXT NOT NULL,"
	"  ua TEXT NOT NULL,"
	"  user TEXT NOT NULL,"
	"  achievement_id INTEGER NOT NULL,"
	"  hardcore INTEGER NOT NULL,"
	"  queued_at INTEGER NOT NULL,"
	"  retries INTEGER NOT NULL DEFAULT 0,"
	"  last_error TEXT,"
	"  status INTEGER NOT NULL DEFAULT 0);"
	"CREATE INDEX IF NOT EXISTS awards_status ON awards(status, seq);";

enum {
	Q_BEGIN,
	Q_COMMIT,
	Q_CACHE_PUT,
	Q_CACHE_GET,
	Q_CACHE_STALE,
	Q_ACH_PUT,
	Q_ACH_GAME,
	Q_PLAYED_PUT,
	Q_AWARD_ADD,
	Q_AWARD_PENDING,
	Q_AWARD_NEXT,
	Q_AWARD_DELETE,
	Q_AWARD_FAIL,
	Q_AWARD_RETRY,
	Q_AWARD_COUNT,
	Q_AWARD_PENDING_IDS,
	__Q_MAX
};

static const char *const sql[__Q_MAX] = {
	[Q_BEGIN] = "BEGIN",
	[Q_COMMIT] = "COMMIT",
	[Q_CACHE_PUT] = "INSERT OR REPLACE INTO cache (action, scope, user, hardcore, body, game_id, form, fetched_at) "
			"VALUES (?1, lower(?2), lower(?3), ?4, ?5, ?6, ?7, ?8)",
	[Q_CACHE_GET] = "SELECT body FROM cache WHERE action = ?1 AND scope = lower(?2) AND user = lower(?3) "
			"AND hardcore = ?4",
	[Q_CACHE_STALE] = "SELECT c.action, c.scope, c.user, c.hardcore, c.form, c.game_id FROM cache c "
			  "JOIN played p ON p.game_id = c.game_id "
			  "WHERE c.action IN ('patch', 'achievementsets') AND c.form IS NOT NULL AND c.user <> '' "
			  "AND p.last_at >= ?1 AND c.fetched_at < ?2",
	[Q_ACH_PUT] = "INSERT OR REPLACE INTO achievements (id, game_id) VALUES (?1, ?2)",
	[Q_ACH_GAME] = "SELECT game_id FROM achievements WHERE id = ?1",
	[Q_PLAYED_PUT] = "INSERT OR REPLACE INTO played (game_id, last_at) VALUES (?1, ?2)",
	[Q_AWARD_ADD] = "INSERT INTO awards (action, body, ua, user, achievement_id, hardcore, queued_at) "
			"VALUES (?1, ?2, ?3, lower(?4), ?5, ?6, ?7)",
	[Q_AWARD_PENDING] = "SELECT 1 FROM awards WHERE status = 0 AND action = ?1 AND achievement_id = ?2 "
			    "AND hardcore = ?3 AND user = lower(?4)",
	[Q_AWARD_NEXT] = "SELECT seq, action, body, ua, user, achievement_id, hardcore, queued_at, retries "
			 "FROM awards WHERE status = 0 ORDER BY seq LIMIT 1",
	[Q_AWARD_DELETE] = "DELETE FROM awards WHERE seq = ?1",
	[Q_AWARD_FAIL] = "UPDATE awards SET status = 1, last_error = ?2 WHERE seq = ?1",
	[Q_AWARD_RETRY] = "UPDATE awards SET retries = retries + 1, last_error = ?2 WHERE seq = ?1",
	[Q_AWARD_COUNT] = "SELECT count(*) FROM awards WHERE status = 0",
	[Q_AWARD_PENDING_IDS] = "SELECT a.achievement_id FROM awards a "
				"JOIN achievements x ON x.id = a.achievement_id "
				"WHERE a.status = 0 AND a.action = 'awardachievement' AND x.game_id = ?1 "
				"AND a.user = lower(?2) AND (?3 = 0 OR a.hardcore = 1)",
};

static sqlite3 *db;
static sqlite3_stmt *stmts[__Q_MAX];

/* Statements are prepared once; this guard resets one when it leaves scope. */
static void stmt_resetp(sqlite3_stmt **s)
{
	if (*s) {
		sqlite3_reset(*s);
		sqlite3_clear_bindings(*s);
	}
}
#define _autoreset_ _cleanup_(stmt_resetp)

static sqlite3_stmt *q(int id)
{
	return db ? stmts[id] : NULL;
}

/* SQLITE_ROW or SQLITE_DONE; anything else is logged. */
static int step(sqlite3_stmt *s)
{
	int rc = sqlite3_step(s);

	if (rc != SQLITE_ROW && rc != SQLITE_DONE)
		ULOG_ERR("sqlite: %s\n", sqlite3_errmsg(db));
	return rc;
}

static bool exec_done(sqlite3_stmt *s)
{
	return s && step(s) == SQLITE_DONE;
}

static void bind_text(sqlite3_stmt *s, int i, const char *v)
{
	if (v)
		sqlite3_bind_text(s, i, v, -1, SQLITE_TRANSIENT);
	else
		sqlite3_bind_null(s, i);
}

static const char *col_text(sqlite3_stmt *s, int i)
{
	const char *v = (const char *)sqlite3_column_text(s, i);

	return v ? v : "";
}

/* Commits when the guard leaves scope; batches many small writes into one fsync. */
static void txn_commitp(bool *open)
{
	_autoreset_ sqlite3_stmt *s = *open ? q(Q_COMMIT) : NULL;

	if (s)
		exec_done(s);
}
#define _autotxn_ _cleanup_(txn_commitp)

static bool txn_begin(void)
{
	_autoreset_ sqlite3_stmt *s = q(Q_BEGIN);

	return exec_done(s);
}

static int schema_version(void)
{
	sqlite3_stmt *s = NULL;
	int v = -1;

	if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &s, NULL) == SQLITE_OK && step(s) == SQLITE_ROW)
		v = sqlite3_column_int(s, 0);
	sqlite3_finalize(s);
	return v;
}

int db_open(const char *path)
{
	char pragma[64];
	int version;

	if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
		ULOG_ERR("cannot open %s: %s\n", path, db ? sqlite3_errmsg(db) : "out of memory");
		db_close();
		return -1;
	}
	version = schema_version();
	if (version > SCHEMA_VERSION) {
		ULOG_ERR("%s has schema %d; this build understands up to %d\n", path, version, SCHEMA_VERSION);
		db_close();
		return -1;
	}
	snprintf(pragma, sizeof(pragma), "PRAGMA user_version = %d", SCHEMA_VERSION);
	if (sqlite3_exec(db, schema, NULL, NULL, NULL) != SQLITE_OK ||
	    sqlite3_exec(db, pragma, NULL, NULL, NULL) != SQLITE_OK) {
		ULOG_ERR("cannot create schema in %s: %s\n", path, sqlite3_errmsg(db));
		db_close();
		return -1;
	}
	for (int i = 0; i < __Q_MAX; i++) {
		if (sqlite3_prepare_v3(db, sql[i], -1, SQLITE_PREPARE_PERSISTENT, &stmts[i], NULL) != SQLITE_OK) {
			ULOG_ERR("sqlite: %s: %s\n", sqlite3_errmsg(db), sql[i]);
			db_close();
			return -1;
		}
	}
	return 0;
}

void db_close(void)
{
	for (int i = 0; i < __Q_MAX; i++)
		sqlite3_finalize(TAKE_PTR(stmts[i]));
	sqlite3_close(TAKE_PTR(db));
}

static void bind_key(sqlite3_stmt *s, const struct cache_key *k)
{
	bind_text(s, 1, k->action);
	bind_text(s, 2, k->scope ? k->scope : "");
	bind_text(s, 3, k->user ? k->user : "");
	sqlite3_bind_int(s, 4, k->hardcore);
}

int db_cache_put(const struct cache_key *key, const char *body, size_t len, const struct cache_meta *meta)
{
	_autoreset_ sqlite3_stmt *s = q(Q_CACHE_PUT);
	const struct cache_meta none = { 0 };

	if (!s)
		return -1;
	meta = meta ? meta : &none;
	bind_key(s, key);
	sqlite3_bind_text(s, 5, body, (int)len, SQLITE_TRANSIENT);
	if (meta->game)
		sqlite3_bind_int64(s, 6, meta->game);
	bind_text(s, 7, meta->form);
	sqlite3_bind_int64(s, 8, time(NULL));
	return exec_done(s) ? 0 : -1;
}

char *db_cache_get(const struct cache_key *key, size_t *len)
{
	_autoreset_ sqlite3_stmt *s = q(Q_CACHE_GET);
	size_t n;

	if (!s)
		return NULL;
	bind_key(s, key);
	if (step(s) != SQLITE_ROW)
		return NULL;
	n = sqlite3_column_bytes(s, 0);
	if (len)
		*len = n;
	return xstrndup(col_text(s, 0), n);
}

void db_cache_stale(int64_t played_since, int64_t stale_before, db_stale_cb cb, void *priv)
{
	_autoreset_ sqlite3_stmt *s = q(Q_CACHE_STALE);

	if (!s)
		return;
	sqlite3_bind_int64(s, 1, played_since);
	sqlite3_bind_int64(s, 2, stale_before);
	while (step(s) == SQLITE_ROW) {
		const struct cache_key key = {
			.action = col_text(s, 0),
			.scope = col_text(s, 1),
			.user = col_text(s, 2),
			.hardcore = sqlite3_column_int(s, 3),
		};
		const struct cache_meta meta = {
			.form = col_text(s, 4),
			.game = (uint32_t)sqlite3_column_int64(s, 5),
		};

		cb(&key, &meta, priv);
	}
}

static void put_achievement(uint32_t id, uint32_t game)
{
	_autoreset_ sqlite3_stmt *s = q(Q_ACH_PUT);

	if (!s)
		return;
	sqlite3_bind_int64(s, 1, id);
	sqlite3_bind_int64(s, 2, game);
	exec_done(s);
}

static void learn_achievements(struct blob_attr *array, uint32_t game)
{
	struct blob_attr *cur;
	size_t rem;

	if (!array)
		return;
	blobmsg_for_each_attr(cur, array, rem) {
		int64_t id = blobmsg_type(cur) == BLOBMSG_TYPE_TABLE ? msg_int(msg_sub(cur), "ID", 0) : 0;

		if (id > 0 && id != RA_CLIENT_WARNING_ID && id <= UINT32_MAX)
			put_achievement((uint32_t)id, game);
	}
}

void db_learn(const char *action, struct msg_table payload)
{
	_autotxn_ bool txn = false;
	struct blob_attr *pd, *sets, *cur;
	int64_t game = 0;
	size_t rem;

	if (!strcmp(action, "patch")) {
		pd = msg_attr(payload, "PatchData", BLOBMSG_TYPE_TABLE);
		game = pd ? msg_int(msg_sub(pd), "ID", 0) : 0;
		if (game > 0 && (txn = txn_begin()))
			learn_achievements(msg_attr(msg_sub(pd), "Achievements", BLOBMSG_TYPE_ARRAY), (uint32_t)game);
	} else if (!strcmp(action, "achievementsets")) {
		/* Subset achievements unlock under the session's top-level game. */
		game = msg_int(payload, "GameId", 0);
		sets = msg_attr(payload, "Sets", BLOBMSG_TYPE_ARRAY);
		if (game <= 0 || !sets || !(txn = txn_begin()))
			return;
		blobmsg_for_each_attr(cur, sets, rem)
			if (blobmsg_type(cur) == BLOBMSG_TYPE_TABLE)
				learn_achievements(msg_attr(msg_sub(cur), "Achievements", BLOBMSG_TYPE_ARRAY),
						   (uint32_t)game);
	}
}

uint32_t db_game_for(uint32_t achievement_id)
{
	_autoreset_ sqlite3_stmt *s = q(Q_ACH_GAME);

	if (!s)
		return 0;
	sqlite3_bind_int64(s, 1, achievement_id);
	return step(s) == SQLITE_ROW ? (uint32_t)sqlite3_column_int64(s, 0) : 0;
}

void db_note_played(uint32_t game)
{
	_autoreset_ sqlite3_stmt *s = game ? q(Q_PLAYED_PUT) : NULL;

	if (!s)
		return;
	sqlite3_bind_int64(s, 1, game);
	sqlite3_bind_int64(s, 2, time(NULL));
	exec_done(s);
}

int db_award_add(const struct db_award *a)
{
	_autoreset_ sqlite3_stmt *s = q(Q_AWARD_ADD);

	if (!s)
		return -1;
	bind_text(s, 1, a->action);
	bind_text(s, 2, a->body);
	bind_text(s, 3, a->ua);
	bind_text(s, 4, a->user);
	sqlite3_bind_int64(s, 5, a->id);
	sqlite3_bind_int(s, 6, a->hardcore);
	sqlite3_bind_int64(s, 7, a->queued_at);
	return exec_done(s) ? 0 : -1;
}

bool db_award_pending(const char *action, uint32_t id, bool hardcore, const char *user)
{
	_autoreset_ sqlite3_stmt *s = q(Q_AWARD_PENDING);

	if (!s)
		return false;
	bind_text(s, 1, action);
	sqlite3_bind_int64(s, 2, id);
	sqlite3_bind_int(s, 3, hardcore);
	bind_text(s, 4, user ? user : "");
	return step(s) == SQLITE_ROW;
}

struct db_award *db_award_next(void)
{
	_autoreset_ sqlite3_stmt *s = q(Q_AWARD_NEXT);
	struct db_award *a;
	char *action, *body, *ua, *user;

	if (!s || step(s) != SQLITE_ROW)
		return NULL;
	a = calloc_a(sizeof(*a), &action, strlen(col_text(s, 1)) + 1, &body, strlen(col_text(s, 2)) + 1,
		     &ua, strlen(col_text(s, 3)) + 1, &user, strlen(col_text(s, 4)) + 1);
	if (!a)
		return NULL;
	a->seq = sqlite3_column_int64(s, 0);
	a->action = strcpy(action, col_text(s, 1));
	a->body = strcpy(body, col_text(s, 2));
	a->ua = strcpy(ua, col_text(s, 3));
	a->user = strcpy(user, col_text(s, 4));
	a->id = (uint32_t)sqlite3_column_int64(s, 5);
	a->hardcore = sqlite3_column_int(s, 6);
	a->queued_at = sqlite3_column_int64(s, 7);
	a->retries = (uint32_t)sqlite3_column_int64(s, 8);
	return a;
}

static void award_update(int id, int64_t seq, const char *error)
{
	_autoreset_ sqlite3_stmt *s = q(id);

	if (!s)
		return;
	sqlite3_bind_int64(s, 1, seq);
	if (error)
		bind_text(s, 2, error);
	exec_done(s);
}

void db_award_delete(int64_t seq)
{
	award_update(Q_AWARD_DELETE, seq, NULL);
}

void db_award_fail(int64_t seq, const char *error)
{
	award_update(Q_AWARD_FAIL, seq, error);
}

void db_award_retry(int64_t seq, const char *error)
{
	award_update(Q_AWARD_RETRY, seq, error);
}

unsigned db_award_count(void)
{
	_autoreset_ sqlite3_stmt *s = q(Q_AWARD_COUNT);

	return s && step(s) == SQLITE_ROW ? (unsigned)sqlite3_column_int64(s, 0) : 0;
}

void db_award_pending_ids(uint32_t game, const char *user, bool hardcore_only, struct idset *out)
{
	_autoreset_ sqlite3_stmt *s = q(Q_AWARD_PENDING_IDS);

	if (!s)
		return;
	sqlite3_bind_int64(s, 1, game);
	bind_text(s, 2, user ? user : "");
	sqlite3_bind_int(s, 3, hardcore_only);
	while (step(s) == SQLITE_ROW)
		idset_add(out, (uint32_t)sqlite3_column_int64(s, 0));
}
