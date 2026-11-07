# Sustentación – Huffman concurrente integrado al editor (Alternativa 1)

Archivos: [huffman.h](huffman.h), [huffman.c](huffman.c) (módulo), [huff_tool.c](huff_tool.c) (CLI de pruebas),
[textEditor.c](textEditor.c) (integración). C + pthreads, POSIX/Linux, sin librerías de alto nivel de sincronización.

## 1. Visión general

```
 Hilo UI (editor) ── huff_start ──► Coordinador ──► Pool de N workers
      ▲  huff_poll (cada tick)          │ (escritor ordenado)
      └── progreso/estado ◄─────────────┘
```

- **UI**: nunca bloquea. `huff_start` solo crea el hilo coordinador. La UI consulta `huff_poll` en cada *tick*.
- **Coordinador**: orquesta las fases, crea/une los workers y es el **único que escribe** el archivo de salida.
- **Workers** (N = nº de CPUs, 2..8): procesan bloques de 64 KiB.

## 2. Concurrencia en Huffman (25 pts)

1. **Fase 1 – conteo de frecuencias** (`count_worker`): cada worker toma el siguiente índice de bloque de una cola
   protegida por mutex (`next_task`), lee con `pread` (sin compartir offset), cuenta en una **tabla local** `local[256]`
   y al terminar hace la **reducción** sumando a la tabla global bajo mutex. Sin contención por byte.
2. **Construcción del árbol global** (coordinador, determinista: desempate por posición). Se guarda la tabla de
   frecuencias en la cabecera; el descompresor reconstruye el mismo árbol.
3. **Fase 2 – codificación en paralelo** (`block_worker`): cada worker codifica un bloque completo (cabecera de bloque
   + CRC32 + bits MSB-first) y lo deposita en `slots[i]`.
4. **Salida ordenada**: el coordinador espera el bloque `i` (`cv_ready`), lo escribe y pasa al `i+1`. Aunque los
   workers terminen desordenados, el archivo sale en orden secuencial.
5. **Ventana acotada** (`WINDOW = 16`): un worker no toma el bloque `i` si `i >= next_write + WINDOW`
   (espera en `cv_space`). Memoria acotada y sin interbloqueo: el bloque `next_write` siempre está dentro de la ventana.
6. La **descompresión** usa el mismo motor (valida el índice de bloques, decodifica en paralelo, escribe en orden).

Formato: `"HUF1" | tamaño(u64) | tam_bloque(u32) | nbloques(u32) | freq[256](u64)` y por bloque
`orig_len | crc32 | comp_len | datos`.

## 3. Mecanismos de sincronización (25 pts)

| Primitiva | Uso |
|---|---|
| `pthread_mutex_t mu` | Protege `next_task`, `next_write`, `slots[i].ready`, `freq` (reducción), `done/total`, `stop`, `state`. |
| `cv_space` | Workers duermen mientras la ventana está llena; el escritor la despierta (`broadcast`) al avanzar. |
| `cv_ready` | El escritor duerme hasta que el bloque `i` esté listo; el worker hace `signal` tras publicarlo. |
| `pthread_once` | Inicialización única de la tabla CRC32. |

- **Sin espera activa**: todo bloqueo es `pthread_cond_wait` dentro de `while (predicado)` (protege de despertares
  espurios). La UI se despierta por el timeout de `read()` de termios (`VTIME`), no con un bucle ocupado.
- **Sin carreras**: todo estado compartido se toca bajo `mu`; los buffers de bloque son privados de un worker hasta
  publicarlos, y luego solo los toca el escritor. `pread` evita compartir el offset del descriptor.
  Verificado con **ThreadSanitizer** (sin reportes) y AddressSanitizer.
- **Sin interbloqueo**: un solo mutex (no hay orden de bloqueo que violar) y la ventana siempre contiene el bloque
  que el escritor espera. Cancelación/error: `job_fail`/`huff_cancel` ponen `stop=1` y hacen `broadcast` en ambas
  variables de condición, así nadie queda dormido.

