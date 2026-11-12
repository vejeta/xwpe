/* we_buffer.c -- the one owner of the text a window shows.
   Copyright (C) 2026 Juan Manuel Mendez Rey
   This file is part of xwpe and is distributed under the GNU General Public
   License, version 2 or later.

   Every change to the number of lines of a buffer is one splice: at line y,
   n_del lines go and n_ins lines come.  The splice keeps, for the window
   that owns the buffer and no other, its undo list, its dirty mark, its
   block marks, its breakpoint rows and its syntax state in step with the
   lines.  A buffer whose window has closed (b->f == NULL) keeps its lines
   alone.

   The file is pure by its imports: memory and string functions of the C
   library and the five seam functions of we_buffer.h, nothing else of the
   editor, so the C test links it alone. */

#include "we_buffer.h"

/* tests only: 0, or how many allocations from now the first refusal is */
static int buffer_fail_from = 0;

/**
 * e_buffer_test_fail_allocs - From the @n-th allocation of the owner on,
 * every allocation fails (0: none fails).
 *
 * Only the C test calls it: it proves that a change refused for want of
 * memory at any point leaves the user's text, marks and undo exactly as
 * they were.
 */
void e_buffer_test_fail_allocs(int n)
{
 buffer_fail_from = n;
}

/* The test fault armed at startup and not yet fired, and from which
   allocation of its action on memory is refused. */
static int buffer_fault_armed = E_BUFFER_FAULT_NONE;
static int buffer_fault_n = 0;

/* The names $XWPE_TEST_FAULT gives the points, indexed by E_BUFFER_FAULT_*. */
static const char *const buffer_fault_names[] = {
 "none", "block-move", "undo", "redo", "block-copy", "read"
};
#define BUFFER_FAULT_POINTS ((int)(sizeof buffer_fault_names / sizeof buffer_fault_names[0]))

/**
 * e_buffer_test_fault_point - The fault point a test names.
 * @name: "block-move", "undo", "redo", "block-copy" or "read".
 *
 * Lets a test of the running editor say which action runs out of memory.
 * Return: E_BUFFER_FAULT_*, or E_BUFFER_FAULT_NONE for a name it does not
 * know.
 */
int e_buffer_test_fault_point(const char *name)
{
 int p;

 for (p = E_BUFFER_FAULT_NONE + 1; p < BUFFER_FAULT_POINTS; p++)
  if (strcmp(name, buffer_fault_names[p]) == 0)
   return(p);
 return(E_BUFFER_FAULT_NONE);
}

/**
 * e_buffer_test_fault_arm - Arm one fault point: the first action that
 * enters @point has every owner allocation from its @n-th on refused.
 *
 * Only the startup of a test run arms one, so a test can prove that an
 * action refused for want of memory leaves the user's text as it was.
 */
void e_buffer_test_fault_arm(int point, int n)
{
 if (point <= E_BUFFER_FAULT_NONE || point >= BUFFER_FAULT_POINTS || n < 1)
  return;
 buffer_fault_armed = point;
 buffer_fault_n = n;
}

/**
 * e_buffer_test_fault_enter - An action that a test may starve of memory
 * begins.
 * @point: which one (E_BUFFER_FAULT_*).
 *
 * When the fault is armed for @point it fires now, once: memory is refused
 * from the armed allocation on, and the trace says so, so a test that named
 * the point wrongly cannot pass by accident.  An action entered again from
 * inside itself does not fire it a second time.
 * Return: E_BUFFER_FAULT_FIRED for the frame that fired it, else 0; the
 * caller hands it to e_buffer_test_fault_leave.
 */
int e_buffer_test_fault_enter(int point)
{
 if (point == E_BUFFER_FAULT_NONE || point != buffer_fault_armed)
  return(0);
 buffer_fail_from = buffer_fault_n;
 buffer_fault_armed = E_BUFFER_FAULT_NONE;
 wpe_buffer_trace("buffer fault %s armed n=%d", buffer_fault_names[point], buffer_fault_n);
 return(E_BUFFER_FAULT_FIRED);
}

/**
 * e_buffer_test_fault_leave - The action a fault was fired in ends: memory
 * is back.
 * @token: what e_buffer_test_fault_enter returned; only the frame that
 * fired the fault ends it.
 */
void e_buffer_test_fault_leave(int token)
{
 if (token == E_BUFFER_FAULT_FIRED)
  buffer_fail_from = 0;
}

static int buffer_alloc_refused(void)
{
 if (buffer_fail_from == 0)
  return(0);
 if (buffer_fail_from > 1)
 {
  buffer_fail_from--;
  return(0);
 }
 return(1);
}

static void *buffer_malloc(size_t n)
{
 if (buffer_alloc_refused())
  return(NULL);
 return(malloc(n ? n : 1));
}

static void *buffer_realloc(void *p, size_t n)
{
 if (buffer_alloc_refused())
  return(NULL);
 return(realloc(p, n ? n : 1));
}

/**
 * e_str_len - The number of bytes of a line before its end.
 * @s: a line as the buffer stores it.
 *
 * A line ends at its line break (WPE_WR) or at its terminating zero; every
 * column the user sees is counted with this.
 */
int e_str_len(unsigned char *s)
{
 int i;

 for (i = 0; *(s+i) != '\0' && *(s+i) != WPE_WR; i++)
  ;
 return (i);
}

/* ---- the window the buffer belongs to ---- */

/* The buffer keeps a window's state in step: a file window, not a view and
   not a buffer whose window has closed. */
static int buffer_keeps_history(BUFFER *b)
{
 return(b->f != NULL && b->history == E_BUFFER_HISTORY_FULL);
}

static int buffer_covered_now(BUFFER *b)
{
 return(b->covered > 0);
}

/**
 * e_buffer_covered - Would a change made now be left out of Undo?
 * @b: the buffer.
 *
 * The record of an operation is not taken while an outer record covers it
 * (a block operation, a character primitive), since replaying the outer one
 * recomputes the inner change.  Return: 1 when covered.
 */
int e_buffer_covered(BUFFER *b)
{
 return(buffer_covered_now(b));
}

/* The clipboard (window 0) shown in front, in its viewer.  Behind other
   windows it is the editor's to fill: every Copy does. */
static int buffer_is_clipboard_in_front(BUFFER *b)
{
 ECNT *cn = b->f->ed;

 if (cn == NULL || cn->mxedt < 1)
  return(0);
 return(b->f == cn->f[0] && cn->f[cn->mxedt] == b->f);
}

/* A buffer the user's commands may not change: a file opened read-only, or
   the clipboard in its viewer.  The editor's own views (Help, Messages, the
   panes) are written by the editor, never by a command, and stay open to it. */
static int buffer_closed_to_user(BUFFER *b)
{
 if (b->shell || b->f == NULL || b->f->ins != WPE_INS_READONLY)
  return(0);
 if (b->history == E_BUFFER_HISTORY_FULL)
  return(1);
 return(buffer_is_clipboard_in_front(b));
}

/**
 * e_buffer_change_refused - Must a change of the user's to @b be refused?
 * @b: the buffer the change is about to touch.
 *
 * A file opened read-only and the clipboard viewer keep their text through
 * every command, a block command, a line command, a replacement or a
 * formatter alike: the one place every character primitive and every
 * recorded splice asks, so no command can get past it.  The status row
 * says why nothing happened.
 * Return: 1 when the change must not be made, 0 when it may.
 */
int e_buffer_change_refused(BUFFER *b)
{
 if (!buffer_closed_to_user(b))
  return(0);
 wpe_buffer_trace("buffer read-only refused");
 wpe_buffer_say_read_only(b);
 return(1);
}

static int *buffer_brp(BUFFER *b)
{
#ifdef DEBUGGER
 if (buffer_keeps_history(b) && b->f->s != NULL)
  return(b->f->s->brp);
#endif
 return(NULL);
}

/* ---- lines ---- */

/* How much a byte buffer of the owner grows by at least, so a long line of
   a file being read is not copied once per byte. */
#define BUFFER_BYTES_STEP 128

/**
 * buffer_row_alloc - The memory of one empty row of a buffer @mx_x wide.
 * @out: receives the row.
 *
 * The one place a row of the owner gets its room: Max. Columns bytes of
 * text, the line break after them and the terminating zero, all zero from
 * the start.  So a line exactly as long as Max. Columns is one row with its
 * line break, and no byte past a row's end is ever read as text.
 * Return: E_BUFFER_OK or E_BUFFER_NOMEM.
 */
static int buffer_row_alloc(int mx_x, STRING *out)
{
 size_t room = (size_t)E_BUFFER_ROW_BYTES(mx_x);

 out->s = buffer_malloc(room);
 if (out->s == NULL)
  return(E_BUFFER_NOMEM);
 memset(out->s, 0, room);
 out->len = 0;
 out->nrc = 0;
 return(E_BUFFER_OK);
}

/**
 * e_buffer_line_new - Make one empty line the buffer can hold.
 * @b: the buffer (its width); @out: receives the line.
 *
 * Every line has room for the buffer's width and its line break, so typing
 * into it never outruns its memory.  Return: E_BUFFER_OK or E_BUFFER_NOMEM.
 */
int e_buffer_line_new(BUFFER *b, STRING *out)
{
 return(buffer_row_alloc(b->mx.x, out));
}

/**
 * e_buffer_bytes_reserve - Make room for @need bytes in a byte buffer the
 * owner's memory holds.
 * @bytes: the buffer (NULL for none yet); @cap: its room, updated.
 *
 * The file reader collects one line of the file here before the owner
 * wraps it into rows, so a test that starves the owner while a file is
 * read reaches this memory too.
 * Return: E_BUFFER_OK, or E_BUFFER_NOMEM with the buffer as it was.
 */
int e_buffer_bytes_reserve(unsigned char **bytes, int *cap, int need)
{
 unsigned char *more;
 int room = *cap;

 if (need <= room)
  return(E_BUFFER_OK);
 while (room < need)
  room = room * 2 + BUFFER_BYTES_STEP;
 if ((more = buffer_realloc(*bytes, (size_t)room)) == NULL)
  return(E_BUFFER_NOMEM);
 *bytes = more;
 *cap = room;
 return(E_BUFFER_OK);
}

static void buffer_line_measure(STRING *l)
{
 l->len = e_str_len(l->s);
 l->nrc = (int)strlen((char *)l->s);
}

/**
 * e_buffer_lines_free - Free lines nobody adopted.
 * @lines: the array; @n: how many.
 */
void e_buffer_lines_free(STRING *lines, int n)
{
 int i;

 if (lines == NULL)
  return;
 for (i = 0; i < n; i++)
  free(lines[i].s);
 free(lines);
}

static int buffer_lines_equal(const STRING *a, const STRING *b)
{
 return(a->len == b->len && a->s[a->len] == b->s[b->len] &&
        memcmp(a->s, b->s, (size_t)a->len) == 0);
}

/* ---- the width of a row ---- */

/* Does the row width rule hold in @b?  It does in a file's buffer and in a
   shell; a view the editor writes (Messages, Help, the panes) keeps a
   compiler's long line as one row. */
static int buffer_rows_bounded(const BUFFER *b)
{
 return(b->shell || !b->view);
}

/**
 * buffer_row_fits - Does row @l keep the shape every row of @b has?
 *
 * At most Max. Columns bytes of text, ended by the line break or by the
 * zero (a row whose line goes on below), the zero right after the break,
 * and nothing but zero in the last byte of its room.  A row that keeps it
 * is saved with every byte and typed into without reaching past its
 * memory.  Return: 1 when it fits.
 */
static int buffer_row_fits(const BUFFER *b, const STRING *l)
{
 int hard;

 if (l->s == NULL || l->len < 0 || l->len > E_BUFFER_ROW_TEXT_MAX(b))
  return(0);
 if (l->s[l->len] != WPE_WR && l->s[l->len] != '\0')
  return(0);
 hard = (l->s[l->len] == WPE_WR);
 if (l->nrc != l->len + hard || l->s[l->nrc] != '\0')
  return(0);
 return(l->s[E_BUFFER_ROW_BYTES(b->mx.x) - 1] == '\0');
}

/* Can every one of @n lines go into @b?  The first that cannot is traced,
   so a refused change names the row that was too wide.  @y: the row the
   first line would take.  Return: 1, or 0 for a refusal. */
static int buffer_lines_fit(const BUFFER *b, int y, const STRING *lines, int n)
{
 int i;

 if (!buffer_rows_bounded(b) || lines == NULL)
  return(1);
 for (i = 0; i < n; i++)
  if (!buffer_row_fits(b, &lines[i]))
  {
   wpe_buffer_trace("buffer row wider than the window y=%d len=%d nrc=%d width=%d",
                    y + i, lines[i].len, lines[i].nrc, b->mx.x);
   return(0);
  }
 return(1);
}

/**
 * e_buffer_rows_check - The first row of @b that does not keep the row
 * shape, or -1.
 *
 * The test dump writes it after every key, so a primitive that wrote a row
 * past Max. Columns in place, where no splice sees it, shows at once.  A
 * view is never checked.
 */
int e_buffer_rows_check(BUFFER *b)
{
 int y;

 if (!buffer_rows_bounded(b))
  return(-1);
 for (y = 0; y < b->mxlines; y++)
  if (!buffer_row_fits(b, &b->bf[y]))
   return(y);
 return(-1);
}

/* ---- the debugger's margin: brp[0] is the count, brp[1..] the rows ---- */

static int brp_has(const int *brp, int row)
{
 int i;

 for (i = 1; i <= brp[0]; i++)
  if (brp[i] == row)
   return(1);
 return(0);
}

static int brp_count_in(const int *brp, int lo, int hi)
{
 int i, n = 0;

 if (brp == NULL)
  return(0);
 for (i = 1; i <= brp[0]; i++)
  if (brp[i] >= lo && brp[i] < hi)
   n++;
 return(n);
}

/* ---- marks ---- */

#define BUFFER_MARKS_MAX 4

/* The marks a splice keeps in step: the block and the last find.  A mark
   with a negative line is unset.  The debugger's current-line mark is not
   one: its end column means "to the end of the line" and the next stop
   sets it again. */
static int buffer_marks(BUFFER *b, POINT **m)
{
 SCHIRM *s;
 int n = 0;

 if (b->f == NULL || (s = b->f->s) == NULL)
  return(0);
 m[n++] = &s->mark_begin;
 m[n++] = &s->mark_end;
 m[n++] = &s->fa;
 m[n++] = &s->fe;
 return(n);
}

static void buffer_point_clamp(BUFFER *b, POINT *p)
{
 if (p->y < 0)
  return;
 if (p->y >= b->mxlines)
  p->y = b->mxlines - 1;
 if (p->x > b->bf[p->y].len)
  p->x = b->bf[p->y].len;
 if (p->x < 0)
  p->x = 0;
}

/* Move one mark for a splice at y of n_del lines over n_ins, of which the
   first `changed` are the same rows changed in place. */
static void buffer_mark_move(BUFFER *b, POINT *p, int y, int n_del, int n_ins,
                             int changed)
{
 if (p->y < y)
  return;
 if (p->y < y + changed)
  return;
 if (p->y < y + n_del)
 {
  if (y + n_ins < b->mxlines)
  {
   p->y = y + n_ins;
   p->x = 0;
  }
  else
  {
   p->y = b->mxlines - 1;
   p->x = b->bf[p->y].len;
  }
  return;
 }
 p->y += n_ins - n_del;
}

static void buffer_marks_move(BUFFER *b, int y, int n_del, int n_ins, int changed)
{
 POINT *m[BUFFER_MARKS_MAX];
 int i, n = buffer_marks(b, m);

 for (i = 0; i < n; i++)
 {
  if (m[i]->y < 0)
   continue;
  buffer_mark_move(b, m[i], y, n_del, n_ins, changed);
  buffer_point_clamp(b, m[i]);
 }
 buffer_point_clamp(b, &b->b);
}

/* ---- syntax state ---- */

static void buffer_syntax_note(BUFFER *b, int lo, int hi)
{
 if (b->syn_lo < 0 || lo < b->syn_lo)
  b->syn_lo = lo;
 if (hi > b->syn_hi)
  b->syn_hi = hi;
}

/* At cover and step depth 0: one recompute from the lowest line touched. */
static void buffer_syntax_flush(BUFFER *b)
{
 if (b->covered > 0 || b->step_depth > 0 || b->syn_lo < 0)
  return;
 if (b->f != NULL && b->f->c_sw != NULL)
  wpe_buffer_syntax_changed(b, b->syn_lo);
 b->syn_lo = -1;
 b->syn_hi = -1;
}

/* Move the syntax states with the lines: the first row of the new span
   starts in the state the old span started in, so the recompute begins
   from a true state. */
static void buffer_syntax_move(BUFFER *b, int y, int n_del, int n_ins, int old_mxlines)
{
 int *c_sw, seed, i, tail;

 if (b->f == NULL || (c_sw = b->f->c_sw) == NULL)
  return;
 if (y < old_mxlines)
  seed = c_sw[y];
 else if (y > 0)
  seed = c_sw[y-1];
 else
  seed = 0;
 tail = old_mxlines - y - n_del;
 if (tail > 0 && n_del != n_ins)
  memmove(c_sw + y + n_ins, c_sw + y + n_del, (size_t)tail * sizeof(int));
 for (i = y; i < y + n_ins; i++)
  c_sw[i] = seed;
 buffer_syntax_note(b, y, y + n_ins);
}

/* ---- records ---- */

/**
 * e_buffer_record_new - A zeroed undo record of one kind.
 * @kind: 'a' 'r' 's' 'p' 'c' 'v' 'd' or E_UNDO_LINES.
 * Return: the record, or NULL when memory ran out.
 */
Undo *e_buffer_record_new(int kind)
{
 Undo *rec = buffer_malloc(sizeof(Undo));

 if (rec == NULL)
  return(NULL);
 memset(rec, 0, sizeof(Undo));
 rec->type = kind;
 return(rec);
}

/**
 * e_buffer_record_text - Keep in a character record the @n bytes it must
 * put back.
 * @rec: an 'r' or 's' record; @src: the bytes, read now.
 *
 * Undo of a deletion or a replacement types these bytes again.  The copy is
 * the owner's memory, so a test that starves the owner reaches it too.
 * Return: E_BUFFER_OK, or E_BUFFER_NOMEM with the record unchanged.
 */
int e_buffer_record_text(Undo *rec, const unsigned char *src, int n)
{
 unsigned char *copy;

 if (n < 0)
  n = 0;
 if ((copy = buffer_malloc((size_t)n)) == NULL)
  return(E_BUFFER_NOMEM);
 if (n > 0)
  memcpy(copy, src, (size_t)n);
 rec->u.pt = copy;
 return(E_BUFFER_OK);
}

/* The window and screen a shell has of its own; the buffer itself comes
   from e_buffer_init.  Return: E_BUFFER_OK or E_BUFFER_NOMEM (nothing
   kept). */
static int buffer_shell_window(BUFFER *shell)
{
 FENSTER *fn = buffer_malloc(sizeof(FENSTER));
 SCHIRM *sn;

 if (fn == NULL)
  return(E_BUFFER_NOMEM);
 if ((sn = buffer_malloc(sizeof(SCHIRM))) == NULL)
 {
  free(fn);
  return(E_BUFFER_NOMEM);
 }
 memset(fn, 0, sizeof(FENSTER));
 memset(sn, 0, sizeof(SCHIRM));
 fn->s = sn;
 fn->b = shell;
 fn->ed = NULL;
 shell->f = fn;
 return(E_BUFFER_OK);
}

