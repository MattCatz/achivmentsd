#include <string.h>
#include <strings.h>

#include <libubox/kvlist.h>

#include "config.h"
#include "creds.h"
#include "db.h"
#include "msg.h"
#include "util.h"

/* lowercased user -> token */
static struct kvlist tokens;

void creds_init(void)
{
	kvlist_init(&tokens, kvlist_strlen);
}

void creds_note(const char *user, const char *token)
{
	_autofree_ char *u = NULL;

	if (!user || !*user || !token || !*token)
		return;
	u = strdup_lower(user);
	kvlist_set(&tokens, u, token);
}

/* Reads `key = "value"` from retroarch.cfg. */
static char *retroarch_value(const char *key)
{
	_autofree_ char *data = cfg.retroarch_cfg ? read_file(cfg.retroarch_cfg, NULL) : NULL;
	size_t kl = strlen(key);
	char *line, *save = NULL;

	if (!data)
		return NULL;
	for (line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *v, *end;

		line += strspn(line, " \t");
		if (strncmp(line, key, kl) || (line[kl] != ' ' && line[kl] != '='))
			continue;
		v = strchr(line + kl, '=');
		if (!v)
			continue;
		v += 1 + strspn(v + 1, " \t");
		if (*v == '"')
			v++;
		end = v + strcspn(v, "\"\r");
		if (end > v)
			return xstrndup(v, end - v);
	}
	return NULL;
}

static char *cached_login_token(const char *user)
{
	const struct cache_key key = cache_key_login(user);
	_autofree_ char *body = db_cache_get(&key, NULL);
	_autoblob_ struct blob_buf b = { 0 };

	if (!msg_parse(&b, body) || !msg_success(msg_root(&b)))
		return NULL;
	return xstrdup(msg_str(msg_root(&b), "Token"));
}

char *creds_token(const char *user)
{
	_autofree_ char *u = strdup_lower(user);
	_autofree_ char *ra_user = NULL;
	const char *known;
	char *token;

	if (!u)
		return NULL;
	if ((known = kvlist_get(&tokens, u)))
		return xstrdup(known);
	if ((token = cached_login_token(u)))
		return token;
	ra_user = retroarch_value("cheevos_username");
	if (ra_user && !strcasecmp(ra_user, u))
		return retroarch_value("cheevos_token");
	return NULL;
}

void creds_free(void)
{
	kvlist_free(&tokens);
}
