/* we_buffer.h -- the text a window shows has one owner.
   Copyright (C) 2026 Juan Manuel Mendez Rey
   This file is part of xwpe and is distributed under the GNU General Public
   License, version 2 or later.

   Every change to the lines of a buffer goes through here, and the window's
   undo, dirty mark, block marks, breakpoint lines and syntax state move with
   the lines.  The characters inside a line stay with the char primitives of
   we_edit.c, which call the owner for every change of the line count and
   push their records through it. */

#ifndef WE_BUFFER_H
#define WE_BUFFER_H

#include "edit.h"

#define E_BUFFER_OK              0
#define E_BUFFER_NOMEM         (-1)  /* nothing changed: lines, marks, lists as they were */
#define E_BUFFER_RANGE         (-2)  /* y past the end, or a negative count: nothing changed */
#define E_BUFFER_NOTHING       (-3)  /* Undo or Redo with an empty list */
#define E_BUFFER_LOCKED        (-4)  /* the window is closed to the user's changes:
                                        nothing changed, the status row says so */
#define E_BUFFER_SAVE_PER_LINE  10   /* what a line counts toward f->save */

/* The most edits (lines taken out plus lines put in) the diff of a
   whole-buffer swap looks for before it gives up and takes what is left as
   one changed hunk.  The search costs time in proportion to this bound times
   the number of lines, and memory in proportion to its square. */
#ifndef E_BUFFER_DIFF_MAX
#define E_BUFFER_DIFF_MAX     2000
#endif

/* b->history */
#define E_BUFFER_HISTORY_FULL    0   /* a file: undo, dirty mark, breakpoints */
#define E_BUFFER_NO_HISTORY      1   /* a view the editor writes: no record, no dirty
                                        count, no breakpoint rows; marks still kept */

/* The room of a row of a buffer W wide: W bytes of text, the line break,
   the terminating zero.  Every row the owner makes has it. */
#define E_BUFFER_ROW_BYTES(w)   ((w) + 2)
/* The most bytes of text a row of a file holds: Max. Columns. */
#define E_BUFFER_ROW_TEXT_MAX(b) ((b)->mx.x)
/* The column past a full row's line break: a whole-file write passes it as
   its end column, so the break after a row of Max. Columns bytes is written. */
#define E_BUFFER_ROW_END(b)     ((b)->mx.x + 1)

/* what ends the head of a split line */
#define E_BUFFER_HEAD_OPEN       0   /* unterminated: the block code inserts the rest */
#define E_BUFFER_HEAD_WR         1   /* ends with the line break (Enter) */

/* b->replaying: the list a replay's inverses go to */
#define E_BUFFER_REPLAY_NONE     0
#define E_BUFFER_REPLAY_UNDO     1   /* inverses onto b->rd */
#define E_BUFFER_REPLAY_REDO     2   /* inverses onto b->ud */

/* Undo.type of a line record, and Undo.flags */
#define E_UNDO_LINES           'R'
#define E_UNDO_MORE              1   /* the record below this one belongs to the same step */
#define E_UNDO_EMPTIED           2   /* the canonical empty line is no changed row */
#define E_UNDO_STEP_BLOCK        4   /* blk_before and blk_after hold the step's block */

/* how a splice counts for Ctrl-U */
typedef enum {
 E_BUFFER_STEP_NEW,      /* its own 'R' record (one Ctrl-U, or part of an open step) */
 E_BUFFER_STEP_COVERED,  /* no record: the caller's own record replays it */
 E_BUFFER_STEP_REPLAY    /* Undo/Redo: exactly one inverse on the list the replay names */
} WpeBufferStep;

void   e_buffer_init(BUFFER *b, struct CNT *cn, struct FNST *f, int mx_x, int history);
int    e_buffer_line_new(BUFFER *b, STRING *out);

int    e_buffer_splice(BUFFER *b, int y, int n_del, STRING *lines, int n_ins,
                       const int *brk, int nbrk, WpeBufferStep step);
int    e_buffer_insert_blank(BUFFER *b, int y, WpeBufferStep step);
int    e_buffer_delete_lines(BUFFER *b, int y, int n, WpeBufferStep step);
int    e_buffer_open_row(BUFFER *b, int y);
int    e_buffer_split_line(BUFFER *b, int y, int x, WpeBufferStep step, int head_term);
int    e_buffer_detach_lines(BUFFER *b, int y, int n, STRING *out, int *brk_out, int *nbrk);
int    e_buffer_replace_all(BUFFER *b, const char *text, WpeBufferStep step);
int    e_buffer_append_text(BUFFER *b, const char *text, int skip_blank);
int    e_buffer_append_rows(BUFFER *b, const char *text);
int    e_buffer_clear(BUFFER *b);
/* A row set with either of these keeps all @n bytes, even past the buffer's
   width: they are for views (the assistant's pane wraps a long row when it
   paints).  On a buffer that is not a view a row wider than the width is
   refused with E_BUFFER_RANGE and nothing changes. */
