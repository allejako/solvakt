#ifndef SOLVAKT_SERVER_H
#define SOLVAKT_SERVER_H

#include "common/config.h"

#include <signal.h>

/* Servern äger pipelinen fetch -> parse -> compute -> cache, fetcher-processen
 * och Unix-socketen som klienterna ansluter till. */
typedef struct Server Server;

/* Skapar köer och cache, startar fetcher-processen och börjar lyssna på
 * cfg->socket_path. Returnerar NULL om något steg misslyckas. */
Server *server_create(const Config *cfg, const char *fetcher_path);

/* Startar pipeline-trådarna och tar emot klienter tills *stop blir sant.
 * Stänger sedan ned trådar och fetcher i ordning. Returnerar 0 eller -1. */
int server_run(Server *server, const volatile sig_atomic_t *stop);

/* Stänger socketen, tar bort socketfilen och frigör alla resurser. */
void server_destroy(Server *server);

#endif
