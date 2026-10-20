#ifndef ACHD_ROUTER_H
#define ACHD_ROUTER_H

struct client;

/* Answers one parsed request, from upstream when reachable, else from cache. */
void router_handle(struct client *c);

#endif
