#ifndef ACHD_DB_H
#define ACHD_DB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "msg.h"
#include "util.h"

/*
 * All persistent state, in <state_dir>/achivmentsd.db:
 *   cache         RA responses by cache key, with what is needed to re-request them
 *   achievements  achievement ID -> game ID, learned from patch/achievementsets
 *   played        game ID -> last startsession
 *   awards        queued award/leaderboard submissions; failed ones stay for inspection
 */
int db_open(const char *path);
void db_close(void);

/* Identifies a cached response. scope and user are matched case-insensitively. */
struct cache_key {
	const char *action;
	const char *scope;   /* game ID, or ROM hash for hash lookups; "" if the action has neither */
	const char *user;    /* "" for user-independent actions */
	bool hardcore;
};

/* login and login2 share one entry per user. */
static inline struct cache_key cache_key_login(const char *user)
{
	return (struct cache_key){ .action = "login2", .scope = "", .user = user };
}

/* form is the request without its token, or NULL if the entry is not refreshable. */
struct cache_meta {
	const char *form;
	uint32_t game;
};

int db_cache_put(const struct cache_key *key, const char *body, size_t len, const struct cache_meta *meta);
/* malloc'd body, or NULL. */
char *db_cache_get(const struct cache_key *key, size_t *len);

typedef void (*db_stale_cb)(const struct cache_key *key, const struct cache_meta *meta, void *priv);
/* Refreshable patch/achievementsets entries fetched before stale_before for games played since played_since. */
void db_cache_stale(int64_t played_since, int64_t stale_before, db_stale_cb cb, void *priv);

void db_learn(const char *action, struct msg_table payload);
uint32_t db_game_for(uint32_t achievement_id);
void db_note_played(uint32_t game);

struct db_award {
	int64_t seq;
	const char *action;
	const char *body;
	const char *ua;
	const char *user;      /* stored lowercased */
	uint32_t id;           /* achievement or leaderboard ID */
	bool hardcore;
	int64_t queued_at;
	uint32_t retries;
};

int db_award_add(const struct db_award *a);
bool db_award_pending(const char *action, uint32_t id, bool hardcore, const char *user);
/* Oldest pending award (one calloc_a allocation), or NULL. */
struct db_award *db_award_next(void);
void db_award_delete(int64_t seq);
void db_award_fail(int64_t seq, const char *error);
void db_award_retry(int64_t seq, const char *error);
unsigned db_award_count(void);
/* Pending achievement IDs for game/user; hardcore_only limits to h=1 awards. */
void db_award_pending_ids(uint32_t game, const char *user, bool hardcore_only, struct idset *out);

#endif
