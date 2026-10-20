#include "netstate.h"
#include "util.h"

static bool online;
static void (*change_cb)(bool online);

bool net_online(void)
{
	return online;
}

void net_set_online(bool up, const char *why)
{
	if (up == online)
		return;
	online = up;
	if (up)
		ULOG_INFO("upstream reachable\n");
	else
		ULOG_INFO("upstream unreachable: %s\n", why ? why : "unknown");
	if (change_cb)
		change_cb(up);
}

void netstate_init(void (*on_change)(bool online))
{
	change_cb = on_change;
}
