/* we_block.c                                             */
/* Copyright (C) 1993 Fred Kruse                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

#include "messages.h"
#include "edit.h"
#include "we_clip.h"
#include "we_buffer.h"
#include <ctype.h>

/* The marked block of a window: from (kax, kay) up to, not including,
   (kex, key). */
typedef struct
{
 int kax, kay, kex, key;
} WpeBlock;

/* The two partial lines of a block, taken as bytes before anything moves:
   the first line from the block's start on, its line break included, and
   the last line before the block's end. */
typedef struct
{
 unsigned char *first;
 int nfirst;
 unsigned char *last;
 int nlast;
} WpeBlockEnds;

/* Whole lines a block move took out of their buffer, with the breakpoint
   rows they carried (relative to the first of them), still in hand until
   they land. */
typedef struct
{
 STRING *lines;
 int *rows;
 int n, nrows;
 int at;                /* the row they were taken from */
 int first_in_hand;     /* lines[0] is merged into a line, not landed whole */
} WpeBlockLines;

/**
 * e_blk_of - The block a window has marked.
 * @s: the window's screen; @k: receives the block.
 */
static void e_blk_of(SCHIRM *s, WpeBlock *k)
{
 k->kax = s->mark_begin.x;
 k->kay = s->mark_begin.y;
 k->kex = s->mark_end.x;
 k->key = s->mark_end.y;
}

/**
 * e_blk_is_empty - Does the block hold no character?
 * Return: 1 when its end is not after its start.
 */
static int e_blk_is_empty(const WpeBlock *k)
{
 return(k->key < k->kay || (k->kay == k->key && k->kex <= k->kax));
}

/**
 * e_blk_cover_begin - Leave the steps of a block operation out of Undo.
 * @bv: the buffer the block comes from; @bz: the buffer it goes to.
 *
 * The operation's own record, pushed after e_blk_cover_end, replays the
 * whole of it, so one Ctrl-U takes the operation back; the lines, block
 * marks, breakpoints and colours of both buffers still move with every step.
 */
static void e_blk_cover_begin(BUFFER *bv, BUFFER *bz)
{
 e_buffer_cover_begin(bv, NULL);
 if (bz != bv)
  e_buffer_cover_begin(bz, NULL);
}

/**
 * e_blk_cover_end - Close what e_blk_cover_begin opened, in reverse order.
 */
static void e_blk_cover_end(BUFFER *bv, BUFFER *bz)
{
 if (bz != bv)
  e_buffer_cover_end(bz);
 e_buffer_cover_end(bv);
}

/**
 * e_block_record_push - Keep a block copy ('c') or move ('v') for Ctrl-U.
 * @kind: 'c' or 'v'; @b: the buffer; @x, @y: the cursor the record replays
 * from.
 *
 * The record keeps the block as the window marks it now, so Undo finds the
 * copied or moved lines where they landed.  A refused record is said on the
 * status row; the operation itself stays done.
 */
void e_block_record_push(int kind, BUFFER *b, int x, int y)
{
 Undo *rec = e_buffer_record_new(kind);

 if (rec == NULL)
 {
  wpe_buffer_say_nomem(b, "the undo record");
  return;
 }
 rec->b.x = x;
 rec->b.y = y;
 rec->a = b->f->s->mark_begin;
 rec->e = b->f->s->mark_end;
 e_buffer_record_push(b, rec);
}

/**
 * e_blk_shell_new - The private buffer a block delete moves its lines into.
 * @b: the buffer the block leaves.
 *
 * The shell keeps breakpoint rows of its own, so the breakpoints of the
 * deleted lines wait in it until Undo brings the lines back; it has no file
 * name, so no debugger is ever told of it, and every splice into it runs
 * covered, so it records nothing.
 * Return: the shell, or NULL when memory ran out.
 */
static BUFFER *e_blk_shell_new(BUFFER *b)
{
 BUFFER *bn = MALLOC(sizeof(BUFFER));
 SCHIRM *sn = MALLOC(sizeof(SCHIRM));
 FENSTER *fn = MALLOC(sizeof(FENSTER));

 if (bn == NULL || sn == NULL || fn == NULL)
 {
  FREE(bn);  FREE(sn);  FREE(fn);
  return(NULL);
 }
 memset(sn, 0, sizeof(SCHIRM));
 memset(fn, 0, sizeof(FENSTER));
 fn->s = sn;
 fn->b = bn;
 fn->ed = NULL;
 e_buffer_init(bn, b->cn, fn, b->cn->maxcol, E_BUFFER_HISTORY_FULL);
 if (bn->mxlines != 1)
 {
  e_buffer_shell_free(bn);
  return(NULL);
 }
 sn->fb = bn->fb = b->fb;
 e_line_make_hard_empty(&bn->bf[0]);
 return(bn);
}

/**
 * e_blk_delete_into_shell_now - Delete the marked block of window @f by moving
 * its lines into a shell its undo record keeps.
 *
 * One Ctrl-U moves the lines back, with their breakpoints, and marks them
 * as the block again.  Return: 0, or -1 when nothing was deleted (memory
 * ran out, or the block could not move).
 */
static int e_blk_delete_into_shell_now(FENSTER *f)
{
 BUFFER *b = f->b;
 BUFFER *bn;
 Undo *rec = e_buffer_record_new('d');

 if (rec == NULL)
  return(-1);
 if ((bn = e_blk_shell_new(b)) == NULL)
 {
  e_buffer_record_free(rec);
  return(-1);
 }
 rec->b = f->s->mark_begin;
 rec->u.pt = bn;
 if (e_move_block_covered(0, 0, b, bn, f) != E_BLOCK_MOVED)
 {
  e_buffer_record_free(rec);
  return(-1);
 }
 e_buffer_record_push(b, rec);
 return(0);
}

/**
 * e_blk_delete_into_shell - A block delete, as one action a test can starve
 * of memory: its record, its shell and its move all count.
 * Return: as e_blk_delete_into_shell_now.
 */
static int e_blk_delete_into_shell(FENSTER *f)
{
 int r, fault;

 fault = e_buffer_test_fault_enter(E_BUFFER_FAULT_BLOCK_MOVE);
 r = e_blk_delete_into_shell_now(f);
 e_buffer_test_fault_leave(fault);
 return(r);
}

/*	delete block */
int e_blck_del(FENSTER *f)
{
 BUFFER *b;
 SCHIRM *s;
 int i;

 for (i = f->ed->mxedt; i > 0 && !DTMD_ISTEXT(f->ed->f[i]->dtmd); i--)
  ;
 if (i <= 0)
  return(0);
 e_switch_window(f->ed->edt[i], f);
 f = f->ed->f[f->ed->mxedt];
 b = f->b;  s = f->s;
 if ((s->mark_end.y < s->mark_begin.y) ||
   ((s->mark_begin.y == s->mark_end.y) && (s->mark_end.x <= s->mark_begin.x)))
 {
  return(0);
 }
 if (e_window_text_locked(f))
  return(WPE_ESC);
 if (s->mark_begin.y == s->mark_end.y)
 {
  e_del_nchar(b, s, s->mark_begin.x, s->mark_begin.y,
    s->mark_end.x - s->mark_begin.x);
  b->b.x = s->mark_end.x = s->mark_begin.x;
 }
 else
 {
  if (e_blk_delete_into_shell(f) != 0)
   wpe_buffer_say_nomem(b, "the block deletion");
  f->save = b->cn->maxchg + 1;
 }
 sc_txt_1(f);
 e_cursor(f, 1);
 e_schirm(f, 1);
 return(0);
}

/*      dup selected block BD*/
int e_blck_dup(char *dup, FENSTER *f)
{
 BUFFER *b;
 SCHIRM *s;
 int i;

 for (i = f->ed->mxedt; i > 0 && !DTMD_ISTEXT(f->ed->f[i]->dtmd); i--)
  ;
 if (i <= 0)
  return(0);
 e_switch_window(f->ed->edt[i], f);
 f = f->ed->f[f->ed->mxedt];
 if (e_window_text_locked(f))
  return(0);
 b = f->b;  s = f->s;
 i=s->mark_end.x - s->mark_begin.x;
 /* Brian thinks that 80 characters is enough.  For the time being return
    nothing if longer than that. */
 if ((s->mark_end.y != s->mark_begin.y) || (i <= 0) || (i >= 80))
 {
  return(0);
 }
 strncpy(dup, &b->bf[s->mark_begin.y].s[s->mark_begin.x], i);
 dup[i]=0;
 return(i);
}

