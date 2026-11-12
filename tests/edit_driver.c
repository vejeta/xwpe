/* edit_driver.c -- the buffer owner (we_buffer.c), driven one line at a time.
 *
 * Reads commands on standard input, one per line, does each to a buffer the
 * way the editor's block commands, Return, Backspace and Undo do, and after
 * each prints what the buffer holds: its rows with their line ends, the
 * breakpoint rows, the block marks, the undo and redo depths, the modified
 * count.  A test program (tests/edit_model/cdriver.py) runs the same
 * commands on a small model of the owner and compares the two, thousands of
 * steps a second, with no terminal and no editor.
 *
 * Links we_buffer.c only; the editor's side of its seam is stubbed below, as
 * tests/test_splice.c does it.
 *
 *   open <width> <numundo>        a new buffer of one empty row (buffer 1)
 *   open2 <width>                 a second, empty buffer (a window of another width)
 *   text <s>                      the buffer holds text <s>, no undo, saved
 *   splice <y> <n_del> <s>        rows y .. y+n_del-1 become the lines of <s>
 *   delete <y> <n>                n rows go from row y
 *   blank <y>                     an empty row above row y
 *   split <y> <x>                 row y breaks at column x, the head ends hard
 *   move <y_from> <n> <y_to>      n rows move above row y_to, their breakpoints with them
 *   copy <y_from> <n> <y_to>      copies of n rows go in above row y_to
 *   xmove <y_from> <n> <y_to>     as move, from buffer 1 into buffer 2
 *   undo | redo                   one Ctrl-U or Ctrl-R
 *   fail <k>                      the k-th owner allocation of the next command and
 *                                 every later one fail; 0 gives memory back
 *   brp <row>...                  the breakpoint rows (none: clear them)
 *   mark <y1> <x1> <y2> <x2>      the block's begin and end
 *   dump                          print the state, change nothing
 *
 * A string <s> has no blanks: \n is a line break, \xHH a byte, \\ a backslash.
 * Each command prints  R <name> rc=<owner's return code>  then
 *   L <i> <H|S> <row>   B <rows|->   M <y>,<x> <y>,<x>   U ud=<n> rd=<n> save=<n>
 * and, once buffer 2 is open, the same L lines as  L2.  The last line is E.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "../we_buffer.h"

#define DEFAULT_WIDTH     80
#define DEFAULT_NUMUNDO   10
#define LINE_MAX_BYTES    65536
#define MAX_TOKENS        64
#define MAX_BRP           4096
#define SECOND_WIDTH_NONE 0

/* ---- the editor's side of the seam: recorders that do nothing ---- */

void wpe_buffer_syntax_changed(BUFFER *b, int y)
{
 (void) b;
 (void) y;
}

void wpe_buffer_lines_moved(BUFFER *b, int y, int n_del, int n_ins,
                            const int *rows_removed, int n_removed,
                            const int *rows_added, int n_added)
{
 (void) b; (void) y; (void) n_del; (void) n_ins;
 (void) rows_removed; (void) n_removed; (void) rows_added; (void) n_added;
}

void wpe_buffer_say_nomem(BUFFER *b, const char *what)
{
 (void) what;
 if (e_buffer_is_shell(b))
  e_buffer_shell_refused(b);
}

/* The driver has no status row: a refusal for a read-only window is
   seen by the client through the splice's return code alone. */
void wpe_buffer_say_read_only(BUFFER *b)
{
 (void) b;
}

int wpe_buffer_replay_kind(BUFFER *b, SCHIRM *s, Undo *rec)
{
 (void) b;
 (void) s;
 (void) rec;
 return(E_BUFFER_OK);
}

void wpe_buffer_replay_on_shell(BUFFER *shell, SCHIRM *s, const Undo *rec, int y0)
{
 (void) shell; (void) s; (void) rec; (void) y0;
}

void wpe_buffer_trace(const char *fmt, ...)
{
 (void) fmt;
}

/* ---- the two windows ---- */

typedef struct {
 ECNT cn;
 FENSTER win;
 SCHIRM scr;
 BUFFER buf;
 int open;
} Slot;

static Slot first, second;

/**
 * slot_open - Give @sl an empty buffer @width wide.
 *
 * The buffer starts as the editor's does: one empty row, nothing to undo,
 * nothing marked, saved.
 */
static void slot_open(Slot *sl, int width, int numundo)
{
 memset(sl, 0, sizeof *sl);
 sl->cn.numundo = numundo;
 sl->win.s = &sl->scr;
 sl->win.datnam = "t.c";
 e_buffer_init(&sl->buf, &sl->cn, &sl->win, width, E_BUFFER_HISTORY_FULL);
 sl->win.b = &sl->buf;
 sl->open = 1;
}

/**
 * slot_set_breakpoints - Replace the breakpoint rows of @sl by @rows.
 */
static void slot_set_breakpoints(Slot *sl, const int *rows, int n)
{
 int i;

 sl->scr.brp = realloc(sl->scr.brp, (size_t)(n + 1) * sizeof(int));
 sl->scr.brp[0] = n;
 for (i = 0; i < n; i++)
  sl->scr.brp[i + 1] = rows[i];
}

