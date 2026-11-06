#ifndef WPE_DEBSESS_H
#define WPE_DEBSESS_H

/* The state of a debugging session, and the readers of the debuggers' text
   that fill it, apart from drawing.

   Everything here is plain C over strings: no curses, no X11, no Wayland,
   no editor header, no descriptor and no global of the editor.  The file
   system is consulted only through a probe the caller hands in.  That is
   what lets tests/test_debsess.c feed it the words real debuggers print and
   check what the editor makes of them, without starting an editor or a
   debugger; we_debug.c calls the same functions, so the code tested is the
   code the user runs. */

#include <stddef.h>
#include <limits.h>
#include "we_debreq.h"
#include "we_gdbmi.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Which debugger printed the text: the values of we_debug.c's DEB_* (they
   are persisted in the options file, so they never change). */
#define E_DS_GDB   0
#define E_DS_SDB   1
#define E_DS_DBX   2
#define E_DS_XDB   3
#define E_DS_JDB   4
#define E_DS_PDB   5
#define E_DS_A68G  6
#define E_DS_DAP   7

/* Sizes of what a session keeps. */
#define E_DS_PATH_MAX     PATH_MAX   /* a source file a frame names */
#define E_DS_NAME_MAX     128        /* a function, a signal, a class */
#define E_DS_LINE_MAX     512        /* an error the debugger gave */
#define E_DS_FRAMES_MAX   64         /* frames past this are not kept */
#define E_DS_WATCHES_MAX  64         /* watches past this are not kept */
#define E_DS_BRKS_MAX     128        /* breakpoints past this are not kept */
#define E_DS_EXPR_MAX     256        /* a watch expression */
#define E_DS_VALUE_MAX    512        /* a watch value, cut past this */
#define E_DS_NO_LINE      0          /* a frame without a source line */
#define E_DS_NOT_FOUND    (-1)       /* no frame, no line, no number */
#define E_DS_CONSOLE_MAX  8192       /* gdb's words kept for Messages until shown */

/* What the debugger said of a breakpoint: the hollow, solid and refused
   marks the user sees in the margin. */
#define E_D_BRK_PENDING   0   /* not told to the debugger, or not confirmed */
#define E_D_BRK_VERIFIED  1   /* the debugger has it */
#define E_D_BRK_REFUSED   2   /* the debugger said no */
#define E_D_BRK_MSG_MAX   256
/* What the session says when a debugger gave no breakpoint number. */
#define E_D_BRK_NO_NUMBER "the debugger gave no breakpoint number"
/* The symbols a debugger puts before its words start here. */
#define E_D_FIRST_NON_ASCII 0x80

/* gdb's answer to a breakpoint set: "Breakpoint 2 at ...", its number next. */
#define E_D_GDB_BRK_ANSWER  "Breakpoint "
/* xdb says this and gives the number on the line after it. */
#define E_D_XDB_BRK_ADDED   "Added:"
/* dbx answers a breakpoint set with its number first: "(2) stop at ...". */
#define E_D_DBX_BRK_OPEN    '('

/* The words a watch shows when the debugger has no value for it. */
#define E_DS_NO_SYMBOL      "No symbol in current context\n"
#define E_DS_NOT_IN_SCOPE   "<not in scope>"

/* jdb's words. */
#define E_D_JDB_EXIT        "The application exited"
#define E_D_JDB_DISCONNECTED "The application has been disconnected"
#define E_D_JDB_STOP_MARK   "Breakpoint hit:"
#define E_D_JDB_STEP_MARK   "Step completed:"
#define E_D_JDB_EXCEPTION_MARK "Exception occurred:"
#define E_D_JDB_PROMPT_TAIL "] "
#define E_D_JDB_BARE_PROMPT "> "
#define E_D_JDB_SUSPENDED   "All threads suspended."
#define E_D_JDB_ECHO        "> "
#define E_D_JDB_LINE_MARK   "line="
#define E_D_JDB_NESTED_SEP  '$'      /* P$Inner lives in P.java */
#define E_D_JDB_SET_NOW     "Set breakpoint "
#define E_D_JDB_DEFERS      "Deferring breakpoint "
#define E_D_JDB_UNABLE      "Unable to set breakpoint "

