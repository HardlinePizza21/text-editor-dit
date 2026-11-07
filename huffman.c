/*
 * huffman.c - Codificación de Huffman concurrente por bloques.
 *
 * Arquitectura (ver HUFFMAN_SUSTENTACION.md):
 *   - Hilo coordinador (controller) + pool de N hilos trabajadores.
 *   - Fase 1 (solo compresión): los trabajadores toman bloques de una cola
 *     protegida por mutex, cuentan frecuencias en una tabla local y la reducen
 *     (suma) en la tabla global al terminar.
 *   - Fase 2: los trabajadores codifican/decodifican bloques en paralelo y
 *     depositan el resultado en slots[i]. El coordinador es el único que
 *     escribe, en orden estricto, esperando en una variable de condición.
 *     Una ventana (WINDOW) acota los bloques en vuelo -> memoria acotada,
 *     sin espera activa: todo bloqueo es pthread_cond_wait.
 *
 * Formato:  "HUF1" | u64 tamaño | u32 tam_bloque | u32 nbloques | u64 freq[256]
 *           por bloque: u32 orig_len | u32 crc32 | u32 comp_len | datos (bits MSB-first)
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "huffman.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEFAULT_BLOCK (64u * 1024u)
#define MAX_BLOCK (64u * 1024u * 1024u)
#define WINDOW 16
#define MAX_CODE_LEN 64
#define HEADER_SIZE (4 + 8 + 4 + 4 + 256 * 8)
#define BLOCK_HDR 12

typedef struct {
    unsigned char *buf;
    size_t len;
    int ready;
} Slot;

typedef struct {
    uint32_t orig_len, crc, comp_len;
    uint64_t data_off;
} BlockInfo;

typedef struct {
    int left, right; /* -1 en hojas */
    int symbol;
} TNode;

struct HuffJob {
    HuffMode mode;
    char *in_path, *out_path, *tmp_path;
    int in_fd, out_fd;
    dev_t in_dev;
    ino_t in_ino;
    int nworkers;
    uint32_t block_size;
    uint64_t orig_size;
    uint64_t nblocks;
    uint64_t file_size;

    pthread_mutex_t mu;
    pthread_cond_t cv_space; /* los trabajadores esperan ventana libre */
    pthread_cond_t cv_ready; /* el escritor espera el bloque i */

    uint64_t next_task;  /* siguiente bloque a repartir */
    uint64_t next_write; /* siguiente bloque a escribir */
    Slot *slots;
    BlockInfo *info;

    uint64_t freq[256];
    uint64_t code[256];
    uint8_t clen[256];
    TNode nodes[512];
    int root;

    int stop; /* cancelación o error: todos deben salir */
    int cancelled;
    int failed;
    char errmsg[160];
    uint64_t done, total;
    HuffState state;

    pthread_t controller;
};

/* ---------------------------------------------------------------- utilidades */

static uint32_t crc_table[256];
static pthread_once_t crc_once = PTHREAD_ONCE_INIT;

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static uint32_t crc32_buf(const unsigned char *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--)
        c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void put32(unsigned char *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}
