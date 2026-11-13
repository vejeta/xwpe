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

/* What a block move or copy does to the lines, which decides where the
   block is marked afterwards. */
#define E_BLK_SHAPE_LINES        0   /* a block of several lines lands at the cursor */
#define E_BLK_SHAPE_CHARS        1   /* a block inside one line lands at the cursor */
#define E_BLK_SHAPE_REORDER      2   /* ... at another place of its own line */
#define E_BLK_SHAPE_SHIFT_LEFT   3   /* the text before the block goes after it */
#define E_BLK_SHAPE_SHIFT_RIGHT  4   /* the text after the block goes before it */

/* A splice of the operation, as it meets its buffer when it commits. */
typedef struct
{
 BUFFER *b;
 int y, n_del, n_ins;
} WpeBlockSplice;

/* A block move or copy being prepared: the change that makes it whole or
   not at all, its splices so far (each moves the rows below it, so the next
   is placed by where its line stands then), and where the block lands. */
typedef struct
{
 WpeBufferTxn t;
 WpeBlockSplice done[E_BUFFER_TXN_SPLICES];
 int ndone;
 WpeBlock k;            /* the block, where it is now */
 BUFFER *bv, *bz;       /* the buffer it leaves (or is copied from), the one it reaches */
 int x, y;              /* the cursor in bz: its column on the line, its line */
 int shape;             /* E_BLK_SHAPE_* */
 int landed;            /* the row the block's start lands on ... */
 int landed_after;      /* ... as of this many splices */
 int mark_x;            /* the column the landed block starts at */
 int n;                 /* a block inside one line: its width */
 int wrapped;           /* the lines the shifted line became */
 Undo *rec;             /* the undo record taken with the operation, or NULL */
 int left_row;          /* a move: the row the block's old start stands on now */
 int end_row;           /* the row the line holding the block's end starts on ... */
 int end_after;         /* ... as of this many splices */
} WpeBlockOp;

/* A move whose old place is not given (a shift keeps its own rule). */
#define E_BLK_NO_ROW (-1)

static int e_blk_move_covered(int x, int y, BUFFER *bv, BUFFER *bz, FENSTER *f, int kind,
                              Undo **rec, int *left_row);
static void e_blk_end_at_line_front(WpeBlockOp *op);

static int e_copy_block_covered(int x, int y, BUFFER *bs, BUFFER *bd, FENSTER *f, int kind,
                                Undo **rec);

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
 * e_block_record_keep - Keep the record of a block copy ('c') or move ('v')
 * for Ctrl-U.
 * @rec: the record the operation reserved before it moved anything; @b:
 * the buffer; @x, @y: the cursor the record replays from.
 *
 * The record keeps the block as the window marks it now, so Undo finds the
 * copied or moved lines where they landed.  It was taken with the rest of
 * the operation's memory, so an operation that is made can always be
 * undone.
 */
void e_block_record_keep(Undo *rec, BUFFER *b, int x, int y)
{
 if (rec == NULL)
  return;
 rec->b.x = x;
 rec->b.y = y;
 rec->a = b->f->s->mark_begin;
 rec->e = b->f->s->mark_end;
 e_buffer_record_push(b, rec);
}

/**
 * e_blk_text_window - The text window on top, brought to the front: the
 * window a block command acts on.
 * @f: the window the command came from.
 * Return: the window, or NULL when no text window is open.
 */
static FENSTER *e_blk_text_window(FENSTER *f)
{
 int i;

 for (i = f->ed->mxedt; i > 0 && !DTMD_ISTEXT(f->ed->f[i]->dtmd); i--)
  ;
 if (i <= 0)
  return(NULL);
 e_switch_window(f->ed->edt[i], f);
 return(f->ed->f[f->ed->mxedt]);
}

/**
 * e_blk_shell_new - The private buffer a block delete moves its lines into.
 * @b: the buffer the block leaves.
 *
 * The shell keeps breakpoint rows of its own, so the breakpoints of the
 * deleted lines wait in it until Undo brings the lines back; it has no file
 * name, so no debugger is ever told of it, and every splice into it runs
 * covered, so it records nothing.  It is as wide as @b, not as Max. Columns
 * is now: Undo gives its lines back to @b, and a line narrower than @b's
 * width would overflow at the first character typed into it.
 * Return: the shell, or NULL when memory ran out.
 */
static BUFFER *e_blk_shell_new(BUFFER *b)
{
 BUFFER *bn = e_buffer_shell_new(b, b->mx.x);

 if (bn == NULL)
  return(NULL);
 e_line_make_hard_empty(&bn->bf[0]);
 return(bn);
}

/**
 * e_blk_delete_into_shell_now - Delete the marked block of window @f by moving
 * its lines into a shell its undo record keeps.
 *
 * One Ctrl-U moves the lines back, with their breakpoints, and marks them
 * as the block again.  The record and the shell are taken before anything
 * moves, so a delete that could not be undone is not made.
 * Return: E_BLOCK_MOVED, E_BLOCK_NOT_MOVED, or E_BLOCK_NOMEM with the text
 * as it was.
 */