## 4. Integración con el editor (20 pts)

- **Teclas**: `Ctrl-E` comprime el archivo abierto → `<archivo>.huf`; `Ctrl-D` pide un `.huf` y lo descomprime
  (quita `.huf`, o añade `.out`); `Ctrl-X` cancela.
- **No congela la UI**: el trabajo corre en hilos; `editorReadKey` devuelve `KEY_TICK` cuando vence el timeout
  de lectura mientras haya un job, lo que refresca la pantalla ~10 veces/s con la barra
  `Comprimiendo [#####---------------] 25%`.
- **Condiciones de carrera editor ↔ archivo**:
  - `Ctrl-S` se **bloquea** si el archivo es entrada o salida del job (`huff_path_busy`: compara ruta e inodo).
  - `Ctrl-E` se rechaza con cambios sin guardar (se comprimiría un estado distinto al del buffer).
  - La descompresión no puede escribir sobre el archivo abierto.
  - La edición del buffer en memoria sí sigue permitida: el job solo usa el archivo en disco, nunca `E.row`.
  - Un solo job a la vez.
  - La salida se escribe a un temporal (`mkstemp`) y se publica con `rename` atómico: nunca queda un archivo parcial.

## 5. Corrección algorítmica (15 pts)

- Cada bloque guarda un **CRC32** del original; al descomprimir se verifica y se aborta si no coincide.
- Se valida cabecera, suma de frecuencias, tamaños e índice de bloques (archivo truncado/corrupto → error limpio).
- Casos probados con `cmp` (idénticos): archivo vacío, 1 byte, un solo símbolo (300 KB de ceros), aleatorio (5 MB),
  texto, y distintos nº de hilos y tamaños de bloque (p. ej. 4 hilos al comprimir, 3 al descomprimir, bloque 7777).
  Un byte alterado produce `datos corruptos: checksum del bloque N`.

```bash
make huff_tool
./huff_tool c archivo archivo.huf 4
./huff_tool d archivo.huf copia 3
cmp archivo copia && md5sum archivo copia
```

## 6. Manejo de errores y recursos (15 pts)

- Todos los `malloc/calloc/open/pread/write/fsync/close/rename` se comprueban; el primer error queda en `errmsg`.
- `pread_full`/`write_full` manejan lecturas/escrituras parciales y `EINTR`.
- Los descriptores se cierran siempre en el coordinador (`in_fd`, `out_fd`, `O_CLOEXEC`); los temporales se `unlink`
  si hay error o cancelación.
- Todos los hilos son **joinable y se unen** (`join_all`, `huff_finish`); el pool se une antes de liberar
  `slots/info`, y los buffers pendientes en slots se liberan.
- **Señales**: los hilos se crean con todas las señales bloqueadas (`pthread_sigmask`), así solo las recibe el hilo
  de UI. Al salir del editor (`Ctrl-Q`) `editorHuffShutdown` cancela y une el job.
- Si falla `pthread_create`, se continúa con los hilos creados (o se reporta error si no hay ninguno).

## 7. Preguntas probables

- *¿Por qué `pread` y no `read`?* Los workers comparten el descriptor; `pread` no usa el offset compartido → sin lock.
- *¿Por qué reducción local en el conteo?* Evita contención en un mutex por cada byte; se sincroniza una vez por worker.
- *¿Cómo se garantiza el orden?* Índice de bloque + `slots[]` + escritor único que espera `slots[i].ready`.
- *¿Y si un bloque lento retrasa a los demás?* La ventana limita los bloques en vuelo; los demás workers esperan en
  `cv_space` (sin CPU) hasta que el escritor avance.
- *Limitación conocida*: longitud máxima de código 64 bits (se reporta error si se excede; requeriría un archivo de
  más de ~10^13 bytes con frecuencias tipo Fibonacci).