static void put64(unsigned char *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}
static uint32_t get32(const unsigned char *p)
{
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}
static uint64_t get64(const unsigned char *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static int pread_full(int fd, void *buf, size_t n, uint64_t off)
{
    unsigned char *p = buf;
    while (n > 0) {
        ssize_t r = pread(fd, p, n, (off_t)off);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0) { /* archivo truncado mientras se procesaba */
            errno = EIO;
            return -1;
        }
        p += r;
        off += (uint64_t)r;
        n -= (size_t)r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n)
{
    const unsigned char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* Registra el primer error y despierta a todos los hilos. */
static void job_fail(HuffJob *j, const char *fmt, ...)
{
    pthread_mutex_lock(&j->mu);
    if (!j->failed && !j->cancelled) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(j->errmsg, sizeof j->errmsg, fmt, ap);
        va_end(ap);
        j->failed = 1;
    }
    j->stop = 1;
    pthread_cond_broadcast(&j->cv_space);
    pthread_cond_broadcast(&j->cv_ready);
    pthread_mutex_unlock(&j->mu);
}

static void job_progress(HuffJob *j)
{
    pthread_mutex_lock(&j->mu);
    j->done++;
    pthread_mutex_unlock(&j->mu);
}

/* ------------------------------------------------------------ árbol Huffman */

static void build_tree(HuffJob *j)
{
    int n = 0;
    uint64_t w[512];
    int alive[512], na = 0;

    for (int s = 0; s < 256; s++) {
        if (j->freq[s]) {
            j->nodes[n] = (TNode){-1, -1, s};
            w[n] = j->freq[s];
            alive[na++] = n++;
        }
    }
    if (na == 0) {
        j->root = -1;
        return;
    }
    while (na > 1) {
        /* dos mínimos, desempate por posición: árbol determinista */
        int a = 0, b = -1;
        for (int i = 1; i < na; i++)
            if (w[alive[i]] < w[alive[a]])
                a = i;
        for (int i = 0; i < na; i++) {
            if (i == a)
                continue;
            if (b < 0 || w[alive[i]] < w[alive[b]])
                b = i;
        }
        int ia = alive[a], ib = alive[b];
        j->nodes[n] = (TNode){ia, ib, -1};
        w[n] = w[ia] + w[ib];
        int lo = a < b ? a : b, hi = a < b ? b : a;
        alive[lo] = n++;
        alive[hi] = alive[--na];
    }
    j->root = alive[0];
}

static int assign_codes(HuffJob *j)
{
    memset(j->clen, 0, sizeof j->clen);
    if (j->root < 0)
        return 0;
    if (j->nodes[j->root].left < 0) { /* un solo símbolo: código "0" */
        j->code[j->nodes[j->root].symbol] = 0;
        j->clen[j->nodes[j->root].symbol] = 1;
        return 0;
    }
    int stack[512], depth[512];
    uint64_t bits[512];
    int sp = 0;
    stack[sp] = j->root;
    depth[sp] = 0;
    bits[sp++] = 0;
    while (sp > 0) {
        sp--;
        int nd = stack[sp], d = depth[sp];
        uint64_t b = bits[sp];
        TNode *t = &j->nodes[nd];
        if (t->left < 0) {
            j->code[t->symbol] = b;
            j->clen[t->symbol] = (uint8_t)d;
            continue;
        }
        if (d + 1 > MAX_CODE_LEN)
            return -1;
        stack[sp] = t->left;
        depth[sp] = d + 1;
        bits[sp++] = b << 1;
        stack[sp] = t->right;
        depth[sp] = d + 1;
        bits[sp++] = (b << 1) | 1;
    }
    return 0;
}

/* ------------------------------------------------------------- trabajadores */

static void *count_worker(void *arg)
{
    HuffJob *j = arg;
    uint64_t local[256] = {0};
    unsigned char *buf = malloc(j->block_size);
    if (!buf) {
        job_fail(j, "sin memoria");
        return NULL;
    }
    for (;;) {
        pthread_mutex_lock(&j->mu);
        if (j->stop || j->next_task >= j->nblocks) {
            pthread_mutex_unlock(&j->mu);
            break;
        }
        uint64_t i = j->next_task++;
        pthread_mutex_unlock(&j->mu);

        uint64_t off = i * j->block_size;
        size_t len = (size_t)(j->orig_size - off < j->block_size ? j->orig_size - off : j->block_size);
        if (pread_full(j->in_fd, buf, len, off) != 0) {
            job_fail(j, "lectura fallida (el archivo cambió?)");
            break;
        }
        for (size_t k = 0; k < len; k++)
            local[buf[k]]++;
        job_progress(j);
    }
    free(buf);
    pthread_mutex_lock(&j->mu); /* reducción: suma de tablas locales */
    for (int s = 0; s < 256; s++)
        j->freq[s] += local[s];
    pthread_mutex_unlock(&j->mu);
    return NULL;
}

/* 0 ok, -1 sin memoria, -2 error de E/S o datos inconsistentes, -3 corrupto */
static int compress_block(HuffJob *j, uint64_t i, unsigned char **out, size_t *outlen)
{
    uint64_t off = i * j->block_size;
    size_t len = (size_t)(j->orig_size - off < j->block_size ? j->orig_size - off : j->block_size);
    unsigned char *src = malloc(len);
    if (!src)
        return -1;
    if (pread_full(j->in_fd, src, len, off) != 0) {
        free(src);
        return -2;
    }
    uint64_t nbits = 0;
    for (size_t k = 0; k < len; k++) {
        if (!j->clen[src[k]]) { /* símbolo fuera de la tabla: el archivo cambió */
            free(src);
            return -2;
        }
        nbits += j->clen[src[k]];
    }
    size_t comp = (size_t)((nbits + 7) / 8);
    unsigned char *dst = calloc(1, BLOCK_HDR + comp);
    if (!dst) {
        free(src);
        return -1;
    }
    put32(dst, (uint32_t)len);
    put32(dst + 4, crc32_buf(src, len));
    put32(dst + 8, (uint32_t)comp);

    unsigned char *w = dst + BLOCK_HDR;
    uint64_t pos = 0;
    for (size_t k = 0; k < len; k++) {
        uint64_t c = j->code[src[k]];
        for (int b = j->clen[src[k]] - 1; b >= 0; b--, pos++)
            if ((c >> b) & 1)
                w[pos >> 3] |= (unsigned char)(0x80 >> (pos & 7));
    }
    free(src);
    *out = dst;
    *outlen = BLOCK_HDR + comp;
    return 0;
}

static int decompress_block(HuffJob *j, uint64_t i, unsigned char **out, size_t *outlen)
{
    BlockInfo *bi = &j->info[i];
    unsigned char *comp = malloc(bi->comp_len ? bi->comp_len : 1);
    unsigned char *dst = malloc(bi->orig_len ? bi->orig_len : 1);
    if (!comp || !dst) {
        free(comp);
        free(dst);
        return -1;
    }
    if (pread_full(j->in_fd, comp, bi->comp_len, bi->data_off) != 0) {
        free(comp);
        free(dst);
        return -2;
    }
    const TNode *nd = j->nodes;
    if (nd[j->root].left < 0) {
        memset(dst, nd[j->root].symbol, bi->orig_len);
    } else {
        uint64_t pos = 0, maxbits = (uint64_t)bi->comp_len * 8;
        for (uint32_t k = 0; k < bi->orig_len; k++) {
            int cur = j->root;
            while (nd[cur].left >= 0) {
                if (pos >= maxbits) {
                    free(comp);
                    free(dst);
                    return -3;
                }
                int bit = (comp[pos >> 3] >> (7 - (pos & 7))) & 1;
                pos++;
                cur = bit ? nd[cur].right : nd[cur].left;
            }
            dst[k] = (unsigned char)nd[cur].symbol;
        }
    }
    free(comp);
    if (crc32_buf(dst, bi->orig_len) != bi->crc) {
        free(dst);
        return -3;
    }
    *out = dst;
    *outlen = bi->orig_len;
    return 0;
}

static void *block_worker(void *arg)
{
    HuffJob *j = arg;
    for (;;) {
        pthread_mutex_lock(&j->mu);
        while (!j->stop && j->next_task < j->nblocks &&
               j->next_task >= j->next_write + WINDOW)
            pthread_cond_wait(&j->cv_space, &j->mu);
        if (j->stop || j->next_task >= j->nblocks) {
            pthread_mutex_unlock(&j->mu);
            return NULL;
        }
        uint64_t i = j->next_task++;
        pthread_mutex_unlock(&j->mu);

        unsigned char *buf = NULL;
        size_t len = 0;
        int rc = j->mode == HUFF_COMPRESS ? compress_block(j, i, &buf, &len)
                                          : decompress_block(j, i, &buf, &len);
        if (rc != 0) {
            job_fail(j, rc == -1   ? "sin memoria (bloque %lu)"
                        : rc == -3 ? "datos corruptos: checksum del bloque %lu"
                                   : "error de E/S en el bloque %lu",
                     (unsigned long)i);
            return NULL;
        }
        pthread_mutex_lock(&j->mu);
        j->slots[i].buf = buf;
        j->slots[i].len = len;
        j->slots[i].ready = 1;
        pthread_cond_signal(&j->cv_ready);
        pthread_mutex_unlock(&j->mu);
    }
}

/* ------------------------------------------------------------- coordinador */

static int spawn(HuffJob *j, void *(*fn)(void *), pthread_t *th)
{
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old); /* las señales llegan solo al hilo de UI */
    int created = 0;
    while (created < j->nworkers && pthread_create(&th[created], NULL, fn, j) == 0)
        created++;
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    return created;
}