/*   write buffer to screen */
int e_show_clipboard(FENSTER *f)
{
 ECNT *cn = f->ed;
 FENSTER *fo;
 int i, j;

 for (j = 1; j <= cn->mxedt; j++)
  if (cn->f[j] == cn->f[0])
  {
   e_switch_window(cn->edt[j], f);
   return(0);
  }

 if (cn->mxedt > MAXEDT)
 {
  e_error(e_msg[ERR_MAXWINS], 0, cn->fb);
  return(0);
 }
 for (j = 1; j <= MAXEDT; j++)
 {
  for (i = 1; i <= cn->mxedt && cn->edt[i] != j; i++)
   ;
  if ( i > cn->mxedt) break;
 }
 cn->curedt=j;
 (cn->mxedt)++;
 cn->edt[cn->mxedt]=j;

 f = cn->f[cn->mxedt] = cn->f[0];
 f->ins = 8;   /* the Show Buffer is a read-only viewer: no typing/paste/copy,
                  and it wears the read-only padlock (see e_win_kind_of) */
#ifdef PROG
 if (WpeIsProg())
 {
  for (i = cn->mxedt-1; i > 0; i--)
   ;
  if (i < 1)
  {
   f->a = e_set_pnt(0, 1);
   f->e = e_set_pnt(MAXSCOL-1, 2*MAXSLNS/3);
  }
  else
  {
   f->a = e_set_pnt(cn->f[i]->a.x+1, cn->f[i]->a.y+1);
   f->e = e_set_pnt(cn->f[i]->e.x, cn->f[i]->e.y);
  }
 }
 else
#endif
 {
  if (cn->mxedt < 2)
  {
   f->a = e_set_pnt(0, 1);
   f->e = e_set_pnt(MAXSCOL-1, MAXSLNS-2);
  }
  else
  {
   f->a = e_set_pnt(cn->f[cn->mxedt-1]->a.x+1, cn->f[cn->mxedt-1]->a.y+1);
   f->e = e_set_pnt(cn->f[cn->mxedt-1]->e.x, cn->f[cn->mxedt-1]->e.y);
  }
 }
 f->winnum = cn->curedt;

 if (cn->mxedt > 1)
 {
  fo = cn->f[cn->mxedt-1];
  e_ed_rahmen(fo, 0);
 }
 /* The clipboard window (cn->f[0]) persists across shows; its view (pic) was
    freed when it was last closed.  We only reach here when it is NOT in the
    active list (the loop at the top would have switched to it otherwise), so
    f->pic is stale -- NULL it so e_firstl/e_ed_kst allocate a fresh view
    instead of closing an already-freed pic (the "free(): invalid pointer"
    double-free).  Matches e_firstl's contract that fresh windows have
    pic == NULL. */
 f->pic = NULL;
 e_firstl(f, 1);
 e_zlsplt(f);
 e_schirm(f, 1);
 e_cursor(f, 1);
 return(0);
}

/*   move block to buffer */
/* e_clip_export_buffer - mirror the internal clipboard (window 0) onto the OS
   clipboard, so a plain Copy or Cut (^C / ^Ins, Shift-Del) can be pasted into
   any other application.  Window 0 holds the just-copied block; we serialize it
   exactly as a save would (e_buffer_to_text, one '\n' per line) and drop the
   single trailing newline that serializer always appends, so a one-line copy
   does not gain a spurious blank line in the OS clipboard.  The bytes go to the
   front-end's OS writer (OSC 52 in a terminal, the X selections under X11); a
   no-op when no front-end registered one (headless build). */
static void e_clip_export_buffer(BUFFER *b0)
{
 char *text;
 int len;

 if (!b0)
  return;
 if (!(text = e_buffer_to_text(b0)))
  return;
 len = (int)strlen(text);
 if (len > 0 && text[len - 1] == '\n')
  len--;
 e_clip_os_set(text, len);
 free(text);
}

/* e_block_to_text - serialize the marked block straight from the SOURCE buffer.
   A line longer than mx.x is stored word-wrapped across several buffer lines;
   only a segment that ends in the real line marker (nrc > len, i.e. a trailing
   WPE_WR) is a true line break.  Joining the wrap-continuation segments -- as
   saving the file does -- keeps a copied long line whole, so the OS clipboard
   gets the full lines instead of the fragments the per-buffer-line clip serializer
   produced.  Returns a malloc'd string (caller frees), or NULL for an empty
   selection. */
static char *e_block_to_text(BUFFER *b, SCHIRM *s)
{
 int kay = s->mark_begin.y, kax = s->mark_begin.x;
 int key = s->mark_end.y, kex = s->mark_end.x;
 size_t cap = 1024, len = 0;
 char *t;
 int y;

 if (key < kay || (kay == key && kex <= kax)) return NULL;
 if ((t = malloc(cap)) == NULL) return NULL;
 for (y = kay; y <= key && y < b->mxlines; y++) {
  const char *ls = (const char *)b->bf[y].s;
  int ll = b->bf[y].len, a, e;
  if (ll < 0) ll = 0;
  a = (y == kay) ? kax : 0;
  e = (y == key) ? kex : ll;
  if (a < 0) a = 0;
  if (e > ll) e = ll;
  if (e < a) e = a;
  if (len + (size_t)(e - a) + 2 > cap) {
   char *nt;
   while (len + (size_t)(e - a) + 2 > cap) cap *= 2;
   if ((nt = realloc(t, cap)) == NULL) { free(t); return NULL; }
   t = nt;
  }
  if (ls && e > a) { memcpy(t + len, ls + a, (size_t)(e - a)); len += (size_t)(e - a); }
  if (y < key && b->bf[y].nrc > b->bf[y].len)   /* real line end, not a wrap segment */
   t[len++] = '\n';
 }
 t[len] = '\0';
 return t;
}

/**
 * e_clip_clear - Empty the editor's clipboard before a copy lands in it.
 * @b0: the clipboard's buffer.
 *
 * It keeps one empty line that ends with a line break, as it always did,
 * so the copied block lands on a line of its own.  Return: 0, or -1 when
 * memory ran out (the clipboard then keeps what it had).
 */
static int e_clip_clear(BUFFER *b0)
{
 if (e_buffer_clear(b0) != E_BUFFER_OK)
  return(-1);
 e_line_make_hard_empty(&b0->bf[0]);
 return(0);
}

int e_edt_del(FENSTER *f)
{
 e_edt_copy(f);
 e_blck_del(f);
 return(0);
}

/* copy block to buffer */
int e_edt_copy(FENSTER *f)
{
 BUFFER *b;
 BUFFER *b0 = f->ed->f[0]->b;
 int i, save;

 for (i = f->ed->mxedt; i > 0 && !DTMD_ISTEXT(f->ed->f[i]->dtmd); i--)
  ;
 if (i <= 0)
  return(0);
 e_switch_window(f->ed->edt[i], f);
 f = f->ed->f[f->ed->mxedt];
 b = f->b;  save = f->save;
 /* The only window that must NOT be a copy source is the clipboard viewer
    itself (window 0): the code below frees window 0's lines and then copies
    from the source, so if the source IS window 0 that is a use-after-free.
    Guard exactly that, by buffer identity -- other read-only panes (ins == 8:
    the AI chat/Messages output) are legitimate sources; refusing them left
    their text uncopyable. */
 if (b == b0)
  return(0);
 if ((f->s->mark_end.y < f->s->mark_begin.y) ||
   ((f->s->mark_begin.y == f->s->mark_end.y) &&
     (f->s->mark_end.x <= f->s->mark_begin.x)))
 {
  return(0);
 }
 if (e_clip_clear(b0) != 0)
 {
  wpe_buffer_say_nomem(b0, "the copy");
  return(0);
 }
 e_copy_block(0, 0, b, b0, f);
 f->save = save;
 /* Export to the OS clipboard from the SOURCE selection, not the clip buffer:
    e_buffer_to_text splits a wrapped long line at every buffer segment, so the
    OS clipboard got fragments.  e_block_to_text rejoins the wrap-continuations.
    Fall back to the clip buffer if serializing the selection fails. */
 {
  char *bt = e_block_to_text(b, f->s);
  if (bt) {
   int L = (int)strlen(bt);
   if (L > 0 && bt[L - 1] == '\n') L--;
   e_clip_os_set(bt, L);
   free(bt);
  } else {
   e_clip_export_buffer(b0);
  }
 }
 return(0);
}

/**
 * e_clip_pull_os_into_buffer - Before a paste, take what another application
 * copied into the editor's clipboard.
 * @w0: window 0, the editor's clipboard.
 *
 * Ctrl-V, Shift-Insert, Alt-Insert and a dialog field's Alt-paste then paste
 * what was last copied in any application (the merged-clipboard model).  Does
 * nothing when the editor owns the selection, because its own clipboard is the
 * newer copy, and in a terminal, which cannot read the system's selection:
 * the editor's clipboard is pasted there.
 */
void e_clip_pull_os_into_buffer(FENSTER *w0)
{
 char *text;
 int len = 0;

 if ((text = e_clip_os_get(&len)) == NULL)
  return;
 if (e_buffer_replace_all(w0->b, text, E_BUFFER_STEP_NEW) == E_BUFFER_NOMEM)
  wpe_buffer_say_nomem(w0->b, "the pasted text");
 free(text);
 w0->s->mark_begin.x = w0->s->mark_begin.y = 0;
 w0->s->mark_end.y = w0->b->mxlines - 1;
 w0->s->mark_end.x = w0->b->bf[w0->b->mxlines - 1].len;
}

/*            Copy block buffer into window  */
int e_edt_einf(FENSTER *f)
{
 BUFFER *b;
 BUFFER *b0 = f->ed->f[0]->b;
 int i;

 /* A window closed to text changes (the clipboard/Show Buffer viewer, a locked
    file, one waiting for an assistant reply) rejects paste -- ^V, Shift-Ins, or
    a middle click -- BEFORE any side effect: pasting into the clipboard viewer
    is a self-paste that corrupts the heap, and editing a read-only buffer makes
    no sense.  (The check further down runs only after
    e_clip_pull_os_into_buffer has already rebuilt window 0, so it is too late
    to guard here.) */
 if (e_window_text_locked(f))
  return(0);

 e_clip_pull_os_into_buffer(f->ed->f[0]);   /* merge: ^V from the OS clipboard */

 for (i = f->ed->mxedt; i > 0 && !DTMD_ISTEXT(f->ed->f[i]->dtmd); i--)
  ;
 if (i <= 0)
  return(0);
 e_switch_window(f->ed->edt[i], f);
 f = f->ed->f[f->ed->mxedt];
 b = f->b;
 if (e_window_text_locked(f))
  return(0);
 e_blk_cover_begin(b0, b);
 i = e_copy_block(b->b.x, b->b.y, b0, b, f);
 e_blk_cover_end(b0, b);
 if (i != E_BLOCK_MOVED)
  return(0);
 e_block_record_push('c', b, b->b.x, b->b.y);
 sc_txt_2(f);
 f->save = b->cn->maxchg + 1;
 return(0);
}

