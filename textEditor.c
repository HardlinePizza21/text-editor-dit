/*** Includes ***/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <string.h>
#include <stdarg.h>
#include <sys/types.h>
#include <fcntl.h>
#include <time.h>
#include "textEditor.h"

struct editorConfig E;

/*** Terminal ***/

static int rawModeOn = 0;

/* Entra en modo raw + pantalla alterna. Simétrica a terminalLeave(). */
int terminalEnter(void)
{
    if (rawModeOn)
        return 0;
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
        return -1;
    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1; /* read() expira a los 100 ms: permite refrescar el progreso */
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        return -1;
    rawModeOn = 1;
    write(STDOUT_FILENO, "\x1b[?1049h\x1b[2J\x1b[H", 16);
    return 0;
}

void terminalLeave(void)
{
    if (!rawModeOn)
        return;
    write(STDOUT_FILENO, "\x1b[?1049l", 8);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
    rawModeOn = 0;
}

int editorReadKey(void)
{
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
    {
        if (nread == 0 || (nread == -1 && (errno == EAGAIN || errno == EINTR)))
            return KEY_NONE;
        return KEY_ERROR;
    }

    if (c == '\x1b')
    {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1)
            return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
            return '\x1b';

        if (seq[0] == '[')
        {
            if (seq[1] >= '0' && seq[1] <= '9')
            {
                if (read(STDIN_FILENO, &seq[2], 1) != 1)
                    return '\x1b';
                if (seq[2] == '~')
                {
                    switch (seq[1])
                    {
                    case '1':
                    case '7':
                        return HOME_KEY;
                    case '3':
                        return DEL_KEY;
                    case '4':
                    case '8':
                        return END_KEY;
                    case '5':
                        return PAGE_UP;
                    case '6':
                        return PAGE_DOWN;
                    }
                }
            }
            else
            {
                switch (seq[1])
                {
                case 'A':
                    return ARROW_UP;
                case 'B':
                    return ARROW_DOWN;
                case 'C':
                    return ARROW_RIGHT;
                case 'D':
                    return ARROW_LEFT;
                case 'H':
                    return HOME_KEY;
                case 'F':
                    return END_KEY;
                }
            }
        }
        return '\x1b';
    }
    return c;
}