static void join_all(pthread_t *th, int n)
{
    for (int i = 0; i < n; i++)
        pthread_join(th[i], NULL);
}

static int read_header_and_index(HuffJob *j)
{
    unsigned char h[HEADER_SIZE];
    if (j->file_size < HEADER_SIZE || pread_full(j->in_fd, h, HEADER_SIZE, 0) != 0 ||
        memcmp(h, "HUF1", 4) != 0) {
        job_fail(j, "no es un archivo HUF1 válido");
        return -1;
    }
    j->orig_size = get64(h + 4);
    j->block_size = get32(h + 12);
    j->nblocks = get32(h + 16);
    for (int s = 0; s < 256; s++)
        j->freq[s] = get64(h + 20 + 8 * s);
    uint64_t fsum = 0;
    for (int s = 0; s < 256; s++)
        fsum += j->freq[s];
    if (fsum != j->orig_size || j->block_size == 0 || j->block_size > MAX_BLOCK ||
        j->nblocks != (j->orig_size + j->block_size - 1) / j->block_size ||
        j->nblocks * BLOCK_HDR > j->file_size) {
        job_fail(j, "cabecera corrupta");
        return -1;
    }
    j->info = calloc(j->nblocks ? j->nblocks : 1, sizeof(BlockInfo));
    if (!j->info) {
        job_fail(j, "sin memoria");
        return -1;
    }
    uint64_t pos = HEADER_SIZE, sum = 0;
    for (uint64_t i = 0; i < j->nblocks; i++) {
        unsigned char bh[BLOCK_HDR];
        uint64_t want = j->orig_size - i * j->block_size;
        if (want > j->block_size)
            want = j->block_size;
        if (pos + BLOCK_HDR > j->file_size || pread_full(j->in_fd, bh, BLOCK_HDR, pos) != 0) {
            job_fail(j, "archivo truncado");
            return -1;
        }
        BlockInfo *b = &j->info[i];
        b->orig_len = get32(bh);
        b->crc = get32(bh + 4);
        b->comp_len = get32(bh + 8);
        b->data_off = pos + BLOCK_HDR;
        if (b->orig_len != want || b->data_off + b->comp_len > j->file_size) {
            job_fail(j, "índice de bloques corrupto");
            return -1;
        }
        sum += b->orig_len;
        pos = b->data_off + b->comp_len;
    }
    if (pos != j->file_size || sum != j->orig_size) {
        job_fail(j, "archivo corrupto (tamaño inconsistente)");
        return -1;
    }
    return 0;
}

