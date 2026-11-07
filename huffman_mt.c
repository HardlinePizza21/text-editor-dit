/*
 * Compresión/descompresión de Huffman concurrente.
 * Ver HUFFMAN_THREADPOOL.md para el diseño completo.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "huffman_mt.h"
#include "threadpool.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHUNK_SIZE (256u * 1024u)
#define MAX_CHUNK_SIZE (16u * 1024u * 1024u)
#define MAX_CODE_LEN 15
#define FORMAT_VERSION 1
#define HEADER_SIZE (4 + 1 + 4 + 8 + 4 + 256)
#define RECORD_HDR 12
#define MAX_THREADS 32

static const char MAGIC[4] = {'D', 'H', 'U', 'F'};

/* ---------------------------------------------------------------- utils */

static uint32_t crc_table[256];
static pthread_once_t crc_once = PTHREAD_ONCE_INIT;

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++)
    {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static uint32_t crc32_buf(const uint8_t *p, size_t n)
{
    pthread_once(&crc_once, crc_init);
    uint32_t c = 0xFFFFFFFFu;
    while (n--)
        c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}
static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}
static uint32_t get32(const uint8_t *p)
{
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}
static uint64_t get64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static int pread_all(int fd, void *buf, size_t len, uint64_t off)
{
    uint8_t *p = buf;
    while (len)
    {
        ssize_t n = pread(fd, p, len, (off_t)off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += n;
        off += n;
        len -= n;
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len)
    {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += n;
        len -= n;
    }
    return 0;
}

void huffctl_init(HuffCtl *ctl)
{
    atomic_init(&ctl->cancel, 0);
    atomic_init(&ctl->phase, HUFF_PHASE_IDLE);
    atomic_init(&ctl->done, 0);
    atomic_init(&ctl->total, 0);
    ctl->err[0] = '\0';
}

static int default_threads(int n)
{
    if (n <= 0)
    {
        long c = sysconf(_SC_NPROCESSORS_ONLN);
        n = c > 0 ? (int)c : 2;
    }
    return n > MAX_THREADS ? MAX_THREADS : n;
}

/* ------------------------------------------------------- tabla de códigos */

/* Longitudes de Huffman (<= MAX_CODE_LEN). Si se excede, se reescalan las
 * frecuencias y se reconstruye (técnica usada por bzip2). Determinista. */
static void build_lengths(const uint64_t freq_in[256], uint8_t len[256])
{
    uint64_t f[256];
    memcpy(f, freq_in, sizeof(f));
    memset(len, 0, 256);

    int nsym = 0, only = 0;
    for (int i = 0; i < 256; i++)
        if (f[i])
        {
            nsym++;
            only = i;
        }
    if (nsym == 0)
        return;
    if (nsym == 1)
    {
        len[only] = 1;
        return;
    }

    for (;;)
    {
        uint64_t w[512];
        int parent[512];
        int alive[512], nalive = 0, nnodes = 0;
        int leaf_of[256];

        for (int i = 0; i < 256; i++)
            if (f[i])
            {
                w[nnodes] = f[i];
                parent[nnodes] = -1;
                leaf_of[i] = nnodes;
                alive[nalive++] = nnodes++;
            }
            else
                leaf_of[i] = -1;

        while (nalive > 1)
        {
            int a = 0, b = -1;
            for (int i = 1; i < nalive; i++)
                if (w[alive[i]] < w[alive[a]])
                    a = i;
            for (int i = 0; i < nalive; i++)
                if (i != a && (b < 0 || w[alive[i]] < w[alive[b]]))
                    b = i;
            int na = alive[a], nb = alive[b];
            w[nnodes] = w[na] + w[nb];
            parent[nnodes] = -1;
            parent[na] = parent[nb] = nnodes;
            int hi = a > b ? a : b, lo = a > b ? b : a;
            alive[hi] = alive[--nalive];
            alive[lo] = nnodes++;
        }

        int maxlen = 0;
        for (int s = 0; s < 256; s++)
            if (leaf_of[s] >= 0)
            {
                int d = 0;
                for (int n = leaf_of[s]; parent[n] >= 0; n = parent[n])
                    d++;
                if (d > maxlen)
                    maxlen = d;
                len[s] = (uint8_t)(d > 255 ? 255 : d);
            }
        if (maxlen <= MAX_CODE_LEN)
            return;
        for (int i = 0; i < 256; i++)
            if (f[i])
                f[i] = (f[i] + 1) / 2;
    }
}

/* Códigos canónicos: ordenados por (longitud, símbolo). */
static void assign_codes(const uint8_t len[256], uint16_t code[256])
{
    int count[MAX_CODE_LEN + 2] = {0};
    uint32_t next[MAX_CODE_LEN + 2] = {0};
    for (int i = 0; i < 256; i++)
        count[len[i]]++;
    count[0] = 0;
    uint32_t c = 0;
    for (int l = 1; l <= MAX_CODE_LEN; l++)
    {
        c = (c + count[l - 1]) << 1;
        next[l] = c;
    }
    for (int i = 0; i < 256; i++)
        code[i] = len[i] ? (uint16_t)next[len[i]]++ : 0;
}

/* ------------------------------------------------- etapa de salida ordenada */

typedef struct
{
    uint8_t hdr[RECORD_HDR];
    size_t hdrlen;
    uint8_t *data;
    size_t len;
    int ready;
} Slot;

typedef struct
{
    HuffCtl *ctl;
    pthread_mutex_t m;
    pthread_cond_t ready_cv; /* el escritor espera un slot listo */
    pthread_cond_t space_cv; /* los workers esperan lugar en la ventana */
    atomic_int abort;
    Slot *slots;
    size_t n, next, window;
    int fd;           /* entrada */
    uint64_t size;    /* tamaño de la entrada (compresión) */
    uint32_t chunk;
    pthread_mutex_t hm; /* protege hist (reducción) */
    uint64_t hist[256];
    uint8_t len[256];
    uint16_t code[256];
    uint16_t *table;     /* tabla de decodificación */
    uint64_t *offsets;   /* offsets de registros (descompresión) */
    uint64_t orig_size;
} Run;

typedef struct
{
    Run *r;
    size_t i;
} Arg;

static void run_fail(Run *r, const char *msg)
{
    pthread_mutex_lock(&r->m);
    if (msg && !r->ctl->err[0])
        snprintf(r->ctl->err, sizeof(r->ctl->err), "%s", msg);
    atomic_store(&r->abort, 1);
    pthread_cond_broadcast(&r->ready_cv);
    pthread_cond_broadcast(&r->space_cv);
    pthread_mutex_unlock(&r->m);
}

static int run_aborted(Run *r)
{
    if (atomic_load(&r->ctl->cancel))
        run_fail(r, NULL);
    return atomic_load(&r->abort);
}

static int run_init(Run *r, HuffCtl *ctl, size_t nslots, int nthreads)
{
    memset(r, 0, sizeof(*r));
    r->ctl = ctl;
    r->fd = -1;
    pthread_mutex_init(&r->m, NULL);
    pthread_mutex_init(&r->hm, NULL);
    pthread_cond_init(&r->ready_cv, NULL);
    pthread_cond_init(&r->space_cv, NULL);
    atomic_init(&r->abort, 0);
    r->n = nslots;
    r->window = (size_t)nthreads * 2 + 2;
    if (nslots)
    {
        r->slots = calloc(nslots, sizeof(Slot));
        if (!r->slots)
            return -1;
    }
    return 0;
}

static void run_destroy(Run *r)
{
    for (size_t i = 0; i < r->n && r->slots; i++)
        free(r->slots[i].data);
    free(r->slots);
    free(r->table);
    free(r->offsets);
    if (r->fd >= 0)
        close(r->fd);
    pthread_mutex_destroy(&r->m);
    pthread_mutex_destroy(&r->hm);
    pthread_cond_destroy(&r->ready_cv);
    pthread_cond_destroy(&r->space_cv);
}

/* Un worker publica su resultado, respetando la ventana de reordenamiento.
 * Retorna 0 si debe continuar, -1 si se abortó. */
static int wait_window(Run *r, size_t i)
{
    pthread_mutex_lock(&r->m);
    while (i >= r->next + r->window && !atomic_load(&r->abort))
        pthread_cond_wait(&r->space_cv, &r->m);
    int a = atomic_load(&r->abort);
    pthread_mutex_unlock(&r->m);
    return a ? -1 : 0;
}

static void publish(Run *r, size_t i)
{
    pthread_mutex_lock(&r->m);
    r->slots[i].ready = 1;
    pthread_cond_broadcast(&r->ready_cv);
    pthread_mutex_unlock(&r->m);
}

/* Coordinador: escribe los slots en estricto orden 0..n-1. Si count_done es
 * distinto de cero, el progreso se actualiza por cada slot escrito. */
static int ordered_write(Run *r, int out_fd, int count_done)
{
    for (size_t i = 0; i < r->n; i++)
    {
        pthread_mutex_lock(&r->m);
        while (!r->slots[i].ready && !atomic_load(&r->abort))
            pthread_cond_wait(&r->ready_cv, &r->m);
        int a = atomic_load(&r->abort);
        pthread_mutex_unlock(&r->m);
        if (a)
            return -1;

        Slot *s = &r->slots[i];
        if ((s->hdrlen && write_all(out_fd, s->hdr, s->hdrlen) < 0) ||
            (s->len && write_all(out_fd, s->data, s->len) < 0))
        {
            run_fail(r, strerror(errno));
            return -1;
        }
        if (count_done)
            atomic_fetch_add(&r->ctl->done, (i + 1 < r->n || r->orig_size % r->chunk == 0)
                                                ? r->chunk
                                                : r->orig_size % r->chunk);
        free(s->data);
        s->data = NULL;

        pthread_mutex_lock(&r->m);
        r->next = i + 1;
        pthread_cond_broadcast(&r->space_cv);
        pthread_mutex_unlock(&r->m);

        if (run_aborted(r))
            return -1;
    }
    return 0;
}

/* ------------------------------------------------------------ compresión */

static size_t chunk_len(const Run *r, size_t i)
{
    uint64_t off = (uint64_t)i * r->chunk;
    uint64_t rem = r->size - off;
    return rem < r->chunk ? (size_t)rem : r->chunk;
}

static void count_task(void *p)
{
    Arg *a = p;
    Run *r = a->r;
    if (run_aborted(r))
        return;
    size_t n = chunk_len(r, a->i);
    uint8_t *buf = malloc(n);
    if (!buf)
        return run_fail(r, "sin memoria");
    if (pread_all(r->fd, buf, n, (uint64_t)a->i * r->chunk) < 0)
    {
        free(buf);
        return run_fail(r, "error leyendo la entrada (¿cambió el archivo?)");
    }
    uint64_t local[256] = {0};
    for (size_t k = 0; k < n; k++)
        local[buf[k]]++;
    free(buf);

    pthread_mutex_lock(&r->hm);
    for (int k = 0; k < 256; k++)
        r->hist[k] += local[k];
    pthread_mutex_unlock(&r->hm);
    atomic_fetch_add(&r->ctl->done, n);
}

static void encode_task(void *p)
{
    Arg *a = p;
    Run *r = a->r;
    size_t i = a->i;
    if (run_aborted(r) || wait_window(r, i) < 0)
        return;

    size_t n = chunk_len(r, i);
    uint8_t *in = malloc(n);
    uint8_t *out = malloc(n * 2 + 8);
    if (!in || !out)
    {
        free(in);
        free(out);
        return run_fail(r, "sin memoria");
    }
    if (pread_all(r->fd, in, n, (uint64_t)i * r->chunk) < 0)
    {
        free(in);
        free(out);
        return run_fail(r, "error leyendo la entrada (¿cambió el archivo?)");
    }

    uint64_t acc = 0;
    int nb = 0;
    size_t o = 0;
    for (size_t k = 0; k < n; k++)
    {
        int l = r->len[in[k]];
        acc = (acc << l) | r->code[in[k]];
        nb += l;
        while (nb >= 8)
        {
            out[o++] = (uint8_t)(acc >> (nb - 8));
            nb -= 8;
        }
    }
    if (nb)
        out[o++] = (uint8_t)(acc << (8 - nb));

    Slot *s = &r->slots[i];
    put32(s->hdr, (uint32_t)n);
    put32(s->hdr + 4, (uint32_t)o);
    put32(s->hdr + 8, crc32_buf(in, n));
    s->hdrlen = RECORD_HDR;
    s->data = out;
    s->len = o;
    free(in);
    atomic_fetch_add(&r->ctl->done, n);
    publish(r, i);
}

static int same_file(const char *a, const char *b)
{
    struct stat sa, sb;
    return stat(a, &sa) == 0 && stat(b, &sb) == 0 &&
           sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

static int open_tmp_output(const char *out, char *tmp, size_t tmpsz, HuffCtl *ctl)
{
    if (snprintf(tmp, tmpsz, "%s.XXXXXX", out) >= (int)tmpsz)
    {
        snprintf(ctl->err, sizeof(ctl->err), "ruta de salida demasiado larga");
        return -1;
    }
    int fd = mkstemp(tmp);
    if (fd < 0)
        snprintf(ctl->err, sizeof(ctl->err), "no se puede crear salida: %s", strerror(errno));
    return fd;
}

static int finish_output(Run *r, int ofd, const char *tmp, const char *out, mode_t mode, int rc)
{
    if (rc == 0 && (fsync(ofd) < 0))
    {
        snprintf(r->ctl->err, sizeof(r->ctl->err), "fsync: %s", strerror(errno));
        rc = -1;
    }
    if (rc == 0)
        fchmod(ofd, mode & 0777);
    if (close(ofd) < 0 && rc == 0)
    {
        snprintf(r->ctl->err, sizeof(r->ctl->err), "close: %s", strerror(errno));
        rc = -1;
    }
    if (rc == 0 && rename(tmp, out) < 0)
    {
        snprintf(r->ctl->err, sizeof(r->ctl->err), "rename: %s", strerror(errno));
        rc = -1;
    }
    if (rc != 0)
        unlink(tmp);
    return rc;
}

static int final_rc(Run *r, int rc)
{
    if (r->ctl->err[0])
        return -1;
    if (atomic_load(&r->ctl->cancel))
        return -2;
    return rc ? -1 : 0;
}

int huff_compress_file(const char *in, const char *out, int nthreads, HuffCtl *ctl)
{
    HuffCtl local;
    if (!ctl)
    {
        huffctl_init(&local);
        ctl = &local;
    }
    ctl->err[0] = '\0';
    nthreads = default_threads(nthreads);

    int fd = open(in, O_RDONLY);
    if (fd < 0)
    {
        snprintf(ctl->err, sizeof(ctl->err), "no se puede abrir %s: %s", in, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode))
    {
        snprintf(ctl->err, sizeof(ctl->err), "%s no es un archivo regular", in);
        close(fd);
        return -1;
    }
    if (same_file(in, out))
    {
        snprintf(ctl->err, sizeof(ctl->err), "entrada y salida son el mismo archivo");
        close(fd);
        return -1;
    }

    uint64_t size = (uint64_t)st.st_size;
    size_t nchunks = (size + CHUNK_SIZE - 1) / CHUNK_SIZE;
    if (nchunks > UINT32_MAX)
    {
        snprintf(ctl->err, sizeof(ctl->err), "archivo demasiado grande");
        close(fd);
        return -1;
    }

    Run r;
    if (run_init(&r, ctl, nchunks, nthreads) < 0)
    {
        snprintf(ctl->err, sizeof(ctl->err), "sin memoria");
        run_destroy(&r);
        close(fd);
        return -1;
    }
    r.fd = fd;
    r.size = size;
    r.orig_size = size;
    r.chunk = CHUNK_SIZE;
    atomic_store(&ctl->done, 0);
    atomic_store(&ctl->total, size * 2);

    ThreadPool *tp = tp_create(nthreads);
    Arg *args = calloc(nchunks ? nchunks : 1, sizeof(Arg));
    int ofd = -1, rc = -1;
    char tmp[PATH_MAX + 16];
    if (!tp || !args)
    {
        snprintf(ctl->err, sizeof(ctl->err), "no se pudo crear el pool de hilos");
        goto done;
    }
    for (size_t i = 0; i < nchunks; i++)
    {
        args[i].r = &r;
        args[i].i = i;
    }

    /* Fase 1: conteo de frecuencias en paralelo */
    atomic_store(&ctl->phase, HUFF_PHASE_COUNT);
    for (size_t i = 0; i < nchunks; i++)
        if (tp_submit(tp, count_task, &args[i]) < 0)
        {
            run_fail(&r, "no se pudo encolar tarea");
            break;
        }
    tp_wait(tp);
    if (run_aborted(&r))
        goto done;

    /* Fase 2: árbol global y códigos canónicos */
    atomic_store(&ctl->phase, HUFF_PHASE_TREE);
    build_lengths(r.hist, r.len);
    assign_codes(r.len, r.code);

    ofd = open_tmp_output(out, tmp, sizeof(tmp), ctl);
    if (ofd < 0)
        goto done;

    uint8_t hdr[HEADER_SIZE];
    memcpy(hdr, MAGIC, 4);
    hdr[4] = FORMAT_VERSION;
    put32(hdr + 5, CHUNK_SIZE);
    put64(hdr + 9, size);
    put32(hdr + 17, (uint32_t)nchunks);
    memcpy(hdr + 21, r.len, 256);
    if (write_all(ofd, hdr, sizeof(hdr)) < 0)
    {
        snprintf(ctl->err, sizeof(ctl->err), "error escribiendo: %s", strerror(errno));
        goto done;
    }

    /* Fase 3: codificación paralela; este hilo escribe en orden */
    atomic_store(&ctl->phase, HUFF_PHASE_ENCODE);
    for (size_t i = 0; i < nchunks; i++)
        if (tp_submit(tp, encode_task, &args[i]) < 0)
        {
            run_fail(&r, "no se pudo encolar tarea");
            break;
        }
    rc = ordered_write(&r, ofd, 0);
    run_fail(&r, NULL); /* libera a cualquier worker bloqueado */
    tp_wait(tp);
    if (rc == 0 && atomic_load(&ctl->cancel))
        rc = -1;

done:
    if (tp)
    {
        run_fail(&r, NULL);
        tp_wait(tp);
        tp_destroy(tp);
    }
    if (ofd >= 0)
        rc = finish_output(&r, ofd, tmp, out, st.st_mode, rc);
    if (rc == 0)
        atomic_store(&ctl->done, size * 2);
    rc = final_rc(&r, rc);
    free(args);
    run_destroy(&r);
    atomic_store(&ctl->phase, HUFF_PHASE_IDLE);
    return rc;
}

/* ------------------------------------------------------- descompresión */

static void decode_task(void *p)
{
    Arg *a = p;
    Run *r = a->r;
    size_t i = a->i;
    if (run_aborted(r) || wait_window(r, i) < 0)
        return;

    uint8_t rh[RECORD_HDR];
    uint64_t off = r->offsets[i];
    if (pread_all(r->fd, rh, RECORD_HDR, off) < 0)
        return run_fail(r, "archivo comprimido truncado");
    uint32_t olen = get32(rh), nbytes = get32(rh + 4), crc = get32(rh + 8);

    uint8_t *in = malloc(nbytes ? nbytes : 1);
    uint8_t *out = malloc(olen ? olen : 1);
    if (!in || !out)
    {
        free(in);
        free(out);
        return run_fail(r, "sin memoria");
    }
    if (pread_all(r->fd, in, nbytes, off + RECORD_HDR) < 0)
    {
        free(in);
        free(out);
        return run_fail(r, "archivo comprimido truncado");
    }

    uint64_t acc = 0;
    int nb = 0, bad = 0;
    size_t pos = 0;
    for (uint32_t j = 0; j < olen; j++)
    {
        while (nb < MAX_CODE_LEN && pos < nbytes)
        {
            acc = (acc << 8) | in[pos++];
            nb += 8;
        }
        uint32_t v = nb >= MAX_CODE_LEN ? (uint32_t)(acc >> (nb - MAX_CODE_LEN))
                                        : (uint32_t)(acc << (MAX_CODE_LEN - nb));
        uint16_t e = r->table[v & 0x7FFF];
        int l = e & 15;
        if (l == 0 || l > nb)
        {
            bad = 1;
            break;
        }
        out[j] = (uint8_t)(e >> 4);
        nb -= l;
        acc &= ((uint64_t)1 << nb) - 1;
    }
    free(in);
    if (bad || crc32_buf(out, olen) != crc)
    {
        free(out);
        return run_fail(r, "datos corruptos (CRC o código inválido)");
    }

    Slot *s = &r->slots[i];
    s->hdrlen = 0;
    s->data = out;
    s->len = olen;
    publish(r, i);
}

static int build_table(Run *r)
{
    uint32_t kraft = 0;
    for (int i = 0; i < 256; i++)
    {
        if (r->len[i] > MAX_CODE_LEN)
            return -1;
        if (r->len[i])
            kraft += 1u << (MAX_CODE_LEN - r->len[i]);
    }
    if (kraft > (1u << MAX_CODE_LEN))
        return -1;
    assign_codes(r->len, r->code);
    r->table = calloc(1u << MAX_CODE_LEN, sizeof(uint16_t));
    if (!r->table)
        return -1;
    for (int s = 0; s < 256; s++)
    {
        int l = r->len[s];
        if (!l)
            continue;
        uint32_t base = (uint32_t)r->code[s] << (MAX_CODE_LEN - l);
        for (uint32_t k = 0; k < (1u << (MAX_CODE_LEN - l)); k++)
            r->table[base + k] = (uint16_t)((s << 4) | l);
    }
    return 0;
}

int huff_decompress_file(const char *in, const char *out, int nthreads, HuffCtl *ctl)
{
    HuffCtl local;
    if (!ctl)
    {
        huffctl_init(&local);
        ctl = &local;
    }
    ctl->err[0] = '\0';
    nthreads = default_threads(nthreads);

    int fd = open(in, O_RDONLY);
    if (fd < 0)
    {
        snprintf(ctl->err, sizeof(ctl->err), "no se puede abrir %s: %s", in, strerror(errno));
        return -1;
    }
    struct stat st;
    uint8_t hdr[HEADER_SIZE];
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
        pread_all(fd, hdr, sizeof(hdr), 0) < 0 || memcmp(hdr, MAGIC, 4) != 0 ||
        hdr[4] != FORMAT_VERSION)
    {
        snprintf(ctl->err, sizeof(ctl->err), "%s no es un archivo .huf válido", in);
        close(fd);
        return -1;
    }
    if (same_file(in, out))
    {
        snprintf(ctl->err, sizeof(ctl->err), "entrada y salida son el mismo archivo");
        close(fd);
        return -1;
    }

    uint32_t chunk = get32(hdr + 5);
    uint64_t orig = get64(hdr + 9);
    uint32_t nchunks = get32(hdr + 17);
    uint64_t fsize = (uint64_t)st.st_size;
    if (chunk == 0 || chunk > MAX_CHUNK_SIZE ||
        (orig + chunk - 1) / chunk != nchunks || (uint64_t)nchunks * RECORD_HDR > fsize)
    {
        snprintf(ctl->err, sizeof(ctl->err), "cabecera corrupta");
        close(fd);
        return -1;
    }

    Run r;
    int ofd = -1, rc = -1;
    ThreadPool *tp = NULL;
    Arg *args = NULL;
    char tmp[PATH_MAX + 16];
    if (run_init(&r, ctl, nchunks, nthreads) < 0)
    {
        snprintf(ctl->err, sizeof(ctl->err), "sin memoria");
        run_destroy(&r);
        close(fd);
        return -1;
    }
    r.fd = fd;
    r.chunk = chunk;
    r.orig_size = orig;
    memcpy(r.len, hdr + 21, 256);
    atomic_store(&ctl->done, 0);
    atomic_store(&ctl->total, orig);
    atomic_store(&ctl->phase, HUFF_PHASE_TREE);

    if (build_table(&r) < 0)
    {
        snprintf(ctl->err, sizeof(ctl->err), "tabla de códigos inválida");
        goto done;
    }

    /* Indexado secuencial de registros (solo cabeceras, saltando datos) */
    r.offsets = calloc(nchunks ? nchunks : 1, sizeof(uint64_t));
    args = calloc(nchunks ? nchunks : 1, sizeof(Arg));
    if (!r.offsets || !args)
    {
        snprintf(ctl->err, sizeof(ctl->err), "sin memoria");
        goto done;
    }
    uint64_t off = HEADER_SIZE;
    for (uint32_t i = 0; i < nchunks; i++)
    {
        uint8_t rh[RECORD_HDR];
        uint64_t expect = (i + 1 < nchunks) ? chunk : orig - (uint64_t)i * chunk;
        if (off + RECORD_HDR > fsize || pread_all(fd, rh, RECORD_HDR, off) < 0 ||
            get32(rh) != expect || off + RECORD_HDR + get32(rh + 4) > fsize ||
            get32(rh + 4) > expect * 2 + 8)
        {
            snprintf(ctl->err, sizeof(ctl->err), "archivo comprimido corrupto o truncado");
            goto done;
        }
        r.offsets[i] = off;
        args[i].r = &r;
        args[i].i = i;
        off += RECORD_HDR + get32(rh + 4);
    }
    if (off != fsize)
    {
        snprintf(ctl->err, sizeof(ctl->err), "datos sobrantes al final del archivo");
        goto done;
    }

    ofd = open_tmp_output(out, tmp, sizeof(tmp), ctl);
    if (ofd < 0)
        goto done;
    tp = tp_create(nthreads);
    if (!tp)
    {
        snprintf(ctl->err, sizeof(ctl->err), "no se pudo crear el pool de hilos");
        goto done;
    }

    atomic_store(&ctl->phase, HUFF_PHASE_DECODE);
    for (uint32_t i = 0; i < nchunks; i++)
        if (tp_submit(tp, decode_task, &args[i]) < 0)
        {
            run_fail(&r, "no se pudo encolar tarea");
            break;
        }
    rc = ordered_write(&r, ofd, 1);
    run_fail(&r, NULL);
    tp_wait(tp);
    if (rc == 0 && atomic_load(&ctl->cancel))
        rc = -1;

done:
    if (tp)
    {
        run_fail(&r, NULL);
        tp_wait(tp);
        tp_destroy(tp);
    }
    if (ofd >= 0)
        rc = finish_output(&r, ofd, tmp, out, st.st_mode, rc);
    if (rc == 0)
        atomic_store(&ctl->done, orig);
    rc = final_rc(&r, rc);
    free(args);
    run_destroy(&r);
    atomic_store(&ctl->phase, HUFF_PHASE_IDLE);
    return rc;
}

/* ------------------------------------------------------ trabajo asíncrono */

struct HuffJob
{
    pthread_t th;
    HuffCtl ctl;
    HuffMode mode;
    char in[PATH_MAX], out[PATH_MAX];
    int nthreads;
    atomic_int state;
};

static void *job_main(void *p)
{
    HuffJob *j = p;
    int rc = j->mode == HUFF_COMPRESS
                 ? huff_compress_file(j->in, j->out, j->nthreads, &j->ctl)
                 : huff_decompress_file(j->in, j->out, j->nthreads, &j->ctl);
    atomic_store(&j->state, rc == 0 ? JOB_OK : rc == -2 ? JOB_CANCELLED : JOB_FAILED);
    return NULL;
}

HuffJob *huffjob_start(HuffMode mode, const char *in, const char *out, int nthreads)
{
    HuffJob *j = calloc(1, sizeof(*j));
    if (!j)
        return NULL;
    huffctl_init(&j->ctl);
    j->mode = mode;
    j->nthreads = nthreads;
    snprintf(j->in, sizeof(j->in), "%s", in);
    snprintf(j->out, sizeof(j->out), "%s", out);
    atomic_init(&j->state, JOB_RUNNING);
    if (pthread_create(&j->th, NULL, job_main, j) != 0)
    {
        free(j);
        return NULL;
    }
    return j;
}

JobState huffjob_state(HuffJob *j) { return atomic_load(&j->state); }

int huffjob_percent(HuffJob *j)
{
    unsigned long long t = atomic_load(&j->ctl.total), d = atomic_load(&j->ctl.done);
    if (t == 0)
        return huffjob_state(j) == JOB_RUNNING ? 0 : 100;
    if (d > t)
        d = t;
    return (int)(d * 100 / t);
}

const char *huffjob_phase_name(HuffJob *j)
{
    switch (atomic_load(&j->ctl.phase))
    {
    case HUFF_PHASE_COUNT:
        return "contando frecuencias";
    case HUFF_PHASE_TREE:
        return "preparando";
    case HUFF_PHASE_ENCODE:
        return "codificando";
    case HUFF_PHASE_DECODE:
        return "decodificando";
    default:
        return "iniciando";
    }
}

HuffMode huffjob_mode(HuffJob *j) { return j->mode; }
const char *huffjob_input(HuffJob *j) { return j->in; }
const char *huffjob_output(HuffJob *j) { return j->out; }
void huffjob_cancel(HuffJob *j) { atomic_store(&j->ctl.cancel, 1); }

JobState huffjob_finish(HuffJob *j, char *err, size_t errlen)
{
    pthread_join(j->th, NULL);
    JobState s = atomic_load(&j->state);
    if (err && errlen)
        snprintf(err, errlen, "%s", j->ctl.err);
    free(j);
    return s;
}