/* Did e_buffer_init give the shell all it needs: its line and, with the
   debugger, its margin? */
static int buffer_shell_complete(BUFFER *shell)
{
 if (shell->mxlines != 1)
  return(0);
#ifdef DEBUGGER
 if (shell->f->s->brp == NULL)
  return(0);
#endif
 return(1);
}

/**
 * e_buffer_shell_new - A private buffer like @like, @width bytes wide, with
 * one empty line.
 * @like: the buffer whose lines the shell holds for a while (its editor
 * and colours); @width: the width of the shell's lines.
 *
 * A block delete moves the block's lines into a shell its Undo record
 * keeps, so the breakpoints of those lines wait in it until Undo brings
 * them back.  A character Undo replays its primitive on a shell holding
 * copies of the rows it may touch: refused there for want of memory, the
 * user's text was never touched.  Every piece is the owner's memory, so a
 * test that starves the owner reaches it.
 * Return: the shell, or NULL when memory ran out.
 */
BUFFER *e_buffer_shell_new(BUFFER *like, int width)
{
 BUFFER *shell = buffer_malloc(sizeof(BUFFER));

 if (shell == NULL)
  return(NULL);
 memset(shell, 0, sizeof(BUFFER));
 if (buffer_shell_window(shell) != E_BUFFER_OK)
 {
  free(shell);
  return(NULL);
 }
 e_buffer_init(shell, like->cn, shell->f, width, E_BUFFER_HISTORY_FULL);
 shell->shell = 1;
 if (!buffer_shell_complete(shell))
 {
  e_buffer_shell_free(shell);
  return(NULL);
 }
 shell->fb = like->fb;
 shell->f->s->fb = like->fb;
 return(shell);
}

/**
 * e_buffer_is_shell - Is @b a shell the owner made?
 *
 * A refusal inside a shell is counted there, not said on the status row:
 * the change it belongs to is refused whole and says so once.
 */
int e_buffer_is_shell(BUFFER *b)
{
 return(b != NULL && b->shell);
}

/**
 * e_buffer_shell_refused - A change inside shell @b was refused for want of
 * memory.
 *
 * The replay that ran on the shell is then refused whole, so the user's
 * text, breakpoints and both Undo lists stay as they were.
 */
void e_buffer_shell_refused(BUFFER *b)
{
 if (e_buffer_is_shell(b))
  b->refusals++;
}

/**
 * e_buffer_shell_free - Free a shell: its lines, its window and screen, and
 * whatever Undo records were pushed in it.
 * @shell: the buffer, with its window and screen.
 */
void e_buffer_shell_free(BUFFER *shell)
{
 int i;

 if (shell == NULL)
  return;
 e_buffer_history_drop(shell);
 if (shell->f != NULL)
 {
  if (shell->f->s != NULL)
  {
#ifdef DEBUGGER
   free(shell->f->s->brp);
#endif
   free(shell->f->s);
  }
  free(shell->f);
 }
 if (shell->bf != NULL)
 {
  for (i = 0; i < shell->mxlines; i++)
   free(shell->bf[i].s);
  free(shell->bf);
 }
 free(shell);
}

/**
 * e_buffer_record_free - Free one undo record and what it holds.
 * @rec: a record no list holds any more.
 */
void e_buffer_record_free(Undo *rec)
{
 if (rec == NULL)
  return;
 if (rec->type == E_UNDO_LINES)
  e_buffer_lines_free(rec->old, rec->n_old);
 else if (rec->type == 'r' || rec->type == 's')
  free(rec->u.pt);
 else if (rec->type == 'd')
  e_buffer_shell_free((BUFFER *) rec->u.pt);
 free(rec->brk);
 free(rec);
}

static void buffer_list_free(Undo *ud)
{
 Undo *next;

 for (; ud != NULL; ud = next)
 {
  next = ud->next;
  e_buffer_record_free(ud);
 }
}

/* Keep the user's last `steps` actions on the list, freeing older ones.  A
   step is one record without E_UNDO_MORE and the MORE records above it. */
static void buffer_list_trim(Undo *ud, int steps)
{
 int seen = 0;

 if (steps < 1)
  steps = 1;
 for (; ud != NULL; ud = ud->next)
 {
  if (ud->flags & E_UNDO_MORE)
   continue;
  if (++seen == steps)
  {
   buffer_list_free(ud->next);
   ud->next = NULL;
   return;
  }
 }
}

/* How many actions Ctrl-U can take back: the user's number of undo steps
   counts the actions before the newest one, as it always has, so a setting
   of 10 undoes 11 actions. */
static int buffer_undo_steps_kept(BUFFER *b)
{
 return(b->cn->numundo + 1);
}

/* Link a record that must be kept: on the list the replay names, MORE
   inside an open step, the redo list cleared by a fresh record, the undo
   list bounded by the user's number of undo steps. */
static void buffer_record_link(BUFFER *b, Undo *rec)
{
 if (b->step_depth > 0)
 {
  if (b->step_pushed > 0)
   rec->flags |= E_UNDO_MORE;
  b->step_pushed++;
 }
 if (b->replaying == E_BUFFER_REPLAY_UNDO)
 {
  rec->next = b->rd;
  b->rd = rec;
  return;
 }
 if (b->replaying == E_BUFFER_REPLAY_NONE && b->rd != NULL)
 {
  buffer_list_free(b->rd);
  b->rd = NULL;
 }
 rec->next = b->ud;
 b->ud = rec;
 if (b->cn != NULL)
  buffer_list_trim(b->ud, buffer_undo_steps_kept(b));
}

/* Which list a record was pushed for, as the trace says it: a fresh change
   ("none"), or the inverse a Ctrl-U ("undo") or a Ctrl-R ("redo") left, so
   a test counts the user's own steps apart from what a replay pushes. */
static const char *buffer_replay_name(const BUFFER *b)
{
 if (b->replaying == E_BUFFER_REPLAY_UNDO)
  return("undo");
 if (b->replaying == E_BUFFER_REPLAY_REDO)
  return("redo");
 return("none");
}

/* 1 when the record goes on into the one pushed next, as the trace says it. */
static int buffer_record_more(const Undo *rec)
{
 if (rec->flags & E_UNDO_MORE)
  return(1);
 return(0);
}

/**
 * e_buffer_record_push - Keep the record of a change for Ctrl-U.
 * @b: the buffer; @rec: a record the caller composed.
 *
 * The character and block operations push here.  A record made under a
 * cover, or on a view the user does not save, is freed: replaying the outer
 * record recomputes the change, and a view has no undo.
 * Return: the record as kept, or NULL when it was freed.
 */
Undo *e_buffer_record_push(BUFFER *b, Undo *rec)
{
 if (rec == NULL)
  return(NULL);
 if (b->history != E_BUFFER_HISTORY_FULL || buffer_covered_now(b))
 {
  e_buffer_record_free(rec);
  return(NULL);
 }
 buffer_record_link(b, rec);
 wpe_buffer_trace("buffer record kind=%c more=%d replay=%s", rec->type,
                  buffer_record_more(rec), buffer_replay_name(b));
 return(rec);
}

/* ---- the splice ---- */

static void buffer_splice_mem_free(BufferSpliceMem *m)
{
 free(m->canon.s);
 if (m->rec != NULL)
 {
  free(m->rec->old);
  free(m->rec->brk);
  free(m->rec);
 }
 free(m->removed);
 free(m->added);
 memset(m, 0, sizeof(*m));
}

/* (a) the table and the syntax states grow together: mx.y and c_sw change
   only when both could. */
static int buffer_grow(BUFFER *b, int new_count)
{
 int newy = b->mx.y;
 STRING *tmp;
 int *tmp2;

 while (new_count > newy - 2)
  newy += MAXLINES;
 if (newy == b->mx.y)
  return(E_BUFFER_OK);
 tmp = buffer_realloc(b->bf, (size_t)newy * sizeof(STRING));
 if (tmp == NULL)
  return(E_BUFFER_NOMEM);
 b->bf = tmp;
 if (b->f != NULL && b->f->c_sw != NULL)
 {
  tmp2 = buffer_realloc(b->f->c_sw, (size_t)newy * sizeof(int));
  if (tmp2 == NULL)
   return(E_BUFFER_NOMEM);
  b->f->c_sw = tmp2;
 }
 b->mx.y = newy;
 return(E_BUFFER_OK);
}

/* Room for the rows a splice adds to the margin. */
static int buffer_brp_reserve(BUFFER *b, int extra)
{
#ifdef DEBUGGER
 int *tmp;

 if (extra <= 0 || buffer_brp(b) == NULL)
  return(E_BUFFER_OK);
 tmp = buffer_realloc(b->f->s->brp, (size_t)(b->f->s->brp[0] + extra + 1) * sizeof(int));
 if (tmp == NULL)
  return(E_BUFFER_NOMEM);
 b->f->s->brp = tmp;
#endif
 return(E_BUFFER_OK);
}

/* Room in the pocket for rows a covered deletion removes. */
static int buffer_pocket_reserve(Undo *pocket, int extra)
{
 int *tmp;

 if (pocket == NULL || extra <= 0)
  return(E_BUFFER_OK);
 tmp = buffer_realloc(pocket->brk, (size_t)(pocket->nbrk + extra) * sizeof(int));
 if (tmp == NULL)
  return(E_BUFFER_NOMEM);
 pocket->brk = tmp;
 return(E_BUFFER_OK);
}

static int buffer_splice_records(BUFFER *b, WpeBufferStep step)
{
 if (b->history != E_BUFFER_HISTORY_FULL)
  return(0);
 if (step == E_BUFFER_STEP_REPLAY)
  return(1);
 return(step == E_BUFFER_STEP_NEW && !buffer_covered_now(b));
}

static Undo *buffer_splice_pocket(BUFFER *b, int records)
{
 if (records || !buffer_keeps_history(b))
  return(NULL);
 return(b->pocket);
}

/* An array of n items of size bytes, or NULL for none (n <= 0) or when
   memory ran out: the caller tells the two apart by n. */
static void *buffer_array(int n, size_t size)
{
 if (n <= 0)
  return(NULL);
 return(buffer_malloc((size_t)n * size));
}

/* The line record a recording splice keeps, with room for the n_del lines
   it replaces and the rows_out breakpoint rows those carry.  Return: the
   record, or NULL with nothing kept when memory ran out. */
static Undo *buffer_line_record_new(int n_del, int rows_out)
{
 Undo *rec = e_buffer_record_new(E_UNDO_LINES);

 if (rec == NULL)
  return(NULL);
 rec->old = buffer_array(n_del, sizeof(STRING));
 if (n_del > 0 && rec->old == NULL)
 {
  free(rec);
  return(NULL);
 }
 rec->brk = buffer_array(rows_out, sizeof(int));
 if (rows_out > 0 && rec->brk == NULL)
 {
  free(rec->old);
  free(rec);
  return(NULL);
 }
 return(rec);
}

/* Take the memory of one splice of n_del lines over n_ins, the buffer
   holding lines_now lines when it commits: the table's growth, the
   canonical line of a buffer emptied, the record, and room for rows_out
   removed and nbrk added breakpoint rows.  Nothing moves.  The margin and
   the pocket are not reserved here: they are reallocated to a size counted
   from now, so a change of several splices reserves them once. */
static int buffer_splice_take(BUFFER *b, int lines_now, int n_del, int n_ins, int nbrk,
                              int rows_out, int records, int detach, BufferSpliceMem *m)
{
 int *brp = buffer_brp(b);
 int n_total = n_ins;

 memset(m, 0, sizeof(*m));
 m->need_canon = (lines_now - n_del + n_ins == 0);
 if (m->need_canon)
  n_total = 1;
 if (buffer_grow(b, lines_now - n_del + n_total) != E_BUFFER_OK)
  return(E_BUFFER_NOMEM);
 if (m->need_canon && e_buffer_line_new(b, &m->canon) != E_BUFFER_OK)
  return(E_BUFFER_NOMEM);
 if (records && !detach)
 {
  m->rec = buffer_line_record_new(n_del, rows_out);
  if (m->rec == NULL)
  {
   buffer_splice_mem_free(m);
   return(E_BUFFER_NOMEM);
  }
 }
 m->removed = buffer_array(rows_out, sizeof(int));
 if (brp != NULL)
  m->added = buffer_array(nbrk, sizeof(int));
 if ((rows_out > 0 && m->removed == NULL) ||
     (nbrk > 0 && brp != NULL && m->added == NULL))
 {
  buffer_splice_mem_free(m);
  return(E_BUFFER_NOMEM);
 }
 return(E_BUFFER_OK);
}

/* Take every allocation the splice needs; nothing moves yet.  pocket_room:
   the pocket to make room in for the rows the splice removes, or NULL when
   the caller made that room already. */
static int buffer_splice_prepare(BUFFER *b, int y, int n_del, int n_ins, int nbrk,
                                 int records, int detach, Undo *pocket_room,
                                 BufferSpliceMem *m)
{
 int rows_out = brp_count_in(buffer_brp(b), y, y + n_del);

 if (buffer_splice_take(b, b->mxlines, n_del, n_ins, nbrk, rows_out, records, detach, m)
     != E_BUFFER_OK)
  return(E_BUFFER_NOMEM);
 if (buffer_brp_reserve(b, nbrk) != E_BUFFER_OK ||
     buffer_pocket_reserve(pocket_room, rows_out) != E_BUFFER_OK)
 {
  buffer_splice_mem_free(m);
  return(E_BUFFER_NOMEM);
 }
 return(E_BUFFER_OK);
}

/* The margin: rows on removed lines leave it (into the record, the pocket,
   the detach's hands, or nobody), rows below move, rows from brk[] arrive. */
static void buffer_brp_move(BUFFER *b, int y, int n_del, int n_ins, int changed,
                            const int *brk, int nbrk, BufferSpliceMem *m,
                            Undo *pocket, int *brk_out, int *nbrk_out)
{
 int *brp = buffer_brp(b);
 int i, k, row;

 if (brp == NULL)
  return;
 for (i = 1, k = 1; i <= brp[0]; i++)
 {
  row = brp[i];
  if (row >= y + changed && row < y + n_del)
  {
   m->removed[m->nremoved++] = row;
   if (m->rec != NULL)
    m->rec->brk[m->rec->nbrk++] = row - y;
   else if (brk_out != NULL)
    brk_out[(*nbrk_out)++] = row - y;
   else if (pocket != NULL)
    pocket->brk[pocket->nbrk++] = row - pocket->b.y;
   continue;
  }
  if (row >= y + n_del)
   row += n_ins - n_del;
  brp[k++] = row;
 }
 brp[0] = k - 1;
 for (i = 0; i < nbrk && brk != NULL; i++)
 {
  row = y + brk[i];
  if (brk[i] < 0 || brk[i] >= n_ins || brp_has(brp, row))
   continue;
  brp[++brp[0]] = row;
  m->added[m->nadded++] = row;
 }
}

/* How a splice moves the breakpoint margin. */
#define BUFFER_ROWS_POSITIONAL 0   /* changed rows keep theirs: buffer_brp_move */
#define BUFFER_ROWS_REWRITE    1   /* the span's rows are brk[] alone: buffer_brp_rewrite */

/* Does brk[] (relative to the span) set row rel of a span of n_ins lines?
   Only its first k entries are looked at. */
static int buffer_brk_holds(const int *brk, int k, int rel, int n_ins)
{
 int i;

 if (rel < 0 || rel >= n_ins)
  return(0);
 for (i = 0; i < k && brk != NULL; i++)
  if (brk[i] == rel)
   return(1);
 return(0);
}

/* The margin after a rewrite: the span's new lines were made from copies,
   so no row of the old span stays by position; the rows the new lines
   carry, brk[] relative to y, arrive, and the rows below move.  For the
   debugger, which drops [y+k, y+n_del) itself and moves the rows below
   (k the lines kept in place), m->removed holds the rows in [y, y+k) that
   lose their breakpoint and m->added the rows of the span that hold one
   now and either lie past y+k or held none before. */
static void buffer_brp_rewrite(BUFFER *b, int y, int n_del, int n_ins, const int *brk,
                               int nbrk, BufferSpliceMem *m)
{
 int *brp = buffer_brp(b);
 int i, k, row, kept = n_del < n_ins ? n_del : n_ins;

 if (brp == NULL)
  return;
 for (i = 0; i < nbrk && brk != NULL; i++)
 {
  if (brk[i] < 0 || brk[i] >= n_ins || buffer_brk_holds(brk, i, brk[i], n_ins))
   continue;
  if (brk[i] >= kept || !brp_has(brp, y + brk[i]))
   m->added[m->nadded++] = y + brk[i];
 }
 for (i = 1, k = 1; i <= brp[0]; i++)
 {
  row = brp[i];
  if (row >= y && row < y + n_del)
  {
   if (row < y + kept && !buffer_brk_holds(brk, nbrk, row - y, n_ins))
    m->removed[m->nremoved++] = row;
   continue;
  }
  if (row >= y + n_del)
   row += n_ins - n_del;
  brp[k++] = row;
 }
 brp[0] = k - 1;
 for (i = 0; i < nbrk && brk != NULL; i++)
 {
  row = y + brk[i];
  if (brk[i] < 0 || brk[i] >= n_ins || brp_has(brp, row))
   continue;
  brp[++brp[0]] = row;
 }
}

static const char *buffer_step_name(WpeBufferStep step)
{
 if (step == E_BUFFER_STEP_NEW)
  return("new");
 if (step == E_BUFFER_STEP_COVERED)
  return("covered");
 return("replay");
}

/* The second half of a splice, with its memory taken: it cannot fail, so a
   change made of several splices whose memory was all taken first is made
   whole or not at all.  Arguments as buffer_splice; rows: BUFFER_ROWS_*;
   m: what buffer_splice_prepare took, used up and freed here. */