/* The kinds of line an a68g stop is made of, and how many come before its
   prompt: the numbered source line, the column marker, the thread line. */
#define E_D_A68G_LINE_OTHER    0
#define E_D_A68G_LINE_SOURCE   1
#define E_D_A68G_LINE_MARKER   2
#define E_D_A68G_LINE_THREAD   3
#define E_D_A68G_LINE_QUESTION 4   /* the terminate question, not a stop */
#define E_D_A68G_STOP_LINES    3
#define E_D_A68G_MAIN_FRAME    "(main program)"
/* What a68g asks, with no line end, when SIGINT interrupts the program. */
#define E_D_A68G_TERMINATE     "Terminate a68g (yes|no):"

/* Where the session is. */
#define E_DS_STARTING   0
#define E_DS_STOPPED    1
#define E_DS_RUNNING    2
#define E_DS_ANSWERING  3
#define E_DS_ENDED      4

/* Why the program stopped. */
#define E_DS_STOP_UNKNOWN    0
#define E_DS_STOP_BREAKPOINT 1
#define E_DS_STOP_STEP       2
#define E_DS_STOP_SIGNAL     3
#define E_DS_STOP_PAUSE      4
#define E_DS_STOP_EXITED     5

/* A watch's state. */
#define E_DS_WATCH_WAITING 0   /* no answer yet: it comes at the next stop */
#define E_DS_WATCH_VALUE   1
#define E_DS_WATCH_ERROR   2

/* What changed since the view last looked, one bit each. */
#define E_DS_CHANGED_MARK    0x01u
#define E_DS_CHANGED_WATCHES 0x02u
#define E_DS_CHANGED_STACK   0x04u
#define E_DS_CHANGED_BRKS    0x08u
#define E_DS_CHANGED_STATUS  0x10u
#define E_DS_CHANGED_ENDED   0x20u
#define E_DS_CHANGED_CONSOLE 0x40u

/* Why the editor interrupted the program, if it did: the controller's
   E_D_INT_* values.  A SIGINT stop is the user's Pause, or the editor's own
   stop to apply a breakpoint change, or the program's own. */
#define E_DS_INT_NONE   0
#define E_DS_INT_PAUSE  1
#define E_DS_INT_SYNC   2

typedef struct
{
 char file[E_DS_PATH_MAX];   /* "" when the debugger names none */
 int line;                   /* 1-based, or E_DS_NO_LINE */
 int level;                  /* 0 is the innermost */
 char func[E_DS_NAME_MAX];
} E_DS_FRAME;

typedef struct
{
 char expr[E_DS_EXPR_MAX];
 char value[E_DS_VALUE_MAX];
 int state;                  /* E_DS_WATCH_* */
} E_DS_WATCH;

typedef struct
{
 char file[E_DS_PATH_MAX];
 int line, id, state;        /* id: the debugger's number, 0 for none */
 char msg[E_D_BRK_MSG_MAX];
} E_DS_BRK;

/* The file system, as the caller sees it: 1 when @file is there. */
typedef int (*E_DS_FILE_PROBE)(const char *file, void *ctx);

typedef struct
{
 int phase;                  /* E_DS_STARTING .. E_DS_ENDED */
 int stop_reason;            /* E_DS_STOP_* */
 char signal[E_DS_NAME_MAX];
 int exit_code;
 E_DS_FRAME frames[E_DS_FRAMES_MAX];
 int nframes;
 int marked_frame;
 E_DS_WATCH watches[E_DS_WATCHES_MAX];
 int nwatches;
 E_DS_BRK brks[E_DS_BRKS_MAX];
 int nbrks;
 char last_error[E_DS_LINE_MAX];
 unsigned changed;           /* E_DS_CHANGED_* */
 E_DS_FILE_PROBE is_file;    /* NULL: every named file counts as there */
 void *is_file_ctx;
 int interrupt_for;          /* E_DS_INT_*: the interrupt in flight */
 int own_stop;               /* 1: the last stop was the editor's own interrupt */
 char console[E_DS_CONSOLE_MAX]; /* gdb's words not yet shown in Messages */
 int console_len;
 int console_lost;           /* bytes that did not fit and were dropped */
} E_D_SESSION;