/*   move block within window */
int e_blck_move(FENSTER *f)
{
 BUFFER *b;
 int i;
 POINT ka;

 for (i = f->ed->mxedt; i > 0 && !DTMD_ISTEXT(f->ed->f[i]->dtmd); i--)
  ;
 if (i <= 0)
  return(0);
 e_switch_window(f->ed->edt[i], f);
 f = f->ed->f[f->ed->mxedt];
 b = f->b;
 ka.x = f->ed->f[f->ed->mxedt]->s->mark_begin.x;
 if (b->b.y > f->ed->f[f->ed->mxedt]->s->mark_begin.y)
  ka.y = f->ed->f[f->ed->mxedt]->s->mark_begin.y;
 else
 {
  ka.y = f->ed->f[f->ed->mxedt]->s->mark_end.y;
  if (b->b.y == f->ed->f[f->ed->mxedt]->s->mark_begin.y &&
    b->b.x < f->ed->f[f->ed->mxedt]->s->mark_begin.x)
   ka.x = f->ed->f[f->ed->mxedt]->s->mark_end.x +
     f->ed->f[f->ed->mxedt]->s->mark_begin.x - b->b.x;
 }
 if (e_move_block_covered(b->b.x, b->b.y, b, b, f) != E_BLOCK_MOVED)
  return(0);
 e_block_record_push('v', b, ka.x, ka.y);
 f->save = b->cn->maxchg + 1;
 return(0);
}

/*    move Block    */
/**
 * e_blk_line_bytes - The bytes of line @y, its line break included.
 */
static int e_blk_line_bytes(BUFFER *b, int y)
{
 int n = b->bf[y].len;

 if (b->bf[y].s[n] != '\0')        /* the line break */
  n++;
 return(n);
}

/**
 * e_blk_bytes_dup - A copy of @n bytes of @src (at least one byte is
 * allocated).  Return: the copy, or NULL when memory ran out.
 */
static unsigned char *e_blk_bytes_dup(const unsigned char *src, int n)
{
 unsigned char *out;
 int size = n;

 if (size < 1)
  size = 1;
 if ((out = MALLOC(size)) == NULL)
  return(NULL);
 if (n > 0)
  memcpy(out, src, (size_t)n);
 return(out);
}

/**
 * e_blk_ends_free - Free the two partial lines of a block.
 */
static void e_blk_ends_free(WpeBlockEnds *e)
{
 FREE(e->first);
 FREE(e->last);
 e->first = e->last = NULL;
}

/**
 * e_blk_ends_take - Copy the two partial lines of block @k of buffer @b.
 * @with_first: 0 when the first line goes whole (a move of a block that
 * starts at column 0), so only the last line's bytes are wanted.
 *
 * Taken before any line moves, so a block copied or moved onto one of its
 * own lines still carries exactly the text the user marked.
 * Return: 0, or -1 when memory ran out (nothing is held).
 */
static int e_blk_ends_take(BUFFER *b, const WpeBlock *k, int with_first, WpeBlockEnds *e)
{
 e->first = e->last = NULL;
 e->nfirst = 0;
 e->nlast = k->kex;
 if (with_first)
 {
  e->nfirst = e_blk_line_bytes(b, k->kay) - k->kax;
  if (e->nfirst < 0)
   e->nfirst = 0;
  if ((e->first = e_blk_bytes_dup(b->bf[k->kay].s + k->kax, e->nfirst)) == NULL)
   return(-1);
 }
 if ((e->last = e_blk_bytes_dup(b->bf[k->key].s, e->nlast)) == NULL)
 {
  e_blk_ends_free(e);
  return(-1);
 }
 return(0);
}

/**
 * e_blk_line_dup - A copy of a line, as a new line of buffer @dst.
 * @src: the line; @out: receives the copy.
 *
 * Block Copy duplicates the block's whole lines, so the copy does not
 * change with later edits of the original.  The copy is clamped to the
 * width of @dst, line break included.  Return: E_BUFFER_OK or
 * E_BUFFER_NOMEM.
 */
static int e_blk_line_dup(BUFFER *dst, const STRING *src, STRING *out)
{
 int n = src->nrc;

 if (e_buffer_line_new(dst, out) != E_BUFFER_OK)
  return(E_BUFFER_NOMEM);
 if (n > dst->mx.x)
  n = dst->mx.x;
 if (n < 0)
  n = 0;
 memcpy(out->s, src->s, (size_t)n);
 out->s[n] = '\0';
 out->len = e_str_len(out->s);
 out->nrc = e_str_nrc(out->s);
 return(E_BUFFER_OK);
}

/**
 * e_blk_lines_dup - Copies of @n lines of @src from row @from, as lines of
 * @dst.  Return: the array (the caller splices or frees it), or NULL when
 * memory ran out.
 */
static STRING *e_blk_lines_dup(BUFFER *src, int from, int n, BUFFER *dst)
{
 STRING *out = MALLOC(n * sizeof(STRING));
 int i;

 if (out == NULL)
  return(NULL);
 for (i = 0; i < n; i++)
  if (e_blk_line_dup(dst, &src->bf[from + i], &out[i]) != E_BUFFER_OK)
  {
   e_buffer_lines_free(out, i);
   return(NULL);
  }
 return(out);
}

/**
 * e_blk_ends_land - Write the two partial lines of a block where it lands.
 * @b, @s: the destination buffer and its screen; @x, @y: where the block
 * starts; @ylast: the row the block's last line ends on; @first: the bytes
 * of the first line (NULL when it landed whole); @nfirst: how many;
 * @last, @nlast: the bytes of the last line.
 *
 * The last line's bytes go in front of the rest of the line the block was
 * dropped into, the first line's after the part before the cursor; a line
 * that grows past the window's width wraps as typing would wrap it.
 */
static void e_blk_ends_land(BUFFER *b, SCHIRM *s, int x, int y, int ylast,
                            unsigned char *first, int nfirst,
                            unsigned char *last, int nlast)
{
 if (nlast > 0)
  e_ins_nchar(b, s, last, 0, ylast, nlast);
 if (first != NULL && nfirst > 0)
  e_ins_nchar(b, s, first, x, y, nfirst);
}

/**
 * e_blk_marks_landed - Mark the block where it landed and put the cursor at
 * its end.
 * @b, @s: the destination; @x, @y: where it starts; @k: its old place (for
 * its size).
 */
static void e_blk_marks_landed(BUFFER *b, SCHIRM *s, int x, int y, const WpeBlock *k)
{
 s->mark_begin.x = x;
 s->mark_begin.y = y;
 b->b.x = s->mark_end.x = k->kex;
 b->b.y = s->mark_end.y = k->key - k->kay + y;
}

/**
 * e_blk_lines_free - Free what a move holds of the lines it took out: the
 * arrays, and the first line when it was merged into another line rather
 * than landed whole (its bytes were copied there).
 */
static void e_blk_lines_free(WpeBlockLines *dl)
{
 if (dl->lines != NULL && dl->first_in_hand)
  FREE(dl->lines[0].s);
 dl->first_in_hand = 0;
 FREE(dl->lines);
 FREE(dl->rows);
 dl->lines = NULL;
 dl->rows = NULL;
}

/**
 * e_blk_detach - Take the whole lines of block @k out of @bv, with their
 * breakpoints, without freeing them.
 *
 * Those are the lines between the first and the last, and the first too
 * when the block starts at column 0: then the first line belongs to the
 * block whole, and its breakpoint travels with it.  Nothing is freed, so no
 * line can be freed twice, and the rows leave the margin with their lines.
 * Return: E_BUFFER_OK, or E_BUFFER_NOMEM with nothing taken.
 */
static int e_blk_detach(BUFFER *bv, const WpeBlock *k, int whole_first, WpeBlockLines *dl)
{
 int r;

 memset(dl, 0, sizeof(*dl));
 if (whole_first)
  dl->at = k->kay;
 else
  dl->at = k->kay + 1;
 dl->n = k->key - dl->at;
 if (dl->n <= 0)
 {
  dl->n = 0;
  return(E_BUFFER_OK);
 }
 dl->lines = MALLOC(dl->n * sizeof(STRING));
 dl->rows = MALLOC(dl->n * sizeof(int));
 if (dl->lines == NULL || dl->rows == NULL)
 {
  e_blk_lines_free(dl);
  return(E_BUFFER_NOMEM);
 }
 r = e_buffer_detach_lines(bv, dl->at, dl->n, dl->lines, dl->rows, &dl->nrows);
 if (r != E_BUFFER_OK)
  e_blk_lines_free(dl);
 return(r);
}

/**
 * e_blk_rows_after_first - The breakpoint rows of the lines after the first,
 * counted from the second: the first line is merged into another line, not
 * landed whole.
 * @dl: the lines; @out: room for dl->nrows rows.  Return: how many.
 */
static int e_blk_rows_after_first(const WpeBlockLines *dl, int *out)
{
 int i, k = 0;

 for (i = 0; i < dl->nrows; i++)
  if (dl->rows[i] >= 1)
   out[k++] = dl->rows[i] - 1;
 return(k);
}

/**
 * e_blk_land_whole - Land every taken line whole at row @y of @bz, above
 * the line there, each with its breakpoint.
 * Return: as e_buffer_splice; on success no line is left in hand.
 */
static int e_blk_land_whole(BUFFER *bz, int y, WpeBlockLines *dl)
{
 int r = e_buffer_splice(bz, y, 0, dl->lines, dl->n, dl->rows, dl->nrows,
                         E_BUFFER_STEP_COVERED);

 if (r == E_BUFFER_OK)
  dl->n = 0;
 return(r);
}

/**
 * e_blk_land_between - Land the taken lines at row @y of @bz, between the
 * halves of a line just cut there; with @whole_first the first of them is
 * kept in hand, to be merged into the line before the cut.
 * Return: as e_buffer_splice; on success only that first line is in hand.
 */