static int write_header(HuffJob *j)
{
    unsigned char h[HEADER_SIZE];
    memcpy(h, "HUF1", 4);
    put64(h + 4, j->orig_size);
    put32(h + 12, j->block_size);
    put32(h + 16, (uint32_t)j->nblocks);
    for (int s = 0; s < 256; s++)
        put64(h + 20 + 8 * s, j->freq[s]);
    return write_full(j->out_fd, h, sizeof h);
}

static void run_job(HuffJob *j)
{
    struct stat st;
    pthread_t *th = calloc((size_t)j->nworkers, sizeof *th);
    int nth = 0;

    if (!th) {
        job_fail(j, "sin memoria");
        return;
    }
    if (fstat(j->in_fd, &st) != 0) {
        job_fail(j, "fstat: %s", strerror(errno));
        goto out;
    }
    j->file_size = (uint64_t)st.st_size;

    if (j->mode == HUFF_COMPRESS) {
        j->orig_size = j->file_size;
        j->nblocks = (j->orig_size + j->block_size - 1) / j->block_size;
        if (j->nblocks > UINT32_MAX) {
            job_fail(j, "archivo demasiado grande");
            goto out;
        }
        pthread_mutex_lock(&j->mu);
        j->total = 2 * j->nblocks;
        pthread_mutex_unlock(&j->mu);

        nth = j->nblocks ? spawn(j, count_worker, th) : 0;
        if (j->nblocks && nth == 0) {
            job_fail(j, "no se pudieron crear hilos");
            goto out;
        }
        join_all(th, nth);
        nth = 0;
        pthread_mutex_lock(&j->mu);
        int stop = j->stop;
        j->next_task = 0;
        pthread_mutex_unlock(&j->mu);
        if (stop)
            goto out;
    } else {
        if (read_header_and_index(j) != 0)
            goto out;
        pthread_mutex_lock(&j->mu);
        j->total = j->nblocks;
        pthread_mutex_unlock(&j->mu);
    }

    build_tree(j);
    if (assign_codes(j) != 0) {
        job_fail(j, "código Huffman demasiado largo");
        goto out;
    }
    if (j->mode == HUFF_COMPRESS && write_header(j) != 0) {
        job_fail(j, "escritura: %s", strerror(errno));
        goto out;
    }

    j->slots = calloc(j->nblocks ? j->nblocks : 1, sizeof(Slot));
    if (!j->slots) {
        job_fail(j, "sin memoria");
        goto out;
    }
    nth = j->nblocks ? spawn(j, block_worker, th) : 0;
    if (j->nblocks && nth == 0) {
        job_fail(j, "no se pudieron crear hilos");
        goto out;
    }

    /* Escritor ordenado: duerme en cv_ready hasta que el bloque i esté listo. */
    for (uint64_t i = 0; i < j->nblocks; i++) {
        pthread_mutex_lock(&j->mu);
        while (!j->slots[i].ready && !j->stop)
            pthread_cond_wait(&j->cv_ready, &j->mu);
        if (j->stop) {
            pthread_mutex_unlock(&j->mu);
            break;
        }
        unsigned char *buf = j->slots[i].buf;
        size_t len = j->slots[i].len;
        j->slots[i].buf = NULL;
        j->next_write = i + 1;
        pthread_cond_broadcast(&j->cv_space);
        pthread_mutex_unlock(&j->mu);

        int rc = write_full(j->out_fd, buf, len);
        int e = errno;
        free(buf);
        if (rc != 0) {
            job_fail(j, "escritura: %s", strerror(e));
            break;
        }
        job_progress(j);
    }

out:
    join_all(th, nth); /* todo camino de error ya hizo stop=1 + broadcast */
    if (j->slots)
        for (uint64_t i = 0; i < j->nblocks; i++)
            free(j->slots[i].buf);
    free(th);
}