static void buffer_splice_commit(BUFFER *b, int y, int n_del, STRING *lines, int n_ins,
                                 const int *brk, int nbrk, WpeBufferStep step,
                                 int no_changed, STRING *out, int *brk_out, int *nbrk_out,
                                 Undo *pocket, int rows, BufferSpliceMem *m)
{
 int changed, old_mxlines, i, delta;

 if (m->need_canon)
 {
  lines = &m->canon;
  n_ins = 1;
  no_changed = 1;
 }
 changed = no_changed ? 0 : (n_del < n_ins ? n_del : n_ins);
 if (m->rec != NULL)
 {
  m->rec->b = b->b;
  m->rec->span = y;
  m->rec->n_old = n_del;
  m->rec->n_new = n_ins;
  if (no_changed)
   m->rec->flags |= E_UNDO_EMPTIED;
  if (b->f != NULL && b->f->s != NULL)
  {
   m->rec->a = b->f->s->mark_begin;
   m->rec->e = b->f->s->mark_end;
  }
 }
 if (out != NULL && nbrk_out != NULL)
  *nbrk_out = 0;
 if (rows == BUFFER_ROWS_REWRITE)
  buffer_brp_rewrite(b, y, n_del, n_ins, brk, nbrk, m);
 else
  buffer_brp_move(b, y, n_del, n_ins, changed, brk, nbrk, m, pocket, brk_out, nbrk_out);
 for (i = 0; i < n_del; i++)
 {
  if (m->rec != NULL)
   m->rec->old[i] = b->bf[y+i];
  else if (out != NULL)
   out[i] = b->bf[y+i];
  else
   free(b->bf[y+i].s);
 }
 old_mxlines = b->mxlines;
 delta = n_ins - n_del;
 if (old_mxlines - y - n_del > 0 && delta != 0)
  memmove(b->bf + y + n_ins, b->bf + y + n_del,
          (size_t)(old_mxlines - y - n_del) * sizeof(STRING));
 for (i = 0; i < n_ins; i++)
  b->bf[y+i] = lines[i];
 m->canon.s = NULL;             /* adopted, when it was made */
 b->mxlines = old_mxlines + delta;
 buffer_syntax_move(b, y, n_del, n_ins, old_mxlines);
 buffer_marks_move(b, y, n_del, n_ins, changed);
 if (buffer_keeps_history(b))
  b->f->save += E_BUFFER_SAVE_PER_LINE * (n_del + n_ins);
 if (buffer_keeps_history(b))
  wpe_buffer_lines_moved(b, y, n_del, n_ins, m->removed, m->nremoved, m->added, m->nadded);
 if (m->rec != NULL)
 {
  buffer_record_link(b, m->rec);
  wpe_buffer_trace("buffer record kind=R n_old=%d n_new=%d more=%d replay=%s",
                   m->rec->n_old, m->rec->n_new, buffer_record_more(m->rec),
                   buffer_replay_name(b));
  m->rec = NULL;
 }
 wpe_buffer_trace("buffer splice y=%d del=%d ins=%d step=%s covered=%d", y, n_del,
                  n_ins, buffer_step_name(step), b->covered);
 buffer_splice_mem_free(m);
 buffer_syntax_flush(b);
}

/* The splice itself.  no_changed: no row of the span is a changed row (a
   buffer emptied, or the replay of such a record).  detach: the removed
   lines go to out[] with their rows instead of being freed or recorded. */
static int buffer_splice(BUFFER *b, int y, int n_del, STRING *lines, int n_ins,
                         const int *brk, int nbrk, WpeBufferStep step,
                         int no_changed, STRING *out, int *brk_out, int *nbrk_out)
{
 BufferSpliceMem m;
 int records, i;
 Undo *pocket;

 if (y < 0 || n_del < 0 || n_ins < 0 || y > b->mxlines)
  return(E_BUFFER_RANGE);
 if (y == b->mxlines && n_del > 0)
  return(E_BUFFER_RANGE);
 if (n_del > b->mxlines - y)
  n_del = b->mxlines - y;
 if (n_del == 0 && n_ins == 0)
  return(E_BUFFER_OK);
 if (!no_changed && out == NULL && n_del == n_ins)
 {
  for (i = 0; i < n_del && buffer_lines_equal(&b->bf[y+i], &lines[i]); i++)
   ;
  if (i == n_del)
  {
   for (i = 0; i < n_ins; i++)
    free(lines[i].s);
   return(E_BUFFER_OK);
  }
 }
 if (!buffer_lines_fit(b, y, lines, n_ins))
  return(E_BUFFER_RANGE);
 records = buffer_splice_records(b, step);
 pocket = buffer_splice_pocket(b, records);
 if (buffer_splice_prepare(b, y, n_del, n_ins, nbrk, records, out != NULL, pocket, &m) != E_BUFFER_OK)
 {
  e_buffer_shell_refused(b);
  return(E_BUFFER_NOMEM);
 }
 buffer_splice_commit(b, y, n_del, lines, n_ins, brk, nbrk, step, no_changed, out,
                      brk_out, nbrk_out, pocket, BUFFER_ROWS_POSITIONAL, &m);
 return(E_BUFFER_OK);
}

/**
 * e_buffer_splice - Replace n_del lines at y with n_ins new ones.
 * @b: the buffer; @y: the first line; @n_del: lines that go (clamped to the
 * end); @lines: the lines that come, adopted on success (the array stays the
 * caller's); @n_ins: how many; @brk: breakpoint rows to set, relative to y
 * (NULL for none); @nbrk: how many; @step: how the change counts for Ctrl-U.
 *
 * The one way the number of lines of a buffer changes.  The user's block,
 * breakpoints, colours and modified mark follow the lines, and Ctrl-U gives
 * back exactly what was there.  A buffer is never left without a line: the
 * owner adds an empty one.  A change of its own (a new step) to a window the
 * user cannot change is refused, and the status row says why.
 * Return: E_BUFFER_OK, E_BUFFER_NOMEM (nothing changed, the lines stay the
 * caller's), E_BUFFER_LOCKED (the same) or E_BUFFER_RANGE.
 */
int e_buffer_splice(BUFFER *b, int y, int n_del, STRING *lines, int n_ins,
                    const int *brk, int nbrk, WpeBufferStep step)
{
 if (step == E_BUFFER_STEP_NEW && e_buffer_change_refused(b))
  return(E_BUFFER_LOCKED);
 return(buffer_splice(b, y, n_del, lines, n_ins, brk, nbrk, step, 0, NULL, NULL, NULL));
}

/* A splice the editor makes on its own account through the calls below that
   take whole texts (its views, the clipboard a Copy fills, a tool's rewrite):
   not a command of the user's, so never refused for a window the user
   cannot change. */
static int buffer_editor_splice(BUFFER *b, int y, int n_del, STRING *lines, int n_ins,
                                WpeBufferStep step)
{
 return(buffer_splice(b, y, n_del, lines, n_ins, NULL, 0, step, 0, NULL, NULL, NULL));
}

/**
 * e_buffer_insert_blank - Put an empty line at y.
 * Return: as e_buffer_splice.
 */
int e_buffer_insert_blank(BUFFER *b, int y, WpeBufferStep step)
{
 STRING line;
 int r;

 if (e_buffer_line_new(b, &line) != E_BUFFER_OK)
  return(E_BUFFER_NOMEM);
 r = e_buffer_splice(b, y, 0, &line, 1, NULL, 0, step);
 if (r != E_BUFFER_OK)
  free(line.s);
 return(r);
}

/**
 * e_buffer_delete_lines - Take n lines away from y (to the end at most).
 * Return: as e_buffer_splice.
 */
int e_buffer_delete_lines(BUFFER *b, int y, int n, WpeBufferStep step)
{
 if (y >= b->mxlines && y > 0)
  return(E_BUFFER_RANGE);
 return(e_buffer_splice(b, y, n, NULL, 0, NULL, 0, step));
}

/**
 * e_buffer_open_row - Give a character change a row at @y to write in.
 * @b: the buffer; @y: the row the change, or the record it replays, names.
 *
 * A character record names the row its text goes back to.  When the
 * deletion it records emptied the last row of the text, that row went with
 * it, and the record (or the change still running under its cover) names
 * the row just past the text.  The owner opens that row again, empty and
 * with no line break, before anything is written there: Undo of Ctrl-T on
 * the last word of a file puts the word back on its own line, instead of
 * into memory the row no longer has.  A row inside the text needs nothing.
 * The new row has no record of its own: the record of the change that
 * fills it takes it away again.
 * Return: E_BUFFER_OK, E_BUFFER_NOMEM (nothing changed), or E_BUFFER_RANGE
 * for any row further out, which no record can rightly name (traced,
 * nothing written).
 */
int e_buffer_open_row(BUFFER *b, int y)
{
 if (y >= 0 && y < b->mxlines)
  return(E_BUFFER_OK);
 if (y != b->mxlines || y == 0)
 {
  wpe_buffer_trace("buffer change past the text y=%d lines=%d", y, b->mxlines);
  return(E_BUFFER_RANGE);
 }
 return(e_buffer_insert_blank(b, y, E_BUFFER_STEP_COVERED));
}

/* A copy of `n` bytes of `src` as a line of the buffer, with `term` as the
   byte after them (WPE_WR or '\0'): at most a row's text, Max. Columns. */
static int buffer_line_from_bytes(BUFFER *b, const unsigned char *src, int n, int term,
                                  STRING *out)
{
 if (e_buffer_line_new(b, out) != E_BUFFER_OK)
  return(E_BUFFER_NOMEM);
 if (n > E_BUFFER_ROW_TEXT_MAX(b))
  n = E_BUFFER_ROW_TEXT_MAX(b);
 if (n < 0)
  n = 0;
 memcpy(out->s, src, (size_t)n);
 out->s[n] = (unsigned char) term;
 out->s[n + (term ? 1 : 0)] = '\0';
 buffer_line_measure(out);
 return(E_BUFFER_OK);
}

/* Is (y, x) the end of a row whose line goes on in the row below, or ends
   the text without a line break? */
static int buffer_at_soft_end(BUFFER *b, int y, int x)
{
 return(x == b->bf[y].len && b->bf[y].s[x] != WPE_WR);
}

/**
 * buffer_split_soft_end - Enter at the end of a row without a line break:
 * the row gets its break and the row below, the rest of its line, becomes a
 * line of its own; no row is added.
 * @b: the buffer; @y: the row; @step: how it counts.
 *
 * A row of exactly Max. Columns bytes takes the break inside its own room,
 * so every byte stays.  Return: as e_buffer_splice.
 */
static int buffer_split_soft_end(BUFFER *b, int y, WpeBufferStep step)
{
 STRING head;
 int r;

 if (buffer_line_from_bytes(b, b->bf[y].s, b->bf[y].len, WPE_WR, &head) != E_BUFFER_OK)
  return(E_BUFFER_NOMEM);
 r = e_buffer_splice(b, y, 1, &head, 1, NULL, 0, step);
 if (r != E_BUFFER_OK)
  free(head.s);
 return(r);
}

/**
 * e_buffer_split_line - Break line y at column x: its tail becomes line y+1.
 * @step: how it counts; @head_term: E_BUFFER_HEAD_WR ends the head with a
 * line break (Enter), E_BUFFER_HEAD_OPEN leaves it open (a block inserts
 * the rest next).
 *
 * The line keeps its breakpoint; marks behind the column move with the tail.
 * At column 0 the empty line goes above and the whole line, its breakpoint
 * and its marks move down one.  Enter at the end of a row without a line
 * break gives the row its break and adds no row: the row below already is
 * the rest of the line.  Return: as e_buffer_splice.
 */
int e_buffer_split_line(BUFFER *b, int y, int x, WpeBufferStep step, int head_term)
{
 STRING pair[2];
 POINT *m[BUFFER_MARKS_MAX], moved[BUFFER_MARKS_MAX];
 int nm, i, r, term, tail_n, len;

 if (y < 0 || y >= b->mxlines || x < 0)
  return(E_BUFFER_RANGE);
 term = head_term == E_BUFFER_HEAD_WR ? WPE_WR : '\0';
 len = b->bf[y].len;
 if (x > len)
  x = len;
 if (head_term == E_BUFFER_HEAD_WR && buffer_at_soft_end(b, y, x))
  return(buffer_split_soft_end(b, y, step));
 if (x == 0)
 {
  if (buffer_line_from_bytes(b, (const unsigned char *) "", 0, term, &pair[0]) != E_BUFFER_OK)
   return(E_BUFFER_NOMEM);
  r = e_buffer_splice(b, y, 0, pair, 1, NULL, 0, step);
  if (r != E_BUFFER_OK)
   free(pair[0].s);
  return(r);
 }
 if (buffer_line_from_bytes(b, b->bf[y].s, x, term, &pair[0]) != E_BUFFER_OK)
  return(E_BUFFER_NOMEM);
 if (e_buffer_line_new(b, &pair[1]) != E_BUFFER_OK)
 {
  free(pair[0].s);
  return(E_BUFFER_NOMEM);
 }
 tail_n = (int)strlen((char *) b->bf[y].s + x);
 if (tail_n > E_BUFFER_ROW_TEXT_MAX(b) + 1)       /* the text and its line break */
  tail_n = E_BUFFER_ROW_TEXT_MAX(b) + 1;
 memcpy(pair[1].s, b->bf[y].s + x, (size_t)tail_n);
 pair[1].s[tail_n] = '\0';
 buffer_line_measure(&pair[1]);
 nm = buffer_marks(b, m);
 for (i = 0; i < nm; i++)
  moved[i].y = -1;
 for (i = 0; i < nm; i++)
  if (m[i]->y == y && m[i]->x > x)
  {
   moved[i].x = m[i]->x - x;
   moved[i].y = y + 1;
  }
 r = e_buffer_splice(b, y, 1, pair, 2, NULL, 0, step);
 if (r != E_BUFFER_OK)
 {
  free(pair[0].s);
  free(pair[1].s);
  return(r);
 }
 for (i = 0; i < nm; i++)
  if (moved[i].y >= 0)
   *m[i] = moved[i];
 return(E_BUFFER_OK);
}

/**
 * e_buffer_detach_lines - Take n lines out of the buffer without freeing them.
 * @out: receives the lines; @brk_out: receives their breakpoint rows,
 * relative to y (room for n); @nbrk: receives how many.
 *
 * A block move's first half: the lines and their breakpoints leave to be
 * spliced in elsewhere, so nothing is freed twice and no breakpoint is lost.
 * Nothing is recorded.  Return: as e_buffer_splice.
 */
int e_buffer_detach_lines(BUFFER *b, int y, int n, STRING *out, int *brk_out, int *nbrk)
{
 int zero = 0;

 if (nbrk == NULL)
  nbrk = &zero;
 if (y < 0 || n < 0 || y >= b->mxlines)
  return(E_BUFFER_RANGE);
 if (n > b->mxlines - y)
  n = b->mxlines - y;
 return(buffer_splice(b, y, n, NULL, 0, NULL, 0, E_BUFFER_STEP_COVERED, 1, out,
                      brk_out, nbrk));
}

/* ---- text ---- */

/* The bytes of line y before its end; a slot with no string has none. */
static size_t buffer_line_bytes(BUFFER *b, int y)
{
 if (b->bf[y].s == NULL || b->bf[y].len < 0)
  return(0);
 return((size_t)b->bf[y].len);
}

/**
 * e_buffer_to_text - The buffer as text: one '\n' after every line.
 * Return: a malloc'd string the caller frees, or NULL.
 */
char *e_buffer_to_text(BUFFER *b)
{
 size_t cap = 1, used = 0, len;
 char *out;
 int y;

 for (y = 0; y < b->mxlines; y++)
  cap += buffer_line_bytes(b, y) + 1;
 if ((out = buffer_malloc(cap)) == NULL)
  return(NULL);
 for (y = 0; y < b->mxlines; y++)
 {
  len = buffer_line_bytes(b, y);
  if (len > 0)
   memcpy(out + used, b->bf[y].s, len);
  used += len;
  out[used++] = '\n';
 }
 out[used] = '\0';
 return(out);
}

/* Where to break a piece of `len` bytes of str that does not fit in `maxc`
   columns: just after a blank or a dash, else before maxc, never inside a
   UTF-8 character. */
static int buffer_wrap_cut(const char *str, int maxc)
{
 int brk = maxc, cut = maxc;

 while (brk > 0 && str[brk - 1] != ' ' && str[brk - 1] != '-')
  brk--;
 if (brk > 0)
  return(brk);
 while (cut > 1 && ((unsigned char) str[cut] & 0xC0) == 0x80)
  cut--;
 return(cut);
}

/**
 * e_buffer_wrap_cut - Where a row that must hold at most @maxc bytes is
 * cut: just after its last blank or dash in reach, else before @maxc, never
 * inside a UTF-8 character.  @row holds more than @maxc bytes.
 *
 * The one wrap rule: typing past Max. Columns cuts a row where reading,
 * pasting and rewrapping a file cut it, so the user sees the same rows
 * however the text got there.
 * Return: the bytes the row keeps, at least 1 when @maxc is.
 */
int e_buffer_wrap_cut(const unsigned char *row, int maxc)
{
 return(buffer_wrap_cut((const char *) row, maxc));
}

/* Append one logical line to lines[*n], soft-wrapped at Max. Columns: every
   piece but the last ends without a line break, the last with last_term
   (WPE_WR, or '\0' for a line that goes on in the next).  A piece is at most
   mx_x bytes, so a line exactly mx_x long is one row with its break. */
static int buffer_lines_push(STRING **lines, int *n, int *cap, int mx_x,
                             const char *str, int len, int last_term)
{
 int maxc = mx_x, start = 0;

 if (maxc < 1)
  maxc = 1;
 do
 {
  int seg = len - start, hard = 1;
  STRING *l;

  if (seg > maxc)
  {
   seg = buffer_wrap_cut(str + start, maxc);
   hard = 0;
  }
  if (*n == *cap)
  {
   STRING *tmp = buffer_realloc(*lines, (size_t)(*cap * 2 + 8) * sizeof(STRING));
   if (tmp == NULL)
    return(E_BUFFER_NOMEM);
   *lines = tmp;
   *cap = *cap * 2 + 8;
  }
  l = &(*lines)[*n];
  if (buffer_row_alloc(mx_x, l) != E_BUFFER_OK)
   return(E_BUFFER_NOMEM);
  if (seg > 0)                  /* an empty line may come with no bytes at all */
   memcpy(l->s, str + start, (size_t)seg);
  if (hard)
   l->s[seg] = (unsigned char) last_term;
  buffer_line_measure(l);
  (*n)++;
  start += seg;
 } while (start < len);
 return(E_BUFFER_OK);
}

/**
 * e_buffer_rows_push - Append one line of text to @lines as the rows of a
 * buffer @mx_x wide.
 * @lines, @n, @cap: the rows so far, their count and room, all updated;
 * @str, @len: the line's bytes, without its line break; @term: WPE_WR for
 * a line that ends with a break, '\0' for one that does not.
 *
 * The one wrap rule of the editor: a file read, a paste, a block moved and
 * a tool's rewrite all cut a long line into rows here, after the last blank
 * or dash that fits in Max. Columns, else at Max. Columns and never inside
 * a UTF-8 character.  Return: E_BUFFER_OK, or E_BUFFER_NOMEM with the rows
 * pushed so far left in @lines for the caller to free.
 */
int e_buffer_rows_push(STRING **lines, int *n, int *cap, int mx_x,
                       const unsigned char *str, int len, int term)
{
 return(buffer_lines_push(lines, n, cap, mx_x, (const char *) str, len, term));
}

/**
 * e_buffer_lines_from_text - Split text into lines of a buffer @mx_x wide.
 * @n: receives the number of lines.
 *
 * Lines split on '\n'; a trailing '\n' only ends the last line; a line wider
 * than the buffer is soft-wrapped at a word boundary, as the file reader
 * does, so saving gives back the same bytes.  Return: lines the caller owns
 * until a splice adopts them (NULL with *n == 0 for empty text or no memory;
 * *n < 0 for no memory).
 */
STRING *e_buffer_lines_from_text(const char *text, int mx_x, int *n)
{
 STRING *lines = NULL;
 int cap = 0;
 const char *p = text ? text : "", *nl;

 *n = 0;
 while (*p)
 {
  nl = strchr(p, '\n');
  if (buffer_lines_push(&lines, n, &cap, mx_x, p, nl ? (int)(nl - p) : (int)strlen(p),
                        WPE_WR) != E_BUFFER_OK)
  {
   e_buffer_lines_free(lines, *n);
   *n = -1;
   return(NULL);
  }
  if (!nl)
   break;
  p = nl + 1;
 }
 return(lines);
}

