/**
 * huffman.h - Compresión/descompresión Huffman concurrente por bloques (pthreads).
 *
 * Un "job" lanza un hilo coordinador que a su vez lanza un pool de hilos
 * trabajadores. La interfaz es no bloqueante: el llamador consulta el progreso
 * con huff_poll() y recoge el resultado con huff_finish().
 */
#ifndef HUFFMAN_H
#define HUFFMAN_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HuffJob HuffJob;

typedef enum { HUFF_COMPRESS = 0, HUFF_DECOMPRESS = 1 } HuffMode;
typedef enum { HUFF_RUNNING = 0, HUFF_DONE, HUFF_FAILED, HUFF_CANCELLED } HuffState;

/* nworkers <= 0 -> automático (nº de CPUs, máx. 8); block_size == 0 -> 64 KiB.
 * Devuelve NULL (con errno) si no pudo arrancar. */
HuffJob *huff_start(HuffMode mode, const char *in_path, const char *out_path,
                    int nworkers, unsigned block_size);

/* Estado actual y porcentaje 0..100. No bloquea más que un mutex breve. */
HuffState huff_poll(HuffJob *job, int *percent);

/* Pide cancelar; el job termina en HUFF_CANCELLED (huff_finish sigue siendo necesario). */
void huff_cancel(HuffJob *job);

/* Espera (pthread_join) al coordinador, libera el job. Devuelve el estado final;
 * si errmsg != NULL copia el mensaje de error (o ""). */
HuffState huff_finish(HuffJob *job, char *errmsg, int errlen);

/* 1 si el job sigue corriendo y usa 'path' como entrada o salida (mismo archivo). */
int huff_path_busy(HuffJob *job, const char *path);

#ifdef __cplusplus
}
#endif

#endif