/* ---- the session ---- */

/**
 * e_ds_reset - Forget everything of the session before.
 * @s: the session.
 *
 * A new debugging session starts with no stop, no frames, no values and no
 * answers, so nothing the user saw in the last one is shown as this one's.
 * The file probe is kept.
 */
void e_ds_reset(E_D_SESSION *s);

/**
 * e_ds_set_file_probe - Tell the session how to ask whether a file exists.
 * @s: the session; @probe: the question, NULL to take every name as there;
 * @ctx: handed to @probe.
 *
 * pdb's stop line may hold "(1)" in a directory name; the one that names an
 * existing file is the stop, so the user's bar lands in the right file.
 */
void e_ds_set_file_probe(E_D_SESSION *s, E_DS_FILE_PROBE probe, void *ctx);

/**
 * e_ds_set_watch - Put watch @i in the list, waiting for its value.
 * @s: the session; @i: its number; @expr: what the user typed.
 *
 * The user sees the watch listed at once, its value to come at the stop.
 */
void e_ds_set_watch(E_D_SESSION *s, int i, const char *expr);

/**
 * e_ds_set_brk - Put breakpoint @i in the list, not yet confirmed.
 * @s: the session; @i: its number in the editor's list; @file, @line: where.
 *
 * The line shows hollow until the debugger says it has it.
 */
void e_ds_set_brk(E_D_SESSION *s, int i, const char *file, int line);

/**
 * e_ds_take_cli_stop_line - Read where the program stopped from the
 * debugger's own report of the stop.
 * @s: the session; @backend: E_DS_*; @text: the report (gdb's "Breakpoint
 * 1, f () at p.c:3", pdb's answer, jdb's answer, a68g's numbered line).
 *
 * The user's bar goes to the file and line the debugger names.
 * Return: 1 when the report names a place, else 0 (the session is unchanged).
 */
int e_ds_take_cli_stop_line(E_D_SESSION *s, int backend, const char *text);

/**
 * e_ds_begin_stack - Forget the frames before a new stack listing is read.
 * @s: the session.
 */
void e_ds_begin_stack(E_D_SESSION *s);

/**
 * e_ds_take_where_row - Add the frame one row of the stack listing names.
 * @s: the session; @backend: E_DS_*; @row: one row of "where" (or sdb's
 * and xdb's "t", a68g's "calls").
 *
 * The Stack window and the stop's mark come from these frames.
 * Return: 1 when the row is a frame, else 0.
 */
int e_ds_take_where_row(E_D_SESSION *s, int backend, const char *row);

/**
 * e_ds_answer_borrow - Present text the caller holds as a debugger answer.
 * @a: the answer to fill; @text: the answer's lines, joined, or NULL when
 * the debugger gave nothing before its prompt; @error: 1 when the text came
 * from the debugger's error stream.
 *
 * The editor's own line readers hand their answers to the session in the
 * same shape the request engine will, so the session reads both alike.
 * The text is only read, never grown or freed.
 */
void e_ds_answer_borrow(E_D_ANSWER *a, const char *text, int error);

/**
 * e_ds_take_watch_answer - Take the debugger's answer to watch @i's print.
 * @s: the session; @backend: E_DS_*; @i: the watch; @a: the answer, its
 * lines joined, with no text when none came, its error flag set when the
 * debugger said it has no value.
 *
 * The user reads the value, or the debugger's reason for having none.
 */
void e_ds_take_watch_answer(E_D_SESSION *s, int backend, int i,
                            const E_D_ANSWER *a);

/**
 * e_ds_take_brk_answer - Take the debugger's answer to setting breakpoint @i.
 * @s: the session; @backend: E_DS_*; @i: the breakpoint; @a: the line that
 * carries its number (gdb, dbx, xdb) or jdb's word on it; an error line,
 * with the error flag; no text when the debugger gave nothing.
 *
 * The user's mark turns solid when the debugger has the breakpoint, and
 * stays hollow with the debugger's words when it does not.
 */