/* ---- strings ---- */

static int hex_value(int c)
{
 if (c >= '0' && c <= '9')
  return(c - '0');
 if (c >= 'a' && c <= 'f')
  return(c - 'a' + 10);
 if (c >= 'A' && c <= 'F')
  return(c - 'A' + 10);
 return(-1);
}

/**
 * unescape - The bytes a command's string stands for, ended by a NUL.
 */
static void unescape(const char *in, char *out, size_t cap)
{
 size_t n = 0;

 while (*in && n + 1 < cap)
 {
  if (in[0] == '\\' && in[1] == 'n')
  {
   out[n++] = '\n';
   in += 2;
  }
  else if (in[0] == '\\' && in[1] == '\\')
  {
   out[n++] = '\\';
   in += 2;
  }
  else if (in[0] == '\\' && in[1] == 'x' && hex_value(in[2]) >= 0 && hex_value(in[3]) >= 0)
  {
   out[n++] = (char)(hex_value(in[2]) * 16 + hex_value(in[3]));
   in += 4;
  }
  else
   out[n++] = *in++;
 }
 out[n] = '\0';
}

/**
 * print_escaped - A row's bytes as a command would write them.
 */
static void print_escaped(const unsigned char *s, int len)
{
 int i;

 for (i = 0; i < len; i++)
  if (s[i] > 0x20 && s[i] < 0x7f && s[i] != '\\')
   putchar(s[i]);
  else
   printf("\\x%02x", s[i]);
}

/* ---- the state, as text ---- */

static int list_len(Undo *u)
{
 int n = 0;

 for (; u != NULL; u = u->next)
  n++;
 return(n);
}

static void print_rows(const char *tag, Slot *sl)
{
 int y;

 for (y = 0; y < sl->buf.mxlines; y++)
 {
  STRING *l = &sl->buf.bf[y];

  printf("%s %d %c ", tag, y, l->s[l->len] == WPE_WR ? 'H' : 'S');
  print_escaped(l->s, l->len);
  putchar('\n');
 }
}

static void print_state(const char *name, int rc)
{
 int i;

 printf("R %s rc=%d\n", name, rc);
 print_rows("L", &first);
 printf("B");
 if (first.scr.brp == NULL || first.scr.brp[0] == 0)
  printf(" -");
 else
  for (i = 1; i <= first.scr.brp[0]; i++)
   printf(" %d", first.scr.brp[i]);
 printf("\nM %d,%d %d,%d\n", first.scr.mark_begin.y, first.scr.mark_begin.x,
        first.scr.mark_end.y, first.scr.mark_end.x);
 printf("U ud=%d rd=%d save=%d\n", list_len(first.buf.ud), list_len(first.buf.rd),
        first.win.save);
 if (second.open)
  print_rows("L2", &second);
 printf("E\n");
 fflush(stdout);
}

/* ---- the commands ---- */

/**
 * splice_lines - Put @lines (@n of them) in place of @n_del rows of @sl from
 * row @y, and give back what the owner did not keep.
 *
 * The owner adopts the lines when the splice succeeds but never the array
 * that holds them; when it refuses, the lines stay ours too.  Freeing both
 * here keeps a long walk's memory flat, so a sanitizer run of the driver
 * reports the owner's leaks and not the driver's own.
 * Return: the owner's return code.
 */
static int splice_lines(Slot *sl, int y, int n_del, STRING *lines, int n,
                        WpeBufferStep step)
{
 int rc = e_buffer_splice(&sl->buf, y, n_del, lines, n, NULL, 0, step);

 if (rc == E_BUFFER_OK)
  free(lines);
 else
  e_buffer_lines_free(lines, n);
 return(rc);
}

/**
 * load_text - Make the buffer of @sl hold @text and forget how it got there.
 */
static int load_text(Slot *sl, const char *text)
{
 STRING *lines;
 int n = 0, rc;

 lines = e_buffer_lines_from_text(text, sl->buf.mx.x, &n);
 if (n < 0)
  return(E_BUFFER_NOMEM);
 if (n == 0)
  rc = e_buffer_delete_lines(&sl->buf, 0, sl->buf.mxlines, E_BUFFER_STEP_COVERED);
 else
  rc = splice_lines(sl, 0, sl->buf.mxlines, lines, n, E_BUFFER_STEP_COVERED);
 e_buffer_history_drop(&sl->buf);
 sl->win.save = 0;
 return(rc);
}

static int do_splice(int y, int n_del, const char *text)
{
 STRING *lines;
 int n = 0;

 lines = e_buffer_lines_from_text(text, first.buf.mx.x, &n);
 if (n < 0)
  return(E_BUFFER_NOMEM);
 if (n == 0)
  return(e_buffer_delete_lines(&first.buf, y, n_del, E_BUFFER_STEP_NEW));
 return(splice_lines(&first, y, n_del, lines, n, E_BUFFER_STEP_NEW));
}

