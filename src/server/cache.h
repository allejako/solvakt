#ifndef SOLVAKT_CACHE_H
#define SOLVAKT_CACHE_H

#include "common/types.h"

#include <pthread.h>

/* Cache-steget: senaste beräknade plan, skyddad av mutex och med TTL.
 * Compute-tråden skriver, klienttrådarna läser. */
typedef struct
{
    Plan            plan;
    time_t          stored_at;
    int             ttl_s;
    int             has_data;
    pthread_mutex_t mutex;
} Cache;

#define CACHE_OK       0
#define CACHE_EMPTY   -1 /* ingen plan har lagrats ännu */
#define CACHE_EXPIRED -2 /* planen är äldre än TTL */

void cache_init(Cache *cache, int ttl_s);
void cache_destroy(Cache *cache);

void cache_put(Cache *cache, const Plan *plan, time_t now);

/* Kopierar planen till *out om den finns och inte har gått ut. age_s får
 * planens ålder i sekunder och kan vara NULL. */
int cache_get(Cache *cache, Plan *out, time_t now, int *age_s);

#endif