static int e_blk_land_between(BUFFER *bz, int y, int whole_first, WpeBlockLines *dl)
{
 STRING *mids = dl->lines;
 int *rows = dl->rows, nrows = dl->nrows, n = dl->n, r;

 if (whole_first)
 {
  mids++;
  n--;
  if (nrows > 0 && (rows = MALLOC(nrows * sizeof(int))) == NULL)
   return(E_BUFFER_NOMEM);
  nrows = e_blk_rows_after_first(dl, rows);
 }
 r = E_BUFFER_OK;
 if (n > 0)
  r = e_buffer_splice(bz, y, 0, mids, n, rows, nrows, E_BUFFER_STEP_COVERED);
 if (rows != dl->rows)
  FREE(rows);
 if (r != E_BUFFER_OK)
  return(r);
 dl->first_in_hand = whole_first;
 dl->n = 0;
 return(E_BUFFER_OK);
}

/**
 * e_blk_land_lines - Put the whole lines of a moved block into @bz at
 * (@x, @y), before its partial lines are written.
 * @landed: receives how many rows were added above the line the block was
 * dropped into (that line is then row @y + *@landed).
 *
 * At column 0 a block whose first line is whole lands as whole lines above
 * the line at the cursor, each with its breakpoint.  Otherwise the line at
 * the cursor is cut in two and the lines between go between the halves.
 * Return: E_BUFFER_OK, or E_BUFFER_NOMEM with every taken line still in
 * hand (*@landed then says how many rows the cut added).
 */
static int e_blk_land_lines(BUFFER *bz, int x, int y, int whole_first,
                            WpeBlockLines *dl, int *landed)
{
 int n = dl->n;

 *landed = 0;
 if (whole_first && x == 0)
 {
  if (n > 0 && e_blk_land_whole(bz, y, dl) != E_BUFFER_OK)
   return(E_BUFFER_NOMEM);
  *landed = n;
  return(E_BUFFER_OK);
 }
 if (e_buffer_split_line(bz, y, x, E_BUFFER_STEP_COVERED, E_BUFFER_HEAD_OPEN) != E_BUFFER_OK)
  return(E_BUFFER_NOMEM);
 *landed = 1;
 if (n == 0)
  return(E_BUFFER_OK);
 if (e_blk_land_between(bz, y + 1, whole_first, dl) != E_BUFFER_OK)
  return(E_BUFFER_NOMEM);
 *landed += n;
 if (whole_first)
  (*landed)--;
 return(E_BUFFER_OK);
}

/**
 * e_blk_lines_put_back - A move that could not land its lines returns them
 * to where they were taken from, with their breakpoints.
 * @at: their row now (moved by what the failed landing added above it).
 *
 * The user's text is then what it was: a line the landing may have cut at
 * the cursor is written to the file as the one line it was.  Only when even
 * that is refused are the lines lost, and the status row says so.
 */
static void e_blk_lines_put_back(BUFFER *bv, WpeBlockLines *dl, int at)
{
 if (dl->n > 0 && e_buffer_splice(bv, at, 0, dl->lines, dl->n, dl->rows, dl->nrows,
                                  E_BUFFER_STEP_COVERED) == E_BUFFER_OK)
  dl->n = 0;
 if (dl->n > 0)
 {
  e_buffer_lines_free(dl->lines, dl->n);
  dl->lines = NULL;
  dl->n = 0;
 }
 e_blk_lines_free(dl);
}

/**
 * e_blk_dup_chars - Copy a run of characters out of a line into a new buffer.
 * @b: The text buffer to read from.
 * @y: The line to read.
 * @x: The first column to copy.
 * @n: How many characters to copy.
 *
 * The single-line block Move cases lift the marked characters out of one
 * line so they can be re-inserted elsewhere on the same or another line.
 * Returns a freshly allocated maxcol-sized buffer holding the @n characters
 * (the caller frees it), or NULL on out-of-memory.
 */
static unsigned char *e_blk_dup_chars(BUFFER *b, int y, int x, int n)
{
 unsigned char *out;
 int i;

 if ((out = MALLOC(b->cn->maxcol * sizeof(char))) == NULL)
  return NULL;
 for (i = 0; i < n; i++)
  out[i] = b->bf[y].s[x + i];
 return out;
}

/**
 * e_blk_nomem - Say on the status row that a block operation was refused
 * for want of memory.
 * @b: the buffer it was to change; @what: the operation, as the user names
 * it.  Return: E_BLOCK_NOT_MOVED.
 */
static int e_blk_nomem(BUFFER *b, const char *what)
{
 wpe_buffer_say_nomem(b, what);
 return(E_BLOCK_NOT_MOVED);
}

/**
 * e_blk_row_after_landing - Where row @row of a block's own buffer is once
 * the landing added @landed rows, which moved it down when the block landed
 * @above it.
 */
static int e_blk_row_after_landing(int row, int above, int landed)
{
 if (above)
  return(row + landed);
 return(row);
}

/**
 * e_blk_first_of_moved - The bytes of a moved block's first line that are
 * still to be written where it landed.
 * @first, @nfirst: receive them; NULL when the first line landed whole.
 *
 * A block that starts at column 0 took its first line whole: dropped at
 * column 0 it landed whole with its breakpoint, dropped inside a line its
 * text is merged there.  Otherwise the first line's part from the block's
 * start on is written after the cursor.
 */
static void e_blk_first_of_moved(const WpeBlockLines *dl, WpeBlockEnds *e,
                                 unsigned char **first, int *nfirst)
{
 if (dl->first_in_hand)
 {
  *first = dl->lines[0].s;
  *nfirst = dl->lines[0].nrc;
  return;
 }
 *first = e->first;
 *nfirst = e->nfirst;
}

/**
 * e_blk_lands_on_itself - Would a block land inside itself?  A move or copy
 * of a block of more than one line to a row of the block in the same
 * buffer changes nothing the user could want, so it is refused.
 */
static int e_blk_lands_on_itself(BUFFER *bv, BUFFER *bz, int y, const WpeBlock *k)
{
 return(bv == bz && y >= k->kay && y <= k->key);
}

/**
 * e_blk_column_on_line - The column (@x, @y) of @b stands for: a cursor
 * past the end of its line lands the block at the end of that line.
 * Return: the column, or -1 when @y is not a line of @b.
 */
static int e_blk_column_on_line(BUFFER *b, int x, int y)
{
 if (y < 0 || y >= b->mxlines)
  return(-1);
 if (x > b->bf[y].len)
  return(b->bf[y].len);
 if (x < 0)
  return(0);
 return(x);
}

/**
 * e_blk_ends_cut - Take the two partial lines of a moved block out of the
 * buffer it leaves, joining what is left of the first and the last line.
 * @bv, @sv: the buffer and its screen; @k: the block; @whole_first: its
 * first line was taken whole; @shift: how far the landing moved the
 * block's rows down (it landed above them in the same buffer).
 */
static void e_blk_ends_cut(BUFFER *bv, SCHIRM *sv, const WpeBlock *k, int whole_first,
                           int shift, const WpeBlockEnds *e)
{
 int first = k->kay + shift;

 if (e->nlast > 0 && whole_first)
  e_del_nchar(bv, sv, 0, first, e->nlast);
 else if (e->nlast > 0)
  e_del_nchar(bv, sv, 0, first + 1, e->nlast);
 if (!whole_first)
  e_del_nchar(bv, sv, k->kax, first, e->nfirst);
}

/**
 * e_blk_move_lines - Move a block of more than one line to (@x, @y) of
 * @bz: whole lines first taken out, then landed, then the partial lines.
 * @k: the block.
 *
 * Each line of the block changes hands once, so none is freed twice, and a
 * breakpoint on a line that moves whole moves with it; Ctrl-U and Ctrl-R
 * bring it back and forth with its code.  The cursor of a move inside one
 * buffer is outside the block (the caller checked).
 * Return: E_BLOCK_MOVED, or E_BLOCK_NOT_MOVED when memory ran out (the
 * status row says so; the user's text is as it was).
 */
static int e_blk_move_lines(int x, int y, BUFFER *bv, BUFFER *bz, const WpeBlock *k)
{
 SCHIRM *sv = bv->f->s;
 SCHIRM *sz = bz->f->s;
 int same = (bv == bz), whole_first = (k->kax == 0), below, landed, before;
 WpeBlockEnds ends;
 WpeBlockLines dl;
 unsigned char *first;
 int nfirst;

 below = same && y > k->key;
 if (e_blk_ends_take(bv, k, !whole_first, &ends) != 0)
  return(e_blk_nomem(bz, "the block move"));
 if (e_blk_detach(bv, k, whole_first, &dl) != E_BUFFER_OK)
 {
  e_blk_ends_free(&ends);
  return(e_blk_nomem(bz, "the block move"));
 }
 if (below)
  y -= dl.n;
 if (e_blk_land_lines(bz, x, y, whole_first, &dl, &landed) != E_BUFFER_OK)
 {
  e_blk_lines_put_back(bv, &dl, e_blk_row_after_landing(dl.at, same && !below, landed));
  e_blk_ends_free(&ends);
  return(e_blk_nomem(bz, "the block move"));
 }
 before = bv->mxlines;
 e_blk_ends_cut(bv, sv, k, whole_first, e_blk_row_after_landing(0, same && !below, landed),
                &ends);
 if (below)
  y -= before - bv->mxlines;
 e_blk_first_of_moved(&dl, &ends, &first, &nfirst);
 e_blk_ends_land(bz, sz, x, y, y + landed, first, nfirst, ends.last, ends.nlast);
 e_blk_lines_free(&dl);
 e_blk_ends_free(&ends);
 sv->mark_begin.x = sv->mark_end.x = bv->b.x = k->kax;
 sv->mark_begin.y = sv->mark_end.y = bv->b.y = k->kay;
 e_blk_marks_landed(bz, sz, x, y, k);
 return(E_BLOCK_MOVED);
}

