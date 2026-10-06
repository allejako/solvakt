#include "common/queue.h"

#include <stdlib.h>

int queue_init(Queue *q, size_t capacity)
{
    if (capacity == 0)
        return -1;
    q->items = calloc(capacity, sizeof(void *));
    if (q->items == NULL)
        return -1;
    q->capacity = capacity;
    q->head = 0;
    q->count = 0;
    q->closed = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    return 0;
}

void queue_destroy(Queue *q)
{
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->mutex);
    free(q->items);
    q->items = NULL;
}

int queue_push(Queue *q, void *item)
{
    pthread_mutex_lock(&q->mutex);
    /* while och inte if: cond_wait kan vakna utan att villkoret gäller. */
    while (q->count == q->capacity && !q->closed)
        pthread_cond_wait(&q->not_full, &q->mutex);

    if (q->closed)
    {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }

    q->items[(q->head + q->count) % q->capacity] = item;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

int queue_pop(Queue *q, void **item)
{
    pthread_mutex_lock(&q->mutex);
    while (q->count == 0 && !q->closed)
        pthread_cond_wait(&q->not_empty, &q->mutex);

    if (q->count == 0)
    {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }

    *item = q->items[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

void queue_close(Queue *q)
{
    pthread_mutex_lock(&q->mutex);
    q->closed = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
}
