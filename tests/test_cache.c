#include "check.h"
#include "server/cache.h"

#include <stdlib.h>

static void test_empty_then_hit(void)
{
    Cache cache;
    Plan *plan = calloc(1, sizeof(Plan));
    Plan *out = calloc(1, sizeof(Plan));
    int age = -1;

    cache_init(&cache, 60);
    CHECK(cache_get(&cache, out, 1000, &age) == CACHE_EMPTY);

    plan->count = 3;
    plan->slots[2].price = 1.25;
    cache_put(&cache, plan, 1000);

    CHECK(cache_get(&cache, out, 1010, &age) == CACHE_OK);
    CHECK(age == 10);
    CHECK(out->count == 3);
    CHECK_NEAR(out->slots[2].price, 1.25, 1e-12);

    cache_destroy(&cache);
    free(plan);
    free(out);
}

static void test_ttl(void)
{
    Cache cache;
    Plan *plan = calloc(1, sizeof(Plan));
    Plan *out = calloc(1, sizeof(Plan));
    int age = 0;

    cache_init(&cache, 60);
    plan->count = 1;
    cache_put(&cache, plan, 1000);

    /* Giltig till och med exakt TTL, utgången sekunden efter. */
    CHECK(cache_get(&cache, out, 1060, &age) == CACHE_OK);
    CHECK(cache_get(&cache, out, 1061, &age) == CACHE_EXPIRED);
    CHECK(age == 61);

    /* En ny plan gör cachen giltig igen. */
    plan->count = 2;
    cache_put(&cache, plan, 1100);
    CHECK(cache_get(&cache, out, 1101, NULL) == CACHE_OK);
    CHECK(out->count == 2);

    cache_destroy(&cache);
    free(plan);
    free(out);
}

typedef struct
{
    Cache *cache;
    int    errors;
} ReaderArg;

static void *reader(void *arg)
{
    ReaderArg *r = arg;
    Plan *out = calloc(1, sizeof(Plan));
    for (int i = 0; i < 2000; i++)
    {
        if (cache_get(r->cache, out, 1000, NULL) != CACHE_OK)
            continue;
        /* Skrivaren sätter alltid count och pris ihop; en halvskriven plan
         * skulle synas som att de inte stämmer överens. */
        if ((int)out->slots[0].price != out->count)
            r->errors++;
    }
    free(out);
    return NULL;
}

static void test_concurrent_readers_and_writer(void)
{
    Cache cache;
    cache_init(&cache, 60);

    pthread_t threads[4];
    ReaderArg args[4];
    for (int i = 0; i < 4; i++)
    {
        args[i] = (ReaderArg){&cache, 0};
        pthread_create(&threads[i], NULL, reader, &args[i]);
    }

    Plan *plan = calloc(1, sizeof(Plan));
    for (int i = 1; i <= 2000; i++)
    {
        plan->count = i % MAX_SLOTS + 1;
        plan->slots[0].price = plan->count;
        cache_put(&cache, plan, 1000);
    }
    free(plan);

    for (int i = 0; i < 4; i++)
    {
        pthread_join(threads[i], NULL);
        CHECK(args[i].errors == 0);
    }
    cache_destroy(&cache);
}

int main(void)
{
    test_empty_then_hit();
    test_ttl();
    test_concurrent_readers_and_writer();
    return TEST_RESULT();
}