/**
 * e_move_block - Move the block the user's window marks from @bv to
 * (@x, @y) of @bz.
 * @f: the window the user works in (its marks are the block).
 *
 * Block Move (Ctrl-K V), a block delete into its undo record and that
 * record's Undo all move lines this way.  Return: E_BLOCK_MOVED, or
 * E_BLOCK_NOT_MOVED when there was nothing to move, the window is closed to
 * changes, the cursor is inside the block, or memory ran out.
 */
int e_move_block(int x, int y, BUFFER *bv, BUFFER *bz, FENSTER *f)
{
 SCHIRM *s = f->ed->f[f->ed->mxedt]->s;
 SCHIRM *sv = bv->f->s;
 SCHIRM *sz = bz->f->s;
 int n;
 int kax = s->mark_begin.x, kay = s->mark_begin.y, kex = s->mark_end.x, key = s->mark_end.y;
 WpeBlock k;
 unsigned char *cstr;

 e_blk_of(s, &k);
 if (e_blk_is_empty(&k))
  return(E_BLOCK_NOT_MOVED);
 if (e_window_text_locked(f))
 {
  return(E_BLOCK_NOT_MOVED);
 }
 if (bv == bz && y >= kay && y <= key && x >= kax && x <= kex)
 {
  return(E_BLOCK_NOT_MOVED);
 }
 if (kay == key)
 {
  n = kex - kax;
  bz->b.x = x; bz->b.y = y;
  if ((cstr = e_blk_dup_chars(bv, key, kax, n)) == NULL)
  {
   e_error(e_msg[ERR_LOWMEM], 0, bz->fb);
   return(E_BLOCK_NOT_MOVED);
  }
  e_ins_nchar(bz, sz, cstr, x, y, n);
  bv->b.x = kax;  bv->b.y = kay+bz->b.y-y;
  e_del_nchar(bv, sv, sv->mark_begin.x, sv->mark_begin.y, n);
  if (bv == bz && kay == y && x > kex)
   x -= n;
  sz->mark_begin.x = x;   sz->mark_end.x = bz->b.x = x + n;
  sz->mark_begin.y = sz->mark_end.y = bz->b.y = y;
  e_cursor(f, 1);
  e_schirm(f, 1);
  FREE(cstr);
  return(E_BLOCK_MOVED);
 }

 if (bv == bz && y == kay && x < kax )
 {
  n = kax - x;
  bv->b.x = x; bv->b.y = y;
  if ((cstr = e_blk_dup_chars(bv, y, x, n)) == NULL)
  {
   e_error(e_msg[ERR_LOWMEM], 0, bz->fb);
   return(E_BLOCK_NOT_MOVED);
  }
  e_del_nchar(bv, sv, x, y, n);
  bz->b.x = kex; bz->b.y = key;
  e_ins_nchar(bz, sz, cstr, kex, key, n);
  bv->b.x = sv->mark_begin.x = x;  bv->b.y = sv->mark_begin.y = y;
  sv->mark_end.x = kex;  sv->mark_end.y = key;
  e_cursor(f, 1);
  e_schirm(f, 1);
  FREE(cstr);
  return(E_BLOCK_MOVED);
 }

 if (bv == bz && y == key && x > kex )
 {
  n = x - kex;
  bv->b.x = kex; bv->b.y = y;
  if ((cstr = e_blk_dup_chars(bv, y, kex, n)) == NULL)
  {
   e_error(e_msg[ERR_LOWMEM], 0, bz->fb);
   return(E_BLOCK_NOT_MOVED);
  }
  e_del_nchar(bv, sv, kex, y, n);
  bz->b.x = kex; bz->b.y = key;
  e_ins_nchar(bz, sz, cstr, kax, kay, n);

  bv->b.x = sv->mark_end.x;  bv->b.y = sv->mark_end.y;
  e_cursor(f, 1);
  e_schirm(f, 1);
  FREE(cstr);
  return(E_BLOCK_MOVED);
 }

 if (e_blk_lands_on_itself(bv, bz, y, &k))
  return(E_BLOCK_NOT_MOVED);
 x = e_blk_column_on_line(bz, x, y);
 if (x < 0)
  return(E_BLOCK_NOT_MOVED);
 if (e_blk_move_lines(x, y, bv, bz, &k) != E_BLOCK_MOVED)
  return(E_BLOCK_NOT_MOVED);
 f->save = f->ed->maxchg + 1;
 sc_txt_1(bv->f);
 sc_txt_1(bz->f);
 e_cursor(f, 1);
 e_schirm(f, 1);
 return(E_BLOCK_MOVED);
}

/**
 * e_move_block_covered - Move a block as one action for Undo.
 * @x, @y, @bv, @bz, @f: as e_move_block.
 *
 * The steps of the move are left out of Undo on both buffers; the caller
 * pushes the one record that replays the whole move, so one Ctrl-U takes it
 * back and the breakpoints return with their lines.
 * Return: as e_move_block.
 */
int e_move_block_covered(int x, int y, BUFFER *bv, BUFFER *bz, FENSTER *f)
{
 int r, fault;

 e_blk_cover_begin(bv, bz);
 fault = e_buffer_test_fault_enter(E_BUFFER_FAULT_BLOCK_MOVE);
 r = e_move_block(x, y, bv, bz, f);
 e_buffer_test_fault_leave(fault);
 e_blk_cover_end(bv, bz);
 return(r);
}

/*   copy block within window   */
int e_blck_copy(FENSTER *f)
{
 BUFFER *b;
 int i;

 for (i = f->ed->mxedt; i > 0 && !DTMD_ISTEXT(f->ed->f[i]->dtmd); i--)
  ;
 if (i <= 0)
  return(0);
 e_switch_window(f->ed->edt[i], f);
 f = f->ed->f[f->ed->mxedt];
 b = f->b;
 /* Block-Copy (Ctrl-K C / Block menu) duplicates the block at the cursor, which
    would edit the buffer -- impossible in a window closed to text changes (a
    read-only tool pane such as the AI chat / Messages output, or a window
    waiting for an assistant reply).  Rather than do nothing, treat it there as
    a clipboard copy, so the instinctive "Copy" pulls the selected text out
    like Edit > Copy does. */
 if (e_window_text_locked(f)) return(e_edt_copy(f));
 f->save = 1;
 e_blk_cover_begin(b, b);
 i = e_copy_block(b->b.x, b->b.y, b, b, f);
 e_blk_cover_end(b, b);
 if (i == E_BLOCK_MOVED)
  e_block_record_push('c', b, b->b.x, b->b.y);
 sc_txt_2(f);
 f->save = b->cn->maxchg + 1;
 return(0);
}

/**
 * e_blk_copy_lands_inside - Would a copy of a block of more than one line
 * land inside the block it copies?  It may land at the block's start or end.
 */
static int e_blk_copy_lands_inside(BUFFER *bs, BUFFER *bd, int x, int y, const WpeBlock *k)
{
 if (bs != bd)
  return(0);
 return((y > k->kay && y < k->key) || (y == k->kay && x >= k->kax) ||
        (y == k->key && x < k->kex));
}

/**
 * e_blk_copy_lines - Copy a block of more than one line to (@x, @y) of @bd.
 * @bs: the buffer it is copied from; @k: the block; @f: the user's window.
 *
 * The block's text is taken first, so a copy onto one of its own lines is
 * the text the user marked.  The line at the cursor is cut in two, copies
 * of the whole lines go between the halves (a copy carries no breakpoint),
 * then the partial lines are written.  Breakpoints below the cursor move
 * down with their code.
 * Return: E_BLOCK_MOVED, or E_BLOCK_NOT_MOVED when the cursor is inside
 * the block or not on a line, or memory ran out (the status row then says
 * so; the file's text is as it was).
 */
static int e_blk_copy_lines(int x, int y, BUFFER *bs, BUFFER *bd, const WpeBlock *k)
{
 SCHIRM *s_dst = bd->f->s;
 int n = k->key - k->kay - 1;
 WpeBlockEnds ends;
 STRING *mids = NULL;

 if (e_blk_copy_lands_inside(bs, bd, x, y, k))
  return(E_BLOCK_NOT_MOVED);
 x = e_blk_column_on_line(bd, x, y);
 if (x < 0)
  return(E_BLOCK_NOT_MOVED);
 if (e_blk_ends_take(bs, k, 1, &ends) != 0)
  return(e_blk_nomem(bd, "the block copy"));
 if (n > 0 && (mids = e_blk_lines_dup(bs, k->kay + 1, n, bd)) == NULL)
 {
  e_blk_ends_free(&ends);
  return(e_blk_nomem(bd, "the block copy"));
 }
 if (e_buffer_split_line(bd, y, x, E_BUFFER_STEP_COVERED, E_BUFFER_HEAD_OPEN) != E_BUFFER_OK ||
     (n > 0 && e_buffer_splice(bd, y + 1, 0, mids, n, NULL, 0, E_BUFFER_STEP_COVERED) != E_BUFFER_OK))
 {
  e_buffer_lines_free(mids, n);
  e_blk_ends_free(&ends);
  return(e_blk_nomem(bd, "the block copy"));
 }
 FREE(mids);
 e_blk_ends_land(bd, s_dst, x, y, y + 1 + n, ends.first, ends.nfirst, ends.last, ends.nlast);
 e_blk_ends_free(&ends);
 e_blk_marks_landed(bd, s_dst, x, y, k);
 return(E_BLOCK_MOVED);
}