void e_ds_take_brk_answer(E_D_SESSION *s, int backend, int i,
                          const E_D_ANSWER *a);

/**
 * e_ds_take_dap_stopped - Take a debug adapter's stop and its stack.
 * @s: the session; @reason: the adapter's "reason"; @fr, @n: its frames,
 * innermost first.
 */
void e_ds_take_dap_stopped(E_D_SESSION *s, const char *reason,
                           const E_DS_FRAME *fr, int n);

/**
 * e_ds_set_interrupt - Say why the editor is about to interrupt the program.
 * @s: the session; @why: E_DS_INT_*, or E_DS_INT_NONE to take it back.
 *
 * The stop that answers the interrupt is then told from one the program
 * brought on itself: the user's Pause is shown, the editor's own stop to
 * apply a breakpoint change is not.
 */
void e_ds_set_interrupt(E_D_SESSION *s, int why);

/**
 * e_ds_take_mi_record - Take one record of gdb's MI output.
 * @s: the session; @r: the record.
 *
 * A stop gives the user's bar its file, line and function and the Stack
 * window its frame; the exit gives the status row the program's exit code;
 * a SIGINT stop is the user's Pause or the editor's own interrupt by what
 * e_ds_set_interrupt said; an error answer gives the status row gdb's own
 * message; gdb's console, target and log words wait for Messages in
 * s->console.  Answers that belong to one request (a breakpoint's, a
 * watch's) are taken by the calls below, which know which entry asked.
 * Return: 1 when the record changed the session, else 0.
 */
int e_ds_take_mi_record(E_D_SESSION *s, const E_GDBMI_RECORD *r);

/**
 * e_ds_take_mi_brk_result - Take gdb's answer to setting breakpoint @i.
 * @s: the session; @i: the breakpoint; @r: the result record of the
 * -break-insert that asked (the caller matched it by its token).
 *
 * The user's mark turns solid when gdb has the breakpoint, stays hollow
 * when gdb holds it pending (the file is not loaded yet), and stays hollow
 * with gdb's own message when gdb refuses it.
 * Return: 1 when @r answers a breakpoint set, else 0.
 */
int e_ds_take_mi_brk_result(E_D_SESSION *s, int i, const E_GDBMI_RECORD *r);

/**
 * e_ds_mi_watch_text - A watch's answer under MI, in the shape the Watches
 * window reads under gdb's line interface ("= 3").
 * @r: the result record of -data-evaluate-expression; @out, @n: the text.
 *
 * The Watches window then shows the same line whichever interface gdb
 * speaks.
 * Return: 1 with the value in @out, 0 when gdb has none (an error answer):
 * the watch then shows its own words for that.
 */
int e_ds_mi_watch_text(const E_GDBMI_RECORD *r, char *out, size_t n);

/**
 * e_ds_mi_where_rows - The Stack window's rows from -stack-list-frames
 * and -stack-list-arguments 1, in the form gdb's "bt" prints them:
 * "#1  0x0000555555555166 in main (argc=1) at p.c:14".
 * @frames: the frames' result record; @args: the arguments' result record,
 * or NULL; @out, @n: the rows, each ending in a line end, cut at a whole
 * row.
 *
 * Enter on a row reads its "at file:line" as it does under the line
 * interface, so following a frame works the same.
 * Return: the number of rows, 0 when the record holds no stack.
 */
int e_ds_mi_where_rows(const E_GDBMI_RECORD *frames,
                       const E_GDBMI_RECORD *args, char *out, size_t n);

/**
 * e_ds_console_clear - Forget the words Messages has shown.
 * @s: the session.
 */
void e_ds_console_clear(E_D_SESSION *s);

/**
 * e_ds_frame_to_mark - The frame whose line the user's bar goes to.
 * @s: the session; @has_source: 1 when a file can be shown here, NULL to
 * take the first frame that names a line; @ctx: handed to it.
 *
 * After a stop in library code with no source here (a sleep, a lock) the
 * innermost frame of the user's own code is marked, as the Stack window
 * shows it.
 * Return: its index, or E_DS_NOT_FOUND.
 */
