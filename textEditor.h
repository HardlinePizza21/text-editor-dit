/**
 * @file textEditor.h
 * @brief Editor de texto "dit" embebido en el shell. Ver INTEGRACION_EDITOR.md.
 *
 * La única función que necesita el shell es editorRun(). El resto de
 * prototipos son internos del módulo.
 */

#ifndef TEXTEDITOR_H
#define TEXTEDITOR_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <termios.h>
#include "huffman_mt.h"

#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3
#define DIT_VERSION "0.1.0"
#define ABUF_INIT {NULL, 0}
#define CTRL_KEY(k) ((k) & 0x1f)

/* Resultado de editorRun() */
typedef enum
{
    EDITOR_OK = 0,
    EDITOR_ERR_TTY, /* no hay terminal interactiva o no se pudo configurar */
    EDITOR_ERR_IO   /* error irrecuperable leyendo el teclado */
} EditorStatus;

/* Resultado de procesar una tecla */
typedef enum
{
    EDITOR_CONTINUE = 0,
    EDITOR_QUIT,
    EDITOR_FAILED
} EditorAction;

enum editorKey
{
    KEY_ERROR = -2,
    KEY_NONE = -1, /* timeout de lectura: permite refrescar sin tecla */
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
    KEY_TICK = 2000
};


/* ─── Structures ──────────────────────────────────────────────────────── */

typedef struct erow {
    char *chars;
    char *render;
    int size;
    int rsize;
} erow;

struct editorConfig
{
    int cx, cy;
    int rx;
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    int dirty;
    int prompting; /* >0 mientras hay un prompt: la barra de mensajes no muestra el progreso */
    erow *row;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios orig_termios;
    HuffJob *job; /* compresión/descompresión en segundo plano (NULL si no hay) */
};

struct abuf
{
    char *b;
    int len;
};

/* ---- API pública ---- */
EditorStatus editorRun(const char *filename);

/* ---- Internos ---- */
// Terminal
int terminalEnter(void);
void terminalLeave(void);
int editorReadKey(void);
int getCursorPosition(int *rows, int *cols);
int getWindowSize(int *rows, int *cols);

// Row operations
int editorRowCxToRx(erow *row, int cx);
void editorUpdateRow(erow *row);
void editorInsertRow(int at, char *s, size_t len);
void editorFreeRow(erow *row);
void editorDelRow(int at);
void editorRowInsertChar(erow *row, int at, int c);
void editorRowAppendString(erow *row, char *s, int len);
void editorRowDelChar(erow *row, int at);

// Editor operations
void editorInsertChar(int c);
void editorInsertNewline(void);
void editorDelChar(void);

// File i/o
char *editorRowsToString(int *buflen);
int editorSave(void);
int editorOpen(char *filename);

// Background job
void editorStartJob(HuffMode mode);
void editorCancelJob(void);
void editorPollJob(void);

// Append buffer
void abAppend(struct abuf *ab, const char *s, int len);
void abFree(struct abuf *ab);

// Output
void editorScroll(void);
void editorDrawRows(struct abuf *ab);
void editorDrawStatusBar(struct abuf *ab);
void editorDrawMessageBar(struct abuf *ab);
void editorRefreshScreen(void);
void editorSetStatusMessage(const char *fmt, ...);

// Input
char *editorPrompt(char *prompt);
char *editorPromptDefault(char *prompt, const char *def);
void editorMoveCursor(int key);
EditorAction editorProcessKeypress(void);

#ifdef __cplusplus
} /* extern "C" */
#endif /* __cplusplus */

#endif
