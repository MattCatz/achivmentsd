#ifndef ACHD_CREDS_H
#define ACHD_CREDS_H

/*
 * API tokens for requests the daemon makes itself (award replay, refresh).
 * Lookup order: the newest token seen in emulator traffic, the cached login2
 * response, then cheevos_token in retroarch.cfg.
 */
void creds_init(void);
void creds_note(const char *user, const char *token);
/* malloc'd, or NULL when no token is known. */
char *creds_token(const char *user);
void creds_free(void);

#endif