int e_ds_frame_to_mark(const E_D_SESSION *s,
                       int (*has_source)(const char *file, void *ctx),
                       void *ctx);

/* ---- the readers, one per thing a debugger prints ---- */

/**
 * e_ds_line_num - The source line, and into @file the file, a gdb, dbx or
 * xdb line names; for sdb only the line ("file:line"), its file being asked
 * for apart (see e_ds_sdb_file_of).
 * @backend: E_DS_*; @str: the line, of any length; @file, @n: the file.
 * Return: the line, or -1 when @str names none.
 */
int e_ds_line_num(int backend, const char *str, char *file, size_t n);

/**
 * e_ds_line_num2 - The line and file of an sdb or xdb "t" row:
 * ">  main [p.c:7]".
 * @str: the row; @file, @n: the file.
 * Return: the line, or -1.
 */
int e_ds_line_num2(const char *str, char *file, size_t n);

/**
 * e_ds_sdb_file_of - The file of sdb's answer to "e": "\"p.c\"".
 * @answer: the answer; @file, @n: the file, "" when it names none.
 */
void e_ds_sdb_file_of(const char *answer, char *file, size_t n);

/**
 * e_ds_where_row_frame - The frame number a gdb "where" row starts with.
 * @row: "#2  0x... in tick (n=0) at s.c:4".
 * Return: the number, or E_DS_NOT_FOUND when the row has none.
 */
int e_ds_where_row_frame(const char *row);

/**
 * e_ds_watch_value - What a watch shows after its expression.
 * @backend: E_DS_*; @answer: the debugger's answer, its lines joined, or
 * NULL when it gave none; @text: set to the words to show (inside @answer,
 * or one of the session's own words).
 * Return: E_DS_WATCH_VALUE, or E_DS_WATCH_ERROR when the debugger has none.
 */
int e_ds_watch_value(int backend, const char *answer, const char **text);

/**
 * e_ds_brk_is_number_line - Is this the line of gdb's, dbx's or xdb's answer
 * to a breakpoint set that carries its number (for xdb: the "Added:" line
 * before it)?
 * @backend: E_DS_GDB, _DBX or _XDB; @line: one line of the answer.
 */
int e_ds_brk_is_number_line(int backend, const char *line);

/**
 * e_ds_brk_number - The breakpoint number on that line.
 * @backend: E_DS_GDB, _DBX or _XDB; @line: "Breakpoint 2 at ...", "(2) ...",
 * "b2 ...".
 */
int e_ds_brk_number(int backend, const char *line);

/**
 * e_ds_brk_words - A debugger's reply as the status row shows it: without
 * the marks it puts before its words and without its line end.
 * @out, @size: where the words go; @line: the reply.
 */
void e_ds_brk_words(char *out, size_t size, const char *line);

/**
 * e_ds_pdb_stop_parse - Read a stop line in pdb's grammar at @p:
 * "> FILENAME(LINENO)FUNCNAME()", then the line's end or "->REPR".
 * @p: a "> " in pdb's answer; @file, @n: the file; @line: the line;
 * @is_file, @ctx: the file probe (NULL: every name is there).
 * Return: 1 when @p begins such a line.
 */
int e_ds_pdb_stop_parse(const char *p, char *file, size_t n, int *line,
                        E_DS_FILE_PROBE is_file, void *ctx);

/**
 * e_ds_pdb_find_stop - Where the line pdb names its stop with starts.
 * @text: a part of pdb's answer; @is_file, @ctx: the file probe.
 * Return: its "> ", or NULL.
 */
const char *e_ds_pdb_find_stop(const char *text, E_DS_FILE_PROBE is_file,
                               void *ctx);

/**
 * e_ds_pdb_stop_place - Where pdb stopped, from its answer.
 * @text: the answer; @file, @n: the file; @line: the line; @is_file, @ctx:
 * the file probe.
 * Return: 1 when the answer names a place.
 */
int e_ds_pdb_stop_place(const char *text, char *file, size_t n, int *line,
                        E_DS_FILE_PROBE is_file, void *ctx);

