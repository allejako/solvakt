#ifndef SOLVAKT_FETCHER_PROC_H
#define SOLVAKT_FETCHER_PROC_H

#include <stdatomic.h>
#include <stddef.h>
#include <sys/types.h>

/* Serverns sida av fetcher-processen: startar barnprocessen med fork/exec
 * och pratar med den över två anonyma pipes. */
typedef struct
{
    pid_t pid;
    int   to_fd;   /* skrivs av servern, är fetcherns stdin */
    int   from_fd; /* läses av servern, är fetcherns stdout */
    int   timeout_s;
} Fetcher;

#define FETCH_OK      0
#define FETCH_FAILED -1 /* fetchern svarade ERR (t.ex. HTTP-fel eller timeout) */
#define FETCH_DEAD   -2 /* pipen är stängd eller svaret uteblev; starta om */

/* Startar exe_path som barnprocess. Returnerar 0 eller -1. */
int fetcher_start(Fetcher *f, const char *exe_path, int http_timeout_s);

/* Hämtar en URL via fetchern. Vid FETCH_OK pekar *body på en nollterminerad
 * buffert som anroparen frigör med free(). Om running inte är NULL avbryts
 * väntan när flaggan blir false. */
int fetcher_request(Fetcher *f, const char *url, char **body, size_t *len,
                    const atomic_bool *running);

/* Kontrollerar utan att blockera om barnprocessen har avslutats. I så fall
 * städas den upp med waitpid (ingen zombie blir kvar), pipes stängs och 1
 * returneras. Returnerar 0 om fetchern lever eller redan är stoppad. */
int fetcher_reap_if_dead(Fetcher *f);

/* Stänger pipes, väntar in barnprocessen med waitpid och dödar den om den
 * inte avslutas själv. Säker att anropa flera gånger. */
void fetcher_stop(Fetcher *f);

#endif
