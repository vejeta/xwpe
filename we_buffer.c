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

/* The cover the block code still spells as a global (see we_buffer.h). */
int e_undo_sw = 0;

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
 return(b->covered > 0 || e_undo_sw > 0);
}

/* The block code still adds its own lines by hand under e_undo_sw and then
   moves the breakpoints by the size of the block (e_brk_recalc).  A line an
   insertion adds meanwhile (the wrap of a pasted line) is left to it as
   well, as it always was: counting it here too would move the breakpoints
   below twice, and the paste's Undo would leave them a line off. */
static int buffer_rows_left_to_block_code(int n_del, int n_ins)
{
 return(e_undo_sw > 0 && n_del == 0 && n_ins > 0);
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

static int *buffer_brp(BUFFER *b)
{
#ifdef DEBUGGER
 if (buffer_keeps_history(b) && b->f->s != NULL)
  return(b->f->s->brp);
#endif
 return(NULL);
}

/* ---- lines ---- */

/**
 * e_buffer_line_new - Make one empty line the buffer can hold.
 * @b: the buffer (its width); @out: receives the line.
 *
 * Every line has room for the buffer's width, so typing into it never
 * outruns its memory.  Return: E_BUFFER_OK or E_BUFFER_NOMEM.
 */
int e_buffer_line_new(BUFFER *b, STRING *out)
{
 out->s = buffer_malloc((size_t)b->mx.x + 1);
 if (out->s == NULL)
  return(E_BUFFER_NOMEM);
 out->s[0] = '\0';
 out->len = 0;
 out->nrc = 0;
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

static int buffer_lines_equal(STRING *a, STRING *b)
{
 return(a->len == b->len && a->s[a->len] == b->s[b->len] &&
        memcmp(a->s, b->s, (size_t)a->len) == 0);
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
 * e_buffer_shell_free - Free the private buffer a block delete keeps its
 * lines in.
 * @shell: the buffer, with its window and screen.
 */
void e_buffer_shell_free(BUFFER *shell)
{
 int i;

 if (shell == NULL)
  return;
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
 wpe_buffer_trace("buffer record kind=%c more=%d", rec->type,
                  (rec->flags & E_UNDO_MORE) ? 1 : 0);
 return(rec);
}

/* ---- the splice ---- */

/* What one splice needs from memory, taken before anything moves. */
typedef struct {
 STRING canon;        /* the canonical empty line of a buffer emptied */
 int need_canon;
 Undo *rec;           /* the line record, when this splice records */
 int *removed;        /* rows of removed lines, absolute, before */
 int *added;          /* rows set from brk[], absolute, after */
 int nremoved, nadded;
} BufferSpliceMem;

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

/* Take every allocation the splice needs; nothing moves yet. */
static int buffer_splice_prepare(BUFFER *b, int y, int n_del, int n_ins, int nbrk,
                                 int records, int detach, BufferSpliceMem *m)
{
 int *brp = buffer_brp(b);
 int rows_out = brp_count_in(brp, y, y + n_del);
 int n_total = n_ins;

 memset(m, 0, sizeof(*m));
 m->need_canon = (b->mxlines - n_del + n_ins == 0);
 if (m->need_canon)
  n_total = 1;
 if (buffer_grow(b, b->mxlines - n_del + n_total) != E_BUFFER_OK)
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
     (nbrk > 0 && brp != NULL && m->added == NULL) ||
     buffer_brp_reserve(b, nbrk) != E_BUFFER_OK ||
     buffer_pocket_reserve(buffer_splice_pocket(b, records), rows_out) != E_BUFFER_OK)
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

static const char *buffer_step_name(WpeBufferStep step)
{
 if (step == E_BUFFER_STEP_NEW)
  return("new");
 if (step == E_BUFFER_STEP_COVERED)
  return("covered");
 return("replay");
}

/* The splice itself.  no_changed: no row of the span is a changed row (a
   buffer emptied, or the replay of such a record).  detach: the removed
   lines go to out[] with their rows instead of being freed or recorded. */
static int buffer_splice(BUFFER *b, int y, int n_del, STRING *lines, int n_ins,
                         const int *brk, int nbrk, WpeBufferStep step,
                         int no_changed, STRING *out, int *brk_out, int *nbrk_out)
{
 BufferSpliceMem m;
 int records, changed, old_mxlines, i, delta, rows_move;
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
 records = buffer_splice_records(b, step);
 pocket = buffer_splice_pocket(b, records);
 if (buffer_splice_prepare(b, y, n_del, n_ins, nbrk, records, out != NULL, &m) != E_BUFFER_OK)
  return(E_BUFFER_NOMEM);
 if (m.need_canon)
 {
  lines = &m.canon;
  n_ins = 1;
  no_changed = 1;
 }
 changed = no_changed ? 0 : (n_del < n_ins ? n_del : n_ins);
 if (m.rec != NULL)
 {
  m.rec->b = b->b;
  m.rec->span = y;
  m.rec->n_old = n_del;
  m.rec->n_new = n_ins;
  if (no_changed)
   m.rec->flags |= E_UNDO_EMPTIED;
  if (b->f != NULL && b->f->s != NULL)
  {
   m.rec->a = b->f->s->mark_begin;
   m.rec->e = b->f->s->mark_end;
  }
 }
 if (out != NULL && nbrk_out != NULL)
  *nbrk_out = 0;
 rows_move = !buffer_rows_left_to_block_code(n_del, n_ins);
 if (rows_move)
  buffer_brp_move(b, y, n_del, n_ins, changed, brk, nbrk, &m, pocket, brk_out, nbrk_out);
 for (i = 0; i < n_del; i++)
 {
  if (m.rec != NULL)
   m.rec->old[i] = b->bf[y+i];
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
 m.canon.s = NULL;              /* adopted, when it was made */
 b->mxlines = old_mxlines + delta;
 buffer_syntax_move(b, y, n_del, n_ins, old_mxlines);
 buffer_marks_move(b, y, n_del, n_ins, changed);
 if (buffer_keeps_history(b))
  b->f->save += E_BUFFER_SAVE_PER_LINE * (n_del + n_ins);
 if (buffer_keeps_history(b) && rows_move)
  wpe_buffer_lines_moved(b, y, n_del, n_ins, m.removed, m.nremoved, m.added, m.nadded);
 if (m.rec != NULL)
 {
  buffer_record_link(b, m.rec);
  wpe_buffer_trace("buffer record kind=R n_old=%d n_new=%d more=%d", m.rec->n_old,
                   m.rec->n_new, (m.rec->flags & E_UNDO_MORE) ? 1 : 0);
  m.rec = NULL;
 }
 wpe_buffer_trace("buffer splice y=%d del=%d ins=%d step=%s covered=%d", y, n_del,
                  n_ins, buffer_step_name(step), b->covered);
 buffer_splice_mem_free(&m);
 buffer_syntax_flush(b);
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
 * owner adds an empty one.  Return: E_BUFFER_OK, E_BUFFER_NOMEM (nothing
 * changed, the lines stay the caller's) or E_BUFFER_RANGE.
 */
int e_buffer_splice(BUFFER *b, int y, int n_del, STRING *lines, int n_ins,
                    const int *brk, int nbrk, WpeBufferStep step)
{
 return(buffer_splice(b, y, n_del, lines, n_ins, brk, nbrk, step, 0, NULL, NULL, NULL));
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

/* A copy of `n` bytes of `src` as a line of the buffer, with `term` as the
   byte after them (WPE_WR or '\0'). */
static int buffer_line_from_bytes(BUFFER *b, const unsigned char *src, int n, int term,
                                  STRING *out)
{
 if (e_buffer_line_new(b, out) != E_BUFFER_OK)
  return(E_BUFFER_NOMEM);
 if (n > b->mx.x - 1)
  n = b->mx.x - 1;
 if (n < 0)
  n = 0;
 memcpy(out->s, src, (size_t)n);
 out->s[n] = (unsigned char) term;
 out->s[n + (term ? 1 : 0)] = '\0';
 buffer_line_measure(out);
 return(E_BUFFER_OK);
}

/**
 * e_buffer_split_line - Break line y at column x: its tail becomes line y+1.
 * @step: how it counts; @head_term: E_BUFFER_HEAD_WR ends the head with a
 * line break (Enter), E_BUFFER_HEAD_OPEN leaves it open (a block inserts
 * the rest next).
 *
 * The line keeps its breakpoint; marks behind the column move with the tail.
 * At column 0 the empty line goes above and the whole line, its breakpoint
 * and its marks move down one.  Return: as e_buffer_splice.
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
 if (tail_n > b->mx.x)
  tail_n = b->mx.x;
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

/* Append one logical line to lines[*n], soft-wrapped as the file reader
   wraps it: every piece but the last ends without a line break. */
static int buffer_lines_push(STRING **lines, int *n, int *cap, int mx_x,
                             const char *str, int len)
{
 int maxc = mx_x - 1, start = 0;

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
  if ((l->s = buffer_malloc((size_t)mx_x + 1)) == NULL)
   return(E_BUFFER_NOMEM);
  memcpy(l->s, str + start, (size_t)seg);
  l->s[seg] = hard ? WPE_WR : '\0';
  l->s[seg + 1] = '\0';
  buffer_line_measure(l);
  (*n)++;
  start += seg;
 } while (start < len);
 return(E_BUFFER_OK);
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
  if (buffer_lines_push(&lines, n, &cap, mx_x, p, nl ? (int)(nl - p) : (int)strlen(p))
      != E_BUFFER_OK)
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
 * e_buffer_replace_all - Make the buffer hold `text`, changing only what differs.
 * @step: how the change counts for Ctrl-U.
 *
 * A tool's rewrite of the file (an accepted Edit, a rename, a format) leaves
 * the lines it did not touch where they were: their breakpoints, marks and
 * colours stay, and Ctrl-U gives back the old lines.  The common leading and
 * trailing lines are kept; the middle is one splice.  Return: as
 * e_buffer_splice; identical text changes nothing and records nothing.
 */
int e_buffer_replace_all(BUFFER *b, const char *text, WpeBufferStep step)
{
 STRING *lines;
 int n, head = 0, tail = 0, r, i;

 lines = e_buffer_lines_from_text(text, b->mx.x, &n);
 if (n < 0)
  return(E_BUFFER_NOMEM);
 if (n == 0)
 {
  if (b->mxlines == 1 && b->bf[0].len == 0)
   return(E_BUFFER_OK);         /* empty text over the one empty line */
  return(e_buffer_splice(b, 0, b->mxlines, NULL, 0, NULL, 0, step));
 }
 while (head < n && head < b->mxlines && buffer_lines_equal(&b->bf[head], &lines[head]))
  head++;
 while (tail < n - head && tail < b->mxlines - head &&
        buffer_lines_equal(&b->bf[b->mxlines - 1 - tail], &lines[n - 1 - tail]))
  tail++;
 for (i = 0; i < head; i++)
  free(lines[i].s);
 for (i = n - tail; i < n; i++)
  free(lines[i].s);
 r = e_buffer_splice(b, head, b->mxlines - head - tail, lines + head,
                     n - head - tail > 0 ? n - head - tail : 0, NULL, 0, step);
 if (r != E_BUFFER_OK)
  for (i = head; i < n - tail; i++)
   free(lines[i].s);
 free(lines);
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
 r = e_buffer_splice(b, b->mxlines, 0, lines, k, NULL, 0, E_BUFFER_STEP_NEW);
 if (r != E_BUFFER_OK)
  for (i = 0; i < k; i++)
   free(lines[i].s);
 free(lines);
 return(r);
}

/**
 * e_buffer_clear - Leave the buffer with one empty line.
 * Return: as e_buffer_splice.
 */
int e_buffer_clear(BUFFER *b)
{
 return(e_buffer_splice(b, 0, b->mxlines, NULL, 0, NULL, 0, E_BUFFER_STEP_NEW));
}

/**
 * e_buffer_set_line - Replace the bytes of row y, keeping its line break.
 * @text: the new bytes; @n: how many (clamped to the buffer's width).
 *
 * A changed row: its breakpoint and the marks on it stay.  Recorded unless
 * covered or on a view.  Return: as e_buffer_splice.
 */
int e_buffer_set_line(BUFFER *b, int y, const char *text, int n)
{
 STRING line;
 int r;

 if (y < 0 || y >= b->mxlines || n < 0)
  return(E_BUFFER_RANGE);
 if (buffer_line_from_bytes(b, (const unsigned char *) text, n,
                            b->bf[y].s[b->bf[y].len], &line) != E_BUFFER_OK)
  return(E_BUFFER_NOMEM);
 r = e_buffer_splice(b, y, 1, &line, 1, NULL, 0, E_BUFFER_STEP_NEW);
 if (r != E_BUFFER_OK)
  free(line.s);
 return(r);
}

/**
 * e_buffer_insert_raw - Put an empty line at yd, and nothing else.
 *
 * What e_new_line does for the file reader, the views the editor writes
 * and the block code until they splice: no mark, breakpoint, colour or undo
 * moves, exactly as before.  Return: E_BUFFER_OK, or E_BUFFER_NOMEM with
 * nothing changed (the caller decides what a refusal means).
 */
int e_buffer_insert_raw(BUFFER *b, int yd)
{
 STRING line;

 if (buffer_grow(b, b->mxlines + 1) != E_BUFFER_OK ||
     e_buffer_line_new(b, &line) != E_BUFFER_OK)
  return(E_BUFFER_NOMEM);
 if (b->mxlines > yd)
  memmove(b->bf + yd + 1, b->bf + yd, (size_t)(b->mxlines - yd) * sizeof(STRING));
 b->bf[yd] = line;
 b->mxlines++;
 return(E_BUFFER_OK);
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

/* Put the rows a covered deletion parked in a char record's pocket back on
   their lines, after the record's replay rebuilt them. */
static void buffer_pocket_restore(BUFFER *b, Undo *rec)
{
 int *brp, i, row;

 if (rec->type == E_UNDO_LINES || rec->nbrk == 0)
  return;
 if (buffer_brp_reserve(b, rec->nbrk) != E_BUFFER_OK || (brp = buffer_brp(b)) == NULL)
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
 b->b = rec->b;
 buffer_point_clamp(b, &b->b);
 if (b->f != NULL && b->f->s != NULL)
 {
  b->f->s->mark_begin = rec->a;
  b->f->s->mark_end = rec->e;
  buffer_point_clamp(b, &b->f->s->mark_begin);
  buffer_point_clamp(b, &b->f->s->mark_end);
 }
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

/* One step back (Undo) or forward (Redo).  A record is taken off its list
   only after its replay succeeded: a replay refused for want of memory
   leaves it, the user's lines and the cursor where they were. */
static int buffer_replay(BUFFER *b, SCHIRM *s, Undo **list, int replaying)
{
 Undo *rec;
 POINT cursor;
 int more, r = E_BUFFER_OK;

 if (*list == NULL)
  return(E_BUFFER_NOTHING);
 b->replaying = replaying;
 e_buffer_step_begin(b);
 do
 {
  rec = *list;
  cursor = b->b;
  b->b = rec->b;
  if (rec->type == E_UNDO_LINES)
   r = buffer_replay_lines(b, rec);
  else
   r = wpe_buffer_replay_kind(b, s, rec);
  if (r != E_BUFFER_OK)
  {
   b->b = cursor;
   buffer_replay_refused(b, replaying);
   break;
  }
  *list = rec->next;
  buffer_pocket_restore(b, rec);
  more = (rec->flags & E_UNDO_MORE) != 0;
  e_buffer_record_free(rec);
 } while (more && *list != NULL);
 e_buffer_step_end(b);
 b->replaying = E_BUFFER_REPLAY_NONE;
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
 */
void e_buffer_mark_view(BUFFER *b)
{
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