/**
 * e_ds_pdb_frame_parse - Read one frame of pdb's "where".
 * @row: "> /dir/p.py(2)sq()->9" or "  /dir/p.py(6)<module>()".
 * @fr: receives the function, file and line.
 * Return: 1 when @row is a frame.
 */
int e_ds_pdb_frame_parse(const char *row, E_DS_FRAME *fr);

/**
 * e_ds_jdb_stop_line - The source line a jdb stop names ("line=1,004"
 * included), or 0.
 */
int e_ds_jdb_stop_line(const char *stop);

/**
 * e_ds_jdb_stop_class - The class a jdb stop names, with its package; a
 * nested class gives its outer one.
 * @stop: what jdb printed; @out, @n: receives "com.x.P".
 * Return: 1 when the stop names a class.
 */
int e_ds_jdb_stop_class(const char *stop, char *out, size_t n);

/**
 * e_ds_jdb_line_starts_with_mark - Does the line at @line start with @mark,
 * after the "> " jdb echoes?
 * Return: where the mark starts, or NULL.
 */
const char *e_ds_jdb_line_starts_with_mark(const char *line, const char *mark);

/**
 * e_ds_jdb_stop_line_in - The last line of jdb's answer that names a stop.
 * Return: where its mark starts, or NULL.
 */
const char *e_ds_jdb_stop_line_in(const char *text);

/**
 * e_ds_jdb_says - Does a line of jdb's answer start with @mark?
 */
int e_ds_jdb_says(const char *text, const char *mark);

/**
 * e_ds_jdb_says_end - Does a line of jdb's answer say the program is gone?
 */
int e_ds_jdb_says_end(const char *text);

/**
 * e_ds_jdb_asks_command - Does jdb's answer end with its prompt?
 */
int e_ds_jdb_asks_command(const char *text);

/**
 * e_ds_jdb_reply_is_stop - Does jdb's answer so far say the program
 * stopped: a stop line, its "line=" after it, the prompt last?
 */
int e_ds_jdb_reply_is_stop(const char *text);

/**
 * e_ds_jdb_says_suspended_text - Does jdb's answer say "suspend" took hold:
 * its words for it and the bare prompt last?
 */
int e_ds_jdb_says_suspended_text(const char *text);

/**
 * e_ds_jdb_live_word - jdb's word on a breakpoint command, if this line is
 * one: "Set breakpoint P:7", "Deferring breakpoint P:7.", "Unable to set
 * breakpoint P:7 : ...".
 * @line: one line; @spec, @n: receives "P:7"; @words: set to where jdb's
 * words start in @line.
 * Return: E_D_BRK_VERIFIED, _PENDING or _REFUSED, or E_DS_NOT_FOUND.
 */
int e_ds_jdb_live_word(const char *line, char *spec, size_t n,
                       const char **words);

/**
 * e_ds_jdb_frame_parse - Read one frame of jdb's "where":
 * "  [1] P.sq (P.java:3)" or "  [3] java.lang.Thread.run (native method)".
 * Return: 1 when @row is a frame.
 */
int e_ds_jdb_frame_parse(const char *row, E_DS_FRAME *fr);

/**
 * e_ds_a68g_parse_line - The source line of a68g's "5   f := f * i", or -1.
 */
int e_ds_a68g_parse_line(const char *s);

/**
 * e_ds_a68g_line_kind - Which part of a stop a line of a68g's answer is.
 * @s: the line; @len: its length, without the line end.
 * Return: E_D_A68G_LINE_*.
 */
int e_ds_a68g_line_kind(const char *s, int len);

/**
 * e_ds_a68g_lines_make_a_stop - Are these kinds, newest first, the lines
 * a68g writes before its prompt at a stop?
 */
int e_ds_a68g_lines_make_a_stop(const int *kinds);

/**
 * e_ds_a68g_proc_name - The procedure a68g's "calls" row declares:
 * "PROC sq = (INT v) INT:" gives "sq".
 */
void e_ds_a68g_proc_name(const char *text, char *out, size_t n);

#endif /* WPE_DEBSESS_H */