int getCursorPosition(int *rows, int *cols)
{
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;

    while (i < sizeof(buf) - 1)
    {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;
        if (buf[i] == 'R')
            break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[')
        return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
        return -1;
    return 0;
}

int getWindowSize(int *rows, int *cols)
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
            return -1;
        return getCursorPosition(rows, cols);
    }
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

/* Se invoca en cada refresco para seguir los cambios de tamaño de la terminal. */
static void editorUpdateWindowSize(void)
{
    int r, c;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
    {
        r = ws.ws_row;
        c = ws.ws_col;
        E.screenrows = r > 2 ? r - 2 : 1;
        E.screencols = c;
    }
}

/*** Row operations ***/
int editorRowCxToRx(erow *row, int cx)
{

    int rx = 0;

    int j;

    for (j = 0; j < cx; j++)
    {
        if (row->chars[j] == '\t')
            rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        rx++;
    }

    return rx;
}

void editorUpdateRow(erow *row)
{
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; j++)
        if (row->chars[j] == '\t')
            tabs++;
    free(row->render);
    row->render = malloc(row->size + tabs * (KILO_TAB_STOP - 1) + 1);
    int idx = 0;
    for (j = 0; j < row->size; j++)
    {
        if (row->chars[j] == '\t')
        {
            row->render[idx++] = ' ';
            while (idx % KILO_TAB_STOP != 0)
                row->render[idx++] = ' ';
        }
        else
        {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorInsertRow(int at, char *s, size_t len)
{
    if (at < 0 || at > E.numrows)
        return;

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

void editorFreeRow(erow *row)
{
    free(row->render);
    free(row->chars);
}

void editorDelRow(int at)
{
    if (at < 0 || at >= E.numrows)
        return;

    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    E.numrows--;
    E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c)
{
    if (at < 0 || at > row->size)
        at = row->size;

    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, int len)
{
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);

    row->size += len;
    row->chars[row->size] = '\0';

    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at)
{
    if (at < 0 || at > row->size)
        return;

    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/*** Editor operations ***/

void editorInsertChar(int c)
{

    if (E.cy == E.numrows)
    {
        editorInsertRow(E.numrows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorInsertNewline()
{
    if (E.cx == 0)
    {
        editorInsertRow(E.cy, "", 0);
    }
    else
    {
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';

        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = 0;
}

void editorDelChar()
{
    if (E.cy == E.numrows)
        return;
    if (E.cy == 0 && E.cx == 0)
        return;

    erow *row = &E.row[E.cy];
    if (E.cx > 0)
    {
        editorRowDelChar(row, E.cx);
        E.cx--;
    }
    else
    {
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

/*** File i/o ***/
char *editorRowsToString(int *buflen)
{
    int totallen = 0;
    for (int j = 0; j < E.numrows; j++)
        totallen += E.row[j].size + 1;
    *buflen = totallen;

    char *buf = malloc(totallen ? totallen : 1);
    char *p = buf;

    for (int j = 0; j < E.numrows; j++)
    {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

/* Devuelve 1 si ambas rutas designan el mismo archivo (o son iguales si no existen). */
static int samePath(const char *a, const char *b)
{
    struct stat sa, sb;
    if (stat(a, &sa) == 0 && stat(b, &sb) == 0)
        return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
    return strcmp(a, b) == 0;
}

/* Un archivo está bloqueado si un job de compresión lo está leyendo o escribiendo. */
static int editorPathLocked(const char *path)
{
    return E.job && path &&
           (samePath(path, huffjob_input(E.job)) || samePath(path, huffjob_output(E.job)));
}

static int editorBufferLocked(void)
{
    return editorPathLocked(E.filename);
}

static void editorWarnLocked(void)
{
    editorSetStatusMessage("Archivo bloqueado por %s en curso (Ctrl-X para cancelar)",
                           huffjob_mode(E.job) == HUFF_COMPRESS ? "compresion" : "descompresion");
}

int editorSave(void)
{
    char *name = E.filename;
    char *prompted = NULL;
    if (name == NULL)
    {
        name = prompted = editorPrompt("Save as: %s (ESC to cancel)");
        if (name == NULL)
        {
            editorSetStatusMessage("Save aborted");
            return -1;
        }
    }

    if (editorPathLocked(name))
    {
        free(prompted);
        editorWarnLocked();
        return -1;
    }
    if (prompted)
        E.filename = prompted;

    int len;
    char *buf = editorRowsToString(&len);

    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1)
    {
        if (ftruncate(fd, len) != -1 && write(fd, buf, len) == len)
        {
            close(fd);
            free(buf);
            E.dirty = 0;
            editorSetStatusMessage("%d bytes written to disk", len);
            return 0;
        }
        close(fd);
    }
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
    free(buf);
    return -1;
}

static void editorClearRows(void)
{
    for (int i = 0; i < E.numrows; i++)
        editorFreeRow(&E.row[i]);
    free(E.row);
    E.row = NULL;
    E.numrows = 0;
}

/* Carga el archivo en el buffer. Si no existe, deja un buffer vacío con ese nombre. */
int editorOpen(char *filename)
{
    free(E.filename);
    E.filename = strdup(filename);

    int fd = open(filename, O_RDONLY);
    if (fd == -1)
        return errno == ENOENT ? 0 : -1;

    char buf[4096];
    ssize_t n;
    char *line = NULL;
    size_t linelen = 0;
    size_t linecap = 0;

    while ((n = read(fd, buf, sizeof(buf))) > 0)
    {
        for (ssize_t i = 0; i < n; i++)
        {
            if (buf[i] == '\n')
            {
                while (linelen > 0 && (line[linelen - 1] == '\r'))
                    linelen--;

                editorInsertRow(E.numrows, line ? line : "", linelen);

                free(line);
                line = NULL;
                linelen = 0;
                linecap = 0;
                continue;
            }
            if (linelen + 1 > linecap)
            {
                linecap = linecap == 0 ? 128 : linecap * 2;
                char *tmp = realloc(line, linecap);
                if (!tmp)
                {
                    free(line);
                    close(fd);
                    return -1;
                }
                line = tmp;
            }
            line[linelen++] = buf[i];
        }
    }

    int rc = n == -1 ? -1 : 0;
    if (linelen > 0)
    {
        while (linelen > 0 && line[linelen - 1] == '\r')
            linelen--;
        editorInsertRow(E.numrows, line, linelen);
    }
    free(line);
    E.dirty = 0;
    close(fd);
    return rc;
}

static void editorReload(void)
{
    char *name = strdup(E.filename);
    editorClearRows();
    editorOpen(name);
    free(name);
    if (E.cy > E.numrows)
        E.cy = E.numrows;
    E.cx = 0;
    E.rowoff = 0;
    E.coloff = 0;
}

/*** Background compression job ***/

/* Se llama en cada tick del loop: recoge el resultado de un job terminado. */
void editorPollJob(void)
{
    if (!E.job || huffjob_state(E.job) == JOB_RUNNING)
        return;

    HuffMode mode = huffjob_mode(E.job);
    char out[4096], err[160];
    snprintf(out, sizeof(out), "%s", huffjob_output(E.job));
    JobState st = huffjob_finish(E.job, err, sizeof(err));
    E.job = NULL;

    const char *what = mode == HUFF_COMPRESS ? "Compresion" : "Descompresion";
    if (st == JOB_OK)
    {
        if (E.filename && !E.dirty && samePath(E.filename, out))
        {
            editorReload();
            editorSetStatusMessage("%s completada: %s (buffer recargado)", what, out);
        }
        else
            editorSetStatusMessage("%s completada: %s", what, out);
    }
    else if (st == JOB_CANCELLED)
        editorSetStatusMessage("%s cancelada", what);
    else
        editorSetStatusMessage("%s fallida: %s", what, err);
}

static int editorConfirm(const char *question)
{
    int answer = -1;
    E.prompting++;
    while (answer < 0)
    {
        editorSetStatusMessage("%s (y/n)", question);
        editorRefreshScreen();
        int c = editorReadKey();
        if (c == 'y' || c == 'Y')
            answer = 1;
        else if (c == 'n' || c == 'N' || c == '\x1b' || c == KEY_ERROR)
            answer = 0;
        else
            editorPollJob();
    }
    E.prompting--;
    editorSetStatusMessage("");
    return answer;
}

void editorStartJob(HuffMode mode)
{
    if (E.job)
    {
        editorSetStatusMessage("Ya hay una operacion en curso (Ctrl-X para cancelar)");
        return;
    }
    if (!E.filename || access(E.filename, R_OK) != 0)
    {
        editorSetStatusMessage("Guarda el archivo en disco antes de %s",
                               mode == HUFF_COMPRESS ? "comprimir" : "descomprimir");
        return;
    }
    if (E.dirty)
    {
        editorSetStatusMessage("Hay cambios sin guardar: guarda (Ctrl-S) antes de %s",
                               mode == HUFF_COMPRESS ? "comprimir" : "descomprimir");
        return;
    }

    char def[4096];
    size_t fl = strlen(E.filename), el = strlen(HUFF_EXT);
    if (mode == HUFF_COMPRESS)
        snprintf(def, sizeof(def), "%s%s", E.filename, HUFF_EXT);
    else if (fl > el && strcmp(E.filename + fl - el, HUFF_EXT) == 0)
        snprintf(def, sizeof(def), "%.*s", (int)(fl - el), E.filename);
    else
        snprintf(def, sizeof(def), "%s.out", E.filename);

    char prompt[256];
    snprintf(prompt, sizeof(prompt), "%s a (Enter = %.100s): %%s (ESC cancela)",
             mode == HUFF_COMPRESS ? "Comprimir" : "Descomprimir", def);
    char *out = editorPromptDefault(prompt, def);
    if (!out)
        return;

    if (samePath(out, E.filename))
        editorSetStatusMessage("La salida no puede ser el archivo de entrada");
    else if (access(out, F_OK) == 0 && !editorConfirm("El destino existe, sobrescribir?"))
        ;
    else if (E.job) /* un job pudo iniciarse mientras se esperaba la confirmación */
        editorSetStatusMessage("Ya hay una operacion en curso");
    else
    {
        E.job = huffjob_start(mode, E.filename, out, 0);
        if (!E.job)
            editorSetStatusMessage("No se pudo iniciar el trabajo en segundo plano");
    }
    free(out);
}

void editorCancelJob(void)
{
    if (!E.job)
    {
        editorSetStatusMessage("No hay operacion en curso");
        return;
    }
    huffjob_cancel(E.job);
    editorSetStatusMessage("Cancelando...");
}

/*** append buffer ***/

void abAppend(struct abuf *ab, const char *s, int len)
{

    char *new = realloc(ab->b, ab->len + len);

    memcpy(&new[ab->len], s, len);

    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab)
{
    free(ab->b);
}

/*** Output ***/

void editorScroll()
{
    E.rx = 0;
    if (E.cy < E.numrows)
    {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    if (E.cy < E.rowoff)
    {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows)
    {
        E.rowoff = E.cy - E.screenrows + 1;
    }

    if (E.rx < E.coloff)
    {
        E.coloff = E.rx;
    }

    if (E.rx >= E.coloff + E.screencols)
    {
        E.coloff = E.rx - E.screencols + 1;
    }
}

void editorDrawRows(struct abuf *ab)
{
    int y;
    for (y = 0; y < E.screenrows; y++)
    {
        int fileRow = y + E.rowoff;
        if (fileRow >= E.numrows)
        {
            if (E.numrows == 0 && y == E.screenrows / 3)
            {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "Dit Editor -- Version %s", DIT_VERSION);

                if (welcomelen > E.screencols)
                    welcomelen = E.screencols;

                int padding = (E.screencols - welcomelen) / 2;
                if (padding)
                {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--)
                    abAppend(ab, " ", 1);

                abAppend(ab, welcome, welcomelen);
            }
            else
            {
                abAppend(ab, "~", 1);
            }
        }
        else
        {
            int len = E.row[fileRow].rsize - E.coloff;
            if (len < 0)
                len = 0;
            if (len > E.screencols)
                len = E.screencols;
            abAppend(ab, &E.row[fileRow].render[E.coloff], len);
        }

        abAppend(ab, "\x1b[K", 3);
        abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuf *ab)
{
    abAppend(ab, "\x1b[7m", 4);

    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                       E.filename ? E.filename : "[No name]", E.numrows,
                       E.dirty ? "(modified)" : "");
    int rlen;
    if (E.job)
        rlen = snprintf(rstatus, sizeof(rstatus), "[%s %d%%] %d/%d",
                        huffjob_mode(E.job) == HUFF_COMPRESS ? "comprimiendo" : "descomprimiendo",
                        huffjob_percent(E.job), E.cy + 1, E.numrows);
    else
        rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);

    if (len > E.screencols)
        len = E.screencols;
    abAppend(ab, status, len);

    while (len < E.screencols)
    {
        if (E.screencols - len == rlen)
        {
            abAppend(ab, rstatus, rlen);
            break;
        }
        else
        {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab, "\x1b[m", 3);

    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab)
{
    abAppend(ab, "\x1b[K", 3);
    int recent = E.statusmsg[0] && time(NULL) - E.statusmsg_time < 2;
    if (E.job && !E.prompting && !recent)
    {
        char bar[200];
        int pct = huffjob_percent(E.job), width = 20;
        char fill[21];
        for (int i = 0; i < width; i++)
            fill[i] = i < pct * width / 100 ? '#' : '-';
        fill[width] = '\0';
        int n = snprintf(bar, sizeof(bar), "%s [%s] %3d%% %s (Ctrl-X cancela)",
                         huffjob_mode(E.job) == HUFF_COMPRESS ? "Comprimiendo" : "Descomprimiendo",
                         fill, pct, huffjob_phase_name(E.job));
        if (n > E.screencols)
            n = E.screencols;
        abAppend(ab, bar, n);
        return;
    }
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols)
        msglen = E.screencols;
    if (msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen()
{
    editorUpdateWindowSize();
    editorScroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}
void editorSetStatusMessage(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    // Lo mismo que snprintf pero la v indica variadic, asi que ap es una lista de tamano indefinido
    // snprintf recibe cualquier cantidad de argumentos mientras vsnprintf recibe la estructura que los agrupa
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/*** Input ***/
/* Pide una línea al usuario. Enter con texto vacío devuelve strdup(def) si def != NULL. */
char *editorPromptDefault(char *prompt, const char *def)
{
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';
    E.prompting++;

    while (1)
    {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == KEY_NONE)
        {
            editorPollJob();
            continue;
        }
        if (c == KEY_ERROR)
        {
            free(buf);
            E.prompting--;
            return NULL;
        }
        if (c == CTRL_KEY('h') || c == BACKSPACE || c == DEL_KEY)
        {
            if (buflen != 0)
                buf[--buflen] = '\0';
        }
        else if (c == '\x1b')
        {
            editorSetStatusMessage("");
            free(buf);
            E.prompting--;
            return NULL;
        }
        else if (c == '\r')
        {
            if (buflen != 0 || def)
            {
                editorSetStatusMessage("");
                E.prompting--;
                if (buflen == 0)
                {
                    free(buf);
                    return strdup(def);
                }
                return buf;
            }
        }
        else if (!iscntrl(c) && c < 128)
        {
            if (buflen == bufsize - 1)
            {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }

            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
    }
}

char *editorPrompt(char *prompt)
{
    return editorPromptDefault(prompt, NULL);
}

void editorMoveCursor(int key)
{
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key)
    {
    case ARROW_LEFT:
        if (E.cx != 0)
        {
            E.cx--;
        }
        else if (E.cy > 0)
        {
            E.cy--;
            E.cx = E.row[E.cy].size;
        }
        break;
    case ARROW_RIGHT:
        if (row && E.cx < row->size)
        {
            E.cx++;
        }
        else if (row && E.cx == row->size)
        {
            E.cy++;
            E.cx = 0;
        }
        break;
    case ARROW_UP:
        if (E.cy != 0)
        {
            E.cy--;
        }
        break;
    case ARROW_DOWN:
        if (E.cy < E.numrows)
        {
            E.cy++;
        }
        break;
    }
    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen)
        E.cx = rowlen;
}

EditorAction editorProcessKeypress(void)
{
    static int quit_times = KILO_QUIT_TIMES;
    int c = editorReadKey();

    if (c == KEY_NONE)
    {
        editorPollJob();
        return EDITOR_CONTINUE;
    }
    if (c == KEY_ERROR)
        return EDITOR_FAILED;

    /* Teclas que modifican el buffer: bloqueadas mientras un job usa el archivo */
    int mutates = c == '\r' || c == BACKSPACE || c == CTRL_KEY('h') || c == DEL_KEY ||
                  c == '\t' || (!iscntrl(c) && c < 256 && c >= 32);
    if (mutates && editorBufferLocked())
    {
        editorWarnLocked();
        return EDITOR_CONTINUE;
    }

    switch (c)
    {
    case '\r':
        editorInsertNewline();
        break;
    case CTRL_KEY('s'):
        editorSave();
        break;
    case CTRL_KEY('k'):
        editorStartJob(HUFF_COMPRESS);
        break;
    case CTRL_KEY('u'):
        editorStartJob(HUFF_DECOMPRESS);
        break;
    case CTRL_KEY('x'):
        editorCancelJob();
        break;

    case CTRL_KEY('q'):
        if ((E.dirty || E.job) && quit_times > 0)
        {
            editorSetStatusMessage(E.job ? "WARNING!!! Operation in progress (will be cancelled). "
                                           "Press Ctrl-Q %d more times to quit."
                                         : "WARNING!!! File has unsaved changes. "
                                           "Press Ctrl-Q %d more times to quit.",
                                   quit_times);
            quit_times--;
            return EDITOR_CONTINUE;
        }
        return EDITOR_QUIT;

    case PAGE_DOWN:
    case PAGE_UP:
    {
        if (c == PAGE_UP)
        {
            E.cy = E.rowoff;
        }
        else if (c == PAGE_DOWN)
        {
            E.cy = E.rowoff + E.screenrows + 1;
            if (E.cy > E.numrows)
                E.cy = E.numrows;
        }

        int times = E.screenrows;
        while (times--)
        {
            editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
        break;
    }

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        editorMoveCursor(c);
        break;

    case HOME_KEY:
        E.cx = 0;
        break;
    case END_KEY:
        if (E.cy < E.numrows)
        {
            E.cx = E.row[E.cy].size;
        }
        break;
    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
        if (c == DEL_KEY)
            editorMoveCursor(ARROW_RIGHT);
        editorDelChar();
        break;

    case CTRL_KEY('l'):
    case '\x1b':
        break;
    default:
        if (c == '\t' || (!iscntrl(c) && c >= 32))
            editorInsertChar(c);
        break;
    }

    quit_times = KILO_QUIT_TIMES;
    return EDITOR_CONTINUE;
}

/*** init ***/

static int initEditor(void)
{
    memset(&E, 0, sizeof(E));
    if (getWindowSize(&E.screenrows, &E.screencols) == -1)
        return -1;
    E.screenrows = E.screenrows > 2 ? E.screenrows - 2 : 1;
    return 0;
}

static void editorFree(void)
{
    editorClearRows();
    free(E.filename);
    E.filename = NULL;
}

/*** Entry point ***/

/* Único punto de entrada del editor. Posee terminal, estado y job; al volver
 * todo está restaurado/liberado. */
EditorStatus editorRun(const char *filename)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return EDITOR_ERR_TTY;
    if (terminalEnter() == -1)
        return EDITOR_ERR_TTY;
    if (initEditor() == -1)
    {
        terminalLeave();
        return EDITOR_ERR_TTY;
    }

    if (filename && editorOpen((char *)filename) == -1)
        editorSetStatusMessage("Can't open %s: %s", filename, strerror(errno));
    else
        editorSetStatusMessage("HELP: Ctrl-S save | Ctrl-K compress | Ctrl-U decompress | Ctrl-X cancel | Ctrl-Q quit");

    EditorStatus status = EDITOR_OK;
    for (;;)
    {
        editorRefreshScreen();
        EditorAction a = editorProcessKeypress();
        if (a == EDITOR_QUIT)
            break;
        if (a == EDITOR_FAILED)
        {
            status = EDITOR_ERR_IO;
            break;
        }
    }

    if (E.job)
    {
        huffjob_cancel(E.job);
        huffjob_finish(E.job, NULL, 0);
        E.job = NULL;
    }
    terminalLeave();
    editorFree();
    return status;
}
