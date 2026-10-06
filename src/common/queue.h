#ifndef SOLVAKT_QUEUE_H
#define SOLVAKT_QUEUE_H

#include <pthread.h>
#include <stddef.h>

/* Trådsäker, begränsad FIFO-kö för producent-konsument-mönstret mellan
 * pipeline-stegen. Kön äger inte elementen; den som hämtar ett element
 * ansvarar för att frigöra det. */
typedef struct
{
    void          **items;
    size_t          capacity;
    size_t          head;
    size_t          count;
    int             closed;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} Queue;

int queue_init(Queue *q, size_t capacity);
void queue_destroy(Queue *q);

/* Blockerar medan kön är full. Returnerar 0, eller -1 om kön är stängd. */
int queue_push(Queue *q, void *item);

/* Blockerar medan kön är tom. Returnerar 0, eller -1 när kön är både stängd
 * och tom. Element som låg i kön när den stängdes går alltså att hämta. */
int queue_pop(Queue *q, void **item);

/* Väcker alla väntande trådar; därefter misslyckas queue_push. */
void queue_close(Queue *q);

#endif