/**
 * do_move - Move or copy @n rows from @from to above row @y_to of @to, whole
 * or not at all.  The move is a transaction that makes its own line record,
 * so the owner alone can undo it (the editor's block records are replayed by
 * the editor, which this driver does not have).
 */
static int do_move(Slot *from, int y_from, int n, Slot *to, int y_to, int copy)
{
 WpeBufferTxn t;
 int rc;

 e_buffer_txn_begin(&t);
 if (copy)
  rc = e_buffer_txn_copy_lines(&t, &to->buf, y_to, &from->buf, y_from, n,
                               E_BUFFER_STEP_NEW);
 else
  rc = e_buffer_txn_move_lines(&t, &from->buf, y_from, n, &to->buf, y_to);
 /* Both return how many lines landed, or a negative refusal. */
 if (rc < 0)
 {
  e_buffer_txn_abort(&t);
  return(rc);
 }
 return(e_buffer_txn_commit(&t));
}

static int tokenize(char *line, char **tok)
{
 int n = 0;
 char *p = strtok(line, " \t\r\n");

 while (p != NULL && n < MAX_TOKENS)
 {
  tok[n++] = p;
  p = strtok(NULL, " \t\r\n");
 }
 return(n);
}

/**
 * run_command - Do one command; print the state after it.
 * Return: 0, or 1 when the command is not one.
 */
static int run_command(int n, char **tok)
{
 static char text[LINE_MAX_BYTES];
 int rc = E_BUFFER_OK, i, rows[MAX_BRP];
 const char *name = tok[0];

 if (strcmp(name, "open") == 0 && n == 3)
  slot_open(&first, atoi(tok[1]), atoi(tok[2]));
 else if (strcmp(name, "open2") == 0 && n == 2)
 {
  slot_open(&second, atoi(tok[1]), DEFAULT_NUMUNDO);
  load_text(&second, "");
 }
 else if (strcmp(name, "text") == 0 && n == 2)
 {
  unescape(tok[1], text, sizeof text);
  rc = load_text(&first, text);
 }
 else if (strcmp(name, "splice") == 0 && n == 4)
 {
  unescape(tok[3], text, sizeof text);
  rc = do_splice(atoi(tok[1]), atoi(tok[2]), text);
 }
 else if (strcmp(name, "delete") == 0 && n == 3)
  rc = e_buffer_delete_lines(&first.buf, atoi(tok[1]), atoi(tok[2]), E_BUFFER_STEP_NEW);
 else if (strcmp(name, "blank") == 0 && n == 2)
  rc = e_buffer_insert_blank(&first.buf, atoi(tok[1]), E_BUFFER_STEP_NEW);
 else if (strcmp(name, "split") == 0 && n == 3)
  rc = e_buffer_split_line(&first.buf, atoi(tok[1]), atoi(tok[2]), E_BUFFER_STEP_NEW,
                           E_BUFFER_HEAD_WR);
 else if (strcmp(name, "move") == 0 && n == 4)
  rc = do_move(&first, atoi(tok[1]), atoi(tok[2]), &first, atoi(tok[3]), 0);
 else if (strcmp(name, "copy") == 0 && n == 4)
  rc = do_move(&first, atoi(tok[1]), atoi(tok[2]), &first, atoi(tok[3]), 1);
 else if (strcmp(name, "xmove") == 0 && n == 4 && second.open)
  rc = do_move(&first, atoi(tok[1]), atoi(tok[2]), &second, atoi(tok[3]), 0);
 else if (strcmp(name, "undo") == 0 && n == 1)
  rc = e_buffer_undo(&first.buf, &first.scr);
 else if (strcmp(name, "redo") == 0 && n == 1)
  rc = e_buffer_redo(&first.buf, &first.scr);
 else if (strcmp(name, "fail") == 0 && n == 2)
  e_buffer_test_fail_allocs(atoi(tok[1]));
 else if (strcmp(name, "brp") == 0)
 {
  for (i = 1; i < n && i <= MAX_BRP; i++)
   rows[i - 1] = atoi(tok[i]);
  slot_set_breakpoints(&first, rows, n - 1);
 }
 else if (strcmp(name, "mark") == 0 && n == 5)
 {
  first.scr.mark_begin.y = atoi(tok[1]);
  first.scr.mark_begin.x = atoi(tok[2]);
  first.scr.mark_end.y = atoi(tok[3]);
  first.scr.mark_end.x = atoi(tok[4]);
 }
 else if (strcmp(name, "dump") != 0)
  return(1);
 print_state(name, rc);
 return(0);
}

int main(void)
{
 static char line[LINE_MAX_BYTES];
 char *tok[MAX_TOKENS];
 int n;

 slot_open(&first, DEFAULT_WIDTH, DEFAULT_NUMUNDO);
 while (fgets(line, sizeof line, stdin) != NULL)
 {
  n = tokenize(line, tok);
  if (n == 0)
   continue;
  if (run_command(n, tok) != 0)
  {
   printf("R %s rc=-99\nE\n", tok[0]);
   fflush(stdout);
  }
 }
 return(0);
}
