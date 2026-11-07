/* huff_tool: CLI de prueba del módulo Huffman.  Uso: huff_tool c|d <entrada> <salida> [hilos] [bloque] */
#include "huffman.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char **argv)
{
    if (argc < 4 || (argv[1][0] != 'c' && argv[1][0] != 'd')) {
        fprintf(stderr, "uso: %s c|d <entrada> <salida> [hilos] [bloque]\n", argv[0]);
        return 2;
    }
    HuffMode m = argv[1][0] == 'c' ? HUFF_COMPRESS : HUFF_DECOMPRESS;
    HuffJob *j = huff_start(m, argv[2], argv[3], argc > 4 ? atoi(argv[4]) : 0,
                            argc > 5 ? (unsigned)atoi(argv[5]) : 0);
    if (!j) {
        perror("huff_start");
        return 1;
    }
    int pct = 0, last = -1;
    struct timespec ts = {0, 50 * 1000 * 1000};
    while (huff_poll(j, &pct) == HUFF_RUNNING) {
        if (pct != last) {
            fprintf(stderr, "\r%3d%%", pct);
            last = pct;
        }
        nanosleep(&ts, NULL);
    }
    char err[160];
    HuffState s = huff_finish(j, err, sizeof err);
    fprintf(stderr, "\r%s %s\n", s == HUFF_DONE ? "OK" : "FALLO", err);
    return s == HUFF_DONE ? 0 : 1;
}
