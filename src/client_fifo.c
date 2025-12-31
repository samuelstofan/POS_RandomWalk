#include "client_fifo.h"

void fifo_push(StepFIFO *f, Step s)
{
    pthread_mutex_lock(&f->mtx);

    if (f->count == STEP_FIFO_CAP) {
        f->tail = (f->tail + 1) % STEP_FIFO_CAP;
        f->count--;
    }

    f->buf[f->head] = s;
    f->head = (f->head + 1) % STEP_FIFO_CAP;
    f->count++;

    pthread_mutex_unlock(&f->mtx);
}

int fifo_pop(StepFIFO *f, Step *out)
{
    pthread_mutex_lock(&f->mtx);

    if (f->count == 0) {
        pthread_mutex_unlock(&f->mtx);
        return 0;
    }

    *out = f->buf[f->tail];
    f->tail = (f->tail + 1) % STEP_FIFO_CAP;
    f->count--;

    pthread_mutex_unlock(&f->mtx);
    return 1;
}