int    e_buffer_set_line(BUFFER *b, int y, const char *text, int n);
int    e_buffer_set_line_ended(BUFFER *b, int y, const char *text, int n, int term);
char  *e_buffer_to_text(BUFFER *b);
STRING *e_buffer_lines_from_text(const char *text, int mx_x, int *n);
void   e_buffer_lines_free(STRING *lines, int n);
int    e_buffer_rows_push(STRING **lines, int *n, int *cap, int mx_x,
                          const unsigned char *str, int len, int term);
int    e_buffer_wrap_cut(const unsigned char *row, int maxc);
int    e_buffer_bytes_reserve(unsigned char **bytes, int *cap, int need);
int    e_buffer_line_copy(BUFFER *b, const STRING *src, STRING *out);
int    e_buffer_rows_check(BUFFER *b);
void   e_buffer_marks_inside(BUFFER *b);

/* ---- a change made of several splices, whole or not at all ----

   A block move takes lines out of one place and puts them, and the parts of
   two lines, somewhere else, maybe in another buffer.  Every splice of it is
   prepared first (all the memory it needs is taken, nothing moves); only
   when all of them could be prepared do they commit, and a commit cannot
   fail.  So a move refused for want of memory leaves both buffers, their
   marks, breakpoints and undo exactly as they were.  The coordinates of an
   item are those its splice meets when it commits, after the items before
   it. */

/* What one splice needs from memory, taken before anything moves. */
typedef struct {
 STRING canon;        /* the canonical empty line of a buffer emptied */
 int need_canon;
 Undo *rec;           /* the line record, when this splice records */
 int *removed;        /* rows of removed lines, absolute, before */
 int *added;          /* rows set from brk[], absolute, after */
 int nremoved, nadded;
} BufferSpliceMem;