/**
 * e_buffer_lines_from_parts - Join runs of bytes into one line of a buffer
 * @mx_x wide, soft-wrapped as the file reader wraps it.
 * @parts: the runs, in order; @nparts: how many; @n: receives the number of
 * lines.
 *
 * A block move builds the lines where two texts meet (the part of a line
 * before the cursor and the block's first line, the block's last line and
 * the rest of the line) from the bytes as they stand before anything
 * moves.  A line break that ends the last run ends the last line; without
 * one the last line goes on in the next, as a soft-wrapped line does.
 * Return: the lines, the caller's until a splice adopts them, or NULL with
 * *n < 0 when memory ran out.
 */
STRING *e_buffer_lines_from_parts(int mx_x, const WpeBufferPart *parts, int nparts, int *n)
{
 STRING *lines = NULL;
 char *joined;
 int cap = 0, total = 0, used = 0, i, term = '\0';

 *n = 0;
 for (i = 0; i < nparts; i++)
  if (parts[i].n > 0)
   total += parts[i].n;
 if ((joined = buffer_malloc((size_t)total + 1)) == NULL)
 {
  *n = -1;
  return(NULL);
 }
 for (i = 0; i < nparts; i++)
  if (parts[i].n > 0)
  {
   memcpy(joined + used, parts[i].s, (size_t)parts[i].n);
   used += parts[i].n;
  }
 if (used > 0 && (unsigned char) joined[used - 1] == WPE_WR)
 {
  term = WPE_WR;
  used--;
 }
 if (buffer_lines_push(&lines, n, &cap, mx_x, joined, used, term) != E_BUFFER_OK)
 {
  e_buffer_lines_free(lines, *n);
  lines = NULL;
  *n = -1;
 }
 free(joined);
 return(lines);
}

/* Does a row end its line of text (a line break), or does the line go on in
   the row below (a soft-wrapped row)? */
static int buffer_row_ends_line(const STRING *row)
{
 return(row->len >= 0 && row->s[row->len] == WPE_WR);
}

/* The bytes of a row before its end. */
static int buffer_row_bytes(const STRING *row)
{
 if (row->s == NULL || row->len < 0)
  return(0);
 return(row->len);
}

/**
 * buffer_rows_rewrapped - @n rows as lines of a buffer @mx_x wide.
 * @rows: the rows, in order; @has_row: for each, whether it carries a
 * breakpoint (NULL: none do); @n_out: receives the number of lines;
 * @brk: room for @n rows, receives the line each marked line of text
 * starts on (NULL when @has_row is); @nbrk: receives how many.
 *
 * Each line of text, its soft-wrapped rows joined, is wrapped again at the
 * new width as the file reader would wrap it, so text that goes into a
 * window of another width keeps every byte and every line break.  The last
 * row keeps its own end: without a line break it goes on in the row that
 * follows it.  Return: the lines, or NULL with *n_out < 0 when memory ran
 * out.
 */
static STRING *buffer_rows_rewrapped(const STRING *const *rows, const int *has_row, int n,
                                     int mx_x, int *n_out, int *brk, int *nbrk)
{
 STRING *lines = NULL;
 char *joined;
 int cap = 0, total = 0, used = 0, marked = 0, first, term, i;

 *n_out = 0;
 if (nbrk != NULL)
  *nbrk = 0;
 for (i = 0; i < n; i++)
  total += buffer_row_bytes(rows[i]);
 if ((joined = buffer_malloc((size_t)total + 1)) == NULL)
 {
  *n_out = -1;
  return(NULL);
 }
 for (i = 0; i < n; i++)
 {
  memcpy(joined + used, rows[i]->s, (size_t)buffer_row_bytes(rows[i]));
  used += buffer_row_bytes(rows[i]);
  if (has_row != NULL && has_row[i])
   marked = 1;
  if (!buffer_row_ends_line(rows[i]) && i < n - 1)
   continue;
  term = '\0';
  if (buffer_row_ends_line(rows[i]))
   term = WPE_WR;
  first = *n_out;
  if (buffer_lines_push(&lines, n_out, &cap, mx_x, joined, used, term) != E_BUFFER_OK)
  {
   e_buffer_lines_free(lines, *n_out);
   free(joined);
   *n_out = -1;
   return(NULL);
  }
  if (marked && brk != NULL)
   brk[(*nbrk)++] = first;
  used = 0;
  marked = 0;
 }
 free(joined);
 return(lines);
}

/* Copies of n rows of src from row `from`, each as it is: the buffer they
   go to is as wide as src and every one of them fits its rows
   (buffer_rows_copy_whole). */
static STRING *buffer_lines_copy_as_they_are(BUFFER *src, int from, int n, BUFFER *dst)
{
 STRING *out = buffer_array(n, sizeof(STRING));
 int i;

 if (out == NULL)
  return(NULL);
 for (i = 0; i < n; i++)
 {
  if (e_buffer_line_copy(dst, &src->bf[from + i], &out[i]) != E_BUFFER_OK)
  {
   e_buffer_lines_free(out, i);
   return(NULL);
  }
  buffer_line_measure(&out[i]);
 }
 return(out);
}

/* Does a row hold at most @width bytes of text, ended by its line break or
   by the zero of a row whose line goes on below? */
static int buffer_row_within(const STRING *row, int width)
{
 if (row->s == NULL || row->len < 0 || row->len > width)
  return(0);
 return(row->s[row->len] == WPE_WR || row->s[row->len] == '\0');
}

/* Can n rows of src from row `from` go into dst as they are?  Only when
   both are as wide and every row already fits a row of dst: a row of
   Messages or of the assistant's pane may be wider than the window, and is
   wrapped again instead of losing its tail. */
static int buffer_rows_copy_whole(BUFFER *src, int from, int n, BUFFER *dst)
{
 int i;

 if (src->mx.x != dst->mx.x)
  return(0);
 for (i = 0; i < n; i++)
  if (!buffer_row_within(&src->bf[from + i], dst->mx.x))
   return(0);
 return(1);
}

/**
 * e_buffer_lines_copy - Copies of @n lines of @src from row @from, as lines
 * of @dst.
 * @n_out: receives how many lines the copy has.
 *
 * Block Copy and Paste put copies of the block's whole lines in, so the copy
 * does not change with later edits of the original and carries none of its
 * breakpoints.  Into a window of another width (Max. Columns changed between
 * the two windows' opening), or from a row wider than the window (a long
 * line of Messages or of the assistant's pane), each line of text is
 * wrapped again at the new width, so the paste keeps every byte and every
 * line break; it may then take more rows than it had.  Return: the lines,
 * the caller's until a splice adopts them, or NULL when memory ran out (or
 * for n <= 0).
 */
STRING *e_buffer_lines_copy(BUFFER *src, int from, int n, BUFFER *dst, int *n_out)
{
 const STRING **rows;
 STRING *out;
 int i;

 *n_out = 0;
 if (n <= 0)
  return(NULL);
 if (buffer_rows_copy_whole(src, from, n, dst))
 {
  out = buffer_lines_copy_as_they_are(src, from, n, dst);
  if (out != NULL)
   *n_out = n;
  return(out);
 }
 if ((rows = buffer_array(n, sizeof(STRING *))) == NULL)
  return(NULL);
 for (i = 0; i < n; i++)
  rows[i] = &src->bf[from + i];
 out = buffer_rows_rewrapped(rows, NULL, n, dst->mx.x, n_out, NULL, NULL);
 free((void *) rows);
 if (*n_out < 0)
  *n_out = 0;
 return(out);
}

/* ---- a change of several splices, whole or not at all ---- */

/* The second half of a move lands what its first half took out: no line of
   its own until then. */
#define BUFFER_TXN_NOT_LANDED (-1)

/**
 * e_buffer_txn_begin - Start a change of several splices.
 * @t: on the caller's stack, for one change.
 */
void e_buffer_txn_begin(WpeBufferTxn *t)
{
 memset(t, 0, sizeof(*t));
 t->sp = t->sp_inline;
 t->cap = E_BUFFER_TXN_SPLICES;
 t->refused = E_BUFFER_OK;
}

/* The change is over (made or given back): slots it took memory for go
   back, and the inline ones serve again. */
static void buffer_txn_release(WpeBufferTxn *t)
{
 if (t->sp != t->sp_inline)
  free(t->sp);
 t->sp = t->sp_inline;
 t->cap = E_BUFFER_TXN_SPLICES;
 t->nsp = 0;
}

/* Room for one more slot: past the inline ones the slots move to memory of
   their own, twice as many each time.  Return: E_BUFFER_OK or
   E_BUFFER_NOMEM (the slots as they were). */
static int buffer_txn_grow(WpeBufferTxn *t)
{
 WpeBufferTxnSplice *more;
 int cap = t->cap * 2;

 if (t->nsp < t->cap)
  return(E_BUFFER_OK);
 if (t->sp == t->sp_inline)
 {
  if ((more = buffer_malloc((size_t)cap * sizeof(WpeBufferTxnSplice))) == NULL)
   return(E_BUFFER_NOMEM);
  memcpy(more, t->sp_inline, (size_t)t->nsp * sizeof(WpeBufferTxnSplice));
 }
 else if ((more = buffer_realloc(t->sp, (size_t)cap * sizeof(WpeBufferTxnSplice))) == NULL)
  return(E_BUFFER_NOMEM);
 t->sp = more;
 t->cap = cap;
 return(E_BUFFER_OK);
}

/* The first refusal is the one the change reports: later items are not
   prepared at all. */
static int buffer_txn_refuse(WpeBufferTxn *t, int why)
{
 if (t->refused == E_BUFFER_OK)
  t->refused = why;
 return(t->refused);
}

/* The transaction's view of buffer b: its line count as the items so far
   leave it.  Return: the entry, or NULL when the change already touches as
   many buffers as a transaction holds. */
static WpeBufferTxnBuffer *buffer_txn_buffer(WpeBufferTxn *t, BUFFER *b)
{
 WpeBufferTxnBuffer *e;
 int i;

 for (i = 0; i < t->nbuf; i++)
  if (t->buf[i].b == b)
   return(&t->buf[i]);
 if (t->nbuf == E_BUFFER_TXN_BUFFERS)
  return(NULL);
 e = &t->buf[t->nbuf++];
 memset(e, 0, sizeof(*e));
 e->b = b;
 e->lines = b->mxlines;
 return(e);
}

/* The buffer one more item changes.  Return: the entry, or NULL with the
   change refused. */
static WpeBufferTxnBuffer *buffer_txn_room(WpeBufferTxn *t, BUFFER *b)
{
 WpeBufferTxnBuffer *e;

 e = buffer_txn_buffer(t, b);
 if (e == NULL)
  buffer_txn_refuse(t, E_BUFFER_RANGE);
 return(e);
}

/* Is a splice of n_del lines at y a place in a buffer of `lines` lines? */
static int buffer_txn_in_range(int lines, int y, int n_del, int n_ins)
{
 if (y < 0 || n_del < 0 || n_ins < 0 || y > lines)
  return(0);
 return(!(y == lines && n_del > 0));
}

/* The rows a splice of n_del lines may take off the margin: all of them,
   since the items before it decide which lines stand there. */
static int buffer_txn_rows_bound(BUFFER *b, int n_del)
{
 if (buffer_brp(b) == NULL)
  return(0);
 return(n_del);
}

/* Prepare one splice of the change in the next free slot: its memory is
   taken, its buffer's count moves on.  Return: E_BUFFER_OK or
   E_BUFFER_NOMEM (nothing kept). */
static int buffer_txn_take(WpeBufferTxn *t, WpeBufferTxnBuffer *e, WpeBufferTxnSplice *sp,
                           int nbrk)
{
 int records = 0, rows = buffer_txn_rows_bound(sp->b, sp->n_del);

 if (!sp->detach)
  records = buffer_splice_records(sp->b, sp->step);
 sp->pocket = NULL;
 if (!sp->detach && sp->kind != E_BUFFER_ITEM_REWRITE)
  sp->pocket = buffer_splice_pocket(sp->b, records);
 if (buffer_splice_take(sp->b, e->lines, sp->n_del, sp->n_ins, nbrk, rows, records,
                        sp->detach, &sp->m) != E_BUFFER_OK)
  return(E_BUFFER_NOMEM);
 e->lines += sp->n_ins - sp->n_del;
 if (sp->m.need_canon)
  e->lines++;
 if (buffer_brp(sp->b) != NULL)
  e->brp_rows += nbrk;
 if (sp->pocket != NULL)
 {
  e->pocket = sp->pocket;
  e->pocket_rows += rows;
 }
 t->nsp++;
 return(E_BUFFER_OK);
}

/* The next free splice slot, set up for a splice of b at y.  It counts
   only once buffer_txn_take prepared it.  Return: the slot, or NULL when
   no memory was left for one more. */
static WpeBufferTxnSplice *buffer_txn_slot(WpeBufferTxn *t, BUFFER *b, int y, int n_del,
                                           int n_ins, WpeBufferStep step)
{
 WpeBufferTxnSplice *sp;

 if (buffer_txn_grow(t) != E_BUFFER_OK)
  return(NULL);
 sp = &t->sp[t->nsp];
 memset(sp, 0, sizeof(*sp));
 sp->b = b;
 sp->y = y;
 sp->n_del = n_del;
 sp->n_ins = n_ins;
 sp->step = step;
 sp->landed_from = BUFFER_TXN_NOT_LANDED;
 sp->kind = E_BUFFER_ITEM_SPLICE;
 return(sp);
}

/**
 * e_buffer_txn_splice - Add a splice to the change: at line @y, @n_del lines
 * go and @lines[0..@n_ins) come.
 * @t: the change; @b: the buffer; @step: how it counts for Ctrl-U (a block
 * operation passes E_BUFFER_STEP_COVERED: its own record replays it).
 *
 * @y is where the splice meets the buffer once the items before it have
 * committed.  The lines (array and strings) are the transaction's from now
 * on, whatever this returns: committed they are the buffer's, refused or
 * aborted they are freed.  Return: E_BUFFER_OK, E_BUFFER_NOMEM or
 * E_BUFFER_RANGE (the change is then refused as a whole).
 */
int e_buffer_txn_splice(WpeBufferTxn *t, BUFFER *b, int y, int n_del, STRING *lines,
                        int n_ins, WpeBufferStep step)
{
 WpeBufferTxnBuffer *e;
 WpeBufferTxnSplice *sp;

 if (t->refused != E_BUFFER_OK || (e = buffer_txn_room(t, b)) == NULL)
 {
  e_buffer_lines_free(lines, n_ins);
  return(t->refused);
 }
 if (!buffer_txn_in_range(e->lines, y, n_del, n_ins) || !buffer_lines_fit(b, y, lines, n_ins))
 {
  e_buffer_lines_free(lines, n_ins);
  return(buffer_txn_refuse(t, E_BUFFER_RANGE));
 }
 if (n_del > e->lines - y)
  n_del = e->lines - y;
 if (n_del == 0 && n_ins == 0)
 {
  free(lines);
  return(E_BUFFER_OK);
 }
 if ((sp = buffer_txn_slot(t, b, y, n_del, n_ins, step)) == NULL)
 {
  e_buffer_lines_free(lines, n_ins);
  return(buffer_txn_refuse(t, E_BUFFER_NOMEM));
 }
 sp->lines = lines;
 if (buffer_txn_take(t, e, sp, 0) != E_BUFFER_OK)
 {
  e_buffer_lines_free(lines, n_ins);
  return(buffer_txn_refuse(t, E_BUFFER_NOMEM));
 }
 return(E_BUFFER_OK);
}

/**
 * e_buffer_txn_splice_parts - Add a splice whose new lines are the runs of
 * bytes @parts joined (and soft-wrapped at the buffer's width).
 *
 * The bytes are copied now, so they may be read from lines a later item
 * changes.  Return: how many lines it puts in (one or more), or
 * E_BUFFER_NOMEM / E_BUFFER_RANGE as e_buffer_txn_splice.
 */
int e_buffer_txn_splice_parts(WpeBufferTxn *t, BUFFER *b, int y, int n_del,
                              const WpeBufferPart *parts, int nparts, WpeBufferStep step)
{
 STRING *lines;
 int n, r;

 if (t->refused != E_BUFFER_OK)
  return(t->refused);
 lines = e_buffer_lines_from_parts(b->mx.x, parts, nparts, &n);
 if (n < 0)
  return(buffer_txn_refuse(t, E_BUFFER_NOMEM));
 r = e_buffer_txn_splice(t, b, y, n_del, lines, n, step);
 if (r != E_BUFFER_OK)
  return(r);
 return(n);
}

/**
 * e_buffer_txn_copy_lines - Add copies of @n lines of @src from row @from,
 * put in at row @y of @to.
 *
 * A copy carries no breakpoint.  Into a buffer of another width the copy is
 * wrapped again at that width (e_buffer_lines_copy).  Return: how many lines
 * it puts in (@n, or more when wrapped again), or E_BUFFER_NOMEM /
 * E_BUFFER_RANGE as e_buffer_txn_splice.
 */
int e_buffer_txn_copy_lines(WpeBufferTxn *t, BUFFER *to, int y, BUFFER *src, int from,
                            int n, WpeBufferStep step)
{
 STRING *lines;
 int k, r;

 if (t->refused != E_BUFFER_OK || n <= 0)
  return(t->refused);
 if ((lines = e_buffer_lines_copy(src, from, n, to, &k)) == NULL)
  return(buffer_txn_refuse(t, E_BUFFER_NOMEM));
 r = e_buffer_txn_splice(t, to, y, 0, lines, k, step);
 if (r != E_BUFFER_OK)
  return(r);
 return(k);
}

/**
 * e_buffer_txn_delete_lines - Add the deletion of @n lines at @y.
 * Return: as e_buffer_txn_splice.
 */
int e_buffer_txn_delete_lines(WpeBufferTxn *t, BUFFER *b, int y, int n, WpeBufferStep step)
{
 return(e_buffer_txn_splice(t, b, y, n, NULL, 0, step));
}

/* The first half of a move: n lines of `from` at y leave with their
   breakpoint rows, into the slot's own hands. */
static int buffer_txn_detach(WpeBufferTxn *t, WpeBufferTxnBuffer *e, BUFFER *from, int y, int n)
{
 WpeBufferTxnSplice *sp = buffer_txn_slot(t, from, y, n, 0, E_BUFFER_STEP_COVERED);

 if (sp == NULL)
  return(E_BUFFER_NOMEM);
 sp->detach = 1;
 sp->out = buffer_array(n, sizeof(STRING));
 if (sp->out == NULL)
  return(E_BUFFER_NOMEM);
 if (buffer_brp(from) != NULL && (sp->brk_out = buffer_array(n, sizeof(int))) == NULL)
 {
  free(sp->out);
  return(E_BUFFER_NOMEM);
 }
 if (buffer_txn_take(t, e, sp, 0) != E_BUFFER_OK)
 {
  free(sp->out);
  free(sp->brk_out);
  return(E_BUFFER_NOMEM);
 }
 return(E_BUFFER_OK);
}

/* The second half of a move: the lines the detach in slot `half` took out
   land at y of `to`, each with its breakpoint. */
