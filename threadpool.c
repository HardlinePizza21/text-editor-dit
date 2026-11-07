#include "threadpool.h"
#include <pthread.h>
#include <stdlib.h>

typedef struct Task
{
    void (*fn)(void *);
    void *arg;
    struct Task *next;
} Task;

struct ThreadPool
{
    pthread_mutex_t lock;
    pthread_cond_t has_work; /* hay tareas o se está apagando */
    pthread_cond_t idle;     /* cola vacía y ningún hilo ocupado */
    Task *head, *tail;
    int active;
    int shutdown;
    int nthreads;
    pthread_t *threads;
};

static void *worker(void *p)
{
    ThreadPool *tp = p;
    pthread_mutex_lock(&tp->lock);
    for (;;)
    {
        while (!tp->head && !tp->shutdown)
            pthread_cond_wait(&tp->has_work, &tp->lock);
        if (!tp->head && tp->shutdown)
            break;

        Task *t = tp->head;
        tp->head = t->next;
        if (!tp->head)
            tp->tail = NULL;
        tp->active++;
        pthread_mutex_unlock(&tp->lock);

        t->fn(t->arg);
        free(t);

        pthread_mutex_lock(&tp->lock);
        tp->active--;
        if (!tp->head && tp->active == 0)
            pthread_cond_broadcast(&tp->idle);
    }
    pthread_mutex_unlock(&tp->lock);
    return NULL;
}

ThreadPool *tp_create(int nthreads)
{
    if (nthreads < 1)
        nthreads = 1;
    ThreadPool *tp = calloc(1, sizeof(*tp));
    if (!tp)
        return NULL;
    tp->threads = calloc(nthreads, sizeof(pthread_t));
    if (!tp->threads)
    {
        free(tp);
        return NULL;
    }
    pthread_mutex_init(&tp->lock, NULL);
    pthread_cond_init(&tp->has_work, NULL);
    pthread_cond_init(&tp->idle, NULL);

    for (int i = 0; i < nthreads; i++)
    {
        if (pthread_create(&tp->threads[i], NULL, worker, tp) != 0)
            break;
        tp->nthreads++;
    }
    if (tp->nthreads == 0)
    {
        free(tp->threads);
        free(tp);
        return NULL;
    }
    return tp;
}

int tp_submit(ThreadPool *tp, void (*fn)(void *), void *arg)
{
    Task *t = malloc(sizeof(*t));
    if (!t)
        return -1;
    t->fn = fn;
    t->arg = arg;
    t->next = NULL;

    pthread_mutex_lock(&tp->lock);
    if (tp->shutdown)
    {
        pthread_mutex_unlock(&tp->lock);
        free(t);
        return -1;
    }
    if (tp->tail)
        tp->tail->next = t;
    else
        tp->head = t;
    tp->tail = t;
    pthread_cond_signal(&tp->has_work);
    pthread_mutex_unlock(&tp->lock);
    return 0;
}

void tp_wait(ThreadPool *tp)
{
    pthread_mutex_lock(&tp->lock);
    while (tp->head || tp->active)
        pthread_cond_wait(&tp->idle, &tp->lock);
    pthread_mutex_unlock(&tp->lock);
}

void tp_destroy(ThreadPool *tp)
{
    if (!tp)
        return;
    pthread_mutex_lock(&tp->lock);
    tp->shutdown = 1;
    pthread_cond_broadcast(&tp->has_work);
    pthread_mutex_unlock(&tp->lock);
    for (int i = 0; i < tp->nthreads; i++)
        pthread_join(tp->threads[i], NULL);
    free(tp->threads);
    pthread_mutex_destroy(&tp->lock);
    pthread_cond_destroy(&tp->has_work);
    pthread_cond_destroy(&tp->idle);
    free(tp);
}
