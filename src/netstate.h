#ifndef ACHD_NETSTATE_H
#define ACHD_NETSTATE_H

#include <stdbool.h>

/*
 * Upstream reachability, as seen by the last upstream request. Nothing probes:
 * the state starts offline and only real traffic moves it. Background work
 * (replay, refresh) runs only while online.
 */
/* on_change runs on every transition between reachable and unreachable. */
void netstate_init(void (*on_change)(bool online));
bool net_online(void);
void net_set_online(bool online, const char *why);

#endif