/**
 * e_blk_copy_chars - Copy a block inside one line to (@x, @y) of @bd.
 * @bs: the buffer it is copied from; @k: the block.
 *
 * The characters go in at the cursor and are marked there as the block.
 * Return: E_BLOCK_MOVED, or E_BLOCK_NOT_MOVED when the cursor is inside the
 * block or memory ran out.
 */
static int e_blk_copy_chars(int x, int y, BUFFER *bs, BUFFER *bd, const WpeBlock *k)
{
 SCHIRM *s_dst = bd->f->s;
 int n = k->kex - k->kax;
 unsigned char *cstr;

 if (bs == bd && y == k->kay && x >= k->kax && x < k->kex)
  return(E_BLOCK_NOT_MOVED);
 if ((cstr = e_blk_bytes_dup(bs->bf[k->key].s + k->kax, n)) == NULL)
 {
  e_error(e_msg[ERR_LOWMEM], 0, bd->fb);
  return(E_BLOCK_NOT_MOVED);
 }
 bd->b.x = x; bd->b.y = y;
 e_ins_nchar(bd, s_dst, cstr, x, y, n);
 s_dst->mark_begin.x = x;
 s_dst->mark_end.x = bd->b.x = x + n;
 s_dst->mark_begin.y = s_dst->mark_end.y = bd->b.y = y;
 FREE(cstr);
 return(E_BLOCK_MOVED);
}

/**
 * e_blk_copy_fault_point - The test fault point of a copy into @bd: a copy
 * or a paste into the user's text is one; a copy into the clipboard, a view
 * the editor writes, is not, so a test can fill the clipboard and then
 * starve the paste.
 */
static int e_blk_copy_fault_point(BUFFER *bd)
{
 if (bd->history == E_BUFFER_NO_HISTORY)
  return(E_BUFFER_FAULT_NONE);
 return(E_BUFFER_FAULT_BLOCK_COPY);
}

/**
 * e_copy_block - Copy the block @buffer_src's window marks to (@x, @y) of
 * @buffer_dst.
 * @f: the window the user works in.
 *
 * Block Copy (Ctrl-K C), Copy to the clipboard and Paste all copy this way.
 * Return: E_BLOCK_MOVED, or E_BLOCK_NOT_MOVED when there was nothing to
 * copy, the cursor is inside the block, or memory ran out.
 */
int e_copy_block(int x, int y, BUFFER *buffer_src, BUFFER *buffer_dst,
  FENSTER *f)
{
 WpeBlock k;
 int r, fault;

 e_blk_of(buffer_src->f->s, &k);
 if (e_blk_is_empty(&k))
  return(E_BLOCK_NOT_MOVED);
 fault = e_buffer_test_fault_enter(e_blk_copy_fault_point(buffer_dst));
 if (k.kay == k.key)
  r = e_blk_copy_chars(x, y, buffer_src, buffer_dst, &k);
 else
  r = e_blk_copy_lines(x, y, buffer_src, buffer_dst, &k);
 e_buffer_test_fault_leave(fault);
 if (r != E_BLOCK_MOVED)
  return(r);
 e_cursor(f, 1);
 e_schirm(f, 1);
 return(E_BLOCK_MOVED);
}

/*   delete block marks   */
int e_blck_hide(FENSTER *f)
{
 BUFFER *b;
 SCHIRM *s;
 int i;

 for (i = f->ed->mxedt; i > 0 && !DTMD_ISTEXT(f->ed->f[i]->dtmd); i--)
  ;
 if (i <= 0)
  return(0);
 e_switch_window(f->ed->edt[i], f);
 f = f->ed->f[f->ed->mxedt];
 b = f->b;
 s = f->s;
 s->mark_begin = e_set_pnt(0, 0);
 s->mark_end = e_set_pnt(0, 0);
 e_schirm(f, 1);
 return(0);
}

/*   mark begin of block   */
int e_blck_begin(FENSTER *f)
{
 int i;

 for (i = f->ed->mxedt; i > 0 && !DTMD_ISTEXT(f->ed->f[i]->dtmd); i--)
  ;
 if (i <= 0) return(0);
 e_switch_window(f->ed->edt[i], f);
 f = f->ed->f[f->ed->mxedt];
 /* Modern (default): Begin Mark starts a FRESH, empty block at the cursor,
    so re-marking clears any stale end left over from a previous block (e.g.
    the destination marks of a just-completed Copy/Move) -- End Mark (^K K)
    then extends it.  WordStar mode (ED_BLOCK_WORDSTAR): begin and end are
    independent markers, so Begin Mark sets only the begin and leaves the end
    untouched (classic Borland behaviour).  Toggle in Options -> Editor. */
 if (f->ed->edopt & ED_BLOCK_WORDSTAR)
  f->s->mark_begin = f->b->b;
 else
  f->s->mark_begin = f->s->mark_end = f->b->b;
 e_schirm(f, 1);
 return(0);
}

/*           Set end of block   */
int e_blck_end(FENSTER *f)
{
 int i;

 for (i = f->ed->mxedt; i > 0 && !DTMD_ISTEXT(f->ed->f[i]->dtmd); i--)
  ;
 if (i <= 0)
  return(0);
 e_switch_window(f->ed->edt[i], f);
 f = f->ed->f[f->ed->mxedt];
 f->s->mark_end = f->b->b;
 e_schirm(f, 1);
 return(0);
}

/* goto begin of block   */
int e_blck_gt_beg(FENSTER *f)
{
 int i;

 for (i = f->ed->mxedt; i > 0 && !DTMD_ISTEXT(f->ed->f[i]->dtmd); i--)
  ;
 if (i <= 0)
  return(0);
 e_switch_window(f->ed->edt[i], f);
 f = f->ed->f[f->ed->mxedt];
 f->b->b = f->s->mark_begin;
 e_schirm(f, 1);
 return(0);
}

/*   goto end of block   */
int e_blck_gt_end(FENSTER *f)
{
 int i;

 for (i = f->ed->mxedt; i > 0 && !DTMD_ISTEXT(f->ed->f[i]->dtmd); i--)
  ;
 if (i <= 0)
  return(0);
 e_switch_window(f->ed->edt[i], f);
 f = f->ed->f[f->ed->mxedt];
 f->b->b = f->s->mark_end;
 e_schirm(f, 1);
 return(0);
}

/*   mark text line in block   */
int e_blck_mrk_all(FENSTER *f)
{
 int i;

 for (i = f->ed->mxedt; i > 0 && !DTMD_ISTEXT(f->ed->f[i]->dtmd); i--)
  ;
 if (i <= 0)
  return(0);
 e_switch_window(f->ed->edt[i], f);
 f = f->ed->f[f->ed->mxedt];
 f->s->mark_begin.x = 0;
 f->s->mark_begin.y = 0;
 f->s->mark_end.y = f->b->mxlines-1;
 f->s->mark_end.x = f->b->bf[f->b->mxlines-1].len;
 e_schirm(f, 1);
 return(0);
}

/*   mark text line in block   */
int e_blck_mrk_line(FENSTER *f)
{
 int i;

 for (i = f->ed->mxedt; i > 0 && !DTMD_ISTEXT(f->ed->f[i]->dtmd); i--)
  ;
 if (i <= 0)
  return(0);
 e_switch_window(f->ed->edt[i], f);
 f = f->ed->f[f->ed->mxedt];
 f->s->mark_begin.x = 0;
 f->s->mark_begin.y = f->b->b.y;
 if(f->b->b.y < f->b->mxlines-1)
 {
  f->s->mark_end.x = 0;
  f->s->mark_end.y = f->b->b.y+1;
 }
 else
 {
  f->s->mark_end.x = f->b->bf[f->b->b.y].len;
  f->s->mark_end.y = f->b->b.y;
 }
 e_schirm(f, 1);
 return(0);
}

/*	block: up- or downcase
mode=0 every character downcase
mode=1 every character upcase
mode=2 first character in each word upcase
mode=3 first character in each line upcase   */
/**
 * e_case_of - The character at column @x of line @s after a case change.
 * @mode: E_CASE_DOWN, E_CASE_UP, E_CASE_WORDS or E_CASE_LINE.
 */
static unsigned char e_case_of(const unsigned char *s, int x, int mode)
{
 if (mode == E_CASE_DOWN)
  return((unsigned char) tolower(s[x]));
 if (mode == E_CASE_UP)
  return((unsigned char) toupper(s[x]));
 if (mode == E_CASE_WORDS && (x == 0 || s[x-1] == ' ' || s[x-1] == '\t'))
  return((unsigned char) toupper(s[x]));
 if (mode == E_CASE_LINE && x == 0)
  return((unsigned char) toupper(s[x]));
 return(s[x]);
}

/**
 * e_case_line_changed - A copy of row @y of @b with the part the block
 * covers changed to case @mode.
 * @screen: the window's screen (its block); @out: receives the copy.
 * Return: E_BUFFER_OK or E_BUFFER_NOMEM.
 */
static int e_case_line_changed(BUFFER *b, SCHIRM *screen, int y, int mode, STRING *out)
{
 int x, x_begin = 0, x_end = b->bf[y].len, n = b->bf[y].nrc;

 if (e_buffer_line_new(b, out) != E_BUFFER_OK)
  return(E_BUFFER_NOMEM);
 if (n > b->mx.x)
  n = b->mx.x;
 memcpy(out->s, b->bf[y].s, (size_t)n);
 out->s[n] = '\0';
 out->len = e_str_len(out->s);
 out->nrc = e_str_nrc(out->s);
 if (y == screen->mark_begin.y)
  x_begin = screen->mark_begin.x;
 if (y == screen->mark_end.y)
  x_end = screen->mark_end.x;
 if (x_end > out->len)
  x_end = out->len;
 for (x = x_begin; x < x_end; x++)
  out->s[x] = e_case_of(out->s, x, mode);
 return(E_BUFFER_OK);
}