static int buffer_txn_land(WpeBufferTxn *t, WpeBufferTxnBuffer *e, BUFFER *to, int y, int n,
                           int half)
{
 WpeBufferTxnSplice *sp = buffer_txn_slot(t, to, y, 0, n, E_BUFFER_STEP_COVERED);

 if (sp == NULL)
  return(E_BUFFER_NOMEM);
 sp->landed_from = half;
 return(buffer_txn_take(t, e, sp, n));
}

/* Undo the preparation of the slot just taken: its memory goes back. */
static void buffer_txn_untake(WpeBufferTxn *t, WpeBufferTxnBuffer *e)
{
 WpeBufferTxnSplice *sp = &t->sp[--t->nsp];

 e->lines -= sp->n_ins - sp->n_del;
 if (sp->m.need_canon)
  e->lines--;
 buffer_splice_mem_free(&sp->m);
 free(sp->out);
 free(sp->brk_out);
}

static int buffer_txn_view(WpeBufferTxn *t, BUFFER *b, int y, const STRING **line,
                           int *has_row);

/* The n lines that stand at row y of b once the items so far commit, built
   again as lines mx_x wide, with the line each breakpoint goes to in brk
   (room for n).  Return: the lines, or NULL with *n_out < 0 for no memory
   and *n_out == 0 for a row the view cannot read. */
static STRING *buffer_txn_rows_rebuilt(WpeBufferTxn *t, BUFFER *b, int y, int n, int mx_x,
                                       int *n_out, int *brk, int *nbrk)
{
 const STRING **rows = buffer_array(n, sizeof(STRING *));
 int *has_row = buffer_array(n, sizeof(int));
 STRING *lines = NULL;
 int i;

 *n_out = -1;
 if (rows != NULL && has_row != NULL)
 {
  for (i = 0; i < n; i++)
   if (buffer_txn_view(t, b, y + i, &rows[i], &has_row[i]) != E_BUFFER_OK)
    break;
  *n_out = 0;
  if (i == n)
   lines = buffer_rows_rewrapped(rows, has_row, n, mx_x, n_out, brk, nbrk);
 }
 free((void *) rows);
 free(has_row);
 return(lines);
}

/* Give back what a rebuilt move took before its landing could be added:
   the detach just prepared and the rebuilt lines.  Return: the refusal. */
static int buffer_txn_rebuilt_drop(WpeBufferTxn *t, WpeBufferTxnBuffer *ef, STRING *lines,
                                   int k, int *brk, int why)
{
 buffer_txn_untake(t, ef);
 e_buffer_lines_free(lines, k);
 free(brk);
 return(buffer_txn_refuse(t, why));
}

/* A move between buffers of different widths: the lines leave @from as a
   detach does (their breakpoints with them) and copies built at @to's width
   land, each line of text carrying the breakpoint one of its rows had; the
   originals are freed when the copies land. */
static int buffer_txn_move_rebuilt(WpeBufferTxn *t, WpeBufferTxnBuffer *ef, BUFFER *from,
                                   int y_from, int n, BUFFER *to, int y_to)
{
 WpeBufferTxnBuffer *et;
 WpeBufferTxnSplice *sp;
 STRING *lines;
 int *brk, nbrk = 0, k, half = t->nsp;

 if ((brk = buffer_array(n, sizeof(int))) == NULL)
  return(buffer_txn_refuse(t, E_BUFFER_NOMEM));
 lines = buffer_txn_rows_rebuilt(t, from, y_from, n, to->mx.x, &k, brk, &nbrk);
 if (lines == NULL)
 {
  free(brk);
  if (k < 0)
   return(buffer_txn_refuse(t, E_BUFFER_NOMEM));
  return(buffer_txn_refuse(t, E_BUFFER_RANGE));
 }
 if (buffer_txn_detach(t, ef, from, y_from, n) != E_BUFFER_OK)
 {
  e_buffer_lines_free(lines, k);
  free(brk);
  return(buffer_txn_refuse(t, E_BUFFER_NOMEM));
 }
 et = buffer_txn_buffer(t, to);
 if (et == NULL || !buffer_txn_in_range(et->lines, y_to, 0, k))
  return(buffer_txn_rebuilt_drop(t, ef, lines, k, brk, E_BUFFER_RANGE));
 if ((sp = buffer_txn_slot(t, to, y_to, 0, k, E_BUFFER_STEP_COVERED)) == NULL)
  return(buffer_txn_rebuilt_drop(t, ef, lines, k, brk, E_BUFFER_NOMEM));
 sp->landed_from = half;
 sp->lines = lines;
 sp->brk = brk;
 sp->nbrk = nbrk;
 if (buffer_txn_take(t, et, sp, nbrk) != E_BUFFER_OK)
  return(buffer_txn_rebuilt_drop(t, ef, lines, k, brk, E_BUFFER_NOMEM));
 return(k);
}

/* Must a move build its lines again at the width of @to instead of handing
   them over?  When the widths differ, and when they leave a view, whose
   rows may be wider than the window, for a buffer whose rows may not. */
static int buffer_move_rebuilds(const BUFFER *from, const BUFFER *to)
{
 if (from->mx.x != to->mx.x)
  return(1);
 return(!buffer_rows_bounded(from) && buffer_rows_bounded(to));
}

/**
 * e_buffer_txn_move_lines - Add a move of @n whole lines from row @y_from of
 * @from to row @y_to of @to (the same buffer or another).
 *
 * Each line changes hands once, with its breakpoint: none is copied, none
 * freed, so none can be lost or freed twice.  Into a buffer of another width
 * the lines are built again at that width instead (their breakpoints go with
 * them), since a line is as wide as the buffer that holds it.  @y_to is a
 * row of @to once the lines have left @from.  Return: how many lines land
 * (@n, or more when built again), or E_BUFFER_NOMEM / E_BUFFER_RANGE as
 * e_buffer_txn_splice.
 */
int e_buffer_txn_move_lines(WpeBufferTxn *t, BUFFER *from, int y_from, int n,
                            BUFFER *to, int y_to)
{
 WpeBufferTxnBuffer *ef, *et;
 int half;

 if (t->refused != E_BUFFER_OK || n <= 0)
  return(t->refused);
 if ((ef = buffer_txn_room(t, from)) == NULL)
  return(t->refused);
 if (y_from < 0 || y_from >= ef->lines)
  return(buffer_txn_refuse(t, E_BUFFER_RANGE));
 if (n > ef->lines - y_from)
  n = ef->lines - y_from;
 if (buffer_move_rebuilds(from, to))
  return(buffer_txn_move_rebuilt(t, ef, from, y_from, n, to, y_to));
 half = t->nsp;
 if (buffer_txn_detach(t, ef, from, y_from, n) != E_BUFFER_OK)
  return(buffer_txn_refuse(t, E_BUFFER_NOMEM));
 if ((et = buffer_txn_buffer(t, to)) == NULL || !buffer_txn_in_range(et->lines, y_to, 0, n))
 {
  buffer_txn_untake(t, ef);
  return(buffer_txn_refuse(t, E_BUFFER_RANGE));
 }
 if (buffer_txn_land(t, et, to, y_to, n, half) != E_BUFFER_OK)
 {
  buffer_txn_untake(t, ef);
  return(buffer_txn_refuse(t, E_BUFFER_NOMEM));
 }
 return(n);
}

/* ---- the items a step of Undo or Redo is made of ---- */

/* Free what a prepared item holds that nobody adopted: its memory, the
   lines it would have put in (unless they are a record's or a move's), and
   a rewrite's shell and its copy of the rows. */
static void buffer_txn_item_free(WpeBufferTxnSplice *sp)
{
 buffer_splice_mem_free(&sp->m);
 free(sp->out);
 free(sp->brk_out);
 sp->out = NULL;
 sp->brk_out = NULL;
 if (sp->kind == E_BUFFER_ITEM_REWRITE)
 {
  e_buffer_shell_free(sp->shell);
  sp->shell = NULL;
  free((void *) sp->brk);
  sp->brk = NULL;
  return;
 }
 if (sp->landed_from != BUFFER_TXN_NOT_LANDED && sp->lines != NULL)
 {
  e_buffer_lines_free(sp->lines, sp->n_ins);     /* a rebuilt move's own lines */
  free((void *) sp->brk);
  sp->lines = NULL;
  sp->brk = NULL;
  return;
 }
 if (sp->lines_kept || sp->detach || sp->landed_from != BUFFER_TXN_NOT_LANDED)
  return;
 e_buffer_lines_free(sp->lines, sp->n_ins);
}

/* Is a slot one half of a move?  Its lines are known only once the detach
   committed, so the view cannot see through it. */
static int buffer_txn_is_move(const WpeBufferTxnSplice *sp)
{
 return(sp->detach || sp->landed_from != BUFFER_TXN_NOT_LANDED);
}

/* How many lines an item leaves in its span: the canonical empty line of a
   buffer it empties counts as one. */
static int buffer_txn_item_lines(const WpeBufferTxnSplice *sp)
{
 if (sp->m.need_canon)
  return(1);
 return(sp->n_ins);
}

/* How many rows at the top of an item's span are changed rows, which keep
   the breakpoint the line there had: none for a rewrite (its rows are its
   brk[] alone), an emptied buffer, or a replay of one. */
static int buffer_txn_item_changed(const WpeBufferTxnSplice *sp)
{
 if (sp->kind == E_BUFFER_ITEM_REWRITE || sp->no_changed || sp->m.need_canon)
  return(0);
 if (sp->n_del < sp->n_ins)
  return(sp->n_del);
 return(sp->n_ins);
}

/* Line rel of an item's span once it commits. */
static const STRING *buffer_txn_item_line(const WpeBufferTxnSplice *sp, int rel)
{
 if (sp->m.need_canon)
  return(&sp->m.canon);
 return(&sp->lines[rel]);
}

/* Does an item set a breakpoint row on line rel of its span? */
static int buffer_txn_item_row(const WpeBufferTxnSplice *sp, int rel)
{
 if (sp->m.need_canon || buffer_brp(sp->b) == NULL)
  return(0);
 return(buffer_brk_holds(sp->brk, sp->nbrk, rel, sp->n_ins));
}

/* The line count of b as the items so far leave it. */
static int buffer_txn_lines_now(WpeBufferTxn *t, BUFFER *b)
{
 int i;

 for (i = 0; i < t->nbuf; i++)
  if (t->buf[i].b == b)
   return(t->buf[i].lines);
 return(b->mxlines);
}

/* The line at row y of b once every item prepared so far commits, and
   whether a breakpoint stands on it then.  Walks the items of b from the
   newest down, each moving y to where the line stood before it; a changed
   row keeps the breakpoint of the line below it in that walk.  Return:
   E_BUFFER_OK, or E_BUFFER_RANGE for a row past the end or behind a move. */
static int buffer_txn_view(WpeBufferTxn *t, BUFFER *b, int y, const STRING **line,
                           int *has_row)
{
 const STRING *found = NULL;
 WpeBufferTxnSplice *sp;
 int i, rel, row = 0, *brp;

 if (y < 0 || y >= buffer_txn_lines_now(t, b))
  return(E_BUFFER_RANGE);
 for (i = t->nsp - 1; i >= 0; i--)
 {
  sp = &t->sp[i];
  if (sp->b != b || y < sp->y)
   continue;
  if (buffer_txn_is_move(sp))
  {
   wpe_buffer_trace("buffer txn view meets a move");
   return(E_BUFFER_RANGE);
  }
  if (y >= sp->y + buffer_txn_item_lines(sp))
  {
   y += sp->n_del - buffer_txn_item_lines(sp);
   continue;
  }
  rel = y - sp->y;
  if (found == NULL)
   found = buffer_txn_item_line(sp, rel);
  if (buffer_txn_item_row(sp, rel))
   row = 1;
  if (rel >= buffer_txn_item_changed(sp))
  {
   *line = found;
   *has_row = row;
   return(E_BUFFER_OK);
  }
 }
 if (y < 0 || y >= b->mxlines)
  return(E_BUFFER_RANGE);
 if (found == NULL)
  found = &b->bf[y];
 brp = buffer_brp(b);
 if (brp != NULL && brp_has(brp, y))
  row = 1;
 *line = found;
 *has_row = row;
 return(E_BUFFER_OK);
}

/**
 * e_buffer_txn_line - The line at row @y of @b once the items of @t so far
 * commit, and whether a breakpoint stands on it then.
 * @line: receives the line, to be read at once: a later item's preparation
 * may move the buffer's table; @has_row: receives 1 or 0.
 *
 * The second record of an Undo step (the join of an Enter with
 * auto-indent) is replayed against what the first leaves, before anything
 * is made, so the step is made whole or not at all.
 * Return: E_BUFFER_OK, or E_BUFFER_RANGE for a row past the end or behind a
 * block move's item.
 */
int e_buffer_txn_line(WpeBufferTxn *t, BUFFER *b, int y, const STRING **line, int *has_row)
{
 return(buffer_txn_view(t, b, y, line, has_row));
}

/**
 * e_buffer_txn_replay_lines - Add the replay of line record @rec: the lines
 * it keeps come back in place of the ones that stand there now.
 * @t: the step's change; @b: the buffer; @rec: an E_UNDO_LINES record.
 *
 * Its inverse, for the other list, is taken now with the rest; the record's
 * lines stay the record's until the commit, so a step refused for memory
 * leaves the record whole for the next try.
 * Return: E_BUFFER_OK, E_BUFFER_NOMEM or E_BUFFER_RANGE.
 */
int e_buffer_txn_replay_lines(WpeBufferTxn *t, BUFFER *b, Undo *rec)
{
 WpeBufferTxnBuffer *e;
 WpeBufferTxnSplice *sp;
 int n_del = rec->n_new;

 if (t->refused != E_BUFFER_OK || (e = buffer_txn_room(t, b)) == NULL)
  return(t->refused);
 if (rec->type != E_UNDO_LINES || !buffer_txn_in_range(e->lines, rec->span, rec->n_new,
                                                        rec->n_old) ||
     !buffer_lines_fit(b, rec->span, rec->old, rec->n_old))
  return(buffer_txn_refuse(t, E_BUFFER_RANGE));
 if (n_del > e->lines - rec->span)
  n_del = e->lines - rec->span;
 if ((sp = buffer_txn_slot(t, b, rec->span, n_del, rec->n_old, E_BUFFER_STEP_REPLAY)) == NULL)
  return(buffer_txn_refuse(t, E_BUFFER_NOMEM));
 sp->kind = E_BUFFER_ITEM_REPLAY;
 sp->rec = rec;
 sp->lines = rec->old;
 sp->lines_kept = 1;
 sp->brk = rec->brk;
 sp->nbrk = rec->nbrk;
 sp->no_changed = (rec->flags & E_UNDO_EMPTIED) != 0;
 if (buffer_txn_take(t, e, sp, rec->nbrk) != E_BUFFER_OK)
  return(buffer_txn_refuse(t, E_BUFFER_NOMEM));
 return(E_BUFFER_OK);
}

/* The bytes a copy of line l keeps: through its terminating zero, never
   past the room of a row of the buffer's width. */
static size_t buffer_line_kept_bytes(BUFFER *b, const STRING *l)
{
 int n = l->nrc + 1;

 if (n > E_BUFFER_ROW_BYTES(b->mx.x))
  n = E_BUFFER_ROW_BYTES(b->mx.x);
 if (n < 1)
  n = 1;
 return((size_t)n);
}

/* Do two lines hold the same bytes, through the terminating zero? */
static int buffer_lines_same_bytes(BUFFER *b, const STRING *l, const STRING *c)
{
 if (l->len != c->len || l->nrc != c->nrc)
  return(0);
 return(memcmp(l->s, c->s, buffer_line_kept_bytes(b, l)) == 0);
}

/* Is a char record ('a' 'r' 's' 'p') one a shell's inverse may be? */
static int buffer_record_is_char(int kind)
{
 return(kind == 'a' || kind == 'r' || kind == 's' || kind == 'p');
}

/* Did the replay on a shell record only char kinds?  Another kind would
   replay against the user's buffer as something it is not. */
static int buffer_shell_records_replayable(BUFFER *shell)
{
 Undo *rec;

 for (rec = shell->ud; rec != NULL; rec = rec->next)
  if (!buffer_record_is_char(rec->type))
  {
   wpe_buffer_trace("buffer shell recorded %c", rec->type);
   return(0);
  }
 return(1);
}

/* Did the replay stay inside its span?  A shell holds one row above the
   span and one below (when they exist) as guards: a primitive that touched
   one went past the rows the span rule allows. */
static int buffer_txn_guards_hold(WpeBufferTxn *t, BUFFER *b, int y, int n_del,
                                  BUFFER *shell, int g_top, int g_bot)
{
 const STRING *l;
 int row;

 if (g_top && (buffer_txn_view(t, b, y - 1, &l, &row) != E_BUFFER_OK ||
               !buffer_lines_same_bytes(shell, l, &shell->bf[0])))
  return(0);
 if (g_bot && (buffer_txn_view(t, b, y + n_del, &l, &row) != E_BUFFER_OK ||
               !buffer_lines_same_bytes(shell, l, &shell->bf[shell->mxlines - 1])))
  return(0);
 return(1);
}

/* Can a shell's rows replace rows [y, y + n_del) of a buffer of `lines`
   lines?  The shell must be as wide (its lines are adopted), its guards
   must have rows to guard, and the replay must have stayed inside its
   span and recorded only char kinds.  Return: E_BUFFER_OK or
   E_BUFFER_RANGE, traced. */
static int buffer_txn_rewrite_check(WpeBufferTxn *t, BUFFER *b, int lines, int y, int n_del,
                                    BUFFER *shell, int g_top, int g_bot)
{
 if (shell == NULL || shell->mx.x != b->mx.x || g_top < 0 || g_top > 1 || g_bot < 0 ||
     g_bot > 1 || shell->mxlines - g_top - g_bot < 0 || (g_top && y == 0) ||
     (g_bot && y + n_del >= lines))
 {
  wpe_buffer_trace("buffer rewrite does not fit y=%d del=%d", y, n_del);
  return(E_BUFFER_RANGE);
 }
 if (!buffer_txn_guards_hold(t, b, y, n_del, shell, g_top, g_bot))
 {
  wpe_buffer_trace("buffer replay span overrun");
  return(E_BUFFER_RANGE);
 }
 if (!buffer_shell_records_replayable(shell))
  return(E_BUFFER_RANGE);
 if (!buffer_lines_fit(b, y, shell->bf + g_top, shell->mxlines - g_top - g_bot))
  return(E_BUFFER_RANGE);
 return(E_BUFFER_OK);
}

/* The rows of a rewrite, the transaction's own copy.  Return: E_BUFFER_OK
   (NULL for none) or E_BUFFER_NOMEM. */
static int buffer_rows_copy(const int *brk, int nbrk, int **out)
{
 *out = NULL;
 if (nbrk <= 0 || brk == NULL)
  return(E_BUFFER_OK);
 if ((*out = buffer_array(nbrk, sizeof(int))) == NULL)
  return(E_BUFFER_NOMEM);
 memcpy(*out, brk, (size_t)nbrk * sizeof(int));
 return(E_BUFFER_OK);
}

/* Refuse a rewrite before its slot counted: the shell and the copy of its
   rows go. */
static int buffer_txn_rewrite_refused(WpeBufferTxn *t, BUFFER *shell, int *rows, int why)
{
 e_buffer_shell_free(shell);
 free(rows);
 return(buffer_txn_refuse(t, why));
}

