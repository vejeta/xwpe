/* we_debug.c                                             */
/* Copyright (C) 1993 Fred Kruse                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

#include "messages.h"
#include "edit.h"
#include "progr.h"
#include "we_fdloop.h"
#include "we_dap.h"
#include "we_bsp.h"
#include "we_lsp.h"
#include "we_debprompt.h"
#include "we_debpath.h"
#include "we_debsess.h"

#ifndef NO_XWINDOWS
#include "WeXterm.h"
#endif

#ifdef DEBUGGER

#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <poll.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <glob.h>
#include <stdint.h>
#include <time.h>

/* jdb protocol tracing now routes through the unified WPE_TRACE facility
   (build with ./configure --enable-trace); records are line-tagged "[jdb]".
   The jdb_trace() spelling is kept so existing call sites are unchanged. */
#include "we_trace.h"
#define jdb_trace(...) WPE_TRACE("jdb", __VA_ARGS__)

#ifndef TERMCAP
/* Because term.h defines "buttons" it messes up we_gpm.c if in edit.h */
#include <term.h>
#endif

#define SVLINES 12

int e_d_delbreak(FENSTER *f);
int e_d_error(char *s);
int print_to_end_of_buffer(BUFFER * b,char * str,int wrap_limit);

/* The error stream's line reader: the descriptor is non-blocking from the
   start (e_d_set_nonblock), so it takes what has come and never waits. */
static int e_e_line_read(int n, signed char *s, int max);


#define MAXOUT  2 * MAXSCOL * MAXSLNS

char *e_debugger, *e_deb_swtch = NULL, *e_d_save_schirm;
int e_d_swtch = 0, rfildes[2], ofildes;

/* The line debugger's process, from its start until the owner of the
   editor's children reports its end: the session's end line comes from
   that report, never from asking the system. */
#define E_D_DIED_LABEL_MAX 64
static struct {
 WpeChild child;
 int live;           /* a session runs on it: its end ends the session */
 int asked_end;      /* the editor ended it (Quit, Esc, a deadline, the end
                        of its output, the editor's own end) */
 int end_expected;   /* it said the program ended, and goes with it */
 int ended;          /* its end was reported */
 int status;         /* its wait status, when status_known */
 int status_known;
 int say_later;      /* the session ended before the end was reported: the
                        line that it died is said when it is */
 int held;           /* it ended behind a dialog: the session ends after */
 int in_fd;          /* its input, open until it ends, so the quit queued
                        behind a debugger slow to read still reaches it */
 char label[E_D_DIED_LABEL_MAX];   /* its name, for a line said later */
} e_d_dbg = { { 0 }, 0, 0, 0, 0, 0, 0, 0, 0, -1, "" };

/* Set while the program runs under the run controller (e_d_run_begin) and
   cleared at its stop or at the end of the session.  The key router, the
   cursor hooks of the terminal and X11 backends and the refusal of debugger
   commands read it: the main event loop goes on processing keys and the
   program's output while the debugger has the program. */
int e_d_async_pending = 0;
int e_d_nbrpts = 0, e_d_zwtchs = 0, *e_d_ybrpts, *e_d_nrbrpts;

/* What the debugger said of entry i of the breakpoint list, kept beside the
   list: e_d_brk_state[i] is one of these, e_d_brk_msg[i] the debugger's own
   words when it refused the breakpoint or has it only as deferred.  The
   states (E_D_BRK_PENDING, _VERIFIED, _REFUSED) are the session's, in
   we_debsess.h. */
#define E_D_BRK_SAY_EXTRA 96    /* room for the words around the message */
int *e_d_brk_state;
char (*e_d_brk_msg)[E_D_BRK_MSG_MAX];

/* number of watch expressions in Watches window */
int e_d_nwtchs = 0; 

/* e_d_nrwtchs[i] is the y coordinate (count starts at 0) of the first line of
   the i-th watch in the Watches window */
int *e_d_nrwtchs; 

char **e_d_swtchs; /* e_d_swtchs[i] is the i-th watch expression (a char*) */

/* The longest line read from a debugger.  A stop, a frame or a breakpoint
   answer names a source file, and a file may sit under any path up to
   PATH_MAX; the margin is for what the debugger prints around it (the
   function and its arguments, "at ", ":line").  A longer line is cut at this
   size and the rest dropped, never read as a second line.  Lines whose end
   matters -- a stop line, a frame, a value -- are read whole instead
   (E_D_LINE): their "at file:line" comes after a function name and
   arguments of any length. */
#define E_D_LINE_MARGIN  512
#define E_D_LINE_MAX     (PATH_MAX + E_D_LINE_MARGIN)
/* A debugger line of any length, read by e_d_line_read_nb_whole. */
typedef struct
{
 signed char *s;          /* the line, NUL-terminated; NULL before a read */
 int size;                /* bytes allocated at s */
} E_D_LINE;
/* The longest line read whole: far beyond any value, frame or stop line a
   debugger prints.  A longer one is a runaway; it is kept cut at this size,
   ends in E_D_LINE_CUT_MARK, and its rest is dropped as a cut line's is. */
#define E_D_LINE_WHOLE_MAX  (4 * 1024 * 1024)
#define E_D_LINE_CUT_MARK   " [...]"
/* The most an E_D_LINE ever holds: a whole line, one more read, the mark. */
#define E_D_LINE_ROOM_MAX   (E_D_LINE_WHOLE_MAX + 2 * E_D_LINE_MAX)
static int e_d_line_room(E_D_LINE *ln, int len);
static int e_d_line_drain_errors(signed char *s, int max);
/* How many of the last bytes dropped from a cut line are kept to recognise a
   prompt that the debugger printed right after it, split or not: the longest
   prompt, "(a68g) ", is 7 bytes. */
#define E_D_PROMPT_TAIL  8
int e_d_nstack;
char e_d_file[E_D_LINE_MAX], **e_d_sbrpts;
char *e_d_out = NULL;
/* Which debugger backend e_deb_type selects (see e_run_debug's dispatch).
   The numeric values are persisted in the options file, so keep them stable.
   They are the session's backend numbers too, so e_deb_type is handed to
   the readers of we_debsess.c as it is. */
enum {
 DEB_GDB = E_DS_GDB,   /* GNU gdb (default)        prompt "(gdb)" */
 DEB_SDB = E_DS_SDB,   /* classic sdb              prompt "*"     */
 DEB_DBX = E_DS_DBX,   /* dbx -i                   prompt "(dbx)" */
 DEB_XDB = E_DS_XDB,   /* HP xdb -L                prompt ">"     */
 DEB_JDB = E_DS_JDB,   /* Java jdb                                */
 DEB_PDB = E_DS_PDB,   /* Python python3 -m pdb    prompt "(Pdb)" */
 DEB_A68G = E_DS_A68G, /* Algol 68 Genie a68g --monitor  prompt "(a68g)" */
 DEB_DAP = E_DS_DAP    /* Debug Adapter Protocol client (Go/dlv, Rust/lldb-dap, ...) */
};

/* What the session knows: stops, frames, watch values, breakpoint answers.
   Filled as the debuggers answer; the windows are still drawn from the
   readers' results directly. */
static E_D_SESSION e_d_session;

int e_deb_type = 0, e_deb_mode = 0;
/* Which DAP adapter to prefer for languages that offer a choice (Rust: gdb vs
   lldb-dap).  Persisted in xwperc ("DAPAdapter : N"), set via the Debug ->
   debugger Options dialog, and overridable at run time by $XWPE_DAP_ADAPTER.
   0 = auto-detect (first in PATH), 1 = prefer gdb, 2 = prefer lldb-dap. */
enum { DAP_ADAPTER_AUTO = 0, DAP_ADAPTER_GDB = 1, DAP_ADAPTER_LLDB = 2 };
int e_dap_adapter = DAP_ADAPTER_AUTO;
/* The last SVLINES lines the debugger answered a run or a step with, each
   whole, oldest first, read by the stop parsers through e_d_sp. */
static E_D_LINE e_d_out_ln[SVLINES];
char *e_d_sp[SVLINES];
char e_d_tty[80];

/* Pty for capturing debugged program's output */
int e_d_pty_master = -1;
int e_d_pty_slave = -1;
char e_d_pty_slave_name[128];
/* The program's side of the pty was closed by everyone who held it: the
   program and whatever it started have ended.  Set by the readers of the
   master, cleared when the pty is opened or closed. */
static int e_d_pty_hung = 0;
char *e_d_prog_output = NULL;
int e_d_prog_output_len = 0;
int e_d_prog_output_cap = 0;

/* Single growth point for the e_d_prog_output capture buffer.  Every site that
   used to open-code "if needed, realloc; memcpy; len += n" now routes through
   these so an out-of-memory realloc can never leak the old pointer or write
   past the buffer.  Forward-declared here because e_d_pty_drain() (above the
   definitions) is one of the callers. */
static int  e_d_prog_output_reserve(int extra);
static void e_d_prog_output_append(char *data, int len);
static void e_d_prog_output_append_line(char *data, int len);
static int  e_d_start_forget(void);       /* the start watcher (see e_run_debug) */
static int  e_d_start_refused(FENSTER *f);
static int  e_d_refuse_while_running(FENSTER *f);   /* the program runs */
static int  e_d_watch_refused(FENSTER *f);   /* a debugger without watches */
static const char *e_d_start_label(void);
static void e_d_show_stop_position(FENSTER *f);  /* line:column after a stop */
static void e_d_note_output_closed(void);        /* the debugger's pipe ended */
static void e_d_jdb_missing_forget(void);        /* classes without source */
static void e_d_jdb_pause_forget(void);          /* a Pause still held */
static void e_d_end_forget(void);               /* how the last debugger ended */
/* gdb's answers at a stop, read on the fd loop (e_d_ga, below). */
static int  e_d_ga_sync_runs(void);          /* a change is told while it runs */
static int  e_d_ga_refused(FENSTER *f);      /* a command waits for an answer */
static void e_d_ga_forget(void);             /* the session ends */
static void e_d_ga_run_begins(void);         /* the program runs again */
static int  e_d_ga_pause_takes_sync(FENSTER *f);
static void e_d_ga_owe_settle(int owed);     /* interrupts gdb may answer */
static int  e_d_ga_at_stop(void);            /* gdb stopped, nothing asked */
static int  e_d_ga_brk_sync(ECNT *cn);       /* tell gdb the list's changes */
static int  e_d_ga_ask_plain(ECNT *cn, const char *cmd);
static int  e_d_ga_watches_ask(ECNT *cn, int sw);
static int  e_d_ga_stack_ask(ECNT *cn, int sw);
static int  e_d_ga_follow_frame(ECNT *cn, int frame, const char *file,
                                int line);
static int  e_d_ga_finish_from_frame(ECNT *cn, int below, int sw);
static int  e_d_ga_leave_unknown_source(ECNT *cn);
static int  e_d_gdb_finish_out_of_pause(FENSTER *f, int sw);
/* gdb's start and every resume, asked on the fd loop too (below).  How the
   run is handed over once gdb has answered what is asked first: */
#define E_D_GO_START     1   /* the first run, which says "Starting program" */
#define E_D_GO_CONTINUE  2   /* Continue, which says "Continuing." */
#define E_D_GO_STEP      3   /* a step, Finish or Go to cursor */
#define E_D_GO_ON        4   /* on from a stop the user is not shown */
#define E_D_GO_FINISH    5   /* Finish, once the frames say it has a caller */
static int  e_d_ga_go(FENSTER *f, int how, const char *cmd, int start_brk);
static int  e_d_ga_go_step(FENSTER *f, const char *cmd, const char *follow);
static void e_d_ga_go_on(const char *cmd);
static int  e_d_ga_go_keeps_start_brk(int sw);
static int  e_d_ga_go_keeps_goto_line(int line);
static void e_d_ga_drop_start_entry(int i);
static int  e_d_ga_set_up_again(FENSTER *f);
/* What the request engine is told of gdb, pdb, jdb, a68g, sdb, dbx and xdb
   (e_d_ga, below). */
typedef struct e_d_ga_backend E_D_GA_BACKEND;
static const E_D_GA_BACKEND *e_d_ga_backend_of(int type);
static const E_D_GA_BACKEND *e_d_ga_this(void);
static void e_d_ga_remove_all(ECNT *cn);     /* Remove all at a stop */
/* pdb's stops, Pause, syncs and Finish, asked on the fd loop the same way. */
static void e_d_gp_stop_ask(ECNT *cn, const char *file, int line);
static void e_d_gp_pause_ask(ECNT *cn);
static int  e_d_pdb_sync_on_loop(ECNT *cn);
static int  e_d_ga_finish_ask(FENSTER *f);   /* pdb and jdb */
static int  e_d_ga_finishes_on_loop(void);
static int  e_d_ga_sets_cursor_brk(void);
/* sdb's, dbx's and xdb's stops, steps and run lines, the same way. */
static void e_d_cli_stopped(FENSTER *f);
static int  e_d_cli_step_go(FENSTER *f, int sw);
static int  e_d_cli_run_line_own(const char *line);
static void e_d_cli_awaits_forget(void);
/* jdb's stops and Pause, asked on the fd loop too. */
static void e_d_gj_stop_ask(ECNT *cn, const char *stop);
static void e_d_gj_pause_ask(ECNT *cn);
static void e_d_gj_forget_events(void);   /* jdb's events between answers */
static void e_d_jdb_owe_at_stop(void);
static void e_d_frames_put_in_window(ECNT *cn, int is, int sw);
static int  e_d_gdb_run_line_own(const char *line);
static int  e_d_gb_removal_told(int id);     /* gdb is removing it now */
/* The number of the breakpoint whose "d N" gdb is answering, or 0.  While
   the program runs the user may set the line again meanwhile; that removal
   is then not taken back (e_d_brk_gone_revive), since gdb carries it out. */
static int  e_d_gb_removing_id;
/* The number of a breakpoint gdb may have set though its answer was given
   up on: it is removed by its place ("clear file:line"), and a mark set
   there again is a new breakpoint, never this one revived. */
#define E_D_GB_ID_UNKNOWN 0
static void e_d_gb_add_given_up(void);
/* gdb's command that clears every breakpoint. */
#define E_D_GDB_DELETE_ALL "delete"
/* The debugger keys typed while gdb answers, kept to run in order. */
#define E_D_ACT_STEP    1   /* F7, Trace into */
#define E_D_ACT_NEXT    2   /* F8, Step over */
#define E_D_ACT_RUN     3   /* Ctrl-G R, Run or Continue */
#define E_D_ACT_FINISH  4   /* Ctrl-G F, Finish function */
#define E_D_ACT_GOTO    5   /* Ctrl-G U or F4, Go to cursor */
static int  e_d_act_deferred(int kind, FENSTER *f, int line);
/* Room for one status line and for one watch's print command. */
#define E_D_WATCH_CMD_MAX 256
#define E_D_WATCH_REFUSED_FMT "watches are not available with %s"
static int  e_d_a68g_cur_line = 0;   /* source line a68g is currently stopped on */

int e_d_pty_verase(void)
{
 struct termios t;

 if (e_d_pty_master >= 0 && tcgetattr(e_d_pty_master, &t) == 0)
  return t.c_cc[VERASE];
 return 0x7F;
}

extern int wfildes[2], efildes[2];
extern struct termios otermio, ntermio, ttermio;
extern struct e_s_prog e_sv_prog;
extern BUFFER *e_p_w_buffer;
extern char *att_no;
extern char *e_tmp_dir;
extern int e_algol68_use_ga68_in(const char *dir, const char *name);
extern int e_check_c_file_w(FENSTER *fw);

#ifdef NOTPARM
/* char *tparm(); */
char *tgoto();
#endif
#ifdef DEFTPUTS
int tputs();
#endif

char *npipe[5] = {  NULL, NULL, NULL, NULL, NULL  };

/* Per-step read of jdb, pdb and a68g output: scratch buffer size (the run of
   pdb, jdb and a68g waits as long as the program takes). */
#define DEB_STEP_BUF     (PATH_MAX + 4096)

/* Safety bound on the stderr-drain loop (e_d_line_drain_errors): a debugger
   that floods its error stream (e.g. a68g's abend->io_write_string recursion
   when its output pipe breaks) must never hold xwpe in an unbounded read.  A
   normal debug step emits at most a handful of stderr lines; anything past this
   is a runaway, so we stop draining and let the loop re-enter on the next call
   (never leave xwpe in a busy loop). */
#define E_D_DRAIN_MAX    4096

/* Settle time (ms) for the legacy external-xterm debugger before xwpe reclaims
   the input focus -- see the comment at its use in e_exec_deb. */
#define DEB_XTERM_SETTLE_MS 200

/**
 * e_d_pty_drain - Take what the program has written to its terminal and not
 * yet been read into e_d_prog_output, without waiting.
 *
 * The User Screen (Alt-F5) and the end of a session call this so the user
 * sees the program's last output.  Nothing is waited for: while the program
 * runs, the fd loop reads its terminal as the output comes
 * (e_d_on_pty_readable), and a program that is stopped or has ended writes
 * nothing more, so what it wrote is already in the terminal's buffer.
 */
void e_d_pty_drain(void)
{
 char buf[4096];
 int n;

 if (e_d_pty_master < 0)
  return;
 /* The master is non-blocking from its creation: the read ends when the
    program has written nothing more. */
 while ((n = read(e_d_pty_master, buf, sizeof(buf))) > 0)
  e_d_prog_output_append(buf, n);
}

/* Commands for the debugger go through e_d_send_cmd (below), which takes a
   NUL-terminated command and measures it itself: the hand-counted lengths of
   the old open-coded writes could be wrong (the old jdb delete-all sent "db"
   for "db *\n").  It hands the whole command to the loop's writer. */
/* Runaway guard: how many debugger-output lines have been read since the
   last command we sent.  A single command's reply is bounded (a stack, a
   value, a stopped-at line); a debugger that streams far more than any real
   reply -- e.g. a68g stuck in its abend->io_write_string recursion -- is a
   runaway, and the readers would otherwise spin on it forever.  Reset on
   every e_d_send_cmd (a fresh reply is expected) and checked by the readers
   (e_d_nb_fill, e_d_line_drain_errors). */
static long e_d_read_runaway = 0;
#define E_D_RUNAWAY_MAX  100000L

/* Room for the command a run was started with, sent again when an interrupt
   left over from a race stops the program before it moved.  It holds the
   longest command the editor builds for a run: gdb's advance "file":line
   (e_d_gdb_run_to_line), a path and a line number. */
#define E_D_RUN_AGAIN_MAX (E_D_LINE_MAX + PATH_MAX)
/* The last command sent to the debugger: the run controller keeps it as the
   command of the run it is handed. */
static char e_d_last_cmd[E_D_RUN_AGAIN_MAX];

/* ==== which interface gdb speaks, and how a command goes out over MI ====

   gdb is driven over its line interface (CLI) or over its machine interface
   (MI), as XWPE_GDB_INTERFACE says; unset means the line interface.  Under
   MI every command carries a number, so gdb's answer is matched to the
   command that asked it, and the editor reads named fields instead of
   gdb's sentences.  The editor's own commands are written once, in gdb's
   line syntax: the resumes become MI's -exec-* commands here, and any other
   command goes through -interpreter-exec console, which runs it as typed. */

#define E_D_GDBMI_ENV          "XWPE_GDB_INTERFACE"
#define E_D_GDBMI_WORD_MI      "mi"
#define E_D_GDBMI_WORD_CLI     "cli"
#define E_D_GDBMI_SWITCH       "--interpreter=mi2"
#define E_D_GDBMI_UNKNOWN_FMT \
 " XWPE_GDB_INTERFACE=%.40s is neither mi nor cli: gdb uses its line interface"
#define E_D_GDBMI_SAY_MAX      192
#define E_D_GDBMI_CONSOLE_HEAD "-interpreter-exec console \""
#define E_D_GDBMI_CONSOLE_TAIL "\""
#define E_D_GDBMI_MI_LEAD      '-'
/* A command line once numbered and, for the console, quoted: every byte
   may need its escape. */
#define E_D_GDBMI_LINE_MAX     (2 * E_D_RUN_AGAIN_MAX + 64)
/* The highest number a command is given; the next one is 1 again.  It has
   as many digits as gdb's answer is read with (E_GDBMI_TOKEN_DIGITS_MAX in
   we_gdbmi.c), so every answer still names its command; a number comes
   back only after a billion commands, long after any answer to it. */
#define E_D_GDBMI_TOKEN_LAST   999999999

/* The session's interface. */
static struct
{
 int on;                     /* this session's gdb speaks MI */
 int force_cli;              /* the next gdb start keeps the line interface:
                                the gdb of this session is too old for MI */
 int token;                  /* the number the last command was given */
 int pending;                /* the number of the engine's command in flight */
 char say[E_D_GDBMI_SAY_MAX];   /* what the start says on the status row */
} e_d_mi;

/**
 * e_d_gdbmi_on - Does this session's gdb speak MI?
 *
 * Every MI path asks this; under the line interface none of them runs, so
 * the line interface behaves exactly as it did before MI existed.
 */
static int e_d_gdbmi_on(void)
{
 return(e_d_mi.on && e_deb_type == DEB_GDB);
}

/* The editor's resumes and its quit, in gdb's line syntax and as MI says
   them; NULL drops the line (the answer to a question MI never asks). */
static const struct
{
 const char *cli;
 const char *mi;
} e_d_gdbmi_words[] =
{
 { "c",      "-exec-continue" },
 { "n",      "-exec-next" },
 { "s",      "-exec-step" },
 { "finish", "-exec-finish" },
 { "q",      "-gdb-exit" },
 { "y",      NULL }
};

/**
 * e_d_gdbmi_word_of - The MI command for one line of the editor's own.
 * @line: the line, without its line end; @mi: set to the MI command, NULL
 * to drop the line.
 * Return: 1 when the line is one of e_d_gdbmi_words, else 0.
 */
static int e_d_gdbmi_word_of(const char *line, const char **mi)
{
 size_t k;

 for (k = 0; k < sizeof e_d_gdbmi_words / sizeof e_d_gdbmi_words[0]; k++)
  if (!strcmp(line, e_d_gdbmi_words[k].cli))
  {
   *mi = e_d_gdbmi_words[k].mi;
   return(1);
  }
 return(0);
}

/**
 * e_d_gdbmi_quote - Copy @in into @out as the inside of an MI string:
 * quotes and backslashes escaped.
 * @out, @n: where it goes, cut to fit.
 */
static void e_d_gdbmi_quote(char *out, size_t n, const char *in)
{
 size_t w = 0;

 for (; *in && w + 2 < n; in++)
 {
  if (*in == '"' || *in == '\\')
   out[w++] = '\\';
  out[w++] = *in;
 }
 out[w] = '\0';
}

/**
 * e_d_gdbmi_line_of - One line of the editor's command, numbered, as MI
 * takes it.
 * @line: the line, no line end; @token: its number; @out, @n: the MI line,
 * with its line end.
 * Return: 1, or 0 when the line is to be dropped.
 */
static int e_d_gdbmi_line_of(const char *line, int token, char *out, size_t n)
{
 char quoted[E_D_GDBMI_LINE_MAX];
 const char *mi;

 if (e_d_gdbmi_word_of(line, &mi))
 {
  if (!mi)
   return(0);
  snprintf(out, n, "%d%s\n", token, mi);
  return(1);
 }
 if (line[0] == E_D_GDBMI_MI_LEAD)
 {
  snprintf(out, n, "%d%s\n", token, line);
  return(1);
 }
 e_d_gdbmi_quote(quoted, sizeof quoted, line);
 snprintf(out, n, "%d%s%s%s\n", token, E_D_GDBMI_CONSOLE_HEAD, quoted,
          E_D_GDBMI_CONSOLE_TAIL);
 return(1);
}

/* What the line that says the debugger stopped reading its input adds. */
#define E_D_INPUT_ENDS "Debug > Quit ends it"
static const char *e_d_input_name(void);   /* with the request engine, below */

/**
 * e_d_pipe_write - Hand @len bytes of @s to the debugger's input, whole.
 *
 * Never waits for the debugger: what it does not read now is written by the
 * loop when it reads again, so the editor keeps its keys while gdb is
 * stopped or busy, and the status row names it once it has read nothing for
 * a while.  A broken pipe means the debugger is gone; its end of output
 * says so.
 * Return: WPE_FD_WRITE_SENT, _QUEUED, _REFUSED (nothing went out) or
 * _CLOSED.
 */
static int e_d_pipe_write(const char *s, size_t len)
{
 return wpe_fd_write(rfildes[1], s, len, e_d_input_name(), E_D_INPUT_ENDS);
}

/* The debugged program, as the stall line names it. */
#define E_D_PROGRAM_WHAT "the program"

/**
 * e_d_pty_type - Type @key into the debugged program's terminal while it
 * runs: Backspace as the terminal's own erase byte, Enter as a line end,
 * text as UTF-8.
 * @key: a key e_key_is_program_text took for the program.
 *
 * Never waits for the program: what its terminal does not take now (a
 * program that stopped reading with a full line typed ahead) is queued on
 * the loop, so Debug > Quit and the editor's other keys keep working.
 * Return: 0 when the key is on its way, -1 when there is no terminal or it
 * would not take it.
 */
int e_d_pty_type(int key)
{
 unsigned char buf[E_PTY_KEY_BYTES_MAX];
 int len, r;

 if (e_d_pty_master < 0)
  return(-1);
 if (key == WPE_DC)
 {
  buf[0] = (unsigned char)e_d_pty_verase();
  len = 1;
 }
 else
  len = e_pty_key_bytes(key, buf);
 if (len <= 0)
  return(0);
 r = wpe_fd_write(e_d_pty_master, buf, (size_t)len, E_D_PROGRAM_WHAT,
                  E_D_INPUT_ENDS);
 if (r == WPE_FD_WRITE_SENT || r == WPE_FD_WRITE_QUEUED)
  return(0);
 return(-1);
}

/**
 * e_d_close_fd - Close one of the session's descriptors (the debugger's
 * input among them).
 * @fd: the descriptor, set to -1; already -1 is left alone.
 *
 * Bytes still queued for it are dropped with it: the debugger is being
 * ended, and a descriptor opened later on the same number must not get
 * them.
 */
static void e_d_close_fd(int *fd)
{
 if (*fd < 0)
  return;
 wpe_fd_close(*fd);
 *fd = -1;
}

/**
 * e_d_gdbmi_next_token - The number the next command gets: one more than
 * the last, and 1 after E_D_GDBMI_TOKEN_LAST, so a long session never sends
 * a number gdb's answer could not be matched by.
 */
static int e_d_gdbmi_next_token(void)
{
 if (e_d_mi.token >= E_D_GDBMI_TOKEN_LAST)
  return(1);
 return(e_d_mi.token + 1);
}

/**
 * e_d_gdbmi_lines_max - How many lines @cmd holds, the last one without its
 * line end counted too.
 */
static size_t e_d_gdbmi_lines_max(const char *cmd)
{
 size_t n = 1;

 for (; *cmd; cmd++)
  if (*cmd == '\n')
   n++;
 return(n);
}

/**
 * e_d_gdbmi_send_lines - Send the editor's command @cmd to a gdb that
 * speaks MI, each of its lines numbered.
 *
 * Every line goes out in one write, so a command of several lines (Quit's
 * "q" and "y") is never left half sent behind a gdb that stopped reading.
 * The number of the last line is kept, so the request engine knows which
 * answer is the one it waits for.
 * Return: as e_d_pipe_write.
 */
static int e_d_gdbmi_send_lines(const char *cmd)
{
 char line[E_D_RUN_AGAIN_MAX];
 char out[E_D_GDBMI_LINE_MAX];
 const char *p = cmd, *nl;
 size_t len, used = 0, room = e_d_gdbmi_lines_max(cmd) * E_D_GDBMI_LINE_MAX;
 char *all = malloc(room);
 int r;

 if (!all)
  return(WPE_FD_WRITE_REFUSED);
 while (*p)
 {
  nl = strchr(p, '\n');
  len = nl ? (size_t)(nl - p) : strlen(p);
  if (len >= sizeof line)
   len = sizeof line - 1;
  memcpy(line, p, len);
  line[len] = '\0';
  p = nl ? nl + 1 : p + strlen(p);
  if (!line[0])
   continue;
  if (!e_d_gdbmi_line_of(line, e_d_gdbmi_next_token(), out, sizeof out))
   continue;
  e_d_mi.token = e_d_gdbmi_next_token();
  len = strlen(out);
  memcpy(all + used, out, len);
  used += len;
 }
 r = e_d_pipe_write(all, used);
 free(all);
 return(r);
}

/**
 * e_d_send_cmd - Send the debugger @cmd, its line ends included.
 *
 * The command goes out whole, behind what the debugger has not read yet,
 * without the editor waiting for it to read.
 * Return: WPE_FD_WRITE_SENT or _QUEUED when it is on its way;
 * WPE_FD_WRITE_REFUSED when nothing went out; WPE_FD_WRITE_CLOSED when the
 * debugger closed its input (its end is reported by its output's end).
 */
static int e_d_send_cmd(const char *cmd)
{
 e_d_read_runaway = 0;
 snprintf(e_d_last_cmd, sizeof e_d_last_cmd, "%s", cmd);
 if (e_d_gdbmi_on())
  return(e_d_gdbmi_send_lines(cmd));
 return(e_d_pipe_write(cmd, strlen(cmd)));
}

/**
 * e_d_send_went - Is a command handed to e_d_send_cmd on its way?
 * @r: what e_d_send_cmd returned.
 */
static int e_d_send_went(int r)
{
 return(r == WPE_FD_WRITE_SENT || r == WPE_FD_WRITE_QUEUED);
}

static void e_d_pty_strip_cr(char *s, int len)
{
 int r, w;
 for (r = 0, w = 0; r < len; r++)
  if (s[r] != '\r')
   s[w++] = s[r];
 s[w] = '\0';
}

/* forward declarations */
static void e_d_pty_flush_line(FENSTER *, int);
void e_d_pty_flush_to_messages(FENSTER *);

/* Partial-read state shared by the incremental (async) debugger readers: each
   field holds a fragment carried between non-blocking reads.  Grouped so a
   session boundary can clear them all in one call (e_d_async_reset). */
static struct {
 E_D_LINE nb;               /* e_d_line_read_nb: what has come of gdb's lines */
 int  nb_len;               /* bytes held in nb */
 int  nb_scanned;           /* of which are known to hold no line end */
 int  nb_dropping;          /* the rest of a line handed out cut is dropped */
 char pty_line_buf[1024];   /* pty -> Messages: line being assembled */
 int  pty_line_len;
 int  pty_owns_last_line;   /* pty output left the current line unterminated */
} e_d_async;

static void e_d_async_reset(void)
{
 e_d_async.nb_len = 0;
 e_d_async.nb_scanned = 0;
 e_d_async.nb_dropping = 0;
 e_d_async.pty_line_len = 0;
 e_d_async.pty_owns_last_line = 0;
}

/* A cut line on the error stream.  e_e_line_read keeps the start of a line
   longer than its caller's buffer and drops the rest; the dropping goes on
   in the next read, so no read ever waits for the end of a line. */
static struct {
 int err_discarding;      /* the rest of a cut error line is pending */
} e_d_cut;

/**
 * e_d_cut_reset - Forget a cut line left over from another debugger session.
 *
 * A new session starts reading at the start of a line, so nothing the last
 * debugger printed can make the first answer of this one look cut.
 */
static void e_d_cut_reset(void)
{
 memset(&e_d_cut, 0, sizeof e_d_cut);
}

/**
 * e_d_gdbmi_prompt_at - Under MI, does the line that byte @i of @s ends
 * hold nothing but gdb's "(gdb)"?  Read only while gdb starts: its banner
 * ends at the first such line.
 */
static int e_d_gdbmi_prompt_at(signed char *s, int i)
{
 int start = i;

 while (start > 0 && s[start - 1] != '\n')
  start--;
 return(e_gdbmi_is_prompt((const char *)s + start, (size_t)(i + 1 - start)));
}

static int e_d_check_prompt(signed char *s, int i)
{
 if (e_d_gdbmi_on())
  return(e_d_gdbmi_prompt_at(s, i));
 if (e_deb_type == DEB_GDB && i > 4 && s[i] == ' ' && !strncmp((char*)s+i-5, "(gdb)", 5))
  return 1;
 if (e_deb_type == DEB_DBX && i > 4 && s[i] == ' ' && !strncmp((char*)s+i-5, "(dbx)", 5))
  return 1;
 if (e_deb_type == DEB_PDB && i > 4 && s[i] == ' ' && !strncmp((char*)s+i-5, "(Pdb)", 5))
  return 1;
 if (e_deb_type == DEB_A68G && i > 5 && s[i] == ' ' && !strncmp((char*)s+i-6, "(a68g)", 6))
  return 1;
 if (e_deb_type == DEB_SDB && s[i] == '*')
  return 1;
 if (e_deb_type == DEB_XDB && s[i] == '>')
  return 1;
 if (e_deb_type == DEB_JDB && i > 0 &&
     ((s[i] == ' ' && s[i-1] == ']') || (s[i] == ' ' && s[i-1] == '>')))
  return 1;
 return 0;
}

/**
 * e_d_nb_is_line_end - Say whether a byte ends a line for the async reader.
 *
 * One definition for the line loop and for the dropping of a cut line, so a
 * line the loop would end is never dropped past its end.
 */
static int e_d_nb_is_line_end(char c)
{
 return(c == '\n' || c == '\0');
}

/**
 * e_d_nb_consume - Forget the first @n buffered bytes of the async reader.
 */
static void e_d_nb_consume(int n)
{
 char *held = (char *)e_d_async.nb.s;

 e_d_async.nb_len -= n;
 memmove(held, held + n, e_d_async.nb_len);
 held[e_d_async.nb_len] = '\0';
 e_d_async.nb_scanned = 0;
}

/**
 * e_d_nb_ends_in_prompt - Say whether the async reader's buffer ends with
 * the debugger's prompt.
 */
static int e_d_nb_ends_in_prompt(void)
{
 return(e_d_async.nb_len > 0 &&
        e_d_check_prompt(e_d_async.nb.s, e_d_async.nb_len - 1));
}

/**
 * e_d_nb_prompts_held - How many prompts the async reader holds unread,
 * after the stop it handed out.
 *
 * gdb answers an interrupt that reached it at its prompt with one more
 * prompt; when it came in the same read as the stop it waits here.
 */
static int e_d_nb_prompts_held(void)
{
 int i, n = 0;

 for (i = 0; i < e_d_async.nb_len; i++)
  if (e_d_check_prompt(e_d_async.nb.s, i))
   n++;
 return(n);
}

/**
 * e_d_nb_forget - Drop what the async reader holds unread, once it has been
 * accounted for, so the next run does not read it as its own answer.
 */
static void e_d_nb_forget(void)
{
 e_d_async.nb_len = 0;
 e_d_async.nb_scanned = 0;
 e_d_async.nb_dropping = 0;
}

/**
 * e_d_nb_take - Hand out the first @len buffered bytes as one line, and keep
 * what follows them for the next.
 * @line: where the line goes, grown as needed; @len: its bytes, its end
 * included; @used: how many buffered bytes it took.
 * Return: 0, or -1 when memory ran out.
 */
static int e_d_nb_take(E_D_LINE *line, int len, int used)
{
 if (e_d_line_room(line, len) < 0)
  return(-1);
 memcpy(line->s, e_d_async.nb.s, len);
 line->s[len] = '\0';
 e_d_nb_consume(used);
 return(0);
}

/**
 * e_d_nb_holds_line - Say whether a whole line is already buffered.
 *
 * Nothing more is read while one is: gdb may print megabytes of short lines
 * in one run (a breakpoint's commands in a loop), and reading on would hold
 * them all at once until the buffer's bound ended the session.
 */
static int e_d_nb_holds_line(void)
{
 const char *held = (const char *)e_d_async.nb.s;
 int i;

 for (i = e_d_async.nb_scanned; i < e_d_async.nb_len; i++)
  if (e_d_nb_is_line_end(held[i]))
   return(1);
 e_d_async.nb_scanned = e_d_async.nb_len;
 return(0);
}

/* What the async reader (e_d_line_read_nb_whole) hands out. */
#define E_D_NB_LINE         2      /* a whole line of the answer */
#define E_D_NB_PROMPT       1      /* the debugger's prompt: it waits */
#define E_D_NB_END          (-1)   /* output ended, ran away, or no memory */
#define E_D_NB_NOTHING_YET  (-2)   /* no whole line has come yet */
/* What e_d_nb_fill answers for a runaway debugger or memory run out. */
#define E_D_NB_FILL_FAILED  (-3)
/* No read was made: a whole line was already held. */
#define E_D_NB_NOT_READ     (-4)

/**
 * e_d_nb_take_cut - Hand out a line that passed E_D_LINE_WHOLE_MAX cut,
 * ending in E_D_LINE_CUT_MARK, and drop its rest as it comes.
 * @line: where the line goes.
 *
 * Only a runaway gdb prints such a line; memory stays bounded and the user
 * sees that the line is not all there.
 * Return: E_D_NB_LINE, or E_D_NB_END when memory ran out.
 */
static int e_d_nb_take_cut(E_D_LINE *line)
{
 int len = e_d_async.nb_len;

 if (e_d_nb_take(line, len, len - E_D_PROMPT_TAIL) < 0)
  return(E_D_NB_END);
 strcpy((char *)line->s + len, E_D_LINE_CUT_MARK);
 e_d_async.nb_dropping = 1;
 return(E_D_NB_LINE);
}

/**
 * e_d_nb_take_last - Hand out what a debugger that has ended left unended,
 * as its last line.
 * @line: where the line goes.
 *
 * A debugger that dies in the middle of a line leaves its end of file
 * readable for ever: kept back as a line still to come, the half line
 * would wake the editor without end.  Handed out like any other line, it
 * lets the next read find the end, so the session ends and the user reads
 * that the debugger is gone.
 * Return: E_D_NB_LINE, or E_D_NB_END when memory ran out.
 */
static int e_d_nb_take_last(E_D_LINE *line)
{
 int len = e_d_async.nb_len;

 if (e_d_nb_take(line, len, len) < 0)
  return(E_D_NB_END);
 return(E_D_NB_LINE);
}

/**
 * e_d_nb_still_dropping - Go on dropping the rest of a line handed out cut.
 *
 * The dropping ends at the line's end, or at gdb's prompt when gdb asks for
 * a command right after the line without ending it; the last bytes are kept
 * meanwhile, in case they begin that prompt.
 * Return: 1 while nothing but dropped text has come, else 0.
 */
static int e_d_nb_still_dropping(void)
{
 const char *held = (const char *)e_d_async.nb.s;
 int i;

 for (i = 0; i < e_d_async.nb_len; i++)
  if (e_d_nb_is_line_end(held[i]))
  {
   e_d_nb_consume(i + 1);
   e_d_async.nb_dropping = 0;
   return(0);
  }
 if (e_d_nb_ends_in_prompt())
 {
  e_d_async.nb_dropping = 0;
  return(0);
 }
 if (e_d_async.nb_len > E_D_PROMPT_TAIL)
  e_d_nb_consume(e_d_async.nb_len - E_D_PROMPT_TAIL);
 return(1);
}

/**
 * e_d_nb_fill - Add to the async reader's buffer what gdb has written, without
 * waiting.
 * @n: gdb's output.
 *
 * Each read takes at most E_D_LINE_MAX bytes and counts as one line toward
 * the runaway bound, as a debugger streaming without end is counted, and a line
 * is never held past E_D_LINE_WHOLE_MAX, so memory stays bounded.
 * Return: what read() answered, or E_D_NB_FILL_FAILED for a runaway or
 * memory run out.
 */
static int e_d_nb_fill(int n)
{
 int flags, nread;

 if (e_d_line_room(&e_d_async.nb, e_d_async.nb_len) < 0)
  return(E_D_NB_FILL_FAILED);
 flags = fcntl(n, F_GETFL, 0);
 fcntl(n, F_SETFL, flags | O_NONBLOCK);
 nread = read(n, e_d_async.nb.s + e_d_async.nb_len, E_D_LINE_MAX - 1);
 fcntl(n, F_SETFL, flags & ~O_NONBLOCK);
 if (nread <= 0)
  return(nread);
 e_d_async.nb_len += nread;
 e_d_async.nb.s[e_d_async.nb_len] = '\0';
 if (++e_d_read_runaway > E_D_RUNAWAY_MAX)
 {
  e_d_read_runaway = 0;
  e_d_async.nb_len = 0;
  e_d_async.nb_scanned = 0;
  return(E_D_NB_FILL_FAILED);
 }
 return(nread);
}

/**
 * e_d_line_read_nb_whole - Hand out the next whole line gdb wrote, or its
 * prompt, without waiting.
 * @n: gdb's output; @line: where the line goes, grown as needed.
 *
 * The fd-loop's reader for a run or a step: a stop line names its file and
 * line at its end, after a function name and arguments of any length (a C++
 * template frame passes E_D_LINE_MAX easily), so it is handed out whole and
 * the stop is marked whatever its length.  A line not yet ended stays
 * buffered until it is.
 * Return: E_D_NB_LINE, E_D_NB_PROMPT, E_D_NB_NOTHING_YET while nothing
 * whole has come, or E_D_NB_END at the end of the output, for a runaway
 * gdb, or when memory ran out.
 */
static int e_d_line_read_nb_whole(int n, E_D_LINE *line)
{
 signed char err[E_D_LINE_MAX];
 char *held;
 int nread = E_D_NB_NOT_READ, i;

 e_d_line_drain_errors(err, sizeof err);
 if (!e_d_nb_holds_line())
 {
  nread = e_d_nb_fill(n);
  if (nread == E_D_NB_FILL_FAILED)
   return(E_D_NB_END);
  if (nread == 0 && e_d_async.nb_dropping)
   e_d_async.nb_len = 0;  /* the dropped rest is no prompt to wait for */
  if (nread == 0 && e_d_async.nb_len == 0)
  {
   e_d_note_output_closed();
   return(E_D_NB_END);
  }
  if (nread < 0 && e_d_async.nb_len == 0)
   return(E_D_NB_NOTHING_YET);
 }
 if (e_d_async.nb_dropping && e_d_nb_still_dropping())
  return(E_D_NB_NOTHING_YET);
 held = (char *)e_d_async.nb.s;
 for (i = e_d_async.nb_scanned; i < e_d_async.nb_len; i++)
 {
  if (e_d_nb_is_line_end(held[i]))
  {
   if (e_d_nb_take(line, i + 1, i + 1) < 0)
    return(E_D_NB_END);
   if (e_d_check_prompt(line->s, i))
    return(E_D_NB_PROMPT);
   return(E_D_NB_LINE);
  }
 }
 e_d_async.nb_scanned = e_d_async.nb_len;
 if (e_d_nb_ends_in_prompt())
 {
  if (e_d_nb_take(line, e_d_async.nb_len, e_d_async.nb_len) < 0)
   return(E_D_NB_END);
  return(E_D_NB_PROMPT);
 }
 if (e_d_async.nb_len >= E_D_LINE_WHOLE_MAX)
  return(e_d_nb_take_cut(line));
 if (nread == 0)
  return(e_d_nb_take_last(line));
 return(E_D_NB_NOTHING_YET);
}

/* What e_d_line_read_nb reads into before it hands out a line's head. */
static E_D_LINE e_d_nb_line;

/**
 * e_d_line_read_nb - Hand out the head of the next line gdb wrote, or its
 * prompt, without waiting.
 * @n: gdb's output; @s, @max: the caller's buffer.
 *
 * For callers that only look at a line's start, such as the debugger's
 * banner read while it starts; the line is still read whole, so what follows
 * it is read as the next line.
 * Return: as e_d_line_read_nb_whole.
 */
int e_d_line_read_nb(int n, signed char *s, int max)
{
 int ret = e_d_line_read_nb_whole(n, &e_d_nb_line);

 if (ret == E_D_NB_PROMPT || ret == E_D_NB_LINE)
 {
  strncpy((char *)s, (char *)e_d_nb_line.s, max - 1);
  s[max - 1] = '\0';
 }
 return(ret);
}

static FENSTER *e_d_find_messages_window(void)
{
 int i;
 ECNT *cn = WpeEditor;
 for (i = cn->mxedt; i > 0; i--)
  if (!strcmp(cn->f[i]->datnam, "Messages"))
   return cn->f[i];
 return NULL;
}

/* forward declarations */
static void e_d_drain_pty_to_messages(void);
static void e_d_messages_place_cursor(FENSTER *mf);

int _messages_activated = 0;

static void e_d_messages_erase_char(BUFFER *b, int last)
{
 int len = b->bf[last].len;

 if (len > 0)
 {
  b->bf[last].len = len - 1;
  b->bf[last].s[len - 1] = '\0';
  b->b.x = len - 1;
 }
}

static void e_d_messages_insert_char(BUFFER *b, int last, int c)
{
 int len = b->bf[last].len;

 if (len < b->mx.x - 1)
 {
  b->bf[last].s = REALLOC(b->bf[last].s, len + 2);
  b->bf[last].s[len] = c;
  b->bf[last].s[len + 1] = '\0';
  b->bf[last].len = len + 1;
  b->b.x = len + 1;
 }
}

static void e_d_messages_append_char(FENSTER *mf, int c)
{
 BUFFER *b = mf->b;
 int last = b->mxlines - 1;

 if (c == '\r')
  return;
 if (c == '\n' || last < 0)
 {
  e_new_line(b->mxlines, b);
  b->b.y = b->mxlines - 1;
  b->b.x = 0;
  return;
 }
 if (c == '\b' || c == 0x7F)
 {
  e_d_messages_erase_char(b, last);
  return;
 }
 b->b.y = last;
 e_d_messages_insert_char(b, last, c);
}

static void e_d_pty_ensure_own_line(FENSTER *mf)
{
 BUFFER *b = mf->b;
 int last = b->mxlines - 1;

 if (e_d_async.pty_owns_last_line)
  return;
 if (last >= 0 && b->bf[last].len > 0)
  e_d_messages_append_char(mf, '\n');
 e_d_async.pty_owns_last_line = 1;
}

/* Ensure the capture buffer has room for `extra` more bytes beyond the current
   length.  Returns 0 on success, -1 if the buffer could not be grown -- in
   which case the existing allocation is preserved intact and the caller must
   skip its copy rather than dereference a freed pointer. */
static int e_d_prog_output_reserve(int extra)
{
 char *grown;
 int newcap;

 if (e_d_prog_output_len + extra <= e_d_prog_output_cap)
  return 0;
 newcap = (e_d_prog_output_len + extra + 1024) * 2;
 grown = REALLOC(e_d_prog_output, newcap);
 if (!grown)
  return -1;
 e_d_prog_output = grown;
 e_d_prog_output_cap = newcap;
 return 0;
}

/* Append `len` raw bytes to the capture buffer, keeping it NUL-terminated.
   The NUL sits at index e_d_prog_output_len and is not counted in the length,
   so length-bounded readers (Ctrl-G P) and string readers both stay safe. */
static void e_d_prog_output_append(char *data, int len)
{
 if (e_d_prog_output_reserve(len + 1) < 0)
  return;
 memcpy(e_d_prog_output + e_d_prog_output_len, data, len);
 e_d_prog_output_len += len;
 e_d_prog_output[e_d_prog_output_len] = '\0';
}

/* Append one line of captured program output followed by a newline.  Used by
   the jdb/pdb step parsers, which record each emitted line for Ctrl-G P
   replay. */
static void e_d_prog_output_append_line(char *data, int len)
{
 e_d_prog_output_append(data, len);
 e_d_prog_output_append("\n", 1);
}

/* Public hooks so the Ctrl-F9 run path (we_prog.c) can feed the same captured
   buffer the debugger uses, letting the Alt-F5 "User Screen" replay the last
   program's raw output verbatim regardless of how it was launched.
   e_d_prog_output_reset() starts a fresh capture at the top of each run. */
void e_d_prog_output_reset(void)
{
 e_d_prog_output_len = 0;
}

void e_d_prog_output_add(char *data, int len)
{
 e_d_prog_output_append(data, len);
}

/**
 * e_d_pty_hung_up - Did a read of the pty master find the program's side
 * closed?
 * @n: what read() returned; @err: errno after it.
 *
 * Once every holder of the slave has closed it, Linux answers EIO and the
 * BSDs and macOS answer end of file; the master then stays readable for
 * ever, so a reader that kept watching it would wake the editor without
 * end.  Under gdb the editor's debugger keeps the slave open and this never
 * happens; under jdb the program's VM is the only holder.
 * Return: 1 when the program's side is gone.
 */
static int e_d_pty_hung_up(int n, int err)
{
 if (n == 0)
  return(1);
 return(n < 0 && err == EIO);
}

/* How many reads of the pty, or of a debugger's pipe, one wake of the fd
   loop takes: a program or debugger that prints without end must not keep
   the editor from the user's keys (Debug > Quit among them).  The rest is
   read at the next wake. */
#define E_D_READS_PER_WAKE     64
/* Read until the pty holds nothing more: at a stop the program prints no
   more, and all it printed belongs in Messages before the mark. */
#define E_D_PTY_READS_ALL      0

/**
 * e_d_pty_append_to_messages - Write a piece the program printed at the end
 * of Messages.
 * @mf: the Messages window; @buf, @n: the piece.
 */
static void e_d_pty_append_to_messages(FENSTER *mf, const char *buf, int n)
{
 int i;

 e_d_pty_ensure_own_line(mf);
 for (i = 0; i < n; i++)
 {
  if (buf[i] == '\n')
   e_d_async.pty_owns_last_line = 0;
  e_d_messages_append_char(mf, (unsigned char)buf[i]);
 }
}

/**
 * e_d_pty_read_to_messages - Take what the program printed: into Messages
 * when it is open, and always onto the User Screen.
 * @mf: the Messages window, or NULL when the user closed it;
 * @max_reads: at most this many reads, or E_D_PTY_READS_ALL.
 *
 * With Messages closed the output is read all the same: the user finds it
 * on the User Screen (Alt-F5), and the pty, emptied, no longer wakes the
 * editor again and again while the program runs.
 * Return: the bytes read.
 */
static int e_d_pty_read_to_messages(FENSTER *mf, int max_reads)
{
 char buf[256];
 int n = 1, total = 0, err, reads = 0;

 if (e_d_pty_master < 0)
  return 0;
 while ((max_reads == E_D_PTY_READS_ALL || reads++ < max_reads) &&
        (n = read(e_d_pty_master, buf, sizeof(buf))) > 0)
 {
  e_d_prog_output_append(buf, n);
  if (mf)
   e_d_pty_append_to_messages(mf, buf, n);
  total += n;
 }
 err = errno;
 if (e_d_pty_hung_up(n, err))
  e_d_pty_hung = 1;
 return total;
}

static void e_d_messages_redraw(FENSTER *mf)
{
 e_messages_scroll_to_bottom(mf);
 e_ed_rahmen(mf, 1);
 e_schirm(mf, 1);
 e_cursor(mf, 1);
 e_refresh();
}

static void e_d_on_pty_readable(int fd, void *data);

/* DAP debug bridge (Go/dlv and other Debug Adapter Protocol backends).  The
   editor-facing glue lives here, next to the other backend helpers, because it
   needs the file-scope debugger globals (e_d_swtch, e_d_file, the breakpoint and
   watch arrays).  The protocol engine itself is in we_dap.c / we_dap_proto.c,
   free of editor state, so it can be unit-tested against a real adapter.
   Forward-declared here because e_d_quit and e_d_p_watches (defined above) hook
   into them. */
static int  e_d_dap_active(void);
static int  e_d_dap_died_line(char *out, size_t n);  /* the adapter died */
static int  e_d_scala_setup_first(FENSTER *f, const char *dir); /* setup-ide */
/* e_start_debug's answer while the build it needs (or the Scala build's
   setup) is running: the command that asked returns quietly and is run again
   when it has ended. */
#define E_D_BUILDING (-3)
static void e_scala_setup_end_at_exit(void);    /* setup-ide at the end */
static void e_d_after_build(FENSTER *f, int result, void *data);  /* (below) */
static void e_d_resume_drop(void);              /* nothing runs again */
static void e_d_compile_deliver_held(void);     /* compile for debug */
static void e_d_compile_end_at_exit(void);
static void e_d_debugger_deliver_held(void);    /* the debugger's end */
static void e_d_start_deliver_held(void *data); /* all of the start's holds */
static void e_d_start_deliver_later(void);      /* (below) */
static int  e_d_dap_source_ext(const char *datnam);
static int  e_d_dap_start(FENSTER *f);
static int  e_d_dap_run(FENSTER *f);
static int  e_d_dap_step(FENSTER *f, int sw);
static int  e_d_dap_watches(FENSTER *f, int sw);
static void e_d_dap_quit(FENSTER *f);
static int  e_d_dap_exit_code(long *code);      /* the `exited` event's code */
static int  e_d_p_frames(ECNT *cn, int is, int sw); /* the call stack (below) */
static int  e_d_frame_show(FENSTER *f);
static int  e_d_frames_backend(void);
static int  e_d_dap_start_watch(FENSTER *f);   /* the adapter's start (below) */
static int  e_d_dap_start_finish(FENSTER *f);
static void e_d_dap_start_close_messages(ECNT *cn);
static void e_scala_pin_jdk(FENSTER *f, const char *who);  /* pin Scala JVM (below) */
static void e_d_brk_forget_start_entries(void);   /* breakpoints (below) */
static void e_d_brk_state_reserve(int n);         /* their state (below) */
static void e_d_brk_forget_session(void);
static int  e_d_brk_sync_at_stop(ECNT *cn);       /* applied at a stop */
static int  e_d_brk_changed_while_running(void);  /* anything to apply? */
static void e_d_watches_after_session(ECNT *cn);  /* watches (below) */
static int  e_d_jdb_sync_live(ECNT *cn);          /* applied at once (below) */
static int  e_d_dap_sync_live(ECNT *cn);
/* Set when the debugger's word on a breakpoint arrived while the program
   runs (jdb, the adapters), so the windows are drawn again with its mark. */
static int  e_d_brk_live_changed;
/* The offset in jdb's run answer up to which its words on breakpoints sent
   while the program runs are read, so each is acted on once. */
static int  e_d_jdb_live_scanned;
/* How many breakpoints jdb was sent "clear" for in this run (the record is
   e_d_jdb_cleared, below). */
static int  e_d_jdb_ncleared;
static void e_d_dap_on_breakpoints(const char *file, const int *line,
                                   const int *verified,
                                   const char *const *message, int n,
                                   void *ud);      /* the adapter's word */
static void e_d_cursor_brk_clear(void);           /* Go to cursor's (below) */
/* The debug adapters' answers on the fd loop (below, after the line
   debuggers' engine, whose waiting line and kept keys they share). */
static void e_d_dr_begin(void);
static void e_d_dr_forget(void);
static void e_d_dr_run_begins(void);
static void e_d_dr_watch(void);
static int  e_d_dr_answering(void);
static int  e_d_dr_busy(void);
static void e_d_dr_cancel(void);
static void e_d_dr_refusal_line(char *line, size_t n);
static void e_d_dap_stop_ask(FENSTER *f, int pause);
static int  e_d_dap_watches_ask(ECNT *cn, int sw);
static void e_d_dap_close_begin(e_dap_session *s, e_bsp_session *bsp);
static void e_d_dap_adapter_ended(e_dap_session *s, void *data);
static void e_d_act_schedule(void);               /* the kept keys (below) */
static int  e_d_gdb_run_to_line(FENSTER *f, int line);   /* gdb's (below) */
static int  e_d_goto_cursor_line(FENSTER *f, int line);  /* Go to cursor's */

/* What a slot of the answer lines shows when its memory could not be had. */
static char e_d_out_none[1];

/**
 * e_d_out_show - Point the stop parsers (e_d_sp) at the answer lines, oldest
 * first.
 */
static void e_d_out_show(void)
{
 int i;

 for (i = 0; i < SVLINES; i++)
 {
  if (e_d_out_ln[i].s)
   e_d_sp[i] = (char *)e_d_out_ln[i].s;
  else
   e_d_sp[i] = e_d_out_none;
 }
}

/**
 * e_d_out_clear - Forget the answer lines before a run or a step is read,
 * so a stop is never parsed out of the last one's answer.
 */
static void e_d_out_clear(void)
{
 int i;

 for (i = 0; i < SVLINES; i++)
  if (e_d_line_room(&e_d_out_ln[i], 0) == 0)
   e_d_out_ln[i].s[0] = '\0';
 e_d_out_none[0] = '\0';
 e_d_out_show();
}

/**
 * e_d_out_rotate - Make the oldest answer line the newest one, to be read
 * or copied into.
 * Return: that line.
 */
static E_D_LINE *e_d_out_rotate(void)
{
 E_D_LINE oldest = e_d_out_ln[0];
 int i;

 for (i = 1; i < SVLINES; i++)
  e_d_out_ln[i-1] = e_d_out_ln[i];
 e_d_out_ln[SVLINES-1] = oldest;
 e_d_out_show();
 return(&e_d_out_ln[SVLINES-1]);
}

/**
 * e_d_out_keep - Keep a whole line of gdb's answer as the newest answer line.
 * @line: the line.
 *
 * The line is kept to its end, so the stop parsers find the "at file:line"
 * that gdb prints after a frame of any length.
 */
static void e_d_out_keep(const char *line)
{
 E_D_LINE *newest = e_d_out_rotate();
 int len = (int)strlen(line);

 if (e_d_line_room(newest, len) < 0)
  return;
 memcpy(newest->s, line, len + 1);
 e_d_out_show();
}

/* The run controller: one way for every debugger to let the program run
   while the editor keeps its keys.  A run, a continue or a step is handed to
   it once the debugger has been told; it watches the debugger's answer and
   the program's output on the fd loop and comes back to the backend when the
   debugger stops the program or goes away.  A run has no deadline: the
   program may take an hour, and the user keeps Debug > Quit meanwhile. */

/* What a backend's reader found in what the debugger has written so far. */
#define E_D_FEED_GOING    0   /* nothing yet that ends the run */
#define E_D_FEED_STOPPED  1   /* the debugger asks for a command: a stop */
#define E_D_FEED_GONE     2   /* its output ended: the debugger is gone */

/* Whether a run is in the debugger's hands. */
#define E_D_RUN_IDLE      0
#define E_D_RUN_GOING     1

/* Where a Pause stands during a run. */
#define E_D_PAUSE_NONE    0   /* the user has not asked */
#define E_D_PAUSE_ASKED   1   /* the interrupt is sent; the next stop answers */
#define E_D_PAUSE_TAKEN   2   /* the stop that came was the interrupt's */

/* What the one interrupt in flight during a run is for. */
#define E_D_INT_NONE      0   /* none is in flight */
#define E_D_INT_PAUSE     1   /* the user's Pause: the stop is shown */
#define E_D_INT_SYNC      2   /* breakpoints changed: apply them, run on */

/* What a backend's sync_stop made of the interrupt's stop. */
#define E_D_SYNC_READY    0   /* the debugger takes commands; run on after */
#define E_D_SYNC_LEAVING  1   /* still in the debugger's own code: a command
                                 to leave it was sent and the run goes on */

/* What the controller asks of the backend that runs the program. */
typedef struct
{
 const char *name;                    /* the backend, as the user knows it */
 int  (*answer_fd)(void);             /* the debugger's answer, waited on */
 int  (*output_fd)(void);             /* the program's own output, or -1 */
 int  (*feed)(FENSTER *f);            /* read what has come: E_D_FEED_* */
 void (*stopped)(FENSTER *f);         /* mark the stop, or say the end */
 /* Pause: make the running program stop, so the stop that follows is the
    user's.  Returns 0 when the interrupt was sent.  NULL for a backend
    that cannot pause yet (e_d_pause says so). */
 int  (*interrupt)(void);
 /* Telling the debugger of breakpoints changed while the program runs.
    sync_live sends them at once, for a debugger that reads commands while
    the program runs (jdb, the adapters); NULL for the rest, which the
    controller interrupts (interrupt_stop) or leaves to the next stop.
    sync_at_stop sends them once the program has stopped, and every backend
    plugs the same list-driven function in. */
 int  (*sync_live)(ECNT *cn);
 int  (*sync_at_stop)(ECNT *cn);
 /* Let the program run on to a source line, handing the run to the
    controller again.  Go to cursor as the first command uses it once the
    start has stopped; NULL for a backend that does not run on yet. */
 int  (*run_to_line)(FENSTER *f, int line);
 /* A run was handed to the controller: forget the answer of the run before,
    for a backend whose reader keeps one across reads.  NULL for the rest. */
 void (*begin)(void);
 /* The program's own output arrived on output_fd().  NULL for a backend
    whose output is the editor's pty (gdb, jdb, sdb, dbx, xdb). */
 void (*on_output)(int fd, void *data);
 /* Nonzero when the editor has no way to give the running program the
    text the user types, although it has an output descriptor. */
 int no_input;
 /* For a debugger interrupted to take breakpoint changes (gdb, pdb, a68g):
    is the stop just read the one an interrupt made?  @taken: the
    interrupt's stop was seen already and the backend is leaving its own
    code.  Reads nothing.  NULL for a backend that is never interrupted for
    a change, which then waits for the next stop. */
 int  (*interrupt_stop)(int taken);
 /* At that stop, before the changes are sent: say what the program
    printed, and leave the debugger's own code if the stop is in it.
    Return: E_D_SYNC_READY or E_D_SYNC_LEAVING.  NULL: always ready. */
 int  (*sync_stop)(FENSTER *f);
 /* At a stop of the program's own while interrupts are still on their
    way (@owed of them, sent for @why, an E_D_INT_*): take back what the
    debugger did with them.  Return: how many are still pending in the
    program, to come back at its next resume.  NULL: none is counted. */
 int  (*settle)(ECNT *cn, int owed, int why);
 /* Start reading a new answer before a command sent from a stop; NULL
    for a backend whose reader keeps no answer across reads. */
 void (*answer_reset)(void);
 /* Nonzero when the program shares the debugger's process and can take
    the interrupt for itself with a SIGINT handler of its own (pdb); gdb's
    and a68g's programs never see it. */
 int program_takes_sigint;
 /* At a stop the debugger made: is it a breakpoint the user has removed
    since, whose removal the debugger has not yet taken?  Such a stop is
    not shown; the run goes on.  Reads nothing.  NULL: never. */
 int  (*stale_stop)(void);
 /* At the stop an interrupt made for a breakpoint change: tell the
    debugger of the changes through requests on the fd loop, and run on when
    they are answered, so the editor keeps its keys meanwhile.  Return: 1,
    the stop is dealt with.  NULL: the changes are told at once, and
    answered before the editor reads its next key. */
 int  (*sync_on_loop)(ECNT *cn);
} E_D_RUN_OPS;

static const E_D_RUN_OPS *e_d_pdb_run_ops(void);   /* pdb's table (below) */
static const E_D_RUN_OPS *e_d_jdb_run_ops(void);   /* jdb's table (below) */
static const E_D_RUN_OPS *e_d_a68g_run_ops(void);  /* a68g's table (below) */
static const E_D_RUN_OPS *e_d_dap_run_ops(void);   /* the adapters' (below) */
static const E_D_RUN_OPS *e_d_gdbmi_run_ops(void); /* gdb over MI (below) */
static int e_d_run_to_cursor_line(FENSTER *f, int line);   /* Go to cursor */
static int e_d_ga_run_to_cursor(FENSTER *f, int line);     /* pdb's, jdb's */
static int e_d_cursor_brk_pending(void);
static int e_d_jdb_run_started(FENSTER *f, int start_brk);
static const char *e_d_jdb_continue_command(void);   /* cont or resume */

/* The run in progress. */
typedef struct
{
 int state;                  /* E_D_RUN_IDLE or E_D_RUN_GOING */
 const E_D_RUN_OPS *ops;     /* the backend that runs it */
 ECNT *cn;                   /* the editor; never a window, which the user
                                may close while the program runs */
 int start_brk;             /* the editor's start breakpoint (list entry + 1)
                                to take out at the stop, or 0 */
 int goto_line;             /* the cursor line Go to cursor still has to run
                                on to once this run stops, or 0 */
 int stop_held;             /* a stop arrived while a box or a non-text
                                window was in front; shown by
                                e_d_run_deliver_held_stop */
 int pause;                 /* E_D_PAUSE_*: Pause during this run */
 int interrupt_for;         /* E_D_INT_*: the interrupt in flight */
 int int_taken;             /* its stop was seen; the backend is still
                               leaving its own code (pdb) */
 int int_owed;              /* interrupts sent whose stop has not been seen
                               yet; kept across runs, since gdb and a68g
                               may hand one back at the next resume */
 int int_refused;           /* the program handles SIGINT itself: changes
                               wait for its next stop for this session */
 int int_quiet;             /* the debugger had written nothing unread when
                               the interrupt for a change was sent */
 int say_own_sigint;        /* that was found at this stop: say so */
 const char *cont;          /* the command that runs on after the
                               interrupt's stop, for a Run or a Continue;
                               NULL for a step, Finish or Go to cursor */
 char again[E_D_RUN_AGAIN_MAX];   /* the command this run was sent */
 int sync_pending;          /* breakpoints changed since the last key wait:
                               told to the debugger when the keys run out
                               (e_d_run_flush_live_sync) */
 int stop_on_loop;          /* the backend's @stopped asks the debugger on
                               the fd loop; the stop is finished when its
                               answers have come (e_d_run_stop_tail) */
} E_D_RUN;

static E_D_RUN e_d_run;

/* After a gdb Pause that marked a frame above the innermost one: the frame
   just below the marked one, or E_D_NO_FRAME; and the step command that
   follows the "finish" out of that call (see e_d_gdb_step_from_pause). */
#define E_D_NO_FRAME (-1)
/* gdb's commands for it. */
#define E_D_GDB_FINISH       "finish\n"
#define E_D_GDB_NEXT         "n\n"
#define E_D_GDB_STEP         "s\n"
#define E_D_GDB_ADDRESS_LEAD "0x"   /* a frame gdb prints in mid-line */
static int e_d_pause_below = E_D_NO_FRAME;
static const char *e_d_pause_follow = NULL;
static int e_d_run_resume(void);   /* keep the run going (below) */
static void e_d_run_new_command(void);         /* (below) */
static int e_d_run_interrupt(int why);         /* (below) */
extern char *e_d_msg[];                        /* the debugger's words */

/* What gdb prints when the program ends: "[Inferior 1 (process N) exited
   normally]" or "... exited with code NN]", where NN is octal. */
#define E_D_GDB_EXIT_NORMAL "exited normally"
#define E_D_GDB_EXIT_CODE   "exited with code "
#define E_D_GDB_EXIT_BASE   8
/* What pdb prints when the script calls sys.exit(N). */
#define E_D_PDB_EXIT_STATUS "Exit status: "
/* What pdb prints when the script runs off its end: a normal end, code 0,
   as gdb's "exited normally" is. */
#define E_D_PDB_FINISHED    "The program finished"
/* What pdb prints when it stops at the end of a function ("return"). */
#define E_D_PDB_RETURN      "--Return--"
/* What pdb prints when SIGINT interrupts the running script. */
#define E_D_PDB_INTERRUPTED "Program interrupted"
/* What pdb's answer to a SIGINT that meets it at its prompt holds:
   "--KeyboardInterrupt--", or "*** KeyboardInterrupt" when the signal cut
   short the command it was running. */
#define E_D_PDB_INT_ANSWER  "KeyboardInterrupt"
/* The first of those, whole: the only one a stop's text can hold.  That
   text also holds what the script printed, which may name
   KeyboardInterrupt itself (its own Ctrl-C handler), so a stop is searched
   for pdb's exact line only. */
#define E_D_PDB_INT_AT_PROMPT "--KeyboardInterrupt--"
#define E_D_EXIT_PLAIN      "Program exited."
#define E_D_EXIT_CODE_FMT   "Program exited with code %ld"
/* A program killed by a signal, in gdb's words. */
#define E_D_EXIT_SIGNAL_FMT "Program terminated with signal %s"
#define E_D_EXIT_SIGNAL_NUM_FMT "Program terminated with signal %d"
/* Said after the end line by a backend that keeps the program's output out
   of Messages (pdb hands it to Ctrl-G P only). */
#define E_D_EXIT_OUTPUT_HINT " Program output: Ctrl-G P"
#define E_D_EXIT_LINE_MAX   96
/* jdb's words for its end ("The application exited", which has no "exited"
   the shorter word of every other backend would match), for a stop (its
   prompt alone is not one: jdb echoes the prompt before the program's
   output), for an uncaught exception, and the "] " that ends its prompt
   when it waits for a command, are E_D_JDB_* of we_debsess.h. */
/* Pause under jdb: "suspend" stops every thread of the VM and answers with
   E_D_JDB_SUSPENDED and a bare "> " prompt, with no current thread (at a
   stop it answers the same line and the thread's prompt).  "threads" lists
   them, "where ID" reads one's stack and "thread ID" picks the one to look
   at (e_d_gj_pause_ask).  "resume", not "cont" (which needs a breakpoint
   event), lets all of them go again. */
#define E_D_JDB_SUSPEND     "suspend\n"
#define E_D_JDB_RESUME      "resume\n"
#define E_D_JDB_CONT        "cont\n"
#define E_D_JDB_THREAD_ID_MAX 32
/* Said on the status row when the file's language picks another debugger. */
#define E_D_SWITCH_JDB_LINE "Java file: switching to jdb debugger."
#define E_D_SWITCH_PDB_LINE "Python file: switching to pdb debugger."

/**
 * e_d_exit_code_of - Read the program's exit code from what the debugger
 * reported at its end.
 * @line: a line the debugger printed when the program ended, or NULL;
 * @code: the code.
 *
 * gdb's "exited normally" is 0 and its "exited with code 03" is octal;
 * pdb's "Exit status: 3" (after sys.exit) is decimal and its "The program
 * finished" is a normal end, 0; pdb prints both after sys.exit, the status
 * first, so the status is read first.  A debug adapter prints no line: its code
 * comes with the `exited` event.  jdb and a68g name none.
 * Return: 1 when a code is known, else 0.
 */
static int e_d_exit_code_of(const char *line, long *code)
{
 const char *p;
 char *end;

 if (e_deb_type == DEB_DAP)
  return(e_d_dap_exit_code(code));
 if (!line)
  return(0);
 if ((p = strstr(line, E_D_PDB_EXIT_STATUS)) != NULL)
 {
  *code = strtol(p + strlen(E_D_PDB_EXIT_STATUS), &end, 10);
  return(end != p + strlen(E_D_PDB_EXIT_STATUS));
 }
 if (strstr(line, E_D_PDB_FINISHED))
 {
  *code = 0;
  return(1);
 }
 if (strstr(line, E_D_GDB_EXIT_NORMAL))
 {
  *code = 0;
  return(1);
 }
 if ((p = strstr(line, E_D_GDB_EXIT_CODE)) != NULL)
 {
  *code = strtol(p + strlen(E_D_GDB_EXIT_CODE), &end, E_D_GDB_EXIT_BASE);
  return(end != p + strlen(E_D_GDB_EXIT_CODE));
 }
 return(0);
}

/**
 * e_d_output_only_on_user_screen - Does this backend keep the program's
 * output out of Messages?
 *
 * pdb shares one stream with the program, and its output is kept for
 * Ctrl-G P only; gdb, jdb, a68g and the debug adapters write it to Messages
 * too.
 * Return: 1 for pdb.
 */
static int e_d_output_only_on_user_screen(void)
{
 return(e_deb_type == DEB_PDB);
}

/**
 * e_d_exit_line_add_hint - End the exit line with where the output is.
 * @msg, @n: the line, completed in place.
 *
 * Under pdb the program's output is not in Messages, so the end
 * line says where it is, where a box used to say it and wait for a key.
 */
static void e_d_exit_line_add_hint(char *msg, size_t n)
{
 size_t len = strlen(msg);

 if (len > 0 && msg[len - 1] != '.')
  snprintf(msg + len, n - len, ".");
 len = strlen(msg);
 snprintf(msg + len, n - len, "%s", E_D_EXIT_OUTPUT_HINT);
}

/**
 * e_d_say_program_exited - Write in Messages that the program ended.
 * @line: the debugger's line that said so, or NULL for a debug adapter
 * (its code comes with an event); @f: the window on top.
 *
 * The user reads "Program exited with code N", as Turbo Debugger's
 * "Terminated, exit code N", so a failing program is told apart from one
 * that succeeded without a second run.  A debugger that names no code
 * keeps the plain "Program exited.".  Every backend but jdb ends through
 * here (jdb words its line from the VM's wait status, in
 * e_d_jdb_exit_line): one line in Messages, never a box to dismiss.
 */
static void e_d_say_program_exited(const char *line, FENSTER *f)
{
 char msg[E_D_EXIT_LINE_MAX];
 long code;

 if (e_d_exit_code_of(line, &code))
  snprintf(msg, sizeof msg, E_D_EXIT_CODE_FMT, code);
 else
  snprintf(msg, sizeof msg, "%s", E_D_EXIT_PLAIN);
 if (e_d_output_only_on_user_screen())
  e_d_exit_line_add_hint(msg, sizeof msg);
 e_d_p_message(msg, f, 0);
}

/**
 * e_d_line_says_program_exited - Does this line say the program ended?
 * @line: one line the debugger printed.
 *
 * gdb, pdb and a68g say "exited"; jdb says "The application exited", which
 * that word misses, so a Java program that ran to its end was reported as
 * missing source instead of as a program that finished.  Every backend now
 * gets its end line.
 * Return: 1 when the line says the program ended.
 */
static int e_d_line_says_program_exited(const char *line)
{
 if (strstr(line, "exited"))
  return(1);
 return(strstr(line, E_D_JDB_EXIT) != NULL);
}

/**
 * e_d_gdb_follow_up_after_finish - A step pressed after Pause first ran out
 * of the library call; now send the step itself.
 *
 * Called at each gdb stop.  When the stop is the end of the finish (not a
 * breakpoint, a signal or the program's end, which are the user's) and it
 * is in the middle of a line (gdb prints the address then), the step
 * command waiting in e_d_pause_follow goes out and the run goes on from
 * here, so the user gets one stop, on the next line of the code.  A finish
 * that returns to the start of a line is already that stop.  When an
 * interrupt may still be answered with a prompt, that prompt is taken back
 * first (e_d_ga_go_on), so it is never read as the step's stop.
 * Return: 1 when the step goes out and the run goes on.
 */
static int e_d_gdb_follow_up_after_finish(void)
{
 const char *cmd = e_d_pause_follow;
 int i, mid_line = 0;

 if (!cmd)
  return(0);
 e_d_pause_follow = NULL;
 for (i = 0; i < SVLINES; i++)
 {
  if (e_d_line_says_program_exited(e_d_sp[i]) ||
      !strncmp(e_d_sp[i], e_d_msg[ERR_BREAKPOINT],
               strlen(e_d_msg[ERR_BREAKPOINT])) ||
      !strncmp(e_d_sp[i], e_d_msg[ERR_PROGSIGNAL],
               strlen(e_d_msg[ERR_PROGSIGNAL])))
   return(0);
  if (!strncmp(e_d_sp[i], E_D_GDB_ADDRESS_LEAD,
               sizeof E_D_GDB_ADDRESS_LEAD - 1))
   mid_line = 1;
 }
 if (!mid_line)
  return(0);
 e_d_ga_go_on(cmd);
 return(1);
}

/* Whether the prompt that ends a run is kept as the newest answer line. */
#define E_D_PROMPT_DROPPED  0   /* gdb: its stop checks never saw it */
#define E_D_PROMPT_KEPT     1   /* sdb, dbx, xdb: their checks expect it */

/* The line the run's reader hands to the answer, reused per line. */
static E_D_LINE e_d_run_line;

/**
 * e_d_line_feed_stop - The prompt came: the program stopped.
 * @prompt: E_D_PROMPT_KEPT to keep the prompt as the newest answer line,
 * where the stop checks of sdb, dbx and xdb expect it.
 * Return: E_D_FEED_STOPPED.
 */
static int e_d_line_feed_stop(int prompt)
{
 if (prompt == E_D_PROMPT_KEPT)
  e_d_out_keep((char *)e_d_run_line.s);
 return(E_D_FEED_STOPPED);
}

/**
 * e_d_line_feed_answer - Read what gdb, sdb, dbx or xdb has answered so far.
 * @prompt: E_D_PROMPT_KEPT or E_D_PROMPT_DROPPED.
 *
 * Every whole line is kept as an answer line for the stop's checks, so a
 * stop line of any length is marked; the prompt says the program stopped.
 * The checks of sdb, dbx and xdb look at every answer line but the newest,
 * which their old reader always filled with the prompt: kept here too,
 * their stop and end lines are read where they always were.
 * @own: takes a line that says the run began, which is then no answer line
 * for the stop's checks; NULL for a debugger that has none.
 * Return: E_D_FEED_GOING, E_D_FEED_STOPPED or E_D_FEED_GONE.
 */
static int e_d_line_feed_answer(int prompt, int (*own)(const char *line))
{
 int ret;

 for (;;)
 {
  ret = e_d_line_read_nb_whole(wfildes[0], &e_d_run_line);
  if (ret == E_D_NB_NOTHING_YET)
   return(E_D_FEED_GOING);
  if (ret == E_D_NB_END)
   return(E_D_FEED_GONE);
  if (ret == E_D_NB_PROMPT)
   return(e_d_line_feed_stop(prompt));
  if (own && own((const char *)e_d_run_line.s))
   continue;
  if (e_d_run_line.s[0])
   e_d_out_keep((char *)e_d_run_line.s);
 }
}

/**
 * e_d_gdb_feed - Read what gdb has answered to a run or a step so far.
 * @f: the window the run was started from (unused).
 *
 * "Starting program: ..." is shown in Messages as it comes, and the line
 * that says a Continue began is no part of the stop.
 * Return: as e_d_line_feed_answer.
 */
static int e_d_gdb_feed(FENSTER *f)
{
 return(e_d_line_feed_answer(E_D_PROMPT_DROPPED, e_d_gdb_run_line_own));
}

/**
 * e_d_legacy_feed - Read what sdb, dbx or xdb has answered to a run or a
 * step so far.
 * @f: the window the run was started from (unused).
 *
 * The line dbx or xdb answers a first run with ("Running: ...") is shown in
 * Messages as it comes and is no part of the stop.
 * Return: as e_d_line_feed_answer.
 */
static int e_d_legacy_feed(FENSTER *f)
{
 return(e_d_line_feed_answer(E_D_PROMPT_KEPT, e_d_cli_run_line_own));
}

/**
 * e_d_line_answer_fd - Where gdb, sdb, dbx and xdb answer: their output pipe.
 */
static int e_d_line_answer_fd(void)
{
 return(wfildes[0]);
}

/**
 * e_d_line_output_fd - Where the program's own output arrives: the pty it
 * was given, or -1 when none could be opened.
 */
static int e_d_line_output_fd(void)
{
 return(e_d_pty_master);
}

/* Where the graphical editors' helper script records the PID of the
   debugger it starts: the script, not the debugger, is the editor's child
   there. */
#define E_D_DEBUGGER_PID_FILE "we_dbg_pid"

/**
 * e_d_debugger_pid_path - The file the debugger's PID is recorded in under
 * the graphical editors.
 * @path: PATH_MAX bytes for the answer.
 */
static void e_d_debugger_pid_path(char *path)
{
 snprintf(path, PATH_MAX, "%s/%s", e_tmp_dir, E_D_DEBUGGER_PID_FILE);
}

/**
 * e_d_debugger_pid_forget - Remove the recorded PID, so a number the
 * system hands to another process later is never signalled.
 */
static void e_d_debugger_pid_forget(void)
{
 char path[PATH_MAX];

 if (!WpeIsXwin())
  return;
 e_d_debugger_pid_path(path);
 unlink(path);
}

/**
 * e_d_debugger_pid_read - The PID the helper script recorded for the
 * debugger.
 * Return: the PID, or 0 when the script has not recorded one.
 */
static pid_t e_d_debugger_pid_read(void)
{
 char path[PATH_MAX];
 FILE *fp;
 long pid = 0;

 e_d_debugger_pid_path(path);
 fp = fopen(path, "r");
 if (!fp)
  return(0);
 if (fscanf(fp, "%ld", &pid) != 1)
  pid = 0;
 fclose(fp);
 return((pid_t)pid);
}

/**
 * e_d_interrupt_debugger - Pause the running program by sending SIGINT to
 * the debugger, as Ctrl-C does in a shell.
 *
 * gdb stops its program and reports "Program received signal SIGINT", pdb
 * reports "Program interrupted" at the line the program is on, a68g asks
 * whether to terminate (answered by e_d_a68g_keep_running_after_interrupt),
 * and sdb, dbx and xdb report the interrupt in their own words.  The user
 * gets the stop marked on the line the program was running.  The signal
 * goes to the debugger alone, never to its program's group.  In the
 * terminal editor the debugger is the editor's own child; in the graphical
 * editors the child is the helper script and the debugger is what the
 * script recorded, started by exec so that it is one process.
 * Return: 0 when the signal was sent, -1 when there is no debugger.
 */
static int e_d_interrupt_debugger(void)
{
 if (!WpeIsXwin())
  return(wpe_child_signal_pid(&e_d_dbg.child, SIGINT));
 return(wpe_child_signal_named(&e_d_dbg.child, e_d_debugger_pid_read(),
                               SIGINT));
}

static int e_d_gdb_interrupt_stop(int taken);              /* (below) */
static int e_d_gdb_settle_later(ECNT *cn, int owed, int why);  /* (below) */
static void e_d_gdb_stopped(FENSTER *f);                   /* (below) */
static int e_d_gdb_sync_on_loop(ECNT *cn);                 /* (below) */

/* gdb: a line protocol answered on one pipe, a prompt at each stop.  A
   breakpoint changed while the program runs is applied by interrupting it.
   What gdb answers at a stop is read on the fd loop, through its request
   engine (e_d_ga), so the editor keeps its keys while gdb answers; the
   breakpoints changed during a run are told by the stop's own requests, so
   gdb has no sync_at_stop. */
static const E_D_RUN_OPS e_d_gdb_run_ops =
{
 "gdb",
 e_d_line_answer_fd,
 e_d_line_output_fd,
 e_d_gdb_feed,
 e_d_gdb_stopped,
 e_d_interrupt_debugger,
 NULL,
 NULL,
 e_d_gdb_run_to_line,
 NULL,
 NULL,
 0,
 e_d_gdb_interrupt_stop,
 NULL,
 e_d_gdb_settle_later,
 NULL,
 0,
 NULL,
 e_d_gdb_sync_on_loop
};

/* sdb, dbx and xdb: gdb's line protocol, with their own stop wording.  What
   they answer at a stop is read on the fd loop through the request engine
   (e_d_ga), so the editor keeps its keys meanwhile; the breakpoints changed
   while the program ran are told by the stop's own requests, so they have
   no sync_at_stop.  They are never interrupted for a breakpoint change (how
   each takes one at that moment was never recorded): it waits for the next
   stop. */
static const E_D_RUN_OPS e_d_legacy_run_ops =
{
 "sdb, dbx or xdb",
 e_d_line_answer_fd,
 e_d_line_output_fd,
 e_d_legacy_feed,
 e_d_cli_stopped,
 e_d_interrupt_debugger,
 NULL,
 NULL,
 NULL,
 NULL
};

/**
 * e_d_run_ops_of_backend - The run controller's table for the selected
 * debugger, or NULL when its run is still read by its own reader.
 *
 * Every backend goes through the controller.  gdb without a pty for its
 * program has no output to watch; its answers are read the same way.
 */
static const E_D_RUN_OPS *e_d_run_ops_of_backend(void)
{
 if (e_deb_type == DEB_GDB && e_d_gdbmi_on())
  return(e_d_gdbmi_run_ops());
 if (e_deb_type == DEB_GDB)
  return(&e_d_gdb_run_ops);
 if (e_deb_type == DEB_DAP)
  return(e_d_dap_run_ops());
 if (e_deb_type == DEB_PDB)
  return(e_d_pdb_run_ops());
 if (e_deb_type == DEB_JDB)
  return(e_d_jdb_run_ops());
 if (e_deb_type == DEB_A68G)
  return(e_d_a68g_run_ops());
 if (e_deb_type == DEB_SDB || e_deb_type == DEB_DBX || e_deb_type == DEB_XDB)
  return(&e_d_legacy_run_ops);
 return(NULL);
}

/**
 * e_d_run_going - Is a run in the debugger's hands?
 *
 * While it is, the debugger reads no command: anything that would talk to
 * it waits for the stop.
 * Return: 1 while the program runs under the controller.
 */
static int e_d_run_going(void)
{
 return(e_d_run.state == E_D_RUN_GOING);
}

/**
 * e_d_run_takes_typed_text - Does the running program read what the user
 * types in the editor?
 *
 * Under gdb and the legacy debuggers the program has a terminal of its own,
 * so a line typed while it runs is its input.  pdb's program shares pdb's
 * stdin and has none: there the letters belong in the file being edited.
 * A debug adapter's program has a terminal of the adapter's, which the
 * editor does not write to: its letters belong in the file as well.
 * Return: 1 while a run is going and the program has its own terminal.
 */
int e_d_run_takes_typed_text(void)
{
 if (!e_d_run_going() && !e_d_ga_sync_runs())
  return(0);
 if (e_d_run.ops->no_input)
  return(0);
 return(e_d_run.ops->output_fd() >= 0);
}

/**
 * e_d_run_release_hold - Forget a held stop.
 *
 * Called when the stop is finally shown and when the session ends, so a
 * stop of a finished run is never shown later.
 */
static void e_d_run_release_hold(void)
{
 e_d_run.stop_held = 0;
}

static void e_d_running_refusal_forget(void);   /* (below, the refusals) */

/**
 * e_d_run_unwatch - Stop watching the debugger and the program, and give
 * the debugger keys back to the editor.
 *
 * Called at the stop, when the debugger went away, and when the session
 * ends, so a descriptor closed afterwards is never left on the loop.
 */
static void e_d_run_unwatch(void)
{
 int out;

 if (!e_d_run_going())
  return;
 e_d_run_release_hold();
 wpe_fd_del(e_d_run.ops->answer_fd());
 out = e_d_run.ops->output_fd();
 if (out >= 0)
  wpe_fd_del(out);
 e_d_run.state = E_D_RUN_IDLE;
 e_d_async_pending = 0;
 e_d_running_refusal_forget();
 e_pick_editor_bars(WpeEditor);   /* drawn at the end of the stop */
}

/**
 * e_d_run_forget - Forget the run in progress, because the session ends.
 *
 * Quit while the program runs ends the session at once: nothing is left
 * watching a debugger that is about to be closed.
 */
static void e_d_run_forget(void)
{
 e_d_run_unwatch();
 e_d_run.ops = NULL;
 e_d_run.cn = NULL;
 e_d_run.start_brk = 0;
 e_d_run.goto_line = 0;
 e_d_run.pause = E_D_PAUSE_NONE;
 e_d_run_new_command();
 e_d_run.int_owed = 0;
 e_d_run.int_refused = 0;
 e_d_pause_below = E_D_NO_FRAME;
 e_d_pause_follow = NULL;
}

/**
 * e_d_run_drop_start_brk_at_stop - Take the editor's start breakpoint out
 * once the program stops, not while it runs.
 * @sw: the breakpoint's list entry + 1, as e_mk_brk_main gave it.
 *
 * The debugger reads no command while the program runs, and the answer read
 * for one sent then would be the stop itself, lost to the user.
 */
static void e_d_run_drop_start_brk_at_stop(int sw)
{
 e_d_run.start_brk = sw;
}

/**
 * e_d_run_repaint_margin - Draw the text window on top again so a breakpoint
 * mark that changed shows, and nothing else on the screen is redrawn.
 * @cn: the editor.
 *
 * Clearing the desktop and drawing every window, as a window switch does,
 * rewrites the whole screen at each hidden sync; the user would see the
 * screen flash for a mark that went from hollow to solid.  Drawing the one
 * window the way a toggle does leaves the terminal writing only the cells
 * that differ.  A mark in a window below is drawn when it is next raised.
 */
static void e_d_run_repaint_margin(ECNT *cn)
{
 FENSTER *f;

 if (cn->mxedt <= 0)
  return;
 f = cn->f[cn->mxedt];
 if (!DTMD_ISTEXT(f->dtmd))
  return;
 e_schirm(f, 1);
 e_cursor(f, 1);
 e_refresh();
}

/**
 * e_d_run_sync_breakpoints - Tell the debugger the breakpoints the user set
 * and removed while the program ran, now that it reads commands again.
 * @f: the window on top.
 *
 * The marks the user made while it ran turn solid on the debugger's word, or
 * stay hollow with its reason on the status row, and a removed line no
 * longer stops the next run.  The window on top is painted again so the
 * margin shows it.
 */
static void e_d_run_sync_breakpoints(FENSTER *f)
{
 const E_D_RUN_OPS *ops = e_d_run.ops;

 if (!ops || !ops->sync_at_stop || !e_d_brk_changed_while_running())
  return;
 ops->sync_at_stop(f->ed);
 e_d_run_repaint_margin(f->ed);
}

/**
 * e_d_run_stop_chores - Take out the breakpoints the editor set for this run
 * only, now that the debugger reads commands again.
 * @f: the window the run was started from.
 *
 * Go to cursor's breakpoint and the start breakpoint stop the program once;
 * a later run goes past their lines.  A session that ended took them along.
 * The breakpoints the user changed while the program ran are told to the
 * debugger first.
 */
static void e_d_run_stop_chores(FENSTER *f)
{
 int start_brk = e_d_run.start_brk;

 e_d_run.start_brk = 0;
 if (e_d_swtch < 3)
  return;
 e_d_run_sync_breakpoints(f);
 e_d_cursor_brk_clear();
 if (start_brk)
  e_mk_brk_main(f, start_brk);
}

/**
 * e_d_run_window - The window a run's stop or end is reported through: the
 * one on top now, as for a debugger key pressed at that moment.
 *
 * The window the run was started from may be gone: the user can close it
 * (Ctrl-W) or switch away while the program runs, and the stop would then
 * be shown through freed memory.  Nothing a stop does needs that window:
 * the stop finds the source it names by file, switching to it or opening
 * it again, and the end is written to Messages.  The editor always has a
 * window on top, its clipboard window at the bottom of the stack, which is
 * never closed.  Asked again at each use: the stop itself switches windows,
 * and a stop that ends the session forgets the run.
 * @cn: the editor.
 * Return: the window on top.
 */
static FENSTER *e_d_run_window(ECNT *cn)
{
 return(cn->f[cn->mxedt]);
}

/**
 * e_d_run_goto_after_start - Run on to the cursor's line once the start of
 * a session has stopped, when Go to cursor was the command that started it.
 * @cn: the editor; @ops: the table of the run that stopped.
 *
 * Go to cursor pressed before any session first runs to the start of main,
 * like any first command; the user gets the cursor's line, as in Borland,
 * not a stop on the way.  A session the stop ended is left alone.
 */
static void e_d_run_goto_after_start(ECNT *cn, const E_D_RUN_OPS *ops)
{
 int line = e_d_run.goto_line;

 e_d_run.goto_line = 0;
 if (line <= 0 || e_d_swtch < 3 || !ops->run_to_line)
  return;
 ops->run_to_line(e_d_run_window(cn), line);
}

#define E_D_PAUSE_NO_SESSION_LINE \
 " Nothing to pause: no debugger session is running"
#define E_D_PAUSE_STOPPED_LINE \
 " Nothing to pause: the program is stopped"
#define E_D_PAUSE_NO_WAY_LINE \
 " Pause is not available under this debugger yet"
#define E_D_PAUSE_FAILED_LINE \
 " The program could not be interrupted"
#define E_D_PAUSING_LINE \
 " Pausing the program"
#define E_D_PAUSED_LINE \
 " Paused"
#define E_D_PAUSE_MISSED_LINE \
 " The interrupt did not stop the program: this is its next stop"

/** What a signal report says when SIGINT stopped the program: gdb names
    it, dbx says "interrupt", sdb gives the signal's number. */
#define E_D_SIGINT_NAME     "SIGINT"
#define E_D_INTERRUPT_WORD  "nterrupt"
#define E_D_SDB_SIGINT      "(sig 2)"

/* A signal report that is not shown: Pause's own stop is not an error. */
static char e_d_pause_no_report[] = "";

/**
 * e_d_program_runs - Is the program running under the debugger right now?
 *
 * The source windows' bottom row names Pause while this holds and the
 * stepping keys when it does not.
 * Return: 1 while a run is in the debugger's hands.
 */
int e_d_program_runs(void)
{
 return(e_d_run_going());
}

/**
 * e_d_pause_taken - The stop that arrived is the interrupt Pause sent.
 *
 * Called by a backend that recognised its own interrupt report, so the
 * status row says "Paused" and not that the interrupt missed.
 */
static void e_d_pause_taken(void)
{
 if (e_d_run.pause == E_D_PAUSE_ASKED)
  e_d_run.pause = E_D_PAUSE_TAKEN;
}

/**
 * e_d_line_says_sigint - Does this signal report name SIGINT?
 * @line: the debugger's report.
 * Return: 1 when it does, in gdb's, dbx's or sdb's words.
 */
static int e_d_line_says_sigint(const char *line)
{
 if (strstr(line, E_D_SIGINT_NAME))
  return(1);
 if (strstr(line, E_D_INTERRUPT_WORD))
  return(1);
 return(strstr(line, E_D_SDB_SIGINT) != NULL);
}

/**
 * e_d_pause_asked_for - Is this line the report of the interrupt Pause sent?
 * @line: a line of the debugger's answer.
 *
 * xdb has no fixed form for the report, so it is recognised by naming SIGINT
 * or the interrupt, and only while a Pause waits for its stop: the same
 * words in the program's own output at another stop are not taken for it.
 * Return: 1 for Pause's report.
 */
static int e_d_pause_asked_for(const char *line)
{
 return(e_d_run.pause == E_D_PAUSE_ASKED && e_d_line_says_sigint(line));
}

/**
 * e_d_pause_signal_line - The text to show for a signal the debugger
 * reported at a stop.
 * @line: the debugger's report ("Program received signal SIGINT, ...").
 *
 * The SIGINT the user asked for with Pause is not an error: nothing is
 * shown for it and the stop is marked as a pause.  Any other signal, and a
 * SIGINT nobody asked for, is shown as before.
 * Return: @line, or an empty text when it is Pause's own stop.
 */
static char *e_d_pause_signal_line(char *line)
{
 if (e_d_run.pause != E_D_PAUSE_ASKED)
  return(line);
 if (!e_d_line_says_sigint(line))
  return(line);
 e_d_pause_taken();
 return(e_d_pause_no_report);
}

/* Set when the debugger's refusal of a breakpoint went to the status row
 * during the stop being handled, so the Pause report does not wipe it. */
static int e_d_refusal_said;

/**
 * e_d_pause_report - Say on the status row how Pause ended, once the
 * program has stopped.
 * @cn: the editor.
 *
 * The user reads "Paused" when the interrupt stopped the program, and that
 * the interrupt missed when the stop was another one (the program handles
 * SIGINT itself, or a breakpoint came first).  A session the stop ended
 * says nothing.  When the debugger refused a breakpoint at this same stop its
 * words stay on the row: the pause shows in the marked line, the refusal
 * shows nowhere else.
 */
static void e_d_pause_report(ECNT *cn)
{
 int pause = e_d_run.pause;

 e_d_run.pause = E_D_PAUSE_NONE;
 if (pause == E_D_PAUSE_NONE || !e_d_session_runs())
  return;
 if (e_d_refusal_said)
  return;
 if (pause == E_D_PAUSE_TAKEN)
  e_status_say_until_key(e_d_run_window(cn), E_D_PAUSED_LINE);
 else
  e_status_say_until_key(e_d_run_window(cn), E_D_PAUSE_MISSED_LINE);
}

#define E_D_OWN_SIGINT_LINE \
 " The program handles Ctrl-C itself: breakpoint changes wait for its stops"

/**
 * e_d_run_counts_interrupts - Does the backend that runs the program keep
 * count of the interrupts it is sent?
 *
 * gdb, pdb and a68g can be left holding one after a race with a stop of
 * the program's own; the count is how the editor keeps their answers in
 * step with its commands.
 */
static int e_d_run_counts_interrupts(void)
{
 return(e_d_run.ops && e_d_run.ops->interrupt_stop != NULL);
}

/**
 * e_d_run_answer_waiting - Has the debugger written something the run has
 * not read yet?
 *
 * Looked at, without waiting, as an interrupt is sent: a debugger that has
 * stopped by itself has already written its stop, so an interrupt that
 * then stops nothing met that stop, and was not taken by the program.
 * Return: 1 when its answer is readable now.
 */
static int e_d_run_answer_waiting(void)
{
 return(wpe_fd_readable_now(e_d_run.ops->answer_fd()) == WPE_FD_READABLE);
}

/**
 * e_d_run_interrupt - Send the backend's interrupt as the one interrupt in
 * flight during this run.
 * @why: E_D_INT_PAUSE for the user's Pause, E_D_INT_SYNC to apply a
 * breakpoint change.
 * Return: 0, or -1 when it could not be sent.
 */
static int e_d_run_interrupt(int why)
{
 e_d_run.int_quiet = !e_d_run_answer_waiting();
 /* Said to the session before the signal goes out, so a stop record that
    answers it, read at any time after, is known for the editor's own. */
 e_ds_set_interrupt(&e_d_session, why);
 if (e_d_run.ops->interrupt() < 0)
 {
  e_ds_set_interrupt(&e_d_session, E_DS_INT_NONE);
  return(-1);
 }
 e_d_run.interrupt_for = why;
 e_d_run.int_taken = 0;
 if (e_d_run_counts_interrupts())
  e_d_run.int_owed++;
 return(0);
}

/**
 * e_d_run_may_interrupt_for_sync - Can a breakpoint change be applied now
 * by stopping the program for it?
 *
 * Only a Run or a Continue is interrupted for it, since it runs on with the
 * same command afterwards.  A step, Finish or Go to cursor would be cut
 * short and turned into a Continue, so a change made during one waits for
 * its stop, which comes soon.  One interrupt is in flight at a time: a
 * change made meanwhile is applied at its stop with the others.  A program
 * found to handle SIGINT itself is not interrupted again.
 * Return: 1 when the interrupt may be sent.
 */
static int e_d_run_may_interrupt_for_sync(void)
{
 const E_D_RUN_OPS *ops = e_d_run.ops;

 if (!ops->interrupt_stop || !ops->interrupt || !e_d_run.cont)
  return(0);
 if (e_d_run.interrupt_for != E_D_INT_NONE || e_d_run.int_refused)
  return(0);
 return(1);
}

/**
 * e_d_run_apply_breakpoint_changes - Get the breakpoints the user changed
 * to the debugger now.
 * @cn: the editor.
 *
 * jdb and the debug adapters are told at once.  gdb, pdb and a68g are
 * interrupted, told, and let run on, with nothing of that stop shown.
 * sdb, dbx and xdb, a step in progress, and a program that handles SIGINT
 * itself keep the change for the next stop.
 */
static void e_d_run_apply_breakpoint_changes(ECNT *cn)
{
 if (!e_d_run_going() || e_d_run.stop_held)
  return;
 if (!e_d_brk_changed_while_running())
  return;
 if (e_d_run.ops->sync_live)
 {
  e_d_run.ops->sync_live(cn);
  return;
 }
 if (!e_d_run_may_interrupt_for_sync())
  return;
 e_d_run_interrupt(E_D_INT_SYNC);
}

/**
 * e_d_run_breakpoints_changed - The user set or removed a breakpoint while
 * the program runs: get it to the debugger as soon as the keys already
 * typed have been read.
 * @cn: the editor (unused: the run keeps its own).
 *
 * The change is told to the debugger when the editor next waits for a key
 * (e_d_run_flush_live_sync), so the toggles of one burst of keys reach it
 * as their net change.  A mark set and taken off again in one burst never
 * reaches it, and a loop that passes the line a million times a second
 * cannot stop on a line the user no longer marks.  The user sees the mark
 * turn solid while the program runs, and the program stops on it the next
 * time it gets there.  A stop already read but held behind a box applies
 * it.
 */
static void e_d_run_breakpoints_changed(ECNT *cn)
{
 (void)cn;
 if (!e_d_run_going() || e_d_run.stop_held)
  return;
 e_d_run.sync_pending = 1;
}

/**
 * e_d_run_flush_live_sync - Tell the debugger of the breakpoints changed
 * since the editor last waited for a key.
 *
 * Called by the key loops of every display just before they sleep for a
 * key, once the keys already typed have all been handled.  Nothing is sent
 * for a burst whose changes cancel out.
 */
void e_d_run_flush_live_sync(void)
{
 if (!e_d_run.sync_pending)
  return;
 e_d_run.sync_pending = 0;
 if (!e_d_run.cn)
  return;
 e_d_run_apply_breakpoint_changes(e_d_run.cn);
}

/**
 * e_d_run_continues_with - The run just handed over is a Run or a Continue,
 * which @cmd lets go on after a stop the editor made for a breakpoint
 * change.
 * @cmd: the debugger's continue command, ending in a newline.
 */
static void e_d_run_continues_with(const char *cmd)
{
 if (e_d_run_going())
  e_d_run.cont = cmd;
}

/**
 * e_d_run_stop_was_interrupt - Is the stop just read the one an interrupt
 * made?
 *
 * Asked only while an interrupt is on its way or its stop is being left,
 * so a SIGINT the program raises for itself is shown as it always was.
 * The first sight of the stop pays the interrupt off.  A Pause waiting for
 * it is answered by the backend, which knows its own report of it, except
 * on the way out of pdb's own code, where that report is already behind.
 * Return: 1 for the interrupt's stop.
 */
static int e_d_run_stop_was_interrupt(void)
{
 const E_D_RUN_OPS *ops = e_d_run.ops;

 if (!ops->interrupt_stop)
  return(0);
 if (e_d_run.int_owed <= 0 && !e_d_run.int_taken)
  return(0);
 if (!ops->interrupt_stop(e_d_run.int_taken))
  return(0);
 if (!e_d_run.int_taken && e_d_run.int_owed > 0)
  e_d_run.int_owed--;
 if (e_d_run.interrupt_for == E_D_INT_PAUSE && e_d_run.int_taken)
  e_d_pause_taken();
 return(1);
}

/**
 * e_d_run_settle - A stop came that no interrupt made while interrupts were
 * on their way: let the backend take back what the debugger did with them.
 * @cn: the editor.
 *
 * Done before any command of the stop's, so every answer the user is shown
 * belongs to its own command.
 */
static void e_d_run_settle(ECNT *cn)
{
 const E_D_RUN_OPS *ops = e_d_run.ops;

 if (e_d_run.int_owed <= 0)
  return;
 if (!ops->settle)
 {
  e_d_run.int_owed = 0;
  return;
 }
 e_d_run.int_owed = ops->settle(cn, e_d_run.int_owed, e_d_run.interrupt_for);
}

/**
 * e_d_run_go_on_now - Send the debugger @cmd from a stop the user is not
 * shown, with nothing left to ask it first, and keep the run going.
 * @cmd: the command, ending in a newline.
 *
 * The program runs on as if it had not stopped; the editor keeps its keys.
 */
static void e_d_run_go_on_now(const char *cmd)
{
 if (e_d_run.ops->answer_reset)
  e_d_run.ops->answer_reset();
 e_d_out_clear();
 e_d_nb_forget();
 e_d_send_cmd(cmd);
 e_d_run_resume();
}

/**
 * e_d_run_go_on - Send the debugger @cmd from a stop the user is not shown,
 * and keep the run going.
 * @cmd: the command, ending in a newline.
 *
 * The program runs on as if it had not stopped; the editor keeps its keys.
 * gdb, pdb, jdb and a68g may first have answers to take back (e_d_ga_go_on).
 */
static void e_d_run_go_on(const char *cmd)
{
 if (e_d_ga_backend_of(e_deb_type))
 {
  e_d_ga_go_on(cmd);
  return;
 }
 e_d_run_go_on_now(cmd);
}

/**
 * e_d_run_sync_and_go_on - The interrupt sent for a breakpoint change has
 * stopped the program: tell the debugger, then let the program run on.
 * @cn: the editor.
 *
 * Nothing of this stop is shown: no mark, no watches, no "Paused".  Only
 * the margin changes: the marks the user made turn solid, or stay hollow
 * with the debugger's words on the status row.  A backend whose stop is in
 * its own code first leaves it (pdb), the run going on meanwhile.  Another
 * interrupt still on its way is settled first, so the debugger's answers
 * stay in step with the commands.
 * Return: 1, the stop is dealt with.
 */
static int e_d_run_sync_and_go_on(ECNT *cn)
{
 const E_D_RUN_OPS *ops = e_d_run.ops;

 if (ops->sync_stop &&
     ops->sync_stop(e_d_run_window(cn)) == E_D_SYNC_LEAVING)
 {
  e_d_run.int_taken = 1;
  return(1);
 }
 e_d_run.interrupt_for = E_D_INT_NONE;
 e_d_run.int_taken = 0;
 e_d_run_settle(cn);
 if (ops->sync_on_loop)
  return(ops->sync_on_loop(cn));
 e_d_run_sync_breakpoints(e_d_run_window(cn));
 e_d_run_go_on(e_d_run.cont);
 return(1);
}

/**
 * e_d_run_send_again - An interrupt a race left in the program stopped it
 * at its next resume, before it moved: send the run's command again.
 *
 * The user asked for a Continue or a step, not for this stop, and does not
 * see it.
 * Return: 1 when the command went out again, 0 when there is none (the
 * stop is then shown).
 */
static int e_d_run_send_again(void)
{
 e_d_run.interrupt_for = E_D_INT_NONE;
 e_d_run.int_taken = 0;
 if (!e_d_run.again[0])
  return(0);
 e_d_run_go_on(e_d_run.again);
 return(1);
}

/**
 * e_d_run_check_own_sigint - The interrupt sent for a breakpoint change did
 * not stop the program, and this stop is its own: did the program take the
 * interrupt with a SIGINT handler of its own?
 *
 * Such a program (a pdb script that sets a handler while it runs) is never
 * stopped by the interrupt, and would run its handler at every change.
 * The changes then wait for its stops for the rest of the session, and the
 * status row says so once.  A race with a stop of the program's own is
 * told apart by what was waiting to be read when the interrupt went: when
 * the debugger had written something, the case is left open and nothing is
 * said; the change is applied at this stop either way.  When it had not,
 * the interrupt may still have met the debugger at the prompt of this
 * stop, which pdb answers with one more prompt: the settle step that takes
 * that prompt back takes this conclusion back too
 * (e_d_run_own_sigint_disproved).
 */
static void e_d_run_check_own_sigint(void)
{
 if (!e_d_run.ops->program_takes_sigint || !e_d_run.int_quiet)
  return;
 e_d_run.int_refused = 1;
 e_d_run.say_own_sigint = 1;
}

/**
 * e_d_run_own_sigint_disproved - The debugger answered an interrupt at its
 * prompt: the stop of the program's own won the race, and the program never
 * saw the interrupt.
 *
 * What e_d_run_check_own_sigint concluded at this stop, before it was said,
 * is dropped: the user is not told the program handles Ctrl-C itself, and a
 * breakpoint changed while it runs next still stops it at once.
 */
static void e_d_run_own_sigint_disproved(void)
{
 if (!e_d_run.say_own_sigint)
  return;
 e_d_run.say_own_sigint = 0;
 e_d_run.int_refused = 0;
}

/**
 * e_d_run_stop_is_stale - Is the stop just read a breakpoint the user took
 * off before the program got there?
 *
 * Only a Run or a Continue is let go on, and only when no Pause or other
 * interrupt is in flight, whose own stop this might be taken for.  The
 * backend says whether the stop is on a line the list no longer marks and
 * whose removal the debugger had not yet taken.  The user then sees no
 * stop on an unmarked line.
 * Return: 1 for a stop to hide and run on from.
 */
static int e_d_run_stop_is_stale(void)
{
 const E_D_RUN_OPS *ops = e_d_run.ops;

 if (!ops->stale_stop || !e_d_run.cont)
  return(0);
 if (e_d_run.interrupt_for != E_D_INT_NONE || e_d_run.pause != E_D_PAUSE_NONE)
  return(0);
 return(ops->stale_stop());
}

/**
 * e_d_run_interrupt_handled - Deal with a stop an interrupt may have made,
 * before the backend shows it.
 * @cn: the editor.
 *
 * A stop on a breakpoint the user has just removed is hidden too, and the
 * program runs on.
 * The stop of an interrupt sent for a breakpoint change is never shown:
 * the change is applied and the program runs on.  A stop of the program's
 * own that comes while that interrupt is on its way (a breakpoint, the
 * end) is the user's: it is shown, the change is applied there, and
 * nothing runs on behind the user's back.  A Pause pressed meanwhile has
 * turned the interrupt into the Pause.  An interrupt a race left in the
 * program is answered by sending the run's command again.
 * Return: 1 when the stop is dealt with and nothing is to be shown.
 */
static int e_d_run_interrupt_handled(ECNT *cn)
{
 int intr;

 if (e_d_run_stop_is_stale())
  return(e_d_run_sync_and_go_on(cn));
 intr = e_d_run_stop_was_interrupt();
 if (intr && e_d_run.interrupt_for == E_D_INT_SYNC)
  return(e_d_run_sync_and_go_on(cn));
 if (intr && e_d_run.interrupt_for == E_D_INT_NONE)
  return(e_d_run_send_again());
 if (!intr && e_d_run.interrupt_for == E_D_INT_SYNC)
  e_d_run_check_own_sigint();
 if (!intr)
  e_d_run_settle(cn);
 e_d_run.interrupt_for = E_D_INT_NONE;
 e_d_run.int_taken = 0;
 return(0);
}

/**
 * e_d_run_say_own_sigint - Say, once, that the program handles Ctrl-C
 * itself, when this stop found it out.
 * @cn: the editor.
 *
 * The user then knows why a breakpoint set while it ran took effect only
 * now, and that later ones will wait for a stop too.  The words stay over
 * "Paused", as a refusal's do.
 */
static void e_d_run_say_own_sigint(ECNT *cn)
{
 if (!e_d_run.say_own_sigint || !e_d_session_runs())
  return;
 e_d_run.say_own_sigint = 0;
 e_d_refusal_said = 1;
 e_status_say_until_key(e_d_run_window(cn), E_D_OWN_SIGINT_LINE);
}

/**
 * e_d_run_stop_tail - The last of a stop: go on to the cursor if Go to
 * cursor asked, then say how a Pause ended.
 * @cn: the editor; @ops: the table of the run that stopped.
 *
 * Called once the stop is marked and its chores are done, at once for a
 * backend that answers its stop before the editor reads a key, and when the
 * last answer has come for one that answers on the fd loop (gdb).
 */
static void e_d_run_stop_tail(ECNT *cn, const E_D_RUN_OPS *ops)
{
 e_d_run_goto_after_start(cn, ops);
 if (e_d_run_going())
  return;
 e_d_run_say_own_sigint(cn);
 e_d_pause_report(cn);
}

/**
 * e_d_run_on_stop - The debugger stopped the program: mark where, then
 * tidy up after the run, and go on to the cursor if Go to cursor asked.
 *
 * A backend whose stop is not the user's yet (pdb stopped in Python's own
 * code) sends its next command and keeps the run going from @stopped: the
 * tidying then waits for the stop the user is shown.  A backend that asks
 * its stop's questions on the fd loop (gdb) does the tidying in its own
 * requests and ends with e_d_run_stop_tail once they are answered.
 */
static void e_d_run_on_stop(void)
{
 const E_D_RUN_OPS *ops = e_d_run.ops;
 ECNT *cn = e_d_run.cn;

 e_d_run_unwatch();
 e_d_refusal_said = 0;
 e_d_run.stop_on_loop = 0;
 if (e_d_run_interrupt_handled(cn))
  return;
 ops->stopped(e_d_run_window(cn));
 if (e_d_run_going() || e_d_run.stop_on_loop)
  return;
 e_d_run_stop_chores(e_d_run_window(cn));
 e_d_run_stop_tail(cn, ops);
}

/**
 * e_d_run_on_gone - The debugger went away while the program ran: the
 * session ends, and the user reads why.
 */
static void e_d_run_on_gone(void)
{
 ECNT *cn = e_d_run.cn;

 e_d_run_unwatch();
 e_d_quit(e_d_run_window(cn));
}

/**
 * e_d_run_hold_stop - The program stopped while a box or a non-text window
 * is in front: keep the stop for the editor's key loop.
 *
 * Showing it now would switch to the source over a menu, a dialog, the File
 * Manager or the Window list, whose own key loop would then go on reading
 * the user's keys underneath.  The answer is read already, so the
 * debugger's descriptor is no longer watched (it would report readable for
 * ever); the run stays going, so debugger keys still say the program runs
 * until the stop is shown by e_d_run_deliver_held_stop.
 */
static void e_d_run_deliver_held_stop(void *data);

static void e_d_run_hold_stop(void)
{
 wpe_fd_del(e_d_run.ops->answer_fd());
 e_d_run.stop_held = 1;
 wpe_paint_request(e_d_run_deliver_held_stop, NULL, "the program's stop");
}

/**
 * e_d_run_deliver_held_stop - Show a stop that arrived while a box or a
 * non-text window was in front.
 * @data: unused.
 *
 * The screen's owner runs this once the editor is back in its key loop,
 * the point that is outside every dialog, menu and non-text window, however
 * the user got back to a source window: closing the box, opening a file
 * from the File Manager, picking a window from the Window list.  The user
 * then sees the mark at the stop and the debugger keys work again.  A stop
 * of a run that has ended since is not held any more (e_d_run_unwatch drops
 * it).
 */
static void e_d_run_deliver_held_stop(void *data)
{
 (void)data;
 if (!e_d_run.stop_held || !e_d_run_going())
  return;
 e_d_run_release_hold();
 e_d_run_on_stop();
}

/**
 * e_d_brk_repaint_live_answers - Draw the margin again with the debugger's
 * word on the breakpoints, if it changed since the margin was drawn.
 * @data: the desktop.
 */
static void e_d_brk_repaint_live_answers(void *data)
{
 if (!e_d_brk_live_changed)
  return;
 e_d_brk_live_changed = 0;
 e_d_run_repaint_margin((ECNT *)data);
}

/**
 * e_d_run_show_live_answers - Draw the windows again when the debugger's
 * word on a breakpoint set or removed while the program runs has come.
 *
 * The user sees the hollow mark turn solid while the program goes on.
 * With a box or a non-text window in front the drawing waits for the box
 * to close, so nothing is drawn over it and the mark is right afterwards.
 */
static void e_d_run_show_live_answers(void)
{
 if (!e_d_brk_live_changed)
  return;
 wpe_paint_request(e_d_brk_repaint_live_answers, e_d_run.cn,
                   "a breakpoint's answer");
}

/**
 * e_d_run_on_answer - The debugger wrote while the program runs.
 * @fd: its answer; @data: unused.
 *
 * Runs on the fd loop, between the user's keys: the editor never waits for
 * the program.
 */
static void e_d_run_on_answer(int fd, void *data)
{
 int fed;

 if (!e_d_run_going())
  return;
 fed = e_d_run.ops->feed(e_d_run_window(e_d_run.cn));
 if (fed == E_D_FEED_GOING)
  e_d_run_show_live_answers();
 if (fed == E_D_FEED_STOPPED && !wpe_paint_idle())
  e_d_run_hold_stop();
 else if (fed == E_D_FEED_STOPPED)
  e_d_run_on_stop();
 else if (fed == E_D_FEED_GONE)
  e_d_run_on_gone();
}

/**
 * e_d_run_mark_going - The program runs: refuse the debugger keys until
 * the run ends, and show the program's output in Messages meanwhile.
 * @ops: the backend's table.
 */
static void e_d_run_mark_going(const E_D_RUN_OPS *ops)
{
 int out = ops->output_fd();
 int was_going = e_d_run_going();

 e_d_run.state = E_D_RUN_GOING;
 e_d_async_pending = 1;
 if (!was_going)
  e_refresh_editor_bars(WpeEditor);   /* the bar names Pause while it runs */
 if (out < 0)
  return;
 if (ops->on_output)
  e_fd_watch(out, POLLIN, ops->on_output, NULL, "the program's output");
 else if (!e_d_pty_hung)
  e_fd_watch(out, POLLIN, e_d_on_pty_readable, NULL, "the program's output");
}

/**
 * e_d_run_watch - Watch the debugger's answer and the program's output on
 * the fd loop, and keep the editor's debugger keys refused until the stop.
 * @ops: the backend's table.
 * Return: 0, or -1 when the debugger cannot be watched.
 */
static int e_d_run_watch(const E_D_RUN_OPS *ops)
{
 e_d_ga_run_begins();
 e_d_dr_run_begins();
 if (e_fd_watch(ops->answer_fd(), POLLIN, e_d_run_on_answer, NULL,
                "the debugger's answer") < 0)
  return(-1);
 e_d_run_mark_going(ops);
 return(0);
}

/**
 * e_d_run_wait_for_process - Keep the run going after the debugger said
 * the program ended, until the program's process has ended too.
 *
 * Called from a backend's @stopped (jdb, whose end comes before the
 * process's).  The debugger's answer is no longer read; the program's
 * output still reaches Messages, the user keeps every key, debugger keys
 * say the program runs, and Debug > Quit ends it.  The backend calls
 * e_d_run_process_ended when the process ends.  The debugger's own end,
 * which follows the program's, is then no surprise: it does not end the
 * session, and its end line is the program's.
 */
static void e_d_run_wait_for_process(void)
{
 e_d_dbg.end_expected = 1;
 e_d_run_mark_going(e_d_run.ops);
}

/**
 * e_d_run_process_ended - The process a run waited for has ended: deliver
 * the end as a stop, so the backend says how the program ended.
 *
 * Called from the editor's loop between keys.  With a box or a non-text
 * window in front the end is held, as a stop is, and shown once the user
 * is back at a source window.
 */
static void e_d_run_process_ended(void)
{
 if (!e_d_run_going())
  return;
 if (!wpe_paint_idle())
  e_d_run_hold_stop();
 else
  e_d_run_on_stop();
}

/**
 * e_d_run_keep_command - Keep the command the debugger was last sent as the
 * command of the run, the one sent again when an interrupt left over from a
 * race stops the program before it moved.
 *
 * A command that filled the buffer may have been cut, and a cut command
 * sent again would run something else (a step to another line): none is
 * kept then, and the stop is shown instead of a wrong command going out.
 */
static void e_d_run_keep_command(void)
{
 if (strlen(e_d_last_cmd) + 1 >= sizeof e_d_last_cmd)
 {
  e_d_run.again[0] = '\0';
  return;
 }
 snprintf(e_d_run.again, sizeof e_d_run.again, "%s", e_d_last_cmd);
}

/**
 * e_d_run_new_command - A new run, continue or step was handed over: no
 * interrupt of this run is in flight yet, and until the caller says it is a
 * Run or a Continue, a breakpoint change waits for its stop.
 *
 * The interrupts still owed by the run before are kept: gdb and a68g hand
 * one back at this resume.
 */
static void e_d_run_new_command(void)
{
 e_d_run.interrupt_for = E_D_INT_NONE;
 e_d_run.int_taken = 0;
 e_d_run.say_own_sigint = 0;
 e_d_run.cont = NULL;
 e_d_run_keep_command();
}

/**
 * e_d_run_begin - Hand a run, a continue or a step the debugger was just
 * told about to the fd loop, and give the editor back its keys.
 * @f: the source window; @ops: the backend's table;
 * @start_brk: the editor's start breakpoint to take out at the stop, or 0.
 *
 * Until the stop the user can type the program's input, use the menus,
 * close windows and quit the session; debugger commands are refused with a
 * line that says the program is running.  Only the editor is kept, not
 * @f, so a window closed meanwhile is never used (see e_d_run_window).
 * When the loop cannot watch the debugger, its answer would never be read
 * and the session would wait for ever: the bottom row says why and the
 * caller ends the session.  The program's own output, if it cannot be
 * watched, is still shown at the stop.
 * Return: 0, or -1 when the debugger cannot be watched.
 */
static int e_d_run_begin(FENSTER *f, const E_D_RUN_OPS *ops, int start_brk)
{
 e_d_out_clear();
 if (ops->begin)
  ops->begin();
 if (e_d_run_watch(ops) < 0)
  return(-1);
 e_d_run.ops = ops;
 e_d_run.cn = f->ed;
 e_d_run.start_brk = start_brk;
 e_d_run.goto_line = 0;
 e_d_run.pause = E_D_PAUSE_NONE;
 e_d_run_new_command();
 e_d_pause_below = E_D_NO_FRAME;
 e_d_pause_follow = NULL;
 jdb_trace("e_d_run_begin: the program runs under %s\n", ops->name);
 e_d_async_reset();
 return(0);
}

/**
 * e_d_run_resume - Keep the run going after its stop was handled, because
 * the backend sent the debugger one more command from it.
 *
 * Called from a backend's @stopped, so the answer to that command is read
 * on the fd loop like the run's own and the editor keeps its keys.  What
 * the run was given (start breakpoint, Go to cursor's line) is kept.  When
 * the debugger cannot be watched the session ends, as at the start.
 * Return: 0, or -1 when the session ended.
 */
static int e_d_run_resume(void)
{
 ECNT *cn = e_d_run.cn;

 e_d_run_keep_command();
 if (e_d_run_watch(e_d_run.ops) == 0)
  return(0);
 e_d_quit(e_d_run_window(cn));
 return(-1);
}

/**
 * e_d_run_hand_over - Let the program run under the controller, or end the
 * session when the debugger cannot be watched.
 * @f: the source window; @ops: the backend's table;
 * @start_brk: the editor's start breakpoint to take out at the stop, or 0.
 * Return: 0, or the end of the session.
 */
static int e_d_run_hand_over(FENSTER *f, const E_D_RUN_OPS *ops, int start_brk)
{
 if (e_d_run_begin(f, ops, start_brk) < 0)
  return(e_d_quit(f));
 return(0);
}

void e_d_place_cursor_in_messages(void)
{
 FENSTER *mf = e_d_find_messages_window();

 if (mf)
  e_d_messages_place_cursor(mf);
}

static void e_d_activate_messages_window(void)
{
 _messages_activated = 1;
}

static void e_d_messages_place_cursor(FENSTER *mf)
{
 BUFFER *b = mf->b;
 SCHIRM *s = mf->s;
 int last = b->mxlines - 1;
 int row, col;

 if (last < 0) return;
 if (!e_d_async.pty_owns_last_line && b->bf[last].len > 0)
 {
  row = mf->a.y + 1 + (last + 1 - s->c.y);
  col = mf->a.x + 1;
 }
 else
 {
  row = mf->a.y + 1 + (last - s->c.y);
  { extern int e_utf8_visual_len(unsigned char *, int);
    col = mf->a.x + 1 +
     e_utf8_visual_len((unsigned char *)b->bf[last].s, b->bf[last].len);
  }
 }
 if (row > mf->e.y - 1) row = mf->e.y - 1;
 if (col > mf->e.x - 1) col = mf->e.x - 1;
 fk_locate(col, row);
 e_refresh();
}

/**
 * e_d_pty_unwatch_if_hung - Stop watching the pty master once the
 * program's side of it is gone.
 * @fd: the master, as the fd loop watches it.
 *
 * The master of a pty nobody holds the other side of is readable for ever;
 * left on the loop it would keep the editor awake, spinning, until the
 * session ends.
 */
static void e_d_pty_unwatch_if_hung(int fd)
{
 if (e_d_pty_hung)
  wpe_fd_del(fd);
}

/**
 * e_d_messages_show_output - Draw Messages with the program's latest output.
 * @data: unused; Messages is looked up afresh, the user may have closed it.
 */
static void e_d_messages_show_output(void *data)
{
 FENSTER *mf = e_d_find_messages_window();

 (void)data;
 if (!mf)
  return;
 e_d_messages_redraw(mf);
 e_d_messages_place_cursor(mf);
}

/**
 * e_d_on_pty_readable - The program printed while it runs: show it.
 * @fd: the pty master; @data: unused.
 *
 * The user sees the text in Messages as it comes, or on the User Screen
 * when Messages is closed; while a box owns the screen it is kept in
 * Messages and drawn when the box closes.  It is read either way: text left
 * in the pty would wake the editor again at once, and it would use a whole
 * core until the program stopped.
 */
static void e_d_on_pty_readable(int fd, void *data)
{
 FENSTER *mf = e_d_find_messages_window();
 int n;

 (void)data;
 n = e_d_pty_read_to_messages(mf, E_D_READS_PER_WAKE);
 e_d_pty_unwatch_if_hung(fd);
 if (n > 0 && mf)
  wpe_paint_request(e_d_messages_show_output, NULL, "program output");
}

static void e_d_pty_flush_line(FENSTER *f, int force)
{
 if (e_d_async.pty_line_len == 0)
  return;
 if (!force && !memchr(e_d_async.pty_line_buf, '\n', e_d_async.pty_line_len))
  return;
 e_d_async.pty_line_buf[e_d_async.pty_line_len] = '\0';
 e_d_pty_strip_cr(e_d_async.pty_line_buf, e_d_async.pty_line_len);
 if (e_d_async.pty_line_buf[0])
  e_d_p_message(e_d_async.pty_line_buf, f, 0);
 e_d_async.pty_line_len = 0;
}

void e_d_pty_flush_to_messages(FENSTER *f)
{
 char buf[256];
 int n;

 if (e_d_pty_master < 0)
  return;
 while ((n = read(e_d_pty_master, buf, sizeof(buf))) > 0)
 {
  int space = sizeof(e_d_async.pty_line_buf) - e_d_async.pty_line_len - 1;
  e_d_prog_output_append(buf, n);      /* the User Screen gets all of it */
  if (n > space) n = space;
  memcpy(e_d_async.pty_line_buf + e_d_async.pty_line_len, buf, n);
  e_d_async.pty_line_len += n;
  e_d_pty_flush_line(f, 0);
 }
}

static void e_d_drain_pty_to_messages(void)
{
 FENSTER *mf;

 if (e_d_pty_master < 0)
  return;
 mf = e_d_find_messages_window();
 if (!mf)
  return;
 if (e_d_pty_read_to_messages(mf, E_D_PTY_READS_ALL) > 0)
 {
  e_d_messages_redraw(mf);
  e_d_messages_place_cursor(mf);
 }
}

/* Close pty fds but KEEP the output buffer.
   The buffer is preserved so Ctrl-G P can show program output
   even after the debugger session ends. It is freed when a
   new debug session starts (in e_exec_deb). */
void e_d_pty_close(void)
{
 if (e_d_pty_master >= 0)
 {
  e_d_pty_drain();  /* capture any remaining output before closing */
  e_d_close_fd(&e_d_pty_master);   /* keys typed and not read go with it */
 }
 if (e_d_pty_slave >= 0)
 {
  close(e_d_pty_slave);
  e_d_pty_slave = -1;
 }
 e_d_pty_slave_name[0] = '\0';
 e_d_pty_hung = 0;
 /* Buffer and its length are intentionally kept so Ctrl-G P can still show the
    captured output; e_exec_deb frees it when the next session starts.  We do
    NOT zero the capacity here: the allocation is still live, so cap must keep
    reflecting it (cap >= len) -- otherwise the invariant breaks. */
}

/* Entry 0 (ERR_CTRLCPRESS) is no longer shown: Ctrl-C now ends a wait for
   the debugger as Esc does.  It stays so the numbers of the others hold. */
char *e_d_msg[] = {  "Ctrl C pressed\nQuit Debugger ?",
			"Quit Debugger ?",
			"End of Debugging",
			"Program is not running",
			E_DS_NO_SYMBOL,         /* the session's words */
			"No Source-Code",       /*  Number 5  */
			"Start Debugger",
			"Can\'t start Debugger",
			"Starting program:",
			"Can\'t start Program",
			"Program exited",       /*   Number 10   */
			"Program terminated",
			"Program received signal",
			"Unknown Break\nQuit Debugger ?",
			"Can\'t find file %s",
			"Can\'t create named pipe",  /*   Number 15   */
			"Breakpoint",
			"(sig ",
			"program exited",
			"signal",
			"interrupt",      /* Number 20  */
			"stopped in",
			"BREAKPOINT",
			"Child process terminated normally",
			"software termination",
			"Continuing.\n",  /* Number 25  */
			"No stack.",
			"no process"
		  };

extern char *e_p_msg[];

/**
 * e_deb_inp - Ctrl-G: read the debugger command's letter and run it.
 * @f: the window the prefix was pressed in.
 *
 * The letter is always taken here, never left for the editor to type: a
 * letter whose command no longer applies (the session ended while the user
 * reached for the second key) says so on the status row or in a box and
 * leaves the text alone.  The window is looked up again after the key is
 * read, because the end of a session closes the Watches and Stack windows
 * and switches to the source, and the answer belongs on the window the user
 * now sees.  A stop that comes while the prefix waits for its letter is
 * shown after the letter: the command is the one the user chose for the
 * editor as they saw it.
 * Return: 0 when the letter was a command, else the key that was not one.
 */
int e_deb_inp(FENSTER *f)
{
 ECNT *cn = f->ed;
 int c = 0;

 c = wpe_paint_modal_getch();
 f = cn->f[cn->mxedt];
 switch(c)
 {
  case 'p':
  case ('p' - 'a' + 1):
   e_deb_out(f);
   break;
  case 'o':
  case ('o' - 'a' + 1):
   e_deb_options(f);
   break;
  case 'b':
  case ('b' - 'a' + 1):
   e_breakpoint(f);
   break;
  case 'm':
  case ('m' - 'a' + 1):
   e_remove_breakpoints(f);
   break;
  case 'a':
  case 1:
   e_remove_all_watches(f);
   break;
  case 'd':
  case ('d' - 'a' + 1):
   e_delete_watches(f);
   break;
  case 'w':
  case ('w' - 'a' + 1):
   e_make_watches(f);
   break;
  case 'e':
  case ('e' - 'a' + 1):
   e_edit_watches(f);
   break;
  case 'k':
  case ('k' - 'a' + 1):
   e_deb_stack(f);
   break;
  case 't':
  case ('t' - 'a' + 1):
   e_deb_trace(f);
   break;
  case 's':
  case ('s' - 'a' + 1):
   e_deb_next(f);
   break;
  case 'r':
  case 'c':
  case ('r' - 'a' + 1):
  case ('c' - 'a' + 1):
   e_deb_run(f);
   break;
  case 'q':
  case ('q' - 'a' + 1):
   e_d_quit(f);
   break;
  case 'u':
  case ('u'-'a'+1):
   e_d_goto_cursor(f);
   break;
  case 'f':
  case ('f'-'a'+1):
   e_d_finish_func(f);
   break;
  case 'i':
  case ('i'-'a'+1):
   e_d_pause(f);
   break;
  default:
   return(c);
 }
 return(0);
}

int e_d_q_quit(FENSTER *f)
{
 int ret = e_message(1, e_d_msg[ERR_QUITDEBUG], f);

 if (ret == 'Y')
  e_d_quit(f);
 return(0);
}

/**
 * e_d_session_runs - Is a debugger session open?
 *
 * The debugger's keys and its bar are offered only while a session runs, so
 * F4 stays Search and a source window keeps its editing keys the rest of the
 * time: the user is never shown a key that does something else.
 * Return: 1 while a session is starting, stopped or running.
 */
int e_d_session_runs(void)
{
 return(e_d_swtch > 0);
}

int e_debug_switch(FENSTER *f, int c)
{
 switch (c)
 {
  case F7:
   e_deb_trace(f);
   break;
  case F8:
   e_deb_next(f);
   break;
  case CF10:
   e_deb_run(f);
   break;
  case CF2:
   e_d_q_quit(f);
   break;
  case CtrlBrk:
   e_d_pause(f);
   break;
  default:
   if (f->ed->edopt & ED_CUA_STYLE)
   {
    switch (c)
    {
     case F5:
      e_breakpoint(f);
      break;
     case CF5:
      e_make_watches(f);
      break;
     case CF3:
      e_deb_stack(f);
      break;
     default:
      return(c);
    }
   }
   else
   {
    switch (c)
    {
     case F4:
      /* Borland's F4 is Go to cursor while a program is stopped and Search
         the rest of the time, so the key does what the bar under it says:
         only a source window whose bar offers it runs to the cursor, never
         Messages or a backend that cannot. */
      if (!e_bar_offers_goto_cursor(f))
       return(c);
      e_d_goto_cursor(f);
      break;
     case CF8:
      e_breakpoint(f);
      break;
     case CF7:
      e_make_watches(f);
      break;
     case CF6:
      e_deb_stack(f);
      break;
     default:
      return(c);
    }
   }
 }
 return(0);
}

/**
 * e_d_err_is_line_end - Say whether a byte ends a line of the debugger's
 * error stream, whose reader has never ended a line at a carriage return.
 */
static int e_d_err_is_line_end(signed char c)
{
 return(c == '\n' || c == '\0' || c == EOF);
}

/**
 * e_d_err_skip_rest - Drop what has arrived of the rest of a cut line on the
 * debugger's error stream.
 * @n: the error stream, which is never waited on.
 *
 * A warning that names a deeply nested file can be longer than the buffer it
 * is read into; its start is kept and the rest must not come back as a
 * warning of its own.  At most one line's worth is dropped per call, so a
 * debugger that floods its error stream without a newline cannot hold the
 * editor here.
 * Return: 1 once the line's end was dropped, 0 while more of it may follow.
 */
static int e_d_err_skip_rest(int n)
{
 signed char c;
 int k;

 for (k = 0; k < E_D_LINE_MAX; k++)
 {
  if (read(n, &c, 1) != 1)
   return(0);
  if (e_d_err_is_line_end(c))
  {
   e_d_cut.err_discarding = 0;
   return(1);
  }
 }
 return(0);
}

/*  Input Routines   */
static int e_e_line_read(int n, signed char *s, int max)
{
 int i, ret = 0;

 if (e_d_cut.err_discarding && !e_d_err_skip_rest(n))
  return(-1);
 for (i = 0; i < max - 1; i++)
 {
  ret = read(n, s + i, 1);
  if (ret != 1 || e_d_err_is_line_end(s[i]))
   break;
 }
 if (ret != 1 && i == 0)
  return(-1);
 if (i == max - 1)
 {
  /* The line filled the buffer: keep its start, drop its end. */
  s[i] = '\0';
  e_d_cut.err_discarding = 1;
  e_d_err_skip_rest(n);
 }
 else if (ret == 1)
  s[i+1] = '\0';          /* the line keeps its end, as it always has */
 else
  s[i] = '\0';            /* nothing more yet: the bytes read so far */
 jdb_trace("e_e_line_read: fd=%d i=%d ret=%d s='%s'\n", n, i, ret, (char*)s);
 /* Debuggers print their prompt on the output stream: an error line is never
    one, and taking it for one ends a "where" as if the program had exited. */
 return(2);
}

/**
 * e_d_prompt_text - The word prompt the current debugger emits, or NULL.
 *
 * sdb, xdb and jdb are recognised by their trailing characters instead, and the
 * DAP client speaks a framed protocol of its own, so neither has a prompt string
 * here.  Keeping the mapping beside the reader is what lets the four backends
 * that do have one share a single recognition path, instead of four copies of
 * the same arithmetic -- which is how a 7-byte prompt came to be matched with
 * offsets written for a 6-byte one.
 * Return: a prompt literal, or NULL when this backend has none.
 */
static const char *e_d_prompt_text(void)
{
 switch (e_deb_type)
 {
  case DEB_GDB:  return("(gdb) ");
  case DEB_DBX:  return("(dbx) ");
  case DEB_PDB:  return("(Pdb) ");
  case DEB_A68G: return("(a68g) ");
  default:       return(NULL);
 }
}

/**
 * e_d_line_drain_errors - Drain what the debugger has written to its error
 * stream before its output is read, without spinning on a flood of it.
 * @s, @max: scratch room for one error line.
 *
 * Every drained line counts toward the runaway budget too: a backend that
 * floods stderr (a68g's abend->io_write_string recursion, whose echoes also
 * bounce back as "echo: I/O error" once its stdout pipe fills) is absorbed
 * here up to E_D_DRAIN_MAX lines per call without ever bumping the once-per-
 * call counter of the reader, so the flood would otherwise stay invisible to
 * the watchdog and keep the editor busy for hours.
 * Return: -1 for a runaway debugger, else 0.
 */
static int e_d_line_drain_errors(signed char *s, int max)
{
 int drained = 0;

 while (e_e_line_read(efildes[0], s, max) >= 0)
 {
  if (++e_d_read_runaway > E_D_RUNAWAY_MAX)
  {
   e_d_read_runaway = 0;
   s[0] = '\0';
   return(-1);
  }
  if (++drained >= E_D_DRAIN_MAX)
   break;
 }
 return(0);
}

/**
 * e_d_line_room - Make room in a whole line for one more fill.
 * @ln: the line; @len: the bytes it holds so far.
 *
 * The room grows by doubling, so a long value costs a few copies, not one
 * per fill, and never past E_D_LINE_ROOM_MAX.
 * Return: 0, or -1 when memory ran out or the room would pass that bound.
 */
static int e_d_line_room(E_D_LINE *ln, int len)
{
 size_t need = (size_t)len + E_D_LINE_MAX, size = (size_t)ln->size;
 signed char *grown;

 if (len < 0 || need > E_D_LINE_ROOM_MAX)
  return(-1);
 if (size >= need)
  return(0);
 if (size < E_D_LINE_MAX)
  size = E_D_LINE_MAX;
 while (size < need)
  size *= 2;
 if (size > E_D_LINE_ROOM_MAX)
  size = E_D_LINE_ROOM_MAX;
 grown = REALLOC(ln->s, size);
 if (!grown)
  return(-1);
 ln->s = grown;
 ln->size = (int)size;
 return(0);
}

/* The line gdb is asked to echo after interrupts it may have answered at
   its prompt: what comes before it are those answers. */
#define E_D_GDB_SETTLE_MARK "xwpe-interrupts-settled"
#define E_D_GDB_SETTLE_CMD  "echo " E_D_GDB_SETTLE_MARK "\\n\n"

/**
 * e_d_gdb_interrupt_stop - Is gdb's stop the one an interrupt made?
 * @taken: unused: gdb's stop is whole at once.
 *
 * gdb names it "Program received signal SIGINT".  A breakpoint the user set
 * while the program ran is set at this stop and the program runs on, so the
 * user never sees a stop he did not ask for.
 * Return: 1 for the interrupt's stop.
 */
static int e_d_gdb_interrupt_stop(int taken)
{
 size_t lead = strlen(e_d_msg[ERR_PROGSIGNAL]);
 int i;

 (void)taken;
 for (i = 0; i < SVLINES; i++)
  if (!strncmp(e_d_sp[i], e_d_msg[ERR_PROGSIGNAL], lead) &&
      strstr(e_d_sp[i], E_D_SIGINT_NAME))
   return(1);
 return(0);
}

/**
 * e_d_words_in - How many times @words stand in the first @len bytes of
 * @text, so the editor can tell how many of the debugger's answers of one
 * kind (its prompts, its answers to interrupts) came in what it read.
 * @words: what is counted; NULL or empty counts nothing.
 */
static int e_d_words_in(const char *text, size_t len, const char *words)
{
 const char *end = text + len, *at;
 size_t wlen;
 int n = 0;

 if (!words || !*words)
  return(0);
 wlen = strlen(words);
 while (text + wlen <= end && (at = strstr(text, words)) != NULL &&
        at + wlen <= end)
 {
  n++;
  text = at + wlen;
 }
 return(n);
}

/**
 * e_d_prompts_in - How many of the debugger's prompts @text holds.
 * @text: what the debugger wrote; @len: its length.
 */
static int e_d_prompts_in(const char *text, size_t len)
{
 return(e_d_words_in(text, len, e_d_prompt_text()));
}

/**
 * e_d_gdb_prompts_with_the_stop - How many prompts beyond its own came in
 * the line that ended the stop just read.
 *
 * gdb answers an interrupt that met it at its prompt with one more prompt.
 * Written before the editor read the stop, it arrives glued to the stop's
 * own prompt and is read with it as one line; counted here, that interrupt
 * is paid off, and a SIGINT the program later raises by itself is shown to
 * the user instead of being taken for it and run past.
 */
static int e_d_gdb_prompts_with_the_stop(void)
{
 int n;

 if (!e_d_run_line.s)
  return(0);
 n = e_d_prompts_in((const char *)e_d_run_line.s,
                    strlen((const char *)e_d_run_line.s));
 if (n <= 1)
  return(0);
 return(n - 1);
}

/**
 * e_d_gdb_settle_later - Take back the answers gdb already wrote to
 * interrupts that reached it after the program had stopped by itself, and
 * leave the rest to the first step of the stop's requests.
 * @cn: unused; @owed: the interrupts sent whose stop never came; @why:
 * unused.
 *
 * Such an interrupt is answered with "Quit" and one more prompt.  Left
 * unread, that prompt would be taken for the answer to the next command and
 * every answer after it would come one command late: the watches, the Stack
 * window and the next stop would all be wrong.  The prompts already read
 * with the stop are counted here; the ones still on their way are counted
 * by the settle step (e_d_gs_settle_cmd), which makes gdb echo a marker and
 * counts the prompts before it, on the fd loop, so the user keeps the keys.
 * An interrupt with no such answer reached the program instead, which gdb
 * reports as a SIGINT stop at its next resume.
 * Return: the interrupts that may still be owed, until the step has read
 * the rest.
 */
static int e_d_gdb_settle_later(ECNT *cn, int owed, int why)
{
 int extra;

 (void)cn;
 (void)why;
 extra = e_d_nb_prompts_held() + e_d_gdb_prompts_with_the_stop();
 e_d_nb_forget();
 owed -= extra;
 if (owed < 0)
  owed = 0;
 e_d_ga_owe_settle(owed);
 return(owed);
}

int e_d_is_watch(int c, FENSTER *f)
{
 if (strcmp(f->datnam, "Watches"))
  return(0);
 if(c == EINFG)
  return(e_make_watches(f));
 else if (c == ENTF)
  return(e_delete_watches(f));
 else
  return(0);
}

static void e_d_jdb_vm_end(void);   /* the Java program's VM (below) */
static void e_d_session_forget(void);   /* (below, with the stop readers) */
static int  e_d_output_closed;          /* (below) */

/**
 * e_d_quit_command - What the session's debugger is told to quit with: its
 * own word, and the answer to the "really quit?" it asks with a program.
 * Return: the command, or NULL for a debugger that is told nothing.
 */
static const char *e_d_quit_command(void)
{
 switch (e_deb_type)
 {
  case DEB_GDB:  return("q\ny\n");
  case DEB_XDB:  return("q\ny\n");
  case DEB_SDB:  return("q\n");
  case DEB_DBX:  return("quit\n");
  case DEB_JDB:  return("quit\n");
  case DEB_PDB:  return("q\ny\n");          /* pdb: quit + confirm */
  case DEB_A68G: return("quit\nyes\n");     /* a68g: "Terminate a68g (yes|no):" */
  default:       return(NULL);
 }
}

/**
 * e_d_quit_can_be_asked - Is the debugger told to quit on its input?
 *
 * Only one that has answered: a debugger that never did is ended by signal.
 */
static int e_d_quit_can_be_asked(void)
{
 return(rfildes[1] >= 0 && e_d_swtch >= 2 && e_d_quit_command() != NULL);
}

/**
 * e_d_debugger_end_signal - How the debugger is ended once its session is
 * over: the signal its end begins with, before the owner's grace and
 * SIGKILL.
 * @program_runs: the program was running, so the debugger reads no command.
 *
 * Measured with each debugger started as the editor starts it, its program
 * a loop that never ends (gdb 18.1 line and MI, python3 -m pdb, a68g 3.1.2,
 * jdb of OpenJDK 26):
 *  - output ended: the debugger is broken or dying.  No signal: one that
 *    is dying is reported within the grace with the status it died of,
 *    which a signal sent now could not be told from; one that runs on gets
 *    the owner's SIGKILL after the grace (e_d_debugger_end_was_the_editors);
 *  - told to quit at its prompt: no signal.  gdb kills its program and
 *    ends, pdb and a68g (the program is the debugger) end, within the grace;
 *  - program running, or never told: the quit is not read while the
 *    program runs, so SIGTERM.  gdb, pdb and a68g end at once, and gdb's
 *    program with it;
 *  - SIGKILL after the grace ends any of them, and gdb's program with gdb;
 *    without a signal the program also ends when the editor closes its
 *    terminal.  jdb's program survives jdb's quit, SIGTERM and SIGKILL
 *    alike, which is why the editor ends it first (e_d_jdb_vm_end).
 * The graphical editors' child is the script around the debugger, which
 * reads nothing: SIGTERM ends it, and the debugger reads its quit through
 * the pipe; under X11 gdb and its running program were gone within the
 * grace, at Quit and at the editor's end alike.
 * Return: SIGTERM or WPE_CHILD_NO_SIGNAL.
 */
static int e_d_debugger_end_signal(int program_runs)
{
 if (e_d_output_closed)
  return(WPE_CHILD_NO_SIGNAL);
 if (WpeIsXwin() || program_runs || !e_d_quit_can_be_asked())
  return(SIGTERM);
 return(WPE_CHILD_NO_SIGNAL);
}

/**
 * e_d_debugger_close_input - Close the debugger's input that was kept open
 * past its session for the quit queued in it.
 */
static void e_d_debugger_close_input(void)
{
 e_d_close_fd(&e_d_dbg.in_fd);
}

/**
 * e_d_debugger_end_by_editor - Begin the end of the debugger, noting first
 * that the editor ends it.
 * @sig: the signal its end begins with, or WPE_CHILD_NO_SIGNAL.
 *
 * Every end the editor causes (Quit, Esc or the deadline while it starts,
 * the end of its output, the editor's own end) goes through here, so the
 * status it then ends with, the editor's own signal, is never reported to
 * the user as a debugger that ended unexpectedly.
 */
static void e_d_debugger_end_by_editor(int sig)
{
 e_d_dbg.asked_end = 1;
 wpe_child_end(&e_d_dbg.child, sig);
}

/**
 * e_d_debugger_end - The session is over: end its debugger without waiting.
 * @sig: what e_d_debugger_end_signal chose.
 *
 * Quit returns at once; the owner of the editor's children ends the
 * debugger on its ladder, and its input stays open until it has ended, so
 * the quit command it is reading its way to is never dropped.
 */
static void e_d_debugger_end(int sig)
{
 e_d_dbg.live = 0;
 if (e_d_dbg.child.pid <= 0)
 {
  e_d_close_fd(&rfildes[1]);
  return;
 }
 e_d_debugger_close_input();
 e_d_dbg.in_fd = rfildes[1];
 rfildes[1] = -1;
 e_d_debugger_end_by_editor(sig);
}

int e_d_quit_basic(FENSTER *f)
{
 int i, kbdflgs, end_sig;

 /* How the debugger is ended is read before the session's memory of its
    output and of the running program is dropped below. */
 end_sig = e_d_debugger_end_signal(e_d_run_going());
 /* The Java program goes first: jdb's own "quit" would let a VM stopped
    at a breakpoint run on, and jdb never started it. */
 e_d_jdb_vm_end();
 e_d_start_forget();
 e_d_end_forget();
 e_d_jdb_missing_forget();
 e_d_jdb_pause_forget();
 e_d_ga_forget();
 e_d_gj_forget_events();
 e_d_run_forget();
 e_d_session_forget();
 if (!e_d_swtch)
  return 0;
 /* A debugger that never answered is not spoken to: it is ended below. */
 if (e_d_quit_can_be_asked())
  e_d_send_cmd(e_d_quit_command());
 jdb_trace("e_d_quit_basic: quitting debugger type=%d\n", e_deb_type);
 kbdflgs = fcntl(0, F_GETFL, 0 );
 fcntl(0, F_SETFL, kbdflgs & ~O_NONBLOCK);
 e_d_swtch = 0;
 e_d_debugger_end(end_sig);
 e_d_debugger_pid_forget();
 if (!WpeIsXwin())
 {
  if (e_d_out)
   FREE(e_d_out);
  e_d_out = NULL;
 }
 e_d_close_fd(&wfildes[0]);
 e_d_close_fd(&efildes[0]);
 e_d_close_fd(&rfildes[0]);
 e_d_close_fd(&wfildes[1]);
 e_d_pty_close();
 e_d_async_reset();
 e_d_cut_reset();
 _messages_activated = 0;
 e_d_brk_forget_start_entries();
 /* The session is over: drop the editor's memory of Go to cursor's temporary
    breakpoint.  e_d_swtch is already 0, so nothing is sent to a debugger that
    no longer exists. */
 e_d_cursor_brk_clear();
 if (WpeIsXwin())
 {
  remove(npipe[0]);
  remove(npipe[1]);
  remove(npipe[2]);
  remove(npipe[3]);
  remove(npipe[4]);
 }
 else
 {
  if (efildes[1] >= 0)
  {  close(efildes[1]);  efildes[1] = -1;  }
  if (!e_deb_mode)
   e_g_sys_end();
  else
  {
   e_d_switch_out(1);
   fk_locate(MAXSCOL, MAXSLNS);
   e_putp("\r\n");
   e_putp(att_no);
   e_d_switch_out(0);
  }
 }
 return 0;
}

/* How a debugger that ended by itself is reported; a signal is given as the
   shell gives it, 128 + the signal number. */
#define E_D_DIED_FMT         "%s ended unexpectedly (exit status %d)"
#define E_D_DIED_NO_STATUS   "%s ended unexpectedly"
#define E_D_SIGNAL_STATUS_BASE 128
#define E_D_DIED_LINE_MAX    128

/* The debugger's output pipe reached its end.  Reset when the session
   ends. */
static int e_d_output_closed = 0;

/**
 * e_d_note_output_closed - Record that the debugger's output ended.
 *
 * Every reader that meets end of file on the debugger's pipe calls this, so
 * the session's end can tell a debugger that died from one that was asked
 * to quit.
 */
static void e_d_note_output_closed(void)
{
 e_d_output_closed = 1;
}

/**
 * e_d_end_forget - Forget how the last debugger ended, for the next session.
 */
static void e_d_end_forget(void)
{
 e_d_output_closed = 0;
}

/**
 * e_d_debugger_status - How the debugger ended, as far as it is known now.
 * @status: receives its wait status.
 *
 * From its reported end, or from its slot when the SIGCHLD handler has
 * reaped it and the report is still on its way.  Nothing waits for it.
 * Return: 1 when the status is known, else 0.
 */
static int e_d_debugger_status(int *status)
{
 if (e_d_dbg.status_known)
 {
  *status = e_d_dbg.status;
  return(1);
 }
 if (e_d_dbg.child.pid <= 0)
  return(0);
 return(wpe_child_ended(&e_d_dbg.child, status) == WPE_CHILD_ENDED);
}

/**
 * e_d_debugger_died_unasked - Did the debugger end, or close its output,
 * before the editor began to end it?
 *
 * Once the editor has ended it (Quit, Esc or the deadline of a start, the
 * end of its output, the editor's own end), or it said it goes with its
 * program, its end is no surprise: the user reads why the session ended,
 * never that the debugger ended unexpectedly with the editor's own signal.
 */
static int e_d_debugger_died_unasked(void)
{
 if (e_d_dbg.asked_end || e_d_dbg.end_expected)
  return(0);
 return(e_d_output_closed || e_d_dbg.ended);
}

/* The line for a debugger that closed its output and then had to be ended
   by the editor: it stopped answering, it did not end by itself. */
#define E_D_OUTPUT_CLOSED_FMT "%s stopped answering (its output closed)"

/**
 * e_d_debugger_end_was_the_editors - Was the debugger, whose output had
 * closed, ended by the editor's own SIGKILL rather than by itself?
 *
 * A debugger whose output closed gets no signal at first: one that was
 * dying is reported within the grace with the status it died of (its exit
 * code, or the signal someone else killed it with), and only one that still
 * runs after the grace gets the owner's SIGKILL.  Saying "ended
 * unexpectedly (exit status 137)" for that one would blame it for the
 * editor's kill.
 * Return: 1 when it ended of the SIGKILL the owner sent it.
 */
static int e_d_debugger_end_was_the_editors(void)
{
 if (!e_d_dbg.asked_end || !e_d_dbg.status_known)
  return(0);
 if (!e_d_dbg.child.killed_by_end || !WIFSIGNALED(e_d_dbg.status))
  return(0);
 return(WTERMSIG(e_d_dbg.status) == SIGKILL);
}

/**
 * e_d_debugger_stopped_answering - Is "stopped answering (its output
 * closed)" the true line for the debugger of a session that ended at the
 * end of its output?
 *
 * It is when the editor's own SIGKILL ended it, and also when a new session
 * starts before the system reported it gone: it may still be running then,
 * so the user must not read that it ended.
 * Return: 1 for that line, 0 for the line that names how it ended.
 */
static int e_d_debugger_stopped_answering(void)
{
 if (!e_d_dbg.ended)
  return(1);
 return(e_d_debugger_end_was_the_editors());
}

/**
 * e_d_exit_status_number - The number a user reads for a wait status.
 * @status: the wait status.
 * Return: the exit code, or 128 + the signal for a debugger killed by one.
 */
static int e_d_exit_status_number(int status)
{
 if (WIFSIGNALED(status))
  return(E_D_SIGNAL_STATUS_BASE + WTERMSIG(status));
 return(WEXITSTATUS(status));
}

/* How a session ends, for its end line. */
#define E_D_END_NORMAL     0   /* "End of Debugging" */
#define E_D_END_DIED       1   /* the debugger died: the line names it */
#define E_D_END_DIED_LATER 2   /* it died, and how is known once it is reaped */

/**
 * e_d_died_line_of - The line for a debugger @label that ended unasked.
 * @out, @n: receives it; @status_known: whether @status holds its wait
 * status.
 */
static void e_d_died_line_of(char *out, size_t n, const char *label,
                             int status_known, int status)
{
 if (status_known)
  snprintf(out, n, E_D_DIED_FMT, label, e_d_exit_status_number(status));
 else
  snprintf(out, n, E_D_DIED_NO_STATUS, label);
}

/**
 * e_d_died_line - The line that says the debugger ended without being asked.
 * @out, @n: receives it.
 *
 * Its output can end a moment before the system reports the process gone:
 * the session then ends at once, and the line, with the exit status, is
 * said when the report comes (e_d_on_debugger_ended), never waited for.
 * Return: E_D_END_NORMAL, E_D_END_DIED with @out set, or
 * E_D_END_DIED_LATER.
 */
static int e_d_died_line(char *out, size_t n)
{
 int status;

 if (e_d_dap_active())
  return(e_d_dap_died_line(out, n));
 if (!e_d_debugger_died_unasked())
  return(E_D_END_NORMAL);
 snprintf(e_d_dbg.label, sizeof e_d_dbg.label, "%s", e_d_start_label());
 if (e_d_debugger_status(&status))
 {
  e_d_died_line_of(out, n, e_d_dbg.label, 1, status);
  return(E_D_END_DIED);
 }
 if (e_d_dbg.ended || e_d_dbg.child.pid <= 0)
 {
  e_d_died_line_of(out, n, e_d_dbg.label, 0, 0);
  return(E_D_END_DIED);
 }
 return(E_D_END_DIED_LATER);
}

/**
 * e_d_debugger_say_died - Say, now that the system reported it, how the
 * debugger of a session that is already over ended.
 *
 * The line goes where the session's end line would have gone: Messages and
 * the status row.  Said once.  A debugger the editor had to kill because it
 * closed its output but ran on is said to have stopped answering, not to
 * have ended with the editor's signal; so is one a new session lets go of
 * before its end was reported.
 */
static void e_d_debugger_say_died(void)
{
 FENSTER *top = WpeEditor->f[WpeEditor->mxedt];
 char line[E_D_DIED_LINE_MAX];

 e_d_dbg.say_later = 0;
 if (e_d_debugger_stopped_answering())
  snprintf(line, sizeof line, E_D_OUTPUT_CLOSED_FMT, e_d_dbg.label);
 else
  e_d_died_line_of(line, sizeof line, e_d_dbg.label, e_d_dbg.status_known,
                   e_d_dbg.status);
 e_d_p_message(line, top, 1);
 e_status_say(top, line);
}

/* Set only while a start that never became a session is being torn down; see
   e_d_start_quit. */
static int e_d_start_never_ran = 0;

/**
 * e_d_session_opened - Did a debugging session the user saw ever open?
 *
 * A debugger that answered is a session, so quitting it says "End of
 * Debugging".  A start the user cancelled with Esc, or that passed its
 * deadline, launched a debugger that never answered: no session opened, and
 * saying one ended would contradict the line that names the real reason.
 * Return: 1 when a session opened and its end is worth reporting.
 */
static int e_d_session_opened(void)
{
 if (e_d_start_never_ran)
  return(0);
 return(e_d_session_runs());
}

/**
 * e_d_say_session_end - Write in Messages how the session ended.
 * @died: the line for a debugger that died, or "" for a normal end.
 * @f: the window on top.
 *
 * A debugger that crashed or was killed is named as such, with its exit
 * status, instead of the "End of Debugging" a quit the user asked for
 * shows, so the user knows to look at the debugger rather than the program.
 */
static void e_d_say_session_end(const char *died, FENSTER *f)
{
 if (died[0])
  e_d_p_message((char *)died, f, 1);
 else
  e_d_p_message(e_d_msg[ERR_ENDDEBUG], f, 1);
}

int e_d_quit(FENSTER *f)
{
 ECNT *cn = f->ed;
 int i, ran = e_d_session_opened(), end;
 char died[E_D_DIED_LINE_MAX];

 jdb_trace("e_d_quit: CALLED, e_d_swtch=%d, e_deb_type=%d\n",
           e_d_swtch, e_deb_type);
 /* Quit while the program still compiles for its session: the compiler is
    ended and no session starts behind the user's back once it would have
    finished. */
 if (e_d_compile_busy())
  e_d_compile_cancel();
 end = e_d_died_line(died, sizeof died);
 if (end != E_D_END_DIED)
  died[0] = '\0';
 e_d_start_forget();  /* an adapter start stops being watched before it closes */
 if (e_d_dap_active())
  e_d_dap_quit(f);   /* disconnect the adapter; common cleanup runs below */
 e_d_quit_basic(f);
 /* A start that failed opened no session, so there is none to end: the user
    reads only the reason it failed, not an end that never began.  A
    debugger whose exit status is not known yet has it said when it is. */
 if (ran && end == E_D_END_DIED_LATER)
  e_d_dbg.say_later = 1;
 else if (ran)
  e_d_say_session_end(died, f);
 WpeMouseChangeShape(WpeEditingShape);
 e_d_delbreak(f);
 e_d_watches_after_session(cn);
 /* Switch back to the source file, not Messages.
    The user expects to continue editing after quitting the debugger. */
 for (i = cn->mxedt; i > 0; i--)
  if (e_check_c_file(cn->f[i]->datnam) || e_d_dap_source_ext(cn->f[i]->datnam))
  {
   e_switch_window(cn->edt[i], cn->f[cn->mxedt]);
   break;
  }
 /* The editing keys come back on the bottom row; painted before the line
    below so a debugger that died still has the last word. */
 e_refresh_editor_bars(cn);
 if (died[0])
  e_status_say(cn->f[cn->mxedt], died);
 return(0);
}

/*    Watches   */
int e_d_add_watch(char *str, FENSTER *f)
{
 int ret;

 ret = e_add_arguments(str, "Add Watch", f, 0, AltA, &f->ed->wdf);
 if (ret != WPE_ESC)
 {
  f->ed->wdf = e_add_df(str, f->ed->wdf);
 }
 fk_cursor(1);
 return(ret);
}

int e_remove_all_watches(FENSTER *f)
{
 ECNT *cn = f->ed;
 int i, n;

 if (e_d_ga_refused(f)) return(0);
 if (e_d_nwtchs < 1) return(0);
 for (n = 0; n < e_d_nwtchs;n++) FREE(e_d_swtchs[n]);
 FREE(e_d_swtchs);
 FREE(e_d_nrwtchs);
 e_d_nwtchs = 0;
 for (i = cn->mxedt; i > 0; i--)
 {
  if (!strcmp(cn->f[i]->datnam, "Watches"))
  {
   e_switch_window(cn->edt[i], cn->f[cn->mxedt]);
   e_close_window(cn->f[cn->mxedt]);
   break;
  }
 }
 e_close_buffer(e_p_w_buffer);
 e_p_w_buffer = NULL;
 return(0);
}

int e_delete_watches(FENSTER *f)
{
 ECNT *cn = f->ed;
 BUFFER *b = cn->f[cn->mxedt]->b;
 int n;

 f = cn->f[cn->mxedt];
 if (e_d_ga_refused(f))
  return(0);
 if (e_d_nwtchs < 1 || strcmp(f->datnam, "Watches"))
  return(0);
 for (n = 0; n < e_d_nwtchs && e_d_nrwtchs[n] <= b->b.y; n++)
  ;
 FREE(e_d_swtchs[n - 1]);
 for (; n < e_d_nwtchs; n++)
  e_d_swtchs[n - 1] = e_d_swtchs[n];
 e_d_nwtchs--;
 e_d_swtchs = REALLOC(e_d_swtchs, e_d_nwtchs * sizeof(char *));
 e_d_nrwtchs = REALLOC(e_d_nrwtchs, e_d_nwtchs * sizeof(int));
 e_d_p_watches(f, 0);
 return(0);
}

int e_make_watches(FENSTER *f)
{
 char str[128];
 int i, y;

 if (e_d_ga_refused(f) || e_d_watch_refused(f))
  return(0);
 if ((f->ed->mxedt > 0) && (strcmp(f->datnam, "Watches") == 0))
 { /* sets y=number of watch we're inserting */
  for(y = 0; y < e_d_nwtchs && e_d_nrwtchs[y] < f->b->b.y; y++)
   ;
 }
 else
  y = e_d_nwtchs;
 if (f->ed->wdf && f->ed->wdf->anz > 0)
  snprintf(str, sizeof str, "%s", f->ed->wdf->name[0]);
 else
  str[0] = '\0';
 if (e_d_add_watch(str, f))
 {
  e_d_nwtchs++;
  if (e_d_nwtchs == 1)
  {
   e_d_swtchs = MALLOC(sizeof(char *));
   e_d_nrwtchs = MALLOC(sizeof(int));
  }
  else
  {
   e_d_swtchs = REALLOC(e_d_swtchs, e_d_nwtchs * sizeof(char *));
   e_d_nrwtchs = REALLOC(e_d_nrwtchs, e_d_nwtchs * sizeof(int));
  }
  
  /*
    move watch number y and following up one position so that we can insert 
    at position y 
  */
  for (i = e_d_nwtchs - 1; i > y; i--)
  {
   e_d_swtchs[i] = e_d_swtchs[i-1];
   
   /* The following instruction is pointless as e_d_nrwtchs[i] is invalidated
      by inserting the new watch and has to be recomputed by e_d_p_watches()
   */
   e_d_nrwtchs[i] = e_d_nrwtchs[i-1];
  }
  e_d_swtchs[y] = MALLOC(strlen(str) + 1); /* insert...                    */
  strcpy(e_d_swtchs[y], str);              /*       ... new watch at pos y */
  e_d_p_watches(f, 1);
  return(0);
 }
 return(-1);
}

int e_edit_watches(FENSTER *f)
{
 BUFFER *b = f->ed->f[f->ed->mxedt]->b;
 char str[128];
 int l;

 if (e_d_ga_refused(f) || strcmp(f->datnam, "Watches"))
  return(0);
 for (l = 0; l < e_d_nwtchs && e_d_nrwtchs[l] <= b->b.y; l++)
  ;
 if (l == e_d_nwtchs && b->bf[b->b.y].len == 0)
  return(e_make_watches(f));
 snprintf(str, sizeof str, "%s", e_d_swtchs[l - 1]);
 if (e_d_add_watch(str, f))
 {
  e_d_swtchs[l - 1] = REALLOC(e_d_swtchs[l - 1], strlen(str) + 1);
  strcpy(e_d_swtchs[l - 1], str);
  e_d_p_watches(f, 0);
  return(0);
 }
 return(-1);
}

/**
 * e_d_watch_command - The command that makes the current debugger print one
 * watch expression.
 * @out: where the command goes, newline included; @n: its size;
 * @expr: the watch expression as the user typed it.
 *
 * One table for every line-oriented backend, so a debugger added later
 * either gets its print command here or is refused by name, instead of
 * being sent whatever the buffer held.  pdb prints with "p" and jdb with
 * "print", as gdb and dbx do.
 * Return: 1 when @out holds a command, 0 when this debugger has none.
 */
static int e_d_watch_command(char *out, size_t n, const char *expr)
{
 switch (e_deb_type)
 {
  case DEB_GDB:
  case DEB_XDB:
  case DEB_PDB:
   snprintf(out, n, "p %s\n", expr);
   return(1);
  case DEB_SDB:
   snprintf(out, n, "%s/\n", expr);
   return(1);
  case DEB_DBX:
  case DEB_JDB:
   snprintf(out, n, "print %s\n", expr);
   return(1);
  case DEB_A68G:
   /* a68g: "evaluate EXPR" -- the expression is taken literally, so it
      must NOT be quoted ("n" would be the CHAR denotation, not variable n). */
   snprintf(out, n, "evaluate %s\n", expr);
   return(1);
  default:
   return(0);
 }
}

/**
 * e_d_watch_has_command - Can the current debugger print a watch at all?
 *
 * DAP evaluates watches through the adapter, the line-oriented backends
 * through their print command.
 * Return: 1 when a watch can be shown, else 0.
 */
static int e_d_watch_has_command(void)
{
 char probe[E_D_WATCH_CMD_MAX];

 if (e_deb_type == DEB_DAP)
  return(1);
 return(e_d_watch_command(probe, sizeof probe, ""));
}

/**
 * e_d_watch_refused - Say on the status row that this debugger has no
 * watches, when it has none.
 * @f: the window the user is in.
 *
 * The user learns at once why no Watches window opens, and the editor never
 * sends the debugger a command it did not build, nor waits for its answer.
 * Return: 1 when the watch is refused, else 0.
 */
static int e_d_watch_refused(FENSTER *f)
{
 char line[E_D_WATCH_CMD_MAX];

 if (e_d_watch_has_command())
  return(0);
 snprintf(line, sizeof line, E_D_WATCH_REFUSED_FMT, e_d_start_label());
 e_status_say(f, line);
 return(1);
}

#define E_D_WATCH_LATER " (at the next stop)"

/* The Watches window was last drawn with the words above, not with values. */
static int e_d_watches_shown_waiting = 0;

/**
 * e_d_watches_wait - Must the Watches window wait for the next stop?
 *
 * While the program runs the debugger reads no command, so a watch the user
 * adds, edits or deletes is listed at once and its value is read when the
 * program next stops.  Without a session nothing waits.
 * Return: 1 while the program runs under a session, else 0.
 */
static int e_d_watches_wait(void)
{
 return(e_d_swtch && e_d_async_pending);
}

/**
 * e_d_watch_put_waiting - Add the line of watch @l to the Watches buffer,
 * saying its value comes at the next stop.
 * @b: the Watches buffer; @l: the watch's number.
 *
 * The user sees a watch typed while the program runs in the list at once,
 * and sees why it has no value yet.
 */
static void e_d_watch_put_waiting(BUFFER *b, int l)
{
 char *line = WpeMalloc(strlen(e_d_swtchs[l]) + sizeof(E_D_WATCH_LATER) + 2);

 sprintf(line, "%s:%s", e_d_swtchs[l], E_D_WATCH_LATER);
 e_d_nrwtchs[l] = b->mxlines;
 print_to_end_of_buffer(b, line, b->mx.x);
 WpeFree(line);
 e_d_watches_shown_waiting = 1;
 e_ds_set_watch(&e_d_session, l, e_d_swtchs[l]);
}

/**
 * e_d_watch_put_value - Add the line of watch @l to the Watches buffer with
 * the value the debugger answered, and keep it in the session.
 * @b: the Watches buffer; @l: the watch's number; @answer: the answer, its
 * lines joined, or NULL when the debugger gave none.
 *
 * The user reads "expr: value", or the debugger's reason for having none
 * (a variable out of scope under a68g reads "<not in scope>").  The answer
 * is read from the debugger's output only: its error stream is not part of
 * it.
 */
static void e_d_watch_put_value(BUFFER *b, int l, const char *answer)
{
 E_D_ANSWER a;
 const char *text;
 char *line;

 e_ds_watch_value(e_deb_type, answer, &text);
 line = WpeMalloc(strlen(e_d_swtchs[l]) + strlen(text) + 4);
 sprintf(line, "%s: %s", e_d_swtchs[l], text);
 e_d_nrwtchs[l] = b->mxlines;
 print_to_end_of_buffer(b, line, b->mx.x);
 WpeFree(line);
 e_ds_answer_borrow(&a, answer, 0);
 e_ds_set_watch(&e_d_session, l, e_d_swtchs[l]);
 e_ds_take_watch_answer(&e_d_session, e_deb_type, l, &a);
}

/**
 * e_d_watches_window_open - Is a Watches window open?
 * @cn: the editor.
 * Return: 1 when one is open, else 0.
 */
static int e_d_watches_window_open(ECNT *cn)
{
 int i;

 for (i = cn->mxedt; i > 0; i--)
  if (!strcmp(cn->f[i]->datnam, "Watches"))
   return(1);
 return(0);
}

/**
 * e_d_watches_after_session - The session has ended: redraw the watches
 * that were waiting for a stop.
 * @cn: the editor.
 *
 * A program that ends, or a session the user quits, while watches wait for
 * the next stop has no next stop.  The user then reads, for each watch, the
 * words the debugger's watches show without a session, not a promise of a
 * value that never comes.  Watches that show the last stop's values keep
 * them, as they always did.
 */
static void e_d_watches_after_session(ECNT *cn)
{
 if (!e_d_watches_shown_waiting)
  return;
 e_d_watches_shown_waiting = 0;
 if (e_d_nwtchs < 1 || !e_d_watches_window_open(cn))
  return;
 e_d_p_watches(cn->f[cn->mxedt], 0);
}

/**
 * e_d_watches_window_take - The Watches window the values are written to,
 * opened when there are watches and it is not.
 * @cn: the editor.
 *
 * With no watch and no window the screen is only drawn again, as it always
 * was, and nothing is opened for nothing.
 * Return: its index, 0 when there is nothing to write, -1 when it could not
 * be opened.
 */
static int e_d_watches_window_take(ECNT *cn)
{
 int iw;

 for (iw = cn->mxedt; iw > 0 && strcmp(cn->f[iw]->datnam, "Watches"); iw--)
  ;
 if (iw == 0 && !e_d_nwtchs)
 {
  e_rep_win_tree(cn);
  return(0);
 }
 if (iw > 0)
  return(iw);
 if (e_edit(cn, "Watches"))
  return(-1);
 return(cn->mxedt);
}

/**
 * e_d_watches_buffer_clear - Empty the Watches buffer before its lines are
 * written again.
 * @b: the Watches buffer.
 */
static void e_d_watches_buffer_clear(BUFFER *b)
{
 e_p_red_buffer(b);
 FREE(b->bf[0].s);
 b->mxlines = 0;
 e_d_watches_shown_waiting = 0;
}

/**
 * e_d_watches_show_end - Close the Watches buffer and draw it.
 * @cn: the editor; @iw: the Watches window; @b: its buffer; @sw: 1 to bring
 * the window to the top, as Add watch does.
 */
static void e_d_watches_show_end(ECNT *cn, int iw, BUFFER *b, int sw)
{
 e_new_line(b->mxlines, b);
 fk_cursor(1);
 if (sw && iw != cn->mxedt)
  e_switch_window(cn->edt[iw], cn->f[cn->mxedt]);
 else
  e_rep_win_tree(cn);
}

/**
 * e_d_watch_answer_at - The answer kept for watch @l, or NULL when none was
 * (the list could not be made, or the debugger gave none).
 * @values: the answers, or NULL.
 */
static const char *e_d_watch_answer_at(char **values, int l)
{
 if (!values)
  return(NULL);
 return(values[l]);
}

/**
 * e_d_watches_show_values - Write every watch with the value the debugger
 * answered for it.
 * @cn: the editor; @values: the answers, one per watch, any of them NULL
 * for none; @sw: 1 to bring the window to the top.
 *
 * The answers were read on the fd loop while the user kept the keys; they
 * are written in one go, so the window never shows half old and half new
 * values.
 * Return: 0, or -1 when the window could not be opened.
 */
static int e_d_watches_show_values(ECNT *cn, char **values, int sw)
{
 BUFFER *b;
 int iw = e_d_watches_window_take(cn), l;

 if (iw <= 0)
  return(iw);
 b = cn->f[iw]->b;
 e_d_watches_buffer_clear(b);
 for (l = 0; l < e_d_nwtchs; l++)
  e_d_watch_put_value(b, l, e_d_watch_answer_at(values, l));
 e_d_watches_show_end(cn, iw, b, sw);
 return(0);
}

/* Among other things, e_d_p_watches() must recompute e_d_nrwtchs when
   called from e_edit_watches(),
   but has code paths that don't do this ==> possible BUG
*/
int e_d_p_watches(FENSTER *f, int sw)
{
 ECNT *cn = f->ed;
 BUFFER *b;
 int iw, l;

 if (e_d_start_refused(f))
  return(0);
 if (e_d_dap_active())
  return(e_d_dap_watches(f, sw));   /* DAP: values via evaluate(context=watch) */
 if (e_d_watch_refused(f))
  return(0);
 e_d_switch_out(0);
 if (e_d_ga_at_stop())
  return(e_d_ga_watches_ask(cn, sw));
 if (!e_d_watches_wait() && (e_d_swtch > 2) && (e_d_p_stack(f, 0) == -1))
  return(-1);
 iw = e_d_watches_window_take(cn);
 if (iw <= 0)
  return(iw);
 b = cn->f[iw]->b;
 e_d_watches_buffer_clear(b);

 /* The debugger is asked only at a stop (above).  With a session that runs
    or answers, a watch's value comes at the next stop; with none, the
    watch says it has no value. */
 for (l = 0; l < e_d_nwtchs; l++)
 {
  if (e_d_swtch)
   e_d_watch_put_waiting(b, l);
  else
   e_d_watch_put_value(b, l, NULL);
 }
 e_d_watches_show_end(cn, iw, b, sw);
 return(0);
}

int e_p_show_watches(FENSTER *f)
{
 int i;

 for (i = f->ed->mxedt; i > 0; i--)
  if (!strcmp(f->ed->f[i]->datnam, "Watches"))
  {
   e_switch_window(f->ed->edt[i], f->ed->f[f->ed->mxedt]);
   break;
  }
 if (i <= 0 && e_edit(f->ed, "Watches"))
 {
  return(-1);
 }
 return(0);
}

/***************************************/
/***  reinitialize watches from prj  ***/
int e_d_reinit_watches(FENSTER * f,char * prj)
{
 int i,e,g,q,y,r;
 char * prj2;

 for(i = f->ed->mxedt; i > 0; i--)
 {
  if (!strcmp(f->ed->f[i]->datnam, "Watches"))
  {  
   e_remove_all_watches(f->ed->f[f->ed->edt[i]]);
   break; 
  }
 }
 g=strlen(prj);
 prj2=MALLOC(sizeof(char)*(g+1));
 strcpy(prj2,prj);
 q=0;
 y=0;
 r=0;
 while(q<g) 
 {
  e=q;
  while(prj2[e]!=';' && e<g) e++;
  prj2[e]='\0';
  q=e+1;
  r++;
 } 
 e_d_nwtchs=r;
 e_d_swtchs = (char **) MALLOC(e_d_nwtchs * sizeof(char *));
 e_d_nrwtchs =(int *) MALLOC(e_d_nwtchs * sizeof(int));   

 for(e=0,q=0;e<r;e++)
 {
  e_d_swtchs[e] = MALLOC(strlen(prj2+q)+1);
  strcpy(e_d_swtchs[e], prj2+q); 
  q+=strlen(prj2+q)+1;
 } 
 FREE(prj2);
 e_d_p_watches(f, 1);   
 return 0;
}
/***************************************/

/*  stack   */
int e_deb_stack(FENSTER *f)
{
 if (e_d_refuse_while_running(f))
  return(0);
 e_d_switch_out(0);
 return(e_d_p_stack(f, 1));
}

/* The rows of the last stack listing, whole, each with the Stack window line
   it starts on.  The window wraps a long row, sometimes inside a path, so
   following a frame reads its file and line from here. */
static struct
{
 char **text;
 int *first;
 int n, cap;
} e_d_stack_rows;

/**
 * e_d_stack_rows_clear - Forget the rows of the previous stack listing.
 */
static void e_d_stack_rows_clear(void)
{
 int i;

 for (i = 0; i < e_d_stack_rows.n; i++)
  FREE(e_d_stack_rows.text[i]);
 e_d_stack_rows.n = 0;
}

/**
 * e_d_stack_rows_grow - Make room for one more kept row.
 * Return: 0, or -1 when memory ran out.
 */
static int e_d_stack_rows_grow(void)
{
 int cap = e_d_stack_rows.cap ? 2 * e_d_stack_rows.cap : SVLINES;
 char **text;
 int *first;

 if (e_d_stack_rows.n < e_d_stack_rows.cap)
  return(0);
 text = REALLOC(e_d_stack_rows.text, cap * sizeof(char *));
 if (!text)
  return(-1);
 e_d_stack_rows.text = text;
 first = REALLOC(e_d_stack_rows.first, cap * sizeof(int));
 if (!first)
  return(-1);
 e_d_stack_rows.first = first;
 e_d_stack_rows.cap = cap;
 return(0);
}

/**
 * e_d_stack_rows_keep - Keep a row of the stack listing as the debugger
 * printed it.
 * @text: the row; @first: the Stack window line it starts on.
 *
 * Without memory the row is not kept, and following it falls back on the
 * window's text.
 */
static void e_d_stack_rows_keep(const char *text, int first)
{
 char *copy;

 if (e_d_stack_rows_grow() < 0 || !(copy = MALLOC(strlen(text) + 1)))
  return;
 strcpy(copy, text);
 e_d_stack_rows.text[e_d_stack_rows.n] = copy;
 e_d_stack_rows.first[e_d_stack_rows.n++] = first;
}

/**
 * e_d_stack_rows_find - The kept row that starts on Stack window line @first.
 * Return: its text, or NULL when none was kept there.
 */
static const char *e_d_stack_rows_find(int first)
{
 int i;

 for (i = 0; i < e_d_stack_rows.n; i++)
  if (e_d_stack_rows.first[i] == first)
   return(e_d_stack_rows.text[i]);
 return(NULL);
}

/**
 * e_d_stack_put_row - Lay one row of a stack listing out in the Stack
 * window's buffer, wrapped where it is wider than the window.
 * @f: the Stack window, whose width wraps the row; @str: the row as the
 * debugger printed it, its line end included; @pi, @pj: the buffer line
 * and column the next row starts at, carried from one row to the next.
 *
 * A frame of a function with a very long name or arguments is shown to its
 * end, wrapped with a backslash, and kept whole so following it finds its
 * file and line.
 */
static void e_d_stack_put_row(FENSTER *f, char *str, int *pi, int *pj)
{
 BUFFER *b = f->b;
 int i = *pi, j = *pj, k = 0, l;

 e_d_stack_rows_keep(str, i);
 do
 {
  if (i >= b->mxlines)
   e_new_line(i, b);
  if ((i > 0 && j == 0 && *(b->bf[i-1].s+b->bf[i-1].len-1) == '\\') ||
    (e_deb_type == DEB_GDB && j == 0 && (k > 0 || str[k] != '#')))
  {
   for(j = 0; j < 3; j++)
    b->bf[i].s[j] = ' ';
  }
  for(; isspace(str[k]); k++)
   ;
  for(; j < NUM_COLS_ON_SCREEN_SAFE - 2 && str[k] != '\n' && str[k] != '\0';
    j++, k++)
   *(b->bf[i].s+j) = str[k];
  if (str[k] != '\0')
  {
   if (str[k] != '\n')
   {
    for(l = j-1; l > 2 && !isspace(b->bf[i].s[l]) && b->bf[i].s[l] != '=';
      l--)
     ;
    if (l > 2)
    {
     k -= (j - l - 1);
     for (l++; l < j; l++)
      b->bf[i].s[l] = ' ';
    }
    *(b->bf[i].s+j) = '\\';
    *(b->bf[i].s+j+1) = '\n';
    *(b->bf[i].s+j+2) = '\0';
    j++;
   }
   else
   {
    *(b->bf[i].s+j) = '\n';
    *(b->bf[i].s+j+1) = '\0';
   }
  }
  b->bf[i].len = j;
  b->bf[i].nrc = j + 1;
  if (j != 0 && str[k] != '\0')
  {
   i++;
   j = 0;
  }
  else
   j--;
 }
 while (str[k] != '\n' && str[k] != '\0');
 *pi = i;
 *pj = j;
}

/**
 * e_d_stack_show_end - Drop the Stack window's lines past the listing and
 * draw it.
 * @cn: the editor; @is: the Stack window; @rows: the lines the listing
 * filled; @sw: 1 to bring the window to the top.
 */
static void e_d_stack_show_end(ECNT *cn, int is, int rows, int sw)
{
 BUFFER *b = cn->f[is]->b;
 SCHIRM *s = cn->f[is]->s;
 int i;

 for (i = rows; i < b->mxlines; i++)
  e_del_line(i, b, s);
 if (sw && is != cn->mxedt)
  e_switch_window(cn->edt[is], cn->f[cn->mxedt]);
 e_rep_win_tree(cn);
}

int e_d_p_stack(FENSTER *f, int sw)
{
 ECNT *cn = f->ed;
 int is;

 if (e_d_swtch < 3)
  return(e_error(e_d_msg[ERR_NOTRUNNING], 0, f->fb));
 e_d_out_clear();
 for (is = cn->mxedt; is > 0 && strcmp(cn->f[is]->datnam, "Stack"); is--)
  ;
 if (!sw && is == 0)
  return(0);
 if(is == 0)
 {
  if (e_edit(cn, "Stack"))
   return(-1);
  else
   is = cn->mxedt;
 }
 f = cn->f[is];
 if (!e_d_swtch)
  return(0);
 if (e_d_ga_at_stop())
  return(e_d_ga_stack_ask(cn, sw));
 if (e_d_frames_backend())
  return(e_d_p_frames(cn, is, sw));
 /* A line debugger that runs or answers is never asked: the Stack window
    is refused then (e_deb_stack), and filled at the next stop. */
 return(0);
}

/**
 * e_d_text_take_row - Copy the next row of a debugger's answer, its line end
 * included, and move past it.
 * @p: where the row starts; set to where the next one does.
 *
 * Rows of any length are taken whole: a frame's "at file:line" comes after
 * a function name and arguments of any length.
 * Return: the row, to be freed with FREE, or NULL when memory ran out.
 */
static char *e_d_text_take_row(const char **p)
{
 const char *start = *p, *end = strchr(start, '\n');
 size_t len = end ? (size_t)(end - start) + 1 : strlen(start);
 char *row = MALLOC(len + 1);

 if (!row)
  return(NULL);
 memcpy(row, start, len);
 row[len] = '\0';
 *p = start + len;
 return(row);
}

/**
 * e_d_stack_open - The Stack window, opened when it is not.
 * @cn: the editor.
 * Return: its index, or 0 when it could not be opened.
 */
static int e_d_stack_open(ECNT *cn)
{
 int is;

 for (is = cn->mxedt; is > 0 && strcmp(cn->f[is]->datnam, "Stack"); is--)
  ;
 if (is > 0)
  return(is);
 if (e_edit(cn, "Stack"))
  return(0);
 return(cn->mxedt);
}

/**
 * e_d_stack_show_text - Fill the Stack window with gdb's "bt" answer.
 * @cn: the editor; @text: the answer's rows, joined, or NULL; @sw: 1 to
 * bring the window to the top, as Ctrl-G K does.
 *
 * The answer was read on the fd loop while the user kept the keys; its rows
 * are laid out exactly as the rows of the other debuggers' listings.
 * Return: 0, or -1 when gdb listed no frame at all: the program is gone.
 */
static int e_d_stack_show_text(ECNT *cn, const char *text, int sw)
{
 int is = e_d_stack_open(cn), i = 0, j = 0;
 char *row;

 if (is <= 0)
  return(0);
 if (!text || !text[0])
  return(-1);
 e_d_stack_rows_clear();
 while (*text && (row = e_d_text_take_row(&text)) != NULL)
 {
  e_d_stack_put_row(cn->f[is], row, &i, &j);
  FREE(row);
 }
 e_d_stack_show_end(cn, is, i, sw);
 return(0);
}

/**
 * e_make_stack_done - Free the text of the Stack row being followed and pass
 * the result on.
 * @row: the text, or NULL; @ret: what e_make_stack answers.
 *
 * Every way out of e_make_stack goes through here, so following a frame in
 * the Stack window, however often, does not leave memory behind.
 * Return: @ret.
 */
static int e_make_stack_done(char *row, int ret)
{
 if (row)
  FREE(row);
 return(ret);
}

/**
 * e_make_stack_grow - Make room in the Stack row text for one more line of
 * the Stack window.
 * @row: the text so far; @len: the length of the line to add.
 * Return: the larger text, or NULL when memory ran out (@row is then freed).
 */
static char *e_make_stack_grow(char *row, int len)
{
 char *grown = REALLOC(row, strlen(row) + len + 2);

 if (!grown)
  FREE(row);
 return(grown);
}

/**
 * e_make_stack_row_ends - Say whether Stack window line @i is the last line
 * of the row it belongs to.
 * @b: the Stack window's text.
 *
 * gdb's rows start with "#"; the other debuggers' wrapped rows end each line
 * but their last in a backslash.
 */
static int e_make_stack_row_ends(BUFFER *b, int i)
{
 if (i == b->mxlines - 1)
  return(1);
 if (e_deb_type == DEB_GDB)
  return(b->bf[i+1].s[0] == '#');
 return(b->bf[i].s[b->bf[i].len - 1] != '\\');
}

/**
 * e_make_stack_join - Put a row back together from the Stack window lines
 * it was wrapped over.
 * @b: the Stack window's text; @i: the row's first line; @row: an empty text.
 *
 * Used when the listing the row came from is not kept (the window was filled
 * otherwise); the kept row is exact, this is the window's view of it.
 * Return: the row, or NULL when memory ran out (@row is then freed).
 */
static char *e_make_stack_join(BUFFER *b, int i, char *row)
{
 for(; i < b->mxlines; i++)
 {
  if(!(row = e_make_stack_grow(row, b->bf[i].len)))
   return(NULL);
  strcat(row, (char *) b->bf[i].s);
  if(e_make_stack_row_ends(b, i))
   break;
  else if(row[strlen(row)-2] == '\\')
   row[strlen(row)-2] = '\0';
  else if(row[strlen(row)-1] == '\n')
   row[strlen(row)-1] = '\0';
 }
 return(row);
}

/**
 * e_make_stack_row - The text of the Stack row that starts on line @i, as
 * the debugger printed it.
 * @b: the Stack window's text; @row: an empty text, replaced.
 *
 * A row longer than the window is wrapped there, sometimes inside a word or
 * a path; following the frame reads its file and line from the row as the
 * debugger printed it, so a frame of any length is followed.
 * Return: the row, or NULL when memory ran out (@row is then freed).
 */
static char *e_make_stack_row(BUFFER *b, int i, char *row)
{
 const char *kept = e_d_stack_rows_find(i);
 char *copy;

 if (!kept)
  return(e_make_stack_join(b, i, row));
 copy = REALLOC(row, strlen(kept) + 1);
 if (!copy)
 {
  FREE(row);
  return(NULL);
 }
 strcpy(copy, kept);
 return(copy);
}

int e_make_stack(FENSTER *f)
{
   char file[E_D_LINE_MAX], *tmpstr;
   int i, line = 0, dif;
   BUFFER *b = f->ed->f[f->ed->mxedt]->b;
   if (e_d_refuse_while_running(f))
      return(0);
   if (e_d_frames_backend())
      return(e_d_frame_show(f));
   tmpstr = MALLOC(1);
   if(!tmpstr)
      return(e_error(e_msg[ERR_LOWMEM], 0, f->fb));
   e_d_switch_out(0);
   if(e_deb_type != DEB_SDB)
   {  tmpstr[0] = '\0';
      if(e_deb_type == DEB_GDB)
      {  for(i = dif = 0; i <= b->b.y; i++)
	    if(b->bf[i].s[0] == '#') dif = atoi((char *) (b->bf[i].s + 1));
	 for(i = b->b.y; i >= 0 && b->bf[i].s[0] != '#'; i--);
	 if(i < 0) return(e_make_stack_done(tmpstr, 1));
      }
      else
      {  for(i = 1, dif = 0; i <= b->b.y; i++)
	    if(b->bf[i-1].s[b->bf[i-1].len - 1] != '\\') dif++;
	 for(i = b->b.y; i > 0 && b->bf[i-1].s[b->bf[i-1].len - 1] == '\\';
	    i--);
	 if(i == 0 && b->bf[i].len == 0) return(e_make_stack_done(tmpstr, 1));
      }
      if(!(tmpstr = e_make_stack_row(b, i, tmpstr)))
      {  e_error(e_msg[ERR_LOWMEM], 0, f->fb);  return(-1);  }

      /* A row of any length is followed: e_make_line_num refuses only a
	 file name longer than any path. */
      if(e_deb_type == DEB_XDB && (line = e_make_line_num2(tmpstr, file)) < 0)
	 return(e_make_stack_done(tmpstr,
				  e_error(e_d_msg[ERR_NOSOURCE], 0, f->fb)));
      else if(e_deb_type != DEB_XDB && (line = e_make_line_num(tmpstr, file)) < 0)
	 return(e_make_stack_done(tmpstr,
				  e_error(e_d_msg[ERR_NOSOURCE], 0, f->fb)));
      FREE(tmpstr);           /* the file and line are read out of it */
      if(e_d_ga_at_stop())
	 return(e_d_ga_follow_frame(f->ed, dif, file, line));
      /* No frame move can be sent: while the debugger starts the start's
	 line says why; with no session the row's line is only shown. */
      if(e_d_start_refused(f))
	 return(0);
   }
   else if(e_deb_type == DEB_SDB)
   {  FREE(tmpstr);           /* sdb's rows are read in place */
      for(i = b->b.y; i >= 0 && (line =
	 e_make_line_num2((char *)b->bf[i].s, file)) < 0; i--);
   }
   if(e_d_p_watches(f, 0) == -1) return(-1);
   e_d_goto_break(file, line, f);
   return(0);
}
/*******************************************************/
/** resyncing schirm - screen output with breakpoints **/

int e_brk_schirm(FENSTER *f)
{
 int i;
 int n;

 SCHIRM *s = f->s;
 s->brp= REALLOC(s->brp, sizeof(int));
 s->brp[0]=0;
 for(i=0;i<e_d_nbrpts;i++)
 {
  if(!strcmp(f->datnam,e_d_sbrpts[i]))
  {
   for(n=1;n<= (s->brp[0]);n++) if(e_d_ybrpts[i]==(s->brp[n])) break;
   if(n>s->brp[0]) 
   {
    /****  New break, not in schirm  ****/   
    (s->brp[0])++;
    s->brp = REALLOC(s->brp, (s->brp[0]+1) * sizeof(int));
    s->brp[s->brp[0]] = e_d_ybrpts[i]-1; 
   }
  }
 }
 return 0;
}
/*****************************************/

/*******************************************/
/***  reinitialize breakpoints from prj  ***/
int e_d_reinit_brks(FENSTER * f,char * prj)
{
   int line,e,g,q,r;
   char * p,*name,*prj2;
/***  remove breakpoints, schirms will be synced later  ***/

   e_remove_breakpoints(f);
   g=strlen(prj);
   prj2=MALLOC(sizeof(char)*(g+1));
   strcpy(prj2,prj);
   q=0;
   r=0;
   while(q<g) 
   {
     e=q;
     while(prj2[e]!=';' && e<g) e++;
     prj2[e]='\0';
     q=e+1;
     r++;
   } 
/**** for sure ****/   
   e_d_nbrpts=0;
   
/**** allocate memory for breakpoints ****/   
   e_d_sbrpts = MALLOC(sizeof(char *) * r);
   e_d_ybrpts = MALLOC(sizeof(int) * r);
   e_d_nrbrpts = MALLOC(sizeof(int) * r);
   e_d_brk_state_reserve(r);

   name=prj2;
   for(q=0;q<r;q++)
   {
     p=strrchr(name,':');
     e=strlen(name);
     if(p!=NULL)
     {
       *p='\0';
       {
         p++;
         line=atoi(p);
         if(line>0) {
/**** hopefully we have filename and line number ****/

	   e_d_ybrpts[e_d_nbrpts]=line;
	   e_d_sbrpts[e_d_nbrpts]=MALLOC(sizeof(char)*(strlen(name)+1));
	   strcpy(e_d_sbrpts[e_d_nbrpts],name);
	   e_d_nrbrpts[e_d_nbrpts] = 0;
	   e_d_brk_state[e_d_nbrpts] = E_D_BRK_PENDING;
	   e_d_brk_msg[e_d_nbrpts][0] = '\0';
	   e_d_nbrpts++;
	   
/**** needed to keep schirm in sync ****/
	   
	   for(g = f->ed->mxedt; g > 0; g--)
            if(!strcmp(f->ed->f[g]->datnam, name))
            {  
              e_brk_schirm(f->ed->f[g]);
            }
         }
       }
     }
     name+=e+1;
   }
   FREE(prj2);
   return 0;
}


/**** Recalculate breakpoints , because of line/block
    deleting/adding ****/
int e_brk_recalc(FENSTER *f, int start, int len)
{
 ECNT *cn = f->ed;
 BUFFER *b;
 int n, rend, count, yline;
 int *br_lines;

 if ((len == 0) || (cn == NULL))
  return 1;
 b = cn->f[cn->mxedt]->b;

 rend = start - 1 + abs(len);
 yline = b->b.y;

/**** deleting removed breakpoints ****/
 if (len < 0)
 {
  for (n = 0; n < e_d_nbrpts; n++)
   if ((!strcmp(f->datnam, e_d_sbrpts[n])) &&
     (e_d_ybrpts[n] <= (rend + 1)) && (e_d_ybrpts[n] >= (start + 1))) 
   {
    b->b.y = e_d_ybrpts[n] - 1;
    e_make_breakpoint(f, 0);
   }
 }

/**** scanning for breakpoints to move ****/
 for (count = 0, n = 0; n < e_d_nbrpts; n++)
  if ((!strcmp(f->datnam, e_d_sbrpts[n])) && (e_d_ybrpts[n] >= (start + 1)))
   count++;
 if (count == 0)
  return 1;
 br_lines = (int*)malloc(sizeof(int) * count);
 for (n = 0, count = 0; n < e_d_nbrpts; n++)
  if ((!strcmp(f->datnam, e_d_sbrpts[n])) && (e_d_ybrpts[n] >= (start + 1))) 
  {
   br_lines[count++] = e_d_ybrpts[n];
  }

/**** moving breakpoints ****/
 for(n = 0; n < count; n++) 
 {
  b->b.y = br_lines[n] - 1;
  e_make_breakpoint(f, 0);
 }
 for(n = 0; n < count; n++) 
 {
  b->b.y = br_lines[n] + len - 1;
  e_make_breakpoint(f, 0);
 }
 b->b.y = yline;
 free(br_lines);
 return 0;
}
/*****************************************/

/**
 * e_d_breakpoints_changed - Have the open project keep the new breakpoints.
 * @cn: the editor.
 *
 * The project file stores the breakpoints, but it is written only when the
 * project window has changes.  Marking it here is what makes a breakpoint
 * set or removed with a project open still be so when the editor is started
 * on that project again.
 */
static void e_d_breakpoints_changed(ECNT *cn)
{
 int i = e_find_project_window(cn);

 if (i > 0)
  cn->f[i]->save = 1;
}

/* Room for a breakpoint command that names a file, and for its answer. */
#define E_D_BRK_CMD_MAX     (PATH_MAX + 64)
#define E_D_BRK_ANSWER_MAX  E_D_LINE_MAX
/* No line to add to a debug adapter's list. */
#define E_D_BRK_NO_LINE     0

/* The debugger's reply to one breakpoint command: what it made of it, and its
   words for the user when it refused or deferred it. */
typedef struct
{
 int state;                    /* E_D_BRK_PENDING, _VERIFIED or _REFUSED */
 char msg[E_D_BRK_MSG_MAX];    /* its words, or "" */
} E_D_BRK_ANSWER;
/* A debug adapter's verdict comes later, through on_breakpoints. */
#define E_D_BRK_ASKED     3

static void e_d_brk_path_of(ECNT *cn, const char *file, char *out, size_t size);
static void e_d_brk_jdb_class(const char *file, char *out, size_t size);
static int e_d_brk_arm(ECNT *cn, int i);
static int e_d_brk_disarm(ECNT *cn, const char *file, int line, int id);
static void e_d_brk_disarm_all(ECNT *cn);

/**
 * e_d_brk_is_start_entry - Is list entry @i the breakpoint the editor sets
 * on the program's start symbol for itself, not one of the user's?
 *
 * Such an entry has no file name.  Remove all and the end of a session
 * leave it out: it is no line the user marked.
 */
static int e_d_brk_is_start_entry(int i)
{
 return(e_d_sbrpts[i][0] == '\0');
}

/**
 * e_d_brk_state_reserve - Give the state and the message of the breakpoint
 * list room for @n entries.
 * @n: the entries the list will hold.
 *
 * The state lives beside the list, so every place that makes the list room
 * makes it here too; the new entries are not told to the debugger yet.
 */
static void e_d_brk_state_reserve(int n)
{
 int old = e_d_nbrpts;
 int i;

 if (e_d_brk_state)
  FREE(e_d_brk_state);
 if (e_d_brk_msg)
  FREE(e_d_brk_msg);
 e_d_brk_state = MALLOC(sizeof(int) * n);
 e_d_brk_msg = MALLOC(sizeof(*e_d_brk_msg) * n);
 for (i = old; i < n; i++)
 {
  e_d_brk_state[i] = E_D_BRK_PENDING;
  e_d_brk_msg[i][0] = '\0';
 }
}

/**
 * e_d_brk_state_grow - Make room in the state for the list's next entry,
 * keeping what the debugger said of the entries it has.
 * @n: the entries the list will hold.
 */
static void e_d_brk_state_grow(int n)
{
 int *state = e_d_brk_state;
 char (*msg)[E_D_BRK_MSG_MAX] = e_d_brk_msg;

 e_d_brk_state = MALLOC(sizeof(int) * n);
 e_d_brk_msg = MALLOC(sizeof(*e_d_brk_msg) * n);
 if (state)
 {
  memcpy(e_d_brk_state, state, sizeof(int) * (n - 1));
  memcpy(e_d_brk_msg, msg, sizeof(*e_d_brk_msg) * (n - 1));
  FREE(state);
  FREE(msg);
 }
 e_d_brk_state[n - 1] = E_D_BRK_PENDING;
 e_d_brk_msg[n - 1][0] = '\0';
}

/**
 * e_d_brk_list_free - Empty the breakpoint list.
 *
 * The margin marks are left as they are; the caller says whether they go.
 */
static void e_d_brk_list_free(void)
{
 int i;

 for (i = 0; i < e_d_nbrpts; i++)
  FREE(e_d_sbrpts[i]);
 e_d_nbrpts = 0;
 if (e_d_brk_state)
 {
  FREE(e_d_brk_state);
  e_d_brk_state = NULL;
 }
 if (e_d_brk_msg)
 {
  FREE(e_d_brk_msg);
  e_d_brk_msg = NULL;
 }
 if (e_d_sbrpts)
 {
  FREE(e_d_sbrpts);
  e_d_sbrpts = NULL;
 }
 if (e_d_ybrpts)
 {
  FREE(e_d_ybrpts);
  e_d_ybrpts = NULL;
 }
 if (e_d_nrbrpts)
 {
  FREE(e_d_nrbrpts);
  e_d_nrbrpts = NULL;
 }
}

/**
 * e_d_brk_list_drop - Take entry @i out of the breakpoint list.
 *
 * Used when the user removes a breakpoint and when the editor's own start
 * breakpoint is no longer needed.
 */
static void e_d_brk_list_drop(int i)
{
 FREE(e_d_sbrpts[i]);
 for (; i < e_d_nbrpts - 1; i++)
 {
  e_d_sbrpts[i] = e_d_sbrpts[i+1];
  e_d_ybrpts[i] = e_d_ybrpts[i+1];
  e_d_nrbrpts[i] = e_d_nrbrpts[i+1];
  e_d_brk_state[i] = e_d_brk_state[i+1];
  memcpy(e_d_brk_msg[i], e_d_brk_msg[i+1], E_D_BRK_MSG_MAX);
 }
 e_d_nbrpts--;
 if (e_d_nbrpts == 0)
  e_d_brk_list_free();
}

/**
 * e_d_brk_forget_start_entries - At the end of a session, keep only the
 * user's breakpoints in the list.
 *
 * The user's breakpoints outlive the session: the next Ctrl-G R, F7 or F8
 * stops on them again with any debugger.  The start breakpoint the editor
 * set for itself does not, or the next session would think the user had
 * breakpoints and never stop at the start.
 */
static void e_d_brk_forget_start_entries(void)
{
 int i = 0;

 while (i < e_d_nbrpts)
 {
  if (e_d_brk_is_start_entry(i))
   e_d_brk_list_drop(i);
  else
   i++;
 }
 e_d_brk_forget_session();
}

/**
 * e_d_brk_index_of - Where the list keeps the breakpoint on @line of @file.
 * Return: its index, or -1 when the list has none there.
 */
static int e_d_brk_index_of(const char *file, int line)
{
 int i;

 for (i = 0; i < e_d_nbrpts; i++)
  if (e_d_ybrpts[i] == line && !strcmp(e_d_sbrpts[i], file))
   return(i);
 return(-1);
}

/**
 * e_d_brk_drawn_hollow - Is the breakpoint on @line of @file drawn as the
 * outline of the red bar, because the debugger has not confirmed it?
 * @file: the window's file name; @line: 1-based.
 *
 * Asked by the line painter, so the user sees at a glance which marks the
 * debugger really has and which wait for the next stop or were refused.
 * Without a session every breakpoint is the user's plain mark and is drawn
 * solid, as it always was.
 * Return: 1 for a hollow one.
 */
int e_d_brk_drawn_hollow(const char *file, int line)
{
 int i;

 if (!e_d_session_runs())
  return(0);
 i = e_d_brk_index_of(file, line);
 if (i < 0)
  return(0);
 return(e_d_brk_state[i] != E_D_BRK_VERIFIED);
}

/**
 * e_d_brk_set_state - Record what the debugger said of list entry @i.
 * @state: E_D_BRK_*; @msg: its words, or NULL for none.
 */
static void e_d_brk_set_state(int i, int state, const char *msg)
{
 e_d_brk_state[i] = state;
 if (msg)
  snprintf(e_d_brk_msg[i], E_D_BRK_MSG_MAX, "%s", msg);
 else
  e_d_brk_msg[i][0] = '\0';
}

/**
 * e_d_brk_take_answer - Record a debugger's reply on list entry @i.
 * @ans: the reply read; E_D_BRK_ASKED leaves the entry as it is, for an
 * adapter whose word comes later.
 */
static void e_d_brk_take_answer(int i, const E_D_BRK_ANSWER *ans)
{
 if (ans->state == E_D_BRK_ASKED)
  return;
 e_d_brk_set_state(i, ans->state, ans->msg);
}

/* Breakpoints the user removed while the program ran: the debugger still has
   them until the next stop, when it is told. */
typedef struct
{
 char *file;                   /* the breakpoint's file name */
 int line;                     /* 1-based */
 int id;                       /* the number the debugger gave it */
 int state;                    /* what the debugger had said of it */
 char msg[E_D_BRK_MSG_MAX];
} E_D_BRK_GONE;

static E_D_BRK_GONE *e_d_brk_gone;
static int e_d_brk_ngone;

/**
 * e_d_brk_must_wait - Does a breakpoint change have to wait for the next
 * stop, because the program runs and the debugger reads no command?
 * Return: 1 while the program runs.
 */
static int e_d_brk_must_wait(void)
{
 return(e_d_async_pending);
}

static int e_d_brk_unsent(int i);   /* (below) */

/**
 * e_d_brk_debugger_has - Has the debugger been given list entry @i?
 *
 * gdb, dbx and xdb number what they set, and the other line debuggers are
 * marked when told.  A debug adapter is given the breakpoints of a file as
 * a whole, at its launch too, so it has every entry the editor is not
 * still holding back; a removal while the program runs then reaches it.
 * Return: 1 when it has.
 */
static int e_d_brk_debugger_has(int i)
{
 if (e_deb_type == DEB_DAP)
  return(!e_d_brk_unsent(i));
 return(e_d_nrbrpts[i] != 0);
}

/**
 * e_d_brk_gone_add - Remember that the debugger still has the breakpoint
 * list entry @i, which the user has just removed while the program runs.
 *
 * Without the record the debugger would keep stopping on a line the margin
 * no longer marks.  An entry the debugger never heard of needs none.
 */
static void e_d_brk_gone_add(int i)
{
 E_D_BRK_GONE *rec;

 if (!e_d_brk_debugger_has(i))
  return;
 e_d_brk_gone = REALLOC(e_d_brk_gone,
                        sizeof(E_D_BRK_GONE) * (e_d_brk_ngone + 1));
 rec = &e_d_brk_gone[e_d_brk_ngone++];
 rec->file = MALLOC(strlen(e_d_sbrpts[i]) + 1);
 strcpy(rec->file, e_d_sbrpts[i]);
 rec->line = e_d_ybrpts[i];
 rec->id = e_d_nrbrpts[i];
 rec->state = e_d_brk_state[i];
 snprintf(rec->msg, E_D_BRK_MSG_MAX, "%s", e_d_brk_msg[i]);
}

/**
 * e_d_brk_gone_free - Forget the removals the debugger was told of, or that
 * a session's end made pointless.
 */
static void e_d_brk_gone_free(void)
{
 int i;

 for (i = 0; i < e_d_brk_ngone; i++)
  FREE(e_d_brk_gone[i].file);
 if (e_d_brk_gone)
  FREE(e_d_brk_gone);
 e_d_brk_gone = NULL;
 e_d_brk_ngone = 0;
}

/**
 * e_d_brk_gone_revive - The user set again, while the program runs, a
 * breakpoint he had just removed: the debugger still has it, so the new
 * entry takes over what it was and nothing is sent at the stop.
 * @i: the new list entry.
 *
 * Sending the removal and then the new mark would clear the new one on the
 * debuggers that name a breakpoint by its line.  A removal gdb is already
 * answering cannot be taken back: the new entry is then set afresh.
 */
static void e_d_brk_gone_revive(int i)
{
 int n;

 for (n = 0; n < e_d_brk_ngone; n++)
  if (e_d_brk_gone[n].line == e_d_ybrpts[i] &&
      !strcmp(e_d_brk_gone[n].file, e_d_sbrpts[i]) &&
      e_d_brk_gone[n].id != E_D_GB_ID_UNKNOWN &&
      !e_d_gb_removal_told(e_d_brk_gone[n].id))
   break;
 if (n == e_d_brk_ngone)
  return;
 e_d_nrbrpts[i] = e_d_brk_gone[n].id;
 e_d_brk_set_state(i, e_d_brk_gone[n].state, e_d_brk_gone[n].msg);
 FREE(e_d_brk_gone[n].file);
 for (; n < e_d_brk_ngone - 1; n++)
  e_d_brk_gone[n] = e_d_brk_gone[n + 1];
 e_d_brk_ngone--;
}

/**
 * e_d_brk_say_refusal - Tell the user, on the status row, that the debugger
 * refused list entry @i and why.
 * @f: the window on top.
 *
 * The mark stays, hollow: it is still the user's, and it is tried again at
 * the next start.  A breakpoint the debugger keeps for later (jdb, for a
 * class not loaded yet) is said too, as not confirmed.  The row holds until a
 * key is pressed, since the stop or the repaint that follows would otherwise
 * wipe it.
 */
static void e_d_brk_say_refusal(FENSTER *f, int i)
{
 char line[E_D_BRK_MSG_MAX + E_D_BRK_SAY_EXTRA];

 if (e_d_brk_state[i] == E_D_BRK_REFUSED)
  snprintf(line, sizeof(line), " Breakpoint at line %d not set: %s",
           e_d_ybrpts[i], e_d_brk_msg[i]);
 else if (e_d_brk_state[i] == E_D_BRK_PENDING && e_d_brk_msg[i][0])
  snprintf(line, sizeof(line), " Breakpoint at line %d not confirmed: %s",
           e_d_ybrpts[i], e_d_brk_msg[i]);
 else
  return;
 e_d_refusal_said = 1;
 e_status_say_until_key(f, line);
}

/**
 * e_d_brk_say_if_refused - Tell the user the debugger refused list entry
 * @i, and only that.
 * @f: the window on top.
 *
 * For the breakpoints set when a session starts: jdb keeps every one of
 * them for when its class loads, which is no news worth the status row.
 */
static void e_d_brk_say_if_refused(FENSTER *f, int i)
{
 if (e_d_brk_state[i] == E_D_BRK_REFUSED)
  e_d_brk_say_refusal(f, i);
}

/**
 * e_d_brk_unsent - Is list entry @i one the debugger has not been told of
 * yet?
 * Return: 1 for a breakpoint set while the program ran.
 */
static int e_d_brk_unsent(int i)
{
 return(!e_d_brk_is_start_entry(i) && e_d_brk_state[i] == E_D_BRK_PENDING &&
        e_d_nrbrpts[i] == 0);
}

/**
 * e_d_brk_changed_while_running - Did the user set or remove a breakpoint
 * that the debugger has not been told of?
 *
 * A stop with nothing to tell leaves the screen and the status row exactly
 * as the stop made them.
 * Return: 1 when a change is waiting.
 */
static int e_d_brk_changed_while_running(void)
{
 int i;

 if (e_d_brk_ngone > 0)
  return(1);
 for (i = 0; i < e_d_nbrpts; i++)
  if (e_d_brk_unsent(i))
   return(1);
 return(0);
}

/**
 * e_d_brk_apply_removals - Tell the debugger of the breakpoints the user
 * removed while the program ran.
 * @cn: the editor.
 * Return: 0, or -1 when the debugger stopped answering.
 */
static int e_d_brk_apply_removals(ECNT *cn)
{
 int i;

 for (i = 0; i < e_d_brk_ngone; i++)
  if (e_d_brk_disarm(cn, e_d_brk_gone[i].file, e_d_brk_gone[i].line,
                     e_d_brk_gone[i].id) == -1)
   return(-1);
 e_d_brk_gone_free();
 return(0);
}

/**
 * e_d_brk_apply_additions - Tell the debugger of the breakpoints the user
 * set while the program ran.
 * @cn: the editor.
 *
 * Each turns solid on the debugger's word, or stays hollow with its words on
 * the status row.  An entry the debugger has already answered for is left.
 * Return: 0, or -1 when the debugger stopped answering.
 */
static int e_d_brk_apply_additions(ECNT *cn)
{
 int i;

 for (i = 0; i < e_d_nbrpts; i++)
 {
  if (!e_d_brk_unsent(i))
   continue;
  if (e_d_brk_arm(cn, i) == -1)
   return(-1);
  e_d_brk_say_refusal(cn->f[cn->mxedt], i);
 }
 return(0);
}

/**
 * e_d_brk_sync_at_stop - The program stopped: tell the debugger the
 * breakpoints the user set and removed while it ran.
 * @cn: the editor.
 *
 * The floor that works with every debugger: a change made while the program
 * runs is kept by the editor and applied at the next stop of any kind, so
 * the marks in the margin and the debugger agree again.
 * Return: 0, or -1 when the debugger stopped answering.
 */
static int e_d_brk_sync_at_stop(ECNT *cn)
{
 if (e_d_brk_apply_removals(cn) == -1)
  return(-1);
 return(e_d_brk_apply_additions(cn));
}

/**
 * e_d_brk_forget_session - At the end of a session, make the list ready for
 * the next one.
 *
 * The debugger that knew the breakpoints is gone: removals it was never told
 * of are moot, and every entry waits to be told to the next debugger.
 */
static void e_d_brk_forget_session(void)
{
 int i;

 e_d_brk_gone_free();
 for (i = 0; i < e_d_nbrpts; i++)
 {
  e_d_nrbrpts[i] = 0;
  e_d_brk_set_state(i, E_D_BRK_PENDING, NULL);
 }
}

/*  Breakpoints   */
int e_breakpoint(FENSTER *f)
{
 return(e_make_breakpoint(f, 0));
}

int e_remove_breakpoints(FENSTER *f)
{
 ECNT *cn = f->ed;
 int i;

 if (e_d_start_refused(f) || e_d_refuse_while_running(f))
  return(0);
 if (e_d_swtch)
  e_d_brk_disarm_all(cn);
 /* Opening a project clears the list before it reads the project's own, so
    only a list that had breakpoints is a change the project has to keep. */
 if (e_d_nbrpts > 0)
  e_d_breakpoints_changed(cn);
 for (i = cn->mxedt; i >= 0; i--)
  if (DTMD_ISTEXT(cn->f[i]->dtmd))
   cn->f[i]->s->brp[0] = 0;
 e_d_brk_list_free();
 e_rep_win_tree(cn);
 return(0);
}

/* The entry symbol ga68 gives an Algol 68 program.  Named here because this is
   the one place that has to know about it: the symbol is emitted BEFORE the
   first line record of the source, so a breakpoint on it stops four bytes
   early, in the prologue, where gdb has no file, no line and no locals to
   offer.  The editor could then not show the source, F8 could not follow
   source lines, and a watch had nothing to evaluate. */
#define GA68_ENTRY_SYMBOL "__algol68_main"

/**
 * e_d_gdb_start_break - The gdb command that stops a run at the program's start.
 * @out: where the command goes; @n: its size.
 *
 * Normally this breaks on the configured start symbol, which is what the user
 * sees and can change in Options.  For ga68 it breaks on the source file's
 * first line instead, and gdb moves that to the first line that has code: the
 * same function, a few bytes later, but inside the line table, so the editor
 * gets the file and line it needs to show the source and the user gets locals
 * and working watches.  A Borland-school editor stops on the first statement of
 * your program, not on a symbol belonging to the runtime.
 *
 * A user who types their own symbol keeps a symbol breakpoint: the special case
 * is recognised by the value, not by the language, so the setting still means
 * what it says.
 */
static void e_d_gdb_start_break(char *out, size_t n)
{
 const char *sym = e_get_start_symbol();

 if (sym && !strcmp(sym, GA68_ENTRY_SYMBOL) && e_d_file[0])
  snprintf(out, n, "b %s:1\n", e_d_file);
 else
  snprintf(out, n, "b %s\n", sym);
}

/* Grow the three parallel breakpoint arrays (source file name, source line,
   debugger-assigned id) by one entry.  The caller then fills in element
   e_d_nbrpts - 1 of each.  Used wherever a breakpoint is recorded
   (e_mk_brk_main, e_make_breakpoint). */
static void e_d_brk_grow(void)
{
 e_d_nbrpts++;
 if (e_d_nbrpts == 1)
 {
  e_d_sbrpts = MALLOC(sizeof(char *));
  e_d_ybrpts = MALLOC(sizeof(int));
  e_d_nrbrpts = MALLOC(sizeof(int));
 }
 else
 {
  e_d_sbrpts = REALLOC(e_d_sbrpts, e_d_nbrpts * sizeof(char *));
  e_d_ybrpts = REALLOC(e_d_ybrpts, e_d_nbrpts * sizeof(int));
  e_d_nrbrpts = REALLOC(e_d_nrbrpts, e_d_nbrpts * sizeof(int));
 }
 e_d_brk_state_grow(e_d_nbrpts);
}

/* What a breakpoint answer held, besides 0 for a line of the debugger's
   output: the kinds the verdicts (e_d_brk_numbered_verdict) tell apart. */
#define E_D_READ_PROMPT  1   /* the prompt came: no (more) answer */
#define E_D_READ_ERROR   2   /* a line of the debugger's error stream */

int e_mk_brk_main(FENSTER *f, int sw)
{
 if (sw)
 {
  /* Remove all may already have taken the entry out of the list. */
  if (sw > e_d_nbrpts || !e_d_brk_is_start_entry(sw-1))
   return(0);
  if (e_d_run_going())
  {
   e_d_run_drop_start_brk_at_stop(sw);
   return(0);
  }
  if (e_d_ga_go_keeps_start_brk(sw))
   return(0);
  if (e_d_ga_backend_of(e_deb_type))
  {
   e_d_ga_drop_start_entry(sw-1);
   return(0);
  }
  e_d_brk_list_drop(sw-1);   /* an adapter stops at the start by itself */
 }
 else
 {
  e_d_brk_grow();
  e_d_sbrpts[e_d_nbrpts - 1] = MALLOC(1);
  e_d_sbrpts[e_d_nbrpts - 1][0] = '\0';   /* no file: the start symbol */
  e_d_ybrpts[e_d_nbrpts - 1] = 0;         /* no line until sdb names one */
  e_d_nrbrpts[e_d_nbrpts - 1] = 0;        /* not set in the debugger yet */
  /* Every pipe debugger is told by the run that follows, on the fd loop
     (e_d_ga_go); pdb, a68g and the adapters need no command for it. */
 }
 return(sw ? 0 : e_d_nbrpts);
}

/**
 * e_d_brk_toggle_off - The user removed the breakpoint on @line of window
 * @f's file: take it out of the list and, while a session runs, out of the
 * debugger.
 *
 * The next run then goes past that line with every debugger, and nothing is
 * waited for that was not asked.  While the program runs the removal is
 * kept and handed to the run controller, which tells the debugger as soon
 * as it can take it (e_d_run_breakpoints_changed).
 * Return: 0, or -1 when the debugger could not be told.
 */
static int e_d_brk_toggle_off(FENSTER *f, int line)
{
 int i = e_d_brk_index_of(f->datnam, line);
 int on_loop = e_d_ga_at_stop();
 int id;

 if (i < 0)
  return(0);                   /* only the margin had it */
 if (e_d_swtch && (e_d_brk_must_wait() || on_loop))
  e_d_brk_gone_add(i);
 id = e_d_nrbrpts[i];
 e_d_brk_list_drop(i);
 if (!e_d_swtch)
  return(0);
 if (e_d_brk_must_wait())
 {
  e_d_run_breakpoints_changed(f->ed);
  return(0);
 }
 if (on_loop)
  return(e_d_ga_brk_sync(f->ed));
 return(e_d_brk_disarm(f->ed, f->datnam, line, id));
}

/**
 * e_d_brk_toggle_on - The user set a breakpoint on @line of window @f's
 * file: add it to the list and, while a session runs, to the debugger.
 *
 * The next run stops there with every debugger, the one already running
 * included.  While the program runs the entry is drawn hollow and handed to
 * the run controller, which tells the debugger as soon as it can take it
 * (e_d_run_breakpoints_changed); it turns solid on the debugger's word.
 * Return: 0, or -1 when the debugger could not be told.
 */
static int e_d_brk_toggle_on(FENSTER *f, int line)
{
 e_d_brk_grow();
 e_d_sbrpts[e_d_nbrpts - 1] = MALLOC(strlen(f->datnam) + 1);
 strcpy(e_d_sbrpts[e_d_nbrpts - 1], f->datnam);
 e_d_ybrpts[e_d_nbrpts - 1] = line;
 e_d_nrbrpts[e_d_nbrpts - 1] = 0;
 if (!e_d_swtch)
  return(0);
 if (e_d_brk_must_wait())
 {
  e_d_brk_gone_revive(e_d_nbrpts - 1);
  e_d_run_breakpoints_changed(f->ed);
  return(0);
 }
 if (e_d_ga_at_stop())
  return(e_d_ga_brk_sync(f->ed));
 if (e_d_brk_arm(f->ed, e_d_nbrpts - 1) == -1)
  return(-1);
 e_d_brk_say_refusal(f, e_d_nbrpts - 1);
 return(0);
}

int e_make_breakpoint(FENSTER *f, int sw)
{
 ECNT *cn = f->ed;
 SCHIRM *s = cn->f[cn->mxedt]->s;
 BUFFER *b = cn->f[cn->mxedt]->b;
 int i;

 if (e_d_start_refused(f))
  return(0);
 if (!e_d_ga_sync_runs() && e_d_ga_refused(f))
  return(0);
 if (!sw)
 {
  if (!e_check_c_file(f->datnam) && !e_d_dap_source_ext(f->datnam))
   return(e_error(e_p_msg[ERR_NO_CFILE], 0, f->fb));
  e_d_breakpoints_changed(cn);      /* the line's breakpoint goes or comes */
  for(i = 0; i < s->brp[0] && s->brp[i+1] != b->b.y; i++)
   ;
  if (i < s->brp[0])
  {
   for (i++; i < s->brp[0]; i++) s->brp[i] = s->brp[i+1];
   (s->brp[0])--;
   if (e_d_brk_toggle_off(f, b->b.y + 1) == -1)
    return(-1);
  }
  else
  {
   if (e_d_brk_toggle_on(f, b->b.y + 1) == -1)
    return(-1);
   (s->brp[0])++;
   s->brp = REALLOC(s->brp, (s->brp[0]+1) * sizeof(int));
   s->brp[s->brp[0]] = b->b.y;
  }
 }
 e_schirm(f, 1);
 return(1);
}

/*   start Debugger   */
/* Roll back a debugger launch that failed partway through e_exec_deb():
   close every pipe/pty fd opened so far and clear e_d_swtch so the user can
   try again.  Before this, an error mid-setup leaked those fds AND left
   e_d_swtch=1, which wedged the debugger for the rest of the session (every
   later attempt returned "already running" immediately). */
static void e_d_exec_fail(void)
{
 e_d_close_fd(&rfildes[0]);
 e_d_close_fd(&rfildes[1]);
 e_d_close_fd(&wfildes[0]);
 e_d_close_fd(&wfildes[1]);
 e_d_close_fd(&efildes[0]);
 e_d_close_fd(&efildes[1]);
 e_d_pty_close();
 e_d_swtch = 0;
}

/**
 * e_d_child_errors_nonblocking - Say whether the debugger being started
 * writes its errors without waiting for the editor to read them.
 *
 * Its output always waits: a debugger writing to a full non-blocking pipe
 * drops what does not fit (gdb) or dies of it (pdb's BlockingIOError), and
 * a long burst of program output then hangs the session or ends it as if
 * the program had exited.  Its errors do not wait, since the editor reads
 * them only between output lines; jdb cannot write either way but waiting.
 */
static int e_d_child_errors_nonblocking(void)
{
 return(e_deb_type != DEB_JDB);
}

/**
 * e_d_fd_nonblocking - Make writes to @fd return at once instead of waiting.
 */
static void e_d_fd_nonblocking(int fd)
{
 fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

/* Room for the longest command line a debugger is started with (jdb's five
   words) and the NULL that ends it. */
#define E_D_ARGV_MAX 8

/* How jdb is started: it waits on loopback, on a port the system picks, for
   the program's VM to connect to it, and the editor starts that VM itself
   on the session's pty.  Never a VM that listens: whoever reached its port
   first could run code as the user.  jdb's words are pinned to English,
   the only ones the editor reads, whatever the user's locale. */
#define E_D_JDB_LOCALE_LANG    "-J-Duser.language=en"
#define E_D_JDB_LOCALE_COUNTRY "-J-Duser.country=US"
#define E_D_JDB_CONNECT        "-connect"
#define E_D_JDB_LISTEN_SPEC \
 "com.sun.jdi.SocketListen:localAddress=127.0.0.1,port=0"

/**
 * e_d_jdb_argv - jdb's command line: listen on loopback for the VM.
 * @argv: receives the words, NULL-terminated.
 *
 * The program is not on it: the editor starts the VM with the main class
 * once jdb says where it listens.
 */
static void e_d_jdb_argv(char **argv)
{
 argv[0] = e_debugger;
 argv[1] = E_D_JDB_LOCALE_LANG;
 argv[2] = E_D_JDB_LOCALE_COUNTRY;
 argv[3] = E_D_JDB_CONNECT;
 argv[4] = E_D_JDB_LISTEN_SPEC;
 argv[5] = NULL;
}

/**
 * e_d_debugger_argv - The command line the selected debugger is started
 * with, the same in the terminal and in the graphical editor.
 * @prog: the program, or the source for pdb and a68g;
 * @argv: receives the words, NULL-terminated (E_D_ARGV_MAX entries).
 */
static void e_d_debugger_argv(char *prog, char **argv)
{
 int n = 0;

 if (e_deb_type == DEB_JDB)
 {
  e_d_jdb_argv(argv);
  return;
 }
 argv[n++] = e_debugger;
 if (e_deb_type == DEB_PDB)
 {
  argv[n++] = "-m";
  argv[n++] = "pdb";
 }
 else if (e_deb_swtch)
  argv[n++] = e_deb_swtch;
 argv[n++] = prog;
 argv[n] = NULL;
}

#ifndef NO_XWINDOWS
/**
 * e_d_script_word - Write one word of the debugger's command line to the
 * graphical editor's helper script, quoted for the shell.
 * @fp: the script; @word: the word.
 *
 * A program path with a blank in it stays one word, as it is when the
 * terminal editor starts the debugger.
 */
static void e_d_script_word(FILE *fp, const char *word)
{
 fputc('\'', fp);
 for (; *word; word++)
 {
  if (*word == '\'')
   fputs("'\\''", fp);
  else
   fputc(*word, fp);
 }
 fputc('\'', fp);
}

/**
 * e_d_script_command - Write the debugger's command line to the helper
 * script.
 * @fp: the script; @argv: the words, NULL-terminated.
 *
 * The line starts a shell that records its PID and then becomes the
 * debugger (exec), so the recorded PID is the debugger's own: Pause sends
 * it SIGINT, which the script that hosts the debugger could not pass on.
 */
static void e_d_script_command(FILE *fp, char **argv)
{
 char pidfile[PATH_MAX];
 int i;

 e_d_debugger_pid_path(pidfile);
 fputs("sh -c 'echo $$ > \"$1\"; shift; exec \"$@\"' sh ", fp);
 e_d_script_word(fp, pidfile);
 fputc(' ', fp);
 for (i = 0; argv[i]; i++)
 {
  if (i > 0)
   fputc(' ', fp);
  e_d_script_word(fp, argv[i]);
 }
}

/**
 * e_d_script_forget_pid - Have the helper script remove the debugger's
 * recorded PID the moment the debugger has ended.
 * @fp: the script.
 *
 * The script then waits for a Return on the terminal the editor came from,
 * and the session can outlive the debugger meanwhile; Pause must never
 * signal the number by then, which the system may have given to another
 * process.
 */
static void e_d_script_forget_pid(FILE *fp)
{
 char pidfile[PATH_MAX];

 e_d_debugger_pid_path(pidfile);
 fputs("rm -f ", fp);
 e_d_script_word(fp, pidfile);
 fputc('\n', fp);
}
#endif

/**
 * e_d_debugger_keeps_pty_slave - Does the debugger hold the program's pty?
 *
 * gdb and the legacy debuggers start the program themselves and hand it the
 * pty, so the debugger keeps it open and the editor drops its own copy.
 * Under jdb the editor starts the program's VM itself: jdb never touches
 * the pty, and the editor keeps its copy until the VM has it.  The VM is
 * then the only holder, so its end shows on the master as a hang-up.
 * Return: 0 for jdb, 1 for the others.
 */
static int e_d_debugger_keeps_pty_slave(void)
{
 return(e_deb_type != DEB_JDB);
}

/**
 * e_d_pty_slave_close - Drop this process's copy of the program's side of
 * the pty.
 */
static void e_d_pty_slave_close(void)
{
 if (e_d_pty_slave < 0)
  return;
 close(e_d_pty_slave);
 e_d_pty_slave = -1;
}

/* ---- The Java program's VM, started by the editor ------------------------
 *
 * jdb only listens; the editor starts the VM on the session's pty with
 * server=n, so the VM dials jdb's loopback port and nothing listens that
 * could run the user's code for someone else.  The program's output never
 * mixes with what jdb says, and the program reads what the user types.
 * The editor owns the VM: it ends it by its PID at Quit and at every other
 * end of the session.
 */

#define E_D_JDB_JAVA      "java"
#define E_D_JDB_AGENT_FMT \
 "-agentlib:jdwp=transport=dt_socket,server=n,suspend=y,address=127.0.0.1:%d"
#define E_D_JDB_AGENT_MAX 128
#define E_D_JDB_VM_ARGC   3

/* The VM of the session. */
typedef struct {
 WpeChild child;             /* the VM; its pid is 0 once it is gone */
 int launched;               /* a VM was started for this session */
 int status;                 /* its wait status, when status_known */
 int status_known;
 int awaiting_end;           /* jdb said the program ended; the session
                                waits for the process the editor started */
 char main_class[PATH_MAX];  /* the class it runs */
 char java[PATH_MAX];        /* the java it is started with */
 char dir[PATH_MAX];         /* where it runs: the build's directory */
} E_D_JDB_VM;

static E_D_JDB_VM e_d_jdb_vm;

/**
 * e_d_find_in_path - Where @name is found on PATH, as the shell finds it.
 * @name: a command; @out, @n: receives its path.
 * Return: 1 when an executable file of that name is on PATH, else 0.
 */
static int e_d_find_in_path(const char *name, char *out, size_t n)
{
 const char *path = getenv("PATH"), *dir, *end;
 int len;

 if (!path)
  return(0);
 for (dir = path; ; dir = end + 1)
 {
  end = strchr(dir, ':');
  if (!end)
   end = dir + strlen(dir);
  len = (int)(end - dir);
  if (len == 0)
   snprintf(out, n, "./%s", name);
  else
   snprintf(out, n, "%.*s%c%s", len, dir, DIRC, name);
  if (access(out, X_OK) == 0)
   return(1);
  if (*end == '\0')
   return(0);
 }
}

/**
 * e_d_jdb_java_path - The java the program's VM is started with.
 * @out, @n: receives it.
 *
 * The java next to jdb, the one jdb itself started the VM with before the
 * editor did: a class compiled by that JDK's javac runs there, while the
 * java first on PATH may be another release.  Otherwise java on PATH.
 */
static void e_d_jdb_java_path(char *out, size_t n)
{
 char jdb[PATH_MAX], real[PATH_MAX], *slash;

 if (e_d_find_in_path(e_debugger, jdb, sizeof jdb) && realpath(jdb, real))
 {
  slash = strrchr(real, DIRC);
  if (slash)
  {
   slash[1] = '\0';
   /* A path that does not fit is no java found: PATH is tried next. */
   if (snprintf(out, n, "%s%s", real, E_D_JDB_JAVA) < (int)n &&
       access(out, X_OK) == 0)
    return;
  }
 }
 if (e_d_find_in_path(E_D_JDB_JAVA, out, n))
  return;
 snprintf(out, n, "%s", E_D_JDB_JAVA);
}

/**
 * e_d_jdb_vm_prepare - Note what the session's VM will run, and where,
 * before jdb is started.
 * @f: the source window; @main_class: the class to run.
 */
static void e_d_jdb_vm_prepare(FENSTER *f, const char *main_class)
{
 char *dir = e_build_dir(f);

 e_d_jdb_vm.launched = 0;
 e_d_jdb_vm.status_known = 0;
 e_d_jdb_vm.awaiting_end = 0;
 snprintf(e_d_jdb_vm.main_class, sizeof e_d_jdb_vm.main_class, "%s",
          main_class);
 e_d_jdb_java_path(e_d_jdb_vm.java, sizeof e_d_jdb_vm.java);
 e_d_jdb_vm.dir[0] = '\0';
 if (!dir)
  return;
 snprintf(e_d_jdb_vm.dir, sizeof e_d_jdb_vm.dir, "%s", dir);
 FREE(dir);
}

/**
 * e_d_jdb_vm_forget - Stop following the VM, so nothing is ever sent to a
 * process that reused its number.  One reported ended needs nothing.
 */
static void e_d_jdb_vm_forget(void)
{
 wpe_child_forget(&e_d_jdb_vm.child);
}

/**
 * e_d_jdb_vm_note_end - Keep how the VM ended, for the end line, and
 * forget its PID.
 * @status: its wait status.
 */
static void e_d_jdb_vm_note_end(int status)
{
 e_d_jdb_vm.status = status;
 e_d_jdb_vm.status_known = 1;
 e_d_jdb_vm_forget();
}

/**
 * e_d_jdb_vm_gone - Has the session's VM ended?
 *
 * Asked without waiting: the editor's SIGCHLD handler reaps the VM and keeps
 * its status in the VM's slot of the owner of the editor's children.
 * Return: 1 when there is no VM running.
 */
static int e_d_jdb_vm_gone(void)
{
 int st, ended;

 if (e_d_jdb_vm.child.pid <= 0)
  return(1);
 ended = wpe_child_ended(&e_d_jdb_vm.child, &st);
 if (ended == WPE_CHILD_RUNS)
  return(0);
 if (ended == WPE_CHILD_ENDED)
  e_d_jdb_vm_note_end(st);
 else
  e_d_jdb_vm_forget();
 return(1);
}

/**
 * e_d_jdb_vm_end - End the Java program at once.
 *
 * Quit, Esc, the start's deadline and every end of the session come here,
 * so no Java program outlives its session: jdb's own "quit" would let a VM
 * stopped at a breakpoint run on.  The VM leads a session of its own, so
 * what it started goes with it, a wrapper script around java included.
 * SIGKILL comes first, as the VM is the program and nothing asks it to
 * end; the owner of the editor's children follows its end from there, so
 * Quit returns at once whatever the process does, and the session the
 * user starts next is not told of it.
 */
static void e_d_jdb_vm_end(void)
{
 e_d_jdb_vm.launched = 0;
 e_d_jdb_vm.awaiting_end = 0;
 if (e_d_jdb_vm_gone())
  return;
 wpe_child_end_detached(&e_d_jdb_vm.child, SIGKILL);
}

/**
 * e_d_jdb_vm_on_end - The owner of the editor's children reported that
 * the VM's process ended.
 * @c: the VM; @status: its wait status; @how: WPE_CHILD_ENDED, or
 * WPE_CHILD_ENDED_UNKNOWN; @data: unused.
 *
 * Its status is kept for the end line.  When jdb has already said the
 * program ended, the session was waiting for this, and the user now reads
 * how the program ended.
 */
static void e_d_jdb_vm_on_end(WpeChild *c, int status, int how, void *data)
{
 (void)c;
 (void)data;
 if (how == WPE_CHILD_ENDED)
  e_d_jdb_vm_note_end(status);
 if (!e_d_jdb_vm.awaiting_end)
  return;
 e_d_jdb_vm.awaiting_end = 0;
 e_d_run_process_ended();
}

/**
 * e_d_jdb_vm_await_end - Keep the session until the process the editor
 * started for the program ends, when jdb said the program ended first.
 *
 * jdb sees the end when the VM's socket closes, which is while the process
 * exits, so the exit code is not known yet.  The session goes on as a
 * running program (the user keeps every key; debugger keys say the program
 * runs; Debug > Quit ends it at once) until the editor's SIGCHLD handler
 * reaps the process, normally a moment later.  A java that is a wrapper
 * script living on after the VM keeps the session until it ends, and the
 * end line then gives the wrapper's status, as a shell would.
 * Return: 1 when the session now waits, 0 when the end line can be said.
 */
static int e_d_jdb_vm_await_end(void)
{
 if (e_d_jdb_vm_gone())
  return(0);
 e_d_jdb_vm.awaiting_end = 1;
 e_d_run_wait_for_process();
 return(1);
}

/**
 * e_d_jdb_vm_fork - Start the Java program's VM, which connects to jdb.
 * @port: the loopback port jdb listens on.
 *
 * The VM runs on the session's pty, in a session of its own with the pty
 * as its controlling terminal, so it reads what the user types and its end
 * shows on the master; without a pty it writes to the terminal the debugger
 * session runs on, as gdb's program does.  It runs in the build's
 * directory.  A terminal, a directory or a java that fails is named where
 * the VM's words go, so the user reads it in Messages.  The editor's copy
 * of the pty's slave goes once the VM has it.
 * Return: 0, or -1 when no process could be made.
 */
static int e_d_jdb_vm_fork(int port)
{
 char agent[E_D_JDB_AGENT_MAX], *argv[E_D_JDB_VM_ARGC + 1];
 WpeChildSpec spec;

 snprintf(agent, sizeof agent, E_D_JDB_AGENT_FMT, port);
 argv[0] = e_d_jdb_vm.java;
 argv[1] = agent;
 argv[2] = e_d_jdb_vm.main_class;
 argv[3] = NULL;
 memset(&spec, 0, sizeof spec);
 spec.argv = argv;
 spec.io = WPE_CHILD_IO_TTY_PATH;
 spec.group = WPE_CHILD_OWN_SESSION;
 spec.tty_fd = e_d_pty_slave;
 spec.tty_path = e_d_tty;
 spec.say_failed = 1;
 spec.side_pty_slave = WPE_CHILD_NO_FD;
 spec.what = e_d_jdb_vm.main_class;
 if (e_d_jdb_vm.dir[0])
  spec.cwd = e_d_jdb_vm.dir;
 if (wpe_child_start(&spec, &e_d_jdb_vm.child, e_d_jdb_vm_on_end, NULL) != 0)
  return(-1);
 e_d_jdb_vm.launched = 1;
 e_d_pty_slave_close();
 return(0);
}

/**
 * e_d_own_tty_name - Record the editor's own terminal in e_d_tty.
 *
 * The program under gdb and the legacy debuggers writes there when no pty
 * could be opened, and a session whose terminal is unknown is refused
 * ("No terminal for the program").  Asked of the kernel directly: running
 * a separate program for the name raced the editor's child-exit handler,
 * which can make the read of its answer fail, so a debug start failed at
 * random.  When the editor has no terminal (started from a desktop), the
 * text says so, and any other failure gives the system's reason, so the
 * refusal message says why.
 */
static void e_d_own_tty_name(void)
{
 int err = ttyname_r(STDIN_FILENO, e_d_tty, sizeof e_d_tty);

 if (err == 0)
  return;
 if (err == ENOTTY)
  strcpy(e_d_tty, "the editor has no terminal and no pty could be opened");
 else
  snprintf(e_d_tty, sizeof e_d_tty, "%s", strerror(err));
}

/**
 * e_d_program_tty_name - Record in e_d_tty the terminal the debugged
 * program writes to.
 *
 * Debugging works from a desktop launcher or any other start without a
 * terminal on the editor's input, as long as the program has a pty to
 * write to: the program's own pty is its terminal, and its output reaches
 * the editor through the pty's master.  Only when no pty could be opened is
 * the editor's own terminal the program's, as it always was.  When there
 * is neither, e_d_tty holds the reason and the session is refused with it.
 * A pty name too long for e_d_tty is not cut short into the name of some
 * other file: the editor's own terminal is used instead.
 */
static void e_d_program_tty_name(void)
{
 if (e_d_pty_slave_name[0] == DIRC &&
     strlen(e_d_pty_slave_name) < sizeof e_d_tty)
 {
  snprintf(e_d_tty, sizeof e_d_tty, "%s", e_d_pty_slave_name);
  return;
 }
 e_d_own_tty_name();
}

/* The directory the debugger runs in (the child changes into it before it
   execs the debugger) when it is also the directory of the program it was
   given, or "".  Paths relative to it name one file to pdb whichever
   command carries them. */
static char e_d_debugger_dir[PATH_MAX];

/**
 * e_d_dir_holds_file - Is @path a file directly in directory @dir?
 * @dir: a directory, with or without a closing separator; @path: a path.
 */
static int e_d_dir_holds_file(const char *dir, const char *path)
{
 size_t dlen = strlen(dir);
 const char *rest;

 if (path[0] != DIRC)
  return(strchr(path, DIRC) == NULL);   /* a bare name, run from @dir */
 while (dlen > 1 && dir[dlen - 1] == DIRC)
  dlen--;
 if (dlen == 0 || strncmp(path, dir, dlen) || path[dlen] != DIRC)
  return(0);
 for (rest = path + dlen; *rest == DIRC; rest++)
  ;                            /* "dir/" joined with "/name" */
 return(strchr(rest, DIRC) == NULL);
}

/**
 * e_d_debugger_dir_note - Remember the directory the debugger was started
 * in, as the child changes into it, when the program it was given lies in
 * it.
 * @f: the window the session was started from; @prog: the program, for
 * pdb its script.
 *
 * pdb resolves a relative path in "clear" against the directory it runs
 * in, but one in "b" against its script's directory.  Only when the two are
 * one does a relative path name the same file in both, and a breakpoint in
 * a file under a very deep directory can then still be named in a command
 * short enough for the request engine.  Otherwise (a project built in
 * another directory) the full path is sent, and a breakpoint is never set
 * in another file of the same relative name.
 */
static void e_d_debugger_dir_note(FENSTER *f, const char *prog)
{
 char *dir = e_build_dir(f);

 e_d_debugger_dir[0] = '\0';
 if (!dir)
  return;
 if (prog && e_d_dir_holds_file(dir, prog))
  snprintf(e_d_debugger_dir, sizeof e_d_debugger_dir, "%s", dir);
 FREE(dir);
}

/**
 * e_d_debugger_spec - What every debugger start has in common.
 * @spec: receives it; @argv: the command; @dir: where it runs.
 *
 * The debugger runs in the program's directory, as the language servers
 * and adapters do: gdb finds a relatively named program ("prog.e") there and
 * resolves source and breakpoint paths right.
 */
static void e_d_debugger_spec(WpeChildSpec *spec, char **argv, const char *dir)
{
 memset(spec, 0, sizeof *spec);
 spec->argv = argv;
 spec->cwd = dir;
 spec->side_pty_slave = WPE_CHILD_NO_FD;
 spec->tty_fd = WPE_CHILD_NO_FD;
 spec->what = e_debugger;
}

/**
 * e_d_debugger_side_pty - The program's side of the pty the debugger
 * itself is to hold: gdb and the legacy debuggers start the program and
 * name the pty to it.  Under jdb the editor keeps it for the program's VM.
 */
static int e_d_debugger_side_pty(void)
{
 if (!e_d_debugger_keeps_pty_slave())
  return(WPE_CHILD_NO_FD);
 return(e_d_pty_slave);
}

/**
 * e_d_pty_open - The pty the debugged program's output is read from.
 *
 * Without one the program writes to the editor's terminal, as it always
 * could, and the session starts all the same.  The master never blocks: a
 * read ends when the program has printed nothing more, and keys typed for a
 * program that stopped reading wait on the loop instead of in a write.
 */
static void e_d_pty_open(void)
{
 if (wpe_child_pty_make(&e_d_pty_master, &e_d_pty_slave, e_d_pty_slave_name,
                        sizeof e_d_pty_slave_name) < 0)
  e_d_pty_slave_name[0] = '\0';
 e_d_pty_hung = 0;
}

static void e_d_on_debugger_ended(WpeChild *c, int status, int how,
                                  void *data);   /* (below) */
static void e_d_debugger_let_go(void);           /* (below) */

/**
 * e_d_debugger_start - Start the debugger of a new session, its end
 * watched from before its fork.
 * @spec: how.
 * Return: 0, or -1 when it could not be started.
 */
static int e_d_debugger_start(const WpeChildSpec *spec)
{
 e_d_debugger_let_go();
 if (wpe_child_start(spec, &e_d_dbg.child, e_d_on_debugger_ended, NULL) != 0)
  return(-1);
 e_d_dbg.live = 1;
 e_d_dbg.asked_end = 0;
 e_d_dbg.end_expected = 0;
 e_d_dbg.ended = 0;
 e_d_dbg.status_known = 0;
 e_d_dbg.say_later = 0;
 e_d_dbg.held = 0;
 return(0);
}

/**
 * e_d_start_on_pipes - Start the terminal editor's debugger: commands on
 * pipes, the program on the pty beside them.
 * @f: the source window; @argv: the debugger's command.
 *
 * The debugger and its program take SIGINT as a shell gives it, so Pause
 * can stop them; left in the editor's foreground process group they would
 * also get the SIGINT the terminal sends for a Ctrl-C typed by habit, and
 * the session would end or hang.  In a session of its own the debugger
 * hears only what the editor sends it, and Ctrl-C stays a key of the
 * editor's.  The hangup that would end it when the editor's terminal closes
 * no longer reaches it, so the system is asked to end it when the editor
 * dies by SIGKILL; otherwise a program stuck in a loop would go on running.
 * Return: 0, or -1 when it could not be started (said in a box).
 */
static int e_d_start_on_pipes(FENSTER *f, char **argv)
{
 WpeChildSpec spec;
 char *dir = e_build_dir(f);
 int ret;

 e_d_debugger_spec(&spec, argv, dir);
 spec.io = WPE_CHILD_IO_PIPES_AND_PTY;
 spec.group = WPE_CHILD_OWN_SESSION;
 spec.die_with_editor = 1;
 spec.stderr_nonblocking = e_d_child_errors_nonblocking();
 spec.side_pty_slave = e_d_debugger_side_pty();
 spec.exec_failed = e_p_msg[ERR_IN_COMMAND];
 ret = e_d_debugger_start(&spec);
 if (dir)
  FREE(dir);
 if (ret != 0)
 {
  e_error(e_p_msg[ERR_PROCESS], 0, f->fb);
  return(-1);
 }
 /* The editor's ends never block: a debugger that stops reading gets its
    commands queued on the loop instead. */
 rfildes[1] = e_d_dbg.child.in_fd;
 wfildes[0] = e_d_dbg.child.out_fd;
 efildes[0] = e_d_dbg.child.err_fd;
 /* jdb's program gets the slave from the editor, later. */
 if (e_d_debugger_keeps_pty_slave())
  e_d_pty_slave_close();
 e_d_program_tty_name();
 return(0);
}

#ifndef NO_XWINDOWS
/* The graphical editors' debugger runs in a helper script, on five named
   pipes; without a pty the script runs in an xterm of its own, whose
   command line has these words. */
#define E_D_FIFO_N     5
#define E_D_XTERM_ARGC 10

/**
 * e_d_fifos_make - The named pipes the helper script and the editor talk on.
 * Return: 0, or -1 when one could not be made.
 */
static int e_d_fifos_make(void)
{
 int i;

 for (i = 0; i < E_D_FIFO_N; i++)
 {
  if (npipe[i])
   FREE(npipe[i]);
  npipe[i] = MALLOC(strlen(e_tmp_dir) + 16);
  sprintf(npipe[i], "%s/we_pipe%d", e_tmp_dir, i);
  remove(npipe[i]);
 }
 for (i = 0; i < E_D_FIFO_N; i++)
  if (mkfifo(npipe[i], S_IRUSR | S_IWUSR) < 0)
   return(-1);
 return(0);
}

/**
 * e_d_script_write - Write the helper script that runs the debugger on the
 * named pipes.
 * @file, @n: receives its path; @argv: the debugger's command.
 *
 * It names the program's terminal first (the pty's, or its own when there
 * is no pty), so the editor reads the program's output there.
 * Return: 0, or -1 when it could not be written.
 */
static int e_d_script_write(char *file, size_t n, char **argv)
{
 FILE *fp;

 snprintf(file, n, "%s/we_sys", e_tmp_dir);
 fp = fopen(file, "w+");
 if (!fp)
  return(-1);
 fprintf(fp, "#!/bin/sh\n");
 if (e_d_pty_master >= 0)
  fprintf(fp, "echo '%s' > %s\n", e_d_pty_slave_name, npipe[1]);
 else
  fprintf(fp, "tty > %s\n", npipe[1]);
 e_d_script_command(fp, argv);
 fprintf(fp, " < %s > %s 2> %s\n", npipe[0], npipe[1], npipe[2]);
 e_d_script_forget_pid(fp);
 fprintf(fp,
   "echo 'type <Return> to continue' > /dev/tty\nread i < /dev/tty\n");
 fprintf(fp, "rm -f %s\n", file);
 fclose(fp);
 chmod(file, 0755);
 return(0);
}

/**
 * e_d_script_argv - How the helper script is run: by itself when the
 * program has the pty, else in an xterm that is the program's terminal.
 * @argv: room for E_D_XTERM_ARGC + 1 words; @file: the script.
 */
static void e_d_script_argv(char **argv, char *file)
{
 int i = 0;

 if (e_d_pty_master >= 0)
 {
  argv[i++] = file;
  argv[i] = NULL;
  return;
 }
 argv[i++] = XTERM_CMD;
 argv[i++] = "+sb";
 argv[i++] = "-geometry";
 argv[i++] = "80x25-0-0";
 argv[i++] = "-T";
 argv[i++] = "xwpe-output";
 argv[i++] = "-e";
 argv[i++] = user_shell;
 argv[i++] = "-c";
 argv[i++] = file;
 argv[i] = NULL;
}

static int e_d_script_connect(FENSTER *f);   /* (below) */

/**
 * e_d_start_in_script - Start the graphical editors' debugger, in its
 * helper script on the named pipes.
 * @f: the source window; @argv: the debugger's command.
 *
 * The script stays in the editor's group: when the debugger ends, it asks
 * on the terminal the editor came from for a Return, as it always did.
 * Return: 0, or -1 when it could not be started (said in a box).
 */
static int e_d_start_in_script(FENSTER *f, char **argv)
{
 char file[PATH_MAX], *script[E_D_XTERM_ARGC + 1], *dir;
 WpeChildSpec spec;
 int ret;

 if (e_d_fifos_make() < 0)
 {
  e_error(e_d_msg[ERR_CANTPIPE], 0, f->fb);
  return(-1);
 }
 if (e_d_script_write(file, sizeof file, argv) < 0)
 {
  e_error(e_p_msg[ERR_PROCESS], 0, f->fb);
  return(-1);
 }
 e_d_script_argv(script, file);
 dir = e_build_dir(f);
 e_d_debugger_spec(&spec, script, dir);
 spec.io = WPE_CHILD_IO_INHERIT_AND_PTY;
 spec.group = WPE_CHILD_EDITORS_GROUP;
 spec.side_pty_slave = e_d_debugger_side_pty();
 ret = e_d_debugger_start(&spec);
 if (dir)
  FREE(dir);
 if (ret != 0)
 {
  remove(file);
  e_error(e_p_msg[ERR_PROCESS], 0, f->fb);
  return(-1);
 }
 if (e_d_debugger_keeps_pty_slave())
  e_d_pty_slave_close();
 return(e_d_script_connect(f));
}
#endif

/**
 * e_d_debugger_launch - Start the session's debugger, the way this
 * display runs it.
 * @f: the source window; @argv: its command.
 * Return: 0, or -1 when it could not be started.
 */
static int e_d_debugger_launch(FENSTER *f, char **argv)
{
#ifndef NO_XWINDOWS
 if (WpeIsXwin())
  return(e_d_start_in_script(f, argv));
#endif
 return(e_d_start_on_pipes(f, argv));
}

int e_exec_deb(FENSTER *f, char *prog)
{
 char *argv[E_D_ARGV_MAX];

 if (e_d_swtch)
  return(1);
 e_d_swtch = 1;
 e_d_cut_reset();
 e_d_debugger_argv(prog, argv);
 if (e_deb_type == DEB_JDB)
  e_d_jdb_vm_prepare(f, prog);
 /* Mark the pipe fds closed up front so e_d_exec_fail() can tell which ones
    a failed launch actually opened (the opens below set the real values). */
 rfildes[0] = rfildes[1] = -1;
 wfildes[0] = wfildes[1] = -1;
 efildes[0] = efildes[1] = -1;
 fflush(stdout);
 if (e_d_prog_output) { FREE(e_d_prog_output); e_d_prog_output = NULL; }
 e_d_prog_output_len = 0;
 e_d_prog_output_cap = 0;
 e_d_pty_open();
 e_d_debugger_pid_forget();
 if (e_d_debugger_launch(f, argv) < 0)
 {
  e_d_exec_fail();
  return(0);
 }
 e_d_debugger_dir_note(f, prog);
 return(1);
}

#ifndef NO_XWINDOWS
/**
 * e_d_script_terminal_raw - The external xterm's terminal passes every
 * byte as it comes, so the program in it reads keys one by one.
 */
static void e_d_script_terminal_raw(void)
{
 tcgetattr(rfildes[0], &ntermio);
 ntermio.c_iflag = 0;
 ntermio.c_oflag = 0;
 ntermio.c_lflag = 0;
 ntermio.c_cc[VMIN] = 1;
 ntermio.c_cc[VTIME] = 0;
#ifdef VSWTCH
 ntermio.c_cc[VSWTCH] = 0;
#endif
 tcsetattr(rfildes[0], TCSADRAIN, &ntermio);
}

/**
 * e_d_x_focus_to_editor - Give the editor's X window the keyboard.
 *
 * After a debug session starts the user types into the editor, not into a
 * window the debugger came up behind.  The native Wayland editor has no X
 * connection (and nothing to focus), so there it does nothing; asking Xlib
 * with no connection would end the editor.
 */
static void e_d_x_focus_to_editor(void)
{
 if (WpeXInfo.display == NULL)
  return;
 XSetInputFocus(WpeXInfo.display, WpeXInfo.window,
   RevertToParent, CurrentTime);
 XFlush(WpeXInfo.display);
}

/**
 * e_d_script_connect - Open the helper script's named pipes and the
 * program's terminal it names, once the script runs.
 * @f: the source window.
 * Return: 0, or -1 when one could not be opened (said in a box).
 */
static int e_d_script_connect(FENSTER *f)
{
 int i;

 /* Closed on exec, as every descriptor the editor keeps for a child: jdb's
    program, started later, must not hold the debugger's pipes open. */
 if ((wfildes[0] = open(npipe[1], O_RDONLY | O_CLOEXEC)) < 0)
 {
  e_error(e_p_msg[ERR_PIPEOPEN], 0, f->fb);
  return(-1);
 }
 for (i = 0;
   i < 80 && read(wfildes[0], &e_d_tty[i], 1) == 1 && e_d_tty[i] != '\0' &&
     e_d_tty[i] != '\n';
   i++)
  ;
 if (e_d_tty[i] == '\n')
  e_d_tty[i] = '\0';
 close(wfildes[0]);
 if ((rfildes[0] = open(e_d_tty, O_RDONLY | O_CLOEXEC)) < 0 ||
   (wfildes[1] = open(e_d_tty, O_WRONLY | O_CLOEXEC)) < 0)
 {
  e_error(e_p_msg[ERR_PIPEOPEN], 0, f->fb);
  return(-1);
 }
 if ((rfildes[1] = open(npipe[0], O_WRONLY | O_CLOEXEC)) < 0 ||
   (wfildes[0] = open(npipe[1], O_RDONLY | O_CLOEXEC)) < 0 ||
   (efildes[0] = open(npipe[2], O_RDONLY | O_CLOEXEC)) < 0)
 {
  e_error(e_p_msg[ERR_PIPEOPEN], 0, f->fb);
  return(-1);
 }
 /* The debugger's input and its terminal never block the editor: what
    they do not take now is queued on the loop. */
 e_d_fd_nonblocking(rfildes[1]);
 e_d_fd_nonblocking(wfildes[1]);
 if (e_deb_mode)
  e_d_script_terminal_raw();
 /* Legacy external-xterm debugger path (e_deb_mode): the child just
    exec'd an xterm to host the debugger.  Let that xterm map its window
    and settle before we pull the input focus back to xwpe, or the
    focus-set races the xterm and may not stick.  The fully event-driven
    cure is to wait for the xterm's MapNotify via SubstructureNotify on the
    root window; that is deferred to the X11-xterm verification pass since it
    cannot be exercised in the headless test harness.  Until then this is a
    single named, bounded settle, not an open-coded magic number. */
 usleep(DEB_XTERM_SETTLE_MS * 1000);
 e_d_x_focus_to_editor();
 return(0);
}
#endif

/* ===================================================================== *
 *  DAP debug bridge -- editor glue for Debug Adapter Protocol backends.  *
 *                                                                       *
 *  Maps xwpe's debugger menu/keys onto the protocol engine (we_dap.c):  *
 *    Ctrl-G R (Run/Continue)  -> e_d_dap_run                            *
 *    F7 / F8  (Step in/over)  -> e_d_dap_step                           *
 *    Watches window refresh   -> e_d_dap_watches (evaluate)             *
 *    Ctrl-G Q (Quit)          -> e_d_dap_quit                           *
 *  Only the Go/dlv vertical slice is wired today; Rust (lldb-dap) and   *
 *  Scala (Metals) reuse the same bridge with a different adapter argv.  *
 * ===================================================================== */

static e_dap_session *g_dap = NULL;       /* the live adapter session, or NULL */
static e_bsp_session *g_bsp = NULL;       /* JVM/Scala: the build server hosting
                                             the DAP adapter, kept alive for the
                                             whole session (NULL otherwise)     */
static int            g_dap_first = 0;   /* the program still runs to its first
                                            stop, which no command has taken */
static int            g_dap_run_fd = -1; /* the adapter's descriptor, as the
                                            run watches it                   */
static int            g_dap_closing = 0; /* the session is being closed     */
static int            g_dap_shown = 0;   /* how much of the program's output
                                            (e_d_prog_output) Messages has  */
static e_dap_host     g_dap_host;
static char           g_dap_progdir[1024] = "";  /* the user's program directory */
static char           g_dap_stop_file[1024] = "";/* full path of the current stop */
static int            g_dap_stop_line = -1;      /* line of the current stop      */
static char           g_dap_stop_reason[32] = "";/* the adapter's reason for it  */
static int            g_dap_entry_func = 0;      /* the editor set an entry
                                                    function breakpoint      */
static char           g_dap_adapter_name[64] = "";/* "dlv", "gdb", ...: what
                                                    the start's lines name   */
#define E_D_DAP_START_FAILED_LINE "Could not start the debug adapter."
#define E_D_DAP_ERROR_MAX 2048
static char g_dap_start_error[E_D_DAP_ERROR_MAX] = ""; /* why the adapter
                                                    refused the start      */
#define E_D_DAP_RUN_FAILED_LINE   "Program failed to start under the debugger."
/* The stop reasons that mean "the program has just started": the adapter's
   own entry stop, and the entry-function breakpoint the editor sets itself
   (Go's main.main).  Any other reason is a stop the user asked for. */
#define E_D_DAP_REASON_ENTRY    "entry"
#define E_D_DAP_REASON_FUNCTION "function breakpoint"

/* True while a DAP session is open. */
static int e_d_dap_active(void)
{
 return(g_dap != NULL);
}

/* e_d_swtch once an adapter's handshake is done (e_d_dap_start_finish). */
#define E_D_DAP_SESSION_OPEN 3

/**
 * e_d_dap_adapter_died - Did the session's adapter go away without the
 * program ending?
 *
 * Its transport ended, or its process ended, and it never said the program
 * ended: it crashed, or was killed from outside.  Only a session that had
 * opened (e_d_swtch 3) counts; a start that failed says why in its own box.
 * Return: 1 when the session ends because the adapter died.
 */
static int e_d_dap_adapter_died(void)
{
 if (e_d_swtch != E_D_DAP_SESSION_OPEN || e_dap_program_said_ended(g_dap))
  return(0);
 return(e_dap_ended(g_dap) || e_dap_adapter_gone(g_dap));
}

/**
 * e_d_dap_died_line - The line that says the debug adapter ended without
 * being asked, as for a line debugger that died.
 * @out, @n: receives it.
 *
 * The status is read from what the SIGCHLD handler kept, before Quit hands
 * the adapter on: an adapter that was reaped and not reported yet still has
 * its exit status said.  The user knows to look at the adapter, not at the
 * program, instead of reading "End of Debugging".
 * Return: 1 when the adapter died, 0 when the session ends normally.
 */
static int e_d_dap_died_line(char *out, size_t n)
{
 int status = 0;

 if (!e_d_dap_adapter_died())
  return(0);
 if (e_dap_adapter_end_status(g_dap, &status))
  snprintf(out, n, E_D_DIED_FMT, g_dap_adapter_name,
           e_d_exit_status_number(status));
 else
  snprintf(out, n, E_D_DIED_NO_STATUS, g_dap_adapter_name);
 return(1);
}

/* Tail of a path: "/tmp/p/main.go" -> "main.go". */
static const char *e_d_dap_basename(const char *path)
{
 const char *slash = path ? strrchr(path, '/') : NULL;
 return(slash ? slash + 1 : path);
}

/* One DAP-debuggable language: which file extension, which adapter to spawn,
   how it connects, and whether xwpe must compile the source first.  Adding a
   language is a row here, never new plumbing. */
typedef struct {
 const char  *ext;          /* source extension, e.g. ".go" / ".rs" */
 char *const *argv;         /* preferred adapter command line       */
 char *const *argv_alt;     /* alternative adapter, or NULL         */
 const char  *entry_func;   /* stop-at-entry function, or NULL      */
 int          stdio;        /* 0 = reverse-TCP (dlv), 1 = stdio (gdb/lldb) */
 int          compile;      /* 0 = adapter builds (Go), 1 = compile first (Rust) */
 int          bsp;          /* 1 = JVM/Scala: bootstrap via BSP (Bloop/scala-cli)
                               for the DAP endpoint, then connect TCP.  argv[0] is
                               the build tool that must be in PATH (scala-cli). */
} e_d_dap_lang;

static char *const DAP_ARGV_GO[]        = { "dlv", "dap", NULL };
static char *const DAP_ARGV_RUST_GDB[]  = { "gdb", "--interpreter=dap", NULL };
static char *const DAP_ARGV_RUST_LLDB[] = { "lldb-dap", NULL };
static char *const DAP_ARGV_SCALA[]     = { "scala-cli", NULL };

static const e_d_dap_lang DAP_LANGS[] = {
 { ".go", DAP_ARGV_GO,       NULL,             "main.main", 0, 0, 0 },
 /* Rust: gdb and lldb-dap both work over stdio.  Default to gdb (everywhere on
    Linux); fall back to lldb-dap when gdb is absent -- the macOS case, where
    lldb is native.  e_d_dap_choose_argv also honours XWPE_DAP_ADAPTER. */
 { ".rs", DAP_ARGV_RUST_GDB, DAP_ARGV_RUST_LLDB, NULL,      1, 1, 0 },
 /* Scala/JVM: no standalone DAP server exists; scala-cli (bundling Bloop) hosts
    scala-debug-adapter and hands back a tcp:// DAP endpoint over BSP.  xwpe runs
    the BSP bootstrap (we_bsp.c), then connects the DAP engine to the endpoint.
    The build server compiles, so xwpe does not (compile=0). */
 { ".scala", DAP_ARGV_SCALA, NULL,             NULL,        0, 0, 1 },
};
#define DAP_NLANGS ((int)(sizeof(DAP_LANGS) / sizeof(DAP_LANGS[0])))

/* True if `datnam` ends in a DAP-debugged language's extension.  The debugger
   gates (e_make_breakpoint, e_d_quit) otherwise admit only files e_check_c_file
   recognises -- the C/compiler-table check -- which does not know a build-server
   language like Scala (.scala is built by Bloop, not an xwpe compiler entry).
   This lets such files carry breakpoints and restore focus on quit. */
static int e_d_dap_source_ext(const char *datnam)
{
 int i, n;

 if (!datnam)
  return(0);
 n = strlen(datnam);
 for (i = 0; i < DAP_NLANGS; i++)
 {
  int el = strlen(DAP_LANGS[i].ext);
  if (n > el && !strcmp(datnam + n - el, DAP_LANGS[i].ext))
   return(1);
 }
 return(0);
}

/* Pick the adapter for a language.  Order of preference: an explicit
   XWPE_DAP_ADAPTER=<name substring> that names an installed candidate (lets a
   user force, e.g., "lldb" even where gdb exists); otherwise the first
   candidate that is in PATH (so a gdb-less macOS box auto-selects lldb-dap). */
static char *const *e_d_dap_choose_argv(const e_d_dap_lang *lang)
{
 const char *pref = getenv("XWPE_DAP_ADAPTER");
 int primary_ok = (e_test_command(lang->argv[0]) == 0);
 int alt_ok = (lang->argv_alt && e_test_command(lang->argv_alt[0]) == 0);

 /* Preference order: $XWPE_DAP_ADAPTER (env, for CI/scripts) wins, else the
    saved dialog choice e_dap_adapter, else auto-detect (first in PATH).  Both
    the env value and the saved choice are matched by name against the candidate
    adapters, so the order is robust to the descriptor's primary/alt layout. */
 if (!pref || !*pref)
  pref = (e_dap_adapter == DAP_ADAPTER_GDB)  ? "gdb"  :
         (e_dap_adapter == DAP_ADAPTER_LLDB) ? "lldb" : NULL;
 if (pref && *pref)
 {
  if (alt_ok && strstr(lang->argv_alt[0], pref))
   return(lang->argv_alt);
  if (primary_ok && strstr(lang->argv[0], pref))
   return(lang->argv);
 }
 if (!primary_ok && alt_ok)
  return(lang->argv_alt);
 return(lang->argv);
}

/* The DAP language descriptor for this editor window, or NULL if its file is
   not a DAP-debugged language.  Used to route Ctrl-G R away from the gdb/text
   pipeline and to pick the adapter/transport/compile policy. */
static const e_d_dap_lang *e_d_dap_lang_for(FENSTER *f)
{
 int i, n;

 if (!f || !f->datnam || !DTMD_ISTEXT(f->dtmd))
  return(NULL);
 n = strlen(f->datnam);
 for (i = 0; i < DAP_NLANGS; i++)
 {
  int el = strlen(DAP_LANGS[i].ext);
  if (n > el && !strcmp(f->datnam + n - el, DAP_LANGS[i].ext))
   return(&DAP_LANGS[i]);
 }
 return(NULL);
}

/* on_stopped: just RECORD where the program halted.  The actual cursor jump +
   Watches refresh is deferred to the stop's requests (e_d_dap_stop_ask), made
   once the step/run functions have settled -- so intermediate stops (e.g. a
   "step over" that momentarily lands in the language runtime before we run
   on) never flash on screen. */
static void e_d_dap_on_stopped(const char *file, int line, const char *reason,
                               void *ud)
{
 (void)ud;
 g_dap_stop_line = line;
 g_dap_stop_reason[0] = '\0';
 if (reason)
  snprintf(g_dap_stop_reason, sizeof(g_dap_stop_reason), "%s", reason);
 g_dap_stop_file[0] = '\0';
 if (file)
 {
  strncpy(g_dap_stop_file, file, sizeof(g_dap_stop_file) - 1);
  g_dap_stop_file[sizeof(g_dap_stop_file) - 1] = '\0';
 }
}

static int e_d_dap_watches_fill(FENSTER *f, char **values, int nvalues,
                                int sw);

/**
 * e_d_dap_stop_has_place - Does the recorded stop name a source line?
 *
 * A stop in code without source (the runtime, before Pause moved it) is
 * neither marked nor given watch values, as it never was.
 */
static int e_d_dap_stop_has_place(void)
{
 return(g_dap_stop_line >= 0 && g_dap_stop_file[0]);
}

/**
 * e_d_dap_show_stop - Show the stop the adapter's answers describe: the
 * Watches and Stack windows filled, then the mark on the source line, as
 * every debugger's stop does.
 * @f: the window on top; @values, @nvalues: each watch's value, or NULL
 * and 0 for none.
 */
static void e_d_dap_show_stop(FENSTER *f, char **values, int nvalues)
{
 if (!f || !e_d_dap_stop_has_place())
  return;
 e_d_nstack = 0;                      /* a stop selects the top frame again */
 strncpy(e_d_file, e_d_dap_basename(g_dap_stop_file), sizeof(e_d_file) - 1);
 e_d_file[sizeof(e_d_file) - 1] = '\0';
 e_d_dap_watches_fill(f, values, nvalues, 0);
 e_d_goto_break(g_dap_stop_file, g_dap_stop_line, f);
}

/**
 * e_d_dap_drop_line_cr - Copy @text without the CR a pty puts before LF.
 * @text: the adapter's output event; @out: receives the copy, as long as
 * @text plus its NUL.
 *
 * A program run on a pty ends its lines with CR LF; the CR would show as
 * "^M" at the end of every line in Messages.  A CR that ends the text is the
 * first half of a CR LF split across two events, and goes too.  Any other
 * CR is the program's own and is kept.
 */
static void e_d_dap_drop_line_cr(const char *text, char *out)
{
 const char *p;
 char *o = out;

 for (p = text; *p; p++)
 {
  if (*p == '\r' && (p[1] == '\n' || p[1] == '\0'))
   continue;
  *o++ = *p;
 }
 *o = '\0';
}

/**
 * e_d_dap_show_new_output - Write in Messages what the program printed and
 * Messages does not have yet.
 * @force: show it even if Messages is closed and the program runs.
 *
 * The program's output is kept whole for the User Screen (Alt-F5) as it
 * comes; Messages gets the part after g_dap_shown.  While the program runs
 * and Messages is closed nothing is written unless @force, so no window
 * opens over a box the user has open; the stop shows the rest, as under
 * gdb.  The adapter's lines end in CR LF on a pty, and the CR is dropped.
 */
static void e_d_dap_show_new_output(int force)
{
 FENSTER *top = e_d_run_window(WpeEditor);
 char *clean, *text;
 int new_len;

 if (g_dap_shown > e_d_prog_output_len)
  g_dap_shown = e_d_prog_output_len;      /* another run reset the buffer */
 new_len = e_d_prog_output_len - g_dap_shown;
 if (new_len <= 0)
  return;
 if (!force && e_d_run_going() && !e_d_find_messages_window())
  return;
 clean = MALLOC(new_len + 1);
 if (!clean)
  return;
 memcpy(clean, e_d_prog_output + g_dap_shown, new_len);
 clean[new_len] = '\0';
 g_dap_shown = e_d_prog_output_len;
 e_d_dap_drop_line_cr(clean, clean);
 text = clean;
 while (*text == '\n')
  text++;                       /* e_d_p_message ignores text led by one */
 if (e_d_run_going())
  e_d_p_message(text, top, 0);
 else
  e_d_p_message(text, top, 1);
 FREE(clean);
}

/**
 * e_d_dap_on_output - The program printed, or the adapter spoke for it.
 * @text: the piece; @ud: unused.
 *
 * It goes on the User Screen at once and into Messages when Messages is
 * open (see e_d_dap_show_new_output).  Once Quit has begun nothing is
 * shown: the windows are being put back.
 */
static void e_d_dap_on_output(const char *text, void *ud)
{
 (void)ud;
 if (!text || g_dap_closing)
  return;
 e_d_prog_output_append((char *)text, strlen(text));
 e_d_dap_show_new_output(0);
}

/**
 * e_d_dap_exit_code - The exit code the adapter reported for the program.
 * @code: receives it.
 * Return: 1 when the adapter's `exited` event named one, else 0.
 */
static int e_d_dap_exit_code(long *code)
{
 int c;

 if (!g_dap || !e_dap_exit_code(g_dap, &c))
  return(0);
 *code = c;
 return(1);
}

/**
 * e_d_dap_end_session - The program ran to its end under a debug adapter.
 * @f: the window on top.
 *
 * The user reads "Program exited with code N" in Messages, the mark goes
 * and the editor takes keys again, exactly as at the end of a gdb session;
 * an adapter session used to stay half-open with the mark still painted.
 * Return: 0.
 */
static int e_d_dap_end_session(FENSTER *f)
{
 e_d_dap_show_new_output(1);        /* what it printed, before its end line */
 e_d_say_program_exited(NULL, f);
 return(e_d_quit(f));
}

/**
 * e_d_dap_program_ended - Has the adapter's program already ended?
 *
 * A program that never reaches a breakpoint ends while the adapter starts;
 * whichever command comes first (Run, Step, Finish function) then ends the
 * session with its end line instead of waiting for a stop that never comes.
 * Return: 1 when the adapter reported the end.
 */
static int e_d_dap_program_ended(void)
{
 return(g_dap && e_dap_ended(g_dap));
}

#define E_D_DAP_ENDING_LINE " Ending the program and the debug adapter..."

/**
 * e_d_dap_say_ending - Tell the user Quit is waiting for the adapter.
 * @f: the window on top, or NULL when the editor itself is ending.
 *
 * The adapter ends a running program only when it has answered Quit, which
 * takes a moment (lldb-dap about a second); the bottom row says so.
 */
static void e_d_dap_say_ending(FENSTER *f)
{
 if (f && e_dap_running(g_dap))
  e_status_say(f, E_D_DAP_ENDING_LINE);
}

/**
 * e_d_dap_quit - End the debug adapter and its program, and reset the
 * bridge.
 * @f: the window on top, or NULL.
 *
 * Called from e_d_quit before the common cleanup, on any start or run
 * failure, and when the editor ends.  Nothing watches the session for the
 * user once this begins: the answers it still owes are given up, and the
 * adapter is ended in the background on the fd loop (e_d_dap_close_begin),
 * so the user has the editor back at once while the adapter answers
 * `disconnect`, ends a program that runs and exits.
 */
static void e_d_dap_quit(FENSTER *f)
{
 e_d_run_forget();
 g_dap_closing = 1;
 e_d_dr_forget();
 if (g_dap)
 {
  e_d_dap_say_ending(f);
  /* The build server that hosts the adapter is closed after it: killing
     it earlier would drop the DAP connection in the middle of Quit. */
  e_d_dap_close_begin(g_dap, g_bsp);
  g_dap = NULL;
  g_bsp = NULL;
 }
 if (g_bsp)
 {
  e_bsp_close(g_bsp);
  g_bsp = NULL;
 }
 g_dap_closing = 0;
 g_dap_first = 0;
 g_dap_run_fd = -1;
 g_dap_stop_file[0] = '\0';
 g_dap_stop_line = -1;
}

/**
 * e_d_dap_end_at_exit - The editor is ending: end the debug adapter too.
 *
 * Called by the editor's end after the user's work is saved, so a logout
 * or a "kill PID" during a debug session leaves no adapter and no program
 * behind (lldb-dap ends a running program only when it is told to).  The
 * adapter's output is no longer shown: the windows are going.  The editor's
 * end then keeps its loop running until e_d_dap_all_closed says every
 * adapter is gone, within E_DAP_CLOSE_MAX_MS.
 */
void e_d_dap_end_at_exit(void)
{
 e_scala_setup_end_at_exit();
 e_d_dap_quit(NULL);
}

/* ---- Compiling for debug (Rust), without holding the editor ----
 *
 * A compile-first language is built to a debuggable binary with the file's
 * configured compiler before its adapter starts (dlv builds Go itself).
 * The compiler runs as a child of the editor's, as a build does: the editor
 * keeps its keys, Messages says it runs, Esc ends it, and the command that
 * asked for the session runs again once it has succeeded.
 */

#define E_D_COMPILE_WHAT           "the compiler"
#define E_D_COMPILE_SHELL          "/bin/sh"
#define E_D_COMPILE_LINE           "Compiling for debug... (Esc cancels)"
#define E_D_COMPILE_CANCELLED_LINE "compile for debug cancelled"
#define E_D_COMPILE_FAILED_LINE    "Compilation failed -- see Messages."
#define E_D_COMPILE_ARGC           3
#define E_D_COMPILE_PATH_MAX       1024
#define E_D_COMPILE_CMD_MAX        (4 * E_D_COMPILE_PATH_MAX)
#define E_D_COMPILE_ERR_LINE_MAX   512
#define E_D_COMPILE_DEFAULT_CC     "rustc"
#define E_D_COMPILE_DEFAULT_FLAGS  "-g"

/* The one compile for debug that may run. */
static struct {
 WpeChild child;
 int running;                          /* started, its end not reported yet */
 int held;                             /* ended behind a dialog */
 int ok;                               /* it ended with exit status 0 */
 int compiled;                         /* the next start of the session uses binary */
 char cmd[E_D_COMPILE_CMD_MAX];
 char src[E_D_COMPILE_PATH_MAX];       /* the file it compiles */
 char binary[E_D_COMPILE_PATH_MAX];
 char errf[E_D_COMPILE_PATH_MAX];      /* where the compiler's errors go */
} e_d_compile;

/**
 * e_d_compile_command - Build the compiler's command line for @f.
 * @f: the source window.
 *
 * The compiler and its flags come from the file's Options entry; the
 * binary is written next to the source, the errors into a file read once
 * the compiler has ended.
 * Return: 0, or -1 when the compiler is not on PATH (said in a box).
 */
static int e_d_compile_command(FENSTER *f)
{
 char stem[E_D_COMPILE_PATH_MAX], m[160];
 const char *cc = E_D_COMPILE_DEFAULT_CC, *flags = E_D_COMPILE_DEFAULT_FLAGS;

 e_check_c_file_w(f);                 /* set e_s_prog to this file's compiler */
 if (e_s_prog.compiler && e_s_prog.compiler[0])
  cc = e_s_prog.compiler;
 if (e_s_prog.comp_str && e_s_prog.comp_str[0])
  flags = e_s_prog.comp_str;
 if (e_test_command(cc))
 {
  snprintf(m, sizeof m, "Compiler '%s' not in PATH.", cc);
  e_error(m, 0, f->fb);
  return(-1);
 }
 snprintf(stem, sizeof stem, "%s", f->datnam);
 WpeStringCutChar(stem, '.');
 snprintf(e_d_compile.binary, sizeof e_d_compile.binary, "%s%s.dbg",
          f->dirct, stem);
 snprintf(e_d_compile.src, sizeof e_d_compile.src, "%s%s", f->dirct,
          f->datnam);
 snprintf(e_d_compile.errf, sizeof e_d_compile.errf, "%s%s.dbgerr",
          f->dirct, stem);
 snprintf(e_d_compile.cmd, sizeof e_d_compile.cmd, "%s %s -o '%s' '%s' 2>'%s'",
          cc, flags, e_d_compile.binary, e_d_compile.src, e_d_compile.errf);
 return(0);
}

/**
 * e_d_compile_is_for - Is @f the file the compile for debug ran for?
 *
 * The session starts on the source window it finds, which need not be the
 * one compiled when the user opened another file meanwhile: that file is
 * then compiled in its turn, never debugged with this file's binary.
 */
static int e_d_compile_is_for(FENSTER *f)
{
 char src[E_D_COMPILE_PATH_MAX];

 if (!f || !f->dirct || !f->datnam)
  return(0);
 snprintf(src, sizeof src, "%s%s", f->dirct, f->datnam);
 return(strcmp(src, e_d_compile.src) == 0);
}

/**
 * e_d_compile_file_open - Is the file the compile ran for still open?
 *
 * One the user closed while it compiled gets no session after it.
 */
static int e_d_compile_file_open(void)
{
 int i;

 for (i = WpeEditor->mxedt; i > 0; i--)
  if (e_d_compile_is_for(WpeEditor->f[i]))
   return(1);
 return(0);
}

/**
 * e_d_compile_show_errors - What the compiler said, one line in Messages
 * each, where build errors always appear.
 * @f: the window on top.
 */
static void e_d_compile_show_errors(FENSTER *f)
{
 char line[E_D_COMPILE_ERR_LINE_MAX];
 size_t l;
 FILE *fp = fopen(e_d_compile.errf, "r");

 while (fp && fgets(line, sizeof line, fp))
 {
  l = strlen(line);
  if (l && line[l - 1] == '\n')
   line[l - 1] = '\0';
  e_d_p_message(line, f, 1);
 }
 if (fp)
  fclose(fp);
}

/**
 * e_d_compile_tell - The compile has ended: start the session the user
 * asked for, or show why it cannot start.
 *
 * A debug session the user started meanwhile is left alone, as after
 * scala-cli setup-ide; and nothing starts for a file closed meanwhile.
 */
static void e_d_compile_tell(void)
{
 FENSTER *top = WpeEditor->f[WpeEditor->mxedt];

 e_d_compile.held = 0;
 if (!e_d_compile.ok)
 {
  e_d_resume_drop();
  e_d_compile_show_errors(top);
  unlink(e_d_compile.errf);
  e_error(E_D_COMPILE_FAILED_LINE, 0, top->fb);
  return;
 }
 unlink(e_d_compile.errf);
 if (e_d_swtch || e_d_start_busy() || !e_d_compile_file_open())
 {
  e_d_resume_drop();
  return;
 }
 e_d_compile.compiled = 1;
 e_d_after_build(top, 0, NULL);
 e_d_compile.compiled = 0;
}

/**
 * e_d_compile_on_ended - The compiler ended: go on now, or once the dialog
 * that is open closes.
 * @c: its process; @status: its wait status; @how: WPE_CHILD_ENDED or
 * WPE_CHILD_ENDED_UNKNOWN; @data: unused.
 */
static void e_d_compile_on_ended(WpeChild *c, int status, int how, void *data)
{
 (void)c;
 (void)data;
 e_d_compile.running = 0;
 e_d_compile.ok = (how == WPE_CHILD_ENDED && WIFEXITED(status) &&
                   WEXITSTATUS(status) == 0);
 e_d_compile.held = 1;
 if (wpe_paint_idle())
  e_d_compile_tell();
 else
  e_d_start_deliver_later();
}

/**
 * e_d_compile_deliver_held - Go on with a compile that ended behind a
 * dialog.  Run by the screen's owner once the dialog has closed.
 */
static void e_d_compile_deliver_held(void)
{
 if (e_d_compile.held && wpe_paint_idle())
  e_d_compile_tell();
}

/**
 * e_d_compile_begin - Start the compiler for @f without waiting for it.
 * @f: the source window.
 * Return: 0, or -1 when it could not be started (said in a box).
 */
static int e_d_compile_begin(FENSTER *f)
{
 char *argv[E_D_COMPILE_ARGC + 1];
 WpeChildSpec spec;

 if (e_d_compile_command(f) < 0)
  return(-1);
 argv[0] = E_D_COMPILE_SHELL;
 argv[1] = "-c";
 argv[2] = e_d_compile.cmd;
 argv[3] = NULL;
 memset(&spec, 0, sizeof spec);
 spec.argv = argv;
 spec.io = WPE_CHILD_IO_PIPES;
 spec.group = WPE_CHILD_OWN_SESSION;
 spec.stdin_null = 1;
 spec.stdout_to_null = 1;
 spec.stderr_to_null = 1;
 spec.what = E_D_COMPILE_WHAT;
 e_d_compile.held = 0;
 if (wpe_child_start(&spec, &e_d_compile.child, e_d_compile_on_ended,
                     NULL) != 0)
 {
  e_error(E_D_COMPILE_FAILED_LINE, 0, f->fb);
  return(-1);
 }
 e_d_compile.running = 1;
 e_d_p_message(E_D_COMPILE_LINE, f, 1);
 return(0);
}

/**
 * e_d_dap_compile_first - The binary a compile-first language's session
 * debugs: compiled now, in the background, or just compiled.
 * @f: the source window; @binary, @binsz: receives the binary once it is
 * there.
 * Return: 0 with @binary set, E_D_BUILDING while the compiler runs (the
 * command runs again when it has succeeded), or -1 (said in a box).
 */
static int e_d_dap_compile_first(FENSTER *f, char *binary, size_t binsz)
{
 if (e_d_compile.compiled && e_d_compile_is_for(f))
 {
  snprintf(binary, binsz, "%s", e_d_compile.binary);
  return(0);
 }
 if (e_d_compile.running)
 {
  e_status_say(f, E_D_COMPILE_LINE);
  return(E_D_BUILDING);
 }
 if (e_d_compile_begin(f) < 0)
  return(-1);
 return(E_D_BUILDING);
}

/**
 * e_d_compile_busy - Is the program being compiled for its debug session?
 * The key loop asks, so Esc cancels it, as its waiting line says.
 */
int e_d_compile_busy(void)
{
 return(e_d_compile.running);
}

/**
 * e_d_compile_drop - End a compile no longer wanted, in the background;
 * nothing goes on after it.
 */
static void e_d_compile_drop(void)
{
 e_d_compile.held = 0;
 if (!e_d_compile.running)
  return;
 e_d_compile.running = 0;
 wpe_child_end_detached(&e_d_compile.child, SIGTERM);
 unlink(e_d_compile.errf);
}

/**
 * e_d_compile_cancel - Esc while the program compiles for its debug
 * session: the compiler is ended, no session starts behind the user's
 * back later, and Messages and the status row say so.
 */
void e_d_compile_cancel(void)
{
 FENSTER *top = WpeEditor->f[WpeEditor->mxedt];

 e_d_resume_drop();
 e_d_compile_drop();
 e_d_p_message(E_D_COMPILE_CANCELLED_LINE, top, 0);
 e_status_say(top, E_D_COMPILE_CANCELLED_LINE);
}

/**
 * e_d_compile_end_at_exit - The editor is ending: a compiler still running
 * is ended on the owner's ladder, which the editor's end waits for.
 */
static void e_d_compile_end_at_exit(void)
{
 e_d_compile_drop();
}

/* ---- scala-cli setup-ide, without holding the editor ----
 *
 * `scala-cli setup-ide` writes the files Metals and the Scala debugger need
 * to reach the build server; on a cold cache it takes tens of seconds.  It
 * runs as a child of the editor's: the editor keeps its keys, Messages says
 * it runs, and what was asked for (the language server, the debug session)
 * goes on when it has ended, outside any dialog.
 */

#define E_SCALA_SETUP_PROGRAM "scala-cli"
#define E_SCALA_SETUP_VERB    "setup-ide"
#define E_SCALA_SETUP_WHAT    "scala-cli setup-ide"
#define E_SCALA_SETUP_DIR_MAX 1024
#define E_SCALA_SETUP_ARGC    3
/* The waiting line, for the language server and the debugger alike. */
#define E_SCALA_SETUP_LINE \
 "Preparing the Scala build (scala-cli setup-ide); Esc cancels it..."
#define E_SCALA_SETUP_CANCELLED_LINE "scala-cli setup-ide cancelled"

/* What a setup that has ended goes on with: @ok when it succeeded; @dir the
   project it ran in. */
typedef void (*e_scala_setup_done_fn)(int ok, const char *dir);

typedef struct
{
 WpeChild child;
 int running;                       /* started, its end not reported yet */
 int held;                          /* ended behind a dialog: not told yet */
 int ok;                            /* how it ended, while held */
 char dir[E_SCALA_SETUP_DIR_MAX];   /* the project, also its argument */
 e_scala_setup_done_fn done;
} E_SCALA_SETUP;

/* One for the language server, one for the debugger: either may run while
   the other is asked for. */
static E_SCALA_SETUP e_scala_setup_lsp, e_scala_setup_dap;

/**
 * e_scala_setup_succeeded - Did the setup command end with exit status 0?
 * @status: its wait status; @how: how the owner reported its end.
 */
static int e_scala_setup_succeeded(int status, int how)
{
 return(how == WPE_CHILD_ENDED && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0);
}

/**
 * e_scala_setup_tell - Go on with what the setup was run for.
 * @su: the setup, ended.
 */
static void e_scala_setup_tell(E_SCALA_SETUP *su)
{
 char dir[E_SCALA_SETUP_DIR_MAX];

 su->held = 0;
 snprintf(dir, sizeof dir, "%s", su->dir);
 su->done(su->ok, dir);
}

/**
 * e_scala_setup_on_ended - The setup command ended: go on now, or once the
 * dialog that is open closes.
 * @c: its process; @status: its wait status; @how: WPE_CHILD_ENDED or
 * WPE_CHILD_ENDED_UNKNOWN; @data: the setup.
 */
static void e_scala_setup_on_ended(WpeChild *c, int status, int how,
                                   void *data)
{
 E_SCALA_SETUP *su = data;

 (void)c;
 su->running = 0;
 su->ok = e_scala_setup_succeeded(status, how);
 su->held = 1;
 if (wpe_paint_idle())
  e_scala_setup_tell(su);
 else
  e_d_start_deliver_later();
}

/**
 * e_scala_setup_deliver_held - Go on with a setup that ended behind a
 * dialog.  Run by the screen's owner once the dialog has closed.
 */
static void e_scala_setup_deliver_held(void)
{
 if (!wpe_paint_idle())
  return;
 if (e_scala_setup_lsp.held)
  e_scala_setup_tell(&e_scala_setup_lsp);
 if (e_scala_setup_dap.held)
  e_scala_setup_tell(&e_scala_setup_dap);
}

/**
 * e_scala_setup_begin - Run `scala-cli setup-ide @dir` without waiting.
 * @su: the setup; @dir: the project; @done: what goes on once it has ended.
 *
 * It reads nothing and its output goes nowhere, as before: only the files
 * it writes count.
 * Return: 0, or -1 when it could not be started (the owner's line says why
 * when no slot was free).
 */
static int e_scala_setup_begin(E_SCALA_SETUP *su, const char *dir,
                               e_scala_setup_done_fn done)
{
 char *argv[E_SCALA_SETUP_ARGC + 1];
 WpeChildSpec spec;

 snprintf(su->dir, sizeof su->dir, "%s", dir);
 argv[0] = E_SCALA_SETUP_PROGRAM;
 argv[1] = E_SCALA_SETUP_VERB;
 argv[2] = su->dir;
 argv[3] = NULL;
 memset(&spec, 0, sizeof spec);
 spec.argv = argv;
 spec.io = WPE_CHILD_IO_PIPES;
 spec.group = WPE_CHILD_OWN_SESSION;
 spec.stdin_null = 1;
 spec.stdout_to_null = 1;
 spec.stderr_to_null = 1;
 spec.what = E_SCALA_SETUP_WHAT;
 su->done = done;
 su->held = 0;
 if (wpe_child_start(&spec, &su->child, e_scala_setup_on_ended, su) != 0)
  return(-1);
 su->running = 1;
 return(0);
}

/**
 * e_scala_setup_busy - Is the setup still running, for @dir when given?
 * @su: the setup; @dir: a project, or NULL for any.
 */
static int e_scala_setup_busy(const E_SCALA_SETUP *su, const char *dir)
{
 if (!su->running)
  return(0);
 return(!dir || !strcmp(su->dir, dir));
}

/**
 * e_scala_setup_drop - The setup is no longer wanted (another file, the
 * editor's end): it is ended in the background and nothing goes on after.
 * @su: the setup.
 */
static void e_scala_setup_drop(E_SCALA_SETUP *su)
{
 su->held = 0;
 if (!su->running)
  return;
 su->running = 0;
 wpe_child_end_detached(&su->child, SIGTERM);
}

/**
 * e_scala_setup_end_at_exit - The editor is ending: a setup still running
 * is ended on the owner's ladder, which the editor's end waits for, so none
 * outlives the editor.
 */
static void e_scala_setup_end_at_exit(void)
{
 e_scala_setup_drop(&e_scala_setup_lsp);
 e_scala_setup_drop(&e_scala_setup_dap);
}

/**
 * e_d_dap_spawn - Start the adapter process of a language whose adapter is
 * the editor's own child (dlv, gdb, lldb-dap), with its end watched.
 * @adapter: its command line; @lang: the language; @program, @dir: what it
 * debugs and where.
 *
 * The adapter is started by the owner of the editor's children, which
 * follows its end from before the fork: Quit ends it in the background and
 * learns of its exit the moment it happens, instead of waiting for it, and
 * an adapter killed from outside is told as such when its session ends.
 * Return: the session, or NULL when the adapter could not be started.
 */
static e_dap_session *e_d_dap_spawn(char *const *adapter,
                                    const e_d_dap_lang *lang,
                                    const char *program, const char *dir)
{
 e_dap_session *s;

 if (lang->stdio)
  s = e_dap_start_stdio(adapter, program, lang->entry_func, dir, &g_dap_host);
 else
  s = e_dap_start(adapter, program, lang->entry_func, dir, &g_dap_host);
 e_dap_on_adapter_end(s, e_d_dap_adapter_ended, NULL);
 return(s);
}

/* ---- the Scala build server's start, on the loop ----
 *
 * A Scala session's adapter lives in the build server, reached through five
 * requests (initialize, the targets, a compile, the main classes, the debug
 * session).  On a cold project the compile takes tens of seconds; the editor
 * keeps its keys meanwhile, as it does while a program is compiled for its
 * debug session: the requests run on the loop, the status row says which
 * one is waited for after a quiet second, Esc ends the build server and
 * starts nothing, and the command that asked for the session (Run, Step, Go
 * to cursor) runs again once the endpoint is known.
 */

#define E_D_BSP_LINE           "Starting Scala build server (BSP)..."
#define E_D_BSP_CANCELLED_LINE "Scala build server start cancelled."
#define E_D_BSP_FAILED_LINE    "Could not start the Scala debug server."
#define E_D_BSP_NAME           "the build server"
#define E_D_BSP_WAIT_FMT \
 " waiting for %s to answer `%s` ... %lds  (Esc cancels)"
#define E_D_BSP_NO_TIMER       (-1)
#define E_D_BSP_NO_FD          (-1)
#define E_D_BSP_ERR_MAX        256

static long e_d_ga_now_ms(void *io);   /* the editor's clock (below) */

/* A waiting line asked for while the screen was not the editor's: what the
   editor waits for and how long it had waited then, so the line drawn
   later says the seconds of the moment it is drawn. */
typedef struct
{
 int  due;                   /* a line waits for the screen              */
 char cmd[E_D_REQ_CMD_MAX];  /* what is waited for                       */
 long secs;                  /* the seconds it had waited, when asked    */
 long asked_ms;              /* the clock then                           */
} e_d_wait_line;

/* What the screen's owner calls a waiting line kept for later. */
#define E_D_WAIT_LINE_WHAT "a waiting line"

/**
 * e_d_wait_line_keep - Keep a waiting line for later when the screen is
 * not the editor's.
 * @w: where it is kept; @cmd, @secs: what is waited for and since when;
 * @show: says the line again, from @w; it is run by the screen's owner.
 *
 * While a box, a menu, the User Screen or a command the editor waits for
 * has the screen, "waiting for gdb to answer ... 12s" is not drawn over
 * it.  Once the screen is the editor's again the line is said once more,
 * with the seconds counted on to that moment; an answer that came
 * meanwhile cleared it (e_d_wait_line_forget) and nothing is drawn.
 * Return: 1 when kept (the caller draws nothing now), 0 when the caller
 * draws the line now.
 */
static int e_d_wait_line_keep(e_d_wait_line *w, const char *cmd, long secs,
                              WpePaintReplay show)
{
 if (wpe_paint_idle())
 {
  w->due = 0;
  return(0);
 }
 w->due = 1;
 snprintf(w->cmd, sizeof w->cmd, "%s", cmd ? cmd : "");
 w->secs = secs;
 w->asked_ms = e_d_ga_now_ms(NULL);
 wpe_paint_request(show, w, E_D_WAIT_LINE_WHAT);
 return(1);
}

/**
 * e_d_wait_line_secs_now - The seconds a kept waiting line says now.
 * @w: the kept line.
 */
static long e_d_wait_line_secs_now(const e_d_wait_line *w)
{
 long since = e_d_ga_now_ms(NULL) - w->asked_ms;

 if (since < 0)
  since = 0;
 return(w->secs + since / WPE_TIMER_MS_PER_S);
}

/**
 * e_d_wait_line_take - Take a kept waiting line for drawing.
 * @w: the kept line.
 * Return: 1 when a line is still due (it is no longer kept), 0 when the
 * answer came meanwhile.
 */
static int e_d_wait_line_take(e_d_wait_line *w)
{
 if (!w->due)
  return(0);
 w->due = 0;
 return(1);
}

/** e_d_wait_line_forget - The answer came: a kept waiting line is not drawn. */
static void e_d_wait_line_forget(e_d_wait_line *w)
{
 w->due = 0;
}

static struct
{
 e_bsp_session *b;           /* the start, until the session takes it    */
 E_D_REQ_IO io;              /* the clock, the timers, the waiting line  */
 int timer[E_D_TIMERS];      /* the loop's timer ids, E_D_BSP_NO_TIMER   */
 int fd;                     /* the descriptor the loop reads            */
 int held;                   /* it ended; the owner is not told yet      */
 int ok;                     /* ... with the endpoint                    */
 int ready;                  /* the endpoint waits for the command again */
 int waiting_painted;        /* the waiting line is on the status row    */
 e_d_wait_line kept;         /* a waiting line asked for under a box     */
} e_d_bsp = { NULL, { 0 }, { E_D_BSP_NO_TIMER, E_D_BSP_NO_TIMER,
                             E_D_BSP_NO_TIMER }, E_D_BSP_NO_FD, 0, 0, 0, 0,
              { 0 } };

/** e_d_bsp_window - The window the status row is drawn for. */
static FENSTER *e_d_bsp_window(void)
{
 return(WpeEditor->f[WpeEditor->mxedt]);
}

/**
 * e_d_bsp_waiting - "waiting for the build server to answer
 * `buildTarget/compile` ... 12s (Esc cancels)" on the status row.
 * @cmd, @secs: what is waited for and since when; @io: unused.
 */
static void e_d_bsp_waiting_show_kept(void *data);   /* (below) */

static void e_d_bsp_waiting(const char *cmd, long secs, void *io)
{
 char line[E_D_BSP_ERR_MAX + E_D_REQ_CMD_MAX];

 (void)io;
 if (e_d_wait_line_keep(&e_d_bsp.kept, cmd, secs, e_d_bsp_waiting_show_kept))
  return;                  /* a box or a command has the screen */
 snprintf(line, sizeof line, E_D_BSP_WAIT_FMT, E_D_BSP_NAME, cmd, secs);
 e_status_say(e_d_bsp_window(), line);
 e_d_bsp.waiting_painted = 1;
}

/**
 * e_d_bsp_waiting_show_kept - Say the build server's waiting line kept
 * while the screen was not the editor's, with the seconds of now.
 * @data: the kept line.
 */
static void e_d_bsp_waiting_show_kept(void *data)
{
 e_d_wait_line *w = data;

 if (e_d_wait_line_take(w))
  e_d_bsp_waiting(w->cmd, e_d_wait_line_secs_now(w), NULL);
}

/** e_d_bsp_waiting_clear - The wait is over: the key hints come back. */
static void e_d_bsp_waiting_clear(void)
{
 FENSTER *f = e_d_bsp_window();

 e_d_wait_line_forget(&e_d_bsp.kept);
 if (!e_d_bsp.waiting_painted)
  return;
 e_d_bsp.waiting_painted = 0;
 if (!f || !f->fb || wpe_fd_waiting_for())
  return;
 e_pr_uul(f->fb);
 e_refresh();
}

/** e_d_bsp_notice - Why a request was given up, until the next key. */
static void e_d_bsp_notice(const char *msg, void *io)
{
 char line[E_D_BSP_ERR_MAX + E_D_REQ_MSG_MAX];

 (void)io;
 e_d_bsp.waiting_painted = 0;
 e_d_wait_line_forget(&e_d_bsp.kept);
 snprintf(line, sizeof line, " %s", msg);
 if (e_status_keep_until_free(line))
  return;                  /* drawn when the screen is the editor's */
 e_status_say_until_key(e_d_bsp_window(), line);
}

/** e_d_bsp_disarm - Stop timer @which, if it runs. */
static void e_d_bsp_disarm(int which)
{
 if (e_d_bsp.timer[which] == E_D_BSP_NO_TIMER)
  return;
 wpe_timer_del(e_d_bsp.timer[which]);
 e_d_bsp.timer[which] = E_D_BSP_NO_TIMER;
}

static void e_d_bsp_after(void);   /* (below) */

/**
 * e_d_bsp_on_timer - A timer the start asked for has gone off.
 * @data: which one (E_D_TIMER_*).
 */
static void e_d_bsp_on_timer(void *data)
{
 int which = (int)(intptr_t)data;

 e_d_bsp.timer[which] = E_D_BSP_NO_TIMER;
 if (!e_d_bsp.b)
  return;
 e_bsp_request_timer(e_d_bsp.b, which);
 e_d_bsp_after();
}

/**
 * e_d_bsp_arm_timer - Ask the loop to call the start back.
 * @which: E_D_TIMER_*; @ms: the delay, or E_D_TIMER_OFF; @io: unused.
 */
static void e_d_bsp_arm_timer(int which, long ms, void *io)
{
 (void)io;
 e_d_bsp_disarm(which);
 if (ms == E_D_TIMER_OFF)
  return;
 e_d_bsp.timer[which] = wpe_timer_add(ms, 0, e_d_bsp_on_timer,
                                      (void *)(intptr_t)which);
 if (e_d_bsp.timer[which] <= 0)
  e_d_bsp.timer[which] = E_D_BSP_NO_TIMER;
}

/**
 * e_d_bsp_on_readable - The build server wrote: its answer sends the next
 * request, or ends the start.
 */
static void e_d_bsp_on_readable(int fd, void *data)
{
 (void)fd;
 (void)data;
 if (!e_d_bsp.b)
  return;
 e_bsp_read_now(e_d_bsp.b);
 e_d_bsp_after();
}

/**
 * e_d_bsp_unwatch - The loop no longer reads the build server for the
 * start: it ended, or the session has taken it.
 */
static void e_d_bsp_unwatch(void)
{
 int i;

 if (e_d_bsp.fd != E_D_BSP_NO_FD)
  wpe_fd_del(e_d_bsp.fd);
 e_d_bsp.fd = E_D_BSP_NO_FD;
 for (i = 0; i < E_D_TIMERS; i++)
  e_d_bsp_disarm(i);
}

/**
 * e_d_bsp_drop - End a start no longer wanted (cancelled, failed, or left
 * over): the launcher is ended through the owner, without waiting.
 */
static void e_d_bsp_drop(void)
{
 e_bsp_session *b = e_d_bsp.b;

 e_d_bsp_unwatch();
 e_d_bsp.b = NULL;
 e_d_bsp.held = 0;
 e_d_bsp.ready = 0;
 e_d_bsp_waiting_clear();
 if (b)
  e_bsp_close(b);
}

/**
 * e_d_bsp_done - The start ended (called from inside the start's own read
 * or timer): only kept, and told once the read has returned.
 * @ok: the endpoint is known; @ctx: unused.
 */
static void e_d_bsp_done(int ok, void *ctx)
{
 (void)ctx;
 e_d_bsp.held = 1;
 e_d_bsp.ok = ok;
}

/**
 * e_d_bsp_fail - The start failed: the box says why, and nothing runs.
 */
static void e_d_bsp_fail(void)
{
 char err[E_D_BSP_ERR_MAX];
 const char *why = e_bsp_error(e_d_bsp.b);

 snprintf(err, sizeof err, "%s", (why && *why) ? why : E_D_BSP_FAILED_LINE);
 e_d_bsp_drop();
 e_d_resume_drop();
 e_error(err, 0, e_d_bsp_window()->fb);
}

/**
 * e_d_bsp_tell - The start ended: on success the command that asked for the
 * session runs again and takes the endpoint; a failure says why.
 *
 * A debug session the user started meanwhile, in another file, is left
 * alone, as when setup-ide ends.
 */
static void e_d_bsp_tell(void)
{
 e_d_bsp.held = 0;
 e_d_bsp_unwatch();
 e_d_bsp_waiting_clear();
 if (!e_d_bsp.ok)
 {
  e_d_bsp_fail();
  return;
 }
 if (e_d_swtch || e_d_start_busy())
 {
  e_d_bsp_drop();
  e_d_resume_drop();
  return;
 }
 e_d_bsp.ready = 1;
 e_d_after_build(e_d_bsp_window(), 0, NULL);
 if (e_d_bsp.ready)
  e_d_bsp_drop();          /* no command took it: nothing to debug */
}

/**
 * e_d_bsp_after - After the start's read or timer: tell its end, now or
 * once the dialog that is open closes.
 *
 * Once the start has ended nothing more is read for it, so the loop stops
 * watching the build server at once: a launcher that died would otherwise
 * stay readable at its end of file and keep the editor spinning for as long
 * as the dialog stays open.
 */
static void e_d_bsp_after(void)
{
 if (!e_d_bsp.held)
  return;
 e_d_bsp_unwatch();
 if (wpe_paint_idle())
  e_d_bsp_tell();
 else
  e_d_start_deliver_later();
}

/**
 * e_d_bsp_deliver_held - Tell a start that ended behind a dialog.  Run by
 * the screen's owner once the dialog has closed.
 */
static void e_d_bsp_deliver_held(void)
{
 e_d_bsp_after();
}

/**
 * e_d_bsp_begin - Start the build server, without waiting for its answers.
 * @f: the source window; @dir: the project.
 * Return: E_D_BUILDING (the command runs again once the endpoint is
 * known), or -1 (said in a box).
 */
static int e_d_bsp_begin(FENSTER *f, const char *dir)
{
 char err[E_D_BSP_ERR_MAX];
 int i, fd;

 memset(&e_d_bsp.io, 0, sizeof e_d_bsp.io);
 e_d_bsp.io.now_ms = e_d_ga_now_ms;
 e_d_bsp.io.arm_timer = e_d_bsp_arm_timer;
 e_d_bsp.io.waiting = e_d_bsp_waiting;
 e_d_bsp.io.notice = e_d_bsp_notice;
 e_d_bsp.io.name = E_D_BSP_NAME;
 for (i = 0; i < E_D_TIMERS; i++)
  e_d_bsp.timer[i] = E_D_BSP_NO_TIMER;
 e_d_bsp.held = e_d_bsp.ready = e_d_bsp.ok = 0;
 e_d_bsp.b = e_bsp_start_async(dir, &e_d_bsp.io, e_d_bsp_done, NULL,
                               err, sizeof err);
 if (!e_d_bsp.b)
 {
  e_d_resume_drop();
  e_error(err[0] ? err : E_D_BSP_FAILED_LINE, 0, f->fb);
  return(-1);
 }
 fd = e_bsp_fd(e_d_bsp.b);
 if (e_fd_watch(fd, POLLIN, e_d_bsp_on_readable, NULL,
                "the build server's answers") != 0)
 {
  e_d_bsp_drop();
  e_d_resume_drop();
  return(-1);
 }
 e_d_bsp.fd = fd;
 e_d_p_message(E_D_BSP_LINE, f, 1);
 return(E_D_BUILDING);
}

/**
 * e_d_bsp_first - The DAP endpoint a Scala session connects to: known now,
 * asked of the build server in the background, or being asked.
 * @f: the source window; @dir: the project; @host, @hostsz, @port: receive
 * the endpoint once it is known.
 * Return: 0 with the endpoint set and g_bsp holding the build server,
 * E_D_BUILDING while it is asked, or -1 (said in a box).
 */
static int e_d_bsp_first(FENSTER *f, const char *dir, char *host,
                         size_t hostsz, int *port)
{
 const char *h;

 if (e_d_bsp.ready && e_d_bsp.b)
 {
  h = e_bsp_endpoint(e_d_bsp.b, port);
  snprintf(host, hostsz, "%s", h ? h : "");
  g_bsp = e_d_bsp.b;          /* the session keeps it open from now on */
  e_d_bsp.b = NULL;
  e_d_bsp.ready = 0;
  return(0);
 }
 if (e_d_bsp.b)
 {
  e_status_say(f, E_D_BSP_LINE);
  return(E_D_BUILDING);
 }
 return(e_d_bsp_begin(f, dir));
}

/**
 * e_d_bsp_busy - Is the build server answering the start's requests?  The
 * key loop asks, so Esc cancels it, as its waiting line says.
 */
int e_d_bsp_busy(void)
{
 return(e_d_bsp.b && e_bsp_starting(e_d_bsp.b));
}

/**
 * e_d_bsp_cancel - Esc while the build server answers the start: it is
 * ended, no session starts behind the user's back later, and Messages and
 * the status row say so.
 */
void e_d_bsp_cancel(void)
{
 FENSTER *top = e_d_bsp_window();

 e_d_bsp_drop();
 e_d_resume_drop();
 e_d_p_message(E_D_BSP_CANCELLED_LINE, top, 0);
 e_status_say(top, E_D_BSP_CANCELLED_LINE);
}

/* Open the adapter for f's language, install the user's pre-set breakpoints, and
   run to the first stop.  Leaves e_d_swtch=3 and e_deb_type=DEB_DAP so the other
   hooks route here.  Returns 0 on success, -1 on failure (popup already shown). */
static int e_d_dap_start(FENSTER *f)
{
 const e_d_dap_lang *lang = e_d_dap_lang_for(f);
 char dir[1024], binary[1024];
 char *const *adapter;
 const char *program;
 int i, dlen, compiled;

 if (g_dap)
  return(0);
 if (!lang)
  return(-1);
 adapter = e_d_dap_choose_argv(lang);   /* gdb vs lldb-dap etc. */
 if (e_test_command(adapter[0]))
 {
  char m[160];
  sprintf(m, "Debug adapter '%s' not in PATH.", adapter[0]);
  e_error(m, 0, f->fb);
  return(-1);
 }
 g_dap_host.on_stopped = e_d_dap_on_stopped;
 g_dap_host.on_output = e_d_dap_on_output;
 g_dap_host.on_breakpoints = e_d_dap_on_breakpoints;
 /* The end is not said from inside the adapter's event: the run or step
    that reached it ends the session through e_d_dap_end_session. */
 g_dap_host.on_terminated = NULL;
 g_dap_host.ud = NULL;
 g_dap_first = 0;
 e_d_prog_output_reset();             /* a fresh User Screen for this session */
 g_dap_shown = 0;
 /* Program directory without the trailing slash: the adapter's working dir, and
    the root that tells the user's own code from library/runtime stops.  (dlv's
    package resolution also rejects a trailing "DIR/".) */
 dlen = snprintf(dir, sizeof(dir), "%s", f->dirct ? f->dirct : ".");
 while (dlen > 1 && dir[dlen - 1] == DIRC)
  dir[--dlen] = '\0';
 strncpy(g_dap_progdir, dir, sizeof(g_dap_progdir) - 1);
 g_dap_progdir[sizeof(g_dap_progdir) - 1] = '\0';
 g_dap_stop_file[0] = '\0';
 g_dap_stop_line = -1;
 g_dap_stop_reason[0] = '\0';
 g_dap_entry_func = (lang->entry_func != NULL);
 /* Compile-first (Rust): build the binary now and debug it.  Adapter-builds
    (Go): the directory itself is the "program" dlv compiles. */
 if (lang->compile)
 {
  compiled = e_d_dap_compile_first(f, binary, sizeof(binary));
  if (compiled != 0)
   return(compiled);
  program = binary;
 }
 else
  program = dir;
 if (lang->bsp)
 {
  /* JVM/Scala: drive the BSP bootstrap (Bloop via scala-cli) to get a DAP
     endpoint, keep the build server alive in g_bsp, then connect the engine. */
  char bhost[64];
  int bport = 0, started;
  /* scala-cli launches Bloop from JAVA_HOME/PATH to COMPILE the debuggee, so the
     too-new-JDK crash hits debug too -- pin it here, before the BSP spawns. */
  e_scala_pin_jdk(f, "Scala debug");
  if (e_d_scala_setup_first(f, dir))
   return(E_D_BUILDING);
  started = e_d_bsp_first(f, dir, bhost, sizeof(bhost), &bport);
  if (started != 0)
   return(started);          /* asked in the background, or said in a box */
  g_dap = e_dap_start_tcp(bhost, bport, program, lang->entry_func, &g_dap_host);
 }
 else
  g_dap = e_d_dap_spawn(adapter, lang, program, dir);
 if (!g_dap)
 {
  e_error(E_D_DAP_START_FAILED_LINE, 0, f->fb);
  if (g_bsp)
  {  e_bsp_close(g_bsp);  g_bsp = NULL;  }
  return(-1);
 }
 strncpy(e_d_file, f->datnam, sizeof(e_d_file) - 1);
 e_d_file[sizeof(e_d_file) - 1] = '\0';
 /* Install breakpoints the user toggled before Run.  e_make_breakpoint already
    recorded them as (file, line) in the e_d_*brpts arrays; translate each to a
    full path the adapter can resolve. */
 for (i = 0; i < e_d_nbrpts; i++)
 {
  char *full = e_mkfilename(f->dirct, e_d_sbrpts[i]);
  e_dap_add_breakpoint(g_dap, full ? full : e_d_sbrpts[i], e_d_ybrpts[i]);
  if (full)
   FREE(full);
 }
 e_deb_type = DEB_DAP;
 snprintf(g_dap_adapter_name, sizeof(g_dap_adapter_name), "%s", adapter[0]);
 e_d_dr_begin();
 return(e_d_dap_start_watch(f));      /* the handshake runs on the fd loop */
}

/**
 * e_d_dap_start_finish - The adapter has answered the whole handshake: the
 * program runs on to its first stop.
 * @f: the window on top.
 *
 * Nothing waits for that stop, however far away it is.  The command that
 * asked for the session runs again right after this and takes the run over
 * (see e_d_dap_take_first_run): the first Run or Step shows the stop, as
 * it always has.
 * Return: 0.
 */
static int e_d_dap_start_finish(FENSTER *f)
{
 (void)f;
 e_d_swtch = E_D_DAP_SESSION_OPEN;
 g_dap_first = 1;
 g_dap_run_fd = e_dap_fd(g_dap);
 return(0);
}

/**
 * e_d_dap_path_in_user_code - Is @path a file of the user's own program?
 * @path: a source path the adapter named.
 *
 * Tells the user's frames from the language runtime's (Go's runtime under
 * GOROOT, Rust's std): stepping never wanders into the runtime, and Finish
 * function knows main has no caller of the user's to return to.  Unknown
 * paths count as the user's, so nothing is skipped when it cannot be told.
 * Return: 1 for the user's code.
 */
static int e_d_dap_path_in_user_code(const char *path)
{
 size_t n;

 if (!g_dap_progdir[0] || !path || !path[0])
  return(1);
 n = strlen(g_dap_progdir);
 return(!strncmp(path, g_dap_progdir, n) &&
        (path[n] == DIRC || path[n] == '\0'));
}

/* True when the current stop is inside the user's program source tree.  After
   main() returns, a "step over" steps UP into the Go runtime (runtime/proc.go
   under GOROOT) -- legitimately, but it drops the user into unfamiliar code that
   looks like the debugger is jumping around at random.  This lets the step verbs
   decide to run on rather than show runtime internals.  Unknown paths default to
   "user code" so we never skip when we cannot tell. */
static int e_d_dap_in_user_code(void)
{
 return(e_d_dap_path_in_user_code(g_dap_stop_file));
}

/**
 * e_d_dap_stopped_at_entry - Is the current stop the program's start, rather
 * than a stop the user asked for?
 *
 * Told by the reason the adapter gives, never by the stop being the first:
 * Rust and Scala have no entry function, so their first stop is already the
 * user's breakpoint, and continuing it would run past a line that executes
 * only once.
 * Return: 1 for the adapter's entry stop or the editor's own entry-function
 * breakpoint, 0 for a breakpoint, a step or anything else.
 */
static int e_d_dap_stopped_at_entry(void)
{
 if (!strcmp(g_dap_stop_reason, E_D_DAP_REASON_ENTRY))
  return(1);
 if (g_dap_entry_func && !strcmp(g_dap_stop_reason, E_D_DAP_REASON_FUNCTION))
  return(1);
 return(0);
}

/* What a debug adapter's run does when the program stops: the command that
   began it, E_D_DAP_ON_STOP_*. */
#define E_D_DAP_ON_STOP_CONTINUE 0   /* Run: go past the start's entry stop
                                        when breakpoints wait further on */
#define E_D_DAP_ON_STOP_INTO     1   /* F7: show the stop */
#define E_D_DAP_ON_STOP_OVER     2   /* F8: show the stop, past the runtime */
#define E_D_DAP_ON_STOP_OUT      3   /* Finish function: show the stop */
#define E_D_DAP_ON_STOP_SHOW     4   /* a first command that needs a stopped
                                        program: show the start's stop */
/* F8 runs on at most this many runtime stops before showing one. */
#define E_D_DAP_RUNTIME_STEPS_MAX 100

static struct {
 int kind;                 /* E_D_DAP_ON_STOP_*                          */
 int first;                /* the run is the start's: its stop is the
                              program's start                          */
 int runtime_steps;        /* runtime stops F8 has run past so far       */
} g_dap_run;

/**
 * e_d_dap_answer_fd - The adapter's descriptor, as the run watches it.
 *
 * Kept from the start of the run: after the adapter hangs up the engine
 * no longer names it, but the loop still has it to unwatch.
 */
static int e_d_dap_answer_fd(void)
{
 return(g_dap_run_fd);
}

/**
 * e_d_dap_output_fd - dlv's pty, where the program and the build write; -1
 * for an adapter that carries the program's output in its messages.
 */
static int e_d_dap_output_fd(void)
{
 if (!g_dap)
  return(-1);
 return(e_dap_output_fd(g_dap));
}

/**
 * e_d_dap_begin - A run was handed to the controller: note the descriptor
 * it will watch.
 */
static void e_d_dap_begin(void)
{
 g_dap_run_fd = e_dap_fd(g_dap);
}

/**
 * e_d_dap_on_program_output - dlv's pty has output while the program runs.
 * @fd: the pty; @data: unused.
 *
 * Read and passed on, so the program never stalls on a full pty and the
 * editor never wakes for text nobody took.  When the engine has closed the
 * pty (the adapter ended) it is no longer watched.
 */
static void e_d_dap_on_program_output(int fd, void *data)
{
 (void)data;
 if (!g_dap)
 {
  wpe_fd_del(fd);
  return;
 }
 e_dap_drain_output(g_dap);
 if (e_dap_output_fd(g_dap) != fd)
  wpe_fd_del(fd);
}

/**
 * e_d_dap_feed - Read what the adapter sent while the program runs.
 * @f: the window on top.
 *
 * The program's stop, and the end of the program, both end the run for
 * the backend's stopped function to tell; an adapter that went away
 * without the program ending ends the session.  One that went before the
 * program's first stop did not start it: a box says so.
 * Return: E_D_FEED_*.
 */
static int e_d_dap_feed(FENSTER *f)
{
 int st = e_dap_run_pump(g_dap);

 if (st == E_DAP_RUN_STOPPED)
  return(E_D_FEED_STOPPED);
 if (st != E_DAP_RUN_ENDED)
  return(E_D_FEED_GOING);
 if (e_dap_ended(g_dap))
  return(E_D_FEED_STOPPED);
 if (g_dap_run.first)
  e_error(E_D_DAP_RUN_FAILED_LINE, 0, f->fb);
 return(E_D_FEED_GONE);
}

/**
 * e_d_dap_run_on - Let the program run on from a stop it was shown or held
 * at, and keep the run with the controller.
 * @f: the window on top; @verb: the DAP request that resumes it.
 *
 * Used when the stop is not the one the user asked for: the start's entry
 * stop, and the runtime code F8 steps out of.
 */
static void e_d_dap_run_on(FENSTER *f, const char *verb)
{
 if (e_dap_resume(g_dap, verb) != 0)
 {
  e_d_quit(f);
  return;
 }
 e_d_run_resume();
}

/**
 * e_d_dap_runs_on_from_start - Does Run go on from the start's stop?
 *
 * An entry stop is passed when the user has breakpoints, or Go to cursor
 * its own, further on (gdb's temporary breakpoint at main does the same);
 * without them it is the stop the user is shown.  Any other first stop is
 * a breakpoint the user asked for.
 * Return: 1 to continue.
 */
static int e_d_dap_runs_on_from_start(void)
{
 if (g_dap_run.kind != E_D_DAP_ON_STOP_CONTINUE)
  return(0);
 if (!e_d_dap_stopped_at_entry())
  return(0);
 return(e_d_nbrpts > 0 || e_d_cursor_brk_pending());
}

/**
 * e_d_dap_runs_out_of_runtime - Does F8 go on from a stop in the language
 * runtime?
 *
 * Step-over must never wander into the language runtime: once main()
 * returns there is no user line left to step to, so "next" lands in
 * runtime/proc.go under GOROOT.  The program then runs on to the next user
 * line or to its end instead of showing runtime internals.  Step-into (F7)
 * is left alone: the user asked to descend.  The count bounds it, so an
 * adapter that never reaches user code cannot keep it for ever.
 * Return: 1 to continue.
 */
static int e_d_dap_runs_out_of_runtime(void)
{
 if (g_dap_run.kind != E_D_DAP_ON_STOP_OVER)
  return(0);
 if (e_d_dap_in_user_code())
  return(0);
 return(g_dap_run.runtime_steps++ < E_D_DAP_RUNTIME_STEPS_MAX);
}

/* The reason the engine gives the stop that follows Pause, whatever the
   adapter called it (lldb-dap says "exception" for SIGSTOP). */
#define E_D_DAP_REASON_PAUSE "pause"

/**
 * e_d_dap_interrupt - Pause the program an adapter runs: send DAP "pause".
 *
 * The adapter's stop follows through the run's reader and is the user's
 * Pause, whatever reason the adapter gives it.
 * Return: 0 when the request was sent, -1 when the program is not running.
 */
static int e_d_dap_interrupt(void)
{
 if (!g_dap)
  return(-1);
 return(e_dap_pause(g_dap));
}

/**
 * e_d_dap_stop_is_pause - Is the stop that arrived the one Pause asked for?
 * Return: 1 when it is.
 */
static int e_d_dap_stop_is_pause(void)
{
 return(!strcmp(g_dap_stop_reason, E_D_DAP_REASON_PAUSE));
}

/**
 * e_d_dap_stopped_by_pause - The program stopped because the user paused
 * it: mark where their code is.
 * @f: the window on top.
 *
 * The status row says "Paused" at the end of the stop (e_d_pause_report),
 * not the adapter's own word for the stop, and the stop is never run on
 * from as a start's or a runtime's stop is.  The stop's stack moves the
 * mark into the user's code (e_d_dap_pause_pick).
 */
static void e_d_dap_stopped_by_pause(FENSTER *f)
{
 e_d_pause_taken();
 e_d_dap_stop_ask(f, 1);
}

/**
 * e_d_dap_stopped - The program stopped or ended: what the user sees.
 * @f: the window on top.
 *
 * What the program printed meanwhile shows in Messages first.  An end says
 * how the program ended and closes the session.  A stop is marked and the
 * watches filled, unless it is only the start's entry stop or runtime code
 * the command runs on from, which the user is never shown.
 */
static void e_d_dap_stopped(FENSTER *f)
{
 int first = g_dap_run.first;

 g_dap_run.first = 0;
 e_d_dap_show_new_output(1);
 if (e_dap_ended(g_dap))
 {
  e_d_dap_end_session(f);
  return;
 }
 if (e_d_dap_stop_is_pause())
 {
  e_d_dap_stopped_by_pause(f);
  return;
 }
 if (first && e_d_dap_runs_on_from_start())
 {
  e_d_dap_run_on(f, "continue");
  return;
 }
 if (!first && e_d_dap_runs_out_of_runtime())
 {
  e_d_dap_run_on(f, "continue");
  return;
 }
 e_d_dap_stop_ask(f, 0);
}

/* A debug adapter: its messages on one descriptor, the program's output on
   the same or on a pty of the adapter's own, a stop told by a message.
   Typed text goes nowhere yet, so the running line offers only Quit.  The
   adapter takes breakpoint changes while the program runs. */
static const E_D_RUN_OPS e_d_dap_ops =
{
 "debug adapter",
 e_d_dap_answer_fd,
 e_d_dap_output_fd,
 e_d_dap_feed,
 e_d_dap_stopped,
 e_d_dap_interrupt,
 e_d_dap_sync_live,
 e_d_brk_sync_at_stop,
 NULL,
 e_d_dap_begin,
 e_d_dap_on_program_output,
 1
};

/**
 * e_d_dap_run_ops - The run controller's table for the debug adapters.
 */
static const E_D_RUN_OPS *e_d_dap_run_ops(void)
{
 return(&e_d_dap_ops);
}

/**
 * e_d_dap_take_first_run - Hand the program that runs to its first stop to
 * the controller, for the first command of the session.
 * @f: the source window;
 * @kind: what the command does at that stop, E_D_DAP_ON_STOP_*.
 *
 * The adapter was started without waiting for the stop, so the program
 * is on its way to it; the editor keeps its keys until it gets there,
 * however long that is.  The adapter may have written the stop with its
 * last answer to the start, so it is read once at once.
 * Return: 0, or the end of the session.
 */
static int e_d_dap_take_first_run(FENSTER *f, int kind)
{
 int ret;

 g_dap_first = 0;
 g_dap_run.kind = kind;
 g_dap_run.first = 1;
 g_dap_run.runtime_steps = 0;
 ret = e_d_run_hand_over(f, e_d_dap_run_ops(), 0);
 if (e_d_run_going())
  e_d_run_on_answer(g_dap_run_fd, NULL);
 return(ret);
}

/**
 * e_d_dap_resume_run - Let the stopped program go on, and give the editor
 * its keys until it stops again.
 * @f: the source window; @verb: "continue", "next", "stepIn" or "stepOut";
 * @kind: what the command does at the next stop, E_D_DAP_ON_STOP_*.
 * Return: 0, or the end of the session.
 */
static int e_d_dap_resume_run(FENSTER *f, const char *verb, int kind)
{
 if (e_dap_ended(g_dap))
  return(e_d_dap_end_session(f));
 g_dap_run.kind = kind;
 g_dap_run.first = 0;
 g_dap_run.runtime_steps = 0;
 if (e_dap_resume(g_dap, verb) != 0)
  return(e_d_quit(f));
 return(e_d_run_hand_over(f, e_d_dap_run_ops(), 0));
}

/**
 * e_d_dap_run - Ctrl-G R: run the program, or let it go on from its stop.
 * @f: the source window.
 *
 * The very first Run shows the stop the session starts at, or goes past it
 * to the user's breakpoints (see e_d_dap_runs_on_from_start).  The program
 * runs under the controller: the editor keeps its keys until it stops.
 * Return: 0, or the end of the session.
 */
static int e_d_dap_run(FENSTER *f)
{
 if (!g_dap && e_d_dap_start(f) < 0)
  return(-1);
 if (g_dap_first)
  return(e_d_dap_take_first_run(f, E_D_DAP_ON_STOP_CONTINUE));
 e_d_nstack = 0;
 e_d_delbreak(f);
 e_d_switch_out(1);
 return(e_d_dap_resume_run(f, "continue", E_D_DAP_ON_STOP_CONTINUE));
}

/**
 * e_d_dap_step - F8 (@sw nonzero, step over) and F7 (@sw zero, step into).
 * @f: the source window.
 *
 * The first step of a session shows the stop it starts at without
 * advancing, like the text backends; later steps run under the controller.
 * Return: 0, or the end of the session.
 */
static int e_d_dap_step(FENSTER *f, int sw)
{
 if (!g_dap)
  return(-1);
 if (g_dap_first)
  return(e_d_dap_take_first_run(f, E_D_DAP_ON_STOP_SHOW));
 if (sw)
  return(e_d_dap_resume_run(f, "next", E_D_DAP_ON_STOP_OVER));
 return(e_d_dap_resume_run(f, "stepIn", E_D_DAP_ON_STOP_INTO));
}

/**
 * e_d_dap_finish - Finish function under a debug adapter: stepOut.
 * @f: the source window.
 *
 * The program runs until the current function returns, and the mark lands
 * on the caller's line, as gdb's "finish" puts it; a program that ends on
 * the way ends the session with its end line.  As the session's first
 * command there is nothing to finish: it shows the stop it starts at.
 * Return: 0, or the end of the session.
 */
static int e_d_dap_finish(FENSTER *f)
{
 if (g_dap_first)
  return(e_d_dap_take_first_run(f, E_D_DAP_ON_STOP_SHOW));
 return(e_d_dap_resume_run(f, "stepOut", E_D_DAP_ON_STOP_OUT));
}

/* A watch line under an adapter: "name: value", or the words below when
   the adapter gave no value. */
#define E_D_DAP_NO_VALUE "<no value>"
#define E_D_DAP_WATCH_SEP ": "

/**
 * e_d_dap_watch_put - Add the line of watch @l to the Watches buffer with
 * the value the adapter gave it.
 * @b: the Watches buffer; @l: the watch; @val: its value, or NULL when the
 * adapter gave none (the user reads "<no value>").
 */
static void e_d_dap_watch_put(BUFFER *b, int l, const char *val)
{
 char *line;

 if (!val)
  val = E_D_DAP_NO_VALUE;
 line = WpeMalloc(strlen(e_d_swtchs[l]) + strlen(E_D_DAP_WATCH_SEP) +
                  strlen(val) + 1);
 sprintf(line, "%s" E_D_DAP_WATCH_SEP "%s", e_d_swtchs[l], val);
 e_d_nrwtchs[l] = b->mxlines;
 print_to_end_of_buffer(b, line, b->mx.x);
 WpeFree(line);
}

/**
 * e_d_dap_value_at - The value kept for watch @l, or NULL when there is
 * none (no list, a watch added since it was asked, or the adapter gave
 * none).
 * @values, @n: the values, one per watch asked, or NULL and 0.
 */
static const char *e_d_dap_value_at(char **values, int n, int l)
{
 if (!values || l >= n)
  return(NULL);
 return(values[l]);
}

/**
 * e_d_dap_at_stop - Is the adapter's program stopped, so its watches can be
 * asked?
 */
static int e_d_dap_at_stop(void)
{
 return(g_dap && !e_dap_running(g_dap) && !e_dap_ended(g_dap));
}

/**
 * e_d_dap_watches - Show the watches under a debug adapter (Ctrl-G W, an
 * edit or a removal, a frame picked in the Stack window).
 * @f: the window the key was pressed in; @sw: 1 to bring the window up.
 *
 * At a stop the values are asked of the adapter on the fd loop and written
 * when it has answered, the user keeping the editor meanwhile; while the
 * program runs each watch says its value comes at the next stop, and with
 * no session each says it has no value.
 * Return: 0, or -1 when the Watches window could not be opened.
 */
static int e_d_dap_watches(FENSTER *f, int sw)
{
 if (e_d_dr_answering())
  return(0);              /* the answer that is out fills the window */
 if (e_d_nwtchs > 0 && !e_d_watches_wait() && e_d_dap_at_stop())
  return(e_d_dap_watches_ask(f->ed, sw));
 return(e_d_dap_watches_fill(f, NULL, 0, sw));
}

/**
 * e_d_dap_watches_fill - Rebuild the Watches window with the values the
 * adapter gave, and the Stack window from the stop's frames when it is
 * open.
 * @f: a window of the editor; @values, @nvalues: each watch's value, or
 * NULL and 0 for none; @sw: 1 to bring the Watches window to the top.
 *
 * Mirrors e_d_p_watches's window handling with the values already read.
 * Return: 0, or -1 when a window could not be opened.
 */
static int e_d_dap_watches_fill(FENSTER *f, char **values, int nvalues,
                                int sw)
{
 ECNT *cn = f->ed;
 BUFFER *b;
 int iw, l;

 e_d_switch_out(0);
 if (!e_d_watches_wait() && (e_d_swtch > 2) && (e_d_p_stack(f, 0) == -1))
  return(-1);
 for (iw = cn->mxedt; iw > 0 && strcmp(cn->f[iw]->datnam, "Watches"); iw--)
  ;
 if (iw == 0 && !e_d_nwtchs)
 {
  e_rep_win_tree(cn);
  return(0);
 }
 else if (iw == 0)
 {
  if (e_edit(cn, "Watches"))
   return(-1);
  iw = cn->mxedt;
 }
 f = cn->f[iw];
 b = cn->f[iw]->b;
 e_p_red_buffer(b);
 FREE(b->bf[0].s);
 b->mxlines = 0;
 e_d_watches_shown_waiting = 0;
 for (l = 0; l < e_d_nwtchs; l++)
 {
  if (e_d_watches_wait())
   e_d_watch_put_waiting(b, l);
  else
   e_d_dap_watch_put(b, l, e_d_dap_value_at(values, nvalues, l));
 }
 e_new_line(b->mxlines, b);
 fk_cursor(1);
 if (sw && iw != cn->mxedt)
  e_switch_window(cn->edt[iw], cn->f[cn->mxedt]);
 else
  e_rep_win_tree(cn);
 return(0);
}

/* The debugger builds the program first, in the background like F9; the
   command that asked for the debugger (Run, Step, Go to cursor) is kept and
   run again once the build has succeeded. */
typedef int (*e_d_resume_fn)(FENSTER *f, int arg);
static void e_d_ga_set_up(ECNT *cn, e_d_resume_fn pressed, int arg);
static e_d_resume_fn e_d_resume_cmd = NULL;
static int e_d_resume_arg = 0;

/**
 * e_d_resume_drop - What the debugger was asked for will not run after
 * all: the user cancelled what it waited for, or that failed.
 */
static void e_d_resume_drop(void)
{
 e_d_resume_cmd = NULL;
}
static int e_d_build_done = 0;     /* the build for this start has succeeded */

int e_d_goto_func(FENSTER *f, int flag);

/**
 * e_deb_run_again - Run (Ctrl-G R) again once the build has succeeded.
 * @f: the top window; @arg: unused.
 */
static int e_deb_run_again(FENSTER *f, int arg)
{
 (void)arg;
 return(e_deb_run(f));
}

/**
 * e_d_after_build - The debugger's build has ended: on success, run the
 * debugger command the user pressed, as if pressed again now.
 * @f: the top window; @result: the build's result; @data: unused.
 *
 * A failed build has already put the cursor on its first error, and the
 * debugger does not start on a program that did not build.
 */
static void e_d_after_build(FENSTER *f, int result, void *data)
{
 e_d_resume_fn cmd = e_d_resume_cmd;

 (void)data;
 e_d_resume_cmd = NULL;
 if (result || !cmd)
  return;
 e_d_build_done = 1;
 cmd(f->ed->f[f->ed->mxedt], e_d_resume_arg);
 e_d_build_done = 0;
}

/**
 * e_d_build_first - Start the build the debugger needs, and wait for it.
 * @f: the source window.
 * Return: E_D_BUILDING; the debugger starts from e_d_after_build.
 */
static int e_d_build_first(FENSTER *f)
{
 e_p_make_then(f, e_d_after_build, NULL);
 return(E_D_BUILDING);
}

#define E_D_SCALA_SETUP_FAILED_LINE \
 "scala-cli setup-ide failed (is scala-cli installed?)"

/**
 * e_d_scala_setup_done - The debugger's setup ended: start the session the
 * user asked for, as if the key were pressed again now.
 * @ok: it succeeded; @dir: the project.
 *
 * A debug session the user started meanwhile, in another file, is left
 * alone: the Scala one is not started on top of it, and no command meant
 * for it is run in that session.
 */
static void e_d_scala_setup_done(int ok, const char *dir)
{
 FENSTER *top = WpeEditor->f[WpeEditor->mxedt];

 (void)dir;
 if (e_d_swtch || e_d_start_busy())
 {
  e_d_resume_cmd = NULL;
  return;
 }
 if (ok)
 {
  e_d_after_build(top, 0, NULL);
  return;
 }
 e_d_resume_cmd = NULL;
 e_d_p_message(E_D_SCALA_SETUP_FAILED_LINE, top, 1);
 e_status_say(top, E_D_SCALA_SETUP_FAILED_LINE);
}

/**
 * e_d_scala_setup_first - The first Scala debug session in a project: run
 * `scala-cli setup-ide` first, without holding the editor, like the build
 * a C program is debugged after.
 * @f: the source window; @dir: the project.
 * Return: 1 when the session starts once it has ended (or it could not be
 * started, said in a box), 0 when the project needs none.
 */
static int e_d_scala_setup_first(FENSTER *f, const char *dir)
{
 if (!e_bsp_needs_setup(dir))
  return(0);
 if (e_scala_setup_busy(&e_scala_setup_dap, NULL))
 {
  e_status_say(f, E_SCALA_SETUP_LINE);
  return(1);
 }
 if (e_scala_setup_begin(&e_scala_setup_dap, dir, e_d_scala_setup_done) != 0)
 {
  e_d_resume_cmd = NULL;
  e_error(E_D_SCALA_SETUP_FAILED_LINE, 0, f->fb);
  return(1);
 }
 e_d_p_message(E_SCALA_SETUP_LINE, f, 1);
 return(1);
}

/* ---- Starting a pipe debugger (gdb, jdb, pdb, a68g) ---------------------
 *
 * A started debugger answers with a banner and its prompt.  Until it has, the
 * editor never waits for it: the debugger's output is watched on the fd loop,
 * as a build's is, a waiting line in Messages counts the seconds, Esc ends the
 * debugger, and a deadline ends one that never answers.  When the prompt
 * arrives the session goes on from there, and the command the user pressed
 * (Run, Step, Go to cursor) runs as if pressed again.
 */

/* How long (seconds) a started debugger may stay silent before the start is
   given up; the environment variable overrides it. */
#define E_D_START_TIMEOUT 30
#define E_D_START_TIMEOUT_ENV "XWPE_DEBUG_START_TIMEOUT"
/* The waiting line turns at the build's cadence. */
#define E_D_START_TICK_NS 120000000L
#define E_D_START_WAIT_FMT "starting %s ... %lds  (Esc cancels)"
#define E_D_START_PHASE_FMT "%s %s ... %lds  (Esc cancels)"
#define E_D_START_CANCELLED_FMT "%s start cancelled"
#define E_D_START_TIMED_OUT_FMT "%s did not answer within %d s"
#define E_D_START_UNWATCHED_FMT "%s start given up: could not watch its output (too many open streams)"
#define E_D_START_BUSY_LINE " The debugger is starting: Esc cancels it"
#define E_D_START_LINE_MAX 128
#define E_D_START_NO_FD (-1)
/* e_run_debug's answer while the debugger it started has not answered: the
   command that asked returns quietly and is run again on the answer. */
#define E_D_STARTING (-4)

/* Why a start that got no answer is being ended. */
#define E_D_START_WAITING   0
#define E_D_START_CANCELLED 1
#define E_D_START_TIMED_OUT 2
#define E_D_START_UNWATCHED 3   /* the loop had no room to read its answer */
#define E_D_START_NO_PORT   4   /* jdb named no port to listen on */
#define E_D_START_VM_GONE   5   /* the Java VM ended before it reached jdb */

#define E_D_START_NO_PORT_FMT "%s named no port to listen on"
#define E_D_START_VM_GONE_FMT \
 "java ended before %s connected (exit status %d)"
#define E_D_START_VM_GONE_PLAIN_FMT "java ended before %s connected"
/* What the waiting line says once jdb listens and the VM was started. */
#define E_D_JDB_VM_STARTING "starting the Java VM"

/* The one debugger start that may be waiting. */
static struct {
 int active;        /* a debugger was started and has not answered yet      */
 int ending;        /* E_D_START_WAITING, or why the debugger is being ended */
 int held;         /* it answered while a box was up                       */
 int wait_row;      /* the waiting line is Messages' last row               */
 int timeout;       /* the deadline, in seconds                             */
 time_t start;
 int hb_timer;      /* the heartbeat timer, or WPE_TIMER_NONE               */
 int out_fd;        /* the debugger's output, while it is watched           */
 int dap;           /* a debug adapter starts, not a pipe debugger          */
 int pty_fd;        /* an adapter's debuggee pty, while it is watched       */
 int opened_messages; /* Messages was opened for the waiting line alone      */
 ECNT *cn;
} e_d_start = { 0, E_D_START_WAITING, 0, 0, E_D_START_TIMEOUT, 0,
                WPE_TIMER_NONE, E_D_START_NO_FD, 0, E_D_START_NO_FD, 0, NULL };

/* The command to run once the debugger has answered. */
static e_d_resume_fn e_d_start_cmd = NULL;
static int e_d_start_arg = 0;

static void e_d_jdb_start_begin(void);                   /* jdb's start */
static void e_d_jdb_start_on_output(int fd, void *data);  /* (below) */
static int e_d_start_watch_fd(int fd, wpe_fd_callback cb);

/**
 * e_d_start_busy - Is a started debugger still waiting to answer?
 *
 * The editor's key loop asks this so Esc ends the start, as the waiting line
 * in Messages says.
 * Return: 1 while the start waits.
 */
int e_d_start_busy(void)
{
 return(e_d_start.active);
}

/**
 * e_d_start_refused - Refuse a debugger command while the debugger starts.
 * @f: the window the key was pressed in.
 *
 * Nothing may be sent to a debugger that has not answered: the command
 * would only be read after the banner, or never.  The status row says why
 * the key did nothing and that Esc ends the start.
 * Return: 1 when the command must not run.
 */
static int e_d_start_refused(FENSTER *f)
{
 if (!e_d_start.active)
  return(0);
 e_status_say(f, E_D_START_BUSY_LINE);
 return(1);
}

#define E_D_RUNNING_LINE \
 " The program is running: type its input in Messages, or Debug > Quit"
/* A debugger that gave the program no terminal of its own (pdb, a68g) takes no
   input typed in the editor. */
#define E_D_RUNNING_NO_INPUT_LINE \
 " The program is running: Debug > Quit ends the session"

/**
 * e_d_running_line - The line that says the program runs, and what the user
 * can do meanwhile.
 *
 * Typed text reaches the program only when it has a terminal of its own,
 * so the line offers typing its input only then.
 */
static const char *e_d_running_line(void)
{
 if (e_d_run_takes_typed_text())
  return(E_D_RUNNING_LINE);
 return(E_D_RUNNING_NO_INPUT_LINE);
}

/**
 * e_d_running_refusal_say - Tell the user a key was refused because the
 * program runs, and keep the line until the next key.
 * @f: the window the key was pressed in.
 *
 * A program that prints repaints Messages with every line, and each repaint
 * put the key hints back at once: the user saw the key do nothing.  The
 * line stays until the user presses a key or the program stops.
 */
static void e_d_running_refusal_say(FENSTER *f)
{
 e_status_say_until_key(f, e_d_running_line());
}

/**
 * e_d_running_refusal_forget - The program no longer runs: a refusal still
 * on the status row would say it does.
 *
 * At a stop, or when the session ends, the bottom row names the stepping
 * keys again without waiting for a key; a newer line said meanwhile stays.
 */
static void e_d_running_refusal_forget(void)
{
 if (e_status_drop_held_line(E_D_RUNNING_LINE))
  return;
 e_status_drop_held_line(E_D_RUNNING_NO_INPUT_LINE);
}

/**
 * e_d_refuse_while_running - Refuse a debugger command while the program
 * runs under the run controller (gdb, sdb, dbx, xdb, pdb, jdb, a68g).
 * @f: the window the key was pressed in.
 *
 * A program that runs cannot be stepped, continued or asked about: the
 * debugger reads the command only at the next stop, so it would be answered
 * out of turn and the session would lose its place.  The status row says
 * what to do instead:
 * type the program's input, or end the session with Debug > Quit.  While
 * gdb answers at a stop the command is refused the same way, with the
 * waiting line (e_d_ga_refused).
 * Return: 1 when the command must not run.
 */
static int e_d_refuse_while_running(FENSTER *f)
{
 if (e_d_ga_refused(f))
  return(1);
 if (!e_d_async_pending)
  return(0);
 e_d_running_refusal_say(f);
 return(1);
}

/**
 * e_d_pause_refused - Say why Pause has nothing to do, when it has not.
 * @f: the window the key was pressed in.
 *
 * Pause only means something while the program runs: with no session, or
 * at a stop (or a stop waiting behind a box), the status row says so and
 * nothing is sent to the debugger.  A backend that cannot interrupt yet
 * says that.
 * Return: 1 when Pause must not go on.
 */
static int e_d_pause_refused(FENSTER *f)
{
 if (!e_d_session_runs())
  e_status_say(f, E_D_PAUSE_NO_SESSION_LINE);
 else if (!e_d_run_going() || e_d_run.stop_held)
  e_status_say(f, E_D_PAUSE_STOPPED_LINE);
 else if (!e_d_run.ops->interrupt)
  e_status_say(f, E_D_PAUSE_NO_WAY_LINE);
 else
  return(0);
 return(1);
}

/**
 * e_d_pause_takes_over_sync - Pause was pressed while an interrupt sent for
 * a breakpoint change is on its way: that interrupt becomes the Pause.
 *
 * No second interrupt is sent, which the debugger could answer out of
 * turn; the stop that comes is shown as the user's Pause, and the change is
 * applied at it.
 */
static void e_d_pause_takes_over_sync(void)
{
 e_d_run.interrupt_for = E_D_INT_PAUSE;
}

/**
 * e_d_pause - Debug > Interrupt (pause), Ctrl-Break, Ctrl-G I: stop the
 * program that runs under the debugger where it is.
 * @f: the window the key was pressed in.
 *
 * The user sees the line the program was on marked, the Watches refreshed
 * and "Paused" on the status row, and can then step, inspect or continue.
 * A program that handles SIGINT itself is not stopped by it: the status
 * row says so at the next stop.  At a stop, or with no session, the status
 * row says there is nothing to pause.
 * Return: 0.
 */
int e_d_pause(FENSTER *f)
{
 if (e_d_start_refused(f) || e_d_ga_pause_takes_sync(f))
  return(0);
 if (e_d_pause_refused(f))
  return(0);
 if (e_d_run.pause == E_D_PAUSE_ASKED)
 {
  e_status_say(f, E_D_PAUSING_LINE);
  return(0);
 }
 if (e_d_run.interrupt_for == E_D_INT_SYNC)
  e_d_pause_takes_over_sync();
 else if (e_d_run_interrupt(E_D_INT_PAUSE) < 0)
 {
  e_status_say(f, E_D_PAUSE_FAILED_LINE);
  return(0);
 }
 e_d_run.pause = E_D_PAUSE_ASKED;
 e_status_say(f, E_D_PAUSING_LINE);
 return(0);
}

/**
 * e_d_start_timeout - The start's deadline in seconds.
 *
 * E_D_START_TIMEOUT, or a positive whole number of seconds from the
 * environment, for a debugger known to start slowly (a large program, a
 * slow machine) or for a test that should not wait half a minute.
 */
static int e_d_start_timeout(void)
{
 const char *env = getenv(E_D_START_TIMEOUT_ENV);
 long secs;
 char *end;

 if (!env || !*env)
  return(E_D_START_TIMEOUT);
 secs = strtol(env, &end, 10);
 if (*end || secs <= 0 || secs > INT_MAX)
  return(E_D_START_TIMEOUT);
 return((int)secs);
}

/**
 * e_d_start_label - The debugger's name as the user knows it.
 *
 * pdb runs as python3, but the user picked Python debugging, so the lines
 * say "pdb"; a debug adapter is named by its command ("dlv", "gdb").
 */
static const char *e_d_start_label(void)
{
 if (e_deb_type == DEB_DAP && g_dap_adapter_name[0])
  return(g_dap_adapter_name);
 if (e_deb_type == DEB_PDB)
  return("pdb");
 if (e_debugger)
  return(e_debugger);
 return("debugger");
}

/**
 * e_d_start_wait_line_drop - Take the waiting line out of Messages.
 */
static void e_d_start_wait_line_drop(void)
{
 FENSTER *mf = e_build_messages(e_d_start.cn);

 if (!e_d_start.wait_row)
  return;
 e_d_start.wait_row = 0;
 if (mf)
  e_build_drop_last_row(mf->b);
}

/**
 * e_d_start_adapter_alive - Has the starting debug adapter answered?
 *
 * An adapter that answered initialize is alive; what it does next (dlv
 * building a large package on a cold cache) may take as long as it needs,
 * so the deadline, meant for an adapter that never answers, stops applying
 * and only Esc ends the start.
 */
static int e_d_start_adapter_alive(void)
{
 return(e_d_start.dap && e_dap_start_answered(g_dap));
}

/**
 * e_d_start_wait_text - The waiting line's text.
 * @line: where it goes; @n: its size; @secs: the seconds so far.
 *
 * "starting dlv ... 3s  (Esc cancels)" until the debugger has answered;
 * then, for an adapter, what it is doing: "dlv building ... 40s  (Esc
 * cancels)".
 */
static void e_d_start_wait_text(char *line, size_t n, long secs)
{
 if (e_d_start_adapter_alive())
 {
  snprintf(line, n, E_D_START_PHASE_FMT, e_d_start_label(),
           e_dap_start_waits_for(g_dap), secs);
  return;
 }
 if (e_d_jdb_vm.launched)
 {
  snprintf(line, n, E_D_START_PHASE_FMT, e_d_start_label(),
           E_D_JDB_VM_STARTING, secs);
  return;
 }
 snprintf(line, n, E_D_START_WAIT_FMT, e_d_start_label(), secs);
}

/**
 * e_d_start_wait_line_put - Write "starting <debugger> ... Ns (Esc cancels)"
 * at the foot of Messages.
 *
 * What the user sees while the debugger starts: that it starts, for how
 * long, and that Esc ends it, where a build shows its own waiting line.
 */
static void e_d_start_wait_line_put(void)
{
 FENSTER *mf = e_build_messages(e_d_start.cn);
 char line[E_D_START_LINE_MAX];

 if (!mf || e_d_start.wait_row)
  return;
 e_d_start_wait_text(line, sizeof line, (long)(time(NULL) - e_d_start.start));
 print_to_end_of_buffer(mf->b, line, 0);
 e_d_start.wait_row = 1;
}

/**
 * e_d_start_wait_line_turn - Show the waiting line with the seconds so far.
 */
static void e_d_start_wait_line_turn(void)
{
 e_d_start_wait_line_drop();
 e_d_start_wait_line_put();
 e_messages_show_tail(e_d_start.cn);
}

/**
 * e_d_start_dap_signal - End the starting debug adapter.
 * @sig: SIGTERM, which hands it to the owner's ladder (SIGKILL after the
 * grace comes from there), or SIGKILL when the start ends at once.
 */
static void e_d_start_dap_signal(int sig)
{
 if (sig == SIGTERM)
  e_dap_end_adapter(g_dap, SIGTERM);
 else
  e_dap_signal(g_dap, sig);
}

/**
 * e_d_start_adapter_gone - Has the starting debug adapter exited?  A line
 * debugger's end is reported to e_d_on_debugger_ended instead.
 */
static int e_d_start_adapter_gone(void)
{
 return(e_d_start.dap && e_dap_adapter_gone(g_dap));
}

/**
 * e_d_start_unwatch_answers - Stop reading the starting debugger's output,
 * once it has ended or the start is over; its program's output, when the
 * start watches it, stays watched.
 */
static void e_d_start_unwatch_answers(void)
{
 if (e_d_start.out_fd == E_D_START_NO_FD)
  return;
 wpe_fd_del(e_d_start.out_fd);
 e_d_start.out_fd = E_D_START_NO_FD;
}

/**
 * e_d_start_unwatch_output - Stop reading the debugger's output.
 */
static void e_d_start_unwatch_output(void)
{
 if (e_d_start.pty_fd != E_D_START_NO_FD)
 {
  wpe_fd_del(e_d_start.pty_fd);
  e_d_start.pty_fd = E_D_START_NO_FD;
 }
 e_d_start_unwatch_answers();
}

/**
 * e_d_start_heartbeat_stop - Stop the waiting line's timer.
 */
static void e_d_start_heartbeat_stop(void)
{
 wpe_timer_del(e_d_start.hb_timer);
 e_d_start.hb_timer = WPE_TIMER_NONE;
}

/**
 * e_d_start_forget - Drop everything the start watches.
 *
 * Called when the start has ended, one way or another, and by the session's
 * own teardown, so a quit during the start leaves nothing registered.
 * Return: 1 when a start was waiting.
 */
static int e_d_start_forget(void)
{
 if (!e_d_start.active)
  return(0);
 e_d_start_unwatch_output();
 e_d_start_heartbeat_stop();
 e_d_start_wait_line_drop();
 e_d_start.active = 0;
 e_d_start.held = 0;
 e_d_start.ending = E_D_START_WAITING;
 return(1);
}

/**
 * e_d_jdb_vm_gone_line - The line that says the Java VM ended before it
 * reached jdb, with its exit status when it is known.
 * @line, @n: receives it; @label: the debugger's name.
 *
 * Under it in Messages the user has already read what the VM said.
 */
static void e_d_jdb_vm_gone_line(char *line, size_t n, const char *label)
{
 if (e_d_jdb_vm.status_known)
  snprintf(line, n, E_D_START_VM_GONE_FMT, label,
           e_d_exit_status_number(e_d_jdb_vm.status));
 else
  snprintf(line, n, E_D_START_VM_GONE_PLAIN_FMT, label);
}

/**
 * e_d_start_say_end - Write why the start ended in Messages.
 * @why: why it ended (an E_D_START_ reason other than E_D_START_WAITING).
 * @label: the debugger's name; @timeout: the deadline that passed.
 * @f: the window on top.
 */
static void e_d_start_say_end(int why, const char *label, int timeout, FENSTER *f)
{
 char line[E_D_START_LINE_MAX + E_D_START_LINE_MAX];

 if (why == E_D_START_CANCELLED)
  snprintf(line, sizeof line, E_D_START_CANCELLED_FMT, label);
 else if (why == E_D_START_UNWATCHED)
  snprintf(line, sizeof line, E_D_START_UNWATCHED_FMT, label);
 else if (why == E_D_START_NO_PORT)
  snprintf(line, sizeof line, E_D_START_NO_PORT_FMT, label);
 else if (why == E_D_START_VM_GONE)
  e_d_jdb_vm_gone_line(line, sizeof line, label);
 else
  snprintf(line, sizeof line, E_D_START_TIMED_OUT_FMT, label, timeout);
 e_d_p_message(line, f, 1);
}

/**
 * e_d_dap_keep_start_error - Keep the adapter's reason for refusing the
 * start, before the session that holds it is closed.
 */
static void e_d_dap_keep_start_error(void)
{
 const char *why = e_dap_start_error(g_dap);

 g_dap_start_error[0] = '\0';
 if (why)
  snprintf(g_dap_start_error, sizeof g_dap_start_error, "%s", why);
}

/**
 * e_d_dap_say_start_error - Show the adapter's refusal in Messages, one
 * line per line of it.
 * @f: the window on top.
 *
 * When dlv cannot build the package, the user reads the compiler's error
 * where build errors always appear, and goes on editing: no box to dismiss.
 */
static void e_d_dap_say_start_error(FENSTER *f)
{
 char *line = g_dap_start_error, *nl;

 while (line && *line)
 {
  nl = strchr(line, '\n');
  if (nl)
   *nl = '\0';
  e_d_p_message(line, f, 1);
  line = nl ? nl + 1 : NULL;
 }
 g_dap_start_error[0] = '\0';
}

/**
 * e_d_start_say_died - Say that the debugger ended by itself before it
 * answered.
 * @dap: a debug adapter was starting; @f: the window on top.
 *
 * A pipe debugger is reported as before, by the session's end; an adapter
 * that hung up says so in a box, as when it could not be started at all.
 */
static void e_d_start_say_died(int dap, FENSTER *f)
{
 if (dap && g_dap_start_error[0])
 {
  e_d_dap_say_start_error(f);
  return;
 }
 if (dap)
 {
  e_error(E_D_DAP_START_FAILED_LINE, 0, f->fb);
  return;
 }
 e_show_error(0, f);
}

/**
 * e_d_start_quit - Tear the session of an ended start down.
 * @why: why the start ended; @f: the window on top.
 *
 * A start the user cancelled, one that passed its deadline, and one whose
 * output the loop could not watch, are all a debugger that was launched and
 * never answered: the user saw no session, so
 * Messages keeps the one line that says why the start ended instead of also
 * reporting the end of a session that never began.  A debugger that ended by
 * itself before answering is torn down as a session, as before.
 */
static void e_d_start_quit(int why, FENSTER *f)
{
 if (why != E_D_START_WAITING)
  e_d_start_never_ran = 1;
 e_d_quit(f);
 e_d_start_never_ran = 0;
}

/**
 * e_d_start_end - The debugger is gone, or must go: end the session.
 *
 * A start the user cancelled, or that passed its deadline, ends with a line
 * that says so; a debugger that ended by itself before answering is
 * reported as before, by the session's end.  Either way the editor is left
 * as it was before Ctrl-G R.
 */
static void e_d_start_end(void)
{
 ECNT *cn = e_d_start.cn;
 FENSTER *f = cn->f[cn->mxedt];
 int why = e_d_start.ending, timeout = e_d_start.timeout;
 int dap = e_d_start.dap;
 char label[E_D_START_LINE_MAX];

 strncpy(label, e_d_start_label(), sizeof label - 1);
 label[sizeof label - 1] = '\0';
 e_d_start_forget();
 e_d_start_quit(why, f);
 f = cn->f[cn->mxedt];
 if (why == E_D_START_WAITING)
  e_d_start_say_died(dap, f);
 else
  e_d_start_say_end(why, label, timeout, f);
}

/**
 * e_d_start_stop - End a debugger that has not answered.
 * @why: E_D_START_CANCELLED (Esc), E_D_START_TIMED_OUT (the deadline), or
 * another reason the start cannot go on.
 *
 * A Java VM the editor started for jdb is ended first, at once.  The
 * debugger gets SIGTERM, and SIGKILL after the grace of the owner of the
 * editor's children; the session ends when the owner reports it gone.  A
 * debug adapter is ended the same way, its end seen by the heartbeat;
 * without a process of the editor's own to wait for, or when the timer
 * table had no room for the heartbeat, the session ends at once.
 */
static void e_d_start_stop(int why)
{
 if (e_d_start.ending != E_D_START_WAITING)
  return;
 e_d_jdb_vm_end();
 e_d_start.ending = why;
 e_d_start_unwatch_output();
 if (!e_d_start.dap && e_d_dbg.child.pid > 0)
 {
  e_d_debugger_end_by_editor(SIGTERM);
  return;
 }
 if (e_d_start.dap && e_d_start.hb_timer != WPE_TIMER_NONE &&
     e_dap_adapter_pid(g_dap) > 0)
 {
  e_d_start_dap_signal(SIGTERM);
  return;
 }
 if (e_d_start.dap)
  e_d_start_dap_signal(SIGKILL);
 e_d_start_end();
}

/**
 * e_d_debugger_end_comes_reported - Will the owner of the editor's children
 * report the starting debugger's end?
 *
 * Under the terminal editor the debugger is the editor's own child.  Under
 * the graphical editors the child is the script around it, which outlives
 * the debugger while it waits for a Return, so there the end of the
 * debugger's output is the only sign that it went.
 */
static int e_d_debugger_end_comes_reported(void)
{
 if (WpeIsXwin())
  return(0);
 return(e_d_dbg.live && e_d_dbg.child.pid > 0);
}

/**
 * e_d_start_on_output_end - The starting debugger's output ended before it
 * answered.
 *
 * A debugger that is dying is reported gone a moment later, with its exit
 * status, and the start ends then with the line that names it.  One that
 * closed its output and runs on can never answer: the start goes on
 * waiting, as for any debugger that does not answer, so the user still
 * sees the waiting line and ends it with Esc, or the deadline does, and
 * reads that line, not an end the editor's own signal caused.  Where the
 * end is not reported (the graphical editors' script), the start ends here.
 */
static void e_d_start_on_output_end(void)
{
 if (e_d_debugger_end_comes_reported())
 {
  e_d_start_unwatch_answers();
  return;
 }
 e_d_start_end();
}

/**
 * e_d_start_check_deadline - End the start once its deadline has passed.
 */
static void e_d_start_check_deadline(void)
{
 if (e_d_start.ending != E_D_START_WAITING)
  return;
 if (e_d_start_adapter_alive())
  return;
 if (time(NULL) - e_d_start.start >= e_d_start.timeout)
  e_d_start_stop(E_D_START_TIMED_OUT);
}

/**
 * e_d_start_run_pressed - Run the command the user pressed once the debugger
 * has answered, and show its result.
 * @cmd: the command (Run, Step, Go to cursor, ...); @f: the top window;
 * @arg: its argument.
 *
 * A command pressed at the keyboard is painted by the key loop's refresh
 * after it returns.  This one runs from the descriptor loop when the
 * debugger's prompt arrives, where no such refresh follows, so the first
 * stop (pdb's breakpoint, for one) would stay painted off screen, unmarked,
 * until the next key.  The asynchronous gdb stop ends with the same refresh.
 */
static void e_d_start_run_pressed(e_d_resume_fn cmd, FENSTER *f, int arg)
{
 cmd(f, arg);
 e_refresh();
}

/**
 * e_d_start_after_answer - Set a debug adapter's session up now that the
 * adapter has answered.
 * @f: the window on top.
 *
 * The adapter's handshake already carried the breakpoints, so the program
 * runs to its first stop.  Every pipe debugger is set up on the fd loop
 * instead (e_d_start_sets_up_later), so only an adapter comes here.
 * Return: 0, or -1 when the session ended instead (and said why).
 */
static int e_d_start_after_answer(FENSTER *f)
{
 ECNT *cn = f->ed;

 e_d_dap_start_close_messages(cn);
 return(e_d_dap_start_finish(cn->f[cn->mxedt]));
}

/**
 * e_d_start_session_ready - The session is set up: run the command the user
 * pressed to start it.
 * @cn: the editor; @cmd: that command, or NULL; @arg: its argument.
 */
static void e_d_start_session_ready(ECNT *cn, e_d_resume_fn cmd, int arg)
{
 /* A debug adapter's start returned before any bar was chosen, and the
    selected debugger may differ from the one the start began with: name
    the keys of the session that now runs. */
 e_refresh_editor_bars(cn);
 if (cmd)
  e_d_start_run_pressed(cmd, cn->f[cn->mxedt], arg);
}

/**
 * e_d_start_sets_up_later - A pipe debugger answered its banner: set it up
 * through the request engine, and run the command the user pressed once it
 * has answered.
 * @cn: the editor; @cmd, @arg: the command that started the session.
 *
 * The user keeps the editor while the debugger takes its settings and
 * breakpoints; a debugger key typed meanwhile is kept for after the first
 * stop.
 * Return: 1 when the set-up goes on later (every pipe debugger), else 0.
 */
static int e_d_start_sets_up_later(ECNT *cn, e_d_resume_fn cmd, int arg)
{
 if (!e_d_ga_backend_of(e_deb_type) || e_d_start.dap)
  return(0);
 e_deb_prompt_reset();
 e_d_ga_set_up(cn, cmd, arg);
 return(1);
}

/**
 * e_d_start_continue - The debugger answered: go on with the session.
 *
 * What followed the banner before (the settings sent to gdb, the
 * breakpoints) runs now, and then the command the user pressed.
 */
static void e_d_start_continue(void)
{
 ECNT *cn = e_d_start.cn;
 e_d_resume_fn cmd = e_d_start_cmd;
 int arg = e_d_start_arg;
 FENSTER *f;

 e_d_start_forget();
 e_messages_show_tail(cn);       /* Messages without the waiting line */
 e_d_start_cmd = NULL;
 f = cn->f[cn->mxedt];
 if (e_d_start_sets_up_later(cn, cmd, arg))
  return;
 if (e_d_start_after_answer(f) < 0)
  return;
 e_d_start_session_ready(cn, cmd, arg);
}

/**
 * e_d_start_answered - The prompt arrived.
 *
 * The session goes on at once, or, when a dialog is open, as soon as the
 * editor is back in its key loop, so nothing is painted under the dialog.
 */
static void e_d_start_answered(void)
{
 e_d_start_unwatch_output();
 if (wpe_paint_idle())
 {
  e_d_start_continue();
  return;
 }
 e_d_start_heartbeat_stop();
 e_d_start.held = 1;
 e_d_start_deliver_later();
}

/**
 * e_d_start_holds_left - Is any part of a start still held: a build or a
 * setup that ended, a build server's answer, the debugger's answer or its
 * end?  One of them may have opened a box that holds the others again.
 */
static int e_d_start_holds_left(void)
{
 if (e_d_compile.held || e_d_bsp.held)
  return(1);
 if (e_scala_setup_lsp.held || e_scala_setup_dap.held)
  return(1);
 if (e_d_dbg.held && e_d_dbg.live)
  return(1);
 if (e_d_dbg.say_later && e_d_dbg.ended)
  return(1);
 return(e_d_start.active && e_d_start.held);
}

/**
 * e_d_start_deliver_held - Go on with a session whose debugger answered,
 * whose build or setup ended, or whose debugger ended, behind a dialog.
 * @data: unused.
 *
 * Run by the screen's owner once the editor is back in its key loop, the
 * point that is outside every dialog.
 */
static void e_d_start_deliver_held(void *data)
{
 (void)data;
 e_scala_setup_deliver_held();
 e_d_compile_deliver_held();
 e_d_bsp_deliver_held();
 e_d_debugger_deliver_held();
 if (e_d_start.active && e_d_start.held && wpe_paint_idle())
  e_d_start_continue();
 if (e_d_start_holds_left() && !wpe_paint_idle())
  e_d_start_deliver_later();
}

/**
 * e_d_start_deliver_later - Something of the start was held behind a box:
 * the screen's owner goes on with it once the box has closed.
 */
static void e_d_start_deliver_later(void)
{
 wpe_paint_request(e_d_start_deliver_held, NULL, "the debugger's start");
}

/* ---- The debugger's end, as the owner of the editor's children reports it */

/**
 * e_d_debugger_session_end - The debugger of a session that still ran has
 * gone: end the session, now or once the dialog that is open has closed.
 *
 * A start still waiting for the debugger's answer ends with the line its
 * end deserves; a session ends with the line that names the debugger and
 * its exit status.
 */
static void e_d_debugger_session_end(void)
{
 if (e_d_start.active)
 {
  if (!e_d_start.held)
   e_d_start_end();
  return;
 }
 if (!wpe_paint_idle())
 {
  e_d_dbg.held = 1;
  e_d_start_deliver_later();
  return;
 }
 e_d_dbg.held = 0;
 e_d_quit(WpeEditor->f[WpeEditor->mxedt]);
}

/**
 * e_d_debugger_deliver_held - End a session whose debugger ended behind a
 * dialog, or say how the debugger of an ended session went.  Run by the
 * screen's owner once the dialog has closed.
 */
static void e_d_debugger_deliver_held(void)
{
 if (!wpe_paint_idle())
  return;
 if (e_d_dbg.held && e_d_dbg.live)
  e_d_debugger_session_end();
 if (e_d_dbg.say_later && e_d_dbg.ended)
  e_d_debugger_say_died();
}

/**
 * e_d_on_debugger_ended - The owner of the editor's children reported that
 * the session's debugger ended (or did not end even after SIGKILL).
 * @c: its process; @status: its wait status; @how: WPE_CHILD_ENDED or
 * WPE_CHILD_ENDED_UNKNOWN; @data: unused.
 *
 * The one place a line debugger's end is learned.  The input kept open for
 * its quit is closed.  A session already over says now, once, how the
 * debugger died, when its output ended before the report.  A session that
 * still ran on it ends here, even when a process the debugger started keeps
 * its output open, so no end of file would ever come.
 */
static void e_d_on_debugger_ended(WpeChild *c, int status, int how,
                                  void *data)
{
 (void)c;
 (void)data;
 e_d_dbg.ended = 1;
 e_d_dbg.status = status;
 e_d_dbg.status_known = (how == WPE_CHILD_ENDED);
 e_d_debugger_close_input();
 if (e_d_dbg.say_later)
 {
  if (wpe_paint_idle())
   e_d_debugger_say_died();
  else
   e_d_start_deliver_later();
  return;
 }
 if (!e_d_dbg.live || e_d_dbg.end_expected)
  return;
 if (!e_d_dbg.asked_end)
  e_d_note_output_closed();
 e_d_debugger_session_end();
}

/**
 * e_d_debugger_let_go - A new session starts while the last one's debugger
 * is still ending: its end goes on, followed by the owner alone, and its
 * input is closed.  A line it still owed the user is said now: its output
 * closed and it may still run, so it stopped answering; the exit status is
 * not waited for.
 */
static void e_d_debugger_let_go(void)
{
 if (e_d_dbg.say_later)
  e_d_debugger_say_died();
 e_d_dbg.live = 0;
 e_d_dbg.held = 0;
 if (e_d_dbg.child.pid > 0)
  wpe_child_end_detached(&e_d_dbg.child, SIGTERM);
 e_d_debugger_close_input();
}

/**
 * e_d_end_at_exit - The editor is ending: end the line debugger too.
 *
 * Called by the editor's end after the user's work is saved, so a logout
 * or a "kill PID" during a debug session leaves no debugger and no program
 * behind.  The debugger is told to quit, as Quit tells it, so it ends its
 * program itself, and it never gets a signal that would dump core; the
 * editor's end then runs its loop, bounded, until it has gone, and kills
 * whatever is left.  Nothing is shown: the windows are going.
 */
void e_d_end_at_exit(void)
{
 e_d_jdb_vm_end();
 e_d_compile_end_at_exit();
 e_d_bsp_drop();             /* a build server still starting is ended too */
 if (!e_d_dbg.live)
  return;
 e_d_dbg.say_later = 0;
 if (e_d_quit_can_be_asked())
  e_d_send_cmd(e_d_quit_command());
 e_d_debugger_end(e_d_debugger_end_signal(e_d_run_going()));
}

/**
 * e_d_start_on_output - The debugger wrote: read its banner up to the prompt.
 * @fd: its output; @data: unused.
 *
 * The banner itself is not shown, as before.  End of file before the prompt
 * means the debugger will never answer (e_d_start_on_output_end).  The
 * deadline is the heartbeat's.
 */
static void e_d_start_on_output(int fd, void *data)
{
 signed char buf[256];
 int ret;

 (void)data;
 if (!e_d_start.active || e_d_start.ending != E_D_START_WAITING)
  return;
 for (;;)
 {
  ret = e_d_line_read_nb(fd, buf, sizeof buf);
  if (ret == -2)
   break;
  if (ret == -1)
  {
   e_d_start_on_output_end();
   return;
  }
  if (ret == 1)
  {
   e_d_start_answered();
   return;
  }
 }
 /* An adapter that is gone and no longer reported by the reader (its end
    already read) ends the start here, so the hang-up cannot wake this
    callback again and again.  A line debugger's end is reported to
    e_d_on_debugger_ended. */
 if (e_d_start_adapter_gone())
  e_d_start_end();
}

/* ---- jdb's start: the port, then the VM -------------------------------
 *
 * jdb says first where it listens; the editor then starts the program's VM,
 * which connects to it, and the start has answered once jdb says the VM
 * started and gives its first prompt.  The generic banner reader cannot
 * serve here: it reads jdb's line "VM Started: > No frames ..." as a prompt
 * and then every breakpoint's answer one behind.
 */

/* jdb's first line, which names the port it waits on for the VM. */
#define E_D_JDB_LISTEN_LINE "Listening at address:"
/* What jdb says once the VM connected, before its first prompt. */
#define E_D_JDB_VM_STARTED  "VM Started:"
#define E_D_JDB_PORT_MAX    65535
#define E_D_JDB_PORT_DIGITS 5       /* the digits of E_D_JDB_PORT_MAX */
#define E_D_JDB_PORT_SEP    ':'

/**
 * e_d_jdb_listen_port - The port jdb's listen line names.
 * @line: the line, "Listening at address: localhost:40367", up to its '\n'.
 *
 * Only the digits after the last ':' up to the end of the line count; the
 * host jdb prints is not used, the VM always dials 127.0.0.1, which jdb
 * was told to bind.  Nothing is guessed: a line that names no valid port
 * ends the start.
 * Return: the port, or -1.
 */
static int e_d_jdb_listen_port(const char *line)
{
 const char *end = strchr(line, '\n'), *sep = NULL, *p;
 long port;

 if (end > line && end[-1] == '\r')
  end--;                      /* a line that ends "\r\n" */
 for (p = line; p < end; p++)
  if (*p == E_D_JDB_PORT_SEP)
   sep = p;
 if (!sep || sep + 1 == end || end - sep > E_D_JDB_PORT_DIGITS + 1)
  return(-1);
 for (p = sep + 1; p < end; p++)
  if (!isdigit((unsigned char)*p))
   return(-1);
 port = strtol(sep + 1, NULL, 10);
 if (port < 1 || port > E_D_JDB_PORT_MAX)
  return(-1);
 return((int)port);
}

/**
 * e_d_jdb_start_show_vm_words - Show in Messages what the Java VM has
 * written so far, above the waiting line.
 *
 * A VM that cannot start says why on its terminal ("Could not find or load
 * main class", a class compiled for a newer Java): the user reads it where
 * build errors appear, or on the User Screen when he closed Messages: the
 * VM's words are read either way, so they never wake the editor again and
 * again.
 * @max_reads: E_D_READS_PER_WAKE while it runs, E_D_PTY_READS_ALL once
 * it ended.
 */
static void e_d_jdb_start_show_vm_words(int max_reads)
{
 FENSTER *mf = e_build_messages(e_d_start.cn);

 e_d_start_wait_line_drop();
 e_d_pty_read_to_messages(mf, max_reads);
}

/**
 * e_d_jdb_start_on_program_output - The VM wrote while jdb starts: show it.
 * @fd: the pty master; @data: unused.
 */
static void e_d_jdb_start_on_program_output(int fd, void *data)
{
 (void)data;
 if (!e_d_start.active)
  return;
 e_d_jdb_start_show_vm_words(E_D_READS_PER_WAKE);
 if (e_d_pty_hung)
 {
  wpe_fd_del(fd);
  e_d_start.pty_fd = E_D_START_NO_FD;
 }
 if (e_d_start.ending == E_D_START_WAITING)
  e_d_start_wait_line_turn();
}

/**
 * e_d_jdb_start_launch_vm - jdb listens: start the program's VM on the
 * session's pty and show what it says while it connects.
 * @port: the port jdb named.
 */
static void e_d_jdb_start_launch_vm(int port)
{
 if (e_d_jdb_vm_fork(port) < 0)
 {
  e_d_start_stop(E_D_START_VM_GONE);
  return;
 }
 e_d_start_wait_line_turn();          /* "jdb starting the Java VM ..." */
 if (e_d_pty_master < 0)
  return;
 e_d_start.pty_fd = e_d_pty_master;
 e_d_start_watch_fd(e_d_start.pty_fd, e_d_jdb_start_on_program_output);
}

/**
 * e_d_jdb_start_say_line - Write one line of jdb's answer in Messages.
 * @line: where it starts; it ends at '\n'.
 */
static void e_d_jdb_start_say_line(const char *line)
{
 char copy[E_D_START_LINE_MAX];
 const char *end = strchr(line, '\n');
 int len = (int)(end - line);

 if (len > 0 && line[len - 1] == '\r')
  len--;
 if (len >= (int)sizeof copy)
  len = (int)sizeof copy - 1;
 memcpy(copy, line, len);
 copy[len] = '\0';
 e_d_p_message(copy, e_d_start.cn->f[e_d_start.cn->mxedt], 1);
}

/**
 * e_d_jdb_start_on_listen_line - Start the VM once jdb's listen line is
 * whole.
 * @text: what jdb has said so far.
 *
 * A listen line without a valid port is shown and ends the start.  The
 * waiting line goes first, so jdb's line stays above the line that says
 * why the start ended.
 */
static void e_d_jdb_start_on_listen_line(const char *text)
{
 const char *at = strstr(text, E_D_JDB_LISTEN_LINE);
 int port;

 if (!at || !strchr(at, '\n'))
  return;                     /* not said yet, or not whole yet */
 port = e_d_jdb_listen_port(at);
 if (port > 0)
 {
  e_d_jdb_start_launch_vm(port);
  return;
 }
 e_d_start_wait_line_drop();
 e_d_jdb_start_say_line(at);
 e_d_start_stop(E_D_START_NO_PORT);
}

/**
 * e_d_jdb_start_vm_connected - Has jdb said that the VM connected and
 * stopped at its start, and asked for a command?
 * @text: what jdb has said so far.
 * Return: 1 when it has.
 */
static int e_d_jdb_start_vm_connected(const char *text)
{
 size_t len = strlen(text), tail = strlen(E_D_JDB_PROMPT_TAIL);

 if (!strstr(text, E_D_JDB_VM_STARTED))
  return(0);
 return(len >= tail && strcmp(text + len - tail, E_D_JDB_PROMPT_TAIL) == 0);
}

/**
 * e_d_jdb_start_vm_ended - Did the VM the editor started for jdb end
 * before it reached jdb?
 *
 * The heartbeat asks: the VM's end is seen by reaping it, not by the pty,
 * which the graphical editor still holds open.
 * Return: 1 when it did.
 */
static int e_d_jdb_start_vm_ended(void)
{
 if (!e_d_jdb_vm.launched)
  return(0);
 return(e_d_jdb_vm_gone());
}

/**
 * e_d_jdb_start_read - jdb's start: read what it said and act on it.
 * @text: what jdb has said so far.
 *
 * Its listen line starts the VM; its first prompt after the VM started
 * ends the start, and the session goes on.
 */
static void e_d_jdb_start_read(const char *text)
{
 if (!e_d_jdb_vm.launched)
 {
  e_d_jdb_start_on_listen_line(text);
  return;
 }
 if (e_d_jdb_start_vm_connected(text))
  e_d_start_answered();
}

/**
 * e_d_start_on_tick - The heartbeat: turn the waiting line, apply the
 * deadline, and finish a debugger that is being ended.
 * @data: unused.
 *
 * It runs while the debugger is silent too, so the seconds on the waiting
 * line keep counting and a debugger that never answers is ended at its
 * deadline.
 */
static void e_d_start_on_tick(void *data)
{
 (void)data;
 if (!e_d_start.active || e_d_start.held)
  return;
 if (e_d_start_adapter_gone())
 {
  e_d_start_end();
  return;
 }
 if (e_d_start.ending != E_D_START_WAITING)
  return;              /* the owner's ladder ends it, and reports it */
 if (e_d_jdb_start_vm_ended())
 {
  e_d_jdb_start_show_vm_words(E_D_PTY_READS_ALL);
  e_d_start_stop(E_D_START_VM_GONE);
  return;
 }
 e_d_start_check_deadline();
 if (e_d_start.ending == E_D_START_WAITING)
  e_d_start_wait_line_turn();
}

/**
 * e_d_start_heartbeat_start - Start the timer that turns the waiting line.
 *
 * Present only while a debugger starts, so the idle editor still sleeps until
 * a key.
 */
static void e_d_start_heartbeat_start(void)
{
 e_d_start.hb_timer = wpe_timer_add(E_D_START_TICK_NS / WPE_TIMER_NS_PER_MS, 1,
                                    e_d_start_on_tick, NULL);
 if (e_d_start.hb_timer < 0)
  e_d_start.hb_timer = WPE_TIMER_NONE;
}

/**
 * e_d_start_cancel - Esc while the debugger starts: end it.
 *
 * The debugger gets SIGTERM, then SIGKILL if it is still there about two
 * seconds later, and Messages says "<debugger> start cancelled".  A
 * debugger that already answered behind a dialog is not cancelled: the
 * session goes on.
 */
void e_d_start_cancel(void)
{
 if (!e_d_start.active)
  return;
 if (e_d_start.held)
 {
  e_d_start_deliver_held(NULL);
  return;
 }
 e_d_start_stop(E_D_START_CANCELLED);
}

/**
 * e_d_start_begin - Note that a debugger start waits from now on.
 * @f: a window of the desktop; @dap: 1 for a debug adapter.
 */
static void e_d_start_begin(FENSTER *f, int dap)
{
 e_d_start.active = 1;
 e_d_start.dap = dap;
 e_d_start.opened_messages = 0;
 e_d_start.ending = E_D_START_WAITING;
 e_d_start.held = 0;
 e_d_start.wait_row = 0;
 e_d_start.timeout = e_d_start_timeout();
 e_d_start.start = time(NULL);
 e_d_start.cn = f->ed;
}

/**
 * e_d_start_show - Start the heartbeat and show the waiting line, so the
 * user sees at once that the debugger starts and that Esc ends it.
 */
static void e_d_start_show(void)
{
 e_d_start_heartbeat_start();
 e_d_start_wait_line_put();
 e_messages_show_tail(e_d_start.cn);
}

/**
 * e_d_start_watch_fd - Read a starting debugger's descriptor on the fd loop.
 * @fd: its output, the adapter's connection, or its program's pty;
 * @cb: what reads it.
 *
 * When the loop has no room for it the debugger's answer would never be
 * read and the waiting line would stand until the deadline: the bottom row
 * says why, the start is ended at once, and Messages says it was given up.
 * Called once the heartbeat runs, so the end goes the way Esc's does.
 * Return: 0, or -1 when the start is being ended.
 */
static int e_d_start_watch_fd(int fd, wpe_fd_callback cb)
{
 if (e_fd_watch(fd, POLLIN, cb, NULL, "the debugger's output") == 0)
  return(0);
 e_d_start_stop(E_D_START_UNWATCHED);
 return(-1);
}

/**
 * e_d_start_watch - Wait for the debugger's answer without holding the editor.
 * @f: the source window.
 *
 * From here the start goes on in e_d_start_on_output and e_d_start_on_tick;
 * the editor takes keys at once.
 */
static void e_d_start_watch(FENSTER *f)
{
 e_d_start_begin(f, 0);
 e_d_async_reset();
 e_d_start_show();
 e_d_start.out_fd = wfildes[0];
 if (e_deb_type == DEB_JDB)
 {
  e_d_jdb_start_begin();
  e_d_start_watch_fd(e_d_start.out_fd, e_d_jdb_start_on_output);
  return;
 }
 e_d_start_watch_fd(e_d_start.out_fd, e_d_start_on_output);
}

/* ---- Starting a debug adapter (Go, Rust, Scala) -------------------------
 *
 * The same start as a pipe debugger's, with the same waiting line, Esc,
 * deadline and refused keys; what is awaited is the adapter's whole
 * handshake (initialize, launch, breakpoints, configurationDone), which the
 * engine runs one answer at a time as its descriptor becomes readable.
 * Each transport gives one descriptor to watch: the pipe a stdio adapter
 * (gdb, lldb-dap) writes to; the socket of the Scala adapter the build
 * server hosts; and for dlv, which dials back in, the listening socket
 * until it has, then the connection.  dlv's pty, where its build and the
 * program write, is read meanwhile so neither stalls on it.
 */

static void e_d_dap_start_on_readable(int fd, void *data);

/**
 * e_d_dap_start_follow_fd - Watch the descriptor the adapter's start waits
 * on now.
 *
 * It changes once, when dlv dials in and the listening socket gives way to
 * the connection.
 * Return: 0, or -1 when the loop could not watch it and the start is being
 * ended (the session may be gone already: the caller stops there).
 */
static int e_d_dap_start_follow_fd(void)
{
 int fd = e_dap_start_fd(g_dap);

 if (fd == e_d_start.out_fd)
  return(0);
 if (e_d_start.out_fd != E_D_START_NO_FD)
  wpe_fd_del(e_d_start.out_fd);
 e_d_start.out_fd = fd;
 if (fd == E_D_START_NO_FD)
  return(0);
 return(e_d_start_watch_fd(fd, e_d_dap_start_on_readable));
}

/**
 * e_d_dap_start_on_readable - The adapter dialled in or wrote: take its
 * answers.
 * @fd: the start's descriptor; @data: unused.
 *
 * The handshake moves on with every answer; when it is done the session
 * goes on.  An adapter that hung up, or refused a step (dlv's launch when
 * the package does not build), ends the start; the adapter is then ended by
 * its PID with the session.
 */
static void e_d_dap_start_on_readable(int fd, void *data)
{
 int st;

 (void)fd;
 (void)data;
 if (!e_d_start.active || e_d_start.ending != E_D_START_WAITING || !g_dap)
  return;
 st = e_dap_start_pump(g_dap);
 if (st == E_DAP_START_FAILED)
 {
  e_d_dap_keep_start_error();     /* a refusal is shown; a hang-up is boxed */
  e_d_start_end();
  return;
 }
 if (st == E_DAP_START_READY)
 {
  e_d_start_answered();
  return;
 }
 if (e_d_dap_start_follow_fd() < 0)
  return;
 e_d_start_check_deadline();
}

/**
 * e_d_dap_start_on_program_output - dlv's pty has output: show it in
 * Messages, above the waiting line.
 * @fd: the pty; @data: unused.
 */
static void e_d_dap_start_on_program_output(int fd, void *data)
{
 (void)data;
 if (!e_d_start.active || !g_dap)
  return;
 e_d_start_wait_line_drop();
 e_dap_drain_output(g_dap);
 if (e_dap_output_fd(g_dap) != fd)
 {
  wpe_fd_del(fd);                      /* the pty is closed: the adapter ended */
  e_d_start.pty_fd = E_D_START_NO_FD;
 }
 if (e_d_start.ending == E_D_START_WAITING)
  e_d_start_wait_line_turn();
}

/**
 * e_d_dap_start_messages - Make sure Messages is open for the waiting line.
 * @f: the source window.
 *
 * Go and Scala build nothing in the editor first, so Messages may not be
 * open yet; the waiting line, and Esc's promise, must be seen.
 */
static int e_d_dap_start_messages(FENSTER *f)
{
 if (e_build_messages(f->ed))
  return(0);
 e_edit(f->ed, "Messages");
 return(e_build_messages(f->ed) != NULL);
}

/**
 * e_d_messages_empty - Does Messages hold nothing at all?
 * @b: its buffer.
 */
static int e_d_messages_empty(BUFFER *b)
{
 return(b->mxlines <= 1 && b->bf[0].len == 0);
}

/**
 * e_d_window_index - The desktop slot that holds @w, or 0.
 * @cn: the desktop; @w: a window.
 */
static int e_d_window_index(ECNT *cn, FENSTER *w)
{
 int i;

 for (i = cn->mxedt; i > 0; i--)
  if (cn->f[i] == w)
   return(i);
 return(0);
}

/**
 * e_d_dap_start_close_messages - Close the Messages window the start opened
 * only for its waiting line.
 * @cn: the desktop.
 *
 * Messages and Watches share the bottom strip: an empty Messages left open
 * would hide the Watches a user added before Run, which the first stop
 * fills.  A Messages that received any output stays open.
 */
static void e_d_dap_start_close_messages(ECNT *cn)
{
 FENSTER *mf = e_build_messages(cn);
 FENSTER *top = cn->f[cn->mxedt];
 int i;

 if (!e_d_start.opened_messages || !mf || !e_d_messages_empty(mf->b))
  return;
 e_d_start.opened_messages = 0;
 i = e_d_window_index(cn, mf);
 e_switch_window(cn->edt[i], cn->f[cn->mxedt]);
 e_close_window(cn->f[cn->mxedt]);
 if (top == mf)
  return;
 i = e_d_window_index(cn, top);
 if (i > 0 && i != cn->mxedt)
  e_switch_window(cn->edt[i], cn->f[cn->mxedt]);
}

/**
 * e_d_dap_start_watch - Wait for the adapter's handshake without holding the
 * editor.
 * @f: the source window.
 *
 * From here the start goes on in e_d_dap_start_on_readable and
 * e_d_start_on_tick; the editor takes keys at once.
 * Return: E_D_STARTING; the command pressed runs again once the adapter has
 * answered.
 */
static int e_d_dap_start_watch(FENSTER *f)
{
 ECNT *cn = f->ed;
 int opened = e_d_dap_start_messages(f);

 e_d_start_begin(cn->f[cn->mxedt], 1);
 e_d_start.opened_messages = opened;
 e_d_start_show();                    /* the heartbeat first: a refused fd ends the start */
 if (e_d_dap_start_follow_fd() < 0)
  return(E_D_STARTING);
 e_d_start.pty_fd = e_dap_output_fd(g_dap);
 if (e_d_start.pty_fd != E_D_START_NO_FD)
  e_d_start_watch_fd(e_d_start.pty_fd, e_d_dap_start_on_program_output);
 return(E_D_STARTING);
}

/**
 * e_d_resume_after_start - Name the command to run once the debugger has
 * answered.
 * @cmd: the command; @arg: its argument.
 *
 * A key refused during the start does not replace the command that started
 * it.
 */
static void e_d_resume_after_start(e_d_resume_fn cmd, int arg)
{
 if (e_d_start.active)
  return;
 e_d_start_cmd = cmd;
 e_d_start_arg = arg;
}

/**
 * e_d_deferred - Did e_run_debug leave the command to run later?
 * @ret: e_run_debug's answer.
 *
 * True while the build the debugger needs runs, and while the debugger it
 * started has not answered: the command runs again when they are done.
 */
static int e_d_deferred(int ret)
{
 if (ret == E_D_BUILDING)
  return(1);
 return(ret == E_D_STARTING);
}

/**
 * e_d_resume_after_build - Name the command to run again once the build the
 * debugger starts with has succeeded.
 * @cmd: the command; @arg: its argument.
 */
static void e_d_resume_after_build(e_d_resume_fn cmd, int arg)
{
 e_d_resume_after_start(cmd, arg);
 if (e_d_swtch || e_build_busy())
  return;                 /* already debugging, or a build owns the resume */
 e_d_resume_cmd = cmd;
 e_d_resume_arg = arg;
}

/* A line the status row owes the user once the debugger's bar is up. */
static const char *e_d_pending_status;

/**
 * e_d_say_pending_status - Name the debugger picked for the file's language.
 * @f: the window on top.
 *
 * Said once the debugger's bar is up and kept on the row until the next
 * key: the start goes on by itself (breakpoints, Watches, the first stop)
 * and each of those repaints would put the key hints back before the user
 * could read which debugger runs.
 */
static void e_d_say_pending_status(FENSTER *f)
{
 if (!e_d_pending_status)
  return;
 e_status_say_until_key(f, e_d_pending_status);
 e_d_pending_status = NULL;
}

/**
 * e_d_gdbmi_pick - Choose the interface this session's gdb speaks, from
 * XWPE_GDB_INTERFACE, and the switch gdb is started with for it.
 *
 * Unset or "cli" keeps the line interface; "mi" asks for MI; anything else
 * keeps the line interface and the status row says why, so a typo does not
 * pass unnoticed.  A gdb found too old for MI at the start of this session
 * keeps the line interface for the session started again in its place
 * (e_d_gdbmi_fall_back), with its own status line.
 * Return: the switch, or NULL for the line interface.
 */
static char *e_d_gdbmi_pick(void)
{
 const char *want = getenv(E_D_GDBMI_ENV);

 e_d_mi.on = 0;
 e_d_mi.token = 0;
 e_d_mi.pending = 0;
 if (e_d_mi.force_cli)
 {
  e_d_mi.force_cli = 0;
  e_d_pending_status = e_d_mi.say;
  return(NULL);
 }
 if (!want || !want[0] || !strcmp(want, E_D_GDBMI_WORD_CLI))
  return(NULL);
 if (strcmp(want, E_D_GDBMI_WORD_MI))
 {
  snprintf(e_d_mi.say, sizeof e_d_mi.say, E_D_GDBMI_UNKNOWN_FMT, want);
  e_d_pending_status = e_d_mi.say;
  return(NULL);
 }
 /* The mark goes to the innermost frame whose file is here, asked of the
    file system through the session's probe: set it now, since the first
    session of the editor starts before any session has ended, and with no
    probe a Pause in a library would mark the library's frame. */
 e_d_session_forget();
 e_d_mi.on = 1;
 return(E_D_GDBMI_SWITCH);
}

/** Message shown when the source's path is too long to hand to a debugger. */
#define E_D_PATH_TOO_LONG_LINE "Debugger not started: the file path is too long"

/**
 * e_d_exe_name_of - Name of the program built from a source file: the name
 * without its extension, plus ".e" unless the compiler adds none (fpc).
 *
 * @out: destination, @n: its size, @datnam: the source file name.
 * Return: 0 on success, -1 when the result does not fit in @n.
 */
static int e_d_exe_name_of(char *out, size_t n, const char *datnam)
{
 if (e_d_copy_arg(out, n, datnam))
  return(-1);
 WpeStringCutChar(out, '.');
 /* Non-GNU compilers (fpc) produce executables without .e extension.
    GNU compilers use the .e convention from xwpe's link step. */
 if (!(e_s_prog.comp_sw & 1))
 {
  if (strlen(out) + 3 > n)
  {
   out[0] = '\0';
   return(-1);
  }
  strcat(out, ".e");
 }
 return(0);
}

/**
 * e_d_pick_start_arg - Choose the argument the debugger is started on when
 * no project is open: the built program, or for pdb/a68g the source's path.
 *
 * @cn: the editor's windows, @f: the file being debugged, @estr/@n: output.
 * Return: 0 on success, -1 when the path does not fit (nothing is started).
 */
static int e_d_pick_start_arg(ECNT *cn, FENSTER *f, char *estr, size_t n)
{
 int _fi;

 if (e_s_prog.exe_name && e_s_prog.exe_name[0])
  return(e_d_copy_arg(estr, n, e_s_prog.exe_name));
 if (e_deb_type != DEB_PDB && e_deb_type != DEB_A68G)
  return(e_d_exe_name_of(estr, n, f->datnam));
 /* pdb/a68g: debug the source file (.py/.a68) with full path.
    e_d_file has the basename; we need dirct + datnam for the
    full path so pdb can find it from any working directory. */
 for (_fi = cn->mxedt; _fi > 0; _fi--)
  if (e_check_c_file_w(cn->f[_fi])) break;
 if (_fi > 0)
  return(e_d_debuggee_path(estr, n, cn->f[_fi]->dirct, cn->f[_fi]->datnam));
 return(e_d_copy_arg(estr, n, e_d_file));
}

int e_start_debug(FENSTER *f)
{
 ECNT *cn = f->ed;
 int i, file;
 char estr[PATH_MAX];

 e_d_pending_status = NULL;
 efildes[0] = efildes[1] = -1;
 wfildes[0] = wfildes[1] = -1;
 rfildes[0] = rfildes[1] = -1;
 if (e_d_swtch)
  return(0);
 /* Find the source file window -- f may point to Messages if the
    cursor is there from a previous compile.  Same fix as #45. */
 for (i = cn->mxedt; i > 0; i--)
  if (strcmp(cn->f[i]->datnam, "Messages") && DTMD_ISTEXT(cn->f[i]->dtmd))
   break;
 if (i > 0)
 {
  e_switch_window(cn->edt[i], cn->f[cn->mxedt]);
  f = cn->f[cn->mxedt];
 }
 /* DAP-debugged languages (Go, ...) are built and launched by the adapter
    (e.g. `dlv dap` compiles the package itself), so we bypass xwpe's compiler
    pipeline entirely -- no e_p_make, no e_exec_deb.  Scan ALL windows for the
    source file: the loop above can settle on a debug pane (the Watches and Stack
    windows are DTMD_ISTEXT too, and one of them is topmost when the user adds a
    watch before pressing Run), so trusting the focused window would mis-route a
    .go session into the gdb path.  Find a .go window explicitly and debug it. */
 for (i = cn->mxedt; i > 0; i--)
  if (e_d_dap_lang_for(cn->f[i]))
   return(e_d_dap_start(cn->f[i]));
 if (!e_d_build_done)
  return(e_d_build_first(f));
 e_d_build_done = 0;
 /* Full path (e_check_c_file_w), not bare datnam: the dialect sniff must open
    the source so e_s_prog matches what e_p_make just built.  With a bare name
    for a file outside the cwd the sniff fails, e_s_prog falls back to a68g
    (comp_sw 1), and the gdb branch below then omits the ".e" suffix -- gdb is
    launched on a non-existent binary ("No symbol table is loaded"). */
 for (i = cn->mxedt; i > 0; i--)
  if (e_check_c_file_w(cn->f[i]))
   break;
 if (i > 0)
 {
  if (e_d_copy_arg(e_d_file, sizeof(e_d_file), cn->f[i]->datnam))
  {
   e_error(E_D_PATH_TOO_LONG_LINE, 0, f->fb);
   return(-1);
  }
  f = cn->f[i];
 }
 else if (e__project)
 {
  if (e_d_copy_arg(e_d_file, sizeof(e_d_file), cn->f[cn->mxedt-1]->datnam))
  {
   e_error(E_D_PATH_TOO_LONG_LINE, 0, f->fb);
   return(-1);
  }
  f = cn->f[cn->mxedt-1];
 }
 e_d_out_clear();
 /* Auto-select debugger by file extension.
    gdb cannot debug Java bytecode or Python scripts.  The switch is said on
    the status row, not in a box: the user asked to run, and a box would make
    him dismiss it before the very command he pressed does anything. */
 { int _l = strlen(e_d_file);
   if (_l > 5 && !strcmp(e_d_file + _l - 5, ".java"))
   {
    if (e_deb_type != DEB_JDB)
     e_d_pending_status = E_D_SWITCH_JDB_LINE;
    e_deb_type = DEB_JDB;
   }
   else if (_l > 3 && !strcmp(e_d_file + _l - 3, ".py"))
   {
    if (e_deb_type != DEB_PDB)
     e_d_pending_status = E_D_SWITCH_PDB_LINE;
    e_deb_type = DEB_PDB;
   }
   else if ((_l > 4 && !strcmp(e_d_file + _l - 4, ".a68")) ||
            (_l > 4 && !strcmp(e_d_file + _l - 4, ".alg")))
   {
    /* Two Algol 68 toolchains: ga68 (a GCC front-end -> native binary, so
       gdb debugs the compiled .e, breaking at __algol68_main) and a68g (the
       Genie interpreter -> its own --monitor on the source).  Follow the
       configured compiler so the debugger matches what built the program.
       Auto-selected silently -- no modal notice, which would block the very
       Ctrl-G R the user just pressed.  e_algol68_use_ga68_in sniffs the file's
       full path (dir + name), so a source outside the current directory is
       still detected correctly. */
    e_deb_type = e_algol68_use_ga68_in(f->dirct, e_d_file) ? DEB_GDB : DEB_A68G;
   }
 }
 jdb_trace("e_start_debug: e_d_file='%s', e_deb_type=%d, comp_sw=%d\n",
           e_d_file, e_deb_type, e_s_prog.comp_sw);
 if (e_deb_type == DEB_SDB) {  e_debugger = "sdb";  e_deb_swtch = NULL;  }
 else if (e_deb_type == DEB_DBX) {  e_debugger = "dbx";  e_deb_swtch = "-i";  }
 else if (e_deb_type == DEB_XDB) {  e_debugger = "xdb";  e_deb_swtch = "-L";  }
 else if (e_deb_type == DEB_JDB) {  e_debugger = "jdb";  e_deb_swtch = NULL;  }
 else if (e_deb_type == DEB_PDB) {  e_debugger = "python3";  e_deb_swtch = NULL;  }
 else if (e_deb_type == DEB_A68G) {  e_debugger = "a68g";  e_deb_swtch = "--monitor";  }
 else {  e_debugger = "gdb";  e_deb_swtch = e_d_gdbmi_pick();  }
 if (e_test_command(e_debugger))
 {
  snprintf(estr, sizeof(estr), "Debugger \'%s\' not in Path", e_debugger);
  e_error(estr, 0, f->fb);
  return(-1);
 }
 /* Note: e_sys_ini()/e_sys_end() were here originally but they cause
    screen flicker (rmcup/smcup) which is unnecessary for the gdb fork.
    The pty handles program output; no screen switching needed. */
 if (e__project && (file = e_exec_deb(f, e_s_prog.exe_name )) == 0)
 {
  return(-2);
 }
 else if (!e__project)
 {
  if (e_d_pick_start_arg(cn, f, estr, sizeof(estr)))
  {
   e_error(E_D_PATH_TOO_LONG_LINE, 0, f->fb);
   return(-1);
  }
  jdb_trace("e_start_debug: launching '%s %s'\n", e_debugger, estr);
  if ((file = e_exec_deb(f, estr)) == 0)
  {
   jdb_trace("e_start_debug: e_exec_deb failed\n");
   return(-2);
  }
 }
 jdb_trace("e_start_debug: debugger launched OK, e_d_swtch=%d\n", e_d_swtch);
 e_d_p_message(e_d_msg[ERR_STARTDEBUG], f, 1);
 WpeMouseChangeShape(WpeDebuggingShape);
 if (cn->mxedt > 1)
  e_switch_window(cn->edt[cn->mxedt - 1], cn->f[cn->mxedt]);
 return(0);
}

/**
 * e_d_set_nonblock - Make one of the debugger's pipes non-blocking.
 * @fd: the pipe's read end, or -1.
 */
static void e_d_set_nonblock(int fd)
{
 int flags = fcntl(fd, F_GETFL, 0);

 fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/**
 * e_d_start_take_focus - Give the graphical editor's window the keyboard
 * back once the debugger is set up, so the user types into the editor and
 * not into the window the debugger came up behind.
 */
static void e_d_start_take_focus(void)
{
#ifndef NO_XWINDOWS
 if (WpeIsXwin())
  e_d_x_focus_to_editor();
#endif
}

/**
 * e_run_debug - Make sure a debugger session is running.
 * @f: the window the debugger command was pressed in.
 *
 * Builds first when needed and starts the debugger; the editor does not wait
 * for its banner (see e_d_start_watch).  While a start waits, a second
 * debugger command is refused on the status row.
 * Return: 0 when the session runs, E_D_BUILDING or E_D_STARTING when the
 * command will be run again later, or a negative error.
 */
int e_run_debug(FENSTER *f)
{
 int ret;

 jdb_trace("e_run_debug: entry, e_d_swtch=%d, e_deb_type=%d\n",
           e_d_swtch, e_deb_type);
 if (e_d_start_refused(f))
  return(E_D_STARTING);
 if (e_d_swtch < 1 && (ret = e_start_debug(f)) < 0) return(ret);
 /* From here a session is open, so the source window's bottom row names the
    debugger's keys instead of the editing ones. */
 e_refresh_editor_bars(f->ed);
 e_d_say_pending_status(f);
 if (e_d_swtch < 2 && e_d_ga_set_up_again(f))
  return(E_D_STARTING);
 if (e_d_swtch < 2)
 {
  e_d_set_nonblock(efildes[0]);
  e_d_set_nonblock(wfildes[0]);
  jdb_trace("e_run_debug: waiting for the banner on the fd loop\n");
  e_d_start_watch(f);
  return(E_D_STARTING);
 }
 return(0);
}

/* jdb reports a stop as  Breakpoint hit: "thread=main", P.main(), line=6 bci=0
   or  Step completed: "thread=main", P.sq(), line=3 bci=0 : the class and
   method come before the line, and the class names the source file.  The
   readers of it (E_D_JDB_LINE_MARK, E_D_JDB_NESTED_SEP) are in we_debsess.c. */
#define E_D_JDB_SOURCE_EXT  ".java"
#define E_D_STOP_PATH_MAX   E_DS_PATH_MAX

/**
 * e_d_window_path_of - The full path of the open window that shows @name.
 * @cn: the editor; @name: a file name without directory;
 * @out, @n: receives the directory and the name.
 *
 * Lets a stop that names only a file find that file's window wherever it
 * was opened from, not only in the current directory.
 * Return: 1 when such a window is open, else 0.
 */
static int e_d_window_path_of(ECNT *cn, const char *name, char *out, size_t n)
{
 int i, len;

 for (i = cn->mxedt; i > 0; i--)
 {
  if (strcmp(cn->f[i]->datnam, name))
   continue;
  len = strlen(cn->f[i]->dirct);
  if (len > 0 && cn->f[i]->dirct[len - 1] == DIRC)
   snprintf(out, n, "%s%s", cn->f[i]->dirct, name);
  else
   snprintf(out, n, "%s%c%s", cn->f[i]->dirct, DIRC, name);
  return(1);
 }
 return(0);
}

/* The line and the class a jdb stop names are read by e_ds_jdb_stop_line
   and e_ds_jdb_stop_class (we_debsess.c): a stop past line 999 keeps its
   digit groups, a nested class P$Inner gives P. */

/**
 * e_d_jdb_class_file - The source file name of a class: com.x.P -> P.java.
 * @cls: the class with its package; @out, @n: receives the file name.
 */
static void e_d_jdb_class_file(const char *cls, char *out, size_t n)
{
 const char *simple = strrchr(cls, '.');

 if (simple)
  simple++;
 else
  simple = cls;
 snprintf(out, n, "%s%s", simple, E_D_JDB_SOURCE_EXT);
}

/**
 * e_d_jdb_package_file - The source path of a class below its source root:
 * com.x.P -> com/x/P.java.
 * @cls: the class with its package; @out, @n: receives the path.
 */
static void e_d_jdb_package_file(const char *cls, char *out, size_t n)
{
 size_t i, len;

 snprintf(out, n, "%s", cls);
 for (i = 0; out[i]; i++)
  if (out[i] == '.')
   out[i] = DIRC;
 len = strlen(out);
 snprintf(out + len, n - len, "%s", E_D_JDB_SOURCE_EXT);
}

/**
 * e_d_jdb_session_dir - The directory of the Java file the session debugs.
 * @cn: the editor; @out, @n: receives the directory, or "" when that file
 * is not open.
 */
static void e_d_jdb_session_dir(ECNT *cn, char *out, size_t n)
{
 char *slash;

 out[0] = '\0';
 if (!e_d_window_path_of(cn, e_d_file, out, n))
  return;
 slash = strrchr(out, DIRC);
 if (slash)
  slash[1] = '\0';
 else
  out[0] = '\0';
}

/**
 * e_d_jdb_try_source - Is @rel readable under @dir?  ("" is the current
 * directory.)
 * @dir: a source root; @rel: a path below it; @out, @n: receives the path.
 * Return: 1 when the file is there.
 */
static int e_d_jdb_try_source(const char *dir, const char *rel,
                              char *out, size_t n)
{
 snprintf(out, n, "%s%s", dir, rel);
 return(access(out, R_OK) == 0);
}

/**
 * e_d_jdb_find_source - Where the source of the class a jdb stop names is.
 * @cn: the editor; @cls: the class with its package; @out, @n: the path.
 *
 * An open window showing the file wins; otherwise the file is looked for
 * below the session's source roots, the directory of the Java file being
 * debugged and the current directory, first in its package's directories
 * (com/x/P.java), then directly.  javac finds a package's sources the same
 * way, so a class it compiled is found here too.
 * Return: 1 when the source was found, else 0.
 */
static int e_d_jdb_find_source(ECNT *cn, const char *cls, char *out, size_t n)
{
 char name[E_D_STOP_PATH_MAX], rel[E_D_STOP_PATH_MAX], dir[E_D_STOP_PATH_MAX];

 /* A Java class name never holds these; a stop line that does is not
    turned into a path outside the source roots. */
 if (strchr(cls, DIRC) || strstr(cls, ".."))
  return(0);
 e_d_jdb_class_file(cls, name, sizeof name);
 if (e_d_window_path_of(cn, name, out, n))
  return(1);
 e_d_jdb_package_file(cls, rel, sizeof rel);
 e_d_jdb_session_dir(cn, dir, sizeof dir);
 if (dir[0] && e_d_jdb_try_source(dir, rel, out, n))
  return(1);
 if (e_d_jdb_try_source("", rel, out, n))
  return(1);
 if (dir[0] && e_d_jdb_try_source(dir, name, out, n))
  return(1);
 return(e_d_jdb_try_source("", name, out, n));
}

/* Classes whose source was not found, each said once per session. */
#define E_D_JDB_MISSING_MAX 16
#define E_D_JDB_NO_SOURCE_FMT "%s: source not found"
static char e_d_jdb_missing[E_D_JDB_MISSING_MAX][E_D_STOP_PATH_MAX];
static int e_d_jdb_nmissing = 0;

/**
 * e_d_jdb_missing_forget - Forget which classes had no source, for the
 * next session.
 */
static void e_d_jdb_missing_forget(void)
{
 e_d_jdb_nmissing = 0;
}

/**
 * e_d_jdb_missing_seen - Was this class already said to have no source?
 * @cls: the class; it is remembered when it was not.
 * Return: 1 when it was already said (or no room is left to remember it).
 */
static int e_d_jdb_missing_seen(const char *cls)
{
 int i;

 for (i = 0; i < e_d_jdb_nmissing; i++)
  if (!strcmp(e_d_jdb_missing[i], cls))
   return(1);
 if (e_d_jdb_nmissing >= E_D_JDB_MISSING_MAX)
  return(1);
 snprintf(e_d_jdb_missing[e_d_jdb_nmissing++], E_D_STOP_PATH_MAX, "%s", cls);
 return(0);
}

/**
 * e_d_jdb_say_no_source - Say once that a class has no source to mark.
 * @f: the window on top; @cls: the class jdb stopped in.
 *
 * A stop in a class whose file cannot be found (a library, a second class
 * written in another class's file) leaves the session running: the status
 * row says so the first time, and later stops in it stay quiet, where every
 * stop used to raise a dialog.
 */
static void e_d_jdb_say_no_source(FENSTER *f, const char *cls)
{
 char line[E_D_STOP_PATH_MAX + sizeof E_D_JDB_NO_SOURCE_FMT];

 e_d_switch_out(0);
 if (e_d_jdb_missing_seen(cls))
  return;
 snprintf(line, sizeof line, E_D_JDB_NO_SOURCE_FMT, cls);
 e_status_say(f, line);
}

/**
 * e_d_jdb_mark_stop - Mark the line jdb stopped on in its source window.
 * @f: the window the command came from; @stop: what jdb printed.
 *
 * The user sees the green bar in the Java file jdb names, found by name as
 * the gdb stop finds its file, never in whichever window is on top
 * (Messages after the build, Watches after a watch was added).  jdb names a
 * class where gdb names a file: that is the one difference, resolved here.
 * A class in a package is opened from its package's directory.  The
 * watches of the stop were read before, on the fd loop (e_d_gj_stop_ask).
 * Return: e_d_goto_break's answer, or 0 when the stop names no line or its
 * source is not found.
 */
static int e_d_jdb_mark_stop(FENSTER *f, const char *stop)
{
 char cls[E_D_STOP_PATH_MAX], path[E_D_STOP_PATH_MAX];
 int line = e_ds_jdb_stop_line(stop);

 if (line <= 0)
  return(0);
 e_ds_take_cli_stop_line(&e_d_session, DEB_JDB, stop);
 if (!e_ds_jdb_stop_class(stop, cls, sizeof cls))
 {
  snprintf(cls, sizeof cls, "%s", e_d_file);
  WpeStringCutChar(cls, '.');
 }
 if (!e_d_jdb_find_source(f->ed, cls, path, sizeof path))
 {
  e_d_jdb_say_no_source(f, cls);
  return(0);
 }
 return(e_d_goto_break(path, line, f));
}

/**
 * e_d_pdb_is_file - Say whether a file pdb named exists as a regular file:
 * the file probe the stop readers of we_debsess.c ask.
 * @file: the name; @ctx: unused.
 *
 * pdb names the program's files by their real path, so a stop that names
 * one is pdb's own, whatever else on the line looks like a stop ("> name(3)"
 * in a value or in the program's output, a directory "a b(1)/").
 */
static int e_d_pdb_is_file(const char *file, void *ctx)
{
 struct stat st;

 (void)ctx;
 return(stat(file, &st) == 0 && S_ISREG(st.st_mode));
}

/**
 * e_d_session_forget - The debugging session ended: the session state
 * starts over, asking the file system as it is.
 *
 * Nothing of the last session (a stop, a frame, a value, an answer) is
 * then taken for the next one's.
 */
static void e_d_session_forget(void)
{
 e_ds_reset(&e_d_session);
 e_ds_set_file_probe(&e_d_session, e_d_pdb_is_file, NULL);
}

/**
 * e_d_pdb_stop_parse - Read a stop line in pdb's grammar at @p, with the
 * file system as it is: see e_ds_pdb_stop_parse.
 */
static int e_d_pdb_stop_parse(const char *p, char *file, size_t n, int *line)
{
 return(e_ds_pdb_stop_parse(p, file, n, line, e_d_pdb_is_file, NULL));
}

/**
 * e_d_pdb_find_stop - Where pdb names its stop in a part of its answer,
 * with the file system as it is: see e_ds_pdb_find_stop.
 */
static const char *e_d_pdb_find_stop(const char *text)
{
 return(e_ds_pdb_find_stop(text, e_d_pdb_is_file, NULL));
}

/**
 * e_d_pdb_step_place - Where a pdb step or continue stopped, from pdb's
 * stop line, kept in the session too.
 * @stop: pdb's stop line, as e_d_pdb_find_stop found it; @file, @n: the
 * file it names.
 *
 * After F7 or F8 the green bar moves in the file pdb names, not in the
 * window on top (Watches, once a watch was added).
 * Return: the line, or 0 when the line names no place.
 */
static int e_d_pdb_step_place(const char *stop, char *file, size_t n)
{
 int line;

 if (!e_d_pdb_stop_parse(stop, file, n, &line))
  return(0);
 e_ds_set_file_probe(&e_d_session, e_d_pdb_is_file, NULL);
 e_ds_take_cli_stop_line(&e_d_session, DEB_PDB, stop);
 e_d_swtch = 3;
 return(line);
}

/* A debugger's answer to one step or continue, gathered by the readers of
   jdb, pdb and a68g until the prompt that ends it.  A program can print
   more in one step than the buffer holds: the oldest part is then handed on
   (as program output) and reading goes on, while the newest E_D_STEP_KEEP
   bytes, where the stop line and the prompt are, always stay. */
typedef struct {
 char buf[DEB_STEP_BUF];
 int  len;
 int  mid_line;          /* buf starts inside a line whose start was handed on */
} E_D_STEP_REPLY;

#define E_D_STEP_KEEP  E_D_LINE_MAX
#if DEB_STEP_BUF - 1 <= E_D_STEP_KEEP
#error "a step answer must hold more than the part of it that is always kept"
#endif

/* Receives the part of a step answer that is handed on: @text, a string,
   ends at a line end or inside a line longer than the kept part; @mid_line
   says it starts inside a line whose start was handed on before. */
typedef void (*E_D_STEP_SPILL)(void *ctx, char *text, int mid_line);

/**
 * e_d_step_reply_init - Start gathering a new step answer.
 */
static void e_d_step_reply_init(E_D_STEP_REPLY *r)
{
 r->len = 0;
 r->mid_line = 0;
 r->buf[0] = '\0';
}

/**
 * e_d_step_reply_cut - Where a full step answer is split: after the last line
 * end before the kept part, or where the kept part starts when one line is
 * longer than all the rest.
 */
static int e_d_step_reply_cut(const E_D_STEP_REPLY *r)
{
 int keep_from = r->len - E_D_STEP_KEEP, k;

 for (k = keep_from; k > 0; k--)
  if (r->buf[k - 1] == '\n')
   return(k);
 return(keep_from);
}

/**
 * e_d_step_reply_make_room - Hand the oldest part of a full step answer to
 * @spill and keep the rest, so reading goes on.
 * @r: the answer; @spill, @ctx: who takes the oldest part (NULL drops it).
 *
 * A step whose program prints more than the buffer holds would otherwise
 * read nothing more, and nothing read looks like the debugger's end: the
 * session would be ended under the user.
 */
static void e_d_step_reply_make_room(E_D_STEP_REPLY *r, E_D_STEP_SPILL spill,
                                     void *ctx)
{
 int cut = e_d_step_reply_cut(r);
 char at_cut = r->buf[cut];

 r->buf[cut] = '\0';
 if (spill)
  spill(ctx, r->buf, r->mid_line);
 r->buf[cut] = at_cut;
 r->mid_line = (r->buf[cut - 1] != '\n');
 r->len -= cut;
 memmove(r->buf, r->buf + cut, r->len);
 r->buf[r->len] = '\0';
}

/**
 * e_d_step_reply_read - Read what the debugger wrote next into a step answer,
 * making room first when the answer fills the buffer.
 * @fd: the debugger's output; @r: the answer; @spill, @ctx: as for
 * e_d_step_reply_make_room.
 * Return: what read() returned: the bytes added, 0 at the end of the stream
 * (and only then), -1 when nothing could be read.
 */
static int e_d_step_reply_read(int fd, E_D_STEP_REPLY *r, E_D_STEP_SPILL spill,
                               void *ctx)
{
 int n;

 if (r->len >= (int)sizeof(r->buf) - 1)
  e_d_step_reply_make_room(r, spill, ctx);
 n = read(fd, r->buf + r->len, sizeof(r->buf) - r->len - 1);
 if (n > 0)
  r->len += n;
 r->buf[r->len] = '\0';
 return(n);
}

/**
 * e_d_step_reply_hand_on_lines - Hand on every whole line of a step answer
 * read so far, and keep the part after the last line end.
 * @r: the answer; @spill, @ctx: who takes the lines, as for
 * e_d_step_reply_make_room.
 *
 * A program that prints while it runs shows its text as it prints, while
 * the stop line and the prompt, which end the answer, stay for the stop.
 */
static void e_d_step_reply_hand_on_lines(E_D_STEP_REPLY *r,
                                         E_D_STEP_SPILL spill, void *ctx)
{
 int cut = r->len;

 while (cut > 0 && r->buf[cut - 1] != '\n')
  cut--;
 if (cut == 0)
  return;
 r->buf[cut - 1] = '\0';
 spill(ctx, r->buf, r->mid_line);
 r->buf[cut - 1] = '\n';
 r->mid_line = 0;
 r->len -= cut;
 memmove(r->buf, r->buf + cut, r->len);
 r->buf[r->len] = '\0';
}

/**
 * e_d_text_next_line - Copy the next line of a debugger's answer, cut to the
 * size of @line, and step past the whole of it.
 * @text: the answer; @pos: where the line starts, moved past its end;
 * @line, @size: where the line goes.
 *
 * A line longer than @line loses its end instead of coming back as a line of
 * its own, which the scanners would misread (half a line of program output
 * taken for a source line number, for one).
 * Return: the length copied.
 */
static int e_d_text_next_line(const char *text, int *pos, char *line, int size)
{
 int len = 0, p = *pos;

 while (text[p] && text[p] != '\n' && len < size - 1)
  line[len++] = text[p++];
 while (text[p] && text[p] != '\n')
  p++;
 if (text[p] == '\n')
  p++;
 line[len] = '\0';
 *pos = p;
 return(len);
}

/*  Run  */

/**
 * e_d_run_continue_command - The command that lets the program run on after
 * a stop the editor made to apply a breakpoint change.
 *
 * gdb, pdb and a68g are the debuggers interrupted for one; the first run
 * of a session goes on with it too, never with "run" again.  jdb is never
 * interrupted for one, but a stop on a breakpoint the user has just
 * removed goes on with its "cont" (e_d_jdb_stale_stop).
 * Return: the command, or NULL for a debugger that has none for it.
 */
static const char *e_d_run_continue_command(void)
{
 if (e_deb_type == DEB_GDB || e_deb_type == DEB_PDB)
  return("c\n");
 if (e_deb_type == DEB_A68G)
  return("continue\n");
 if (e_deb_type == DEB_JDB)
  return(E_D_JDB_CONT);
 return(NULL);
}

/**
 * e_d_run_hand_over_continue - Hand a Run or a Continue to the controller.
 * @f: the source window; @ops: the backend's table;
 * @start_brk: the editor's start breakpoint to take out at the stop, or 0.
 *
 * A breakpoint the user sets while it runs can then be applied at once:
 * the program is interrupted, told, and runs on with the continue command.
 * Return: as e_d_run_hand_over.
 */
static int e_d_run_hand_over_continue(FENSTER *f, const E_D_RUN_OPS *ops,
                                      int start_brk)
{
 int ret = e_d_run_hand_over(f, ops, start_brk);

 e_d_run_continues_with(e_d_run_continue_command());
 return(ret);
}

int e_deb_run(FENSTER *f)
{
 ECNT *cn = f->ed;
 char eing[E_D_LINE_MAX + 128];
 int ret, prsw = 0, main_brk = 0;

 if (e_d_act_deferred(E_D_ACT_RUN, f, 0))
  return(0);
 if (e_d_refuse_while_running(f))
  return(0);
 jdb_trace("e_deb_run: entry, e_d_swtch=%d, e_deb_type=%d\n",
           e_d_swtch, e_deb_type);
 e_d_resume_after_build(e_deb_run_again, 0);
 ret = 0;
 if (e_d_swtch < 2)
  ret = e_run_debug(f);
 if (e_d_deferred(ret))
  return(0);
 if (ret < 0)
 {
  jdb_trace("e_deb_run: e_run_debug failed ret=%d, quitting\n", ret);
  e_d_quit(f);
  if (ret == -1) {  e_show_error(0, f);  return(ret);  }
  return(e_error(e_d_msg[ERR_CANTDEBUG], 0, f->fb));
 }
 /* DAP backend: e_run_debug -> e_start_debug already opened the adapter and
    stopped at the entry function.  Continue to the first user breakpoint (or
    program end) through the bridge; the pipe/pty machinery below is gdb-only. */
 if (e_deb_type == DEB_DAP)
  return(e_d_dap_run(f));
 /* Interpreted debuggers (jdb, pdb) don't need tty redirect */
 jdb_trace("e_deb_run: tty check, e_deb_type=%d, e_d_tty='%s'\n",
           e_deb_type, e_d_tty);
 if (e_deb_type != DEB_JDB && e_deb_type != DEB_PDB)
 {
  for (ret = 0; isspace(e_d_tty[ret]); ret++)
   ;
  if (e_d_tty[ret] != DIRC)
  {
   jdb_trace("e_deb_run: tty check FAILED, quitting\n");
   e_d_quit(f);
   snprintf(eing, sizeof eing, "No terminal for the program: %s", e_d_tty);
   return(e_d_error(eing));
  }
 }
 /* a68g auto-breaks at line 1, so it never needs a temporary breakpoint at
    main; calling e_mk_brk_main for it would allocate a phantom breakpoint slot
    and corrupt e_d_nbrpts across restarts (it has no a68g command branch). */
 if (e_d_swtch < 3 && e_d_nbrpts <= 0 && e_deb_type != DEB_A68G)
 {
  if ((main_brk = e_mk_brk_main(f, 0)) < -1) return(main_brk);
 }
 if (e_deb_type == DEB_DBX)
 {
  if (e_d_swtch < 3)
  {
   if (e_prog.arguments)
    snprintf(eing, sizeof eing, "run %s > %s\n", e_prog.arguments, e_d_tty);
   else
    sprintf(eing, "run > %s\n", e_d_tty);
  }
  else
  {
   strcpy(eing, "cont\n");
   prsw = 1;
  }
 }
 else
 {
  if (e_d_swtch < 3)
  {
   /* When pty is active, redirect stdout to the pty slave path.
      gdb's "set inferior-tty" doesn't deliver output to the master
      when gdb itself communicates via pipes, so we use explicit
      shell-style redirect instead. */
   if (e_deb_type == DEB_JDB)
   {
    /* jdb: the VM the editor started is already connected and held at
       its start; "cont" lets it run.  No arguments reach the program. */
    strcpy(eing, "cont\n");
   }
   else if (e_deb_type == DEB_PDB)
   {
    /* pdb: program already loaded and paused at line 1.
       "continue" runs to first breakpoint. */
    strcpy(eing, "c\n");
    prsw = 1;  /* treat as continue, not first run */
   }
   else if (e_deb_type == DEB_A68G)
   {
    /* a68g --monitor auto-breaks at line 1; "continue" runs to the
       first user breakpoint (or to program end). */
    strcpy(eing, "continue\n");
    prsw = 1;  /* treat as continue, not first run */
   }
   else if (e_d_pty_master >= 0 && e_deb_type == DEB_GDB)
   {
    if (e_prog.arguments)
     snprintf(eing, sizeof eing, "r %s\n", e_prog.arguments);
    else
     strcpy(eing, "r\n");
   }
   else
   {
    if (e_prog.arguments)
     snprintf(eing, sizeof eing, "r %s > %s\n", e_prog.arguments, e_d_tty);
    else
     sprintf(eing, "r > %s\n", e_d_tty);
   }
  }
  else
  {
   if (e_deb_type == DEB_JDB)
    strcpy(eing, e_d_jdb_continue_command());
   else if (e_deb_type == DEB_A68G)
    strcpy(eing, "continue\n");
   else
    strcpy(eing, "c\n");
   prsw = 1;
  }
 }
 f = cn->f[cn->mxedt];
 e_d_nstack = 0;
 e_d_delbreak(f);
 e_d_switch_out(1);
 /* Every pipe debugger runs on the request engine (e_d_ga_backend_of), so
    the run goes out on the fd loop and the editor keeps its keys. */
 if (prsw)
  return(e_d_ga_go(f, E_D_GO_CONTINUE, eing, main_brk));
 return(e_d_ga_go(f, E_D_GO_START, eing, main_brk));
}

int e_deb_trace(FENSTER *f)
{
 return(e_d_step_next(f, 0));
}

int e_deb_next(FENSTER *f)
{
 return(e_d_step_next(f, 1));
}

/* What jdb says when the program reached its end, and when its VM went
   away (killed, or crashed), jdb closing its pipe after either, and the
   "> " it echoes before a command's answer, are read in we_debsess.c. */

/* jdb's answer to the command that runs, continues or steps the program.
   Only jdb writes to its pipe: the program has the pty. */
static E_D_STEP_REPLY e_d_jdb_reply;
/* jdb's answer while it starts: its listen line, then its first prompt. */
static E_D_STEP_REPLY e_d_jdb_start_reply;

/* Whether jdb's answer names a stop (a line that names it, its "line="
   after it, the prompt last: jdb can write its answer to a breakpoint set
   while the program runs, and a prompt, between the stop's words and its
   place), says the program is gone, or ends with its prompt, is read by
   e_ds_jdb_reply_is_stop, e_ds_jdb_says_end and e_ds_jdb_asks_command
   (we_debsess.c).  Only jdb writes to its pipe, so the program's own text,
   whatever it says, is never read there. */

/* jdb's prompt, which ends each of its answers with no line end: the bare
   E_D_JDB_BARE_PROMPT while it has no current thread (before the program
   first runs, while it runs, after Pause's "suspend"), and the thread and
   frame, "main[1] ", at a stop.  The thread's name may hold blanks
   ("Signal Dispatcher[1] ") but never starts with one, unlike a row of
   "where" ("  [1] P.sq (P.java:3)"). */
#define E_D_JDB_PROMPT_OPEN  '['
#define E_D_JDB_PROMPT_CLOSE "] "

/**
 * e_d_jdb_prompt_len - The length of the jdb prompt that starts at @p.
 * @p: a place in jdb's answer.
 *
 * The editor tells where one of jdb's answers ends, and steps over a prompt
 * jdb wrote in front of a line, by the prompt's own shape.
 * Return: its bytes, or 0 when no prompt starts there.
 */
static size_t e_d_jdb_prompt_len(const char *p)
{
 size_t bare = strlen(E_D_JDB_BARE_PROMPT);
 const char *open, *q;

 if (!strncmp(p, E_D_JDB_BARE_PROMPT, bare))
  return(bare);
 if (!*p || isspace((unsigned char)*p))
  return(0);
 open = strchr(p, E_D_JDB_PROMPT_OPEN);
 if (!open || memchr(p, '\n', (size_t)(open - p)))
  return(0);
 for (q = open + 1; isdigit((unsigned char)*q); q++)
  ;
 if (q == open + 1 ||
     strncmp(q, E_D_JDB_PROMPT_CLOSE, strlen(E_D_JDB_PROMPT_CLOSE)))
  return(0);
 return((size_t)(q - p) + strlen(E_D_JDB_PROMPT_CLOSE));
}

/**
 * e_d_jdb_skip_prompts - Where a line of jdb's answer starts once the
 * prompts jdb wrote in front of it are stepped over.
 * @line: the line.
 *
 * jdb writes its prompt with no line end, so the next thing it writes (an
 * answer, an event) goes on the prompt's line.
 */
static const char *e_d_jdb_skip_prompts(const char *line)
{
 size_t len;

 while ((len = e_d_jdb_prompt_len(line)) > 0)
  line += len;
 return(line);
}

/**
 * e_d_jdb_prompts_only - How many prompts make up the whole of @len bytes
 * at @line, and nothing else.
 * Return: their number, or 0 when the bytes hold anything else.
 */
static int e_d_jdb_prompts_only(const char *line, size_t len)
{
 size_t at = 0, one;
 int k = 0;

 while (at < len && (one = e_d_jdb_prompt_len(line + at)) > 0 &&
        at + one <= len)
 {
  at += one;
  k++;
 }
 if (at != len)
  return(0);
 return(k);
}

/* jdb's answers still owed to commands sent while the program ran: Pause's
   "suspend", and a breakpoint set or cleared (e_d_jdb_sync_live).  Each is
   one line and one prompt.  One that has not come when the stop is read
   would end the answer to the stop's first command, so the stop takes them
   back by order first (the settle step, e_d_gj_settle_answered). */
static int e_d_jdb_owed;
/* The first words of jdb's answers to those commands. */
#define E_D_JDB_REMOVED     "Removed: breakpoint "
#define E_D_JDB_NOT_FOUND   "Not found: breakpoint "
#define E_D_JDB_RESUMED     "All threads resumed."

/**
 * e_d_jdb_owe_one - A command went to jdb while the program runs: its
 * answer is owed.
 */
static void e_d_jdb_owe_one(void)
{
 e_d_jdb_owed++;
}

/**
 * e_d_jdb_owed_line - Is this line of jdb's run answer the answer to a
 * command sent while the program ran?  Then it is owed no more.
 * @line: a whole line.
 */
static void e_d_jdb_owed_line(const char *line)
{
 char spec[PATH_MAX];
 const char *at;

 if (e_d_jdb_owed <= 0)
  return;
 if (e_ds_jdb_live_word(line, spec, sizeof spec, &at) != E_DS_NOT_FOUND ||
     strstr(line, E_D_JDB_REMOVED) || strstr(line, E_D_JDB_NOT_FOUND) ||
     strstr(line, E_D_JDB_SUSPENDED) || strstr(line, E_D_JDB_RESUMED))
  e_d_jdb_owed--;
}

/**
 * e_d_jdb_owe_at_stop - jdb stopped the program while answers are still
 * owed: the run's controller counts them as the answers its stop takes
 * back first (e_d_run_settle), as for the interrupts gdb and pdb answer.
 */
static void e_d_jdb_owe_at_stop(void)
{
 e_d_run.int_owed = e_d_jdb_owed;
}

/**
 * e_d_jdb_owed_forget - Nothing is owed any more: the settle step took the
 * answers back, or the session ended.
 */
static void e_d_jdb_owed_forget(void)
{
 e_d_jdb_owed = 0;
}

/**
 * e_d_jdb_says_suspended - Does jdb's answer say Pause's "suspend" took
 * hold and ask for the next command?
 * @text: the answer.
 *
 * Told by the line jdb answers "suspend" with and its bare prompt as the
 * last thing it wrote, and only while Pause waits for its stop, so the
 * same words at another time are not taken for it.
 * Return: 1 when the pause's stop has come.
 */
static int e_d_jdb_says_suspended(const char *text)
{
 if (e_d_run.pause == E_D_PAUSE_NONE)
  return(0);
 return(e_ds_jdb_says_suspended_text(text));
}

/* Set when jdb's stop was Pause's: the VM is suspended as a whole, and
   Continue has to resume it (see e_d_jdb_continue_command). */
static int e_d_jdb_suspended;
/* Pause's "suspend" went to jdb in this run. */
static int e_d_jdb_suspend_sent;
/* The program stopped on its own while that "suspend" was on its way: the
   VM is then held twice, once by its stop and once by the suspend, and
   every resume ("cont", "next", "resume") lifts it only once. */
static int e_d_jdb_suspended_twice;

/**
 * e_d_jdb_note_suspend_at_stop - jdb's stop has come: when it is one of the
 * program's own (a breakpoint, a step's end) and Pause's "suspend" went to
 * jdb in this run, the VM is held twice.
 */
static void e_d_jdb_note_suspend_at_stop(void)
{
 if (e_d_jdb_suspend_sent && e_ds_jdb_stop_line_in(e_d_jdb_reply.buf))
  e_d_jdb_suspended_twice = 1;
}

/**
 * e_d_jdb_release_second_suspend - A run starts from a stop that a Pause's
 * "suspend" met: the command just sent lifts the VM once, and one more
 * "resume" lifts the suspend's hold.
 *
 * The user's Continue, step or Finish after such a stop then lets the
 * program run, instead of leaving it held with the editor saying it runs.
 * jdb's answer to the extra "resume" is owed like any command sent while
 * the program runs (e_d_jdb_owed).
 */
static void e_d_jdb_release_second_suspend(void)
{
 if (!e_d_jdb_suspended_twice)
  return;
 e_d_jdb_suspended_twice = 0;
 e_d_send_cmd(E_D_JDB_RESUME);
 e_d_jdb_owe_one();
}

/**
 * e_d_jdb_pause_forget - Forget a Pause when the debug session ends.
 *
 * A session quit while paused leaves no VM to resume.  The next
 * session's first Continue then answers its own stop with "cont", as
 * every Continue after a breakpoint does, not with a "resume" meant for
 * the old program, and no answer of the old jdb's is waited for.
 */
static void e_d_jdb_pause_forget(void)
{
 e_d_jdb_suspended = 0;
 e_d_jdb_suspend_sent = 0;
 e_d_jdb_suspended_twice = 0;
 e_d_jdb_owed_forget();
}

/**
 * e_d_jdb_continue_command - The command that lets the program go on.
 *
 * "cont" answers a breakpoint event, which a Pause has none of: after one
 * the VM is let go with "resume", all its threads at once.  The user gets
 * the program running again after Pause, as after any other stop.
 * Return: the command, ending in a newline.
 */
static const char *e_d_jdb_continue_command(void)
{
 if (!e_d_jdb_suspended)
  return(E_D_JDB_CONT);
 e_d_jdb_suspended = 0;
 return(E_D_JDB_RESUME);
}

/**
 * e_d_jdb_interrupt - Pause the Java program: suspend its VM.
 *
 * jdb takes commands while the program runs.  Its answer is read by the
 * run's reader, which shows it as a stop (e_d_jdb_feed); when a stop of
 * the program's own comes first, the answer is owed to that stop.
 * Return: 0, the command is sent.
 */
static int e_d_jdb_interrupt(void)
{
 e_d_send_cmd(E_D_JDB_SUSPEND);
 e_d_jdb_owe_one();
 e_d_jdb_suspend_sent = 1;
 return(0);
}

/**
 * e_d_jdb_pause_stop - jdb answered Pause's "suspend": the program has
 * stopped as the user asked.
 *
 * Return: E_D_FEED_STOPPED.
 */
static int e_d_jdb_pause_stop(void)
{
 e_d_pause_taken();
 e_d_jdb_suspended = 1;
 return(E_D_FEED_STOPPED);
}

/**
 * e_d_jdb_begin - Forget the answer of the run before when a run starts,
 * and any event jdb wrote between the answers of the stop before: the
 * run's reader reads what jdb writes from here.  The command that starts
 * the run has gone; a stop held twice gets its second "resume" now.
 */
static void e_d_jdb_begin(void)
{
 e_d_step_reply_init(&e_d_jdb_reply);
 e_d_jdb_live_scanned = 0;
 e_d_jdb_ncleared = 0;
 e_d_jdb_suspend_sent = 0;
 e_d_gj_forget_events();
 e_d_jdb_release_second_suspend();
}

/**
 * e_d_jdb_answer_fd - Where jdb answers: its output pipe.
 */
static int e_d_jdb_answer_fd(void)
{
 return(wfildes[0]);
}

/* jdb's word that a breakpoint kept for a class has been set, once the class
   loaded: "Set deferred breakpoint P:4". */
#define E_D_JDB_SET_DEFERRED "Set deferred breakpoint "

/**
 * e_d_jdb_confirm_deferred - jdb has set the deferred breakpoint @spec names:
 * the user's mark on that line is the debugger's now.
 * @spec: "P:4", up to the end of the line.
 *
 * The line turns solid at the stop that follows.
 */
static void e_d_jdb_confirm_deferred(const char *spec)
{
 char cls[PATH_MAX], entry[PATH_MAX];
 const char *colon = strchr(spec, ':');
 size_t len;
 int line, i;

 if (!colon)
  return;
 len = (size_t)(colon - spec);
 if (len >= sizeof(cls) || memchr(spec, '\n', len))
  return;
 memcpy(cls, spec, len);
 cls[len] = '\0';
 line = atoi(colon + 1);
 for (i = 0; i < e_d_nbrpts; i++)
 {
  if (e_d_brk_is_start_entry(i) || e_d_ybrpts[i] != line)
   continue;
  e_d_brk_jdb_class(e_d_sbrpts[i], entry, sizeof(entry));
  if (strcmp(entry, cls) || e_d_brk_state[i] == E_D_BRK_VERIFIED)
   continue;
  e_d_brk_set_state(i, E_D_BRK_VERIFIED, NULL);
  e_d_brk_live_changed = 1;
 }
}

/* jdb's answers to a breakpoint command sent while the program runs come
   in the run's output, in any order with the program's events; each line
   is read by e_ds_jdb_live_word (we_debsess.c). */
#define E_D_JDB_SPEC_MAX  PATH_MAX

/**
 * e_d_jdb_take_live_word - jdb answered a breakpoint command sent while the
 * program runs: record what it said of the user's breakpoint @spec names.
 * @spec: "P:7"; @state: E_D_BRK_*; @line: jdb's line, for its words.
 *
 * A breakpoint jdb set turns solid; one it defers (its class is not loaded)
 * or refuses stays hollow with jdb's words on the status row, said once.
 */
static void e_d_jdb_take_live_word(const char *spec, int state,
                                   const char *line)
{
 char cls[E_D_JDB_SPEC_MAX], entry[E_D_JDB_SPEC_MAX], words[E_D_BRK_MSG_MAX];
 const char *colon = strchr(spec, ':');
 ECNT *cn = WpeEditor;
 E_D_ANSWER a;
 int i, num;

 if (!colon || (size_t)(colon - spec) >= sizeof(cls))
  return;
 memcpy(cls, spec, colon - spec);
 cls[colon - spec] = '\0';
 num = atoi(colon + 1);
 e_ds_brk_words(words, sizeof(words), line);
 e_ds_answer_borrow(&a, line, 0);
 for (i = 0; i < e_d_nbrpts; i++)
 {
  if (e_d_brk_is_start_entry(i) || e_d_ybrpts[i] != num)
   continue;
  e_d_brk_jdb_class(e_d_sbrpts[i], entry, sizeof(entry));
  if (strcmp(entry, cls))
   continue;
  e_ds_take_brk_answer(&e_d_session, DEB_JDB, i, &a);
  if (state == E_D_BRK_VERIFIED)
   e_d_brk_set_state(i, state, NULL);
  else
   e_d_brk_set_state(i, state, words);
  e_d_brk_live_changed = 1;
  e_d_brk_say_refusal(cn->f[cn->mxedt], i);
 }
}

/**
 * e_d_jdb_live_word_in - Act on one line of jdb's answer, if it is jdb's
 * word on a breakpoint command sent while the program runs.
 * @line: the line.
 */
static void e_d_jdb_live_word_in(const char *line)
{
 char spec[E_D_JDB_SPEC_MAX];
 const char *at = NULL;
 int state = e_ds_jdb_live_word(line, spec, sizeof(spec), &at);

 if (state != E_DS_NOT_FOUND)
  e_d_jdb_take_live_word(spec, state, at);
}

/**
 * e_d_jdb_note_live_answers - Read jdb's words on the breakpoints the user
 * set while the program runs, from the whole lines of the run's answer not
 * read yet.
 * @text: what jdb has said in this run so far.
 *
 * The user sees each mark turn solid as soon as jdb has it, while the
 * program goes on, or the reason it stays hollow.  Each answer read is one
 * the stop no longer has to take back (e_d_jdb_owed_line).
 */
static void e_d_jdb_note_live_answers(const char *text)
{
 char line[E_D_LINE_MAX];
 int len = (int)strlen(text), pos;

 if (e_d_jdb_live_scanned > len)
  e_d_jdb_live_scanned = 0;
 pos = e_d_jdb_live_scanned;
 while (pos < len && strchr(text + pos, '\n'))
 {
  e_d_text_next_line(text, &pos, line, sizeof line);
  e_d_jdb_live_word_in(line);
  e_d_jdb_owed_line(line);
 }
 e_d_jdb_live_scanned = pos;
}

/**
 * e_d_jdb_note_deferred_sets - Read jdb's words for the breakpoints it has
 * just set because their class loaded.
 * @text: what jdb has said in this run so far.
 *
 * A breakpoint set before its class loads is hollow ("Deferring ..."); this
 * is how the editor learns jdb has it, so the line is solid at the stop.
 */
static void e_d_jdb_note_deferred_sets(const char *text)
{
 const char *p = text;

 while ((p = strstr(p, E_D_JDB_SET_DEFERRED)) != NULL)
 {
  p += strlen(E_D_JDB_SET_DEFERRED);
  e_d_jdb_confirm_deferred(p);
 }
}

/**
 * e_d_jdb_feed - Read what jdb has said so far.
 * @f: the window the run was started from (unused).
 *
 * The run goes on until jdb names a stop and asks for a command, or says
 * the program is gone, however long the program runs.  jdb closes its pipe
 * after it says the program is gone, so the pipe's end alone means jdb
 * itself is gone.
 * Return: E_D_FEED_GOING, E_D_FEED_STOPPED or E_D_FEED_GONE.
 */
static int e_d_jdb_feed(FENSTER *f)
{
 int flags = fcntl(wfildes[0], F_GETFL, 0);
 int reads, n, fed = E_D_FEED_GOING;

 (void)f;
 fcntl(wfildes[0], F_SETFL, flags | O_NONBLOCK);
 for (reads = 0; reads < E_D_READS_PER_WAKE && fed == E_D_FEED_GOING;
      reads++)
 {
  n = e_d_step_reply_read(wfildes[0], &e_d_jdb_reply, NULL, NULL);
  if (n < 0)
   break;                 /* nothing more for now */
  if (n == 0)
  {
   e_d_note_output_closed();
   fed = E_D_FEED_GONE;
   break;
  }
  jdb_trace("e_d_jdb_feed: read %d bytes total=%d: [%s]\n",
            n, e_d_jdb_reply.len, e_d_jdb_reply.buf);
  e_d_jdb_note_live_answers(e_d_jdb_reply.buf);
  e_d_jdb_note_deferred_sets(e_d_jdb_reply.buf);
  if (e_ds_jdb_says_end(e_d_jdb_reply.buf) ||
      e_ds_jdb_reply_is_stop(e_d_jdb_reply.buf))
   fed = E_D_FEED_STOPPED;
  else if (e_d_jdb_says_suspended(e_d_jdb_reply.buf))
   fed = e_d_jdb_pause_stop();
 }
 fcntl(wfildes[0], F_SETFL, flags);
 if (fed == E_D_FEED_STOPPED)
 {
  e_d_jdb_owe_at_stop();
  e_d_jdb_note_suspend_at_stop();
 }
 return(fed);
}

/**
 * e_d_jdb_signal_name - The name of a signal as a user knows it.
 * @sig: the signal number.
 * Return: "SIGKILL", "SIGSEGV" ..., or NULL for a signal not in the list.
 */
static const char *e_d_jdb_signal_name(int sig)
{
 static const struct { int sig; const char *name; } names[] = {
  { SIGHUP, "SIGHUP" }, { SIGINT, "SIGINT" }, { SIGQUIT, "SIGQUIT" },
  { SIGILL, "SIGILL" }, { SIGABRT, "SIGABRT" }, { SIGFPE, "SIGFPE" },
  { SIGKILL, "SIGKILL" }, { SIGBUS, "SIGBUS" }, { SIGSEGV, "SIGSEGV" },
  { SIGPIPE, "SIGPIPE" }, { SIGALRM, "SIGALRM" }, { SIGTERM, "SIGTERM" }
 };
 size_t i;

 for (i = 0; i < sizeof names / sizeof names[0]; i++)
  if (names[i].sig == sig)
   return(names[i].name);
 return(NULL);
}

/**
 * e_d_jdb_exit_line - The line that says how the Java program ended.
 * @line, @n: receives it.
 *
 * As under gdb: "Program exited with code 3" for a program that exited
 * with a failure code, "Program terminated with signal SIGKILL" for one
 * that was killed, and the plain "Program exited." for a program that ran
 * to its end, or whose end the editor did not see.
 */
static void e_d_jdb_exit_line(char *line, size_t n)
{
 const char *name;
 int status = e_d_jdb_vm.status;

 if (e_d_jdb_vm.status_known && WIFSIGNALED(status))
 {
  name = e_d_jdb_signal_name(WTERMSIG(status));
  if (name)
   snprintf(line, n, E_D_EXIT_SIGNAL_FMT, name);
  else
   snprintf(line, n, E_D_EXIT_SIGNAL_NUM_FMT, WTERMSIG(status));
  return;
 }
 if (e_d_jdb_vm.status_known && WIFEXITED(status) && WEXITSTATUS(status) != 0)
 {
  snprintf(line, n, E_D_EXIT_CODE_FMT, (long)WEXITSTATUS(status));
  return;
 }
 snprintf(line, n, "%s", E_D_EXIT_PLAIN);
}

/**
 * e_d_jdb_end_session - Say how the program ended and close the session.
 * @f: the window on top.
 *
 * The user reads one line in Messages, under the program's last output, as
 * under every other backend: that the program exited, with its failure
 * code when it had one, or which signal killed it.  The line comes once
 * the process the editor started has ended and its status is known; until
 * then the session waits for it without holding a key
 * (e_d_jdb_vm_await_end), and this runs again when it ends.
 */
static void e_d_jdb_end_session(FENSTER *f)
{
 char line[E_D_EXIT_LINE_MAX];

 if (e_d_jdb_vm_await_end())
  return;
 e_d_jdb_exit_line(line, sizeof line);
 e_d_p_message(line, f, 0);
 e_d_quit(f);
}

/**
 * e_d_jdb_start_begin - Forget what the last jdb start said.
 */
static void e_d_jdb_start_begin(void)
{
 e_d_step_reply_init(&e_d_jdb_start_reply);
}

/**
 * e_d_jdb_start_on_output - jdb wrote while it starts: read it all and act
 * on it.
 * @fd: jdb's output; @data: unused.
 *
 * End of file before jdb asked for a command means jdb will never answer
 * (e_d_start_on_output_end); the deadline and the VM's own end are the
 * heartbeat's.
 */
static void e_d_jdb_start_on_output(int fd, void *data)
{
 int n, reads;

 (void)data;
 if (!e_d_start.active || e_d_start.ending != E_D_START_WAITING)
  return;
 for (reads = 0; reads < E_D_READS_PER_WAKE; reads++)
 {
  n = e_d_step_reply_read(fd, &e_d_jdb_start_reply, NULL, NULL);
  if (n == 0)
  {
   e_d_note_output_closed();   /* the end says how jdb ended */
   e_d_start_on_output_end();
   return;
  }
  if (n < 0)
   break;                 /* nothing more for now */
 }
 jdb_trace("e_d_jdb_start_on_output: [%s]\n", e_d_jdb_start_reply.buf);
 e_d_jdb_start_read(e_d_jdb_start_reply.buf);
}

/* Said on the status row at a stop on an uncaught exception. */
#define E_D_JDB_EXCEPTION_FMT "Uncaught exception: %s"
#define E_D_JDB_EXCEPTION_NAME_MAX 160

/**
 * e_d_jdb_exception_name - The exception class a stop line of jdb names.
 * @stop: jdb's stop line; @out, @n: receives "java.lang.ArithmeticException".
 * Return: 1 when @stop is an exception stop, else 0.
 */
static int e_d_jdb_exception_name(const char *stop, char *out, size_t n)
{
 const char *name, *end;
 size_t len;

 if (strncmp(stop, E_D_JDB_EXCEPTION_MARK,
             strlen(E_D_JDB_EXCEPTION_MARK)) != 0)
  return(0);
 name = stop + strlen(E_D_JDB_EXCEPTION_MARK);
 while (*name == ' ')
  name++;
 for (end = name; *end && !isspace((unsigned char)*end); end++)
  ;
 len = end - name;
 if (len == 0 || len >= n)
  return(0);
 memcpy(out, name, len);
 out[len] = '\0';
 return(1);
}

/**
 * e_d_jdb_say_exception - Name the exception on the status row when the
 * program stopped on one it did not catch.
 * @f: the window the stop came to; @stop: jdb's stop line.
 *
 * The green bar alone looks like a breakpoint: the user reads why the
 * program stopped, and which exception, also when it threw inside a
 * library whose source is not there to mark.  The line stays until the
 * next key.
 */
static void e_d_jdb_say_exception(FENSTER *f, const char *stop)
{
 char name[E_D_JDB_EXCEPTION_NAME_MAX];
 char line[E_D_JDB_EXCEPTION_NAME_MAX + sizeof E_D_JDB_EXCEPTION_FMT];
 ECNT *cn = f->ed;

 if (!e_d_jdb_exception_name(stop, name, sizeof name))
  return;
 snprintf(line, sizeof line, E_D_JDB_EXCEPTION_FMT, name);
 e_status_say_until_key(cn->f[cn->mxedt], line);
}

/**
 * e_d_jdb_stopped - What jdb does when its stop comes: show the program's
 * last output, then ask jdb on the fd loop what the stop needs, or say the
 * end.
 * @f: the window on top.
 *
 * The user sees what the program printed up to the stop in Messages (a
 * line it has not ended yet included) at once; the green bar on the line
 * jdb stopped on, in the Java file it names, and the watches come when jdb
 * has answered, the keys reaching the editor meanwhile.  After Pause the
 * thread that runs the user's code is looked for first.  The end is one
 * line in Messages saying the program ended.  The same order as under gdb.
 */
static void e_d_jdb_stopped(FENSTER *f)
{
 e_d_jdb_ncleared = 0;       /* the stop is the user's: jdb has the rest */
 e_d_drain_pty_to_messages();
 if (e_ds_jdb_says_end(e_d_jdb_reply.buf))
 {
  e_d_jdb_end_session(f);
  return;
 }
 e_d_switch_out(0);
 e_d_run.stop_on_loop = 1;
 if (!e_ds_jdb_stop_line_in(e_d_jdb_reply.buf) && e_d_jdb_suspended)
  e_d_gj_pause_ask(f->ed);
 else
  e_d_gj_stop_ask(f->ed, e_ds_jdb_stop_line_in(e_d_jdb_reply.buf));
}

/* The breakpoints the user removed while the program ran, which jdb has
   been sent "clear" for in this run.  jdb may report a hit on one of them
   before it takes the clear, and that stop is not the user's. */
#define E_D_JDB_CLEARED_MAX 32
typedef struct
{
 char cls[E_D_JDB_SPEC_MAX];   /* the class, as "clear" names it */
 int line;                     /* 1-based */
} E_D_JDB_CLEARED;

static E_D_JDB_CLEARED e_d_jdb_cleared[E_D_JDB_CLEARED_MAX];

/**
 * e_d_jdb_cleared_find - Which "clear" sent in this run names @cls and
 * @line?
 * Return: its place in the record, or -1.
 */
static int e_d_jdb_cleared_find(const char *cls, int line)
{
 int i;

 for (i = 0; i < e_d_jdb_ncleared; i++)
  if (e_d_jdb_cleared[i].line == line && !strcmp(e_d_jdb_cleared[i].cls, cls))
   return(i);
 return(-1);
}

/**
 * e_d_jdb_cleared_drop - Take entry @at out of the record of cleared
 * breakpoints.
 */
static void e_d_jdb_cleared_drop(int at)
{
 for (; at < e_d_jdb_ncleared - 1; at++)
  e_d_jdb_cleared[at] = e_d_jdb_cleared[at + 1];
 e_d_jdb_ncleared--;
}

/**
 * e_d_jdb_note_cleared - Remember that jdb is sent "clear" for @line of
 * @file.
 *
 * The user then never sees jdb's stop on a line he has just unmarked, which
 * a program passing it often can reach before the clear is taken.  When the
 * record is full the oldest entry goes.
 */
static void e_d_jdb_note_cleared(const char *file, int line)
{
 char cls[E_D_JDB_SPEC_MAX];

 e_d_brk_jdb_class(file, cls, sizeof(cls));
 if (e_d_jdb_cleared_find(cls, line) >= 0)
  return;
 if (e_d_jdb_ncleared == E_D_JDB_CLEARED_MAX)
  e_d_jdb_cleared_drop(0);
 snprintf(e_d_jdb_cleared[e_d_jdb_ncleared].cls, E_D_JDB_SPEC_MAX, "%s", cls);
 e_d_jdb_cleared[e_d_jdb_ncleared].line = line;
 e_d_jdb_ncleared++;
}

/**
 * e_d_jdb_forget_cleared - jdb is sent "stop at" for @line of @file again:
 * a stop there is the user's.
 */
static void e_d_jdb_forget_cleared(const char *file, int line)
{
 char cls[E_D_JDB_SPEC_MAX];
 int at;

 e_d_brk_jdb_class(file, cls, sizeof(cls));
 at = e_d_jdb_cleared_find(cls, line);
 if (at >= 0)
  e_d_jdb_cleared_drop(at);
}

/**
 * e_d_jdb_marked - Does the breakpoint list have @line of class @cls?
 * Return: 1 when the user has a mark there.
 */
static int e_d_jdb_marked(const char *cls, int line)
{
 char entry[E_D_JDB_SPEC_MAX];
 int i;

 for (i = 0; i < e_d_nbrpts; i++)
 {
  if (e_d_ybrpts[i] != line)
   continue;
  e_d_brk_jdb_class(e_d_sbrpts[i], entry, sizeof(entry));
  if (!strcmp(entry, cls))
   return(1);
 }
 return(0);
}

/**
 * e_d_jdb_removal_waits - Is the removal of @line of class @cls still
 * waiting to be sent to jdb?
 * Return: 1 when it is.
 */
static int e_d_jdb_removal_waits(const char *cls, int line)
{
 char entry[E_D_JDB_SPEC_MAX];
 int i;

 for (i = 0; i < e_d_brk_ngone; i++)
 {
  if (e_d_brk_gone[i].line != line)
   continue;
  e_d_brk_jdb_class(e_d_brk_gone[i].file, entry, sizeof(entry));
  if (!strcmp(entry, cls))
   return(1);
 }
 return(0);
}

/**
 * e_d_jdb_stale_stop - Is jdb's stop a breakpoint hit on a line the user
 * has since unmarked?
 *
 * Told from the line jdb names: a "Breakpoint hit:" (a step's end or an
 * exception is always the user's) on a line the list does not mark, for
 * which jdb was sent a "clear" or is about to be.  The user then sees no
 * stop on a line that no longer has a mark.
 * Return: 1 for such a stop.
 */
static int e_d_jdb_stale_stop(void)
{
 const char *stop = e_ds_jdb_stop_line_in(e_d_jdb_reply.buf);
 char cls[E_D_JDB_SPEC_MAX];
 int line;

 if (!stop || strncmp(stop, E_D_JDB_STOP_MARK, strlen(E_D_JDB_STOP_MARK)))
  return(0);
 line = e_ds_jdb_stop_line(stop);
 if (line <= 0 || !e_ds_jdb_stop_class(stop, cls, sizeof(cls)))
  return(0);
 if (e_d_jdb_marked(cls, line))
  return(0);
 return(e_d_jdb_cleared_find(cls, line) >= 0 ||
        e_d_jdb_removal_waits(cls, line));
}

/**
 * e_d_jdb_answer_reset - Read a new answer of jdb's, for a command sent
 * from a stop the user is not shown.
 */
static void e_d_jdb_answer_reset(void)
{
 e_d_step_reply_init(&e_d_jdb_reply);
 e_d_jdb_live_scanned = 0;
}

/**
 * e_d_jdb_settle_later - Leave the answers jdb still owes to commands sent
 * while the program ran to the first step of the requests that follow.
 * @cn: unused; @owed: how many; @why: unused.
 *
 * jdb answers every such command, Pause's "suspend" included, with one
 * line and one prompt, whatever the program did meanwhile: none is left
 * in the program to come back at a later resume, as gdb's interrupts can.
 * Return: the answers still owed.
 */
static int e_d_jdb_settle_later(ECNT *cn, int owed, int why)
{
 (void)cn;
 (void)why;
 e_d_ga_owe_settle(owed);
 return(owed);
}

/* jdb: its answers on its pipe, the program's output on the pty, a prompt
   at each stop.  jdb takes breakpoint commands while the program runs, so
   it is never interrupted for one.  What a stop asks jdb (its watches, its
   stack, the thread a Pause stopped in, the breakpoints changed meanwhile)
   is asked on the fd loop, through the request engine (e_d_ga), so jdb has
   no sync_at_stop; its settle leaves the answers still owed to commands
   sent while the program ran to the first step of those requests. */
static const E_D_RUN_OPS e_d_jdb_ops =
{
 "jdb",
 e_d_jdb_answer_fd,
 e_d_line_output_fd,
 e_d_jdb_feed,
 e_d_jdb_stopped,
 e_d_jdb_interrupt,
 e_d_jdb_sync_live,
 NULL,
 e_d_ga_run_to_cursor,
 e_d_jdb_begin,
 NULL,
 0,
 NULL,
 NULL,
 e_d_jdb_settle_later,
 e_d_jdb_answer_reset,
 0,
 e_d_jdb_stale_stop,
 NULL
};

/**
 * e_d_jdb_run_ops - The run controller's table for jdb.
 */
static const E_D_RUN_OPS *e_d_jdb_run_ops(void)
{
 return(&e_d_jdb_ops);
}

/**
 * e_d_jdb_run_started - jdb was sent the command that runs, continues or
 * steps the program: let it run under the controller.
 * @f: the source window;
 * @start_brk: the editor's start breakpoint to take out at the stop, or 0.
 *
 * The editor keeps its keys until the stop, however long the program takes
 * to get there; its output shows in Messages meanwhile, and what the user
 * types is its input.
 * Return: 0, or the end of the session.
 */
static int e_d_jdb_run_started(FENSTER *f, int start_brk)
{
 e_d_swtch = 3;
 return(e_d_run_hand_over(f, e_d_jdb_run_ops(), start_brk));
}

/**
 * e_d_pdb_is_metadata - Say whether a line of pdb's answer is pdb's own (the
 * prompt, the stop and source lines, its banners), not the program's output.
 */
static int e_d_pdb_is_metadata(const char *line)
{
 return(line[0] == '>' || !strncmp(line, "-> ", 3) ||
        !strncmp(line, "(Pdb)", 5) ||
        !strncmp(line, "--Return--", 10) ||
        !strncmp(line, "--Call--", 8) ||
        strstr(line, "The program") != NULL);
}

/**
 * e_d_pdb_capture_output - Keep every line of pdb's answer that the program
 * printed (print() results and the like) for Ctrl-G P.
 * @text: the answer, or a part of it.
 */
static void e_d_pdb_capture_output(const char *text, int skip_first)
{
 char line[E_D_LINE_MAX];
 int pos = 0, len;

 if (skip_first)
  e_d_text_next_line(text, &pos, line, sizeof line);
 while (text[pos])
 {
  len = e_d_text_next_line(text, &pos, line, sizeof line);
  if (len > 0 && !e_d_pdb_is_metadata(line))
   e_d_prog_output_append_line(line, len);
 }
}


/* What the pdb step reader carries across the parts of a long answer. */
typedef struct {
 int returned;                  /* a "--Return--" was handed on */
 int stopped;                   /* a stop line was handed on: */
 char file[E_D_STOP_PATH_MAX];  /* the file it names */
 int line;                      /* and the line */
 int open_is_pdb;               /* the part ends inside one of pdb's lines */
} E_D_PDB_SPILL;

/**
 * e_d_pdb_part_ends_in_own_line - Say whether a part of pdb's answer ends
 * inside a line of pdb's own, whose rest comes in the next part.
 * @text: the part; @starts_in_own: it starts inside such a line.
 */
static int e_d_pdb_part_ends_in_own_line(const char *text, int starts_in_own)
{
 const char *last = strrchr(text, '\n');

 if (!last)
  return(starts_in_own);
 if (last[1] == '\0')
  return(0);
 return(e_d_pdb_is_metadata(last + 1));
}

/**
 * e_d_pdb_spill - Keep as program output the oldest part of a pdb answer too
 * long for the buffer, and note a "--Return--" or a stop line in it.
 * @ctx: an E_D_PDB_SPILL.
 *
 * A step whose program prints a lot goes on to its stop, with every line the
 * program printed kept, instead of ending the session.  A stop in a function
 * whose name is longer than the buffer is still marked: its "> file(line)"
 * comes first and is handed on here, and the rest of the line is pdb's, not
 * the program's output.
 */
static void e_d_pdb_spill(void *ctx, char *text, int mid_line)
{
 E_D_PDB_SPILL *sp = ctx;
 int continues_own = mid_line && sp->open_is_pdb;
 const char *stop = e_d_pdb_find_stop(text);

 if (strstr(text, E_D_PDB_RETURN))
  sp->returned = 1;
 if (stop &&
     e_d_pdb_stop_parse(stop, sp->file, sizeof sp->file, &sp->line))
  sp->stopped = 1;
 e_d_pdb_capture_output(text, continues_own);
 sp->open_is_pdb = e_d_pdb_part_ends_in_own_line(text, continues_own);
}

/**
 * e_d_pdb_internal_place - Say whether pdb stopped in Python's own code
 * (<string>, <frozen ...>), which the user has no source of.
 * @file: the file pdb named.
 */
static int e_d_pdb_internal_place(const char *file)
{
 return(file[0] == '<');
}

/**
 * e_d_pdb_stop_is_internal - Say whether pdb's answer stops in Python's own
 * code, so the reader steps on to the user's.
 * @kept: the part of the answer still held; @sp: what was handed on.
 *
 * A stop in the part still held is the newest; one handed on counts only
 * when the held part names none.
 */
static int e_d_pdb_stop_is_internal(const char *kept, const E_D_PDB_SPILL *sp)
{
 const char *stop = e_d_pdb_find_stop(kept);

 if (stop)
  return(stop[2] == '<');
 return(sp->stopped && e_d_pdb_internal_place(sp->file));
}

/**
 * e_d_pdb_answer_place - Where pdb's answer to a step or continue says it
 * stopped.
 * @kept: the part of the answer still held; @sp: what was handed on;
 * @file, @n: the file.
 *
 * A stop in a function whose name is longer than the buffer is marked from
 * its "> file(line)", which was handed on before the rest of the line came.
 * Return: the line to mark, or 0 when the answer names none (code run by
 * exec() names line 0, and marks nothing either).
 */
static int e_d_pdb_answer_place(const char *kept, E_D_PDB_SPILL *sp,
                                char *file, size_t n)
{
 const char *stop = e_d_pdb_find_stop(kept);

 file[0] = '\0';
 if (stop)
  return(e_d_pdb_step_place(stop, file, n));
 if (!sp->stopped)
  return(0);
 e_d_swtch = 3;
 snprintf(file, n, "%s", sp->file);
 return(sp->line);
}

/* The prompt pdb ends every answer with: the program stopped. */
#define E_D_PDB_PROMPT "(Pdb) "
/* How many times a stop in Python's own code is stepped past, looking for
   the script's own lines, before the run is left where it is. */
#define E_D_PDB_INTERNAL_STEPS_MAX 100
/* How many reads one wake of the fd loop takes from pdb: a program that
   prints without end must not keep the editor from its keys. */
#define E_D_PDB_FEED_READS_MAX 64
#define E_D_PDB_STEP_ON "n\n"
#define E_D_PDB_UP      "up\n"      /* the caller's line, after a "return" */
#define E_D_PDB_FINISH  "return\n"  /* run to the end of the function */
#define E_D_PDB_INTERNAL_LINE \
 " pdb stayed in Python's own code: step with F7 or F8, or run on"

/* pdb's run: what its answer holds so far, and what the stops that are not
   the user's yet still have to do.  pdb has no pty and shares its stdin and
   stdout with the program, so the run reads one pipe and sends a command
   only at a stop, never while the program runs. */
typedef struct {
 E_D_STEP_REPLY reply;          /* the answer to the command sent */
 E_D_PDB_SPILL spill;           /* what was handed on of a long answer */
 int returned;                  /* the answer was a "--Return--" */
 int finish_up;                 /* Finish: "up" follows the "--Return--" */
 int internal_steps;            /* stops in Python's own code stepped past */
 int pause_seen;                /* Pause: the script's line is known: */
 char pause_file[E_D_STOP_PATH_MAX];   /* its file */
 int pause_line;                       /* and line */
} E_D_PDB_RUN;

static E_D_PDB_RUN e_d_pdb_run;

/**
 * e_d_pdb_answer_reset - Start reading a new answer from pdb.
 *
 * Called for each command a run sends, so the answer of the one before is
 * not mistaken for this one's prompt.
 */
static void e_d_pdb_answer_reset(void)
{
 memset(&e_d_pdb_run.spill, 0, sizeof e_d_pdb_run.spill);
 e_d_step_reply_init(&e_d_pdb_run.reply);
 e_d_pdb_run.returned = 0;
}

/**
 * e_d_pdb_begin - Forget everything of the run before when a run starts.
 */
static void e_d_pdb_begin(void)
{
 e_d_pdb_answer_reset();
 e_d_pdb_run.finish_up = 0;
 e_d_pdb_run.internal_steps = 0;
 e_d_pdb_run.pause_seen = 0;
}

/**
 * e_d_pdb_answer_fd - Where pdb answers: its output pipe, which the program
 * writes to as well.
 */
static int e_d_pdb_answer_fd(void)
{
 return(wfildes[0]);
}

/**
 * e_d_pdb_no_output_fd - pdb gives the program no pty of its own.
 */
static int e_d_pdb_no_output_fd(void)
{
 return(-1);
}

/**
 * e_d_pdb_feed - Read what pdb or the program has written so far.
 * @f: the window the run was started from (unused).
 *
 * The program's text and pdb's prompt arrive on one pipe, the prompt
 * without a line end, so the answer is kept as it comes and the run stops
 * when the prompt is in it.  A program that prints a lot is read as it
 * prints, a bounded amount at a time so the editor keeps its keys.
 * Return: E_D_FEED_GOING, E_D_FEED_STOPPED or E_D_FEED_GONE.
 */
static int e_d_pdb_feed(FENSTER *f)
{
 E_D_PDB_RUN *r = &e_d_pdb_run;
 int flags = fcntl(wfildes[0], F_GETFL, 0);
 int reads, n, fed = E_D_FEED_GOING;

 fcntl(wfildes[0], F_SETFL, flags | O_NONBLOCK);
 for (reads = 0; reads < E_D_PDB_FEED_READS_MAX && fed == E_D_FEED_GOING;
      reads++)
 {
  n = e_d_step_reply_read(wfildes[0], &r->reply, e_d_pdb_spill, &r->spill);
  if (n < 0)
   break;                 /* nothing more for now */
  if (n == 0)
  {  /* EOF: pdb restarts a finished script, so it never ends by itself */
   e_d_note_output_closed();
   fed = E_D_FEED_GONE;
  }
  else if (strstr(r->reply.buf, E_D_PDB_PROMPT))
   fed = E_D_FEED_STOPPED;
 }
 fcntl(wfildes[0], F_SETFL, flags);
 return(fed);
}

/**
 * e_d_pdb_send_on - Send pdb a command from a stop and read its answer on
 * the fd loop.
 * @cmd: the command.
 *
 * Used for the commands a stop calls for (step on out of Python's own
 * code, "up" after a Finish), which pdb must read now and not while the
 * program runs.
 */
static void e_d_pdb_send_on(const char *cmd)
{
 e_d_pdb_answer_reset();
 e_d_send_cmd(cmd);
 e_d_run_resume();
}

/**
 * e_d_pdb_say_end - Say that the program ended and close the session, when
 * pdb's answer is its end.
 * @f: the window on top; @text: pdb's answer.
 *
 * pdb names an exit status after sys.exit(N) and says "The program
 * finished" when the script runs off its end; it then restarts the script,
 * so the editor ends the session itself.
 * Return: 1 when the program ended.
 */
static int e_d_pdb_say_end(FENSTER *f, const char *text)
{
 const char *end = strstr(text, E_D_PDB_EXIT_STATUS);

 if (!end)
  end = strstr(text, E_D_PDB_FINISHED);
 if (!end)
  return(0);
 e_d_switch_out(0);
 e_d_say_program_exited(end, f);
 e_d_quit(f);
 return(1);
}

/**
 * e_d_pdb_step_out_of_internal - pdb stopped in Python's own code, where
 * the user has no source: step on, up to a bound.
 * @f: the window on top.
 *
 * The user gets the next stop in the script, or one line saying pdb stayed
 * in Python's own code when the bound is reached.
 * Return: 1 when pdb steps on, 0 when the run stays where it is.
 */
static int e_d_pdb_step_out_of_internal(FENSTER *f)
{
 if (++e_d_pdb_run.internal_steps > E_D_PDB_INTERNAL_STEPS_MAX)
 {
  e_d_switch_out(0);
  e_status_say(f, E_D_PDB_INTERNAL_LINE);
  return(0);
 }
 e_d_pdb_send_on(E_D_PDB_STEP_ON);
 return(1);
}

/**
 * e_d_pdb_finish_up - Finish function reached the end of the function:
 * ask pdb for the caller's line.
 *
 * pdb's "return" stops at the end of the function, still inside it; "up"
 * shows the call the user finishes into, as gdb's "finish" does.
 */
static void e_d_pdb_finish_up(void)
{
 e_d_pdb_run.finish_up = 0;
 e_d_switch_out(1);
 e_d_nstack = 1;
 e_d_pdb_send_on(E_D_PDB_UP);
}

/**
 * e_d_pdb_note_return - Remember that pdb's answer was a "--Return--".
 * @r: the run.
 */
static void e_d_pdb_note_return(E_D_PDB_RUN *r)
{
 if (r->spill.returned || strstr(r->reply.buf, E_D_PDB_RETURN))
  r->returned = 1;
}

/* What a Pause's stop under pdb turned out to be, once its stack is read. */
#define E_D_PDB_PAUSE_NOT_SEEN 0   /* no handler in the stack: ordinary stop */
#define E_D_PDB_PAUSE_MARKED   1   /* the script's line is to be marked */
#define E_D_PDB_PAUSE_GOING    2   /* "return" is to be sent: pdb runs on */
#define E_D_PDB_PAUSE_LOST     3   /* bound reached: nothing marked, said */

/**
 * e_d_pdb_stop_shown - The rest of a stop of pdb's that is no Pause in its
 * own code: step past Python's own code, follow Finish with "up", or show
 * the stop once pdb has answered what it asks.
 * @f: the window on top.
 *
 * Also what a Pause's stop does when pdb's stack has no SIGINT handler in
 * it, so the stop is an ordinary one.
 */
static void e_d_pdb_stop_shown(FENSTER *f)
{
 E_D_PDB_RUN *r = &e_d_pdb_run;
 char file[E_D_STOP_PATH_MAX];
 int line;

 if (e_d_pdb_stop_is_internal(r->reply.buf, &r->spill))
 {
  if (!e_d_pdb_step_out_of_internal(f))
   e_d_gp_stop_ask(f->ed, "", 0);
  return;
 }
 if (r->finish_up && r->returned)
 {
  e_d_pdb_finish_up();
  return;
 }
 r->finish_up = 0;
 line = e_d_pdb_answer_place(r->reply.buf, &r->spill, file, sizeof file);
 e_d_switch_out(0);
 e_d_gp_stop_ask(f->ed, file, line);
}

/**
 * e_d_pdb_stopped - What pdb does when its prompt comes: keep the program's
 * output for Ctrl-G P, then mark the stop, say the end, or go on.
 * @f: the window on top.
 *
 * The user sees the green bar on the line pdb stopped on, or one line in
 * Messages saying the program ended.  A stop in Python's own code is
 * stepped past, and Finish's "--Return--" is followed by "up", both sent
 * from here, at the stop.  What the stop asks pdb (its stack after a Pause,
 * the watches, the breakpoints changed while it ran) is asked on the fd
 * loop, the keys reaching the editor meanwhile.
 */
static void e_d_pdb_stopped(FENSTER *f)
{
 E_D_PDB_RUN *r = &e_d_pdb_run;

 e_d_pdb_note_return(r);
 if (strstr(r->reply.buf, E_D_PDB_INTERRUPTED))
  e_d_pause_taken();
 e_d_pdb_capture_output(r->reply.buf, r->reply.mid_line &&
                        r->spill.open_is_pdb);
 if (e_d_pdb_say_end(f, r->reply.buf))
  return;
 if (e_d_run.pause == E_D_PAUSE_TAKEN)
 {
  e_d_gp_pause_ask(f->ed);
  return;
 }
 e_d_pdb_stop_shown(f);
}

/**
 * e_d_pdb_settle_later - Count the answers pdb already wrote to interrupts
 * that met it at its prompt, and leave the rest to the first step of the
 * requests that follow.
 * @cn: unused; @owed: the interrupts sent whose stop never came; @why:
 * unused.
 *
 * pdb answers an interrupt that finds it at its prompt with
 * "--KeyboardInterrupt--" and one more prompt.  Left unread, that prompt
 * would end the answer to the next command, and every answer after it
 * would come one command late.  The answers read with the stop are
 * counted here by pdb's own line for them, not by their prompts: an
 * interrupt that meets pdb while it prints the stop cuts that print short
 * and leaves one prompt for both.  Nor by the bare word: the script's own
 * Ctrl-C handler may print it, and the user would then not be told that
 * the script handles Ctrl-C itself.  The ones still on their way are
 * counted by the settle
 * step (e_d_gs_settle_cmd), on the fd loop.
 * Return: the interrupts that may still be owed.
 */
static int e_d_pdb_settle_later(ECNT *cn, int owed, int why)
{
 int extra = e_d_words_in(e_d_pdb_run.reply.buf,
                          strlen(e_d_pdb_run.reply.buf),
                          E_D_PDB_INT_AT_PROMPT);

 (void)cn;
 (void)why;
 if (extra > 0)
 {
  owed -= extra;
  e_d_run_own_sigint_disproved();
 }
 if (owed < 0)
  owed = 0;
 e_d_ga_owe_settle(owed);
 return(owed);
}

static int e_d_pdb_interrupt_stop(int taken);            /* (below) */

/* pdb: one pipe for the debugger and the program, a prompt at each stop.  A
   breakpoint changed while the script runs is applied by interrupting it.
   An interrupt that reaches pdb at its prompt is answered with
   "--KeyboardInterrupt--" and one more prompt, taken back by the settle
   step.  What a stop asks pdb is asked on the fd loop, through the request
   engine (e_d_ga), so pdb has no sync_at_stop: the stop's own requests tell
   it the breakpoints changed during the run. */
static const E_D_RUN_OPS e_d_pdb_ops =
{
 "pdb",
 e_d_pdb_answer_fd,
 e_d_pdb_no_output_fd,
 e_d_pdb_feed,
 e_d_pdb_stopped,
 e_d_interrupt_debugger,
 NULL,
 NULL,
 NULL,
 e_d_pdb_begin,
 NULL,
 0,
 e_d_pdb_interrupt_stop,
 NULL,
 e_d_pdb_settle_later,
 e_d_pdb_answer_reset,
 1,
 NULL,
 e_d_pdb_sync_on_loop
};

/**
 * e_d_pdb_run_ops - The run controller's table for pdb.
 */
static const E_D_RUN_OPS *e_d_pdb_run_ops(void)
{
 return(&e_d_pdb_ops);
}

/* The source line a68g reports for the current stop ("5           f := f
   * i"; a bare number, or one ended by ':', is no position) is read by
   e_ds_a68g_parse_line (we_debsess.c). */

/* Return 1 if an a68g monitor line is protocol chatter that must not be shown
   as program output: the prompt, the "Breakpoint"/"Continuing" banners, the
   "main thread" note, the temporary-breakpoint note, the termination messages,
   and the column-marker line (only spaces and '-'/'^').  Empty lines too. */
static int e_d_a68g_is_metadata(const char *s)
{
 int k;

 if (!s[0])
  return(1);
 if (!strncmp(s, "(a68g)", 6) ||
     !strncmp(s, "Breakpoint", 10) ||
     !strncmp(s, "Continuing", 10) ||
     !strncmp(s, "Execution", 9) ||
     strstr(s, "main thread") ||
     strstr(s, "now removed") ||
     strstr(s, "Genie finished") ||
     strstr(s, "Terminate a68g"))
  return(1);
 for (k = 0; s[k]; k++)
  if (s[k] != ' ' && s[k] != '\t' && s[k] != '-' && s[k] != '^')
   return(0);
 return(1);   /* only spaces/dashes/carets: the column marker */
}

/**
 * e_d_a68g_show_in_messages - Write a line of the program's output in
 * Messages, unless the run is going and the window is closed.
 * @line: the line; @f: the window on top.
 *
 * While the program runs the fd loop must not open a window: it could land
 * on top of a box the user has open.  The line is still kept for the User
 * Screen (Alt-F5) by the caller.  At the stop the window is opened again if
 * need be, as gdb's output is.
 */
static void e_d_a68g_show_in_messages(char *line, FENSTER *f)
{
 if (e_d_run_going() && !e_d_find_messages_window())
  return;
 e_d_p_message(line, f, 0);
}

/* Scan a chunk of a68g monitor output line by line: show every non-chatter,
   non-numbered line as program output -- in the Messages window (Ctrl-G P, the
   path gdb reaches via its pty) and in the User-Screen buffer (Alt-F5) -- and
   return the last reported source line number (or -1).  Shared by the step and
   continue paths so program output is never lost, including the final exit
   chunk that a68g emits just before EOF.  When mid_line is set, buf starts
   inside a line whose start was scanned before: that first piece is output,
   never a source line number. */
static int e_d_a68g_scan(FENSTER *f, const char *buf, int mid_line)
{
 char _ln[E_D_LINE_MAX];
 int _bi = 0, _line = -1;

 while (buf[_bi])
 {
  int _pl, _li, _piece = (mid_line && _bi == 0);

  _li = e_d_text_next_line(buf, &_bi, _ln, sizeof _ln);
  _pl = -1;
  if (!_piece)
   _pl = e_ds_a68g_parse_line(_ln);
  if (_pl > 0)
  {
   _line = _pl;                          /* last numbered line wins */
   e_ds_take_cli_stop_line(&e_d_session, DEB_A68G, _ln);
  }
  else if (!e_d_a68g_is_metadata(_ln))
  {
   e_d_a68g_show_in_messages(_ln, f);    /* Messages window (Ctrl-G P) */
   e_d_prog_output_append_line(_ln, _li); /* User Screen buffer (Alt-F5) */
  }
 }
 return(_line);
}

/**
 * e_d_a68g_window - Find the window holding the Algol 68 program.
 * @cn: the editor.
 *
 * a68g never names its source file, so the stop marker and the Stack window
 * both go to the open .a68 / .alg window, whatever its directory.
 * Return: the window index, or 0 when no such window is open.
 */
static int e_d_a68g_window(ECNT *cn)
{
 int i, l;

 for (i = cn->mxedt; i > 0; i--)
 {
  char *nm = cn->f[i]->datnam;
  l = (int)strlen(nm);
  if (l > 4 && (!strcmp(nm + l - 4, ".a68") || !strcmp(nm + l - 4, ".alg")))
   return(i);
 }
 return(0);
}

/* Move the editor cursor to LINE in the Algol 68 source window.  a68g debugs a
   single source file but does NOT name it in its stop output, so -- unlike gdb
   -- there is no filename to match an open window against.  Locate the .a68 /
   .alg window directly (by extension, so the working directory and how the file
   was opened, e.g. "docs/examples/x.a68", do not matter), switch to it, and set
   the cursor.  This is the single-file analogue of e_d_goto_break. */
static void e_d_a68g_goto(FENSTER *f, int line)
{
 ECNT *cn = f->ed;
 BUFFER *b;
 SCHIRM *s;
 int i = e_d_a68g_window(cn);

 if (i <= 0)
 {  e_d_switch_out(0);  return;  }
 if (i != cn->mxedt)
  e_switch_window(cn->edt[i], cn->f[cn->mxedt]);
 f = cn->f[cn->mxedt];
 b = f->b;
 s = f->s;
 if (line >= 1 && line <= b->mxlines)
 {
  s->da.y = b->b.y = line - 1;
  s->da.x = b->b.x = 0;
  s->de.x = MAXSCOL;
 }
 e_d_swtch = 3;
 e_schirm(f, 1);
 e_cursor(f, 1);
 e_d_show_stop_position(f);
}

/* What the a68g step reader carries across the parts of a long answer. */
typedef struct {
 FENSTER *f;
 int line;               /* the last source line a handed-on part reported */
 int finished;           /* a handed-on part said the program finished */
} E_D_A68G_SPILL;

/* What a68g prints when the program ends; its output closes right after. */
#define E_D_A68G_FINISHED "Genie finished"

/**
 * e_d_a68g_spill - Show as program output the oldest part of an a68g answer
 * too long for the buffer, and remember a source line it reports.
 * @ctx: an E_D_A68G_SPILL.
 *
 * A step whose program prints a lot goes on to its stop, with all its output
 * shown, instead of being taken for the end of the program.
 */
static void e_d_a68g_spill(void *ctx, char *text, int mid_line)
{
 E_D_A68G_SPILL *sp = ctx;
 int line = e_d_a68g_scan(sp->f, text, mid_line);

 if (line > 0)
  sp->line = line;
 if (strstr(text, E_D_A68G_FINISHED))
  sp->finished = 1;
}

/* The prompt a68g ends every answer with: the program stopped. */
#define E_D_A68G_PROMPT "(a68g) "
/* At every stop, a breakpoint, a step, a Finish and an error alike, a68g
   writes three lines before that prompt: the numbered source line, a line
   that marks the column under it, and which thread stopped.  A program's
   own "(a68g) " has none of them before it.  Which kind of line each is
   (E_D_A68G_LINE_*, how many: E_D_A68G_STOP_LINES) is read by
   e_ds_a68g_line_kind (we_debsess.c). */
/* When SIGINT interrupts the program, a68g writes the stop's source line and
   column marker, then asks E_D_A68G_TERMINATE (we_debsess.h) with no line
   end.  Answered "no" it goes on to the thread line and its prompt, so the
   stop reads as any other. */
#define E_D_A68G_KEEP         "no\n"
#define E_D_A68G_NEXT "next\n"
#define E_D_A68G_STEP "step\n"
/* How many "next" Step Over sends for one press while a68g still reports
   the same line (the first one is the press itself). */
#define E_D_A68G_SAME_LINE_MAX 256
/* How many reads one wake of the fd loop takes from a68g: a program that
   prints without end must not keep the editor from its keys. */
#define E_D_A68G_FEED_READS_MAX 64

/* a68g's run: what its answer holds so far, and what Step Over still has to
   do at a stop.  a68g has no pty and writes the program's text and its own
   on one pipe, and the program shares a68g's stdin, so the run reads that
   pipe and sends a command only at a stop, never while the program runs. */
typedef struct {
 E_D_STEP_REPLY reply;          /* the answer to the command sent */
 E_D_A68G_SPILL spill;          /* what was handed on of a long answer */
 int ended;                     /* a68g's output has closed */
 int shown[E_D_A68G_STOP_LINES]; /* last lines shown, newest first */
 int step_over;                 /* Step Over: go on while the line stays */
 int from_line;                 /* the line Step Over started from */
 int same_line_steps;           /* "next" sent again for the same line */
 int kept_running;              /* the terminate question was answered "no" */
} E_D_A68G_RUN;

static E_D_A68G_RUN e_d_a68g_run;

/**
 * e_d_a68g_forget_shown - Forget the lines shown of the answer before.
 * @r: the run.
 */
static void e_d_a68g_forget_shown(E_D_A68G_RUN *r)
{
 int i;

 for (i = 0; i < E_D_A68G_STOP_LINES; i++)
  r->shown[i] = E_D_A68G_LINE_OTHER;
}

/**
 * e_d_a68g_answer_reset - Start reading a new answer from a68g.
 *
 * Called for each command a run sends, so the answer of the one before is
 * not mistaken for this one's prompt.
 */
static void e_d_a68g_answer_reset(void)
{
 e_d_step_reply_init(&e_d_a68g_run.reply);
 e_d_a68g_run.spill.line = -1;
 e_d_a68g_run.spill.finished = 0;
 e_d_a68g_run.ended = 0;
 e_d_a68g_run.kept_running = 0;
 e_d_a68g_forget_shown(&e_d_a68g_run);
}

/**
 * e_d_a68g_run_steps_over - Is the run just handed over a Step Over, the
 * "next" a68g was last sent?
 *
 * Step Over goes out once a68g has answered what it still owes, so it is
 * known for what it is by the command that went out, not by the key.
 */
static int e_d_a68g_run_steps_over(void)
{
 return(!strcmp(e_d_last_cmd, E_D_A68G_NEXT));
}

/**
 * e_d_a68g_begin - Forget everything of the run before when a run starts,
 * and note whether it is a Step Over from the line the program stands on.
 *
 * F8 then moves the mark to the next line, however many units a68g has to
 * step through on this one (see e_d_a68g_steps_on).
 */
static void e_d_a68g_begin(void)
{
 e_d_a68g_answer_reset();
 e_d_a68g_run.step_over = e_d_a68g_run_steps_over();
 e_d_a68g_run.from_line = e_d_a68g_cur_line;
 e_d_a68g_run.same_line_steps = 0;
}

/**
 * e_d_a68g_answer_fd - Where a68g answers: its output pipe, which the
 * program writes to as well.
 */
static int e_d_a68g_answer_fd(void)
{
 return(wfildes[0]);
}

/**
 * e_d_a68g_no_output_fd - a68g gives the program no pty of its own.
 */
static int e_d_a68g_no_output_fd(void)
{
 return(-1);
}

/**
 * e_d_a68g_recent_kinds - The kinds of the last E_D_A68G_STOP_LINES whole
 * lines before @upto, newest first.
 * @r: the run; @upto: where the whole lines end in the answer (just past a
 * line end, or 0); @kinds: filled with E_D_A68G_STOP_LINES kinds.
 *
 * Lines already shown, and so dropped from the answer, count too: the
 * stop's lines may have come, and been shown, before its prompt.  A line
 * whose start was handed on before is only the end of a line, and stops
 * the count.
 */
static void e_d_a68g_recent_kinds(const E_D_A68G_RUN *r, int upto, int *kinds)
{
 const E_D_STEP_REPLY *rep = &r->reply;
 int n = 0, end = upto, start, i, kind;

 while (n < E_D_A68G_STOP_LINES && end > 0)
 {
  start = end - 1;
  while (start > 0 && rep->buf[start - 1] != '\n')
   start--;
  if (start == 0 && rep->mid_line)
  {
   kinds[n++] = E_D_A68G_LINE_OTHER;
   break;
  }
  kind = e_ds_a68g_line_kind(rep->buf + start, end - 1 - start);
  end = start;
  if (kind != E_D_A68G_LINE_QUESTION)   /* the question is not in a stop */
   kinds[n++] = kind;
 }
 for (i = n; i < E_D_A68G_STOP_LINES; i++)
  kinds[i] = r->shown[i - n];
}

/**
 * e_d_a68g_prompt_ends_answer - Has a68g stopped: does the answer read so
 * far end with its prompt, right after the lines of a stop?
 * @r: the run.
 *
 * a68g writes the program's text unbuffered on the same pipe, so a program
 * that prints "(a68g) " with no line end looks like a stop on its own, and
 * so does one that also prints "This is the main thread" before it.  Only
 * a real stop puts the numbered source line, the column marker and the
 * thread line before it, so the user's debugger keys stay refused until
 * the program really stops.
 * Return: 1 when a68g stopped.
 */
static int e_d_a68g_prompt_ends_answer(const E_D_A68G_RUN *r)
{
 const E_D_STEP_REPLY *rep = &r->reply;
 const char *end = strrchr(rep->buf, '\n');
 int kinds[E_D_A68G_STOP_LINES];
 int tail = 0;

 if (end)
  tail = (int)(end - rep->buf) + 1;
 if (strcmp(rep->buf + tail, E_D_A68G_PROMPT))
  return(0);
 if (tail == 0 && rep->mid_line)
  return(0);
 e_d_a68g_recent_kinds(r, tail, kinds);
 return(e_ds_a68g_lines_make_a_stop(kinds));
}

/**
 * e_d_a68g_note_shown_lines - Remember the kinds of the last whole lines
 * read so far, before they are shown and dropped from the answer.
 * @r: the run.
 */
static void e_d_a68g_note_shown_lines(E_D_A68G_RUN *r)
{
 const char *end = strrchr(r->reply.buf, '\n');
 int kinds[E_D_A68G_STOP_LINES], i;

 if (!end)
  return;
 e_d_a68g_recent_kinds(r, (int)(end - r->reply.buf) + 1, kinds);
 for (i = 0; i < E_D_A68G_STOP_LINES; i++)
  r->shown[i] = kinds[i];
}

/**
 * e_d_a68g_show_lines_as_they_come - Show the whole lines read so far in
 * Messages, when the window is open.
 * @r: the run.
 *
 * The user sees what the program prints while it runs.  With Messages
 * closed the lines wait in the answer, so no window is opened by the fd
 * loop (it could land on a box); they are shown at the stop.
 */
static void e_d_a68g_show_lines_as_they_come(E_D_A68G_RUN *r)
{
 if (!e_d_find_messages_window())
  return;
 e_d_a68g_note_shown_lines(r);
 e_d_step_reply_hand_on_lines(&r->reply, e_d_a68g_spill, &r->spill);
}

/**
 * e_d_a68g_asks_to_terminate - Does the answer read so far end with a68g's
 * question whether to terminate, which an interrupt makes it ask?
 * @r: the run.
 * Return: 1 when a68g waits for yes or no.
 */
static int e_d_a68g_asks_to_terminate(const E_D_A68G_RUN *r)
{
 const char *end = strrchr(r->reply.buf, '\n');
 const char *tail = end ? end + 1 : r->reply.buf;

 return(!strncmp(tail, E_D_A68G_TERMINATE, sizeof E_D_A68G_TERMINATE - 1));
}

/**
 * e_d_a68g_keep_running_after_interrupt - Answer a68g's terminate question
 * with "no", once, so the interrupt only stops the program.
 * @r: the run.
 *
 * Pause sends a68g SIGINT; a68g prints where it is and asks whether to
 * terminate.  "no" leaves it at its prompt on that line, which is the stop
 * the user asked for.  a68g reads this answer from the stdin the program
 * shares, but it is waiting for it, so nothing else is sent meanwhile.
 */
static void e_d_a68g_keep_running_after_interrupt(E_D_A68G_RUN *r)
{
 if (r->kept_running)
  return;
 r->kept_running = 1;
 e_d_pause_taken();
 e_d_send_cmd(E_D_A68G_KEEP);
}

/**
 * e_d_a68g_feed - Read what a68g or the program has written so far.
 * @f: the window the run was started from.
 *
 * The program's text and a68g's prompt arrive on one pipe, the prompt
 * without a line end, so the run stops when the prompt ends the answer, or
 * when the pipe closes because the program ended.  Until then every whole
 * line is shown in Messages as it comes (see
 * e_d_a68g_show_lines_as_they_come).  A program that prints a lot is read
 * a bounded amount at a time so the editor keeps its keys.
 * Return: E_D_FEED_GOING or E_D_FEED_STOPPED.
 */
static int e_d_a68g_feed(FENSTER *f)
{
 E_D_A68G_RUN *r = &e_d_a68g_run;
 int flags = fcntl(wfildes[0], F_GETFL, 0);
 int reads, n, fed = E_D_FEED_GOING;

 r->spill.f = f;
 fcntl(wfildes[0], F_SETFL, flags | O_NONBLOCK);
 for (reads = 0; reads < E_D_A68G_FEED_READS_MAX && fed == E_D_FEED_GOING;
      reads++)
 {
  n = e_d_step_reply_read(wfildes[0], &r->reply, e_d_a68g_spill, &r->spill);
  if (n < 0)
   break;                 /* nothing more for now */
  if (n == 0)
  {  /* a68g exited: its last text is the program's end */
   r->ended = 1;
   fed = E_D_FEED_STOPPED;
  }
  else if (e_d_a68g_asks_to_terminate(r))
   e_d_a68g_keep_running_after_interrupt(r);
  else if (e_d_a68g_prompt_ends_answer(r))
   fed = E_D_FEED_STOPPED;
  else
   e_d_a68g_show_lines_as_they_come(r);
 }
 fcntl(wfildes[0], F_SETFL, flags);
 return(fed);
}

/**
 * e_d_a68g_say_end - The program ended: say how, and close the session.
 * @f: the window on top.
 *
 * The user reads one line in Messages that the program exited, and the
 * program's last output is in Messages and the User Screen already.
 */
static void e_d_a68g_say_end(FENSTER *f)
{
 E_D_A68G_RUN *r = &e_d_a68g_run;

 if (r->ended && !r->spill.finished &&
     !strstr(r->reply.buf, E_D_A68G_FINISHED))
  e_d_note_output_closed();   /* a68g died: the end says so */
 e_d_switch_out(0);
 e_d_say_program_exited(r->reply.buf, f);
 e_d_quit(f);
}

static void e_d_ga68_stop_ask(ECNT *cn, int line);   /* (below) */

/**
 * e_d_a68g_steps_on - Does Step Over go on from this stop?
 * @line: the line a68g reported.
 *
 * a68g steps by interruptable unit, so one line with several units, such as
 * a print with several arguments, is reported again and again.  Step Over
 * goes on until the line changes, so one press of F8 moves to the next line
 * and one press on the last line runs the program to its end.  Step Into
 * stays unit by unit.
 * Return: 1 when Step Over should send "next" again (the caller sends it).
 */
static int e_d_a68g_steps_on(int line)
{
 E_D_A68G_RUN *r = &e_d_a68g_run;

 if (!r->step_over || e_d_swtch < 3 || line <= 0 || line != r->from_line)
  return(0);
 return(r->same_line_steps < E_D_A68G_SAME_LINE_MAX - 1);
}

/**
 * e_d_a68g_step_again - Send "next" once more from a stop, and read its
 * answer on the fd loop, so the editor keeps its keys meanwhile.
 */
static void e_d_a68g_step_again(void)
{
 e_d_a68g_run.same_line_steps++;
 e_d_a68g_answer_reset();
 e_d_nstack = 0;
 e_d_send_cmd(E_D_A68G_NEXT);
 e_d_run_resume();
}

/**
 * e_d_a68g_stopped - What a68g's run does when its prompt comes: show the
 * program's output, then say the end, step on, or mark the stop.
 * @f: the window on top.
 *
 * The user sees the program's output in Messages (Ctrl-G P) and the green
 * bar on the line a68g stopped on, or one line saying the program ended.
 * What the stop asks a68g (the watches, the Stack window, the breakpoints
 * changed while it ran) is asked on the fd loop (e_d_ga68_stop_ask), the
 * keys reaching the editor meanwhile.
 */
static void e_d_a68g_stopped(FENSTER *f)
{
 E_D_A68G_RUN *r = &e_d_a68g_run;
 int line;

 line = e_d_a68g_scan(f, r->reply.buf, r->reply.mid_line);
 if (line <= 0)
  line = r->spill.line;
 e_d_a68g_cur_line = line;   /* drives line-granular stepping */
 if (r->ended || r->spill.finished ||
     strstr(r->reply.buf, E_D_A68G_FINISHED))
 {
  e_d_a68g_say_end(f);
  return;
 }
 if (e_d_a68g_steps_on(line))
 {
  e_d_a68g_step_again();
  return;
 }
 e_d_ga68_stop_ask(f->ed, line);
}

/**
 * e_d_a68g_interrupt_stop - Is a68g's stop the one an interrupt made?
 * @taken: unused: a68g's stop is whole at once.
 *
 * An interrupt makes a68g ask whether to terminate, which the run's reader
 * answers "no" (e_d_a68g_keep_running_after_interrupt).  A breakpoint the
 * user set while the program ran is set at this stop, which the user does
 * not see, and the program runs on.
 * Return: 1 for the interrupt's stop.
 */
static int e_d_a68g_interrupt_stop(int taken)
{
 (void)taken;
 return(e_d_a68g_run.kept_running && !e_d_a68g_run.ended);
}

/**
 * e_d_a68g_sync_stop - a68g stopped on the interrupt sent for a breakpoint
 * change: show what the program printed up to it.
 * @f: the window on top.
 *
 * The program's output reaches Messages as at any stop; the line a68g
 * stopped on is not marked.
 * Return: E_D_SYNC_READY: the monitor takes commands.
 */
static int e_d_a68g_sync_stop(FENSTER *f)
{
 E_D_A68G_RUN *r = &e_d_a68g_run;
 int line = e_d_a68g_scan(f, r->reply.buf, r->reply.mid_line);

 if (line > 0)
  e_d_a68g_cur_line = line;    /* where a Pause pressed meanwhile marks */
 return(E_D_SYNC_READY);
}

/**
 * e_d_a68g_settle - Account for interrupts a68g got after the program had
 * stopped by itself.
 * @cn: unused; @owed: the interrupts not accounted for; @why: unused.
 *
 * Unlike gdb and pdb, a68g writes nothing when an interrupt meets it at its
 * prompt (measured on a68g 3.1.2): no line and no second prompt, so there
 * is nothing for a settle command to take back before the stop's requests,
 * and a68g's tables have no settle step.  It keeps the interrupt and stops
 * the program on it at the next continue or step, before the program moves,
 * asking whether to terminate after a continue; the run's reader answers
 * "no" and the controller sends the command again.
 * Return: @owed, all still pending.
 */
static int e_d_a68g_settle(ECNT *cn, int owed, int why)
{
 (void)cn;
 (void)why;
 return(owed);
}

static int e_d_a68g_sync_on_loop(ECNT *cn);   /* (below) */

/* a68g: one pipe for the monitor and the program, a prompt at each stop.
   No run_to_line: a68g has no stop at the start of main to run on from, so
   Go to cursor as the first command is a case of its own.  A breakpoint
   changed while the program runs is applied by interrupting it.  What a
   stop asks a68g is asked on the fd loop, through the request engine
   (e_d_ga), so a68g has no sync_at_stop: the stop's own requests tell it
   the breakpoints changed during the run. */
static const E_D_RUN_OPS e_d_a68g_ops =
{
 "a68g",
 e_d_a68g_answer_fd,
 e_d_a68g_no_output_fd,
 e_d_a68g_feed,
 e_d_a68g_stopped,
 e_d_interrupt_debugger,
 NULL,
 NULL,
 NULL,
 e_d_a68g_begin,
 NULL,
 0,
 e_d_a68g_interrupt_stop,
 e_d_a68g_sync_stop,
 e_d_a68g_settle,
 e_d_a68g_answer_reset,
 0,
 NULL,
 e_d_a68g_sync_on_loop
};

/**
 * e_d_a68g_run_ops - The run controller's table for a68g.
 */
static const E_D_RUN_OPS *e_d_a68g_run_ops(void)
{
 return(&e_d_a68g_ops);
}

/**
 * e_d_a68g_step_go - Step Over (@sw 1) or Step Into (@sw 0) under a68g,
 * once a68g has answered what it still owes.
 * @f: the source window.
 *
 * The user keeps the editor while a68g answers; the step goes out after,
 * and a key typed meanwhile waits for its stop.  Step Over is line by line
 * (see e_d_a68g_steps_on).
 * Return: 0, or the end of the session.
 */
static int e_d_a68g_step_go(FENSTER *f, int sw)
{
 e_d_nstack = 0;
 if (sw)
  return(e_d_ga_go_step(f, E_D_A68G_NEXT, NULL));
 return(e_d_ga_go_step(f, E_D_A68G_STEP, NULL));
}

/* The call stack under pdb, jdb, a68g and the debug adapters.  Each prints
   its stack its own way, so it is read into one list of frames, innermost
   first: the Stack window shows it in one format, Enter on a row marks that
   frame's line, and Finish function knows whether the function stopped in
   has a caller to return to. */
#define E_D_FRAMES_MAX       64
#define E_D_FRAME_NAME_MAX   128
#define E_D_FRAME_ROW_MAX    (E_D_FRAME_NAME_MAX + E_D_STOP_PATH_MAX + 32)
#define E_D_FRAME_NO_LINE    0
#define E_D_FRAME_ROW_MARK   '#'      /* every frame row starts "#N  "        */
#define E_D_PDB_OWN_FILE     "<string>" /* pdb's exec of the script: the last
                                           frame of pdb's own machinery  */
#define E_D_JDB_FINISH       "step up\n"
#define E_D_A68G_CALLS_FMT   "calls %d"  /* a68g's stack, innermost first    */
#define E_D_A68G_FINISH      "finish\n"
#define E_D_FINISH_OUTERMOST "Finish function: not meaningful in the outermost frame."

typedef struct
{
 char func[E_D_FRAME_NAME_MAX];   /* the function, as the debugger names it  */
 char file[E_D_STOP_PATH_MAX];    /* its source, a path the editor opens;
                                     "" when the debugger names none        */
 int line;                        /* 1-based, or E_D_FRAME_NO_LINE           */
 int user;                        /* 1 for the user's own code               */
 int dap_id;                      /* a debug adapter's frameId               */
} e_d_frame;

static e_d_frame e_d_frames[E_D_FRAMES_MAX];
static int e_d_nframes = 0;

/**
 * e_d_frames_backend - Is the stack of the running debugger read into frames?
 *
 * pdb, jdb, a68g and the debug adapters; gdb and the legacy debuggers keep
 * their own Stack window, filled with the debugger's text as it prints it.
 * Return: 1 for a frames debugger.
 */
static int e_d_frames_backend(void)
{
 return(e_deb_type == DEB_PDB || e_deb_type == DEB_JDB ||
        e_deb_type == DEB_A68G || e_deb_type == DEB_DAP);
}

/**
 * e_d_frame_add - Append one frame, the next one out, to the list.
 * @func: its function; @file: its source path or ""; @line: its line or
 * E_D_FRAME_NO_LINE; @user: 1 for the user's own code.
 *
 * Frames past E_D_FRAMES_MAX are dropped: a runaway recursion still shows
 * its innermost frames, the ones the user is looking at.
 * Return: the frame's index, or -1 when the list is full.
 */
static int e_d_frame_add(const char *func, const char *file, int line, int user)
{
 e_d_frame *fr;

 if (e_d_nframes >= E_D_FRAMES_MAX)
  return(-1);
 fr = &e_d_frames[e_d_nframes];
 snprintf(fr->func, sizeof fr->func, "%s", func);
 snprintf(fr->file, sizeof fr->file, "%s", file);
 fr->line = line;
 fr->user = user;
 fr->dap_id = 0;
 return(e_d_nframes++);
}

/**
 * e_d_frames_drop_outer - Forget the frames from @first outwards.
 * @first: index of the first frame to drop.
 *
 * pdb lists its own machinery outside the script; the user never wrote it,
 * so it is not offered as a frame to look at or to finish into.
 */
static void e_d_frames_drop_outer(int first)
{
 if (first < e_d_nframes)
  e_d_nframes = first;
}

/**
 * e_d_frames_reverse - Turn an outermost-first list innermost first.
 *
 * pdb prints its stack from the outside in; the Stack window lists every
 * debugger's frames innermost first, as gdb's backtrace does.
 */
static void e_d_frames_reverse(void)
{
 e_d_frame tmp;
 int i, j;

 for (i = 0, j = e_d_nframes - 1; i < j; i++, j--)
 {
  tmp = e_d_frames[i];
  e_d_frames[i] = e_d_frames[j];
  e_d_frames[j] = tmp;
 }
}

/**
 * e_d_frames_next_row - Cut the next line out of the debugger's answer.
 * @p: in: where the line starts; out: where the next one starts.
 * @row, @n: receives the line without its end.
 * Return: 1 while there was a line, else 0.
 */
static int e_d_frames_next_row(const char **p, char *row, size_t n)
{
 size_t len;
 const char *end;

 if (!**p)
  return(0);
 end = strchr(*p, '\n');
 if (!end)
  end = *p + strlen(*p);
 len = end - *p;
 if (len >= n)
  len = n - 1;
 memcpy(row, *p, len);
 row[len] = '\0';
 if (len > 0 && row[len - 1] == '\r')
  row[len - 1] = '\0';
 *p = *end ? end + 1 : end;
 return(1);
}

/**
 * e_d_frames_skip_prompt - Step over prompts at the start of a line.
 * @row: one line of an answer.
 *
 * a68g prints an answer right after its prompt, on the prompt's line.
 * Return: where the line's own text starts.
 */
static const char *e_d_frames_skip_prompt(const char *row)
{
 const char *prompt = e_d_prompt_text();
 size_t plen;

 if (!prompt)
  return(row);
 plen = strlen(prompt);
 while (!strncmp(row, prompt, plen))
  row += plen;
 return(row);
}

/* One frame of pdb's "where" ("> /dir/p.py(2)sq()->9", the selected one,
   or "  /dir/p.py(6)<module>()"; the "-> source" lines under each are no
   frames) is read by e_ds_pdb_frame_parse (we_debsess.c). */

/**
 * e_d_pdb_take_frames - Read pdb's whole stack, its SIGINT handler
 * included, out of its answer to "where".
 * @reply: the answer.
 *
 * pdb lists outermost first and starts with its own frames (Python's
 * runpy, pdb.py, bdb.py, then "<string>", the exec of the script); those
 * are dropped, and the rest is turned innermost first.
 */
static void e_d_pdb_take_frames(const char *reply)
{
 char row[E_D_FRAME_ROW_MAX];
 const char *p = reply ? reply : "";
 E_DS_FRAME fr;
 int own = -1;

 e_d_nframes = 0;
 while (e_d_frames_next_row(&p, row, sizeof row))
 {
  if (!e_ds_pdb_frame_parse(e_d_frames_skip_prompt(row), &fr))
   continue;
  if (own < 0 && !strcmp(fr.file, E_D_PDB_OWN_FILE))
   own = e_d_nframes;
  e_d_frame_add(fr.func, fr.file, fr.line, 1);
 }
 e_d_frames_reverse();
 if (own >= 0)
  e_d_frames_drop_outer(e_d_nframes - own - 1);
}

/* The function of pdb's own SIGINT handler: the frame under the script's. */
#define E_D_PDB_SIGINT_HANDLER "sigint_handler"

/* pdb's own sources: a stop in either is never a place the user has text of */
#define E_D_PDB_OWN_SOURCES_N  2
static const char *const e_d_pdb_own_sources[E_D_PDB_OWN_SOURCES_N] =
{
 "pdb.py", "bdb.py"
};
#define E_D_PDB_PAUSE_LOST_LINE \
 " pdb stayed in its own code: Pause could not reach the script's line"

/**
 * e_d_pdb_in_own_source - Say whether a stack frame lies in pdb.py or
 * bdb.py.
 * @file: the frame's file, with or without its directory.
 *
 * The user benefits because the Pause stop is only shown when it is in
 * the script, never in the debugger's own code.
 */
static int e_d_pdb_in_own_source(const char *file)
{
 const char *base = strrchr(file, '/');
 int i;

 if (base)
  base++;
 else
  base = file;
 for (i = 0; i < E_D_PDB_OWN_SOURCES_N; i++)
  if (!strcmp(base, e_d_pdb_own_sources[i]))
   return(1);
 return(0);
}

/**
 * e_d_pdb_at_handler_return - Say whether pdb is stopped at the end of its
 * SIGINT handler, the point where one continue resumes the script.
 * @handler: the index of the handler in the stack read, innermost first.
 *
 * A script that sleeps or waits never stops again by itself once the
 * handler returns into it, so this is the stop the user is shown; "return"
 * is not sent from here, or the script would run on and the Pause be lost.
 */
static int e_d_pdb_at_handler_return(int handler)
{
 return(handler == 0 && e_d_pdb_run.returned);
}

/**
 * e_d_pdb_pause_note_script - Remember the script's line at the Pause stop.
 * @fr: the frame under pdb's SIGINT handler.
 *
 * Python 3.13 lists, after the handler has returned, only pdb's own frames:
 * the line the user paused on is then known only from the first stop.
 */
static void e_d_pdb_pause_note_script(const e_d_frame *fr)
{
 snprintf(e_d_pdb_run.pause_file, sizeof e_d_pdb_run.pause_file, "%s",
          fr->file);
 e_d_pdb_run.pause_line = fr->line;
 e_d_pdb_run.pause_seen = 1;
}

/**
 * e_d_pdb_is_handler_frame - Say whether a frame is pdb's own SIGINT
 * handler.
 * @fr: a frame of pdb's stack.
 *
 * A script may have a function of the same name; only the one in pdb's own
 * files is taken for the handler, so the user's frames of that name, and
 * the frames inside them, stay in the Stack window and can be selected.
 */
static int e_d_pdb_is_handler_frame(const e_d_frame *fr)
{
 if (strcmp(fr->func, E_D_PDB_SIGINT_HANDLER))
  return(0);
 return(e_d_pdb_in_own_source(fr->file));
}

/**
 * e_d_pdb_handler_index - Find pdb's SIGINT handler in the frames read.
 *
 * Everything inside it is pdb's own machinery; knowing where it sits lets
 * the Stack window start at the user's code after a Pause.
 *
 * Return: its index, innermost first, or e_d_nframes when it is not there.
 */
static int e_d_pdb_handler_index(void)
{
 int i;

 for (i = 0; i < e_d_nframes; i++)
  if (e_d_pdb_is_handler_frame(&e_d_frames[i]))
   break;
 return(i);
}

/**
 * e_d_frames_drop_inner - Forget the @count innermost frames.
 * @count: how many to drop.
 *
 * What a Pause leaves inside the script's frame is pdb's own machinery; the
 * Stack window then starts at the user's code.  Rows are numbered from the
 * first one kept, and pdb's "up" and "down" are relative to the selected
 * row, so the hidden frames need no offset when a frame is selected.
 */
static void e_d_frames_drop_inner(int count)
{
 if (count <= 0 || count > e_d_nframes)
  return;
 memmove(e_d_frames, e_d_frames + count,
         (e_d_nframes - count) * sizeof e_d_frames[0]);
 e_d_nframes -= count;
}

/**
 * e_d_pdb_take_stack - Read pdb's stack for the Stack window out of its
 * answer to "where".
 * @reply: the answer.
 *
 * After a Pause pdb's SIGINT handler, and the bdb.py frames inside it, sit
 * on top of the script's frame; they are dropped so the window lists only
 * the script's frames.
 */
static void e_d_pdb_take_stack(const char *reply)
{
 e_d_pdb_take_frames(reply);
 e_d_frames_drop_inner(e_d_pdb_handler_index() + 1);
}

/**
 * e_d_pdb_pause_needs_return - Say whether a Pause stop is still in pdb's
 * own code, so that "return" must be sent.
 * @handler: the index of the SIGINT handler in the stack read.
 *
 * The user benefits because the Pause is then shown at the script's line
 * and never inside pdb.py or bdb.py.
 */
static int e_d_pdb_pause_needs_return(int handler)
{
 if (handler >= e_d_nframes && !e_d_pdb_run.pause_seen)
  return(0);
 if (e_d_pdb_at_handler_return(handler) || e_d_nframes <= 0)
  return(0);
 return(e_d_pdb_in_own_source(e_d_frames[0].file));
}

/**
 * e_d_pdb_interrupt_stop - Is pdb's stop the one an interrupt made?
 * @taken: the interrupt's stop was seen already, and pdb is being made to
 * leave its own code: every stop on that way is still the interrupt's.
 * Return: 1 for the interrupt's stop.
 */
static int e_d_pdb_interrupt_stop(int taken)
{
 if (taken)
  return(1);
 return(strstr(e_d_pdb_run.reply.buf, E_D_PDB_INTERRUPTED) != NULL);
}

/**
 * e_d_pdb_capture_before_interrupt - Keep for Ctrl-G P what the script
 * printed before pdb's "Program interrupted", and not pdb's own words.
 * @r: the run.
 */
static void e_d_pdb_capture_before_interrupt(E_D_PDB_RUN *r)
{
 char *at = strstr(r->reply.buf, E_D_PDB_INTERRUPTED);
 char held = '\0';

 if (at)
 {
  held = *at;
  *at = '\0';
 }
 e_d_pdb_capture_output(r->reply.buf, r->reply.mid_line &&
                        r->spill.open_is_pdb);
 if (at)
  *at = held;
}

/**
 * e_d_pdb_sync_must_leave - Must pdb be made to leave its own code before
 * the breakpoint change is sent and the script continued?
 * @handler: the index of pdb's SIGINT handler in the stack read.
 *
 * With a breakpoint set, pdb's interrupt stops inside bdb.py, where a
 * continue only ends pdb's own step and leaves the script stopped (F251);
 * "return" leaves those frames one by one, up to the bound Pause uses.
 * Return: 1 when "return" is to be sent.
 */
static int e_d_pdb_sync_must_leave(int handler)
{
 if (!e_d_pdb_pause_needs_return(handler))
  return(0);
 return(++e_d_pdb_run.internal_steps <= E_D_PDB_INTERNAL_STEPS_MAX);
}

/* One frame of jdb's "where" ("  [1] P.sq (P.java:3)", possibly after a
   prompt "main[1] ", or "  [3] java.lang.Thread.run (native method)", which
   gets no line and no file; "1,620" is line 1620) is read by
   e_ds_jdb_frame_parse (we_debsess.c). */

/**
 * e_d_jdb_frame_source - Find the source of a jdb frame's class.
 * @cn: the editor; @func: the frame's method, "com.x.P.sq"; @out, @n: the
 * path.
 *
 * Found the way a stop's source is found, so the frames of the user's
 * classes open in the window the stop marker uses.
 * Return: 1 when found.
 */
static int e_d_jdb_frame_source(ECNT *cn, const char *func, char *out, size_t n)
{
 char cls[E_D_FRAME_NAME_MAX];
 char *method_dot, *nested;

 snprintf(cls, sizeof cls, "%s", func);
 method_dot = strrchr(cls, '.');
 if (!method_dot)
  return(0);
 *method_dot = '\0';
 nested = strchr(cls, E_D_JDB_NESTED_SEP);
 if (nested)
  *nested = '\0';
 return(e_d_jdb_find_source(cn, cls, out, n));
}

/**
 * e_d_jdb_frames_kept - Is the stack read at this stop still the one to
 * show, so jdb is not asked "where" again?
 *
 * jdb's "where" starts at the selected frame, so with an outer frame
 * selected it would miss the inner ones.  The stack does not change while
 * the program stays stopped, and every stop selects the innermost frame
 * again, so the list read at the stop is kept while a frame is selected.
 */
static int e_d_jdb_frames_kept(void)
{
 return(e_d_nstack > 0 && e_d_nframes > 0);
}

/**
 * e_d_jdb_take_frames - Read jdb's stack out of its answer to "where".
 * @cn: the editor; @reply: the answer.
 *
 * A frame counts as the user's when its class's source is found; it then
 * opens in the window the stop marker uses.  A prompt jdb wrote in front
 * of a row (one that ended an event) is stepped over by the row parser.
 */
static void e_d_jdb_take_frames(ECNT *cn, const char *reply)
{
 char row[E_D_FRAME_ROW_MAX], path[E_D_STOP_PATH_MAX];
 const char *p = reply ? reply : "";
 E_DS_FRAME fr;
 int user;

 e_d_nframes = 0;
 while (e_d_frames_next_row(&p, row, sizeof row))
 {
  if (!e_ds_jdb_frame_parse(row, &fr))
   continue;
  user = e_d_jdb_frame_source(cn, fr.func, path, sizeof path);
  if (user)
   e_d_frame_add(fr.func, path, fr.line, user);
  else
   e_d_frame_add(fr.func, fr.file, fr.line, user);
 }
}

/* The procedure a68g's "calls" shows ("PROC sq = (INT v) INT:" gives "sq",
   a text that is no PROC is shown as it is) is read by e_ds_a68g_proc_name
   (we_debsess.c). */

/**
 * e_d_a68g_take_frames - Read a68g's stack out of its answer to "calls".
 * @cn: the editor; @reply: the answer.
 *
 * a68g's "calls" lists the procedures being run, innermost first, each by
 * the line of its declaration; it names no call sites.  So the innermost
 * frame gets the line the program is stopped on, an outer procedure its
 * declaration line, and the main program, which calls them all, ends the
 * list without a line.  Stopped in the main program, it is the only frame.
 */
static void e_d_a68g_take_frames(ECNT *cn, const char *reply)
{
 char row[E_D_FRAME_ROW_MAX], name[E_D_FRAME_NAME_MAX], path[E_D_STOP_PATH_MAX];
 const char *p = reply ? reply : "", *text;
 int iw = e_d_a68g_window(cn), line;

 e_d_nframes = 0;
 path[0] = '\0';
 if (iw > 0)
  e_d_window_path_of(cn, cn->f[iw]->datnam, path, sizeof path);
 while (e_d_frames_next_row(&p, row, sizeof row))
 {
  text = e_d_frames_skip_prompt(row);
  line = e_ds_a68g_parse_line(text);
  if (line <= 0)
   continue;
  while (isdigit((unsigned char)*text))
   text++;
  e_ds_a68g_proc_name(text, name, sizeof name);
  if (e_d_nframes == 0)
   line = e_d_a68g_cur_line;
  e_d_frame_add(name, path, line, 1);
 }
 if (e_d_nframes == 0)
  e_d_frame_add(E_D_A68G_MAIN_FRAME, path, e_d_a68g_cur_line, 1);
 else
  e_d_frame_add(E_D_A68G_MAIN_FRAME, path, E_D_FRAME_NO_LINE, 1);
}

/**
 * e_d_dap_take_frames - Keep a debug adapter's stack, from its answer to
 * the stop's stackTrace.
 * @ans: the answer.
 *
 * The adapter lists innermost first and goes on into the language runtime
 * (Go's runtime.main, Rust's std, a C start routine without source); those
 * frames are listed too, but are not the user's, so Finish function never
 * returns into them.
 */
static void e_d_dap_take_frames(const E_D_ANSWER *ans)
{
 static e_dap_frame got[E_D_FRAMES_MAX];
 int i, n, k, user;

 e_d_nframes = 0;
 n = e_dap_answer_frames(ans, got, E_D_FRAMES_MAX);
 for (i = 0; i < n; i++)
 {
  user = got[i].path[0] && e_d_dap_path_in_user_code(got[i].path);
  k = e_d_frame_add(got[i].name, got[i].path, got[i].line, user);
  if (k >= 0)
   e_d_frames[k].dap_id = got[i].id;
 }
}

/**
 * e_d_collect_frames - Read the running debugger's stack into the list.
 * @cn: the editor.
 *
 * Return: 0.
 */
static int e_d_collect_frames(ECNT *cn)
{
 (void)cn;
 if (e_deb_type == DEB_JDB)
  return(0);       /* jdb's frames are asked on the fd loop (e_d_gj_frames_*) */
 if (e_deb_type == DEB_A68G)
  return(0);       /* a68g's are asked on the fd loop (e_d_ga68_frames_*) */
 if (e_deb_type == DEB_DAP)
  return(0);       /* an adapter's are asked at each stop (e_d_dap_stop_steps) */
 e_d_nframes = 0;
 return(0);        /* pdb's frames are asked on the fd loop (e_d_gp_frames_*) */
}

/**
 * e_d_frame_row - The Stack window's row for one frame.
 * @fr: the frame; @index: its place, 0 innermost; @row, @n: the text.
 *
 * "#0  sq at p.py:2", like gdb's backtrace: the user reads which function
 * runs where, and Enter on the row finds the frame by its number.
 */
static void e_d_frame_row(const e_d_frame *fr, int index, char *row, size_t n)
{
 const char *base = strrchr(fr->file, DIRC);

 if (base)
  base++;
 else
  base = fr->file;
 if (!base[0])
  snprintf(row, n, "%c%d  %s", E_D_FRAME_ROW_MARK, index, fr->func);
 else if (fr->line == E_D_FRAME_NO_LINE)
  snprintf(row, n, "%c%d  %s in %s", E_D_FRAME_ROW_MARK, index, fr->func, base);
 else
  snprintf(row, n, "%c%d  %s at %s:%d", E_D_FRAME_ROW_MARK, index, fr->func,
           base, fr->line);
}

/**
 * e_d_stack_window_fill - Write the frames into the Stack window's buffer.
 * @b: the Stack window's buffer.
 */
static void e_d_stack_window_fill(BUFFER *b)
{
 char row[E_D_FRAME_ROW_MAX];
 int i;

 e_p_red_buffer(b);
 FREE(b->bf[0].s);
 b->mxlines = 0;
 for (i = 0; i < e_d_nframes; i++)
 {
  e_d_frame_row(&e_d_frames[i], i, row, sizeof row);
  print_to_end_of_buffer(b, row, b->mx.x);
 }
 e_new_line(b->mxlines, b);
}

/**
 * e_d_p_frames - Show the stack in the Stack window (Ctrl-G K) and keep it
 * current at every stop.
 * @cn: the editor; @is: the Stack window; @sw: 1 to bring it to the top.
 *
 * The user sees every frame, innermost first, with its function, file and
 * line, whichever debugger runs the program.
 * Return: 0, or -1 when the debugger's output ended.
 */
static int e_d_p_frames(ECNT *cn, int is, int sw)
{
 if (e_d_collect_frames(cn) == -1)
  return(-1);
 e_d_frames_put_in_window(cn, is, sw);
 return(0);
}

/**
 * e_d_frames_put_in_window - Show the frames read in Stack window @is.
 * @cn: the editor; @sw: 1 to bring the window to the top.
 *
 * The user sees every frame, innermost first, with its function, file and
 * line, however the debugger's answer was read.
 */
static void e_d_frames_put_in_window(ECNT *cn, int is, int sw)
{
 e_d_stack_window_fill(cn->f[is]->b);
 fk_cursor(1);
 if (sw && is != cn->mxedt)
  e_switch_window(cn->edt[is], cn->f[cn->mxedt]);
 else
  e_rep_win_tree(cn);
}

/**
 * e_d_stack_row_frame - The frame the cursor's row in the Stack window shows.
 * @b: the Stack window's buffer.
 * Return: the frame's number, or -1 above the first frame.
 */
static int e_d_stack_row_frame(BUFFER *b)
{
 int i;

 for (i = b->b.y; i >= 0 && i < b->mxlines; i--)
  if (b->bf[i].s[0] == E_D_FRAME_ROW_MARK)
   return(atoi((char *)b->bf[i].s + 1));
 return(-1);
}

/**
 * e_d_dap_frame_select - Select frame @index of a debug adapter's stack for
 * the watches.
 * @index: the frame, 0 innermost.
 *
 * Its variables are then the ones the Watches window shows, as after gdb's
 * "up" and "down".  pdb, jdb and a68g move their frame through their own
 * requests on the fd loop (e_d_ga_follow_frame).
 */
static void e_d_dap_frame_select(int index)
{
 if (index == e_d_nstack)
  return;
 e_dap_select_frame(g_dap, e_d_frames[index].dap_id);
 e_d_nstack = index;
}

/**
 * e_d_frame_follow_refused - Enter on a row of the Stack window under gdb,
 * pdb, jdb or a68g when the debugger cannot be asked now: while it answers,
 * the waiting line says which answer the editor waits for, as for every
 * other debugger key; while a session starts, the start's line.
 * @f: the Stack window.
 *
 * Nothing is sent: a frame move sent now would be answered out of turn.
 * Return: 0.
 */
static int e_d_frame_follow_refused(FENSTER *f)
{
 if (!e_d_ga_refused(f))
  e_d_start_refused(f);
 return(0);
}

/**
 * e_d_jdb_thread_id - The id of the thread a row of jdb's "threads" names.
 * @row: "  (java.lang.Thread)0x1 main   running", or a "Group main:" row,
 * possibly after a prompt jdb wrote at the end of an event;
 * @id, @n: receives "0x1".
 * Return: 1 when @row names a thread.
 */
static int e_d_jdb_thread_id(const char *row, char *id, size_t n)
{
 const char *p = e_d_jdb_skip_prompts(row);
 size_t len;

 while (*p == ' ')
  p++;
 if (*p != '(' || !(p = strchr(p, ')')))
  return(0);
 p++;
 for (len = 0; p[len] && !isspace((unsigned char)p[len]); len++)
  ;
 if (len == 0 || len >= n)
  return(0);
 memcpy(id, p, len);
 id[len] = '\0';
 return(1);
}

/**
 * e_d_jdb_user_frame - The innermost frame of the user's own code in the
 * frames read.
 * Return: its index, or -1 when none of them has source and a line.
 */
static int e_d_jdb_user_frame(void)
{
 int i;

 for (i = 0; i < e_d_nframes; i++)
  if (e_d_frames[i].user && e_d_frames[i].line != E_D_FRAME_NO_LINE)
   return(i);
 return(-1);
}

/**
 * e_d_frame_goto - Mark the line of a frame the user picked in the Stack
 * window, the way a stop is marked.
 * @f: the Stack window; @fr: the frame.
 * Return: e_d_goto_break's answer, or the error a frame without a source
 * line gets (a library frame), as under gdb.
 */
static int e_d_frame_goto(FENSTER *f, const e_d_frame *fr)
{
 char file[E_D_STOP_PATH_MAX];

 if (!fr->file[0] || fr->line == E_D_FRAME_NO_LINE)
  return(e_error(e_d_msg[ERR_NOSOURCE], 0, f->fb));
 snprintf(file, sizeof file, "%s", fr->file);
 return(e_d_goto_break(file, fr->line, f));
}

/**
 * e_d_frame_line - The line a frame of the Stack window is shown on.
 * @fr: the frame.
 * Return: its line, or -1 for a frame with no source line (a library
 * frame), which the user is then told has no source.
 */
static int e_d_frame_line(const e_d_frame *fr)
{
 if (!fr->file[0] || fr->line == E_D_FRAME_NO_LINE)
  return(-1);
 return(fr->line);
}

/**
 * e_d_frame_show - Enter in the Stack window: go to the frame under the
 * cursor.
 * @f: the Stack window.
 *
 * The frame becomes the selected one, the watches show its variables, and
 * the source window shows its line with the stop marker, as under gdb.
 * Return: 0, 1 when the cursor is on no frame, -1 when the debugger ended.
 */
static int e_d_frame_show(FENSTER *f)
{
 BUFFER *b = f->ed->f[f->ed->mxedt]->b;
 int index = e_d_stack_row_frame(b);
 e_d_frame fr;

 e_d_switch_out(0);
 if (index < 0 || index >= e_d_nframes)
  return(1);
 fr = e_d_frames[index];
 if (e_d_ga_at_stop())
  return(e_d_ga_follow_frame(f->ed, index, fr.file, e_d_frame_line(&fr)));
 if (e_d_ga_backend_of(e_deb_type) || e_d_dr_answering())
  return(e_d_frame_follow_refused(f));
 e_d_dap_frame_select(index);
 if (e_d_p_watches(f, 0) == -1)
  return(-1);
 return(e_d_frame_goto(f, &fr));
}

/**
 * e_d_finish_frame - The frame Finish function returns from.
 *
 * pdb's "return" acts on the selected frame; jdb's "step up", a68g's
 * "finish" and DAP's stepOut on the innermost one.
 * Return: the frame's index.
 */
static int e_d_finish_frame(void)
{
 if (e_deb_type == DEB_PDB)
  return(e_d_nstack);
 return(0);
}

/**
 * e_d_frame_is_outermost - Has frame @index no caller of the user's?
 * @index: the frame, 0 innermost.
 *
 * main has nothing to return to: the language runtime that called it is
 * not the user's code.
 * Return: 1 when the frame is the outermost one.
 */
static int e_d_frame_is_outermost(int index)
{
 return(index + 1 >= e_d_nframes || !e_d_frames[index + 1].user);
}

/**
 * e_d_say_finish_outermost - Tell the user Finish function has nowhere to go.
 * @f: the source window.
 *
 * One line in Messages instead of letting the program run on to its end;
 * the session stays stopped where it was.
 */
static void e_d_say_finish_outermost(FENSTER *f)
{
 char msg[] = E_D_FINISH_OUTERMOST;

 e_d_p_message(msg, f, 0);
}

/**
 * e_d_pdb_finish - Finish function under pdb.
 * @f: the source window.
 *
 * pdb's "return" stops at the end of the function, still inside it; "up"
 * then shows the caller's line, the call the user finishes into, as gdb's
 * "finish" does.  Stepping on goes from the end of the function.  The
 * program runs under the run controller: the editor keeps its keys, and the
 * "up" is sent when pdb stops at the end of the function.
 * Return: 0, or the end of the session.
 */
static int e_d_pdb_finish(FENSTER *f)
{
 int ret;

 e_d_send_cmd(E_D_PDB_FINISH);
 ret = e_d_run_hand_over(f, e_d_pdb_run_ops(), 0);
 if (e_d_run_going())
  e_d_pdb_run.finish_up = 1;
 return(ret);
}

/**
 * e_d_jdb_finish - Finish function under jdb: "step up".
 * @f: the source window.
 *
 * The program runs under the run controller: the editor keeps its keys
 * until the function returns.
 * Return: 0, or the end of the session.
 */
static int e_d_jdb_finish(FENSTER *f)
{
 e_d_send_cmd(E_D_JDB_FINISH);
 return(e_d_jdb_run_started(f, 0));
}

/**
 * e_d_a68g_finish - Finish function under a68g: "finish".
 * @f: the source window.
 *
 * a68g stops at the next unit of the caller after the procedure returns.
 * The program runs under the run controller: the editor keeps its keys
 * until the procedure returns.
 * Return: 0, or the end of the session.
 */
static int e_d_a68g_finish(FENSTER *f)
{
 e_d_send_cmd(E_D_A68G_FINISH);
 return(e_d_run_hand_over(f, e_d_a68g_run_ops(), 0));
}

/**
 * e_d_finish_send - Run the selected debugger's own finish command.
 * @f: the source window.
 * Return: the command's answer.
 */
static int e_d_finish_send(FENSTER *f)
{
 if (e_deb_type == DEB_DAP)
  return(e_d_dap_finish(f));
 if (e_deb_type == DEB_PDB)
  return(e_d_pdb_finish(f));
 if (e_deb_type == DEB_JDB)
  return(e_d_jdb_finish(f));
 return(e_d_a68g_finish(f));
}

/**
 * e_d_finish_to_caller - Finish function (Ctrl-G F) under pdb, jdb, a68g and
 * the debug adapters.
 * @f: the source window.
 *
 * The program runs until the current function returns and the marker lands
 * on the caller's line.  In the outermost frame there is no caller: the user
 * gets one line saying so and the session stays where it was.
 * Return: 0, or the end of the session.
 */
static int e_d_finish_to_caller(FENSTER *f)
{
 int from;

 if (e_deb_type == DEB_DAP && g_dap_first)
  return(e_d_dap_finish(f));      /* the program is not stopped yet */
 from = e_d_finish_frame();
 if (e_deb_type == DEB_DAP && e_d_dap_program_ended())
  return(e_d_dap_end_session(f));
 if (e_d_collect_frames(f->ed) == -1)
  return(e_d_quit(f));
 if (e_d_frame_is_outermost(from))
 {
  e_d_say_finish_outermost(f);
  return(0);
 }
 e_d_delbreak(f);
 e_d_switch_out(1);
 e_d_nstack = 0;
 return(e_d_finish_send(f));
}

/**
 * e_d_gdb_step_from_pause - Step Over or Step Into right after a Pause that
 * stopped in library code.
 * @f: the source window; @sw: 1 for Step Over, 0 for Step Into.
 *
 * gdb steps the innermost frame, which after Pause is the library call the
 * program sleeps in, line by line through code the user has no source for.
 * The paused line is finished first, as in any debugger: the call is run
 * out of with "finish" from the frame below the marked one, and the step
 * is sent when that stops (e_d_gdb_follow_up_after_finish), so the next
 * stop is on a line of the user's code.
 * Return: 0, or the end of the session.
 */
static int e_d_gdb_step_from_pause(FENSTER *f, int sw)
{
 return(e_d_ga_finish_from_frame(f->ed, e_d_pause_below, sw));
}

/**
 * e_d_gdb_finish_out_of_pause - The frame below the marked one is selected:
 * run out of the library call, and step once that stops.
 * @f: the window on top; @sw: 1 for Step Over, 0 for Step Into.
 * Return: 0, or the end of the session.
 */
static int e_d_gdb_finish_out_of_pause(FENSTER *f, int sw)
{
 e_d_nstack = 0;
 if (sw)
  return(e_d_ga_go_step(f, E_D_GDB_FINISH, E_D_GDB_NEXT));
 return(e_d_ga_go_step(f, E_D_GDB_FINISH, E_D_GDB_STEP));
}

/**
 * e_d_gdb_step_go - F7 or F8 under gdb: step, the stack back at its top.
 * @f: the source window; @cmd: gdb's step or next command.
 * Return: 0, or the end of the session.
 */
static int e_d_gdb_step_go(FENSTER *f, const char *cmd)
{
 e_d_nstack = 0;
 return(e_d_ga_go_step(f, cmd, NULL));
}

/* pdb's step commands. */
#define E_D_PDB_NEXT "n\n"
#define E_D_PDB_STEP "s\n"

/**
 * e_d_pdb_step_go - F7 or F8 under pdb: step from the innermost frame,
 * once pdb has answered what it still owes.
 * @f: the source window; @sw: 1 for Step Over, 0 for Step Into.
 * Return: 0, or the end of the session.
 */
static int e_d_pdb_step_go(FENSTER *f, int sw)
{
 e_d_nstack = 0;
 if (sw)
  return(e_d_ga_go_step(f, E_D_PDB_NEXT, NULL));
 return(e_d_ga_go_step(f, E_D_PDB_STEP, NULL));
}

/* jdb's step commands. */
#define E_D_JDB_NEXT "next\n"
#define E_D_JDB_STEP "step\n"

/**
 * e_d_jdb_step_go - F7 or F8 under jdb, once jdb has answered what it still
 * owes (the breakpoints changed since the stop, Go to cursor's own).
 * @f: the source window; @sw: 1 for Step Over, 0 for Step Into.
 *
 * The user keeps the editor while jdb answers; the step goes out after, and
 * a key typed meanwhile waits for its stop.
 * Return: 0, or the end of the session.
 */
static int e_d_jdb_step_go(FENSTER *f, int sw)
{
 e_d_nstack = 0;
 if (sw)
  return(e_d_ga_go_step(f, E_D_JDB_NEXT, NULL));
 return(e_d_ga_go_step(f, E_D_JDB_STEP, NULL));
}

int e_d_step_next(FENSTER *f, int sw)
{
 int ret, main_brk = 0;

 if (sw && e_d_act_deferred(E_D_ACT_NEXT, f, 0))
  return(0);
 if (!sw && e_d_act_deferred(E_D_ACT_STEP, f, 0))
  return(0);
 if (e_d_refuse_while_running(f))
  return(0);
 jdb_trace("e_d_step_next: entry, sw=%d, e_d_swtch=%d, e_deb_type=%d, "
           "e_d_nbrpts=%d\n", sw, e_d_swtch, e_deb_type, e_d_nbrpts);
 e_d_resume_after_build(e_d_step_next, sw);
 ret = 0;
 if (e_d_swtch < 2)
  ret = e_run_debug(f);
 if (e_d_deferred(ret))
  return(0);
 if (ret < 0)
 {
  jdb_trace("e_d_step_next: e_run_debug failed ret=%d\n", ret);
  e_d_quit(f);
  if (ret == -1)
  {  e_show_error(0, f);  return(ret);  }
  return(e_error(e_d_msg[ERR_CANTDEBUG], 0, f->fb));
 }
 if (e_d_swtch < 3)
 {
  /* If the user has breakpoints set, use those instead of adding
     a temporary breakpoint at main(). This way the first F8/F7
     stops at the user's breakpoint, not at main's entry point.
     a68g auto-breaks at line 1, so it skips e_mk_brk_main entirely. */
  if (e_d_nbrpts <= 0 && e_deb_type != DEB_A68G)
  {
   jdb_trace("e_d_step_next: no breakpoints, calling e_mk_brk_main\n");
   if ((main_brk = e_mk_brk_main(f, 0)) < -1) return(main_brk);
   jdb_trace("e_d_step_next: e_mk_brk_main returned main_brk=%d\n", main_brk);
  }
  ret = e_deb_run(f);
  jdb_trace("e_d_step_next: e_deb_run returned ret=%d\n", ret);
  if (e_d_nbrpts <= 0 && e_deb_type != DEB_A68G)
   e_mk_brk_main(f, main_brk);
  return(ret);
 }
 e_d_delbreak(f);
 e_d_switch_out(1);
 if (e_deb_type == DEB_DAP)
  return e_d_dap_step(f, sw);        /* sw -> "next" (over), else "stepIn" */
 if (e_deb_type == DEB_A68G)
  return(e_d_a68g_step_go(f, sw));   /* line-granular Step Over */
 if (e_deb_type == DEB_GDB && e_d_pause_below != E_D_NO_FRAME)
  return(e_d_gdb_step_from_pause(f, sw));
 if (sw && e_deb_type == DEB_GDB)
  return(e_d_gdb_step_go(f, E_D_GDB_NEXT));
 if (e_deb_type == DEB_GDB)
  return(e_d_gdb_step_go(f, E_D_GDB_STEP));
 if (e_deb_type == DEB_PDB)
  return(e_d_pdb_step_go(f, sw));
 if (e_deb_type == DEB_JDB)
  return(e_d_jdb_step_go(f, sw));
 return(e_d_cli_step_go(f, sw));       /* sdb, dbx, xdb */
}

/* The debugger commands Go to cursor and Finish function pass on. */
#define E_D_GOTO_CURSOR 'U'
#define E_D_GOTO_FINISH 'F'


/* The breakpoint Go to cursor sets for itself, removed at the next stop. */
static struct {
 int set;                      /* 1 while it is in the debugger            */
 int wanted;                   /* pdb: set it before the Continue goes out */
 int line;                     /* its 1-based line                         */
 int id;                       /* the number dbx or xdb gave it            */
 char file[PATH_MAX];          /* the window's file name, as breakpoints   */
 char path[PATH_MAX];          /* the full path the debugger is given      */
} e_d_cursor_brk;

/**
 * e_d_cursor_brk_pending - Is Go to cursor's own breakpoint in the debugger?
 *
 * A debug adapter's first stop is the program's start; Go to cursor as the
 * first command must run on from it to the cursor, like a breakpoint of the
 * user's would.
 * Return: 1 between Go to cursor setting the breakpoint and the stop.
 */
static int e_d_cursor_brk_pending(void)
{
 return(e_d_cursor_brk.set);
}

/**
 * e_d_goto_supported - Does the selected debugger do this command?
 * @flag: E_D_GOTO_CURSOR or E_D_GOTO_FINISH.
 *
 * Go to cursor works with every debugger: gdb has "advance", the others get
 * a breakpoint of their own on the line.  Finish function works with gdb
 * ("finish"), pdb ("return", then "up" to the caller), jdb ("step up"),
 * a68g ("finish") and the debug adapters (stepOut); sdb, dbx and xdb keep
 * their old behaviour.
 * Return: 1 when it does.
 */
static int e_d_goto_supported(int flag)
{
 if (flag == E_D_GOTO_CURSOR)
  return(1);
 return(e_deb_type == DEB_GDB || e_d_frames_backend());
}

/**
 * e_d_user_brk_on - Has the user a breakpoint of his own on this line?
 * @file: the window's file name; @line: 1-based.
 *
 * The program stops there anyway, so Go to cursor only continues, and the
 * user's breakpoint is never the one removed at the stop.
 * Return: 1 when he has.
 */
static int e_d_user_brk_on(const char *file, int line)
{
 int i;

 for (i = 0; i < e_d_nbrpts; i++)
  if (e_d_ybrpts[i] == line && !strcmp(e_d_sbrpts[i], file))
   return(1);
 return(0);
}

/**
 * e_d_brk_jdb_class - The class jdb names a breakpoint by: the file name
 * without ".java".
 * @file: the breakpoint's file name; @out: where to put it; @size: its size.
 */
static void e_d_brk_jdb_class(const char *file, char *out, size_t size)
{
 snprintf(out, size, "%s", file);
 WpeStringCutChar(out, '.');
}

/**
 * e_d_brk_line_cmd - Write the command that sets (@set 1) or clears (@set 0)
 * the breakpoint on @line of @file, for a debugger that names a breakpoint
 * by its line: pdb and jdb (a68g is told on the fd loop, e_d_ga68_*).
 * @path: the file's full path, which pdb is given; @out, @size: the command.
 * Return: 1 when the selected debugger has such a command.
 */
static int e_d_brk_line_cmd(int set, const char *file, const char *path,
                            int line, char *out, size_t size)
{
 char cls[PATH_MAX];

 if (e_deb_type == DEB_PDB && set)
  snprintf(out, size, "b %s:%d\n", path, line);
 else if (e_deb_type == DEB_PDB)
  snprintf(out, size, "clear %s:%d\n", path, line);
 else if (e_deb_type == DEB_JDB)
 {
  e_d_brk_jdb_class(file, cls, sizeof(cls));
  if (set)
   snprintf(out, size, "stop at %s:%d\n", cls, line);
  else
   snprintf(out, size, "clear %s:%d\n", cls, line);
 }
 else
  return(0);
 return(1);
}

/* What a68g, pdb and jdb say of a breakpoint they take no more than that. */
#define E_D_A68G_MONITOR_ERROR  "monitor error:"
#define E_D_PDB_ERROR_MARK      "***"
#define E_D_PDB_PAST_END        "End of file"   /* a line past the last */
#define E_D_JDB_NOT_SET         "Unable to set"
#define E_D_JDB_DEFERRED        "Deferring"

/* A debugger's reply is copied for the status row by e_ds_brk_words
   (we_debsess.c): gdb leads an error with a symbol, pdb with "***", and the
   user reads the words. */

/**
 * e_d_brk_line_refuses - Is this line of the selected debugger's reply its
 * refusal of a breakpoint?
 * @line: a line of the debugger's output.
 *
 * pdb, a68g and jdb say it on their output, with no number to look for; the
 * numbered debuggers say it on their error stream, which the reader sees
 * apart.
 * Return: 1 for a refusal.
 */
static int e_d_brk_line_refuses(const char *line)
{
 if (e_deb_type == DEB_PDB)
  return(!strncmp(line, E_D_PDB_ERROR_MARK, strlen(E_D_PDB_ERROR_MARK)) ||
         !strncmp(line, E_D_PDB_PAST_END, strlen(E_D_PDB_PAST_END)));
 if (e_deb_type == DEB_A68G)
  return(strstr(line, E_D_A68G_MONITOR_ERROR) != NULL);
 if (e_deb_type == DEB_JDB)
  return(strstr(line, E_D_JDB_NOT_SET) != NULL);
 return(0);
}

/**
 * e_d_brk_line_defers - Does this line say the debugger keeps the breakpoint
 * for later, not yet set?
 * @line: a line of the debugger's output.
 *
 * jdb sets a breakpoint of a class that is not loaded when the class loads;
 * until then the mark stays hollow.
 * Return: 1 for a deferral.
 */
static int e_d_brk_line_defers(const char *line)
{
 return(e_deb_type == DEB_JDB && strstr(line, E_D_JDB_DEFERRED) != NULL);
}

/**
 * e_d_brk_judge_line - Let one line of a debugger's reply decide what it
 * made of the breakpoint.
 * @ans: the reply so far; @line: the line; @from_errors: it came on the
 * error stream.
 *
 * The first refusal stands; a deferral turns a plain acceptance into
 * "not confirmed yet".
 */
static void e_d_brk_judge_line(E_D_BRK_ANSWER *ans, const char *line,
                               int from_errors)
{
 if (ans->state == E_D_BRK_REFUSED)
  return;
 if (from_errors || e_d_brk_line_refuses(line))
 {
  ans->state = E_D_BRK_REFUSED;
  e_ds_brk_words(ans->msg, sizeof(ans->msg), line);
  return;
 }
 if (ans->state == E_D_BRK_VERIFIED && e_d_brk_line_defers(line))
 {
  ans->state = E_D_BRK_PENDING;
  e_ds_brk_words(ans->msg, sizeof(ans->msg), line);
 }
}

/**
 * e_d_dap_send_file_breakpoints - Give the adapter @file's breakpoints as
 * the user's list has them, with @extra_line too when it is not 0.
 * @path: the file's full path.
 *
 * DAP's setBreakpoints replaces every breakpoint of a source, so a line set
 * or removed while the program is stopped reaches the adapter as the whole
 * list again: the next run then stops exactly where the margin says.  Go to
 * cursor passes its own line as @extra_line.  Nothing waits for the answer:
 * it is read on the fd loop and turns the mark solid or hollow
 * (e_d_dap_on_breakpoints), and the adapter takes the run that may follow
 * only after the list.
 * Return: 0, or -1 when the adapter cannot be told.
 */
static int e_d_dap_send_file_breakpoints(const char *file, const char *path,
                                         int extra_line)
{
 int *brk_lines = MALLOC(sizeof(int) * (e_d_nbrpts + 1));
 int i, n = 0, ret;

 for (i = 0; i < e_d_nbrpts; i++)
  if (!strcmp(e_d_sbrpts[i], file))
   brk_lines[n++] = e_d_ybrpts[i];
 if (extra_line > 0 && !e_d_user_brk_on(file, extra_line))
  brk_lines[n++] = extra_line;
 ret = e_dap_set_file_breakpoints_async(g_dap, path, brk_lines, n);
 FREE(brk_lines);
 e_d_dr_watch();                      /* its answer is read on the fd loop */
 return(ret);
}

/**
 * e_d_dap_say_answer_at_stop - The adapter's word on list entry @i came at
 * a stop: say on the status row why it is not set, when it is not.
 *
 * A breakpoint set at a stop is answered on the fd loop, a moment after the
 * key; the user then reads the adapter's reason as when the answer was
 * waited for.  The start's answers and those that come while the program
 * runs are not said, as before.
 */
static void e_d_dap_say_answer_at_stop(int i)
{
 if (e_d_swtch < 3 || e_d_start.active || e_d_run_going() || g_dap_first)
  return;
 e_d_brk_say_refusal(WpeEditor->f[WpeEditor->mxedt], i);
}

/**
 * e_d_brk_same_file - Do a list entry's file name and an adapter's absolute
 * path name the same file?
 * @cn: the editor; @name: the entry's file name; @abs: the adapter's path.
 */
static int e_d_brk_same_file(ECNT *cn, const char *name, const char *abs)
{
 char path[PATH_MAX], a[PATH_MAX], b[PATH_MAX];

 e_d_brk_path_of(cn, name, path, sizeof(path));
 if (!strcmp(path, abs))
  return(1);
 if (!realpath(path, a) || !realpath(abs, b))
  return(0);
 return(!strcmp(a, b));
}

/**
 * e_d_dap_on_breakpoints - A debug adapter has said which of the breakpoints
 * it was given it set.
 * @file: the source's absolute path; @line, @verified, @message: per entry
 * of @n, the line asked for, whether the adapter set it, and its words;
 * @ud: unused.
 *
 * A verified one is drawn solid.  One the adapter has not set yet (gdb's
 * pending ones, until it says so with a later event) stays hollow, with its
 * reason kept, and at a stop the status row says why, as when it is set.
 */
static void e_d_dap_on_breakpoints(const char *file, const int *line,
                                   const int *verified,
                                   const char *const *message, int n,
                                   void *ud)
{
 ECNT *cn = WpeEditor;
 int k, i;

 for (k = 0; k < n; k++)
  for (i = 0; i < e_d_nbrpts; i++)
  {
   if (e_d_ybrpts[i] != line[k] || e_d_brk_is_start_entry(i) ||
       !e_d_brk_same_file(cn, e_d_sbrpts[i], file))
    continue;
   if (verified[k])
    e_d_brk_set_state(i, E_D_BRK_VERIFIED, NULL);
   else
    e_d_brk_set_state(i, E_D_BRK_PENDING, message[k]);
   e_d_brk_live_changed = 1;
   e_d_dap_say_answer_at_stop(i);
  }
}

/**
 * e_d_brk_line_send - Arm (@set 1) or disarm (@set 0) the breakpoint on
 * @line of @file in a debug adapter.
 * @path: the file's full path.
 *
 * A breakpoint set or removed at a stop, and Go to cursor's own breakpoint,
 * come through here, so the next run stops where the margin says.  The
 * adapter is given the user's list again, so the list must already say
 * what the user wants.  @ans, when given, is marked asked: the adapter's
 * verdict comes later, through on_breakpoints.  The line debuggers are
 * told on the fd loop (e_d_ga_brk_sync) and never come here.
 * Return: 0, or -1 when the adapter could not be told.
 */
static int e_d_brk_line_send(int set, const char *file, const char *path,
                             int line, E_D_BRK_ANSWER *ans)
{
 if (ans)
 {
  ans->state = E_D_BRK_ASKED;
  ans->msg[0] = '\0';
 }
 if (e_deb_type != DEB_DAP || !e_d_dap_active())
  return(0);
 if (set)
  return(e_d_dap_send_file_breakpoints(file, path, line));
 return(e_d_dap_send_file_breakpoints(file, path, E_D_BRK_NO_LINE));
}

/**
 * e_d_brk_path_of - The full path of a breakpoint's file, as pdb and the
 * debug adapters are given it.
 * @cn: the editor; @file: the name the breakpoint list keeps;
 * @out, @size: receives the path.
 *
 * The window that shows the file gives its directory, so a breakpoint in a
 * file opened from elsewhere is set in that file; a file no window shows is
 * named as the list has it.
 */
static void e_d_brk_path_of(ECNT *cn, const char *file, char *out, size_t size)
{
 if (!e_d_window_path_of(cn, file, out, size))
  snprintf(out, size, "%s", file);
}

/* What the reader has to say when a debugger gave no breakpoint number is
   the session's E_D_BRK_NO_NUMBER. */

/**
 * e_d_brk_numbered_verdict - Say what gdb, dbx or xdb made of a breakpoint
 * from the line that carries its number.
 * @ans: receives the verdict; @ret: 0 for a line, E_D_READ_PROMPT or
 * E_D_READ_ERROR; @str: that line, or the error line.
 *
 * A line of the error stream is the debugger's refusal ("No source file
 * named x.c."), and its words are kept for the user.
 */
static void e_d_brk_numbered_verdict(E_D_BRK_ANSWER *ans, int ret,
                                     const char *str)
{
 ans->state = E_D_BRK_VERIFIED;
 ans->msg[0] = '\0';
 if (ret == 2)
 {
  ans->state = E_D_BRK_REFUSED;
  e_ds_brk_words(ans->msg, sizeof(ans->msg), str);
 }
 else if (ret == 1)
 {
  ans->state = E_D_BRK_REFUSED;
  snprintf(ans->msg, sizeof(ans->msg), "%s", E_D_BRK_NO_NUMBER);
 }
}

/**
 * e_d_brk_answer_of - The session's view of an answer's kind @ret for
 * @str: the line, the error line with its flag, or no text at the prompt.
 */
static void e_d_brk_answer_of(E_D_ANSWER *a, int ret, const char *str)
{
 if (ret == E_D_READ_PROMPT)
  e_ds_answer_borrow(a, NULL, 0);
 else if (ret == E_D_READ_ERROR)
  e_ds_answer_borrow(a, str, 1);
 else
  e_ds_answer_borrow(a, str, 0);
}

/**
 * e_d_brk_session_answer - Keep in the session what gdb, dbx or xdb made of
 * the breakpoint on @line of @file.
 * @ret: the answer's kind, as for e_d_brk_numbered_verdict; @str: the line.
 *
 * The session then holds the number and the verdict the breakpoint list
 * shows, for the view that will draw from it.
 */
static void e_d_brk_session_answer(const char *file, int line, int ret,
                                   const char *str)
{
 E_D_ANSWER a;
 int i;

 for (i = 0; i < e_d_nbrpts; i++)
 {
  if (e_d_ybrpts[i] != line || strcmp(e_d_sbrpts[i], file))
   continue;
  e_d_brk_answer_of(&a, ret, str);
  e_ds_set_brk(&e_d_session, i, file, line);
  e_ds_take_brk_answer(&e_d_session, e_deb_type, i, &a);
  return;
 }
}

/**
 * e_d_brk_arm - Set list entry @i in the debug adapter.
 * @cn: the editor.
 *
 * Used for a breakpoint the user sets at a stop and for the ones set while
 * the program ran, so the program stops on each line the margin marks.  The
 * entry is drawn solid once the adapter confirms it.  The line debuggers
 * are told on the fd loop (e_d_ga_brk_sync).
 * Return: 0, or -1 when the adapter could not be told.
 */
static int e_d_brk_arm(ECNT *cn, int i)
{
 char path[PATH_MAX];
 E_D_BRK_ANSWER ans;

 e_d_nrbrpts[i] = i + 1;
 e_d_brk_path_of(cn, e_d_sbrpts[i], path, sizeof(path));
 if (e_d_brk_line_send(1, e_d_sbrpts[i], path, e_d_ybrpts[i], &ans) == -1)
  return(-1);
 e_d_brk_take_answer(i, &ans);
 return(0);
}

/**
 * e_d_brk_disarm - Take a breakpoint the user removed out of the debug
 * adapter.
 * @cn: the editor; @file, @line: the breakpoint, already gone from the
 * list; @id: unused: an adapter is given a file's list as a whole.
 *
 * The next run goes past that line.  The line debuggers are told on the fd
 * loop (e_d_gb_remove_cmd and its kin).
 * Return: 0, or -1 when the adapter could not be told.
 */
static int e_d_brk_disarm(ECNT *cn, const char *file, int line, int id)
{
 char path[PATH_MAX];

 (void)id;
 e_d_brk_path_of(cn, file, path, sizeof(path));
 return(e_d_brk_line_send(0, file, path, line, NULL));
}

/**
 * e_d_jdb_send_live - Send jdb a breakpoint command for @line of @file
 * while the program runs, without waiting for the answer.
 * @cn: the editor; @set: 1 for "stop at", 0 for "clear".
 *
 * jdb's answer comes in the run's output and is read there
 * (e_d_jdb_note_live_answers); one that has not come by the next stop is
 * taken back by that stop's first request (e_d_jdb_owed).
 */
static void e_d_jdb_send_live(ECNT *cn, int set, const char *file, int line)
{
 char path[PATH_MAX], cmd[E_D_BRK_CMD_MAX];

 e_d_brk_path_of(cn, file, path, sizeof(path));
 if (!e_d_brk_line_cmd(set, file, path, line, cmd, sizeof(cmd)))
  return;
 if (set)
  e_d_jdb_forget_cleared(file, line);
 else
  e_d_jdb_note_cleared(file, line);
 e_d_send_cmd(cmd);
 e_d_jdb_owe_one();
}

/**
 * e_d_jdb_sync_live - Tell jdb at once of the breakpoints the user set and
 * removed while the program runs.
 * @cn: the editor.
 *
 * jdb takes commands while the program runs: the program stops on a new
 * mark the next time it gets there, and no longer on a removed one,
 * without a pause.  Each mark turns solid when jdb's "Set breakpoint"
 * comes.
 * Return: 0.
 */
static int e_d_jdb_sync_live(ECNT *cn)
{
 int i;

 for (i = 0; i < e_d_brk_ngone; i++)
  e_d_jdb_send_live(cn, 0, e_d_brk_gone[i].file, e_d_brk_gone[i].line);
 e_d_brk_gone_free();
 for (i = 0; i < e_d_nbrpts; i++)
 {
  if (!e_d_brk_unsent(i))
   continue;
  e_d_jdb_send_live(cn, 1, e_d_sbrpts[i], e_d_ybrpts[i]);
  e_d_nrbrpts[i] = i + 1;          /* sent: the answer decides its state */
 }
 return(0);
}

/**
 * e_d_brk_gone_drop_file - Forget the removals in @file, which the debugger
 * has just been given the file's whole list for.
 * @file: the file's name as the list keeps it (not a removal's own copy).
 */
static void e_d_brk_gone_drop_file(const char *file)
{
 int i, kept = 0;

 for (i = 0; i < e_d_brk_ngone; i++)
 {
  if (strcmp(e_d_brk_gone[i].file, file))
  {
   e_d_brk_gone[kept++] = e_d_brk_gone[i];
   continue;
  }
  FREE(e_d_brk_gone[i].file);
 }
 e_d_brk_ngone = kept;
}

/**
 * e_d_brk_mark_file_sent - Every entry of @file in the list has now been
 * given to the debugger; its answer decides each one's state.
 */
static void e_d_brk_mark_file_sent(const char *file)
{
 int i;

 for (i = 0; i < e_d_nbrpts; i++)
  if (!strcmp(e_d_sbrpts[i], file) && e_d_nrbrpts[i] == 0)
   e_d_nrbrpts[i] = i + 1;
}

/**
 * e_d_dap_sync_file_live - Give the adapter @file's breakpoints, as the
 * list has them now, while the program runs.
 * @cn: the editor; @file: the name the list keeps.
 *
 * DAP replaces a source's breakpoints as a whole, so one request carries
 * the marks set and the ones removed.  The answer comes through the run's
 * reader (e_d_dap_on_breakpoints).
 * Return: 0, or -1 when the adapter cannot take it now (the change then
 * waits for the next stop).
 */
static int e_d_dap_sync_file_live(ECNT *cn, const char *file)
{
 char name[PATH_MAX], path[PATH_MAX];
 int *brk_lines = MALLOC(sizeof(int) * (e_d_nbrpts + 1));
 int i, n = 0, ret;

 snprintf(name, sizeof(name), "%s", file);
 for (i = 0; i < e_d_nbrpts; i++)
  if (!strcmp(e_d_sbrpts[i], name))
   brk_lines[n++] = e_d_ybrpts[i];
 e_d_brk_path_of(cn, name, path, sizeof(path));
 ret = e_dap_set_file_breakpoints_async(g_dap, path, brk_lines, n);
 FREE(brk_lines);
 if (ret < 0)
  return(-1);
 e_d_brk_mark_file_sent(name);
 e_d_brk_gone_drop_file(name);
 return(0);
}

/**
 * e_d_dap_sync_live - Tell the debug adapter at once of the breakpoints
 * the user set and removed while the program runs.
 * @cn: the editor.
 *
 * The adapters take setBreakpoints while the program runs: the program
 * stops on a new mark the next time it gets there, and no longer on a
 * removed one, without a pause.  A mark turns solid on the adapter's
 * "verified", or on a later breakpoint event.
 * Return: 0, or -1 when the adapter could not take it (the rest waits for
 * the next stop).
 */
static int e_d_dap_sync_live(ECNT *cn)
{
 int i;

 if (!e_d_dap_active())
  return(-1);
 while (e_d_brk_ngone > 0)
  if (e_d_dap_sync_file_live(cn, e_d_brk_gone[0].file) < 0)
   return(-1);
 for (i = 0; i < e_d_nbrpts; i++)
  if (e_d_brk_unsent(i) && e_d_dap_sync_file_live(cn, e_d_sbrpts[i]) < 0)
   return(-1);
 return(0);
}

/**
 * e_d_dap_clear_every_file - Give the adapter an empty breakpoint list for
 * each file the user's list names.
 * @cn: the editor.
 *
 * Remove all breakpoints under a debug adapter: the program then stops on
 * none of them on its way to the end.  The entry the editor set for itself
 * names no file, so there is no source to clear and the adapter is not asked
 * about one.
 */
static void e_d_dap_clear_every_file(ECNT *cn)
{
 char path[PATH_MAX];
 int i;

 if (!e_d_dap_active())
  return;
 for (i = 0; i < e_d_nbrpts; i++)
  if (!e_d_brk_is_start_entry(i))
  {
   e_d_brk_path_of(cn, e_d_sbrpts[i], path, sizeof(path));
   e_dap_set_file_breakpoints_async(g_dap, path, NULL, 0);
  }
 e_d_dr_watch();
}

/**
 * e_d_brk_disarm_all - Take every breakpoint of the list out of the running
 * debugger, before the list is emptied.
 * @cn: the editor.
 *
 * After Remove all breakpoints the next run goes to the end of the program
 * whichever debugger runs it.  The line debuggers are told at a stop on the
 * fd loop, in the way each takes it (e_d_ga_remove_all); Remove all is
 * refused while they run or answer.  A debug adapter is given an empty
 * list for each file.
 */
static void e_d_brk_disarm_all(ECNT *cn)
{
 if (e_d_ga_at_stop())
  e_d_ga_remove_all(cn);
 else if (e_deb_type == DEB_DAP)
  e_d_dap_clear_every_file(cn);
}

/**
 * e_d_cursor_brk_send - Set or clear the cursor's breakpoint in the debug
 * adapter.
 * @set: 1 to set it, 0 to clear it.
 *
 * Go to cursor under an adapter stops on the cursor's line and nowhere
 * else afterwards.  The line debuggers set and clear theirs on the request
 * engine (e_d_ga_run_to_cursor), and gdb runs there with "advance".
 * Return: 0, or -1 when the adapter could not be told.
 */
static int e_d_cursor_brk_send(int set)
{
 return(e_d_brk_line_send(set, e_d_cursor_brk.file, e_d_cursor_brk.path,
                          e_d_cursor_brk.line, NULL));
}

/**
 * e_d_cursor_brk_clear - Remove Go to cursor's breakpoint at the stop.
 *
 * Whichever stop comes first (the cursor's line, a breakpoint of the
 * user's on the way, the end of a function), the line must not stop the
 * program again on a later run.  A session that ended took it along.
 * While the program still runs under the run controller the debugger reads
 * no command, so the controller clears it at the stop instead.
 */
static void e_d_cursor_brk_clear(void)
{
 if (!e_d_run_going())
  e_d_cursor_brk.wanted = 0;
 if (!e_d_cursor_brk.set || e_d_run_going())
  return;
 e_d_cursor_brk.set = 0;
 if (e_d_swtch < 3)
  return;
 if (e_deb_type == DEB_DAP && !e_d_dap_active())
  return;
 e_d_cursor_brk_send(0);
}

/* The file Go to cursor was pressed in, taken with its line at the key: the
   debugger starts meanwhile, and the window on top may be another by then. */
static struct {
 char file[PATH_MAX];          /* the window's file name                   */
 char dirct[PATH_MAX];         /* the window's directory                   */
} e_d_goto_target;

/**
 * e_d_goto_take_target - Remember the file of the window Go to cursor was
 * pressed in.
 * @f: that window.
 */
static void e_d_goto_take_target(FENSTER *f)
{
 e_d_goto_target.file[0] = '\0';
 e_d_goto_target.dirct[0] = '\0';
 if (f->datnam)
  snprintf(e_d_goto_target.file, sizeof(e_d_goto_target.file), "%s",
           f->datnam);
 if (f->dirct)
  snprintf(e_d_goto_target.dirct, sizeof(e_d_goto_target.dirct), "%s",
           f->dirct);
}

/**
 * e_d_cursor_brk_aim - Point Go to cursor's own breakpoint at @line of the
 * file Go to cursor was pressed in, with the full path the debugger is
 * given.
 * @line: the cursor's 1-based line.
 */
static void e_d_cursor_brk_aim(int line)
{
 char *full;

 snprintf(e_d_cursor_brk.file, sizeof(e_d_cursor_brk.file), "%s",
          e_d_goto_target.file);
 full = e_mkfilename(e_d_goto_target.dirct, e_d_goto_target.file);
 snprintf(e_d_cursor_brk.path, sizeof(e_d_cursor_brk.path), "%s",
          full ? full : e_d_goto_target.file);
 if (full)
  FREE(full);
 e_d_cursor_brk.line = line;
 e_d_cursor_brk.id = 0;
}

/**
 * e_d_ga_run_to_cursor - Go to cursor under pdb or jdb: its own breakpoint
 * on the line, set on the request engine just before the Continue goes
 * out, and cleared by the stop's own requests.
 * @f: the source window; @line: the cursor's 1-based line.
 *
 * The user keeps the editor while the debugger sets it, as for any other
 * resume.
 * Return: what the Continue returned.
 */
static int e_d_ga_run_to_cursor(FENSTER *f, int line)
{
 if (!e_d_user_brk_on(e_d_goto_target.file, line))
 {
  e_d_cursor_brk_aim(line);
  e_d_cursor_brk.wanted = 1;
 }
 return(e_deb_run(f));
}

/**
 * e_d_run_to_cursor_line - Go to cursor for a debugger without "advance":
 * a breakpoint of its own on the line, continue, and the breakpoint gone
 * again at the next stop.
 * @f: the source window; @line: the cursor's 1-based line, in the file
 * e_d_goto_take_target took.
 *
 * Borland's Go to cursor, on every debug adapter, and on pdb, jdb and a68g
 * through the request engine (e_d_ga_run_to_cursor).  pdb's "until"
 * would stop when the current function returns and never reach a line in
 * another one, which is why pdb takes this way too.
 * Return: what the continue returned.
 */
static int e_d_run_to_cursor_line(FENSTER *f, int line)
{
 int ret;

 if (e_d_user_brk_on(e_d_goto_target.file, line))
  return(e_deb_run(f));
 e_d_cursor_brk_aim(line);
 if (e_d_cursor_brk_send(1) < 0)
  return(-1);
 e_d_cursor_brk.set = 1;
 ret = e_deb_run(f);
 e_d_cursor_brk_clear();
 return(ret);
}

/**
 * e_d_gdb_run_command - Send gdb a command that lets the program run to a
 * stop, and give the editor its keys back until it comes.
 * @f: the source window; @cmd: an "advance" command or "finish\n".
 *
 * Go to cursor and Finish function under gdb.  The user can use the menus
 * and quit the session while the program runs, as after Run or a step.
 * Return: 0, or the end of the session.
 */
static int e_d_gdb_run_command(FENSTER *f, const char *cmd)
{
 e_d_delbreak(f);
 e_d_switch_out(1);
 e_d_nstack = 0;
 return(e_d_ga_go_step(f, cmd, NULL));
}

/**
 * e_d_gdb_run_to_line - Run gdb's program on to a line of the source,
 * wherever it is: in the function that runs now, in one it calls, or in
 * the caller if this function returns first.
 * @f: the source window; @line: the 1-based line.
 *
 * "advance" and not "until": until stops only in the current frame or
 * an outer one, so a cursor inside a called function was run past and the
 * program ran to its end.  The line goes with the file Go to cursor was
 * pressed in, named as the breakpoints name it: a bare line number is
 * read in the file the program is stopped in, so a cursor in a second
 * source file ran to the same line number of the first one.
 * Return: 0, or the end of the session.
 */
static int e_d_gdb_run_to_line(FENSTER *f, int line)
{
 char str[E_D_LINE_MAX + PATH_MAX];

 if (e_d_goto_target.file[0])
  snprintf(str, sizeof(str), "advance \"%s\":%d\n",
           e_d_goto_target.file, line);
 else
  snprintf(str, sizeof(str), "advance %d\n", line);
 return(e_d_gdb_run_command(f, str));
}

/**
 * e_d_gdb_finish - Run gdb's program until the current function returns.
 * @f: the source window.
 * Return: 0, or the end of the session.
 */
static int e_d_gdb_finish(FENSTER *f)
{
 return(e_d_gdb_run_command(f, "finish\n"));
}

/**
 * e_d_run_goto_after_start_line - Go to cursor started the session: run on
 * to the cursor's @line once the start has stopped.
 *
 * Kept by the run when it is under way, or by gdb's run that still waits
 * for gdb's answers before it goes out.
 */
static void e_d_run_goto_after_start_line(int line)
{
 if (e_d_ga_go_keeps_goto_line(line))
  return;
 if (e_d_run_going() && e_d_run.ops->run_to_line)
  e_d_run.goto_line = line;
}

/**
 * e_d_goto_first_command - Go to cursor or Finish function pressed before
 * any session: start it and run to the start of main.
 * @f: the source window; @flag: E_D_GOTO_CURSOR or E_D_GOTO_FINISH;
 * @line: the cursor's 1-based line.
 *
 * Under gdb, Go to cursor then goes on to the cursor's line when the
 * start has stopped (see e_d_run_goto_after_start).
 * Return: what the run returned.
 */
static int e_d_goto_first_command(FENSTER *f, int flag, int line)
{
 int main_brk, ret;

 if ((main_brk = e_mk_brk_main(f, 0)) < -1)
  return(main_brk);
 ret = e_deb_run(f);
 if (flag == E_D_GOTO_CURSOR)
  e_d_run_goto_after_start_line(line);
 e_mk_brk_main(f, main_brk);
 return(ret);
}

/**
 * e_d_goto_run - Go to cursor or Finish function, wherever the session
 * stands: start it, run to the start of main, or let the program run on.
 * @f: the source window; @flag: E_D_GOTO_CURSOR or E_D_GOTO_FINISH;
 * @line: the cursor's 1-based line when the key was pressed.
 *
 * The line is taken when the key is pressed: the debugger starts and the
 * program is built meanwhile, and the command runs again when they are
 * done, with the cursor moved.
 * Return: what the command returned.
 */
static int e_d_goto_run(FENSTER *f, int flag, int line)
{
 ECNT *cn = f->ed;
 int ret = 0;

 if (!e_d_goto_supported(flag))
  return 0;
 if (e_d_refuse_while_running(f))
  return(0);
 if (flag == E_D_GOTO_CURSOR)
  e_d_resume_after_build(e_d_goto_cursor_line, line);
 else
  e_d_resume_after_build(e_d_goto_func, flag);
 if (e_d_swtch < 2)
  ret = e_run_debug(f);
 if (e_d_deferred(ret))
  return(0);
 if (ret < 0)
 {
  e_d_quit(f);
  if (ret == -1)
  {  e_show_error(0, f);  return(ret);  }
  return(e_error(e_d_msg[ERR_CANTDEBUG], 0, f->fb));
 }
 if (e_d_swtch < 3)
  return(e_d_goto_first_command(f, flag, line));
 if (flag == E_D_GOTO_CURSOR && e_d_ga_sets_cursor_brk())
  return(e_d_ga_run_to_cursor(cn->f[cn->mxedt], line));
 if (flag == E_D_GOTO_FINISH && e_d_ga_finishes_on_loop())
  return(e_d_ga_finish_ask(cn->f[cn->mxedt]));
 if (flag == E_D_GOTO_CURSOR && e_deb_type != DEB_GDB)
  return(e_d_run_to_cursor_line(cn->f[cn->mxedt], line));
 if (flag == E_D_GOTO_FINISH && e_deb_type != DEB_GDB)
  return(e_d_finish_to_caller(cn->f[cn->mxedt]));
 if (flag == E_D_GOTO_CURSOR)
  return(e_d_gdb_run_to_line(f, line));
 return(e_d_gdb_finish(f));
}

/**
 * e_d_goto_cursor_line - Go to cursor, to a line given.
 * @f: the source window; @line: the 1-based line.
 *
 * What the command runs again as, once the build and the debugger's start
 * are done.
 */
static int e_d_goto_cursor_line(FENSTER *f, int line)
{
 return(e_d_goto_run(f, E_D_GOTO_CURSOR, line));
}

/**
 * e_d_goto_func - Go to cursor (Ctrl-G U, F4) or Finish function (Ctrl-G F).
 * @f: the window the key was pressed in; @flag: E_D_GOTO_CURSOR or
 * E_D_GOTO_FINISH.
 *
 * The program runs to the cursor's line, wherever the line is, or until
 * the function returns, and the stop is marked.  Pressed before any
 * session, the debugger starts first.  While the program runs the editor
 * keeps its keys, and Debug > Quit ends the session.
 * Return: 0, or the end of the session.
 */
int e_d_goto_func(FENSTER *f, int flag)
{
 ECNT *cn = f->ed;
 FENSTER *top = cn->f[cn->mxedt];

 if (flag == E_D_GOTO_CURSOR &&
     e_d_act_deferred(E_D_ACT_GOTO, top, top->b->b.y + 1))
  return(0);
 if (flag == E_D_GOTO_FINISH && e_d_act_deferred(E_D_ACT_FINISH, top, 0))
  return(0);
 if (flag == E_D_GOTO_CURSOR)
  e_d_goto_take_target(top);
 return(e_d_goto_run(f, flag, top->b->b.y + 1));
}

int e_d_goto_cursor(FENSTER *f)
{
 return e_d_goto_func(f, E_D_GOTO_CURSOR);
}

int e_d_finish_func(FENSTER *f)
{
 return e_d_goto_func(f, E_D_GOTO_FINISH);
}

/**
 * e_d_stop_file_line - Read the file and line out of a stop line and make
 * that file the one the stop is shown in.
 * @line: a line that names them ("... at file:line" for gdb).
 *
 * The file goes through a buffer of its own, never a line of the answer
 * still to be parsed, and a file name longer than any path is refused.
 * Return: the line number, or -1 when the line names none.
 */
static int e_d_stop_file_line(char *line)
{
 char file[E_D_LINE_MAX];
 int num = e_make_line_num(line, file);

 if (num >= 0)
  strcpy(e_d_file, file);
 return(num);
}

static int e_d_where_frame;   /* number of the last "#N" row of the walk */

/**
 * e_d_where_row_line - The source line a row of the stack listing names,
 * for the frame to mark.
 * @row: one row of "where"; @file: E_D_LINE_MAX bytes for the file's name.
 *
 * Normally the first row that names a line is the stop.  After Pause the
 * program stands wherever it happened to be, often in library code that
 * has debug information but no source here (a sleep call, a lock): a row
 * whose file cannot be read is passed over, so the frame marked is the
 * innermost one in the user's code, as the Stack window would show it.
 * Return: the line, or -1 when the row names none to mark.
 */
static int e_d_where_row_line(char *row, char *file)
{
 int line = e_make_line_num(row, file);
 int frame = e_ds_where_row_frame(row);

 e_ds_take_where_row(&e_d_session, e_deb_type, row);
 if (frame != E_DS_NOT_FOUND)
  e_d_where_frame = frame;               /* the frame this row belongs to */
 if (line < 0 || e_d_run.pause == E_D_PAUSE_NONE)
  return(line);
 if (access(file, R_OK) != 0)
  return(-1);
 return(line);
}

/**
 * e_make_line_num - The line number and, into @file (E_D_LINE_MAX bytes),
 * the file a debugger line names, as e_ds_line_num reads them.
 * @str: the line; @file: receives the file.
 *
 * Following a gdb, dbx or xdb frame in the Stack window, or a gdb stop,
 * marks the right line in the right file.  sdb names only the line; its
 * file is asked on the fd loop by the stop's own step
 * (e_d_gsdb_which_file_cmd), so it never comes here.
 * Return: the line, or -1.
 */
int e_make_line_num(char *str, char *file)
{
 return(e_ds_line_num(e_deb_type, str, file, E_D_LINE_MAX));
}

/**
 * e_make_line_num2 - The line and, into @file (E_D_LINE_MAX bytes), the
 * file of an sdb or xdb "t" row: see e_ds_line_num2.
 */
int e_make_line_num2(char *str, char *file)
{
 return(e_ds_line_num2(str, file, E_D_LINE_MAX));
}

/**
 * e_d_show_stop_position - Repaint the line:column field of the window a
 * stop was marked in.
 * @f: the source window that now carries the current-line mark.
 *
 * A stop can arrive after the editor has painted its status for the key
 * that started the step, so without this the field kept the previous stop's
 * line until the next key.  The user reads the same line in the field as
 * under the green bar after every stop, whatever the backend.
 */
static void e_d_show_stop_position(FENSTER *f)
{
 e_zlsplt(f);
 e_refresh();
}

int e_d_goto_break(char *file, int line, FENSTER *f)
{
 ECNT *cn = f->ed;
 BUFFER *b;
 SCHIRM *s;
 FENSTER ftmp;
 int i;
 char str[PATH_MAX + 64];

/*   if(schirm != e_d_save_schirm) e_d_switch_out(0);  */
 e_d_switch_out(0);
 ftmp.ed = cn;
 ftmp.fb = f->fb;
 WpeFilenameToPathFile(file, &ftmp.dirct, &ftmp.datnam);
 e_d_out_clear();
 for (i = cn->mxedt; i > 0; i--)
  if (!strcmp(cn->f[i]->datnam, ftmp.datnam) &&
    !strcmp(cn->f[i]->dirct, ftmp.dirct))
  {
   /*  for(j = 0; j <= cn->mxedt; j++)
	 if(!strcmp(cn->f[j]->datnam, "Stack"))
	 {  if(cn->f[i]->e.x > 2*MAXSCOL/3-1) cn->f[i]->e.x = 2*MAXSCOL/3-1;
	 break;
	 }  */
   e_switch_window(cn->edt[i], cn->f[cn->mxedt]);
   break;
  }
 f = cn->f[cn->mxedt];
 FREE(ftmp.dirct);
 FREE(ftmp.datnam);
 if (i <= 0)
 {
  if (access(file, 0))
  {
   /* Source file not found -- stepped into system/library code.
      Set a temporary breakpoint at the next user function (main)
      and continue, skipping all libc frames. */
   if (e_deb_type == DEB_GDB)
    return(e_d_ga_leave_unknown_source(cn));
   snprintf(str, sizeof(str), e_d_msg[ERR_CANTFILE], file);
   return(e_error(str, 0, f->fb));
  }
  if (e_edit(cn, file))
   return(WPE_ESC);
  b = cn->f[cn->mxedt]->b;
  s = cn->f[cn->mxedt]->s;
 }
 f = cn->f[cn->mxedt];
 b = cn->f[cn->mxedt]->b;
 s = cn->f[cn->mxedt]->s;
 s->da.y = b->b.y = line - 1;
 s->da.x = b->b.x = 0;
 s->de.x = MAXSCOL;
 e_schirm(f, 1);
 e_cursor(f, 1);
 e_d_show_stop_position(f);
 e_d_pty_flush_to_messages(f);
#ifndef NO_XWINDOWS
 if (WpeIsXwin())
  e_x_reclaim_focus();
#endif
 return(0);
}

/* ==== the line that says a gdb run began ====

   gdb answers "r" with "Starting program: <path> <args>" and "c" with
   "Continuing."; both come before anything the program does, on the
   output the run reads.  The run's own reader takes them as they come, so
   no command waits for them in front of the editor. */

/* How many characters of "Starting program:" are compared. */
#define E_D_GDB_START_LEAD 17

/* The line the run that just went out still owes. */
static struct
{
 int start;                  /* "Starting program: ...", after "r" */
 int cont;                   /* "Continuing.", after "c" */
} e_d_gdb_awaits;

/**
 * e_d_gdb_awaits_forget - Expect no such line: the run stopped, or the
 * session ended.
 */
static void e_d_gdb_awaits_forget(void)
{
 e_d_gdb_awaits.start = 0;
 e_d_gdb_awaits.cont = 0;
}

/**
 * e_d_gdb_line_ends_with - Does @line end with @tail?
 */
static int e_d_gdb_line_ends_with(const char *line, const char *tail)
{
 size_t len = strlen(line), n = strlen(tail);

 return(len >= n && !strcmp(line + len - n, tail));
}

/**
 * e_d_gdb_run_line_own - The run's reader found @line: is it the one that
 * says the run began?
 *
 * The user reads "Starting program: ..." in Messages as soon as gdb says
 * it, as before; neither line is taken for part of the stop.
 * Return: 1 when the line was taken here.
 */
static int e_d_gdb_run_line_own(const char *line)
{
 ECNT *cn = e_d_run.cn ? e_d_run.cn : WpeEditor;

 if (e_d_gdb_awaits.start &&
     !strncmp(e_d_msg[ERR_STARTPROG], line, E_D_GDB_START_LEAD))
 {
  e_d_gdb_awaits.start = 0;
  e_d_p_message((char *)line, cn->f[cn->mxedt], 0);
  return(1);
 }
 if (e_d_gdb_awaits.cont && e_d_gdb_line_ends_with(line, e_d_msg[ERR_CONTINUE]))
 {
  e_d_gdb_awaits.cont = 0;
  return(1);
 }
 return(0);
}

/**
 * e_d_gdb_run_never_started - gdb came back to its prompt after "r" without
 * saying "Starting program": the program could not be started.
 * @f: the window on top.
 *
 * The user is told so, and the session ends, instead of being shown a stop
 * of a program that never ran.
 * Return: 1 when the session ended.
 */
static int e_d_gdb_run_never_started(FENSTER *f)
{
 int never = e_d_gdb_awaits.start;

 e_d_gdb_awaits_forget();
 if (!never)
  return(0);
 e_d_quit(f);
 e_error(e_d_msg[ERR_CANTPROG], 0, f->fb);
 return(1);
}

/* ==== gdb's answers at a stop, on the fd loop ====

   Everything gdb answers once the program has stopped (the stop's frame,
   the watches, the stack, breakpoint answers) is asked through the request
   engine of we_debreq.c and read by the editor's fd loop, between the
   user's keys.  Nothing here reads the keyboard or waits on gdb's pipe: a
   key typed while gdb answers reaches the editor, a key that needs gdb is
   refused with the waiting line or, for a step, kept and run in order once
   gdb is done, and Esc, Ctrl-C or the deadline give up on an answer that
   does not come.  Each sequence of commands is a table of steps (E_D_STEP),
   one named function per command and per answer.

   pdb, jdb and a68g run on the same engine (their own steps are further
   down, "pdb's answers, start and resumes", "jdb's answers, start and
   resumes" and "a68g's answers, start and resumes").  What differs between
   them is in their backend tables (E_D_GA_BACKEND): the words and prompt,
   how a breakpoint is set and taken out, the events jdb writes in the
   middle of its answers, and the tables of steps a stop, the Stack window
   and a start ask; everything else here is one code for all four. */

/* What runs on the request engine. */
#define E_D_GA_NONE   0   /* nothing */
#define E_D_GA_STOP   1   /* the answers of a stop the user is shown */
#define E_D_GA_SYNC   2   /* a stop made to tell gdb of breakpoint changes:
                             for the user the program runs */
#define E_D_GA_ASK    3   /* a command of the user's at a stop */
#define E_D_GA_START  4   /* the settings and breakpoints after the banner */
#define E_D_GA_RESUME 5   /* what a run, a step or a Continue asks first */

#define E_D_GDB_NAME        "gdb"   /* as the waiting line names it */
#define E_D_GA_READ_MAX     4096    /* bytes read from gdb per wake */
#define E_D_GA_LINE_MAX     (E_D_REQ_CMD_MAX + 128)   /* a status line */
#define E_D_GA_NO_TIMER     (-1)
#define E_D_GA_WAIT_FMT \
 " waiting for %s to answer `%s` ... %lds  (Esc cancels)"
#define E_D_GDB_PROMPT      "(gdb) "
#define E_D_GDB_SETTLE_ECHO "echo " E_D_GDB_SETTLE_MARK "\\n"
#define E_D_GDB_FRAME_SELECT_FMT "frame %d"
#define E_D_GDB_FLUSH       "call (void)fflush(0)"
#define E_D_GDB_WHERE       "where"
#define E_D_GDB_BT          "bt"
#define E_D_GDB_BREAK_FMT   "b %s:%d"
#define E_D_GDB_DELETE_FMT  "d %d"
#define E_D_GDB_CLEAR_FMT   "clear %s:%d"   /* every breakpoint on a line */
#define E_D_GDB_TBREAK_MAIN "tbreak main"
#define E_D_GDB_CONTINUE    "c\n"
#define E_D_GDB_UP_FMT      "up %d"
#define E_D_GDB_DOWN_FMT    "down %d"
#define E_D_GA_ACT_MAX      8      /* debugger keys kept while gdb answers */
#define E_D_GA_MARKER_MAX   E_D_REQ_MARKER_MAX   /* a marker with its number */
#define E_D_GA_MARKER_FMT   "%s-%d"   /* the marker, numbered */
/* How many characters of gdb's report words are compared, as the stop
   checks have always compared them. */
#define E_D_GDB_EXIT_LEAD     14
#define E_D_GDB_TERM_LEAD     18
#define E_D_GDB_SIGNAL_LEAD   23
#define E_D_GDB_BREAK_LEAD    10
#define E_D_GDB_NOSTACK_LEAD  9

/* A debugger key kept while gdb answers: its kind, and for Go to cursor
   the file and line it was pressed on. */
typedef struct
{
 int kind;
 int line;
 char file[PATH_MAX];
 char dirct[PATH_MAX];
} E_D_ACT;

typedef struct e_d_ga_ctx E_D_GA_CTX;     /* what a chain's steps learn */
typedef struct e_d_ga_go E_D_GA_GO;       /* what a start or resume does */

/* What the engine is told of the debugger it talks to: everything in which
   gdb, pdb, jdb and a68g differ.  The waiting line, the keys kept and refused,
   the cancel and every step they share then stay one code. */
struct e_d_ga_backend
{
 const char *name;              /* as the waiting line names it */
 const char *prompt;            /* what ends each of its answers */
 const char *settle_cmd;        /* makes it write settle_marker after the
                                   answers it still owes (a cancelled
                                   command's, an interrupt's) */
 const char *settle_marker;
 int reads_errors;              /* it refuses on its error stream (gdb);
                                   pdb refuses on its output, and its error
                                   stream is the program's own */
 int says_run_lines;            /* a run is answered with a line the run's
                                   reader takes ("Starting program") */
 /* The command that sets list entry @i, and the one that takes out a
    breakpoint the list no longer has. */
 void (*brk_add_cmd)(int i, char *cmd, size_t n);
 void (*brk_remove_cmd)(const E_D_BRK_GONE *g, char *cmd, size_t n);
 /* Its answer to a breakpoint set: the number it gave it, 0 when it
    refused; then what the list entry keeps of that answer. */
 int  (*brk_set_id)(const E_D_ANSWER *ans);
 void (*brk_verdict)(int i, int id, const E_D_ANSWER *ans);
 void (*remove_all)(ECNT *cn);          /* Remove all at a stop */
 int  (*show_stack)(E_D_GA_CTX *st);   /* the Stack window from its answer */
 void (*pause_instead)(ECNT *cn);       /* Pause during a sync: show it */
 int  (*finish_now)(E_D_GA_GO *go);     /* Finish once its frames are read */
 /* Does the answer so far end with the debugger's prompt?  @prompt_len
    receives the bytes of prompt that end it. */
 int  (*ends_in_prompt)(const E_D_ANSWER *ans, size_t *prompt_len);
 /* The command that sets the editor's start breakpoint (no line end), for
    a debugger told of it before the first run; NULL for none. */
 void (*start_brk_cmd)(char *cmd, size_t n);
 /* The command that sets (@set 1) or clears Go to cursor's own breakpoint,
    for a debugger without "advance"; NULL for gdb, which has it. */
 void (*cursor_brk_cmd)(int set, char *cmd, size_t n);
 /* A debugger that reports events in the middle of answers (jdb): which
    lines are no answer's, what they say, what came when nothing was
    asked, whether it is still writing an event with nothing asked (its
    output is then read on), and a sequence of answers over, after which
    an event kept meanwhile is shown.  NULL for gdb and pdb, which report
    none. */
 int  (*line_is_async)(const char *line, void *io);
 void (*deliver_async)(const char *line, void *io);
 void (*unsolicited)(const char *bytes, size_t n);
 int  (*reads_on)(void);
 void (*chain_over)(void);
 /* The answer to @cmd was given up on and the debugger still owes it.
    NULL for a debugger that answers in the order it is asked, whose late
    answer the settle marker's resync drops (gdb, pdb). */
 void (*given_up)(const char *cmd);
 /* Copy @n bytes the debugger wrote from @in to @out (room for 2 * @n),
    giving each prompt that more text follows on its line a line of its
    own, so the answer it ends is seen to end there (jdb writes its prompt
    with no line end, and an event or a late answer then goes on its line).
    Return: the bytes in @out.  NULL for gdb and pdb. */
 size_t (*separate_prompts)(const char *in, size_t n, char *out);
 const E_D_STEP *set_up_steps;          /* after the banner */
 const E_D_STEP *brk_steps;             /* a breakpoint toggled at a stop */
 const E_D_STEP *watches_steps;         /* a watch added, edited, deleted */
 const E_D_STEP *stack_steps;           /* Ctrl-G K */
 const E_D_STEP *follow_steps;          /* Enter on a row of the Stack */
 const E_D_STEP *go_steps;              /* what a resume asks first */
 /* A run or a Continue has just gone out: note what the debugger answers
    it with before the program runs, for the run's reader to take (dbx's
    and xdb's "Running:"), or show what the user is told of it (sdb's
    command).  NULL: says_run_lines decides (gdb, pdb, jdb, a68g). */
 void (*run_went)(const E_D_GA_GO *go);
 /* For a debugger whose interrupt may take away the command it has just
    read (pdb, in its own process: the signal is taken on the way back
    from the read, so the command never runs): the words its answer to an
    interrupt holds, and the settle command that makes it echo @marker.
    The settle step counts interrupts by those words and, after an answer
    that is one and not the marker, asks again with a new marker.  Since
    the signal is taken by the debugger's own process, none is owed once
    the last marker has come back.  NULL for a debugger that keeps a
    command an interrupt meets (gdb answers it after its "Quit", measured
    on gdb 18.1) or is not interrupted at a stop. */
 const char *int_answer;
 void (*settle_cmd_for)(const char *marker, char *cmd, size_t n);
 /* For a debugger that numbers its commands (gdb over MI): how a command
    of the engine's goes out, so the number of the one in flight is known.
    NULL sends it as it is (e_d_ga_send). */
 int  (*send)(const char *cmd, void *io);
 /* Does @ans refuse what was asked?  Then @words receives the debugger's
    reason.  NULL: the error flag says so and the first line is the reason,
    as the line debuggers refuse. */
 int  (*refused_words)(const E_D_ANSWER *ans, char *words, size_t n);
};

/* The request engine, its world and what runs on it. */
static struct
{
 E_D_REQ_QUEUE rq;
 E_D_REQ_IO io;
 E_D_REQ_LINK link;
 E_D_REQUESTER via;
 const E_D_GA_BACKEND *be;   /* the debugger this session's engine talks to */
 int made;                   /* rq was initialised once: free before again */
 int live;                   /* this session's engine is ready */
 int timer[E_D_TIMERS];      /* the loop's timer ids, E_D_GA_NO_TIMER */
 int answer_watched;         /* the fd loop reads gdb's output for it */
 int errors_watched;         /* ... and gdb's error stream */
 int chain;                  /* E_D_GA_*: what runs */
 int marker_wanted;          /* the request out is the settle echo */
 char marker[E_D_GA_MARKER_MAX];   /* the marker that request waits for */
 int settle_seq;             /* settle commands sent in this settle */
 int settle_again;           /* an interrupt took the last one: ask again */
 int owed;                   /* interrupts the settle step takes back */
 int pause_asked;            /* Pause pressed during a sync */
 int running_said;           /* a key was refused as while the program runs:
                                the waiting line leaves that line alone */
 int waiting_painted;        /* the waiting line is on the status row */
 int set_up_owed;            /* the start's set-up was given up on: the next
                                debugger command sets gdb up again */
 ECNT *cn;
 E_D_ACT act[E_D_GA_ACT_MAX];
 int nact;
 int act_timer;
 e_d_wait_line kept;         /* a waiting line asked for under a box */
} e_d_ga;

/**
 * e_d_ga_this - The backend table of the debugger that runs now, for a
 * path that only runs while that debugger is one of the engine's (a
 * command at its stop, its start, a resume).
 */
static const E_D_GA_BACKEND *e_d_ga_this(void)
{
 return(e_d_ga_backend_of(e_deb_type));
}

/* What the stall line calls a debugger the engine has no name for. */
#define E_D_INPUT_WHAT "the debugger"

/**
 * e_d_input_name - The debugger's name as the status row says it ("gdb"),
 * when it stops reading its input.
 */
static const char *e_d_input_name(void)
{
 const E_D_GA_BACKEND *be = e_d_ga_this();

 if (be && be->name)
  return(be->name);
 return(E_D_INPUT_WHAT);
}

/**
 * e_d_ga_editor - The editor the engine's session belongs to, so a step
 * that has no context of its own opens a frame's file in the right place.
 */
static ECNT *e_d_ga_editor(void)
{
 if (e_d_ga.cn)
  return(e_d_ga.cn);
 return(WpeEditor);
}

/**
 * e_d_ga_window - The window on top, where status lines are said.
 */
static FENSTER *e_d_ga_window(void)
{
 ECNT *cn = e_d_ga.cn ? e_d_ga.cn : WpeEditor;

 return(cn->f[cn->mxedt]);
}

/* ---- the engine's world ---- */

/**
 * e_d_ga_send - Write one command to gdb, with its line end.
 * @cmd: the command; @io: unused.
 * Return: 0 when it is on its way (written, or queued behind what gdb has
 * not read yet), -1 when gdb's input is closed or would not take it: the
 * request then ends at once instead of waiting for an answer to a command
 * gdb never got.
 */
static int e_d_ga_send(const char *cmd, void *io)
{
 char line[E_D_REQ_CMD_MAX + 2];

 (void)io;
 if (rfildes[1] < 0)
  return(-1);
 snprintf(line, sizeof line, "%s\n", cmd);
 e_d_nb_forget();
 if (!e_d_send_went(e_d_send_cmd(line)))
  return(-1);
 return(0);
}

/**
 * e_d_ga_ends_in_prompt - Does the debugger's answer end with its prompt?
 * @ans: the answer so far; @prompt_len: set to the prompt's bytes when it
 * does.
 */
static int e_d_ga_ends_in_prompt(const E_D_ANSWER *ans, size_t *prompt_len)
{
 const char *prompt = e_d_ga.be->prompt;
 size_t n = strlen(prompt);

 if (!ans->text || ans->len < n)
  return(0);
 if (memcmp(ans->text + ans->len - n, prompt, n))
  return(0);
 *prompt_len = n;
 return(1);
}

/**
 * e_d_ga_marker_in - Where the marker of the settle command out stands in
 * @text, or NULL.  A numbered marker is not taken for one whose number
 * only starts with its own, so an earlier settle command's echo never
 * ends a later one's answer.
 */
static const char *e_d_ga_marker_in(const char *text)
{
 return(e_d_req_marker_in(text, e_d_ga.marker));
}

/**
 * e_d_ga_marker_missing - Is the settle command's answer still without its
 * marker?  The prompts before the marker answer interrupts, not it.
 * @ans: the answer so far.
 */
static int e_d_ga_marker_missing(const E_D_ANSWER *ans)
{
 if (!e_d_ga.marker_wanted)
  return(0);
 return(e_d_ga_marker_in(ans->text) == NULL);
}

/**
 * e_d_ga_ends_on_interrupt_answer - Is the settle command's answer, still
 * without its marker, whole with the debugger's answer to an interrupt?
 * @ans, @prompt_len: as E_D_REQ_IO's answer_ended.
 *
 * For a debugger whose interrupt may take away the command it has just
 * read (the backend's int_answer), that answer and its prompt may be all
 * that comes for the settle command: the step then asks again rather than
 * wait for an echo that will never come, and the user is not left with a
 * waiting line until Debug > Quit.
 */
static int e_d_ga_ends_on_interrupt_answer(const E_D_ANSWER *ans,
                                           size_t *prompt_len)
{
 const char *words = e_d_ga.be->int_answer;

 if (!words || !ans->text || !strstr(ans->text, words))
  return(0);
 return(e_d_ga.be->ends_in_prompt(ans, prompt_len));
}

/**
 * e_d_ga_answer_ended - Is the debugger's answer whole?
 * @ans, @prompt_len, @io: as E_D_REQ_IO's answer_ended.
 *
 * An answer ends with the debugger's prompt.  The settle command's answer
 * ends only with the prompt after the marker: the prompts before it answer
 * interrupts.  For a debugger whose interrupt may take the settle command
 * away, it also ends with the prompt after an answer to an interrupt, and
 * the settle step asks again.  The marker is looked for first, so the
 * backend's prompt test is asked only for an answer that is whole once its
 * prompt is there.
 */
static int e_d_ga_answer_ended(const E_D_ANSWER *ans, size_t *prompt_len,
                               void *io)
{
 (void)io;
 if (e_d_ga_marker_missing(ans))
  return(e_d_ga_ends_on_interrupt_answer(ans, prompt_len));
 return(e_d_ga.be->ends_in_prompt(ans, prompt_len));
}

/**
 * e_d_ga_now_ms - The monotonic clock in milliseconds, for the engine's
 * waiting line and deadline.
 */
static long e_d_ga_now_ms(void *io)
{
 struct timespec ts;

 (void)io;
 clock_gettime(CLOCK_MONOTONIC, &ts);
 return((long)ts.tv_sec * WPE_TIMER_MS_PER_S +
        (long)(ts.tv_nsec / WPE_TIMER_NS_PER_MS));
}

static void e_d_ga_after_engine(void);   /* (below) */
static void e_d_gs_settle_forget(void);  /* (below, the settle step) */

/**
 * e_d_ga_note_given_up - An answer was just given up on (Esc, Ctrl-C, the
 * deadline) and the debugger still owes it: tell the backend which
 * command's, for a debugger whose late answer may come after the settle
 * marker's (jdb's print).
 */
static void e_d_ga_note_given_up(void)
{
 if (!e_d_ga.rq.resync || !e_d_ga.be->given_up)
  return;
 e_d_ga.be->given_up(e_d_ga.rq.owed_cmd);
}

/**
 * e_d_ga_on_timer - A timer the engine asked for has gone off.
 * @data: which one (E_D_TIMER_*).
 */
static void e_d_ga_on_timer(void *data)
{
 int which = (int)(intptr_t)data;
 int was_resync = e_d_ga.rq.resync;

 e_d_ga.timer[which] = E_D_GA_NO_TIMER;
 if (!e_d_ga.live)
  return;
 e_d_req_timer(&e_d_ga.rq, &e_d_ga.io, which);
 if (!was_resync)
  e_d_ga_note_given_up();
 e_d_ga_after_engine();
}

/**
 * e_d_ga_disarm - Stop the engine's timer @which, if it runs.
 */
static void e_d_ga_disarm(int which)
{
 if (e_d_ga.timer[which] == E_D_GA_NO_TIMER)
  return;
 wpe_timer_del(e_d_ga.timer[which]);
 e_d_ga.timer[which] = E_D_GA_NO_TIMER;
}

/**
 * e_d_ga_arm_timer - Ask the loop to call the engine back in @ms.
 * @which: E_D_TIMER_*; @ms: the delay, or E_D_TIMER_OFF; @io: unused.
 */
static void e_d_ga_arm_timer(int which, long ms, void *io)
{
 (void)io;
 e_d_ga_disarm(which);
 if (ms == E_D_TIMER_OFF)
  return;
 e_d_ga.timer[which] = wpe_timer_add(ms, 0, e_d_ga_on_timer,
                                     (void *)(intptr_t)which);
 if (e_d_ga.timer[which] <= 0)
  e_d_ga.timer[which] = E_D_GA_NO_TIMER;
}

/**
 * e_d_ga_waiting_line - "waiting for gdb to answer `where` ... 3s (Esc
 * cancels)": what the editor waits for, for how long, and how to stop it.
 * @line, @n: where it goes; @cmd: the command; @secs: the seconds so far.
 */
static void e_d_ga_waiting_line(char *line, size_t n, const char *cmd,
                                long secs)
{
 snprintf(line, n, E_D_GA_WAIT_FMT, e_d_ga.io.name, cmd, secs);
}

/**
 * e_d_ga_waiting - Show the waiting line on the status row.
 * @cmd, @secs: what is waited for and since when; @io: unused.
 */
static void e_d_ga_waiting_show_kept(void *data);   /* (below) */

static void e_d_ga_waiting(const char *cmd, long secs, void *io)
{
 char line[E_D_GA_LINE_MAX];

 (void)io;
 if (e_d_ga.running_said)
  return;
 if (e_d_wait_line_keep(&e_d_ga.kept, cmd, secs, e_d_ga_waiting_show_kept))
  return;                  /* a box or a command has the screen */
 e_d_ga_waiting_line(line, sizeof line, cmd, secs);
 e_status_say(e_d_ga_window(), line);
 e_d_ga.waiting_painted = 1;
}

/**
 * e_d_ga_waiting_show_kept - Say the debugger's waiting line kept while the
 * screen was not the editor's, with the seconds of now.
 * @data: the kept line.
 */
static void e_d_ga_waiting_show_kept(void *data)
{
 e_d_wait_line *w = data;

 if (e_d_wait_line_take(w))
  e_d_ga_waiting(w->cmd, e_d_wait_line_secs_now(w), NULL);
}

/**
 * e_d_ga_waiting_clear - gdb has answered: take the waiting line off the
 * status row, so it does not go on saying the editor waits.  A line kept
 * until the next key (why an answer was given up on, a breakpoint gdb
 * refused) stays.
 */
static void e_d_ga_waiting_clear(void)
{
 FENSTER *f;

 e_d_wait_line_forget(&e_d_ga.kept);
 if (!e_d_ga.waiting_painted)
  return;
 e_d_ga.waiting_painted = 0;
 f = e_d_ga_window();
 if (!f || !f->fb)
  return;
 e_pr_uul(f->fb);
 e_refresh();
}

/**
 * e_d_ga_notice - Say why an answer was given up on, until the next key.
 * @msg: the sentence; @io: unused.
 */
static void e_d_ga_notice(const char *msg, void *io)
{
 char line[E_D_GA_LINE_MAX + E_D_REQ_MSG_MAX];

 (void)io;
 e_d_wait_line_forget(&e_d_ga.kept);
 snprintf(line, sizeof line, " %s", msg);
 if (e_status_keep_until_free(line))
  return;                  /* drawn when the screen is the editor's */
 e_status_say_until_key(e_d_ga_window(), line);
}

/* ---- watching gdb's descriptors ---- */

static void e_d_ga_on_answer(int fd, void *data);   /* (below) */
static void e_d_ga_on_errors(int fd, void *data);   /* (below) */

/**
 * e_d_ga_watch - Have the fd loop read gdb's output and error stream for
 * the engine while a request is out.
 */
static void e_d_ga_watch(void)
{
 if (!e_d_ga.answer_watched &&
     e_fd_watch(wfildes[0], POLLIN, e_d_ga_on_answer, NULL,
                "the debugger's answer") == 0)
  e_d_ga.answer_watched = 1;
 if (!e_d_ga.be->reads_errors)
  return;
 if (!e_d_ga.errors_watched && efildes[0] >= 0 &&
     e_fd_watch(efildes[0], POLLIN, e_d_ga_on_errors, NULL,
                "the debugger's error stream") == 0)
  e_d_ga.errors_watched = 1;
}

/**
 * e_d_ga_unwatch_answer - Stop reading gdb's output for the engine.
 *
 * A run that has taken the descriptor over keeps it.
 */
static void e_d_ga_unwatch_answer(void)
{
 if (!e_d_ga.answer_watched)
  return;
 e_d_ga.answer_watched = 0;
 if (!e_d_run_going())
  wpe_fd_del(wfildes[0]);
}

/**
 * e_d_ga_unwatch_errors - Stop reading gdb's error stream for the engine.
 */
static void e_d_ga_unwatch_errors(void)
{
 if (!e_d_ga.errors_watched)
  return;
 e_d_ga.errors_watched = 0;
 if (efildes[0] >= 0)
  wpe_fd_del(efildes[0]);
}

/**
 * e_d_ga_after_engine - The engine has done what it could with what came:
 * stop reading for it when it waits for nothing.
 *
 * The error stream stays watched while a breakpoint's late refusal may
 * still come (the last request's late_error hook), until the program runs.
 * The output stays watched while jdb is in the middle of writing an event
 * nobody asked for, so the event is read whole and shown.  A stop jdb
 * reported meanwhile is shown once nothing is asked any more, also when
 * the last thing asked was the settle command after an answer given up on.
 */
static void e_d_ga_after_engine(void)
{
 if (!e_d_ga.live)
  return;
 if (e_d_req_pending(&e_d_ga.rq))
  return;
 if (e_d_ga.be->chain_over)
  e_d_ga.be->chain_over();
 if (e_d_ga.be->reads_on && e_d_ga.be->reads_on())
  return;
 e_d_ga_unwatch_answer();
 if (!e_d_ga.rq.late_fn)
  e_d_ga_unwatch_errors();
}

/**
 * e_d_ga_debugger_gone - gdb's output ended while the editor waited for an
 * answer: the session ends, and the user reads why.
 */
static void e_d_ga_debugger_gone(void)
{
 e_d_note_output_closed();
 e_d_ga_unwatch_answer();
 e_d_ga_unwatch_errors();
 e_d_quit(e_d_ga_window());
}

/**
 * e_d_ga_feed - Hand the engine what the debugger wrote; what it does not
 * take, written when nothing is asked, goes to the backend (jdb's events).
 * @bytes, @n: what was read.
 */
static void e_d_ga_feed(const char *bytes, size_t n)
{
 size_t taken = e_d_req_feed(&e_d_ga.rq, &e_d_ga.io, bytes, n);

 if (taken < n && e_d_ga.live && e_d_ga.be->unsolicited)
  e_d_ga.be->unsolicited(bytes + taken, n - taken);
}

/**
 * e_d_ga_feed_separated - As e_d_ga_feed, for a debugger whose prompts are
 * first given lines of their own (separate_prompts).
 * @bytes, @n: what was read.
 */
static void e_d_ga_feed_separated(const char *bytes, size_t n)
{
 char split[2 * E_D_GA_READ_MAX];

 e_d_ga_feed(split, e_d_ga.be->separate_prompts(bytes, n, split));
}

/**
 * e_d_ga_on_answer - gdb wrote on its output while the engine waits.
 * @fd: gdb's output; @data: unused.
 *
 * Runs on the fd loop, between the user's keys.  Bytes that come when
 * nothing is asked are gdb's and pdb's own and are dropped; jdb's are read
 * for an event it wrote behind the answer's prompt (unsolicited).
 */
static void e_d_ga_on_answer(int fd, void *data)
{
 char buf[E_D_GA_READ_MAX];
 ssize_t n;

 (void)data;
 n = read(fd, buf, sizeof buf);
 if (n < 0 && (errno == EAGAIN || errno == EINTR))
  return;
 if (n <= 0)
 {
  e_d_ga_debugger_gone();
  return;
 }
 if (e_d_ga.be->separate_prompts)
  e_d_ga_feed_separated(buf, (size_t)n);
 else
  e_d_ga_feed(buf, (size_t)n);
 e_d_ga_after_engine();
}

/**
 * e_d_ga_on_errors - gdb wrote on its error stream while the engine waits.
 * @fd: gdb's error stream; @data: unused.
 *
 * Its words join the answer they belong to, or a breakpoint's refusal that
 * comes just after its prompt.
 */
static void e_d_ga_on_errors(int fd, void *data)
{
 char buf[E_D_GA_READ_MAX];
 ssize_t n;

 (void)data;
 n = read(fd, buf, sizeof buf);
 if (n < 0 && (errno == EAGAIN || errno == EINTR))
  return;
 if (n <= 0)
 {
  e_d_ga_unwatch_errors();
  return;
 }
 e_d_req_feed_errors(&e_d_ga.rq, &e_d_ga.io, buf, (size_t)n);
 e_d_ga_after_engine();
}

/**
 * e_d_ga_errors_waiting - Hand the engine what the debugger has already
 * written on its error stream, without waiting.
 * @buf, @n: where it goes; @io: unused.
 *
 * Asked when the prompt that ends an answer has been read.  The debugger
 * wrote its refusal before that prompt, so it is waiting now: the status
 * row gets "No source file named q.c." with the breakpoint it refuses, even
 * when a busy editor read the prompt before the error stream.
 * Return: the bytes, 0 when none are waiting.
 */
static size_t e_d_ga_errors_waiting(char *buf, size_t n, void *io)
{
 ssize_t got;

 (void)io;
 if (wpe_fd_readable_now(efildes[0]) != WPE_FD_READABLE)
  return(0);
 got = read(efildes[0], buf, n);
 if (got <= 0)
  return(0);
 return((size_t)got);
}

/**
 * e_d_gs_numbered_marker - The next numbered settle marker of this settle.
 * @out, @n: where it goes.
 *
 * Each settle command of a debugger whose interrupt may take one away
 * carries a number of its own, so an echo that was only late is never
 * taken for a later command's.
 */
static void e_d_gs_numbered_marker(char *out, size_t n)
{
 e_d_ga.settle_seq++;
 snprintf(out, n, E_D_GA_MARKER_FMT, e_d_ga.be->settle_marker,
          e_d_ga.settle_seq);
}

/**
 * e_d_ga_resync_settle - The settle command the engine sends after the user
 * gave up on an answer, for a debugger whose settle commands are numbered.
 * @cmd, @cmd_n: the command; @marker, @marker_n: the echo it makes the
 * debugger write; @io: unused.
 *
 * Esc during a second settle round leaves the echoes of both rounds owed,
 * in the plain marker's words: the resync asks for a number past them, so
 * it ends at its own echo and the user's next command reads its own answer.
 */
static void e_d_ga_resync_settle(char *cmd, size_t cmd_n, char *marker,
                                 size_t marker_n, void *io)
{
 (void)io;
 e_d_gs_numbered_marker(marker, marker_n);
 e_d_ga.be->settle_cmd_for(marker, cmd, cmd_n);
}

/* ---- the engine's life ---- */

/**
 * e_d_ga_world - Fill in what the engine needs from the editor.
 */
static void e_d_ga_world(void)
{
 memset(&e_d_ga.io, 0, sizeof e_d_ga.io);
 e_d_ga.io.send = e_d_ga_send;
 if (e_d_ga.be->send)
  e_d_ga.io.send = e_d_ga.be->send;
 e_d_ga.io.answer_ended = e_d_ga_answer_ended;
 e_d_ga.io.line_is_async = e_d_ga.be->line_is_async;
 e_d_ga.io.deliver_async = e_d_ga.be->deliver_async;
 e_d_ga.io.now_ms = e_d_ga_now_ms;
 e_d_ga.io.arm_timer = e_d_ga_arm_timer;
 e_d_ga.io.waiting = e_d_ga_waiting;
 e_d_ga.io.notice = e_d_ga_notice;
 if (e_d_ga.be->reads_errors)
  e_d_ga.io.errors_waiting = e_d_ga_errors_waiting;
 e_d_ga.io.name = e_d_ga.be->name;
 e_d_ga.io.settle_cmd = e_d_ga.be->settle_cmd;
 e_d_ga.io.settle_marker = e_d_ga.be->settle_marker;
 if (e_d_ga.be->settle_cmd_for)
  e_d_ga.io.resync_settle = e_d_ga_resync_settle;
 e_d_ga.link.rq = &e_d_ga.rq;
 e_d_ga.link.io = &e_d_ga.io;
 e_d_ga.via.request = e_d_req_request;
 e_d_ga.via.engine = &e_d_ga.link;
}

/**
 * e_d_ga_begin - Make the engine ready for this session, once, for the
 * debugger that runs it.
 * @cn: the editor.
 *
 * The deadline is E_D_REQ_TIMEOUT_S, or the user's XWPE_DEBUG_ANSWER_TIMEOUT
 * for a program whose answers take long.
 */
static void e_d_ga_begin(ECNT *cn)
{
 int i;

 e_d_ga.cn = cn;
 if (e_d_ga.live)
  return;
 if (e_d_ga.made)
  e_d_req_free(&e_d_ga.rq);
 e_d_req_init(&e_d_ga.rq, getenv(E_D_REQ_TIMEOUT_ENV));
 e_d_ga.made = 1;
 e_d_ga.be = e_d_ga_backend_of(e_deb_type);
 e_d_ga_world();
 for (i = 0; i < E_D_TIMERS; i++)
  e_d_ga.timer[i] = E_D_GA_NO_TIMER;
 e_d_ga.act_timer = E_D_GA_NO_TIMER;
 e_d_ga.chain = E_D_GA_NONE;
 e_d_ga.nact = 0;
 e_d_ga.owed = 0;
 e_d_gs_settle_forget();
 e_d_ga.pause_asked = 0;
 e_d_ga.running_said = 0;
 e_d_ga.waiting_painted = 0;
 e_d_ga.set_up_owed = 0;
 e_d_ga.live = 1;
}

/**
 * e_d_ga_actions_drop - Forget the debugger keys kept while gdb answered.
 *
 * After a cancel or at the end of the session they have nothing left to
 * act on.
 */
static void e_d_ga_actions_drop(void)
{
 e_d_ga.nact = 0;
 if (e_d_ga.act_timer == E_D_GA_NO_TIMER)
  return;
 wpe_timer_del(e_d_ga.act_timer);
 e_d_ga.act_timer = E_D_GA_NO_TIMER;
}

/**
 * e_d_ga_forget - The session ends: every request still out is told so,
 * nothing more is read or sent, and no step paints anything.
 *
 * The engine's memory is kept until the next session begins, since this
 * may be called from inside one of its own callbacks.
 */
static void e_d_ga_forget(void)
{
 int i;

 if (!e_d_ga.live)
  return;
 e_d_ga.live = 0;
 e_d_ga_actions_drop();
 e_d_req_cancel(&e_d_ga.rq, &e_d_ga.io, E_D_REQ_GONE);
 e_d_ga_unwatch_answer();
 e_d_ga_unwatch_errors();
 for (i = 0; i < E_D_TIMERS; i++)
  e_d_ga_disarm(i);
 e_d_ga.chain = E_D_GA_NONE;
 e_d_gs_settle_forget();
 e_d_ga.owed = 0;
 e_d_ga.pause_asked = 0;
 e_d_ga.set_up_owed = 0;
 e_d_gdb_awaits_forget();
 e_d_cli_awaits_forget();
}

/**
 * e_d_ga_run_begins - The program runs again: the run reads gdb's output,
 * and an error line from now on is no breakpoint's late refusal.
 */
static void e_d_ga_run_begins(void)
{
 if (!e_d_ga.live)
  return;
 e_d_ga.answer_watched = 0;     /* the run's watch replaces the engine's */
 e_d_req_set_late_error(&e_d_ga.rq, NULL, NULL);
 e_d_ga_unwatch_errors();
}

/**
 * e_d_ga_owe_settle - Note the interrupts gdb may still answer, for the
 * settle step of the next sequence of requests.
 * @owed: how many.
 */
static void e_d_ga_owe_settle(int owed)
{
 e_d_ga.owed = owed;
}

/* ---- what the user sees while gdb answers ---- */

/**
 * e_d_ga_answering - Is the editor waiting for the debugger, or working
 * through a sequence of its answers?  A debug adapter's answers count as
 * a line debugger's do, so its keys are kept and refused the same way.
 */
static int e_d_ga_answering(void)
{
 if (e_d_dr_answering())
  return(1);
 if (!e_d_ga.live)
  return(0);
 return(e_d_req_pending(&e_d_ga.rq) || e_d_ga.chain != E_D_GA_NONE);
}

/**
 * e_d_ga_sync_runs - Is gdb being told of breakpoint changes at a stop the
 * user is not shown?  For the user the program runs meanwhile.
 */
static int e_d_ga_sync_runs(void)
{
 return(e_d_ga_answering() && e_d_ga.chain == E_D_GA_SYNC);
}

/**
 * e_d_ga_at_stop - Is the debugger (gdb, pdb, jdb, a68g) at its prompt and asked
 * nothing, so a command of the user's goes to it through the engine?
 *
 * True at a stop, and also before the program first runs once the debugger
 * has answered its banner (a start the user gave up on and tries again), so
 * no command of the user's ever waits on it in front of a frozen editor.
 */
static int e_d_ga_at_stop(void)
{
 if (!e_d_ga_backend_of(e_deb_type) || e_d_swtch < 1 || e_d_start.active)
  return(0);
 if (e_d_run_going() || e_d_ga_answering())
  return(0);
 return(1);
}

/**
 * e_d_ga_refusal_line - What a key that needs gdb is told while gdb
 * answers: the waiting line, or, while an old answer is still dropped, that
 * gdb is still answering it.
 * @line, @n: where it goes.
 */
static void e_d_ga_refusal_line(char *line, size_t n)
{
 E_D_REQ_QUEUE *rq = &e_d_ga.rq;
 long secs;

 if (e_d_dr_answering())
 {
  e_d_dr_refusal_line(line, n);
  return;
 }
 if (rq->resync || rq->n == 0)
 {
  line[0] = ' ';
  e_d_req_busy_message(rq, &e_d_ga.io, line + 1, n - 1);
  return;
 }
 secs = (e_d_ga_now_ms(NULL) - rq->sent_ms) / WPE_TIMER_MS_PER_S;
 e_d_ga_waiting_line(line, n, rq->q[rq->head].cmd, secs);
}

/**
 * e_d_ga_refused - Refuse a debugger command while gdb answers.
 * @f: the window the key was pressed in.
 *
 * A command sent now would be answered out of turn.  While the user is
 * shown a stop the status row shows the waiting line, so the user sees why
 * and that Esc ends the wait; while gdb is told of a breakpoint change the
 * program runs for the user, and the key is refused as while it runs.
 * Return: 1 when the command must not run.
 */
static int e_d_ga_refused(FENSTER *f)
{
 char line[E_D_GA_LINE_MAX + E_D_REQ_MSG_MAX];

 if (!e_d_ga_answering())
  return(0);
 if (e_d_ga.chain == E_D_GA_SYNC)
 {
  e_d_ga.running_said = 1;
  e_d_running_refusal_say(f);
  return(1);
 }
 e_d_ga_refusal_line(line, sizeof line);
 if (!line[1])
  e_d_ga_waiting_line(line, sizeof line, E_D_GDB_WHERE, 0);
 e_status_say(f, line);
 e_d_ga.waiting_painted = 1;
 return(1);
}

/**
 * e_d_ga_pause_takes_sync - Pause pressed while gdb is told of a breakpoint
 * change: the program is already stopped, so the stop becomes the user's.
 * @f: the window the key was pressed in.
 *
 * When gdb has answered, the program is not let run on: its stop is marked
 * and the status row says "Paused".
 * Return: 1 when Pause was taken here.
 */
static int e_d_ga_pause_takes_sync(FENSTER *f)
{
 if (!e_d_ga_sync_runs())
  return(0);
 e_d_ga.pause_asked = 1;
 e_status_say(f, E_D_PAUSING_LINE);
 return(1);
}

/**
 * e_d_answer_busy - Does the editor wait for gdb's answer?
 *
 * The editor's key loop asks this so Esc and Ctrl-C end the wait, as the
 * waiting line says.
 * Return: 1 while it waits.
 */
int e_d_answer_busy(void)
{
 if (e_d_dr_busy())
  return(1);
 return(e_d_ga.live && e_d_req_pending(&e_d_ga.rq));
}

/**
 * e_d_answer_cancel - Give up on the answer gdb owes, and on every command
 * that waited behind it.
 *
 * The user gets the editor back at once, with a line naming the command gdb
 * did not answer.  The keys kept for after the answer are dropped with it;
 * gdb's late answer is dropped too, so it is never taken for another
 * command's.  Debug > Quit still ends the session.
 */
void e_d_answer_cancel(void)
{
 int was_resync = e_d_ga.rq.resync;

 if (e_d_dr_busy())
 {
  e_d_dr_cancel();
  return;
 }
 if (!e_d_ga.live)
  return;
 e_d_ga_actions_drop();
 e_d_req_cancel(&e_d_ga.rq, &e_d_ga.io, E_D_REQ_CANCELLED);
 if (!was_resync)
  e_d_ga_note_given_up();
 e_d_ga_after_engine();
}

/**
 * e_d_ga_chain_start - Run a table of steps on gdb's engine.
 * @cn: the editor; @steps: the table; @ctx: handed to every step;
 * @finished: told once at the end; @kind: E_D_GA_STOP, _SYNC or _ASK.
 * Return: 1 while it runs or when it ended well at once, 0 when it failed.
 */
static int e_d_ga_chain_start(ECNT *cn, const E_D_STEP *steps, void *ctx,
                              e_d_chain_done_fn finished, int kind)
{
 int ret;

 if (!e_d_ga_backend_of(e_deb_type))
 {
  finished(0, ctx);              /* no debugger of the engine's runs */
  return(0);
 }
 e_d_ga_begin(cn);
 e_d_ga.chain = kind;
 e_d_ga_watch();
 ret = e_d_chain_run(&e_d_ga.via, steps, ctx, finished);
 e_d_ga_after_engine();
 return(ret);
}

/**
 * e_d_ga_chain_ended - A table of steps is over: nothing runs on the engine
 * any more.
 * Return: 1 when the session goes on, 0 when it ended meanwhile.
 */
static int e_d_ga_chain_ended(void)
{
 e_d_ga.chain = E_D_GA_NONE;
 e_d_gs_settle_forget();
 e_d_ga.running_said = 0;
 e_d_gb_removing_id = 0;
 e_d_gb_add_given_up();
 if (!e_d_ga.live)
  return(0);
 e_d_ga_waiting_clear();
 if (e_d_ga.be->chain_over)
  e_d_ga.be->chain_over();
 return(1);
}

/* ---- the debug adapters' answers on the fd loop ----
 *
 * What the editor asks an adapter at a stop (the stack, the watches) runs
 * as the same tables of steps the line debuggers' stops run, through the
 * adapter's own engine (e_dap_req_request over its seq table) instead of
 * the line engine.  The user keeps the editor meanwhile: a letter goes into
 * the file, a debugger key is kept or refused with the line debuggers'
 * words (e_d_act_deferred, e_d_ga_refused), the waiting line and the
 * deadline are the line debuggers', and Esc gives the answer up.
 */

#define E_D_DR_NO_FD      (-1)
#define E_D_DR_FALLBACK_CMD "stackTrace"   /* named while a step is between
                                              two requests */

static struct
{
 E_D_REQ_IO io;              /* the clock, the timers, the waiting line */
 E_D_REQUESTER via;          /* the adapter's seq table */
 int timer[E_D_TIMERS];      /* the loop's timer ids, E_D_GA_NO_TIMER */
 int chain;                  /* E_D_GA_*: what runs, or E_D_GA_NONE */
 int forgetting;             /* the session ends: no step paints anything */
 int watched;                /* the fd loop reads the adapter for its answers */
 int fd;                     /* the descriptor it reads */
} e_d_dr;

/**
 * e_d_dr_waiting_line - "waiting for dlv to answer `evaluate` ... 3s (Esc
 * cancels)", in the line debuggers' words.
 * @line, @n: where it goes; @cmd: the request; @secs: the seconds so far.
 */
static void e_d_dr_waiting_line(char *line, size_t n, const char *cmd,
                                long secs)
{
 snprintf(line, n, E_D_GA_WAIT_FMT, g_dap_adapter_name, cmd, secs);
}

/**
 * e_d_dr_waiting - Show the waiting line on the status row.
 * @cmd, @secs: what is waited for and since when; @io: unused.
 */
static void e_d_dr_waiting(const char *cmd, long secs, void *io)
{
 char line[E_D_GA_LINE_MAX];

 (void)io;
 e_d_dr_waiting_line(line, sizeof line, cmd, secs);
 e_status_say(e_d_ga_window(), line);
 e_d_ga.waiting_painted = 1;
}

static void e_d_dr_after_engine(void);

/**
 * e_d_dr_on_timer - A timer the adapter's requests asked for has gone off.
 * @data: which one (E_D_TIMER_*).
 */
static void e_d_dr_on_timer(void *data)
{
 int which = (int)(intptr_t)data;

 e_d_dr.timer[which] = E_D_GA_NO_TIMER;
 if (!g_dap)
  return;
 e_dap_request_timer(g_dap, which);
 e_d_dr_after_engine();
}

/**
 * e_d_dr_disarm - Stop timer @which, if it runs.
 */
static void e_d_dr_disarm(int which)
{
 if (e_d_dr.timer[which] == E_D_GA_NO_TIMER)
  return;
 wpe_timer_del(e_d_dr.timer[which]);
 e_d_dr.timer[which] = E_D_GA_NO_TIMER;
}

/**
 * e_d_dr_arm_timer - Ask the loop to call the adapter's requests back.
 * @which: E_D_TIMER_*; @ms: the delay, or E_D_TIMER_OFF; @io: unused.
 */
static void e_d_dr_arm_timer(int which, long ms, void *io)
{
 (void)io;
 e_d_dr_disarm(which);
 if (ms == E_D_TIMER_OFF)
  return;
 e_d_dr.timer[which] = wpe_timer_add(ms, 0, e_d_dr_on_timer,
                                     (void *)(intptr_t)which);
 if (e_d_dr.timer[which] <= 0)
  e_d_dr.timer[which] = E_D_GA_NO_TIMER;
}

/**
 * e_d_dr_begin - A debug adapter session starts: give its engine the
 * editor's clock, timers and status row.
 *
 * The deadline is E_D_REQ_TIMEOUT_S, or the user's XWPE_DEBUG_ANSWER_TIMEOUT
 * for a program whose watches take long to compute, as for the line
 * debuggers.
 */
static void e_d_dr_begin(void)
{
 int i;

 memset(&e_d_dr.io, 0, sizeof e_d_dr.io);
 e_d_dr.io.now_ms = e_d_ga_now_ms;
 e_d_dr.io.arm_timer = e_d_dr_arm_timer;
 e_d_dr.io.waiting = e_d_dr_waiting;
 e_d_dr.io.notice = e_d_ga_notice;
 e_d_dr.io.name = g_dap_adapter_name;
 e_d_dr.via.request = e_dap_req_request;
 e_d_dr.via.engine = g_dap;
 for (i = 0; i < E_D_TIMERS; i++)
  e_d_dr.timer[i] = E_D_GA_NO_TIMER;
 e_d_dr.chain = E_D_GA_NONE;
 e_d_dr.forgetting = 0;
 e_d_dr.watched = 0;
 e_d_dr.fd = E_D_DR_NO_FD;
 e_dap_requests_init(g_dap, &e_d_dr.io, getenv(E_D_REQ_TIMEOUT_ENV));
}

static void e_d_dr_on_answer(int fd, void *data);

/**
 * e_d_dr_watch - Have the fd loop read the adapter while it owes answers
 * and the program is stopped.
 *
 * While the program runs the run's own reader takes them.  A loop with no
 * room leaves them unread, and the deadline ends the wait with its line.
 */
static void e_d_dr_watch(void)
{
 int fd;

 if (!g_dap || e_d_dr.watched || e_d_run_going())
  return;
 if (!e_dap_answers_owed(g_dap))
  return;
 fd = e_dap_fd(g_dap);
 if (fd < 0)
  return;
 if (e_fd_watch(fd, POLLIN, e_d_dr_on_answer, NULL,
                "the debug adapter's answer") != 0)
  return;
 e_d_dr.watched = 1;
 e_d_dr.fd = fd;
}

/**
 * e_d_dr_unwatch - Stop reading the adapter for its answers.
 *
 * A run that has taken the descriptor over keeps it.
 */
static void e_d_dr_unwatch(void)
{
 if (!e_d_dr.watched)
  return;
 e_d_dr.watched = 0;
 if (!e_d_run_going())
  wpe_fd_del(e_d_dr.fd);
 e_d_dr.fd = E_D_DR_NO_FD;
}

/**
 * e_d_dr_run_begins - The program runs again: the run's reader takes the
 * adapter's descriptor, answers included.
 */
static void e_d_dr_run_begins(void)
{
 e_d_dr.watched = 0;
 e_d_dr.fd = E_D_DR_NO_FD;
}

/**
 * e_d_dr_show_breakpoint_answers - Draw the margin again once the adapter's
 * word on a breakpoint set at a stop has come.
 *
 * The user sees the hollow mark turn solid.  While the stop's own answers
 * still come (they draw the windows at their end) the drawing waits for
 * them; with a box or a non-text window in front it waits for the box to
 * close.
 */
static void e_d_dr_show_breakpoint_answers(void)
{
 if (!e_d_brk_live_changed || e_d_run_going() || e_d_dr.chain != E_D_GA_NONE)
  return;
 wpe_paint_request(e_d_brk_repaint_live_answers, WpeEditor,
                   "a breakpoint's answer");
}

/**
 * e_d_dr_after_engine - The adapter's engine has done what it could with
 * what came: draw what changed, and stop reading when nothing is owed.
 */
static void e_d_dr_after_engine(void)
{
 if (!g_dap)
 {
  e_d_dr_unwatch();
  return;
 }
 e_d_dr_show_breakpoint_answers();
 if (!e_dap_answers_owed(g_dap))
  e_d_dr_unwatch();
}

/**
 * e_d_dr_on_answer - The adapter wrote while the program is stopped.
 * @fd: its descriptor; @data: unused.
 *
 * Runs on the fd loop, between the user's keys.  An adapter that hung up
 * ends the session, and the user reads why.
 */
static void e_d_dr_on_answer(int fd, void *data)
{
 (void)data;
 if (!g_dap)
 {
  wpe_fd_del(fd);
  e_d_dr.watched = 0;
  return;
 }
 if (e_dap_read_now(g_dap))
 {
  e_d_dr_after_engine();
  return;
 }
 e_d_dr_unwatch();
 e_d_quit(e_d_ga_window());
}

/**
 * e_d_dr_answering - Does the editor wait for an adapter's answer, or work
 * through a table of its answers?
 */
static int e_d_dr_answering(void)
{
 if (!g_dap)
  return(0);
 return(e_d_dr.chain != E_D_GA_NONE || e_dap_requests_pending(g_dap));
}

/**
 * e_d_dr_busy - Is an adapter's answer waited for, so Esc and Ctrl-C give
 * it up?
 */
static int e_d_dr_busy(void)
{
 return(g_dap && e_dap_requests_pending(g_dap));
}

/**
 * e_d_dr_refusal_line - What a key that needs the adapter is told while it
 * answers: the waiting line, naming the request.
 * @line, @n: where it goes.
 */
static void e_d_dr_refusal_line(char *line, size_t n)
{
 long secs = 0;
 const char *cmd = e_dap_request_waited(g_dap, &secs);

 if (!cmd)
  cmd = E_D_DR_FALLBACK_CMD;
 e_d_dr_waiting_line(line, n, cmd, secs);
}

/**
 * e_d_dr_cancel - Esc or Ctrl-C: give up on the answer the adapter owes,
 * and on what the stop still had to ask.
 *
 * The user gets the editor back at once, with the line debuggers' line
 * naming the request; the keys kept for after the answer are dropped, and
 * the adapter's late answer is dropped when it comes.  Debug > Quit still
 * ends the session.
 */
static void e_d_dr_cancel(void)
{
 char msg[E_D_REQ_MSG_MAX];
 const char *cmd = e_dap_request_waited(g_dap, NULL);

 e_d_ga_actions_drop();
 if (cmd)
 {
  snprintf(msg, sizeof msg, E_D_REQ_CANCEL_FMT, g_dap_adapter_name, cmd);
  e_d_ga_notice(msg, NULL);
 }
 e_dap_requests_cancel(g_dap, E_D_REQ_CANCELLED);
 e_d_dr_after_engine();
}

/**
 * e_d_dr_forget - The adapter session ends: every answer still owed is
 * given up, nothing more is read for it, and no step paints anything.
 */
static void e_d_dr_forget(void)
{
 int i;

 e_d_dr.forgetting = 1;
 e_d_ga_actions_drop();
 if (g_dap)
  e_dap_requests_cancel(g_dap, E_D_REQ_GONE);
 e_d_dr_unwatch();
 for (i = 0; i < E_D_TIMERS; i++)
  e_d_dr_disarm(i);
 e_d_dr.chain = E_D_GA_NONE;
 e_d_dr.forgetting = 0;
}

/**
 * e_d_dr_chain_start - Run a table of steps on the adapter's engine.
 * @cn: the editor; @steps: the table; @ctx: handed to every step;
 * @finished: told once at the end; @kind: E_D_GA_STOP or E_D_GA_ASK.
 * Return: 1 while it runs or when it ended well at once, 0 when it failed.
 */
static int e_d_dr_chain_start(ECNT *cn, const E_D_STEP *steps, void *ctx,
                              e_d_chain_done_fn finished, int kind)
{
 int ret;

 if (!g_dap)
 {
  finished(0, ctx);
  return(0);
 }
 e_d_ga.cn = cn;
 e_d_dr.chain = kind;
 ret = e_d_chain_run(&e_d_dr.via, steps, ctx, finished);
 e_d_dr_watch();
 e_d_dr_after_engine();
 return(ret);
}

/**
 * e_d_dr_chain_ended - A table of steps is over on the adapter's engine.
 * Return: 1 when the session goes on, 0 when it ended meanwhile.
 */
static int e_d_dr_chain_ended(void)
{
 e_d_dr.chain = E_D_GA_NONE;
 e_d_ga.running_said = 0;
 if (!g_dap || e_d_dr.forgetting)
  return(0);
 e_d_ga_waiting_clear();
 return(1);
}

/* ---- an adapter's stop and watches, asked on the fd loop ---- */

/* What the steps of an adapter's stop or watches learn. */
typedef struct
{
 ECNT *cn;
 int stop;                   /* the answers are a stop's: mark it */
 int pause;                  /* the stop is the user's Pause */
 int pause_frame;            /* the frame Pause marks, 0 innermost */
 int watch;                  /* the next watch to ask */
 char **values;              /* each watch's value, or NULL */
 int nvalues;
 int watches_sw;             /* bring the Watches window to the top */
} E_D_DAP_CTX;

static E_D_DAP_CTX e_d_dc;

/**
 * e_d_dc_free - Give back the values a table of steps kept.
 * @st: the context.
 */
static void e_d_dc_free(E_D_DAP_CTX *st)
{
 int i;

 for (i = 0; i < st->nvalues; i++)
  free(st->values[i]);
 free(st->values);
 st->values = NULL;
 st->nvalues = 0;
}

/**
 * e_d_dc_reset - Make the context ready for a new table of steps, with
 * room for a value for every watch.
 * @st: the context; @cn: the editor.
 */
static void e_d_dc_reset(E_D_DAP_CTX *st, ECNT *cn)
{
 e_d_dc_free(st);
 memset(st, 0, sizeof(*st));
 st->cn = cn;
 if (e_d_nwtchs <= 0)
  return;
 st->values = calloc((size_t)e_d_nwtchs, sizeof(char *));
 if (st->values)
  st->nvalues = e_d_nwtchs;
}

/**
 * e_d_dap_frame_is_users - Is a frame of the stack the user's own code, in
 * a file the editor can open?
 * @fr: the frame.
 * Return: 1 when it is.
 */
static int e_d_dap_frame_is_users(const e_d_frame *fr)
{
 if (!fr->file[0] || fr->line <= 0 || !fr->user)
  return(0);
 return(access(fr->file, R_OK) == 0);
}

/**
 * e_d_dap_pause_pick - After Pause, move the stop to the user's code.
 *
 * The adapter stops the program wherever it is, often inside the language
 * runtime (dlv in Go's scheduler, a sleep in a library).  The first frame
 * of the stack that is in the user's program becomes the stop to mark and
 * the frame the watches are read in, so the user sees the line the program
 * is working on.  A stop already in the user's code is left alone.
 * Return: the index of the frame chosen, 0 when the stop stays as it is.
 */
static int e_d_dap_pause_pick(void)
{
 int i;

 if (e_d_dap_in_user_code() && g_dap_stop_file[0])
  return(0);
 for (i = 0; i < e_d_nframes; i++)
 {
  if (!e_d_dap_frame_is_users(&e_d_frames[i]))
   continue;
  snprintf(g_dap_stop_file, sizeof g_dap_stop_file, "%s", e_d_frames[i].file);
  g_dap_stop_line = e_d_frames[i].line;
  e_dap_select_frame(g_dap, e_d_frames[i].dap_id);
  return(i);
 }
 return(0);
}

/* step: the stack of the stop (Stack window, Finish function, Pause) */

static int e_d_dc_frames_cmd(char *cmd, size_t n, void *ctx)
{
 (void)ctx;
 return(e_dap_stage_stack_trace(g_dap, E_D_FRAMES_MAX, cmd, n));
}

static int e_d_dc_frames_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 E_D_DAP_CTX *st = ctx;

 (void)how;
 e_d_dap_take_frames(ans);
 if (st->pause)
  st->pause_frame = e_d_dap_pause_pick();
 return(E_D_STEP_NEXT);
}

/* step: one watch's value; AGAIN until every watch has been asked */

static int e_d_dc_watch_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_DAP_CTX *st = ctx;

 if (st->stop && !e_d_dap_stop_has_place())
  return(0);
 if (st->watch >= st->nvalues || st->watch >= e_d_nwtchs)
  return(0);           /* a project reload took watches away meanwhile */
 return(e_dap_stage_evaluate(g_dap, e_d_swtchs[st->watch], cmd, n));
}

static int e_d_dc_watch_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 E_D_DAP_CTX *st = ctx;

 (void)how;
 st->values[st->watch] = e_dap_answer_result(ans);
 st->watch++;
 if (st->watch < st->nvalues)
  return(E_D_STEP_AGAIN);
 return(E_D_STEP_NEXT);
}

static const E_D_STEP e_d_dap_stop_steps[] =
{
 { e_d_dc_frames_cmd, e_d_dc_frames_answered },
 { e_d_dc_watch_cmd,  e_d_dc_watch_answered },
 { NULL, NULL }
};

static const E_D_STEP e_d_dap_watch_steps[] =
{
 { e_d_dc_watch_cmd,  e_d_dc_watch_answered },
 { NULL, NULL }
};

/**
 * e_d_dap_stop_tail - The stop is dealt with: take out the breakpoints the
 * editor set for this run, tell the adapter of the user's changes, and, when
 * the stop was shown, say how a Pause ended and run the next kept key.
 * @cn: the editor; @shown: 0 when the stop's answers were given up on.
 */
static void e_d_dap_stop_tail(ECNT *cn, int shown)
{
 e_d_run_stop_chores(cn->f[cn->mxedt]);
 if (!shown)
  return;
 e_d_run_stop_tail(cn, e_d_run.ops);
 e_d_act_schedule();
}

/**
 * e_d_dc_stop_finished - The adapter has answered everything the stop
 * asks: the user sees the stop marked and the windows filled.
 * @ok: 0 when an answer was given up on (the status row already says
 * which): nothing is drawn and the kept keys are dropped; @ctx: the context.
 */
static void e_d_dc_stop_finished(int ok, void *ctx)
{
 E_D_DAP_CTX *st = ctx;
 ECNT *cn = st->cn;

 if (!e_d_dr_chain_ended())
 {
  e_d_dc_free(st);
  return;
 }
 if (!ok)
 {
  e_d_ga_actions_drop();
  e_d_dc_free(st);
  e_d_dap_stop_tail(cn, 0);
  return;
 }
 e_d_dap_show_stop(cn->f[cn->mxedt], st->values, st->nvalues);
 if (st->pause)
  e_d_nstack = st->pause_frame;
 e_d_dc_free(st);
 e_d_dap_stop_tail(cn, 1);
}

/**
 * e_d_dap_stop_ask - The program stopped where the user is to see it: ask
 * the adapter, on the fd loop, for the stack and the watches, and show the
 * stop once it has answered.
 * @f: the window on top; @pause: 1 for the stop Pause asked for.
 *
 * The user sees the program's output at once; the mark, the watches and
 * the stack come when the adapter answers, the keys reaching the editor
 * meanwhile.
 */
static void e_d_dap_stop_ask(FENSTER *f, int pause)
{
 E_D_DAP_CTX *st = &e_d_dc;

 e_d_dc_reset(st, f->ed);
 st->stop = 1;
 st->pause = pause;
 e_d_nframes = 0;
 e_d_run.stop_on_loop = 1;
 e_d_dr_chain_start(f->ed, e_d_dap_stop_steps, st, e_d_dc_stop_finished,
                    E_D_GA_STOP);
}

/**
 * e_d_dc_watches_finished - The watches have been answered: fill their
 * window, then run the next kept key.
 * @ok: 0 when an answer was given up on; @ctx: the context.
 */
static void e_d_dc_watches_finished(int ok, void *ctx)
{
 E_D_DAP_CTX *st = ctx;
 ECNT *cn = st->cn;

 if (!e_d_dr_chain_ended() || !ok)
 {
  e_d_ga_actions_drop();
  e_d_dc_free(st);
  return;
 }
 e_d_dap_watches_fill(cn->f[cn->mxedt], st->values, st->nvalues,
                      st->watches_sw);
 e_d_dc_free(st);
 e_d_act_schedule();
}

/**
 * e_d_dap_watches_ask - Ask every watch again at a stop, after the user
 * added, edited or deleted one, or picked a frame.
 * @cn: the editor; @sw: 1 to bring the Watches window to the top.
 * Return: 0.
 */
static int e_d_dap_watches_ask(ECNT *cn, int sw)
{
 E_D_DAP_CTX *st = &e_d_dc;

 e_d_dc_reset(st, cn);
 st->watches_sw = sw;
 e_d_dr_chain_start(cn, e_d_dap_watch_steps, st, e_d_dc_watches_finished,
                    E_D_GA_ASK);
 return(0);
}

/* ---- Quit: an adapter ended in the background ----
 *
 * Quit gives the user the editor back at once.  The adapter is asked to
 * end its program (`disconnect`) on the fd loop, within E_DAP_DISCONNECT_MS;
 * then the owner of the editor's children ends it on its one ladder (the
 * grace to exit by itself, SIGKILL, the kill grace) and reports its end, the
 * moment it happens.  The build server that hosts the Scala adapter is
 * closed after it.
 */

#define E_D_DAP_CLOSE_SLOTS  4   /* adapters being ended at once */
#define E_D_DAP_CLOSE_FREE   0
#define E_D_DAP_CLOSE_ANSWER 1   /* disconnect sent; waits for its answer */
#define E_D_DAP_CLOSE_ENDING 2   /* the owner's ladder ends it; waits for its end */
#define E_D_DAP_CLOSE_LATE_FMT \
 " %s did not answer `disconnect` within %d s: ended it"
#define E_D_DAP_CLOSE_LINE_MAX 160
#define E_D_DAP_MS_PER_S       1000
/* How the owner's ladder begins for a closing adapter.  One asked to go on
   its pipe (`disconnect` sent, answered or not) gets no signal, only the
   grace to exit by itself; one never asked (it had not answered
   initialize) gets SIGTERM.  No signal makes a program outlive its adapter
   less: measured, dlv's program ends with dlv's pty whatever ends dlv,
   lldb-dap's survives SIGTERM and SIGKILL alike, and gdb ignores SIGTERM
   while its program ends at gdb's SIGKILL. */
#define E_D_DAP_END_ASKED      WPE_CHILD_NO_SIGNAL
#define E_D_DAP_END_NOT_ASKED  SIGTERM

typedef struct
{
 int phase;                  /* E_D_DAP_CLOSE_* */
 e_dap_session *s;
 e_bsp_session *bsp;         /* the build server that hosts it, or NULL */
 int timer;                  /* the answer's deadline, or WPE_TIMER_NONE */
 int fd;                     /* its transport, while watched, or -1 */
 int pty;                    /* dlv's pty, while watched, or -1 */
 char name[sizeof g_dap_adapter_name];
} E_D_DAP_CLOSING;

static E_D_DAP_CLOSING e_d_dap_closing[E_D_DAP_CLOSE_SLOTS];

/**
 * e_d_dap_close_unwatch - Stop reading what a closing adapter writes.
 * @c: the closing adapter.
 */
static void e_d_dap_close_unwatch(E_D_DAP_CLOSING *c)
{
 if (c->fd >= 0)
  wpe_fd_del(c->fd);
 if (c->pty >= 0)
  wpe_fd_del(c->pty);
 c->fd = c->pty = E_D_DR_NO_FD;
}

/**
 * e_d_dap_close_disarm - Stop the closing adapter's deadline.
 * @c: the closing adapter.
 */
static void e_d_dap_close_disarm(E_D_DAP_CLOSING *c)
{
 if (c->timer == WPE_TIMER_NONE)
  return;
 wpe_timer_del(c->timer);
 c->timer = WPE_TIMER_NONE;
}

static void e_d_dap_close_on_timer(void *data);

/**
 * e_d_dap_close_arm - Give the closing adapter's phase its deadline.
 * @c: the closing adapter; @ms: the deadline.
 */
static void e_d_dap_close_arm(E_D_DAP_CLOSING *c, long ms)
{
 e_d_dap_close_disarm(c);
 c->timer = wpe_timer_add(ms, 0, e_d_dap_close_on_timer, c);
 if (c->timer <= 0)
  c->timer = WPE_TIMER_NONE;
}

/**
 * e_d_dap_close_done - The adapter is gone: free its session, then close
 * the build server that hosted it.
 * @c: the closing adapter.
 */
static void e_d_dap_close_done(E_D_DAP_CLOSING *c)
{
 e_dap_session *s = c->s;
 e_bsp_session *bsp = c->bsp;

 e_d_dap_close_unwatch(c);
 e_d_dap_close_disarm(c);
 memset(c, 0, sizeof(*c));
 c->phase = E_D_DAP_CLOSE_FREE;
 e_dap_free(s);
 if (bsp)
  e_bsp_close(bsp);
}

/**
 * e_d_dap_close_gone - Has the closing adapter exited (and been reaped)?
 * @c: the closing adapter.
 *
 * Read from what the SIGCHLD handler kept, so it is right the moment the
 * adapter is reaped.  One the editor did not spawn (Scala's, hosted by the
 * build server) has nothing to exit.
 */
static int e_d_dap_close_gone(E_D_DAP_CLOSING *c)
{
 if (e_dap_adapter_pid(c->s) <= 0)
  return(1);
 return(e_dap_adapter_gone(c->s));
}

/**
 * e_d_dap_close_end - Hand the adapter to the owner's ladder: it ends
 * without the editor waiting, and Quit is done when its end is reported.
 * @c: the closing adapter; @polite_sig: E_D_DAP_END_ASKED once
 * `disconnect` went out, E_D_DAP_END_NOT_ASKED when it could not.
 */
static void e_d_dap_close_end(E_D_DAP_CLOSING *c, int polite_sig)
{
 e_d_dap_close_disarm(c);
 if (e_d_dap_close_gone(c))
 {
  e_d_dap_close_done(c);
  return;
 }
 e_dap_hang_up(c->s);
 c->phase = E_D_DAP_CLOSE_ENDING;
 e_dap_end_adapter(c->s, polite_sig);
}

/**
 * e_d_dap_close_say_late - Tell the user the adapter did not answer Quit
 * in time and is being ended by signal.
 * @c: the closing adapter.
 */
static void e_d_dap_close_say_late(E_D_DAP_CLOSING *c)
{
 char line[E_D_DAP_CLOSE_LINE_MAX];
 ECNT *cn = WpeEditor;

 if (!cn || cn->mxedt < 0)
  return;
 snprintf(line, sizeof line, E_D_DAP_CLOSE_LATE_FMT, c->name,
          E_DAP_DISCONNECT_MS / E_D_DAP_MS_PER_S);
 e_status_say_until_key(cn->f[cn->mxedt], line);
}

static void e_d_dap_close_on_discard(int fd, void *data);

/**
 * e_d_dap_close_answered - The adapter answered `disconnect` (or its
 * transport ended): the program is ended; the owner gives the adapter its
 * grace to exit by itself before SIGKILL.
 * @c: the closing adapter.
 *
 * What it still writes is read and dropped, so it never blocks on a full
 * pipe while it exits.
 */
static void e_d_dap_close_answered(E_D_DAP_CLOSING *c)
{
 e_dap_hang_up(c->s);
 if (c->fd >= 0 && e_dap_fd(c->s) < 0)
 {
  wpe_fd_del(c->fd);
  c->fd = E_D_DR_NO_FD;
 }
 if (c->fd >= 0)
  e_fd_watch(c->fd, POLLIN, e_d_dap_close_on_discard, c,
             "the debug adapter's last words");
 e_d_dap_close_end(c, E_D_DAP_END_ASKED);
}

/**
 * e_d_dap_close_on_answer - A closing adapter wrote: look for its answer
 * to `disconnect`.
 * @fd: its transport; @data: the closing adapter.
 */
static void e_d_dap_close_on_answer(int fd, void *data)
{
 E_D_DAP_CLOSING *c = data;

 (void)fd;
 if (e_dap_run_pump(c->s) == E_DAP_RUN_ENDED)
  e_d_dap_close_answered(c);
}

/**
 * e_d_dap_close_on_discard - A closing adapter that answered wrote more:
 * drop it.
 * @fd: its transport; @data: the closing adapter.
 */
static void e_d_dap_close_on_discard(int fd, void *data)
{
 E_D_DAP_CLOSING *c = data;

 e_dap_discard_input(c->s);
 if (e_dap_fd(c->s) >= 0)
  return;
 wpe_fd_del(fd);
 c->fd = E_D_DR_NO_FD;
}

/**
 * e_d_dap_close_on_output - dlv's pty has output while dlv ends: drop it,
 * so neither dlv nor its program stalls on a full pty.
 * @fd: the pty; @data: the closing adapter.
 */
static void e_d_dap_close_on_output(int fd, void *data)
{
 E_D_DAP_CLOSING *c = data;

 e_dap_drain_output(c->s);
 if (e_dap_output_fd(c->s) == fd)
  return;
 wpe_fd_del(fd);
 c->pty = E_D_DR_NO_FD;
}

/**
 * e_d_dap_close_on_timer - The adapter did not answer `disconnect` in time:
 * said on the status row, then the owner's ladder ends it (it was asked
 * already: the grace, then SIGKILL).
 * @data: the closing adapter.
 */
static void e_d_dap_close_on_timer(void *data)
{
 E_D_DAP_CLOSING *c = data;

 c->timer = WPE_TIMER_NONE;
 if (c->phase != E_D_DAP_CLOSE_ANSWER)
  return;
 e_d_dap_close_say_late(c);
 e_d_dap_close_end(c, E_D_DAP_END_ASKED);
}

/**
 * e_d_dap_closing_of - The place Quit ends @s from, or NULL.
 * @s: a session.
 */
static E_D_DAP_CLOSING *e_d_dap_closing_of(const e_dap_session *s)
{
 int i;

 for (i = 0; i < E_D_DAP_CLOSE_SLOTS; i++)
  if (e_d_dap_closing[i].phase != E_D_DAP_CLOSE_FREE &&
      e_d_dap_closing[i].s == s)
   return(&e_d_dap_closing[i]);
 return(NULL);
}

/**
 * e_d_dap_adapter_ended - The owner of the editor's children reported that
 * an adapter ended (or that it did not end even after SIGKILL).
 * @s: its session; @data: unused.
 *
 * One that Quit is ending is done.  One that ends during its session (it
 * crashed, it was killed, or it ended with its program) keeps how it ended
 * in its session (e_dap_adapter_end_status) for the line that ends the
 * session, which the end of its transport brings; a start still waiting
 * for it sees it gone on its next tick.
 */
static void e_d_dap_adapter_ended(e_dap_session *s, void *data)
{
 E_D_DAP_CLOSING *c = e_d_dap_closing_of(s);

 (void)data;
 if (c)
  e_d_dap_close_done(c);
}

/**
 * e_d_dap_close_slot - A free place for an adapter Quit is ending, or NULL.
 */
static E_D_DAP_CLOSING *e_d_dap_close_slot(void)
{
 int i;

 for (i = 0; i < E_D_DAP_CLOSE_SLOTS; i++)
  if (e_d_dap_closing[i].phase == E_D_DAP_CLOSE_FREE)
   return(&e_d_dap_closing[i]);
 return(NULL);
}

/**
 * e_d_dap_close_await_answer - `disconnect` went out: read the adapter for
 * its answer, within E_DAP_DISCONNECT_MS.
 * @c: the closing adapter.
 */
static void e_d_dap_close_await_answer(E_D_DAP_CLOSING *c)
{
 int fd = e_dap_fd(c->s), pty = e_dap_output_fd(c->s);

 c->phase = E_D_DAP_CLOSE_ANSWER;
 if (fd >= 0 && e_fd_watch(fd, POLLIN, e_d_dap_close_on_answer, c,
                           "the debug adapter's answer to Quit") == 0)
  c->fd = fd;
 if (pty >= 0 && e_fd_watch(pty, POLLIN, e_d_dap_close_on_output, c,
                            "the program's output") == 0)
  c->pty = pty;
 e_d_dap_close_arm(c, E_DAP_DISCONNECT_MS);
}

/**
 * e_d_dap_close_now - No place to end the adapter in the background (four
 * are being ended already): end it by SIGKILL and free the session.  The
 * SIGCHLD handler reaps it.
 * @s: the session; @bsp: its build server, or NULL.
 */
static void e_d_dap_close_end_program(e_dap_session *s, int polite_sig);

static void e_d_dap_close_now(e_dap_session *s, e_bsp_session *bsp)
{
 e_d_dap_close_end_program(s, SIGTERM);
 e_dap_signal(s, SIGKILL);
 e_dap_free(s);   /* the owner stops following it; the handler reaps it */
 if (bsp)
  e_bsp_close(bsp);
}

/**
 * e_d_dap_close_end_program - Quit: make sure the program the adapter
 * named ends with the session.
 * @s: the session; @polite_sig: WPE_CHILD_NO_SIGNAL once `disconnect`
 * asked the adapter to end it, SIGTERM when it could not be asked.
 *
 * An adapter that answers Quit and leaves the program running would leave
 * the user's program behind the session, and behind the editor: the one it
 * named in its `process` event gets the owner's grace, then SIGKILL, that
 * process alone.  An adapter that names none (Scala's, hosted by the build
 * server) is trusted to end its program, which it does on `disconnect`.
 */
static void e_d_dap_close_end_program(e_dap_session *s, int polite_sig)
{
 wpe_child_end_named(e_dap_program_pid(s), polite_sig);
}

/**
 * e_d_dap_close_begin - Quit: end the adapter and its program without
 * holding the editor.
 * @s: the session, given up by the editor; @bsp: the build server that
 * hosts the adapter, or NULL.
 *
 * Nothing the adapter says reaches the windows any more.  An adapter that
 * never answered initialize is not spoken to: it is ended by signal.
 */
static void e_d_dap_close_begin(e_dap_session *s, e_bsp_session *bsp)
{
 E_D_DAP_CLOSING *c = e_d_dap_close_slot();

 e_dap_host_forget(s);
 if (!c)
 {
  e_d_dap_close_now(s, bsp);
  return;
 }
 memset(c, 0, sizeof(*c));
 c->s = s;
 c->bsp = bsp;
 c->timer = WPE_TIMER_NONE;
 c->fd = c->pty = E_D_DR_NO_FD;
 snprintf(c->name, sizeof c->name, "%s", g_dap_adapter_name);
 if (e_dap_disconnect(s) == 0)
 {
  e_d_dap_close_end_program(s, WPE_CHILD_NO_SIGNAL);
  e_d_dap_close_await_answer(c);
  return;
 }
 e_d_dap_close_end_program(s, SIGTERM);
 e_d_dap_close_end(c, E_D_DAP_END_NOT_ASKED);
}

/**
 * e_d_dap_all_closed - Has every adapter Quit ended in the background gone?
 *
 * The editor's end keeps its loop running until it has, so a logout during
 * a debug session leaves no adapter and no program behind.
 */
int e_d_dap_all_closed(void)
{
 int i;

 for (i = 0; i < E_D_DAP_CLOSE_SLOTS; i++)
  if (e_d_dap_closing[i].phase != E_D_DAP_CLOSE_FREE)
   return(0);
 return(1);
}

/* ---- the debugger keys typed while gdb answers ---- */

static void e_d_act_on_timer(void *data);   /* (below) */

/**
 * e_d_act_schedule - Run the next kept key once gdb is done, from the
 * editor's loop rather than from inside an answer.
 */
static void e_d_act_schedule(void)
{
 if (e_d_ga.nact == 0 || e_d_ga.act_timer != E_D_GA_NO_TIMER)
  return;
 e_d_ga.act_timer = wpe_timer_add(0, 0, e_d_act_on_timer, NULL);
 if (e_d_ga.act_timer <= 0)
  e_d_ga.act_timer = E_D_GA_NO_TIMER;
}

/**
 * e_d_act_push - Keep a debugger key typed while gdb answers.
 * @kind: E_D_ACT_*; @f: the window it was pressed in; @line: Go to cursor's
 * line there.
 */
static void e_d_act_push(int kind, FENSTER *f, int line)
{
 E_D_ACT *a = &e_d_ga.act[e_d_ga.nact++];

 a->kind = kind;
 a->line = line;
 snprintf(a->file, sizeof a->file, "%s", f->datnam ? f->datnam : "");
 snprintf(a->dirct, sizeof a->dirct, "%s", f->dirct ? f->dirct : "");
}

/**
 * e_d_act_pop - Take the oldest kept key off the list.
 * @a: receives it.
 */
static void e_d_act_pop(E_D_ACT *a)
{
 int i;

 *a = e_d_ga.act[0];
 for (i = 1; i < e_d_ga.nact; i++)
  e_d_ga.act[i - 1] = e_d_ga.act[i];
 e_d_ga.nact--;
}

/**
 * e_d_act_deferred - Keep a step, a run, Finish or Go to cursor typed while
 * gdb answers at a stop, to run once it is done.
 * @kind: E_D_ACT_*; @f: the window the key was pressed in; @line: Go to
 * cursor's line.
 *
 * F8 F8 typed while the watches of a stop are read steps twice, in order,
 * as the user meant.  While gdb is told of a breakpoint change the program
 * runs for the user, and the key is refused as while it runs, never kept.
 * A ninth key, or one typed while gdb still answers a command given up on,
 * is refused with the line that says so.
 * Return: 1 when the key was kept or refused here.
 */
static int e_d_act_deferred(int kind, FENSTER *f, int line)
{
 if (!e_d_ga_answering())
  return(0);
 if (e_d_ga.chain == E_D_GA_SYNC || e_d_ga.rq.resync ||
     e_d_ga.nact >= E_D_GA_ACT_MAX)
  return(e_d_ga_refused(f));
 e_d_act_push(kind, f, line);
 return(1);
}

/**
 * e_d_act_goto - Go to cursor, kept while gdb answered, on its own line.
 * @f: the window on top; @a: the kept key.
 */
static int e_d_act_goto(FENSTER *f, const E_D_ACT *a)
{
 snprintf(e_d_goto_target.file, sizeof e_d_goto_target.file, "%s", a->file);
 snprintf(e_d_goto_target.dirct, sizeof e_d_goto_target.dirct, "%s",
          a->dirct);
 return(e_d_goto_run(f, E_D_GOTO_CURSOR, a->line));
}

/**
 * e_d_act_run - Do what a kept key asks, now that gdb is done.
 * @a: the kept key.
 */
static void e_d_act_run(const E_D_ACT *a)
{
 FENSTER *f = e_d_ga_window();

 switch (a->kind)
 {
  case E_D_ACT_STEP:
   e_d_step_next(f, 0);
   break;
  case E_D_ACT_NEXT:
   e_d_step_next(f, 1);
   break;
  case E_D_ACT_RUN:
   e_deb_run(f);
   break;
  case E_D_ACT_FINISH:
   e_d_goto_run(f, E_D_GOTO_FINISH, 0);
   break;
  case E_D_ACT_GOTO:
   e_d_act_goto(f, a);
   break;
 }
}

/**
 * e_d_act_on_timer - The loop is free: run the oldest kept key.
 * @data: unused.
 *
 * Only once gdb is done and the program is stopped; a key that started a
 * run leaves the next one for after that run's stop.
 */
static void e_d_act_on_timer(void *data)
{
 E_D_ACT a;

 (void)data;
 e_d_ga.act_timer = E_D_GA_NO_TIMER;
 if ((!e_d_ga.live && !e_d_dap_active()) || !e_d_session_runs())
 {
  e_d_ga.nact = 0;
  return;
 }
 if (e_d_ga.nact == 0 || e_d_ga_answering() || e_d_run_going())
  return;
 e_d_act_pop(&a);
 e_d_act_run(&a);
 if (!e_d_run_going() && !e_d_ga_answering())
  e_d_act_schedule();
}

/* ---- the answers of a stop ---- */

/* What a chain's steps learn, read by its end: a stop's, a sync's or a
   command's at a stop, under gdb, pdb, jdb or a68g (one chain runs at a
   time). */
struct e_d_ga_ctx
{
 ECNT *cn;
 int need_where;             /* the stop's own lines named no place */
 int need_file;              /* they named a line but not its file (sdb) */
 int line;                   /* the line to mark, or -1 (gdb: say none) */
 char file[E_D_LINE_MAX];
 int where_rows;             /* rows `where` answered */
 int no_stack;               /* `where` said the program has no stack */
 int pause_frame;            /* after Pause, the frame to select, or 0 */
 int stack_wanted;           /* the Stack window is open: ask its rows */
 char *stack;                /* gdb's `bt` answer */
 int stack_sw;               /* bring the Stack window to the top */
 int watch;                  /* the next watch to ask */
 char **values;              /* the watches' answers */
 int nvalues;
 int watches_sw;             /* bring the Watches window to the top */
 int start_brk;              /* the start breakpoint (entry + 1) to delete */
 int frame;                  /* the frame to move to (Stack window) */
 int moved;                  /* that move was asked */
 int below;                  /* the frame to finish out of after Pause */
 int step_over;              /* the step after that finish: 1 for F8 */
 int pause_how;              /* pdb's Pause: E_D_PDB_PAUSE_* */
 int up;                     /* pdb: frames up to the script's, or 0 */
 int leave;                  /* pdb's sync: "return" out of its own code */
 char *stop;                 /* jdb: the stop line it reported, or NULL */
 char *threads;              /* jdb's Pause: its "threads" answer */
 size_t thread_pos;          /* ... read up to here */
 char thread[E_D_JDB_THREAD_ID_MAX];   /* ... the thread looked at */
 int user_frame;             /* ... its innermost frame of the user's code,
                                or -1 while none is found */
 int mi_frames;              /* gdb over MI: the Stack window's frames are
                                read, its arguments come next */
};

static E_D_GA_CTX e_d_gs;

/**
 * e_d_gs_free - Give back what a stop's steps kept.
 * @st: the context.
 */
static void e_d_gs_free(E_D_GA_CTX *st)
{
 int i;

 for (i = 0; i < st->nvalues; i++)
  if (st->values[i])
   FREE(st->values[i]);
 if (st->values)
  FREE(st->values);
 if (st->stack)
  FREE(st->stack);
 if (st->stop)
  FREE(st->stop);
 if (st->threads)
  FREE(st->threads);
 memset(st, 0, sizeof *st);
}

/**
 * e_d_gs_reset - Start a new context: one sequence of requests at a time.
 * @st: the context; @cn: the editor.
 */
static void e_d_gs_reset(E_D_GA_CTX *st, ECNT *cn)
{
 e_d_gs_free(st);
 st->cn = cn;
 st->line = -1;
 if (e_d_nwtchs > 0)
  st->values = calloc((size_t)e_d_nwtchs, sizeof(char *));
 if (st->values)
  st->nvalues = e_d_nwtchs;
}

/**
 * e_d_gs_copy - Keep a copy of an answer's text, or NULL when it has none.
 */
static char *e_d_gs_copy(const E_D_ANSWER *ans)
{
 if (!ans->text)
  return(NULL);
 return(WpeStrdup(ans->text));
}

/* step: take back the answers to interrupts that met the debugger at its
   prompt (gdb's "Quit", pdb's "--KeyboardInterrupt--", each with one more
   prompt) */

/**
 * e_d_gs_settle_next_marker - Choose the marker the settle command about to
 * go out makes the debugger echo.
 *
 * A debugger whose interrupt may take the settle command away is asked
 * again after such an interrupt; each command then has a marker of its own,
 * numbered, so the echo of one that was only late is not taken for the
 * echo of the last, whose answer would then be left to the next command.
 * Any other debugger is asked once, with its plain marker.
 */
static void e_d_gs_settle_next_marker(void)
{
 const E_D_GA_BACKEND *be = e_d_ga.be;

 if (!be->settle_cmd_for)
 {
  snprintf(e_d_ga.marker, sizeof e_d_ga.marker, "%s", be->settle_marker);
  return;
 }
 e_d_gs_numbered_marker(e_d_ga.marker, sizeof e_d_ga.marker);
}

/**
 * e_d_gs_settle_forget - The settle is over, or the session's engine starts
 * afresh: the next settle starts with its first command.
 */
static void e_d_gs_settle_forget(void)
{
 e_d_ga.marker_wanted = 0;
 e_d_ga.settle_seq = 0;
 e_d_ga.settle_again = 0;
}

static int e_d_gs_settle_cmd(char *cmd, size_t n, void *ctx)
{
 (void)ctx;
 if (e_d_ga.owed <= 0 && !e_d_ga.settle_again)
  return(0);
 e_d_ga.settle_again = 0;
 e_d_gs_settle_next_marker();
 e_d_ga.marker_wanted = 1;
 if (e_d_ga.be->settle_cmd_for)
  e_d_ga.be->settle_cmd_for(e_d_ga.marker, cmd, n);
 else
  snprintf(cmd, n, "%s", e_d_ga.be->settle_cmd);
 return(1);
}

/**
 * e_d_gs_interrupts_answered - How many interrupts the settle command's
 * answer shows the debugger answered at its prompt.
 * @ans: the answer.
 *
 * Counted by the backend's words for such an answer when it has them (an
 * echo of an earlier settle command that was only late also ends in a
 * prompt, and is no interrupt's), else by the prompts before the marker.
 */
static int e_d_gs_interrupts_answered(const E_D_ANSWER *ans)
{
 const char *mark;

 if (!ans->text)
  return(0);
 if (e_d_ga.be->int_answer)
  return(e_d_words_in(ans->text, ans->len, e_d_ga.be->int_answer));
 mark = e_d_ga_marker_in(ans->text);
 if (!mark)
  return(0);
 return(e_d_prompts_in(ans->text, (size_t)(mark - ans->text)));
}

/**
 * e_d_gs_still_owed - The interrupts still owed after a settle command's
 * answer, so the next resume settles only for interrupts that can still
 * come back.
 * @owed: those owed before it, less the ones it answered; @marker_back:
 * the answer holds its marker.
 *
 * Once the marker is back, the rest of the interrupts reached the program
 * and come back as a stop of their own at its next resume (gdb's inferior);
 * for a debugger that takes the signal in its own process (pdb), none is
 * left.
 */
static int e_d_gs_still_owed(int owed, int marker_back)
{
 if (owed < 0)
  return(0);
 if (marker_back && e_d_ga.be->int_answer)
  return(0);
 return(owed);
}

/**
 * e_d_gs_settle_ask_again - An interrupt took the settle command away: run
 * the settle step once more, with a new marker, whatever is still owed.
 * @owed: the interrupts still owed.
 * Return: E_D_STEP_AGAIN.
 */
static int e_d_gs_settle_ask_again(int owed)
{
 e_d_ga.owed = owed;
 e_d_ga.settle_again = 1;
 return(E_D_STEP_AGAIN);
}

/**
 * e_d_gs_settle_answered - Count the interrupts the debugger answered at
 * its prompt, and ask again when one of them took the settle command away,
 * so the stop is shown and its watches read their own answers instead of
 * the editor waiting for an echo that never comes.
 */
static int e_d_gs_settle_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 int answered = e_d_gs_interrupts_answered(ans);
 int marker_back = e_d_ga_marker_in(ans->text) != NULL;
 int owed = e_d_gs_still_owed(e_d_ga.owed - answered, marker_back);

 (void)how;
 (void)ctx;
 e_d_ga.marker_wanted = 0;
 if (answered > 0)
  e_d_run_own_sigint_disproved();
 e_d_run.int_owed = owed;
 if (!marker_back)
  return(e_d_gs_settle_ask_again(owed));
 e_d_ga.owed = 0;
 e_d_gs_settle_forget();
 return(E_D_STEP_NEXT);
}

/* step: the program's buffered output, which a pty holds until a line end */

static int e_d_gs_flush_cmd(char *cmd, size_t n, void *ctx)
{
 (void)ctx;
 if (e_d_pty_master < 0)
  return(0);
 snprintf(cmd, n, "%s", E_D_GDB_FLUSH);
 return(1);
}

static int e_d_gs_ignore_answer(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 (void)how;
 (void)ans;
 (void)ctx;
 return(E_D_STEP_NEXT);
}

/* step: where the program stopped, when its stop lines did not say */

static int e_d_gs_where_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 if (!st->need_where)
  return(0);
 snprintf(cmd, n, "%s", E_D_GDB_WHERE);
 return(1);
}

/**
 * e_d_gs_where_row - Read one row of `where`.
 * @st: the context; @row: the row.
 * Return: 1 when the reading is over (the place is found, or there is no
 * stack), 0 to read the next row.
 */
static int e_d_gs_where_row(E_D_GA_CTX *st, char *row)
{
 char file[E_D_LINE_MAX];
 int line;

 st->where_rows++;
 if (!strncmp(row, e_d_msg[ERR_NOSTACK], E_D_GDB_NOSTACK_LEAD))
 {
  st->no_stack = 1;
  return(1);
 }
 if ((line = e_d_where_row_line(row, file)) < 0)
  return(0);
 st->line = line;
 snprintf(st->file, sizeof st->file, "%s", file);
 return(1);
}

/**
 * e_d_gs_where_answered - Find the frame to mark in `where`'s rows: the
 * innermost one with a line, or after Pause the innermost one whose source
 * is here, whose frame is then selected so the watches read its locals.
 */
static int e_d_gs_where_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 E_D_GA_CTX *st = ctx;
 const char *p = ans->text ? ans->text : "";
 char *row;
 int over = 0;

 (void)how;
 e_d_where_frame = 0;
 e_ds_begin_stack(&e_d_session);
 while (!over && *p && (row = e_d_text_take_row(&p)) != NULL)
 {
  over = e_d_gs_where_row(st, row);
  FREE(row);
 }
 if (st->no_stack)
  return(E_D_STEP_STOP);
 if (st->line >= 0 && e_d_run.pause != E_D_PAUSE_NONE && e_d_where_frame > 0)
  st->pause_frame = e_d_where_frame;
 return(E_D_STEP_NEXT);
}

/* step: after Pause, the user's frame becomes gdb's selected one.  gdb
   stops in the innermost frame, often library code, while the mark is on
   the user's frame further out: the watches then read the user's locals,
   and the next F8 or F7 first finishes out of the library call
   (e_d_gdb_step_from_pause). */

static int e_d_gs_frame_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 if (st->pause_frame <= 0)
  return(0);
 snprintf(cmd, n, E_D_GDB_FRAME_SELECT_FMT, st->pause_frame);
 return(1);
}

static int e_d_gs_frame_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 (void)how;
 (void)ans;
 e_d_nstack = st->pause_frame;
 e_d_pause_below = st->pause_frame - 1;
 return(E_D_STEP_NEXT);
}

/* step: the Stack window's rows, when it is open */

static int e_d_gs_stack_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 if (!st->stack_wanted)
  return(0);
 snprintf(cmd, n, "%s", E_D_GDB_BT);
 return(1);
}

static int e_d_gs_stack_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 (void)how;
 st->stack = e_d_gs_copy(ans);
 return(E_D_STEP_NEXT);
}

/* step: one watch; again until every watch has its answer */

static int e_d_gs_watch_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_GA_CTX *st = ctx;
 char line[E_D_WATCH_CMD_MAX];
 size_t len;

 if (st->watch >= st->nvalues)
  return(0);
 if (!e_d_watch_command(line, sizeof line, e_d_swtchs[st->watch]))
  return(0);
 len = strlen(line);
 if (len > 0 && line[len - 1] == '\n')
  line[len - 1] = '\0';
 snprintf(cmd, n, "%s", line);
 return(1);
}

/**
 * e_d_gs_watch_value - The value an answer gives a watch: its lines joined
 * by blanks, as they were always shown; none when gdb answered on its error
 * stream ("No symbol ..."), whose words the watch replaces with its own.
 * Return: the value, to be freed, or NULL.
 */
static char *e_d_gs_watch_value(const E_D_ANSWER *ans)
{
 char *value;
 size_t i, len;

 if (ans->error || !ans->text)
  return(NULL);
 value = WpeStrdup(ans->text);
 if (!value)
  return(NULL);
 len = strlen(value);
 for (i = 0; i + 1 < len; i++)
  if (value[i] == '\n')
   value[i] = ' ';
 return(value);
}

static int e_d_gs_watch_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 (void)how;
 st->values[st->watch++] = e_d_gs_watch_value(ans);
 return(E_D_STEP_AGAIN);
}

/* ---- breakpoint answers ---- */

/* The breakpoint whose refusal may still come on gdb's error stream after
   its prompt (the engine's late_error hook): heard until the next request
   goes out. */
static struct
{
 char file[PATH_MAX];
 int line;
 int asked;                  /* "b file:line" went out and is not answered */
} e_d_gb_late;

/**
 * e_d_gb_number_line - The line of gdb's answer to "b file:line" that
 * gives the breakpoint's number, copied into @out.
 * Return: the number, or 0 when no line gives one.
 */
static int e_d_gb_number_line(const char *text, char *out, size_t n)
{
 const char *p = text ? text : "";
 char *row;
 int id = 0;

 out[0] = '\0';
 while (!id && *p && (row = e_d_text_take_row(&p)) != NULL)
 {
  if (e_ds_brk_is_number_line(E_DS_GDB, row))
  {
   id = e_ds_brk_number(E_DS_GDB, row);
   snprintf(out, n, "%s", row);
  }
  FREE(row);
 }
 return(id);
}

/**
 * e_d_gb_first_words - The first line of an answer that says something,
 * for the status row when gdb refused.
 */
static void e_d_gb_first_words(const char *text, char *out, size_t n)
{
 const char *p = text ? text : "";
 char *row;

 out[0] = '\0';
 while (!out[0] && *p && (row = e_d_text_take_row(&p)) != NULL)
 {
  if (row[0] && row[0] != '\n')
   snprintf(out, n, "%s", row);
  FREE(row);
 }
}

/**
 * e_d_gb_take_verdict - Keep what gdb said of list entry @i.
 * @i: the entry; @id: the number gdb gave it, or 0; @ret: the reader's code
 * for @str as the session reads it (0 a line, 1 the prompt, 2 an error
 * line); @str: the words.
 */
static void e_d_gb_take_verdict(int i, int id, int ret, const char *str)
{
 E_D_BRK_ANSWER v;

 e_d_nrbrpts[i] = id;
 e_d_brk_numbered_verdict(&v, ret, str);
 e_d_brk_take_answer(i, &v);
 e_d_brk_session_answer(e_d_sbrpts[i], e_d_ybrpts[i], ret, str);
}

/**
 * e_d_gb_on_late_error - gdb refused the breakpoint just set on its error
 * stream, after its prompt: the mark turns hollow with gdb's words.
 * @err: the words; @ctx: unused (the entry is e_d_gb_late).
 */
static void e_d_gb_on_late_error(const E_D_ANSWER *err, void *ctx)
{
 char words[E_D_BRK_ANSWER_MAX];
 int i = e_d_brk_index_of(e_d_gb_late.file, e_d_gb_late.line);

 (void)ctx;
 if (i < 0 || e_d_nrbrpts[i] != 0)
  return;
 e_d_gb_first_words(err->text, words, sizeof words);
 e_d_gb_take_verdict(i, 0, E_D_READ_ERROR, words);
 e_d_brk_say_refusal(e_d_ga_window(), i);
 e_d_run_repaint_margin(e_d_ga.cn);
}

/* step: one breakpoint the user removed; again until none is left */

/**
 * e_d_gb_removal_told - Is gdb answering the removal of breakpoint @id?
 */
static int e_d_gb_removal_told(int id)
{
 return(id != 0 && id == e_d_gb_removing_id);
}

static int e_d_gb_remove_cmd(char *cmd, size_t n, void *ctx)
{
 (void)ctx;
 if (e_d_brk_ngone <= 0)
  return(0);
 e_d_gb_removing_id = e_d_brk_gone[0].id;
 e_d_ga.be->brk_remove_cmd(&e_d_brk_gone[0], cmd, n);
 return(1);
}

/**
 * e_d_gdb_brk_remove_cmd - gdb's command that takes out a breakpoint the
 * list no longer has: "d N" by the number gdb gave it, or "clear file:line"
 * for one whose number was never read.
 * @g: the removal; @cmd, @n: the command.
 */
static void e_d_gdb_brk_remove_cmd(const E_D_BRK_GONE *g, char *cmd, size_t n)
{
 if (g->id == E_D_GB_ID_UNKNOWN)
  snprintf(cmd, n, E_D_GDB_CLEAR_FMT, g->file, g->line);
 else
  snprintf(cmd, n, E_D_GDB_DELETE_FMT, g->id);
}

/**
 * e_d_gb_gone_drop_id - gdb took the removal of breakpoint @id: forget it,
 * so the removals the user makes meanwhile are still told.
 */
static void e_d_gb_gone_drop_id(int id)
{
 int i;

 for (i = 0; i < e_d_brk_ngone && e_d_brk_gone[i].id != id; i++)
  ;
 if (i == e_d_brk_ngone)
  return;
 FREE(e_d_brk_gone[i].file);
 for (i++; i < e_d_brk_ngone; i++)
  e_d_brk_gone[i - 1] = e_d_brk_gone[i];
 e_d_brk_ngone--;
 if (e_d_brk_ngone == 0)
  e_d_brk_gone_free();
}

static int e_d_gb_remove_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 (void)how;
 (void)ans;
 (void)ctx;
 e_d_gb_gone_drop_id(e_d_gb_removing_id);
 e_d_gb_removing_id = 0;
 return(E_D_STEP_AGAIN);
}

/**
 * e_d_gb_gone_keep - gdb set a breakpoint the user took off again while
 * gdb answered: keep it as a removal, so gdb is told at the next chance, as
 * for any breakpoint taken off while the program runs, and does not go on
 * stopping on a line the margin no longer marks.
 * @file, @line: where gdb set it; @id: the number gdb gave it.
 */
static void e_d_gb_gone_keep(const char *file, int line, int id)
{
 E_D_BRK_GONE *rec;
 E_D_BRK_GONE *grown = REALLOC(e_d_brk_gone,
                               sizeof(E_D_BRK_GONE) * (e_d_brk_ngone + 1));

 if (!grown)
  return;
 e_d_brk_gone = grown;
 rec = &e_d_brk_gone[e_d_brk_ngone];
 rec->file = WpeStrdup(file);
 if (!rec->file)
  return;
 e_d_brk_ngone++;
 rec->line = line;
 rec->id = id;
 rec->state = E_D_BRK_VERIFIED;
 rec->msg[0] = '\0';
}

/* step: one breakpoint the user set; again until none is left unsent */

/**
 * e_d_gb_next_unsent - The first list entry gdb has not been told of.
 * Return: its index, or -1.
 */
static int e_d_gb_next_unsent(void)
{
 int i;

 for (i = 0; i < e_d_nbrpts; i++)
  if (e_d_brk_unsent(i))
   return(i);
 return(-1);
}

static int e_d_gb_add_cmd(char *cmd, size_t n, void *ctx)
{
 int i = e_d_gb_next_unsent();

 (void)ctx;
 if (i < 0)
  return(0);
 snprintf(e_d_gb_late.file, sizeof e_d_gb_late.file, "%s", e_d_sbrpts[i]);
 e_d_gb_late.line = e_d_ybrpts[i];
 e_d_gb_late.asked = 1;
 e_d_ga.be->brk_add_cmd(i, cmd, n);
 return(1);
}

/**
 * e_d_gdb_brk_add_cmd - gdb's "b file:line" for list entry @i.
 * @cmd, @n: the command.
 */
static void e_d_gdb_brk_add_cmd(int i, char *cmd, size_t n)
{
 snprintf(cmd, n, E_D_GDB_BREAK_FMT, e_d_sbrpts[i], e_d_ybrpts[i]);
}

/**
 * e_d_gb_add_given_up - The wait for gdb's answer to "b file:line" was
 * given up on (Esc, Ctrl-C or the deadline): gdb may still have set the
 * breakpoint, under a number the editor never read.
 *
 * It is kept as a removal by place, so the next request gdb takes clears
 * that line first.  Without it the line, set again later, would be in gdb
 * twice, and taking its mark off would leave the program still stopping
 * there.
 */
static void e_d_gb_add_given_up(void)
{
 if (!e_d_gb_late.asked)
  return;
 e_d_gb_late.asked = 0;
 if (e_d_ga.live)
  e_d_gb_gone_keep(e_d_gb_late.file, e_d_gb_late.line, E_D_GB_ID_UNKNOWN);
}

/**
 * e_d_gdb_brk_set_id - The number gdb gave the breakpoint its answer to
 * "b file:line" names, or 0 when it gave none (it refused).
 */
static int e_d_gdb_brk_set_id(const E_D_ANSWER *ans)
{
 char str[E_D_BRK_ANSWER_MAX];

 return(e_d_gb_number_line(ans->text, str, sizeof str));
}

/**
 * e_d_gdb_brk_verdict - Keep with list entry @i what gdb answered: the
 * number @id it gave, or its words when it refused.
 * @ans: the answer.
 */
static void e_d_gdb_brk_verdict(int i, int id, const E_D_ANSWER *ans)
{
 char str[E_D_BRK_ANSWER_MAX];

 if (id > 0)
 {
  e_d_gb_number_line(ans->text, str, sizeof str);
  e_d_gb_take_verdict(i, id, 0, str);
 }
 else if (ans->error)
 {
  e_d_gb_first_words(ans->text, str, sizeof str);
  e_d_gb_take_verdict(i, 0, E_D_READ_ERROR, str);
 }
 else
  e_d_gb_take_verdict(i, 0, E_D_READ_PROMPT, "");
}

/**
 * e_d_gb_add_take - Keep what the debugger answered to the breakpoint just
 * set: the number it gave, or its words when it refused, and listen for a
 * refusal gdb writes just after its prompt.
 * @ans: the answer.
 * Return: the list entry it was for, or -1 when the user took it off
 * meanwhile (the debugger is then told to remove it at the next chance).
 */
static int e_d_gb_add_take(const E_D_ANSWER *ans)
{
 int i = e_d_brk_index_of(e_d_gb_late.file, e_d_gb_late.line);
 int id = e_d_ga.be->brk_set_id(ans);

 e_d_gb_late.asked = 0;
 if (i < 0 && id > 0)
  e_d_gb_gone_keep(e_d_gb_late.file, e_d_gb_late.line, id);
 if (i < 0)
  return(-1);
 e_d_ga.be->brk_verdict(i, id, ans);
 if (e_d_ga.be->reads_errors)
  e_d_req_set_late_error(&e_d_ga.rq, e_d_gb_on_late_error, NULL);
 return(i);
}

/**
 * e_d_gb_add_answered - gdb answered "b file:line": the mark turns solid
 * with the number gdb gave, or stays hollow with gdb's words on the status
 * row.  A refusal gdb writes just after its prompt is heard too.
 */
static int e_d_gb_add_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 int i = e_d_gb_add_take(ans);

 (void)how;
 (void)ctx;
 if (i >= 0)
  e_d_brk_say_refusal(e_d_ga_window(), i);
 return(E_D_STEP_AGAIN);
}

/**
 * e_d_gu_add_answered - As e_d_gb_add_answered, for the breakpoints set as
 * the session starts: only a refusal is news worth the status row.
 */
static int e_d_gu_add_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 int i = e_d_gb_add_take(ans);

 (void)how;
 (void)ctx;
 if (i >= 0)
  e_d_brk_say_if_refused(e_d_ga_window(), i);
 return(E_D_STEP_AGAIN);
}

/* step: the breakpoint the editor set to stop at the program's start */

/**
 * e_d_gb_start_entry_of - The start breakpoint a stop is to take out.
 * @st: the context.
 * Return: its list index, or -1 when there is none (any more).
 */
static int e_d_gb_start_entry_of(const E_D_GA_CTX *st)
{
 int sw = st->start_brk;

 if (sw <= 0 || sw > e_d_nbrpts || !e_d_brk_is_start_entry(sw - 1))
  return(-1);
 return(sw - 1);
}

static int e_d_gb_start_cmd(char *cmd, size_t n, void *ctx)
{
 int i = e_d_gb_start_entry_of(ctx);

 if (i < 0)
  return(0);
 snprintf(cmd, n, E_D_GDB_DELETE_FMT, e_d_nrbrpts[i]);
 return(1);
}

static int e_d_gb_start_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 (void)how;
 (void)ans;
 e_d_brk_list_drop(st->start_brk - 1);
 st->start_brk = 0;
 return(E_D_STEP_NEXT);
}

/* The answers of a stop the user is shown, in the order they were always
   read; then the breakpoints changed while the program ran, and the start
   breakpoint, which the next run must not stop on again. */
static const E_D_STEP e_d_gdb_stop_steps[] =
{
 { e_d_gs_settle_cmd, e_d_gs_settle_answered },
 { e_d_gs_flush_cmd,  e_d_gs_ignore_answer },
 { e_d_gs_where_cmd,  e_d_gs_where_answered },
 { e_d_gs_frame_cmd,  e_d_gs_frame_answered },
 { e_d_gs_stack_cmd,  e_d_gs_stack_answered },
 { e_d_gs_watch_cmd,  e_d_gs_watch_answered },
 { e_d_gb_remove_cmd, e_d_gb_remove_answered },
 { e_d_gb_add_cmd,    e_d_gb_add_answered },
 { e_d_gb_start_cmd,  e_d_gb_start_answered },
 { NULL, NULL }
};

/* A stop made to tell gdb of breakpoint changes while the program runs. */
static const E_D_STEP e_d_gdb_sync_steps[] =
{
 { e_d_gs_settle_cmd, e_d_gs_settle_answered },
 { e_d_gb_remove_cmd, e_d_gb_remove_answered },
 { e_d_gb_add_cmd,    e_d_gb_add_answered },
 { NULL, NULL }
};

/* A breakpoint set or taken off at a stop, under gdb, pdb and jdb alike
   (a68g, which clears every breakpoint at once, has its own). */
static const E_D_STEP e_d_ga_brk_steps[] =
{
 { e_d_gb_remove_cmd, e_d_gb_remove_answered },
 { e_d_gb_add_cmd,    e_d_gb_add_answered },
 { NULL, NULL }
};

/* Add, edit or delete a watch at a stop; Ctrl-G K. */
static const E_D_STEP e_d_gdb_watches_steps[] =
{
 { e_d_gs_stack_cmd,  e_d_gs_stack_answered },
 { e_d_gs_watch_cmd,  e_d_gs_watch_answered },
 { NULL, NULL }
};

/* ---- classifying the stop gdb reported ---- */

/**
 * e_d_gdb_source_row - Is @row gdb's "N<TAB>source" row?
 * Return: N, or 0.
 */
static int e_d_gdb_source_row(const char *row)
{
 int num = atoi(row), k;

 if (num <= 0)
  return(0);
 for (k = 0; row[k] && isdigit((unsigned char)row[k]); k++)
  ;
 if (row[k] != '\t')
  return(0);
 return(num);
}

/**
 * e_d_gdb_row_names_line - Does @row end in ":@line", the "at file:line" of
 * the source row that follows it?
 */
static int e_d_gdb_row_names_line(const char *row, int line)
{
 const char *colon = strrchr(row, ':');

 return(colon && atoi(colon + 1) == line);
}

/**
 * e_d_gdb_place_after - The line of the source row after report line @i,
 * gdb's "N<TAB>..." under its "Breakpoint 1, main () at p.c:N"; the
 * stop's file is taken from the report when it names that line.
 * Return: the line, or 0 when no such row follows.
 */
static int e_d_gdb_place_after(int i)
{
 int j, num = 0;

 for (j = SVLINES - 2; j > i; j--)
  if ((num = e_d_gdb_source_row(e_d_sp[j])) > 0)
   break;
 if (j <= i)
  return(0);
 if (e_d_gdb_row_names_line(e_d_sp[j - 1], num))
  e_d_stop_file_line(e_d_sp[j - 1]);
 return(num);
}

/**
 * e_d_gdb_place_of_step - The line of the newest source row gdb printed
 * for a step or a finish, its file from the row before when it names it.
 * Return: the line, or 0.
 */
static int e_d_gdb_place_of_step(void)
{
 int i, num;

 for (i = SVLINES - 2; i >= 0; i--)
 {
  if ((num = e_d_gdb_source_row(e_d_sp[i])) <= 0)
   continue;
  if (i > 0 && e_d_gdb_row_names_line(e_d_sp[i - 1], num))
   e_d_stop_file_line(e_d_sp[i - 1]);
  return(num);
 }
 return(0);
}

/**
 * e_d_gdb_stop_ended - Did gdb's report say the program ended?  Then say
 * so in Messages and end the session.
 * @f: the window on top.
 * Return: 1 when the session ended.
 */
static int e_d_gdb_stop_ended(FENSTER *f)
{
 int i;

 for (i = 0; i < SVLINES; i++)
  if (e_d_line_says_program_exited(e_d_sp[i]))
  {
   e_d_say_program_exited(e_d_sp[i], f);
   e_d_quit(f);
   return(1);
  }
 for (i = 0; i < SVLINES - 1; i++)
 {
  if (!strncmp(e_d_sp[i], e_d_msg[ERR_PROGEXIT], E_D_GDB_EXIT_LEAD))
  {
   e_d_error(e_d_sp[i]);
   e_d_quit(f);
   return(1);
  }
  if (!strncmp(e_d_sp[i], e_d_msg[ERR_PROGTERM], E_D_GDB_TERM_LEAD))
  {
   e_error(e_d_msg[ERR_PROGTERM], 0, f->fb);
   e_d_quit(f);
   return(1);
  }
 }
 return(0);
}

/**
 * e_d_gdb_stop_place - Read the stop's place from gdb's report, as the
 * report has always been read: a signal is shown, then its place is asked
 * with `where`; a breakpoint or a step names its source row.
 * @st: receives the line, or need_where.
 */
static void e_d_gdb_stop_place(E_D_GA_CTX *st)
{
 int i;

 for (i = 0; i < SVLINES - 1; i++)
 {
  if (!strncmp(e_d_sp[i], e_d_msg[ERR_PROGSIGNAL], E_D_GDB_SIGNAL_LEAD))
  {
   char *said = e_d_pause_signal_line(e_d_sp[i]);

   if (said[0])
    e_d_error(said);
   st->need_where = 1;
   return;
  }
  if (!strncmp(e_d_sp[i], e_d_msg[ERR_BREAKPOINT], E_D_GDB_BREAK_LEAD) &&
      (st->line = e_d_gdb_place_after(i)) > 0)
   break;
 }
 if (st->line <= 0)
  st->line = e_d_gdb_place_of_step();
 if (st->line <= 0)
 {
  st->line = -1;
  st->need_where = 1;
  return;
 }
 snprintf(st->file, sizeof st->file, "%s", e_d_file);
}

/**
 * e_d_gs_stack_window_open - Is the Stack window open, so its rows are
 * asked for at every stop?
 */
static int e_d_gs_stack_window_open(ECNT *cn)
{
 int is;

 for (is = cn->mxedt; is > 0 && strcmp(cn->f[is]->datnam, "Stack"); is--)
  ;
 return(is > 0);
}

/* ---- the end of a stop ---- */

/**
 * e_d_gdb_show_stack - Fill the Stack window with gdb's "bt" answer.
 * @st: the context.
 * Return: 0, or -1 when gdb listed no frame: the program is gone.
 */
static int e_d_gdb_show_stack(E_D_GA_CTX *st)
{
 return(e_d_stack_show_text(st->cn, st->stack, st->stack_sw));
}

/**
 * e_d_gs_show_windows - Fill the Stack and Watches windows with what the
 * debugger answered.
 * @st: the context.
 * Return: 0, or -1 when gdb listed no frame: the program is gone.
 */
static int e_d_gs_show_windows(E_D_GA_CTX *st)
{
 if (st->stack_wanted && e_d_ga.be->show_stack(st) < 0)
  return(-1);
 e_d_watches_show_values(st->cn, st->values, st->watches_sw);
 return(0);
}

/**
 * e_d_gs_say_no_stack - gdb has no stack: the program is gone.  Say so and
 * end the session, as the reader of `where` always did.
 * @st: the context.
 */
static void e_d_gs_say_no_stack(E_D_GA_CTX *st)
{
 FENSTER *f = e_d_ga_window();

 if (st->no_stack)
  e_error(e_d_msg[ERR_PROGEXIT], 0, f->fb);
 else
  e_d_error(e_d_msg[ERR_PROGEXIT]);
 e_d_quit(f);
}

/**
 * e_d_gs_mark - Mark the stop on its source line, or say gdb named none.
 * @file, @line: the stop's place; @line is -1 when gdb named none.
 *
 * Called with the stop's context already given back: a stop in code with
 * no source here asks gdb once more, through a context of its own.
 * Return: 1 when the program runs on (its source is not here, so it was
 * sent on to main), else 0.
 */
static int e_d_gs_mark(char *file, int line)
{
 FENSTER *f = e_d_ga_window();

 if (line < 0)
 {
  e_d_error(e_d_msg[ERR_NOSOURCE]);
  return(0);
 }
 snprintf(e_d_file, sizeof e_d_file, "%s", file);
 e_d_goto_break(file, line, f);
 return(e_d_run_going() || e_d_ga_answering());
}

/**
 * e_d_gs_stop_finished - gdb has answered everything the stop asks: the
 * user sees the stop marked, the windows filled, the breakpoints' words,
 * and how a Pause ended; then the next kept key runs.
 * @ok: 0 when an answer was given up on (the status row already says
 * which): nothing is drawn, and the kept keys are dropped.
 * @ctx: the context.
 */
static void e_d_gs_stop_finished(int ok, void *ctx)
{
 E_D_GA_CTX *st = ctx;
 ECNT *cn = st->cn;
 char file[E_D_LINE_MAX];
 int line = st->line;

 if (!e_d_ga_chain_ended() || !ok)
 {
  e_d_ga_actions_drop();
  e_d_gs_free(st);
  return;
 }
 if (st->no_stack || (st->need_where && st->where_rows == 0) ||
     e_d_gs_show_windows(st) < 0)
 {
  e_d_gs_say_no_stack(st);
  e_d_gs_free(st);
  return;
 }
 snprintf(file, sizeof file, "%s", st->file);
 e_d_gs_free(st);
 if (e_d_gs_mark(file, line))
  return;
 _messages_activated = 0;
 e_refresh();
 e_d_run_stop_tail(cn, e_d_run.ops);
 e_d_act_schedule();
}

/**
 * e_d_gdb_stopped - gdb stopped the program: ask it, on the fd loop, what
 * the stop needs, and show the stop once it has answered.
 * @f: the window on top.
 *
 * The user sees the program's last output at once; the mark, the watches
 * and the stack come when gdb answers, the keys reaching the editor
 * meanwhile.
 */
static void e_d_gdb_stopped(FENSTER *f)
{
 E_D_GA_CTX *st = &e_d_gs;

 if (e_d_gdb_run_never_started(f))
  return;
 if (e_d_gdb_follow_up_after_finish())
 {
  e_d_run.stop_on_loop = 1;        /* the stop the user sees comes later */
  return;
 }
 e_d_drain_pty_to_messages();
 e_d_out_show();
 e_d_switch_out(0);
 if (e_d_gdb_stop_ended(f))
  return;
 e_d_gs_reset(st, f->ed);
 st->start_brk = e_d_run.start_brk;
 e_d_run.start_brk = 0;
 st->stack_wanted = e_d_gs_stack_window_open(f->ed);
 e_d_gdb_stop_place(st);
 e_d_run.stop_on_loop = 1;
 e_d_ga_chain_start(f->ed, e_d_gdb_stop_steps, st, e_d_gs_stop_finished,
                    E_D_GA_STOP);
}

/* ---- a stop made to tell gdb of breakpoint changes ---- */

/**
 * e_d_gs_pause_instead - Pause was pressed while gdb answered at a stop the
 * user was not shown: the program is not let run on, its stop is marked
 * and the status row says "Paused".
 * @cn: the editor.
 */
static void e_d_gs_pause_instead(ECNT *cn)
{
 e_d_async_pending = 0;
 e_d_ga.pause_asked = 0;
 e_d_run.pause = E_D_PAUSE_ASKED;
 e_d_gdb_stopped(e_d_run_window(cn));
}

/**
 * e_d_gs_sync_finished - gdb has the breakpoint changes: the marks show
 * its word, and the program runs on.  A Pause pressed meanwhile keeps the
 * program stopped and shows the stop instead.
 * @ok: 0 when an answer was given up on: the program stays stopped and the
 * status row says which answer did not come; @ctx: the context.
 */
static void e_d_gs_sync_finished(int ok, void *ctx)
{
 E_D_GA_CTX *st = ctx;
 ECNT *cn = st->cn;

 e_d_async_pending = 0;
 e_d_gs_free(st);
 if (!e_d_ga_chain_ended())
  return;
 if (!ok)
 {
  e_refresh_editor_bars(cn);        /* stopped: the stepping keys again */
  return;
 }
 e_d_run_repaint_margin(cn);
 if (e_d_ga.pause_asked)
 {
  e_d_ga.be->pause_instead(cn);
  return;
 }
 e_d_run_go_on(e_d_run.cont);
 if (e_d_brk_changed_while_running())
  e_d_run.sync_pending = 1;
}

/**
 * e_d_ga_sync_with - The interrupt for a breakpoint change stopped the
 * program: tell the debugger with the table of steps @steps on the fd
 * loop, then let it run on (gdb, a68g).
 * @cn: the editor.
 *
 * For the user the program never stopped: the bottom row still names
 * Pause, keys that need the debugger are refused as while it runs, and
 * typed text still goes to a program that has a terminal of its own.
 * Return: 1, the stop is dealt with.
 */
static int e_d_ga_sync_with(ECNT *cn, const E_D_STEP *steps)
{
 E_D_GA_CTX *st = &e_d_gs;

 e_d_gs_reset(st, cn);
 e_d_ga.pause_asked = 0;
 e_d_async_pending = 1;
 e_d_ga_chain_start(cn, steps, st, e_d_gs_sync_finished, E_D_GA_SYNC);
 return(1);
}

/**
 * e_d_gdb_sync_on_loop - The interrupt for a breakpoint change stopped the
 * program: tell gdb on the fd loop, then let it run on.
 * @cn: the editor.
 * Return: 1, the stop is dealt with.
 */
static int e_d_gdb_sync_on_loop(ECNT *cn)
{
 return(e_d_ga_sync_with(cn, e_d_gdb_sync_steps));
}

/* ---- commands of the user's at a stop ---- */

/**
 * e_d_gs_ask_finished - A breakpoint, a Remove all or a frame move has been
 * answered: show the marks, then run the next kept key.
 * @ok: 0 when the answer was given up on; @ctx: the context.
 */
static void e_d_gs_ask_finished(int ok, void *ctx)
{
 E_D_GA_CTX *st = ctx;
 ECNT *cn = st->cn;

 e_d_gs_free(st);
 if (!e_d_ga_chain_ended())
  return;
 if (!ok)
 {
  e_d_ga_actions_drop();
  return;
 }
 e_d_run_repaint_margin(cn);
 e_d_act_schedule();
}

/**
 * e_d_ga_brk_sync - Tell gdb of the breakpoint the user set or took off at
 * a stop.
 * @cn: the editor.
 *
 * The mark is drawn hollow at once and turns solid when gdb answers, or
 * stays hollow with gdb's words.
 * Return: 0.
 */
static int e_d_ga_brk_sync(ECNT *cn)
{
 e_d_gs_reset(&e_d_gs, cn);
 e_d_ga_chain_start(cn, e_d_ga_this()->brk_steps, &e_d_gs, e_d_gs_ask_finished,
                    E_D_GA_ASK);
 return(0);
}

/* step: one plain command whose answer says nothing the editor keeps */

static char e_d_ga_plain[E_D_REQ_CMD_MAX];

static int e_d_gs_plain_cmd(char *cmd, size_t n, void *ctx)
{
 (void)ctx;
 if (!e_d_ga_plain[0])
  return(0);
 snprintf(cmd, n, "%s", e_d_ga_plain);
 e_d_ga_plain[0] = '\0';
 return(1);
}

static const E_D_STEP e_d_gdb_plain_steps[] =
{
 { e_d_gs_plain_cmd, e_d_gs_ignore_answer },
 { NULL, NULL }
};

/**
 * e_d_ga_ask_plain - Send gdb one command at a stop and drop its answer.
 * @cn: the editor; @cmd: the command (Remove all breakpoints' "delete").
 * Return: 0.
 */
static int e_d_ga_ask_plain(ECNT *cn, const char *cmd)
{
 snprintf(e_d_ga_plain, sizeof e_d_ga_plain, "%s", cmd);
 e_d_gs_reset(&e_d_gs, cn);
 e_d_ga_chain_start(cn, e_d_gdb_plain_steps, &e_d_gs, e_d_gs_ask_finished,
                    E_D_GA_ASK);
 return(0);
}

/**
 * e_d_gs_windows_finished - The watches, and the stack when it is open,
 * have been answered: fill their windows.
 * @ok: 0 when an answer was given up on; @ctx: the context.
 */
static void e_d_gs_windows_finished(int ok, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 if (!e_d_ga_chain_ended() || !ok)
 {
  e_d_ga_actions_drop();
  e_d_gs_free(st);
  return;
 }
 if (e_d_gs_show_windows(st) < 0)
 {
  e_d_gs_say_no_stack(st);
  e_d_gs_free(st);
  return;
 }
 e_d_gs_free(st);
 e_d_act_schedule();
}

/**
 * e_d_ga_watches_ask - Read every watch again at a stop, after the user
 * added, edited or deleted one, and the stack with them when its window is
 * open.
 * @cn: the editor; @sw: 1 to bring the Watches window to the top.
 * Return: 0.
 */
static int e_d_ga_watches_ask(ECNT *cn, int sw)
{
 E_D_GA_CTX *st = &e_d_gs;

 e_d_gs_reset(st, cn);
 st->stack_wanted = e_d_gs_stack_window_open(cn);
 st->watches_sw = sw;
 e_d_ga_chain_start(cn, e_d_ga_this()->watches_steps, st,
                    e_d_gs_windows_finished,
                    E_D_GA_ASK);
 return(0);
}

/* The stack alone (Ctrl-G K). */
static const E_D_STEP e_d_gdb_stack_steps[] =
{
 { e_d_gs_stack_cmd, e_d_gs_stack_answered },
 { NULL, NULL }
};

/**
 * e_d_gs_stack_finished - Ctrl-G K's "bt" has been answered: fill the Stack
 * window.
 * @ok: 0 when the answer was given up on; @ctx: the context.
 */
static void e_d_gs_stack_finished(int ok, void *ctx)
{
 E_D_GA_CTX *st = ctx;
 int ret;

 if (!e_d_ga_chain_ended() || !ok)
 {
  e_d_ga_actions_drop();
  e_d_gs_free(st);
  return;
 }
 ret = e_d_ga.be->show_stack(st);
 if (ret < 0)
  e_d_gs_say_no_stack(st);
 e_d_gs_free(st);
 if (ret == 0)
  e_d_act_schedule();
}

/**
 * e_d_ga_stack_ask - Ctrl-G K under gdb: ask "bt" on the fd loop.
 * @cn: the editor; @sw: 1 to bring the Stack window to the top.
 * Return: 0.
 */
static int e_d_ga_stack_ask(ECNT *cn, int sw)
{
 E_D_GA_CTX *st = &e_d_gs;

 e_d_gs_reset(st, cn);
 st->stack_wanted = 1;
 st->stack_sw = sw;
 e_d_ga_chain_start(cn, e_d_ga_this()->stack_steps, st, e_d_gs_stack_finished,
                    E_D_GA_ASK);
 return(0);
}

/* step: move gdb's selected frame to the row followed in the Stack window */

static int e_d_gs_move_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 if (st->frame > e_d_nstack)
  snprintf(cmd, n, E_D_GDB_UP_FMT, st->frame - e_d_nstack);
 else if (st->frame < e_d_nstack)
  snprintf(cmd, n, E_D_GDB_DOWN_FMT, e_d_nstack - st->frame);
 else
  return(0);
 return(1);
}

static int e_d_gs_move_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 (void)how;
 (void)ans;
 e_d_nstack = st->frame;
 return(E_D_STEP_NEXT);
}

static const E_D_STEP e_d_gdb_follow_steps[] =
{
 { e_d_gs_move_cmd,   e_d_gs_move_answered },
 { e_d_gs_stack_cmd,  e_d_gs_stack_answered },
 { e_d_gs_watch_cmd,  e_d_gs_watch_answered },
 { NULL, NULL }
};

/**
 * e_d_gs_follow_finished - The frame followed is gdb's selected one: the
 * watches read its locals, and its line is shown.
 * @ok: 0 when an answer was given up on; @ctx: the context.
 */
static void e_d_gs_follow_finished(int ok, void *ctx)
{
 E_D_GA_CTX *st = ctx;
 char file[E_D_LINE_MAX];
 int line = st->line;

 if (!e_d_ga_chain_ended() || !ok)
 {
  e_d_ga_actions_drop();
  e_d_gs_free(st);
  return;
 }
 if (e_d_gs_show_windows(st) < 0)
 {
  e_d_gs_say_no_stack(st);
  e_d_gs_free(st);
  return;
 }
 snprintf(file, sizeof file, "%s", st->file);
 e_d_gs_free(st);
 if (!e_d_gs_mark(file, line))
  e_d_act_schedule();
}

/**
 * e_d_ga_follow_frame - Enter on a row of the Stack window under gdb: make
 * its frame gdb's selected one and show its line.
 * @cn: the editor; @frame: the row's frame number; @file, @line: its place.
 * Return: 0.
 */
static int e_d_ga_follow_frame(ECNT *cn, int frame, const char *file, int line)
{
 E_D_GA_CTX *st = &e_d_gs;

 e_d_gs_reset(st, cn);
 st->frame = frame;
 st->line = line;
 snprintf(st->file, sizeof st->file, "%s", file);
 st->stack_wanted = e_d_gs_stack_window_open(cn);
 e_d_ga_chain_start(cn, e_d_ga_this()->follow_steps, st,
                    e_d_gs_follow_finished,
                    E_D_GA_ASK);
 return(0);
}

/* step: select the frame below the one marked after Pause */

static int e_d_gs_below_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 snprintf(cmd, n, E_D_GDB_FRAME_SELECT_FMT, st->below);
 return(1);
}

static const E_D_STEP e_d_gdb_below_steps[] =
{
 { e_d_gs_below_cmd, e_d_gs_ignore_answer },
 { NULL, NULL }
};

/**
 * e_d_gs_below_finished - The frame below the marked one is selected: run
 * out of the library call Pause stopped in, then step.
 * @ok: 0 when the answer was given up on; @ctx: the context.
 */
static void e_d_gs_below_finished(int ok, void *ctx)
{
 E_D_GA_CTX *st = ctx;
 int step_over = st->step_over;

 e_d_gs_free(st);
 if (!e_d_ga_chain_ended() || !ok)
 {
  e_d_ga_actions_drop();
  return;
 }
 e_d_gdb_finish_out_of_pause(e_d_ga_window(), step_over);
}

/**
 * e_d_ga_finish_from_frame - F8 or F7 after a Pause in library code: select
 * the frame below the marked one on the fd loop, then finish out of it.
 * @cn: the editor; @below: that frame; @sw: 1 for Step Over.
 * Return: 0.
 */
static int e_d_ga_finish_from_frame(ECNT *cn, int below, int sw)
{
 E_D_GA_CTX *st = &e_d_gs;

 e_d_gs_reset(st, cn);
 st->below = below;
 st->step_over = sw;
 e_d_ga_chain_start(cn, e_d_gdb_below_steps, st, e_d_gs_below_finished,
                    E_D_GA_ASK);
 return(0);
}

/* step: a temporary breakpoint on main, out of code with no source here */

static int e_d_gs_tbreak_cmd(char *cmd, size_t n, void *ctx)
{
 (void)ctx;
 snprintf(cmd, n, "%s", E_D_GDB_TBREAK_MAIN);
 return(1);
}

static const E_D_STEP e_d_gdb_leave_steps[] =
{
 { e_d_gs_tbreak_cmd, e_d_gs_ignore_answer },
 { NULL, NULL }
};

/**
 * e_d_gs_leave_finished - The temporary breakpoint is set: let the program
 * run on to it, as a run of its own.
 * @ok: 0 when the answer was given up on; @ctx: the context.
 */
static void e_d_gs_leave_finished(int ok, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 e_d_gs_free(st);
 if (!e_d_ga_chain_ended() || !ok)
 {
  e_d_ga_actions_drop();
  return;
 }
 e_d_ga_go_on(E_D_GDB_CONTINUE);
}

/**
 * e_d_ga_leave_unknown_source - The stop is in code whose source is not
 * here (a library): stop next at main instead, without the user waiting
 * in front of a frozen editor.
 * @cn: the editor.
 * Return: 0.
 */
static int e_d_ga_leave_unknown_source(ECNT *cn)
{
 e_d_gs_reset(&e_d_gs, cn);
 e_d_ga_chain_start(cn, e_d_gdb_leave_steps, &e_d_gs, e_d_gs_leave_finished,
                    E_D_GA_ASK);
 return(0);
}

/* ==== gdb's start and every resume, on the fd loop ====

   Once gdb has answered its banner it is set up through the same engine
   as the answers at a stop: its settings, then the user's breakpoints, and
   only then the command the user pressed.  Every run, Continue, step,
   Finish and Go to cursor first asks gdb what is still owed (the prompts
   an interrupt may still be answered with, breakpoint changes not yet
   told, the start breakpoint) and only then sends its command, which the
   run controller reads.  A key typed meanwhile reaches the editor; a
   debugger key is kept for after the next stop, and Esc or Ctrl-C give up
   on gdb's answer with a line that names it. */

/* The command after the banner that turns off gdb's confirmations ("Start
   it from the beginning?", "Kill the program being debugged?"), which
   would otherwise wait for an answer the editor never types. */
#define E_D_GDB_CONFIRM_OFF  "set confirm off"
/* Where gdb gives its program a terminal: the editor's pty. */
#define E_D_GDB_TTY_FMT      "set inferior-tty %s"
/* A resume command: "r" with the program's arguments, or "advance" with a
   path. */
#define E_D_GO_CMD_MAX       (E_D_LINE_MAX + PATH_MAX)

/* What a start or a resume is to do once the debugger has answered. */
struct e_d_ga_go
{
 ECNT *cn;
 int how;                    /* E_D_GO_*; 0 for the set-up after the banner */
 char cmd[E_D_GO_CMD_MAX];   /* the resume command, with its line end */
 int start_brk;              /* the start breakpoint (entry + 1) to take out
                                at the stop, or 0 */
 int goto_line;              /* Go to cursor's line, after the start's stop */
 const char *follow;         /* the step that follows a finish out of a Pause */
 e_d_resume_fn pressed;      /* the set-up: the key that started the session */
 int pressed_arg;
};

static E_D_GA_GO e_d_gg;

/**
 * e_d_gg_reset - Start a new start or resume context.
 * @go: the context; @cn: the editor.
 */
static void e_d_gg_reset(E_D_GA_GO *go, ECNT *cn)
{
 memset(go, 0, sizeof *go);
 go->cn = cn;
}

/* ---- the set-up after the banner ---- */

static int e_d_gu_confirm_cmd(char *cmd, size_t n, void *ctx)
{
 (void)ctx;
 snprintf(cmd, n, "%s", E_D_GDB_CONFIRM_OFF);
 return(1);
}

static int e_d_gu_tty_cmd(char *cmd, size_t n, void *ctx)
{
 (void)ctx;
 if (e_d_pty_master < 0)
  return(0);
 snprintf(cmd, n, E_D_GDB_TTY_FMT, e_d_pty_slave_name);
 return(1);
}

/* gdb's settings, then every breakpoint of the user's.  A set-up given up
   on and asked again first clears a line gdb may have set unanswered. */
static const E_D_STEP e_d_gdb_set_up_steps[] =
{
 { e_d_gu_confirm_cmd, e_d_gs_ignore_answer },
 { e_d_gu_tty_cmd,     e_d_gs_ignore_answer },
 { e_d_gb_remove_cmd,  e_d_gb_remove_answered },
 { e_d_gb_add_cmd,     e_d_gu_add_answered },
 { NULL, NULL }
};

static int e_d_gm_fell_back(E_D_GA_GO *go);   /* gdb too old for MI (below) */

/**
 * e_d_gu_set_up_finished - gdb is set up: the command the user pressed to
 * start the session runs now.
 * @ok: 0 when an answer was given up on: the status row names it, the keys
 * kept are dropped, and the next debugger command sets gdb up again (Debug
 * > Quit ends the session); @ctx: the context.
 */
static void e_d_gu_set_up_finished(int ok, void *ctx)
{
 E_D_GA_GO *go = ctx;
 ECNT *cn = go->cn;
 e_d_resume_fn pressed = go->pressed;
 int arg = go->pressed_arg;

 if (!e_d_ga_chain_ended())
  return;
 if (!ok)
 {
  e_d_ga_actions_drop();
  e_d_ga.set_up_owed = 1;
  return;
 }
 if (e_d_gm_fell_back(go))
  return;
 e_d_ga.set_up_owed = 0;
 jdb_trace("e_d_gu_set_up_finished: gdb set up, e_d_swtch -> 2\n");
 e_d_swtch = 2;
 e_d_start_take_focus();
 e_d_start_session_ready(cn, pressed, arg);
 if (!e_d_run_going() && !e_d_ga_answering())
  e_d_act_schedule();
}

/**
 * e_d_ga_set_up - gdb answered its banner: give it its settings and the
 * user's breakpoints on the fd loop, then run @pressed.
 * @cn: the editor; @pressed, @arg: the command that started the session,
 * or NULL.
 */
static void e_d_ga_set_up(ECNT *cn, e_d_resume_fn pressed, int arg)
{
 E_D_GA_GO *go = &e_d_gg;

 e_d_gg_reset(go, cn);
 go->pressed = pressed;
 go->pressed_arg = arg;
 e_d_ga_chain_start(cn, e_d_ga_this()->set_up_steps, go,
                    e_d_gu_set_up_finished,
                    E_D_GA_START);
}

/**
 * e_d_ga_set_up_again - A debugger command after the set-up was given up
 * on: set gdb up again, and run the command once it has answered.
 * @f: the window the key was pressed in.
 * Return: 1 when the set-up went out again.
 */
static int e_d_ga_set_up_again(FENSTER *f)
{
 e_d_resume_fn cmd = e_d_start_cmd;
 int arg = e_d_start_arg;

 if (!e_d_ga_backend_of(e_deb_type) || !e_d_ga.set_up_owed)
  return(0);
 e_d_start_cmd = NULL;
 e_d_ga_set_up(f->ed, cmd, arg);
 return(1);
}

/* ---- what a resume asks first ---- */

/**
 * e_d_gg_start_entry - The editor's start breakpoint, while gdb has not
 * been given it.
 * Return: its list index, or -1.
 */
static int e_d_gg_start_entry(void)
{
 int i;

 for (i = 0; i < e_d_nbrpts; i++)
  if (e_d_brk_is_start_entry(i) && e_d_nrbrpts[i] == 0)
   return(i);
 return(-1);
}

/* step: the breakpoint the editor sets to stop at the program's start */

static int e_d_gg_start_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_GA_GO *go = ctx;

 if (go->how != E_D_GO_START || e_d_gg_start_entry() < 0)
  return(0);
 if (!e_d_ga.be->start_brk_cmd)
  return(0);
 e_d_ga.be->start_brk_cmd(cmd, n);
 return(1);
}

/**
 * e_d_gdb_start_brk_cmd - gdb's command that stops the first run at the
 * program's start (e_d_gdb_start_break, without its line end).
 * @cmd, @n: the command.
 */
static void e_d_gdb_start_brk_cmd(char *cmd, size_t n)
{
 char line[E_D_GO_CMD_MAX];
 size_t len;

 e_d_gdb_start_break(line, sizeof line);
 len = strlen(line);
 if (len > 0 && line[len - 1] == '\n')
  line[len - 1] = '\0';
 snprintf(cmd, n, "%s", line);
}

/**
 * e_d_ga_refused_words - Did the debugger refuse what @ans answers?  Its
 * reason goes into @words, for the status row.
 * @ans: the answer; @words, @n: the reason.
 *
 * The line debuggers refuse on their error stream; gdb over MI in an error
 * record of the answer (the backend's refused_words).
 * Return: 1 when it refused.
 */
static int e_d_ga_refused_words(const E_D_ANSWER *ans, char *words, size_t n)
{
 if (e_d_ga.be->refused_words)
  return(e_d_ga.be->refused_words(ans, words, n));
 if (!ans->error)
  return(0);
 e_d_gb_first_words(ans->text, words, n);
 return(1);
}

/**
 * e_d_gg_start_answered - The debugger answered the start breakpoint: keep
 * what it gave it (gdb's number), so it is taken out again at the stop; say
 * gdb's words when it refused (no start symbol in the program).
 */
static int e_d_gg_start_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 char str[E_D_BRK_ANSWER_MAX];
 int i = e_d_gg_start_entry();
 int id = e_d_ga.be->brk_set_id(ans);

 (void)how;
 (void)ctx;
 if (i >= 0)
  e_d_nrbrpts[i] = id;
 if (id == 0 && e_d_ga_refused_words(ans, str, sizeof str))
  e_d_error(str);
 return(E_D_STEP_NEXT);
}

/* The prompts owed to interrupts first, so none is taken for the stop of
   the command that follows; then the breakpoint changes gdb has not been
   told of, and the start breakpoint before the first run. */
static const E_D_STEP e_d_gdb_go_steps[] =
{
 { e_d_gs_settle_cmd, e_d_gs_settle_answered },
 { e_d_gb_remove_cmd, e_d_gb_remove_answered },
 { e_d_gb_add_cmd,    e_d_gb_add_answered },
 { e_d_gg_start_cmd,  e_d_gg_start_answered },
 { NULL, NULL }
};

/**
 * e_d_gg_asks_first - Is there anything to ask gdb before the resume
 * command goes out?
 * @go: the context.
 */
static int e_d_gg_asks_first(const E_D_GA_GO *go)
{
 if (e_d_run.int_owed > 0 || go->how == E_D_GO_FINISH)
  return(1);
 if (e_d_brk_ngone > 0 || e_d_gb_next_unsent() >= 0 || e_d_cursor_brk.wanted)
  return(1);
 return(go->how == E_D_GO_START && e_d_gg_start_entry() >= 0);
}

/* ---- the resume command itself ---- */

/**
 * e_d_gg_awaits_its_line - Note the line gdb answers a run with, which the
 * run's reader takes ("Starting program" after "r", "Continuing." after
 * "c").  pdb answers a Continue with nothing but the stop.  sdb, dbx and xdb
 * say it in their own way (the backend's run_went).
 * @go: the context.
 */
static void e_d_gg_awaits_its_line(const E_D_GA_GO *go)
{
 const E_D_GA_BACKEND *be = e_d_ga_this();

 if (be->run_went)
 {
  be->run_went(go);
  return;
 }
 if (!be->says_run_lines)
  return;
 if (go->how == E_D_GO_START)
  e_d_gdb_awaits.start = 1;
 else
  e_d_gdb_awaits.cont = 1;
}

/* The status line when a run's command could not be handed to the debugger. */
#define E_D_RUN_NOT_SENT_FMT "%s would not take the command: the program was not run"

/**
 * e_d_gg_run_not_sent - The debugger's input would not take a run's
 * command (closed, or full far past any command): the program does not
 * run, and the user is told why instead of waiting for a stop that cannot
 * come.
 * @f: the window on top.
 * Return: 0, the session stays as it was.
 */
static int e_d_gg_run_not_sent(FENSTER *f)
{
 char line[E_D_GA_LINE_MAX];

 snprintf(line, sizeof line, E_D_RUN_NOT_SENT_FMT, e_d_input_name());
 e_status_say(f, line);
 return(0);
}

/**
 * e_d_gg_run_now - Send "r" or "c" and hand the run to the controller,
 * which reads the line that says the run began.
 * @go: the context.
 * Return: 0, or the end of the session.
 */
static int e_d_gg_run_now(E_D_GA_GO *go)
{
 FENSTER *f = go->cn->f[go->cn->mxedt];
 int ret;

 jdb_trace("e_d_gg_run_now: sending '%s'\n", go->cmd);
 e_d_gdb_awaits_forget();
 if (e_d_send_cmd(go->cmd) == WPE_FD_WRITE_REFUSED)
  return(e_d_gg_run_not_sent(f));
 e_d_swtch = 3;            /* pdb's first run is a Continue: it says nothing */
 e_d_gg_awaits_its_line(go);
 ret = e_d_run_hand_over_continue(f, e_d_run_ops_of_backend(), go->start_brk);
 if (go->goto_line > 0 && e_d_run_going() && e_d_run.ops->run_to_line)
  e_d_run.goto_line = go->goto_line;
 return(ret);
}

/**
 * e_d_gg_step_now - Send a step, Finish or Go to cursor and hand the run to
 * the controller.
 * @go: the context.
 * Return: 0, or the end of the session.
 */
static int e_d_gg_step_now(E_D_GA_GO *go)
{
 FENSTER *f = go->cn->f[go->cn->mxedt];
 int ret;

 if (e_d_send_cmd(go->cmd) == WPE_FD_WRITE_REFUSED)
  return(e_d_gg_run_not_sent(f));
 ret = e_d_run_hand_over(f, e_d_run_ops_of_backend(), 0);
 if (go->follow && e_d_run_going())
  e_d_pause_follow = go->follow;
 return(ret);
}

/**
 * e_d_gg_go_on_now - Let the program run on from a stop the user was not
 * shown.
 * @go: the context.
 *
 * A breakpoint set or taken off while gdb answered is told at the next
 * chance, as for any change made while the program runs.
 */
static int e_d_gg_go_on_now(E_D_GA_GO *go)
{
 e_d_run_go_on_now(go->cmd);
 if (e_d_brk_changed_while_running())
  e_d_run.sync_pending = 1;
 return(0);
}

/**
 * e_d_gg_hand_over - gdb has nothing left to answer: send the resume
 * command.
 * @go: the context.
 * Return: 0, or the end of the session.
 */
static int e_d_gg_hand_over(E_D_GA_GO *go)
{
 if (go->how == E_D_GO_ON)
  return(e_d_gg_go_on_now(go));
 if (go->how == E_D_GO_STEP)
  return(e_d_gg_step_now(go));
 if (go->how == E_D_GO_FINISH)
  return(e_d_ga_this()->finish_now(go));
 return(e_d_gg_run_now(go));
}

/**
 * e_d_ga_drop_start_entry - Take the editor's start breakpoint, list entry
 * @i, out of the list without waiting for gdb; gdb is told to remove it
 * at the next chance.  The removal keeps the line the debugger gave it, for
 * sdb, which names a breakpoint by its line (the start entry has no file:
 * the start symbol is its place).
 */
static void e_d_ga_drop_start_entry(int i)
{
 if (e_d_swtch && e_d_nrbrpts[i] != 0)
  e_d_gb_gone_keep(e_d_sbrpts[i], e_d_ybrpts[i], e_d_nrbrpts[i]);
 e_d_brk_list_drop(i);
}

/**
 * e_d_gg_drop_start_brk - A first run was given up on: its start
 * breakpoint goes, so the next Run does not stop on it unasked.
 */
static void e_d_gg_drop_start_brk(void)
{
 int i;

 for (i = 0; i < e_d_nbrpts; i++)
  if (e_d_brk_is_start_entry(i))
  {
   e_d_ga_drop_start_entry(i);
   return;
  }
}

/**
 * e_d_gg_given_up - An answer the resume waited for was given up on: the
 * program stays where it stopped, and the keys kept are dropped.
 * @go: the context.
 */
static void e_d_gg_given_up(E_D_GA_GO *go)
{
 e_d_ga_actions_drop();
 if (go->how == E_D_GO_START)
  e_d_gg_drop_start_brk();
 if (go->how != E_D_GO_ON)
  return;
 e_d_async_pending = 0;
 e_refresh_editor_bars(go->cn);    /* stopped: the stepping keys again */
}

/**
 * e_d_gg_finished - gdb answered what the resume asked first: send the
 * resume command now.  Pause pressed meanwhile, at a stop the user was not
 * shown, keeps the program stopped and shows the stop instead.
 * @ok: 0 when an answer was given up on; @ctx: the context.
 */
static void e_d_gg_finished(int ok, void *ctx)
{
 E_D_GA_GO *go = ctx;

 if (!e_d_ga_chain_ended())
  return;
 if (!ok)
 {
  e_d_gg_given_up(go);
  return;
 }
 if (go->how == E_D_GO_ON && e_d_ga.pause_asked)
 {
  e_d_ga.be->pause_instead(go->cn);
  return;
 }
 e_d_gg_hand_over(go);
}

/**
 * e_d_gg_launch - Send the resume at once, or first ask gdb what it owes.
 * @go: the context.
 *
 * Going on from a stop the user was not shown is, for the user, the program
 * running: keys that need gdb are refused as while it runs, and Pause shows
 * the stop.  Any other resume keeps the debugger keys for after its stop.
 * Return: 0, or the end of the session.
 */
static int e_d_gg_launch(E_D_GA_GO *go)
{
 int kind = E_D_GA_RESUME;

 if (!e_d_gg_asks_first(go))
  return(e_d_gg_hand_over(go));
 if (go->how == E_D_GO_ON)
 {
  kind = E_D_GA_SYNC;
  e_d_ga.pause_asked = 0;
  e_d_async_pending = 1;
 }
 e_d_ga_owe_settle(e_d_run.int_owed);
 e_d_ga_chain_start(go->cn, e_d_ga_this()->go_steps, go, e_d_gg_finished,
                    kind);
 return(0);
}

/**
 * e_d_ga_go - Run (the first time) or Continue under gdb.
 * @f: the window on top; @how: E_D_GO_START or E_D_GO_CONTINUE; @cmd: "r
 * ..." or "c", with its line end; @start_brk: the start breakpoint (entry +
 * 1) to take out at the stop, or 0.
 *
 * The user gets the editor back at once: the run goes out when gdb has
 * answered what it owes, and its stop is shown when it comes.
 * Return: 0, or the end of the session.
 */
static int e_d_ga_go(FENSTER *f, int how, const char *cmd, int start_brk)
{
 E_D_GA_GO *go = &e_d_gg;

 e_d_gg_reset(go, f->ed);
 go->how = how;
 snprintf(go->cmd, sizeof go->cmd, "%s", cmd);
 go->start_brk = start_brk;
 return(e_d_gg_launch(go));
}

/**
 * e_d_ga_go_step - A step, Finish or Go to cursor under gdb.
 * @f: the window on top; @cmd: gdb's command, with its line end;
 * @follow: after a finish out of a Pause, the step to send at its stop, or
 * NULL.
 * Return: 0, or the end of the session.
 */
static int e_d_ga_go_step(FENSTER *f, const char *cmd, const char *follow)
{
 E_D_GA_GO *go = &e_d_gg;

 e_d_gg_reset(go, f->ed);
 go->how = E_D_GO_STEP;
 snprintf(go->cmd, sizeof go->cmd, "%s", cmd);
 go->follow = follow;
 return(e_d_gg_launch(go));
}

/**
 * e_d_ga_go_on - Let gdb's program run on from a stop the user is not
 * shown (a breakpoint change told, an interrupt left over from a race, the
 * step after a finish), once gdb has answered what it owes.
 * @cmd: the command, with its line end.
 */
static void e_d_ga_go_on(const char *cmd)
{
 E_D_GA_GO *go = &e_d_gg;
 ECNT *cn = e_d_run.cn;

 if (!cn)
  cn = WpeEditor;
 e_d_gg_reset(go, cn);
 go->how = E_D_GO_ON;
 snprintf(go->cmd, sizeof go->cmd, "%s", cmd);
 e_d_gg_launch(go);
}

/**
 * e_d_ga_go_pending - Is a run of gdb's waiting for gdb's answers before it
 * goes out?
 */
static int e_d_ga_go_pending(void)
{
 return(e_d_ga.live && e_d_ga.chain == E_D_GA_RESUME);
}

/**
 * e_d_ga_go_keeps_start_brk - The start breakpoint (@sw, entry + 1) is to be
 * taken out at the stop of the run that is about to go out.
 * Return: 1 when that run keeps it.
 */
static int e_d_ga_go_keeps_start_brk(int sw)
{
 if (!e_d_ga_go_pending())
  return(0);
 e_d_gg.start_brk = sw;
 return(1);
}

/**
 * e_d_ga_go_keeps_goto_line - Go to cursor started the session: the run
 * that is about to go out runs on to @line once the start has stopped.
 * Return: 1 when that run keeps it.
 */
static int e_d_ga_go_keeps_goto_line(int line)
{
 if (!e_d_ga_go_pending())
  return(0);
 e_d_gg.goto_line = line;
 return(1);
}

/**
 * e_d_gdb_remove_all - Remove all breakpoints at a stop under gdb: its one
 * command, "delete", answered on the fd loop.
 * @cn: the editor.
 */
static void e_d_gdb_remove_all(ECNT *cn)
{
 e_d_ga_ask_plain(cn, E_D_GDB_DELETE_ALL);
}

/* ==== pdb's answers, start and resumes, on the fd loop ====

   pdb runs on the same engine as gdb: what a stop asks it (its stack after
   a Pause, the Stack window, the watches, the breakpoints changed while it
   ran, Go to cursor's breakpoint), a breakpoint toggled at a stop, Remove
   all, the set-up after its banner and what each resume asks first.  The
   stop itself is read by pdb's run controller table (e_d_pdb_feed), whose
   answer already names the place, so no stop asks "where" for its mark.
   Where pdb differs from gdb: it names a breakpoint by its place, never by
   a number; it has no command that only echoes, so its settle marker is a
   string it prints; and its error stream is the program's, its refusals
   being on its output ("***"). */

#define E_D_PDB_NAME         "pdb"   /* as the waiting line names it */
#define E_D_PDB_SETTLE_MARK  "xwpe-interrupts-settled"
#define E_D_PDB_SETTLE_CMD   "p '" E_D_PDB_SETTLE_MARK "'"
#define E_D_PDB_SETTLE_FMT   "p '%s'"   /* the same, for a numbered marker */
#define E_D_GP_WHERE         "where"
#define E_D_GP_UP_FMT        "up %d"
#define E_D_GP_BREAK_FMT     "b %s:%d"
#define E_D_GP_CLEAR_FMT     "clear %s:%d"   /* every breakpoint on a line */
#define E_D_GP_PLACE_ROOM    32      /* "clear " and ":<line>" around a path */
/* pdb names a breakpoint by its place: any number stands for one it set. */
#define E_D_GP_ID_SET        1

/* ---- a breakpoint's place, as pdb is told it ---- */

/**
 * e_d_gp_short_path - The path pdb is given in a breakpoint command: @full,
 * or, when that would not fit in one command of the request engine, the
 * path from the directory pdb runs in, which pdb takes too.
 * @full: the file's full path; @out, @n: the path to send.
 *
 * A breakpoint in a script under a very deep directory is then still set,
 * and cleared again, at the start of the session as at a stop.
 */
static void e_d_gp_short_path(const char *full, char *out, size_t n)
{
 size_t dlen = strlen(e_d_debugger_dir);
 const char *rest = full + dlen;

 if (strlen(full) + E_D_GP_PLACE_ROOM < E_D_REQ_CMD_MAX || dlen == 0 ||
     strncmp(full, e_d_debugger_dir, dlen))
 {
  snprintf(out, n, "%s", full);
  return;
 }
 if (*rest != DIRC && e_d_debugger_dir[dlen - 1] != DIRC)
 {
  snprintf(out, n, "%s", full);          /* another directory's name */
  return;
 }
 while (*rest == DIRC)
  rest++;
 snprintf(out, n, "%s", rest);
}

/**
 * e_d_gp_place_of - The path pdb is given for the breakpoint list's @file.
 * @out, @n: the path.
 */
static void e_d_gp_place_of(const char *file, char *out, size_t n)
{
 char full[PATH_MAX];

 e_d_brk_path_of(e_d_ga.cn ? e_d_ga.cn : WpeEditor, file, full, sizeof full);
 e_d_gp_short_path(full, out, n);
}

/**
 * e_d_pdb_brk_add_cmd - pdb's "b path:line" for list entry @i.
 * @cmd, @n: the command.
 */
static void e_d_pdb_brk_add_cmd(int i, char *cmd, size_t n)
{
 char place[PATH_MAX];

 e_d_gp_place_of(e_d_sbrpts[i], place, sizeof place);
 snprintf(cmd, n, E_D_GP_BREAK_FMT, place, e_d_ybrpts[i]);
}

/**
 * e_d_pdb_brk_remove_cmd - pdb's "clear path:line" for a breakpoint the
 * list no longer has.  pdb clears a line by its place, so a breakpoint whose
 * answer was given up on is cleared the same way as any other.
 * @g: the removal; @cmd, @n: the command.
 */
static void e_d_pdb_brk_remove_cmd(const E_D_BRK_GONE *g, char *cmd, size_t n)
{
 char place[PATH_MAX];

 e_d_gp_place_of(g->file, place, sizeof place);
 snprintf(cmd, n, E_D_GP_CLEAR_FMT, place, g->line);
}

/**
 * e_d_place_brk_judge - What a debugger that names a breakpoint by its
 * place (pdb, jdb, a68g) made of one, from its answer's rows: set, kept for
 * later (jdb's "Deferring breakpoint", until the class loads), or refused
 * with its words ("*** Blank or comment", "Unable to set breakpoint",
 * a68g's "monitor error: cannot set breakpoint in that line.").
 * @ans: the answer; @v: receives the verdict.
 */
static void e_d_place_brk_judge(const E_D_ANSWER *ans, E_D_BRK_ANSWER *v)
{
 const char *p = ans->text ? ans->text : "";
 char *row;
 size_t len;

 v->state = E_D_BRK_VERIFIED;
 v->msg[0] = '\0';
 while (*p && (row = e_d_text_take_row(&p)) != NULL)
 {
  len = strlen(row);
  if (len > 0 && row[len - 1] == '\n')
   row[len - 1] = '\0';
  e_d_brk_judge_line(v, row, 0);
  FREE(row);
 }
}

/**
 * e_d_place_brk_set_id - Did pdb, jdb or a68g take the breakpoint its
 * answer is for (set it, or kept it for when its class loads)?
 * Return: E_D_GP_ID_SET when it did, 0 when it refused.
 */
static int e_d_place_brk_set_id(const E_D_ANSWER *ans)
{
 E_D_BRK_ANSWER v;

 e_d_place_brk_judge(ans, &v);
 if (v.state == E_D_BRK_REFUSED)
  return(0);
 return(E_D_GP_ID_SET);
}

/**
 * e_d_place_brk_verdict - Keep with list entry @i what pdb, jdb or a68g
 * answered: the mark turns solid, or stays hollow with the debugger's words.
 * @id: unused (none gives a breakpoint a number the editor names it by);
 * @ans: the answer.
 */
static void e_d_place_brk_verdict(int i, int id, const E_D_ANSWER *ans)
{
 E_D_BRK_ANSWER v;

 (void)id;
 e_d_place_brk_judge(ans, &v);
 e_d_nrbrpts[i] = i + 1;          /* told: its answer decides its state */
 e_d_brk_take_answer(i, &v);
}

/* A Remove all at a stop: every breakpoint the debugger was given is
   cleared. */
static const E_D_STEP e_d_ga_remove_each_steps[] =
{
 { e_d_gb_remove_cmd, e_d_gb_remove_answered },
 { NULL, NULL }
};

/**
 * e_d_ga_remove_each_line - Remove all breakpoints at a stop under pdb or
 * jdb, which have no command for it (pdb's asks "Clear all breaks?", jdb
 * has none): each line it was given is cleared, one request after the
 * other, and the next run goes past every one of them.
 * @cn: the editor.
 */
static void e_d_ga_remove_each_line(ECNT *cn)
{
 int i;

 for (i = 0; i < e_d_nbrpts; i++)
  if (!e_d_brk_is_start_entry(i))
   e_d_brk_gone_add(i);
 e_d_gs_reset(&e_d_gs, cn);
 e_d_ga_chain_start(cn, e_d_ga_remove_each_steps, &e_d_gs,
                    e_d_gs_ask_finished, E_D_GA_ASK);
}

/* ---- the Stack window ---- */

/* step: pdb's stack, when the Stack window is open */

static int e_d_gp_frames_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 if (!st->stack_wanted)
  return(0);
 snprintf(cmd, n, "%s", E_D_GP_WHERE);
 return(1);
}

static int e_d_gp_frames_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 (void)how;
 (void)ctx;
 e_d_pdb_take_stack(ans->text);
 return(E_D_STEP_NEXT);
}

/**
 * e_d_ga_frames_show_stack - Fill the Stack window with the frames pdb or
 * jdb listed, which the editor keeps as frames (e_d_frames), not as text.
 * @st: the context.
 * Return: 0.
 */
static int e_d_ga_frames_show_stack(E_D_GA_CTX *st)
{
 int is = e_d_stack_open(st->cn);

 if (is > 0)
  e_d_frames_put_in_window(st->cn, is, st->stack_sw);
 return(0);
}

/* ---- a stop the user is shown ---- */

/* step: the watches, when the stop is marked (a stop in code with no line
   shows none, as it never did) */

static int e_d_gp_stop_watch_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 if (st->line <= 0)
  return(0);
 return(e_d_gs_watch_cmd(cmd, n, ctx));
}

/**
 * e_d_pdb_settle_cmd_for - pdb's command that prints @marker, so the editor
 * knows every answer pdb owed came before it.
 * @cmd, @n: the command.
 */
static void e_d_pdb_settle_cmd_for(const char *marker, char *cmd, size_t n)
{
 snprintf(cmd, n, E_D_PDB_SETTLE_FMT, marker);
}

/**
 * e_d_pdb_cursor_brk_cmd - pdb's command that sets (@set 1) or clears Go to
 * cursor's own breakpoint, by the path pdb is given.
 * @cmd, @n: the command.
 */
static void e_d_pdb_cursor_brk_cmd(int set, char *cmd, size_t n)
{
 char place[PATH_MAX];

 e_d_gp_short_path(e_d_cursor_brk.path, place, sizeof place);
 if (set)
  snprintf(cmd, n, E_D_GP_BREAK_FMT, place, e_d_cursor_brk.line);
 else
  snprintf(cmd, n, E_D_GP_CLEAR_FMT, place, e_d_cursor_brk.line);
}

/* step: Go to cursor's own breakpoint goes at the first stop after it.
   pdb's "clear" takes every breakpoint off the line, so when the user has
   one of his own there (set while the program ran) the line is left as it
   is: the user's keeps stopping the program, and taking it off later
   clears both. */

static int e_d_gp_cursor_clear_cmd(char *cmd, size_t n, void *ctx)
{
 (void)ctx;
 e_d_cursor_brk.wanted = 0;
 if (!e_d_cursor_brk.set)
  return(0);
 e_d_cursor_brk.set = 0;
 if (e_d_user_brk_on(e_d_cursor_brk.file, e_d_cursor_brk.line))
  return(0);
 e_d_pdb_cursor_brk_cmd(0, cmd, n);
 return(1);
}

/* The answers of a stop the user is shown: the interrupts' prompts first,
   then the Stack window and the watches, then the breakpoints changed while
   the program ran and Go to cursor's own. */
static const E_D_STEP e_d_pdb_stop_steps[] =
{
 { e_d_gs_settle_cmd,       e_d_gs_settle_answered },
 { e_d_gp_frames_cmd,       e_d_gp_frames_answered },
 { e_d_gp_stop_watch_cmd,   e_d_gs_watch_answered },
 { e_d_gb_remove_cmd,       e_d_gb_remove_answered },
 { e_d_gb_add_cmd,          e_d_gb_add_answered },
 { e_d_gp_cursor_clear_cmd, e_d_gs_ignore_answer },
 { NULL, NULL }
};

/**
 * e_d_gp_show_stop - pdb has answered everything the stop asks: the user
 * sees the stop marked, the windows filled and how a Pause ended; then the
 * next kept key runs.
 * @cn: the editor; @file, @line: the stop's place, @line 0 for none;
 * @start_brk: the start breakpoint (entry + 1) to drop, or 0.
 */
static void e_d_gp_show_stop(ECNT *cn, char *file, int line, int start_brk)
{
 FENSTER *f = e_d_ga_window();

 if (start_brk)
  e_mk_brk_main(f, start_brk);
 if (line > 0)
  e_d_goto_break(file, line, f);
 else
  e_d_run_repaint_margin(cn);
 _messages_activated = 0;
 e_refresh();
 e_d_run_stop_tail(cn, e_d_run.ops);
 e_d_act_schedule();
}

/**
 * e_d_gp_stop_finished - The answers of a pdb stop have come: show it.
 * @ok: 0 when an answer was given up on (the status row already says
 * which): nothing is drawn, and the kept keys are dropped.
 * @ctx: the context.
 */
static void e_d_gp_stop_finished(int ok, void *ctx)
{
 E_D_GA_CTX *st = ctx;
 ECNT *cn = st->cn;
 char file[E_D_LINE_MAX];
 int line = st->line, start_brk = st->start_brk;

 if (!e_d_ga_chain_ended() || !ok)
 {
  e_d_ga_actions_drop();
  e_d_gs_free(st);
  return;
 }
 if (line > 0)
  e_d_gs_show_windows(st);
 snprintf(file, sizeof file, "%s", st->file);
 e_d_gs_free(st);
 e_d_gp_show_stop(cn, file, line, start_brk);
}

/**
 * e_d_gp_stop_ask - pdb stopped at a place the user is shown: ask it, on
 * the fd loop, what the stop needs, and show the stop once it has answered.
 * @cn: the editor; @file, @line: where pdb stopped, @line 0 for no line.
 *
 * The keys reach the editor while pdb answers; F7, F8 and Run typed
 * meanwhile are kept and run in order after the stop is shown.
 */
static void e_d_gp_stop_ask(ECNT *cn, const char *file, int line)
{
 E_D_GA_CTX *st = &e_d_gs;

 e_d_gs_reset(st, cn);
 st->line = line;
 snprintf(st->file, sizeof st->file, "%s", file);
 st->start_brk = e_d_run.start_brk;
 e_d_run.start_brk = 0;
 st->stack_wanted = line > 0 && e_d_gs_stack_window_open(cn);
 e_d_run.stop_on_loop = 1;
 e_d_ga_chain_start(cn, e_d_pdb_stop_steps, st, e_d_gp_stop_finished,
                    E_D_GA_STOP);
}

/* ---- a Pause's stop ---- */

/**
 * e_d_gp_place_from_frame - The place of a frame of pdb's stack, kept as
 * the stop's.
 * @st: the context; @fr: the frame.
 */
static void e_d_gp_place_from_frame(E_D_GA_CTX *st, const e_d_frame *fr)
{
 snprintf(st->file, sizeof st->file, "%s", fr->file);
 st->line = fr->line;
}

/**
 * e_d_gp_pause_saved_place - The script's line remembered at the Pause's
 * first stop, when pdb's stack no longer shows the script (Python 3.13
 * after the handler returned).
 * @st: receives the place.
 * Return: E_D_PDB_PAUSE_MARKED, or E_D_PDB_PAUSE_NOT_SEEN when no line was
 * remembered (an ordinary stop).
 */
static int e_d_gp_pause_saved_place(E_D_GA_CTX *st)
{
 if (!e_d_pdb_run.pause_seen)
  return(E_D_PDB_PAUSE_NOT_SEEN);
 snprintf(st->file, sizeof st->file, "%s", e_d_pdb_run.pause_file);
 st->line = e_d_pdb_run.pause_line;
 return(E_D_PDB_PAUSE_MARKED);
}

/**
 * e_d_gp_pause_leave_or_lose - The Pause stopped in pdb's own code: leave
 * it with "return", up to a bound.
 * @st: the context; @handler: the SIGINT handler's index in the stack read.
 *
 * pdb's SIGINT handler arms a step, and with a breakpoint set that step
 * first stops inside bdb.py, where a continue only finishes the step and
 * the script stays stopped.  Each "return" runs out of one of pdb's frames
 * without letting the script advance, so a single continue then resumes it
 * (Ctrl-G R after Ctrl-G I).  When the bound is reached the run stops where
 * it is and the status row says so, and pdb is moved up to the script's
 * frame so that the rows of the Stack window, which start there, still
 * select the frame they show.
 * Return: E_D_PDB_PAUSE_GOING, or E_D_PDB_PAUSE_LOST at the bound.
 */
static int e_d_gp_pause_leave_or_lose(E_D_GA_CTX *st, int handler)
{
 if (++e_d_pdb_run.internal_steps <= E_D_PDB_INTERNAL_STEPS_MAX)
  return(E_D_PDB_PAUSE_GOING);
 if (handler + 1 < e_d_nframes)
  st->up = handler + 1;
 return(E_D_PDB_PAUSE_LOST);
}

/**
 * e_d_gp_pause_decide - After Pause, with pdb's stack read: what the stop
 * is to the user, and which frame pdb makes its current one.
 * @st: receives the script's place and the frames to go up.
 *
 * pdb stops inside its own SIGINT handler, in bdb.py, a few frames inside
 * the line the script was running.  The user wants the script's line: the
 * frame under the handler is found in pdb's stack, pdb is told to go up to
 * it (so the Stack window and the next step start there), and its line is
 * marked.  The handler and what is inside it are not listed afterwards, so
 * the script's frame is row 0 of the Stack window.
 * Return: E_D_PDB_PAUSE_*.
 */
static int e_d_gp_pause_decide(E_D_GA_CTX *st)
{
 int i = e_d_pdb_handler_index();

 if (i + 1 < e_d_nframes && !e_d_pdb_run.pause_seen)
  e_d_pdb_pause_note_script(&e_d_frames[i + 1]);
 if (e_d_pdb_pause_needs_return(i))
  return(e_d_gp_pause_leave_or_lose(st, i));
 if (i + 1 >= e_d_nframes)
  return(e_d_gp_pause_saved_place(st));
 st->up = i + 1;
 e_d_gp_place_from_frame(st, &e_d_frames[i + 1]);
 return(E_D_PDB_PAUSE_MARKED);
}

/* step: pdb's stack at the Pause's stop */

static int e_d_gp_where_cmd(char *cmd, size_t n, void *ctx)
{
 (void)ctx;
 snprintf(cmd, n, "%s", E_D_GP_WHERE);
 return(1);
}

static int e_d_gp_pause_where_answered(E_D_REQ_END how, E_D_ANSWER *ans,
                                       void *ctx)
{
 E_D_GA_CTX *st = ctx;

 (void)how;
 e_d_pdb_take_frames(ans->text);
 st->pause_how = e_d_gp_pause_decide(st);
 if (st->up > 0)
  return(E_D_STEP_NEXT);
 return(E_D_STEP_STOP);
}

/* step: up to the script's frame */

static int e_d_gp_up_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 if (st->up <= 0)
  return(0);
 snprintf(cmd, n, E_D_GP_UP_FMT, st->up);
 return(1);
}

static int e_d_gp_up_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 (void)how;
 (void)ans;
 (void)ctx;
 e_d_nstack = 0;
 return(E_D_STEP_NEXT);
}

static const E_D_STEP e_d_pdb_pause_steps[] =
{
 { e_d_gs_settle_cmd, e_d_gs_settle_answered },
 { e_d_gp_where_cmd,  e_d_gp_pause_where_answered },
 { e_d_gp_up_cmd,     e_d_gp_up_answered },
 { NULL, NULL }
};

/**
 * e_d_gp_pause_shown - pdb's stack after a Pause is read: send "return" to
 * leave pdb's own code, or show the script's line, or say pdb stayed in
 * its own code, or show the stop as any other.
 * @cn: the editor; @how: E_D_PDB_PAUSE_*; @file, @line: the script's place.
 */
static void e_d_gp_pause_shown(ECNT *cn, int how, char *file, int line)
{
 FENSTER *f = e_d_ga_window();

 if (how == E_D_PDB_PAUSE_GOING)
 {
  e_d_pdb_send_on(E_D_PDB_FINISH);
  return;
 }
 e_d_pdb_run.pause_seen = 0;
 if (how == E_D_PDB_PAUSE_NOT_SEEN)
 {
  e_d_pdb_stop_shown(f);
  return;
 }
 e_d_pdb_run.finish_up = 0;
 e_d_switch_out(0);
 if (how == E_D_PDB_PAUSE_LOST)
 {
  e_d_run.pause = E_D_PAUSE_NONE;
  e_status_say_until_key(f, E_D_PDB_PAUSE_LOST_LINE);
  e_d_gp_stop_ask(cn, "", 0);
  return;
 }
 e_d_swtch = 3;
 e_d_gp_stop_ask(cn, file, line);
}

/**
 * e_d_gp_pause_finished - The answers of a Pause's stop have come.
 * @ok: 0 when an answer was given up on: nothing is drawn; @ctx: the
 * context.
 */
static void e_d_gp_pause_finished(int ok, void *ctx)
{
 E_D_GA_CTX *st = ctx;
 ECNT *cn = st->cn;
 int how = st->pause_how, line = st->line;
 char file[E_D_LINE_MAX];

 snprintf(file, sizeof file, "%s", st->file);
 e_d_gs_free(st);
 if (!e_d_ga_chain_ended())
  return;
 if (!ok)
 {
  e_d_ga_actions_drop();
  return;
 }
 e_d_gp_pause_shown(cn, how, file, line);
}

/**
 * e_d_gp_pause_ask - Pause stopped pdb: read its stack on the fd loop, to
 * show the script's line and not pdb's own.
 * @cn: the editor.
 */
static void e_d_gp_pause_ask(ECNT *cn)
{
 E_D_GA_CTX *st = &e_d_gs;

 e_d_gs_reset(st, cn);
 st->line = 0;
 e_d_run.stop_on_loop = 1;
 e_d_ga_chain_start(cn, e_d_pdb_pause_steps, st, e_d_gp_pause_finished,
                    E_D_GA_STOP);
}

/**
 * e_d_gp_pause_instead - Pause was pressed while pdb answered at a stop the
 * user was not shown: the program is not let run on, and the stop is shown
 * as the Pause's, at the script's line.
 * @cn: the editor.
 */
static void e_d_gp_pause_instead(ECNT *cn)
{
 e_d_async_pending = 0;
 e_d_ga.pause_asked = 0;
 e_d_run.pause = E_D_PAUSE_TAKEN;
 e_d_gp_pause_ask(cn);
}

/* ---- a stop made to tell pdb of breakpoint changes ---- */

/* step: pdb's stack, to know whether the stop is still in its own code */

static int e_d_gp_sync_where_answered(E_D_REQ_END how, E_D_ANSWER *ans,
                                      void *ctx)
{
 E_D_GA_CTX *st = ctx;
 int handler;

 (void)how;
 e_d_pdb_take_frames(ans->text);
 handler = e_d_pdb_handler_index();
 if (handler + 1 < e_d_nframes && !e_d_pdb_run.pause_seen)
  e_d_pdb_pause_note_script(&e_d_frames[handler + 1]);
 if (!e_d_pdb_sync_must_leave(handler))
  return(E_D_STEP_NEXT);
 st->leave = 1;
 return(E_D_STEP_STOP);
}

/**
 * e_d_gp_clears_cursor_brk - Does the removal @g take Go to cursor's own
 * breakpoint off with it?  pdb's "clear" takes every breakpoint off its
 * line, Go to cursor's included.
 * Return: 1 when Go to cursor's is set on that line of that file.
 */
static int e_d_gp_clears_cursor_brk(const E_D_BRK_GONE *g)
{
 char full[PATH_MAX];

 if (!e_d_cursor_brk.set || g->line != e_d_cursor_brk.line)
  return(0);
 e_d_brk_path_of(e_d_ga.cn ? e_d_ga.cn : WpeEditor, g->file, full,
                 sizeof full);
 return(!strcmp(full, e_d_cursor_brk.path));
}

/* step: one breakpoint the user removed while the program ran; when its
   line is Go to cursor's too, that one is set again after it (below), so
   the run still stops on the cursor's line */

static int e_d_gp_sync_remove_cmd(char *cmd, size_t n, void *ctx)
{
 if (e_d_brk_ngone > 0 && e_d_gp_clears_cursor_brk(&e_d_brk_gone[0]))
 {
  e_d_cursor_brk.set = 0;
  e_d_cursor_brk.wanted = 1;
 }
 return(e_d_gb_remove_cmd(cmd, n, ctx));
}

static int e_d_ga_cursor_set_cmd(char *cmd, size_t n, void *ctx);  /* below */

/* Once the stop is the script's, the changes are sent with the files' full
   paths (a bare line number would name bdb.py there). */
static const E_D_STEP e_d_pdb_sync_steps[] =
{
 { e_d_gs_settle_cmd,      e_d_gs_settle_answered },
 { e_d_gp_where_cmd,       e_d_gp_sync_where_answered },
 { e_d_gp_sync_remove_cmd, e_d_gb_remove_answered },
 { e_d_gb_add_cmd,         e_d_gb_add_answered },
 { e_d_ga_cursor_set_cmd,  e_d_gs_ignore_answer },
 { NULL, NULL }
};

/**
 * e_d_gp_sync_leave - The interrupt's stop is still in pdb's own code:
 * "return" out of it, the run going on, and the stop that follows is the
 * interrupt's still (F251).
 */
static void e_d_gp_sync_leave(void)
{
 e_d_run.interrupt_for = E_D_INT_SYNC;
 e_d_run.int_taken = 1;
 e_d_pdb_send_on(E_D_PDB_FINISH);
}

/**
 * e_d_gp_sync_finished - pdb has the breakpoint changes: the marks show its
 * word, and the program runs on.  A Pause pressed meanwhile keeps the
 * program stopped and shows the stop instead.
 * @ok: 0 when an answer was given up on: the program stays stopped and the
 * status row says which answer did not come; @ctx: the context.
 */
static void e_d_gp_sync_finished(int ok, void *ctx)
{
 E_D_GA_CTX *st = ctx;
 ECNT *cn = st->cn;
 int leave = st->leave;

 e_d_async_pending = 0;
 e_d_gs_free(st);
 if (!e_d_ga_chain_ended())
  return;
 if (!ok)
 {
  e_refresh_editor_bars(cn);        /* stopped: the stepping keys again */
  return;
 }
 if (e_d_ga.pause_asked)
 {
  e_d_gp_pause_instead(cn);
  return;
 }
 if (leave)
 {
  e_d_gp_sync_leave();
  return;
 }
 e_d_pdb_run.pause_seen = 0;
 e_d_pdb_run.internal_steps = 0;
 e_d_run_repaint_margin(cn);
 e_d_run_go_on(e_d_run.cont);
 if (e_d_brk_changed_while_running())
  e_d_run.sync_pending = 1;
}

/**
 * e_d_pdb_sync_on_loop - The interrupt for a breakpoint change stopped the
 * program: keep what the script printed, leave pdb's own code, tell pdb on
 * the fd loop, then let it run on.
 * @cn: the editor.
 *
 * For the user the program never stopped: the bottom row still names
 * Pause, keys that need pdb are refused as while it runs.
 * Return: 1, the stop is dealt with.
 */
static int e_d_pdb_sync_on_loop(ECNT *cn)
{
 E_D_PDB_RUN *r = &e_d_pdb_run;
 E_D_GA_CTX *st = &e_d_gs;

 e_d_pdb_note_return(r);
 e_d_pdb_capture_before_interrupt(r);
 e_d_gs_reset(st, cn);
 e_d_ga.pause_asked = 0;
 e_d_async_pending = 1;
 e_d_ga_chain_start(cn, e_d_pdb_sync_steps, st, e_d_gp_sync_finished,
                    E_D_GA_SYNC);
 return(1);
}

/* ---- commands of the user's at a stop, the set-up and the resumes ---- */

/* The settings pdb needs none of; every breakpoint of the user's.  A
   set-up given up on and asked again first clears a line pdb may have set
   unanswered. */
static const E_D_STEP e_d_pdb_set_up_steps[] =
{
 { e_d_gb_remove_cmd, e_d_gb_remove_answered },
 { e_d_gb_add_cmd,    e_d_gu_add_answered },
 { NULL, NULL }
};

/* Add, edit or delete a watch at a stop. */
static const E_D_STEP e_d_pdb_watches_steps[] =
{
 { e_d_gp_frames_cmd, e_d_gp_frames_answered },
 { e_d_gs_watch_cmd,  e_d_gs_watch_answered },
 { NULL, NULL }
};

/* Ctrl-G K. */
static const E_D_STEP e_d_pdb_stack_steps[] =
{
 { e_d_gp_frames_cmd, e_d_gp_frames_answered },
 { NULL, NULL }
};

/* Enter on a row of the Stack window: pdb's "up" and "down" are gdb's. */
static const E_D_STEP e_d_pdb_follow_steps[] =
{
 { e_d_gs_move_cmd,   e_d_gs_move_answered },
 { e_d_gp_frames_cmd, e_d_gp_frames_answered },
 { e_d_gs_watch_cmd,  e_d_gs_watch_answered },
 { NULL, NULL }
};

/* step: Go to cursor's own breakpoint, just before the Continue, in the
   words of the debugger that runs (pdb's, jdb's) */

static int e_d_ga_cursor_set_cmd(char *cmd, size_t n, void *ctx)
{
 (void)ctx;
 if (!e_d_cursor_brk.wanted)
  return(0);
 e_d_cursor_brk.wanted = 0;
 e_d_cursor_brk.set = 1;      /* cleared at the stop, answered or not */
 e_d_ga.be->cursor_brk_cmd(1, cmd, n);
 return(1);
}

/* step: Finish needs the frames, to know the function has a caller */

static int e_d_gp_finish_frames_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_GA_GO *go = ctx;

 if (go->how != E_D_GO_FINISH)
  return(0);
 snprintf(cmd, n, "%s", E_D_GP_WHERE);
 return(1);
}

/* What a resume asks pdb first: the interrupts' prompts, the breakpoint
   changes it has not been told of, Go to cursor's own breakpoint, and for
   Finish the frames. */
static const E_D_STEP e_d_pdb_go_steps[] =
{
 { e_d_gs_settle_cmd,        e_d_gs_settle_answered },
 { e_d_gb_remove_cmd,        e_d_gb_remove_answered },
 { e_d_gb_add_cmd,           e_d_gb_add_answered },
 { e_d_ga_cursor_set_cmd,    e_d_gs_ignore_answer },
 { e_d_gp_finish_frames_cmd, e_d_gp_frames_answered },
 { NULL, NULL }
};

/**
 * e_d_ga_finish_now - pdb or jdb listed its frames: run to the end of the
 * function with the debugger's own command (e_d_finish_send), or say
 * Finish has nowhere to go in the outermost frame.
 * @go: the context.
 * Return: 0, or the end of the session.
 */
static int e_d_ga_finish_now(E_D_GA_GO *go)
{
 FENSTER *f = go->cn->f[go->cn->mxedt];

 if (e_d_frame_is_outermost(e_d_finish_frame()))
 {
  e_d_say_finish_outermost(f);
  e_d_act_schedule();
  return(0);
 }
 e_d_delbreak(f);
 e_d_switch_out(1);
 e_d_nstack = 0;
 return(e_d_finish_send(f));
}

/**
 * e_d_ga_finish_ask - Finish function under pdb or jdb: read the frames on
 * the fd loop, then send the debugger's own command for it (the backend's
 * finish_now: pdb's "return", jdb's "step up").
 * @f: the source window.
 *
 * The user keeps the editor while the debugger lists its stack; in the
 * outermost frame there is no caller and one line says so.
 * Return: 0, or the end of the session.
 */
static int e_d_ga_finish_ask(FENSTER *f)
{
 E_D_GA_GO *go = &e_d_gg;

 e_d_gg_reset(go, f->ed);
 go->how = E_D_GO_FINISH;
 return(e_d_gg_launch(go));
}

/**
 * e_d_ga_finishes_on_loop - Is Finish function asked on the request engine
 * (pdb, jdb, a68g), its frames read there before the command goes?
 */
static int e_d_ga_finishes_on_loop(void)
{
 const E_D_GA_BACKEND *be = e_d_ga_backend_of(e_deb_type);

 return(be && be->finish_now);
}

/**
 * e_d_ga_sets_cursor_brk - Does Go to cursor set its own breakpoint on the
 * request engine (pdb, jdb, a68g), rather than run with gdb's "advance" or send
 * it at once (the other debuggers)?
 */
static int e_d_ga_sets_cursor_brk(void)
{
 const E_D_GA_BACKEND *be = e_d_ga_backend_of(e_deb_type);

 return(be && be->cursor_brk_cmd);
}

/* ==== jdb's answers, start and resumes, on the fd loop ====

   jdb runs on the same engine as gdb and pdb: what a stop asks it (the
   watches, the Stack window, the breakpoints changed while it ran, Go to
   cursor's and the start's own breakpoints), the thread and frame a Pause
   stopped in, a breakpoint toggled at a stop, Remove all, the set-up after
   its first prompt and what each resume asks first.  The stop itself is
   read by jdb's run controller table (e_d_jdb_feed), whose answer names
   the place.  Where jdb differs from gdb and pdb:

   - its prompt is "> " with no current thread and "main[1] " at a stop
     (e_d_jdb_prompt_len), never one fixed string;
   - it reports events (a breakpoint hit, a step's end, the program's end)
     on the same output as its answers, whenever they happen.  An event
     that comes while an answer is read is no part of it: its lines are
     claimed (e_d_gj_line_is_async), the prompt that ends it is not taken
     for the answer's, and the stop it names is kept and shown once the
     answers are in (e_d_gj_show_kept_stop).  An answer whose prompt is
     followed by an event in the same read ends at that prompt;
   - it names a thread in its commands, and Pause leaves it with none, so
     the thread that runs the user's code is looked for first;
   - it has no command that only echoes: its settle command is one it does
     not know, whose name it repeats in its refusal;
   - "stop at" twice puts two breakpoints on a line and "clear" takes out
     one, so Go to cursor's own breakpoint is always cleared, a user's on
     the same line staying. */

#define E_D_JDB_NAME          "jdb"   /* as the waiting line names it */
#define E_D_JDB_SETTLE_MARK   "xwpe-interrupts-settled"
#define E_D_JDB_SETTLE_CMD    E_D_JDB_SETTLE_MARK
#define E_D_GJ_WHERE          "where"
#define E_D_GJ_WHERE_FMT      "where %s"
#define E_D_GJ_THREADS        "threads"
#define E_D_GJ_THREAD_FMT     "thread %s"
#define E_D_GJ_UP_FMT         "up %d"
#define E_D_GJ_STOP_AT_FMT    "stop at %s:%d"
#define E_D_GJ_CLEAR_AT_FMT   "clear %s:%d"
#define E_D_GJ_STOP_IN_FMT    "stop in %s.%s"
#define E_D_GJ_CLEAR_IN_FMT   "clear %s.%s"
/* jdb's print: its answer ends with " EXPR = value", after the lines of
   its expression parser's complaint ("com.sun.tools.example.debug.expr.
   ParseException: Name unknown: x") when it has one. */
#define E_D_GJ_PRINT          "print "
#define E_D_GJ_PRINT_SAYS_FMT " %s = "
#define E_D_GJ_PARSER_WORDS   "com.sun.tools.example.debug.expr."
/* The lines of an event after the one that names it: the source line and
   the blank line before its prompt. */
#define E_D_GJ_EVENT_LINES    2
/* A stop of jdb's not shown yet, and a line of jdb's output read with
   nothing asked that has no end yet. */
#define E_D_GJ_LOOSE_MAX      E_D_LINE_MAX
/* The start of a line kept to tell whether it is prompts alone: longer
   than a few prompts, a line is jdb's words. */
#define E_D_GJ_HEAD_MAX       160

/* What jdb wrote in the middle of the engine's answers. */
static struct
{
 int ev_lines;                  /* lines of the event's block still to come */
 int ev_prompts;                /* prompts that end events, not seen yet */
 char *stop;                    /* the stop it reported, kept to be shown */
 int timer;                     /* the loop's timer that shows it, or
                                   E_D_GA_NO_TIMER */
 char loose[E_D_GJ_LOOSE_MAX];  /* the start of a line read with nothing
                                   asked */
 size_t loose_len;
 char *owed_print;              /* the expression of a "print" given up on,
                                   whose answer jdb still owes */
 char head[E_D_GJ_HEAD_MAX];    /* the start of the line being read, while
                                   it may be prompts alone */
 size_t head_len;
 int head_long;                 /* that line is longer than any prompt */
} e_d_gj = { 0, 0, NULL, E_D_GA_NO_TIMER, "", 0, NULL, "", 0, 0 };

/* ---- a breakpoint's place, as jdb is told it ---- */

/**
 * e_d_jdb_start_class - The class of the program jdb runs, whose method
 * the editor's start breakpoint names.
 * @out, @n: receives it ("P" for P.java).
 */
static void e_d_jdb_start_class(char *out, size_t n)
{
 snprintf(out, n, "%s", e_d_file);
 WpeStringCutChar(out, '.');
}

/**
 * e_d_jdb_brk_add_cmd - jdb's "stop at Class:line" for list entry @i.
 * @cmd, @n: the command.
 */
static void e_d_jdb_brk_add_cmd(int i, char *cmd, size_t n)
{
 char cls[PATH_MAX];

 e_d_brk_jdb_class(e_d_sbrpts[i], cls, sizeof cls);
 snprintf(cmd, n, E_D_GJ_STOP_AT_FMT, cls, e_d_ybrpts[i]);
}

/**
 * e_d_jdb_brk_remove_cmd - jdb's "clear" for a breakpoint the list no
 * longer has: "clear Class:line" by its place, which also takes out one
 * whose answer was given up on, or "clear Class.main" for the editor's
 * start breakpoint, which has no file.
 * @g: the removal; @cmd, @n: the command.
 */
static void e_d_jdb_brk_remove_cmd(const E_D_BRK_GONE *g, char *cmd, size_t n)
{
 char cls[PATH_MAX];

 if (!g->file[0])
 {
  e_d_jdb_start_class(cls, sizeof cls);
  snprintf(cmd, n, E_D_GJ_CLEAR_IN_FMT, cls, e_get_start_symbol());
  return;
 }
 e_d_brk_jdb_class(g->file, cls, sizeof cls);
 snprintf(cmd, n, E_D_GJ_CLEAR_AT_FMT, cls, g->line);
}

/**
 * e_d_jdb_start_brk_cmd - jdb's "stop in Class.main", the editor's start
 * breakpoint when the user has none of his own.
 * @cmd, @n: the command.
 */
static void e_d_jdb_start_brk_cmd(char *cmd, size_t n)
{
 char cls[PATH_MAX];

 e_d_jdb_start_class(cls, sizeof cls);
 snprintf(cmd, n, E_D_GJ_STOP_IN_FMT, cls, e_get_start_symbol());
}

/**
 * e_d_jdb_cursor_brk_cmd - jdb's command that sets (@set 1) or clears Go
 * to cursor's own breakpoint, by the class of the file it was pressed in.
 * @cmd, @n: the command.
 */
static void e_d_jdb_cursor_brk_cmd(int set, char *cmd, size_t n)
{
 char cls[PATH_MAX];

 e_d_brk_jdb_class(e_d_cursor_brk.file, cls, sizeof cls);
 if (set)
  snprintf(cmd, n, E_D_GJ_STOP_AT_FMT, cls, e_d_cursor_brk.line);
 else
  snprintf(cmd, n, E_D_GJ_CLEAR_AT_FMT, cls, e_d_cursor_brk.line);
}

/* ---- where an answer ends, and the events in the middle of it ---- */

/**
 * e_d_gj_last_line - The last line of the answer so far: the one with no
 * end yet, or, when the text ends with a line end, the line it ends.
 * @ans: the answer; @len: receives the line's bytes, without its end.
 * Return: where it starts.
 */
static const char *e_d_gj_last_line(const E_D_ANSWER *ans, size_t *len)
{
 const char *text = ans->text;
 size_t end = ans->len;
 const char *start;

 if (end > 0 && text[end - 1] == '\n')
  end--;
 start = text + end;
 while (start > text && start[-1] != '\n')
  start--;
 *len = (size_t)(text + end - start);
 return(start);
}

/* jdb's print shows a String as " EXPR = \"text\"", the text as it is, its
   line ends included. */
#define E_D_GJ_STRING_OPENS   " = \""
#define E_D_GJ_STRING_QUOTE   '"'

/**
 * e_d_gj_find_in_line - Where @word first occurs in the @len bytes of
 * @line, which need not end with a NUL.
 * Return: its place, or NULL.
 */
static const char *e_d_gj_find_in_line(const char *line, size_t len,
                                       const char *word)
{
 size_t wlen = strlen(word), i;

 for (i = 0; i + wlen <= len; i++)
  if (!memcmp(line + i, word, wlen))
   return(line + i);
 return(NULL);
}

/**
 * e_d_gj_string_open_after - Is jdb still in the middle of a String value
 * once @line has ended?
 * @open: whether it was before the line; @line, @len: the line, without
 * its end.
 *
 * A String the program holds may have lines that start like jdb's prompt
 * ("> quoted", "main[1] text"); while jdb writes such a value, a line of
 * it is never taken for the prompt that ends the answer.  The value opens
 * on a line " EXPR = \"" of print's answer and closes on a line that ends
 * with a quote.
 */
static int e_d_gj_string_open_after(int open, const char *line, size_t len)
{
 const char *opens;
 size_t quote;

 if (open)
  return(len == 0 || line[len - 1] != E_D_GJ_STRING_QUOTE);
 if (len == 0 || line[0] != ' ')
  return(0);
 opens = e_d_gj_find_in_line(line, len, E_D_GJ_STRING_OPENS);
 if (!opens)
  return(0);
 quote = (size_t)(opens - line) + strlen(E_D_GJ_STRING_OPENS) - 1;
 return(len == quote + 1 || line[len - 1] != E_D_GJ_STRING_QUOTE);
}

/**
 * e_d_gj_answer_in_string - Is the answer, up to @upto, in the middle of a
 * String value jdb has not finished writing?
 * @ans: the answer so far; @upto: the start of one of its lines.
 */
static int e_d_gj_answer_in_string(const E_D_ANSWER *ans, const char *upto)
{
 const char *p = ans->text, *eol;
 int open = 0;

 while (p < upto && (eol = memchr(p, '\n', (size_t)(upto - p))) != NULL)
 {
  open = e_d_gj_string_open_after(open, p, (size_t)(eol - p));
  p = eol + 1;
 }
 return(open);
}

/**
 * e_d_jdb_ends_in_prompt - Does jdb's answer end with its prompt?
 * @ans: the answer so far; @prompt_len: set to the bytes that end it.
 *
 * The answer's last line must be nothing but prompts.  A prompt there that
 * ends an event jdb wrote meanwhile is not the answer's: the answer ends
 * only with one more.  A line of prompts ended by a line end is the case
 * where jdb wrote an event right behind the answer's prompt: the answer
 * ends at that prompt, and the event is read apart.  A line inside a
 * String value is the value's, whatever it looks like.
 * Return: 1 when the answer is whole.
 */
static int e_d_jdb_ends_in_prompt(const E_D_ANSWER *ans, size_t *prompt_len)
{
 const char *last;
 size_t len;
 int k;

 if (!ans->text || ans->len == 0)
  return(0);
 last = e_d_gj_last_line(ans, &len);
 k = e_d_jdb_prompts_only(last, len);
 if (k <= e_d_gj.ev_prompts)
  return(0);
 if (e_d_gj_answer_in_string(ans, last))
  return(0);
 e_d_gj.ev_prompts -= k - 1;           /* the others ended events */
 if (e_d_gj.ev_prompts < 0)
  e_d_gj.ev_prompts = 0;
 *prompt_len = ans->len - (size_t)(last - ans->text);
 return(1);
}

/**
 * e_d_gj_names_stop - Does the line, past its prompts, name a stop of
 * jdb's (a breakpoint hit, a step's end, an uncaught exception)?
 */
static int e_d_gj_names_stop(const char *line)
{
 const char *text = e_d_jdb_skip_prompts(line);

 return(!strncmp(text, E_D_JDB_STOP_MARK, strlen(E_D_JDB_STOP_MARK)) ||
        !strncmp(text, E_D_JDB_STEP_MARK, strlen(E_D_JDB_STEP_MARK)) ||
        !strncmp(text, E_D_JDB_EXCEPTION_MARK,
                 strlen(E_D_JDB_EXCEPTION_MARK)));
}

/**
 * e_d_gj_names_other_event - Does the line, past its prompts, say a
 * deferred breakpoint was set or that the program is gone?  Neither has a
 * prompt of its own.
 */
static int e_d_gj_names_other_event(const char *line)
{
 const char *text = e_d_jdb_skip_prompts(line);

 return(!strncmp(text, E_D_JDB_SET_DEFERRED, strlen(E_D_JDB_SET_DEFERRED)) ||
        !strncmp(text, E_D_JDB_EXIT, strlen(E_D_JDB_EXIT)) ||
        !strncmp(text, E_D_JDB_DISCONNECTED, strlen(E_D_JDB_DISCONNECTED)));
}

/**
 * e_d_gj_take_event_prompts - Step over the prompts at the start of @line
 * that end events jdb wrote before, which are owed no more.
 * @took: set to 1 when one was stepped over.
 * Return: the rest of the line.
 */
static const char *e_d_gj_take_event_prompts(const char *line, int *took)
{
 size_t len;

 *took = 0;
 while (e_d_gj.ev_prompts > 0 && (len = e_d_jdb_prompt_len(line)) > 0)
 {
  line += len;
  e_d_gj.ev_prompts--;
  *took = 1;
 }
 return(line);
}

/**
 * e_d_gj_head_is_prompts - Is the line read so far nothing but jdb's
 * prompts?
 */
static int e_d_gj_head_is_prompts(void)
{
 if (e_d_gj.head_long || e_d_gj.head_len == 0)
  return(0);
 return(e_d_jdb_prompts_only(e_d_gj.head, e_d_gj.head_len) > 0);
}

/**
 * e_d_gj_head_add - Keep a byte of the line being read, while it may still
 * be prompts alone.
 * @c: the byte; a line end starts a new line.
 */
static void e_d_gj_head_add(char c)
{
 if (c == '\n')
 {
  e_d_gj.head_len = 0;
  e_d_gj.head_long = 0;
  return;
 }
 if (e_d_gj.head_len + 1 >= sizeof e_d_gj.head)
 {
  e_d_gj.head_long = 1;
  return;
 }
 e_d_gj.head[e_d_gj.head_len++] = c;
 e_d_gj.head[e_d_gj.head_len] = '\0';
}

/**
 * e_d_jdb_separate_prompts - Give each prompt jdb wrote in front of more
 * text a line of its own.
 * @in, @n: what jdb wrote; @out: room for 2 * @n bytes.
 *
 * jdb ends every answer with a prompt and no line end, and writes the next
 * thing (an event, or the late answer to a "print", which it runs in a
 * thread of its own) on the prompt's line.  With the prompt on its own
 * line the answer it ends is seen to end there, and the event or the late
 * answer is a line of its own the editor can tell apart.  The line read so
 * far is kept across reads, since a prompt may end one read and the text
 * start the next.
 * Return: the bytes in @out.
 */
static size_t e_d_jdb_separate_prompts(const char *in, size_t n, char *out)
{
 size_t i, k = 0;

 for (i = 0; i < n; i++)
 {
  if (in[i] != '\n' && e_d_gj_head_is_prompts())
  {
   out[k++] = '\n';
   e_d_gj_head_add('\n');
  }
  out[k++] = in[i];
  e_d_gj_head_add(in[i]);
 }
 return(k);
}

/**
 * e_d_gj_event_dropped - The lines and the prompt still owed to an event
 * jdb wrote in the middle of an answer are no longer waited for: the
 * answer was given up on, and what is left of both is dropped up to the
 * settle command's answer.
 *
 * Without it the event's prompt, gone with the answer, would be waited
 * for after the settle command's, and every debugger key would be refused
 * as "still answering" until the user quits.  The stop the event named
 * stays kept, and is shown once the settle command is answered.
 */
static void e_d_gj_event_dropped(void)
{
 e_d_gj.ev_lines = 0;
 e_d_gj.ev_prompts = 0;
}

/**
 * e_d_gj_given_up - The answer to @cmd was given up on and jdb still owes
 * it.  jdb runs "print" in a thread of its own, so its answer can come
 * after the settle command's and land in the middle of the next answer; it
 * names its expression, so it is told apart by that (e_d_gj_late_print).
 * jdb answers every other command the editor sends in order.  The rest of
 * an event jdb wrote during the answer given up on (its prompt above all)
 * is dropped with that answer, up to the settle command's.
 * @cmd: the command.
 */
static void e_d_gj_given_up(const char *cmd)
{
 size_t lead = strlen(E_D_GJ_PRINT);

 e_d_gj_event_dropped();
 if (strncmp(cmd, E_D_GJ_PRINT, lead))
  return;
 if (e_d_gj.owed_print)
  FREE(e_d_gj.owed_print);
 e_d_gj.owed_print = WpeStrdup(cmd + lead);
}

/**
 * e_d_gj_late_print - Is the line part of jdb's late answer to a "print"
 * given up on?
 * @line: the line.
 *
 * Its last line, " EXPR = value", closes it, and the prompt that follows
 * it is owed to it, not to the answer the editor waits for.
 * Return: 1 for such a line.
 */
static int e_d_gj_late_print(const char *line)
{
 char says[E_D_REQ_CMD_MAX + sizeof E_D_GJ_PRINT_SAYS_FMT];

 if (!e_d_gj.owed_print)
  return(0);
 if (strstr(line, E_D_GJ_PARSER_WORDS))
  return(1);
 snprintf(says, sizeof says, E_D_GJ_PRINT_SAYS_FMT, e_d_gj.owed_print);
 if (strncmp(line, says, strlen(says)))
  return(0);
 FREE(e_d_gj.owed_print);
 e_d_gj.owed_print = NULL;
 e_d_gj.ev_prompts++;
 return(1);
}

/**
 * e_d_gj_event_body - Is the line one of the lines that follow an event's
 * first (its source line, the blank line before its prompt)?
 */
static int e_d_gj_event_body(const char *line)
{
 if (e_d_gj.ev_lines <= 0)
  return(0);
 e_d_gj.ev_lines--;
 if (!line[0])
  e_d_gj.ev_lines = 0;               /* the blank line closes the block */
 return(1);
}

/**
 * e_d_gj_line_is_async - Is this whole line of jdb's output no part of the
 * answer the engine waits for, but an event jdb wrote meanwhile?
 * @line: the line, without its end; @io: unused.
 *
 * The user never sees jdb's words on a stop taken for a watch's value or
 * a frame of the stack, and the prompt that ends the event never ends the
 * answer that is still to come.
 * Return: 1 for an event's line (deliver_async takes it).
 */
static int e_d_gj_line_is_async(const char *line, void *io)
{
 const char *rest;
 int took;

 (void)io;
 if (e_d_gj_event_body(line))
  return(1);
 rest = e_d_gj_take_event_prompts(line, &took);
 if (e_d_gj_names_stop(rest))
 {
  e_d_gj.ev_lines = E_D_GJ_EVENT_LINES;
  e_d_gj.ev_prompts++;
  return(1);
 }
 if (e_d_gj_names_other_event(rest))
  return(1);
 if (e_d_gj_late_print(rest))
  return(1);
 return(took && !rest[0]);              /* only an event's prompt */
}

static void e_d_gj_show_soon(void);   /* (below) */

/**
 * e_d_gj_keep_stop - Keep the stop jdb reported in the middle of other
 * answers, to show it once they are in.
 * @line: its line, from its mark on.
 */
static void e_d_gj_keep_stop(const char *line)
{
 if (e_d_gj.stop)
  FREE(e_d_gj.stop);
 e_d_gj.stop = WpeStrdup(line);
 e_d_gj_show_soon();
}

/**
 * e_d_gj_deliver_async - Act on a line of jdb's that was no answer's.
 * @line: the line, without its end; @io: unused.
 *
 * A stop is kept and shown as the stop it is; a deferred breakpoint set
 * turns its mark solid.  The other lines of an event say nothing more.
 */
static void e_d_gj_deliver_async(const char *line, void *io)
{
 const char *text = e_d_jdb_skip_prompts(line);

 (void)io;
 if (e_d_gj_names_stop(text))
  e_d_gj_keep_stop(text);
 else if (!strncmp(text, E_D_JDB_SET_DEFERRED, strlen(E_D_JDB_SET_DEFERRED)))
  e_d_jdb_confirm_deferred(text + strlen(E_D_JDB_SET_DEFERRED));
}

/**
 * e_d_gj_loose_line - One whole line jdb wrote with nothing asked: claim
 * and act on it as on one written in the middle of an answer.
 */
static void e_d_gj_loose_line(const char *line)
{
 if (e_d_gj_line_is_async(line, NULL))
  e_d_gj_deliver_async(line, NULL);
}

/**
 * e_d_gj_loose_tail - What jdb wrote with nothing asked ends without a line
 * end: an event's prompt there is seen, and anything else waits for the
 * rest of its line.
 */
static void e_d_gj_loose_tail(void)
{
 int took;
 const char *rest;

 e_d_gj.loose[e_d_gj.loose_len] = '\0';
 rest = e_d_gj_take_event_prompts(e_d_gj.loose, &took);
 if (!rest[0])
  e_d_gj.loose_len = 0;
}

/**
 * e_d_gj_unsolicited - jdb wrote with no request out: an event it wrote
 * behind the last answer's prompt, or one that came while the editor
 * waited for nothing.
 * @bytes, @n: what it wrote.
 *
 * The output is read on until the event is whole (e_d_gj_reads_on); the
 * stop it names is then shown.
 */
static void e_d_gj_unsolicited(const char *bytes, size_t n)
{
 size_t i;

 for (i = 0; i < n; i++)
 {
  if (bytes[i] != '\n')
  {
   if (e_d_gj.loose_len + 1 < sizeof e_d_gj.loose)
    e_d_gj.loose[e_d_gj.loose_len++] = bytes[i];
   continue;
  }
  e_d_gj.loose[e_d_gj.loose_len] = '\0';
  e_d_gj_loose_line(e_d_gj.loose);
  e_d_gj.loose_len = 0;
 }
 e_d_gj_loose_tail();
 e_d_gj_show_soon();
}

/**
 * e_d_gj_event_unfinished - Is jdb in the middle of writing an event: a
 * line without its end, the lines after its first, or its prompt?
 */
static int e_d_gj_event_unfinished(void)
{
 return(e_d_gj.loose_len > 0 || e_d_gj.ev_lines > 0 || e_d_gj.ev_prompts > 0);
}

/**
 * e_d_gj_reads_on - Must jdb's output be read on with nothing asked, so
 * the event it is writing, or the late answer it owes to a "print" given
 * up on, is read whole and the stop shown?
 */
static int e_d_gj_reads_on(void)
{
 return(e_d_gj_event_unfinished() || e_d_gj.owed_print != NULL);
}

/**
 * e_d_gj_forget_events - Forget what jdb wrote in the middle of answers,
 * and what it still owed: the program runs again, or the session ends.
 */
static void e_d_gj_forget_events(void)
{
 e_d_gj.ev_lines = 0;
 e_d_gj.ev_prompts = 0;
 e_d_gj.loose_len = 0;
 e_d_gj.head_len = 0;
 e_d_gj.head_long = 0;
 if (e_d_gj.owed_print)
  FREE(e_d_gj.owed_print);
 e_d_gj.owed_print = NULL;
 if (e_d_gj.stop)
  FREE(e_d_gj.stop);
 e_d_gj.stop = NULL;
 if (e_d_gj.timer == E_D_GA_NO_TIMER)
  return;
 wpe_timer_del(e_d_gj.timer);
 e_d_gj.timer = E_D_GA_NO_TIMER;
}

/* ---- a stop the user is shown ---- */

/* step: take back jdb's answers to commands sent while the program ran
   (Pause's "suspend", a breakpoint set or cleared), which came after its
   stop.  Each is one line and a prompt before the marker; the breakpoints'
   words among them still turn their marks solid. */

/**
 * e_d_gj_take_owed_words - Read jdb's words on breakpoints in the answers
 * that came before the settle marker.
 * @text: the settle command's answer; @mark: where the marker is in it.
 */
static void e_d_gj_take_owed_words(const char *text, const char *mark)
{
 char line[E_D_LINE_MAX];
 int pos = 0, end = (int)(mark - text);

 while (pos < end)
 {
  e_d_text_next_line(text, &pos, line, sizeof line);
  e_d_jdb_live_word_in(line);
 }
}

static int e_d_gj_settle_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 const char *mark = NULL;

 (void)how;
 (void)ctx;
 e_d_ga.marker_wanted = 0;
 if (ans->text)
  mark = strstr(ans->text, e_d_ga.be->settle_marker);
 if (mark)
  e_d_gj_take_owed_words(ans->text, mark);
 e_d_ga.owed = 0;
 e_d_run.int_owed = 0;        /* jdb answered every one: none comes later */
 e_d_jdb_owed_forget();
 return(E_D_STEP_NEXT);
}

/* step: jdb's stack, when the Stack window is open and the frames read at
   this stop are not kept (e_d_jdb_frames_kept) */

static int e_d_gj_frames_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 if (!st->stack_wanted || e_d_jdb_frames_kept())
  return(0);
 snprintf(cmd, n, "%s", E_D_GJ_WHERE);
 return(1);
}

static int e_d_gj_frames_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 (void)how;
 (void)ctx;
 e_d_jdb_take_frames(e_d_ga_editor(), ans->text);
 return(E_D_STEP_NEXT);
}

/* step: the watches, when the stop names a line (a stop that names none
   shows none, as it never did) */

static int e_d_gj_stop_watch_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 if (st->line <= 0)
  return(0);
 return(e_d_gs_watch_cmd(cmd, n, ctx));
}

/* step: Go to cursor's own breakpoint goes at the first stop after it.
   jdb holds one breakpoint per "stop at", two on a line when the user set
   his own there too, and "clear" takes out one: Go to cursor's is always
   cleared, and the user's stays. */

static int e_d_gj_cursor_clear_cmd(char *cmd, size_t n, void *ctx)
{
 (void)ctx;
 e_d_cursor_brk.wanted = 0;
 if (!e_d_cursor_brk.set)
  return(0);
 e_d_cursor_brk.set = 0;
 e_d_jdb_cursor_brk_cmd(0, cmd, n);
 return(1);
}

/* step: the editor's start breakpoint goes at the first stop, by the
   method it names */

static int e_d_gj_start_clear_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_BRK_GONE start;

 if (e_d_gb_start_entry_of(ctx) < 0)
  return(0);
 memset(&start, 0, sizeof start);
 start.file = "";
 e_d_jdb_brk_remove_cmd(&start, cmd, n);
 return(1);
}

/* The answers of a stop the user is shown: the owed answers first, then
   the Stack window and the watches, then the breakpoints changed while the
   program ran and the editor's own two. */
static const E_D_STEP e_d_jdb_stop_steps[] =
{
 { e_d_gs_settle_cmd,       e_d_gj_settle_answered },
 { e_d_gj_frames_cmd,       e_d_gj_frames_answered },
 { e_d_gj_stop_watch_cmd,   e_d_gs_watch_answered },
 { e_d_gb_remove_cmd,       e_d_gb_remove_answered },
 { e_d_gb_add_cmd,          e_d_gb_add_answered },
 { e_d_gj_cursor_clear_cmd, e_d_gs_ignore_answer },
 { e_d_gj_start_clear_cmd,  e_d_gb_start_answered },
 { NULL, NULL }
};

/**
 * e_d_gj_show_stop - jdb has answered everything the stop asks: the user
 * sees the stop marked in the Java file it names (or why it is not), the
 * exception it stopped on, and how a Pause ended; then the next kept key
 * runs.
 * @cn: the editor; @stop: jdb's stop line, or NULL when it named none.
 */
static void e_d_gj_show_stop(ECNT *cn, const char *stop)
{
 FENSTER *f = e_d_ga_window();

 if (stop)
 {
  e_d_jdb_mark_stop(f, stop);
  e_d_switch_out(0);
  e_d_jdb_say_exception(e_d_ga_window(), stop);
 }
 else
  e_d_run_repaint_margin(cn);
 _messages_activated = 0;
 e_refresh();
 e_d_run_stop_tail(cn, e_d_run.ops);
 e_d_act_schedule();
}

/**
 * e_d_gj_stop_finished - The answers of a jdb stop have come: show it.
 * @ok: 0 when an answer was given up on (the status row already says
 * which): nothing is drawn, and the kept keys are dropped.
 * @ctx: the context.
 */
static void e_d_gj_stop_finished(int ok, void *ctx)
{
 E_D_GA_CTX *st = ctx;
 ECNT *cn = st->cn;
 char *stop = st->stop;

 st->stop = NULL;
 if (!e_d_ga_chain_ended() || !ok)
 {
  e_d_ga_actions_drop();
  e_d_gs_free(st);
  if (stop)
   FREE(stop);
  return;
 }
 if (st->line > 0)
  e_d_gs_show_windows(st);
 e_d_gs_free(st);
 e_d_gj_show_stop(cn, stop);
 if (stop)
  FREE(stop);
}

/**
 * e_d_gj_stop_ask - jdb stopped the program: ask it, on the fd loop, what
 * the stop needs, and show the stop once it has answered.
 * @cn: the editor; @stop: jdb's stop line, or NULL when it named none.
 *
 * The keys reach the editor while jdb answers; F7, F8 and Run typed
 * meanwhile are kept and run in order after the stop is shown.
 */
static void e_d_gj_stop_ask(ECNT *cn, const char *stop)
{
 E_D_GA_CTX *st = &e_d_gs;

 e_d_gs_reset(st, cn);
 if (stop)
 {
  st->stop = WpeStrdup(stop);
  st->line = e_ds_jdb_stop_line(stop);
 }
 st->start_brk = e_d_run.start_brk;
 e_d_run.start_brk = 0;
 st->stack_wanted = st->line > 0 && e_d_gs_stack_window_open(cn);
 e_d_run.stop_on_loop = 1;
 e_d_ga_chain_start(cn, e_d_jdb_stop_steps, st, e_d_gj_stop_finished,
                    E_D_GA_STOP);
}

/* ---- a stop jdb reported in the middle of other answers ---- */

/**
 * e_d_gj_show_kept_stop - Show the stop jdb reported while the editor
 * read other answers (a breakpoint the program reached as a Pause or a
 * command was on its way), as the stop it is: marked, with its watches.
 *
 * Asked once those answers are in, from the loop, so the user sees the
 * last place the program stopped, never a frame or a value taken from
 * jdb's words on it.
 */
static void e_d_gj_show_kept_stop(void)
{
 char *stop = e_d_gj.stop;
 ECNT *cn = e_d_ga_editor();

 e_d_gj.stop = NULL;
 e_d_nstack = 0;
 e_d_nframes = 0;
 e_d_gj_stop_ask(cn, stop);
 FREE(stop);
}

/**
 * e_d_gj_on_show_timer - The loop is free: show the stop jdb reported in
 * the middle of other answers, once nothing is asked and the event is
 * whole.
 * @data: unused.
 *
 * A program let run on meanwhile (a kept key ran first) has moved away
 * from it, and its next stop is the one shown.
 */
static void e_d_gj_on_show_timer(void *data)
{
 (void)data;
 e_d_gj.timer = E_D_GA_NO_TIMER;
 if (!e_d_gj.stop)
  return;
 if (!e_d_ga.live || !e_d_session_runs() || e_d_run_going())
 {
  e_d_gj_forget_events();
  return;
 }
 if (e_d_ga_answering() || e_d_gj_event_unfinished())
  return;                 /* the chain's end or the event's prompt asks again */
 e_d_gj_show_kept_stop();
}

/**
 * e_d_gj_show_soon - Show a stop jdb reported in the middle of other
 * answers from the editor's loop, not from inside one of them.
 */
static void e_d_gj_show_soon(void)
{
 if (!e_d_gj.stop || e_d_gj.timer != E_D_GA_NO_TIMER)
  return;
 e_d_gj.timer = wpe_timer_add(0, 0, e_d_gj_on_show_timer, NULL);
 if (e_d_gj.timer <= 0)
  e_d_gj.timer = E_D_GA_NO_TIMER;
}

/**
 * e_d_gj_chain_over - A sequence of jdb's answers is over: a stop it
 * reported meanwhile is shown now.
 */
static void e_d_gj_chain_over(void)
{
 e_d_gj_show_soon();
}

/* ---- a Pause's stop ---- */

/* step: the threads of the paused VM; "suspend" leaves jdb with no current
   thread, so "where" alone would say "No thread specified" */

static int e_d_gj_threads_cmd(char *cmd, size_t n, void *ctx)
{
 (void)ctx;
 snprintf(cmd, n, "%s", E_D_GJ_THREADS);
 return(1);
}

static int e_d_gj_threads_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 (void)how;
 st->threads = e_d_gs_copy(ans);
 st->thread_pos = 0;
 return(E_D_STEP_NEXT);
}

/**
 * e_d_gj_next_thread - The next thread of the "threads" answer to look at.
 * @st: the context; its thread receives the id.
 * Return: 1 while there is one.
 */
static int e_d_gj_next_thread(E_D_GA_CTX *st)
{
 char row[E_D_FRAME_ROW_MAX];
 const char *p;

 if (!st->threads)
  return(0);
 p = st->threads + st->thread_pos;
 while (e_d_frames_next_row(&p, row, sizeof row))
 {
  st->thread_pos = (size_t)(p - st->threads);
  if (e_d_jdb_thread_id(row, st->thread, sizeof st->thread))
   return(1);
 }
 st->thread_pos = (size_t)(p - st->threads);
 return(0);
}

/* step: one thread's stack, again until one runs the user's code; each
   thread is tried in the order "threads" lists them */

static int e_d_gj_thread_where_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 if (!e_d_gj_next_thread(st))
  return(0);
 snprintf(cmd, n, E_D_GJ_WHERE_FMT, st->thread);
 return(1);
}

static int e_d_gj_thread_where_answered(E_D_REQ_END how, E_D_ANSWER *ans,
                                        void *ctx)
{
 E_D_GA_CTX *st = ctx;

 (void)how;
 e_d_jdb_take_frames(st->cn, ans->text);
 st->user_frame = e_d_jdb_user_frame();
 if (st->user_frame < 0)
  return(E_D_STEP_AGAIN);
 return(E_D_STEP_NEXT);
}

/* step: that thread becomes jdb's current one */

static int e_d_gj_thread_pick_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 if (st->user_frame < 0)
  return(0);
 snprintf(cmd, n, E_D_GJ_THREAD_FMT, st->thread);
 return(1);
}

/* step: up to its innermost frame of the user's code, so the Stack window
   and the watches start there and not in a library call it is in */

static int e_d_gj_up_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 if (st->user_frame <= 0)
  return(0);
 snprintf(cmd, n, E_D_GJ_UP_FMT, st->user_frame);
 return(1);
}

static int e_d_gj_up_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 (void)how;
 (void)ans;
 e_d_nstack = st->user_frame;
 return(E_D_STEP_NEXT);
}

/* step: the watches of that frame */

static int e_d_gj_pause_watch_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 if (st->user_frame < 0)
  return(0);
 return(e_d_gs_watch_cmd(cmd, n, ctx));
}

static const E_D_STEP e_d_jdb_pause_steps[] =
{
 { e_d_gs_settle_cmd,           e_d_gj_settle_answered },
 { e_d_gj_threads_cmd,          e_d_gj_threads_answered },
 { e_d_gj_thread_where_cmd,     e_d_gj_thread_where_answered },
 { e_d_gj_thread_pick_cmd,      e_d_gs_ignore_answer },
 { e_d_gj_up_cmd,               e_d_gj_up_answered },
 { e_d_gj_pause_watch_cmd,      e_d_gs_watch_answered },
 { e_d_gb_remove_cmd,           e_d_gb_remove_answered },
 { e_d_gb_add_cmd,              e_d_gb_add_answered },
 { e_d_gj_cursor_clear_cmd,     e_d_gs_ignore_answer },
 { e_d_gj_start_clear_cmd,      e_d_gb_start_answered },
 { NULL, NULL }
};

/**
 * e_d_gj_pause_shown - The thread a Pause stopped in is read: mark its
 * innermost line of the user's code, with the watches of that frame, or
 * nothing when no thread runs the user's code ("Paused" is all there is to
 * say then); then the next kept key runs.
 * @cn: the editor; @st: the context.
 */
static void e_d_gj_pause_shown(ECNT *cn, E_D_GA_CTX *st)
{
 char file[E_D_STOP_PATH_MAX];
 int line;

 if (st->user_frame < 0)
 {
  e_d_nframes = 0;
  e_d_gs_free(st);
  e_d_gj_show_stop(cn, NULL);
  return;
 }
 snprintf(file, sizeof file, "%s", e_d_frames[st->user_frame].file);
 line = e_d_frames[st->user_frame].line;
 e_d_gs_show_windows(st);
 e_d_gs_free(st);
 e_d_goto_break(file, line, e_d_ga_window());
 _messages_activated = 0;
 e_refresh();
 e_d_run_stop_tail(cn, e_d_run.ops);
 e_d_act_schedule();
}

/**
 * e_d_gj_pause_finished - The answers of a Pause's stop have come.
 * @ok: 0 when an answer was given up on: nothing is drawn; @ctx: the
 * context.
 */
static void e_d_gj_pause_finished(int ok, void *ctx)
{
 E_D_GA_CTX *st = ctx;
 ECNT *cn = st->cn;

 if (!e_d_ga_chain_ended() || !ok)
 {
  e_d_ga_actions_drop();
  e_d_gs_free(st);
  return;
 }
 e_d_gj_pause_shown(cn, st);
}

/**
 * e_d_gj_pause_ask - Pause stopped jdb's VM: look, on the fd loop, for the
 * thread that runs the user's code, and show its line.
 * @cn: the editor.
 */
static void e_d_gj_pause_ask(ECNT *cn)
{
 E_D_GA_CTX *st = &e_d_gs;

 e_d_gs_reset(st, cn);
 st->user_frame = -1;
 st->start_brk = e_d_run.start_brk;
 e_d_run.start_brk = 0;
 e_d_nstack = 0;
 e_d_nframes = 0;
 e_d_run.stop_on_loop = 1;
 e_d_ga_chain_start(cn, e_d_jdb_pause_steps, st, e_d_gj_pause_finished,
                    E_D_GA_STOP);
}

/**
 * e_d_gj_pause_instead - Pause was pressed while jdb answered at a stop the
 * user was not shown (a breakpoint he had just removed): the program is
 * not let run on, and the stop is shown as the Pause's.
 * @cn: the editor.
 */
static void e_d_gj_pause_instead(ECNT *cn)
{
 e_d_async_pending = 0;
 e_d_ga.pause_asked = 0;
 e_d_run.pause = E_D_PAUSE_TAKEN;
 e_d_gj_stop_ask(cn, e_ds_jdb_stop_line_in(e_d_jdb_reply.buf));
}

/* ---- commands of the user's at a stop, the set-up and the resumes ---- */

/* Every breakpoint of the user's; a set-up given up on and asked again
   first clears a line jdb may have set unanswered. */
static const E_D_STEP e_d_jdb_set_up_steps[] =
{
 { e_d_gb_remove_cmd, e_d_gb_remove_answered },
 { e_d_gb_add_cmd,    e_d_gu_add_answered },
 { NULL, NULL }
};

/* Add, edit or delete a watch at a stop. */
static const E_D_STEP e_d_jdb_watches_steps[] =
{
 { e_d_gj_frames_cmd, e_d_gj_frames_answered },
 { e_d_gs_watch_cmd,  e_d_gs_watch_answered },
 { NULL, NULL }
};

/* Ctrl-G K. */
static const E_D_STEP e_d_jdb_stack_steps[] =
{
 { e_d_gj_frames_cmd, e_d_gj_frames_answered },
 { NULL, NULL }
};

/* Enter on a row of the Stack window: jdb's "up" and "down" are gdb's. */
static const E_D_STEP e_d_jdb_follow_steps[] =
{
 { e_d_gs_move_cmd,   e_d_gs_move_answered },
 { e_d_gj_frames_cmd, e_d_gj_frames_answered },
 { e_d_gs_watch_cmd,  e_d_gs_watch_answered },
 { NULL, NULL }
};

/* step: Finish needs the frames, to know the function has a caller */

static int e_d_gj_finish_frames_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_GA_GO *go = ctx;

 if (go->how != E_D_GO_FINISH || e_d_jdb_frames_kept())
  return(0);
 snprintf(cmd, n, "%s", E_D_GJ_WHERE);
 return(1);
}

/* What a resume asks jdb first: the owed answers, the breakpoint changes it
   has not been told of, Go to cursor's own breakpoint, the start breakpoint
   before the first run, and for Finish the frames. */
static const E_D_STEP e_d_jdb_go_steps[] =
{
 { e_d_gs_settle_cmd,        e_d_gj_settle_answered },
 { e_d_gb_remove_cmd,        e_d_gb_remove_answered },
 { e_d_gb_add_cmd,           e_d_gb_add_answered },
 { e_d_ga_cursor_set_cmd,    e_d_gs_ignore_answer },
 { e_d_gg_start_cmd,         e_d_gg_start_answered },
 { e_d_gj_finish_frames_cmd, e_d_gj_frames_answered },
 { NULL, NULL }
};

/* ==== a68g's answers, start and resumes, on the fd loop ====

   a68g runs on the same engine as gdb, pdb and jdb: what a stop asks it
   (the watches, the Stack window, the breakpoints changed while it ran, Go
   to cursor's own breakpoint), a breakpoint toggled at a stop, Remove all,
   the set-up after its first prompt and what each resume asks first.  The
   stop itself is read by a68g's run controller table (e_d_a68g_feed), whose
   answer names the line, so no stop asks where it is.  Where a68g differs:
   it names a breakpoint by its line only and takes its breakpoints off only
   all at once, so a removal clears every one and the user's are set again;
   it has no command that only echoes, so its settle marker is a string it
   evaluates; its stack is "calls"; it selects no frame of its own; and it
   writes nothing behind its prompt, nor when an interrupt meets it there
   (e_d_a68g_settle), so its tables have no settle step and no event hook. */

#define E_D_A68G_NAME        "a68g"   /* as the waiting line names it */
#define E_D_A68G_SETTLE_MARK "xwpe-interrupts-settled"
#define E_D_A68G_SETTLE_CMD  "evaluate \"" E_D_A68G_SETTLE_MARK "\""
#define E_D_A68G_BREAK_FMT   "breakpoint %d"
/* a68g's monitor takes "breakpoint N clear" but keeps stopping on line N;
   this one works, for every breakpoint at once. */
#define E_D_A68G_CLEAR_ALL   "breakpoint clear breakpoints"

/* ---- a breakpoint, as a68g is told it ---- */

/**
 * e_d_a68g_brk_add_cmd - a68g's "breakpoint N" for list entry @i: a68g
 * debugs one source, so the line is all it is told.
 * @cmd, @n: the command.
 */
static void e_d_a68g_brk_add_cmd(int i, char *cmd, size_t n)
{
 snprintf(cmd, n, E_D_A68G_BREAK_FMT, e_d_ybrpts[i]);
}

/**
 * e_d_a68g_brk_remove_cmd - a68g's command that takes out a breakpoint the
 * list no longer has: every breakpoint goes, the only removal that stops
 * the program there no more; the user's are set again after it
 * (e_d_ga68_tell_again).
 * @g: the removal (unused); @cmd, @n: the command.
 */
static void e_d_a68g_brk_remove_cmd(const E_D_BRK_GONE *g, char *cmd, size_t n)
{
 (void)g;
 snprintf(cmd, n, "%s", E_D_A68G_CLEAR_ALL);
}

/**
 * e_d_a68g_cursor_brk_cmd - a68g's command that sets (@set 1) Go to
 * cursor's own breakpoint, or clears it with every other one.
 * @cmd, @n: the command.
 */
static void e_d_a68g_cursor_brk_cmd(int set, char *cmd, size_t n)
{
 if (set)
  snprintf(cmd, n, E_D_A68G_BREAK_FMT, e_d_cursor_brk.line);
 else
  snprintf(cmd, n, "%s", E_D_A68G_CLEAR_ALL);
}

/**
 * e_d_ga68_had - Had a68g set list entry @i for the user?
 * Return: 1 for a breakpoint it took (one it refused it never had).
 */
static int e_d_ga68_had(int i)
{
 return(!e_d_brk_is_start_entry(i) && e_d_nrbrpts[i] != 0 &&
        e_d_brk_state[i] != E_D_BRK_REFUSED);
}

/**
 * e_d_ga68_tell_again - a68g has cleared every breakpoint: the removals it
 * was owed are done, and each breakpoint of the user's it had is set again
 * by the add step that follows, so the program goes on stopping on every
 * line the margin marks and on none other.
 */
static void e_d_ga68_tell_again(void)
{
 int i;

 e_d_brk_gone_free();
 for (i = 0; i < e_d_nbrpts; i++)
  if (e_d_ga68_had(i))
  {
   e_d_nrbrpts[i] = 0;
   e_d_brk_set_state(i, E_D_BRK_PENDING, NULL);
  }
}

/**
 * e_d_ga68_cursor_brk_again - The clear took Go to cursor's own breakpoint
 * too while its run is under way (a breakpoint removed during it): it is
 * set again before the program runs on, so the run still stops on the
 * cursor's line.
 */
static void e_d_ga68_cursor_brk_again(void)
{
 if (!e_d_cursor_brk.set)
  return;
 e_d_cursor_brk.set = 0;
 e_d_cursor_brk.wanted = 1;
}

/**
 * e_d_ga68_cursor_brk_goes - At a stop the user is shown, Go to cursor's
 * own breakpoint goes for good: it becomes a removal like the user's, kept
 * until a68g has answered the clear, so a clear given up on is asked again
 * and the line never stops a later run.
 */
static void e_d_ga68_cursor_brk_goes(void)
{
 e_d_cursor_brk.wanted = 0;
 if (!e_d_cursor_brk.set)
  return;
 e_d_cursor_brk.set = 0;
 e_d_gb_gone_keep(e_d_cursor_brk.file, e_d_cursor_brk.line,
                  E_D_GB_ID_UNKNOWN);
}

/* step: the breakpoints the user removed: a68g clears every one, and the
   user's are set again by the add step after it */

static int e_d_ga68_reset_cmd(char *cmd, size_t n, void *ctx)
{
 (void)ctx;
 if (e_d_brk_ngone <= 0)
  return(0);
 e_d_ga.be->brk_remove_cmd(&e_d_brk_gone[0], cmd, n);
 return(1);
}

static int e_d_ga68_reset_answered(E_D_REQ_END how, E_D_ANSWER *ans,
                                   void *ctx)
{
 (void)how;
 (void)ans;
 (void)ctx;
 e_d_ga68_tell_again();
 e_d_ga68_cursor_brk_again();
 return(E_D_STEP_NEXT);
}

/* step: at a stop the user is shown, the removals and Go to cursor's own */

static int e_d_ga68_stop_reset_cmd(char *cmd, size_t n, void *ctx)
{
 e_d_ga68_cursor_brk_goes();
 return(e_d_ga68_reset_cmd(cmd, n, ctx));
}

/* step: Remove all, one clear for every breakpoint */

static int e_d_ga68_clear_all_cmd(char *cmd, size_t n, void *ctx)
{
 (void)ctx;
 snprintf(cmd, n, "%s", E_D_A68G_CLEAR_ALL);
 return(1);
}

static const E_D_STEP e_d_a68g_remove_all_steps[] =
{
 { e_d_ga68_clear_all_cmd, e_d_ga68_reset_answered },
 { NULL, NULL }
};

/**
 * e_d_a68g_remove_all - Remove all breakpoints at a stop under a68g: its one
 * command for it, answered on the fd loop.
 * @cn: the editor.
 */
static void e_d_a68g_remove_all(ECNT *cn)
{
 e_d_gs_reset(&e_d_gs, cn);
 e_d_ga_chain_start(cn, e_d_a68g_remove_all_steps, &e_d_gs,
                    e_d_gs_ask_finished, E_D_GA_ASK);
}

/* ---- the Stack window ---- */

/* step: a68g's stack, when the Stack window is open */

static int e_d_ga68_frames_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 if (!st->stack_wanted)
  return(0);
 snprintf(cmd, n, E_D_A68G_CALLS_FMT, E_D_FRAMES_MAX);
 return(1);
}

static int e_d_ga68_frames_answered(E_D_REQ_END how, E_D_ANSWER *ans,
                                    void *ctx)
{
 (void)how;
 (void)ctx;
 e_d_a68g_take_frames(e_d_ga_editor(), ans->text);
 return(E_D_STEP_NEXT);
}

/* ---- a stop the user is shown ---- */

/* The Stack window and the watches, then the breakpoints changed while the
   program ran and Go to cursor's own. */
static const E_D_STEP e_d_a68g_stop_steps[] =
{
 { e_d_ga68_frames_cmd,     e_d_ga68_frames_answered },
 { e_d_gs_watch_cmd,        e_d_gs_watch_answered },
 { e_d_ga68_stop_reset_cmd, e_d_ga68_reset_answered },
 { e_d_gb_add_cmd,          e_d_gb_add_answered },
 { NULL, NULL }
};

/**
 * e_d_ga68_show_stop - a68g has answered everything the stop asks: the
 * user sees the green bar on the line a68g stopped on, then the Watches
 * and Stack windows with their values.
 * @st: the context.
 */
static void e_d_ga68_show_stop(E_D_GA_CTX *st)
{
 FENSTER *f = e_d_ga_window();

 if (st->start_brk)
  e_mk_brk_main(f, st->start_brk);
 if (st->line > 0)
  e_d_a68g_goto(f, st->line);
 else
  e_d_switch_out(0);
 e_d_gs_show_windows(st);
}

/**
 * e_d_ga68_stop_finished - The answers of an a68g stop have come: show it,
 * say how a Pause ended, then run the next kept key.
 * @ok: 0 when an answer was given up on (the status row already says
 * which): nothing is drawn, the kept keys are dropped, and Go to cursor's
 * own breakpoint, if the stop did not get to clear it, is kept as a
 * removal, so the next request clears it and a later run passes its line.
 * @ctx: the context.
 */
static void e_d_ga68_stop_finished(int ok, void *ctx)
{
 E_D_GA_CTX *st = ctx;
 ECNT *cn = st->cn;

 if (!e_d_ga_chain_ended())
 {
  e_d_ga_actions_drop();
  e_d_gs_free(st);
  return;
 }
 if (!ok)
 {
  e_d_ga68_cursor_brk_goes();
  e_d_ga_actions_drop();
  e_d_gs_free(st);
  return;
 }
 e_d_ga68_show_stop(st);
 e_d_gs_free(st);
 _messages_activated = 0;
 e_refresh();
 e_d_run_stop_tail(cn, e_d_run.ops);
 e_d_act_schedule();
}

/**
 * e_d_ga68_stop_ask - a68g stopped at a line the user is shown: ask it, on
 * the fd loop, what the stop needs, and show the stop once it has answered.
 * @cn: the editor; @line: the line a68g reported, or 0.
 *
 * The keys reach the editor while a68g answers; F7, F8 and Run typed
 * meanwhile are kept and run in order after the stop is shown.
 */
static void e_d_ga68_stop_ask(ECNT *cn, int line)
{
 E_D_GA_CTX *st = &e_d_gs;

 e_d_gs_reset(st, cn);
 st->line = line;
 st->start_brk = e_d_run.start_brk;
 e_d_run.start_brk = 0;
 st->stack_wanted = e_d_gs_stack_window_open(cn);
 e_d_run.stop_on_loop = 1;
 e_d_ga_chain_start(cn, e_d_a68g_stop_steps, st, e_d_ga68_stop_finished,
                    E_D_GA_STOP);
}

/**
 * e_d_a68g_pause_instead - Pause was pressed while a68g answered at a stop
 * the user was not shown: the program is not let run on, and the stop is
 * shown as the Pause's, on the line a68g stopped on.
 * @cn: the editor.
 */
static void e_d_a68g_pause_instead(ECNT *cn)
{
 e_d_async_pending = 0;
 e_d_ga.pause_asked = 0;
 e_d_run.pause = E_D_PAUSE_TAKEN;
 e_d_ga68_stop_ask(cn, e_d_a68g_cur_line);
}

/* ---- a stop made to tell a68g of breakpoint changes ---- */

/* The removals (a clear of every breakpoint, Go to cursor's own set again
   after it while its run is under way), then the additions. */
static const E_D_STEP e_d_a68g_sync_steps[] =
{
 { e_d_ga68_reset_cmd,    e_d_ga68_reset_answered },
 { e_d_gb_add_cmd,        e_d_gb_add_answered },
 { e_d_ga_cursor_set_cmd, e_d_gs_ignore_answer },
 { NULL, NULL }
};

/**
 * e_d_a68g_sync_on_loop - The interrupt for a breakpoint change stopped the
 * program: tell a68g on the fd loop, then let it run on.
 * @cn: the editor.
 * Return: 1, the stop is dealt with.
 */
static int e_d_a68g_sync_on_loop(ECNT *cn)
{
 return(e_d_ga_sync_with(cn, e_d_a68g_sync_steps));
}

/* ---- commands of the user's at a stop, the set-up and the resumes ---- */

/* Every breakpoint of the user's; a set-up given up on and asked again
   first clears a line a68g may have set unanswered. */
static const E_D_STEP e_d_a68g_set_up_steps[] =
{
 { e_d_ga68_reset_cmd, e_d_ga68_reset_answered },
 { e_d_gb_add_cmd,     e_d_gu_add_answered },
 { NULL, NULL }
};

/* A breakpoint set or taken off at a stop. */
static const E_D_STEP e_d_a68g_brk_steps[] =
{
 { e_d_ga68_reset_cmd, e_d_ga68_reset_answered },
 { e_d_gb_add_cmd,     e_d_gb_add_answered },
 { NULL, NULL }
};

/* Add, edit or delete a watch at a stop; Enter on a row of the Stack
   window too: a68g selects no frame of its own, so following one moves
   nothing in a68g and reads the stack and the watches again. */
static const E_D_STEP e_d_a68g_watches_steps[] =
{
 { e_d_ga68_frames_cmd, e_d_ga68_frames_answered },
 { e_d_gs_watch_cmd,    e_d_gs_watch_answered },
 { NULL, NULL }
};

/* Ctrl-G K. */
static const E_D_STEP e_d_a68g_stack_steps[] =
{
 { e_d_ga68_frames_cmd, e_d_ga68_frames_answered },
 { NULL, NULL }
};

/* step: Finish needs the frames, to know the procedure has a caller */

static int e_d_ga68_finish_frames_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_GA_GO *go = ctx;

 if (go->how != E_D_GO_FINISH)
  return(0);
 snprintf(cmd, n, E_D_A68G_CALLS_FMT, E_D_FRAMES_MAX);
 return(1);
}

/* What a resume asks a68g first: the breakpoint changes it has not been
   told of, Go to cursor's own breakpoint, and for Finish the frames. */
static const E_D_STEP e_d_a68g_go_steps[] =
{
 { e_d_ga68_reset_cmd,         e_d_ga68_reset_answered },
 { e_d_gb_add_cmd,             e_d_gb_add_answered },
 { e_d_ga_cursor_set_cmd,      e_d_gs_ignore_answer },
 { e_d_ga68_finish_frames_cmd, e_d_ga68_frames_answered },
 { NULL, NULL }
};

/* ==== sdb's, dbx's and xdb's answers, start and resumes, on the fd loop ====

   sdb, dbx and xdb run on the same engine as gdb: what a stop asks them
   (the file or the place of the stop when their report did not say it, the
   Stack window, the watches, the breakpoints changed while the program ran,
   Go to cursor's and the start's own breakpoints), a breakpoint toggled at
   a stop, Remove all, the set-up after the banner and what each resume asks
   first.  The stop itself is read by their run controller table
   (e_d_legacy_feed), and its report is parsed as it always was
   (e_d_gc_stop_place).  None of the three is installable here, so what the
   editor's source never recorded is assumed and said where it is used.
   Where they differ:

   - sdb names a breakpoint by its line in the file it was last told with
     "e file", so every command that names a line is sent after an "e" of
     its own (e_d_gsdb_file_first), and its stop report names the line
     without the file, which a bare "e" then answers;
   - dbx and xdb name a breakpoint by the number they gave it.  dbx also
     takes one off by its place ("clear"); xdb only by its number or all at
     once, so one whose number was never read goes with "db *" and the
     user's are set again (as a68g's are);
   - sdb's and xdb's stack listing is "t", whose rows name [file:line];
     dbx's is "where"; xdb counts frames the other way, so moving up its
     stack is "down";
   - an interrupt that meets them at their prompt is taken to write nothing
     (the editor never counted one as owed for them), so their tables have
     no settle step; the settle command, a string they print, serves only
     the resync after an answer given up on. */

#define E_D_SDB_NAME          "sdb"   /* as the waiting line names them */
#define E_D_DBX_NAME          "dbx"
#define E_D_XDB_NAME          "xdb"
#define E_D_SDB_PROMPT        "*"
#define E_D_DBX_PROMPT        "(dbx) "
#define E_D_XDB_PROMPT        ">"
#define E_D_CLI_SETTLE_MARK   "xwpe-interrupts-settled"
/* A string each prints when asked: sdb's own command for it, dbx's and
   xdb's print of a string. */
#define E_D_SDB_SETTLE_CMD    "\"" E_D_CLI_SETTLE_MARK "\""
#define E_D_DBX_SETTLE_CMD    "print \"" E_D_CLI_SETTLE_MARK "\""
#define E_D_XDB_SETTLE_CMD    "p \"" E_D_CLI_SETTLE_MARK "\""
#define E_D_SDB_FILE_FMT      "e %s"        /* make a file sdb's current one */
#define E_D_SDB_WHICH_FILE    "e"           /* which file is current */
#define E_D_SDB_BREAK_FMT     "%d b"
#define E_D_SDB_DELETE_FMT    "%d d"
#define E_D_SDB_START_BRK     "b"           /* at the current function's start */
#define E_D_SDB_START_LINE    ':'           /* "breakpoint 1 at main:7" */
#define E_D_DBX_BREAK_FMT     "stop at \"%s\":%d"
#define E_D_DBX_START_FMT     "stop in %s"
#define E_D_DBX_DELETE_FMT    "delete %d"
#define E_D_DBX_CLEAR_FMT     "clear \"%s\":%d"   /* every breakpoint on a line */
#define E_D_XDB_BREAK_FMT     "b %s:%d"
#define E_D_XDB_START_FMT     "b %s"
#define E_D_XDB_DELETE_FMT    "db %d"
#define E_D_XDB_DELETE_ALL    "db *"
#define E_D_XDB_SET_UP        "sm"          /* xdb's own setting at the start */
#define E_D_DBX_RUNNING       "Running:"    /* dbx's answer to a first run */
#define E_D_CLI_EXIT_LEAD     14   /* characters of the end words compared */
#define E_D_CLI_TERM_LEAD     18
#define E_D_CLI_SIGNAL_LEAD   6
#define E_D_CLI_INTR_LEAD     9
#define E_D_CLI_BREAK_LEAD    10
#define E_D_CLI_AT            " at"   /* after sdb's "BREAKPOINT" */
#define E_D_CLI_AT_LEN        3
#define E_D_CLI_STOPPED_LEAD  10
#define E_D_CLI_NOPROC_LEAD   10
#define E_D_CLI_LINE_WORD     " line "   /* dbx's "stopped in f at line N" */
#define E_D_CLI_IN_WORD       "in "      /* sdb's "f() in file" */
#define E_D_CLI_IN_LEN        3

/* What a line of a stop's report said (e_d_gc_report_line). */
#define E_D_GC_NOT_YET  0   /* nothing yet: read the next line */
#define E_D_GC_PLACED   1   /* the place, or how to ask for it, is known */
#define E_D_GC_ENDED    2   /* the program ended: the session is over */

/* What a debugger answers a first run with, before the program runs. */
#define E_D_CLI_RUN_SAYS_NOTHING  0   /* sdb: Messages shows the command */
#define E_D_CLI_RUN_SAYS_RUNNING  1   /* dbx: "Running: ...", or it never ran */
#define E_D_CLI_RUN_SAYS_A_LINE   2   /* xdb: its first line */

/* The words in which sdb, dbx and xdb differ beyond their engine tables. */
typedef struct
{
 const char *stack_cmd;         /* the stack listing: "where" or "t" */
 const char *step_cmd;          /* F7 */
 const char *next_cmd;          /* F8 */
 const char *remove_all_cmd;    /* Remove all */
 int number_on_next_row;        /* a breakpoint's number comes on the row
                                   after the one that announces it (xdb) */
 int run_answer;                /* E_D_CLI_RUN_* */
 const E_D_STEP *stop_steps;    /* a stop the user is shown */
} E_D_CLI;

/* What a first run still owes the run's reader (e_d_cli_run_line_own). */
static struct
{
 int running;                /* dbx's "Running:" line */
 int a_line;                 /* xdb's first line */
} e_d_cli_awaits;

/**
 * e_d_cli_awaits_forget - Expect no such line: the run stopped, or the
 * session ended.
 */
static void e_d_cli_awaits_forget(void)
{
 e_d_cli_awaits.running = 0;
 e_d_cli_awaits.a_line = 0;
}

static const E_D_CLI *e_d_cli_of(int type);   /* (below) */

/**
 * e_d_cli_this - The words of the debugger that runs now (sdb, dbx or xdb).
 */
static const E_D_CLI *e_d_cli_this(void)
{
 return(e_d_cli_of(e_deb_type));
}

/**
 * e_d_cli_window - The window on top, where Messages lines are said.
 */
static FENSTER *e_d_cli_window(void)
{
 ECNT *cn = e_d_run.cn ? e_d_run.cn : WpeEditor;

 return(cn->f[cn->mxedt]);
}

/* ---- a first run, and the line that says it began ---- */

/**
 * e_d_gc_run_went - A run or a Continue went out to sdb, dbx or xdb: for a
 * first run, note the line the debugger answers it with, or, for sdb, which
 * answers with nothing of its own, show the command in Messages, as the
 * user was always shown.
 * @go: the resume.
 */
static void e_d_gc_run_went(const E_D_GA_GO *go)
{
 int says = e_d_cli_this()->run_answer;

 if (go->how != E_D_GO_START)
  return;
 if (says == E_D_CLI_RUN_SAYS_RUNNING)
  e_d_cli_awaits.running = 1;
 else if (says == E_D_CLI_RUN_SAYS_A_LINE)
  e_d_cli_awaits.a_line = 1;
 else
  e_d_p_message((char *)go->cmd, e_d_cli_window(), 0);
}

/**
 * e_d_cli_run_line_own - The run's reader found @line: is it the line that
 * says a first run began, or one before it?
 *
 * The user reads dbx's "Running: ..." or xdb's first line in Messages as
 * soon as the debugger says it, as before; dbx's lines before "Running:"
 * are dropped, as its reader always dropped them, and none of these is
 * taken for part of the stop.
 * Return: 1 when the line was taken here.
 */
static int e_d_cli_run_line_own(const char *line)
{
 if (e_d_cli_awaits.a_line && line[0])
 {
  e_d_cli_awaits.a_line = 0;
  e_d_p_message((char *)line, e_d_cli_window(), 0);
  return(1);
 }
 if (!e_d_cli_awaits.running)
  return(0);
 if (!strncmp(line, E_D_DBX_RUNNING, sizeof E_D_DBX_RUNNING - 1))
 {
  e_d_cli_awaits.running = 0;
  e_d_p_message((char *)line, e_d_cli_window(), 0);
 }
 return(1);
}

/**
 * e_d_cli_run_never_started - dbx came back to its prompt after "run"
 * without saying "Running:": the program could not be started.
 * @f: the window on top.
 *
 * The user is told so and the session ends, instead of being shown a stop
 * of a program that never ran.
 * Return: 1 when the session ended.
 */
static int e_d_cli_run_never_started(FENSTER *f)
{
 int never = e_d_cli_awaits.running;

 e_d_cli_awaits_forget();
 if (!never)
  return(0);
 e_d_quit(f);
 e_error(e_d_msg[ERR_CANTPROG], 0, f->fb);
 return(1);
}

/* ---- the file sdb is told before a command that names a line ---- */

/* The file last told to sdb with "e file", and whether sdb has answered it:
   the command that names a line in it goes next. */
static struct
{
 char file[PATH_MAX];
 int asked;                  /* "e file" went out and is not answered */
 int told;                   /* sdb answered it: the line's command is next */
} e_d_gsdb;

/**
 * e_d_gsdb_file_first - Before sdb is sent a command on a line of @file:
 * the "e file" that makes it the current file, unless sdb has just answered
 * that one, whose turn is then over.
 * @cmd, @n: the command, when one is to go first.
 * Return: 1 when @cmd holds the "e file" to send now, 0 when the command
 * on the line goes now.
 */
static int e_d_gsdb_file_first(const char *file, char *cmd, size_t n)
{
 if (e_d_gsdb.told && !strcmp(e_d_gsdb.file, file))
 {
  e_d_gsdb.told = 0;
  return(0);
 }
 snprintf(e_d_gsdb.file, sizeof e_d_gsdb.file, "%s", file);
 e_d_gsdb.asked = 1;
 e_d_gsdb.told = 0;
 snprintf(cmd, n, E_D_SDB_FILE_FMT, file);
 return(1);
}

/**
 * e_d_gsdb_file_answered - Was the answer just come sdb's answer to the
 * "e file" e_d_gsdb_file_first sent?  Then the command on the line follows.
 * Return: 1 when it was.
 */
static int e_d_gsdb_file_answered(void)
{
 if (!e_d_gsdb.asked)
  return(0);
 e_d_gsdb.asked = 0;
 e_d_gsdb.told = 1;
 return(1);
}

/**
 * e_d_gsdb_gone_file - The file a removal names: the start breakpoint has
 * none, and sdb is told the start symbol for it, as it was when it was set.
 */
static const char *e_d_gsdb_gone_file(const E_D_BRK_GONE *g)
{
 if (g->file[0])
  return(g->file);
 return(e_get_start_symbol());
}

/* ---- a breakpoint, as each of the three is told it ---- */

/**
 * e_d_sdb_brk_add_cmd - sdb's "N b" for list entry @i, once sdb has been
 * told its file.
 * @cmd, @n: the command.
 */
static void e_d_sdb_brk_add_cmd(int i, char *cmd, size_t n)
{
 snprintf(cmd, n, E_D_SDB_BREAK_FMT, e_d_ybrpts[i]);
}

/**
 * e_d_sdb_brk_remove_cmd - sdb's "N d" for a breakpoint the list no longer
 * has, once sdb has been told its file: sdb takes it off by its line.
 * @g: the removal; @cmd, @n: the command.
 */
static void e_d_sdb_brk_remove_cmd(const E_D_BRK_GONE *g, char *cmd, size_t n)
{
 snprintf(cmd, n, E_D_SDB_DELETE_FMT, g->line);
}

/**
 * e_d_sdb_cursor_brk_cmd - sdb's command that sets (@set 1) or clears Go to
 * cursor's own breakpoint, once sdb has been told its file.
 * @cmd, @n: the command.
 */
static void e_d_sdb_cursor_brk_cmd(int set, char *cmd, size_t n)
{
 if (set)
  snprintf(cmd, n, E_D_SDB_BREAK_FMT, e_d_cursor_brk.line);
 else
  snprintf(cmd, n, E_D_SDB_DELETE_FMT, e_d_cursor_brk.line);
}

/**
 * e_d_dbx_brk_add_cmd - dbx's stop at "file":line for list entry @i.
 * @cmd, @n: the command.
 */
static void e_d_dbx_brk_add_cmd(int i, char *cmd, size_t n)
{
 snprintf(cmd, n, E_D_DBX_BREAK_FMT, e_d_sbrpts[i], e_d_ybrpts[i]);
}

/**
 * e_d_dbx_brk_remove_cmd - dbx's command that takes out a breakpoint the
 * list no longer has: "delete N" by the number dbx gave it, or clear
 * "file":line for one whose number was never read.
 * @g: the removal; @cmd, @n: the command.
 */
static void e_d_dbx_brk_remove_cmd(const E_D_BRK_GONE *g, char *cmd, size_t n)
{
 if (g->id == E_D_GB_ID_UNKNOWN)
  snprintf(cmd, n, E_D_DBX_CLEAR_FMT, g->file, g->line);
 else
  snprintf(cmd, n, E_D_DBX_DELETE_FMT, g->id);
}

/**
 * e_d_dbx_start_brk_cmd - dbx's breakpoint on the program's start symbol,
 * for a first run when the user has none of his own.
 * @cmd, @n: the command.
 */
static void e_d_dbx_start_brk_cmd(char *cmd, size_t n)
{
 snprintf(cmd, n, E_D_DBX_START_FMT, e_get_start_symbol());
}

/**
 * e_d_dbx_cursor_brk_cmd - dbx's command that sets (@set 1) or clears Go to
 * cursor's own breakpoint.
 * @cmd, @n: the command.
 */
static void e_d_dbx_cursor_brk_cmd(int set, char *cmd, size_t n)
{
 if (set)
  snprintf(cmd, n, E_D_DBX_BREAK_FMT, e_d_cursor_brk.file,
           e_d_cursor_brk.line);
 else
  snprintf(cmd, n, E_D_DBX_DELETE_FMT, e_d_cursor_brk.id);
}

/**
 * e_d_xdb_brk_add_cmd - xdb's "b file:line" for list entry @i.
 * @cmd, @n: the command.
 */
static void e_d_xdb_brk_add_cmd(int i, char *cmd, size_t n)
{
 snprintf(cmd, n, E_D_XDB_BREAK_FMT, e_d_sbrpts[i], e_d_ybrpts[i]);
}

/**
 * e_d_xdb_brk_remove_cmd - xdb's command that takes out a breakpoint the
 * list no longer has: "db N" by the number xdb gave it.  xdb takes one off
 * only by its number, so one whose number was never read goes with all the
 * others ("db *"), and the user's are set again (e_d_gx_remove_answered).
 * @g: the removal; @cmd, @n: the command.
 */
static void e_d_xdb_brk_remove_cmd(const E_D_BRK_GONE *g, char *cmd, size_t n)
{
 if (g->id == E_D_GB_ID_UNKNOWN)
  snprintf(cmd, n, "%s", E_D_XDB_DELETE_ALL);
 else
  snprintf(cmd, n, E_D_XDB_DELETE_FMT, g->id);
}

/**
 * e_d_xdb_start_brk_cmd - xdb's breakpoint on the program's start symbol.
 * @cmd, @n: the command.
 */
static void e_d_xdb_start_brk_cmd(char *cmd, size_t n)
{
 snprintf(cmd, n, E_D_XDB_START_FMT, e_get_start_symbol());
}

/**
 * e_d_xdb_cursor_brk_cmd - xdb's command that sets (@set 1) or clears Go to
 * cursor's own breakpoint.
 * @cmd, @n: the command.
 */
static void e_d_xdb_cursor_brk_cmd(int set, char *cmd, size_t n)
{
 if (set)
  snprintf(cmd, n, E_D_XDB_BREAK_FMT, e_d_cursor_brk.file,
           e_d_cursor_brk.line);
 else
  snprintf(cmd, n, E_D_XDB_DELETE_FMT, e_d_cursor_brk.id);
}

/**
 * e_d_gc_number_line - The row of dbx's or xdb's answer to a breakpoint set
 * that gives the breakpoint's number, copied into @out: dbx's "(N) stop
 * at ...", the row after xdb's "Added:".
 * Return: the number, or 0 when no row gives one.
 */
static int e_d_gc_number_line(const char *text, char *out, size_t n)
{
 const char *p = text ? text : "";
 int next = e_d_cli_this()->number_on_next_row;
 int announced = 0, id = 0;
 char *row;

 out[0] = '\0';
 while (!id && *p && (row = e_d_text_take_row(&p)) != NULL)
 {
  if (announced || (!next && e_ds_brk_is_number_line(e_deb_type, row)))
  {
   id = e_ds_brk_number(e_deb_type, row);
   snprintf(out, n, "%s", row);
  }
  else if (e_ds_brk_is_number_line(e_deb_type, row))
   announced = 1;
  FREE(row);
 }
 return(id);
}

/**
 * e_d_gc_brk_set_id - The number dbx or xdb gave the breakpoint its answer
 * names, or 0 when it gave none (it refused).
 */
static int e_d_gc_brk_set_id(const E_D_ANSWER *ans)
{
 char str[E_D_BRK_ANSWER_MAX];

 return(e_d_gc_number_line(ans->text, str, sizeof str));
}

/**
 * e_d_gc_brk_verdict - Keep with list entry @i what dbx or xdb answered:
 * the number @id it gave, or its words when it refused.
 * @ans: the answer.
 */
static void e_d_gc_brk_verdict(int i, int id, const E_D_ANSWER *ans)
{
 char str[E_D_BRK_ANSWER_MAX];

 if (id > 0)
 {
  e_d_gc_number_line(ans->text, str, sizeof str);
  e_d_gb_take_verdict(i, id, 0, str);
 }
 else if (ans->error)
 {
  e_d_gb_first_words(ans->text, str, sizeof str);
  e_d_gb_take_verdict(i, 0, E_D_READ_ERROR, str);
 }
 else
  e_d_gb_take_verdict(i, 0, E_D_READ_PROMPT, "");
}

/**
 * e_d_gsdb_brk_set_id - Did sdb take the breakpoint its answer is for?
 * Its refusals come on its error stream, or in words its answer says.
 * Return: E_D_GP_ID_SET when it did, 0 when it refused.
 */
static int e_d_gsdb_brk_set_id(const E_D_ANSWER *ans)
{
 if (ans->error)
  return(0);
 return(e_d_place_brk_set_id(ans));
}

/**
 * e_d_gsdb_brk_verdict - Keep with list entry @i what sdb answered: the
 * mark turns solid, or stays hollow with sdb's words.
 * @id: as e_d_place_brk_verdict; @ans: the answer.
 */
static void e_d_gsdb_brk_verdict(int i, int id, const E_D_ANSWER *ans)
{
 char str[E_D_BRK_ANSWER_MAX];

 if (!ans->error)
 {
  e_d_place_brk_verdict(i, id, ans);
  return;
 }
 e_d_gb_first_words(ans->text, str, sizeof str);
 e_d_gb_take_verdict(i, 0, E_D_READ_ERROR, str);
}

/* step: one breakpoint the user set, sdb told its file first */

static int e_d_gsdb_add_cmd(char *cmd, size_t n, void *ctx)
{
 int i = e_d_gb_next_unsent();

 if (i < 0)
  return(0);
 if (e_d_gsdb_file_first(e_d_sbrpts[i], cmd, n))
  return(1);
 return(e_d_gb_add_cmd(cmd, n, ctx));
}

static int e_d_gsdb_add_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 if (e_d_gsdb_file_answered())
  return(E_D_STEP_AGAIN);
 return(e_d_gb_add_answered(how, ans, ctx));
}

/* ... as the session starts: only a refusal is news */

static int e_d_gsdb_set_up_add_answered(E_D_REQ_END how, E_D_ANSWER *ans,
                                        void *ctx)
{
 if (e_d_gsdb_file_answered())
  return(E_D_STEP_AGAIN);
 return(e_d_gu_add_answered(how, ans, ctx));
}

/* step: one breakpoint the user removed, sdb told its file first */

static int e_d_gsdb_remove_cmd(char *cmd, size_t n, void *ctx)
{
 if (e_d_brk_ngone <= 0)
  return(0);
 if (e_d_gsdb_file_first(e_d_gsdb_gone_file(&e_d_brk_gone[0]), cmd, n))
  return(1);
 return(e_d_gb_remove_cmd(cmd, n, ctx));
}

static int e_d_gsdb_remove_answered(E_D_REQ_END how, E_D_ANSWER *ans,
                                    void *ctx)
{
 if (e_d_gsdb_file_answered())
  return(E_D_STEP_AGAIN);
 return(e_d_gb_remove_answered(how, ans, ctx));
}

/* step: sdb's breakpoint at the start of the start symbol, before a first
   run when the user has none of his own: "e main", then "b", which names
   the line it set it on */

static int e_d_gsdb_start_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_GA_GO *go = ctx;

 if (go->how != E_D_GO_START || e_d_gg_start_entry() < 0)
  return(0);
 if (e_d_gsdb_file_first(e_get_start_symbol(), cmd, n))
  return(1);
 snprintf(cmd, n, "%s", E_D_SDB_START_BRK);
 return(1);
}

/**
 * e_d_gsdb_start_line - The line sdb's answer to "b" names, after its colon.
 * Return: the line, or 0 when the answer names none (sdb refused).
 */
static int e_d_gsdb_start_line(const E_D_ANSWER *ans)
{
 const char *colon;

 if (ans->error || !ans->text)
  return(0);
 colon = strchr(ans->text, E_D_SDB_START_LINE);
 if (!colon)
  return(0);
 return(atoi(colon + 1));
}

static int e_d_gsdb_start_answered(E_D_REQ_END how, E_D_ANSWER *ans,
                                   void *ctx)
{
 char str[E_D_BRK_ANSWER_MAX];
 int i, line;

 (void)how;
 (void)ctx;
 if (e_d_gsdb_file_answered())
  return(E_D_STEP_AGAIN);
 i = e_d_gg_start_entry();
 line = e_d_gsdb_start_line(ans);
 if (i >= 0 && line > 0)
 {
  e_d_ybrpts[i] = line;
  e_d_nrbrpts[i] = E_D_GP_ID_SET;
 }
 if (line <= 0 && ans->error)
 {
  e_d_gb_first_words(ans->text, str, sizeof str);
  e_d_error(str);
 }
 return(E_D_STEP_NEXT);
}

/* step: Go to cursor's own breakpoint, just before the Continue; dbx and
   xdb give it a number, which is kept so it is taken out again by it */

static int e_d_gc_cursor_set_answered(E_D_REQ_END how, E_D_ANSWER *ans,
                                      void *ctx)
{
 (void)how;
 (void)ctx;
 e_d_cursor_brk.id = e_d_ga.be->brk_set_id(ans);
 return(E_D_STEP_NEXT);
}

/* ... sdb's, told its file first */

static int e_d_gsdb_cursor_set_cmd(char *cmd, size_t n, void *ctx)
{
 if (!e_d_cursor_brk.wanted)
  return(0);
 if (e_d_gsdb_file_first(e_d_cursor_brk.file, cmd, n))
  return(1);
 return(e_d_ga_cursor_set_cmd(cmd, n, ctx));
}

static int e_d_gsdb_cursor_set_answered(E_D_REQ_END how, E_D_ANSWER *ans,
                                        void *ctx)
{
 (void)how;
 (void)ans;
 (void)ctx;
 if (e_d_gsdb_file_answered())
  return(E_D_STEP_AGAIN);
 return(E_D_STEP_NEXT);
}

/* step: xdb's removal; one whose number was never read takes every
   breakpoint off, and the user's are set again by the add step after it */

static int e_d_gx_clearing_all;   /* the removal out is xdb's "db *" */

static int e_d_gx_remove_cmd(char *cmd, size_t n, void *ctx)
{
 e_d_gx_clearing_all = 0;
 if (e_d_brk_ngone <= 0)
  return(0);
 if (e_d_brk_gone[0].id != E_D_GB_ID_UNKNOWN)
  return(e_d_gb_remove_cmd(cmd, n, ctx));
 e_d_gx_clearing_all = 1;
 e_d_ga.be->brk_remove_cmd(&e_d_brk_gone[0], cmd, n);
 return(1);
}

static int e_d_gx_remove_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 if (!e_d_gx_clearing_all)
  return(e_d_gb_remove_answered(how, ans, ctx));
 e_d_gx_clearing_all = 0;
 e_d_ga68_tell_again();
 e_d_ga68_cursor_brk_again();
 return(E_D_STEP_NEXT);
}

/* step: xdb's own setting, sent once its banner is answered */

static int e_d_gx_set_up_cmd(char *cmd, size_t n, void *ctx)
{
 (void)ctx;
 snprintf(cmd, n, "%s", E_D_XDB_SET_UP);
 return(1);
}

/**
 * e_d_gc_remove_all - Remove all breakpoints at a stop under sdb, dbx or
 * xdb: the one command each has for it, answered on the fd loop.
 * @cn: the editor.
 */
static void e_d_gc_remove_all(ECNT *cn)
{
 e_d_ga_ask_plain(cn, e_d_cli_this()->remove_all_cmd);
}

/* ---- the Stack window, and moving in it ---- */

/* step: the stack listing, when the Stack window is open */

static int e_d_gc_stack_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 if (!st->stack_wanted)
  return(0);
 snprintf(cmd, n, "%s", e_d_cli_this()->stack_cmd);
 return(1);
}

/* step: move xdb's selected frame to the row followed in the Stack window:
   xdb counts its frames the other way, so up the stack is "down" */

static int e_d_gx_move_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 if (st->frame > e_d_nstack)
  snprintf(cmd, n, E_D_GDB_DOWN_FMT, st->frame - e_d_nstack);
 else if (st->frame < e_d_nstack)
  snprintf(cmd, n, E_D_GDB_UP_FMT, e_d_nstack - st->frame);
 else
  return(0);
 return(1);
}

/* ---- where a stop is ---- */

/* step: the file of a stop whose report gave sdb's line alone */

static int e_d_gsdb_which_file_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 if (!st->need_file)
  return(0);
 snprintf(cmd, n, "%s", E_D_SDB_WHICH_FILE);
 return(1);
}

static int e_d_gsdb_which_file_answered(E_D_REQ_END how, E_D_ANSWER *ans,
                                        void *ctx)
{
 E_D_GA_CTX *st = ctx;

 (void)how;
 e_ds_sdb_file_of(ans->text ? ans->text : "", st->file, sizeof st->file);
 return(E_D_STEP_NEXT);
}

/* step: where the program stopped, from sdb's or xdb's "t" listing, when
   the stop's own lines did not say */

static int e_d_gc_t_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 if (!st->need_where)
  return(0);
 snprintf(cmd, n, "%s", e_d_cli_this()->stack_cmd);
 return(1);
}

/**
 * e_d_gc_t_row - Read one row of the "t" listing.
 * @st: the context; @row: the row.
 * Return: 1 when the reading is over (the place is found, or there is no
 * process), 0 to read the next row.
 */
static int e_d_gc_t_row(E_D_GA_CTX *st, char *row)
{
 char file[E_D_LINE_MAX];
 int line;

 st->where_rows++;
 if (!strncmp(row, e_d_msg[ERR_NOPROCESS], E_D_CLI_NOPROC_LEAD))
 {
  st->no_stack = 1;
  return(1);
 }
 if ((line = e_make_line_num2(row, file)) < 0)
  return(0);
 st->line = line;
 snprintf(st->file, sizeof st->file, "%s", file);
 return(1);
}

static int e_d_gc_t_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 E_D_GA_CTX *st = ctx;
 const char *p = ans->text ? ans->text : "";
 char *row;
 int over = 0;

 (void)how;
 while (!over && *p && (row = e_d_text_take_row(&p)) != NULL)
 {
  over = e_d_gc_t_row(st, row);
  FREE(row);
 }
 if (st->no_stack)
  return(E_D_STEP_STOP);
 return(E_D_STEP_NEXT);
}

/**
 * e_d_gc_ask_where - The stop's report named no place: show what it said
 * (a signal), then ask the debugger's stack listing where it is.
 * @st: the context; @said: the report's words, or "" for none.
 * Return: E_D_GC_PLACED.
 */
static int e_d_gc_ask_where(E_D_GA_CTX *st, char *said)
{
 if (said && said[0])
  e_d_error(said);
 st->need_where = 1;
 return(E_D_GC_PLACED);
}

/**
 * e_d_gc_said_exit - Does report line @i say the program ended, in the
 * words sdb, dbx or xdb use for it?
 */
static int e_d_gc_said_exit(int i)
{
 if (e_deb_type != DEB_DBX &&
     !strncmp(e_d_sp[i], e_d_msg[ERR_PROGEXIT], E_D_CLI_EXIT_LEAD))
  return(1);
 if (e_deb_type == DEB_DBX &&
     !strncmp(e_d_sp[i], e_d_msg[ERR_PROGEXIT2], E_D_CLI_EXIT_LEAD))
  return(1);
 return(e_deb_type == DEB_XDB &&
        !strncmp(e_d_sp[i], e_d_msg[ERR_NORMALTERM],
                 strlen(e_d_msg[ERR_NORMALTERM])));
}

/**
 * e_d_gc_signal_words - Does report line @i say a signal stopped the
 * program, sdb's, dbx's or xdb's way?
 * Return: the words to show (empty for Pause's own interrupt), or NULL.
 */
static char *e_d_gc_signal_words(int i)
{
 if (e_deb_type == DEB_XDB && e_d_pause_asked_for(e_d_sp[i]))
  return(e_d_pause_signal_line(e_d_sp[i]));
 if (e_deb_type == DEB_XDB &&
     !strncmp(e_d_sp[i], e_d_msg[ERR_SOFTTERM],
              strlen(e_d_msg[ERR_SOFTTERM])))
  return(e_d_sp[i]);
 if (e_deb_type == DEB_DBX &&
     (!strncmp(e_d_sp[i], e_d_msg[ERR_SIGNAL], E_D_CLI_SIGNAL_LEAD) ||
      !strncmp(e_d_sp[i], e_d_msg[ERR_INTERRUPT], E_D_CLI_INTR_LEAD)))
  return(e_d_pause_signal_line(e_d_sp[i]));
 if (e_deb_type == DEB_SDB && strstr(e_d_sp[i], e_d_msg[ERR_SIGNAL]))
  return(e_d_pause_signal_line(e_d_sp[i]));
 if (e_deb_type == DEB_XDB && i == SVLINES - 2 && i > 0 &&
     strstr(e_d_sp[i], ": "))
  return(e_d_sp[i - 1]);
 return(NULL);
}

/**
 * e_d_gc_said_breakpoint - Does report line @i start with a breakpoint's
 * word?  Its place, under sdb, is on the next line.
 */
static int e_d_gc_said_breakpoint(int i)
{
 if (!strncmp(e_d_sp[i], e_d_msg[ERR_BREAKPOINT], E_D_CLI_BREAK_LEAD))
  return(1);
 return(e_deb_type == DEB_SDB &&
        !strncmp(e_d_sp[i], e_d_msg[ERR_BREAKPOINT2], E_D_CLI_BREAK_LEAD));
}

/**
 * e_d_gc_sdb_breakpoint - sdb's "BREAKPOINT at ..." on report line @i, with
 * the line on the next one; its file is asked with "e" (need_file).
 * @st: receives the line.
 * Return: E_D_GC_PLACED, or E_D_GC_NOT_YET when it is not that.
 */
static int e_d_gc_sdb_breakpoint(E_D_GA_CTX *st, int i)
{
 char file[E_D_LINE_MAX];
 int line;

 if (e_deb_type != DEB_SDB ||
     strncmp(e_d_sp[i] + E_D_CLI_BREAK_LEAD, E_D_CLI_AT, E_D_CLI_AT_LEN))
  return(E_D_GC_NOT_YET);
 line = e_ds_line_num(E_DS_SDB, e_d_sp[i + 1], file, sizeof file);
 if (line < 0)
  return(E_D_GC_NOT_YET);
 st->line = line;
 st->need_file = 1;
 return(E_D_GC_PLACED);
}

/**
 * e_d_gc_dbx_file_of - The file between the last two quotes of dbx's
 * "... line N in file \"p.c\"", into @file; left as it is when there are
 * none.
 */
static void e_d_gc_dbx_file_of(const char *row, char *file, size_t n)
{
 int k, l;

 for (k = strlen(row); k >= 0 && row[k] != '\"'; k--)
  ;
 for (k--; k >= 0 && row[k] != '\"'; k--)
  ;
 if (k < 0)
  return;
 for (k++, l = 0; row[k] != '\0' && row[k] != '\"' && l < (int)n - 1;
      k++, l++)
  file[l] = row[k];
 file[l] = '\0';
}

/**
 * e_d_gc_dbx_stopped_in - dbx's "stopped in f at line N in file ..." on
 * report line @i: the line is the number the next rows start with, the
 * file is in the row before that one.
 * @st: receives the place.
 * Return: E_D_GC_PLACED, or E_D_GC_NOT_YET when no row names it.
 */
static int e_d_gc_dbx_stopped_in(E_D_GA_CTX *st, int i)
{
 int j, k, line;

 for (j = i + 1; j < SVLINES - 1; j++)
 {
  for (k = 0; e_d_sp[j][k] == ' '; k++)
   ;
  if ((line = atoi(e_d_sp[j] + k)) <= 0)
   continue;
  if (!strstr(e_d_sp[j - 1], E_D_CLI_LINE_WORD))
   return(E_D_GC_NOT_YET);
  e_d_gc_dbx_file_of(e_d_sp[j - 1], st->file, sizeof st->file);
  st->line = line;
  return(E_D_GC_PLACED);
 }
 return(E_D_GC_NOT_YET);
}

/**
 * e_d_gc_report_line - Read report line @i of a stop, in the order the
 * checks of sdb, dbx and xdb have always run.
 * @st: receives the place, or need_where; @f: the window on top.
 * Return: E_D_GC_NOT_YET, E_D_GC_PLACED or E_D_GC_ENDED.
 */
static int e_d_gc_report_line(E_D_GA_CTX *st, FENSTER *f, int i)
{
 char *said;

 if (e_d_gc_said_exit(i))
 {
  e_d_error(e_d_sp[i]);
  e_d_quit(f);
  return(E_D_GC_ENDED);
 }
 if (e_deb_type == DEB_SDB &&
     !strncmp(e_d_sp[i], e_d_msg[ERR_PROGTERM], E_D_CLI_TERM_LEAD))
 {
  e_error(e_d_msg[ERR_PROGTERM], 0, f->fb);
  e_d_quit(f);
  return(E_D_GC_ENDED);
 }
 if ((said = e_d_gc_signal_words(i)) != NULL)
  return(e_d_gc_ask_where(st, said));
 if (e_d_gc_said_breakpoint(i))
  return(e_d_gc_sdb_breakpoint(st, i));
 if (e_deb_type == DEB_DBX &&
     !strncmp(e_d_sp[i], e_d_msg[ERR_STOPPEDIN], E_D_CLI_STOPPED_LEAD))
  return(e_d_gc_dbx_stopped_in(st, i));
 return(E_D_GC_NOT_YET);
}

/**
 * e_d_gc_sdb_in_file - The file of sdb's "f() in file" row @row, trimmed,
 * when the row has that shape.
 * @file, @n: receives it.
 * Return: 1 when it does.
 */
static int e_d_gc_sdb_in_file(const char *row, char *file, size_t n)
{
 int j, k;

 for (j = strlen(row) - 1; j >= 0 && isspace((unsigned char)row[j]); j--)
  ;
 if (j < 0)
  return(0);
 for (j--; j >= 0 && !isspace((unsigned char)row[j]); j--)
  ;
 if (j < 0)
  return(0);                   /* one word: no "in file" */
 for (j--; j >= 0 && isspace((unsigned char)row[j]); j--)
  ;
 if (j < 0)
  return(0);                   /* one word after blanks: no "in file" */
 for (j--; j >= 0 && !isspace((unsigned char)row[j]); j--)
  ;
 if (strncmp(row + j + 1, E_D_CLI_IN_WORD, E_D_CLI_IN_LEN))
  return(0);
 snprintf(file, n, "%s", row + j + 1 + E_D_CLI_IN_LEN);
 for (k = strlen(file) - 1; k >= 0 && isspace((unsigned char)file[k]); k--)
  ;
 file[k + 1] = '\0';
 return(1);
}

/**
 * e_d_gc_sdb_step_place - sdb's source row after a step, "N: ..." on report
 * line @i: its file is in the nearest row above that reads "f() in file".
 * @st: receives the place.
 * Return: 1 when it was found.
 */
static int e_d_gc_sdb_step_place(E_D_GA_CTX *st, int i)
{
 int line = atoi(e_d_sp[i]);

 for (; i > 0; i--)
  if (e_d_gc_sdb_in_file(e_d_sp[i - 1], st->file, sizeof st->file))
  {
   st->line = line;
   return(1);
  }
 return(0);
}

/**
 * e_d_gc_step_place - The place a step's report names in its source row:
 * sdb's "N: ..." under its "f() in file", xdb's "file:f:N" last.
 * @st: receives the place.
 * Return: 1 when one was found.
 */
static int e_d_gc_step_place(E_D_GA_CTX *st)
{
 char file[E_D_LINE_MAX];
 int i, line;

 for (i = SVLINES - 2; i >= 0; i--)
 {
  if (e_deb_type == DEB_SDB && atoi(e_d_sp[i]) > 0)
   return(e_d_gc_sdb_step_place(st, i));
  if (e_deb_type == DEB_XDB && i == SVLINES - 2 &&
      (line = e_ds_line_num(E_DS_XDB, e_d_sp[i], file, sizeof file)) >= 0)
  {
   st->line = line;
   snprintf(st->file, sizeof st->file, "%s", file);
   return(1);
  }
 }
 return(0);
}

/**
 * e_d_gc_stop_place - Read the stop's place from sdb's, dbx's or xdb's
 * report, as it has always been read: an end ends the session, a signal is
 * shown and its place is asked of the stack listing, a breakpoint or a step
 * names its line; a report that names nothing asks the listing too.
 * @st: receives the place, need_file or need_where; @f: the window on top.
 * Return: 1 when the session ended.
 */
static int e_d_gc_stop_place(E_D_GA_CTX *st, FENSTER *f)
{
 int i, said;

 snprintf(st->file, sizeof st->file, "%s", e_d_file);
 for (i = 0; i < SVLINES - 1; i++)
 {
  said = e_d_gc_report_line(st, f, i);
  if (said == E_D_GC_ENDED)
   return(1);
  if (said == E_D_GC_PLACED)
   return(0);
 }
 if (!e_d_gc_step_place(st))
  st->need_where = 1;
 return(0);
}

/**
 * e_d_gc_stop_ended - Did a line of the report say the program ended?
 * Then say so in Messages and end the session.
 * @f: the window on top.
 * Return: 1 when the session ended.
 */
static int e_d_gc_stop_ended(FENSTER *f)
{
 int i;

 for (i = 0; i < SVLINES; i++)
  if (e_d_line_says_program_exited(e_d_sp[i]))
  {
   e_d_say_program_exited(e_d_sp[i], f);
   e_d_quit(f);
   return(1);
  }
 return(0);
}

/* ---- a stop the user is shown ---- */

/**
 * e_d_gc_start_brk_goes - The stop is the first run's: the editor's start
 * breakpoint goes from the list, and is kept as a removal the stop's own
 * requests tell the debugger, so a later run does not stop there again.
 */
static void e_d_gc_start_brk_goes(void)
{
 int sw = e_d_run.start_brk;

 e_d_run.start_brk = 0;
 if (sw <= 0 || sw > e_d_nbrpts || !e_d_brk_is_start_entry(sw - 1))
  return;
 e_d_ga_drop_start_entry(sw - 1);
}

/**
 * e_d_gc_cursor_brk_goes - At a stop, Go to cursor's own breakpoint goes:
 * it becomes a removal like the user's, by the number dbx or xdb gave it,
 * or by its place when none was read (sdb's always).  A removal by place
 * would take the user's own breakpoint on that line too, so it is left
 * there when the user has one: his keeps stopping the program.
 */
static void e_d_gc_cursor_brk_goes(void)
{
 int id = e_d_cursor_brk.id;

 e_d_cursor_brk.wanted = 0;
 if (!e_d_cursor_brk.set)
  return;
 e_d_cursor_brk.set = 0;
 if (id == E_D_GB_ID_UNKNOWN &&
     e_d_user_brk_on(e_d_cursor_brk.file, e_d_cursor_brk.line))
  return;
 e_d_gb_gone_keep(e_d_cursor_brk.file, e_d_cursor_brk.line, id);
}

/**
 * e_d_cli_stopped - sdb, dbx or xdb stopped the program: ask it, on the fd
 * loop, what the stop needs, and show the stop once it has answered.
 * @f: the window on top.
 *
 * The user sees the program's last output at once; the mark, the watches
 * and the stack come when the debugger answers, the keys reaching the
 * editor meanwhile.  The breakpoints the editor set for this run alone go
 * with the stop's own requests.
 */
static void e_d_cli_stopped(FENSTER *f)
{
 E_D_GA_CTX *st = &e_d_gs;

 if (e_d_cli_run_never_started(f))
  return;
 e_d_drain_pty_to_messages();
 e_d_out_show();
 e_d_switch_out(0);
 if (e_d_gc_stop_ended(f))
  return;
 e_d_gs_reset(st, f->ed);
 e_d_gc_start_brk_goes();
 e_d_gc_cursor_brk_goes();
 st->stack_wanted = e_d_gs_stack_window_open(f->ed);
 if (e_d_gc_stop_place(st, f))
  return;
 e_d_run.stop_on_loop = 1;
 e_d_ga_chain_start(f->ed, e_d_cli_this()->stop_steps, st,
                    e_d_gs_stop_finished, E_D_GA_STOP);
}

/**
 * e_d_gc_pause_instead - Pause was pressed while the debugger answered at a
 * stop the user was not shown: the stop is shown as the Pause's.  sdb, dbx
 * and xdb are never stopped for a breakpoint change, so this comes only
 * from the engine's shared resume.
 * @cn: the editor.
 */
static void e_d_gc_pause_instead(ECNT *cn)
{
 e_d_async_pending = 0;
 e_d_ga.pause_asked = 0;
 e_d_run.pause = E_D_PAUSE_ASKED;
 e_d_cli_stopped(e_d_run_window(cn));
}

/**
 * e_d_cli_step_go - F7 or F8 under sdb, dbx or xdb, once the debugger has
 * answered what it still owes (the breakpoints changed since the stop).
 * @f: the source window; @sw: 1 for Step Over, 0 for Step Into.
 *
 * The user keeps the editor while the debugger answers; the step goes out
 * after, and a key typed meanwhile waits for its stop.
 * Return: 0, or the end of the session.
 */
static int e_d_cli_step_go(FENSTER *f, int sw)
{
 const E_D_CLI *w = e_d_cli_this();

 e_d_nstack = 0;
 if (sw)
  return(e_d_ga_go_step(f, w->next_cmd, NULL));
 return(e_d_ga_go_step(f, w->step_cmd, NULL));
}

/* ---- the tables ---- */

/* The file or the place when the report did not name them, the Stack
   window, the watches, then the breakpoints changed while the program ran
   and those the editor set for this run alone (the start's, Go to
   cursor's), now removals. */
static const E_D_STEP e_d_sdb_stop_steps[] =
{
 { e_d_gsdb_which_file_cmd, e_d_gsdb_which_file_answered },
 { e_d_gc_t_cmd,            e_d_gc_t_answered },
 { e_d_gc_stack_cmd,        e_d_gs_stack_answered },
 { e_d_gs_watch_cmd,        e_d_gs_watch_answered },
 { e_d_gsdb_remove_cmd,     e_d_gsdb_remove_answered },
 { e_d_gsdb_add_cmd,        e_d_gsdb_add_answered },
 { NULL, NULL }
};

static const E_D_STEP e_d_dbx_stop_steps[] =
{
 { e_d_gs_where_cmd,  e_d_gs_where_answered },
 { e_d_gc_stack_cmd,  e_d_gs_stack_answered },
 { e_d_gs_watch_cmd,  e_d_gs_watch_answered },
 { e_d_gb_remove_cmd, e_d_gb_remove_answered },
 { e_d_gb_add_cmd,    e_d_gb_add_answered },
 { NULL, NULL }
};

static const E_D_STEP e_d_xdb_stop_steps[] =
{
 { e_d_gc_t_cmd,      e_d_gc_t_answered },
 { e_d_gc_stack_cmd,  e_d_gs_stack_answered },
 { e_d_gs_watch_cmd,  e_d_gs_watch_answered },
 { e_d_gx_remove_cmd, e_d_gx_remove_answered },
 { e_d_gb_add_cmd,    e_d_gb_add_answered },
 { NULL, NULL }
};

/* Every breakpoint of the user's after the banner (xdb's setting first); a
   set-up given up on and asked again first clears a line the debugger may
   have set unanswered. */
static const E_D_STEP e_d_sdb_set_up_steps[] =
{
 { e_d_gsdb_remove_cmd, e_d_gsdb_remove_answered },
 { e_d_gsdb_add_cmd,    e_d_gsdb_set_up_add_answered },
 { NULL, NULL }
};

static const E_D_STEP e_d_dbx_set_up_steps[] =
{
 { e_d_gb_remove_cmd, e_d_gb_remove_answered },
 { e_d_gb_add_cmd,    e_d_gu_add_answered },
 { NULL, NULL }
};

static const E_D_STEP e_d_xdb_set_up_steps[] =
{
 { e_d_gx_set_up_cmd, e_d_gs_ignore_answer },
 { e_d_gx_remove_cmd, e_d_gx_remove_answered },
 { e_d_gb_add_cmd,    e_d_gu_add_answered },
 { NULL, NULL }
};

/* A breakpoint set or taken off at a stop (dbx's are gdb's,
   e_d_ga_brk_steps). */
static const E_D_STEP e_d_sdb_brk_steps[] =
{
 { e_d_gsdb_remove_cmd, e_d_gsdb_remove_answered },
 { e_d_gsdb_add_cmd,    e_d_gsdb_add_answered },
 { NULL, NULL }
};

static const E_D_STEP e_d_xdb_brk_steps[] =
{
 { e_d_gx_remove_cmd, e_d_gx_remove_answered },
 { e_d_gb_add_cmd,    e_d_gb_add_answered },
 { NULL, NULL }
};

/* Add, edit or delete a watch at a stop, the stack with them when its
   window is open; for sdb also Enter on a Stack row, which moves nothing in
   sdb. */
static const E_D_STEP e_d_gc_watches_steps[] =
{
 { e_d_gc_stack_cmd, e_d_gs_stack_answered },
 { e_d_gs_watch_cmd, e_d_gs_watch_answered },
 { NULL, NULL }
};

/* Ctrl-G K. */
static const E_D_STEP e_d_gc_stack_steps[] =
{
 { e_d_gc_stack_cmd, e_d_gs_stack_answered },
 { NULL, NULL }
};

/* Enter on a row of the Stack window: the frame moved to, then the stack
   and the watches it shows. */
static const E_D_STEP e_d_dbx_follow_steps[] =
{
 { e_d_gs_move_cmd,  e_d_gs_move_answered },
 { e_d_gc_stack_cmd, e_d_gs_stack_answered },
 { e_d_gs_watch_cmd, e_d_gs_watch_answered },
 { NULL, NULL }
};

static const E_D_STEP e_d_xdb_follow_steps[] =
{
 { e_d_gx_move_cmd,  e_d_gs_move_answered },
 { e_d_gc_stack_cmd, e_d_gs_stack_answered },
 { e_d_gs_watch_cmd, e_d_gs_watch_answered },
 { NULL, NULL }
};

/* What a resume asks first: the breakpoint changes the debugger has not
   been told of, the start breakpoint before a first run, Go to cursor's
   own breakpoint. */
static const E_D_STEP e_d_sdb_go_steps[] =
{
 { e_d_gsdb_remove_cmd,     e_d_gsdb_remove_answered },
 { e_d_gsdb_add_cmd,        e_d_gsdb_add_answered },
 { e_d_gsdb_start_cmd,      e_d_gsdb_start_answered },
 { e_d_gsdb_cursor_set_cmd, e_d_gsdb_cursor_set_answered },
 { NULL, NULL }
};

static const E_D_STEP e_d_dbx_go_steps[] =
{
 { e_d_gb_remove_cmd,     e_d_gb_remove_answered },
 { e_d_gb_add_cmd,        e_d_gb_add_answered },
 { e_d_gg_start_cmd,      e_d_gg_start_answered },
 { e_d_ga_cursor_set_cmd, e_d_gc_cursor_set_answered },
 { NULL, NULL }
};

static const E_D_STEP e_d_xdb_go_steps[] =
{
 { e_d_gx_remove_cmd,     e_d_gx_remove_answered },
 { e_d_gb_add_cmd,        e_d_gb_add_answered },
 { e_d_gg_start_cmd,      e_d_gg_start_answered },
 { e_d_ga_cursor_set_cmd, e_d_gc_cursor_set_answered },
 { NULL, NULL }
};

static const E_D_CLI e_d_cli_sdb =
{
 "t", "s\n", "S\n", "D", 0, E_D_CLI_RUN_SAYS_NOTHING, e_d_sdb_stop_steps
};

static const E_D_CLI e_d_cli_dbx =
{
 "where", "step\n", "next\n", "delete all", 0, E_D_CLI_RUN_SAYS_RUNNING,
 e_d_dbx_stop_steps
};

static const E_D_CLI e_d_cli_xdb =
{
 "t", "s\n", "S\n", E_D_XDB_DELETE_ALL, 1, E_D_CLI_RUN_SAYS_A_LINE,
 e_d_xdb_stop_steps
};

/**
 * e_d_cli_of - The words of sdb, dbx or xdb, for debugger @type; sdb's for
 * any other, which never asks (only these three's engine tables point here).
 */
static const E_D_CLI *e_d_cli_of(int type)
{
 if (type == DEB_DBX)
  return(&e_d_cli_dbx);
 if (type == DEB_XDB)
  return(&e_d_cli_xdb);
 return(&e_d_cli_sdb);
}

/* ==== gdb over MI ====

   The same editor as under gdb's line interface, with gdb's answers read
   from named fields: the run's stop from a *stopped record (its reason
   says whether it is the editor's own interrupt and gives the exit code),
   the mark from -stack-list-frames, the watches from
   -data-evaluate-expression, the breakpoints from -break-insert's bkpt
   tuple or its error record.  Every command is numbered and its answer is
   the result record with that number, so errors are part of the answer
   and the error stream is never asked; the program's output stays on its
   own terminal (-inferior-tty-set), where nothing it prints can be taken
   for one of gdb's records.  gdb's sentences are shown, never matched. */

/* The oldest gdb driven over MI, and what its -list-features must name. */
#define E_D_GDBMI_FLOOR_MAJOR     7
#define E_D_GDBMI_FLOOR_MINOR     6
#define E_D_GDBMI_FEATURE_PENDING "pending-breakpoints"
#define E_D_GDBMI_FEATURE_NOTIFY  "breakpoint-notifications"
#define E_D_GDBMI_FEATURES        "features"
#define E_D_GDBMI_BANNER_MAX      512
#define E_D_GDBMI_OLD_FMT \
 " gdb %d.%d is older than %d.%d: this session uses its line interface"
#define E_D_GDBMI_NO_VERSION_LINE \
 " gdb named no version: this session uses its line interface"
#define E_D_GDBMI_NO_FEATURE_FMT \
 " gdb lacks %s: this session uses its line interface"
/* The commands. */
#define E_D_GDBMI_VERSION     "-gdb-version"
#define E_D_GDBMI_LIST        "-list-features"
#define E_D_GDBMI_CONFIRM_OFF "-gdb-set confirm off"
#define E_D_GDBMI_ASYNC_OFF   "-gdb-set mi-async off"
#define E_D_GDBMI_TTY_FMT     "-inferior-tty-set %s"
#define E_D_GDBMI_BREAK_FMT   "-break-insert \"%s:%d\""
#define E_D_GDBMI_BREAK_AT    "-break-insert \"%s\""
#define E_D_GDBMI_DELETE_FMT  "-break-delete %d"
/* The innermost frames only, as many as the session keeps: a deep
   recursion's whole stack is one line too long for the record reader, and
   the stop would then go unmarked. */
#define E_D_GDBMI_FRAMES_FMT  "-stack-list-frames 0 %d"
#define E_D_GDBMI_ARGS_FMT    "-stack-list-arguments 1 0 %d"
#define E_D_GDBMI_FRAMES_LAST (E_DS_FRAMES_MAX - 1)
#define E_D_GDBMI_EVAL_FMT    "-data-evaluate-expression \"%s\""
/* The reason of the stop at the end of a finish. */
#define E_D_GDBMI_FINISHED    "function-finished"
#define E_D_GDBMI_REASON      "reason"
#define E_D_GDBMI_MEANING     "signal-meaning"
#define E_D_GDBMI_CLASS_DONE  "done"
#define E_D_GDBMI_CLASS_ERROR "error"
#define E_D_GDBMI_CLASS_RUN   "running"
#define E_D_GDBMI_CLASS_STOP  "stopped"
/* What the user reads for a signal that stopped the program, built from
   the stop's fields as gdb's line interface words it. */
#define E_D_GDBMI_SIGNAL_FMT  "Program received signal %s, %s."
/* Reading the run's answer. */
#define E_D_GDBMI_DECIMAL        10      /* gdb's numbers, but the exit code */
#define E_D_GDBMI_READ_MAX       16384
#define E_D_GDBMI_READS_PER_WAKE 16
#define E_D_GDBMI_ROWS_MAX       65536   /* the Stack window's text */

/* One record at a time is read; the Stack window's frames wait in the
   second while their arguments are asked. */
static E_GDBMI_RECORD e_d_gdbmi_rec;
static E_GDBMI_RECORD e_d_gdbmi_frames_rec;

/* What the run's reader has seen since the run went out. */
static struct
{
 E_GDBMI_SPLIT split;
 int stop_seen;              /* a *stopped came: its (gdb) ends the run */
 int run_failed;             /* the resume was refused (an error record) */
 int finished;               /* the stop ends a finish */
 int too_old;                /* the set-up found gdb too old for MI */
 char meaning[E_DS_NAME_MAX];   /* the stop's signal, in gdb's words */
} e_d_gm;

/* ---- the engine's world under MI ---- */

/**
 * e_d_gm_send - Send a command of the engine's, numbered, and remember its
 * number: the answer that carries it is the one the engine waits for.
 * @cmd: the command, no line end; @io: unused.
 */
static int e_d_gm_send(const char *cmd, void *io)
{
 int ret = e_d_ga_send(cmd, io);

 e_d_mi.pending = e_d_mi.token;
 return(ret);
}

/**
 * e_d_gm_ends_in_prompt - Is the answer whole: the result record of the
 * command in flight, then (gdb)?
 */
static int e_d_gm_ends_in_prompt(const E_D_ANSWER *ans, size_t *prompt_len)
{
 return(e_gdbmi_answer_ended(ans->text, ans->len, e_d_mi.pending,
                             prompt_len));
}

/**
 * e_d_gm_line_is_async - Is the line no part of the answer in flight: a
 * stop, a notice, the late answer of a command given up on?
 */
static int e_d_gm_line_is_async(const char *line, void *io)
{
 (void)io;
 return(e_gdbmi_line_is_async(line, e_d_mi.pending));
}

/**
 * e_d_gm_deliver_async - Take such a line into the session, so a stop or a
 * late error read meanwhile is known to it.
 */
static void e_d_gm_deliver_async(const char *line, void *io)
{
 (void)io;
 if (e_gdbmi_parse_line(line, &e_d_gdbmi_rec) == E_GDBMI_OK)
  e_ds_take_mi_record(&e_d_session, &e_d_gdbmi_rec);
}

/**
 * e_d_gm_result - The result record of the command an answer is for.
 * @ans: the answer; @r: receives it.
 * Return: 1 when the answer holds it.
 */
static int e_d_gm_result(const E_D_ANSWER *ans, E_GDBMI_RECORD *r)
{
 return(e_gdbmi_parse_result(ans->text, e_d_mi.pending, r) == E_GDBMI_OK);
}

/**
 * e_d_gm_is_done - Is @r the result record of a command gdb carried out?
 */
static int e_d_gm_is_done(const E_GDBMI_RECORD *r)
{
 return(r->kind == E_GDBMI_K_RESULT && !strcmp(r->klass, E_D_GDBMI_CLASS_DONE));
}

/**
 * e_d_gm_refused_words - Did gdb refuse the command @ans answers?  Its
 * message, without its styling, goes into @words.
 */
static int e_d_gm_refused_words(const E_D_ANSWER *ans, char *words, size_t n)
{
 if (!e_d_gm_result(ans, &e_d_gdbmi_rec)
     || strcmp(e_d_gdbmi_rec.klass, E_D_GDBMI_CLASS_ERROR))
  return(0);
 e_ds_take_mi_record(&e_d_session, &e_d_gdbmi_rec);
 snprintf(words, n, "%s", e_d_session.last_error);
 return(1);
}

/* ---- breakpoints under MI ---- */

/**
 * e_d_gm_brk_add_cmd - -break-insert for list entry @i.  Without -f: a
 * place gdb cannot find is refused with gdb's reason, as under the line
 * interface, and not kept pending in silence.
 */
static void e_d_gm_brk_add_cmd(int i, char *cmd, size_t n)
{
 char file[PATH_MAX];

 e_d_gdbmi_quote(file, sizeof file, e_d_sbrpts[i]);
 snprintf(cmd, n, E_D_GDBMI_BREAK_FMT, file, e_d_ybrpts[i]);
}

/**
 * e_d_gm_brk_remove_cmd - -break-delete by the number gdb gave it, or the
 * console's clear by place for one whose number was never read.
 */
static void e_d_gm_brk_remove_cmd(const E_D_BRK_GONE *g, char *cmd, size_t n)
{
 if (g->id == E_D_GB_ID_UNKNOWN)
  snprintf(cmd, n, E_D_GDB_CLEAR_FMT, g->file, g->line);
 else
  snprintf(cmd, n, E_D_GDBMI_DELETE_FMT, g->id);
}

/**
 * e_d_gm_brk_set_id - The number gdb's bkpt tuple gives the breakpoint, 0
 * when gdb refused it.
 */
static int e_d_gm_brk_set_id(const E_D_ANSWER *ans)
{
 if (!e_d_gm_result(ans, &e_d_gdbmi_rec) || !e_d_gm_is_done(&e_d_gdbmi_rec))
  return(0);
 return(e_gdbmi_number(e_gdbmi_get(&e_d_gdbmi_rec, "bkpt.number"),
                       E_D_GDBMI_DECIMAL, 0));
}

/**
 * e_d_gm_brk_verdict - Keep with list entry @i what gdb answered, as the
 * session reads the answer: solid with its number, hollow and pending, or
 * hollow with gdb's message.
 */
static void e_d_gm_brk_verdict(int i, int id, const E_D_ANSWER *ans)
{
 E_DS_BRK *b;

 e_ds_set_brk(&e_d_session, i, e_d_sbrpts[i], e_d_ybrpts[i]);
 if (i >= E_DS_BRKS_MAX || !e_d_gm_result(ans, &e_d_gdbmi_rec)
     || !e_ds_take_mi_brk_result(&e_d_session, i, &e_d_gdbmi_rec))
 {
  e_d_nrbrpts[i] = 0;
  e_d_brk_set_state(i, E_D_BRK_REFUSED, E_D_BRK_NO_NUMBER);
  return;
 }
 b = &e_d_session.brks[i];
 e_d_nrbrpts[i] = id;
 e_d_brk_set_state(i, b->state, b->msg);
}

/**
 * e_d_gm_start_brk_cmd - The breakpoint that stops the first run at the
 * program's start, as e_d_gdb_start_break places it.
 */
static void e_d_gm_start_brk_cmd(char *cmd, size_t n)
{
 const char *sym = e_get_start_symbol();
 char where[PATH_MAX];
 char quoted[PATH_MAX];

 if (sym && !strcmp(sym, GA68_ENTRY_SYMBOL) && e_d_file[0])
  snprintf(where, sizeof where, "%s:1", e_d_file);
 else
  snprintf(where, sizeof where, "%s", sym ? sym : "");
 e_d_gdbmi_quote(quoted, sizeof quoted, where);
 snprintf(cmd, n, E_D_GDBMI_BREAK_AT, quoted);
}

/* ---- the set-up after the banner ---- */

/**
 * e_d_gm_say_too_old - Note why this gdb keeps its line interface, for the
 * status row of the session started again in its place.
 */
static void e_d_gm_say_too_old(const char *line)
{
 snprintf(e_d_mi.say, sizeof e_d_mi.say, "%s", line);
 e_d_gm.too_old = 1;
}

static int e_d_gm_version_cmd(char *cmd, size_t n, void *ctx)
{
 (void)ctx;
 e_d_gm.too_old = 0;
 snprintf(cmd, n, "%s", E_D_GDBMI_VERSION);
 return(1);
}

/**
 * e_d_gm_version_answered - gdb named its version: below the floor the
 * session goes on with the line interface instead.
 */
static int e_d_gm_version_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 char banner[E_D_GDBMI_BANNER_MAX];
 char line[E_D_GDBMI_SAY_MAX];
 int major, minor;

 (void)how;
 (void)ctx;
 e_gdbmi_console_join(ans->text, banner, sizeof banner);
 if (!e_gdbmi_version_of(banner, &major, &minor))
 {
  e_d_gm_say_too_old(E_D_GDBMI_NO_VERSION_LINE);
  return(E_D_STEP_STOP);
 }
 if (major > E_D_GDBMI_FLOOR_MAJOR ||
     (major == E_D_GDBMI_FLOOR_MAJOR && minor >= E_D_GDBMI_FLOOR_MINOR))
  return(E_D_STEP_NEXT);
 snprintf(line, sizeof line, E_D_GDBMI_OLD_FMT, major, minor,
          E_D_GDBMI_FLOOR_MAJOR, E_D_GDBMI_FLOOR_MINOR);
 e_d_gm_say_too_old(line);
 return(E_D_STEP_STOP);
}

static int e_d_gm_features_cmd(char *cmd, size_t n, void *ctx)
{
 (void)ctx;
 snprintf(cmd, n, "%s", E_D_GDBMI_LIST);
 return(1);
}

/**
 * e_d_gm_feature_missing - Note that -list-features lacks @name.
 * Return: E_D_STEP_STOP.
 */
static int e_d_gm_feature_missing(const char *name)
{
 char line[E_D_GDBMI_SAY_MAX];

 snprintf(line, sizeof line, E_D_GDBMI_NO_FEATURE_FMT, name);
 e_d_gm_say_too_old(line);
 return(E_D_STEP_STOP);
}

/**
 * e_d_gm_features_answered - gdb listed its MI features: without pending
 * breakpoints and breakpoint notices the session keeps the line interface.
 */
static int e_d_gm_features_answered(E_D_REQ_END how, E_D_ANSWER *ans,
                                    void *ctx)
{
 (void)how;
 (void)ctx;
 if (!e_d_gm_result(ans, &e_d_gdbmi_rec) || !e_d_gm_is_done(&e_d_gdbmi_rec))
  return(e_d_gm_feature_missing(E_D_GDBMI_FEATURES));
 if (!e_gdbmi_list_has(&e_d_gdbmi_rec, E_D_GDBMI_FEATURES,
                       E_D_GDBMI_FEATURE_PENDING))
  return(e_d_gm_feature_missing(E_D_GDBMI_FEATURE_PENDING));
 if (!e_gdbmi_list_has(&e_d_gdbmi_rec, E_D_GDBMI_FEATURES,
                       E_D_GDBMI_FEATURE_NOTIFY))
  return(e_d_gm_feature_missing(E_D_GDBMI_FEATURE_NOTIFY));
 return(E_D_STEP_NEXT);
}

static int e_d_gm_confirm_cmd(char *cmd, size_t n, void *ctx)
{
 (void)ctx;
 snprintf(cmd, n, "%s", E_D_GDBMI_CONFIRM_OFF);
 return(1);
}

static int e_d_gm_async_cmd(char *cmd, size_t n, void *ctx)
{
 (void)ctx;
 snprintf(cmd, n, "%s", E_D_GDBMI_ASYNC_OFF);
 return(1);
}

/**
 * e_d_gm_tty_cmd - The program's terminal: its output and input go there,
 * never onto gdb's own output, where it could pass for a record.
 */
static int e_d_gm_tty_cmd(char *cmd, size_t n, void *ctx)
{
 (void)ctx;
 if (e_d_pty_master < 0)
  return(0);
 snprintf(cmd, n, E_D_GDBMI_TTY_FMT, e_d_pty_slave_name);
 return(1);
}

/* The version and features first: a gdb too old for MI is started again
   with its line interface before anything else is asked. */
static const E_D_STEP e_d_gdbmi_set_up_steps[] =
{
 { e_d_gm_version_cmd,  e_d_gm_version_answered },
 { e_d_gm_features_cmd, e_d_gm_features_answered },
 { e_d_gm_confirm_cmd,  e_d_gs_ignore_answer },
 { e_d_gm_async_cmd,    e_d_gs_ignore_answer },
 { e_d_gm_tty_cmd,      e_d_gs_ignore_answer },
 { e_d_gb_remove_cmd,   e_d_gb_remove_answered },
 { e_d_gb_add_cmd,      e_d_gu_add_answered },
 { NULL, NULL }
};

/* The session to start again with gdb's line interface. */
static struct
{
 ECNT *cn;
 e_d_resume_fn pressed;
 int arg;
} e_d_gm_restart;

/**
 * e_d_gm_restart_on_timer - End the gdb found too old for MI and start
 * gdb again with its line interface, with the command the user pressed.
 * @data: unused.
 */
static void e_d_gm_restart_on_timer(void *data)
{
 ECNT *cn = e_d_gm_restart.cn ? e_d_gm_restart.cn : WpeEditor;
 FENSTER *f = cn->f[cn->mxedt];

 (void)data;
 e_d_quit_basic(f);
 e_d_mi.force_cli = 1;
 e_d_build_done = 1;
 e_d_start_cmd = e_d_gm_restart.pressed;
 e_d_start_arg = e_d_gm_restart.arg;
 e_run_debug(f);
}

/**
 * e_d_gm_fell_back - The set-up found this gdb too old for MI: end it and
 * start gdb again with its line interface, then run the command the user
 * pressed.
 * @go: the set-up's context.
 *
 * The user's command is not lost and nothing is built again; the status
 * row names the reason once.
 * Return: 1 when the session was started again.
 */
static int e_d_gm_fell_back(E_D_GA_GO *go)
{
 if (!e_d_gdbmi_on() || !e_d_gm.too_old)
  return(0);
 e_d_gm.too_old = 0;
 e_d_gm_restart.cn = go->cn;
 e_d_gm_restart.pressed = go->pressed;
 e_d_gm_restart.arg = go->pressed_arg;
 /* Out of the answer's callback first, the way a kept key is run
    (e_d_act_schedule): this gdb's descriptors are closed and the new gdb's
    watched from the editor's loop, never from inside the loop's call for
    the old ones.  A due timer runs in the same turn of the loop, before any
    key is read, so no key is handled between the two gdbs; keys typed
    during the new start are kept or refused as at any start. */
 if (wpe_timer_add(0, 0, e_d_gm_restart_on_timer, NULL) <= 0)
  e_d_gm_restart_on_timer(NULL);
 return(1);
}

/* ---- a stop under MI ---- */

static int e_d_gm_flush_cmd(char *cmd, size_t n, void *ctx)
{
 return(e_d_gs_flush_cmd(cmd, n, ctx));
}

/**
 * e_d_gm_window_file - The name the editor's own window gives the file a
 * frame names by its full path, so the stop is marked in the window the
 * user has open and not in a second one; the full path when no window
 * shows it.
 * @cn: the editor; @full: gdb's fullname; @out, @n: the name to mark.
 */
static void e_d_gm_window_file(ECNT *cn, const char *full, char *out, size_t n)
{
 char path[PATH_MAX], real_win[PATH_MAX], real_full[PATH_MAX];
 const char *slash = strrchr(full, DIRC);
 const char *name = slash ? slash + 1 : full;

 snprintf(out, n, "%s", full);
 if (!realpath(full, real_full))
  return;
 if (!e_d_window_path_of(cn, name, path, sizeof path))
  return;
 if (realpath(path, real_win) && !strcmp(real_win, real_full))
  snprintf(out, n, "%s", path);
}

static int e_d_gm_frames_cmd(char *cmd, size_t n, void *ctx)
{
 (void)ctx;
 snprintf(cmd, n, E_D_GDBMI_FRAMES_FMT, E_D_GDBMI_FRAMES_LAST);
 return(1);
}

/**
 * e_d_gm_frames_answered - The frames of the stop: the mark goes to the
 * innermost one whose source is here, and after Pause that frame becomes
 * gdb's selected one, so the watches read the user's locals.
 */
static int e_d_gm_frames_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 E_D_GA_CTX *st = ctx;
 const E_DS_FRAME *fr;
 int m;

 (void)how;
 if (!e_d_gm_result(ans, &e_d_gdbmi_rec) || !e_d_gm_is_done(&e_d_gdbmi_rec))
 {
  st->no_stack = 1;
  return(E_D_STEP_STOP);
 }
 e_ds_take_mi_record(&e_d_session, &e_d_gdbmi_rec);
 st->where_rows = e_d_session.nframes;
 e_d_where_frame = 0;
 m = e_d_session.marked_frame;
 if (m < 0 || m >= e_d_session.nframes)
 {
  st->line = -1;
  return(E_D_STEP_NEXT);
 }
 fr = &e_d_session.frames[m];
 st->line = fr->line;
 e_d_gm_window_file(st->cn, fr->file, st->file, sizeof st->file);
 e_d_where_frame = m;
 if (m > 0 && e_d_run.pause != E_D_PAUSE_NONE)
  st->pause_frame = m;
 return(E_D_STEP_NEXT);
}

static int e_d_gm_stack_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 if (!st->stack_wanted)
  return(0);
 if (!st->mi_frames)
  snprintf(cmd, n, E_D_GDBMI_FRAMES_FMT, E_D_GDBMI_FRAMES_LAST);
 else
  snprintf(cmd, n, E_D_GDBMI_ARGS_FMT, E_D_GDBMI_FRAMES_LAST);
 return(1);
}

/**
 * e_d_gm_stack_rows - Write the Stack window's rows from the frames read
 * and their arguments @args (NULL when gdb gave none).
 * @st: the context; its stack text is replaced.
 */
static void e_d_gm_stack_rows(E_D_GA_CTX *st, const E_GDBMI_RECORD *args)
{
 char *rows = MALLOC(E_D_GDBMI_ROWS_MAX);

 if (!rows)
  return;
 if (e_ds_mi_where_rows(&e_d_gdbmi_frames_rec, args, rows,
                        E_D_GDBMI_ROWS_MAX) == 0)
 {
  FREE(rows);
  return;
 }
 if (st->stack)
  FREE(st->stack);
 st->stack = rows;
}

/**
 * e_d_gm_stack_answered - The Stack window's frames, then their arguments:
 * the rows read as gdb's "bt" prints them.
 */
static int e_d_gm_stack_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 (void)how;
 if (!st->mi_frames)
 {
  if (!e_d_gm_result(ans, &e_d_gdbmi_frames_rec)
      || !e_d_gm_is_done(&e_d_gdbmi_frames_rec))
   return(E_D_STEP_NEXT);
  st->mi_frames = 1;
  return(E_D_STEP_AGAIN);
 }
 st->mi_frames = 0;
 if (e_d_gm_result(ans, &e_d_gdbmi_rec) && e_d_gm_is_done(&e_d_gdbmi_rec))
  e_d_gm_stack_rows(st, &e_d_gdbmi_rec);
 else
  e_d_gm_stack_rows(st, NULL);
 return(E_D_STEP_NEXT);
}

static int e_d_gm_watch_cmd(char *cmd, size_t n, void *ctx)
{
 E_D_GA_CTX *st = ctx;
 char quoted[E_D_REQ_CMD_MAX];

 if (st->watch >= st->nvalues)
  return(0);
 e_d_gdbmi_quote(quoted, sizeof quoted, e_d_swtchs[st->watch]);
 snprintf(cmd, n, E_D_GDBMI_EVAL_FMT, quoted);
 return(1);
}

/**
 * e_d_gm_watch_value - What the Watches window shows for an answer: the
 * value as the line interface gives it, or NULL when gdb has none.
 * Return: the text, to be freed, or NULL.
 */
static char *e_d_gm_watch_value(const E_D_ANSWER *ans)
{
 const char *value;
 char *text;
 size_t n;

 if (!e_d_gm_result(ans, &e_d_gdbmi_rec))
  return(NULL);
 value = e_gdbmi_get(&e_d_gdbmi_rec, "value");
 if (!value)
  return(NULL);
 n = strlen(value) + E_D_GDBMI_SAY_MAX;
 text = MALLOC(n);
 if (!text)
  return(NULL);
 if (!e_ds_mi_watch_text(&e_d_gdbmi_rec, text, n))
 {
  FREE(text);
  return(NULL);
 }
 return(text);
}

static int e_d_gm_watch_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 E_D_GA_CTX *st = ctx;

 (void)how;
 st->values[st->watch++] = e_d_gm_watch_value(ans);
 return(E_D_STEP_AGAIN);
}

/* The answers of a stop the user is shown: the program's buffered output,
   the frames and the mark, after Pause the user's frame selected, the Stack
   window, the watches, then the breakpoints changed while it ran and the
   start breakpoint.  No settle step: an interrupt that met gdb at its
   prompt is answered without a number, which no request waits for. */
static const E_D_STEP e_d_gdbmi_stop_steps[] =
{
 { e_d_gm_flush_cmd,  e_d_gs_ignore_answer },
 { e_d_gm_frames_cmd, e_d_gm_frames_answered },
 { e_d_gs_frame_cmd,  e_d_gs_frame_answered },
 { e_d_gm_stack_cmd,  e_d_gm_stack_answered },
 { e_d_gm_watch_cmd,  e_d_gm_watch_answered },
 { e_d_gb_remove_cmd, e_d_gb_remove_answered },
 { e_d_gb_add_cmd,    e_d_gb_add_answered },
 { e_d_gb_start_cmd,  e_d_gb_start_answered },
 { NULL, NULL }
};

/* A stop made to tell gdb of breakpoint changes while the program runs. */
static const E_D_STEP e_d_gdbmi_sync_steps[] =
{
 { e_d_gb_remove_cmd, e_d_gb_remove_answered },
 { e_d_gb_add_cmd,    e_d_gb_add_answered },
 { NULL, NULL }
};

/* Add, edit or delete a watch at a stop. */
static const E_D_STEP e_d_gdbmi_watches_steps[] =
{
 { e_d_gm_stack_cmd, e_d_gm_stack_answered },
 { e_d_gm_watch_cmd, e_d_gm_watch_answered },
 { NULL, NULL }
};

/* Ctrl-G K. */
static const E_D_STEP e_d_gdbmi_stack_steps[] =
{
 { e_d_gm_stack_cmd, e_d_gm_stack_answered },
 { NULL, NULL }
};

/* Enter on a row of the Stack window. */
static const E_D_STEP e_d_gdbmi_follow_steps[] =
{
 { e_d_gs_move_cmd,  e_d_gs_move_answered },
 { e_d_gm_stack_cmd, e_d_gm_stack_answered },
 { e_d_gm_watch_cmd, e_d_gm_watch_answered },
 { NULL, NULL }
};

/* What a resume asks first: the breakpoint changes gdb has not been told
   of, and the start breakpoint before the first run. */
static const E_D_STEP e_d_gdbmi_go_steps[] =
{
 { e_d_gb_remove_cmd, e_d_gb_remove_answered },
 { e_d_gb_add_cmd,    e_d_gb_add_answered },
 { e_d_gg_start_cmd,  e_d_gg_start_answered },
 { NULL, NULL }
};

/* ---- the run's answer under MI ---- */

/**
 * e_d_gm_begin - A run, a step or a Continue went out: nothing of the one
 * before is read as its answer.
 */
static void e_d_gm_begin(void)
{
 e_gdbmi_split_init(&e_d_gm.split);
 e_d_gm.stop_seen = 0;
 e_d_gm.run_failed = 0;
 e_d_gm.finished = 0;
 e_d_gm.meaning[0] = '\0';
}

/**
 * e_d_gm_say_start_words - Show in Messages what gdb said of the program's
 * start ("Starting program: ..."), one line at a time.
 * @text: a console record's words.
 */
static void e_d_gm_say_start_words(const char *text)
{
 ECNT *cn = e_d_run.cn ? e_d_run.cn : WpeEditor;
 char line[E_D_LINE_MAX];
 const char *p = text, *nl;
 size_t len;

 while (*p)
 {
  nl = strchr(p, '\n');
  len = nl ? (size_t)(nl - p) : strlen(p);
  if (len >= sizeof line)
   len = sizeof line - 1;
  memcpy(line, p, len);
  line[len] = '\0';
  p = nl ? nl + 1 : p + strlen(p);
  if (line[0])
   e_d_p_message(line, cn->f[cn->mxedt], 0);
 }
}

/**
 * e_d_gm_take_stop - A *stopped record: the session learns where and why,
 * the editor's interrupt in flight told it first, so its own SIGINT is
 * known by the record's reason and not by gdb's words.
 */
static void e_d_gm_take_stop(const E_GDBMI_RECORD *r)
{
 const char *reason = e_gdbmi_get(r, E_D_GDBMI_REASON);
 const char *meaning = e_gdbmi_get(r, E_D_GDBMI_MEANING);

 e_ds_set_interrupt(&e_d_session, e_d_run.interrupt_for);
 e_ds_take_mi_record(&e_d_session, r);
 e_d_gm.stop_seen = 1;
 e_d_gm.finished = reason && !strcmp(reason, E_D_GDBMI_FINISHED);
 snprintf(e_d_gm.meaning, sizeof e_d_gm.meaning, "%s", meaning ? meaning : "");
}

/**
 * e_d_gm_take_result - A result record during a run: the run began
 * (running), or gdb refused it.
 */
static void e_d_gm_take_result(const E_GDBMI_RECORD *r)
{
 if (!strcmp(r->klass, E_D_GDBMI_CLASS_RUN))
 {
  e_d_gdb_awaits_forget();
  return;
 }
 if (!strcmp(r->klass, E_D_GDBMI_CLASS_ERROR))
 {
  e_ds_take_mi_record(&e_d_session, r);
  e_d_gm.run_failed = 1;
 }
}

/**
 * e_d_gm_take_line - One whole line of gdb's answer to a run.
 * Return: 1 when it ends the run: the (gdb) after a stop or a refusal.
 */
static int e_d_gm_take_line(const char *line)
{
 E_GDBMI_RECORD *r = &e_d_gdbmi_rec;

 if (e_gdbmi_is_prompt(line, strlen(line)))
  return(e_d_gm.stop_seen || e_d_gm.run_failed);
 if (e_gdbmi_parse_line(line, r) != E_GDBMI_OK)
  return(0);
 if (r->kind == E_GDBMI_K_EXEC && !strcmp(r->klass, E_D_GDBMI_CLASS_STOP))
  e_d_gm_take_stop(r);
 else if (r->kind == E_GDBMI_K_RESULT)
  e_d_gm_take_result(r);
 else if (r->kind == E_GDBMI_K_TARGET ||
          (r->kind == E_GDBMI_K_CONSOLE && e_d_gdb_awaits.start))
  e_d_gm_say_start_words(e_gdbmi_stream_text(r));
 return(0);
}

/**
 * e_d_gm_take_bytes - Read the lines of what one read brought.
 * Return: 1 when the run ended in them.  A line too long for the reader is
 * dropped whole: its head is never read as a record.
 */
static int e_d_gm_take_bytes(const char *buf, size_t n)
{
 const char *line;
 size_t used = 0;
 int got, ended = 0;

 while ((got = e_gdbmi_split_next(&e_d_gm.split, buf, n, &used, &line))
        != E_GDBMI_SPLIT_NEED_MORE)
  if (got == E_GDBMI_SPLIT_LINE && !ended && e_d_gm_take_line(line))
   ended = 1;
 return(ended);
}

/**
 * e_d_gm_read_now - Read what gdb has written, without waiting: the
 * descriptor is made non-blocking for this read only, as the line readers
 * do, and left as it was.
 * Return: what read() answered.
 */
static ssize_t e_d_gm_read_now(char *buf, size_t n)
{
 int flags = fcntl(wfildes[0], F_GETFL, 0);
 ssize_t got;
 int err;

 fcntl(wfildes[0], F_SETFL, flags | O_NONBLOCK);
 got = read(wfildes[0], buf, n);
 err = errno;
 fcntl(wfildes[0], F_SETFL, flags);
 errno = err;
 return(got);
}

/**
 * e_d_gdbmi_feed - Read what gdb has answered to a run or a step so far.
 * @f: the window the run was started from (unused).
 *
 * The run ends at the (gdb) after a *stopped record, never at a prompt
 * gdb writes for the run's own ^running.
 * Return: E_D_FEED_GOING, E_D_FEED_STOPPED or E_D_FEED_GONE.
 */
static int e_d_gdbmi_feed(FENSTER *f)
{
 char buf[E_D_GDBMI_READ_MAX];
 signed char err[E_D_LINE_MAX];
 ssize_t n;
 int reads;

 (void)f;
 e_d_line_drain_errors(err, sizeof err);
 for (reads = 0; reads < E_D_GDBMI_READS_PER_WAKE; reads++)
 {
  n = e_d_gm_read_now(buf, sizeof buf);
  if (n < 0 && (errno == EAGAIN || errno == EINTR))
   return(E_D_FEED_GOING);
  if (n <= 0)
  {
   e_d_note_output_closed();
   return(E_D_FEED_GONE);
  }
  if (e_d_gm_take_bytes(buf, (size_t)n))
   return(E_D_FEED_STOPPED);
 }
 return(E_D_FEED_GOING);
}

/**
 * e_d_gdbmi_interrupt_stop - Is the stop just read the one an interrupt
 * made?  Its record says SIGINT; whose it is the session knew from
 * e_ds_set_interrupt.
 * @taken: unused: gdb's stop is whole at once.
 */
static int e_d_gdbmi_interrupt_stop(int taken)
{
 (void)taken;
 if (!e_d_gm.stop_seen)
  return(0);
 if (e_d_session.own_stop || e_d_session.stop_reason == E_DS_STOP_PAUSE)
  return(1);
 return(e_d_session.stop_reason == E_DS_STOP_SIGNAL &&
        !strcmp(e_d_session.signal, E_D_SIGINT_NAME));
}

/**
 * e_d_gdbmi_settle - Interrupts that may still be owed at a stop of the
 * program's own.  An interrupt that met gdb at its prompt is answered
 * without a number, which no request waits for, so nothing is taken back;
 * one that reached the program comes back as a SIGINT stop at its next
 * resume, which is then run on from unseen.
 * Return: @owed.
 */
static int e_d_gdbmi_settle(ECNT *cn, int owed, int why)
{
 (void)cn;
 (void)why;
 return(owed);
}

/**
 * e_d_gm_stop_ended - Did the stop say the program ended?  Then say how in
 * Messages ("Program exited with code 3"), or in a box for a program a
 * signal killed, and end the session.
 * @f: the window on top.
 * Return: 1 when the session ended.
 */
static int e_d_gm_stop_ended(FENSTER *f)
{
 char msg[E_D_EXIT_LINE_MAX];

 if (!e_d_gm.stop_seen || e_d_session.stop_reason != E_DS_STOP_EXITED)
  return(0);
 if (e_d_session.signal[0])
 {
  e_error(e_d_msg[ERR_PROGTERM], 0, f->fb);
  e_d_quit(f);
  return(1);
 }
 snprintf(msg, sizeof msg, E_D_EXIT_CODE_FMT, (long)e_d_session.exit_code);
 e_d_p_message(msg, f, 0);
 e_d_quit(f);
 return(1);
}

/**
 * e_d_gm_say_signal - A signal stopped the program: say which, or note
 * that the user's Pause took hold.
 */
static void e_d_gm_say_signal(void)
{
 char line[E_D_GA_LINE_MAX];

 if (!e_d_gm.stop_seen)
  return;
 if (e_d_session.stop_reason == E_DS_STOP_PAUSE)
 {
  if (e_d_run.pause == E_D_PAUSE_ASKED)
   e_d_pause_taken();
  return;
 }
 if (e_d_session.stop_reason != E_DS_STOP_SIGNAL)
  return;
 snprintf(line, sizeof line, E_D_GDBMI_SIGNAL_FMT, e_d_session.signal,
          e_d_gm.meaning);
 e_d_error(line);
}

/**
 * e_d_gm_follow_up_after_finish - A step pressed after Pause first ran out
 * of the library call; the finish ended there: send the step itself.
 * Return: 1 when the step goes out and the run goes on.
 */
static int e_d_gm_follow_up_after_finish(void)
{
 const char *cmd = e_d_pause_follow;

 if (!cmd)
  return(0);
 e_d_pause_follow = NULL;
 if (!e_d_gm.finished)
  return(0);
 e_d_ga_go_on(cmd);
 return(1);
}

/**
 * e_d_gdbmi_stopped - gdb over MI stopped the program: ask it, on the fd
 * loop, what the stop needs, and show the stop once it has answered.
 * @f: the window on top.
 */
static void e_d_gdbmi_stopped(FENSTER *f)
{
 E_D_GA_CTX *st = &e_d_gs;

 if (e_d_gdb_run_never_started(f))
  return;
 if (e_d_gm_follow_up_after_finish())
 {
  e_d_run.stop_on_loop = 1;        /* the stop the user sees comes later */
  return;
 }
 e_d_drain_pty_to_messages();
 e_d_out_show();
 e_d_switch_out(0);
 if (e_d_gm_stop_ended(f))
  return;
 e_d_gm_say_signal();
 e_d_gs_reset(st, f->ed);
 st->start_brk = e_d_run.start_brk;
 e_d_run.start_brk = 0;
 st->stack_wanted = e_d_gs_stack_window_open(f->ed);
 e_d_run.stop_on_loop = 1;
 e_d_ga_chain_start(f->ed, e_d_gdbmi_stop_steps, st, e_d_gs_stop_finished,
                    E_D_GA_STOP);
}

/**
 * e_d_gm_pause_instead - Pause was pressed while gdb answered at a stop the
 * user was not shown: the stop is marked and the status row says "Paused".
 * @cn: the editor.
 */
static void e_d_gm_pause_instead(ECNT *cn)
{
 e_d_async_pending = 0;
 e_d_ga.pause_asked = 0;
 e_d_run.pause = E_D_PAUSE_ASKED;
 e_d_pause_taken();
 e_d_gdbmi_stopped(e_d_run_window(cn));
}

/**
 * e_d_gdbmi_sync_on_loop - The interrupt for a breakpoint change stopped
 * the program: tell gdb on the fd loop, then let it run on.
 */
static int e_d_gdbmi_sync_on_loop(ECNT *cn)
{
 return(e_d_ga_sync_with(cn, e_d_gdbmi_sync_steps));
}

/* gdb over MI: the run reads records; a stop is a *stopped record; an
   interrupt's stop is known by its reason. */
static const E_D_RUN_OPS e_d_gdbmi_ops =
{
 "gdb",
 e_d_line_answer_fd,
 e_d_line_output_fd,
 e_d_gdbmi_feed,
 e_d_gdbmi_stopped,
 e_d_interrupt_debugger,
 NULL,
 NULL,
 e_d_gdb_run_to_line,
 e_d_gm_begin,
 NULL,
 0,
 e_d_gdbmi_interrupt_stop,
 NULL,
 e_d_gdbmi_settle,
 e_d_gm_begin,
 0,
 NULL,
 e_d_gdbmi_sync_on_loop
};

/**
 * e_d_gdbmi_run_ops - The run controller's table for gdb over MI.
 */
static const E_D_RUN_OPS *e_d_gdbmi_run_ops(void)
{
 return(&e_d_gdbmi_ops);
}

/* ---- the backends ---- */

static const E_D_GA_BACKEND e_d_ga_gdbmi_backend =
{
 E_D_GDB_NAME,
 E_D_GDB_PROMPT,
 NULL,
 NULL,
 0,
 1,
 e_d_gm_brk_add_cmd,
 e_d_gm_brk_remove_cmd,
 e_d_gm_brk_set_id,
 e_d_gm_brk_verdict,
 e_d_gdb_remove_all,
 e_d_gdb_show_stack,
 e_d_gm_pause_instead,
 NULL,
 e_d_gm_ends_in_prompt,
 e_d_gm_start_brk_cmd,
 NULL,
 e_d_gm_line_is_async,
 e_d_gm_deliver_async,
 NULL,
 NULL,
 NULL,
 NULL,
 NULL,
 e_d_gdbmi_set_up_steps,
 e_d_ga_brk_steps,
 e_d_gdbmi_watches_steps,
 e_d_gdbmi_stack_steps,
 e_d_gdbmi_follow_steps,
 e_d_gdbmi_go_steps,
 NULL,
 NULL,
 NULL,
 e_d_gm_send,
 e_d_gm_refused_words
};

static const E_D_GA_BACKEND e_d_ga_gdb_backend =
{
 E_D_GDB_NAME,
 E_D_GDB_PROMPT,
 E_D_GDB_SETTLE_ECHO,
 E_D_GDB_SETTLE_MARK,
 1,
 1,
 e_d_gdb_brk_add_cmd,
 e_d_gdb_brk_remove_cmd,
 e_d_gdb_brk_set_id,
 e_d_gdb_brk_verdict,
 e_d_gdb_remove_all,
 e_d_gdb_show_stack,
 e_d_gs_pause_instead,
 NULL,
 e_d_ga_ends_in_prompt,
 e_d_gdb_start_brk_cmd,
 NULL,
 NULL,
 NULL,
 NULL,
 NULL,
 NULL,
 NULL,
 NULL,
 e_d_gdb_set_up_steps,
 e_d_ga_brk_steps,
 e_d_gdb_watches_steps,
 e_d_gdb_stack_steps,
 e_d_gdb_follow_steps,
 e_d_gdb_go_steps,
 NULL,
 NULL,
 NULL
};

static const E_D_GA_BACKEND e_d_ga_pdb_backend =
{
 E_D_PDB_NAME,
 E_D_PDB_PROMPT,
 E_D_PDB_SETTLE_CMD,
 E_D_PDB_SETTLE_MARK,
 0,
 0,
 e_d_pdb_brk_add_cmd,
 e_d_pdb_brk_remove_cmd,
 e_d_place_brk_set_id,
 e_d_place_brk_verdict,
 e_d_ga_remove_each_line,
 e_d_ga_frames_show_stack,
 e_d_gp_pause_instead,
 e_d_ga_finish_now,
 e_d_ga_ends_in_prompt,
 NULL,
 e_d_pdb_cursor_brk_cmd,
 NULL,
 NULL,
 NULL,
 NULL,
 NULL,
 NULL,
 NULL,
 e_d_pdb_set_up_steps,
 e_d_ga_brk_steps,
 e_d_pdb_watches_steps,
 e_d_pdb_stack_steps,
 e_d_pdb_follow_steps,
 e_d_pdb_go_steps,
 NULL,
 E_D_PDB_INT_ANSWER,
 e_d_pdb_settle_cmd_for
};

/* jdb's prompt is no fixed string (e_d_jdb_ends_in_prompt); E_D_JDB_BARE_PROMPT
   stands for it.  Its refusals of a breakpoint are on its output, and its
   first run is a "cont" that says nothing of its own. */
static const E_D_GA_BACKEND e_d_ga_jdb_backend =
{
 E_D_JDB_NAME,
 E_D_JDB_BARE_PROMPT,
 E_D_JDB_SETTLE_CMD,
 E_D_JDB_SETTLE_MARK,
 0,
 0,
 e_d_jdb_brk_add_cmd,
 e_d_jdb_brk_remove_cmd,
 e_d_place_brk_set_id,
 e_d_place_brk_verdict,
 e_d_ga_remove_each_line,
 e_d_ga_frames_show_stack,
 e_d_gj_pause_instead,
 e_d_ga_finish_now,
 e_d_jdb_ends_in_prompt,
 e_d_jdb_start_brk_cmd,
 e_d_jdb_cursor_brk_cmd,
 e_d_gj_line_is_async,
 e_d_gj_deliver_async,
 e_d_gj_unsolicited,
 e_d_gj_reads_on,
 e_d_gj_chain_over,
 e_d_gj_given_up,
 e_d_jdb_separate_prompts,
 e_d_jdb_set_up_steps,
 e_d_ga_brk_steps,
 e_d_jdb_watches_steps,
 e_d_jdb_stack_steps,
 e_d_jdb_follow_steps,
 e_d_jdb_go_steps,
 NULL,
 NULL,
 NULL
};

/* a68g's word prompt ends each answer; its refusals of a breakpoint are on
   its output ("monitor error"), and its first run is a "continue" that the
   run's reader takes.  It reports no event in the middle of an answer. */
static const E_D_GA_BACKEND e_d_ga_a68g_backend =
{
 E_D_A68G_NAME,
 E_D_A68G_PROMPT,
 E_D_A68G_SETTLE_CMD,
 E_D_A68G_SETTLE_MARK,
 0,
 0,
 e_d_a68g_brk_add_cmd,
 e_d_a68g_brk_remove_cmd,
 e_d_place_brk_set_id,
 e_d_place_brk_verdict,
 e_d_a68g_remove_all,
 e_d_ga_frames_show_stack,
 e_d_a68g_pause_instead,
 e_d_ga_finish_now,
 e_d_ga_ends_in_prompt,
 NULL,
 e_d_a68g_cursor_brk_cmd,
 NULL,
 NULL,
 NULL,
 NULL,
 NULL,
 NULL,
 NULL,
 e_d_a68g_set_up_steps,
 e_d_a68g_brk_steps,
 e_d_a68g_watches_steps,
 e_d_a68g_stack_steps,
 e_d_a68g_watches_steps,
 e_d_a68g_go_steps,
 NULL,
 NULL,
 NULL
};

/* sdb, dbx and xdb answer on their output and refuse on their error stream,
   as gdb does.  dbx and xdb name a breakpoint by the number they give it;
   sdb by its line, in the file it was last told with "e" (its own steps
   tell it before every such command).  None reports an event in the middle
   of an answer, and none has Finish. */
static const E_D_GA_BACKEND e_d_ga_sdb_backend =
{
 E_D_SDB_NAME,
 E_D_SDB_PROMPT,
 E_D_SDB_SETTLE_CMD,
 E_D_CLI_SETTLE_MARK,
 1,
 0,
 e_d_sdb_brk_add_cmd,
 e_d_sdb_brk_remove_cmd,
 e_d_gsdb_brk_set_id,
 e_d_gsdb_brk_verdict,
 e_d_gc_remove_all,
 e_d_gdb_show_stack,
 e_d_gc_pause_instead,
 NULL,
 e_d_ga_ends_in_prompt,
 NULL,
 e_d_sdb_cursor_brk_cmd,
 NULL,
 NULL,
 NULL,
 NULL,
 NULL,
 NULL,
 NULL,
 e_d_sdb_set_up_steps,
 e_d_sdb_brk_steps,
 e_d_gc_watches_steps,
 e_d_gc_stack_steps,
 e_d_gc_watches_steps,
 e_d_sdb_go_steps,
 e_d_gc_run_went,
 NULL,
 NULL
};

static const E_D_GA_BACKEND e_d_ga_dbx_backend =
{
 E_D_DBX_NAME,
 E_D_DBX_PROMPT,
 E_D_DBX_SETTLE_CMD,
 E_D_CLI_SETTLE_MARK,
 1,
 0,
 e_d_dbx_brk_add_cmd,
 e_d_dbx_brk_remove_cmd,
 e_d_gc_brk_set_id,
 e_d_gc_brk_verdict,
 e_d_gc_remove_all,
 e_d_gdb_show_stack,
 e_d_gc_pause_instead,
 NULL,
 e_d_ga_ends_in_prompt,
 e_d_dbx_start_brk_cmd,
 e_d_dbx_cursor_brk_cmd,
 NULL,
 NULL,
 NULL,
 NULL,
 NULL,
 NULL,
 NULL,
 e_d_dbx_set_up_steps,
 e_d_ga_brk_steps,
 e_d_gc_watches_steps,
 e_d_gc_stack_steps,
 e_d_dbx_follow_steps,
 e_d_dbx_go_steps,
 e_d_gc_run_went,
 NULL,
 NULL
};

static const E_D_GA_BACKEND e_d_ga_xdb_backend =
{
 E_D_XDB_NAME,
 E_D_XDB_PROMPT,
 E_D_XDB_SETTLE_CMD,
 E_D_CLI_SETTLE_MARK,
 1,
 0,
 e_d_xdb_brk_add_cmd,
 e_d_xdb_brk_remove_cmd,
 e_d_gc_brk_set_id,
 e_d_gc_brk_verdict,
 e_d_gc_remove_all,
 e_d_gdb_show_stack,
 e_d_gc_pause_instead,
 NULL,
 e_d_ga_ends_in_prompt,
 e_d_xdb_start_brk_cmd,
 e_d_xdb_cursor_brk_cmd,
 NULL,
 NULL,
 NULL,
 NULL,
 NULL,
 NULL,
 NULL,
 e_d_xdb_set_up_steps,
 e_d_xdb_brk_steps,
 e_d_gc_watches_steps,
 e_d_gc_stack_steps,
 e_d_xdb_follow_steps,
 e_d_xdb_go_steps,
 e_d_gc_run_went,
 NULL,
 NULL
};

/**
 * e_d_ga_backend_of - The request engine's table for debugger @type.
 * Return: gdb's, pdb's, jdb's, a68g's, sdb's, dbx's or xdb's, or NULL for a
 * debugger whose answers are read otherwise (the debug adapters').
 */
static const E_D_GA_BACKEND *e_d_ga_backend_of(int type)
{
 if (type == DEB_GDB && e_d_gdbmi_on())
  return(&e_d_ga_gdbmi_backend);
 if (type == DEB_GDB)
  return(&e_d_ga_gdb_backend);
 if (type == DEB_PDB)
  return(&e_d_ga_pdb_backend);
 if (type == DEB_JDB)
  return(&e_d_ga_jdb_backend);
 if (type == DEB_A68G)
  return(&e_d_ga_a68g_backend);
 if (type == DEB_SDB)
  return(&e_d_ga_sdb_backend);
 if (type == DEB_DBX)
  return(&e_d_ga_dbx_backend);
 if (type == DEB_XDB)
  return(&e_d_ga_xdb_backend);
 return(NULL);
}

/**
 * e_d_ga_remove_all - Remove all breakpoints at a stop of gdb's, pdb's,
 * jdb's or a68g's, in the way the debugger that runs takes it, on the fd
 * loop.
 * @cn: the editor.
 */
static void e_d_ga_remove_all(ECNT *cn)
{
 e_d_ga_this()->remove_all(cn);
}

int e_d_delbreak(FENSTER *f)
{
 ECNT *cn = f->ed;
 int i;

 for (i = cn->mxedt; i >= 0; i--)
  if (DTMD_ISTEXT(cn->f[i]->dtmd))
   cn->f[i]->s->da.y = -1;
 e_rep_win_tree(cn);
 e_refresh();
 return(0);
}

int e_d_error(char *s)
{
 int len;

 e_d_switch_out(0);
 if (s[(len = strlen(s) - 1)] == '\n')
  s[len] = '\0';
 return(e_error(s, 0, WpeEditor->fb));
}

/* The graphical editor's debugger terminal, as the stall line names it. */
#define E_D_TERMINAL_WHAT "the debugger's terminal"

/**
 * e_d_putchar - Show one character of the debugger's session: on the
 * editor's screen, or in the graphical editor on the terminal the debugger
 * runs in, never waiting for that terminal to take it.
 * @c: the character.
 * Return: @c, or -1 when the terminal is closed.
 */
int e_d_putchar(int c)
{
 char cc = c;
 int r;

 if (!WpeIsXwin())
  return(fk_putchar(c));
 r = wpe_fd_write(wfildes[1], &cc, 1, E_D_TERMINAL_WHAT, E_D_INPUT_ENDS);
 if (!e_d_send_went(r))
  return(-1);
 return(c);
}

/* The debugger dialog's radio rows, in the order they are drawn.  A build
   that offers xdb gives it a row of its own, so every backend after dbx sits
   one row lower there than in a build that does not. */
#define E_D_RADIO_GDB   0
#define E_D_RADIO_SDB   1
#define E_D_RADIO_DBX   2
#ifdef XDB
#define E_D_RADIO_XDB   3
#define E_D_RADIO_JDB   4
#define E_D_RADIO_PDB   5
#define E_D_RADIO_A68G  6
#else
#define E_D_RADIO_JDB   3
#define E_D_RADIO_PDB   4
#define E_D_RADIO_A68G  5
#endif

/**
 * e_d_dialog_radio_of - The dialog row that names the selected backend.
 *
 * Opening Debug > debugger Options shows the mark on the backend that is
 * already chosen, so the user reads which debugger the next run starts
 * instead of having to remember what the options file said.
 * Return: that row's index in the dialog's radio group.
 */
static int e_d_dialog_radio_of(void)
{
 if (e_deb_type == DEB_A68G)
  return(E_D_RADIO_A68G);
 if (e_deb_type == DEB_PDB)
  return(E_D_RADIO_PDB);
 if (e_deb_type == DEB_JDB)
  return(E_D_RADIO_JDB);
#ifdef XDB
 if (e_deb_type == DEB_XDB)
  return(E_D_RADIO_XDB);
#else
 /* This build offers xdb no row, yet the options file can still name one:
    "Debugger : 3" is taken as it stands and e_start_debug runs "xdb -L"
    whether or not XDB is defined.  dbx holds the row xdb would take, so the
    mark lands there, as it did before xdb had a row of its own, instead of on
    an unrelated backend. */
 if (e_deb_type == DEB_XDB)
  return(E_D_RADIO_DBX);
#endif
 /* gdb, sdb and dbx number their own rows. */
 return(e_deb_type);
}

/**
 * e_d_dialog_type_of - The backend the dialog row @sel names.
 *
 * Picking a row decides which debugger the next Ctrl-G R starts, so the row
 * and the backend have to agree whether or not this build offers xdb: a row
 * that named the wrong backend would start one the user did not choose.
 * Return: the e_deb_type value for that row.
 */
static int e_d_dialog_type_of(int sel)
{
 if (sel == E_D_RADIO_A68G)
  return(DEB_A68G);
 if (sel == E_D_RADIO_PDB)
  return(DEB_PDB);
 if (sel == E_D_RADIO_JDB)
  return(DEB_JDB);
#ifdef XDB
 if (sel == E_D_RADIO_XDB)
  return(DEB_XDB);
#endif
 /* gdb, sdb and dbx number their own rows. */
 return(sel);
}

int e_deb_options(FENSTER *f)
{
 int ret;
 W_OPTSTR *o = e_init_opt_kst(f);

 if (!o) return(-1);
 o->xa = 20;  o->ya = 4;  o->xe = 60;  o->ye = 18;
 o->bgsw = 0;
 o->name = "Debug-Options";
 o->crsw = AltO;
 e_add_txtstr(4, 2, "Debugger:", o);
 e_add_txtstr(20, 2, "Mode:", o);
 { /* Auto-select debugger by file extension */
   ECNT *cn2 = f->ed;
   char *_fn = cn2->f[cn2->mxedt]->datnam;
   int _fnl = strlen(_fn);
   if (_fnl > 5 && !strcmp(_fn + _fnl - 5, ".java"))
    e_deb_type = DEB_JDB;
   else if (_fnl > 3 && !strcmp(_fn + _fnl - 3, ".py"))
    e_deb_type = DEB_PDB;
   else if (_fnl > 4 && (!strcmp(_fn + _fnl - 4, ".a68") ||
                         !strcmp(_fn + _fnl - 4, ".alg")))
    /* full path (dir + name) so a source outside the current dir is detected */
    e_deb_type = e_algol68_use_ga68_in(cn2->f[cn2->mxedt]->dirct, _fn)
                 ? DEB_GDB : DEB_A68G;
   int _sel = e_d_dialog_radio_of();
 e_add_pswstr(0, 5, 3, 0, AltG, 0, "Gdb    ", o);
 e_add_pswstr(0, 5, 4, 0, AltS, 0, "Sdb    ", o);
 e_add_pswstr(0, 5, 5, 0, AltD, 0, "Dbx    ", o);
#ifdef XDB
 /* xdb takes a row of its own here, so dbx keeps the one above it and both
    are selectable; y=9 is the last row the group has room for, Ok and
    Cancel being at y=11. */
 e_add_pswstr(0, 5, 6, 0, AltX, 0, "Xdb    ", o);
 e_add_pswstr(0, 5, 7, 0, AltJ, 0, "Jdb    ", o);
 e_add_pswstr(0, 5, 8, 0, AltP, 0, "Pdb    ", o);
 e_add_pswstr(0, 5, 9, 0, AltA, _sel, "A68g   ", o);
#else
 e_add_pswstr(0, 5, 6, 0, AltJ, 0, "Jdb    ", o);
 e_add_pswstr(0, 5, 7, 0, AltP, 0, "Pdb    ", o);
 e_add_pswstr(0, 5, 8, 0, AltA, _sel, "A68g   ", o);
#endif
 }
 e_add_pswstr(1, 21, 3, 0, AltN, 0, "Normal     ", o);
 e_add_pswstr(1, 21, 4, 0, AltF, e_deb_mode, "Full Screen", o);
 /* DAP adapter preference (Rust: gdb vs lldb-dap).  "Auto" picks the first one
    found in PATH -- lldb-dap on a gdb-less macOS box, gdb on Linux.  Persisted
    in xwperc; $XWPE_DAP_ADAPTER still overrides at run time. */
 e_add_txtstr(20, 6, "DAP adapter:", o);
 e_add_pswstr(2, 21, 7, 0, AltU, 0, "Auto", o);
 e_add_pswstr(2, 21, 8, 0, AltB, 0, "gdb ", o);
 e_add_pswstr(2, 21, 9, 0, AltL, e_dap_adapter, "lldb", o);
 e_add_bttstr(10, 11, 1, AltO, " Ok ", NULL, o);
 e_add_bttstr(25, 11, -1, WPE_ESC, "Cancel", NULL, o);
 ret = e_opt_kst(o);
 if (ret != WPE_ESC)
 {
  e_deb_type = e_d_dialog_type_of(o->pstr[0]->num);
  e_deb_mode = o->pstr[1]->num;
  e_dap_adapter = o->pstr[2]->num;    /* 0=Auto, 1=gdb, 2=lldb-dap */
 }
 freeostr(o);
 return(0);
}

int e_g_sys_ini()
{
 if (!e_d_swtch || e_deb_mode)
  return(0);
 tcgetattr(0, &ttermio);
 return(tcsetattr(0, TCSADRAIN, &otermio));
}

int e_g_sys_end()
{
 if (!e_d_swtch || e_deb_mode)
  return(0);
 return(tcsetattr(0, TCSADRAIN, &ttermio));
}

int e_test_command(char *str)
{
 int i = -1, k;
 char tmp[256], *path = getenv("PATH");

 if (!path) return(-2);
 do
 {
  for(i++, k = 0; (tmp[k] = path[i]) && path[i] != ':'; k++, i++)
   ;
  if (k == 0)
  {  tmp[0] = '.';  k++;  }
  tmp[k] = '/';
  tmp[k+1] = '\0';
  strcat(tmp, str);
  if (!access(tmp, X_OK)) return(0);
 } while(path[i]);
 return(-1);
}

/***********************************************************************
 *  LSP editor bridge -- Language Server Protocol (IDE features).      *
 *  Where the DAP bridge above makes xwpe a debugger, this makes it    *
 *  an IDE: diagnostics, go-to-definition, hover and completion from   *
 *  the same servers VS Code / Neovim / Emacs use (Metals for Scala).  *
 *  Engine: we_lsp.c (free of editor state).  Keys: AltQ then a letter *
 *  (D definition, H hover, C complete) -- "Query the language server".*
 ***********************************************************************/

static e_lsp_session *g_lsp = NULL;        /* the live server session, or NULL */
static char           g_lsp_file[1024] = "";  /* full path g_lsp is opened for */
static FENSTER       *g_lsp_fenster = NULL;
static FENSTER       *g_lsp_pending_ws = NULL; /* worksheet to focus once ready */
/**
 * e_lsp_screen_busy - Is the screen not the editor's to draw on?
 *
 * True while a box, a menu, a run or a command owns it.  The server's news
 * is still read then; what it would draw waits for the screen's owner, and
 * what the server owes the editor (the readiness line, the overlays that
 * went stale) is asked again when the screen is back.
 */
static int e_lsp_screen_busy(void)
{
 return(!wpe_paint_idle());
}

static int            g_lsp_quiet = 0;     /* suppress per-diagnostic Messages
                                              output (live polls show a summary) */
static int            g_lsp_last_err = -1; /* diagnostic totals the server last */
static int            g_lsp_last_warn = -1;/* published                          */
static int            g_lsp_said_err = -1; /* totals the "LSP: N error(s)" line */
static int            g_lsp_said_warn = -1;/* last told, so it is not spammed    */
static int            g_lsp_doctor_announced = 0; /* "Doctor updated" shown once?  */
static char          *g_lsp_doctor_last = NULL; /* last Doctor body shown, for   */
                                           /* dedup; reset per session so a       */
                                           /* re-started Metals shows it again    */
static int            g_lsp_busy = 0;      /* server is mid-task (indexing, ...)  */
static char          *g_lsp_status_last = NULL; /* last status text shown (dedup) */

/* ---- inline diagnostic marks -------------------------------------------- *
 * publishDiagnostics carries a RANGE per problem; we keep the ranges for the
 * file the server is open for and let the editor's per-character paint
 * (e_pr_c_line) recolor those cells -- error spans red, warnings amber -- the
 * cell-grid analogue of an editor's red squiggle.  Each batch replaces the set
 * atomically: on_diagnostic fills `pending`, on_diagnostics_summary (the single
 * end-of-batch callback) swaps it into `active`, which the renderer reads. */
#define LSP_MAX_DIAG 256
typedef struct { int line, c0, c1, sev; char msg[160]; } e_lsp_diag_mark;
static e_lsp_diag_mark g_diag_active[LSP_MAX_DIAG];   /* what the renderer shows */
static int             g_diag_nactive = 0;
static e_lsp_diag_mark g_diag_pending[LSP_MAX_DIAG];  /* filling this batch      */
static int             g_diag_npending = 0;

/* document-highlight spans: occurrences of the symbol under the cursor, marked
   on demand (Alt-Q L) in the found-word colour -- separate from diagnostics so
   the two can coexist (a highlighted name that also has a warning). */
static e_lsp_diag_mark g_hl_active[LSP_MAX_DIAG];
static int             g_hl_nactive = 0;

/* e_lsp_diag_snapshot - the current file's live diagnostics as plain text, so
   the AI assistant can put the same errors/warnings the user sees (the red
   squiggles) into its prompt and "fix this" works without pasting them.  Writes
   one per line ("  line N (error|warning): msg"); returns how many. */
int e_lsp_diag_snapshot(char *out, size_t sz)
{
 int i, n = 0;
 size_t len = 0;
 if (!out || sz == 0) return 0;
 out[0] = '\0';
 for (i = 0; i < g_diag_nactive && len + 96 < sz; i++) {
  const char *sev = g_diag_active[i].sev == 1 ? "error" : "warning";
  len += (size_t)snprintf(out + len, sz - len, "  line %d (%s): %.140s\n",
                          g_diag_active[i].line + 1, sev, g_diag_active[i].msg);
  n++;
 }
 return n;
}

extern int col_num;                        /* 0 = monochrome ncurses (we_unix.c) */

/* The cell attribute for a diagnostic of `severity`, correct for the active
   backend: a VGA fg/bg byte (16*bg+fg) when colour is available (X11 or colour
   ncurses, decoded by fk_colset as bg=c/16, fg=c%16), or an existing marked
   attribute under monochrome ncurses (which has no colour to give). */
static int e_lsp_diag_color(SCHIRM *s, int severity)
{
 int is_err = (severity == 1);
 if (WpeIsXwin() || col_num > 0)
  return is_err ? (16 * 1 + 15)    /* bright white on red   -> error   */
                : (16 * 3 + 15);   /* bright white on brown -> warning */
 return is_err ? s->fb->db.fb : s->fb->ek.fb;  /* mono: standout / underline */
}

/* The file currently shown in `f`, or NULL.  (dirct already ends in '/'.) */
static const char *e_lsp_path_of(FENSTER *f, char *buf, size_t sz)
{
 if (!f || !f->datnam)
  return(NULL);
 snprintf(buf, sz, "%s%s", f->dirct ? f->dirct : "./", f->datnam);
 return(buf);
}

/* Semantic-tokens overlay state (server-driven syntax highlighting, Alt-Q M).
   g_sem_active holds the whole file's decoded tokens; g_sem_on is the per-file
   toggle; g_sem_stale is set when the server asks us to re-query. */
#define LSP_MAX_SEM 8192
static e_lsp_sem_token g_sem_active[LSP_MAX_SEM];
static int             g_sem_nactive = 0;
static int             g_sem_on = 0;
static int             g_sem_stale = 0;

/* The foreground colour (0-15) for a semantic token type, or -1 for "leave it"
   (identifiers the regex already handles fine: variables, operators...).  This
   is the dedicated semantic palette -- distinct colours for the categories that
   matter, so type != keyword != function != parameter.  Comments reuse the
   theme's comment foreground; the rest are fixed 16-colour choices that read
   well on the Borland-blue editor background. */
static int e_lsp_sem_fg(SCHIRM *s, const char *t)
{
 /* Palette indices are xwpe's X11 colour order (WeXterm.c WpeXColorNames), NOT
    ANSI -- e.g. 14 is Turquoise1 (a teal), 11 is Yellow, 12 is Blue.  These are
    chosen to READ on the Borland-blue editor background: warm/light colours.
    Cool colours (teal 14/6, Blue 12) and the dark Forest Green (2) wash out on
    blue, so they are avoided. */
 if (!t)
  return(-1);
 if (!strcmp(t, "keyword") || !strcmp(t, "modifier"))
  return(15);                            /* White */
 if (!strcmp(t, "type") || !strcmp(t, "class") || !strcmp(t, "interface") ||
     !strcmp(t, "enum") || !strcmp(t, "struct") || !strcmp(t, "typeParameter") ||
     !strcmp(t, "namespace"))
  return(11);                            /* Yellow -- the key win: types */
 if (!strcmp(t, "function") || !strcmp(t, "method") || !strcmp(t, "macro"))
  return(9);                             /* bright Red -- the warmest of the 16
                                            colours, high contrast on blue and
                                            distinct from variable-green.  (True
                                            orange would need a 17th colour / palette
                                            redefinition, which does not pass through
                                            a typical tmux; Red works everywhere.) */
 if (!strcmp(t, "parameter"))
  return(13);                            /* Violet */
 if (!strcmp(t, "variable") || !strcmp(t, "property") || !strcmp(t, "enumMember"))
  return(10);                            /* bright Green */
 if (!strcmp(t, "string") || !strcmp(t, "regexp"))
  return(5);                             /* Violet Red (was Brown 3, murky) */
 /* number: left to the base highlighter (would share Red with methods, and the
    base already shows literals in a readable colour on blue). */
 if (!strcmp(t, "comment"))
  return(s->fb->cc.fb & 0x0F);           /* the theme's comment foreground */
 return(-1);                             /* operator / event / decorator / ... */
}

/* The attribute to paint at cell (y,x) when the semantic overlay is on: if a
   token covers it, keep the cell's background (16*(base/16)) and swap in the
   token category's foreground; otherwise return `base` unchanged.  Called per
   cell from e_lsp_decor_attr_at, AFTER diagnostics/highlight (which win). */
static int e_lsp_sem_color_at(SCHIRM *s, int y, int x, int base)
{
 int i, fg, slot;
 const char *legend;

 if (!g_sem_on)
  return(base);
 for (i = 0; i < g_sem_nactive; i++)
  if (g_sem_active[i].line == y && x >= g_sem_active[i].start &&
      x < g_sem_active[i].start + g_sem_active[i].length)
  {
   legend = e_lsp_semantic_legend(g_lsp, g_sem_active[i].type);
   fg = e_lsp_sem_fg(s, legend);
   if (fg < 0)
    return(base);
   /* Keep the cell bg, set the token fg.  If the category has a dedicated
      24-bit colour (e.g. method -> orange), flag the cell so the X11 and the
      direct-colour console renderers paint the true RGB; the 16-colour fg
      stays in the low byte as the fallback for everything else. */
   slot = e_lsp_sem_truecolor(legend);
   if (slot >= 0)
    return(ATTR_TC_MAKE(slot, 16 * (base / 16) + fg));
   return(16 * (base / 16) + fg);
  }
 return(base);
}

/* The server's copy of the document changes at every didOpen and didChange;
   an overlay fetched for an older copy has its positions on other text. */
static unsigned long g_lsp_doc_gen = 0;

static void e_lsp_deliver_held(void *data);   /* (below) */

/**
 * e_lsp_deliver_later - An answer or an overlay came while a box owns the
 * screen: the screen's owner shows it once the box has closed.
 *
 * While the editor is idle nothing is asked: the reader that took the
 * answer shows it itself once its read has returned, never from inside it.
 */
static void e_lsp_deliver_later(void)
{
 if (wpe_paint_idle())
  return;
 wpe_paint_request(e_lsp_deliver_held, NULL, "a language server's answer");
}

/* What an overlay fetch was sent for: the file and the server's copy of it. */
typedef struct
{
 unsigned long gen;
 char path[1024];
} e_lsp_fetch_ticket;

/**
 * e_lsp_fetch_ticket_new - Note the file and the server's copy a fetch is
 * for, so its answer is painted only on that text.
 * Return: the ticket (the fetch's continuation frees it), or NULL.
 */
static e_lsp_fetch_ticket *e_lsp_fetch_ticket_new(void)
{
 e_lsp_fetch_ticket *t = calloc(1, sizeof(*t));

 if (!t)
  return(NULL);
 t->gen = g_lsp_doc_gen;
 snprintf(t->path, sizeof t->path, "%s", g_lsp_file);
 return(t);
}

/**
 * e_lsp_fetch_ticket_current - Is the file still the server's, and its
 * copy the one the fetch was sent for?
 */
static int e_lsp_fetch_ticket_current(unsigned long gen, const char *path)
{
 return(gen == g_lsp_doc_gen && !strcmp(path, g_lsp_file));
}

/* A semantic token answer kept until the editor may paint it. */
static struct
{
 int ready;
 unsigned long gen;
 char path[1024];
 int n;
 e_lsp_sem_token tokens[LSP_MAX_SEM];
} g_sem_held;

/**
 * e_lsp_sem_told - The tokens came: kept, painted later by
 * e_lsp_overlay_deliver.  A fetch replaced by a newer one, or ended
 * without an answer, leaves nothing.
 */
static void e_lsp_sem_told(E_D_REQ_END how, const e_lsp_sem_token *tokens,
                           int n, void *ctx)
{
 e_lsp_fetch_ticket *t = ctx;

 if (how == E_D_REQ_DONE && n >= 0)
 {
  if (n > LSP_MAX_SEM)
   n = LSP_MAX_SEM;
  memcpy(g_sem_held.tokens, tokens, (size_t)n * sizeof(*tokens));
  g_sem_held.n = n;
  g_sem_held.gen = t->gen;
  snprintf(g_sem_held.path, sizeof g_sem_held.path, "%s", t->path);
  g_sem_held.ready = 1;
 }
 free(t);
}

/* Ask for the whole file's semantic tokens; the overlay takes them when the
   server answers.  The editor never waits: a newer fetch replaces this one. */
static void e_lsp_sem_fetch(FENSTER *f)
{
 e_lsp_fetch_ticket *t;

 if (!g_lsp || !f)
  return;
 t = e_lsp_fetch_ticket_new();
 if (!t)
  return;
 if (e_lsp_semantic_tokens_async(g_lsp, g_lsp_file, LSP_MAX_SEM,
                                 e_lsp_sem_told, t) != E_D_SEND_OK)
  free(t);
}

/* host callback (workspace/semanticTokens/refresh): the classification changed
   (e.g. indexing resolved a cross-file type).  Fired inside the server's read
   -- only flag stale; e_lsp_sem_refresh_pending re-fetches once it returned. */
static void e_lsp_on_semantic_refresh(void *ud)
{
 (void)ud;
 if (g_sem_on)
  g_sem_stale = 1;
}

/* If the overlay is on and the server flagged it stale, fetch it again; it
   is repainted when the answer comes.  Called after a poll, never inside. */
static void e_lsp_sem_refresh_pending(FENSTER *f)
{
 if (!g_sem_on || !g_sem_stale || !g_lsp)
  return;
 g_sem_stale = 0;
 e_lsp_sem_fetch(f);
}

/* Called once per line by the renderer: are there any LSP decorations (problem
   marks or highlight spans) to draw for the file in `f`?  Keeps the
   per-character query (below) free of path work. */
int e_lsp_decor_active_for(FENSTER *f)
{
 char path[1200];
 if (!g_lsp || (g_diag_nactive == 0 && g_hl_nactive == 0 &&
                !(g_sem_on && g_sem_nactive > 0)))
  return(0);
 if (!e_lsp_path_of(f, path, sizeof(path)))
  return(0);
 return(strcmp(path, g_lsp_file) == 0);
}

/* Per-character: the colour for cell (y,x).  A diagnostic range wins (error over
   warning); else a document-highlight span paints the found-word colour; else
   the caller's base colour. */
int e_lsp_decor_attr_at(SCHIRM *s, int y, int x, int base)
{
 int i, warn = -1;
 for (i = 0; i < g_diag_nactive; i++)
  if (g_diag_active[i].line == y &&
      x >= g_diag_active[i].c0 && x < g_diag_active[i].c1)
  {
   if (g_diag_active[i].sev == 1)        /* error: take it immediately */
    return(e_lsp_diag_color(s, 1));
   if (warn < 0)
    warn = i;                            /* remember a warning, keep scanning */
  }
 if (warn >= 0)
  return(e_lsp_diag_color(s, g_diag_active[warn].sev));
 for (i = 0; i < g_hl_nactive; i++)
  if (g_hl_active[i].line == y &&
      x >= g_hl_active[i].c0 && x < g_hl_active[i].c1)
   return(s->fb->ek.fb);                 /* found-word colour */
 return(e_lsp_sem_color_at(s, y, x, base)); /* else the semantic overlay, or base */
}

/* Drop all decorations (server closing, or switching the open file). */
static void e_lsp_diag_clear(void)
{
 g_diag_nactive = 0;
 g_diag_npending = 0;
 g_hl_nactive = 0;
}

/* ---- inlay hints (Alt-Q Y) ---------------------------------------------- *
 * An OPT-IN, end-of-line overlay: when on, each line's inferred-type hints are
 * drawn as a distinct grey pill after the line's text (the renderer calls
 * e_lsp_inlay_eol_text per
 * line).  Unlike true IDE inlay hints we do NOT insert mid-line virtual text --
 * that would need a buffer<->screen column remap the 1993 cell grid has no layer
 * for -- so the hint sits at the line end, conveying the same information (": T")
 * without disturbing the cursor/editing/scroll invariants.  Off by default. */
#define LSP_MAX_INLAY 1024
static e_lsp_inlay_hint g_inlay_active[LSP_MAX_INLAY];
static int             g_inlay_nactive = 0;
static int             g_inlay_on = 0;    /* the toggle (per open file)         */
static int             g_inlay_stale = 0; /* server asked us to re-query hints   */

static void e_lsp_inlay_clear(void)
{
 int i;
 for (i = 0; i < g_inlay_nactive; i++)
  free(g_inlay_active[i].label);
 g_inlay_nactive = 0;
}

/* An inlay hint answer kept until the editor may paint it (its labels are
   copies: the engine's are gone after the continuation). */
static struct
{
 int ready;
 unsigned long gen;
 char path[1024];
 int n;
 e_lsp_inlay_hint hints[LSP_MAX_INLAY];
} g_inlay_held;

/**
 * e_lsp_inlay_held_clear - Forget the hints kept for painting.
 */
static void e_lsp_inlay_held_clear(void)
{
 int i;

 for (i = 0; i < g_inlay_held.n; i++)
  free(g_inlay_held.hints[i].label);
 g_inlay_held.n = 0;
 g_inlay_held.ready = 0;
}

/**
 * e_lsp_inlay_told - The hints came: copied and kept, painted later by
 * e_lsp_overlay_deliver.  A fetch replaced by a newer one, or ended
 * without an answer, leaves the kept ones as they were.
 */
static void e_lsp_inlay_told(E_D_REQ_END how, const e_lsp_inlay_hint *hints,
                             int n, void *ctx)
{
 e_lsp_fetch_ticket *t = ctx;
 int i;

 if (how == E_D_REQ_DONE && n >= 0)
 {
  e_lsp_inlay_held_clear();
  for (i = 0; i < n && g_inlay_held.n < LSP_MAX_INLAY; i++)
  {
   if (!hints[i].label || !hints[i].label[0])
    continue;
   g_inlay_held.hints[g_inlay_held.n] = hints[i];
   g_inlay_held.hints[g_inlay_held.n].label = strdup(hints[i].label);
   g_inlay_held.n++;
  }
  g_inlay_held.gen = t->gen;
  snprintf(g_inlay_held.path, sizeof g_inlay_held.path, "%s", t->path);
  g_inlay_held.ready = 1;
 }
 free(t);
}

/* Ask for the file's inferred-type hints; the overlay takes them when the
   server answers.  The editor never waits: a newer fetch replaces this one. */
static void e_lsp_inlay_fetch(FENSTER *f)
{
 e_lsp_fetch_ticket *t;
 int last;

 if (!g_lsp || !f || !f->b)
  return;
 last = f->b->mxlines - 1;
 if (last < 0)
  last = 0;
 t = e_lsp_fetch_ticket_new();
 if (!t)
  return;
 if (e_lsp_inlay_hints_async(g_lsp, g_lsp_file, 0, last, LSP_MAX_INLAY,
                             e_lsp_inlay_told, t) != E_D_SEND_OK)
  free(t);
}

/* host callback (workspace/inlayHint/refresh): the server re-queried -- the
   current hints may be a stale subset (cold toggles miss cross-file inferred
   types until indexing settles).  Fired from inside the server's read, so only
   mark the overlay stale; e_lsp_inlay_refresh_pending re-fetches once it
   returned. */
static void e_lsp_on_inlay_refresh(void *ud)
{
 (void)ud;
 if (g_inlay_on)
  g_inlay_stale = 1;
}

/* If the overlay is on and the server flagged it stale, fetch the hints
   again; they are painted when the answer comes.  Called after a poll, never
   inside one.  Returns 1 if a fetch went out, else 0. */
static int e_lsp_inlay_refresh_pending(FENSTER *f)
{
 if (!g_inlay_on || !g_inlay_stale || !g_lsp)
  return(0);
 g_inlay_stale = 0;
 e_lsp_inlay_fetch(f);
 return(1);
}

/**
 * e_lsp_overlay_repaint - Show an overlay just taken on the language
 * server's window; flushed, since this may run from the loop with no key
 * after it (worksheet results).
 */
static void e_lsp_overlay_repaint(void)
{
 FENSTER *f = g_lsp_fenster;

 if (!f || !DTMD_ISTEXT(f->dtmd))
  return;
 e_schirm(f, 1);
 e_cursor(f, 0);
 e_refresh();
}

/**
 * e_lsp_inlay_take - The kept hints become the overlay, when they are for
 * the server's present copy of the file; older ones are dropped without a
 * line and asked for again.
 * Return: 1 when the overlay changed.
 */
static int e_lsp_inlay_take(void)
{
 int i;

 if (!g_inlay_held.ready)
  return(0);
 if (!g_inlay_on)
 {
  e_lsp_inlay_held_clear();
  return(0);
 }
 if (!e_lsp_fetch_ticket_current(g_inlay_held.gen, g_inlay_held.path))
 {
  e_lsp_inlay_held_clear();
  g_inlay_stale = 1;
  return(0);
 }
 e_lsp_inlay_clear();
 for (i = 0; i < g_inlay_held.n; i++)
  g_inlay_active[i] = g_inlay_held.hints[i];   /* the labels move over */
 g_inlay_nactive = g_inlay_held.n;
 g_inlay_held.n = 0;
 g_inlay_held.ready = 0;
 return(1);
}

/**
 * e_lsp_sem_take - The kept tokens become the overlay, when they are for
 * the server's present copy of the file; older ones are dropped without a
 * line and asked for again.
 * Return: 1 when the overlay changed.
 */
static int e_lsp_sem_take(void)
{
 if (!g_sem_held.ready)
  return(0);
 g_sem_held.ready = 0;
 if (!g_sem_on)
  return(0);
 if (!e_lsp_fetch_ticket_current(g_sem_held.gen, g_sem_held.path))
 {
  g_sem_stale = 1;
  return(0);
 }
 memcpy(g_sem_active, g_sem_held.tokens,
        (size_t)g_sem_held.n * sizeof(g_sem_held.tokens[0]));
 g_sem_nactive = g_sem_held.n;
 return(1);
}

/**
 * e_lsp_overlay_held_drop - Forget the overlay answers not painted yet
 * (the session ends, or another file's server replaces it).
 */
static void e_lsp_overlay_held_drop(void)
{
 e_lsp_inlay_held_clear();
 g_sem_held.ready = 0;
}

/**
 * e_lsp_overlay_deliver - Paint the inlay hints and semantic colours the
 * server answered with, once nothing is drawn over the editor (a dialog,
 * a menu, a command that has the terminal); a stale answer is fetched
 * again by the next refresh, never painted.
 */
static void e_lsp_overlay_deliver(void)
{
 int changed;

 if (!g_inlay_held.ready && !g_sem_held.ready)
  return;
 if (!wpe_paint_idle())
 {
  e_lsp_deliver_later();
  return;
 }
 changed = e_lsp_inlay_take();
 changed |= e_lsp_sem_take();
 if (changed)
  e_lsp_overlay_repaint();
}

/* Per-line gate for the renderer: is the inlay overlay on for the file in f? */
int e_lsp_inlay_active_for(FENSTER *f)
{
 char path[1200];
 if (!g_lsp || !g_inlay_on || g_inlay_nactive == 0)
  return(0);
 if (!e_lsp_path_of(f, path, sizeof(path)))
  return(0);
 return(strcmp(path, g_lsp_file) == 0);
}

/* The concatenated end-of-line hint text for buffer line y, or NULL if none.
   Returns a pointer to a reused static buffer (renderer calls this synchronously
   per visible line). */
const char *e_lsp_inlay_eol_text(int y)
{
 static char buf[256];
 size_t len = 0;
 int i;

 if (!g_inlay_on)
  return(NULL);
 buf[0] = '\0';
 for (i = 0; i < g_inlay_nactive; i++)
 {
  size_t l;
  if (g_inlay_active[i].line != y || !g_inlay_active[i].label)
   continue;
  l = strlen(g_inlay_active[i].label);
  if (len + l + 1 >= sizeof(buf))
   break;
  memcpy(buf + len, g_inlay_active[i].label, l);
  len += l;
  buf[len] = '\0';
 }
 return(len ? buf : NULL);
}

/* The cell attribute for an inlay hint: a distinct "chip" -- black on light grey
   -- so the server annotation reads as a badge and never blends into the code or
   a trailing comment (the old dim-cyan-fg-on-line-bg was invisible against green
   comments and the Borland blue).  Palette: 7 = Light Gray, 0 = Black; this also
   stays clear of the red/brown the diagnostics chips use.  Under mono ncurses
   there is no colour to spare, so fall back to the line's own attribute. */
int e_lsp_inlay_color(SCHIRM *s, int base)
{
 (void)s;
 if (WpeIsXwin() || col_num > 0)
  return(16 * 7 + 0);             /* black text on a light-grey pill */
 return(base);
}

/* The LSP languageId for this window's file, or NULL if none is wired. */
/* Case-sensitive "does NAME end in SUF?" helper, so the extension table below
   reads as a list of suffixes rather than repeated strcmp/length arithmetic. */
static int e_lsp_ends_with(const char *name, const char *suf)
{
 int n = strlen(name), s = strlen(suf);
 return (n > s && !strcmp(name + n - s, suf));
}

/* The LSP languageId for a file, by extension -- also the key into the server
   table (e_lsp_server_for).  Adding a language here + a row there is the whole
   job; the protocol engine (we_lsp.c) is shared.  Returns NULL for files no
   server backs. */
static const char *e_lsp_lang_for(FENSTER *f)
{
 const char *nm;
 if (!f || !f->datnam)
  return(NULL);
 nm = f->datnam;
 if (e_lsp_ends_with(nm, ".scala"))
  return("scala");
 if (e_lsp_ends_with(nm, ".sc"))           /* scala-cli scripts + worksheets */
  return("scala");
 if (e_lsp_ends_with(nm, ".cpp") || e_lsp_ends_with(nm, ".cxx") ||
     e_lsp_ends_with(nm, ".cc")  || e_lsp_ends_with(nm, ".hpp") ||
     e_lsp_ends_with(nm, ".hxx") || e_lsp_ends_with(nm, ".hh")  ||
     e_lsp_ends_with(nm, ".C")   || e_lsp_ends_with(nm, ".ii"))
  return("cpp");                           /* clangd */
 if (e_lsp_ends_with(nm, ".c") || e_lsp_ends_with(nm, ".h"))
  return("c");                             /* clangd (a .h is treated as C) */
 if (e_lsp_ends_with(nm, ".py") || e_lsp_ends_with(nm, ".pyi"))
  return("python");                        /* pyright / pylsp */
 if (e_lsp_ends_with(nm, ".go"))
  return("go");                            /* gopls */
 if (e_lsp_ends_with(nm, ".rs"))
  return("rust");                          /* rust-analyzer */
 return(NULL);
}

/* Language-server descriptor: the command to spawn for a language, a short label
   for the status bar, and the install hint shown when the binary is not on PATH.
   A language may have MORE THAN ONE row, in preference order: e_lsp_server_for()
   returns the first whose binary is actually installed (so Python prefers pyright
   but falls back to pylsp).  This is the "descriptor entry, not new plumbing" the
   manual promises -- a new server is a row, not new code. */
typedef struct {
 const char *lang;       /* matches e_lsp_lang_for() */
 char *const *argv;      /* NULL-terminated spawn command; argv[0] is the PATH check */
 const char *label;      /* short name for the status bar ("Metals", "clangd", ...) */
 const char *missing;    /* shown when the binary is not installed */
 int shares_daemon;      /* E_LSP_SHARES_DAEMON: it may start the build server
                            other editors share (e_lsp_host.server_shares_daemon) */
} e_lsp_server;

/* Metals may start Bloop, which other editors and the Scala debugger use:
   ending Metals must leave it running. */
#define E_LSP_SHARES_DAEMON 1

static char *const E_LSP_ARGV_METALS[]  = { "metals", NULL };
static char *const E_LSP_ARGV_CLANGD[]  = { "clangd", NULL };
static char *const E_LSP_ARGV_PYRIGHT[] = { "pyright-langserver", "--stdio", NULL };
static char *const E_LSP_ARGV_PYLSP[]   = { "pylsp", NULL };
static char *const E_LSP_ARGV_GOPLS[]   = { "gopls", NULL };
static char *const E_LSP_ARGV_RUSTA[]   = { "rust-analyzer", NULL };

#define E_LSP_PY_MISSING \
 "No Python language server in PATH (pip install pyright, or apt install python3-pylsp)."

static const e_lsp_server e_lsp_servers[] = {
 { "scala",  E_LSP_ARGV_METALS,  "Metals", "Metals not in PATH (cs install metals).",
   E_LSP_SHARES_DAEMON },
 { "c",      E_LSP_ARGV_CLANGD,  "clangd", "clangd not in PATH (apt install clangd)." },
 { "cpp",    E_LSP_ARGV_CLANGD,  "clangd", "clangd not in PATH (apt install clangd)." },
 { "python", E_LSP_ARGV_PYRIGHT, "pyright", E_LSP_PY_MISSING },   /* preferred */
 { "python", E_LSP_ARGV_PYLSP,   "pylsp",   E_LSP_PY_MISSING },   /* fallback  */
 { "go",     E_LSP_ARGV_GOPLS,   "gopls",
   "gopls not in PATH (apt install gopls, or go install golang.org/x/tools/gopls@latest)." },
 { "rust",   E_LSP_ARGV_RUSTA,   "rust",   /* bar label kept short: "rust-analyzer"
                                              would not fit the 80-col button row */
   "rust-analyzer not in PATH (apt install rust-analyzer, or rustup component add rust-analyzer)." },
};

/* e_lsp_server_for - the server descriptor for a language.  When a language lists
   several candidates (preference order), returns the first whose binary is on
   PATH; if none is installed, returns the first candidate, so the status bar and
   the "not in PATH" hint still name the preferred one.  NULL if the language has
   no server at all. */
static const e_lsp_server *e_lsp_server_for(const char *lang)
{
 const e_lsp_server *first = NULL;
 unsigned i;
 if (!lang)
  return(NULL);
 for (i = 0; i < sizeof(e_lsp_servers) / sizeof(e_lsp_servers[0]); i++)
 {
  if (strcmp(e_lsp_servers[i].lang, lang))
   continue;
  if (!first)
   first = &e_lsp_servers[i];
  if (e_test_command((char *)e_lsp_servers[i].argv[0]) == 0)
   return(&e_lsp_servers[i]);          /* installed -> use this one */
 }
 return(first);                         /* none installed -> the preferred candidate */
}

/* A Metals worksheet (*.sc / *.worksheet.sc): Metals evaluates each top-level
   expression and returns the result as an inlay hint at the end of the line.  We
   detect these to turn the inlay overlay on automatically (no Alt-Q Y), so the
   results just appear -- a lightweight REPL. */
static int e_lsp_is_worksheet(FENSTER *f)
{
 int n;
 if (!f || !f->datnam)
  return(0);
 n = strlen(f->datnam);
 return(n > 3 && !strcmp(f->datnam + n - 3, ".sc"));
}

/* on_diagnostic: record the problem's range for inline marking, and (unless a
   live poll asked to stay quiet) surface it in the Messages window in the same
   file:line:col: form as F9 build errors. */
static void e_lsp_on_diag(const char *path, int line, int ch,
                          int end_line, int end_ch, int sev,
                          const char *msg, void *ud)
{
 char buf[700];
 const char *kind = sev == 1 ? "error" : sev == 2 ? "warning"
                  : sev == 3 ? "info"  : "hint";
 (void)path; (void)ud;

 /* Accumulate the mark for this batch.  Only single-line ranges are recolored
    (the common case for a token); for a multi-line range, mark its first line
    from the start column to end-of-line so the problem is still visible. */
 if (g_diag_npending < LSP_MAX_DIAG)
 {
  int c1 = (end_line == line) ? end_ch : 100000;  /* same line: exact end */
  if (c1 <= ch)
   c1 = ch + 1;                                   /* zero-width: mark one cell */
  g_diag_pending[g_diag_npending].line = line;
  g_diag_pending[g_diag_npending].c0 = ch;
  g_diag_pending[g_diag_npending].c1 = c1;
  g_diag_pending[g_diag_npending].sev = sev;
  snprintf(g_diag_pending[g_diag_npending].msg,
           sizeof(g_diag_pending[g_diag_npending].msg),
           "%s", msg ? msg : "");                  /* kept for Alt-Q . / , jumps */
  g_diag_npending++;
 }

 if (g_lsp_quiet)                   /* live poll: only the summary line shows */
  return;
 snprintf(buf, sizeof(buf), "%s:%d:%d: %s: %s",
          e_d_dap_basename(g_lsp_file), line + 1, ch + 1, kind, msg ? msg : "");
 if (g_lsp_fenster)
  e_d_p_message(buf, g_lsp_fenster, 1);
}

static void e_lsp_ui_trace(const char *fmt, ...);   /* XWPE_UI_TRACE diag (below) */

/* What the screen's owner calls the server's problems in its trace. */
#define E_LSP_DIAG_WHAT "the language server's problems"

/**
 * e_lsp_diag_show - Draw the server's problems: the marks on the source
 * window and the "LSP: N error(s)" line.
 * @data: unused.
 *
 * The marks are redrawn every time, since the positions may have moved when
 * the totals did not; the line is said only when the totals changed.  Run by
 * the screen's owner, at once when the editor is idle and otherwise when a
 * box, a menu or a run has given the screen back, so what the server found
 * while the user was in a dropdown shows when it closes.
 */
static void e_lsp_diag_show(void *data)
{
 char buf[120];
 (void)data;

 if (!g_lsp_fenster)
  return;
 if (DTMD_ISTEXT(g_lsp_fenster->dtmd))
 {
  e_schirm(g_lsp_fenster, 1);       /* redraw text so marks appear/clear */
  e_cursor(g_lsp_fenster, 0);       /* keep the caret where it was */
 }
 if (g_lsp_last_err == g_lsp_said_err && g_lsp_last_warn == g_lsp_said_warn)
  return;                           /* totals unchanged -- skip the status line */
 g_lsp_said_err = g_lsp_last_err;
 g_lsp_said_warn = g_lsp_last_warn;
 if (g_lsp_said_err == 0 && g_lsp_said_warn == 0)
  snprintf(buf, sizeof(buf), "LSP: no problems.");
 else
  snprintf(buf, sizeof(buf), "LSP: %d error(s), %d warning(s).",
           g_lsp_said_err, g_lsp_said_warn);
 e_d_p_message(buf, g_lsp_fenster, 0);   /* sw=0: do NOT steal focus from the
                                            editor -- the user keeps typing */
}

/* End of a publishDiagnostics batch: swap the freshly-collected marks in
   (atomic replace -- even when the totals are unchanged the positions may have
   moved), then draw them and the non-spammy "N errors, M warnings" line, now
   or when the screen is the editor's again. */
static void e_lsp_on_diag_summary(const char *path, int errors, int warnings,
                                  void *ud)
{
 (void)path; (void)ud;

 memcpy(g_diag_active, g_diag_pending,
        (size_t)g_diag_npending * sizeof(g_diag_active[0]));
 g_diag_nactive = g_diag_npending;
 g_diag_npending = 0;
 g_lsp_last_err = errors;
 g_lsp_last_warn = warnings;
 wpe_paint_request(e_lsp_diag_show, NULL, E_LSP_DIAG_WHAT);
}

/* Strip Metals' leading status icon/spinner (a UTF-8 glyph and spaces) so the
   text is stable across spinner frames -- "Indexing", not "(spinner) Indexing"
   with a different frame each tick. */
static const char *e_lsp_status_clean(const char *t)
{
 if (!t)
  return("");
 while (*t && ((unsigned char)*t >= 0x80 || *t == ' '))
  t++;
 return(t);
}

/* The server's transient status (metals/status): track a busy flag and echo the
   text as a one-line progress note in Messages whenever it CHANGES (deduped, so
   a spinner does not spam).  Answers the user's "tell me what it is doing" --
   the cold ~1-min index now shows "LSP: Indexing", not silence -- and lets a
   later empty hover say "still indexing" instead of "no information".  sw=0:
   never steal focus from the file the user is editing. */
static void e_lsp_on_status(const char *text, int hide, void *ud)
{
 const char *clean;
 char line[200];
 (void)ud;

 if (hide)                                /* finished: clear the busy state */
 {  g_lsp_busy = 0;  return;  }
 clean = e_lsp_status_clean(text);
 g_lsp_busy = (clean && clean[0]) ? 1 : 0;
 if (!g_lsp_busy || !g_lsp_fenster || e_lsp_screen_busy())
  return;                                 /* modal up: track busy, defer the note */
 if (!e_lsp_doc_is_new(&g_lsp_status_last, clean))
  return;                                 /* same as the last note: do not repeat */
 snprintf(line, sizeof(line), "LSP: %s", clean);
 e_d_p_message(line, g_lsp_fenster, 0);
}

/* A document the server pushed for display (the Metals Doctor, already stripped
   to plain text): show it in its OWN "Metals Doctor" window -- NOT in Messages,
   which it would swamp -- and leave the editor file focused (the report opens in
   the background; switch to it with the Window menu / Alt-N).  A one-line note in
   Messages says it updated. */
static void e_lsp_on_show_text(const char *title, const char *body, void *ud)
{
 extern int e_d_p_named(char *winname, char *str, FENSTER *f, int sw);
 extern void e_switch_window(int num, FENSTER *f);
 char line[512];
 const char *p = body;
 FENSTER *home = g_lsp_fenster;
 ECNT *cn;
 int home_id = -1, i;
 (void)ud;

 if (!home || !body)
  return;
 if (e_lsp_screen_busy())
  return;                                /* modal up: do NOT consume the dedup --
                                            Metals re-pushes, so the next push
                                            renders it once the editor is back */
 /* Metals re-pushes the same Doctor on every build event: render only when it
    actually changed.  g_lsp_doctor_last is reset per session (e_lsp_ensure), so
    a re-started server shows its Doctor again rather than being deduped away. */
 if (!e_lsp_doc_is_new(&g_lsp_doctor_last, body))
  return;
 cn = home->ed;
 for (i = 1; i <= cn->mxedt; i++)        /* remember the file's window, to refocus */
  if (cn->f[i] == home) { home_id = cn->edt[i]; break; }

 snprintf(line, sizeof(line), "=== %s ===", title ? title : "Document");
 e_d_p_named("Metals Doctor", line, home, 0);
 while (p && *p)
 {
  const char *nl = strchr(p, '\n');
  size_t len = nl ? (size_t)(nl - p) : strlen(p);
  if (len >= sizeof(line))
   len = sizeof(line) - 1;
  memcpy(line, p, len);
  line[len] = '\0';
  if (line[0])
   e_d_p_named("Metals Doctor", line, home, 0);
  if (!nl)
   break;
  p = nl + 1;
 }
 /* Metals re-pushes a slightly different Doctor on many events (it passes the
    content dedup above), which spams Messages.  Announce it ONCE per session;
    the Doctor window itself keeps updating silently.  Reset in e_lsp_ensure. */
 if (!g_lsp_doctor_announced)
 {
  g_lsp_doctor_announced = 1;
  e_d_p_message("Metals Doctor updated (open its window to read it).", home, 0);
 }
 if (home_id >= 0)                        /* keep the editor file focused */
  e_switch_window(home_id, home);
}

/* Serialize the editor buffer to a malloc'd NUL-terminated string (one '\n' per
   line), so the language server sees the CURRENT text -- including unsaved edits
   -- not the on-disk file.  Each line in b->bf[y].s ends at its in-buffer WPE_WR
   (0x0A) terminator; gather the line pointers and delegate to the pure,
   unit-tested e_lsp_join_lines (we_lsp.c), which copies up to that marker and
   adds exactly one '\n'.  Doubling the delimiter would shift every position and
   silently break cross-file hover / go-to-definition (the 1.6.x regression).
   Caller frees.  (NB: the local is "lns", not "lines" -- <term.h> #defines the
   latter as a terminfo capability.) */
static char *e_lsp_buffer_text(FENSTER *f)
{
 BUFFER *b = f->b;
 char **lns;
 char *body;
 int y, n = b->mxlines;

 if (n < 0)
  n = 0;
 lns = malloc((n ? n : 1) * sizeof(char *));
 if (!lns)
  return(NULL);
 for (y = 0; y < n; y++)
  lns[y] = b->bf[y].s;            /* NULL is fine -- join treats it as empty */
 body = e_lsp_join_lines(lns, n);
 free(lns);
 return(body);
}

/* The text last handed to the server (via didOpen or didChange).  Used so a sync
   with no edits is a no-op: a needless didChange bumps the document version, and
   index-based requests (definition / implementation / typeDefinition / outline)
   then return nothing until the server recompiles the new version -- that was
   the "No definition found" bug on a freshly opened, unmodified file. */
static char *g_lsp_synced_text = NULL;

static void e_lsp_synced_set(char *owned)   /* takes ownership of `owned` */
{
 free(g_lsp_synced_text);
 g_lsp_synced_text = owned;
 g_lsp_doc_gen++;                  /* an overlay fetched before is stale */
}

/* Push the current buffer to the server (textDocument/didChange) ONLY if it
   differs from what the server already has, so unmodified navigation keeps the
   compiled index valid. */
static void e_lsp_sync(FENSTER *f)
{
 char *text;

 if (!g_lsp)
  return;
 text = e_lsp_buffer_text(f);
 if (!text)
  return;
 if (g_lsp_synced_text && !strcmp(text, g_lsp_synced_text))
 {  free(text);  return;  }            /* unchanged: do not bump the version */
 e_lsp_did_change(g_lsp, g_lsp_file, text);
 e_lsp_synced_set(text);               /* keep ownership as the new baseline */
}

/* Make the source window the active one again.  Writing diagnostics to the
   Messages window creates/raises it, which would otherwise leave Messages
   focused -- but LSP is an IDE output pane, it must NOT move the user out of
   their code (focus-stealing belongs only to interactive Run, where the program
   reads stdin in the console).  Keeping the source active also means the Alt-Q
   navigation actions read the cursor from the code, not from Messages. */
static void e_lsp_raise_source(FENSTER *src)
{
 ECNT *cn;
 int i;

 if (!src)
  return;
 cn = src->ed;
 for (i = cn->mxedt; i > 0; i--)
  if (cn->f[i] == src)
  {
   if (i != cn->mxedt)
    e_switch_window(cn->edt[i], cn->f[cn->mxedt]);
   break;
  }
}

/* The longest `java -version` waits for: a JVM that cannot start in this
   long is not one the editor can use, and the user is not kept waiting. */
#define E_JDK_PROBE_MS    5000L
#define E_JDK_PROBE_MAX   1024     /* the banner `java -version` prints */
#define E_JDK_VERSION_KEY "version \""
#define E_JDK_PROBE_WHAT  "java -version"

/**
 * e_jdk_major_from_banner - The feature version a `java -version` banner
 * names (21, 26, or 8 for "1.8.0"), or -1 when it names none.
 * @banner: what `java -version` printed.
 */
static int e_jdk_major_from_banner(const char *banner)
{
 const char *v = strstr(banner, E_JDK_VERSION_KEY);

 if (!v)
  return(-1);
 v += strlen(E_JDK_VERSION_KEY);
 if (strncmp(v, "1.", 2) == 0)
  return(atoi(v + 2));                      /* 1.8.0 -> 8                 */
 return(atoi(v));                           /* 21.0.1 -> 21, 26.0.1 -> 26 */
}

/**
 * e_jdk_major - Major feature version of the JDK at @java_bin, or -1 when it
 * cannot be told.
 * @java_bin: a path, or "java" for the one on PATH.
 *
 * Starting a debug session for a Java or Scala program needs to know whether
 * the JDK is too new for the compiler.  `java -version` runs as a command of
 * its own on the editor's loop, the path handed to it as an argument, and a
 * JVM that hangs costs the user the probe's deadline, not the editor.
 */
static int e_jdk_major(const char *java_bin)
{
 char banner[E_JDK_PROBE_MAX];
 char *argv[3];
 int status, ran;

 argv[0] = (char *)java_bin;
 argv[1] = "-version";
 argv[2] = NULL;
 ran = wpe_child_probe(argv, banner, sizeof banner, E_JDK_PROBE_MS, &status,
                       E_JDK_PROBE_WHAT);
 if (ran != WPE_CAPTURE_ENDED)
  return(-1);
 return(e_jdk_major_from_banner(banner));
}

/* Find an LTS JDK the Scala 3 compiler is happy on (17..23, newest preferred) in
   the usual Linux and macOS locations.  Writes its home to `out`; returns 1 if
   found.  Used to pin Metals' JVM away from a too-new default. */
static int e_find_supported_jdk(char *out, size_t sz)
{
 static const char *pat[] = {
  "/usr/lib/jvm/*21*", "/usr/lib/jvm/*17*",
  "/Library/Java/JavaVirtualMachines/*21*/Contents/Home",
  "/Library/Java/JavaVirtualMachines/*17*/Contents/Home",
  NULL
 };
 int p, i;
 char jbin[1100];

 for (p = 0; pat[p]; p++)
 {
  glob_t g;
  if (glob(pat[p], 0, NULL, &g) != 0)
   continue;
  for (i = 0; i < (int)g.gl_pathc; i++)
  {
   int m;
   snprintf(jbin, sizeof(jbin), "%s/bin/java", g.gl_pathv[i]);
   if (access(jbin, X_OK) != 0)
    continue;
   m = e_jdk_major(jbin);
   if (m >= 17 && m <= 23)
   {  snprintf(out, sz, "%s", g.gl_pathv[i]);  globfree(&g);  return(1);  }
  }
  globfree(&g);
 }
 return(0);
}

/* Pin the JVM a Scala tool runs on away from a too-new default JDK.  The Scala 3
   compiler crashes at start-up on JDK >= 24 ("asTerm called on not-a-Term"),
   which silently breaks whatever drives it: for @who "Metals" the presentation
   compiler (hover/completion go empty -- "No hover information" -- while
   definition/diagnostics still work, a confusing split); for @who "Scala debug"
   the Bloop build server, which scala-cli launches from JAVA_HOME/PATH to
   compile the debuggee.  Both inherit this process's environment, so -- unless
   the user already pinned a supported JAVA_HOME -- repoint JAVA_HOME (and PATH)
   at an LTS JDK and say what was done.  setenv is idempotent across callers (the
   second sees a usable JAVA_HOME and returns).  No-op when the default JDK is
   already fine or no LTS JDK is installed. */
static void e_scala_pin_jdk(FENSTER *f, const char *who)
{
 const char *jh = getenv("JAVA_HOME");
 char jbin[1100], jdk[1024], msg[1200], newpath[4096];
 const char *oldpath;
 int major;

 if (jh && *jh)
 {
  snprintf(jbin, sizeof(jbin), "%s/bin/java", jh);
  if (e_jdk_major(jbin) <= 23)
   return;                       /* user pinned a usable JAVA_HOME: respect it */
 }
 major = e_jdk_major("java");    /* the default on PATH */
 if (major < 24)
  return;                        /* default is fine, or unknown -- do not meddle */
 if (!e_find_supported_jdk(jdk, sizeof(jdk)))
 {
  snprintf(msg, sizeof(msg), "%s: default JDK %d is too new for the Scala "
           "compiler and no LTS JDK (17/21) was found -- set JAVA_HOME to one.",
           who, major);
  e_d_p_message(msg, f, 1);
  return;
 }
 setenv("JAVA_HOME", jdk, 1);
 oldpath = getenv("PATH");
 snprintf(newpath, sizeof(newpath), "%s/bin:%s", jdk, oldpath ? oldpath : "");
 setenv("PATH", newpath, 1);
 snprintf(msg, sizeof(msg), "%s: pinned JAVA_HOME=%s (default JDK %d is too "
          "new for the Scala compiler).", who, jdk, major);
 e_d_p_message(msg, f, 1);
}

/* --- Asynchronous server start ----------------------------------------------
   The Metals cold start is slow (JVM boot + build import + first compile, ~1-3
   min) and used to be done synchronously, freezing the editor.  Now the
   session's stdout fd is registered with the editor's fd-loop (wpe_fd_poll,
   pumped from the input wait), so the initialize handshake and the first
   compile run in the BACKGROUND while the user keeps editing.  The fd is in the
   poll set ONLY during that active startup window -- Metals streams output the
   whole time, so the input loop's blocking poll keeps returning and stays
   responsive -- and is removed once the first diagnostics arrive (steady state:
   an idle server must never sit in the poll set and delay a keystroke).  After
   that, diagnostics flow via e_lsp_on_edit's poll, and the fd is watched again
   whenever a request (or an overlay fetch) is owed its answer. */
static int g_lsp_fd_num = -1;                 /* fd in the poll set, or -1     */
static int g_lsp_ready_done = 0;              /* announced "ready" this session? */
static int e_lsp_ensure(FENSTER *f);          /* fwd */
static void e_lsp_fd_unregister(void);        /* fwd */
static void e_lsp_focus_worksheet(FENSTER *f);/* fwd */
void e_lsp_ui_shutdown(void);                 /* fwd: full session teardown */
static void e_lsp_rq_start_over(void);        /* fwd: the answers on the loop */
static void e_lsp_ask_deliver(void);          /* fwd */

/* If the server has died (closed its stdout), tear the session down: stop
   polling its fd (otherwise the dead fd reports "readable" forever and spins the
   input loop at 100% CPU), close it, drop the overlays, and tell the user.  A
   later Alt-Q (or opening the file again) starts a fresh one. */
#define E_LSP_EXITED_LINE "Language server stopped (it exited). Alt-Q restarts it."
#define E_LSP_INPUT_LOST_LINE \
 "Language server stopped reading its input; it was ended. Alt-Q restarts it."

static void e_lsp_reap_if_dead(FENSTER *f)
{
 int input_lost;

 if (!g_lsp || !e_lsp_dead(g_lsp))
  return;
 input_lost = e_lsp_input_lost(g_lsp);
 e_lsp_ui_shutdown();                     /* unregister fd + close + clear state */
 g_lsp_file[0] = '\0';                    /* so the next Alt-Q starts a new session */
 if (!f)
  return;
 if (input_lost)
  e_d_p_message(E_LSP_INPUT_LOST_LINE, f, 0);
 else
  e_d_p_message(E_LSP_EXITED_LINE, f, 0);
}

/* What the screen's owner calls the server's later news in its trace. */
#define E_LSP_NEWS_WHAT "the language server's news"

/**
 * e_lsp_settle_ready - Move to the steady state once the first diagnostics
 * have landed, when the screen is the editor's.
 *
 * Says "Language server ready.", focuses a worksheet that waited for the
 * server, and stops polling a server nothing is owed by, so an idle server
 * never delays a key.  With a box up it does nothing: e_lsp_catch_up asks
 * again when the box has closed.
 */
static void e_lsp_settle_ready(void)
{
 if (!e_lsp_started(g_lsp) || g_lsp_last_err < 0 || g_lsp_ready_done ||
     e_lsp_screen_busy())
  return;
 /* first diagnostics summary arrived -> steady state. */
 g_lsp_ready_done = 1;
 if (g_lsp_fenster)
  e_d_p_message("Language server ready.", g_lsp_fenster, 0);
 if (g_lsp_pending_ws)
 {
  /* a worksheet was opened while the server booted for the project file:
     focus it now so it evaluates (keeps the fd registered for its results). */
  e_lsp_focus_worksheet(g_lsp_pending_ws);
  g_lsp_pending_ws = NULL;
 }
 else if (e_lsp_is_worksheet(g_lsp_fenster))
  g_inlay_stale = 1;            /* worksheet: pull the first evaluation results */
 else if (g_inlay_on || g_sem_on)
  ;                             /* an overlay is live: keep polling so a later server
                                   push (inlayHint/semanticTokens refresh, once indexed)
                                   reaches us and repaints -- it stays quiet when idle */
 else if (e_lsp_requests_owed(g_lsp))
  ;                             /* an answer is owed: it is read here */
 else
  e_lsp_fd_unregister();        /* stop polling so an idle server never delays a key */
}

/**
 * e_lsp_settle_overlays - Ask the server again for the inlay hints and the
 * semantic colours that went stale, when the screen is the editor's.
 */
static void e_lsp_settle_overlays(void)
{
 if (e_lsp_screen_busy())
  return;                                 /* modal up: keep the stale flag, ask later */
 if (g_inlay_on)
  e_lsp_inlay_refresh_pending(g_lsp_fenster);
 if (g_sem_on)
  e_lsp_sem_refresh_pending(g_lsp_fenster);
}

/**
 * e_lsp_catch_up - Do what the server's news asked for while a box owned
 * the screen.
 * @data: unused.
 *
 * The screen's owner runs this once the editor is idle: the readiness line,
 * the worksheet and the overlays that had to wait are handled then, with no
 * further word from the server needed.
 */
static void e_lsp_catch_up(void *data)
{
 (void)data;
 if (!g_lsp)
  return;
 e_lsp_settle_ready();
 e_lsp_settle_overlays();
}

/* fd-loop callback: drain the server, finish the handshake, hand it the
   document once ready, and leave the poll set once the first diagnostics land. */
static void e_lsp_on_fd_readable(int fd, void *data)
{
 int was_started, qsave, handled;
 (void)fd; (void)data;

 if (!g_lsp)
 {  e_lsp_fd_unregister();  return;  }
 /* A box, a menu or a modal window (file manager, project dialog) pumps this
    same fd poll through its own input loop.  Drain the socket as usual, but
    defer every visible paint until the screen is the editor's again, so a
    background Metals message / Doctor / inlay redraw never corrupts the box.
    What the news owes (the readiness line, a stale overlay) is asked again
    by e_lsp_catch_up once the screen is back. */
 was_started = e_lsp_started(g_lsp);
 qsave = g_lsp_quiet;
 g_lsp_quiet = 1;                          /* startup: show the summary, not each line */
 handled = e_lsp_poll(g_lsp);             /* handshake + diagnostics/status/doctor */
 g_lsp_quiet = qsave;
 if (e_lsp_dead(g_lsp))                    /* server exited (EOF): tear down, do not spin */
 {  e_lsp_reap_if_dead(g_lsp_fenster);  return;  }

 if (!was_started && e_lsp_started(g_lsp))
 {
  /* handshake just completed: open the CURRENT buffer (captures any edits the
     user made while the server was booting) so the server compiles it. */
  FENSTER *home = g_lsp_fenster;
  e_lsp_rq_start_over();                  /* the start's waiting line goes */
  if (home)
  {
   char *text = e_lsp_buffer_text(home);
   e_lsp_did_open(g_lsp, g_lsp_file, text ? text : "");
   e_lsp_did_focus(g_lsp, g_lsp_file);
   e_lsp_synced_set(text ? text : strdup(""));
  }
 }
 e_lsp_settle_ready();
 /* worksheet REPL: the fd stays registered, so as Metals compiles and EVALUATES
    (the results trickle in after the types -- a second, slower pass -- and Metals
    does not always send inlayHint/refresh), each diagnostics batch it publishes
    means the inlay set may have grown.  Re-pull on it once ready; the fetch is a
    cheap single request and the server falls silent when idle, so this is
    event-driven, not polling. */
 /* While an overlay is explicitly ON, a diagnostics batch (e_lsp_poll counts
    nothing else) means "the set may have grown": the diagnostics a server
    publishes once it finishes indexing are the reliable signal, since not every
    server bothers to send inlayHint/semanticTokens refresh for a plain file.
    Its log lines and status are not news: they also come with the overlay's
    own answer, and re-pulling on them would never stop. */
 if (g_inlay_on && handled > 0 && g_lsp_ready_done)
  g_inlay_stale = 1;
 if (g_sem_on && handled > 0 && g_lsp_ready_done)
  g_sem_stale = 1;
 e_lsp_overlay_deliver();                 /* an overlay answer read just now */
 e_lsp_settle_overlays();
 e_lsp_ask_deliver();                     /* an Alt-Q answer read just now */
 if (e_lsp_screen_busy())
  wpe_paint_request(e_lsp_catch_up, NULL, E_LSP_NEWS_WHAT);
}

/**
 * e_lsp_fd_register - Let the editor's loop read the language server.
 *
 * Its answers, diagnostics and overlay updates arrive while the user edits.
 * When the loop has no room for it, the bottom row says so and the server is
 * left unwatched; the next request asks again, so it comes back as soon as
 * a stream closes.
 * Return: 0 when the server is watched (or there is none), -1 when refused.
 */
static int e_lsp_fd_register(void)
{
 if (!g_lsp || g_lsp_fd_num >= 0 || e_lsp_fd(g_lsp) < 0)
  return(0);
 if (e_fd_watch(e_lsp_fd(g_lsp), POLLIN, e_lsp_on_fd_readable, NULL,
                "the language server") < 0)
  return(-1);
 g_lsp_fd_num = e_lsp_fd(g_lsp);
 return(0);
}

/**
 * e_lsp_give_up_unwatched - Stop a language server the loop cannot read.
 *
 * Its handshake would never be read and the bar would say "starting" for
 * ever; the bottom row already says why, and the next Alt-Q starts it again.
 * Return: -1, the start's "not ready" answer.
 */
static int e_lsp_give_up_unwatched(void)
{
 e_lsp_ui_shutdown();
 g_lsp_file[0] = '\0';
 return(-1);
}

static void e_lsp_fd_unregister(void)
{
 if (g_lsp_fd_num >= 0)
 {  wpe_fd_del(g_lsp_fd_num);  g_lsp_fd_num = -1;  }
}

/* (An eager "start on first interaction" autostart was prototyped here but is
   deferred: the keystroke hook also fires on bottom-bar/menu mouse clicks, so it
   would boot the server just from opening the Metals menu.  Eager start belongs
   on a real file-open hook -- a follow-up.  For now the start is lazy: the first
   Alt-Q action kicks off the async start below, and the editor stays responsive
   while it runs.) */

/* Ensure the language server is started for f's file.  ASYNCHRONOUS: the first
   call spawns the server, sends initialize and RETURNS -- the slow cold start
   runs in the background (see above), so the editor never freezes.  Returns 0
   only when the session is READY (the action may proceed); -1 while it is still
   starting (the action politely waits) or on error.  For Scala, `scala-cli
   setup-ide` is run first so Metals auto-connects to the build server. */
/* A read-only dependency / stdlib source Metals extracted under
   .metals/readonly/ -- where go-to-definition into a library lands.  We open it
   (read-only, via its own 0444 permissions) but must NOT start or switch the
   language server for it: its directory is the build cache, not a project root,
   so a server rooted there is useless AND would evict the project's session.
   Language-server actions belong in the project window. */
static int e_lsp_is_dep_source(FENSTER *f)
{
 return (f && f->dirct && strstr(f->dirct, "/.metals/readonly/") != NULL);
}

#define E_LSP_PATH_MAX 1200

/* The file whose `scala-cli setup-ide` has ended, so its next start goes
   straight to the server; and the file the running setup is for. */
static char g_lsp_setup_done[E_LSP_PATH_MAX];
static char g_lsp_setup_for[E_LSP_PATH_MAX];

static int e_lsp_ensure(FENSTER *f);

/**
 * e_lsp_window_of - The window showing @path, or NULL once it was closed.
 * @path: directory and name, as e_lsp_ensure builds it.
 */
static FENSTER *e_lsp_window_of(const char *path)
{
 ECNT *cn = WpeEditor;
 char p[E_LSP_PATH_MAX];
 int i;

 for (i = 1; cn && i <= cn->mxedt; i++)
 {
  FENSTER *w = cn->f[i];

  if (!w || !w->datnam)
   continue;
  snprintf(p, sizeof p, "%s%s", w->dirct ? w->dirct : "./", w->datnam);
  if (!strcmp(p, path))
   return(w);
 }
 return(NULL);
}

/* ---- the language server's answers on the loop ----
 *
 * An Alt-Q request goes out at once and the editor goes on taking keys: the
 * answer is read by the loop (e_lsp_on_fd_readable, or the key hook's poll)
 * and handed to a continuation that only keeps it.  It is shown when the
 * editor is idle, on the window it was asked from, and only while that
 * window's text (and, for the popups at the cursor, the cursor) is as it was
 * sent: the server's positions are for that text.  After a quiet second the
 * status row says what the editor waits for; Esc gives the answers up, and a
 * late answer is dropped by its id.  One request of each kind at a time:
 * the same action again meanwhile is refused with a line that names the
 * request out, while an action of another kind goes out at once.  Each
 * answer is shown as it comes; answers that came together are shown in the
 * order they were asked.
 */

#define E_LSP_ASK_NONE       0
#define E_LSP_ASK_HOVER      1
#define E_LSP_ASK_JUMP       2
#define E_LSP_ASK_COMPLETE   3
#define E_LSP_ASK_FORMAT     4
#define E_LSP_ASK_SIGNATURE  5
#define E_LSP_ASK_REFERENCES 6
#define E_LSP_ASK_CALLS      7
#define E_LSP_ASK_TYPES      8
#define E_LSP_ASK_SELECTION  9
#define E_LSP_ASK_HIGHLIGHT  10
#define E_LSP_ASK_LENSES     11
#define E_LSP_ASK_OUTLINE    12
#define E_LSP_ASK_SYMBOLS    13
#define E_LSP_ASK_ACTIONS    14
#define E_LSP_ASK_ACTION_RUN 15
#define E_LSP_ASK_RENAME     16
#define E_LSP_ASK_KINDS      17
#define E_LSP_ASK_ITEMS_MAX 64     /* candidates the completion list shows */
#define E_LSP_ROWS_MAX     128     /* entries a list or a picker keeps      */
#define E_LSP_HL_MAX       256     /* occurrences a highlight keeps         */
#define E_LSP_RANGES_MAX    64     /* nested ranges an expand-selection reads */
#define E_LSP_NAME_MAX     32
#define E_LSP_WHAT_MAX     32
#define E_LSP_START_METHOD "initialize"
#define E_LSP_WAIT_FMT \
 " waiting for %s to answer `%s` ... %lds  (Esc cancels)"
#define E_LSP_START_WAIT_FMT \
 " waiting for %s to start ... %lds  (Esc cancels)"
#define E_LSP_BUSY_FMT \
 "%s is still answering `%s` (Esc cancels it)"
#define E_LSP_CANCELLED_FMT \
 "Gave up waiting for %s to answer `%s`."
#define E_LSP_STALE_LINE \
 "The language server's answer came after the file changed; ask again."
#define E_LSP_START_CANCELLED_LINE "Language server start cancelled."
#define E_LSP_ASK_FAILED_LINE "Could not ask the language server."

/* One entry of a list answer, copied out of the engine's arrays: a
   location, a symbol, a lens, an action or a range. */
typedef struct
{
 char *name;                     /* symbol, lens title, action title, or NULL */
 char *path;                     /* its file, or NULL for the file asked about */
 int   line, character;          /* 0-based                                */
 int   has_edit;                 /* an action: its edit is in hand         */
 e_lsp_range range;              /* a selection range                      */
} e_lsp_ask_row;

/* A request out, of one kind, and its answer once it came. */
typedef struct
{
 int   kind;                     /* E_LSP_ASK_*, NONE when the slot is free */
 unsigned long seq;              /* the order asked: answers show in it    */
 char  path[E_LSP_PATH_MAX];     /* the file asked about                   */
 char *base;                     /* its text when asked                    */
 int   y, x;                     /* the cursor when asked                  */
 char  what[E_LSP_WHAT_MAX];     /* a jump: "definition", ...              */
 int   has_sel;                  /* a format: of the marked block          */
 int   flag;                     /* calls: outgoing; types: subtypes       */
 int   wlen;                     /* highlight: the identifier's length     */
 int   ready;                    /* the continuation has been told         */
 E_D_REQ_END how;                /* how the request ended                  */
 char *text;                     /* hover, signature, new text, or NULL    */
 int   others;                   /* rename, action: other files it changes */
 int   found;                    /* a jump: a location came                */
 char  target[E_LSP_PATH_MAX];   /* ... its file                           */
 int   line;                     /* ... its line, 0-based                  */
 int   n;                        /* candidates or rows, -1 none            */
 e_lsp_completion_item items[E_LSP_ASK_ITEMS_MAX];
 e_lsp_ask_row *rows;            /* a list answer's entries (malloc'd)     */
} e_lsp_ask_t;

/* One slot per kind of request. */
static e_lsp_ask_t e_lsp_asks[E_LSP_ASK_KINDS];
static unsigned long e_lsp_ask_seq = 0;

/* The requests' world: the editor's clock, timers and status row. */
static struct
{
 E_D_REQ_IO io;
 int  timer[E_D_TIMERS];         /* the loop's timer ids, E_D_GA_NO_TIMER  */
 int  waiting_painted;           /* the waiting line is on the status row  */
 int  start_asked;               /* the user waits for the start           */
 char name[E_LSP_NAME_MAX];      /* the server, in the lines               */
 e_d_wait_line kept;             /* a waiting line asked for under a box   */
} e_lsp_rq;

/**
 * e_lsp_screen_ours - Is the screen the editor's to draw on?  Not while a
 * command run from the editor has the terminal (a script, a shell): a
 * repaint then would take the terminal back from it, and its Ctrl-C would
 * no longer reach it.
 */
static int e_lsp_screen_ours(void)
{
 return(wpe_fd_waiting_for() == NULL);
}

/**
 * e_lsp_rq_window - The window the status row is drawn for: the one on top.
 */
static FENSTER *e_lsp_rq_window(void)
{
 if (!WpeEditor || WpeEditor->mxedt < 0)
  return(NULL);
 return(WpeEditor->f[WpeEditor->mxedt]);
}

/**
 * e_lsp_rq_waiting - Show the waiting line: what the server is asked, or
 * that it is starting, and that Esc gives it up.
 * @cmd: the request's method; @secs: since when; @io: unused.
 *
 * A start the user did not ask for (the eager start on opening a file)
 * says nothing: the user waits for nothing.
 */
static void e_lsp_rq_waiting_show_kept(void *data);   /* (below) */

static void e_lsp_rq_waiting(const char *cmd, long secs, void *io)
{
 char line[E_D_GA_LINE_MAX];
 int starting = !strcmp(cmd, E_LSP_START_METHOD);

 (void)io;
 if (starting && !e_lsp_rq.start_asked)
  return;
 if (e_d_wait_line_keep(&e_lsp_rq.kept, cmd, secs, e_lsp_rq_waiting_show_kept))
  return;                        /* a box or a command has the screen */
 if (starting)
  snprintf(line, sizeof line, E_LSP_START_WAIT_FMT, e_lsp_rq.name, secs);
 else
  snprintf(line, sizeof line, E_LSP_WAIT_FMT, e_lsp_rq.name, cmd, secs);
 e_status_say(e_lsp_rq_window(), line);
 e_lsp_rq.waiting_painted = 1;
}

/**
 * e_lsp_rq_waiting_show_kept - Say the language server's waiting line kept
 * while the screen was not the editor's, with the seconds of now.
 * @data: the kept line.
 */
static void e_lsp_rq_waiting_show_kept(void *data)
{
 e_d_wait_line *w = data;

 if (e_d_wait_line_take(w))
  e_lsp_rq_waiting(w->cmd, e_d_wait_line_secs_now(w), NULL);
}

/**
 * e_lsp_rq_waiting_clear - The wait is over: put the key hints back, unless
 * a line kept until the next key (a deadline's notice) holds the row.
 */
static void e_lsp_rq_waiting_clear(void)
{
 FENSTER *f;

 e_d_wait_line_forget(&e_lsp_rq.kept);
 if (!e_lsp_rq.waiting_painted)
  return;
 e_lsp_rq.waiting_painted = 0;
 f = e_lsp_rq_window();
 if (!f || !f->fb || !e_lsp_screen_ours())
  return;
 e_pr_uul(f->fb);
 e_refresh();
}

/**
 * e_lsp_rq_notice - Say why an answer was given up on, until the next key.
 * @msg: the sentence; @io: unused.
 */
static void e_lsp_rq_notice(const char *msg, void *io)
{
 char line[E_D_GA_LINE_MAX + E_D_REQ_MSG_MAX];

 (void)io;
 e_lsp_rq.waiting_painted = 0;
 e_d_wait_line_forget(&e_lsp_rq.kept);
 snprintf(line, sizeof line, " %s", msg);
 if (e_status_keep_until_free(line))
  return;                       /* drawn when the screen is the editor's */
 e_status_say_until_key(e_lsp_rq_window(), line);
}

/**
 * e_lsp_rq_start_over - The server answered `initialize`: the start's
 * waiting line leaves the status row, and Esc no longer ends the server.
 */
static void e_lsp_rq_start_over(void)
{
 e_lsp_rq.start_asked = 0;
 e_lsp_rq_waiting_clear();
}

/**
 * e_lsp_rq_on_timer - A timer the requests asked for has gone off.
 * @data: which one (E_D_TIMER_*).
 */
static void e_lsp_rq_on_timer(void *data)
{
 int which = (int)(intptr_t)data;

 e_lsp_rq.timer[which] = E_D_GA_NO_TIMER;
 if (!g_lsp)
  return;
 e_lsp_request_timer(g_lsp, which);
 e_lsp_ask_deliver();
}

/** e_lsp_rq_disarm - Stop timer @which, if it runs. */
static void e_lsp_rq_disarm(int which)
{
 if (e_lsp_rq.timer[which] == E_D_GA_NO_TIMER)
  return;
 wpe_timer_del(e_lsp_rq.timer[which]);
 e_lsp_rq.timer[which] = E_D_GA_NO_TIMER;
}

/**
 * e_lsp_rq_arm_timer - Ask the loop to call the requests back.
 * @which: E_D_TIMER_*; @ms: the delay, or E_D_TIMER_OFF; @io: unused.
 */
static void e_lsp_rq_arm_timer(int which, long ms, void *io)
{
 (void)io;
 e_lsp_rq_disarm(which);
 if (ms == E_D_TIMER_OFF)
  return;
 e_lsp_rq.timer[which] = wpe_timer_add(ms, 0, e_lsp_rq_on_timer,
                                       (void *)(intptr_t)which);
 if (e_lsp_rq.timer[which] <= 0)
  e_lsp_rq.timer[which] = E_D_GA_NO_TIMER;
}

/**
 * e_lsp_rq_begin - A session starts: give its requests the editor's clock,
 * timers and status row.
 * @label: the server's name in the lines ("clangd", "Metals").
 *
 * The deadline is E_D_REQ_TIMEOUT_S, or the user's
 * XWPE_DEBUG_ANSWER_TIMEOUT, as for a debugger's answers.
 */
static void e_lsp_rq_begin(const char *label)
{
 int i;

 memset(&e_lsp_rq.io, 0, sizeof e_lsp_rq.io);
 snprintf(e_lsp_rq.name, sizeof e_lsp_rq.name, "%s", label);
 e_lsp_rq.io.now_ms = e_d_ga_now_ms;
 e_lsp_rq.io.arm_timer = e_lsp_rq_arm_timer;
 e_lsp_rq.io.waiting = e_lsp_rq_waiting;
 e_lsp_rq.io.notice = e_lsp_rq_notice;
 e_lsp_rq.io.name = e_lsp_rq.name;
 for (i = 0; i < E_D_TIMERS; i++)
  e_lsp_rq.timer[i] = E_D_GA_NO_TIMER;
 e_lsp_rq.waiting_painted = 0;
 e_lsp_requests_init(g_lsp, &e_lsp_rq.io, getenv(E_D_REQ_TIMEOUT_ENV));
}

/**
 * e_lsp_rq_end - The session is gone: no timer of its runs on, and its
 * waiting line leaves the status row.
 */
static void e_lsp_rq_end(void)
{
 int i;

 for (i = 0; i < E_D_TIMERS; i++)
  e_lsp_rq_disarm(i);
 e_lsp_rq.start_asked = 0;
 e_lsp_rq_waiting_clear();
}

/* The start under way was begun by opening a file, not by an Alt-Q action. */
static int g_lsp_eager_start = 0;

/**
 * e_lsp_start_waited_for - The user asked for something the starting
 * server must answer: from now on the status row counts the start and Esc
 * ends it.
 */
static void e_lsp_start_waited_for(void)
{
 e_lsp_rq.start_asked = 1;
}

/**
 * e_lsp_ask_rows_free - Forget a list answer's entries.
 * @a: the slot.
 */
static void e_lsp_ask_rows_free(e_lsp_ask_t *a)
{
 int i;

 if (!a->rows)
  return;
 for (i = 0; i < a->n; i++)
 {
  free(a->rows[i].name);
  free(a->rows[i].path);
 }
 free(a->rows);
 a->rows = NULL;
}

/**
 * e_lsp_ask_free - Forget a request and its answer; the slot is free.
 * @a: the slot.
 */
static void e_lsp_ask_free(e_lsp_ask_t *a)
{
 e_lsp_ask_rows_free(a);
 free(a->base);
 free(a->text);
 memset(a, 0, sizeof *a);
 a->kind = E_LSP_ASK_NONE;
}

/**
 * e_lsp_ask_clear - Forget every request out and every answer kept.
 */
static void e_lsp_ask_clear(void)
{
 int k;

 for (k = 0; k < E_LSP_ASK_KINDS; k++)
  e_lsp_ask_free(&e_lsp_asks[k]);
}

/**
 * e_lsp_session_drop - End the session without waiting: whoever waits for
 * an answer is told it is gone, the answers kept are forgotten, and the
 * timers and the waiting line go with it.
 */
static void e_lsp_session_drop(void)
{
 if (g_lsp)
  e_lsp_close(g_lsp);
 g_lsp = NULL;
 e_lsp_ask_clear();
 e_lsp_rq_end();
}

/**
 * e_lsp_ask_busy_kind - Is a request of @kind out, its answer not come yet?
 */
static int e_lsp_ask_busy_kind(int kind)
{
 const e_lsp_ask_t *a = &e_lsp_asks[kind];

 return(a->kind != E_LSP_ASK_NONE && !a->ready);
}

/**
 * e_lsp_ask_busy - Is any request out, its answer not come yet?
 */
static int e_lsp_ask_busy(void)
{
 int k;

 for (k = 0; k < E_LSP_ASK_KINDS; k++)
  if (e_lsp_ask_busy_kind(k))
   return(1);
 return(0);
}

/**
 * e_lsp_ask_say_busy - The line that refuses an action: which request is
 * still out and how to give it up.
 * @f: the window.
 */
static void e_lsp_ask_say_busy(FENSTER *f)
{
 char line[E_D_GA_LINE_MAX];
 const char *method = e_lsp_request_waited(g_lsp, NULL);

 snprintf(line, sizeof line, E_LSP_BUSY_FMT, e_lsp_rq.name,
          method ? method : "?");
 e_status_say(f, line);
}

/**
 * e_lsp_ask_refused_elsewhere - An Alt-Q action for another file while
 * requests are out: refused, since that file's server would replace the
 * one still answering and their answers would be lost.
 * @f: the window; @path: its file.
 * Return: 1 when refused.
 */
static int e_lsp_ask_refused_elsewhere(FENSTER *f, const char *path)
{
 if (!g_lsp || !e_lsp_ask_busy() || !strcmp(g_lsp_file, path))
  return(0);
 e_lsp_ask_say_busy(f);
 return(1);
}

/**
 * e_lsp_ask_begin - Note what is asked, from where, on which text, before
 * the request goes out; the same action again while one is out is refused.
 * @f: the window; @kind: E_LSP_ASK_*.
 * Return: the slot, or NULL (refused with a line, or no memory).
 */
static e_lsp_ask_t *e_lsp_ask_begin(FENSTER *f, int kind)
{
 e_lsp_ask_t *a = &e_lsp_asks[kind];

 if (e_lsp_ask_busy_kind(kind))
 {
  e_lsp_ui_trace("lsp ask kind=%d refused: one is out", kind);
  e_lsp_ask_say_busy(f);
  return(NULL);
 }
 e_lsp_ui_trace("lsp ask kind=%d", kind);
 e_lsp_ask_free(a);
 a->base = e_lsp_buffer_text(f);
 if (!a->base)
  return(NULL);
 a->kind = kind;
 a->seq = ++e_lsp_ask_seq;
 snprintf(a->path, sizeof a->path, "%s", g_lsp_file);
 a->y = f->b->b.y;
 a->x = f->b->b.x;
 a->n = -1;
 return(a);
}

/**
 * e_lsp_ask_sent - The request was handed to the engine: watch the server
 * for its answer, or say it could not go out.
 * @f: the window; @a: its slot; @rc: what the engine said (E_D_SEND_*).
 * Return: 0.
 */
static int e_lsp_ask_sent(FENSTER *f, e_lsp_ask_t *a, int rc)
{
 if (rc != E_D_SEND_OK)
 {
  e_lsp_ask_free(a);
  e_error(E_LSP_ASK_FAILED_LINE, 0, f->fb);
  return(0);
 }
 e_lsp_fd_register();
 return(0);
}

/**
 * e_lsp_ask_told - The engine has told the request's end: keep it until
 * the editor can show it (never painted from inside the engine's read).
 * @a: the slot; @how: how it ended.
 */
static void e_lsp_ask_told(e_lsp_ask_t *a, E_D_REQ_END how)
{
 a->how = how;
 a->ready = 1;
 e_lsp_deliver_later();
}

/** e_lsp_text_told - A hover's or a signature's answer: its text, or NULL. */
static void e_lsp_text_told(E_D_REQ_END how, char *text, void *ctx)
{
 e_lsp_ask_t *a = ctx;

 a->text = text;
 e_lsp_ask_told(a, how);
}

/** e_lsp_jump_told - A jump's answer: the location, if one came. */
static void e_lsp_jump_told(E_D_REQ_END how, int found, const char *path,
                            int line, int character, void *ctx)
{
 e_lsp_ask_t *a = ctx;

 (void)character;
 a->found = found;
 if (found)
 {
  snprintf(a->target, sizeof a->target, "%s", path);
  a->line = line;
 }
 e_lsp_ask_told(a, how);
}

/** e_lsp_complete_told - A completion's answer: its candidates. */
static void e_lsp_complete_told(E_D_REQ_END how,
                                const e_lsp_completion_item *items, int n,
                                void *ctx)
{
 e_lsp_ask_t *a = ctx;
 int i;

 if (n > E_LSP_ASK_ITEMS_MAX)
  n = E_LSP_ASK_ITEMS_MAX;
 for (i = 0; i < n; i++)
  a->items[i] = items[i];
 a->n = n;
 e_lsp_ask_told(a, how);
}

/** e_lsp_format_told - A format's answer: the formatted text, or NULL. */
static void e_lsp_format_told(E_D_REQ_END how, char *newtext, void *ctx)
{
 e_lsp_ask_t *a = ctx;

 a->text = newtext;
 e_lsp_ask_told(a, how);
}

/** e_lsp_applied_told - A rename's or an action's answer: the new text. */
static void e_lsp_applied_told(E_D_REQ_END how, char *newtext, int others,
                               void *ctx)
{
 e_lsp_ask_t *a = ctx;

 a->text = newtext;
 a->others = others;
 e_lsp_ask_told(a, how);
}

/**
 * e_lsp_ask_rows_new - Room for @n entries of a list answer; the slot's
 * count says how many came (0 when there is no room).
 * Return: the entries, or NULL.
 */
static e_lsp_ask_row *e_lsp_ask_rows_new(e_lsp_ask_t *a, int n)
{
 int cap = E_LSP_ROWS_MAX;

 if (a->kind == E_LSP_ASK_HIGHLIGHT)
  cap = E_LSP_HL_MAX;
 if (n > cap)
  n = cap;
 a->n = 0;
 if (n <= 0)
  return(NULL);
 a->rows = calloc((size_t)n, sizeof(*a->rows));
 if (a->rows)
  a->n = n;
 return(a->rows);
}

/**
 * e_lsp_ask_strdup - A copy of an engine string the answer keeps, or NULL.
 */
static char *e_lsp_ask_strdup(const char *s)
{
 if (!s)
  return(NULL);
 return(strdup(s));
}

/** e_lsp_locs_told - References or highlight: the locations, copied. */
static void e_lsp_locs_told(E_D_REQ_END how, const e_lsp_location *locs, int n,
                            void *ctx)
{
 e_lsp_ask_t *a = ctx;
 e_lsp_ask_row *rows = e_lsp_ask_rows_new(a, n);
 int i;

 for (i = 0; rows && i < a->n; i++)
 {
  rows[i].path = e_lsp_ask_strdup(locs[i].path);
  rows[i].line = locs[i].line;
  rows[i].character = locs[i].character;
 }
 if (n < 0)
  a->n = -1;
 e_lsp_ask_told(a, how);
}

/** e_lsp_syms_told - Symbols or a hierarchy: the entries, copied. */
static void e_lsp_syms_told(E_D_REQ_END how, const e_lsp_symbol *syms, int n,
                            void *ctx)
{
 e_lsp_ask_t *a = ctx;
 e_lsp_ask_row *rows = e_lsp_ask_rows_new(a, n);
 int i;

 for (i = 0; rows && i < a->n; i++)
 {
  rows[i].name = e_lsp_ask_strdup(syms[i].name);
  rows[i].path = e_lsp_ask_strdup(syms[i].path);
  rows[i].line = syms[i].line;
  rows[i].character = syms[i].character;
 }
 if (n < 0)
  a->n = -1;
 e_lsp_ask_told(a, how);
}

/** e_lsp_ranges_told - Expand selection: the nested ranges, copied. */
static void e_lsp_ranges_told(E_D_REQ_END how, const e_lsp_range *ranges, int n,
                              void *ctx)
{
 e_lsp_ask_t *a = ctx;
 e_lsp_ask_row *rows = e_lsp_ask_rows_new(a, n);
 int i;

 for (i = 0; rows && i < a->n; i++)
  rows[i].range = ranges[i];
 if (n < 0)
  a->n = -1;
 e_lsp_ask_told(a, how);
}

/** e_lsp_lenses_told - Code lenses: titles and lines, copied. */
static void e_lsp_lenses_told(E_D_REQ_END how, const e_lsp_code_lens *lenses,
                              int n, void *ctx)
{
 e_lsp_ask_t *a = ctx;
 e_lsp_ask_row *rows = e_lsp_ask_rows_new(a, n);
 int i;

 for (i = 0; rows && i < a->n; i++)
 {
  rows[i].name = e_lsp_ask_strdup(lenses[i].title);
  rows[i].line = lenses[i].line;
 }
 if (n < 0)
  a->n = -1;
 e_lsp_ask_told(a, how);
}

/** e_lsp_actions_told - Code actions: titles, and which carry their edit. */
static void e_lsp_actions_told(E_D_REQ_END how, const e_lsp_code_action *acts,
                               int n, void *ctx)
{
 e_lsp_ask_t *a = ctx;
 e_lsp_ask_row *rows = e_lsp_ask_rows_new(a, n);
 int i;

 for (i = 0; rows && i < a->n; i++)
 {
  rows[i].name = e_lsp_ask_strdup(acts[i].title);
  rows[i].has_edit = acts[i].has_edit;
 }
 if (n < 0)
  a->n = -1;
 e_lsp_ask_told(a, how);
}

/**
 * e_lsp_ask_window - The window the request was asked from, if it is still
 * the one on top; NULL when it was closed or another is on top.
 * @a: the slot.
 */
static FENSTER *e_lsp_ask_window(const e_lsp_ask_t *a)
{
 FENSTER *w = e_lsp_window_of(a->path);

 if (!w || w != e_lsp_rq_window())
  return(NULL);
 return(w);
}

/**
 * e_lsp_ask_still_true - Are the answer's positions still those of the
 * window's text?  The server answered for the text sent; once the user
 * typed, they point elsewhere.
 * @a: the slot; @w: the window; @cursor_too: the cursor must not have
 * moved either (a popup or a list drawn at it).
 */
static int e_lsp_ask_still_true(const e_lsp_ask_t *a, FENSTER *w,
                                int cursor_too)
{
 char *now = e_lsp_buffer_text(w);
 int same = now && a->base && !strcmp(now, a->base);

 free(now);
 if (!same)
  return(0);
 if (!cursor_too)
  return(1);
 return(w->b->b.y == a->y && w->b->b.x == a->x);
}

/**
 * e_lsp_ask_at_cursor - Is the answer drawn at the cursor, or does it
 * depend on where the cursor was (so it holds only while it has not moved)?
 */
static int e_lsp_ask_at_cursor(int kind)
{
 return(kind == E_LSP_ASK_HOVER || kind == E_LSP_ASK_COMPLETE ||
        kind == E_LSP_ASK_SIGNATURE || kind == E_LSP_ASK_SELECTION ||
        kind == E_LSP_ASK_ACTIONS);
}

static int  e_lsp_hover_show(FENSTER *f, char *hov);                /* (below) */
static void e_lsp_jump_show(FENSTER *f, const e_lsp_ask_t *a);      /* (below) */
static void e_lsp_complete_show(FENSTER *f, const e_lsp_ask_t *a);  /* (below) */
static void e_lsp_format_show(FENSTER *f, const e_lsp_ask_t *a);    /* (below) */
static int  e_lsp_signature_show(FENSTER *f, char *sig);            /* (below) */
static void e_lsp_references_show(FENSTER *f, const e_lsp_ask_t *a);
static void e_lsp_hierarchy_show(FENSTER *f, const e_lsp_ask_t *a);
static void e_lsp_selection_show(FENSTER *f, const e_lsp_ask_t *a);
static void e_lsp_highlight_show(FENSTER *f, const e_lsp_ask_t *a);
static void e_lsp_lenses_show(FENSTER *f, const e_lsp_ask_t *a);
static void e_lsp_outline_show(FENSTER *f, const e_lsp_ask_t *a);
static void e_lsp_symbols_show(FENSTER *f, const e_lsp_ask_t *a);
static void e_lsp_actions_show(FENSTER *f, const e_lsp_ask_t *a);
static void e_lsp_edit_show(FENSTER *f, const e_lsp_ask_t *a);

/**
 * e_lsp_ask_show_kind - Show answer @a on window @w, as its kind does.
 * The text answers (hover, signature) are handed over, since their popup
 * may chain into the next Alt-Q action.
 */
static void e_lsp_ask_show_kind(FENSTER *w, e_lsp_ask_t *a)
{
 char *text = a->text;

 if (a->kind == E_LSP_ASK_HOVER)
 {
  a->text = NULL;
  e_lsp_hover_show(w, text);
 }
 else if (a->kind == E_LSP_ASK_SIGNATURE)
 {
  a->text = NULL;
  e_lsp_signature_show(w, text);
 }
 else if (a->kind == E_LSP_ASK_JUMP)
  e_lsp_jump_show(w, a);
 else if (a->kind == E_LSP_ASK_COMPLETE)
  e_lsp_complete_show(w, a);
 else if (a->kind == E_LSP_ASK_FORMAT)
  e_lsp_format_show(w, a);
 else if (a->kind == E_LSP_ASK_REFERENCES)
  e_lsp_references_show(w, a);
 else if (a->kind == E_LSP_ASK_CALLS || a->kind == E_LSP_ASK_TYPES)
  e_lsp_hierarchy_show(w, a);
 else if (a->kind == E_LSP_ASK_SELECTION)
  e_lsp_selection_show(w, a);
 else if (a->kind == E_LSP_ASK_HIGHLIGHT)
  e_lsp_highlight_show(w, a);
 else if (a->kind == E_LSP_ASK_LENSES)
  e_lsp_lenses_show(w, a);
 else if (a->kind == E_LSP_ASK_OUTLINE)
  e_lsp_outline_show(w, a);
 else if (a->kind == E_LSP_ASK_SYMBOLS)
  e_lsp_symbols_show(w, a);
 else if (a->kind == E_LSP_ASK_ACTIONS)
  e_lsp_actions_show(w, a);
 else if (a->kind == E_LSP_ASK_ACTION_RUN || a->kind == E_LSP_ASK_RENAME)
  e_lsp_edit_show(w, a);
}

/**
 * e_lsp_ask_show - Show an answer that came, on the window it was asked
 * from; an answer whose positions no longer hold is dropped with a line.
 * @a: the slot, freed before anything is shown, so the answer may chain
 * into the next request of its kind.
 */
static void e_lsp_ask_show(e_lsp_ask_t *a)
{
 e_lsp_ask_t shown = *a;
 FENSTER *w = e_lsp_ask_window(&shown);

 memset(a, 0, sizeof *a);
 a->kind = E_LSP_ASK_NONE;
 if (!w || !e_lsp_ask_still_true(&shown, w, e_lsp_ask_at_cursor(shown.kind)))
 {
  e_lsp_ui_trace("lsp show kind=%d dropped: window=%d", shown.kind, w != NULL);
  e_status_say(e_lsp_rq_window(), E_LSP_STALE_LINE);
 }
 else
 {
  e_lsp_ui_trace("lsp show kind=%d n=%d", shown.kind, shown.n);
  e_lsp_ask_show_kind(w, &shown);
 }
 e_lsp_ask_free(&shown);
}

/**
 * e_lsp_ask_next_ready - The answer to show next: of those that came, the
 * one asked first; NULL when none waits to be shown.
 */
static e_lsp_ask_t *e_lsp_ask_next_ready(void)
{
 e_lsp_ask_t *next = NULL;
 int k;

 for (k = 0; k < E_LSP_ASK_KINDS; k++)
 {
  e_lsp_ask_t *a = &e_lsp_asks[k];

  if (a->kind == E_LSP_ASK_NONE || !a->ready)
   continue;
  if (!next || a->seq < next->seq)
   next = a;
 }
 return(next);
}

/* An answer is being shown: one shown from inside another's picker waits. */
static int e_lsp_ask_delivering = 0;

/**
 * e_lsp_ask_deliver - Hand over the answers kept by the continuations, in
 * the order asked, once the editor can show them.  The readers that may have
 * read them call this; what is left behind a box is shown by the screen's
 * owner once the box has closed.
 */
static void e_lsp_ask_deliver(void)
{
 e_lsp_ask_t *a;
 int shown = 0;

 if (e_lsp_ask_delivering)
  return;
 e_lsp_ask_delivering = 1;
 while ((a = e_lsp_ask_next_ready()) != NULL && wpe_paint_idle())
 {
  if (!e_lsp_ask_busy())
   e_lsp_rq_waiting_clear();
  if (a->how == E_D_REQ_DONE)
  {
   e_lsp_ask_show(a);
   shown = 1;
  }
  else
   e_lsp_ask_free(a);    /* given up: the cancel or the deadline said so */
 }
 e_lsp_ask_delivering = 0;
 if (e_lsp_ask_next_ready())
  e_lsp_deliver_later();
 /* Shown from the loop, with no key after it to flush the screen: a
    highlight, a grown mark or a jump would otherwise stay unseen. */
 if (shown)
  e_refresh();
}

/**
 * e_lsp_deliver_held - The screen is the editor's again: an answer that
 * came behind a box is shown, and an overlay fetched meanwhile is painted.
 * @data: unused.
 */
static void e_lsp_deliver_held(void *data)
{
 (void)data;
 e_lsp_ask_deliver();
 e_lsp_overlay_deliver();
}

/**
 * e_lsp_answer_busy - Does Esc have something of the language server's to
 * give up: an answer out, or a start the user waits for?
 */
int e_lsp_answer_busy(void)
{
 if (!g_lsp)
  return(0);
 if (e_lsp_ask_busy())
  return(1);
 return(e_lsp_rq.start_asked && !e_lsp_started(g_lsp));
}

/**
 * e_lsp_start_cancel - Esc while the server the user asked for starts: it
 * is ended through the owner of the editor's children, and nothing is
 * asked of it later.
 */
static void e_lsp_start_cancel(void)
{
 FENSTER *top = e_lsp_rq_window();

 e_lsp_ui_shutdown();
 if (!top)
  return;
 e_d_p_message(E_LSP_START_CANCELLED_LINE, top, 0);
 e_status_say(top, E_LSP_START_CANCELLED_LINE);
}

/**
 * e_lsp_answer_cancel - Esc while the editor waits for the language
 * server: every answer out, or the start, is given up, and the status row
 * says so.  The overlay fetches the user did not ask for go on.
 */
void e_lsp_answer_cancel(void)
{
 char line[E_D_GA_LINE_MAX];
 const char *method;

 if (!g_lsp)
  return;
 if (!e_lsp_ask_busy())
 {
  e_lsp_start_cancel();
  return;
 }
 method = e_lsp_request_waited(g_lsp, NULL);
 snprintf(line, sizeof line, E_LSP_CANCELLED_FMT, e_lsp_rq.name,
          method ? method : "?");
 e_lsp_requests_give_up(g_lsp, E_D_REQ_CANCELLED);
 e_lsp_rq.waiting_painted = 0;
 e_lsp_ask_clear();
 e_status_say(e_lsp_rq_window(), line);
}

/**
 * e_lsp_scala_setup_done - `scala-cli setup-ide` ended: start Metals for the
 * file it was run for, if that file is still open.
 * @ok: unused (Metals imports the build itself when the setup failed);
 * @dir: the project.
 *
 * Runs outside any dialog (e_scala_setup_deliver_held), so the "Starting
 * language server" line is never drawn under a box.
 */
static void e_lsp_scala_setup_done(int ok, const char *dir)
{
 FENSTER *w = e_lsp_window_of(g_lsp_setup_for);

 (void)ok;
 (void)dir;
 snprintf(g_lsp_setup_done, sizeof g_lsp_setup_done, "%s", g_lsp_setup_for);
 g_lsp_setup_for[0] = '\0';
 if (w)
  e_lsp_ensure(w);
}

/**
 * e_lsp_setup_scala_build - Metals-only pre-launch setup for a Scala file:
 * JAVA_HOME pinned so Metals' presentation compiler does not run on a
 * too-new JDK, then `scala-cli setup-ide` in the project so Metals
 * auto-connects to the build server.
 * @f: the file's window; @dir: the project; @path: the file.
 *
 * The setup runs without holding the editor, and Metals starts when it has
 * ended.  Both steps are best-effort: the server starts either way (the
 * user sees diagnostics if the build is misconfigured).
 * Return: -1, the "still starting" answer of e_lsp_ensure.
 */
static int e_lsp_setup_scala_build(FENSTER *f, const char *dir,
                                   const char *path)
{
 if (e_scala_setup_busy(&e_scala_setup_lsp, NULL))
 {
  e_d_p_message(E_SCALA_SETUP_LINE, f, 0);
  return(-1);
 }
 e_scala_pin_jdk(f, "Metals");  /* keep Metals' Scala PC off a too-new JDK */
 snprintf(g_lsp_setup_for, sizeof g_lsp_setup_for, "%s", path);
 if (e_scala_setup_begin(&e_scala_setup_lsp, dir, e_lsp_scala_setup_done) != 0)
 {
  e_lsp_scala_setup_done(0, dir);   /* non-fatal: Metals imports the build */
  return(-1);
 }
 e_d_p_message(E_SCALA_SETUP_LINE, f, 1);
 e_lsp_raise_source(f);
 return(-1);
}

/**
 * e_lsp_scala_setup_drop_other - The user asked for the server of another
 * file: a setup still running for an earlier Scala file is no longer
 * wanted, so Metals does not start for that file later and replace the
 * server the user asked for now.
 * @path: the file the server is now for.
 */
static void e_lsp_scala_setup_drop_other(const char *path)
{
 if (!e_scala_setup_busy(&e_scala_setup_lsp, NULL) ||
     !strcmp(g_lsp_setup_for, path))
  return;
 e_scala_setup_drop(&e_scala_setup_lsp);
 g_lsp_setup_for[0] = '\0';
}

/**
 * e_scala_setup_busy_any - Is `scala-cli setup-ide` running for the
 * language server or the debugger?  The key loop asks, so Esc cancels it,
 * as the waiting line says.
 * Return: 1 while one runs.
 */
int e_scala_setup_busy_any(void)
{
 if (e_scala_setup_busy(&e_scala_setup_lsp, NULL))
  return(1);
 return(e_scala_setup_busy(&e_scala_setup_dap, NULL));
}

/**
 * e_scala_setup_cancel - Esc while `scala-cli setup-ide` runs: end it, and
 * start neither the language server nor the debug session it was run for.
 *
 * As with Esc on a build, the user changed their mind: nothing starts
 * behind their back later, and Messages and the status row say so.
 */
void e_scala_setup_cancel(void)
{
 FENSTER *top = WpeEditor->f[WpeEditor->mxedt];

 if (e_scala_setup_busy(&e_scala_setup_dap, NULL))
  e_d_resume_cmd = NULL;
 e_scala_setup_drop(&e_scala_setup_lsp);
 e_scala_setup_drop(&e_scala_setup_dap);
 g_lsp_setup_for[0] = '\0';
 e_d_p_message(E_SCALA_SETUP_CANCELLED_LINE, top, 0);
 e_status_say(top, E_SCALA_SETUP_CANCELLED_LINE);
}

/**
 * e_lsp_scala_setup_needed - Must `scala-cli setup-ide` run before Metals
 * starts for @path?  Once it has ended for that file, the next start of
 * the server goes straight on (and the one after that runs it again, as
 * every new session did).
 * @lang: the file's language; @path: the file.
 */
static int e_lsp_scala_setup_needed(const char *lang, const char *path)
{
 if (strcmp(lang, "scala"))
  return(0);
 if (!strcmp(g_lsp_setup_done, path))
 {
  g_lsp_setup_done[0] = '\0';
  return(0);
 }
 return(1);
}

static int e_lsp_ensure(FENSTER *f)
{
 const char *lang = e_lsp_lang_for(f);
 static e_lsp_host host;
 const e_lsp_server *srv = e_lsp_server_for(lang);
 char dir[1024], path[1200];
 int dl;

 if (!lang || !srv)
 {  e_error("Language server: unsupported file type.", 0, f->fb);  return(-1);  }
 if (e_lsp_is_dep_source(f))
 {  e_error("Read-only library source -- language-server actions run from the "
            "project window, not here.", 0, f->fb);  return(-1);  }
 snprintf(path, sizeof(path), "%s%s", f->dirct ? f->dirct : "./", f->datnam);
 if (e_lsp_ask_refused_elsewhere(f, path))
  return(-1);                                   /* its answers would be lost */
 if (g_lsp && !strcmp(g_lsp_file, path))
 {
  if (e_lsp_started(g_lsp))
   return(0);                                  /* ready -> the action proceeds */
  e_lsp_start_waited_for();
  e_d_p_message("Language server still starting -- try the action again shortly.",
                f, 0);
  return(-1);                                   /* still starting -> action waits */
 }
 e_lsp_scala_setup_drop_other(path);
 if (g_lsp)
 {  e_lsp_fd_unregister();  e_lsp_session_drop();  e_lsp_diag_clear();
    e_lsp_inlay_clear();  e_lsp_overlay_held_drop();  g_inlay_on = 0;
    g_sem_on = 0;  g_sem_nactive = 0;  }  /* switched files: drop overlays */
 if (e_test_command((char *)srv->argv[0]))
 {  e_error((char *)srv->missing, 0, f->fb);  return(-1);  }

 snprintf(dir, sizeof(dir), "%s", f->dirct ? f->dirct : ".");
 dl = strlen(dir);
 while (dl > 1 && dir[dl - 1] == DIRC)
  dir[--dl] = '\0';
 g_lsp_fenster = f;
 strncpy(g_lsp_file, path, sizeof(g_lsp_file) - 1);
 g_lsp_file[sizeof(g_lsp_file) - 1] = '\0';

 if (e_lsp_scala_setup_needed(lang, path))
  return(e_lsp_setup_scala_build(f, dir, path));   /* Metals starts after it */

 host.on_diagnostic = e_lsp_on_diag;
 host.on_diagnostics_summary = e_lsp_on_diag_summary;
 host.on_show_text = e_lsp_on_show_text;
 host.on_status = e_lsp_on_status;
 host.on_inlay_refresh = e_lsp_on_inlay_refresh;
 host.on_semantic_refresh = e_lsp_on_semantic_refresh;
 host.ud = NULL;
 host.server_shares_daemon = srv->shares_daemon;
 g_inlay_stale = 0;
 if (e_lsp_is_worksheet(f))               /* worksheet: show eval results without Alt-Q Y */
  g_inlay_on = 1;
 g_sem_on = 0;  g_sem_nactive = 0;  g_sem_stale = 0;
 g_lsp_ready_done = 0;                     /* fresh session: re-announce "ready" once */
 g_lsp_pending_ws = NULL;
 g_lsp_last_err = g_lsp_last_warn = -1;   /* fresh session: force first report */
 g_lsp_said_err = g_lsp_said_warn = -1;
 free(g_lsp_doctor_last);  g_lsp_doctor_last = NULL;   /* fresh session: re-show Doctor */
 g_lsp_doctor_announced = 0;
 free(g_lsp_status_last);  g_lsp_status_last = NULL;   /* fresh session: clean status   */
 g_lsp_busy = 0;
 /* ASYNC: spawn + send initialize, then return.  The JVM boot, import and first
    compile complete in the background via e_lsp_on_fd_readable. */
 g_lsp = e_lsp_open_async(srv->argv, dir, lang, &host);
 if (!g_lsp)
 {
  e_error("Could not start the language server.", 0, f->fb);
  g_lsp_file[0] = '\0';
  return(-1);
 }
 e_lsp_rq_begin(srv->label);
 if (!g_lsp_eager_start)
  e_lsp_start_waited_for();
 e_lsp_synced_set(NULL);                   /* not opened yet; on-ready will didOpen */
 if (e_lsp_fd_register() < 0)
  return(e_lsp_give_up_unwatched());
 e_d_p_message("Starting language server (the editor stays responsive)...", f, 1);
 e_lsp_raise_source(f);   /* the sw=1 message surfaced Messages -- put the code
                             back on top so the user keeps editing, not stranded */
 return(-1);                                /* starting -- ready on a later call */
}

/* AltQ E -- (re)start the server for this file and show its diagnostics. */
static int e_lsp_ui_diagnostics(FENSTER *f)
{
 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_d_p_message("Language server ready.", f, 1);
 return(0);
}

/* Defined below; the symbol actions snap the request to the identifier start. */
static int e_lsp_symbol_col(FENSTER *f);
static int e_lsp_ui_dispatch(FENSTER *f, int c);   /* run the Alt-Q action `c` */
void e_lsp_after_bulk_edit(FENSTER *f);            /* re-sync after a whole-buffer edit */

/* The three "jump to a location" actions (definition / implementation / type
   definition) differ only in the request and the not-found wording, so they
   share this driver.  `which` is an E_LSP_LOCATE_* request; `what` names the
   target for the error message.  The jump happens when the answer comes. */
static int e_lsp_ui_jump(FENSTER *f, int which, const char *what)
{
 BUFFER *b = f->b;
 e_lsp_ask_t *a;

 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_sync(f);
 a = e_lsp_ask_begin(f, E_LSP_ASK_JUMP);
 if (!a)
  return(-1);
 snprintf(a->what, sizeof a->what, "%s", what);
 return(e_lsp_ask_sent(f, a, e_lsp_locate_async(g_lsp, which, g_lsp_file,
                                                b->b.y, e_lsp_symbol_col(f),
                                                e_lsp_jump_told, a)));
}

/**
 * e_lsp_jump_show - The location came: the cursor goes there, in the file
 * that holds it; with none, the box says what was not found.
 * @f: the window it was asked from; @a: the answer.
 */
static void e_lsp_jump_show(FENSTER *f, const e_lsp_ask_t *a)
{
 char msg[E_LSP_WHAT_MAX + 32];

 if (!a->found)
 {
  snprintf(msg, sizeof(msg), "No %s found.", a->what);
  e_error(msg, 0, f->fb);
  return;
 }
 e_d_goto_break((char *)a->target, a->line + 1, f);  /* 0-based -> 1-based */
}

/* AltQ D -- jump to the definition of the symbol under the cursor. */
static int e_lsp_ui_definition(FENSTER *f)
{
 return(e_lsp_ui_jump(f, E_LSP_LOCATE_DEFINITION, "definition"));
}

/* AltQ I -- jump to the implementation of the symbol (concrete override of an
   abstract def / trait member). */
static int e_lsp_ui_implementation(FENSTER *f)
{
 return(e_lsp_ui_jump(f, E_LSP_LOCATE_IMPLEMENTATION, "implementation"));
}

/* AltQ T -- jump to the TYPE of the symbol (the class/trait of a val's type,
   not the val's own definition). */
static int e_lsp_ui_type_definition(FENSTER *f)
{
 return(e_lsp_ui_jump(f, E_LSP_LOCATE_TYPE_DEFINITION, "type definition"));
}

/* e_lsp_ui_trace - append a printf-style diagnostic line to the file named by
   $XWPE_UI_TRACE.  A no-op (and zero cost) unless that env var is set.  Used to
   confirm, from a headless VHS/pyte recording, WHICH Alt-Q action ran and what
   key/word it saw -- the data-not-guesswork tool for demo debugging (the
   wpe-vs-we binary mix-up was found exactly this way). */
static void e_lsp_ui_trace(const char *fmt, ...)
{
 const char *tp = getenv("XWPE_UI_TRACE");
 FILE *tf;
 va_list ap;

 if (!tp || !(tf = fopen(tp, "a")))
  return;
 va_start(ap, fmt);
 vfprintf(tf, fmt, ap);
 va_end(ap);
 fputc('\n', tf);
 fclose(tf);
}

/* ---- hover / signature cursor tooltip ----------------------------------- *
 * A small, non-modal box anchored AT THE CURSOR that shows one piece of LSP
 * info (a hover blurb, or a call signature).  Unlike the centered e_message box
 * it used to use, it does not take over the screen: it opens just below the
 * cursor line (or above when there is no room) and ANY key dismisses it (the key
 * is consumed -- it does NOT edit the buffer, so a glance never risks a stray
 * keystroke).  The async LSP fd-loop's painting is suspended while it is up (the
 * modal-depth guard) so a streamed server update cannot draw under and corrupt
 * it.                                                                          */

#define LSP_TIP_MAXLINES 12                  /* cap a long hover blurb          */

/* A comfortable tooltip text width: about 55% of the screen, clamped to a
   readable range, so a long hover WRAPS onto several lines instead of running off
   in one.  Falls back to (almost) the whole screen on a very narrow terminal. */
static int e_lsp_tip_width(void)
{
 int w = MAXSCOL * 11 / 20;
 if (w > 70)
  w = 70;
 if (w < 24)
  w = (MAXSCOL > 14) ? MAXSCOL - 6 : 8;
 return(w);
}

/* e_lsp_tip_split - lay TEXT out as display lines: split on '\n', then WORD-WRAP
   each source line to `wrap` columns (breaking at spaces; one over-long word is
   hard-split).  Up to `max` lines, copied into the reused static buffer (so the
   returned pointers stay valid until the next call).  Trailing blank lines are
   dropped; the widest line width is returned via *widest; returns the count. */
static int e_lsp_tip_split(const char *text, char **tln, int max, int wrap, int *widest)
{
 static char wbuf[4096];
 int n = 0, w = 0, wp = 0;
 const char *p = text;

 if (wrap < 8)
  wrap = 8;
 while (n < max && *p && wp < (int)sizeof(wbuf) - 2)
 {
  char *line = wbuf + wp;
  int col = 0;
  while (*p && *p != '\n')
  {
   const char *ws = p, *we;
   int wlen, sep;
   while (*ws == ' ' || *ws == '\t')           /* collapse runs of whitespace    */
    ws++;
   if (*ws == '\0' || *ws == '\n')
   {  p = ws;  break;  }
   we = ws;
   while (*we && *we != ' ' && *we != '\t' && *we != '\n')
    we++;
   wlen = (int)(we - ws);
   sep = (col > 0) ? 1 : 0;
   if (col > 0 && col + sep + wlen > wrap)      /* word won't fit -> next line    */
    break;
   if (col == 0 && wlen > wrap)                 /* a lone word wider than the box */
   {  wlen = wrap;  we = ws + wrap;  }
   if (wp + sep + wlen >= (int)sizeof(wbuf) - 2)
    break;
   if (sep)
    wbuf[wp++] = ' ';
   memcpy(wbuf + wp, ws, (size_t)wlen);
   wp += wlen;
   col += sep + wlen;
   p = we;
  }
  wbuf[wp++] = '\0';
  tln[n++] = line;
  if (col > w)
   w = col;
  if (*p == '\n')
   p++;
 }
 while (n > 0 && tln[n-1][0] == '\0')           /* drop trailing blank lines      */
  n--;
 *widest = w;
 return(n);
}

/* e_lsp_cursor_popup - draw `text` (titled `title`) in a box at the cursor, wait
   for one key to dismiss it, restore the screen, and RETURN the dismiss key (so a
   caller like diagnostic navigation can keep walking on '.'/',' without making the
   user re-open it).  Used by hover, signature help and the problem jumps. */
static int e_lsp_cursor_popup(FENSTER *f, const char *title, const char *text,
                              const char *hint)
{
 BUFFER *b = f->b;
 SCHIRM *s = f->s;
 char *tln[LSP_TIP_MAXLINES];
 int n, w, i, c, cx, cy, xa, ya, xe, ye;
 PIC *pic;

 if (!text || !*text)
  return(0);
 n = e_lsp_tip_split(text, tln, LSP_TIP_MAXLINES, e_lsp_tip_width(), &w);
 if (n <= 0)
  return(0);
 if (w < (int)strlen(title))
  w = (int)strlen(title);                      /* keep the title readable         */
 if (hint && (int)strlen(hint) > w)
  w = (int)strlen(hint);                       /* ...and the footer legend too    */

 cx = f->a.x + b->b.x - s->c.x + 1;            /* cursor's screen column (approx) */
 cy = f->a.y + b->b.y - s->c.y + 1;            /* cursor's screen row             */
 xa = cx - 1;
 xe = xa + w + 3;                              /* frame + one space padding       */
 if (xe > MAXSCOL - 1)
 {  xe = MAXSCOL - 1;  xa = xe - (w + 3);  }
 if (xa < 0)
 {  xa = 0;  xe = w + 3;  if (xe > MAXSCOL - 1) xe = MAXSCOL - 1;  }
 ya = cy + 1;                                  /* prefer just below the cursor    */
 ye = ya + n + 1;
 if (ye > MAXSLNS - 2 || ya >= f->e.y)         /* no room below (screen or the    */
 {  ye = cy - 1;  ya = ye - (n + 1);  }        /* editor window) -> flip above    */
 if (ya < 1)
 {  ya = 1;  ye = ya + n + 1;  }

 fk_cursor(0);
 pic = e_std_kst(xa, ya, xe, ye, (char *)title, 1,
                 f->fb->nr.fb, f->fb->nt.fb, f->fb->ne.fb);
 if (!pic)
 {  fk_cursor(1);  return(0);  }
 for (i = 0; i < n; i++)
  e_pr_str(xa + 2, ya + 1 + i, tln[i], f->fb->nt.fb, 0, 0, 0, 0);
 if (hint && *hint)                            /* a key legend in the bottom border, */
 {                                             /* Borland-style (like the title above) */
  int hl = (int)strlen(hint);
  int hx = xa + (xe - xa - hl) / 2;
  if (hx < xa + 1)
   hx = xa + 1;
  e_pr_str(hx, ye, (char *)hint, f->fb->nr.fb, 0, 0, 0, 0);
 }
 e_refresh();

 c = wpe_paint_modal_getch();                  /* any key dismisses (consumed)     */

 e_close_view(pic, 1);                         /* restore what the box covered     */
 fk_cursor(1);
 e_cursor(f, 0);
 return(c);
}

/* AltQ H -- show the type/documentation of the symbol under the cursor. */
static int e_lsp_ui_hover(FENSTER *f)
{
 BUFFER *b = f->b;
 e_lsp_ask_t *a;

 { const char *tp = getenv("XWPE_UI_TRACE");
   if (tp) { FILE *tf = fopen(tp, "a");
     if (tf) { fprintf(tf, "e_lsp_ui_hover ENTER\n"); fclose(tf); } } }
 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_sync(f);
 a = e_lsp_ask_begin(f, E_LSP_ASK_HOVER);
 if (!a)
  return(-1);
 return(e_lsp_ask_sent(f, a, e_lsp_hover_async(g_lsp, g_lsp_file, b->b.y,
                                               e_lsp_symbol_col(f),
                                               e_lsp_text_told, a)));
}

/**
 * e_lsp_hover_show - The hover's answer came: its text in a popup at the
 * cursor, or a line saying there is none.
 * @f: the window it was asked from; @hov: the text (freed here), or NULL.
 * Return: 0, or what the Alt-Q action chained from the popup returns.
 */
static int e_lsp_hover_show(FENSTER *f, char *hov)
{
 if (!hov || !*hov)
 {
  if (hov)
   free(hov);
  /* While Metals is still indexing, the presentation compiler has no type info
     yet, so hover legitimately comes back empty.  Say so instead of the bare
     "No hover information", which reads as a permanent answer. */
  if (g_lsp_busy)
   e_error("Language server still working -- try again in a moment.", 0, f->fb);
  else
   e_error("No hover information.", 0, f->fb);
  return(0);
 }
 { int k = e_lsp_cursor_popup(f, "Hover", hov, "Press any key to close");
   free(hov);
   /* Alt-Q while the peek is up: chain straight into the next action (e.g. hover
      then Alt-Q d to jump to the definition) instead of just dismissing. */
   if (k == AltQ)
    return(e_lsp_ui_dispatch(f, wpe_paint_modal_getch()));
 }
 return(0);
}

/* True for an identifier character (the word the completion replaces). */
static int e_lsp_is_ident(int c)
{
 return((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_');
}

/* Column to send for a SYMBOL action (definition / hover / references / ...):
   snap to the START of the identifier under (or just after) the cursor, so the
   action resolves wherever on the word the cursor sits -- a server can return
   nothing at the token's trailing edge, which read as "No definition found"
   even though the cursor looked like it was on the name.  If the cursor is not
   on an identifier at all, the column is returned unchanged. */
static int e_lsp_symbol_col(FENSTER *f)
{
 BUFFER *b = f->b;
 char *line = b->bf[b->b.y].s;
 int x = b->b.x;

 if (!line)
  return(x);
 /* cursor one past the word (e.g. on the ')' after "Shape") -> step onto it */
 if (x > 0 && !e_lsp_is_ident((unsigned char)line[x]) &&
     e_lsp_is_ident((unsigned char)line[x - 1]))
  x--;
 while (x > 0 && e_lsp_is_ident((unsigned char)line[x - 1]))
  x--;
 return(x);
}

/* Defined further down; used by the workspace-symbol and code-action actions. */
static void e_lsp_replace_buffer(FENSTER *f, const char *newtext);
static void e_lsp_word_at_cursor(FENSTER *f, char *out, size_t osz);

/* Copy `src` into `dst` (capacity `sz`), clipping past `maxw` columns with an
   ellipsis so an over-long row never spills past the pick dialog's border
   (workspace-symbol results carry file paths and can be very long). */
static void e_lsp_fit_row(char *dst, size_t sz, const char *src, int maxw)
{
 if ((int)strlen(src) <= maxw)
  snprintf(dst, sz, "%s", src);
 else
  snprintf(dst, sz, "%.*s...", maxw - 3, src);
}

/* Compose the pick dialog's window title into `dst`: the action title, plus a
   "(first N of M)" note when the list was capped, so nothing is hidden silently. */
static void e_lsp_pick_title(char *dst, size_t sz, const char *title,
                             int vis, int n)
{
 if (n > vis)
  snprintf(dst, sz, "%s (first %d of %d)", title, vis, n);
 else
  snprintf(dst, sz, "%s", title);
}

/* Show a single-column pick list (the dialog radio widget: arrows navigate,
   Enter selects, Esc cancels) of `n` labels under `title`, and return the chosen
   index or -1 if cancelled.  At most 16 rows are shown; when the list is longer
   the title states how many were hidden (no silent truncation).  Shared by the
   completion / outline / workspace-symbol / code-action pickers. */
/* e_lsp_pick_at - the shared radio-list picker.  When ax >= 0 the box is anchored
   at screen (ax, ay) -- just below that cell, flipped above when there is no room
   -- so completion can drop down AT THE CURSOR like a real editor; ax < 0 keeps
   the default top-left position used by the outline / symbol / lens pickers. */
static int e_lsp_pick_at(FENSTER *f, const char *title, const char *const *labels,
                         int n, int ax, int ay)
{
 W_OPTSTR *o;
 char name[80];
 static char rows[16][64];     /* truncated rows, persist through e_opt_kst */
 const int maxw = 56;          /* longest row we render; longer is ...-clipped */
 int i, sel = -1, vis, mxlen = 0, titlen, w, bw, bh;

 vis = n < 16 ? n : 16;
 if (ax >= 0)                           /* anchored: a compact dropdown that fits the  */
 {                                      /* editor window at the cursor, not a wall     */
  int below = (f->e.y - 2) - (ay + 1);      /* item rows below the cursor, in-window  */
  int above = (ay - 1) - (f->a.y + 1);      /* ...or above it                          */
  int room  = (below > above) ? below : above;
  if (room > 10)
   room = 10;                           /* a dropdown shows ~10; the title says of N  */
  if (room < 4)
   room = 4;                            /* always offer at least a few choices        */
  if (vis > room)
   vis = room;
 }
 e_lsp_pick_title(name, sizeof(name), title, vis, n);
 for (i = 0; i < vis; i++)
 {
  e_lsp_fit_row(rows[i], sizeof(rows[i]), labels[i], maxw);
  if ((int)strlen(rows[i]) > mxlen)
   mxlen = strlen(rows[i]);
 }
 /* Width must hold BOTH the widest row (label + the "( ) " radio prefix) AND
    the title drawn in the top border -- otherwise a longer title is clipped
    ("Code lenses" -> "Code ...").  Take the larger. */
 titlen = strlen(name);
 w = mxlen + 4;
 if (titlen + 2 > w)
  w = titlen + 2;
 o = e_init_opt_kst(f);
 if (!o)
  return(-1);
 bw = w + 4;                            /* box outer width  */
 bh = vis + 3;                          /* box outer height (rows + frame + Ok)   */
 if (ax >= 0)                           /* drop down at the cursor                 */
 {
  o->xa = ax - 1;
  o->ya = ay + 1;                       /* just below the cursor line             */
  if (o->xa + bw > MAXSCOL - 1)
   o->xa = MAXSCOL - 1 - bw;
  if (o->xa < 0)
   o->xa = 0;
  if (o->ya + bh > MAXSLNS - 2 || o->ya >= f->e.y)   /* no room below -> above     */
   o->ya = ay - 1 - bh;
  if (o->ya < 1)
   o->ya = 1;
 }
 else                                   /* default: the top-left list position     */
 {
  o->xa = 8;
  o->ya = 3;
 }
 o->xe = o->xa + bw;
 o->ye = o->ya + bh;
 o->bgsw = 0;
 o->crsw = AltO;     /* Enter on a radio confirms via the Ok button (Borland) */
 o->name = name;
 /* Each radio option needs a UNIQUE NON-ZERO switch id: e_get_opt_sw_spatial
    returns the focused widget's sw and treats 0 as "not found", so all-zero
    options leave the dialog unable to take initial focus -- its modal loop then
    never reads input and spins at 100% CPU.  Use ids well above the key-code
    range (Alt-keys/cursor keys are < 340) so they never collide with a real
    keypress (navigation is by arrows; these ids are focus handles only). */
 for (i = 0; i < vis; i++)
  e_add_pswstr(0, 3, 1 + i, -1, 10001 + i, 0, rows[i], o);
 e_add_bttstr((o->xe - o->xa - 4) / 2, o->ye - o->ya - 1, 0, AltO, "Ok", NULL, o);
 if (e_opt_kst(o) != WPE_ESC)
  sel = o->pstr[0]->num;
 freeostr(o);
 if (sel < 0 || sel >= vis)
  return(-1);
 return(sel);
}

/* e_lsp_pick - the picker at its default top-left position, for the "browse the
   whole file/project" lists (outline, lenses, workspace/file symbols) where the
   cursor position is irrelevant to the choice. */
static int e_lsp_pick(FENSTER *f, const char *title, const char *const *labels,
                      int n)
{
 return(e_lsp_pick_at(f, title, labels, n, -1, -1));
}

/* Screen cell currently under the text cursor.  A *contextual* popup (completion,
   code actions) drops here so it appears AT the edit point -- like a real editor's
   autocomplete -- instead of parked in the corner.  Mirrors fk_locate's mapping
   of buffer coords to screen coords. */
static int e_lsp_cursor_screen_x(FENSTER *f)
{  return(f->a.x + f->b->b.x - f->s->c.x + 1);  }
static int e_lsp_cursor_screen_y(FENSTER *f)
{  return(f->a.y + f->b->b.y - f->s->c.y + 1);  }

/* AltQ C -- offer completion candidates for the word under the cursor in a
   navigable popup (the dialog radio list); insert the chosen one, replacing the
   partial word.  Reuses the dialog widget system (arrows navigate, Enter
   selects, Esc cancels) -- no bespoke popup. */
static int e_lsp_ui_complete(FENSTER *f)
{
 BUFFER *b = f->b;
 e_lsp_ask_t *a;

 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_sync(f);
 a = e_lsp_ask_begin(f, E_LSP_ASK_COMPLETE);
 if (!a)
  return(-1);
 return(e_lsp_ask_sent(f, a, e_lsp_completion_async(g_lsp, g_lsp_file, b->b.y,
                                                    b->b.x, E_LSP_ASK_ITEMS_MAX,
                                                    e_lsp_complete_told, a)));
}

/**
 * e_lsp_complete_show - The candidates came: the list drops down at the
 * cursor and the chosen one replaces the word typed so far; with none, a
 * box says so.
 * @f: the window it was asked from; @a: the answer.
 */
static void e_lsp_complete_show(FENSTER *f, const e_lsp_ask_t *a)
{
 BUFFER *b = f->b;
 SCHIRM *s = f->s;
 const e_lsp_completion_item *items = a->items;
 const char *labels[E_LSP_ASK_ITEMS_MAX];
 char *line, insbuf[256];
 const char *ins;
 int n = a->n, i, sel, prefix = 0, inslen;

 if (n <= 0)
 {  e_error("No completions.", 0, f->fb);  return;  }

 /* the identifier already typed before the cursor (to be replaced) */
 line = b->bf[b->b.y].s;
 while (prefix < b->b.x && e_lsp_is_ident((unsigned char)line[b->b.x - 1 - prefix]))
  prefix++;

 for (i = 0; i < n; i++)
  labels[i] = items[i].label;
 /* Drop the list down AT THE CURSOR (over the partial word being completed), like
    a real editor's autocomplete, rather than a box at the screen corner. */
 sel = e_lsp_pick_at(f, "Complete", labels, n,
                     e_lsp_cursor_screen_x(f), e_lsp_cursor_screen_y(f));
 if (sel < 0)
  return;

 /* what to insert: the LSP insertText, else the label up to '(' / ':' / ' ' */
 ins = items[sel].insert;
 if (!ins || !*ins)
 {
  const char *p = items[sel].label;
  int k = 0;
  while (p[k] && p[k] != '(' && p[k] != ':' && p[k] != ' ' && k < (int)sizeof(insbuf) - 1)
   k++;
  memcpy(insbuf, p, k);
  insbuf[k] = '\0';
  ins = insbuf;
 }
 inslen = strlen(ins);
 if (prefix > 0)
 {
  b->b.x -= prefix;
  e_del_nchar(b, s, b->b.x, b->b.y, prefix);
 }
 e_ins_nchar(b, s, (char *)ins, b->b.x, b->b.y, inslen);
 b->b.x += inslen;
 e_schirm(f, 1);
}

/* AltQ S -- show the signature of the call the cursor is inside.  The popup
   comes when the server answers (e_lsp_signature_show). */
static int e_lsp_ui_signature(FENSTER *f)
{
 BUFFER *b = f->b;
 e_lsp_ask_t *a;

 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_sync(f);
 a = e_lsp_ask_begin(f, E_LSP_ASK_SIGNATURE);
 if (!a)
  return(-1);
 return(e_lsp_ask_sent(f, a, e_lsp_signature_help_async(g_lsp, g_lsp_file,
                                                        b->b.y, b->b.x,
                                                        e_lsp_text_told, a)));
}

/**
 * e_lsp_signature_show - The signature came: in a popup at the cursor, or
 * a box saying there is none.  Alt-Q while it is up runs the next action.
 * @f: the window it was asked from; @sig: the label (freed here), or NULL.
 * Return: 0, or what the chained action returns.
 */
static int e_lsp_signature_show(FENSTER *f, char *sig)
{
 int k;

 if (!sig || !*sig)
 {
  free(sig);
  e_error("No signature here.", 0, f->fb);
  return(0);
 }
 k = e_lsp_cursor_popup(f, "Signature", sig, "Press any key to close");
 free(sig);
 if (k == AltQ)                         /* chain into the next Alt-Q action */
  return(e_lsp_ui_dispatch(f, wpe_paint_modal_getch()));
 return(0);
}

/* ---- diagnostic navigation (Alt-Q . / Alt-Q ,) -------------------------- *
 * Jump the cursor between the problems the server reported (g_diag_active),
 * ordered by position, wrapping around the file -- the editor equivalent of an
 * IDE's F8 / Shift-F8.  On arrival the problem's message is shown in the cursor
 * tooltip, so you read it where it happens without hunting the Messages list.  */

/* True if diagnostic i lies before diagnostic j in the file (line, then column). */
static int e_lsp_diag_before(int i, int j)
{
 if (g_diag_active[i].line != g_diag_active[j].line)
  return(g_diag_active[i].line < g_diag_active[j].line);
 return(g_diag_active[i].c0 < g_diag_active[j].c0);
}

/* Index of the diagnostic nearest the cursor in direction `dir` (+1 = next,
   -1 = previous), wrapping to the far end when none lie ahead; -1 if there are
   no diagnostics at all. */
static int e_lsp_diag_pick(int cy, int cx, int dir)
{
 int i, best = -1;

 for (i = 0; i < g_diag_nactive; i++)            /* nearest strictly ahead         */
 {
  int dl = g_diag_active[i].line, dc = g_diag_active[i].c0;
  int ahead = (dir > 0) ? (dl > cy || (dl == cy && dc > cx))
                        : (dl < cy || (dl == cy && dc < cx));
  if (!ahead)
   continue;
  if (best < 0 ||
      ((dir > 0) ? e_lsp_diag_before(i, best) : e_lsp_diag_before(best, i)))
   best = i;
 }
 if (best >= 0)
  return(best);
 for (i = 0; i < g_diag_nactive; i++)            /* none ahead -> wrap to extreme  */
  if (best < 0 ||
      ((dir > 0) ? e_lsp_diag_before(i, best) : e_lsp_diag_before(best, i)))
   best = i;
 return(best);
}

/* Move the cursor to the next/previous problem and show its message in the cursor
   tooltip; wraps around the file.  Keeps walking while the tooltip is up and you
   press '.' (next) or ',' (previous) -- so after the first Alt-Q . you step
   through every problem with a bare '.', without re-typing the Alt-Q prefix; any
   other key dismisses. */
static int e_lsp_ui_jump_diag(FENSTER *f, int dir)
{
 BUFFER *b = f->b;

 if (g_diag_nactive == 0)
 {  e_error("No problems reported in this file.", 0, f->fb);  return(0);  }
 for (;;)
 {
  const char *kind;
  int idx = e_lsp_diag_pick(b->b.y, b->b.x, dir);
  int c;
  if (idx < 0)
   break;
  b->b.y = g_diag_active[idx].line;
  b->b.x = g_diag_active[idx].c0;
  e_cursor(f, 0);                                /* clamp + scroll the view to it  */
  e_schirm(f, 1);
  e_refresh();
  if (!g_diag_active[idx].msg[0])
   break;                                        /* nothing to show -> stop here   */
  kind = (g_diag_active[idx].sev == 1) ? "Error"
       : (g_diag_active[idx].sev == 2) ? "Warning"
       : (g_diag_active[idx].sev == 3) ? "Info" : "Hint";
  c = e_lsp_cursor_popup(f, kind, g_diag_active[idx].msg,
                         ". next   , prev   any other key closes");
  if (c == AltQ)                                  /* re-pressed the Alt-Q prefix:    */
  {
   int k = wpe_paint_modal_getch();               /* take its letter                 */
   if (k == '.')  {  dir = +1;  continue;  }      /* '.'/',' keep walking the list   */
   if (k == ',')  {  dir = -1;  continue;  }
   return(e_lsp_ui_dispatch(f, k));               /* Alt-Q <action>: close + run it
                                                     (e.g. Alt-Q A to fix it here)   */
  }
  if (c == '.')
  {  dir = +1;  continue;  }                      /* bare '.' keeps stepping forward */
  if (c == ',')
  {  dir = -1;  continue;  }                      /* ...or backward                  */
  break;                                          /* any other key dismisses         */
 }
 return(0);
}

/* Alt-Q . / Alt-Q , -- jump to the next / previous problem in the file. */
static int e_lsp_ui_next_problem(FENSTER *f) { return(e_lsp_ui_jump_diag(f, +1)); }
static int e_lsp_ui_prev_problem(FENSTER *f) { return(e_lsp_ui_jump_diag(f, -1)); }

/* AltQ R -- list every reference to the symbol under the cursor in Messages. */
static int e_lsp_ui_references(FENSTER *f)
{
 BUFFER *b = f->b;
 e_lsp_ask_t *a;

 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_sync(f);
 a = e_lsp_ask_begin(f, E_LSP_ASK_REFERENCES);
 if (!a)
  return(-1);
 return(e_lsp_ask_sent(f, a, e_lsp_references_async(g_lsp, g_lsp_file, b->b.y,
                                                    e_lsp_symbol_col(f),
                                                    E_LSP_ROWS_MAX,
                                                    e_lsp_locs_told, a)));
}

/**
 * e_lsp_references_show - The references came: listed in Messages as
 * "file:line:col", or a box saying there are none.
 * @f: the window it was asked from; @a: the answer.
 */
static void e_lsp_references_show(FENSTER *f, const e_lsp_ask_t *a)
{
 char line[700];
 int i;

 if (a->n <= 0)
 {
  e_error("No references found.", 0, f->fb);
  return;
 }
 snprintf(line, sizeof(line), "%d reference(s):", a->n);
 e_d_p_message(line, f, 1);
 for (i = 0; i < a->n; i++)
 {
  snprintf(line, sizeof(line), "%s:%d:%d",
           a->rows[i].path ? e_d_dap_basename(a->rows[i].path) : "?",
           a->rows[i].line + 1, a->rows[i].character + 1);
  e_d_p_message(line, f, 1);
 }
}

/* The words of each hierarchy list: none found, and the heading, for each
   of the two directions (callers/callees, supertypes/subtypes). */
static const char *const e_lsp_hier_none[2][2] = {
 { "No callers found.",    "No outgoing calls found." },
 { "No supertypes found.", "No subtypes found." }
};
static const char *const e_lsp_hier_head[2][2] = {
 { "%d caller(s):",    "%d outgoing call(s):" },
 { "%d supertype(s):", "%d subtype(s):" }
};

/**
 * e_lsp_hierarchy_show - The callers, callees, supertypes or subtypes
 * came: listed in Messages as "name  file:line:col", or a box saying there
 * are none.
 * @f: the window it was asked from; @a: the answer.
 */
static void e_lsp_hierarchy_show(FENSTER *f, const e_lsp_ask_t *a)
{
 int types = (a->kind == E_LSP_ASK_TYPES), dir = a->flag ? 1 : 0;
 char line[700];
 int i;

 if (a->n <= 0)
 {
  e_error((char *)e_lsp_hier_none[types][dir], 0, f->fb);
  return;
 }
 snprintf(line, sizeof(line), e_lsp_hier_head[types][dir], a->n);
 e_d_p_message(line, f, 1);
 for (i = 0; i < a->n; i++)
 {
  snprintf(line, sizeof(line), "%s  %s:%d:%d",
           a->rows[i].name ? a->rows[i].name : "?",
           a->rows[i].path ? e_d_dap_basename(a->rows[i].path) : "?",
           a->rows[i].line + 1, a->rows[i].character + 1);
  e_d_p_message(line, f, 1);
 }
}

/* Call hierarchy for the symbol under the cursor, listed in Messages as
   "name  file:line:col" when the server answers: who calls it (outgoing ==
   0) or what it calls (outgoing != 0).  Lets the user trace a call graph
   without leaving xwpe.  Shared body for the two menu actions below. */
static int e_lsp_ui_call_hierarchy(FENSTER *f, int outgoing)
{
 BUFFER *b = f->b;
 e_lsp_ask_t *a;

 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_sync(f);
 a = e_lsp_ask_begin(f, E_LSP_ASK_CALLS);
 if (!a)
  return(-1);
 a->flag = outgoing;
 return(e_lsp_ask_sent(f, a, e_lsp_call_hierarchy_async(g_lsp, g_lsp_file,
                                                        b->b.y,
                                                        e_lsp_symbol_col(f),
                                                        outgoing,
                                                        E_LSP_ROWS_MAX,
                                                        e_lsp_syms_told, a)));
}

/* AltQ B -- callers (incoming calls): who calls the symbol under the cursor. */
static int e_lsp_ui_callers(FENSTER *f)  {  return(e_lsp_ui_call_hierarchy(f, 0));  }
/* AltQ G -- callees (outgoing calls): what the symbol under the cursor calls. */
static int e_lsp_ui_callees(FENSTER *f)  {  return(e_lsp_ui_call_hierarchy(f, 1));  }

/* Type hierarchy for the type under the cursor, listed in Messages as
   "name  file:line:col" when the server answers: its supertypes (subtypes
   == 0: what it extends/implements) or its subtypes (subtypes != 0: what
   extends it).  Shared body for the two menu actions below. */
static int e_lsp_ui_type_hierarchy(FENSTER *f, int subtypes)
{
 BUFFER *b = f->b;
 e_lsp_ask_t *a;

 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_sync(f);
 a = e_lsp_ask_begin(f, E_LSP_ASK_TYPES);
 if (!a)
  return(-1);
 a->flag = subtypes;
 return(e_lsp_ask_sent(f, a, e_lsp_type_hierarchy_async(g_lsp, g_lsp_file,
                                                        b->b.y,
                                                        e_lsp_symbol_col(f),
                                                        subtypes,
                                                        E_LSP_ROWS_MAX,
                                                        e_lsp_syms_told, a)));
}

/* AltQ K -- supertypes (vim K = up = parents): what this type extends. */
static int e_lsp_ui_supertypes(FENSTER *f) {  return(e_lsp_ui_type_hierarchy(f, 0));  }
/* AltQ J -- subtypes (vim J = down = children): what extends this type. */
static int e_lsp_ui_subtypes(FENSTER *f)   {  return(e_lsp_ui_type_hierarchy(f, 1));  }

/* Is position (al,ac) at or before (bl,bc) in document order? */
static int e_lsp_pos_le(int al, int ac, int bl, int bc)
{
 return(al < bl || (al == bl && ac <= bc));
}

/* Does range R fully enclose the selection [sl,sc .. el,ec]? */
static int e_lsp_range_covers(const e_lsp_range *r, int sl, int sc, int el, int ec)
{
 return(e_lsp_pos_le(r->start_line, r->start_char, sl, sc) &&
        e_lsp_pos_le(el, ec, r->end_line, r->end_char));
}

/* AltQ V -- expand selection: grow the block mark to the smallest syntactic
   range (from the server) that strictly encloses the current selection; with no
   selection, select the tightest range around the cursor.  Press again to keep
   widening (token -> expression -> argument list -> call -> statement -> block),
   a structural select rather than line-by-line.  The mark is set exactly as a
   mouse drag sets it (s->mark_begin/mark_end), so Copy/Cut act on it normally. */
static int e_lsp_ui_expand_selection(FENSTER *f)
{
 BUFFER *b = f->b;
 e_lsp_ask_t *a;

 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_sync(f);
 a = e_lsp_ask_begin(f, E_LSP_ASK_SELECTION);
 if (!a)
  return(-1);
 return(e_lsp_ask_sent(f, a, e_lsp_selection_range_async(g_lsp, g_lsp_file,
                                                         b->b.y, b->b.x,
                                                         E_LSP_RANGES_MAX,
                                                         e_lsp_ranges_told,
                                                         a)));
}

/**
 * e_lsp_range_is - Is range @r exactly the selection [sl,sc .. el,ec]?
 */
static int e_lsp_range_is(const e_lsp_range *r, int sl, int sc, int el, int ec)
{
 return(r->start_line == sl && r->start_char == sc &&
        r->end_line == el && r->end_char == ec);
}

/**
 * e_lsp_selection_pick - The range the mark grows to: with nothing marked
 * the tightest; else the first that strictly encloses the mark.
 * Return: its index in @a's rows, or -1 when the mark is already the
 * outermost range.
 */
static int e_lsp_selection_pick(const e_lsp_ask_t *a, SCHIRM *s)
{
 int sl = s->mark_begin.y, sc = s->mark_begin.x;
 int el = s->mark_end.y, ec = s->mark_end.x;
 int i;

 if (sl == el && sc == ec)
  return(0);
 for (i = 0; i < a->n; i++)
  if (e_lsp_range_covers(&a->rows[i].range, sl, sc, el, ec) &&
      !e_lsp_range_is(&a->rows[i].range, sl, sc, el, ec))
   return(i);
 return(-1);
}

/**
 * e_lsp_selection_show - The nested ranges came: the mark grows to the
 * next one out, as a mouse drag sets it, so Copy/Cut act on it.
 * @f: the window it was asked from; @a: the answer.
 */
static void e_lsp_selection_show(FENSTER *f, const e_lsp_ask_t *a)
{
 SCHIRM *s = f->s;
 const e_lsp_range *r;
 int chosen;

 if (a->n <= 0)
 {
  e_error("No selection range at the cursor.", 0, f->fb);
  return;
 }
 chosen = e_lsp_selection_pick(a, s);
 if (chosen < 0)
 {
  e_error("Selection already at the outermost range.", 0, f->fb);
  return;
 }
 r = &a->rows[chosen].range;
 s->mark_begin.y = r->start_line;
 s->mark_begin.x = r->start_char;
 s->mark_end.y = r->end_line;
 s->mark_end.x = r->end_char;
 e_schirm(f, 1);
 e_cursor(f, 0);
}

/* AltQ U -- highlight every occurrence of the symbol under the cursor in this
   file (the found-word colour, like a search highlight that follows meaning,
   not text), painted when the server answers.  Pressing it off any
   identifier clears the highlight. */
static int e_lsp_ui_highlight(FENSTER *f)
{
 BUFFER *b = f->b;
 e_lsp_ask_t *a;
 char word[256];

 e_lsp_word_at_cursor(f, word, sizeof(word));
 if (!word[0])                         /* not on an identifier: clear marks */
 {
  g_hl_nactive = 0;
  e_schirm(f, 1);
  e_cursor(f, 0);
  return(0);
 }
 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_sync(f);
 a = e_lsp_ask_begin(f, E_LSP_ASK_HIGHLIGHT);
 if (!a)
  return(-1);
 a->wlen = strlen(word);   /* every hit is the same symbol: one length */
 return(e_lsp_ask_sent(f, a, e_lsp_document_highlight_async(g_lsp, g_lsp_file,
                                                            b->b.y,
                                                            e_lsp_symbol_col(f),
                                                            E_LSP_HL_MAX,
                                                            e_lsp_locs_told,
                                                            a)));
}

/**
 * e_lsp_highlight_show - The occurrences came: each painted in the
 * found-word colour, or a box saying there are none.
 * @f: the window it was asked from; @a: the answer.
 */
static void e_lsp_highlight_show(FENSTER *f, const e_lsp_ask_t *a)
{
 int i;

 if (a->n <= 0)
 {
  e_error("No occurrences.", 0, f->fb);
  return;
 }
 g_hl_nactive = 0;
 for (i = 0; i < a->n && g_hl_nactive < LSP_MAX_DIAG; i++)
 {
  g_hl_active[g_hl_nactive].line = a->rows[i].line;
  g_hl_active[g_hl_nactive].c0 = a->rows[i].character;
  g_hl_active[g_hl_nactive].c1 = a->rows[i].character + a->wlen;
  g_hl_active[g_hl_nactive].sev = 0;
  g_hl_nactive++;
 }
 e_schirm(f, 1);
 e_cursor(f, 0);
}

/* AltQ Y -- toggle the end-of-line inlay-hint overlay (inferred types).  Turning
   it on starts/syncs the server and snapshots the file's hints; off clears them.
   If the server has nothing yet, it stays off and says so.  Off by default. */
static int e_lsp_ui_inlay(FENSTER *f)
{
 if (g_inlay_on)                        /* currently on -> turn off */
 {
  g_inlay_on = 0;
  e_lsp_inlay_clear();
  e_lsp_inlay_held_clear();
  e_d_p_message("Inlay hints: OFF.", f, 1);   /* sw=1: repaints Messages + editor */
  e_cursor(f, 0);
  return(0);
 }
 if (e_lsp_ensure(f) < 0)               /* turning on: start + sync + fetch */
  return(-1);
 e_lsp_sync(f);
 g_inlay_on = 1;
 /* The hints are painted when the server answers, and fetched again on its
    later news (workspace/inlayHint/refresh, the diagnostics published once
    it has indexed), so a cold server fills them in without the editor
    waiting. */
 e_lsp_inlay_fetch(f);
 e_lsp_fd_register();
 e_lsp_ui_trace("inlay toggle: fetch sent");
 e_d_p_message("Inlay hints: ON -- each shows as a grey pill at the end of its "
               "line when the server has one.  Alt-Q Y to hide.", f, 1);
 e_cursor(f, 0);
 return(0);
}

/* AltQ M -- toggle se(M)antic highlighting: re-colour the file by the server's
   token classification (its own palette, so type/function/parameter/... are
   distinct -- the thing regex highlighting cannot do).  Opt-in; off by default
   so the classic regex colours stay for anyone who prefers them. */
static int e_lsp_ui_semantic(FENSTER *f)
{
 if (g_sem_on)                          /* currently on -> turn off */
 {
  g_sem_on = 0;
  g_sem_nactive = 0;
  g_sem_held.ready = 0;
  e_d_p_message("Semantic highlighting: OFF.", f, 1);
  e_cursor(f, 0);
  return(0);
 }
 if (e_lsp_ensure(f) < 0)               /* turning on: start + sync + fetch */
  return(-1);
 e_lsp_sync(f);
 g_sem_on = 1;
 /* The file recolours when the server answers, and again on its later news
    (semanticTokens/refresh, the diagnostics published once it has indexed),
    so a cold server fills the colours in without the editor waiting. */
 e_lsp_sem_fetch(f);
 e_lsp_fd_register();
 e_d_p_message("Semantic highlighting: ON -- the file recolours by meaning as the "
               "server answers.  Alt-Q M to turn off.", f, 1);
 e_cursor(f, 0);
 return(0);
}

/* AltQ K -- code lenses: the run/test/reference annotations Metals attaches to
   definitions, listed in a popup as "<label>  (line N)"; selecting one jumps to
   that definition.  To actually run/test a Scala main, use the debugger
   (Ctrl-G T) which drives the same BSP/DAP path Metals' "run" lens would. */
static int e_lsp_ui_codelens(FENSTER *f)
{
 e_lsp_ask_t *a;

 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_sync(f);
 a = e_lsp_ask_begin(f, E_LSP_ASK_LENSES);
 if (!a)
  return(-1);
 return(e_lsp_ask_sent(f, a, e_lsp_code_lenses_async(g_lsp, g_lsp_file,
                                                     E_LSP_ROWS_MAX,
                                                     e_lsp_lenses_told, a)));
}

/* The longest row a lens or symbol picker builds. */
#define E_LSP_PICK_ROW_MAX 160

/**
 * e_lsp_lenses_show - The lenses came: a picker of "<label>  (line N)";
 * the chosen one's definition is jumped to.
 * @f: the window it was asked from; @a: the answer.
 */
static void e_lsp_lenses_show(FENSTER *f, const e_lsp_ask_t *a)
{
 static char rows[E_LSP_ROWS_MAX][E_LSP_PICK_ROW_MAX];
 const char *labels[E_LSP_ROWS_MAX];
 int i, sel;

 if (a->n <= 0)
 {
  e_error("No code lenses here.", 0, f->fb);
  return;
 }
 for (i = 0; i < a->n; i++)
 {
  snprintf(rows[i], sizeof(rows[i]), "%s  (line %d)",
           a->rows[i].name ? a->rows[i].name : "?", a->rows[i].line + 1);
  labels[i] = rows[i];
 }
 sel = e_lsp_pick(f, "Code lenses - Enter to go", labels, a->n);
 if (sel < 0)
  return;
 e_d_goto_break((char *)a->path, a->rows[sel].line + 1, f);
}

/* AltQ O -- the file outline: a picker of its symbols, when the server
   answers; the chosen one is jumped to. */
static int e_lsp_ui_outline(FENSTER *f)
{
 e_lsp_ask_t *a;

 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_sync(f);
 a = e_lsp_ask_begin(f, E_LSP_ASK_OUTLINE);
 if (!a)
  return(-1);
 return(e_lsp_ask_sent(f, a, e_lsp_document_symbols_async(g_lsp, g_lsp_file,
                                                          E_LSP_ROWS_MAX,
                                                          e_lsp_syms_told,
                                                          a)));
}

/**
 * e_lsp_outline_show - The file's symbols came: picked from a popup and
 * jumped to.
 * @f: the window it was asked from; @a: the answer.
 */
static void e_lsp_outline_show(FENSTER *f, const e_lsp_ask_t *a)
{
 const char *labels[E_LSP_ROWS_MAX];
 int i, sel;

 if (a->n <= 0)
 {
  e_error("No symbols.", 0, f->fb);
  return;
 }
 for (i = 0; i < a->n; i++)
  labels[i] = a->rows[i].name ? a->rows[i].name : "?";
 sel = e_lsp_pick(f, "File symbols - Enter to go", labels, a->n);
 if (sel < 0)
  return;
 e_d_goto_break((char *)a->path, a->rows[sel].line + 1, f);
}

/* A workspace/symbol "result" Metals returns that is not a real code symbol but
   a help/command hint (e.g. "Add ';' to search library dependencies"), which it
   points at a documentation page like workspace-symbol.md.  Those must not show
   up as jump targets -- jumping would open a Markdown help file, not code. */
static int e_lsp_symbol_is_help(const char *path)
{
 size_t len;

 if (!path)
  return(0);
 len = strlen(path);
 if (len >= 3 && strcmp(path + len - 3, ".md") == 0)
  return(1);
 return(0);
}

/* AltQ W -- workspace symbol search: prompt for a query, list matching symbols
   from across the whole project (each shown with its file) when the server
   answers, and jump to the one chosen (opening its file if it is not the
   current one).  IntelliJ's "Go to Symbol" / Search Everywhere. */
static int e_lsp_ui_workspace_symbols(FENSTER *f)
{
 char query[E_LSP_PICK_ROW_MAX];
 e_lsp_ask_t *a;

 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_word_at_cursor(f, query, sizeof(query));
 if (!e_add_arguments(query, "Search symbol", f, 0, AltS, NULL) || !query[0])
  return(0);                        /* cancelled or empty */
 e_lsp_sync(f);
 a = e_lsp_ask_begin(f, E_LSP_ASK_SYMBOLS);
 if (!a)
  return(-1);
 return(e_lsp_ask_sent(f, a, e_lsp_workspace_symbols_async(g_lsp, query,
                                                           E_LSP_ROWS_MAX,
                                                           e_lsp_syms_told,
                                                           a)));
}

/**
 * e_lsp_symbols_show - The matching symbols came: a picker of "name  -
 * file:line", the server's help hints left out; the chosen one is jumped
 * to, in its own file.
 * @f: the window it was asked from; @a: the answer.
 */
static void e_lsp_symbols_show(FENSTER *f, const e_lsp_ask_t *a)
{
 static char rows[E_LSP_ROWS_MAX][E_LSP_PICK_ROW_MAX];
 const char *labels[E_LSP_ROWS_MAX];
 int keep[E_LSP_ROWS_MAX];
 const char *jumpfile, *base;
 int i, m = 0, sel;

 for (i = 0; i < a->n; i++)
 {
  const e_lsp_ask_row *r = &a->rows[i];

  if (e_lsp_symbol_is_help(r->path))
   continue;                        /* skip Metals help/command hints */
  base = r->path ? e_d_dap_basename(r->path) : "(this file)";
  snprintf(rows[m], sizeof(rows[m]), "%s  -  %s:%d",
           r->name ? r->name : "?", base, r->line + 1);
  labels[m] = rows[m];
  keep[m] = i;
  m++;
 }
 if (m <= 0)
 {
  e_error("No matching symbols.", 0, f->fb);
  return;
 }
 sel = e_lsp_pick(f, "Workspace symbols - Enter to go", labels, m);
 if (sel < 0)
  return;
 sel = keep[sel];
 jumpfile = a->rows[sel].path ? a->rows[sel].path : a->path;
 e_d_goto_break((char *)jumpfile, a->rows[sel].line + 1, f);
}

/* AltQ A -- code actions / quick-fixes at the cursor (organize imports, import
   missing symbol, ...).  The list drops down when the server answers; the
   chosen action rewrites the buffer.  IntelliJ's Alt-Enter. */
static int e_lsp_ui_code_actions(FENSTER *f)
{
 e_lsp_ask_t *a;

 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_sync(f);
 a = e_lsp_ask_begin(f, E_LSP_ASK_ACTIONS);
 if (!a)
  return(-1);
 return(e_lsp_ask_sent(f, a, e_lsp_code_actions_async(g_lsp, g_lsp_file,
                                                      f->b->b.y, f->b->b.x,
                                                      E_LSP_ROWS_MAX,
                                                      e_lsp_actions_told, a)));
}

/**
 * e_lsp_action_run - Run the chosen action on the window's text: an edit
 * in hand is applied at once; a resolve or a command goes out, and its
 * change is applied when it comes, only to the text it was run on.
 * @f: the window; @index: the action; @has_edit: it carries its edit.
 */
static void e_lsp_action_run(FENSTER *f, int index, int has_edit)
{
 e_lsp_ask_t *a = e_lsp_ask_begin(f, E_LSP_ASK_ACTION_RUN);

 if (!a)
  return;
 a->flag = has_edit;
 e_lsp_ask_sent(f, a, e_lsp_code_action_run_async(g_lsp, index, g_lsp_file,
                                                  a->base, e_lsp_applied_told,
                                                  a));
}

/**
 * e_lsp_actions_show - The actions came: the list drops down at the cursor
 * (a quick-fix is about what is under it), and the chosen one runs.
 * @f: the window it was asked from; @a: the answer.
 */
static void e_lsp_actions_show(FENSTER *f, const e_lsp_ask_t *a)
{
 const char *labels[E_LSP_ROWS_MAX];
 int i, sel;

 if (a->n <= 0)
 {
  e_error("No code actions here.", 0, f->fb);
  return;
 }
 for (i = 0; i < a->n; i++)
  labels[i] = a->rows[i].name ? a->rows[i].name : "?";
 sel = e_lsp_pick_at(f, "Code actions - Enter to apply", labels, a->n,
                     e_lsp_cursor_screen_x(f), e_lsp_cursor_screen_y(f));
 if (sel < 0)
  return;
 e_lsp_action_run(f, sel, a->rows[sel].has_edit);
}

/* The note when an edit also changes files the editor does not apply. */
#define E_LSP_OTHERS_ACTION_FMT \
 "Note: this action also changes %d other file(s) -- not applied here."
#define E_LSP_OTHERS_RENAME_FMT \
 "Note: rename also affects %d other file(s) -- not applied (open them and " \
 "rename there)."

/**
 * e_lsp_edit_show - A rename's or a code action's change came, for the
 * text it was asked on (still the window's): the buffer takes it as one
 * undoable change, and a note says which other files it would touch.  A
 * command that changes no file (it only runs something) is not an error.
 * @f: the window it was asked from; @a: the answer.
 */
static void e_lsp_edit_show(FENSTER *f, const e_lsp_ask_t *a)
{
 int rename = (a->kind == E_LSP_ASK_RENAME);
 char m[160];

 if (!a->text && rename)
 {
  e_error("Rename failed (cannot rename here).", 0, f->fb);
  return;
 }
 if (!a->text && a->flag)
 {
  e_error("Could not apply that action.", 0, f->fb);
  return;
 }
 if (a->text && strcmp(a->text, a->base) != 0)
  e_lsp_replace_buffer(f, a->text);
 if (a->others <= 0)
  return;
 if (rename)
  snprintf(m, sizeof(m), E_LSP_OTHERS_RENAME_FMT, a->others);
 else
  snprintf(m, sizeof(m), E_LSP_OTHERS_ACTION_FMT, a->others);
 e_error(m, -1, f->fb);
}

/* Replace the whole editor buffer with `newtext` (a server-side reformat / rename
   / code-action result).  Records a single whole-buffer Undo entry first, so the
   refactor is reversible with Ctrl-U (and Redo-able) like any other edit, then
   swaps the content via the shared e_buffer_set_text. */
static void e_lsp_replace_buffer(FENSTER *f, const char *newtext)
{
 /* One undoable whole-buffer swap with the cursor kept in place -- the shared
    spine (we_edit.c), so a rename/format/code-action reverts with a single
    Ctrl-U and shows the edited spot immediately, not on the next keystroke. */
 e_replace_buffer_undoable(f, newtext);
 e_lsp_after_bulk_edit(f);          /* tell the server the buffer changed, so its
                                       diagnostics re-publish for the fixed text
                                       (e.g. the error a quick-fix removed clears) */
}

/* The identifier under the cursor (for the rename default), or "" . */
static void e_lsp_word_at_cursor(FENSTER *f, char *out, size_t osz)
{
 BUFFER *b = f->b;
 char *line = b->bf[b->b.y].s;
 int len = line ? strlen(line) : 0;
 int a = b->b.x, e = b->b.x, n;

 out[0] = '\0';
 if (!line)
  return;
 while (a > 0 && e_lsp_is_ident((unsigned char)line[a - 1]))
  a--;
 while (e < len && e_lsp_is_ident((unsigned char)line[e]))
  e++;
 n = e - a;
 if (n <= 0)
  return;
 if (n >= (int)osz)
  n = osz - 1;
 memcpy(out, line + a, n);
 out[n] = '\0';
}

/* AltQ F -- reformat the file (scalafmt via the server) in place. */
/* AltQ F -- format.  Context-sensitive, like IntelliJ/VS Code: if a block is
   marked, reformat JUST that range (textDocument/rangeFormatting); otherwise
   reformat the whole file.  Pairs with Alt-Q V (expand selection): widen to the
   method, then format only it. */
static int e_lsp_ui_format(FENSTER *f)
{
 SCHIRM *s = f->s;
 e_lsp_range r;
 e_lsp_ask_t *a;
 int rc;

 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_sync(f);
 a = e_lsp_ask_begin(f, E_LSP_ASK_FORMAT);
 if (!a)
  return(-1);
 r.start_line = s->mark_begin.y;  r.start_char = s->mark_begin.x;
 r.end_line = s->mark_end.y;      r.end_char = s->mark_end.x;
 a->has_sel = (r.start_line != r.end_line || r.start_char != r.end_char);
 if (a->has_sel)
  rc = e_lsp_format_async(g_lsp, g_lsp_file, a->base, &r,
                          e_lsp_format_told, a);
 else
  rc = e_lsp_format_async(g_lsp, g_lsp_file, a->base, NULL,
                          e_lsp_format_told, a);
 return(e_lsp_ask_sent(f, a, rc));
}

/**
 * e_lsp_format_show - The formatted text came: the buffer takes it as one
 * undoable change, or a line says it was already formatted.
 * @f: the window it was asked from (its text is still the text sent);
 * @a: the answer.
 */
static void e_lsp_format_show(FENSTER *f, const e_lsp_ask_t *a)
{
 const char *formatted = a->text;

 if (!formatted)
 {
  e_error("Nothing to format (or no formatter).", 0, f->fb);
  return;
 }
 if (strcmp(formatted, a->base) != 0)
 {
  e_lsp_replace_buffer(f, formatted);
  return;
 }
 if (a->has_sel)
  e_d_p_message("Block already formatted.", f, 1);
 else
  e_d_p_message("Already formatted.", f, 1);
}

/* AltQ N -- rename the symbol under the cursor across the workspace. */
static int e_lsp_ui_rename(FENSTER *f)
{
 BUFFER *b = f->b;
 char newname[256];
 e_lsp_ask_t *a;
 int dret;

 e_lsp_word_at_cursor(f, newname, sizeof(newname));
 e_lsp_ui_trace("e_lsp_ui_rename ENTER word='%s'", newname);
 dret = e_add_arguments(newname, "Rename to", f, 0, AltR, NULL);
 e_lsp_ui_trace("e_lsp_ui_rename dialog ret=%d newname='%s'", dret, newname);
 if (!dret || !newname[0])
  return(0);                         /* cancelled or empty */
 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_sync(f);
 a = e_lsp_ask_begin(f, E_LSP_ASK_RENAME);
 if (!a)
  return(-1);
 return(e_lsp_ask_sent(f, a, e_lsp_rename_async(g_lsp, g_lsp_file, b->b.y,
                                                e_lsp_symbol_col(f), newname,
                                                a->base, e_lsp_applied_told,
                                                a)));
}

/* Called from the editor loop after a keystroke.  Cheap: drains any diagnostics
   the server already sent (non-blocking) and, when a line is finished, pushes
   the buffer so the server recompiles -- live "as you type" diagnostics without
   blocking, and no save needed.  Only acts on the file the server is open for. */
void e_lsp_on_edit(FENSTER *f, int c)
{
 char path[1200];

 if (!g_lsp || !e_lsp_started(g_lsp))
  return;                            /* no session, or still starting (fd-loop runs it) */
 if (!f || !f->datnam)
  return;
 snprintf(path, sizeof(path), "%s%s", f->dirct ? f->dirct : "./", f->datnam);
 if (strcmp(path, g_lsp_file) != 0)
  return;
 g_lsp_quiet = 1;                   /* show only the summary, not each line */
 e_lsp_poll(g_lsp);
 g_lsp_quiet = 0;
 if (e_lsp_dead(g_lsp))             /* server exited while we were away: tear down */
 {  e_lsp_reap_if_dead(f);  return;  }
 if (c == WPE_CR)                   /* a line was completed -> recompile */
  e_lsp_sync(f);
 if (e_lsp_dead(g_lsp))             /* the server could not be given the change */
 {  e_lsp_reap_if_dead(f);  return;  }
 e_lsp_overlay_deliver();           /* an overlay answer this poll read */
 e_lsp_inlay_refresh_pending(f);    /* re-query inlays if the server flagged them */
 e_lsp_sem_refresh_pending(f);      /* re-query semantic tokens if flagged */
 /* An Alt-Q answer this poll read is shown at the key loop's top, once this
    key has been handled, so its positions are checked against the text the
    key made. */
}

/* A whole-buffer edit (undo / redo) landed from outside the per-keystroke path.
   Push it to the server unconditionally so diagnostics, inlays and semantic
   tokens re-publish for the restored text -- undo doesn't go through the Enter
   debounce, so without this the marks would lag the buffer (the symptom: undoing
   a code-action fix leaves the re-introduced error unmarked). */
void e_lsp_after_bulk_edit(FENSTER *f)
{
 if (!g_lsp || !e_lsp_started(g_lsp))
  return;
 e_lsp_on_edit(f, WPE_CR);          /* WPE_CR path = poll + sync + refresh inlays */
}

/* Disconnect the language server (called on editor exit). */
void e_lsp_ui_shutdown(void)
{
 e_lsp_fd_unregister();              /* remove the fd before freeing the session */
 e_lsp_session_drop();               /* ended through the owner, no wait */
 e_lsp_diag_clear();
 e_lsp_inlay_clear();
 e_lsp_overlay_held_drop();
 g_inlay_on = 0;
 g_sem_on = 0;  g_sem_nactive = 0;  g_sem_stale = 0;
 g_lsp_ready_done = 0;
 g_lsp_pending_ws = NULL;
 g_lsp_doctor_announced = 0;
 e_lsp_synced_set(NULL);
 free(g_lsp_doctor_last);
 g_lsp_doctor_last = NULL;
 free(g_lsp_status_last);
 g_lsp_status_last = NULL;
 g_lsp_busy = 0;
 g_lsp_file[0] = '\0';
 g_lsp_fenster = NULL;
}

/**
 * e_lsp_window_closing - A window is closing: the language server's news
 * no longer goes to it.
 * @f: the window, still whole.
 *
 * The server keeps running for the file and may still publish its
 * problems, readiness or status after the user closed the window it was
 * started from.  What it says then is not drawn into a window that is
 * gone: the problems wait for the file to be opened again.
 */
void e_lsp_window_closing(FENSTER *f)
{
 if (!f)
  return;
 if (g_lsp_fenster == f)
  g_lsp_fenster = NULL;
 if (g_lsp_pending_ws == f)
  g_lsp_pending_ws = NULL;
}

/* Human-readable name of the language server wired for f's file, for menu and
   dialog titles -- "Metals" for Scala today; as clangd / pyright / ... are added
   this adapts so the menu is always named after the server it actually drives.
   Returns NULL when the file's language has no server (so callers can hide the
   menu). */
const char *e_lsp_server_label(FENSTER *f)
{
 const char *lang = e_lsp_lang_for(f);
 const e_lsp_server *srv = e_lsp_server_for(lang);

 if (!srv)
  return(NULL);
 return(srv->label);      /* "Metals", "clangd", "pyright", ... -- the bar name */
}

extern WOPT eblst_lsp_o[], eblst_lsp_u[];   /* the LSP bottom bars (we_main.c) */
#ifdef WPE_AI
extern WOPT eblst_lspai_o[], eblst_lspai_u[]; /* combined LSP+AI bars (we_main.c) */
#endif

/* e_lsp_pack_bar - Re-lay-out a 7-button LSP bar after its label changed, so the
   buttons stay tight and the last one never runs off the screen edge.  A uniform
   2-column gap reproduces the original hand-tuned spacing for short server names
   ("Metals", "clangd", "gopls"); it drops to 1 column only when a long name
   ("rust-analyzer") would otherwise push the row past MAXSCOL.  The gap is chosen
   per bar variant, since the CUA ("_u") bar has wider base buttons than the
   default ("_o") one.  e_pack_button_bar updates both draw and click positions. */
static void e_lsp_pack_bar(WOPT *bar)
{
 int i, sum = 0, n = e_editor_bar_size(bar);
 for (i = 0; i < n; i++)
  sum += (int)strlen(bar[i].t);
 /* The buttons are drawn one column in, and the last one must END at or before
    MAXSCOL-2 to render whole (e_pr_uul stops at MAXSCOL-1, exclusive).  The last
    button's end column at gap g (x0=0) is sum + (n-1)*g, so use the wider
    2-column gap only when it still clears that limit; otherwise tighten to 1.
    The bars of the programming modes have one entry more (F9). */
 e_pack_button_bar(bar, n, 0, (sum + (n - 1) * 2 <= MAXSCOL - 2) ? 2 : 1);
}

/* e_lsp_bar_label - Keep the LSP bottom-bar hint naming the *active* server.
   The bar entry is a static "Alt-Q ? Metals"; this rewrites its text to
   "Alt-Q ? <server>" for the file being drawn ("Metals" for Scala, "clangd"
   for C/C++), so a user editing a .c sees the right server, not a hardcoded
   "Metals".  Called from the window-frame draw (we_wind.c) whenever the LSP bar
   is shown; the "Alt-Q ?" prefix (the highlighted part) is unchanged, so the
   entry's hot-key width stays correct. */
void e_lsp_bar_label(FENSTER *f)
{
 static char lbl[40];
 const char *srv = e_lsp_server_label(f);

 if (!srv)
  return;
 snprintf(lbl, sizeof(lbl), "Alt-Q ? %s", srv);
 eblst_lsp_o[5].t = lbl;          /* index 5 is the LSP menu hint in both bars */
 eblst_lsp_u[5].t = lbl;
 e_lsp_pack_bar(eblst_lsp_o);     /* re-fit the row to the (maybe longer) label */
 e_lsp_pack_bar(eblst_lsp_u);
#ifdef WPE_AI
 /* The combined LSP+AI bar names the server too, so a .c with AI on still reads
    "Alt-Q ? clangd" -- the LSP hint is at index 4 there (the AI hint follows). */
 eblst_lspai_o[4].t = lbl;
 eblst_lspai_u[4].t = lbl;
 e_lsp_pack_bar(eblst_lspai_o);
 e_lsp_pack_bar(eblst_lspai_u);
#endif
}

/* The "3" of the 3+1 start UX: when a language-server file is OPENED, boot the
   server in the background (async) so diagnostics/hover/navigation are ready by
   the time the user asks -- without the first Alt-Q having to wait through the
   cold start.  Strictly gated so it never costs anything on a box that is not
   set up for it: only fires when the file's language has a server, that server's
   binary is actually installed, and no session is running yet.  So opening a
   plain file, or a .scala on a box without Metals, spawns nothing and stays
   instant.  Called from e_edit once the window is built. */
/* A worksheet was opened while a session is already running for another file in
   the SAME workspace.  Metals is per-workspace, so it already serves the
   worksheet -- just re-point our single-document overlays at it and tell Metals
   to open + focus it, WITHOUT restarting the server, so its evaluation results
   appear.  (Opening a worksheet alongside the project file -- the natural flow --
   used to leave the session pointed at the first file, so the worksheet showed
   nothing.) */
static void e_lsp_focus_worksheet(FENSTER *f)
{
 char path[1200];
 char *text;

 if (!e_lsp_path_of(f, path, sizeof(path)) || !strcmp(path, g_lsp_file))
  return;                                 /* already the focused file */
 e_lsp_inlay_clear();
 e_lsp_diag_clear();
 g_lsp_fenster = f;
 strncpy(g_lsp_file, path, sizeof(g_lsp_file) - 1);
 g_lsp_file[sizeof(g_lsp_file) - 1] = '\0';
 text = e_lsp_buffer_text(f);
 e_lsp_did_open(g_lsp, g_lsp_file, text ? text : "");
 e_lsp_did_focus(g_lsp, g_lsp_file);
 e_lsp_synced_set(text ? text : strdup(""));
 g_inlay_on = 1;                          /* worksheet: auto-show the results */
 g_inlay_stale = 1;
 e_lsp_fd_register();                     /* stream the evaluation results in */
}

void e_lsp_open_eager(FENSTER *f)
{
 const char *lang = e_lsp_lang_for(f);
 const e_lsp_server *srv = e_lsp_server_for(lang);

 if (getenv("XWPE_LSP_NO_EAGER"))         /* opt out (slow box, or a test harness) */
  return;
 if (!srv)                                /* not an LSP-backed file type */
  return;
 if (e_lsp_is_dep_source(f))              /* a read-only .metals/readonly source */
  return;                                 /* never boot a server rooted in the cache */
 if (g_lsp)
 {
  /* a session already runs: focus a worksheet opened in the same workspace so
     it evaluates, but otherwise leave the running session alone (a different
     file is re-pointed on demand by the first Alt-Q action). */
  if (e_lsp_is_worksheet(f) && g_lsp_fenster && f->dirct &&
      g_lsp_fenster->dirct && !strcmp(f->dirct, g_lsp_fenster->dirct))
  {
   if (e_lsp_started(g_lsp))
    e_lsp_focus_worksheet(f);             /* ready now -> focus it immediately */
   else
    g_lsp_pending_ws = f;                 /* still booting -> focus it once ready */
  }
  return;
 }
 if (e_test_command((char *)srv->argv[0]))  /* server not installed -> stay light */
  return;
 g_lsp_eager_start = 1;                   /* nobody waits for it: no waiting line */
 e_lsp_ensure(f);                         /* async: spawns + returns, ready later */
 g_lsp_eager_start = 0;
}

/* One row of the discoverable LSP action menu: the label shown in the popup
   (with its Alt-Q letter as a cue) and the handler it runs. */
/* The top-menu dropdown engine (we_menue.c): draws a bordered, arrow-navigable
   list and runs the chosen item's function -- the same widget the File/Edit menus
   use, so the language-server menu looks and behaves identically. */
extern int WpeHandleSubmenuFromPrefix(int xa, int ya, int xe, int ye, int prefix,
                                      OPTK *fopt, FENSTER *f);
extern OPTK WpeFillSubmenuItem(char *t, int x, char o, int (*fkt)());

/* Inner text width of the dropdown rows: the action name is left-justified and
   its "Alt-Q X" accelerator right-justified within this, exactly like the top
   menus right-align "F2" / "Shift Del / ^X". */
#define LSP_MENU_TEXTW 27

/* Persistent storage for the formatted "name        Alt-Q X" labels (OPTK keeps
   a pointer, so the strings must outlive the call). */
static char g_lsp_menu_label[26][48];

/* Build the language-server actions as top-menu-style dropdown rows: the action
   name on the left, its "Alt-Q X" shortcut right-aligned, and X (the key you
   press to run it -- in the menu or via the Alt-Q prefix) highlighted.  Reads
   and behaves exactly like a File/Edit submenu.  Returns the item count. */
static int e_lsp_menu_items(OPTK *it)
{
 static const struct { const char *name; char key; int (*fkt)(FENSTER *); } a[] = {
  { "Diagnostics",          'E', e_lsp_ui_diagnostics       },
  { "Go to Definition",     'D', e_lsp_ui_definition        },
  { "Go to Implementation", 'I', e_lsp_ui_implementation    },
  { "Go to Type",           'T', e_lsp_ui_type_definition   },
  { "Hover (type/docs)",    'H', e_lsp_ui_hover             },
  { "Complete",             'C', e_lsp_ui_complete          },
  { "References",           'R', e_lsp_ui_references        },
  { "Incoming calls",       'B', e_lsp_ui_callers           },
  { "Outgoing calls",       'G', e_lsp_ui_callees           },
  { "Supertypes",           'K', e_lsp_ui_supertypes        },
  { "Subtypes",             'J', e_lsp_ui_subtypes          },
  { "Expand selection",     'V', e_lsp_ui_expand_selection  },
  { "Highlight uses",       'U', e_lsp_ui_highlight         },
  { "Inlay hints (toggle)", 'Y', e_lsp_ui_inlay             },
  { "Semantic colours",     'M', e_lsp_ui_semantic          },
  { "Outline",              'O', e_lsp_ui_outline           },
  { "Code lenses",          'L', e_lsp_ui_codelens          },
  { "Workspace symbols",    'W', e_lsp_ui_workspace_symbols },
  { "Code actions",         'A', e_lsp_ui_code_actions      },
  { "Signature help",       'S', e_lsp_ui_signature         },
  { "Rename",               'N', e_lsp_ui_rename            },
  { "Format",               'F', e_lsp_ui_format            },
  { "Next problem",         '.', e_lsp_ui_next_problem      },
  { "Prev problem",         ',', e_lsp_ui_prev_problem      }
 };
 int i, n = (int)(sizeof(a) / sizeof(a[0]));

 for (i = 0; i < n; i++)
 {
  char code[12];
  int pad, hl;
  snprintf(code, sizeof(code), "Alt-Q %c", a[i].key);          /* 7 chars */
  pad = LSP_MENU_TEXTW - (int)strlen(a[i].name) - (int)strlen(code);
  if (pad < 1)
   pad = 1;
  snprintf(g_lsp_menu_label[i], sizeof(g_lsp_menu_label[i]),
           "%s%*s%s", a[i].name, pad, "", code);
  hl = (int)strlen(g_lsp_menu_label[i]) - 1;     /* the X in the right "Alt-Q X" */
  it[i] = WpeFillSubmenuItem(g_lsp_menu_label[i], hl, a[i].key, a[i].fkt);
 }
 return(n);
}

/* Column of the active window's bottom-bar "Metals" entry, so the dropdown opens
   under it; -1 if not found. */
static int e_lsp_bar_entry_x(FENSTER *f)
{
 int i;

 if (!f->blst)
  return(-1);
 for (i = 0; i < f->nblst; i++)
  if (f->blst[i].as == WPE_LSP_MENU)
   return(f->blst[i].x);
 return(-1);
}

/* Open the language-server menu as a Borland-style dropdown that unfolds UPWARD
   from the bottom-bar "Metals" entry (the bar sits at the screen's foot, so the
   list grows up like a pull-up), anchored UNDER that entry's column (not the
   screen edge -- which mis-placed it on wide terminals).  Same frame, colours and
   keys as the top File/Edit menus -- WpeHandleSubmenu runs the modal
   arrow/letter/click loop and calls the chosen action.  Surfaced for mouse-first
   users; Alt-Q+letter stays the keyboard fast path. */
int e_lsp_ui_menu(FENSTER *f)
{
 OPTK items[26];
 int n, xa, xe, ya, ye, w, mx, verdict;

 if (!e_lsp_server_label(f))
 {  e_error("Language server: unsupported file type.", 0, f->fb);  return(0);  }
 n = e_lsp_menu_items(items);
 w = LSP_MENU_TEXTW + 5;                  /* box width incl. frame + margins   */
 mx = e_lsp_bar_entry_x(f);               /* anchor under the "Metals" entry    */
 xa = (mx >= 0) ? mx - 1 : MAXSCOL - 1 - w;
 if (xa + w > MAXSCOL - 1)                /* keep it on screen                  */
  xa = MAXSCOL - 1 - w;
 if (xa < 1)
  xa = 1;
 xe = xa + w;
 ye = MAXSLNS - 2;                        /* bottom edge just above the bar...   */
 ya = ye - (n + 1);                       /* ...so the list opens upward         */
 if (ya < 1)
  ya = 1;
 /* The dropdown hangs from no menu-bar entry and its rows advertise
    "Alt-Q <letter>", a prefix and a letter: the engine is told so, which keeps
    a second Alt-Q (or the Alt form of a row letter) from running a row and
    leaves Left and Right without effect.  A bar menu's own Alt letter still
    comes back as that menu's index, and that menu then opens. */
 verdict = WpeHandleSubmenuFromPrefix(xa, ya, xe, ye, AltQ, items, f);
 if (verdict >= 0 && verdict < MENOPT)
  WpeHandleMainmenu(verdict, f);
 return(0);
}

/* Run the Alt-Q action selected by `c` (the key pressed after the prefix).  Split
   out of e_lsp_ui_key so a dismissable tooltip can re-dispatch: while a hover /
   signature / problem tooltip is up, pressing Alt-Q <letter> closes it and runs
   that action in one motion, instead of just eating the keystroke.  '?' / F1 /
   anything unknown opens the action menu; ESC cancels. */
static int e_lsp_ui_dispatch(FENSTER *f, int c)
{
 switch (c)
 {
  case 'd': case ('d' - 'a' + 1):
   return(e_lsp_ui_definition(f));
  case 'i': case ('i' - 'a' + 1):
   return(e_lsp_ui_implementation(f));
  case 't': case ('t' - 'a' + 1):
   return(e_lsp_ui_type_definition(f));
  case 'h': case ('h' - 'a' + 1):
   return(e_lsp_ui_hover(f));
  case 'c': case ('c' - 'a' + 1):
   return(e_lsp_ui_complete(f));
  case 'r': case ('r' - 'a' + 1):
   return(e_lsp_ui_references(f));
  case 'b': case ('b' - 'a' + 1):
   return(e_lsp_ui_callers(f));        /* B = called By (incoming) */
  case 'g': case ('g' - 'a' + 1):
   return(e_lsp_ui_callees(f));        /* G = Goes to (outgoing)   */
  case 'k': case ('k' - 'a' + 1):
   return(e_lsp_ui_supertypes(f));     /* K = up (vim) = supertypes */
  case 'j': case ('j' - 'a' + 1):
   return(e_lsp_ui_subtypes(f));       /* J = down (vim) = subtypes */
  case 'v': case ('v' - 'a' + 1):
   return(e_lsp_ui_expand_selection(f)); /* V = expand (Visual) selection */
  case 'u': case ('u' - 'a' + 1):
   return(e_lsp_ui_highlight(f));      /* U = Uses */
  case 'y': case ('y' - 'a' + 1):
   return(e_lsp_ui_inlay(f));          /* Y = inlaY hints */
  case 'm': case ('m' - 'a' + 1):
   return(e_lsp_ui_semantic(f));       /* M = se(M)antic colours */
  case 'l': case ('l' - 'a' + 1):
   return(e_lsp_ui_codelens(f));       /* L = Lens */
  case 's': case ('s' - 'a' + 1):
   return(e_lsp_ui_signature(f));
  case 'w': case ('w' - 'a' + 1):
   return(e_lsp_ui_workspace_symbols(f));
  case 'a': case ('a' - 'a' + 1):
   return(e_lsp_ui_code_actions(f));
  case 'f': case ('f' - 'a' + 1):
   return(e_lsp_ui_format(f));
  case 'n': case ('n' - 'a' + 1):
   return(e_lsp_ui_rename(f));
  case 'o': case ('o' - 'a' + 1):
   return(e_lsp_ui_outline(f));
  case 'e': case ('e' - 'a' + 1):
   return(e_lsp_ui_diagnostics(f));
  case '.':
   return(e_lsp_ui_next_problem(f)); /* . = next problem (like F8)          */
  case ',':
   return(e_lsp_ui_prev_problem(f)); /* , = previous problem                */
  case WPE_ESC:
   return(0);                        /* cancel -- no action, no menu        */
  default:
   return(e_lsp_ui_menu(f));         /* '?', F1, anything else -> the menu  */
 }
}

/* Alt-Q: the language-server command prefix, in the Borland spirit of Ctrl-K for
   blocks -- the menu (Alt-B / a click) is the discoverable path, the prefix is
   the silent fast one.  Read the action key and dispatch it. */
int e_lsp_ui_key(FENSTER *f)
{
 int c = wpe_paint_modal_getch();

 e_lsp_ui_trace("e_lsp_ui_key c=%d '%c'", c, (c >= 32 && c < 127) ? c : '?');
 return(e_lsp_ui_dispatch(f, c));
}

#endif

