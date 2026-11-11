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
int    e_buffer_split_line(BUFFER *b, int y, int x, WpeBufferStep step, int head_term);
int    e_buffer_detach_lines(BUFFER *b, int y, int n, STRING *out, int *brk_out, int *nbrk);
int    e_buffer_replace_all(BUFFER *b, const char *text, WpeBufferStep step);
int    e_buffer_append_text(BUFFER *b, const char *text, int skip_blank);
int    e_buffer_clear(BUFFER *b);
int    e_buffer_set_line(BUFFER *b, int y, const char *text, int n);
char  *e_buffer_to_text(BUFFER *b);
STRING *e_buffer_lines_from_text(const char *text, int mx_x, int *n);
void   e_buffer_lines_free(STRING *lines, int n);
int    e_buffer_insert_raw(BUFFER *b, int yd);

void   e_buffer_cover_begin(BUFFER *b, Undo *pocket);
void   e_buffer_cover_end(BUFFER *b);
int    e_buffer_covered(BUFFER *b);
void   e_buffer_step_begin(BUFFER *b);
void   e_buffer_step_end(BUFFER *b);
int    e_buffer_depth_check(BUFFER *b);

Undo  *e_buffer_record_new(int kind);
Undo  *e_buffer_record_push(BUFFER *b, Undo *rec);
void   e_buffer_record_free(Undo *rec);
void   e_buffer_shell_free(BUFFER *shell);
int    e_buffer_undo(BUFFER *b, SCHIRM *s);
int    e_buffer_redo(BUFFER *b, SCHIRM *s);
void   e_buffer_history_drop(BUFFER *b);
void   e_buffer_mark_view(BUFFER *b);

/* The editor's side: a link seam, defined in we_edit.c and we_debug.c and
   stubbed by the C test.  A link seam cannot be unset, and the first file is
   read before the first window exists. */
void   wpe_buffer_syntax_changed(BUFFER *b, int y);
void   wpe_buffer_lines_moved(BUFFER *b, int y, int n_del, int n_ins,
                              const int *rows_removed, int n_removed,
                              const int *rows_added, int n_added);
void   wpe_buffer_say_nomem(BUFFER *b, const char *what);
int    wpe_buffer_replay_kind(BUFFER *b, SCHIRM *s, Undo *rec);
void   wpe_buffer_trace(const char *fmt, ...);

int    e_str_len(unsigned char *s);
void   e_buffer_test_fail_allocs(int n);

/* The cover the block code still spells as a global until its own pass:
   while it is set no splice and no record is kept on any buffer. */
extern int e_undo_sw;

#endif