static void *controller_main(void *arg)
{
    HuffJob *j = arg;
    run_job(j);

    pthread_mutex_lock(&j->mu);
    int ok = !j->stop && !j->failed;
    pthread_mutex_unlock(&j->mu);

    if (ok && (fchmod(j->out_fd, 0644) != 0 || fsync(j->out_fd) != 0)) {
        job_fail(j, "fsync: %s", strerror(errno));
        ok = 0;
    }
    if (close(j->out_fd) != 0 && ok) {
        job_fail(j, "close: %s", strerror(errno));
        ok = 0;
    }
    j->out_fd = -1;
    close(j->in_fd);
    j->in_fd = -1;

    if (ok && rename(j->tmp_path, j->out_path) != 0) {
        job_fail(j, "rename: %s", strerror(errno));
        ok = 0;
    }
    if (!ok)
        unlink(j->tmp_path);

    pthread_mutex_lock(&j->mu);
    j->state = ok ? HUFF_DONE : (j->cancelled ? HUFF_CANCELLED : HUFF_FAILED);
    if (ok)
        j->done = j->total;
    pthread_mutex_unlock(&j->mu);
    return NULL;
}

/* ------------------------------------------------------------------- API */

HuffJob *huff_start(HuffMode mode, const char *in_path, const char *out_path,
                    int nworkers, unsigned block_size)
{
    pthread_once(&crc_once, crc_init);
    HuffJob *j = calloc(1, sizeof *j);
    if (!j)
        return NULL;
    j->mode = mode;
    j->in_fd = j->out_fd = -1;
    j->block_size = block_size ? block_size : DEFAULT_BLOCK;
    if (j->block_size > MAX_BLOCK) {
        errno = EINVAL;
        goto fail;
    }
    if (nworkers <= 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        nworkers = n < 2 ? 2 : (n > 8 ? 8 : (int)n);
    }
    j->nworkers = nworkers;
    j->in_path = strdup(in_path);
    j->out_path = strdup(out_path);
    size_t tl = strlen(out_path) + 8;
    j->tmp_path = malloc(tl);
    if (!j->in_path || !j->out_path || !j->tmp_path) {
        errno = ENOMEM;
        goto fail;
    }
    snprintf(j->tmp_path, tl, "%s.XXXXXX", out_path);

    j->in_fd = open(in_path, O_RDONLY | O_CLOEXEC);
    if (j->in_fd < 0)
        goto fail;
    struct stat st;
    if (fstat(j->in_fd, &st) != 0)
        goto fail;
    if (!S_ISREG(st.st_mode)) {
        errno = EINVAL;
        goto fail;
    }
    j->in_dev = st.st_dev;
    j->in_ino = st.st_ino;
    j->out_fd = mkstemp(j->tmp_path);
    if (j->out_fd < 0)
        goto fail;

    pthread_mutex_init(&j->mu, NULL);
    pthread_cond_init(&j->cv_space, NULL);
    pthread_cond_init(&j->cv_ready, NULL);
    j->state = HUFF_RUNNING;
    j->total = 1;

    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    int rc = pthread_create(&j->controller, NULL, controller_main, j);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (rc != 0) {
        errno = rc;
        unlink(j->tmp_path);
        pthread_mutex_destroy(&j->mu);
        pthread_cond_destroy(&j->cv_space);
        pthread_cond_destroy(&j->cv_ready);
        goto fail;
    }
    return j;

fail:;
    int e = errno;
    if (j->in_fd >= 0)
        close(j->in_fd);
    if (j->out_fd >= 0)
        close(j->out_fd);
    free(j->in_path);
    free(j->out_path);
    free(j->tmp_path);
    free(j);
    errno = e;
    return NULL;
}