/**
 * e_buffer_txn_rewrite - Add a rewrite: rows [@y, @y + @n_del) of @b become
 * the rows shell @shell holds between its guards.
 * @shell: a shell filled from @b (e_buffer_shell_fill) with the span plus
 * @g_top guard rows above and @g_bot below (0 or 1 each), on which a
 * replay ran; taken by the transaction whatever this returns; @brk: the
 * breakpoint rows the new lines carry, relative to @y; @nbrk: how many.
 *
 * The replay of a character record runs its primitive on a copy, so it
 * joins and wraps exactly as typing does; only when every record of the
 * step could be replayed that way does the user's text change, all at once,
 * with the inverses the replay recorded going to the other list.  A replay
 * that reached past its span (a guard changed) or recorded something other
 * than a character change is refused.
 * Return: E_BUFFER_OK, E_BUFFER_NOMEM or E_BUFFER_RANGE.
 */
int e_buffer_txn_rewrite(WpeBufferTxn *t, BUFFER *b, int y, int n_del, BUFFER *shell,
                         int g_top, int g_bot, const int *brk, int nbrk)
{
 WpeBufferTxnBuffer *e;
 WpeBufferTxnSplice *sp;
 int *rows = NULL, r;

 if (t->refused != E_BUFFER_OK || (e = buffer_txn_room(t, b)) == NULL)
  return(buffer_txn_rewrite_refused(t, shell, NULL, t->refused));
 if (!buffer_txn_in_range(e->lines, y, n_del, 0))
  return(buffer_txn_rewrite_refused(t, shell, NULL, E_BUFFER_RANGE));
 if (n_del > e->lines - y)
  n_del = e->lines - y;
 r = buffer_txn_rewrite_check(t, b, e->lines, y, n_del, shell, g_top, g_bot);
 if (r != E_BUFFER_OK)
  return(buffer_txn_rewrite_refused(t, shell, NULL, r));
 if (buffer_rows_copy(brk, nbrk, &rows) != E_BUFFER_OK)
  return(buffer_txn_rewrite_refused(t, shell, NULL, E_BUFFER_NOMEM));
 sp = buffer_txn_slot(t, b, y, n_del, shell->mxlines - g_top - g_bot, E_BUFFER_STEP_COVERED);
 if (sp == NULL)
  return(buffer_txn_rewrite_refused(t, shell, rows, E_BUFFER_NOMEM));
 sp->kind = E_BUFFER_ITEM_REWRITE;
 sp->shell = shell;
 sp->g_top = g_top;
 sp->g_bot = g_bot;
 sp->lines = shell->bf + g_top;
 sp->no_changed = 1;
 sp->brk = rows;
 sp->nbrk = (rows != NULL) ? nbrk : 0;
 if (buffer_txn_take(t, e, sp, sp->nbrk) != E_BUFFER_OK)
  return(buffer_txn_rewrite_refused(t, shell, rows, E_BUFFER_NOMEM));
 return(E_BUFFER_OK);
}

/* After a replay: the cursor and the block go back to where the record
   saw them, inside the text. */
static void buffer_replay_view_back(BUFFER *b, Undo *rec)
{
 b->b = rec->b;
 buffer_point_clamp(b, &b->b);
 if (b->f != NULL && b->f->s != NULL)
 {
  b->f->s->mark_begin = rec->a;
  b->f->s->mark_end = rec->e;
  buffer_point_clamp(b, &b->f->s->mark_begin);
  buffer_point_clamp(b, &b->f->s->mark_end);
 }
}

/* Commit a replay item: the record's lines are the buffer's, its inverse
   is on the other list, the cursor and the block are where it saw them. */
static void buffer_txn_commit_replay(WpeBufferTxnSplice *sp)
{
 BUFFER *b = sp->b;
 Undo *rec = sp->rec;

 b->b = rec->b;
 buffer_splice_commit(b, sp->y, sp->n_del, sp->lines, sp->n_ins, sp->brk, sp->nbrk,
                      E_BUFFER_STEP_REPLAY, sp->no_changed, NULL, NULL, NULL, sp->pocket,
                      BUFFER_ROWS_POSITIONAL, &sp->m);
 free(rec->old);
 rec->old = NULL;
 rec->n_old = 0;
 buffer_replay_view_back(b, rec);
}

/* The shell gave its span's lines away: the guards (and the span, when the
   buffer took its canonical line instead) are freed, and it holds none. */
static void buffer_shell_lines_given(BUFFER *shell, int g_top, int n_span, int adopted)
{
 int i;

 for (i = 0; i < shell->mxlines; i++)
  if (!adopted || i < g_top || i >= g_top + n_span)
   free(shell->bf[i].s);
 shell->mxlines = 0;
}

/* The inverses a replay pushed in its shell move onto the user's buffer,
   oldest first, each its own record of the step the buffer is in, on the
   row of the buffer: row 0 of the shell is row @off. */
static void buffer_shell_records_move(BUFFER *shell, BUFFER *b, int off)
{
 Undo *rec, *next, *oldest = NULL;

 for (rec = shell->ud; rec != NULL; rec = next)
 {
  next = rec->next;
  rec->next = oldest;
  oldest = rec;
 }
 shell->ud = NULL;
 for (rec = oldest; rec != NULL; rec = next)
 {
  next = rec->next;
  rec->next = NULL;
  rec->flags &= ~E_UNDO_MORE;
  rec->b.y += off;
  e_buffer_record_push(b, rec);
 }
}

/* The marks the replay set or kept in its shell, and its cursor, are the
   buffer's now, on its rows; a mark the shell does not hold stays where
   the commit moved it. */
static void buffer_shell_marks_back(BUFFER *shell, BUFFER *b, int off)
{
 POINT *in[BUFFER_MARKS_MAX], *out[BUFFER_MARKS_MAX];
 int n_in = buffer_marks(shell, in), n_out = buffer_marks(b, out), i;

 for (i = 0; i < n_in && i < n_out; i++)
 {
  if (in[i]->y < 0)
   continue;
  out[i]->x = in[i]->x;
  out[i]->y = in[i]->y + off;
  buffer_point_clamp(b, out[i]);
 }
 b->b.x = shell->b.x;
 b->b.y = shell->b.y + off;
 buffer_point_clamp(b, &b->b);
}

/* Commit a rewrite item: the shell's span lines are the buffer's with the
   rows the replay left them, its inverses on the buffer's list, its marks
   and cursor the window's; then the shell is freed. */
static void buffer_txn_commit_rewrite(WpeBufferTxnSplice *sp)
{
 BUFFER *b = sp->b, *shell = sp->shell;
 int off = sp->y - sp->g_top, adopted = !sp->m.need_canon;

 buffer_splice_commit(b, sp->y, sp->n_del, sp->lines, sp->n_ins, sp->brk, sp->nbrk,
                      E_BUFFER_STEP_COVERED, 1, NULL, NULL, NULL, NULL, BUFFER_ROWS_REWRITE,
                      &sp->m);
 buffer_shell_lines_given(shell, sp->g_top, sp->n_ins, adopted);
 buffer_shell_records_move(shell, b, off);
 buffer_shell_marks_back(shell, b, off);
 e_buffer_shell_free(shell);
 sp->shell = NULL;
 free((void *) sp->brk);
 sp->brk = NULL;
}

/**
 * e_buffer_line_copy - A copy of line @src as a row of @b.
 * @out: receives the copy, the caller's until a splice adopts it.
 *
 * Its bytes through the terminating zero, its length and count kept, so a
 * shell's guard row can be compared byte for byte afterwards, and a case
 * change or a block copy starts from exactly the row the user sees.  A line
 * wider than a row of @b is cut and measured again, and then no longer
 * matches.  Return: E_BUFFER_OK or E_BUFFER_NOMEM.
 */
int e_buffer_line_copy(BUFFER *b, const STRING *src, STRING *out)
{
 size_t n = buffer_line_kept_bytes(b, src);

 if (e_buffer_line_new(b, out) != E_BUFFER_OK)
  return(E_BUFFER_NOMEM);
 memcpy(out->s, src->s, n);
 out->len = src->len;
 out->nrc = src->nrc;
 if ((int)n <= src->nrc)
 {
  out->s[n - 1] = '\0';
  buffer_line_measure(out);
 }
 return(E_BUFFER_OK);
}

/* How many of rows [y0, y0 + n) of b carry a breakpoint once the items of
   t commit.  Return: the count, or -1 for a row the view cannot read. */
static int buffer_view_rows(WpeBufferTxn *t, BUFFER *b, int y0, int n)
{
 const STRING *l;
 int i, row, count = 0;

 for (i = 0; i < n; i++)
 {
  if (buffer_txn_view(t, b, y0 + i, &l, &row) != E_BUFFER_OK)
   return(-1);
  count += row;
 }
 return(count);
}

/* The shell's block marks and last find are unset: a replay that leaves
   one unset leaves the user's where it was. */
static void buffer_shell_marks_unset(BUFFER *shell)
{
 POINT *m[BUFFER_MARKS_MAX];
 int i, n = buffer_marks(shell, m);

 for (i = 0; i < n; i++)
 {
  m[i]->x = -1;
  m[i]->y = -1;
 }
}

/**
 * e_buffer_shell_fill - Fill a fresh shell with copies of rows [@y0, @y0 + @n)
 * of @b as the items of @t leave them, with their breakpoint rows.
 * @shell: from e_buffer_shell_new, holding its one empty line; @t: the
 * change being prepared (no items: the buffer as it stands).
 *
 * A character Undo then replays its primitive on the copies, so a replay
 * refused for want of memory changes nothing the user has.  Every byte of
 * each line is copied, through its terminating zero, so a row the replay
 * did not touch compares equal afterwards.  The shell's marks start unset.
 * Return: E_BUFFER_OK, E_BUFFER_NOMEM (the shell as it was) or
 * E_BUFFER_RANGE.
 */
int e_buffer_shell_fill(BUFFER *shell, WpeBufferTxn *t, BUFFER *b, int y0, int n)
{
 const STRING *l;
 STRING *lines;
 int *rows = NULL, nrows, i, row, r;

 if (!e_buffer_is_shell(shell) || n < 1)
  return(E_BUFFER_RANGE);
 if ((nrows = buffer_view_rows(t, b, y0, n)) < 0)
  return(E_BUFFER_RANGE);
 if (buffer_brp(shell) == NULL)
  nrows = 0;
 if ((lines = buffer_array(n, sizeof(STRING))) == NULL)
  return(E_BUFFER_NOMEM);
 if (nrows > 0 && (rows = buffer_array(nrows, sizeof(int))) == NULL)
 {
  free(lines);
  return(E_BUFFER_NOMEM);
 }
 for (i = 0, nrows = 0; i < n; i++)
 {
  buffer_txn_view(t, b, y0 + i, &l, &row);
  if (e_buffer_line_copy(shell, l, &lines[i]) != E_BUFFER_OK)
  {
   e_buffer_lines_free(lines, i);
   free(rows);
   return(E_BUFFER_NOMEM);
  }
  if (row && rows != NULL)
   rows[nrows++] = i;
 }
 r = buffer_splice(shell, 0, shell->mxlines, lines, n, rows, nrows, E_BUFFER_STEP_COVERED, 1,
                   NULL, NULL, NULL);
 free(rows);
 if (r != E_BUFFER_OK)
 {
  e_buffer_lines_free(lines, n);
  return(r);
 }
 free(lines);
 buffer_shell_marks_unset(shell);
 return(E_BUFFER_OK);
}

/**
 * e_buffer_txn_record - The undo record of the operation, reserved with
 * the rest of the change.
 * @kind: 'c', 'v', 'd' and the like.
 *
 * An operation that could not be undone is not made: the record is taken
 * before anything moves.  After a commit it is the caller's to fill and to
 * push (e_buffer_record_push, which takes no memory); a refused or aborted
 * change frees it.  Return: the record, or NULL with the change refused.
 */
Undo *e_buffer_txn_record(WpeBufferTxn *t, int kind)
{
 if (t->refused != E_BUFFER_OK)
  return(NULL);
 if (t->rec == NULL && (t->rec = e_buffer_record_new(kind)) == NULL)
  buffer_txn_refuse(t, E_BUFFER_NOMEM);
 return(t->rec);
}

/**
 * e_buffer_txn_abort - Give back everything the change took: both buffers,
 * their marks, breakpoints and undo are as they were.
 */
void e_buffer_txn_abort(WpeBufferTxn *t)
{
 int i;

 for (i = 0; i < t->nsp; i++)
  buffer_txn_item_free(&t->sp[i]);
 e_buffer_record_free(t->rec);
 t->rec = NULL;
 buffer_txn_release(t);
}

/* Room on each buffer's margin and pocket for every row the items may add,
   reserved once per buffer with the sum. */
static int buffer_txn_reserve_rows(WpeBufferTxn *t)
{
 int i;

 for (i = 0; i < t->nbuf; i++)
  if (buffer_brp_reserve(t->buf[i].b, t->buf[i].brp_rows) != E_BUFFER_OK ||
      buffer_pocket_reserve(t->buf[i].pocket, t->buf[i].pocket_rows) != E_BUFFER_OK)
   return(E_BUFFER_NOMEM);
 return(E_BUFFER_OK);
}

/* The landing of a move between buffers of different widths: the lines
   built at the new width go in with their breakpoints, and the originals the
   detach took out are freed.  It cannot fail. */
static void buffer_txn_commit_rebuilt(WpeBufferTxnSplice *sp, WpeBufferTxnSplice *src)
{
 buffer_splice_commit(sp->b, sp->y, 0, sp->lines, sp->n_ins, sp->brk, sp->nbrk, sp->step, 0,
                      NULL, NULL, NULL, NULL, BUFFER_ROWS_POSITIONAL, &sp->m);
 free(sp->lines);
 free((void *) sp->brk);
 sp->lines = NULL;
 sp->brk = NULL;
 e_buffer_lines_free(src->out, src->n_del);
 free(src->brk_out);
 src->out = NULL;
 src->brk_out = NULL;
}

/* Commit one prepared splice: it cannot fail. */
static void buffer_txn_commit_one(WpeBufferTxn *t, WpeBufferTxnSplice *sp)
{
 WpeBufferTxnSplice *src;

 if (sp->kind == E_BUFFER_ITEM_REPLAY)
 {
  buffer_txn_commit_replay(sp);
  return;
 }
 if (sp->kind == E_BUFFER_ITEM_REWRITE)
 {
  buffer_txn_commit_rewrite(sp);
  return;
 }
 if (sp->detach)
 {
  buffer_splice_commit(sp->b, sp->y, sp->n_del, NULL, 0, NULL, 0, E_BUFFER_STEP_COVERED, 1,
                       sp->out, sp->brk_out, &sp->nbrk_out, NULL, BUFFER_ROWS_POSITIONAL,
                       &sp->m);
  return;
 }
 if (sp->landed_from == BUFFER_TXN_NOT_LANDED)
 {
  buffer_splice_commit(sp->b, sp->y, sp->n_del, sp->lines, sp->n_ins, NULL, 0, sp->step, 0,
                       NULL, NULL, NULL, sp->pocket, BUFFER_ROWS_POSITIONAL, &sp->m);
  free(sp->lines);
  return;
 }
 src = &t->sp[sp->landed_from];
 if (sp->lines != NULL)
 {
  buffer_txn_commit_rebuilt(sp, src);
  return;
 }
 buffer_splice_commit(sp->b, sp->y, 0, src->out, sp->n_ins, src->brk_out, src->nbrk_out,
                      sp->step, 0, NULL, NULL, NULL, NULL, BUFFER_ROWS_POSITIONAL, &sp->m);
 free(src->out);
 free(src->brk_out);
 src->out = NULL;
 src->brk_out = NULL;
}

/**
 * e_buffer_txn_commit - Make the change: every item, in order, or none.
 *
 * The last memory (room on the margins for the breakpoints that arrive) is
 * taken first; then every splice commits, and a commit cannot fail.  A
 * change refused at any point, here or while its items were added, is
 * given back whole (e_buffer_txn_abort).  After success the record from
 * e_buffer_txn_record is the caller's.
 * Return: E_BUFFER_OK, E_BUFFER_NOMEM or E_BUFFER_RANGE.
 */
int e_buffer_txn_commit(WpeBufferTxn *t)
{
 int i;

 if (t->refused == E_BUFFER_OK && buffer_txn_reserve_rows(t) != E_BUFFER_OK)
  buffer_txn_refuse(t, E_BUFFER_NOMEM);
 if (t->refused != E_BUFFER_OK)
 {
  e_buffer_txn_abort(t);
  return(t->refused);
 }
 for (i = 0; i < t->nsp; i++)
  buffer_txn_commit_one(t, &t->sp[i]);
 buffer_txn_release(t);
 t->rec = NULL;
 return(E_BUFFER_OK);
}

/* ---- the diff of a whole-buffer swap ---- */

/* One splice of the swap: at old line old_at, n_del lines become the n_ins
   new lines from new_at.  Both positions count from the start of the middle,
   and the hunks come in order. */
typedef struct
{
 int old_at, n_del, new_at, n_ins;
} BufferHunk;

/* The lines being compared, one level at a time.  keyed: lines count as the
   same when they differ only in blanks and tabs (a re-indented line). */
typedef struct
{
 const STRING *a, *b;
 const unsigned long *ha, *hb;
 int keyed;
} BufferSeq;

/* The middle of a swap being diffed: both sides, their hashes, the hunks. */
typedef struct
{
 const STRING *old_lines, *new_lines;
 unsigned long *hx_old, *hx_new, *hk_old, *hk_new;
 BufferHunk *hunks;
 int nhunks, cap;
} BufferDiff;

#define BUFFER_HASH_SEED  5381UL

static int buffer_is_blank(unsigned char c)
{
 return(c == ' ' || c == '\t');
}

/* A hash of the bytes of a line, or of its bytes without blanks (keyed). */
static unsigned long buffer_hash_line(const STRING *l, int keyed)
{
 unsigned long h = BUFFER_HASH_SEED;
 int i;

 for (i = 0; i < l->len; i++)
 {
  if (keyed && buffer_is_blank(l->s[i]))
   continue;
  h = (h * 33UL) ^ l->s[i];
 }
 if (!keyed)
  h = (h * 33UL) ^ l->s[l->len];
 return(h);
}

static int buffer_keys_equal(const STRING *a, const STRING *b)
{
 int i = 0, j = 0;

 for (;;)
 {
  while (i < a->len && buffer_is_blank(a->s[i]))
   i++;
  while (j < b->len && buffer_is_blank(b->s[j]))
   j++;
  if (i == a->len || j == b->len)
   return(i == a->len && j == b->len);
  if (a->s[i++] != b->s[j++])
   return(0);
 }
}

static int buffer_seq_same(const BufferSeq *q, int i, int j)
{
 if (q->ha[i] != q->hb[j])
  return(0);
 if (q->keyed)
  return(buffer_keys_equal(&q->a[i], &q->b[j]));
 return(buffer_lines_equal(&q->a[i], &q->b[j]));
}

/* Walk back through the saved search states from the end of both sequences
   and write, for every line of a that is also in b, where it is. */
