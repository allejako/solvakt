#include "check.h"
#include "common/queue.h"

#include <stdint.h>
#include <stdlib.h>

#define PRODUCERS 4
#define CONSUMERS 3
#define PER_PRODUCER 5000

typedef struct
{
    Queue *queue;
    long   sum;
    int    count;
} Worker;

static void *producer(void *arg)
{
    Worker *w = arg;
    for (intptr_t i = 1; i <= PER_PRODUCER; i++)
    {
        if (queue_push(w->queue, (void *)i) != 0)
            break;
        w->sum += i;
        w->count++;
    }
    return NULL;
}

static void *consumer(void *arg)
{
    Worker *w = arg;
    void *item;
    while (queue_pop(w->queue, &item) == 0)
    {
        w->sum += (intptr_t)item;
        w->count++;
    }
    return NULL;
}

static void test_fifo_order(void)
{
    Queue q;
    CHECK(queue_init(&q, 4) == 0);
    for (intptr_t i = 1; i <= 4; i++)
        CHECK(queue_push(&q, (void *)i) == 0);

    void *item = NULL;
    for (intptr_t i = 1; i <= 4; i++)
    {
        CHECK(queue_pop(&q, &item) == 0);
        CHECK((intptr_t)item == i);
    }
    queue_destroy(&q);
}

static void test_close(void)
{
    Queue q;
    CHECK(queue_init(&q, 2) == 0);
    CHECK(queue_push(&q, (void *)(intptr_t)7) == 0);
    queue_close(&q);

    /* Stängd kö tar inte emot nytt men lämnar ut det som redan ligger där. */
    CHECK(queue_push(&q, (void *)(intptr_t)8) == -1);
    void *item = NULL;
    CHECK(queue_pop(&q, &item) == 0);
    CHECK((intptr_t)item == 7);
    CHECK(queue_pop(&q, &item) == -1);
    queue_destroy(&q);
}

static void test_zero_capacity(void)
{
    Queue q;
    CHECK(queue_init(&q, 0) == -1);
}

/* Flera producenter och konsumenter mot en liten kö: inget element får
 * tappas eller levereras två gånger. */
static void test_concurrent(void)
{
    Queue q;
    CHECK(queue_init(&q, 8) == 0);

    pthread_t prod_threads[PRODUCERS], cons_threads[CONSUMERS];
    Worker prod[PRODUCERS], cons[CONSUMERS];

    for (int i = 0; i < CONSUMERS; i++)
    {
        cons[i] = (Worker){&q, 0, 0};
        pthread_create(&cons_threads[i], NULL, consumer, &cons[i]);
    }
    for (int i = 0; i < PRODUCERS; i++)
    {
        prod[i] = (Worker){&q, 0, 0};
        pthread_create(&prod_threads[i], NULL, producer, &prod[i]);
    }

    for (int i = 0; i < PRODUCERS; i++)
        pthread_join(prod_threads[i], NULL);
    queue_close(&q);
    for (int i = 0; i < CONSUMERS; i++)
        pthread_join(cons_threads[i], NULL);

    long pushed_sum = 0, popped_sum = 0;
    int pushed = 0, popped = 0;
    for (int i = 0; i < PRODUCERS; i++)
    {
        pushed_sum += prod[i].sum;
        pushed += prod[i].count;
    }
    for (int i = 0; i < CONSUMERS; i++)
    {
        popped_sum += cons[i].sum;
        popped += cons[i].count;
    }

    CHECK(pushed == PRODUCERS * PER_PRODUCER);
    CHECK(popped == pushed);
    CHECK(popped_sum == pushed_sum);
    queue_destroy(&q);
}

int main(void)
{
    test_fifo_order();
    test_close();
    test_zero_capacity();
    test_concurrent();
    return TEST_RESULT();
}