HuffState huff_poll(HuffJob *j, int *percent)
{
    pthread_mutex_lock(&j->mu);
    HuffState s = j->state;
    if (percent)
        *percent = j->total ? (int)(j->done * 100 / j->total) : 100;
    pthread_mutex_unlock(&j->mu);
    return s;
}

void huff_cancel(HuffJob *j)
{
    pthread_mutex_lock(&j->mu);
    if (!j->failed)
        j->cancelled = 1;
    j->stop = 1;
    pthread_cond_broadcast(&j->cv_space);
    pthread_cond_broadcast(&j->cv_ready);
    pthread_mutex_unlock(&j->mu);
}

HuffState huff_finish(HuffJob *j, char *errmsg, int errlen)
{
    pthread_join(j->controller, NULL);
    HuffState s = j->state;
    if (errmsg && errlen > 0)
        snprintf(errmsg, (size_t)errlen, "%s", s == HUFF_FAILED ? j->errmsg : "");
    pthread_mutex_destroy(&j->mu);
    pthread_cond_destroy(&j->cv_space);
    pthread_cond_destroy(&j->cv_ready);
    free(j->in_path);
    free(j->out_path);
    free(j->tmp_path);
    free(j->slots);
    free(j->info);
    free(j);
    return s;
}

int huff_path_busy(HuffJob *j, const char *path)
{
    if (!j || !path)
        return 0;
    pthread_mutex_lock(&j->mu);
    int running = j->state == HUFF_RUNNING;
    pthread_mutex_unlock(&j->mu);
    if (!running)
        return 0;
    if (strcmp(path, j->in_path) == 0 || strcmp(path, j->out_path) == 0)
        return 1;
    struct stat a, b;
    if (stat(path, &a) != 0)
        return 0;
    if (a.st_dev == j->in_dev && a.st_ino == j->in_ino)
        return 1;
    return stat(j->out_path, &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}