/**
 * e_case_lines_changed - Copies of the block's rows with the case changed.
 * Return: the @n copies, or NULL when memory ran out.
 */
static STRING *e_case_lines_changed(BUFFER *b, SCHIRM *screen, int n, int mode)
{
 STRING *lines = MALLOC(n * sizeof(STRING));
 int i;

 if (lines == NULL)
  return(NULL);
 for (i = 0; i < n; i++)
  if (e_case_line_changed(b, screen, screen->mark_begin.y + i, mode, &lines[i]) != E_BUFFER_OK)
  {
   e_buffer_lines_free(lines, i);
   return(NULL);
  }
 return(lines);
}

/*	block: up- or downcase (mode: E_CASE_DOWN, E_CASE_UP, E_CASE_WORDS,
	E_CASE_LINE).  The rows change in place, so their breakpoints and the
	block stay; one Ctrl-U gives the old text back. */
int e_blck_changecase(FENSTER *f, int mode)
{
 BUFFER *b;
 SCHIRM *screen;
 STRING *lines;
 int i, n;

 for (i = f->ed->mxedt; i > 0 && !DTMD_ISTEXT(f->ed->f[i]->dtmd); i--)
  ;
 if (i <= 0) return(0);
 e_switch_window(f->ed->edt[i], f);
 f = f->ed->f[f->ed->mxedt];
 b = f->b;
 screen = f->s;
 n = screen->mark_end.y - screen->mark_begin.y + 1;
 if (screen->mark_begin.y < 0 || n <= 0 || screen->mark_end.y >= b->mxlines)
  return(0);
 if ((lines = e_case_lines_changed(b, screen, n, mode)) == NULL)
 {
  wpe_buffer_say_nomem(b, "the case change");
  return(0);
 }
 if (e_buffer_splice(b, screen->mark_begin.y, n, lines, n, NULL, 0,
                     E_BUFFER_STEP_NEW) != E_BUFFER_OK)
 {
  e_buffer_lines_free(lines, n);
  wpe_buffer_say_nomem(b, "the case change");
  return(0);
 }
 FREE(lines);
 e_schirm(f, 1);
 return 0;
}

int e_changecase_dialog(FENSTER *f)
{
 static int b_sw = 0;
 int ret;
 W_OPTSTR *o = e_init_opt_kst(f);
 if (!o) return(-1);
 o->xa = 21;
 o->xe = 52;
 o->ya = 4;
 o->ye = 15;

 o->bgsw = AltO;
 o->name = "Change Case";
 o->crsw = AltO;
 e_add_txtstr(4, 2, "Which mode:", o);

 e_add_pswstr(1, 5, 3, 0, AltD, 0, "Downcase          ", o);
 e_add_pswstr(0, 5, 4, 0, AltU, b_sw & 1, "Upcase            ", o);
 e_add_pswstr(0, 5, 5, 0, AltH, b_sw & 2, "Headline          ", o);
 e_add_pswstr(0, 5, 6, 0, AltE, b_sw & 3, "First character   ", o);

 e_add_bttstr(7, 9, 1, AltO, " Ok ", NULL, o);
 e_add_bttstr(18, 9, -1, WPE_ESC, "Cancel", NULL, o);
 ret = e_opt_kst(o);

 if (ret != WPE_ESC)
  e_blck_changecase(f,o->pstr[0]->num);
 freeostr(o);
 return 0;
}

/*   unindent block   */
int e_blck_to_left(FENSTER *f)
{
 BUFFER *b;
 SCHIRM *s;
 int n = f->ed->tabn/2, i, j, k, l, m, nn;
 char *tstr = MALLOC((n+2)*sizeof(char));

 for(i = f->ed->mxedt; i > 0 && !DTMD_ISTEXT(f->ed->f[i]->dtmd); i--);
 if(i <= 0) return(0);
 e_switch_window(f->ed->edt[i], f);
 f = f->ed->f[f->ed->mxedt];
 b = f->b;
 s = f->s;
 for(i = 0; i <= n; i++) tstr[i] = ' ';
 tstr[n] = '\0';
 for (i = (!s->mark_begin.x) ? s->mark_begin.y : s->mark_begin.y + 1;
   i < s->mark_end.y || (i == s->mark_end.y && s->mark_end.x > 0); i++)
 {
  for(j = 0; j < b->bf[i].len && isspace(b->bf[i].s[j]); j++);
  for(l = j - 1, k = 0; l >= 0 && k < n; l--)
  {
   if(b->bf[i].s[l] == ' ') k++;
   else if(b->bf[i].s[l] == '\t')
   {
    for(nn = m = 0; m < l; m++)
    {
     if(b->bf[i].s[m] == ' ') nn++;
     else if(b->bf[i].s[m] == '\t')
     nn += f->ed->tabn - (nn % f->ed->tabn);
    }
    k += f->ed->tabn - (nn % f->ed->tabn);
   }
  }
  l = j - l - 1;
  if(l > 0)
  {
   nn = s->mark_begin.x;
   e_del_nchar(b, s, j - l, i, l);
   if (k > n) e_ins_nchar(b, s, tstr, j-l, i, k-n);
   s->mark_begin.x = nn;
  }
 }
 FREE(tstr);
 e_schirm(f, 1);
 return(0);
}

/*   indent block   */
int e_blck_to_right(FENSTER *f)
{
 BUFFER *b;
 SCHIRM *s;
 int n = f->ed->tabn/2, i, j;
 char *tstr = MALLOC((n+1)*sizeof(char));

 for (i = f->ed->mxedt; i > 0 && !DTMD_ISTEXT(f->ed->f[i]->dtmd); i--)
  ;
 if (i <= 0)
  return(0);
 e_switch_window(f->ed->edt[i], f);
 f = f->ed->f[f->ed->mxedt];
 b = f->b;
 s = f->s;
 for (i = 0; i < n; i++)
  tstr[i] = ' ';
 tstr[n] = '\0';
 for (i = (!s->mark_begin.x) ? s->mark_begin.y : s->mark_begin.y + 1;
   i < s->mark_end.y || (i == s->mark_end.y && s->mark_end.x > 0); i++)
 {
  for (j = 0; b->bf[i].len && isspace(b->bf[i].s[j]); j++)
   ;
  e_ins_nchar(b, s, tstr, j, i, n);
  if (i == s->mark_begin.y)
   s->mark_begin.x = 0;
 }
 FREE(tstr);
 e_schirm(f, 1);
 return(0);
}

/*            Read block from file   */
int e_blck_read(FENSTER *f)
{
 if (e_window_text_locked(f))
  return(WPE_ESC);
 WpeCreateFileManager(1, f->ed, "");
 f->save = f->ed->maxchg + 1;
 return(0);
}

/*   write block to file   */
int e_blck_write(FENSTER *f)
{
 WpeCreateFileManager(2, f->ed, "");
 return(0);
}

/*   find again   */
int e_rep_search(FENSTER *f)
{
 SCHIRM *s;
 BUFFER *b;
 FIND *fd = &(f->ed->fd);
 int i, ret, j, iend, jend, end;

 for (i = f->ed->mxedt; i > 0 && !DTMD_ISTEXT(f->ed->f[i]->dtmd); i--)
  ;
 if (i <= 0)
  return(0);
 e_switch_window(f->ed->edt[i], f);
 f = f->ed->f[f->ed->mxedt];
 b = f->b;  s = f->s;
 ret = b->b.x;
 j = b->b.y;
 if ((fd->sw & 8) == 0)
 {
  jend = (fd->sw & 4) ? 0 : b->mxlines-1;
  iend = (fd->sw & 4) ? 0 : b->bf[b->mxlines-1].len;
 }
 else
 {
  jend = (fd->sw & 4) ? s->mark_begin.y : s->mark_end.y;
  iend = (fd->sw & 4) ? s->mark_begin.x : s->mark_end.x;
 }

 for (; ; ret = -1)
 {
  if ((fd->sw & 4) == 0 && j > jend)
   break;
  else if ((fd->sw & 4) != 0 && j < jend)
   break;
  do
  {
   if (j == jend)
    end = iend;
   else if (!(fd->sw & 4))
    end = b->bf[j].len;
   else
    end = 0;
   if (fd->sw & 4 && ret == -1)
    ret = b->bf[j].len;
   if ((fd->sw & 32) == 0)
   {
    if ((fd->sw & 128) != 0)
     ret = e_strstr(ret, end, b->bf[j].s, fd->search);
    else
     ret = e_ustrstr(ret, end, b->bf[j].s, fd->search);
   }
   else
   {
    if ((fd->sw & 128) != 0)
     ret = e_rstrstr(ret, end, b->bf[j].s, fd->search, &(fd->sn));
    else
     ret = e_urstrstr(ret, end, b->bf[j].s, fd->search, &(fd->sn));
   }
   if ((fd->sw & 4) == 0 && j == jend && ret > iend)
    break;
   else if ((fd->sw & 4) != 0 && j == jend && ret < iend)
    break;
   if (ret >= 0 && ((fd->sw & 64) == 0 ||
     (isalnum(*(b->bf[j].s+ret+fd->sn)) == 0 &&
     (ret == 0 || isalnum(*(b->bf[j].s+ret-1)) == 0))))
   {
    s->fa.x = ret;
    b->b.y = s->fa.y = s->fe.y = j;
    s->fe.x = ret + fd->sn;
    b->b.x = ret = !(fd->sw & 4) ? s->fe.x : ret;
    if (!(fd->sw & 1))
    {
     e_cursor(f, 1);
     s->fa.y = j;
     e_schirm(f, 1);
    }
    return(1);
   }
   else if (ret >= 0 && (fd->sw & 64) != 0)
    ret++;
  } while(ret >= 0);
  if ((fd->sw & 4) == 0 && j > jend)
   break;
  else if ((fd->sw & 4) != 0 && j < jend)
   break;
  j = ((fd->sw & 4) == 0) ? j+1 : j-1;
 }
 if (!(fd->sw & 1))
  e_message(0, e_msg[ERR_GETSTRING], f);
 return(0);
}