static int e_blk_delete_into_shell_now(FENSTER *f)
{
 BUFFER *b = f->b;
 BUFFER *bn;
 Undo *rec = e_buffer_record_new('d');
 int r;

 if (rec == NULL)
  return(E_BLOCK_NOMEM);
 if ((bn = e_blk_shell_new(b)) == NULL)
 {
  e_buffer_record_free(rec);
  return(E_BLOCK_NOMEM);
 }
 rec->b = f->s->mark_begin;
 rec->u.pt = bn;
 r = e_move_block_covered(0, 0, b, bn, f, 0, NULL);
 if (r != E_BLOCK_MOVED)
 {
  e_buffer_record_free(rec);
  return(r);
 }
 e_buffer_record_push(b, rec);
 return(E_BLOCK_MOVED);
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

/**
 * e_blk_marks_empty - Does the window mark no block?
 * Return: 1 when its block holds no character.
 */
static int e_blk_marks_empty(SCHIRM *s)
{
 WpeBlock k;

 e_blk_of(s, &k);
 return(e_blk_is_empty(&k));
}

/**
 * e_blk_delete_in_line_now - Delete a block inside one line: its characters
 * go, the cursor stays where the block began.
 *
 * The deletion and its Undo record are made together by the buffer owner or
 * not at all, so a delete that could not be undone is not made.
 * Return: E_BLOCK_MOVED, or E_BLOCK_NOMEM with the text as it was.
 */
static int e_blk_delete_in_line_now(BUFFER *b, SCHIRM *s)
{
 Undo as_if;
 int row = b->b.y, r;

 memset(&as_if, 0, sizeof(as_if));
 as_if.type = 'a';
 as_if.b = s->mark_begin;
 as_if.a.x = s->mark_end.x - s->mark_begin.x;
 r = e_buffer_apply_chars(b, s, &as_if);
 if (r == E_BUFFER_NOMEM)
  return(E_BLOCK_NOMEM);
 if (r != E_BUFFER_OK)
  return(E_BLOCK_NOT_MOVED);
 b->b.y = row;
 b->b.x = s->mark_end.x = s->mark_begin.x;
 return(E_BLOCK_MOVED);
}

/**
 * e_blk_delete_in_line - A delete inside one line, as one action a test can
 * starve of memory.
 * Return: as e_blk_delete_in_line_now.
 */
static int e_blk_delete_in_line(BUFFER *b, SCHIRM *s)
{
 int r, fault;

 fault = e_buffer_test_fault_enter(E_BUFFER_FAULT_BLOCK_MOVE);
 r = e_blk_delete_in_line_now(b, s);
 e_buffer_test_fault_leave(fault);
 return(r);
}

/**
 * e_block_delete_marked - Delete the block window @f marks.
 *
 * Block > Delete, Edit > Cut, and the Undo of a block copy all delete this
 * way.  A block over several lines goes into its undo record whole, or,
 * when memory runs out, stays where it was with the window as it was.
 * Return: E_BLOCK_MOVED, E_BLOCK_NOT_MOVED (no block, or the window is
 * closed to changes) or E_BLOCK_NOMEM; it says nothing itself.
 */
int e_block_delete_marked(FENSTER *f)
{
 BUFFER *b = f->b;
 SCHIRM *s = f->s;
 int r = E_BLOCK_MOVED, in_line;

 if (e_blk_marks_empty(s) || e_window_text_locked(f))
  return(E_BLOCK_NOT_MOVED);
 in_line = (s->mark_begin.y == s->mark_end.y);
 if (in_line)
  r = e_blk_delete_in_line(b, s);
 else
  r = e_blk_delete_into_shell(f);
 if (r == E_BLOCK_MOVED && !in_line)
  f->save = b->cn->maxchg + 1;
 sc_txt_1(f);
 e_cursor(f, 1);
 e_schirm(f, 1);
 return(r);
}

/*	delete block */
int e_blck_del(FENSTER *f)
{
 if ((f = e_blk_text_window(f)) == NULL || e_blk_marks_empty(f->s))
  return(0);
 if (e_window_text_locked(f))
  return(WPE_ESC);
 if (e_block_delete_marked(f) == E_BLOCK_NOMEM)
  wpe_buffer_say_nomem(f->b, "the block deletion");
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
 if (e_clip_clear(b0) != 0 ||
     e_copy_block_covered(0, 0, b, b0, f, 0, NULL) == E_BLOCK_NOMEM)
 {
  wpe_buffer_say_nomem(b0, "the copy");
  return(0);
 }
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
 Undo *rec = NULL;
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
 i = e_copy_block_covered(b->b.x, b->b.y, b0, b, f, 'c', &rec);
 if (i == E_BLOCK_NOMEM)
  wpe_buffer_say_nomem(b, "the paste");
 if (i != E_BLOCK_MOVED)
  return(0);
 e_block_record_keep(rec, b, b->b.x, b->b.y);
 sc_txt_2(f);
 f->save = b->cn->maxchg + 1;
 return(0);
}

/**
 * e_blk_move_undo_point - Where the Undo of a block move puts the cursor to
 * move the block back: the block's old start, seen from where it lands.
 * @b, @s: the buffer and the window's screen before the move.
 */
static POINT e_blk_move_undo_point(BUFFER *b, SCHIRM *s)
{
 POINT ka;

 ka.x = s->mark_begin.x;
 if (b->b.y > s->mark_begin.y)
 {
  ka.y = s->mark_begin.y;
  return(ka);
 }
 ka.y = s->mark_end.y;
 if (b->b.y != s->mark_begin.y || b->b.x >= s->mark_begin.x)
  return(ka);
 if (s->mark_begin.y == s->mark_end.y)
  ka.x = s->mark_end.x;          /* inside one line: back to where it ended */
 else
  ka.x = s->mark_end.x + s->mark_begin.x - b->b.x;
 return(ka);
}

/**
 * e_block_move_marked - Move the block window @f marks to its cursor.
 *
 * Block > Move and the Undo and Redo of a move all move this way.  The move
 * and its undo record are taken whole, or, when memory runs out, not at
 * all: the text, the block, the breakpoints and Undo are as they were.
 * Return: E_BLOCK_MOVED, E_BLOCK_NOT_MOVED or E_BLOCK_NOMEM; it says
 * nothing itself.
 */
int e_block_move_marked(FENSTER *f)
{
 BUFFER *b = f->b;
 POINT ka = e_blk_move_undo_point(b, f->s);
 Undo *rec = NULL;
 int r, left_row;

 r = e_blk_move_covered(b->b.x, b->b.y, b, b, f, 'v', &rec, &left_row);
 if (r != E_BLOCK_MOVED)
  return(r);
 if (left_row != E_BLK_NO_ROW)
  ka.y = left_row;               /* rows the move added or wrapped above it count */
 e_block_record_keep(rec, b, ka.x, ka.y);
 f->save = b->cn->maxchg + 1;
 return(E_BLOCK_MOVED);
}

/*   move block within window */
int e_blck_move(FENSTER *f)
{
 if ((f = e_blk_text_window(f)) == NULL)
  return(0);
 if (e_block_move_marked(f) == E_BLOCK_NOMEM)
  wpe_buffer_say_nomem(f->b, "the block move");
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

/* ---- a block operation as one change: prepared whole, then made ---- */

/**
 * e_blk_op_begin - Start preparing a block move or copy.
 * @op: on the caller's stack; @s: the screen whose marks are the block;
 * @bv: the buffer the block leaves or is copied from; @bz: the buffer it
 * reaches; @kind: the undo record to reserve with it ('c', 'v'), or 0.
 */
static void e_blk_op_begin(WpeBlockOp *op, SCHIRM *s, BUFFER *bv, BUFFER *bz, int kind)
{
 memset(op, 0, sizeof(*op));
 e_buffer_txn_begin(&op->t);
 e_blk_of(s, &op->k);
 op->bv = bv;
 op->bz = bz;
 if (kind != 0)
  op->rec = e_buffer_txn_record(&op->t, kind);
}

/**
 * e_blk_op_state - How the preparation stands: every splice prepared, or
 * the operation refused.
 * Return: E_BLOCK_MOVED (ready to make), E_BLOCK_NOMEM, or
 * E_BLOCK_NOT_MOVED for a splice that did not fit the text.
 */
static int e_blk_op_state(const WpeBlockOp *op)
{
 if (op->t.refused == E_BUFFER_NOMEM)
  return(E_BLOCK_NOMEM);
 if (op->t.refused != E_BUFFER_OK)
  return(E_BLOCK_NOT_MOVED);
 return(E_BLOCK_MOVED);
}

/**
 * e_blk_op_make - Make a prepared operation, or give back what a refused
 * one took.
 * @prepared: what the preparation returned.
 * Return: E_BLOCK_MOVED when the text changed; otherwise the refusal, with
 * both buffers exactly as they were.
 */
static int e_blk_op_make(WpeBlockOp *op, int prepared)
{
 int r;

 if (prepared != E_BLOCK_MOVED)
 {
  e_buffer_txn_abort(&op->t);
  return(prepared);
 }
 r = e_buffer_txn_commit(&op->t);
 if (r == E_BUFFER_NOMEM)
  return(E_BLOCK_NOMEM);
 if (r != E_BUFFER_OK)
  return(E_BLOCK_NOT_MOVED);
 return(E_BLOCK_MOVED);
}

/**
 * e_blk_note - Remember a splice of the operation: the ones after it are
 * placed by where their lines stand once it has committed.
 */
static void e_blk_note(WpeBlockOp *op, BUFFER *b, int y, int n_del, int n_ins)
{
 WpeBlockSplice *d;

 if (op->ndone >= E_BUFFER_TXN_SPLICES)
  return;
 d = &op->done[op->ndone++];
 d->b = b;
 d->y = y;
 d->n_del = n_del;
 d->n_ins = n_ins;
}

/**
 * e_blk_row_since - Where a row of @b stands once the splices noted from
 * the @from-th on have committed.
 * @row: the row as it stood before them.
 */
static int e_blk_row_since(const WpeBlockOp *op, BUFFER *b, int row, int from)
{
 const WpeBlockSplice *d;
 int i;

 for (i = from; i < op->ndone; i++)
 {
  d = &op->done[i];
  if (d->b == b && row >= d->y + d->n_del)
   row += d->n_ins - d->n_del;
 }
 return(row);
}

/**
 * e_blk_row_now - Where line @row of @b, as the user sees it now, stands
 * when the next splice commits.  Lines put in at exactly its place go
 * above it.
 */
static int e_blk_row_now(const WpeBlockOp *op, BUFFER *b, int row)
{
 return(e_blk_row_since(op, b, row, 0));
}

/**
 * e_blk_set_landed - The block's start lands on row @y as the splices
 * noted so far leave the buffer; later ones may still move it.
 */
static void e_blk_set_landed(WpeBlockOp *op, int y)
{
 op->landed = y;
 op->landed_after = op->ndone;
}

/**
 * e_blk_landed_row - The row the block's start stands on once every splice
 * has committed: where the block is marked after the operation.
 */
static int e_blk_landed_row(const WpeBlockOp *op)
{
 return(e_blk_row_since(op, op->bz, op->landed, op->landed_after));
}

/**
 * e_blk_set_end - The line that holds the block's end, once landed, starts
 * on row @y as the splices noted so far leave the buffer.
 */
static void e_blk_set_end(WpeBlockOp *op, int y)
{
 op->end_row = y;
 op->end_after = op->ndone;
}

/**
 * e_blk_end_row - The row the line holding the block's end starts on once
 * every splice has committed.
 */
static int e_blk_end_row(const WpeBlockOp *op)
{
 return(e_blk_row_since(op, op->bz, op->end_row, op->end_after));
}

/**
 * e_blk_set_end_at_line - The block's end lands in front of line @row of
 * the buffer it reaches (as the user sees it now), which no splice
 * rewrites: a block that ends at column 0.
 */
static void e_blk_set_end_at_line(WpeBlockOp *op, int row)
{
 e_blk_set_end(op, e_blk_row_now(op, op->bz, row));
}

/* The end of a line, its line break included, as the end of a run. */
#define WPE_BLK_TO_END (-1)
/* The soft-wrapped rows one rebuilt line takes in with it, at most, and the
   parts such a line is built from (three runs and those rows).  The parts
   live on the stack, so the count is bounded; 64 rows is a line of about
   7600 characters in a window of 120 columns, longer than any line typed or
   read.  A longer line keeps its rows past the 64th as they are: the last
   row taken in has no line break, so the rebuilt line goes on into them and
   the file written is the same. */
#define WPE_BLK_PARAGRAPH_MAX 64
#define WPE_BLK_PARTS_MAX (3 + WPE_BLK_PARAGRAPH_MAX)

/**
 * e_blk_part - A run of the bytes of line @y of @b, from column @from up to
 * @upto (WPE_BLK_TO_END: to the end, its line break included).
 *
 * Runs are read where the lines stand before anything moves, so a block
 * moved or copied onto one of its own lines carries exactly the text the
 * user marked.
 */
static WpeBufferPart e_blk_part(BUFFER *b, int y, int from, int upto)
{
 WpeBufferPart p;
 int end = e_blk_line_bytes(b, y);

 if (upto != WPE_BLK_TO_END && upto < end)
  end = upto;
 if (from > end)
  from = end;
 if (from < 0)
  from = 0;
 p.s = b->bf[y].s + from;
 p.n = end - from;
 return(p);
}

/**
 * e_blk_put_parts_at - Add a splice at row @y as it will stand: @n_del
 * lines go, the runs @parts joined come (soft-wrapped at the width).
 * Return: how many lines it puts in, or a negative E_BUFFER_* refusal.
 */
static int e_blk_put_parts_at(WpeBlockOp *op, BUFFER *b, int y, int n_del,
                              const WpeBufferPart *parts, int nparts)
{
 int k = e_buffer_txn_splice_parts(&op->t, b, y, n_del, parts, nparts,
                                   E_BUFFER_STEP_COVERED);

 if (k >= 0)
  e_blk_note(op, b, y, n_del, k);
 return(k);
}

/**
 * e_blk_put_parts - As e_blk_put_parts_at, at the place line @row (as the
 * user sees it now) will stand.
 * Return: the row it splices at.
 */
static int e_blk_put_parts(WpeBlockOp *op, BUFFER *b, int row, int n_del,
                           const WpeBufferPart *parts, int nparts)
{
 int y = e_blk_row_now(op, b, row);

 e_blk_put_parts_at(op, b, y, n_del, parts, nparts);
 return(y);
}

/**
 * e_blk_row_goes_on - Does row @r of @b continue the line of the row above
 * it (a soft-wrapped row), and may it be rewritten with that line?  Not
 * when it holds the block, or the place the block is dropped at.
 */
static int e_blk_row_goes_on(const WpeBlockOp *op, BUFFER *b, int r)
{
 if (r <= 0 || r >= b->mxlines || b->bf[r - 1].s[b->bf[r - 1].len] != '\0')
  return(0);
 if (b == op->bv && r >= op->k.kay && r <= op->k.key)
  return(0);
 return(!(b == op->bz && r == op->y));
}

/**
 * e_blk_take_paragraph - Add to @parts the rows that go on from row @row
 * (a soft-wrapped line), so the splice rewrites the whole line and wraps it
 * as the file reader would: Undo of a paste that made a full line wrap
 * gives that line back on its one row.
 * @nparts: how many parts there are; grows.
 * Return: how many rows were added (the splice takes them out too).
 */
static int e_blk_take_paragraph(const WpeBlockOp *op, BUFFER *b, int row,
                                WpeBufferPart *parts, int *nparts)
{
 int r = row + 1, taken = 0;

 while (taken < WPE_BLK_PARAGRAPH_MAX && e_blk_row_goes_on(op, b, r))
 {
  parts[(*nparts)++] = e_blk_part(b, r, 0, WPE_BLK_TO_END);
  r++;
  taken++;
 }
 return(taken);
}

/**
 * e_blk_put_line - As e_blk_put_parts, for a line whose last part runs to
 * the end of row @tail_row of @b: the rows its soft-wrapped line goes on to
 * are rewritten with it.
 * @parts: room for WPE_BLK_PARTS_MAX parts.
 * Return: the row it splices at.
 */
static int e_blk_put_line(WpeBlockOp *op, BUFFER *b, int row, int n_del,
                          WpeBufferPart *parts, int nparts, int tail_row)
{
 int more = e_blk_take_paragraph(op, b, tail_row, parts, &nparts);

 return(e_blk_put_parts(op, b, row, n_del + more, parts, nparts));
}

/**
 * e_blk_move_whole - Add the move of @n whole lines of the block's buffer
 * from @row_from to above line @row_to of the buffer it reaches (rows as
 * the user sees them now), each with its breakpoint.
 * Return: the row the first of them lands on, or -1 when there are none.
 */
static int e_blk_move_whole(WpeBlockOp *op, int row_from, int n, int row_to)
{
 int y_from = e_blk_row_now(op, op->bv, row_from), y_to, landed;

 if (n <= 0)
  return(-1);
 e_blk_note(op, op->bv, y_from, n, 0);
 y_to = e_blk_row_now(op, op->bz, row_to);
 landed = e_buffer_txn_move_lines(&op->t, op->bv, y_from, n, op->bz, y_to);
 if (landed < 0)
  landed = n;                    /* refused: nothing commits, the count is never used */
 e_blk_note(op, op->bz, y_to, 0, landed);
 return(y_to);
}

/**
 * e_blk_cut_source - Add the splices that take a moved block's partial
 * lines out of the buffer it leaves, joining what is left of its first and
 * last line (the lines between have gone already).
 *
 * The joined line keeps the breakpoint of the block's first line.  A block
 * that starts at column 0 takes its first line away whole.
 */
static void e_blk_cut_source(WpeBlockOp *op)
{
 const WpeBlock *k = &op->k;
 WpeBufferPart parts[WPE_BLK_PARTS_MAX];
 int y;

 if (k->kax > 0)
 {
  parts[0] = e_blk_part(op->bv, k->kay, 0, k->kax);
  parts[1] = e_blk_part(op->bv, k->key, k->kex, WPE_BLK_TO_END);
  e_blk_put_line(op, op->bv, k->kay, 2, parts, 2, k->key);
  return;
 }
 y = e_blk_row_now(op, op->bv, k->kay);
 e_buffer_txn_delete_lines(&op->t, op->bv, y, 1, E_BUFFER_STEP_COVERED);
 e_blk_note(op, op->bv, y, 1, 0);
 if (k->kex == 0)
  return;
 parts[0] = e_blk_part(op->bv, k->key, k->kex, WPE_BLK_TO_END);
 e_blk_put_line(op, op->bv, k->key, 1, parts, 1, k->key);
}

/**
 * e_blk_move_starting_whole - A block that starts at column 0, dropped at
 * column 0: its whole lines go above the cursor's line with their
 * breakpoints, and its last line's part joins the front of that line.
 */
static void e_blk_move_starting_whole(WpeBlockOp *op)
{
 const WpeBlock *k = &op->k;
 WpeBufferPart parts[WPE_BLK_PARTS_MAX];

 e_blk_set_landed(op, e_blk_move_whole(op, k->kay, k->key - k->kay, op->y));
 e_blk_end_at_line_front(op);
 if (k->kex == 0)
  return;
 parts[0] = e_blk_part(op->bv, k->key, k->kex, WPE_BLK_TO_END);
 e_blk_put_line(op, op->bv, k->key, 1, parts, 1, k->key);
}

/**
 * e_blk_end_at_line_front - Add the block's last line's part in front of the
 * cursor's line, which keeps its breakpoint (nothing to add for a block
 * that ends at column 0), and note the line holding the block's end.
 */
static void e_blk_end_at_line_front(WpeBlockOp *op)
{
 const WpeBlock *k = &op->k;
 WpeBufferPart parts[WPE_BLK_PARTS_MAX];

 if (k->kex == 0)
 {
  e_blk_set_end_at_line(op, op->y);
  return;
 }
 parts[0] = e_blk_part(op->bv, k->key, 0, k->kex);
 parts[1] = e_blk_part(op->bz, op->y, 0, WPE_BLK_TO_END);
 e_blk_set_end(op, e_blk_put_line(op, op->bz, op->y, 1, parts, 2, op->y));
}

/**
 * e_blk_end_into_line - Add the block's last line's part, followed by the
 * rest of the line it was dropped into, below the lines already landed,
 * and note the line holding the block's end.
 */
static void e_blk_end_into_line(WpeBlockOp *op)
{
 const WpeBlock *k = &op->k;
 WpeBufferPart parts[WPE_BLK_PARTS_MAX];

 parts[0] = e_blk_part(op->bv, k->key, 0, k->kex);
 parts[1] = e_blk_part(op->bz, op->y, op->x, WPE_BLK_TO_END);
 e_blk_set_end(op, e_blk_put_line(op, op->bz, op->y + 1, 0, parts, 2, op->y));
}

/**
 * e_blk_move_to_line_start - A block that starts inside a line, dropped at
 * column 0: its first line's part goes above the cursor's line, then the
 * lines between, and its last line's part joins the front of the cursor's
 * line, which keeps its breakpoint.
 */
static void e_blk_move_to_line_start(WpeBlockOp *op)
{
 const WpeBlock *k = &op->k;
 WpeBufferPart parts[WPE_BLK_PARTS_MAX];
 int at;

 at = e_blk_move_whole(op, k->kay + 1, k->key - k->kay - 1, op->y);
 if (at < 0)
  at = e_blk_row_now(op, op->bz, op->y);
 parts[0] = e_blk_part(op->bv, k->kay, k->kax, WPE_BLK_TO_END);
 e_blk_put_parts_at(op, op->bz, at, 0, parts, 1);
 e_blk_set_landed(op, at);
 e_blk_end_at_line_front(op);
 e_blk_cut_source(op);
}

/**
 * e_blk_move_into_line - A block dropped inside a line: the line keeps its
 * front and its breakpoint and takes the block's first line, the lines
 * between follow with their breakpoints, and the block's last line takes
 * the rest of the line.
 */
static void e_blk_move_into_line(WpeBlockOp *op)
{
 const WpeBlock *k = &op->k;
 WpeBufferPart parts[WPE_BLK_PARTS_MAX];

 e_blk_move_whole(op, k->kay + 1, k->key - k->kay - 1, op->y + 1);
 parts[0] = e_blk_part(op->bz, op->y, 0, op->x);
 parts[1] = e_blk_part(op->bv, k->kay, k->kax, WPE_BLK_TO_END);
 e_blk_set_landed(op, e_blk_put_parts(op, op->bz, op->y, 1, parts, 2));
 e_blk_end_into_line(op);
 e_blk_cut_source(op);
}

/**
 * e_blk_plan_lines_move - Prepare the move of a block of several lines to
 * the cursor, by where the block starts and where it is dropped.
 */
static void e_blk_plan_lines_move(WpeBlockOp *op)
{
 op->shape = E_BLK_SHAPE_LINES;
 op->mark_x = op->x;
 if (op->k.kax == 0 && op->x == 0)
  e_blk_move_starting_whole(op);
 else if (op->x == 0)
  e_blk_move_to_line_start(op);
 else
  e_blk_move_into_line(op);
}

/**
 * e_blk_plan_reorder - Prepare the move of a block inside one line to
 * another place of that same line: one line rewritten.
 */
static void e_blk_plan_reorder(WpeBlockOp *op)
{
 const WpeBlock *k = &op->k;
 WpeBufferPart parts[WPE_BLK_PARTS_MAX];

 op->shape = E_BLK_SHAPE_REORDER;
 op->mark_x = op->x;
 parts[0] = e_blk_part(op->bv, k->kay, 0, op->x);
 parts[1] = e_blk_part(op->bv, k->kay, k->kax, k->kex);
 parts[2] = e_blk_part(op->bv, k->kay, op->x, k->kax);
 parts[3] = e_blk_part(op->bv, k->kay, k->kex, WPE_BLK_TO_END);
 if (op->x > k->kex)
 {
  op->mark_x = op->x - op->n;
  parts[0] = e_blk_part(op->bv, k->kay, 0, k->kax);
  parts[1] = e_blk_part(op->bv, k->kay, k->kex, op->x);
  parts[2] = e_blk_part(op->bv, k->kay, k->kax, k->kex);
  parts[3] = e_blk_part(op->bv, k->kay, op->x, WPE_BLK_TO_END);
 }
 e_blk_set_landed(op, e_blk_put_line(op, op->bv, k->kay, 1, parts, 4, k->kay));
}

/**
 * e_blk_plan_chars_move - Prepare the move of a block inside one line to
 * the cursor on another line (or in another buffer): the characters go in
 * at the cursor and out of their line.
 */
static void e_blk_plan_chars_move(WpeBlockOp *op)
{
 const WpeBlock *k = &op->k;
 WpeBufferPart parts[WPE_BLK_PARTS_MAX];

 op->n = k->kex - k->kax;
 if (op->bv == op->bz && op->y == k->kay)
 {
  e_blk_plan_reorder(op);
  return;
 }
 op->shape = E_BLK_SHAPE_CHARS;
 op->mark_x = op->x;
 parts[0] = e_blk_part(op->bz, op->y, 0, op->x);
 parts[1] = e_blk_part(op->bv, k->kay, k->kax, k->kex);
 parts[2] = e_blk_part(op->bz, op->y, op->x, WPE_BLK_TO_END);
 e_blk_set_landed(op, e_blk_put_line(op, op->bz, op->y, 1, parts, 3, op->y));
 parts[0] = e_blk_part(op->bv, k->kay, 0, k->kax);
 parts[1] = e_blk_part(op->bv, k->kay, k->kex, WPE_BLK_TO_END);
 e_blk_put_line(op, op->bv, k->kay, 1, parts, 2, k->kay);
}

/**
 * e_blk_plan_shift_left - The cursor is on the block's first line, before
 * the block: the text between them goes to the block's end, so the block
 * now starts at the cursor.
 */
static void e_blk_plan_shift_left(WpeBlockOp *op)
{
 const WpeBlock *k = &op->k;
 WpeBufferPart parts[WPE_BLK_PARTS_MAX];

 op->shape = E_BLK_SHAPE_SHIFT_LEFT;
 parts[0] = e_blk_part(op->bv, k->kay, 0, op->x);
 parts[1] = e_blk_part(op->bv, k->kay, k->kax, WPE_BLK_TO_END);
 e_blk_put_parts(op, op->bv, k->kay, 1, parts, 2);
 parts[0] = e_blk_part(op->bv, k->key, 0, k->kex);
 parts[1] = e_blk_part(op->bv, k->kay, op->x, k->kax);
 parts[2] = e_blk_part(op->bv, k->key, k->kex, WPE_BLK_TO_END);
 e_blk_put_line(op, op->bv, k->key, 1, parts, 3, k->key);
}

/**
 * e_blk_plan_shift_right - The cursor is on the block's last line, after
 * the block: the text between them goes to the block's start, so the block
 * now ends at the cursor.
 */
static void e_blk_plan_shift_right(WpeBlockOp *op)
{
 const WpeBlock *k = &op->k;
 WpeBufferPart parts[WPE_BLK_PARTS_MAX];

 op->shape = E_BLK_SHAPE_SHIFT_RIGHT;
 op->n = op->x - k->kex;
 parts[0] = e_blk_part(op->bv, k->key, 0, k->kex);
 parts[1] = e_blk_part(op->bv, k->key, op->x, WPE_BLK_TO_END);
 e_blk_put_line(op, op->bv, k->key, 1, parts, 2, k->key);
 parts[0] = e_blk_part(op->bv, k->kay, 0, k->kax);
 parts[1] = e_blk_part(op->bv, k->key, k->kex, op->x);
 parts[2] = e_blk_part(op->bv, k->kay, k->kax, WPE_BLK_TO_END);
 op->wrapped = e_blk_put_parts_at(op, op->bv, e_blk_row_now(op, op->bv, k->kay), 1, parts, 3);
}

/**
 * e_blk_cursor_inside - Is the cursor inside the block it would move?  The
 * block cannot be moved into itself.
 */
static int e_blk_cursor_inside(const WpeBlockOp *op, int x, int y)
{
 const WpeBlock *k = &op->k;

 return(op->bv == op->bz && y >= k->kay && y <= k->key && x >= k->kax && x <= k->kex);
}

/**
 * e_blk_plan_move - Prepare a block move by the place it is dropped at.
 * @x: the cursor's column as the user placed it (past the end of the line
 * is allowed); op->x is that column on the line.
 * Return: E_BLOCK_MOVED when it was planned, E_BLOCK_NOT_MOVED when a
 * block of several lines would land on one of its own lines.
 */
static int e_blk_plan_move(WpeBlockOp *op, int x)
{
 const WpeBlock *k = &op->k;
 int same = (op->bv == op->bz);

 if (k->kay == k->key)
  e_blk_plan_chars_move(op);
 else if (same && op->y == k->kay && x < k->kax)
  e_blk_plan_shift_left(op);
 else if (same && op->y == k->key && op->x > k->kex)
  e_blk_plan_shift_right(op);
 else if (e_blk_lands_on_itself(op->bv, op->bz, op->y, k))
  return(E_BLOCK_NOT_MOVED);
 else
  e_blk_plan_lines_move(op);
 return(E_BLOCK_MOVED);
}

/**
 * e_move_block_prepare - Check a block move and prepare every splice of it;
 * nothing moves yet.
 * @x, @y: the cursor in @bz; @bv: the buffer the block leaves; @bz: the one
 * it reaches; @f: the window the user works in (its marks are the block);
 * @kind: the undo record to reserve with the move ('v', 'c'), or 0.
 * Return: E_BLOCK_MOVED (ready to make), E_BLOCK_NOT_MOVED (nothing to move,
 * the window is closed to changes, the cursor is inside the block) or
 * E_BLOCK_NOMEM.
 */
static int e_move_block_prepare(WpeBlockOp *op, int x, int y, BUFFER *bv, BUFFER *bz,
                                FENSTER *f, int kind)
{
 e_blk_op_begin(op, f->ed->f[f->ed->mxedt]->s, bv, bz, kind);
 if (e_blk_is_empty(&op->k) || e_window_text_locked(f) || e_blk_cursor_inside(op, x, y))
  return(E_BLOCK_NOT_MOVED);
 op->y = y;
 op->x = e_blk_column_on_line(bz, x, y);
 if (op->x < 0)
  return(E_BLOCK_NOT_MOVED);
 if (e_blk_plan_move(op, x) != E_BLOCK_MOVED)
  return(E_BLOCK_NOT_MOVED);
 return(e_blk_op_state(op));
}

/**
 * e_blk_mark_source_left - The block has left its buffer: that buffer's
 * marks close up where the block began, and so does its cursor.
 */
static void e_blk_mark_source_left(WpeBlockOp *op)
{
 SCHIRM *sv = op->bv->f->s;

 sv->mark_begin.x = sv->mark_end.x = op->bv->b.x = op->k.kax;
 sv->mark_begin.y = sv->mark_end.y = op->bv->b.y = op->k.kay;
}

/* Which end of a block a column on a wrapped line is for: a column at the
   very end of a soft-wrapped row is the start of the next row for the
   block's start, and stays at the end of the row for the block's end. */
#define E_BLK_AT_START 0
#define E_BLK_AT_END   1

/**
 * e_blk_past_row - Does column @x of a line lie beyond a soft-wrapped row
 * of @len characters, for the end @which of a block?
 */
static int e_blk_past_row(int len, int x, int which)
{
 if (x > len)
  return(1);
 return(x == len && which == E_BLK_AT_START);
}

/**
 * e_blk_point_on_rows - Where column @x of the line that starts on row @y
 * of @b stands on the screen, once the line has wrapped onto the rows
 * below it.
 * @which: E_BLK_AT_START or E_BLK_AT_END.
 *
 * A block dropped near the end of a line as wide as the window makes the
 * line wrap before the block, so the block is marked on the row it is on:
 * Ctrl-U then takes back the copy, never the user's own characters.
 */
static POINT e_blk_point_on_rows(BUFFER *b, int y, int x, int which)
{
 POINT p;

 while (y + 1 < b->mxlines && b->bf[y].s[b->bf[y].len] == '\0' &&
        e_blk_past_row(b->bf[y].len, x, which))
 {
  x -= b->bf[y].len;
  y++;
 }
 p.x = x;
 p.y = y;
 return(p);
}

/**
 * e_blk_mark_chars_landed - Mark a block of @op->n characters where it
 * landed in a line, on the rows the line wrapped onto, the cursor at its
 * end.
 */
static void e_blk_mark_chars_landed(WpeBlockOp *op)
{
 SCHIRM *sz = op->bz->f->s;
 int y = e_blk_landed_row(op);

 sz->mark_begin = e_blk_point_on_rows(op->bz, y, op->mark_x, E_BLK_AT_START);
 sz->mark_end = e_blk_point_on_rows(op->bz, y, op->mark_x + op->n, E_BLK_AT_END);
 op->bz->b = sz->mark_end;
}

/**
 * e_blk_mark_lines_landed - Mark a block of several lines where it landed,
 * its start in the line it starts in and its end in the line that holds
 * it, on the rows those lines wrapped onto; the cursor at its end.
 *
 * The undo record keeps these marks, so a block dropped into a line as
 * wide as the window is taken back exactly, wrap and all.
 */
static void e_blk_mark_lines_landed(WpeBlockOp *op)
{
 SCHIRM *sz = op->bz->f->s;

 sz->mark_begin = e_blk_point_on_rows(op->bz, e_blk_landed_row(op), op->mark_x,
                                      E_BLK_AT_START);
 sz->mark_end = e_blk_point_on_rows(op->bz, e_blk_end_row(op), op->k.kex, E_BLK_AT_END);
 op->bz->b = sz->mark_end;
}

/**
 * e_blk_mark_lines_moved - A block of several lines moved: its old place
 * closes up, and it is marked where it landed with the cursor at its end.
 */
static void e_blk_mark_lines_moved(WpeBlockOp *op)
{
 e_blk_mark_source_left(op);
 e_blk_mark_lines_landed(op);
}

/**
 * e_blk_mark_chars_moved - A block inside one line moved: its old place
 * closes up, and it is marked where it landed.
 */
static void e_blk_mark_chars_moved(WpeBlockOp *op)
{
 e_blk_mark_source_left(op);
 e_blk_mark_chars_landed(op);
}

/**
 * e_blk_mark_shifted_left - After the text before the block went behind
 * it: the block starts at the cursor, and the cursor stays there.
 */
static void e_blk_mark_shifted_left(WpeBlockOp *op)
{
 SCHIRM *sv = op->bv->f->s;

 op->bv->b.x = sv->mark_begin.x = op->x;
 op->bv->b.y = sv->mark_begin.y = op->k.kay;
 sv->mark_end.x = op->k.kex;
 sv->mark_end.y = op->k.key;
}

/**
 * e_blk_mark_shifted_right - After the text behind the block went before
 * it: the block ends where the cursor was, and the cursor is at its end.
 */
static void e_blk_mark_shifted_right(WpeBlockOp *op)
{
 SCHIRM *sv = op->bv->f->s;

 sv->mark_begin.x = op->k.kax + op->n;
 sv->mark_begin.y = op->k.kay;
 sv->mark_end.x = op->k.kex;
 sv->mark_end.y = op->k.key + op->wrapped - 1;
 op->bv->b = sv->mark_end;
}

/**
 * e_blk_mark_moved - Mark the block where a move put it, by the shape of
 * the move.
 */
static void e_blk_mark_moved(WpeBlockOp *op)
{
 if (op->shape == E_BLK_SHAPE_SHIFT_LEFT)
  e_blk_mark_shifted_left(op);
 else if (op->shape == E_BLK_SHAPE_SHIFT_RIGHT)
  e_blk_mark_shifted_right(op);
 else if (op->shape == E_BLK_SHAPE_LINES)
  e_blk_mark_lines_moved(op);
 else
  e_blk_mark_chars_moved(op);
}

/**
 * e_blk_give_record - Hand the caller the undo record of an operation that
 * was made (it fills and pushes it); a refused one freed it already.
 * @r: what the operation returned; @rec: where the caller wants it, or NULL.
 */
static void e_blk_give_record(const WpeBlockOp *op, int r, Undo **rec)
{
 if (rec == NULL)
  return;
 *rec = NULL;
 if (r == E_BLOCK_MOVED)
  *rec = op->rec;
}

/**
 * e_blk_note_left_row - Remember the row the block's old start stands on
 * once every splice of the move has committed, for the move's Undo.
 *
 * Lines the move added or a line it made wrap above the old place push it
 * down; counting them is what lets Ctrl-U put the block back where it was.
 * The two shifts keep their own rule (E_BLK_NO_ROW).
 */
static void e_blk_note_left_row(WpeBlockOp *op)
{
 op->left_row = E_BLK_NO_ROW;
 if (op->shape == E_BLK_SHAPE_SHIFT_LEFT || op->shape == E_BLK_SHAPE_SHIFT_RIGHT)
  return;
 op->left_row = e_blk_row_now(op, op->bv, op->k.kay);
}

/**
 * e_blk_give_left_row - Hand the caller the row of the move's old place.
 * @r: what the move returned; @left_row: where the caller wants it, or NULL.
 */
static void e_blk_give_left_row(const WpeBlockOp *op, int r, int *left_row)
{
 if (left_row == NULL)
  return;
 *left_row = E_BLK_NO_ROW;
 if (r == E_BLOCK_MOVED)
  *left_row = op->left_row;
}

/**
 * e_blk_show_moved - Show a move that was made: the block marked where it
 * landed, the colours of both buffers recomputed, the window drawn.
 */
static void e_blk_show_moved(WpeBlockOp *op, FENSTER *f)
{
 e_blk_note_left_row(op);
 e_blk_mark_moved(op);
 f->save = f->ed->maxchg + 1;
 sc_txt_1(op->bv->f);
 sc_txt_1(op->bz->f);
 e_cursor(f, 1);
 e_schirm(f, 1);
}

/**
 * e_move_block_covered - Move the block the user's window marks from @bv
 * to (@x, @y) of @bz, as one action for Undo.
 * @f: the window the user works in (its marks are the block); @kind: the
 * undo record to take with the move ('v' for Block Move, 'c' for the way
 * back of a block delete), or 0; @rec: receives that record when the move
 * was made, for the caller to fill and push (NULL with @kind 0).
 *
 * Block Move (Ctrl-K V), a block delete into its undo record and that
 * record's Undo all move lines this way.  The steps are left out of Undo on
 * both buffers; the one record replays the whole move, so one Ctrl-U takes
 * it back and the breakpoints return with their lines.  Every splice and
 * the record are prepared before anything moves: a move refused for want
 * of memory leaves both buffers, their marks, breakpoints and Undo exactly
 * as they were.  Nothing is said here; the caller names the action.
 * Return: E_BLOCK_MOVED, E_BLOCK_NOT_MOVED (nothing to move, the window is
 * closed to changes, the cursor is inside the block) or E_BLOCK_NOMEM.
 */
int e_move_block_covered(int x, int y, BUFFER *bv, BUFFER *bz, FENSTER *f, int kind,
                         Undo **rec)
{
 return(e_blk_move_covered(x, y, bv, bz, f, kind, rec, NULL));
}

/**
 * e_blk_move_covered - e_move_block_covered, also saying where the block's
 * old start stands once it has moved.
 * @left_row: receives that row, or E_BLK_NO_ROW (a shift, or no move);
 * NULL when not wanted.
 * Return: as e_move_block_covered.
 */
static int e_blk_move_covered(int x, int y, BUFFER *bv, BUFFER *bz, FENSTER *f, int kind,
                              Undo **rec, int *left_row)
{
 WpeBlockOp op;
 int r, fault;

 e_blk_cover_begin(bv, bz);
 fault = e_buffer_test_fault_enter(E_BUFFER_FAULT_BLOCK_MOVE);
 r = e_blk_op_make(&op, e_move_block_prepare(&op, x, y, bv, bz, f, kind));
 if (r == E_BLOCK_MOVED)
  e_blk_show_moved(&op, f);
 e_buffer_test_fault_leave(fault);
 e_blk_cover_end(bv, bz);
 e_blk_give_record(&op, r, rec);
 e_blk_give_left_row(&op, r, left_row);
 return(r);
}

/*   copy block within window   */
int e_blck_copy(FENSTER *f)
{
 BUFFER *b;
 Undo *rec = NULL;
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
 i = e_copy_block_covered(b->b.x, b->b.y, b, b, f, 'c', &rec);
 if (i == E_BLOCK_NOMEM)
  wpe_buffer_say_nomem(b, "the block copy");
 if (i != E_BLOCK_MOVED)
  return(0);
 e_block_record_keep(rec, b, b->b.x, b->b.y);
 sc_txt_2(f);
 f->save = b->cn->maxchg + 1;
 return(0);
}

/**
 * e_blk_copy_lands_inside - Would a copy land inside the block it copies?
 * A block of several lines may land at its own start or end; a block inside
 * one line anywhere but between its two ends.
 */
static int e_blk_copy_lands_inside(const WpeBlockOp *op, int x, int y)
{
 const WpeBlock *k = &op->k;

 if (op->bv != op->bz)
  return(0);
 if (k->kay == k->key)
  return(y == k->kay && x >= k->kax && x < k->kex);
 return((y > k->kay && y < k->key) || (y == k->kay && x >= k->kax) ||
        (y == k->key && x < k->kex));
}

/**
 * e_blk_copy_middle - Add copies of the block's lines between its first
 * and its last, put in above line @row_to of the buffer it is copied to (as
 * the user sees it now).  A copy carries no breakpoint.
 */
static void e_blk_copy_middle(WpeBlockOp *op, int row_to)
{
 int m = op->k.key - op->k.kay - 1, y, landed;

 if (m <= 0)
  return;
 y = e_blk_row_now(op, op->bz, row_to);
 landed = e_buffer_txn_copy_lines(&op->t, op->bz, y, op->bv, op->k.kay + 1, m,
                                  E_BUFFER_STEP_COVERED);
 if (landed >= 0)
  e_blk_note(op, op->bz, y, 0, landed);
}

/**
 * e_blk_copy_to_line_start - A block copied to column 0 of a line: its
 * first line's part goes above the line, then copies of the lines between,
 * and its last line's part joins the line's front, which keeps its
 * breakpoint.
 */
static void e_blk_copy_to_line_start(WpeBlockOp *op)
{
 const WpeBlock *k = &op->k;
 WpeBufferPart parts[WPE_BLK_PARTS_MAX];
 int at = e_blk_row_now(op, op->bz, op->y);

 parts[0] = e_blk_part(op->bv, k->kay, k->kax, WPE_BLK_TO_END);
 e_blk_put_parts_at(op, op->bz, at, 0, parts, 1);
 e_blk_set_landed(op, at);
 e_blk_copy_middle(op, op->y);
 e_blk_end_at_line_front(op);
}

/**
 * e_blk_copy_into_line - A block copied inside a line: the line keeps its
 * front and its breakpoint and takes the block's first line, copies of the
 * lines between follow, and the block's last line takes the rest of the
 * line.
 */
static void e_blk_copy_into_line(WpeBlockOp *op)
{
 const WpeBlock *k = &op->k;
 WpeBufferPart parts[WPE_BLK_PARTS_MAX];

 parts[0] = e_blk_part(op->bz, op->y, 0, op->x);
 parts[1] = e_blk_part(op->bv, k->kay, k->kax, WPE_BLK_TO_END);
 e_blk_set_landed(op, e_blk_put_parts(op, op->bz, op->y, 1, parts, 2));
 e_blk_copy_middle(op, op->y + 1);
 e_blk_end_into_line(op);
}

/**
 * e_blk_plan_chars_copy - Prepare the copy of a block inside one line: its
 * characters go in at the cursor, one line rewritten.
 */
static void e_blk_plan_chars_copy(WpeBlockOp *op)
{
 const WpeBlock *k = &op->k;
 WpeBufferPart parts[WPE_BLK_PARTS_MAX];

 op->shape = E_BLK_SHAPE_CHARS;
 op->n = k->kex - k->kax;
 op->mark_x = op->x;
 parts[0] = e_blk_part(op->bz, op->y, 0, op->x);
 parts[1] = e_blk_part(op->bv, k->kay, k->kax, k->kex);
 parts[2] = e_blk_part(op->bz, op->y, op->x, WPE_BLK_TO_END);
 e_blk_set_landed(op, e_blk_put_line(op, op->bz, op->y, 1, parts, 3, op->y));
}

/**
 * e_blk_plan_copy - Prepare a block copy by the block's shape and the
 * place it is dropped at.
 */
static void e_blk_plan_copy(WpeBlockOp *op)
{
 op->shape = E_BLK_SHAPE_LINES;
 op->mark_x = op->x;
 if (op->k.kay == op->k.key)
  e_blk_plan_chars_copy(op);
 else if (op->x == 0)
  e_blk_copy_to_line_start(op);
 else
  e_blk_copy_into_line(op);
}

/**
 * e_copy_block_prepare - Check a block copy and prepare every splice of it;
 * nothing changes yet.
 * @x, @y: the cursor in @bd; @bs: the buffer whose window marks the block;
 * @bd: the buffer it is copied to; @kind: the undo record to reserve ('c'),
 * or 0.
 * Return: E_BLOCK_MOVED (ready to make), E_BLOCK_NOT_MOVED (nothing to
 * copy, the cursor is inside the block or not on a line) or E_BLOCK_NOMEM.
 */
static int e_copy_block_prepare(WpeBlockOp *op, int x, int y, BUFFER *bs, BUFFER *bd,
                                int kind)
{
 e_blk_op_begin(op, bs->f->s, bs, bd, kind);
 if (e_blk_is_empty(&op->k) || e_blk_copy_lands_inside(op, x, y))
  return(E_BLOCK_NOT_MOVED);
 op->y = y;
 op->x = e_blk_column_on_line(bd, x, y);
 if (op->x < 0)
  return(E_BLOCK_NOT_MOVED);
 e_blk_plan_copy(op);
 return(e_blk_op_state(op));
}

/**
 * e_blk_mark_copied - Mark the copy where it landed, the cursor at its end.
 */
static void e_blk_mark_copied(WpeBlockOp *op)
{
 if (op->shape == E_BLK_SHAPE_CHARS)
  e_blk_mark_chars_landed(op);
 else
  e_blk_mark_lines_landed(op);
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
 * e_copy_block_covered - Copy the block @bs's window marks to (@x, @y) of
 * @bd, as one action for Undo.
 * @f: the window the user works in; @kind: the undo record to take with
 * the copy ('c'), or 0; @rec: receives that record when the copy was made,
 * for the caller to fill and push (NULL with @kind 0).
 *
 * Block Copy (Ctrl-K C), Copy to the clipboard and Paste all copy this way.
 * Every line the copy builds, the copies of the block's lines and the
 * record are prepared before anything changes: a copy refused for want of
 * memory leaves the text, the line it was dropped into included, the
 * block, the breakpoints and Undo exactly as they were.  Nothing is said
 * here; the caller names the action.
 * Return: E_BLOCK_MOVED, E_BLOCK_NOT_MOVED or E_BLOCK_NOMEM.
 */
static int e_copy_block_covered(int x, int y, BUFFER *bs, BUFFER *bd, FENSTER *f, int kind,
                                Undo **rec)
{
 WpeBlockOp op;
 int r, fault;

 e_blk_cover_begin(bs, bd);
 fault = e_buffer_test_fault_enter(e_blk_copy_fault_point(bd));
 r = e_blk_op_make(&op, e_copy_block_prepare(&op, x, y, bs, bd, kind));
 if (r == E_BLOCK_MOVED)
  e_blk_mark_copied(&op);
 e_buffer_test_fault_leave(fault);
 e_blk_cover_end(bs, bd);
 e_blk_give_record(&op, r, rec);
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
 int x, x_begin = 0, x_end = b->bf[y].len;

 if (e_buffer_line_copy(b, &b->bf[y], out) != E_BUFFER_OK)
  return(E_BUFFER_NOMEM);
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
 int i, n, r;

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
 r = e_buffer_splice(b, screen->mark_begin.y, n, lines, n, NULL, 0, E_BUFFER_STEP_NEW);
 if (r != E_BUFFER_OK)
 {
  e_buffer_lines_free(lines, n);
  if (r == E_BUFFER_NOMEM)
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

/**
 * e_unindent_blanks - The blanks Ctrl-K U may put back on one row.
 * @tabn: the tab width (Tabstops).
 *
 * A shift to the left takes Tabstops / 2 columns; when the last thing it
 * takes is a tab, the tab may have held up to one tab's width more than
 * that, and those columns come back as blanks so the line moves exactly
 * Tabstops / 2 to the left.  The string holds a whole tab's width of
 * blanks, so they are always blanks and never what lies past it.
 * Return: the string (the caller frees it), or NULL when memory ran out.
 */
static char *e_unindent_blanks(int tabn)
{
 char *blanks = MALLOC((tabn + 1) * sizeof(char));

 if (blanks == NULL)
  return(NULL);
 memset(blanks, ' ', tabn);
 blanks[tabn] = '\0';
 return(blanks);
}

/* What Ctrl-K I and U hand their pieces: the blanks one line moves by. */
typedef struct {
 char *blanks;     /* @n blanks */
 int n;            /* Tabstops / 2 */
 int tabn;         /* Tabstops */
} EShift;

/**
 * e_shift_first_row - The first line Ctrl-K I or U moves: the block's first
 * row, or the row below it when the block starts inside that row.
 */
static int e_shift_first_row(SCHIRM *s)
{
 if (s->mark_begin.x > 0)
  return(s->mark_begin.y + 1);
 return(s->mark_begin.y);
}

/**
 * e_shift_last_row - The last line Ctrl-K I or U moves: the block's last
 * row, or the row above it when the block ends at the start of that row.
 *
 * Before the first row (a block inside one row) there is no line to move,
 * and nothing is begun.
 */
static int e_shift_last_row(SCHIRM *s)
{
 if (s->mark_end.x > 0)
  return(s->mark_end.y);
 return(s->mark_end.y - 1);
}

/**
 * e_shift_right_pieces - Put the blanks of @plan in front of the first
 * character of every line of the block of @s.
 *
 * The lines are read from the block as the pieces leave it: a line that
 * wraps moves the end of the block down with it, as on the user's text.
 */
static void e_shift_right_pieces(BUFFER *b, SCHIRM *s, int top, void *plan)
{
 EShift *sh = plan;
 int i, j;

 for (i = e_shift_first_row(s); i <= e_shift_last_row(s); i++)
 {
  for (j = 0; j < b->bf[i].len && isspace(b->bf[i].s[j]); j++)
   ;                              /* a row of blanks stops at its end, before its line break */
  e_ins_nchar(b, s, (unsigned char *)sh->blanks, j, i, sh->n);
  if (i == s->mark_begin.y)
   s->mark_begin.x = 0;
 }
}

/**
 * e_shift_left_pieces - Take Tabstops / 2 columns of indent from every line
 * of the block of @s; a tab that spans more gives back blanks for the rest.
 */
static void e_shift_left_pieces(BUFFER *b, SCHIRM *s, int top, void *plan)
{
 EShift *sh = plan;
 int n = sh->n, i, j, k, l, m, nn;

 for (i = e_shift_first_row(s); i <= e_shift_last_row(s); i++)
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
     nn += sh->tabn - (nn % sh->tabn);
    }
    k += sh->tabn - (nn % sh->tabn);
   }
  }
  l = j - l - 1;
  if(l > 0)
  {
   nn = s->mark_begin.x;
   e_del_nchar(b, s, j - l, i, l);
   if (k > n) e_ins_nchar(b, s, (unsigned char *)sh->blanks, j-l, i, k-n);
   s->mark_begin.x = nn;
  }
 }
}

/**
 * e_blck_to_left - Ctrl-K U: move every line of the block Tabstops / 2
 * columns to the left.
 * @f: the window the command came from.
 *
 * The lines are one action: one Ctrl-U gives the whole block back as it
 * was, one Ctrl-R shifts it again, however many lines it holds and whatever
 * Num. Undo is.
 * Return: 0.
 */
int e_blck_to_left(FENSTER *f)
{
 BUFFER *b;
 SCHIRM *s;
 int n = f->ed->tabn/2, i;
 char *tstr;
 EShift shift;

 for(i = f->ed->mxedt; i > 0 && !DTMD_ISTEXT(f->ed->f[i]->dtmd); i--);
 if(i <= 0) return(0);
 e_switch_window(f->ed->edt[i], f);
 f = f->ed->f[f->ed->mxedt];
 b = f->b;
 s = f->s;
 tstr = e_unindent_blanks(f->ed->tabn);
 if (tstr == NULL)
 {
  wpe_buffer_say_nomem(b, "the shift to the left");
  return(0);
 }
 shift.blanks = tstr;
 shift.n = n;
 shift.tabn = f->ed->tabn;
 e_edit_action_run(b, e_shift_first_row(s), e_shift_last_row(s), E_BUFFER_FAULT_SHIFT,
                   "the shift to the left", e_shift_left_pieces, &shift);
 FREE(tstr);
 e_schirm(f, 1);
 return(0);
}

/**
 * e_blck_to_right - Ctrl-K I: move every line of the block Tabstops / 2
 * columns to the right.
 * @f: the window the command came from.
 *
 * The lines are one action: one Ctrl-U gives the whole block back as it
 * was, one Ctrl-R shifts it again, however many lines it holds and whatever
 * Num. Undo is.
 * Return: 0.
 */
int e_blck_to_right(FENSTER *f)
{
 BUFFER *b;
 SCHIRM *s;
 int n = f->ed->tabn/2, i;
 char *tstr = MALLOC((n+1)*sizeof(char));
 EShift shift;

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
 shift.blanks = tstr;
 shift.n = n;
 shift.tabn = f->ed->tabn;
 e_edit_action_run(b, e_shift_first_row(s), e_shift_last_row(s), E_BUFFER_FAULT_SHIFT,
                   "the shift to the right", e_shift_right_pieces, &shift);
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
 int i, row;

 for (i = f->ed->mxedt; i > 0 && !DTMD_ISTEXT(f->ed->f[i]->dtmd); i--)
  ;
 if (i <= 0)
  return(0);
 e_switch_window(f->ed->edt[i], f);
 f = f->ed->f[f->ed->mxedt];
 b = f->b;  s = f->s;
 if (e_find_next(b, s, fd))
 {
  if (!(fd->sw & 1))
  {
   row = s->fa.y;
   e_cursor(f, 1);
   s->fa.y = row;
   e_schirm(f, 1);
  }
  return(1);
 }
 if (!(fd->sw & 1))
  e_message(0, e_msg[ERR_GETSTRING], f);
 return(0);
}

/**
 * e_find_next - Find the next occurrence of what @fd names in @b, from the
 * cursor on, within the scope @fd and the block of @s give.
 *
 * Search > Find, Find Again and Replace look on the user's text; Change
 * All looks on the owner's copy of the rows it changes, the same search on
 * the same text, so it finds every occurrence the user's text would show.
 * Nothing is painted and nothing is said.
 * Return: 1 with the occurrence in @s->fa to @s->fe and the cursor behind
 * it (in front of it searching back), 0 when there is none.
 */
int e_find_next(BUFFER *b, SCHIRM *s, FIND *fd)
{
 int ret, j, iend, jend, end;

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
/* fd->sw: the search runs back from the cursor (Back from Cursor). */
#define E_FIND_BACKWARDS 4

/**
 * e_rep_search_on_after - Where Search > Replace looks next after it
 * replaced the occurrence found on row @y: behind the new text, on the row
 * where it ends (a replacement that wrapped ends on a row below), or, when
 * searching backwards, before where it starts now.
 * @b, @s: the buffer and its screen; @backwards: nonzero to search back.
 *
 * The next occurrence is then never looked for inside the new text, nor
 * past the end of a row the replacement made shorter.  A wrap may have
 * moved the text just before the replacement to the row below with it; a
 * backward search starts behind that text, so no occurrence in it is
 * skipped.
 */
static void e_rep_search_on_after(BUFFER *b, SCHIRM *s, int backwards, int y)
{
 if (backwards)
 {
  e_ins_point_at(b, y, s->fa.x, &b->b);
  return;
 }
 s->fe = b->b;
}

/* fd->sw: the scope is the block (Selected Text). */
#define E_FIND_SELECTED 8
/* fd->sw: ask before each replacement (Prompt on Replace). */
#define E_FIND_PROMPT 16
/* A window whose top is above this screen row is scrolled so the hit shows
   at its top, clear of the question box. */
#define E_REPLACE_SCROLL_ROW 11
/* Change All, in the words of the status row when it was not made. */
#define E_CHANGE_ALL_WHAT "Change All"
/* The box's third line when a replacement ran out of memory. */
#define E_REPLACE_SAY_NOMEM "Out of memory: the replacement was not made"

/* What Search > Replace did, for the box it ends with. */
typedef struct {
 int found;          /* occurrences found */
 int replaced;       /* occurrences replaced */
 int read_only;      /* occurrences a read-only window kept */
 int out_of_memory;  /* 1: a replacement was refused for memory, and the command stopped */
 int not_made;       /* 1: Change All was not made at all; the status row says why */
} EReplaceReport;

/* What one replacement, or Change All on the owner's copy, works with. */
typedef struct {
 FIND *fd;           /* the editor's Find and Replace texts and options */
 int x, y;           /* one replacement: the occurrence, on the user's rows */
 int found;          /* Change All: occurrences found on the copy */
 int replaced;       /* Change All: occurrences replaced there */
 int last_y;         /* Change All: the row of the last occurrence, on the user's rows */
} EReplacePlan;

/**
 * e_replace_show_hit - Show the occurrence found on row @y of @f.
 *
 * Each hit Replace asks about, and the last one Change All replaced, is in
 * view when the user reads the question or the report, on the row it was
 * found on even when its replacement wrapped onto the row below.
 */
static void e_replace_show_hit(FENSTER *f, int y)
{
 if (f->a.y < E_REPLACE_SCROLL_ROW)
  f->s->c.y = y - 1;
 e_schirm(f, 1);
}

/**
 * e_rep_search_past_hit - Search on behind the occurrence in @s->fa that
 * stays in the text, never at it again.
 */
static void e_rep_search_past_hit(BUFFER *b, SCHIRM *s, FIND *fd)
{
 s->fe.x = s->fa.x + fd->sn;
 if (fd->sw & E_FIND_BACKWARDS)
  b->b.x = s->fa.x;
 else
  b->b.x = s->fe.x;
}

/**
 * e_replace_hit_pieces - Replace the one occurrence @plan names on the
 * owner's copy of its rows, whose row 0 is the user's row @top.
 */
static void e_replace_hit_pieces(BUFFER *b, SCHIRM *s, int top, void *plan)
{
 EReplacePlan *p = plan;

 e_replace_occurrence(b, s, p->x, p->y - top, p->fd->sn, (unsigned char *)p->fd->replace,
                      p->fd->rn);
}

/**
 * e_replace_hit - Replace the occurrence at (@x, @y) of @b, whole or not at
 * all: what Replace does with each occurrence the user said yes to, and
 * with the one its OK button replaces.
 *
 * A replacement longer than a row is a deletion and the new rows; when
 * memory runs out for either, the occurrence stays as it was.
 * Return: E_BUFFER_OK, or the refusal (E_BUFFER_LOCKED for a read-only
 * window, E_BUFFER_NOMEM or E_BUFFER_RANGE said on the status row).
 */
static int e_replace_hit(BUFFER *b, FIND *fd, int x, int y)
{
 EReplacePlan p;

 p.fd = fd;
 p.x = x;
 p.y = y;
 return(e_edit_action_run(b, y, y, E_BUFFER_FAULT_CHANGE_ALL, "the replacement",
                          e_replace_hit_pieces, &p));
}

/**
 * e_replace_each - Replace on the user's text one occurrence at a time:
 * Replace's OK button, Change All with Prompt on Replace, and a read-only
 * window.
 * @ret: the button (AltA for Change All); @r: what was done, for the box.
 *
 * Each replacement is its own whole change and all of them are one Ctrl-U.
 * One refused for want of memory stops the command as Esc would, and the
 * box says why; one a read-only window keeps is counted and searched past.
 */
static void e_replace_each(FENSTER *f, FIND *fd, int ret, EReplaceReport *r)
{
 BUFFER *b = f->b;
 SCHIRM *s = f->s;
 int fault = e_buffer_test_fault_enter(E_BUFFER_FAULT_CHANGE_ALL);
 int c, found_y, made;

 e_buffer_step_begin(b);
 while (e_rep_search(f) && ((ret == AltA) || (!r->found)))
 {
  r->found++;
  e_replace_show_hit(f, b->b.y);
  c = 'Y';
  if (fd->sw & E_FIND_PROMPT)
   c = e_message(1, "String found:\nReplace this occurrence ?", f);
  if (c == WPE_ESC)
   break;
  if (c != 'Y')
   continue;
  found_y = b->b.y;
  made = e_replace_hit(b, fd, s->fa.x, found_y);
  if (made == E_BUFFER_NOMEM)
  {
   r->out_of_memory = 1;
   e_schirm(f, 1);
   break;
  }
  if (made == E_BUFFER_OK)
  {
   r->replaced++;
   e_rep_search_on_after(b, s, fd->sw & E_FIND_BACKWARDS, found_y);
  }
  else
  {
   if (made == E_BUFFER_LOCKED)
    r->read_only++;
   e_rep_search_past_hit(b, s, fd);
  }
  e_schirm(f, 1);
 }
 e_buffer_step_end(b);
 e_buffer_test_fault_leave(fault);
}

/**
 * e_change_all_rows - The rows of @b Change All can change, from the first
 * occurrence, which the cursor stands on, to the end of its scope.
 *
 * Rows above the first occurrence are never touched searching forward;
 * searching back, the rows from the top to the first occurrence are.
 */
static void e_change_all_rows(BUFFER *b, SCHIRM *s, FIND *fd, int *y0, int *y1)
{
 if (fd->sw & E_FIND_BACKWARDS)
 {
  *y0 = 0;
  *y1 = b->b.y;
  return;
 }
 *y0 = b->b.y;
 *y1 = b->mxlines - 1;
 if ((fd->sw & E_FIND_SELECTED) && s->mark_end.y >= *y0 && s->mark_end.y < *y1)
  *y1 = s->mark_end.y;
}

/**
 * e_change_all_starved - Has the owner's copy Change All works on run out
 * of memory?  The copy is then thrown away whole, so searching on would be
 * work for nothing.
 */
static int e_change_all_starved(BUFFER *copy)
{
 return(copy->refusals > 0);
}

/**
 * e_change_all_pieces - Replace the occurrence the cursor of the copy @b
 * stands on and every later one in the scope, as Change All does.
 *
 * The search after each replacement reads the text the replacement left,
 * so a replacement that wrapped or shortened its row is searched past
 * exactly as on the user's text.
 */
static void e_change_all_pieces(BUFFER *b, SCHIRM *s, int top, void *plan)
{
 EReplacePlan *p = plan;
 FIND *fd = p->fd;
 int y;

 do
 {
  p->found++;
  y = b->b.y;
  p->last_y = y + top;
  if (e_replace_occurrence(b, s, s->fa.x, y, fd->sn, (unsigned char *)fd->replace, fd->rn))
  {
   p->replaced++;
   e_rep_search_on_after(b, s, fd->sw & E_FIND_BACKWARDS, y);
  }
 } while (!e_change_all_starved(b) && e_find_next(b, s, fd));
}

/**
 * e_change_all_whole - Change All without a prompt, as one change: every
 * occurrence in the scope is replaced, or, when memory runs out, none is.
 * @r: what was done, for the box.
 *
 * The first occurrence is found on the user's text, so a Change All that
 * finds nothing needs no memory.  The rest are found and replaced on the
 * owner's copy of the rows from the first one to the end of the scope.
 * Refused, the text, the cursor and the found text are as the dialog left
 * them, and the status row says Change All was not made.
 */
static void e_change_all_whole(FENSTER *f, FIND *fd, EReplaceReport *r)
{
 BUFFER *b = f->b;
 SCHIRM *s = f->s;
 POINT at = b->b, fa = s->fa, fe = s->fe;
 EReplacePlan p;
 int y0, y1, made;

 if (!e_rep_search(f))
  return;
 e_change_all_rows(b, s, fd, &y0, &y1);
 p.fd = fd;
 p.found = 0;
 p.replaced = 0;
 p.last_y = b->b.y;
 made = e_edit_action_run(b, y0, y1, E_BUFFER_FAULT_CHANGE_ALL, E_CHANGE_ALL_WHAT,
                          e_change_all_pieces, &p);
 if (made != E_BUFFER_OK)
 {
  b->b = at;
  s->fa = fa;
  s->fe = fe;
  r->not_made = 1;
  if (made == E_BUFFER_NOMEM)
   e_status_keep_nomem(E_CHANGE_ALL_WHAT);
  return;
 }
 r->found = p.found;
 r->replaced = p.replaced;
 e_replace_show_hit(f, p.last_y);
}

/**
 * e_replace_report - Tell the user what Search > Replace did: the
 * occurrences found and replaced, and why any were not.
 *
 * A Change All that was not made at all shows no box: the status row
 * already says so, and a count of occurrences nobody replaced would
 * mislead.
 */
static void e_replace_report(FENSTER *f, const EReplaceReport *r)
{
 char line[80];

 if (r->not_made)
  return;
 if (!r->found)
 {
  e_message(0, e_msg[ERR_GETSTRING], f);
  return;
 }
 if (r->out_of_memory)
  sprintf(line, "Found %d\nReplaced %d\n%s", r->found, r->replaced, E_REPLACE_SAY_NOMEM);
 else if (r->read_only)
  sprintf(line, "Found %d\nReplaced %d\n%s", r->found, r->replaced, E_SAY_READ_ONLY);
 else
  sprintf(line, "Found %d\nReplaced %d", r->found, r->replaced);
 e_message(0, line, f);
}

int e_replace(FENSTER *f)
{
 SCHIRM *s;
 BUFFER *b;
 FIND *fd = &(f->ed->fd);
 int i, ret;
 EReplaceReport r;
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
 if (ret == WPE_ESC)
  return(0);
 memset(&r, 0, sizeof r);
 if (ret == AltA && !(fd->sw & E_FIND_PROMPT) && !e_buffer_closed(b))
  e_change_all_whole(f, fd, &r);
 else
  e_replace_each(f, fd, ret, &r);
 e_replace_report(f, &r);
 return(0);
}