#define E_BUFFER_TXN_SPLICES  12   /* splices held in the transaction itself; more
                                      (one per hunk of a format's Undo) take memory */
#define E_BUFFER_TXN_BUFFERS   2   /* the buffer a block leaves and the one it reaches */

/* WpeBufferTxnSplice.kind: how a splice of a transaction commits. */
#define E_BUFFER_ITEM_SPLICE   0   /* lines the caller built (or a move's halves) */
#define E_BUFFER_ITEM_REPLAY   1   /* the lines of a line record, put back by Undo or Redo */
#define E_BUFFER_ITEM_REWRITE  2   /* the lines a shell holds after a replay ran on it */

/* One splice of a transaction, prepared and waiting to commit. */
typedef struct {
 BUFFER *b;
 int y, n_del, n_ins;
 STRING *lines;       /* what it puts in: the transaction's until commit */
 WpeBufferStep step;
 Undo *pocket;        /* where its removed rows go under a char record's cover */
 int detach;          /* a move's first half: the lines go to out[], not freed */
 STRING *out;         /* a detach: the lines taken out */
 int *brk_out;        /* ... and their breakpoint rows, relative to y */
 int nbrk_out;
 int landed_from;     /* a move's second half: the detach it lands, else -1 */
 int kind;            /* E_BUFFER_ITEM_* */
 int no_changed;      /* no row of the span is a changed row that keeps its breakpoint */
 const int *brk;      /* rows the new lines carry, relative to y: a replay's are its
                         record's, a rewrite's its own copy */
 int nbrk;
 Undo *rec;           /* a replay: the record whose lines come back */
 int lines_kept;      /* the lines are the record's: an abort leaves them to it */
 BUFFER *shell;       /* a rewrite: the shell whose lines it adopts */
 int g_top, g_bot;    /* ... the shell's guard rows above and below the span (0 or 1) */
 BufferSpliceMem m;
} WpeBufferTxnSplice;

/* A buffer a transaction changes: its line count as the items so far leave
   it, and the breakpoint and pocket rows they may add. */
typedef struct {
 BUFFER *b;
 int lines;
 Undo *pocket;
 int brp_rows, pocket_rows;
} WpeBufferTxnBuffer;

/* A transaction lives on its caller's stack and is never copied: sp points
   into it until it grows past its inline slots. */
typedef struct {
 WpeBufferTxnSplice *sp;   /* sp_inline, or the heap once more were needed */
 int nsp, cap;
 WpeBufferTxnSplice sp_inline[E_BUFFER_TXN_SPLICES];
 WpeBufferTxnBuffer buf[E_BUFFER_TXN_BUFFERS];
 int nbuf;
 Undo *rec;           /* the record of the operation, reserved with the rest */
 int refused;         /* E_BUFFER_OK, or why an item was refused */
} WpeBufferTxn;

/* A run of bytes: one piece of a line a transaction builds. */
typedef struct {
 const unsigned char *s;
 int n;
} WpeBufferPart;

void   e_buffer_txn_begin(WpeBufferTxn *t);
int    e_buffer_txn_splice(WpeBufferTxn *t, BUFFER *b, int y, int n_del, STRING *lines,
                           int n_ins, WpeBufferStep step);
int    e_buffer_txn_splice_parts(WpeBufferTxn *t, BUFFER *b, int y, int n_del,
                                 const WpeBufferPart *parts, int nparts, WpeBufferStep step);
int    e_buffer_txn_move_lines(WpeBufferTxn *t, BUFFER *from, int y_from, int n,
                               BUFFER *to, int y_to);
int    e_buffer_txn_delete_lines(WpeBufferTxn *t, BUFFER *b, int y, int n, WpeBufferStep step);
int    e_buffer_txn_copy_lines(WpeBufferTxn *t, BUFFER *to, int y, BUFFER *src, int from,
                               int n, WpeBufferStep step);
int    e_buffer_txn_replay_lines(WpeBufferTxn *t, BUFFER *b, Undo *rec);
int    e_buffer_txn_rewrite(WpeBufferTxn *t, BUFFER *b, int y, int n_del, BUFFER *shell,
                            int g_top, int g_bot, const int *brk, int nbrk);
int    e_buffer_txn_line(WpeBufferTxn *t, BUFFER *b, int y, const STRING **line,
                         int *has_row);
Undo  *e_buffer_txn_record(WpeBufferTxn *t, int kind);
int    e_buffer_txn_commit(WpeBufferTxn *t);
void   e_buffer_txn_abort(WpeBufferTxn *t);
STRING *e_buffer_lines_from_parts(int mx_x, const WpeBufferPart *parts, int nparts, int *n);
STRING *e_buffer_lines_copy(BUFFER *src, int from, int n, BUFFER *dst, int *n_out);

/* ---- a shell: a private buffer the owner makes ----

   A block delete keeps its lines in one until Undo brings them back; a
   character Undo replays the primitive on one holding copies of the rows it
   may touch, so a replay refused for memory costs the user nothing.  A
   shell has a window and a screen of its own (for its breakpoint rows and
   marks) but no file name, so no debugger is ever told of it. */
BUFFER *e_buffer_shell_new(BUFFER *like, int width);
int    e_buffer_shell_fill(BUFFER *shell, WpeBufferTxn *t, BUFFER *b, int y0, int n);
int    e_buffer_is_shell(BUFFER *b);
void   e_buffer_shell_refused(BUFFER *b);

/* ---- an editing action of several pieces, whole or not at all ----

   Tab and Backspace with auto-indent, Enter, Ctrl-K I and U, Ctrl-O U, M
   and H and Change All change the text in several pieces, each a character
   primitive.  The action asks the owner for a shell over the rows it may
   touch, runs its own pieces on the shell, and commits: the shell's rows
   become the user's in one rewrite, with the pieces' Undo records, the
   cursor and the marks the pieces left.  A piece refused for want of memory
   refuses the whole action, and the user's text, marks, breakpoints and
   both Undo lists stay as they were; the status row says so once. */
typedef struct {
 WpeBufferTxn t;
 BUFFER *b;           /* the user's buffer */
 BUFFER *shell;       /* the rows the pieces run on, until the commit */
 int y0, n_del;       /* the span: rows [y0, y0 + n_del) of b */
 int g_top, g_bot;    /* the shell's guard rows above and below the span (0 or 1) */
 int fault;           /* what e_buffer_test_fault_enter returned */
 const char *what;    /* the action, in the user's words, for the status row */
} WpeBufferAction;

int    e_buffer_action_begin(WpeBufferAction *a, BUFFER *b, int y0, int y1, int point,
                             const char *what);
int    e_buffer_action_commit(WpeBufferAction *a);
void   e_buffer_action_abort(WpeBufferAction *a);

void   e_buffer_cover_begin(BUFFER *b, Undo *pocket);
void   e_buffer_cover_end(BUFFER *b);
int    e_buffer_covered(BUFFER *b);
void   e_buffer_step_begin(BUFFER *b);
void   e_buffer_step_end(BUFFER *b);
int    e_buffer_depth_check(BUFFER *b);

Undo  *e_buffer_record_new(int kind);
int    e_buffer_record_text(Undo *rec, const unsigned char *src, int n);
Undo  *e_buffer_record_push(BUFFER *b, Undo *rec);
void   e_buffer_record_free(Undo *rec);
void   e_buffer_shell_free(BUFFER *shell);
int    e_buffer_undo(BUFFER *b, SCHIRM *s);
int    e_buffer_redo(BUFFER *b, SCHIRM *s);
/* Make the character change record @rec describes, whole or not at all
   (E_BUFFER_OK, E_BUFFER_NOMEM or E_BUFFER_RANGE, nothing changed). */
int    e_buffer_apply_chars(BUFFER *b, SCHIRM *s, const Undo *rec);
void   e_buffer_history_drop(BUFFER *b);
void   e_buffer_mark_view(BUFFER *b);
/* Is @b closed to the changes the user's commands make (a file opened
   read-only, the clipboard while its viewer is in front)?  Says so on the
   status row.  Every character primitive and every recorded splice asks. */
int    e_buffer_change_refused(BUFFER *b);
int    e_buffer_closed(BUFFER *b);

/* The editor's side: a link seam, defined in we_edit.c and we_debug.c and
   stubbed by the C test.  A link seam cannot be unset, and the first file is
   read before the first window exists. */
void   wpe_buffer_syntax_changed(BUFFER *b, int y);
void   wpe_buffer_lines_moved(BUFFER *b, int y, int n_del, int n_ins,
                              const int *rows_removed, int n_removed,
                              const int *rows_added, int n_added);
void   wpe_buffer_say_nomem(BUFFER *b, const char *what);
void   wpe_buffer_say_shape(BUFFER *b, const char *what);
void   wpe_buffer_say_read_only(BUFFER *b);
int    wpe_buffer_replay_kind(BUFFER *b, SCHIRM *s, Undo *rec);
/* Run the character primitive that replays @rec ('a' 'r' 's' 'p') on a
   shell whose row 0 is row @y0 of the buffer the record belongs to; a
   refusal inside counts on the shell (e_buffer_shell_refused). */
void   wpe_buffer_replay_on_shell(BUFFER *shell, SCHIRM *s, const Undo *rec, int y0);
void   wpe_buffer_trace(const char *fmt, ...);

int    e_str_len(unsigned char *s);
void   e_buffer_test_fail_allocs(int n);

/* Tests only: memory refused inside one action of the running editor.  A
   point is armed once at startup ($XWPE_TEST_FAULT, "<name>:<n>"); the first
   action that enters it has every owner allocation from its n-th on refused,
   until it leaves; then memory is back and the point never fires again. */
#define E_BUFFER_FAULT_NONE        0
#define E_BUFFER_FAULT_BLOCK_MOVE  1   /* "block-move": a block move or delete */
#define E_BUFFER_FAULT_UNDO        2   /* "undo": one Ctrl-U */
#define E_BUFFER_FAULT_REDO        3   /* "redo": one Ctrl-R */
#define E_BUFFER_FAULT_BLOCK_COPY  4   /* "block-copy": a block copy or paste */
#define E_BUFFER_FAULT_READ        5   /* "read": reading a file into a window */
#define E_BUFFER_FAULT_INDENT      6   /* "indent": Tab or Backspace with auto-indent */
#define E_BUFFER_FAULT_ENTER       7   /* "enter": Enter, with its auto-indent */
#define E_BUFFER_FAULT_SHIFT       8   /* "shift": Ctrl-K I or Ctrl-K U */
#define E_BUFFER_FAULT_HELP_MARKS  9   /* "help-marks": Ctrl-O U, M or H */
#define E_BUFFER_FAULT_CHANGE_ALL 10   /* "change-all": Search > Replace */
#define E_BUFFER_FAULT_FIRED       1   /* the token of the action that fired it */

int    e_buffer_test_fault_point(const char *name);
void   e_buffer_test_fault_arm(int point, int n);
int    e_buffer_test_fault_enter(int point);
void   e_buffer_test_fault_leave(int token);

#endif