static void buffer_myers_trace_back(int *const *trace, int d, int na, int nb, int *match)
{
 int x = na, y = nb, k, prev_k, prev_x, prev_y, mid_x, mid_y;

 for (; d > 0; d--)
 {
  k = x - y;
  if (k == -d || (k != d && trace[d][k - 1 + d] < trace[d][k + 1 + d]))
   prev_k = k + 1;
  else
   prev_k = k - 1;
  prev_x = trace[d][prev_k + d];
  prev_y = prev_x - prev_k;
  if (prev_k == k + 1)
   mid_x = prev_x;
  else
   mid_x = prev_x + 1;
  mid_y = mid_x - k;
  while (x > mid_x && y > mid_y)
  {
   x--;
   y--;
   match[x] = y;
  }
  x = prev_x;
  y = prev_y;
 }
 while (x > 0 && y > 0)
 {
  x--;
  y--;
  match[x] = y;
 }
}

/* The furthest x reached on diagonal k after d edits, the new best on k. */
static int buffer_myers_reach(const BufferSeq *q, int na, int nb, const int *v, int d, int k)
{
 int x, y;

 if (k == -d || (k != d && v[k - 1] < v[k + 1]))
  x = v[k + 1];
 else
  x = v[k - 1] + 1;
 y = x - k;
 while (x < na && y < nb && buffer_seq_same(q, x, y))
 {
  x++;
  y++;
 }
 return(x);
}

/**
 * buffer_myers - Pair the lines two sequences share, in order.
 * @q: the sequences; @na, @nb: their lengths; @max_d: the most edits to look
 * for; @match: receives, for every line of a, its place in b or -1.
 *
 * The shortest edit script of Myers' O(ND) search, kept to @max_d edits so a
 * rewrite of the whole file costs a bounded time and memory.
 * Return: 1 when @match is filled; 0 when the sequences differ by more than
 * @max_d edits or memory ran out (the caller then takes them as one hunk).
 */
static int buffer_myers(const BufferSeq *q, int na, int nb, int max_d, int *match)
{
 int *v, **trace;
 int off, d, k, x, found = -1, i;

 if (max_d > na + nb)
  max_d = na + nb;
 off = max_d + 1;
 v = buffer_malloc((size_t)(2 * max_d + 3) * sizeof(int));
 trace = buffer_malloc((size_t)(max_d + 1) * sizeof(int *));
 if (v == NULL || trace == NULL)
 {
  free(v);
  free(trace);
  return(0);
 }
 memset(v, 0, (size_t)(2 * max_d + 3) * sizeof(int));
 memset(trace, 0, (size_t)(max_d + 1) * sizeof(int *));
 for (i = 0; i < na; i++)
  match[i] = -1;
 for (d = 0; d <= max_d && found < 0; d++)
 {
  if (d > 0)
  {
   trace[d] = buffer_malloc((size_t)(2 * d + 1) * sizeof(int));
   if (trace[d] == NULL)
    break;
   memcpy(trace[d], v + off - d, (size_t)(2 * d + 1) * sizeof(int));
  }
  for (k = -d; k <= d; k += 2)
  {
   x = buffer_myers_reach(q, na, nb, v + off, d, k);
   v[off + k] = x;
   if (k == na - nb && x >= na)
   {
    found = d;
    break;
   }
  }
 }
 if (found >= 0)
  buffer_myers_trace_back(trace, found, na, nb, match);
 for (i = 0; i <= max_d; i++)
  free(trace[i]);
 free(trace);
 free(v);
 return(found >= 0);
}

static int buffer_diff_emit(BufferDiff *d, int old_at, int n_del, int new_at, int n_ins)
{
 BufferHunk *tmp;

 if (n_del == 0 && n_ins == 0)
  return(1);
 if (d->nhunks == d->cap)
 {
  tmp = buffer_realloc(d->hunks, (size_t)(d->cap * 2 + 8) * sizeof(BufferHunk));
  if (tmp == NULL)
   return(0);
  d->hunks = tmp;
  d->cap = d->cap * 2 + 8;
 }
 d->hunks[d->nhunks].old_at = old_at;
 d->hunks[d->nhunks].n_del = n_del;
 d->hunks[d->nhunks].new_at = new_at;
 d->hunks[d->nhunks].n_ins = n_ins;
 d->nhunks++;
 return(1);
}

static int buffer_diff_gap(BufferDiff *d, int oa, int na, int ob, int nb);

/* Go through one level's pairing: what lies between two pairs is a gap, and
   a run of pairs is a run of lines that stand in both texts.  At the exact
   level the runs are lines that did not change and the gaps go on to the
   keyed level; at the keyed level the runs are lines changed in place (the
   same line re-indented) and the gaps are insertions, deletions or lines
   changed beyond recognition. */
static int buffer_diff_walk(BufferDiff *d, const int *match, int oa, int na, int ob,
                            int nb, int keyed)
{
 int i = 0, j = 0, next, end, run;

 while (i < na || j < nb)
 {
  next = i;
  while (next < na && match[next] < 0)
   next++;
  end = (next == na) ? nb : match[next];
  if (next > i || end > j)
  {
   if (keyed && !buffer_diff_emit(d, oa + i, next - i, ob + j, end - j))
    return(0);
   if (!keyed && !buffer_diff_gap(d, oa + i, next - i, ob + j, end - j))
    return(0);
  }
  if (next == na)
   break;
  run = 1;
  while (next + run < na && match[next + run] == match[next] + run)
   run++;
  if (keyed && !buffer_diff_emit(d, oa + next, run, ob + end, run))
   return(0);
  i = next + run;
  j = end + run;
 }
 return(1);
}

/* Pair the lines of one gap with the blanks left out, or take the gap whole. */
static int buffer_diff_gap(BufferDiff *d, int oa, int na, int ob, int nb)
{
 BufferSeq q;
 int *match, ok;

 if (na == 0 || nb == 0)
  return(buffer_diff_emit(d, oa, na, ob, nb));
 q.a = d->old_lines + oa;
 q.b = d->new_lines + ob;
 q.ha = d->hk_old + oa;
 q.hb = d->hk_new + ob;
 q.keyed = 1;
 match = buffer_malloc((size_t)na * sizeof(int));
 if (match == NULL || !buffer_myers(&q, na, nb, E_BUFFER_DIFF_MAX, match))
 {
  free(match);
  return(buffer_diff_emit(d, oa, na, ob, nb));
 }
 ok = buffer_diff_walk(d, match, oa, na, ob, nb, 1);
 free(match);
 return(ok);
}

/* The exact level over the whole middle; what it cannot pair goes on. */
static int buffer_diff_exact(BufferDiff *d, int na, int nb)
{
 BufferSeq q;
 int *match, ok;

 if (na == 0 || nb == 0)
  return(buffer_diff_emit(d, 0, na, 0, nb));
 q.a = d->old_lines;
 q.b = d->new_lines;
 q.ha = d->hx_old;
 q.hb = d->hx_new;
 q.keyed = 0;
 match = buffer_malloc((size_t)na * sizeof(int));
 if (match == NULL || !buffer_myers(&q, na, nb, E_BUFFER_DIFF_MAX, match))
 {
  free(match);
  return(buffer_diff_gap(d, 0, na, 0, nb));
 }
 ok = buffer_diff_walk(d, match, 0, na, 0, nb, 0);
 free(match);
 return(ok);
}

/**
 * buffer_diff_middle - Cut the middle of a swap into hunks.
 * @old_lines, @na: the lines now in the buffer; @new_lines, @nb: the lines
 * that are to be there.
 *
 * Two levels: Myers on exact lines first, then, inside every stretch it
 * cannot pair, Myers on the lines without their blanks.  A re-indented line
 * pairs with its old self and is a changed row; the lines left over are
 * insertions and deletions.  Return: the hunks in @d, 1; or 0 when memory
 * ran out (the caller takes the whole middle as one hunk).
 */
static int buffer_diff_middle(BufferDiff *d, const STRING *old_lines, int na,
                              const STRING *new_lines, int nb)
{
 int i;
 unsigned long *hashes;

 memset(d, 0, sizeof(*d));
 d->old_lines = old_lines;
 d->new_lines = new_lines;
 hashes = buffer_malloc((size_t)(2 * na + 2 * nb) * sizeof(unsigned long));
 if (hashes == NULL)
  return(0);
 d->hx_old = hashes;
 d->hk_old = hashes + na;
 d->hx_new = hashes + 2 * na;
 d->hk_new = hashes + 2 * na + nb;
 for (i = 0; i < na; i++)
 {
  d->hx_old[i] = buffer_hash_line(&old_lines[i], 0);
  d->hk_old[i] = buffer_hash_line(&old_lines[i], 1);
 }
 for (i = 0; i < nb; i++)
 {
  d->hx_new[i] = buffer_hash_line(&new_lines[i], 0);
  d->hk_new[i] = buffer_hash_line(&new_lines[i], 1);
 }
 if (buffer_diff_exact(d, na, nb))
 {
  free(hashes);
  return(1);
 }
 free(hashes);
 free(d->hunks);
 d->hunks = NULL;
 d->nhunks = 0;
 return(0);
}

/* The one hunk of the whole middle: what a swap does when it cannot diff. */
static int buffer_diff_one_hunk(BufferDiff *d, int na, int nb)
{
 d->hunks = NULL;
 d->nhunks = d->cap = 0;
 return(buffer_diff_emit(d, 0, na, 0, nb));
}

/* A finer cut is worth its search: the buffer keeps breakpoints, colours and
   undo.  Every splice, covered or not, moves the rows below it, so a hunk
   measured where it stands before any of them lands meets exactly the rows
   it measured, inside a block operation's cover too. */
static int buffer_swap_cuts_finely(BUFFER *b)
{
 return(buffer_keeps_history(b));
}

/**
 * buffer_cut_middle - The hunks of a swap: cut by the diff, or one.
 * @b: the buffer; @head: where the middle starts; @na, @nb: the lines it
 * has now and the lines it is to have; @new_lines: the new ones.
 *
 * A buffer that keeps no breakpoints, colours or undo (a view, or one whose
 * window has closed) has nothing a finer cut would keep, so it takes the
 * whole middle as one hunk and pays for no search.  Return: 1, or 0 when
 * memory ran out.
 */
static int buffer_cut_middle(BUFFER *b, BufferDiff *d, int head, int na,
                             const STRING *new_lines, int nb)
{
 if (buffer_swap_cuts_finely(b) && buffer_diff_middle(d, b->bf + head, na, new_lines, nb))
  return(1);
 return(buffer_diff_one_hunk(d, na, nb));
}

/* What one hunk of a swap needs, taken before the first hunk lands. */
typedef struct
{
 BufferSpliceMem m;
 int unchanged;       /* its new lines are the old ones byte for byte: no splice */
} BufferHunkMem;

static void buffer_swap_mem_free(BufferHunkMem *hm, int n)
{
 int h;

 for (h = 0; h < n; h++)
  buffer_splice_mem_free(&hm[h].m);
 free(hm);
}

/* The hunk's new lines are its old ones byte for byte (a pairing of
   re-indented lines can hold some that did not change at all). */
static int buffer_hunk_unchanged(BUFFER *b, int y, const BufferHunk *hk, const STRING *lines)
{
 int i;

 if (hk->n_del != hk->n_ins)
  return(0);
 for (i = 0; i < hk->n_del; i++)
  if (!buffer_lines_equal(&b->bf[y + i], &lines[i]))
   return(0);
 return(1);
}

/* Room for the swap as a whole: the table for the most lines it can reach
   on its way, the pocket for every row its hunks can take off. */
static int buffer_swap_reserve(BUFFER *b, int head, const BufferDiff *d, Undo *pocket)
{
 int h, grow = 0, rows = 0;
 const BufferHunk *hk;

 for (h = 0; h < d->nhunks; h++)
 {
  hk = &d->hunks[h];
  if (hk->n_ins > hk->n_del)
   grow += hk->n_ins - hk->n_del;
  rows += brp_count_in(buffer_brp(b), head + hk->old_at, head + hk->old_at + hk->n_del);
 }
 if (buffer_grow(b, b->mxlines + grow) != E_BUFFER_OK)
  return(E_BUFFER_NOMEM);
 return(buffer_pocket_reserve(pocket, rows));
}

/**
 * buffer_swap_prepare - Take the memory of every hunk of a swap at once.
 * @b: the buffer; @lines: the new text's lines; @head: where the middle
 * starts; @d: the hunks; @records: whether the splices record; @pocket:
 * where removed rows go when they do not; @out: receives the hunks' memory.
 *
 * Every hunk is measured where it stands before any of them lands: the
 * hunks above it only move it, they bring no breakpoint row into it.  So
 * memory refused for any hunk leaves the text exactly as the user had it.
 * Return: E_BUFFER_OK, or E_BUFFER_NOMEM with nothing taken.
 */
static int buffer_swap_prepare(BUFFER *b, const STRING *lines, int head, const BufferDiff *d,
                               int records, Undo *pocket, BufferHunkMem **out)
{
 BufferHunkMem *hm;
 const BufferHunk *hk;
 int h;

 *out = NULL;
 if (d->nhunks == 0)
  return(E_BUFFER_OK);
 hm = buffer_malloc((size_t)d->nhunks * sizeof(BufferHunkMem));
 if (hm == NULL)
  return(E_BUFFER_NOMEM);
 memset(hm, 0, (size_t)d->nhunks * sizeof(BufferHunkMem));
 if (buffer_swap_reserve(b, head, d, pocket) != E_BUFFER_OK)
 {
  free(hm);
  return(E_BUFFER_NOMEM);
 }
 for (h = 0; h < d->nhunks; h++)
 {
  hk = &d->hunks[h];
  hm[h].unchanged = buffer_hunk_unchanged(b, head + hk->old_at, hk, lines + head + hk->new_at);
  if (hm[h].unchanged)
   continue;
  if (buffer_splice_prepare(b, head + hk->old_at, hk->n_del, hk->n_ins, 0, records, 0,
                            NULL, &hm[h].m) != E_BUFFER_OK)
  {
   buffer_swap_mem_free(hm, h);
   return(E_BUFFER_NOMEM);
  }
 }
 *out = hm;
 return(E_BUFFER_OK);
}

/**
 * buffer_swap_hunks - Land the hunks of a swap, all of them or none.
 * @b: the buffer; @lines: the new text's lines; @used: marks the lines a
 * hunk took in; @head: where the middle starts; @d: the hunks; @step: how
 * the swap counts for Ctrl-U.
 *
 * Top-down, each hunk at its old place moved by what the hunks above it
 * added or removed.  A swap that counts for Ctrl-U is one step, so one key
 * takes all of it back.  The memory of every hunk is taken first, so when
 * memory runs out the user's text, breakpoints and undo are as they were.
 * Return: E_BUFFER_OK, or E_BUFFER_NOMEM with nothing changed.
 */
static int buffer_swap_hunks(BUFFER *b, STRING *lines, char *used, int head,
                             const BufferDiff *d, WpeBufferStep step)
{
 int h, offset = 0, i, records = buffer_splice_records(b, step);
 Undo *pocket = buffer_splice_pocket(b, records);
 const BufferHunk *hk;
 BufferHunkMem *hm;

 if (buffer_swap_prepare(b, lines, head, d, records, pocket, &hm) != E_BUFFER_OK)
  return(E_BUFFER_NOMEM);
 if (step == E_BUFFER_STEP_NEW)
  e_buffer_step_begin(b);
 for (h = 0; h < d->nhunks; h++)
 {
  hk = &d->hunks[h];
  if (!hm[h].unchanged)
  {
   buffer_splice_commit(b, head + hk->old_at + offset, hk->n_del,
                        lines + head + hk->new_at, hk->n_ins, NULL, 0, step, 0,
                        NULL, NULL, NULL, pocket, BUFFER_ROWS_POSITIONAL, &hm[h].m);
   for (i = 0; i < hk->n_ins; i++)
    used[head + hk->new_at + i] = 1;
  }
  offset += hk->n_ins - hk->n_del;
 }
 if (step == E_BUFFER_STEP_NEW)
  e_buffer_step_end(b);
 free(hm);
 return(E_BUFFER_OK);
}

/* How many lines at the start, then at the end, the buffer and the new text
   have in common byte for byte. */
static int buffer_common_head(BUFFER *b, const STRING *lines, int n)
{
 int head = 0;

 while (head < n && head < b->mxlines && buffer_lines_equal(&b->bf[head], &lines[head]))
  head++;
 return(head);
}

static int buffer_common_tail(BUFFER *b, const STRING *lines, int n, int head)
{
 int tail = 0;

 while (tail < n - head && tail < b->mxlines - head &&
        buffer_lines_equal(&b->bf[b->mxlines - 1 - tail], &lines[n - 1 - tail]))
  tail++;
 return(tail);
}

/**
 * e_buffer_replace_all - Make the buffer hold `text`, changing only what differs.
 * @step: how the change counts for Ctrl-U.
 *
 * A tool's rewrite of the file (an accepted Edit, a rename, a format) leaves
 * the lines it did not touch where they were: their breakpoints, block
 * marks and colours stay on their code, and one Ctrl-U gives back the old
 * lines.  A line the tool only re-indented counts as the same line.  The
 * common leading and trailing lines are kept and the middle is cut into
 * hunks; a rewrite too different to cut (more than E_BUFFER_DIFF_MAX
 * edits) is one hunk, as a change to the whole middle always was.
 * Return: as e_buffer_splice; identical text changes nothing and records
 * nothing.
 */
int e_buffer_replace_all(BUFFER *b, const char *text, WpeBufferStep step)
{
 STRING *lines;
 BufferDiff d;
 char *used;
 int n, head, tail, na, nb, r, i;

 lines = e_buffer_lines_from_text(text, b->mx.x, &n);
 if (n < 0)
  return(E_BUFFER_NOMEM);
 if (n == 0)
 {
  if (b->mxlines == 1 && b->bf[0].len == 0)
   return(E_BUFFER_OK);         /* empty text over the one empty line */
  return(buffer_editor_splice(b, 0, b->mxlines, NULL, 0, step));
 }
 if (!buffer_lines_fit(b, 0, lines, n))
 {
  e_buffer_lines_free(lines, n);
  return(E_BUFFER_RANGE);
 }
 used = buffer_malloc((size_t)n);
 if (used == NULL)
 {
  e_buffer_lines_free(lines, n);
  return(E_BUFFER_NOMEM);
 }
 memset(used, 0, (size_t)n);
 head = buffer_common_head(b, lines, n);
 tail = buffer_common_tail(b, lines, n, head);
 na = b->mxlines - head - tail;
 nb = n - head - tail;
 if (!buffer_cut_middle(b, &d, head, na, lines + head, nb))
 {
  e_buffer_lines_free(lines, n);
  free(used);
  return(E_BUFFER_NOMEM);
 }
 r = buffer_swap_hunks(b, lines, used, head, &d, step);
 free(d.hunks);
 for (i = 0; i < n; i++)
  if (!used[i])
   free(lines[i].s);
 free(lines);
 free(used);
 return(r);
}

/**
 * e_buffer_append_text - Add text after the last line.
 * @skip_blank: drop empty lines, as build output in Messages does.
 * Return: as e_buffer_splice.
 */