/*   goto line  */
int e_goto_line(FENSTER *f)
{
 int i, num;
 SCHIRM *s;
 BUFFER *b;

 for (i = f->ed->mxedt; i > 0 && !DTMD_ISTEXT(f->ed->f[i]->dtmd); i--)
  ;
 if (i <= 0)
  return(0);
 e_switch_window(f->ed->edt[i], f);
 f = f->ed->f[f->ed->mxedt];
 b = f->b;  s = f->s;
 if ((num = e_num_kst("Goto Line Number", b->b.y+1, b->mxlines, f, 0, AltG)) > 0)
  b->b.y = num - 1;
 else if (num == 0)
  b->b.y = num;
 e_cursor(f, 1);
 return(0);
}

int e_find(FENSTER *f)
{
 SCHIRM *s;
 BUFFER *b;
 FIND *fd = &(f->ed->fd);
 int i, ret;
 char strTemp[80];
 W_OPTSTR *o = e_init_opt_kst(f);

 if (!o)
  return(-1);
 for (i = f->ed->mxedt; i > 0 && !DTMD_ISTEXT(f->ed->f[i]->dtmd); i--)
  ;
 if (i <= 0)
  return(0);
 e_switch_window(f->ed->edt[i], f);
 f = f->ed->f[f->ed->mxedt];
 b = f->b;  s = f->s;
 if (e_blck_dup(strTemp, f))
 {
  strcpy(fd->search, strTemp);
  fd->sn = strlen(fd->search);
 }
 o->xa = 7;  o->ya = 3;  o->xe = 63;  o->ye = 18;
 o->bgsw = 0;
 o->crsw = AltO;
 o->name = "Find";
 e_add_txtstr(32, 4, "Direction:", o);
 e_add_txtstr(4, 4, "Options:", o);
 e_add_txtstr(4, 9, "Scope:", o);
 e_add_txtstr(32, 9, "Begin:", o);
 e_add_wrstr(4, 2, 18, 2, 35, 128, 0, AltT, "Text to Find:", fd->search, &f->ed->sdf, o);
 e_add_sswstr(5, 5, 0, AltC, fd->sw & 128 ? 1 : 0, "Case sensitive    ", o);
 e_add_sswstr(5, 6, 0, AltW, fd->sw & 64 ? 1 : 0, "Whole words only  ", o);
 e_add_sswstr(5, 7, 0, AltR, fd->sw & 32 ? 1 : 0, "Regular expression", o);
 e_add_pswstr(0, 33, 5, 6, AltD, 0, "ForwarD        ", o);
 e_add_pswstr(0, 33, 6, 0, AltB, fd->sw & 4 ? 1 : 0, "Backward       ", o);
 e_add_pswstr(1, 5, 10, 0, AltG, 0, "Global            ", o);
 e_add_pswstr(1, 5, 11, 0, AltS, fd->sw & 8 ? 1 : 0, "Selected Text     ", o);
 e_add_pswstr(2, 33, 10, 0, AltF, 0, "From Cursor    ", o);
 e_add_pswstr(2, 33, 11, 0, AltE, fd->sw & 2 ? 1 : 0, "Entire Scope   ", o);
 e_add_bttstr(16, 13, 1, AltO, " Ok ", NULL, o);
 e_add_bttstr(34, 13, -1, WPE_ESC, "Cancel", NULL, o);
 ret = e_opt_kst(o);
 if (ret != WPE_ESC)
 {
  fd->sw = (o->pstr[0]->num << 2) + (o->pstr[1]->num << 3) +
    (o->pstr[2]->num << 1) + (o->sstr[0]->num << 7) + (o->sstr[1]->num << 6) +
    (o->sstr[2]->num << 5);
  strcpy(fd->search, o->wstr[0]->txt);
  fd->sn = strlen(fd->search);
  if ((fd->sw & 2) != 0)
  {
   if ((fd->sw & 4) == 0)
   {
    b->b.x = (fd->sw & 8) == 0 ? 0 : s->mark_begin.x;
    b->b.y = (fd->sw & 8) == 0 ? 0 : s->mark_begin.y;
   }
   else
   {
    b->b.x = (fd->sw & 8) == 0 ? b->bf[b->mxlines-1].len : s->mark_end.x;
    b->b.y = (fd->sw & 8) == 0 ? b->mxlines-1 : s->mark_end.y;
   }
  }
 }
 freeostr(o);
 if (ret != WPE_ESC)
  e_rep_search(f);
 return(0);
}

/* The menu and menuhandling have been rewritten to support only 4 choices
  rather than 3 sets of 2 alternatives.
    1 Forward from Cursor
    2 Back from Cursor
    3 Global replace (forward from start of text)
    4 Selected Text (from start of selection, and only shown if a block _is_
      marked)
  The result window is either 'String NOT Found' or the number of
  occurrences found and the number actually replaced.
*/
int e_replace(FENSTER *f)
{
 SCHIRM *s;
 BUFFER *b;
 FIND *fd = &(f->ed->fd);
 int i, ret, c, ym, rep=0, found=0;
 char strTemp[80];
 W_OPTSTR *o = e_init_opt_kst(f);

 if (!o)
  return(-1);
 for (i = f->ed->mxedt; i > 0 && !DTMD_ISTEXT(f->ed->f[i]->dtmd); i--)
  ;
 if (i <= 0)
  return(0);
 e_switch_window(f->ed->edt[i], f);
 f = f->ed->f[f->ed->mxedt];
 b = f->b;  s = f->s;
 if (e_blck_dup(strTemp, f))
 {
  strcpy(fd->search, strTemp);
  fd->sn = strlen(fd->search);
 }
 o->xa = 7;  o->ya = 3;  o->xe = 67;  o->ye = 17;
 o->bgsw = 0;
 o->name = "Replace";
 o->crsw = AltO;
 e_add_txtstr(4, 6, "Options:", o);
 e_add_txtstr(32, 6, "Scope:", o);
 e_add_wrstr(4, 2, 18, 2, 35, 128, 0, AltT, "Text to Find:", fd->search, &f->ed->sdf, o);
 e_add_wrstr(4, 4, 18, 4, 35, 128, 0, AltN, "New Text:", fd->replace, &f->ed->rdf, o);
 e_add_sswstr(5, 7, 0, AltC, fd->sw & 128 ? 1 : 0, "Case sensitive    ", o);
 e_add_sswstr(5, 8, 0, AltW, fd->sw & 64 ? 1 : 0, "Whole words only  ", o);
 e_add_sswstr(5, 9, 0, AltR, fd->sw & 32 ? 1 : 0, "Regular expression", o);
 e_add_sswstr(5, 10, 0, AltP, 1, "Prompt on Replace ", o);
 e_add_pswstr(0, 33, 7, 0, AltF, 0, "Forward from Cursor", o);
 e_add_pswstr(0, 33, 8, 0, AltB, fd->sw & 4 ? 1 : 0, "Back from Cursor   ", o);
 e_add_pswstr(0, 33, 9, 0, AltG, fd->sw & 2 ? 1 : 0, "Global Replace     ", o);
 if (s->mark_end.y)
  e_add_pswstr(0, 33, 10, 0, AltS, fd->sw & 10 ? 1 : 0, "Selected Text      ", o);
 e_add_bttstr(10, 12, 1, AltO, " Ok ", NULL, o);
 e_add_bttstr(41, 12, -1, WPE_ESC, "Cancel", NULL, o);
 e_add_bttstr(22, 12, 7, AltA, "Change All", NULL, o);
 ret = e_opt_kst(o);
 if (ret != WPE_ESC)
 {
  strcpy(fd->search, o->wstr[0]->txt);
  fd->sn = strlen(fd->search);
  strcpy(fd->replace, o->wstr[1]->txt);
  fd->rn = strlen(fd->replace);
  fd->sw = 1 + (o->sstr[0]->num << 7)+ (o->sstr[1]->num << 6)
    + (o->sstr[2]->num << 5) + (o->sstr[3]->num << 4);
  switch (o->pstr[0]->num)
  {
   case 2:
    fd->sw |= 2;
    b->b.x = b->b.y = 0;
    break;
   case 1:
    fd->sw |= 4;
    b->b.x = b->bf[b->mxlines-1].len;
    b->b.y = b->mxlines-1;
    break;
   case 3:
    fd->sw |= 10;
    b->b.x = s->mark_begin.x;
    b->b.y = s->mark_begin.y;
   default:
    break;
  }
 }
 freeostr(o);
 if (ret != WPE_ESC)
 {
  while (e_rep_search(f) && ((ret == AltA) || (!found)))
  {
   found++;
   if (f->a.y < 11)
   {
    s->c.y = b->b.y - 1; ym = 15;
   }
   else
   {
    ym = 1;
   }
   e_schirm(f, 1);
   c = 'Y';
   if (fd->sw & 16)
    c = e_message(1, "String found:\nReplace this occurrence ?",f);
   if (c == WPE_ESC)
    break;
   if (c == 'Y')
   {
    rep++;
    e_replace_occurrence(b, s, s->fa.x, b->b.y, fd->sn,
                         (unsigned char *)fd->replace, fd->rn);
    s->fe.x = s->fa.x + fd->rn;
    b->b.x = !(fd->sw & 4) ? s->fe.x : s->fa.x;
    e_schirm(f, 1);
   }
  }
  if (found)
  {
   sprintf(strTemp, "Found %d\nReplaced %d", found, rep);
   e_message(0, strTemp, f);
  }
  else
   e_message(0, e_msg[ERR_GETSTRING], f);
 }
 return(0);
}

