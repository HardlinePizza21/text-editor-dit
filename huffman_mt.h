#ifndef HUFFMAN_MT_H
#define HUFFMAN_MT_H

#include <stdatomic.h>
#include <stddef.h>

#define HUFF_EXT ".huf"

typedef enum { HUFF_COMPRESS, HUFF_DECOMPRESS } HuffMode;

typedef enum
{
    HUFF_PHASE_IDLE,
    HUFF_PHASE_COUNT,  /* conteo paralelo de frecuencias */
    HUFF_PHASE_TREE,   /* construcción del árbol global */
    HUFF_PHASE_ENCODE, /* codificación paralela + escritura ordenada */
    HUFF_PHASE_DECODE  /* decodificación paralela + escritura ordenada */
} HuffPhase;

/* Estado compartido entre quien ejecuta la operación y quien la observa. */
typedef struct
{
    atomic_int cancel;
    atomic_int phase;
    atomic_ullong done;
    atomic_ullong total;
    char err[160]; /* escrito solo bajo mutex interno; leer tras terminar */
} HuffCtl;

void huffctl_init(HuffCtl *ctl);

/* Versión síncrona. Retorna 0 ok, -1 error (ctl->err), -2 cancelado.
 * nthreads <= 0 usa el número de CPUs. ctl puede ser NULL. */
int huff_compress_file(const char *in, const char *out, int nthreads, HuffCtl *ctl);
int huff_decompress_file(const char *in, const char *out, int nthreads, HuffCtl *ctl);

/* ---- Trabajo asíncrono en segundo plano ---- */
typedef struct HuffJob HuffJob;

typedef enum { JOB_RUNNING, JOB_OK, JOB_FAILED, JOB_CANCELLED } JobState;

HuffJob *huffjob_start(HuffMode mode, const char *in, const char *out, int nthreads);
JobState huffjob_state(HuffJob *job);
int huffjob_percent(HuffJob *job);
const char *huffjob_phase_name(HuffJob *job);
HuffMode huffjob_mode(HuffJob *job);
const char *huffjob_input(HuffJob *job);
const char *huffjob_output(HuffJob *job);
void huffjob_cancel(HuffJob *job);
/* Une el hilo, copia el mensaje de error (si hay) y libera el job. */
JobState huffjob_finish(HuffJob *job, char *err, size_t errlen);

#endif
