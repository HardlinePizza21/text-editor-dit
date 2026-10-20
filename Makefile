CC = gcc
CFLAGS = -Wall -Wextra -std=gnu11 -g -D_GNU_SOURCE -pthread
ARCHSALIDA = eafitOS
SRCS = main.c cat_datos.c cat_memoria.c cat_monitoreo.c cat_util.c textEditor.c huffman_mt.c threadpool.c
OBJS = $(SRCS:.c=.o)
HDRS = shell.h textEditor.h huffman_mt.h threadpool.h

all: $(ARCHSALIDA)

$(ARCHSALIDA): $(OBJS)
	$(CC) $(CFLAGS) -o $(ARCHSALIDA) $(OBJS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c $< -o $@

run: $(ARCHSALIDA)
	./$(ARCHSALIDA)

# Pruebas del módulo Huffman con ThreadSanitizer y AddressSanitizer/UBSan
test:
	$(CC) $(CFLAGS) -fsanitize=thread -o test_huffman_tsan huffman_mt.c threadpool.c test_huffman.c
	./test_huffman_tsan
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o test_huffman_asan huffman_mt.c threadpool.c test_huffman.c
	./test_huffman_asan
	rm -f test_huffman_tsan test_huffman_asan

clean:
	rm -f $(ARCHSALIDA) $(OBJS) test_huffman_tsan test_huffman_asan

.PHONY: all run test clean
