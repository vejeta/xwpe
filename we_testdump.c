/* we_testdump.c -- the editor's state, written for a test each time the
   editor waits for a key (see we_testdump.h for the format).
   Copyright (C) 2026 Juan Manuel Méndez Rey
   This file is part of xwpe and is distributed under the GNU General Public
   License, version 2 or later.

   Only the C library is used to write: the dump never draws, never moves
   the cursor and never changes what the editor holds.  It reads the
   windows, their buffers and the search strings, and it counts the keys the
   display's reader returns. */

#include "edit.h"
#include "we_buffer.h"
#include "we_lineend.h"
#include "we_testdump.h"
#include "we_ai.h"
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

/* The variable that names the descriptor, and what it holds when unset. */
#define DUMP_FD_VAR           "XWPE_TEST_DUMP_FD"
#define DUMP_FD_NONE          (-1)
/* Nested places that read keys (a dialog opened from a dropdown, a message
   over a dialog); deeper ones are counted but not named. */
#define DUMP_CTX_MAX          16
/* The longest dialog title the header names; longer ones are cut. */
#define DUMP_TITLE_MAX        64
/* The dump buffer grows by at least this much. */
#define DUMP_CHUNK            4096
/* The first byte above printable ASCII, and the last printable one. */
#define DUMP_PRINT_FIRST      0x20
#define DUMP_PRINT_LAST       0x7e
/* No key has been dumped yet. */
#define DUMP_SEQ_NONE         (-1L)
/* How an L line says the line ends: with its line break, or as a soft row
   the next row continues. */
#define DUMP_END_HARD         'H'
#define DUMP_END_SOFT         'S'
/* The top window's number before the editor has its windows. */
#define DUMP_TOP_NONE         (-1)

/* One place the editor is reading keys from. */
typedef struct
{
 WpeTestCtx kind;
 char title[DUMP_TITLE_MAX];
} DumpCtx;

/* The text of one dump, built before it is written in one go. */
typedef struct
{
 char *s;
 size_t len, size;
 int failed;              /* memory ran out: the dump is not written */
} DumpBuf;

static int dump_fd = DUMP_FD_NONE;
static long dump_keys = 0;              /* keys the display's reader returned */
static long dump_last_seq = DUMP_SEQ_NONE;
static int dump_autosaved = 0;
static DumpCtx dump_ctx[DUMP_CTX_MAX];
static int dump_ctx_depth = 0;
static int (*dump_real_getch)(void);
static int (*dump_real_take)(int want);

/* ---- the buffer ---- */

/**
 * dump_room - Make room for @n more bytes in @d.
 * Return: 1 when there is room, 0 when memory ran out (the dump is dropped).
 */
static int dump_room(DumpBuf *d, size_t n)
{
 size_t want;
 char *s;

 if (d->failed)
  return 0;
 if (d->len + n + 1 <= d->size)
  return 1;
 want = d->size + n + DUMP_CHUNK;
 s = realloc(d->s, want);
 if (s == NULL)
 {
  d->failed = 1;
  return 0;
 }
 d->s = s;
 d->size = want;
 return 1;
}

/**
 * dump_printf - Add formatted text to @d.
 */
