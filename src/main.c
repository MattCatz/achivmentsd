#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <libubox/uloop.h>

#include "awards.h"
#include "config.h"
#include "creds.h"
#include "db.h"
#include "http_server.h"
#include "netstate.h"
#include "refresh.h"
#include "upstream.h"
#include "util.h"

#define DEFAULT_CONFIG "/etc/achivmentsd.json"

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  -c FILE   JSON config (default " DEFAULT_CONFIG ", optional)\n"
		"  -s DIR    state directory (overrides state_dir)\n"
		"  -l HOST   listen address (overrides proxy_host)\n"
		"  -p PORT   listen port (overrides proxy_port)\n"
		"  -u URL    upstream base URL (overrides upstream_host)\n"
		"  -S        log to syslog instead of stderr\n"
		"  -v        debug logging\n"
		"SIGUSR1 assumes upstream is reachable and replays queued awards.\n"
		"SIGHUP refreshes cached data for recently played games.\n",
		prog);
}

static void on_net_change(bool online)
{
	refresh_set_online(online);
	if (online)
		awards_kick();
}

static void on_sigusr1(struct uloop_signal *s)
{
	ULOG_INFO("SIGUSR1: assuming upstream is reachable\n");
	/* A failed replay flips it back. */
	net_set_online(true, NULL);
	awards_kick();
}

static void on_sighup(struct uloop_signal *s)
{
	refresh_scan();
}

static struct uloop_signal sigusr1 = { .signo = SIGUSR1, .cb = on_sigusr1 };
static struct uloop_signal sighup = { .signo = SIGHUP, .cb = on_sighup };

static int run(void)
{
	_autofree_ char *db_path = xasprintf("%s/achivmentsd.db", cfg.state_dir);

	if (!db_path || make_dirs(cfg.state_dir) || db_open(db_path) || up_init() ||
	    http_server_init(cfg.proxy_host, cfg.proxy_port))
		return 1;
	awards_init();
	ULOG_INFO("achivmentsd %s, upstream %s, state %s\n", ACHD_VERSION, cfg.upstream_host, cfg.state_dir);
	refresh_init();
	netstate_init(on_net_change);
	uloop_signal_add(&sigusr1);
	uloop_signal_add(&sighup);
	uloop_run();
	ULOG_INFO("shutting down\n");
	return 0;
}

/* Every step tolerates a module that was never initialised. */
static void teardown(void)
{
	http_server_shutdown();
	refresh_shutdown();
	awards_shutdown();
	up_shutdown();
	creds_free();
	db_close();
	uloop_done();
}

int main(int argc, char **argv)
{
	const char *config = NULL, *state = NULL, *host = NULL, *upstream = NULL;
	bool use_syslog = false;
	int opt, port = 0, ret;

	ulog_threshold(LOG_INFO);
	while ((opt = getopt(argc, argv, "c:s:l:p:u:Svh")) != -1) {
		switch (opt) {
		case 'c': config = optarg; break;
		case 's': state = optarg; break;
		case 'l': host = optarg; break;
		case 'p': port = atoi(optarg); break;
		case 'u': upstream = optarg; break;
		case 'S': use_syslog = true; break;
		case 'v': ulog_threshold(LOG_DEBUG); break;
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}
	ulog_open(use_syslog ? ULOG_SYSLOG : ULOG_STDIO, LOG_DAEMON, "achivmentsd");

	config_defaults();
	if (config_load(config ? config : DEFAULT_CONFIG, config != NULL))
		return 1;
	if (state)
		cfg.state_dir = state;
	if (host)
		cfg.proxy_host = host;
	if (port)
		cfg.proxy_port = port;
	if (upstream)
		cfg.upstream_host = upstream;

	signal(SIGPIPE, SIG_IGN);
	uloop_init();
	creds_init();
	ret = run();
	teardown();
	return ret;
}
