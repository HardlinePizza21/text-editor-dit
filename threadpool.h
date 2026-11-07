#ifndef THREADPOOL_H
#define THREADPOOL_H

/* Pool de hilos de tamaño fijo con cola FIFO de tareas. */
typedef struct ThreadPool ThreadPool;

ThreadPool *tp_create(int nthreads);
/* Encola una tarea. Retorna 0 si se encoló, -1 si falló. */
int tp_submit(ThreadPool *tp, void (*fn)(void *), void *arg);
/* Bloquea hasta que no haya tareas pendientes ni en ejecución. */
void tp_wait(ThreadPool *tp);
/* Espera las tareas pendientes, detiene y une los hilos y libera el pool. */
void tp_destroy(ThreadPool *tp);

#endif
