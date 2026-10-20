/* Pruebas del módulo Huffman concurrente: make test */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "huffman_mt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int failures;
#define CHECK(c, ...)                                  \
    do                                                 \
    {                                                  \
        if (!(c))                                      \
        {                                              \
            failures++;                                \
            printf("  FALLO %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                       \
            printf("\n");                              \
        }                                              \
    } while (0)

static char dir[] = "/tmp/huftestXXXXXX";

static void path(char *buf, const char *name) { snprintf(buf, 256, "%s/%s", dir, name); }

static void write_file(const char *p, const unsigned char *d, size_t n)
{
    FILE *f = fopen(p, "wb");
    if (n)
        fwrite(d, 1, n, f);
    fclose(f);
}

static unsigned char *read_file(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f)
    {
        *n = 0;
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(*n + 1);
    if (fread(b, 1, *n, f) != *n)
        *n = 0;
    fclose(f);
    return b;
}

static int roundtrip(const char *label, const unsigned char *data, size_t n, int threads, size_t *csize)
{
    char in[256], c[256], o[256];
    path(in, "in.bin");
    path(c, "in.bin.huf");
    path(o, "out.bin");
    write_file(in, data, n);
    HuffCtl ctl;
    huffctl_init(&ctl);
    int rc = huff_compress_file(in, c, threads, &ctl);
    CHECK(rc == 0, "%s compress rc=%d %s", label, rc, ctl.err);
    if (rc)
        return -1;
    size_t cn;
    unsigned char *cb = read_file(c, &cn);
    free(cb);
    if (csize)
        *csize = cn;
    rc = huff_decompress_file(c, o, threads, &ctl);
    CHECK(rc == 0, "%s decompress rc=%d %s", label, rc, ctl.err);
    if (rc)
        return -1;
    size_t on;
    unsigned char *ob = read_file(o, &on);
    CHECK(on == n && (n == 0 || memcmp(ob, data, n) == 0), "%s contenido distinto (%zu vs %zu)", label, on, n);
    free(ob);
    CHECK(atomic_load(&ctl.done) == atomic_load(&ctl.total), "progreso incompleto");
    return 0;
}

int main(void)
{
    if (!mkdtemp(dir))
        return 2;
    srand(42);
    int tcs[] = {1, 2, 8};
    size_t big = 256 * 1024 * 5 + 12345;
    unsigned char *rnd = malloc(big), *txt = malloc(big), *one = malloc(big), *skew = malloc(big);
    for (size_t i = 0; i < big; i++)
    {
        rnd[i] = rand();
        txt[i] = "etaoin shrdlu\n"[rand() % 14];
        one[i] = 'a';
        skew[i] = (rand() % 1000 == 0) ? rand() : (rand() % 3 ? 0 : 1); /* alfabeto muy sesgado */
    }
    /* Fibonacci: fuerza longitudes > 15 en el árbol */
    unsigned char *fib = malloc(3000000);
    size_t fn = 0;
    unsigned long a = 1, b = 1;
    for (int s = 0; s < 30 && fn + a < 3000000; s++)
    {
        memset(fib + fn, s, a);
        fn += a;
        unsigned long t = a + b;
        a = b;
        b = t;
    }

    for (int t = 0; t < 3; t++)
    {
        printf("hilos=%d\n", tcs[t]);
        roundtrip("vacio", (unsigned char *)"", 0, tcs[t], NULL);
        roundtrip("1 byte", (unsigned char *)"x", 1, tcs[t], NULL);
        roundtrip("un simbolo", one, big, tcs[t], NULL);
        roundtrip("aleatorio", rnd, big, tcs[t], NULL);
        roundtrip("texto", txt, big, tcs[t], NULL);
        roundtrip("sesgado", skew, big, tcs[t], NULL);
        roundtrip("fibonacci", fib, fn, tcs[t], NULL);
        roundtrip("exacto 1 chunk", rnd, 256 * 1024, tcs[t], NULL);
    }

    /* La salida debe ser idéntica sin importar el nº de hilos */
    {
        char in[256], c1[256], c8[256];
        path(in, "det.bin");
        path(c1, "det1.huf");
        path(c8, "det8.huf");
        write_file(in, txt, big);
        huff_compress_file(in, c1, 1, NULL);
        huff_compress_file(in, c8, 8, NULL);
        size_t n1, n8;
        unsigned char *b1 = read_file(c1, &n1), *b8 = read_file(c8, &n8);
        CHECK(n1 == n8 && memcmp(b1, b8, n1) == 0, "salida depende del nº de hilos");
        CHECK(n1 < big, "texto no comprimió");
        free(b1);
        free(b8);

        /* Corrupción: un bit cambiado en datos, truncado, magic */
        char bad[256], o[256];
        path(bad, "bad.huf");
        path(o, "bad.out");
        HuffCtl ctl;
        huffctl_init(&ctl);
        b1 = read_file(c1, &n1);
        b1[n1 / 2] ^= 0x10;
        write_file(bad, b1, n1);
        CHECK(huff_decompress_file(bad, o, 4, &ctl) == -1, "corrupción no detectada");
        CHECK(access(o, F_OK) != 0, "quedó salida parcial");
        write_file(bad, b1, n1 - 7);
        CHECK(huff_decompress_file(bad, o, 4, &ctl) == -1, "truncado no detectado");
        b1[0] = 'X';
        write_file(bad, b1, n1);
        CHECK(huff_decompress_file(bad, o, 4, &ctl) == -1, "magic no validado");
        free(b1);

        CHECK(huff_compress_file(in, in, 2, &ctl) == -1, "entrada==salida aceptada");
    }

    /* Cancelación en segundo plano */
    {
        char in[256], o[256];
        path(in, "cancel.bin");
        path(o, "cancel.huf");
        size_t sz = 64u * 1024 * 1024;
        unsigned char *d = malloc(sz);
        for (size_t i = 0; i < sz; i++)
            d[i] = rand();
        write_file(in, d, sz);
        free(d);
        HuffJob *j = huffjob_start(HUFF_COMPRESS, in, o, 4);
        CHECK(j != NULL, "job no inició");
        usleep(20000);
        huffjob_cancel(j);
        char err[160];
        JobState s = huffjob_finish(j, err, sizeof(err));
        CHECK(s == JOB_CANCELLED || s == JOB_OK, "estado tras cancelar: %d", s);
        if (s == JOB_CANCELLED)
            CHECK(access(o, F_OK) != 0, "quedó salida tras cancelar");

        j = huffjob_start(HUFF_COMPRESS, in, o, 4);
        int last = -1, mono = 1;
        while (huffjob_state(j) == JOB_RUNNING)
        {
            int p = huffjob_percent(j);
            if (p < last)
                mono = 0;
            last = p;
            usleep(1000);
        }
        s = huffjob_finish(j, err, sizeof(err));
        CHECK(s == JOB_OK, "job completo falló: %s", err);
        CHECK(mono, "progreso no monótono");
    }

    free(rnd);
    free(txt);
    free(one);
    free(skew);
    free(fib);
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd))
        return 2;
    printf(failures ? "FALLARON %d comprobaciones\n" : "OK\n", failures);
    return failures != 0;
}