int e_buffer_append_text(BUFFER *b, const char *text, int skip_blank)
{
 STRING *lines;
 int n, k = 0, i, r;

 lines = e_buffer_lines_from_text(text, b->mx.x, &n);
 if (n < 0)
  return(E_BUFFER_NOMEM);
 for (i = 0; i < n; i++)
 {
  if (skip_blank && lines[i].len == 0)
  {
   free(lines[i].s);
   continue;
  }
  lines[k++] = lines[i];
 }
 r = buffer_editor_splice(b, b->mxlines, 0, lines, k, E_BUFFER_STEP_NEW);
 if (r != E_BUFFER_OK)
  for (i = 0; i < k; i++)
   free(lines[i].s);
 free(lines);
 return(r);
}

/* How many non-empty lines `text` holds, split on '\n'. */
static int buffer_count_rows(const char *text)
{
 const char *p = text, *nl;
 int n = 0;

 while (*p)
 {
  nl = strchr(p, '\n');
  if (nl == NULL)
   return(n + 1);
  if (nl > p)
   n++;
  p = nl + 1;
 }
 return(n);
}

/* One row holding `len` bytes of `src` whole, ended with a line break.  It
   has the room of every row of the buffer, and room for the whole text when
   that is wider (a view's long line). */
static int buffer_row_ended(BUFFER *b, const char *src, int len, int term, STRING *out)
{
 size_t room = (size_t)len + 2;

 if (room < (size_t)E_BUFFER_ROW_BYTES(b->mx.x))
  room = (size_t)E_BUFFER_ROW_BYTES(b->mx.x);
 out->s = buffer_malloc(room);
 if (out->s == NULL)
  return(E_BUFFER_NOMEM);
 memset(out->s, 0, room);
 memcpy(out->s, src, (size_t)len);
 out->s[len] = (unsigned char) term;
 buffer_line_measure(out);
 return(E_BUFFER_OK);
}

static int buffer_row_whole(BUFFER *b, const char *src, int len, STRING *out)
{
 return(buffer_row_ended(b, src, len, WPE_WR, out));
}

/**
 * e_buffer_append_rows - Add text after the last line, one row for each of
 * its lines however wide, the empty ones left out.
 * @text: lines split on '\n'.
 *
 * A compiler's line in Messages stays one row, as it always did: the error
 * walk reads the file name and the line number from one row, and a long
 * path cut onto the next row would name no file to jump to.
 * Return: as e_buffer_splice; without memory nothing changes.
 */
int e_buffer_append_rows(BUFFER *b, const char *text)
{
 const char *p = text ? text : "", *nl;
 STRING *lines;
 int n = buffer_count_rows(p), k = 0, len, r;

 if (n == 0)
  return(E_BUFFER_OK);
 lines = buffer_malloc((size_t)n * sizeof(STRING));
 if (lines == NULL)
  return(E_BUFFER_NOMEM);
 while (*p && k < n)
 {
  nl = strchr(p, '\n');
  len = nl ? (int)(nl - p) : (int)strlen(p);
  if (len > 0 && buffer_row_whole(b, p, len, &lines[k++]) != E_BUFFER_OK)
  {
   e_buffer_lines_free(lines, k - 1);
   return(E_BUFFER_NOMEM);
  }
  if (nl == NULL)
   break;
  p = nl + 1;
 }
 r = buffer_editor_splice(b, b->mxlines, 0, lines, k, E_BUFFER_STEP_NEW);
 if (r != E_BUFFER_OK)
  e_buffer_lines_free(lines, k);
 else
  free(lines);
 return(r);
}

/**
 * e_buffer_clear - Leave the buffer with one empty line.
 * Return: as e_buffer_splice.
 */
int e_buffer_clear(BUFFER *b)
{
 return(buffer_editor_splice(b, 0, b->mxlines, NULL, 0, E_BUFFER_STEP_NEW));
}

/**
 * e_buffer_set_line_ended - Replace the bytes of row y and say how it ends.
 * @text: the new bytes, without a line break; @n: how many, all kept even when
 * the row is wider than the buffer (the assistant's pane writes a long status
 * whole and wraps it when it paints); @term: WPE_WR for a row that ends with a
 * line break, '\0' for one that goes on in the row below (a wrapped line).
 *
 * A changed row: its breakpoint and the marks on it stay.  Recorded unless
 * covered or on a view.  Return: as e_buffer_splice.
 */
int e_buffer_set_line_ended(BUFFER *b, int y, const char *text, int n, int term)
{
 STRING line;
 int r;

 if (y < 0 || y >= b->mxlines || n < 0)
  return(E_BUFFER_RANGE);
 if (buffer_row_ended(b, text, n, term, &line) != E_BUFFER_OK)
  return(E_BUFFER_NOMEM);
 r = buffer_editor_splice(b, y, 1, &line, 1, E_BUFFER_STEP_NEW);
 if (r != E_BUFFER_OK)
  free(line.s);
 return(r);
}

/**
 * e_buffer_set_line - Replace the bytes of row y, keeping its line break.
 * @text: the new bytes; @n: how many, all kept.
 *
 * Return: as e_buffer_set_line_ended.
 */
int e_buffer_set_line(BUFFER *b, int y, const char *text, int n)
{
 if (y < 0 || y >= b->mxlines || n < 0)
  return(E_BUFFER_RANGE);
 return(e_buffer_set_line_ended(b, y, text, n, b->bf[y].s[b->bf[y].len]));
}

/* ---- covers and steps ---- */

/**
 * e_buffer_cover_begin - Until cover_end, splices on b record nothing.
 * @pocket: the caller's own record, which keeps the breakpoint rows a
 * covered deletion removes so its Undo brings them back; NULL for none.
 *
 * Nests; the outermost cover's pocket wins.
 */
void e_buffer_cover_begin(BUFFER *b, Undo *pocket)
{
 if (b->covered == 0)
  b->pocket = pocket;
 b->covered++;
}

/**
 * e_buffer_cover_end - Close a cover; at depth 0 the colours are recomputed
 * once from the lowest line touched.
 */
void e_buffer_cover_end(BUFFER *b)
{
 if (b->covered > 0)
  b->covered--;
 if (b->covered == 0)
  b->pocket = NULL;
 buffer_syntax_flush(b);
}

/**
 * e_buffer_step_begin - Every record pushed until step_end is one Ctrl-U.
 *
 * Enter with auto-indent, a tool's rewrite with its hook, and every Undo or
 * Redo of such a step are one action for the user.  Nests.
 */
void e_buffer_step_begin(BUFFER *b)
{
 if (b->step_depth == 0)
  b->step_pushed = 0;
 b->step_depth++;
}

/**
 * e_buffer_step_end - Close a step.
 */
void e_buffer_step_end(BUFFER *b)
{
 if (b->step_depth > 0)
  b->step_depth--;
 buffer_syntax_flush(b);
}

/**
 * e_buffer_depth_check - Before each key: no cover or step may be left open.
 *
 * A cover left open by a path that returned early would silently drop the
 * undo of every later key.  The key loop calls this; a leak is written to
 * the trace and closed.  Return: 1 when one was found.
 */
int e_buffer_depth_check(BUFFER *b)
{
 if (b == NULL || (b->covered == 0 && b->step_depth == 0))
  return(0);
 wpe_buffer_trace("buffer depth leak covered=%d step=%d", b->covered, b->step_depth);
 b->covered = 0;
 b->step_depth = 0;
 b->pocket = NULL;
 buffer_syntax_flush(b);
 return(1);
}

/* ---- Undo and Redo ---- */

/* How many breakpoint rows the margin holds now: none without one. */
static int buffer_brp_count(BUFFER *b)
{
 int *brp = buffer_brp(b);

 if (brp == NULL)
  return(0);
 return(brp[0]);
}

/**
 * buffer_pocket_prepare - Make room for the breakpoints a record parked,
 * before the record is replayed.
 *
 * An Undo that brings back lines a covered deletion removed also brings
 * back their breakpoints.  The room is asked for first, so that when memory
 * is short the Undo is refused whole, the text, the breakpoints and the
 * record as they were, instead of giving back the lines without them.
 * Return: E_BUFFER_OK or E_BUFFER_NOMEM.
 */
static int buffer_pocket_prepare(BUFFER *b, Undo *rec)
{
 if (rec->type == E_UNDO_LINES || rec->nbrk == 0)
  return(E_BUFFER_OK);
 return(buffer_brp_reserve(b, rec->nbrk));
}

/* Put the rows a covered deletion parked in a char record's pocket back on
   their lines, after the record's replay rebuilt them.  @rows_before is how
   many the margin held when buffer_pocket_prepare made the room: the room
   stands unless the replay itself added rows to the margin, and then it is
   asked for again; a refusal then is said on the status row, never silent. */
static void buffer_pocket_restore(BUFFER *b, Undo *rec, int rows_before)
{
 int *brp, i, row;

 if (rec->type == E_UNDO_LINES || rec->nbrk == 0)
  return;
 if (buffer_brp_count(b) > rows_before && buffer_brp_reserve(b, rec->nbrk) != E_BUFFER_OK)
 {
  wpe_buffer_say_nomem(b, "the breakpoints of Undo");
  return;
 }
 if ((brp = buffer_brp(b)) == NULL)
  return;
 for (i = 0; i < rec->nbrk; i++)
 {
  row = rec->b.y + rec->brk[i];
  if (row < 0 || row >= b->mxlines || brp_has(brp, row))
   continue;
  brp[++brp[0]] = row;
  wpe_buffer_lines_moved(b, row, 0, 0, NULL, 0, &row, 1);
 }
}

/* Replay a line record: the lines it saved come back in place of what
   stands there now, with their breakpoints; the cursor and the block go back
   to where they were before the change. */
static int buffer_replay_lines(BUFFER *b, Undo *rec)
{
 int r;

 r = buffer_splice(b, rec->span, rec->n_new, rec->old, rec->n_old, rec->brk, rec->nbrk,
                   E_BUFFER_STEP_REPLAY, (rec->flags & E_UNDO_EMPTIED) != 0,
                   NULL, NULL, NULL);
 if (r != E_BUFFER_OK)
  return(r);
 free(rec->old);
 rec->old = NULL;
 rec->n_old = 0;
 buffer_replay_view_back(b, rec);
 return(E_BUFFER_OK);
}

/* Say on the status row that an Undo or a Redo was refused. */
static void buffer_replay_refused(BUFFER *b, int replaying)
{
 if (replaying == E_BUFFER_REPLAY_UNDO)
  wpe_buffer_say_nomem(b, "Undo");
 else
  wpe_buffer_say_nomem(b, "Redo");
}

/* The test fault point of an Undo or a Redo. */
static int buffer_replay_fault_point(int replaying)
{
 if (replaying == E_BUFFER_REPLAY_UNDO)
  return(E_BUFFER_FAULT_UNDO);
 return(E_BUFFER_FAULT_REDO);
}

/* Does a replay of this kind put text back where its record names ('r' the
   characters a deletion took, 's' the other half of a replacement)? */
static int buffer_record_puts_text_back(int kind)
{
 return(kind == 'r' || kind == 's');
}

/**
 * buffer_replay_row - Make the row a character record names ready for its
 * replay.
 * @b: the buffer; @rec: the record about to be replayed.
 *
 * Text that goes back to the row a deletion of the last row took gets that
 * row again (e_buffer_open_row), so Undo and Redo of such a deletion bring
 * the line back.  A record that takes text away ('a') or writes over it
 * ('p') always names a row of the text; one that does not is refused and
 * traced, never replayed into memory the text no longer has.  Line and
 * block records keep their own lines and check their own ranges.
 * Return: E_BUFFER_OK, E_BUFFER_NOMEM or E_BUFFER_RANGE (nothing changed).
 */
static int buffer_replay_row(BUFFER *b, const Undo *rec)
{
 if (!buffer_record_is_char(rec->type))
  return(E_BUFFER_OK);
 if (buffer_record_puts_text_back(rec->type))
  return(e_buffer_open_row(b, rec->b.y));
 if (rec->b.y >= 0 && rec->b.y < b->mxlines)
  return(E_BUFFER_OK);
 wpe_buffer_trace("buffer replay %c names a row past the text y=%d lines=%d",
                  rec->type, rec->b.y, b->mxlines);
 return(E_BUFFER_RANGE);
}

/* A mark past the end of row @p->y: just after a line break is the start
   of the next row, so a block that holds the break ends there; past the
   last row of the text, or past a row without a break, the row's end. */
static void buffer_mark_past_row_end(BUFFER *b, POINT *p)
{
 if (b->bf[p->y].s[b->bf[p->y].len] == WPE_WR && p->y < b->mxlines - 1)
 {
  p->y++;
  p->x = 0;
  return;
 }
 p->x = b->bf[p->y].len;
}

/* Pull one mark that is set back inside the text: a row past the end goes
   to the end of the last row, a column past its row as above.  A mark is
   never past its row's end, so the block commands read every block the
   same way. */
static void buffer_mark_pull_inside(BUFFER *b, POINT *p)
{
 if (p->y < 0)
  return;
 if (p->y >= b->mxlines)
 {
  p->y = b->mxlines - 1;
  p->x = b->bf[p->y].len;
 }
 if (p->x > b->bf[p->y].len)
  buffer_mark_past_row_end(b, p);
 if (p->x < 0)
  p->x = 0;
}

/**
 * buffer_marks_inside - Every mark set stands in the text after an Undo or
 * a Redo.
 * @b: the buffer just replayed.
 *
 * A replay sets the block from its record.  When the text it rebuilt is
 * shorter than the record remembers, a mark past the end would make the
 * next block command (Ctrl-K V, Ctrl-K Y) work on rows the text no longer
 * has; it is pulled back to the end of the text instead.
 */
static void buffer_marks_inside(BUFFER *b)
{
 POINT *m[BUFFER_MARKS_MAX];
 int i, n = buffer_marks(b, m);

 for (i = 0; i < n; i++)
  buffer_mark_pull_inside(b, m[i]);
}

/**
 * e_buffer_marks_inside - Every mark of the window, and the anchor a Shift
 * key selects from, stands inside the text, never past the end of its row.
 * @b: the buffer of the window the next key goes to.
 *
 * Called before each key is read.  A key that changed the text in place
 * (Backspace, a typed character in overwrite mode) does not move the anchor
 * the previous key left; a Shift key would then make a block that ends past
 * a line break, and Ctrl-O H, U or M would write a byte behind the break,
 * which Save loses.  Just after a line break is the start of the next row.
 */
void e_buffer_marks_inside(BUFFER *b)
{
 buffer_marks_inside(b);
 if (b->f != NULL && b->f->s != NULL)
  buffer_mark_pull_inside(b, &b->f->s->ks);
}

/* One step back (Undo) or forward (Redo).  A record is taken off its list
   only after its replay succeeded: a replay refused for want of memory
   leaves it, the user's lines and the cursor where they were. */
static int buffer_replay(BUFFER *b, SCHIRM *s, Undo **list, int replaying)
{
 Undo *rec;
 POINT cursor;
 int more, r = E_BUFFER_OK, fault, rows_before;

 if (*list == NULL)
  return(E_BUFFER_NOTHING);
 fault = e_buffer_test_fault_enter(buffer_replay_fault_point(replaying));
 b->replaying = replaying;
 e_buffer_step_begin(b);
 do
 {
  rec = *list;
  if (buffer_pocket_prepare(b, rec) != E_BUFFER_OK)
  {
   r = E_BUFFER_NOMEM;
   break;
  }
  rows_before = buffer_brp_count(b);
  cursor = b->b;
  r = buffer_replay_row(b, rec);
  if (r != E_BUFFER_OK)
   break;
  b->b = rec->b;
  if (rec->type == E_UNDO_LINES)
   r = buffer_replay_lines(b, rec);
  else
   r = wpe_buffer_replay_kind(b, s, rec);
  if (r != E_BUFFER_OK)
  {
   b->b = cursor;
   break;
  }
  *list = rec->next;
  buffer_pocket_restore(b, rec, rows_before);
  more = (rec->flags & E_UNDO_MORE) != 0;
  e_buffer_record_free(rec);
 } while (more && *list != NULL);
 buffer_marks_inside(b);
 e_buffer_step_end(b);
 b->replaying = E_BUFFER_REPLAY_NONE;
 e_buffer_test_fault_leave(fault);
 if (r == E_BUFFER_NOMEM)
  buffer_replay_refused(b, replaying);
 return(r);
}

/**
 * e_buffer_undo - Undo the user's last action: one step, however many
 * records it took.
 * Return: E_BUFFER_OK, E_BUFFER_NOTHING (nothing to undo) or E_BUFFER_NOMEM.
 */
int e_buffer_undo(BUFFER *b, SCHIRM *s)
{
 return(buffer_replay(b, s, &b->ud, E_BUFFER_REPLAY_UNDO));
}

/**
 * e_buffer_redo - Redo the last undone step.
 * Return: as e_buffer_undo.
 */
int e_buffer_redo(BUFFER *b, SCHIRM *s)
{
 return(buffer_replay(b, s, &b->rd, E_BUFFER_REPLAY_REDO));
}

/**
 * e_buffer_history_drop - Forget both lists: a file just read has nothing
 * to undo, and a closed buffer frees what it kept.
 */
void e_buffer_history_drop(BUFFER *b)
{
 buffer_list_free(b->ud);
 buffer_list_free(b->rd);
 b->ud = NULL;
 b->rd = NULL;
}

/**
 * e_buffer_mark_view - The buffer is a view the editor writes.
 *
 * Messages, Help, the clipboard and the like: no undo, no modified mark, no
 * breakpoint rows; the user's marks are still kept so a copy from them works.
 * Its rows may be wider than Max. Columns, so a compiler's line stays one
 * row.  The rows Messages grows for a running program's output have no room
 * past their text: the editor runs a character primitive on a view only on
 * rows the file reader or e_buffer_line_new made (a manual page's buttons,
 * the "No Messages" row), and the key loop types into no view.
 */
void e_buffer_mark_view(BUFFER *b)
{
 b->view = 1;
 b->history = E_BUFFER_NO_HISTORY;
 e_buffer_history_drop(b);
}

/**
 * e_buffer_init - Make a fresh buffer: one empty line, nothing to undo.
 * @cn: the editor; @f: the window it belongs to (NULL for none); @mx_x: its
 * width; @history: E_BUFFER_HISTORY_FULL or E_BUFFER_NO_HISTORY.
 *
 * Every buffer starts the same way, so no field holds what an earlier use of
 * the memory left there.  The window's breakpoint rows start empty when it
 * has none yet, and its modified mark at zero.  The caller checks that the
 * buffer has its line (b->mxlines == 1) before using it.
 */
void e_buffer_init(BUFFER *b, struct CNT *cn, struct FNST *f, int mx_x, int history)
{
 memset(b, 0, sizeof(BUFFER));
 b->cn = cn;
 b->f = f;
 b->mx.x = mx_x;
 b->mx.y = MAXLINES;
 b->history = history;
 b->syn_lo = -1;
 b->syn_hi = -1;
 if (f != NULL)
 {
  f->save = 0;
#ifdef DEBUGGER
  if (f->s != NULL && f->s->brp == NULL && (f->s->brp = buffer_malloc(sizeof(int))) != NULL)
   f->s->brp[0] = 0;
#endif
 }
 if ((b->bf = buffer_malloc(MAXLINES * sizeof(STRING))) == NULL)
  return;
 if (e_buffer_line_new(b, &b->bf[0]) == E_BUFFER_OK)
  b->mxlines = 1;
}
