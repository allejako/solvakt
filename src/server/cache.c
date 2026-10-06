#include "server/cache.h"

#include <string.h>

void cache_init(Cache *cache, int ttl_s)
{
    memset(&cache->plan, 0, sizeof(cache->plan));
    cache->stored_at = 0;
    cache->ttl_s = ttl_s;
    cache->has_data = 0;
    pthread_mutex_init(&cache->mutex, NULL);
}

void cache_destroy(Cache *cache)
{
    pthread_mutex_destroy(&cache->mutex);
}

void cache_put(Cache *cache, const Plan *plan, time_t now)
{
    pthread_mutex_lock(&cache->mutex);
    cache->plan = *plan;
    cache->stored_at = now;
    cache->has_data = 1;
    pthread_mutex_unlock(&cache->mutex);
}

int cache_get(Cache *cache, Plan *out, time_t now, int *age_s)
{
    int result = CACHE_OK;

    pthread_mutex_lock(&cache->mutex);
    int age = (int)(now - cache->stored_at);
    if (!cache->has_data)
        result = CACHE_EMPTY;
    else if (age > cache->ttl_s)
        result = CACHE_EXPIRED;
    else
        *out = cache->plan; /* kopia: läsaren behöver inte hålla låset */

    if (age_s != NULL)
        *age_s = cache->has_data ? age : 0;
    pthread_mutex_unlock(&cache->mutex);

    return result;
}