static void dump_printf(DumpBuf *d, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;

static void dump_printf(DumpBuf *d, const char *fmt, ...)
{
 va_list ap;
 int n;

 va_start(ap, fmt);
 n = vsnprintf(NULL, 0, fmt, ap);
 va_end(ap);
 if (n < 0 || !dump_room(d, (size_t)n))
  return;
 va_start(ap, fmt);
 vsnprintf(d->s + d->len, d->size - d->len, fmt, ap);
 va_end(ap);
 d->len += (size_t)n;
}

/**
 * dump_byte_is_plain - May byte @c go into the dump as it is?
 * @space_plain: 0 where a space separates fields (a name, a search string).
 */
static int dump_byte_is_plain(unsigned char c, int space_plain)
{
 if (c == '\\')
  return 0;
 if (c == ' ')
  return space_plain;
 return c > DUMP_PRINT_FIRST && c <= DUMP_PRINT_LAST;
}

/**
 * dump_bytes - Add @n bytes of @s to @d, every byte a test could not read
 * back as itself written \xHH.
 * @space_plain: as dump_byte_is_plain.
 */
static void dump_bytes(DumpBuf *d, const unsigned char *s, int n, int space_plain)
{
 int i;

 for (i = 0; i < n; i++)
 {
  if (dump_byte_is_plain(s[i], space_plain))
  {
   if (dump_room(d, 1))
    d->s[d->len++] = (char)s[i];
  }
  else
   dump_printf(d, "\\x%02x", s[i]);
 }
}

/**
 * dump_string - Add the C string @s (none: "-") to @d, spaces escaped.
 */
static void dump_string(DumpBuf *d, const char *s)
{
 if (s == NULL || *s == '\0')
 {
  dump_printf(d, "-");
  return;
 }
 dump_bytes(d, (const unsigned char *)s, (int)strlen(s), 0);
}

/**
 * dump_write - Write the whole dump to the test's descriptor.
 *
 * A test that stopped reading makes the editor wait here, as a terminal
 * that stopped reading does; a test that closed its end ends the dumps
 * (the editor ignores SIGPIPE) and the editor goes on.
 */
static void dump_write(DumpBuf *d)
{
 size_t done = 0;
 ssize_t n;

 while (done < d->len)
 {
  n = write(dump_fd, d->s + done, d->len - done);
  if (n < 0 && errno == EINTR)
   continue;
  if (n <= 0)
  {
   dump_fd = DUMP_FD_NONE;
   return;
  }
  done += (size_t)n;
 }
}

/* ---- what the dump says ---- */

/**
 * dump_ctx_name - Add to @d the name of the place the editor reads keys
 * from, as the header gives it.
 * @waiting: the editor waits for something other than a key (a command it
 * runs, a program's output), and looks at the keyboard only in passing.
 */
static void dump_ctx_name(DumpBuf *d, int waiting)
{
 FENSTER *top;

 if (waiting)
 {
  dump_printf(d, "wait");
  return;
 }
 if (dump_ctx_depth > 0 && dump_ctx_depth <= DUMP_CTX_MAX)
 {
  DumpCtx *c = &dump_ctx[dump_ctx_depth - 1];

  if (c->kind == WPE_TEST_CTX_PREFIX)
   dump_printf(d, "prefix");
  else if (c->kind == WPE_TEST_CTX_MENU)
   dump_printf(d, "menu");
  else if (c->kind == WPE_TEST_CTX_MESSAGE)
   dump_printf(d, "message");
  else
  {
   dump_printf(d, "dialog:");
   dump_string(d, c->title);
  }
  return;
 }
 if (WpeEditor == NULL || WpeEditor->mxedt < 1)
 {
  dump_printf(d, "menu");
  return;
 }
 top = WpeEditor->f[WpeEditor->mxedt];
 if (top != NULL && top->dtmd == DTMD_FILEMANAGER)
  dump_printf(d, "filemanager");
 else if (top != NULL && !DTMD_ISTEXT(top->dtmd))
  dump_printf(d, "window");
 else if (wpe_paint_modal_depth() > 0)
  dump_printf(d, "box");
 else
  dump_printf(d, "edit");
}

/**
 * dump_steps - How many Ctrl-U (or Ctrl-R) presses list @u holds.
 *
 * A record flagged E_UNDO_MORE is part of the step of the record below it,
 * so every record without the flag ends one step.
 */
static int dump_steps(const Undo *u)
{
 int n = 0;

 for (; u != NULL; u = u->next)
  if (!(u->flags & E_UNDO_MORE))
   n++;
 return n;
}

/**
 * dump_window_is_text - Does window @f hold lines a test can read?
 */
static int dump_window_is_text(FENSTER *f)
{
 return f != NULL && DTMD_ISTEXT(f->dtmd) && f->b != NULL && f->s != NULL
        && f->b->bf != NULL;
}

/**
 * dump_window_locked - Does window @f refuse typing, as the padlock says?
 *
 * Asked without e_window_text_locked, which also says the reason on the
 * status row: the dump must not draw.
 */
static int dump_window_locked(FENSTER *f)
{
 if (f->ins == WPE_INS_READONLY)
  return 1;
#ifdef WPE_AI
 if (wpe_ai_window_locked(f))
  return 1;
#endif
 return 0;
}

/**
 * dump_breakpoints - Add the window's breakpoint rows ("-" for none).
 */
static void dump_breakpoints(DumpBuf *d, FENSTER *f)
{
#ifdef DEBUGGER
 int i, *brp = f->s->brp;

 if (brp != NULL && brp[0] > 0)
 {
  dump_printf(d, "%d", brp[1]);
  for (i = 2; i <= brp[0]; i++)
   dump_printf(d, ",%d", brp[i]);
  return;
 }
#endif
 (void)f;
 dump_printf(d, "-");
}

/**
 * dump_markers - Add the window's Ctrl-K 0..9 place markers as x,y pairs.
 */
static void dump_markers(DumpBuf *d, SCHIRM *s)
{
 int i, n = (int)(sizeof s->pt / sizeof s->pt[0]);

 dump_printf(d, "%d,%d", s->pt[0].x, s->pt[0].y);
 for (i = 1; i < n; i++)
  dump_printf(d, "/%d,%d", s->pt[i].x, s->pt[i].y);
}

/**
 * dump_window_head - Add the W line of window @n.
 *
 * Its rows= field is the first row wider than Max. Columns or not
 * ended as a row ends (-1 for none), so a key that left a row past the
 * margin is seen at the key, even where the primitive wrote it in place.
 * Its last field, pos=, names the first position that is not a place of the
 * text (-1 for none).
 */
static void dump_window_head(DumpBuf *d, int n, FENSTER *f)
{
 BUFFER *b = f->b;
 SCHIRM *s = f->s;

 dump_printf(d, "W %d ", n);
 dump_string(d, f->datnam);
 dump_printf(d, " ro=%d mx=%d y=%d x=%d mb=%d,%d me=%d,%d ks=%d,%d",
             dump_window_locked(f), b->mx.x, b->b.y, b->b.x,
             s->mark_begin.y, s->mark_begin.x, s->mark_end.y, s->mark_end.x,
             s->ks.y, s->ks.x);
 dump_printf(d, " ins=%d flg=%d save=%d view=%d,%d,%d,%d,%d,%d pt=",
             f->ins, f->flg, f->save, s->c.y, s->c.x, f->a.y, f->a.x,
             f->e.y, f->e.x);
 dump_markers(d, s);
 dump_printf(d, " brp=");
 dump_breakpoints(d, f);
 dump_printf(d, " ud=%d rd=%d rows=%d pos=%d", dump_steps(b->ud), dump_steps(b->rd),
             e_buffer_rows_check(b), e_buffer_first_misplaced(b));
 dump_printf(d, " eol=%s final=%d\n", e_eol_name(b), b->final_break);
}

/**
 * dump_window_lines - Add one L line per line of window @n.
 */
static void dump_window_lines(DumpBuf *d, int n, FENSTER *f)
{
 BUFFER *b = f->b;
 int i;

 for (i = 0; i < b->mxlines; i++)
 {
  STRING *l = &b->bf[i];

  if (l->s == NULL)
  {
   dump_printf(d, "L %d %c \n", n, DUMP_END_SOFT);
   continue;
  }
  if (l->s[l->len] == WPE_WR)
   dump_printf(d, "L %d %c ", n, DUMP_END_HARD);
  else
   dump_printf(d, "L %d %c ", n, DUMP_END_SOFT);
  dump_bytes(d, l->s, l->len, 1);
  dump_printf(d, "\n");
 }
}

/**
 * dump_windows - Add every text window, window 0 (the clipboard) first.
 */
static void dump_windows(DumpBuf *d)
{
 int i;

 if (WpeEditor == NULL)
  return;
 for (i = 0; i <= WpeEditor->mxedt && i <= MAXEDT; i++)
 {
  FENSTER *f = WpeEditor->f[i];

  if (!dump_window_is_text(f))
   continue;
  dump_window_head(d, i, f);
  dump_window_lines(d, i, f);
 }
}

/**
 * dump_find - Add the F line: what Find and Replace will look for.
 */
static void dump_find(DumpBuf *d)
{
 if (WpeEditor == NULL)
  return;
 dump_printf(d, "F ");
 dump_string(d, WpeEditor->fd.search);
 dump_printf(d, " ");
 dump_string(d, WpeEditor->fd.replace);
 dump_printf(d, " %u\n", WpeEditor->fd.sw);
}

/**
 * dump_state - Write one whole dump for the keys taken so far.
 * @waiting: as dump_ctx_name.
 */
static void dump_state(int waiting)
{
 DumpBuf d = { NULL, 0, 0, 0 };
 int top = DUMP_TOP_NONE;

 if (WpeEditor != NULL)
  top = WpeEditor->mxedt;
 dump_printf(&d, "S %ld ctx=", dump_keys);
 dump_ctx_name(&d, waiting);
 dump_printf(&d, " top=%d autosaved=%d\n", top, dump_autosaved);
 dump_windows(&d);
 dump_find(&d);
 dump_printf(&d, "E %ld\n", dump_keys);
 if (!d.failed)
  dump_write(&d);
 free(d.s);
 dump_autosaved = 0;
 dump_last_seq = dump_keys;
}

/**
 * dump_keyboard_pending - Has the terminal a key the editor has not read?
 *
 * Only the terminal's keyboard is looked at; under X11 or Wayland the
 * answer is "none" and each read is dumped, which costs time, not truth:
 * the test waits for the dump that counts all its keys.
 */
static int dump_keyboard_pending(void)
{
 int n = 0;

 if (ioctl(STDIN_FILENO, FIONREAD, &n) == 0 && n > 0)
  return 1;
 return 0;
}

/* ---- the reader ---- */

/**
 * wpe_test_dump_idle - The editor is about to block for a key: write what
 * it holds.
 *
 * The screen is brought up to date first, so a test that reads the dump's
 * end has the screen of the same moment in its terminal already.
 */
void wpe_test_dump_idle(void)
{
 if (dump_fd == DUMP_FD_NONE)
  return;
 if (e_u_refresh)
  e_refresh();
 dump_state(0);
}

/**
 * dump_getch - The display's key reader, with the dump before it blocks.
 *
 * Every key the editor reads, in the text, a prefix, a menu, a dialog or
 * the File Manager, passes here, so a test sees one dump per key it sends
 * (the last one counting all of them), wherever the key lands.
 */
static int dump_getch(void)
{
 int c;

 if (!dump_keyboard_pending())
  wpe_test_dump_idle();
 c = dump_real_getch();
 dump_keys++;
 return c;
}

/**
 * dump_take_key - The display's look at the keyboard without waiting, with
 * one dump the first time it finds no key for the keys taken so far.
 *
 * A loop that must not wait for keys (a program the Run menu started, a
 * short command Esc may end) asks here between other waits; the dump says
 * the editor has taken every key sent and is busy, once, not each time.
 */
static int dump_take_key(int want)
{
 int c = dump_real_take(want);

 if (c != 0)
 {
  dump_keys++;
  return c;
 }
 if (dump_fd != DUMP_FD_NONE && dump_last_seq != dump_keys)
 {
  if (e_u_refresh)
   e_refresh();
  dump_state(1);
 }
 return 0;
}

/**
 * dump_fd_is_the_display - Is @fd one of the editor's standard streams, or a
 * terminal?
 *
 * A dump written there would draw on the user's screen, and closing it for
 * the programs the editor starts would take their output away; so a test
 * that names such a descriptor by mistake gets no dump instead.
 */
static int dump_fd_is_the_display(int fd)
{
 return fd <= STDERR_FILENO || isatty(fd);
}

/**
 * dump_fd_from_env - The descriptor $XWPE_TEST_DUMP_FD names, open for
 * writing and not the display, or DUMP_FD_NONE.
 */
static int dump_fd_from_env(void)
{
 const char *v = getenv(DUMP_FD_VAR);
 char *end;
 long fd;
 int fl;

 if (v == NULL || *v == '\0')
  return DUMP_FD_NONE;
 errno = 0;
 fd = strtol(v, &end, 10);
 if (errno != 0 || *end != '\0' || fd < 0 || fd > INT_MAX)
  return DUMP_FD_NONE;
 if (dump_fd_is_the_display((int)fd))
  return DUMP_FD_NONE;
 fl = fcntl((int)fd, F_GETFL);
 if (fl < 0 || (fl & O_ACCMODE) == O_RDONLY)
  return DUMP_FD_NONE;
 return (int)fd;
}

/**
 * wpe_test_dump_from_env - Start the dumps when $XWPE_TEST_DUMP_FD names
 * a descriptor open for writing.
 *
 * Called once at start, after the display has set its key reader.  The
 * descriptor is closed for the programs the editor starts, so a shell
 * command or a build never writes into the test's pipe or holds it open.
 * Unset, or not a writable descriptor, nothing changes.
 */
void wpe_test_dump_from_env(void)
{
 int fd = dump_fd_from_env();

 if (fd == DUMP_FD_NONE || e_u_getch == NULL)
  return;
 fcntl(fd, F_SETFD, FD_CLOEXEC);
 dump_fd = fd;
 dump_real_getch = e_u_getch;
 e_u_getch = dump_getch;
 if (e_u_key_take != NULL)
 {
  dump_real_take = e_u_key_take;
  e_u_key_take = dump_take_key;
 }
}

/**
 * wpe_test_dump_on - Is a test reading the dumps?
 */
int wpe_test_dump_on(void)
{
 return dump_fd != DUMP_FD_NONE;
}

/* ---- where the keys are read ---- */

/**
 * wpe_test_ctx_enter - The keys read from now on are for @kind.
 * @title: the dialog's title (for WPE_TEST_CTX_DIALOG), or NULL.
 *
 * A test that sends Alt-O then "e" learns from the dump that the next key
 * goes to the Editor Options dialog and not to the text.  Each enter has
 * its wpe_test_ctx_leave.
 */
void wpe_test_ctx_enter(WpeTestCtx kind, const char *title)
{
 if (dump_fd == DUMP_FD_NONE)
  return;
 if (dump_ctx_depth < DUMP_CTX_MAX)
 {
  DumpCtx *c = &dump_ctx[dump_ctx_depth];

  c->kind = kind;
  c->title[0] = '\0';
  if (title != NULL)
  {
   strncpy(c->title, title, sizeof c->title - 1);
   c->title[sizeof c->title - 1] = '\0';
  }
 }
 dump_ctx_depth++;
}

/**
 * wpe_test_ctx_leave - Back to where the keys were read before.
 */
void wpe_test_ctx_leave(void)
{
 if (dump_fd == DUMP_FD_NONE)
  return;
 if (dump_ctx_depth > 0)
  dump_ctx_depth--;
}

/**
 * wpe_test_dump_note_autosave - An autosave wrote the .ASV file: the next
 * dump says so, so a test compares that file with the text of the same
 * moment.
 */
void wpe_test_dump_note_autosave(void)
{
 dump_autosaved = 1;
}
