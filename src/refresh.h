#ifndef ACHD_REFRESH_H
#define ACHD_REFRESH_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Background cache upkeep on a libubox runqueue (one request at a time,
 * stopped while upstream is unreachable):
 *   - unlocks for a game shortly after an award lands, so the offline
 *     startsession matches the server
 *   - on refresh_scan() (SIGHUP), definitions (patch/achievementsets) older
 *     than a day for games started in the last 7 days
 */
void refresh_init(void);
void refresh_set_online(bool online);
void refresh_scan(void);
void refresh_unlocks(uint32_t game, const char *user, int delay_ms);
void refresh_shutdown(void);

#endif
