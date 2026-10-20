#ifndef ACHD_AWARDS_H
#define ACHD_AWARDS_H

#include <stdbool.h>
#include <stdint.h>

#include "util.h"

struct request;

/*
 * Offline award queue, kept in the awards table and replayed in order once
 * upstream is reachable. Awards the server rejects for good (unknown
 * achievement, event achievement without hardcore, ...) stay in the table
 * marked failed, with the server's error.
 */
#define MAX_OFFSET_SECONDS (14 * 24 * 60 * 60)

void awards_init(void);
/* action is "awardachievement" or "submitlbentry". */
int awards_queue(const struct request *r, const char *action);
/* Adds pending achievement IDs for game/user; hardcore_only limits to h=1 awards. */
void awards_pending_ids(uint32_t game, const char *user, bool hardcore_only, struct idset *out);
void awards_kick(void);
void awards_shutdown(void);

/*
 * Request body for replaying a queued award: current token, and o/v
 * recomputed so the unlock is backdated to when it happened. queued_at of 0
 * means the clock was not trusted at queue time; only the emulator's own o is
 * kept then.
 */
char *award_replay_body(const char *form, const char *action, int64_t queued_at, int64_t now, const char *token);

#endif
