/* we_debug.c                                             */
/* Copyright (C) 1993 Fred Kruse                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

#include "messages.h"
#include "edit.h"
#include "we_fdloop.h"
#include "we_dap.h"
#include "we_bsp.h"
#include "we_lsp.h"

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
#include <pty.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <glob.h>

/* jdb protocol tracing now routes through the unified WPE_TRACE facility
   (build with ./configure --enable-trace); records are line-tagged "[jdb]".
   The jdb_trace() spelling is kept so existing call sites are unchanged. */
#include "we_trace.h"
#define jdb_trace(...) WPE_TRACE("jdb", __VA_ARGS__)

#ifndef TERMCAP
/* Because term.h defines "buttons" it messes up we_gpm.c if in edit.h */
#include <term.h>
#endif

#define D_CBREAK -2
#define CTRLC CtrlC
#define SVLINES 12

int e_d_delbreak(FENSTER *f);
int e_d_error(char *s);
int print_to_end_of_buffer(BUFFER * b,char * str,int wrap_limit);


#define MAXOUT  2 * MAXSCOL * MAXSLNS

char *e_debugger, *e_deb_swtch = NULL, *e_d_save_schirm;
int e_d_swtch = 0, rfildes[2], ofildes, e_d_pid = 0;

/* Async debugger state for event-driven step/run.
   When e_d_async_pending is set, the debugger is waiting for gdb
   to respond.  The main event loop processes keyboard and pty
   events normally.  When gdb responds, the callback processes
   the response and clears the flag. */
int e_d_async_pending = 0;
static int e_d_async_main_brk = 0;
static FENSTER *e_d_async_f = NULL;
int e_d_nbrpts = 0, e_d_zwtchs = 0, *e_d_ybrpts, *e_d_nrbrpts;

/* number of watch expressions in Watches window */
int e_d_nwtchs = 0; 

/* e_d_nrwtchs[i] is the y coordinate (count starts at 0) of the first line of
   the i-th watch in the Watches window */
int *e_d_nrwtchs; 

char **e_d_swtchs; /* e_d_swtchs[i] is the i-th watch expression (a char*) */

int e_d_nstack;
char e_d_file[128], **e_d_sbrpts;
char *e_d_out = NULL;
/* Which debugger backend e_deb_type selects (see e_run_debug's dispatch).
   The numeric values are persisted in the options file, so keep them stable. */
enum {
 DEB_GDB = 0,   /* GNU gdb (default)        prompt "(gdb)" */
 DEB_SDB = 1,   /* classic sdb              prompt "*"     */
 DEB_DBX = 2,   /* dbx -i                   prompt "(dbx)" */
 DEB_XDB = 3,   /* HP xdb -L                prompt ">"     */
 DEB_JDB = 4,   /* Java jdb                                */
 DEB_PDB = 5,   /* Python python3 -m pdb    prompt "(Pdb)" */
 DEB_A68G = 6,  /* Algol 68 Genie a68g --monitor  prompt "(a68g)" */
 DEB_DAP = 7    /* Debug Adapter Protocol client (Go/dlv, Rust/lldb-dap, ...) */
};

int e_deb_type = 0, e_deb_mode = 0;
/* Which DAP adapter to prefer for languages that offer a choice (Rust: gdb vs
   lldb-dap).  Persisted in xwperc ("DAPAdapter : N"), set via the Debug ->
   debugger Options dialog, and overridable at run time by $XWPE_DAP_ADAPTER.
   0 = auto-detect (first in PATH), 1 = prefer gdb, 2 = prefer lldb-dap. */
enum { DAP_ADAPTER_AUTO = 0, DAP_ADAPTER_GDB = 1, DAP_ADAPTER_LLDB = 2 };
int e_dap_adapter = DAP_ADAPTER_AUTO;
char e_d_out_str[SVLINES][256];
char *e_d_sp[SVLINES];
char e_d_tty[80];

/* Pty for capturing debugged program's output */
int e_d_pty_master = -1;
int e_d_pty_slave = -1;
char e_d_pty_slave_name[128];
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
static int  e_d_a68g_step_complete(FENSTER *f);   /* a68g: read step/continue response */
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

/* Grace window (ms) e_d_pty_drain waits for more program output after the last
   byte before concluding the program has stopped writing.  This replaces a
   blind 100ms usleep: output already in the pty is drained immediately, and we
   only ever wait this long when the pty has briefly gone quiet. */
#define DEB_PTY_DRAIN_MS 100

/* Per-step async read of jdb/pdb output: scratch buffer size, and how long to
   wait for the next chunk before deciding the step has finished printing. */
#define DEB_STEP_BUF     4096
#define DEB_STEP_POLL_MS 5000

/* Safety bound on the stderr-drain loop at the top of e_d_line_read: a debugger
   that floods its error stream (e.g. a68g's abend->io_write_string recursion
   when its output pipe breaks) must never hold xwpe in an unbounded read.  A
   normal debug step emits at most a handful of stderr lines; anything past this
   is a runaway, so we stop draining and let the loop re-enter on the next call
   (the project convention: never leave xwpe in a busy loop). */
#define E_D_DRAIN_MAX    4096

/* Settle time (ms) for the legacy external-xterm debugger before xwpe reclaims
   the input focus -- see the comment at its use in e_exec_deb. */
#define DEB_XTERM_SETTLE_MS 200

/* Drain any available data from the pty master into e_d_prog_output.
   Polls so it never blocks the editor, and keeps draining until the program
   stops writing for DEB_PTY_DRAIN_MS or closes its end of the pty (POLLHUP). */
void e_d_pty_drain(void)
{
 char buf[4096];
 int n, flags;
 struct pollfd pfd;

 if (e_d_pty_master < 0)
  return;
 flags = fcntl(e_d_pty_master, F_GETFL, 0);
 fcntl(e_d_pty_master, F_SETFL, flags | O_NONBLOCK);
 for (;;)
 {
  pfd.fd = e_d_pty_master;
  pfd.events = POLLIN;
  if (poll(&pfd, 1, DEB_PTY_DRAIN_MS) <= 0)
   break;                        /* idle for the grace window, or poll error */
  if (pfd.revents & POLLIN)
  {
   n = read(e_d_pty_master, buf, sizeof(buf));
   if (n > 0)
   {
    e_d_prog_output_append(buf, n);
    continue;
   }
  }
  break;                         /* POLLHUP/POLLERR, or EOF with no data left */
 }
 fcntl(e_d_pty_master, F_SETFL, flags & ~O_NONBLOCK);
}

/* Send a NUL-terminated command to the debugger's stdin pipe (rfildes[1]).
   Centralises what used to be open-coded write(rfildes[1], s, strlen(s)) calls
   -- and, worse, hand-counted lengths that could be wrong (the old jdb
   delete-all sent write(..., "db *\n", 2), i.e. just "db").  Using strlen here
   removes that whole class of miscount, and the loop tolerates short writes
   and EINTR. */
/* Runaway guard: how many debugger-output lines e_d_line_read has returned
   since the last command we sent.  A single command's reply is bounded (a
   stack, a value, a stopped-at line); a debugger that streams far more than any
   real reply -- e.g. a68g stuck in its abend->io_write_string recursion -- is a
   runaway, and the read loops would otherwise spin on it forever.  Reset on
   every e_d_send_cmd (a fresh reply is expected) and checked in e_d_line_read. */
static long e_d_read_runaway = 0;
#define E_D_RUNAWAY_MAX  100000L

static void e_d_send_cmd(const char *cmd)
{
 size_t len = strlen(cmd);
 size_t off = 0;

 e_d_read_runaway = 0;
 while (off < len)
 {
  ssize_t w = write(rfildes[1], cmd + off, len - off);
  if (w < 0)
  {
   if (errno == EINTR)
    continue;
   break;   /* pipe broken: the debugger is gone, nothing to do here */
  }
  off += (size_t)w;
 }
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
 char nb_buf[512];          /* e_d_line_read_nb: partial gdb line */
 int  nb_len;
 char pty_line_buf[1024];   /* pty -> Messages: line being assembled */
 int  pty_line_len;
 int  pty_owns_last_line;   /* pty output left the current line unterminated */
} e_d_async;

static void e_d_async_reset(void)
{
 e_d_async.nb_len = 0;
 e_d_async.pty_line_len = 0;
 e_d_async.pty_owns_last_line = 0;
}

static int e_d_check_prompt(signed char *s, int i)
{
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

int e_d_line_read_nb(int n, signed char *s, int max)
{
 int flags, nread, i;

 while (e_e_line_read(efildes[0], s, max) >= 0)
  ;
 flags = fcntl(n, F_GETFL, 0);
 fcntl(n, F_SETFL, flags | O_NONBLOCK);
 nread = read(n, e_d_async.nb_buf + e_d_async.nb_len, sizeof(e_d_async.nb_buf) - e_d_async.nb_len - 1);
 fcntl(n, F_SETFL, flags & ~O_NONBLOCK);
 if (nread <= 0 && e_d_async.nb_len == 0)
  return (nread == 0) ? -1 : -2;
 if (nread > 0)
  e_d_async.nb_len += nread;
 e_d_async.nb_buf[e_d_async.nb_len] = '\0';
 for (i = 0; i < e_d_async.nb_len; i++)
 {
  if (e_d_async.nb_buf[i] == '\n' || e_d_async.nb_buf[i] == '\0')
  {
   int len = (i < max - 1) ? i : max - 2;
   memcpy(s, e_d_async.nb_buf, len);
   s[len] = e_d_async.nb_buf[i];
   s[len + 1] = '\0';
   e_d_async.nb_len -= (i + 1);
   if (e_d_async.nb_len > 0)
    memmove(e_d_async.nb_buf, e_d_async.nb_buf + i + 1, e_d_async.nb_len);
   return e_d_check_prompt(s, len) ? 1 : 2;
  }
 }
 if (e_d_check_prompt((signed char *)e_d_async.nb_buf, e_d_async.nb_len - 1))
 {
  int len = (e_d_async.nb_len < max - 1) ? e_d_async.nb_len : max - 2;
  memcpy(s, e_d_async.nb_buf, len);
  s[len] = '\0';
  e_d_async.nb_len = 0;
  return 1;
 }
 return -2;
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

static int e_d_pty_read_to_messages(FENSTER *mf)
{
 char buf[256];
 int n, total = 0, i, flags;

 if (e_d_pty_master < 0)
  return 0;
 flags = fcntl(e_d_pty_master, F_GETFL, 0);
 fcntl(e_d_pty_master, F_SETFL, flags | O_NONBLOCK);
 while ((n = read(e_d_pty_master, buf, sizeof(buf))) > 0)
 {
  e_d_pty_ensure_own_line(mf);
  e_d_prog_output_append(buf, n);
  for (i = 0; i < n; i++)
  {
   if (buf[i] == '\n')
    e_d_async.pty_owns_last_line = 0;
   e_d_messages_append_char(mf, (unsigned char)buf[i]);
  }
  total += n;
 }
 fcntl(e_d_pty_master, F_SETFL, flags & ~O_NONBLOCK);
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

static void e_d_wait_for_input(int gdb_fd)
{
 wpe_fd_add(gdb_fd, POLLIN, NULL, NULL);
 if (e_d_pty_master >= 0)
  wpe_fd_add(e_d_pty_master, POLLIN, NULL, NULL);
 wpe_fd_poll(WPE_FD_POLL_MS);
 wpe_fd_del(gdb_fd);
 if (e_d_pty_master >= 0)
  wpe_fd_del(e_d_pty_master);
}

static void e_d_on_gdb_readable(int fd, void *data);
static void e_d_on_pty_readable(int fd, void *data);

/* DAP debug bridge (Go/dlv and other Debug Adapter Protocol backends).  The
   editor-facing glue lives here, next to the other backend helpers, because it
   needs the file-scope debugger globals (e_d_swtch, e_d_file, the breakpoint and
   watch arrays).  The protocol engine itself is in we_dap.c / we_dap_proto.c,
   free of editor state, so it can be unit-tested against a real adapter.
   Forward-declared here because e_d_quit and e_d_p_watches (defined above) hook
   into them. */
static int  e_d_dap_active(void);
static int  e_d_dap_source_ext(const char *datnam);
static int  e_d_dap_start(FENSTER *f);
static int  e_d_dap_run(FENSTER *f);
static int  e_d_dap_step(FENSTER *f, int sw);
static int  e_d_dap_watches(FENSTER *f, int sw);
static void e_d_dap_quit(FENSTER *f);

typedef struct {
 char buf[SVLINES][256];
 char *sp[SVLINES];
 int active;
 int main_brk;
 struct FNST *f;
} e_d_accum_t;

static e_d_accum_t e_d_accum;

static void e_d_flush_inferior_stdout(void)
{
 char resp[256];

 if (e_deb_type != DEB_GDB || e_d_pty_master < 0)
  return;
 e_d_send_cmd("call (void)fflush(0)\n");
 while (e_d_line_read(wfildes[0], resp, 256, 0, 0) == 0)
  ;
}

static void e_d_accum_complete(void)
{
 FENSTER *f = e_d_accum.f;
 int i;

 e_d_flush_inferior_stdout();
 e_d_drain_pty_to_messages();
 wpe_fd_del(wfildes[0]);
 if (e_d_pty_master >= 0)
  wpe_fd_del(e_d_pty_master);
 e_d_accum.active = 0;
 e_d_async_pending = 0;

 for (i = 0; i < SVLINES; i++)
  e_d_sp[i] = e_d_accum.sp[i];

 for (i = 0; i < SVLINES; i++)
 {
  if (strstr(e_d_sp[i], "exited"))
  {
   e_d_p_message("Program exited.", f, 0);
   e_d_quit(f);
   return;
  }
 }
 { int _i;
   _i = e_d_fst_check(f);
   if (_i < 0) _i = e_d_snd_check(f);
   if (_i < 0) _i = e_d_trd_check(f);
   if (_i < 0)
   {
    e_d_switch_out(0);
    e_error("Program exited. Debugger stopped.", 0, f->fb);
    e_d_quit(f);
    return;
   }
 }
 e_d_accum.f = NULL;
 _messages_activated = 0;
 e_refresh();
}

static void e_d_accum_line(char *line, int ret)
{
 char *spt;
 int i;

 if (!e_d_accum.active)
  return;
 if (ret < 0)
 {
  e_d_accum.active = 0;
  e_d_async_pending = 0;
  wpe_fd_del(wfildes[0]);
  if (e_d_pty_master >= 0)
   wpe_fd_del(e_d_pty_master);
  e_d_quit(e_d_accum.f);
  return;
 }
 if (ret == 1)
 {
  e_d_accum_complete();
  return;
 }
 if (!line[0])
  return;
 spt = e_d_accum.sp[0];
 for (i = 1; i < SVLINES; i++)
  e_d_accum.sp[i-1] = e_d_accum.sp[i];
 e_d_accum.sp[SVLINES-1] = spt;
 strncpy(spt, line, 255);
 spt[255] = '\0';
}

static void e_d_accum_init(FENSTER *f, int main_brk)
{
 int i;

 for (i = 0; i < SVLINES; i++)
 {
  e_d_accum.sp[i] = e_d_accum.buf[i];
  e_d_accum.buf[i][0] = '\0';
 }
 e_d_accum.active = 1;
 e_d_accum.main_brk = main_brk;
 e_d_accum.f = f;
 e_d_async_pending = 1;
 e_d_async_reset();
 wpe_fd_add(wfildes[0], POLLIN, e_d_on_gdb_readable, f);
 if (e_d_pty_master >= 0)
  wpe_fd_add(e_d_pty_master, POLLIN, e_d_on_pty_readable, f);
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

static void e_d_on_pty_readable(int fd, void *data)
{
 FENSTER *mf = e_d_find_messages_window();
 int n;

 if (!mf) return;
 n = e_d_pty_read_to_messages(mf);
 if (n > 0)
 {
  e_d_messages_redraw(mf);
  e_d_messages_place_cursor(mf);
 }
}

static void e_d_on_gdb_readable(int fd, void *data)
{
 signed char buf[256];
 int ret;

 if (!e_d_accum.active)
  return;
 while (e_d_accum.active)
 {
  ret = e_d_line_read_nb(wfildes[0], buf, 256);
  if (ret == -2)
   return;
  e_d_accum_line((char *)buf, ret);
 }
}

static void e_d_pty_drain_nonblock(void)
{
 char buf[256];
 int n, flags;

 if (e_d_pty_master < 0)
  return;
 flags = fcntl(e_d_pty_master, F_GETFL, 0);
 fcntl(e_d_pty_master, F_SETFL, flags | O_NONBLOCK);
 while ((n = read(e_d_pty_master, buf, sizeof(buf))) > 0)
  e_d_prog_output_append(buf, n);
 fcntl(e_d_pty_master, F_SETFL, flags & ~O_NONBLOCK);
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
 int n, flags;

 if (e_d_pty_master < 0)
  return;
 flags = fcntl(e_d_pty_master, F_GETFL, 0);
 fcntl(e_d_pty_master, F_SETFL, flags | O_NONBLOCK);
 while ((n = read(e_d_pty_master, buf, sizeof(buf))) > 0)
 {
  int space = sizeof(e_d_async.pty_line_buf) - e_d_async.pty_line_len - 1;
  if (n > space) n = space;
  memcpy(e_d_async.pty_line_buf + e_d_async.pty_line_len, buf, n);
  e_d_async.pty_line_len += n;
  e_d_pty_flush_line(f, 0);
 }
 fcntl(e_d_pty_master, F_SETFL, flags & ~O_NONBLOCK);
}

static void e_d_drain_pty_to_messages(void)
{
 FENSTER *mf;

 if (e_d_pty_master < 0)
  return;
 mf = e_d_find_messages_window();
 if (!mf)
  return;
 if (e_d_pty_read_to_messages(mf) > 0)
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
  close(e_d_pty_master);
  e_d_pty_master = -1;
 }
 if (e_d_pty_slave >= 0)
 {
  close(e_d_pty_slave);
  e_d_pty_slave = -1;
 }
 e_d_pty_slave_name[0] = '\0';
 /* Buffer and its length are intentionally kept so Ctrl-G P can still show the
    captured output; e_exec_deb frees it when the next session starts.  We do
    NOT zero the capacity here: the allocation is still live, so cap must keep
    reflecting it (cap >= len) -- otherwise the invariant breaks. */
}

char *e_d_msg[] = {  "Ctrl C pressed\nQuit Debugger ?",
			"Quit Debugger ?",
			"End of Debugging",
			"Program is not running",
			"No symbol in current context\n",
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

int e_deb_inp(FENSTER *f)
{
 ECNT *cn = f->ed;
 int c = 0;

 f = cn->f[cn->mxedt];
 c = e_getch();
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

/* Has the debugger child terminated?  Non-blocking, reaps it if so (and clears
   e_d_pid so e_d_quit_basic does not double-reap).  The debugger read loops use
   this so a debugger that died -- but whose pipe never reaches EOF because the
   X11 launch path leaks xwpe's own write ends -- cannot hold xwpe forever
   waiting for output that will never come (the project convention). */
static int e_d_debugger_gone(void)
{
 int st;

 if (e_d_pid <= 0)
  return 1;
 if (waitpid(e_d_pid, &st, WNOHANG) == e_d_pid)
 {
  e_d_pid = 0;
  return 1;
 }
 return 0;
}

/*  Input Routines   */
int e_e_line_read(int n, signed char *s, int max)
{
 int i, ret = 0;

 for (i = 0; i < max - 1; i++)
 {
  ret = read(n, s + i, 1);
  if (ret != 1 || s[i] == EOF || s[i] == '\n'|| s[i] == '\0')
   break;
 }
 if (ret != 1 && i == 0)
  return(-1);
 s[i+1] = '\0';
 jdb_trace("e_e_line_read: fd=%d i=%d ret=%d s='%s'\n", n, i, ret, (char*)s);
 if (e_deb_type == DEB_SDB && s[i] == '*')
  return(1);
 if (e_deb_type == DEB_XDB && s[i] == '>')
  return(1);
 else if (e_deb_type == DEB_DBX && i > 4 && s[i] == ' ' && !strncmp(s+i-5, "(dbx)", 5))
  return(1);
 else if (e_deb_type == DEB_GDB && i > 4 && s[i] == ' ' && !strncmp(s+i-5, "(gdb)", 5))
  return(1);
 else if (e_deb_type == DEB_PDB && i > 4 && s[i] == ' ' && !strncmp(s+i-5, "(Pdb)", 5))
  return(1);
 else if (e_deb_type == DEB_A68G && i > 5 && s[i] == ' ' && !strncmp(s+i-6, "(a68g)", 6))
  return(1);
 else if (e_deb_type == DEB_JDB && i > 0 &&
   ((s[i] == ' ' && s[i-1] == ']') ||
    (s[i] == ' ' && s[i-1] == '>')))
  return(1);
 return(2);
}

int e_d_line_read(int n, signed char *s, int max, int sw, int esw)
{
 static char wt = 0, esc_sv = 0, str[12];
 int i, j, ret = 0, kbdflgs;

 /* Runaway debugger: far more output than any real command reply means the
    debugger is streaming uncontrollably (e.g. a68g's abend recursion).  Give
    up instead of spinning; the caller treats -1 as end-of-output and quits. */
 if (++e_d_read_runaway > E_D_RUNAWAY_MAX)
 {
  e_d_read_runaway = 0;
  s[0] = '\0';
  return(-1);
 }
 if (esw)
 {  if((ret = e_e_line_read(efildes[0], s, max)) >= 0) return(ret);  }
 else
 {  int _drain = 0;
    /* Drain buffered stderr, but never spin on a debugger that floods it. */
    while(e_e_line_read(efildes[0], s, max) >= 0)
     if (++_drain >= E_D_DRAIN_MAX) break;
 }
 for(i = 0; i < max - 1; i++)
 {
  if (esc_sv)  {  s[i] = WPE_ESC;  esc_sv = 0;  continue;  }
  kbdflgs = fcntl(n, F_GETFL, 0 );
  fcntl( n, F_SETFL, kbdflgs | O_NONBLOCK);
  while ((ret = read(n, s + i, 1)) <= 0 && i == 0 && wt >= sw)
  {
   if (ret == 0)
   {
    jdb_trace("e_d_line_read: EOF on pipe (debugger exited)\n");
    fcntl(n, F_SETFL, kbdflgs & ~O_NONBLOCK);
    return(-1);
   }
   /* read() returned EAGAIN: if the debugger process has died but its pipe
      never reached EOF (the X11 launch path leaks xwpe's own write ends),
      stop waiting for output that will never arrive instead of looping. */
   if (e_d_debugger_gone())
   {
    fcntl(n, F_SETFL, kbdflgs & ~O_NONBLOCK);
    return(-1);
   }
   e_d_wait_for_input(n);
   if (e_d_getchar() == D_CBREAK) return(-1);
  }
  fcntl( n, F_SETFL, kbdflgs & ~O_NONBLOCK);
  if (ret == -1) break;
  else if (ret != 1 ||  s[i] == EOF || s[i] == '\0') break;
  else if(s[i] == '\n' || s[i] == WPE_CR)  break;
  else if(s[i] == WPE_ESC)
  {  s[i] = '\0';  esc_sv = 1;  break;  }
 }
 if (ret != 1)
 {
  s[i] = '\0';
  jdb_trace("e_d_line_read: ret!=1 path, i=%d s='%s' (hex:", i, (char*)s);
  { int _k; for (_k = 0; _k < i; _k++)
    jdb_trace(" %02x", (unsigned char)s[_k]);
  }
  jdb_trace(")\n");
  if(e_deb_type == DEB_SDB && i > 0 && s[i-1] == '*')
  {  str[0] = 0;  wt = 0;   return(1);  }
  else if(e_deb_type == DEB_XDB && i > 0 && s[i-1] == '>')
  {  str[0] = 0;  wt = 0;   return(1);  }
  else if(e_deb_type == DEB_JDB && i > 1 &&
    ((s[i-1] == ' ' && s[i-2] == ']') ||
     (s[i-1] == ' ' && s[i-2] == '>')))
  {  jdb_trace("e_d_line_read: jdb prompt DETECTED\n");
     str[0] = 0;  wt = 0;   return(1);  }
  else if(e_deb_type == DEB_JDB)
  {  jdb_trace("e_d_line_read: jdb prompt NOT detected (i=%d, s[i-1]=%02x s[i-2]=%02x)\n",
               i, i>0?(unsigned char)s[i-1]:0, i>1?(unsigned char)s[i-2]:0);
  }
  if(e_deb_type == DEB_GDB)
  {
   if(i > 5 && !strncmp(s+i-6, "(gdb) ", 6))
   {  str[0] = 0;  wt = 0;   return(1);  }
   else if(i < 6)
   {
    for (j = 0; j <= i; j++) str[6+j] = s[j];
    if (!strncmp(str+i-6, "(gdb) ", 6))
    {  str[0] = '\0';  wt = 0;  return(1);  }
   }
  }
  else if (e_deb_type == DEB_DBX)
  {
   if(i > 5 && !strncmp(s+i-6, "(dbx) ", 6))
   {  str[0] = 0;  wt = 0;   return(1);  }
   else if(i < 6)
   {
    for(j = 0; j <= i; j++) str[6+j] = s[j];
    if(!strncmp(str+i-6, "(dbx) ", 6))
    {  str[0] = '\0';  wt = 0;  return(1);  }
   }
  }
  else if (e_deb_type == DEB_PDB)
  {
   if(i > 5 && !strncmp(s+i-6, "(Pdb) ", 6))
   {  str[0] = 0;  wt = 0;   return(1);  }
   else if(i < 6)
   {
    for(j = 0; j <= i; j++) str[6+j] = s[j];
    if(!strncmp(str+i-6, "(Pdb) ", 6))
    {  str[0] = '\0';  wt = 0;  return(1);  }
   }
  }
  else if (e_deb_type == DEB_A68G)
  {
   if(i > 6 && !strncmp(s+i-7, "(a68g) ", 7))
   {  str[0] = 0;  wt = 0;   return(1);  }
   else if(i < 7)
   {
    for(j = 0; j <= i; j++) str[7+j] = s[j];
    if(!strncmp(str+i-7, "(a68g) ", 7))
    {  str[0] = '\0';  wt = 0;  return(1);  }
   }
  }
 }
 else
 {
  s[i+1] = '\0';
  jdb_trace("e_d_line_read: normal line, i=%d s='%s'\n", i, (char*)s);
 }
 if (i != 0) wt = 0;
 else wt++;
 if(i > 4) for(j = 0; j < 6; j++) str[j] = s[j+i-5];
 return(0);
}

int e_d_dum_read()
{
 char str[256];
 int ret;

 jdb_trace("e_d_dum_read: entry\n");
 while ((ret = e_d_line_read(wfildes[0], str, 128, 0, 0)) == 0 || ret == 2)
 {
  jdb_trace("e_d_dum_read: line_read ret=%d str='%s'\n", ret, str);
  if (ret == 2)
   e_d_error(str);
 }
 jdb_trace("e_d_dum_read: exit ret=%d str='%s'\n", ret, str);
 return(ret);
}

/* Output Routines */
int e_d_p_exec(FENSTER *f)
{
 ECNT *cn = f->ed;
 BUFFER *b;
 SCHIRM *s;
 int ret, i, is, j;
 char str[512];

 for (i = cn->mxedt; i > 0 && strcmp(cn->f[i]->datnam, "Messages"); i--)
  ;
 if (i <= 0)
 {  e_edit(cn, "Messages");  i = cn->mxedt;  }
 f = cn->f[i];
 b = cn->f[i]->b;
 s = cn->f[i]->s;
 if (b->bf[b->mxlines-1].len != 0)
  e_new_line(b->mxlines, b);
 for (j = 0, i = is = b->mxlines-1;
   (ret = e_d_line_read(wfildes[0], str, 512, 0, 0)) == 0; )
 {
  if (ret == -1)
   return(ret);
  print_to_end_of_buffer(b, str, b->mx.x);
 }
 if (ret == 1)
 {
  e_new_line(i, b);
  for (j = 0; j < NUM_COLS_ON_SCREEN_SAFE - 2 && str[j] != '\n' &&
    str[j] != '\0'; j++)
   *(b->bf[i].s+j) = str[j];
  b->b.y = i;
  b->b.x = b->bf[i].len = j;
  b->bf[i].nrc = j;
 }
 b->b.y = b->mxlines-1;
 b->b.x = 0;

 e_rep_win_tree(cn);
 return(ret);
}

/*    Help Routines   */
int e_d_getchar()
{
 int i = 1, fd;
 char c = 0, kbdflgs = 0;
 static FILE *_gc_dbg = NULL;

 if (WpeIsXwin()) fd = rfildes[0];
 else fd = 0;

/*   if(WpeIsXwin() || e_deb_mode)    */
 {
  kbdflgs = fcntl(fd, F_GETFL, 0 );
  fcntl( fd, F_SETFL, kbdflgs | O_NONBLOCK);
 }
#ifndef NO_XWINDOWS
 if (WpeIsXwin())
 {
  XEvent _ev;
  KeySym _ks;
  char _buf[8];

  XFlush(WpeXInfo.display);
  while (XPending(WpeXInfo.display))
  {
   XNextEvent(WpeXInfo.display, &_ev);
   if (_ev.type == KeyPress)
   {
    if (XLookupString(&_ev.xkey, _buf, sizeof(_buf), &_ks, NULL) == 1)
    {
     c = _buf[0];
     break;
    }
   }
   else if (_ev.type == Expose)
   {
    XCopyArea(WpeXInfo.display, WpeXInfo.backbuf, WpeXInfo.window,
      WpeXInfo.gc, _ev.xexpose.x, _ev.xexpose.y,
      _ev.xexpose.width, _ev.xexpose.height,
      _ev.xexpose.x, _ev.xexpose.y);
   }
  }
 }
#endif
 if (c || (i = read(fd, &c, 1)) == 1)
 {
  if (c == CTRLC)
  {
/*	 if (WpeIsXwin() || e_deb_mode)    */
   fcntl(fd, F_SETFL, kbdflgs & ~O_NONBLOCK);
   e_d_switch_out(0);
   i = e_message(1, e_d_msg[ERR_CTRLCPRESS], WpeEditor->f[WpeEditor->mxedt]);
   if (i == 'Y')
   {
    e_d_quit(WpeEditor->f[WpeEditor->mxedt]);
    return(D_CBREAK);
   }
   else
    return(c);
  }
  else if (e_d_pty_master >= 0 && ((c >= 32 && c < 127) || c == '\r' || c == '\n' || c == '\b'))
  {
   char _ch = (c == '\r') ? '\n' : c;
   write(e_d_pty_master, &_ch, 1);
   /* No explicit echo: the pty's line discipline echoes the byte back, and
      e_d_pty_read_to_messages picks it up.  Echoing here would double it. */
  }
  else
   write(rfildes[1], &c, 1);
 }
/*   if (WpeIsXwin() || e_deb_mode)   */
 fcntl(fd, F_SETFL, kbdflgs & ~O_NONBLOCK);
 return(i == 1 ? c : 0);
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

int e_d_quit_basic(FENSTER *f)
{
 int i, kbdflgs;
 /* Ignore SIGPIPE before writing to pipes -- the debugger may have
    already exited and closed its end, causing write() to SIGPIPE. */
 signal(SIGPIPE, SIG_IGN);

 if (!e_d_swtch)
  return 0;
 if (rfildes[1] >= 0)
 {
  if (e_deb_type == DEB_GDB || e_deb_type == DEB_XDB)
   e_d_send_cmd("q\ny\n");
  else if (e_deb_type == DEB_SDB)
   e_d_send_cmd("q\n");
  else if (e_deb_type == DEB_DBX)
   e_d_send_cmd("quit\n");
  else if (e_deb_type == DEB_JDB)
   e_d_send_cmd("quit\n");
  else if (e_deb_type == DEB_PDB)
   e_d_send_cmd("q\ny\n");  /* pdb: quit + confirm */
  else if (e_deb_type == DEB_A68G)
   e_d_send_cmd("quit\nyes\n");  /* a68g: quit + "Terminate a68g (yes|no):" */
 }
 jdb_trace("e_d_quit_basic: quitting debugger type=%d\n", e_deb_type);
 kbdflgs = fcntl(0, F_GETFL, 0 );
 fcntl(0, F_SETFL, kbdflgs & ~O_NONBLOCK);
 e_d_swtch = 0;
 if (e_d_pid)
 {
  kill(e_d_pid, SIGKILL);
  waitpid(e_d_pid, NULL, 0);  /* reap zombie */
  e_d_pid = 0;
 }
 if (!WpeIsXwin())
 {
  if (e_d_out)
   FREE(e_d_out);
  e_d_out = NULL;
 }
 if (rfildes[1] >= 0)
 {  close(rfildes[1]);  rfildes[1] = -1;  }
 if (wfildes[0] >= 0)
 {  close(wfildes[0]);  wfildes[0] = -1;  }
 if (efildes[0] >= 0)
 {  close(efildes[0]);  efildes[0] = -1;  }
 if (rfildes[0] >= 0)
 {  close(rfildes[0]);  rfildes[0] = -1;  }
 if (wfildes[1] >= 0)
 {  close(wfildes[1]);  wfildes[1] = -1;  }
 e_d_pty_close();
 e_d_async_reset();
 _messages_activated = 0;
 e_d_async_pending = 0;
 e_d_accum.active = 0;
 e_d_accum.f = NULL;
 e_d_nbrpts = 0;
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
}

int e_d_quit(FENSTER *f)
{
 ECNT *cn = f->ed;
 int i;
 jdb_trace("e_d_quit: CALLED, e_d_swtch=%d, e_deb_type=%d\n",
           e_d_swtch, e_deb_type);
 if (e_d_dap_active())
  e_d_dap_quit(f);   /* disconnect the adapter; common cleanup runs below */
 e_d_quit_basic(f);
 e_d_p_message(e_d_msg[ERR_ENDDEBUG], f, 1);
 WpeMouseChangeShape(WpeEditingShape);
 e_d_delbreak(f);
 /* Switch back to the source file, not Messages.
    The user expects to continue editing after quitting the debugger. */
 for (i = cn->mxedt; i > 0; i--)
  if (e_check_c_file(cn->f[i]->datnam) || e_d_dap_source_ext(cn->f[i]->datnam))
  {
   e_switch_window(cn->edt[i], cn->f[cn->mxedt]);
   return(0);
  }
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

 if ((f->ed->mxedt > 0) && (strcmp(f->datnam, "Watches") == 0))
 { /* sets y=number of watch we're inserting */
  for(y = 0; y < e_d_nwtchs && e_d_nrwtchs[y] < f->b->b.y; y++)
   ;
 }
 else
  y = e_d_nwtchs;
 if (f->ed->wdf && f->ed->wdf->anz > 0)
  strcpy(str, f->ed->wdf->name[0]);
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

 if (strcmp(f->datnam, "Watches"))
  return(0);
 for (l = 0; l < e_d_nwtchs && e_d_nrwtchs[l] <= b->b.y; l++)
  ;
 if (l == e_d_nwtchs && b->bf[b->b.y].len == 0)
  return(e_make_watches(f));
 strcpy(str, e_d_swtchs[l - 1]);
 if (e_d_add_watch(str, f))
 {
  e_d_swtchs[l - 1] = REALLOC(e_d_swtchs[l - 1], strlen(str) + 1);
  strcpy(e_d_swtchs[l - 1], str);
  e_d_p_watches(f, 0);
  return(0);
 }
 return(-1);
}

/* Among other things, e_d_p_watches() must recompute e_d_nrwtchs when
   called from e_edit_watches(), 
   but has code paths that don't do this ==> possible BUG
*/
int e_d_p_watches(FENSTER *f, int sw)
{
 ECNT *cn = f->ed;
 BUFFER *b;
 SCHIRM *s;
 int iw, i, k = 0, l, ret;
 char str1[256], *str; /* is 256 always large enough? */
 char *str2;

 if (e_d_dap_active())
  return(e_d_dap_watches(f, sw));   /* DAP: values via evaluate(context=watch) */
 e_d_switch_out(0);
 if ((e_d_swtch > 2) && (e_d_p_stack(f, 0) == -1))
  return(-1);
 /* Find the watch window */
 for (iw = cn->mxedt; iw > 0 && strcmp(cn->f[iw]->datnam, "Watches"); iw--);
 if (iw == 0 && !e_d_nwtchs)
 { /* if no watches and the mysterious iw!=0 then just repaint window tree */
  e_rep_win_tree(cn);
  return(0);
 }
 else if (iw == 0)
 {
  if(e_edit(cn, "Watches"))
  {
   return(-1);
  }
  else
  {
   iw = cn->mxedt;
  }
 }
 f = cn->f[iw];
 b = cn->f[iw]->b;
 s = cn->f[iw]->s;
 
 /* free all lines of BUFFER b */
 e_p_red_buffer(b);
 FREE(b->bf[0].s);
 b->mxlines=0;

 for (i = 0, l = 0; l < e_d_nwtchs; l++)
 {
  str = str1;
  
  /* Create appropriate command for the debugger */
  if (e_deb_type == DEB_GDB || e_deb_type == DEB_XDB)
  {
   sprintf(str1, "p %s\n", e_d_swtchs[l]);
  }
  else if (e_deb_type == DEB_SDB)
  {
   sprintf(str1, "%s/\n", e_d_swtchs[l]);
  }
  else if (e_deb_type == DEB_DBX)
  {
   sprintf(str1, "print %s\n", e_d_swtchs[l]);
  }
  else if (e_deb_type == DEB_A68G)
  {
   /* a68g: "evaluate EXPR" -- the expression is taken literally, so it
      must NOT be quoted ("n" would be the CHAR denotation, not variable n). */
   sprintf(str1, "evaluate %s\n", e_d_swtchs[l]);
  }

  /* Send command to debugger */
  if(e_d_swtch)
  {
   e_d_send_cmd(str1);
  }

  /* If no debugger or no response, give message of no symbol in context */
  if (!e_d_swtch || (ret = e_d_line_read(wfildes[0], str1, 256, 0, 0)) == 1)
  {
   strcpy(str1, e_d_msg[ERR_NOSYMBOL]);
   k = 0;
  }
  else /* Debugger successfully returned a value */
  {
   if (ret == -1)
   {
    return(ret); /* BUG? e_d_nrwtchs not initialized if this return is taken */
   }
   str = MALLOC(strlen(str1) + 1);
   strcpy(str, str1);
   while ((ret = e_d_line_read(wfildes[0], str1, 256, 0, 0)) == 0 || ret == 2)
   {
    if (ret == -1) return(ret); /* BUG? e_d_nrwtchs not initialized if this return is taken */
    if (ret == 2) e_d_error(str1);
    str = REALLOC(str, (k = strlen(str)) + strlen(str1) + 1);
    if (str[k-1] == '\n') str[k-1] = ' ';
    strcat(str, str1);
   }

   /* Find the beginning of the information (depends on debugger output
     format) */
   if (e_deb_type == DEB_GDB || e_deb_type == DEB_DBX || e_deb_type == DEB_XDB)
   {
    if (e_deb_type == DEB_XDB && str[0] == '0')
    {
     for(k = 1; str[k] != '\0' && !isspace(str[k]); k++);
    }
    else
     for(k = 0; str[k] != '\0' && str[k] != '='; k++);
    if (str[k] == '\0')
    {
     if (str != str1) FREE(str);
     str = str1;
     strcpy(str, e_d_msg[ERR_NOSYMBOL]);
     k = 0;
    }
    for(k++; str[k] != '\0' && isspace(str[k]); k++);
   }
   else if(e_deb_type == DEB_SDB)
   {
    for (k = 0; str[k] != '\0' && str[k] != ':'; k++);
    if (str[k] == '\0') k = 0;
    else k++;
   }
  }

  /* Print variable name */
  for ( ; str[k] != '\0' && isspace(str[k]); k++);
  /* a68g reports an out-of-scope variable with a verbose monitor error
     ("a68g: monitor error: cannot find identifier (n)").  This is normal once
     execution leaves the procedure where the variable lives (e.g. after the
     recursion unwinds); show a short, friendly note instead. */
  if (e_deb_type == DEB_A68G &&
      (strstr(str + k, "cannot find identifier") ||
       strstr(str + k, "monitor error")))
  {
   str2 = WpeMalloc(strlen(e_d_swtchs[l]) + 20);
   sprintf(str2, "%s: <not in scope>", e_d_swtchs[l]);
  }
  else
  {
   str2 = WpeMalloc(strlen(e_d_swtchs[l]) + strlen(str + k) + 4);
   sprintf(str2, "%s: %s", e_d_swtchs[l], str + k);
  }

  e_d_nrwtchs[l] = b->mxlines;
  print_to_end_of_buffer(b, str2, b->mx.x);

  /* Free any allocated string */
  WpeFree(str2);
  if(str != str1)
  {
   FREE(str);
  }
 }

 e_new_line(b->mxlines, b);
 fk_cursor(1);
/* if (b->b.y > i || sw) b->b.y = i;*/
 if (sw && iw != cn->mxedt) e_switch_window(cn->edt[iw], cn->f[cn->mxedt]);
 else e_rep_win_tree(cn);
/* e_d_switch_out(0);   */
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
 e_d_switch_out(0);
 return(e_d_p_stack(f, 1));
}

int e_d_p_stack(FENSTER *f, int sw)
{
 ECNT *cn = f->ed;
 BUFFER *b;
 SCHIRM *s;
 int is, i, j, k, l, ret;
 char str[256];

 if (e_d_swtch < 3)
  return(e_error(e_d_msg[ERR_NOTRUNNING], 0, f->fb));
 for (i = 0; i < SVLINES; i++)
  e_d_out_str[i][0] = '\0';
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
 b = cn->f[is]->b;
 s = cn->f[is]->s;
 if (!e_d_swtch)
  return(0);
 if (e_deb_type == DEB_GDB)
  e_d_send_cmd("bt\n");
 else if (e_deb_type == DEB_SDB || e_deb_type == DEB_XDB)
  e_d_send_cmd("t\n");
 else if (e_deb_type == DEB_DBX)
  e_d_send_cmd("where\n");
 else if (e_deb_type == DEB_A68G)
  e_d_send_cmd("calls\n");
 while ((ret = e_d_line_read(wfildes[0], str, 256, 0, 0)) == 2)
  e_d_error(str);
 if (ret == -1)
  return(-1);
 i = j = 0;
 if (ret == 1)
 {
  e_d_error(e_d_msg[ERR_PROGEXIT]);
  return(e_d_quit(f));
 }
 while(ret != 1)
 {
  k = 0;
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
  while ((ret = e_d_line_read(wfildes[0], str, 256, 0, 0)) == 2)
   e_d_error(str);
  if (ret == -1)
   return(-1);
 }
 for (; i < b->mxlines; i++)
  e_del_line(i, b, s);
 if (sw && is != cn->mxedt)
  e_switch_window(cn->edt[is], cn->f[cn->mxedt]);
 e_rep_win_tree(cn);
 return(0);
}

int e_make_stack(FENSTER *f)
{
   char file[128], str[128], *tmpstr = MALLOC(1);
   int i, ret, line = 0, dif;
   BUFFER *b = f->ed->f[f->ed->mxedt]->b;
   e_d_switch_out(0);
   if(e_deb_type != DEB_SDB)
   {  tmpstr[0] = '\0';
      if(e_deb_type == DEB_GDB)
      {  for(i = dif = 0; i <= b->b.y; i++)
	    if(b->bf[i].s[0] == '#') dif = atoi((char *) (b->bf[i].s + 1));
	 for(i = b->b.y; i >= 0 && b->bf[i].s[0] != '#'; i--);
	 if(i < 0) return(1);
	 for(; i < b->mxlines; i++)
	 {  if(!(tmpstr = REALLOC(tmpstr, strlen(tmpstr) + b->bf[i].len + 2)))
	    {  e_error(e_msg[ERR_LOWMEM], 0, f->fb);  return(-1);  }
	    strcat(tmpstr, (char *) b->bf[i].s);
	    if(i == b->mxlines-1 || b->bf[i+1].s[0] == '#') break;
	    else if(tmpstr[strlen(tmpstr)-2] == '\\')
	       tmpstr[strlen(tmpstr)-2] = '\0';
	    else if(tmpstr[strlen(tmpstr)-1] == '\n')
	       tmpstr[strlen(tmpstr)-1] = '\0';
	 }
      }
      else
      {  for(i = 1, dif = 0; i <= b->b.y; i++)
	    if(b->bf[i-1].s[b->bf[i-1].len - 1] != '\\') dif++;
	 for(i = b->b.y; i > 0 && b->bf[i-1].s[b->bf[i-1].len - 1] == '\\';
	    i--);
	 if(i == 0 && b->bf[i].len == 0) return(1);
	 for(; i < b->mxlines; i++)
	 {  if(!(tmpstr = REALLOC(tmpstr, strlen(tmpstr) + b->bf[i].len + 2)))
	    {  e_error(e_msg[ERR_LOWMEM], 0, f->fb);  return(-1);  }
	    strcat(tmpstr, (char *) b->bf[i].s);
	    if(i == b->mxlines-1 || b->bf[i].s[b->bf[i].len - 1] != '\\')
	       break;
	    else if(tmpstr[strlen(tmpstr)-2] == '\\')
	       tmpstr[strlen(tmpstr)-2] = '\0';
	    else if(tmpstr[strlen(tmpstr)-1] == '\n')
	       tmpstr[strlen(tmpstr)-1] = '\0';
	 }
      }

      if(e_deb_type == DEB_XDB && (line = e_make_line_num2(tmpstr, file)) < 0)
	 return(e_error(e_d_msg[ERR_NOSOURCE], 0, f->fb));
      else if(e_deb_type != DEB_XDB && (line = e_make_line_num(tmpstr, file)) < 0)
	 return(e_error(e_d_msg[ERR_NOSOURCE], 0, f->fb));
      if(dif > e_d_nstack)
	 sprintf(str, "%s %d\n",
	 e_deb_type != DEB_XDB ? "up" : "down", dif - e_d_nstack);
      else if(dif < e_d_nstack)
	 sprintf(str, "%s %d\n",
	 e_deb_type != DEB_XDB ? "down" : "up", e_d_nstack - dif);
      if(dif != e_d_nstack)
      {  e_d_send_cmd(str);
	 while((ret = e_d_line_read(wfildes[0], str, 128, 0, 0)) == 0 || ret == 2)
	    if( ret == 2) e_d_error(str);
	 if(ret == -1) return(ret);
	 e_d_nstack = dif;
      }
   }
   else if(e_deb_type == DEB_SDB)
   {  for(i = b->b.y; i >= 0 && (line =
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

/*  Breakpoints   */
int e_breakpoint(FENSTER *f)
{
 return(e_make_breakpoint(f, 0));
}

int e_remove_breakpoints(FENSTER *f)
{
 ECNT *cn = f->ed;
 int i;

 if (e_d_swtch)
 {
  if (e_deb_type == DEB_GDB)
   e_d_send_cmd("d\ny\n");
  else if (e_deb_type == DEB_SDB)
   e_d_send_cmd("D\n");
  else if (e_deb_type == DEB_DBX)
   e_d_send_cmd("delete all\n");
  else if (e_deb_type == DEB_XDB)
   e_d_send_cmd("db *\n");
  else if (e_deb_type == DEB_A68G)
   e_d_send_cmd("breakpoint clear breakpoints\n");
 }
 for (i = 0; i < e_d_nbrpts; i++)
  FREE(e_d_sbrpts[i]);
 for (i = cn->mxedt; i >= 0; i--)
  if (DTMD_ISTEXT(cn->f[i]->dtmd))
   cn->f[i]->s->brp[0] = 0;
 e_d_nbrpts = 0;
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
 e_rep_win_tree(cn);
 return(0);
}

/* Grow the three parallel breakpoint arrays (source file name, source line,
   debugger-assigned id) by one entry.  The caller then fills in element
   e_d_nbrpts - 1 of each.  Used wherever a breakpoint is recorded
   (e_mk_brk_main, e_make_breakpoint, e_d_a68g_rearm_breakpoints). */
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
}

int e_mk_brk_main(FENSTER *f, int sw)
{
 int i, ret;
 char eing[128], str[256];

 if (sw)
 {
  if (e_d_swtch)
  {
   if (e_deb_type == DEB_GDB) sprintf(eing, "d %d\n", e_d_nrbrpts[sw-1]);
   else if (e_deb_type == DEB_DBX)
    sprintf(eing, "delete %d\n", e_d_nrbrpts[sw-1]);
   else if (e_deb_type == DEB_XDB)
    sprintf(eing, "db %d\n", e_d_nrbrpts[sw-1]);
   else if (e_deb_type == DEB_JDB)
   {
    char cls[128];
    strcpy(cls, e_d_file);
    WpeStringCutChar(cls, '.');
    sprintf(eing, "clear %s.%s\n", cls, e_get_start_symbol());
   }
   else if (e_deb_type == DEB_SDB)
   {
    sprintf(eing, "e %s\n", e_get_start_symbol());
    e_d_send_cmd(eing);
    if (e_d_dum_read() == -1) return(-1);
    sprintf(eing, "%d d\n", e_d_ybrpts[sw-1]);
   }
   jdb_trace("e_mk_brk_main(del): sending '%s'\n", eing);
   e_d_send_cmd(eing);
   if (e_d_dum_read() == -1) return(-1);
  }
  FREE(e_d_sbrpts[sw-1]);
  for (i = sw-1; i < e_d_nbrpts - 1; i++)
  {
   e_d_sbrpts[i] = e_d_sbrpts[i+1];
   e_d_ybrpts[i] = e_d_ybrpts[i+1];
   e_d_nrbrpts[i] = e_d_nrbrpts[i+1];
  }
  e_d_nbrpts--;
  if (e_d_nbrpts == 0)
  {
   FREE(e_d_sbrpts);
   e_d_sbrpts = NULL;
   FREE(e_d_ybrpts);
   e_d_ybrpts = NULL;
   FREE(e_d_nrbrpts);
   e_d_nrbrpts = NULL;
  }
 }
 else
 {
  e_d_brk_grow();
  e_d_sbrpts[e_d_nbrpts - 1] = MALLOC(1);
  if (e_d_swtch)
  {
   if (e_deb_type == DEB_GDB)
   {
    sprintf(eing, "b %s\n", e_get_start_symbol());
    e_d_send_cmd(eing);
    while ((ret = e_d_line_read(wfildes[0], str, 256, 0, 0)) == 0 &&
      strncmp(str, "Breakpoint", 10))
     ;
    if (ret == -1) return(ret);
    if (ret == 2) e_d_error(str);
    e_d_nrbrpts[e_d_nbrpts - 1] = atoi(str+11);
    if (ret != 1 && e_d_dum_read() == -1) return(-1);
   }
   else if (e_deb_type == DEB_DBX)
   {
    sprintf(eing, "stop in %s\n", e_get_start_symbol());
    e_d_send_cmd(eing);
    while ((ret = e_d_line_read(wfildes[0], str, 256, 0, 0)) == 0 &&
      str[0] != '(')
     ;
    if (ret == -1) return(ret);
    if (ret == 2) e_d_error(str);
    e_d_nrbrpts[e_d_nbrpts - 1] = atoi(str+1);
    if (ret != 1 && e_d_dum_read() == -1) return(-1);
   }
   else if (e_deb_type == DEB_XDB)
   {
    sprintf(eing, "b %s\n", e_get_start_symbol());
    e_d_send_cmd(eing);
    while ((ret = e_d_line_read(wfildes[0], str, 256, 0, 0)) == 0 &&
      strncmp(str, "Added:", 6))
     ;
    ret = e_d_line_read(wfildes[0], str, 256, 0, 0);
    if (ret == -1) return(ret);
    if (ret == 2) e_d_error(str);
    e_d_nrbrpts[e_d_nbrpts - 1] = atoi(str+1);
    if (ret != 1 && e_d_dum_read() == -1) return(-1);
   }
   else if (e_deb_type == DEB_JDB)
   {
    { char cls[128];
      strcpy(cls, e_d_file);
      WpeStringCutChar(cls, '.');
      sprintf(eing, "stop in %s.%s\n", cls, e_get_start_symbol());
    }
    jdb_trace("e_mk_brk_main: sending '%s'\n", eing);
    e_d_send_cmd(eing);
    if (e_d_dum_read() == -1) return(-1);
    e_d_nrbrpts[e_d_nbrpts - 1] = e_d_nbrpts;
   }
   else if (e_deb_type == DEB_SDB)
   {
    sprintf(eing, "e %s\n", e_get_start_symbol());
    e_d_send_cmd(eing);
    if (e_d_dum_read() == -1) return(-1);
    sprintf(eing, "b\n");
    e_d_send_cmd(eing);
    if ((ret = e_d_line_read(wfildes[0], str, 256, 0, 0)) == -1)
     return(ret);
    if (ret == 2) e_d_error(str);
    if (ret != 1)
    {
     for (i = 0; str[i] && str[i] != ':'; i++)
      ;
     if (str[i])
      e_d_ybrpts[e_d_nbrpts - 1] = atoi(str+i+1);
     if (e_d_dum_read() == -1) return(-1);
    }
   }
  }
 }
 return(sw ? 0 : e_d_nbrpts);
}

int e_make_breakpoint(FENSTER *f, int sw)
{
 ECNT *cn = f->ed;
 SCHIRM *s = cn->f[cn->mxedt]->s;
 BUFFER *b = cn->f[cn->mxedt]->b;
 int ret, i;
 char eing[128], str[256];

 if (!sw)
 {
  if (!e_check_c_file(f->datnam) && !e_d_dap_source_ext(f->datnam))
   return(e_error(e_p_msg[ERR_NO_CFILE], 0, f->fb));
  for(i = 0; i < s->brp[0] && s->brp[i+1] != b->b.y; i++)
   ;
  if (i < s->brp[0])
  {
   for (i++; i < s->brp[0]; i++) s->brp[i] = s->brp[i+1];
   (s->brp[0])--;
   for (i = 0; i < e_d_nbrpts && (strcmp(e_d_sbrpts[i], f->datnam) ||
     e_d_ybrpts[i] != b->b.y+1); i++)
    ;
   if (e_d_swtch)
   {
    if (e_deb_type == DEB_GDB) sprintf(eing, "d %d\n", e_d_nrbrpts[i]);
    else if (e_deb_type == DEB_DBX)
     sprintf(eing, "delete %d\n", e_d_nrbrpts[i]);
    else if (e_deb_type == DEB_XDB)
     sprintf(eing, "db %d\n", e_d_nrbrpts[i]);
    else if (e_deb_type == DEB_JDB)
    {
     /* jdb: "clear Class:line" */
     char cls[128];
     strcpy(cls, e_d_sbrpts[i]);
     WpeStringCutChar(cls, '.');
     sprintf(eing, "clear %s:%d\n", cls, e_d_ybrpts[i]);
    }
    else if (e_deb_type == DEB_SDB)
    {
     sprintf(eing, "e %s\n", e_d_sbrpts[i]);
     e_d_send_cmd(eing);
     if (e_d_dum_read() == -1) return(-1);
     sprintf(eing, "%d d\n", e_d_ybrpts[i]);
    }
    else if (e_deb_type == DEB_A68G)
     sprintf(eing, "breakpoint %d clear\n", e_d_ybrpts[i]);
    e_d_send_cmd(eing);
    if (e_d_dum_read() == -1) return(-1);
   }
   FREE(e_d_sbrpts[i]);
   for (; i < e_d_nbrpts - 1; i++)
   {
    e_d_sbrpts[i] = e_d_sbrpts[i+1];
    e_d_ybrpts[i] = e_d_ybrpts[i+1];
    e_d_nrbrpts[i] = e_d_nrbrpts[i+1];
   }
   e_d_nbrpts--;
   if (e_d_nbrpts == 0)
   {
    FREE(e_d_sbrpts);
    e_d_sbrpts = NULL;
    FREE(e_d_ybrpts);
    e_d_ybrpts = NULL;
    FREE(e_d_nrbrpts);
    e_d_nrbrpts = NULL;
   }
  }
  else
  {
   e_d_brk_grow();
   e_d_sbrpts[e_d_nbrpts - 1] = MALLOC(strlen(f->datnam) + 1);
   strcpy(e_d_sbrpts[e_d_nbrpts - 1], f->datnam);
   e_d_ybrpts[e_d_nbrpts - 1] = b->b.y + 1;
   if (e_d_swtch)
   {
    if (e_deb_type == DEB_GDB)
    {
     snprintf(eing, sizeof(eing), "b %s:%d\n", f->datnam, b->b.y + 1);
     e_d_send_cmd(eing);
     while ((ret = e_d_line_read(wfildes[0], str, 256, 0, 0)) == 0 &&
       strncmp(str, "Breakpoint", 10))
      ;
     if (ret == -1) return(ret);
     if (ret == 2) e_d_error(str);
     e_d_nrbrpts[e_d_nbrpts - 1] = atoi(str+11);
     if (ret != 1 && e_d_dum_read() == -1) return(-1);
    }
    else if (e_deb_type == DEB_DBX)
    {
     snprintf(eing, sizeof(eing), "stop at \"%s\":%d\n", f->datnam, b->b.y + 1);
     e_d_send_cmd(eing);
     while ((ret = e_d_line_read(wfildes[0], str, 256, 0, 0)) == 0 &&
       str[0] != '(')
      ;
     if (ret == -1) return(ret);
     if (ret == 2) e_d_error(str);
     e_d_nrbrpts[e_d_nbrpts - 1] = atoi(str+1);
     if (ret != 1 && e_d_dum_read() == -1) return(-1);
    }
    else if (e_deb_type == DEB_XDB)
    {
     snprintf(eing, sizeof(eing), "b %s:%d\n", f->datnam, b->b.y + 1);
     e_d_send_cmd(eing);
     while ((ret = e_d_line_read(wfildes[0], str, 256, 0, 0)) == 0 &&
       strncmp(str, "Added:", 6))
      ;
     ret = e_d_line_read(wfildes[0], str, 256, 0, 0);
     if (ret == -1) return(ret);
     if (ret == 2) e_d_error(str);
     e_d_nrbrpts[e_d_nbrpts - 1] = atoi(str+1);
     if (ret != 1 && e_d_dum_read() == -1) return(-1);
    }
    else if (e_deb_type == DEB_SDB)
    {
     snprintf(eing, sizeof(eing), "e %s\n", f->datnam);
     e_d_send_cmd(eing);
     if (e_d_dum_read() == -1) return(-1);
     sprintf(eing, "%d b\n", b->b.y + 1);
     e_d_send_cmd(eing);
     if (e_d_dum_read() == -1) return(-1);
    }
    else if (e_deb_type == DEB_JDB)
    {
     /* jdb: "stop at Class:line" -- use filename without extension as class */
     { char cls[128];
       strcpy(cls, f->datnam);
       WpeStringCutChar(cls, '.');
       sprintf(eing, "stop at %s:%d\n", cls, b->b.y + 1);
     }
     e_d_send_cmd(eing);
     if (e_d_dum_read() == -1) return(-1);
     e_d_nrbrpts[e_d_nbrpts - 1] = e_d_nbrpts;
    }
    else if (e_deb_type == DEB_PDB)
    {
     /* pdb: "b line" for current file, "b file:line" for others */
     sprintf(eing, "b %d\n", b->b.y + 1);
     e_d_send_cmd(eing);
     if (e_d_dum_read() == -1) return(-1);
     e_d_nrbrpts[e_d_nbrpts - 1] = e_d_nbrpts;
    }
    else if (e_deb_type == DEB_A68G)
    {
     /* a68g: "breakpoint line" (single source file) */
     sprintf(eing, "breakpoint %d\n", b->b.y + 1);
     e_d_send_cmd(eing);
     if (e_d_dum_read() == -1) return(-1);
     e_d_nrbrpts[e_d_nbrpts - 1] = e_d_nbrpts;
    }
   }
   (s->brp[0])++;
   s->brp = REALLOC(s->brp, (s->brp[0]+1) * sizeof(int));
   s->brp[s->brp[0]] = b->b.y;
  }
 }
 else
 {
  if(e_deb_type == DEB_GDB)
  {
   for (i = 0; i < e_d_nbrpts; i++)
   {
    sprintf(eing, "b %s:%d\n", e_d_sbrpts[i], e_d_ybrpts[i]);
    e_d_send_cmd(eing);
    while ((ret = e_d_line_read(wfildes[0], str, 256, 0, 0)) == 0 &&
      strncmp(str, "Breakpoint", 10))
     ;
    if (ret == -1) return(ret);
    if (ret == 2) e_d_error(str);
    e_d_nrbrpts[e_d_nbrpts - 1] = atoi(str+11);
    if (ret != 1 && e_d_dum_read() == -1) return(-1);
   }
  }
  else if (e_deb_type == DEB_DBX)
  {
   for (i = 0; i < e_d_nbrpts; i++)
   {
    sprintf(eing, "stop at \"%s\":%d\n", e_d_sbrpts[i], e_d_ybrpts[i]);
    e_d_send_cmd(eing);
    while ((ret = e_d_line_read(wfildes[0], str, 256, 0, 0)) == 0 &&
      str[0] != '(')
     ;
    if (ret == -1) return(ret);
    if (ret == 2) e_d_error(str);
    e_d_nrbrpts[e_d_nbrpts - 1] = atoi(str+1);
    if (ret != 1 && e_d_dum_read() == -1) return(-1);
   }
  }
  else if (e_deb_type == DEB_XDB)
  {
   for (i = 0; i < e_d_nbrpts; i++)
   {
    snprintf(eing, sizeof(eing), "b %s:%d\n", f->datnam, b->b.y + 1);
    e_d_send_cmd(eing);
    while ((ret = e_d_line_read(wfildes[0], str, 256, 0, 0)) == 0 &&
      strncmp(str, "Added:", 6))
     ;
    ret = e_d_line_read(wfildes[0], str, 256, 0, 0);
    if (ret == -1) return(ret);
    if (ret == 2) e_d_error(str);
    e_d_nrbrpts[e_d_nbrpts - 1] = atoi(str+1);
    if (ret != 1 && e_d_dum_read() == -1) return(-1);
   }
  }
  else if (e_deb_type == DEB_JDB)
  {
   for (i = 0; i < e_d_nbrpts; i++)
   {
    /* jdb: "stop at Class:line" -- strip .java extension from filename */
    { char cls[128];
      strcpy(cls, e_d_sbrpts[i]);
      WpeStringCutChar(cls, '.');
      sprintf(eing, "stop at %s:%d\n", cls, e_d_ybrpts[i]);
    }
    jdb_trace("e_make_breakpoint(sw=1): sending '%s'\n", eing);
    e_d_send_cmd(eing);
    if (e_d_dum_read() == -1) return(-1);
    e_d_nrbrpts[i] = i + 1;
   }
  }
  else if (e_deb_type == DEB_PDB)
  {
   for (i = 0; i < e_d_nbrpts; i++)
   {
    /* pdb: "b file:line" */
    sprintf(eing, "b %s:%d\n", e_d_sbrpts[i], e_d_ybrpts[i]);
    e_d_send_cmd(eing);
    if (e_d_dum_read() == -1) return(-1);
    e_d_nrbrpts[i] = i + 1;
   }
  }
  else if (e_deb_type == DEB_A68G)
  {
   for (i = 0; i < e_d_nbrpts; i++)
   {
    /* a68g: "breakpoint line" (single source file) */
    sprintf(eing, "breakpoint %d\n", e_d_ybrpts[i]);
    e_d_send_cmd(eing);
    if (e_d_dum_read() == -1) return(-1);
    e_d_nrbrpts[i] = i + 1;
   }
  }
  else
  {
   for (i = 0; i < e_d_nbrpts; i++)
   {
    sprintf(eing, "e %s\n", e_d_sbrpts[i]);
    e_d_send_cmd(eing);
    if (e_d_dum_read() == -1) return(-1);
    sprintf(eing, "%d b\n", e_d_ybrpts[i]);
    e_d_send_cmd(eing);
    if (e_d_dum_read() == -1) return(-1);
   }
  }
 }
 e_schirm(f, 1);
 return(1);
}

/*   start Debugger   */
/* Close an fd if it is open and mark it closed, so a rollback can run twice
   without double-closing (and never touches a stale descriptor). */
static void e_d_close_fd(int *fd)
{
 if (*fd >= 0)
 {
  close(*fd);
  *fd = -1;
 }
}

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

int e_exec_deb(FENSTER *f, char *prog)
{
 int i;

 if (e_d_swtch)
  return(1);
 e_d_swtch = 1;
 /* Mark the pipe fds closed up front so e_d_exec_fail() can tell which ones
    a failed launch actually opened (the opens below set the real values). */
 rfildes[0] = rfildes[1] = -1;
 wfildes[0] = wfildes[1] = -1;
 efildes[0] = efildes[1] = -1;
 fflush(stdout);
 if (WpeIsXwin())
 {
  for (i = 0; i < 5; i++)
  {
   if (npipe[i])
    FREE(npipe[i]);
   npipe[i] = MALLOC(128);
   sprintf(npipe[i], "%s/we_pipe%d", e_tmp_dir, i);
   remove(npipe[i]);
  }
  if (mkfifo(npipe[0], S_IRUSR | S_IWUSR) < 0 ||
    mkfifo(npipe[1], S_IRUSR | S_IWUSR) < 0 ||
    mkfifo(npipe[2], S_IRUSR | S_IWUSR) < 0 ||
    mkfifo(npipe[3], S_IRUSR | S_IWUSR) < 0 ||
    mkfifo(npipe[4], S_IRUSR | S_IWUSR) < 0)
  {
   e_error(e_d_msg[ERR_CANTPIPE], 0, f->fb);
   e_d_exec_fail();
   return(0);
  }
 }
 else
 {
  if (pipe(rfildes))
  {
   e_error(e_p_msg[ERR_PIPEOPEN], 0, f->fb);
   e_d_exec_fail();
   return(0);
  }
  if (pipe(wfildes))
  {
   e_error(e_p_msg[ERR_PIPEOPEN], 0, f->fb);
   e_d_exec_fail();
   return(0);
  }
  if (pipe(efildes))
  {
   e_error(e_p_msg[ERR_PIPEOPEN], 0, f->fb);
   e_d_exec_fail();
   return(0);
  }
 }

 if (e_d_prog_output) { FREE(e_d_prog_output); e_d_prog_output = NULL; }
 e_d_prog_output_len = 0;
 e_d_prog_output_cap = 0;
 if (openpty(&e_d_pty_master, &e_d_pty_slave, e_d_pty_slave_name,
             NULL, NULL) < 0)
 {
  e_d_pty_master = -1;
  e_d_pty_slave = -1;
  e_d_pty_slave_name[0] = '\0';
 }

 if ((e_d_pid = fork()) > 0)
 {
  if (WpeIsXwin())
  {
   if ((wfildes[0] = open(npipe[1], O_RDONLY)) < 0)
   {
    e_error(e_p_msg[ERR_PIPEOPEN], 0, f->fb);
    e_d_exec_fail();
    return(0);
   }
   for (i = 0;
     i < 80 && read(wfildes[0], &e_d_tty[i], 1) == 1 && e_d_tty[i] != '\0' &&
       e_d_tty[i] != '\n';
     i++)
    ;
   if (e_d_tty[i] == '\n')
    e_d_tty[i] = '\0';
   close(wfildes[0]);
   if ((rfildes[0] = open(e_d_tty, O_RDONLY)) < 0 ||
     (wfildes[1] = open(e_d_tty, O_WRONLY)) < 0)
   {
    e_error(e_p_msg[ERR_PIPEOPEN], 0, f->fb);
    e_d_exec_fail();
    return(0);
   }
   if ((rfildes[1] = open(npipe[0], O_WRONLY)) < 0 ||
     (wfildes[0] = open(npipe[1], O_RDONLY)) < 0 ||
     (efildes[0] = open(npipe[2], O_RDONLY)) < 0)
   {
    e_error(e_p_msg[ERR_PIPEOPEN], 0, f->fb);
    e_d_exec_fail();
    return(0);
   }
   if (e_deb_mode)
   {
    tcgetattr(rfildes[0], &ntermio);
/*   ioctl(rfildes[0], TCGETA, &ntermio);*/
    ntermio.c_iflag = 0;
    ntermio.c_oflag = 0;
    ntermio.c_lflag = 0;
    ntermio.c_cc[VMIN] = 1;
    ntermio.c_cc[VTIME] = 0;
#ifdef VSWTCH
    ntermio.c_cc[VSWTCH] = 0;
#endif
/*   ioctl(rfildes[0], TCSETA, &ntermio);*/
    tcsetattr(rfildes[0], TCSADRAIN, &ntermio);
   }
   /* Legacy external-xterm debugger path (e_deb_mode): the child just
      exec'd an xterm to host the debugger.  Let that xterm map its window
      and settle before we pull the input focus back to xwpe, or the
      focus-set races the xterm and may not stick.  The fully event-driven
      cure is to wait for the xterm's MapNotify via SubstructureNotify on the
      root window; that is deferred to the X11-xterm verification pass since it
      cannot be exercised in the headless test harness.  Until then this is a
      single named, bounded settle, not an open-coded magic number. */
   usleep(DEB_XTERM_SETTLE_MS * 1000);
#ifndef NO_XWINDOWS
   XSetInputFocus(WpeXInfo.display, WpeXInfo.window,
     RevertToParent, CurrentTime);
   XFlush(WpeXInfo.display);
#endif
  }
  else
  {
   FILE *fpp;

   /* Close the pipe ends that belong to the child.
      - rfildes[0] is the child's stdin read-end (parent uses rfildes[1])
      - wfildes[1] is the child's stdout write-end (parent uses wfildes[0])
      - efildes[1] is the child's stderr write-end (parent uses efildes[0])
      Without closing these, read() on wfildes[0]/efildes[0] never returns
      EOF when the child exits, because the parent's copy of the write-end
      keeps the pipe open.  This caused infinite loops in e_d_line_read.
      Note: only for non-X11.  In X11 mode, these fds are overwritten
      with tty/named-pipe fds above and the originals are leaked. */
   { int _ret_fd = wfildes[1];
     close(rfildes[0]);  rfildes[0] = -1;
     close(wfildes[1]);  wfildes[1] = -1;
     close(efildes[1]);  efildes[1] = -1;
     if (e_d_pty_slave >= 0)
     {
      close(e_d_pty_slave);
      e_d_pty_slave = -1;
     }

     if (!(fpp = popen("tty", "r")))
     {
      e_error(e_p_msg[ERR_PIPEOPEN], 0, f->fb);
      e_d_exec_fail();
      return(0);
     }
     fgets(e_d_tty, 80, fpp);
     { int _len = strlen(e_d_tty);
       if (_len > 0 && e_d_tty[_len-1] == '\n') e_d_tty[_len-1] = '\0';
     }
     pclose(fpp);
     return(_ret_fd);
   }
  }
  return(wfildes[1]);
 }
 else if (e_d_pid < 0)
 {
  e_error(e_p_msg[ERR_PROCESS], 0, f->fb);
  e_d_exec_fail();
  return(0);
 }

#ifndef NO_XWINDOWS
 if (WpeIsXwin())
 {
  FILE *fp;
  char file[128];

  sprintf(file, "%s/we_sys", e_tmp_dir);
  fp = fopen(file, "w+");
  fprintf(fp, "#!/bin/sh\n");
  if (e_d_pty_master >= 0)
   fprintf(fp, "echo '%s' > %s\n", e_d_pty_slave_name, npipe[1]);
  else
   fprintf(fp, "tty > %s\n", npipe[1]);
  if (e_deb_type == DEB_PDB)
   fprintf(fp,
     "%s -m pdb %s < %s > %s 2> %s\n"
     "echo 'type <Return> to continue' > /dev/tty\nread i < /dev/tty\n",
     e_debugger, prog, npipe[0], npipe[1], npipe[2]);
  else if (!e_deb_swtch)
   fprintf(fp,
     "%s %s < %s > %s 2> %s\n"
     "echo 'type <Return> to continue' > /dev/tty\nread i < /dev/tty\n",
     e_debugger, prog, npipe[0], npipe[1], npipe[2]);
  else
   fprintf(fp,
     "%s %s %s < %s > %s 2> %s\n"
     "echo 'type <Return> to continue' > /dev/tty\nread i < /dev/tty\n",
     e_debugger, e_deb_swtch, prog, npipe[0], npipe[1], npipe[2]);
  fprintf(fp, "rm -f %s\n", file);
  fclose(fp);
  chmod(file, 0755);

  if (e_d_pty_master >= 0)
  {
   close(e_d_pty_master);
   execl(file, file, NULL);
  }
  else
  {
   execlp(XTERM_CMD, XTERM_CMD, "+sb", "-geometry", "80x25-0-0",
     "-T", "xwpe-output", "-e", user_shell, "-c", file, NULL);
  }
  remove(file);
 }
 else
#endif
 {
  /* Close pty master in child -- only the parent reads from it.
     Keep the slave open: gdb needs it accessible for "run > /dev/pts/N". */
  if (e_d_pty_master >= 0)
   close(e_d_pty_master);

  /* Close the pipe ends not used by the child.
     The child uses: rfildes[0] (stdin), wfildes[1] (stdout), efildes[1] (stderr).
     Close the others to prevent the debugged program from inheriting them,
     and to ensure proper EOF detection in the parent. */
  close(rfildes[1]);
  close(wfildes[0]);
  close(efildes[0]);

  close(0);
  if (fcntl(rfildes[0], F_DUPFD, 0) != 0)
  {
   fprintf(stderr, e_p_msg[ERR_PIPEEXEC], rfildes[0]);
   exit(1);
  }
  close(1);
  if (fcntl(wfildes[1], F_DUPFD, 1) != 1)
  {
   fprintf(stderr, e_p_msg[ERR_PIPEEXEC], wfildes[1]);
   exit(1);
  }
  close(2);
  if (fcntl(efildes[1], F_DUPFD, 2) != 2)
  {
   fprintf(stderr, e_p_msg[ERR_PIPEEXEC], efildes[1]);
   exit(1);
  }
  /* jdb does not handle non-blocking stdout/stderr.
     pdb works fine with non-blocking (needed for prompt detection). */
  if (e_deb_type != DEB_JDB)
  {
   int _fl;
   _fl = fcntl(1, F_GETFL, 0 );
   fcntl(1, F_SETFL, _fl | O_NONBLOCK);
   _fl = fcntl(2, F_GETFL, 0 );
   fcntl( 2, F_SETFL, _fl | O_NONBLOCK);
  }
  if (e_deb_type == DEB_PDB)
   execlp(e_debugger, e_debugger, "-m", "pdb", prog, NULL);
  else if (!e_deb_swtch)
   execlp(e_debugger, e_debugger, prog, NULL);
  else
   execlp(e_debugger, e_debugger, e_deb_swtch, prog, NULL);
 }
 fprintf(stderr,"%s %s %s\n", e_p_msg[ERR_IN_COMMAND], e_debugger, prog);
 exit(1);
}

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
static FENSTER       *g_dap_fenster = NULL;/* window used for UI side-effects   */
static int            g_dap_just_started = 0;/* first Run/Step is the entry stop */
static e_dap_host     g_dap_host;
static char           g_dap_progdir[1024] = "";  /* the user's program directory */
static char           g_dap_stop_file[1024] = "";/* full path of the current stop */
static int            g_dap_stop_line = -1;      /* line of the current stop      */

/* True while a DAP session is open. */
static int e_d_dap_active(void)
{
 return(g_dap != NULL);
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
   Watches refresh is deferred to e_d_dap_paint_stop, called once by the step/run
   functions after they settle -- so intermediate stops (e.g. a "step over" that
   momentarily lands in the language runtime before we run on) never flash on
   screen. */
static void e_d_dap_on_stopped(const char *file, int line, const char *reason,
                               void *ud)
{
 (void)reason; (void)ud;
 g_dap_stop_line = line;
 g_dap_stop_file[0] = '\0';
 if (file)
 {
  strncpy(g_dap_stop_file, file, sizeof(g_dap_stop_file) - 1);
  g_dap_stop_file[sizeof(g_dap_stop_file) - 1] = '\0';
 }
}

/* Move the editor to the recorded stop and refresh the Watches window.  Order
   matches e_d_pr_sig: evaluate watches first, then jump to the source line. */
static void e_d_dap_paint_stop(FENSTER *f)
{
 if (!f || g_dap_stop_line < 0 || !g_dap_stop_file[0])
  return;
 strncpy(e_d_file, e_d_dap_basename(g_dap_stop_file), sizeof(e_d_file) - 1);
 e_d_file[sizeof(e_d_file) - 1] = '\0';
 e_d_p_watches(f, 0);                 /* -> e_d_dap_watches (evaluate) */
 e_d_goto_break(g_dap_stop_file, g_dap_stop_line, f);
}

/* on_output: adapter/program output -> Messages window. */
static void e_d_dap_on_output(const char *text, void *ud)
{
 (void)ud;
 if (g_dap_fenster && text)
  e_d_p_message((char *)text, g_dap_fenster, 1);
}

/* on_terminated: the debuggee exited. */
static void e_d_dap_on_terminated(void *ud)
{
 (void)ud;
 if (g_dap_fenster)
  e_d_p_message("Program exited.", g_dap_fenster, 1);
}

/* Disconnect the adapter and reset bridge state.  Called from e_d_quit before
   the common e_d_quit_basic cleanup, and on any start/run failure. */
static void e_d_dap_quit(FENSTER *f)
{
 (void)f;
 if (g_dap)
 {
  e_dap_close(g_dap);
  g_dap = NULL;
 }
 if (g_bsp)
 {
  /* close the DAP socket first (done above), THEN the build server that hosts
     the adapter -- killing it earlier would drop the DAP connection mid-quit. */
  e_bsp_close(g_bsp);
  g_bsp = NULL;
 }
 g_dap_fenster = NULL;
 g_dap_just_started = 0;
 g_dap_stop_file[0] = '\0';
 g_dap_stop_line = -1;
}

/* Compile-first DAP language (Rust): build the source to a debuggable binary
   with the configured compiler.  dlv builds Go itself; here xwpe drives the
   one-shot compile (compiler + flags from the file's Options entry) so the
   adapter (gdb) gets a ready binary.  On success *binary is the executable; on
   failure the compiler diagnostics are shown in Messages and -1 returned. */
static int e_d_dap_compile(FENSTER *f, char *binary, size_t binsz)
{
 char src[700], stem[400], errf[600], cmd[2200], line[512];
 const char *cc, *flags;
 FILE *fp;

 e_check_c_file_w(f);                 /* set e_s_prog to this file's compiler */
 cc = (e_s_prog.compiler && e_s_prog.compiler[0]) ? e_s_prog.compiler : "rustc";
 flags = (e_s_prog.comp_str && e_s_prog.comp_str[0]) ? e_s_prog.comp_str : "-g";
 if (e_test_command(cc))
 {
  char m[160];
  sprintf(m, "Compiler '%s' not in PATH.", cc);
  e_error(m, 0, f->fb);
  return(-1);
 }
 snprintf(stem, sizeof(stem), "%s", f->datnam);
 WpeStringCutChar(stem, '.');
 snprintf(binary, binsz, "%s%s.dbg", f->dirct, stem);
 snprintf(src, sizeof(src), "%s%s", f->dirct, f->datnam);
 snprintf(errf, sizeof(errf), "%s%s.dbgerr", f->dirct, stem);
 snprintf(cmd, sizeof(cmd), "%s %s -o '%s' '%s' 2>'%s'",
          cc, flags, binary, src, errf);
 e_d_p_message("Compiling for debug...", f, 1);
 if (system(cmd) != 0)
 {
  fp = fopen(errf, "r");
  while (fp && fgets(line, sizeof(line), fp))
  {
   size_t l = strlen(line);
   if (l && line[l - 1] == '\n')
    line[l - 1] = '\0';
   e_d_p_message(line, f, 1);
  }
  if (fp)
   fclose(fp);
  unlink(errf);
  e_error("Compilation failed -- see Messages.", 0, f->fb);
  return(-1);
 }
 unlink(errf);
 return(0);
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
 int i, dlen;

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
 g_dap_host.on_terminated = e_d_dap_on_terminated;
 g_dap_host.ud = NULL;
 g_dap_fenster = f;
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
 /* Compile-first (Rust): build the binary now and debug it.  Adapter-builds
    (Go): the directory itself is the "program" dlv compiles. */
 if (lang->compile)
 {
  if (e_d_dap_compile(f, binary, sizeof(binary)) != 0)
  {  g_dap_fenster = NULL;  return(-1);  }
  program = binary;
 }
 else
  program = dir;
 if (lang->bsp)
 {
  /* JVM/Scala: drive the BSP bootstrap (Bloop via scala-cli) to get a DAP
     endpoint, keep the build server alive in g_bsp, then connect the engine. */
  char bhost[64], mainclass[256], berr[256];
  int bport = 0;
  e_d_p_message("Starting Scala build server (BSP)...", f, 1);
  g_bsp = e_bsp_start(dir, f->datnam, bhost, sizeof(bhost), &bport,
                      mainclass, sizeof(mainclass), berr, sizeof(berr));
  if (!g_bsp)
  {
   e_error(berr[0] ? berr : "Could not start the Scala debug server.", 0, f->fb);
   g_dap_fenster = NULL;
   return(-1);
  }
  g_dap = e_dap_open_tcp(bhost, bport, program, lang->entry_func, &g_dap_host);
 }
 else if (lang->stdio)
  g_dap = e_dap_open_stdio(adapter, program, lang->entry_func, dir, &g_dap_host);
 else
  g_dap = e_dap_open(adapter, program, lang->entry_func, dir, &g_dap_host);
 if (!g_dap)
 {
  e_error("Could not start the debug adapter.", 0, f->fb);
  if (g_bsp)
  {  e_bsp_close(g_bsp);  g_bsp = NULL;  }
  g_dap_fenster = NULL;
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
 if (e_dap_run(g_dap) != 0)           /* launch + configure + run to first stop */
 {
  e_error("Program failed to start under the debugger.", 0, f->fb);
  e_d_dap_quit(f);
  return(-1);
 }
 e_d_swtch = 3;
 g_dap_just_started = 1;              /* the first stop counts as the 1st Run */
 return(0);
}

/* True when the current stop is inside the user's program source tree.  After
   main() returns, a "step over" steps UP into the Go runtime (runtime/proc.go
   under GOROOT) -- legitimately, but it drops the user into unfamiliar code that
   looks like the debugger is jumping around at random.  This lets the step verbs
   decide to run on rather than show runtime internals.  Unknown paths default to
   "user code" so we never skip when we cannot tell. */
static int e_d_dap_in_user_code(void)
{
 size_t n;

 if (!g_dap_progdir[0] || !g_dap_stop_file[0])
  return(1);
 n = strlen(g_dap_progdir);
 return(!strncmp(g_dap_stop_file, g_dap_progdir, n) &&
        (g_dap_stop_file[n] == DIRC || g_dap_stop_file[n] == '\0'));
}

/* Ctrl-G R: run, or continue from the current stop.  The very first Run is
   the entry stop already reported by e_d_dap_start -- stay there when the user
   has no breakpoints (matching gdb's temp-break-at-main), continue otherwise. */
static int e_d_dap_run(FENSTER *f)
{
 if (!g_dap && e_d_dap_start(f) < 0)
  return(-1);
 if (g_dap_just_started)
 {
  g_dap_just_started = 0;
  if (e_d_nbrpts <= 0)
  {
   e_d_dap_paint_stop(f);             /* no breakpoints: rest at the entry */
   return(0);
  }
 }
 e_dap_step(g_dap, "continue");
 if (e_dap_ended(g_dap))
 {
  e_d_dap_quit(f);
  return(0);
 }
 e_d_dap_paint_stop(f);
 return(0);
}

/* F8 (sw!=0 -> step over) / F7 (sw==0 -> step into).  The first step after a
   fresh start lands on the entry stop without advancing, like the text
   backends; subsequent steps issue the DAP verb. */
static int e_d_dap_step(FENSTER *f, int sw)
{
 int guard = 0;

 if (!g_dap)
  return(-1);
 if (g_dap_just_started)
 {
  g_dap_just_started = 0;
  e_d_dap_paint_stop(f);              /* the entry stop, painted once */
  return(0);
 }
 e_dap_step(g_dap, sw ? "next" : "stepIn");
 /* Step-over (F8) must never wander into the language runtime: once main()
    returns there is no user line left to step to, so "next" lands in
    runtime/proc.go under GOROOT.  When that happens, run on to the next user
    line or to program exit instead of showing runtime internals.  Step-into
    (F7) is left alone -- the user explicitly asked to descend.  The guard
    bounds it so a pathological adapter cannot spin forever.  Because the paint
    is deferred (see e_d_dap_on_stopped), the intermediate runtime stop never
    reaches the screen. */
 if (sw)
  while (!e_dap_ended(g_dap) && !e_d_dap_in_user_code() && guard++ < 100)
   e_dap_step(g_dap, "continue");
 if (e_dap_ended(g_dap))
 {
  e_d_dap_quit(f);
  return(0);
 }
 e_d_dap_paint_stop(f);
 return(0);
}

/* Rebuild the Watches window with current values via evaluate(context=watch).
   Mirrors e_d_p_watches's window handling but sources each value from the
   adapter instead of a debugger pipe. */
static int e_d_dap_watches(FENSTER *f, int sw)
{
 ECNT *cn = f->ed;
 BUFFER *b;
 int iw, l;
 char *val, *line;

 e_d_switch_out(0);
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
 for (l = 0; l < e_d_nwtchs; l++)
 {
  val = g_dap ? e_dap_evaluate(g_dap, e_d_swtchs[l]) : NULL;
  line = WpeMalloc(strlen(e_d_swtchs[l]) + (val ? strlen(val) : 13) + 4);
  if (val)
   sprintf(line, "%s: %s", e_d_swtchs[l], val);
  else
   sprintf(line, "%s: <no value>", e_d_swtchs[l]);
  e_d_nrwtchs[l] = b->mxlines;
  print_to_end_of_buffer(b, line, b->mx.x);
  WpeFree(line);
  if (val)
   free(val);
 }
 e_new_line(b->mxlines, b);
 fk_cursor(1);
 if (sw && iw != cn->mxedt)
  e_switch_window(cn->edt[iw], cn->f[cn->mxedt]);
 else
  e_rep_win_tree(cn);
 return(0);
}

int e_start_debug(FENSTER *f)
{
 ECNT *cn = f->ed;
 int i, file;
 char estr[128];

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
 if (e_p_make(f))
  return(-1);
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
  strcpy(e_d_file, cn->f[i]->datnam);
  f = cn->f[i];
 }
 else if (e__project)
 {
  strcpy(e_d_file, cn->f[cn->mxedt-1]->datnam);
  f = cn->f[cn->mxedt-1];
 }
 for (i = 0; i < SVLINES; i++)
 {  e_d_sp[i] = e_d_out_str[i];  e_d_out_str[i][0] = '\0';  }
 /* Auto-select debugger by file extension.
    gdb cannot debug Java bytecode or Python scripts. */
 { int _l = strlen(e_d_file);
   if (_l > 5 && !strcmp(e_d_file + _l - 5, ".java"))
   {
    if (e_deb_type != DEB_JDB)
     e_error("Java file: switching to jdb debugger.", -1, f->fb);
    e_deb_type = DEB_JDB;
   }
   else if (_l > 3 && !strcmp(e_d_file + _l - 3, ".py"))
   {
    if (e_deb_type != DEB_PDB)
     e_error("Python file: switching to pdb debugger.", -1, f->fb);
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
 else {  e_debugger = "gdb";  e_deb_swtch = NULL;  }
 e_d_pid = 0;
 if (e_test_command(e_debugger))
 {
  sprintf(estr, "Debugger \'%s\' not in Path", e_debugger);
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
  if (e_s_prog.exe_name && e_s_prog.exe_name[0])
  {
   strcpy(estr, e_s_prog.exe_name);
  }
  else
  {
   strcpy(estr, f->datnam);
   if (e_deb_type == DEB_PDB || e_deb_type == DEB_A68G)
   {
    /* pdb/a68g: debug the source file (.py/.a68) with full path.
       e_d_file has the basename; we need dirct + datnam for the
       full path so pdb can find it from any working directory. */
    int _fi;
    for (_fi = cn->mxedt; _fi > 0; _fi--)
     if (e_check_c_file_w(cn->f[_fi])) break;
    if (_fi > 0)
     snprintf(estr, sizeof(estr), "%s/%s", cn->f[_fi]->dirct, cn->f[_fi]->datnam);
    else
     strcpy(estr, e_d_file);
   }
   else
   {
    WpeStringCutChar(estr, '.');
    /* Non-GNU compilers (fpc) produce executables without .e extension.
       GNU compilers use the .e convention from xwpe's link step. */
    if (!(e_s_prog.comp_sw & 1))
     strcat(estr, ".e");
   }
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

/* Rebuild the runtime breakpoint list from the surviving editor markers.
   When an a68g program finishes it EOFs and the whole session is torn down,
   which clears e_d_nbrpts -- but the breakpoint markers (s->brp) stay on the
   source windows.  Without this, a re-run (Ctrl-G R) would re-arm nothing and
   sail past the breakpoints.  gdb never needs this because program exit does
   not quit gdb.  No-op when the runtime list is already populated (the normal
   first run, where Ctrl-G B filled both). */
static void e_d_a68g_rearm_breakpoints(FENSTER *f)
{
 ECNT *cn = f->ed;
 int i, k;

 if (e_d_nbrpts > 0)
  return;
 for (i = cn->mxedt; i > 0; i--)
 {
  SCHIRM *s = cn->f[i]->s;
  if (!DTMD_ISTEXT(cn->f[i]->dtmd) || !s->brp || s->brp[0] <= 0)
   continue;
  for (k = 1; k <= s->brp[0]; k++)
  {
   e_d_brk_grow();
   e_d_sbrpts[e_d_nbrpts - 1] = MALLOC(strlen(cn->f[i]->datnam) + 1);
   strcpy(e_d_sbrpts[e_d_nbrpts - 1], cn->f[i]->datnam);
   e_d_ybrpts[e_d_nbrpts - 1] = s->brp[k] + 1;   /* s->brp is 0-based */
  }
 }
}

int e_run_debug(FENSTER *f)
{
 ECNT *cn = f->ed;
 int kbdflgs, ret;

 jdb_trace("e_run_debug: entry, e_d_swtch=%d, e_deb_type=%d\n",
           e_d_swtch, e_deb_type);
 if (e_d_swtch < 1 && (ret = e_start_debug(f)) < 0) return(ret);
 if (e_d_swtch < 2)
 {
  kbdflgs = fcntl(efildes[0], F_GETFL, 0);
  fcntl( efildes[0], F_SETFL, kbdflgs | O_NONBLOCK);
  kbdflgs = fcntl(wfildes[0], F_GETFL, 0);
  fcntl( wfildes[0], F_SETFL, kbdflgs | O_NONBLOCK);

  jdb_trace("e_run_debug: reading banner...\n");
  /* pdb (and other interpreted debuggers) need time to start before
     their banner is available on stdout.  Wait for data with poll().
     a68g --monitor compiles the source first, so it needs the same grace. */
  if (e_deb_type == DEB_PDB || e_deb_type == DEB_A68G)
  {
   struct pollfd _pfd = { .fd = wfildes[0], .events = POLLIN };
   poll(&_pfd, 1, 3000);  /* wait up to 3s for the debugger to write its banner */
  }
  if (e_d_dum_read() == -1) return(-1);
  jdb_trace("e_run_debug: banner read OK\n");
  /* Disable gdb's confirmation prompts ("Start it from the beginning?",
     "Kill the program being debugged?", etc.) to prevent busy loops
     when xwpe sends commands that gdb would otherwise block on. */
  if (e_deb_type == DEB_GDB)
  {
   e_d_send_cmd("set confirm off\n");
   if (e_d_dum_read() == -1) return(-1);
   if (e_d_pty_master >= 0)
   {
    char _tty_cmd[160];
    sprintf(_tty_cmd, "set inferior-tty %s\n", e_d_pty_slave_name);
    e_d_send_cmd(_tty_cmd);
    if (e_d_dum_read() == -1) return(-1);
   }
  }
  if (e_deb_type == DEB_XDB)
  {
   e_d_send_cmd("sm\n");
   if (e_d_dum_read() == -1) return(-1);
  }
  /* a68g: a finished program tore down the session and cleared e_d_nbrpts;
     rebuild it from the surviving markers so a re-run re-arms the breakpoints. */
  if (e_deb_type == DEB_A68G)
   e_d_a68g_rearm_breakpoints(cn->f[cn->mxedt]);
  jdb_trace("e_run_debug: setting breakpoints (nbrpts=%d)...\n", e_d_nbrpts);
  if (e_make_breakpoint(cn->f[cn->mxedt], 1) == -1) return(-1);
  jdb_trace("e_run_debug: breakpoints set OK, e_d_swtch -> 2\n");
  e_d_swtch = 2;
#ifndef NO_XWINDOWS
  if (WpeIsXwin())
  {
   XSetInputFocus(WpeXInfo.display, WpeXInfo.window,
     RevertToParent, CurrentTime);
   XFlush(WpeXInfo.display);
  }
#endif
 }
 return(0);
}

/*  Run  */
int e_deb_run(FENSTER *f)
{
 ECNT *cn = f->ed;
 char eing[256];
 int ret, len, prsw = 0, main_brk = 0;

 jdb_trace("e_deb_run: entry, e_d_swtch=%d, e_deb_type=%d\n",
           e_d_swtch, e_deb_type);
 if (e_d_swtch < 2 && (ret = e_run_debug(f)) < 0)
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
   sprintf(eing, "tty error: %s", e_d_tty);
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
    sprintf(eing, "run %s > %s\n", e_prog.arguments, e_d_tty);
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
    /* jdb: plain "run" -- no redirect, no arguments through run */
    strcpy(eing, "run\n");
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
     sprintf(eing, "r %s\n", e_prog.arguments);
    else
     strcpy(eing, "r\n");
   }
   else
   {
    if (e_prog.arguments)
     sprintf(eing, "r %s > %s\n", e_prog.arguments, e_d_tty);
    else
     sprintf(eing, "r > %s\n", e_d_tty);
   }
  }
  else
  {
   if (e_deb_type == DEB_JDB)
    strcpy(eing, "cont\n");
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
 jdb_trace("e_deb_run: sending '%s' (prsw=%d)\n", eing, prsw);
 e_d_send_cmd(eing);
 if ((e_deb_type == DEB_JDB || e_deb_type == DEB_PDB) && !prsw)
 {
  /* jdb: "run" returns "> " immediately but the VM starts
     asynchronously. Use poll() with timeout to wait for the
     breakpoint prompt "main[1] " without busy-looping.
     Technique from JDEE (Emacs jdb integration): accumulate
     output and scan for "Breakpoint hit" or the prompt pattern.
     poll() avoids CPU waste during JVM startup (~2-3 seconds). */
  struct pollfd _pfd = { .fd = wfildes[0], .events = POLLIN };
  char _jbuf[DEB_STEP_BUF];
  int _jlen = 0, _found = 0, _n;

  jdb_trace("e_deb_run: entering Phase 2 poll loop for jdb\n");
  while (!_found)
  {
   int _pret = poll(&_pfd, 1, 10000);
   if (_pret <= 0)
   {  jdb_trace("e_deb_run: poll timeout!\n");
      e_error("jdb: VM startup timeout.", 0, f->fb);
      e_d_quit(f); return(-1);  }
   _n = read(wfildes[0], _jbuf + _jlen, sizeof(_jbuf) - _jlen - 1);
   if (_n == 0)
   {  /* EOF: jdb process exited */
      jdb_trace("e_deb_run: EOF on pipe (jdb exited)\n");
      e_d_quit(f); return(-1);  }
   if (_n < 0) continue;
   _jlen += _n;
   _jbuf[_jlen] = '\0';
   jdb_trace("e_deb_run: poll read %d bytes total=%d: [%s]\n",
             _n, _jlen, _jbuf);
   if (strstr(_jbuf, "] "))
    _found = 1;
   if (strstr(_jbuf, "exited") || strstr(_jbuf, "Unable to"))
   {  jdb_trace("e_deb_run: error/exit detected in output, quitting\n");
      e_d_quit(f); return(-1);  }
  }
  jdb_trace("e_deb_run: Phase 2 done, _found=%d\n", _found);
  /* Parse jdb breakpoint output to position cursor.
     jdb format: "Breakpoint hit: ..., line=N bci=0\nN    code\nmain[1] "
     Extract line number and navigate to it. */
  { char *_lp = strstr(_jbuf, "line=");
    if (_lp)
    { int _line = atoi(_lp + 5);
      BUFFER *_b = cn->f[cn->mxedt]->b;
      SCHIRM *_s = cn->f[cn->mxedt]->s;
      if (_line > 0 && _line <= _b->mxlines)
      { _s->da.y = _b->b.y = _line - 1;
        _s->da.x = _b->b.x = 0;
        _s->de.x = MAXSCOL;
        e_schirm(cn->f[cn->mxedt], 1);
        e_cursor(cn->f[cn->mxedt], 1);
      }
    }
  }
  e_d_swtch = 3;
  e_d_switch_out(0);
  return(0);  /* skip e_read_output -- jdb output already parsed */
 }
 else if (e_deb_type == DEB_GDB || ((e_deb_type == DEB_DBX  || e_deb_type == DEB_XDB) && !prsw))
 {
  while((ret = e_d_line_read(wfildes[0], eing, 256, 0, 0)) == 2 ||
    !eing[0] || (e_deb_type == DEB_GDB && prsw && ((len = (strlen(eing)-12)) < 0 ||
    strcmp(eing + len, e_d_msg[ERR_CONTINUE]))) ||
    (e_d_swtch < 3 && ((e_deb_type == DEB_GDB && strncmp(e_d_msg[ERR_STARTPROG],eing, 17)) ||
    (e_deb_type == DEB_DBX && strncmp("Running:",eing, 8)))))
  {
   if (ret == 2)
    e_d_error(eing);
   else if (ret < 0)
    return(e_d_quit(f));
  }
 }
 if (!prsw)
  e_d_p_message(eing, f, 0);
 if (e_d_swtch < 3 && ((e_deb_type == DEB_GDB && strncmp(e_d_msg[ERR_STARTPROG],eing, 17)) ||
   (e_deb_type == DEB_DBX && strncmp("Running:",eing, 8))))
 {
  e_d_quit(f);
  return(e_error(e_d_msg[ERR_CANTPROG], 0, f->fb));
 }
 e_d_swtch = 3;
 if (e_d_pty_master >= 0 && e_deb_type == DEB_GDB)
 {
  e_d_accum_init(f, main_brk);
  return(0);
 }
 if (e_deb_type == DEB_A68G)
 {
  /* a68g shares one stream and exits on completion (EOF); the poll-based
     reader captures program output and quits cleanly -- e_read_output's
     prompt loop would return -1 on that EOF before parsing the output.
     (No e_mk_brk_main for a68g, so main_brk is always 0 here.) */
  return(e_d_a68g_step_complete(f));
 }
 { int _ro = e_read_output(f);
   if (main_brk)
    e_mk_brk_main(f, main_brk);
   return(_ro);
 }
}

int e_deb_trace(FENSTER *f)
{
 return(e_d_step_next(f, 0));
}

int e_deb_next(FENSTER *f)
{
 return(e_d_step_next(f, 1));
}

/* Read and apply a jdb step/next response: poll for the "] " prompt, jump the
   editor cursor to the reported source line, and capture any program output
   (between the "> " prompt and the next jdb protocol line) for Ctrl-G P.
   Returns 0, or e_d_quit(f) if the program or jdb exited. */
static int e_d_jdb_step_complete(FENSTER *f)
{
 struct pollfd _pfd = { .fd = wfildes[0], .events = POLLIN };
 char _jbuf[DEB_STEP_BUF];
 int _jlen = 0, _n, _found = 0;

 while (!_found)
 {
  if (poll(&_pfd, 1, DEB_STEP_POLL_MS) <= 0) break;
  _n = read(wfildes[0], _jbuf + _jlen, sizeof(_jbuf) - _jlen - 1);
  if (_n == 0)
   return(e_d_quit(f));   /* EOF: jdb exited cleanly -- quit like gdb does */
  if (_n < 0) continue;
  _jlen += _n;
  _jbuf[_jlen] = '\0';
  jdb_trace("e_d_step_next: poll read %d bytes total=%d: [%s]\n",
            _n, _jlen, _jbuf);
  if (strstr(_jbuf, "] "))
   _found = 1;
  if (strstr(_jbuf, "exited"))
  {  e_error("Program exited. Debugger stopped.", 0, f->fb);
     return(e_d_quit(f));  }
 }
 if (_found)
 {
  char *_lp = strstr(_jbuf, "line=");
  if (_lp)
  { int _line = atoi(_lp + 5);
    BUFFER *_b = f->ed->f[f->ed->mxedt]->b;
    SCHIRM *_s = f->ed->f[f->ed->mxedt]->s;
    if (_line > 0 && _line <= _b->mxlines)
    { _s->da.y = _b->b.y = _line - 1;
      _s->da.x = _b->b.x = 0;
      _s->de.x = MAXSCOL;
      e_schirm(f->ed->f[f->ed->mxedt], 1);
      e_cursor(f->ed->f[f->ed->mxedt], 1);
    }
  }
  /* jdb step response: "Step completed: ..., line=N\nN    code\nmain[1] "
     Extract line from the last numbered line */
  if (!_lp)
  { char *_nl = strrchr(_jbuf, '\n');
    if (_nl) { int _line = atoi(_nl + 1);
      if (_line > 0) {
       BUFFER *_b = f->ed->f[f->ed->mxedt]->b;
       SCHIRM *_s = f->ed->f[f->ed->mxedt]->s;
       if (_line <= _b->mxlines)
       { _s->da.y = _b->b.y = _line - 1;
         _s->da.x = _b->b.x = 0;
         _s->de.x = MAXSCOL;
         e_schirm(f->ed->f[f->ed->mxedt], 1);
         e_cursor(f->ed->f[f->ed->mxedt], 1);
       }
      }
    }
  }
 }
 /* Extract program output from jdb response buffer.
    Program output is text between "> " prompt and "Step completed:"
    or "Breakpoint hit:" -- anything that's not a jdb protocol line. */
 { char *_p = _jbuf, *_end;
   /* Skip past initial "> " prompt */
   if ((_p = strstr(_jbuf, "> ")) != NULL) _p += 2;
   else _p = _jbuf;
   /* Find where jdb protocol resumes */
   _end = strstr(_p, "Step completed:");
   if (!_end) _end = strstr(_p, "Breakpoint hit:");
   if (!_end) _end = strstr(_p, "main[");
   if (_end && _end > _p)
   { int _olen = _end - _p;
     /* Strip leading/trailing whitespace */
     while (_olen > 0 && (_p[0] == '\n' || _p[0] == '\r')) { _p++; _olen--; }
     while (_olen > 0 && (_p[_olen-1] == '\n' || _p[_olen-1] == '\r')) _olen--;
     if (_olen > 0)
       e_d_prog_output_append_line(_p, _olen);
   }
 }
 e_d_switch_out(0);
 return(0);
}

/* Read and apply a pdb step/next response: poll for the "(Pdb) " prompt,
   capture program output for Ctrl-G P, and jump the cursor to the reported
   source line.  When pdb stops in an internal Python frame (<string>,
   <frozen ...>) it steps again automatically until it reaches user code or the
   program exits.  Returns 0, or e_d_quit(f) on program exit. */
static int e_d_pdb_step_complete(FENSTER *f)
{
 struct pollfd _pfd = { .fd = wfildes[0], .events = POLLIN };
 char _buf[DEB_STEP_BUF];
 int internal_frame = 1;

 while (internal_frame)
 {
  int _len = 0, _n, _found = 0;
  internal_frame = 0;
  while (!_found)
  {
   if (poll(&_pfd, 1, DEB_STEP_POLL_MS) <= 0) break;
   _n = read(wfildes[0], _buf + _len, sizeof(_buf) - _len - 1);
   if (_n == 0) return(e_d_quit(f));  /* EOF: pdb exited */
   if (_n < 0) continue;
   _len += _n;
   _buf[_len] = '\0';
   if (strstr(_buf, "(Pdb) ")) _found = 1;
  }
  if (!_found)
   break;
  /* Capture program output from EVERY step response.
     Any line that isn't pdb metadata (prompt, active line, debugger
     messages) is program output (e.g. print() results). */
  { char _line[256];
    int _bi = 0;
    while (_buf[_bi])
    {
     int _li = 0;
     while (_buf[_bi] && _buf[_bi] != '\n' && _li < 255)
      _line[_li++] = _buf[_bi++];
     _line[_li] = '\0';
     if (_buf[_bi] == '\n') _bi++;
     if (_li == 0) continue;
     /* Skip pdb metadata */
     if (_line[0] == '>' || !strncmp(_line, "-> ", 3) ||
         !strncmp(_line, "(Pdb)", 5) ||
         !strncmp(_line, "--Return--", 10) ||
         !strncmp(_line, "--Call--", 8) ||
         strstr(_line, "The program"))
      continue;
     /* Program output -- save for Ctrl-G P */
     e_d_prog_output_append_line(_line, _li);
    }
  }
  /* Check for program exit -- pdb auto-restarts after finish */
  if (strstr(_buf, "The program finished"))
  {
   e_d_switch_out(0);
   e_error("End of code. Ctrl-G P for output.", 0, f->fb);
   return(e_d_quit(f));
  }
  /* Parse "> file(line)func()" to extract file and line number.
     If stopped in an internal Python frame (<string>, <frozen ...>),
     step again automatically until we reach user code or program exit. */
  { char *_gt = strstr(_buf, "> ");
    if (_gt && _gt[2] == '<')
    {
     e_d_send_cmd("n\n");
     internal_frame = 1;   /* re-poll instead of the old goto */
     continue;
    }
    if (_gt)
    {
     char *_op = strchr(_gt + 2, '(');
     if (_op)
     {
      int _line = atoi(_op + 1);
      if (_line > 0)
      {
       SCHIRM *_s = f->ed->f[f->ed->mxedt]->s;
       _s->da.y = _line - 1;
       e_d_swtch = 3;
       e_cursor(f, 1);
       e_schirm(f, 1);
      }
     }
    }
  }
 }
 e_d_switch_out(0);
 return(0);
}

/* Return the source line a68g reports for the current stop, or -1.
   a68g prints the active line as "<lineno><whitespace><source text>"
   (e.g. "5           f := f * i").  A bare number with no following source
   text (or a ':'-terminated number) is not a position. */
static int e_d_a68g_parse_line(const char *s)
{
 int n = 0, k;

 if (!isdigit((unsigned char)s[0]))
  return(-1);
 for (k = 0; isdigit((unsigned char)s[k]); k++)
  n = n * 10 + (s[k] - '0');
 if (s[k] != ' ' && s[k] != '\t')
  return(-1);
 return(n > 0 ? n : -1);
}

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

/* Scan a chunk of a68g monitor output line by line: show every non-chatter,
   non-numbered line as program output -- in the Messages window (Ctrl-G P, the
   path gdb reaches via its pty) and in the User-Screen buffer (Alt-F5) -- and
   return the last reported source line number (or -1).  Shared by the step and
   continue paths so program output is never lost, including the final exit
   chunk that a68g emits just before EOF. */
static int e_d_a68g_scan(FENSTER *f, const char *buf)
{
 char _ln[256];
 int _bi = 0, _line = -1;

 while (buf[_bi])
 {
  int _li = 0, _pl;
  while (buf[_bi] && buf[_bi] != '\n' && _li < 255)
   _ln[_li++] = buf[_bi++];
  _ln[_li] = '\0';
  if (buf[_bi] == '\n') _bi++;
  _pl = e_d_a68g_parse_line(_ln);
  if (_pl > 0)
   _line = _pl;                          /* last numbered line wins */
  else if (!e_d_a68g_is_metadata(_ln))
  {
   e_d_p_message(_ln, f, 0);             /* Messages window (Ctrl-G P) */
   e_d_prog_output_append_line(_ln, _li); /* User Screen buffer (Alt-F5) */
  }
 }
 return(_line);
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
 int i, l;

 for (i = cn->mxedt; i > 0; i--)
 {
  char *nm = cn->f[i]->datnam;
  l = (int)strlen(nm);
  if (l > 4 && (!strcmp(nm + l - 4, ".a68") || !strcmp(nm + l - 4, ".alg")))
   break;
 }
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
}

/* Read and apply an a68g step/next/continue response: poll for the "(a68g) "
   prompt, jump the editor cursor to the reported source line, and capture any
   program output for Ctrl-G P.  a68g prints "Genie finished" and then exits
   (EOF) when the program ends; capture the final output and quit cleanly.
   Returns 0, or e_d_quit(f) if the program or a68g exited. */
static int e_d_a68g_step_complete(FENSTER *f)
{
 struct pollfd _pfd = { .fd = wfildes[0], .events = POLLIN };
 char _buf[DEB_STEP_BUF];
 int _len = 0, _n, _found = 0, _eof = 0, _line;

 while (!_found)
 {
  if (poll(&_pfd, 1, DEB_STEP_POLL_MS) <= 0) break;
  _n = read(wfildes[0], _buf + _len, sizeof(_buf) - _len - 1);
  if (_n == 0) {  _eof = 1;  break;  }   /* EOF: a68g exited */
  if (_n < 0) continue;
  _len += _n;
  _buf[_len] = '\0';
  if (strstr(_buf, "(a68g) ")) _found = 1;
 }
 _buf[_len] = '\0';
 _line = e_d_a68g_scan(f, _buf);
 e_d_a68g_cur_line = _line;   /* drives line-granular stepping in e_d_step_next */
 if (_eof || strstr(_buf, "Genie finished"))
 {
  e_d_switch_out(0);
  e_error("End of code. Ctrl-G P for output.", 0, f->fb);
  return(e_d_quit(f));
 }
 if (!_found)
 {  e_d_switch_out(0);  return(0);  }
 if (_line > 0)
  e_d_a68g_goto(f, _line);
 else
  e_d_switch_out(0);
 /* Re-evaluate the Watches window at the new stop, so a stepped variable shows
    its current value (a68g is idle at the prompt now, ready for `evaluate`).
    gdb does this from its stop handlers; the a68g step path must do it too. */
 if (e_d_nwtchs > 0)
  e_d_p_watches(f, 0);
 return(0);
}

/* a68g steps by "interruptable unit", so one source line (e.g. a print with
   several arguments) reports the same line number several times in a row.  That
   forces the user to press F8 many times on the last line before the program
   ends.  Make Step Over (F8) line-granular like gdb/pdb: keep issuing "next"
   while a68g reports the same source line, so one keypress advances to the next
   line -- and one press on the final line runs the program to completion.  Step
   Into (F7, sw == 0) stays unit-granular so you can still inspect sub-steps. */
static int e_d_a68g_do_step(FENSTER *f, int sw)
{
 int from = e_d_a68g_cur_line, guard = 0, ret;

 do
 {
  e_d_send_cmd(sw ? "next\n" : "step\n");
  e_d_nstack = 0;
  ret = e_d_a68g_step_complete(f);
 }
 while (sw && e_d_swtch >= 3 && e_d_a68g_cur_line > 0 &&
        e_d_a68g_cur_line == from && ++guard < 256);
 return(ret);
}

int e_d_step_next(FENSTER *f, int sw)
{
 int ret, main_brk = 0;

 jdb_trace("e_d_step_next: entry, sw=%d, e_d_swtch=%d, e_deb_type=%d, "
           "e_d_nbrpts=%d\n", sw, e_d_swtch, e_deb_type, e_d_nbrpts);
 if (e_d_swtch < 2 && (ret = e_run_debug(f)) < 0)
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
  return e_d_a68g_do_step(f, sw);   /* line-granular Step Over */
 if (sw && (e_deb_type == DEB_GDB || e_deb_type == DEB_PDB)) e_d_send_cmd("n\n");
 else if (sw && (e_deb_type == DEB_SDB || e_deb_type == DEB_XDB)) e_d_send_cmd("S\n");
 else if (sw && (e_deb_type == DEB_DBX || e_deb_type == DEB_JDB)) e_d_send_cmd("next\n");
 else if (e_deb_type == DEB_DBX || e_deb_type == DEB_JDB) e_d_send_cmd("step\n");
 else e_d_send_cmd("s\n");
 e_d_nstack = 0;
 if (e_deb_type == DEB_JDB)
  return e_d_jdb_step_complete(f);
 if (e_deb_type == DEB_PDB)
  return e_d_pdb_step_complete(f);
 if (e_d_pty_master >= 0 && e_deb_type == DEB_GDB)
 {
  e_d_accum_init(f, 0);
  return(0);
 }
 return(e_read_output(f));
}

int e_d_goto_func(FENSTER *f, int flag)
{
 ECNT *cn = f->ed;
 BUFFER *b = cn->f[cn->mxedt]->b;
 int ret = 0, main_brk = 0;
 char str[128];

 if (e_deb_type != DEB_GDB)/* if gdb */
  return 0;
 if (e_d_swtch < 2 && (ret = e_run_debug(f)) < 0)
 {
  e_d_quit(f);
  if (ret == -1)
  {  e_show_error(0, f);  return(ret);  }
  return(e_error(e_d_msg[ERR_CANTDEBUG], 0, f->fb));
 }
 if (e_d_swtch < 3)
 {
  if ((main_brk = e_mk_brk_main(f, 0)) < -1)
   return(main_brk);
  ret = e_deb_run(f);
  e_mk_brk_main(f, main_brk);
  return(ret);
 }
 e_d_delbreak(f);
 e_d_switch_out(1);
 switch(flag)
 {
  case 'U':
   sprintf(str,"until %d\n",b->b.y+1);
   break;
  case 'F':
   sprintf(str,"finish\n");
   break;
  default:
   *str=0;
   break;
 }
 e_d_nstack = 0;
 if (*str)
 {
  e_d_send_cmd(str);
  ret=e_read_output(f);
  /* Executing Finish twice may not work properly. */
 }
 return ret;
}

int e_d_goto_cursor(FENSTER *f)
{
 return e_d_goto_func(f,'U');
}

int e_d_finish_func(FENSTER *f)
{
 return e_d_goto_func(f,'F');
}

int e_d_fst_check(FENSTER *f)
{
 int i, j, k = 0, l, ret = 0;

 e_d_switch_out(0);
 for (i = 0; i < SVLINES - 1; i++)
 {
  if ((e_deb_type != DEB_DBX && !strncmp(e_d_sp[i], e_d_msg[ERR_PROGEXIT], 14)) ||
    ((e_deb_type == DEB_DBX && !strncmp(e_d_sp[i], e_d_msg[ERR_PROGEXIT2], 14)) ||
    (e_deb_type == DEB_XDB && !strncmp(e_d_sp[i], e_d_msg[ERR_NORMALTERM], strlen(e_d_msg[ERR_NORMALTERM])))))
  {
   e_d_error(e_d_sp[i]);		/*  Program exited   */
   e_d_quit(f);
   return(i);
  }
  else if ((e_deb_type == DEB_GDB || e_deb_type == DEB_SDB) && !strncmp(e_d_sp[i], e_d_msg[ERR_PROGTERM], 18))
  {
   e_error(e_d_msg[ERR_PROGTERM], 0, f->fb);
   e_d_quit(f);
   return(i);
  }
  else if (e_deb_type == DEB_GDB && !strncmp(e_d_sp[i], e_d_msg[ERR_PROGSIGNAL], 23))
  {
   e_d_pr_sig(e_d_sp[i], f);
   return(i);
  }
  else if (e_deb_type == DEB_XDB && !strncmp(e_d_sp[i], e_d_msg[ERR_SOFTTERM], strlen(e_d_msg[ERR_SOFTTERM])))
  {
   e_d_pr_sig(e_d_sp[i], f);
   return(i);
  }
  else if (e_deb_type == DEB_DBX && (!strncmp(e_d_sp[i], e_d_msg[ERR_SIGNAL], 6) ||
    !strncmp(e_d_sp[i], e_d_msg[ERR_INTERRUPT], 9)))
  {
   e_d_pr_sig(e_d_sp[i], f);
   return(i);
  }
  else if (e_deb_type == DEB_SDB && strstr(e_d_sp[i], e_d_msg[ERR_SIGNAL]))
  {
   e_d_pr_sig(e_d_sp[i], f);
   return(i);
  }
  else if (e_deb_type == DEB_XDB && i == SVLINES-2 && strstr(e_d_sp[i], ": "))
  {
   e_d_pr_sig(e_d_sp[i-1], f);
   return(i-1);
  }
  else if (!strncmp(e_d_sp[i], e_d_msg[ERR_BREAKPOINT], 10) ||
    (e_deb_type == DEB_SDB && !strncmp(e_d_sp[i], e_d_msg[ERR_BREAKPOINT2], 10)))
  {
   if (e_deb_type == DEB_GDB)
   {
    for (j = SVLINES - 2; j > i; j--)      /*  Breakpoint   */
    {
     if ((ret = atoi(e_d_sp[j])) > 0)
      for(k = 0; e_d_sp[j][k] && isdigit(e_d_sp[j][k]); k++)
       ;
     if (e_d_sp[j][k] == '\t')
      break;
    }
    if (j > i)
    {
     for (k = strlen(e_d_sp[j-1]); k >= 0 && e_d_sp[j-1][k] != ':'; k--)
      ;
     if (k >= 0 && atoi(e_d_sp[j-1]+k+1) == ret)
     {
      if (e_make_line_num(e_d_sp[j-1], e_d_sp[SVLINES-1]) >= 0)
       strcpy(e_d_file, e_d_sp[SVLINES-1]);
     }
     if (e_d_p_watches(f, 0) == -1)
      return(-1);
     e_d_goto_break(e_d_file, ret, f);
     return(i > 0 ? i-1 : 0);
    }
   }
   else if (e_deb_type == DEB_SDB)	      /*  Breakpoint   */
   {
    if(!strncmp(e_d_sp[i]+10, " at", 3) &&
      (ret = e_make_line_num(e_d_sp[i+1], e_d_sp[SVLINES-1])) >= 0)
    {
     strcpy(e_d_file, e_d_sp[SVLINES-1]);
     if (e_d_p_watches(f, 0) == -1)
      return(-1);
     e_d_goto_break(e_d_file, ret, f);
     return(i);
    }
   }
  }
  else if (e_deb_type == DEB_DBX && !strncmp(e_d_sp[i], e_d_msg[ERR_STOPPEDIN], 10))
  {
   for (j = i + 1; j < SVLINES - 1; j++)		      /*  Breakpoint   */
   {
    for (k = 0; e_d_sp[j][k] == ' ' && e_d_sp[j][k] != '\0'; k++)
     ;
    if ((ret = atoi(e_d_sp[j]+k)) > 0)
    {
     if (!strstr(e_d_sp[j-1], " line "))
      break;
     for (k = strlen(e_d_sp[j-1]); k >= 0 && e_d_sp[j-1][k] != '\"'; k--)
      ;
     for(k--; k >= 0 && e_d_sp[j-1][k] != '\"'; k--)
      ;
     if (k >= 0)
     {
      for(k++, l = 0; e_d_sp[j-1][k] != '\0' && e_d_sp[j-1][k] != '\"';
        k++, l++)
       e_d_file[l] = e_d_sp[j-1][k];
      e_d_file[l] = '\0';
     }
     if (e_d_p_watches(f, 0) == -1)
      return(-1);
     e_d_goto_break(e_d_file, ret, f);
     return(i);
    }
   }
  }
 }
 return(-2);
}

int e_d_snd_check(FENSTER *f)
{
 int i, j, k, ret;

 e_d_switch_out(0);
 for (i = SVLINES - 2; i >= 0; i--)
 {
  if (e_deb_type == DEB_GDB && (ret = atoi(e_d_sp[i])) > 0)
  {
   for (k = 0; e_d_sp[i][k] && isdigit(e_d_sp[i][k]); k++)
    ;
   if (e_d_sp[i][k] != '\t')
    continue;
   if (i > 0)
   {
    for (k = strlen(e_d_sp[i-1]); k >= 0 && e_d_sp[i-1][k] != ':'; k--)
     ;
    if (k >= 0 && atoi(e_d_sp[i-1]+k+1) == ret)
    {
     i--;
     if (e_make_line_num(e_d_sp[i], e_d_sp[SVLINES-1]) >= 0)
      strcpy(e_d_file, e_d_sp[SVLINES-1]);
     do
     {
      for (; k >= 0 && e_d_sp[i][k] != ')'; k--)
       ;
      if (k < 0)
      {  i--;  k = strlen(e_d_sp[i]);  }
      else
       break;
     }  while (i >= 0);
     do
     {
      for (j = 1, k--; k >= 0 && j > 0; k--)
      {
       if (e_d_sp[i][k] == ')')
        j++;
       else if (e_d_sp[i][k] == '(')
        j--;
      }
      if (k < 0)
      {  i--;  k = strlen(e_d_sp[i]);  }
     } while (i >= 0 && j > 0);
     if (k == 0 && i > 0)
     {  i--;  k = strlen(e_d_sp[i]);  }
     for (k--; k >= 0 && isspace(e_d_sp[i][k]); k--)
      ;
     if (k < 0 && i > 0)
      i--;
    }
   }
   if (e_d_p_watches(f, 0) == -1)
    return(-1);
   e_d_goto_break(e_d_file, ret, f);
   return(i);
  }
  else if (e_deb_type == DEB_SDB && (ret = atoi(e_d_sp[i])) > 0)
  {
   for (; i > 0; i--)
   {
    for (j = strlen(e_d_sp[i-1])-1; j >= 0 && isspace(e_d_sp[i-1][j]); j--)
     ;
    if (j < 0)
    {  i--;  continue;  }
    for (j--; j >= 0 && !isspace(e_d_sp[i-1][j]); j--)
     ;
    if (j < 0)
     i--;
    for (j--; j >= 0 && isspace(e_d_sp[i-1][j]); j--)
     ;
    if (j < 0)
     i--;
    for (j--; j >= 0 && !isspace(e_d_sp[i-1][j]); j--)
     ;
    if (!strncmp(e_d_sp[i-1]+j+1, "in ", 3))
    {
     strcpy(e_d_file, e_d_sp[i-1]+j+4);
     for (k = i+2; !e_d_file[0]; k++)
      strcpy(e_d_file, e_d_sp[i-1]+j+4);
     for (k = strlen(e_d_file)-1; k >= 0 && isspace(e_d_file[k]); k--)
      ;
     e_d_file[k+1] = '\0';
     if (e_d_p_watches(f, 0) == -1)
      return(-1);
     e_d_goto_break(e_d_file, ret, f);
     return(i);
    }
   }
   return(-2);
  }
  else if (e_deb_type == DEB_XDB && i == SVLINES-2 &&
    (ret = e_make_line_num(e_d_sp[i], e_d_sp[SVLINES-1])) >= 0)
  {
   strcpy(e_d_file, e_d_sp[SVLINES-1]);
   if (e_d_p_watches(f, 0) == -1)
    return(-1);
   e_d_goto_break(e_d_file, ret, f);
   return(i);
  }
 }
 return(-2);
}

int e_d_trd_check(FENSTER *f)
{
 int ret;
 char str[256];

 str[0] = '\0';
 e_d_switch_out(0);
 if ((ret = e_d_pr_sig(str, f)) == -1) return(-1);
 else if (ret == -2) e_d_error(e_d_msg[ERR_NOSOURCE]);
 return(0);
}

int e_read_output(FENSTER *f)
{
 char *spt;
 int i, ret;

 jdb_trace("e_read_output: CALLED, e_deb_type=%d\n", e_deb_type);
 for (i = 0; i < SVLINES; i++)
 {  e_d_sp[i] = e_d_out_str[i];  e_d_out_str[i][0] = '\0';  }
 while ((ret = e_d_line_read(wfildes[0], e_d_sp[SVLINES-1], 256, 0, 0)) == 2)
  e_d_error(e_d_sp[SVLINES-1]);
 if (ret < 0) return(-1);
 e_d_switch_out(0);
 while(ret != 1)
 {
  spt = e_d_sp[0];
  for (i = 1; i < SVLINES; i++)
   e_d_sp[i-1] = e_d_sp[i];
  e_d_sp[SVLINES-1] = spt;
  do
  {
   while((ret = e_d_line_read(wfildes[0], e_d_sp[SVLINES-1], 256, 0, 0)) == 2)
    e_d_error(e_d_sp[SVLINES-1]);
   if (ret < 0) return(-1);
  } while(!ret && !*e_d_sp[SVLINES-1]);
 }
 for (i = 0; i < SVLINES; i++)
 {
  if (strstr(e_d_sp[i], "exited"))
  {
   e_d_pty_flush_to_messages(f);
   e_d_p_message("Program exited.", f, 0);
   e_d_quit(f);
   return(0);
  }
 }
 /* Flush the inferior's stdout (which is fully buffered because it
    goes to a pty, not a real terminal) and drain any output.
    This is the technique used by gdbgui and Eclipse CDT. */
 if (e_deb_type == DEB_GDB && e_d_pty_master >= 0)
 {
  char _flush_resp[256];
  e_d_send_cmd("call (void)fflush(0)\n");
  while (e_d_line_read(wfildes[0], _flush_resp, 256, 0, 0) == 0)
   ;
  e_d_pty_flush_to_messages(f);
 }
 /* pdb: parse "> file(line)func()" from the buffered output lines */
 if (e_deb_type == DEB_PDB)
 {
  int _found = 0;
  /* Check for program exit FIRST -- pdb restarts the program
     automatically after it finishes, so "The program finished" appears
     before the new "> file(line)" line. We must catch exit before
     navigating to the restart position. */
  for (i = 0; i < SVLINES; i++)
  {
   if (strstr(e_d_sp[i], "The program finished"))
   {
    /* Capture program output from the buffer before quitting */
    int _j;
    for (_j = 0; _j < SVLINES; _j++)
    {
     char *_s = e_d_sp[_j];
     if (_s[0] && !strstr(_s, "> ") && !strstr(_s, "-> ") &&
         !strstr(_s, "(Pdb)") && !strstr(_s, "The program"))
     {
      int _olen = strlen(_s);
      while (_olen > 0 && (_s[_olen-1] == '\n' || _s[_olen-1] == '\r')) _olen--;
      if (_olen > 0)
       e_d_prog_output_append_line(_s, _olen);
     }
    }
    e_error("End of code. Ctrl-G P for output.", 0, f->fb);
    return(e_d_quit(f));
   }
  }
  for (i = 0; i < SVLINES && !_found; i++)
  {
   char *_gt = strstr(e_d_sp[i], "> ");
   if (_gt)
   {
    char *_op = strchr(_gt + 2, '(');
    if (_op)
    {
     int _line = atoi(_op + 1);
     char _file[128];
     int _flen = _op - (_gt + 2);
     if (_flen > 0 && _flen < 127 && _line > 0)
     {
      strncpy(_file, _gt + 2, _flen);
      _file[_flen] = '\0';
      e_d_goto_break(_file, _line, f);
      e_d_swtch = 3;
      _found = 1;
     }
    }
   }
  }
  if (_found) return(0);
  /* Fall through to gdb checks as fallback */
 }
 if ((i = e_d_fst_check(f)) == -1) return(-1);
 if (i < 0 && (i = e_d_snd_check(f)) == -1) return(-1);
 if (i < 0 && (i = e_d_trd_check(f)) == -1) return(-1);
 if (i < 0)
 {
  /* Could not determine where the debugger stopped.  This typically
     happens when the program exited or gdb entered system code.
     Quit the debugger cleanly instead of showing multiple popups. */
  e_d_switch_out(0);
  e_error("Program exited. Debugger stopped.", 0, f->fb);
  return(e_d_quit(f));
 }
 return(0);
}

int e_d_pr_sig(char *str, FENSTER *f)
{
 int i, line = -1, ret = 0;
 char file[128], str2[256];

 if (str && str[0])
  e_d_error(str);
 for (i = 0; i < SVLINES; i++)
  e_d_out_str[i][0] = '\0';
 if (e_deb_type != DEB_SDB && e_deb_type != DEB_XDB)
 {
  e_d_send_cmd("where\n");
  for (i = 0; ((ret = e_d_line_read(wfildes[0], str, 256, 0, 1)) == 0 &&
    (line = e_make_line_num(str, file)) < 0) || ret == 2; i++)
  {
   if (!strncmp(str, e_d_msg[ERR_NOSTACK], 9))
   {
    e_error(e_d_msg[ERR_PROGEXIT], 0, f->fb);
    while (ret == 0 || ret == 2)
     if ((ret = e_d_line_read(wfildes[0], str2, 256, 0, 0)) == 2)
      e_d_error(str2);
    e_d_quit(f);
    return(0);
   }
   else if (ret == 2)
    e_d_error(str);
  }
 }
 else
 {
  e_d_send_cmd("t\n");
  for (i = 0; ((ret = e_d_line_read(wfildes[0], str, 256, 0, 1)) == 0 &&
    (line = e_make_line_num2(str, file)) < 0) || ret == 2; i++)
  {
   if (!strncmp(str, e_d_msg[ERR_NOPROCESS], 10))
   {
    e_error(e_d_msg[ERR_PROGEXIT], 0, f->fb);
    while (ret == 0 || ret == 2)
     if ((ret = e_d_line_read(wfildes[0], str2, 256, 0, 0)) == 2)
      e_d_error(str2);
    e_d_quit(f);
    return(0);
   }
   else if (ret == 2)
    e_d_error(str);
  }
 }
 if (ret == 1 && i == 0)
 {
  e_d_error(e_d_msg[ERR_PROGEXIT]);
  return(e_d_quit(f));
 }
 while (ret == 0 || ret == 2)
  if ((ret = e_d_line_read(wfildes[0], str2, 256, 0, 0)) == 2)
   e_d_error(str2);
 if (ret == -1)
  return(ret);
 if (line >= 0)
 {
  strcpy(e_d_file, file);
  /* Refresh the Watches window here too: a plain Step lands in this "find
     where we stopped via 'where'" path (e_d_trd_check -> e_d_pr_sig), not in
     the breakpoint branches, so without this the watch values would stay
     frozen at the value they had when the watch was first added. */
  if (e_d_p_watches(f, 0) == -1)
   return(-1);
  e_d_goto_break(file, line, f);
  return(0);
 }
 else
  return(-2);
}

int e_make_line_num(char *str, char *file)
{
 char *sp;
 int i, n, num;

 if (e_deb_type == DEB_GDB)
 {
  for (n = strlen(str); n >= 0 && str[n] != ':'; n--)
   ;
  if (n < 0)
   return(-1);
  for (i = n-1; i >= 0 && !isspace(str[i]); i--)
   ;
  for (n = i+1; str[n] != ':'; n++)
   file[n-i-1] = str[n];
  file[n-i-1] = '\0';
  return(atoi(str+n+1));
 }
 else if (e_deb_type == DEB_DBX)
 {
  if (!(sp = strstr(str, " line ")))
   return(-1);
  if (!(num = atoi(sp+6)))
   return(-1);
  for (i = 6;  sp[i] != '\"'; i++)
   ;
  sp += (i+1);
  for (i = 0; (file[i] = sp[i]) != '\"' && file[i] != '\0'; i++)
   ;
  if (file[i] == '\0')
   return(-1);
  file[i] = '\0';
  return(num);
 }
 else if (e_deb_type == DEB_XDB)
 {
  for (sp = str, i = 0; (file[i] = sp[i]) && sp[i] != ':'; i++)
   ;
  if (!sp[i])
   return(-1);
  file[i] = '\0';
  for (i++; sp[i] && sp[i] != ':'; i++)
   ;
  if (!sp[i])
   return(-1);
  for (i++; sp[i] && isspace(sp[i]); i++)
   ;
  if (!isdigit(sp[i]))
   return(-1);
  sp += i;
  return(atoi(sp));
 }
 else
 {
  for (i = 0; str[i] != '\0' && str[i] != ':'; i++)
   ;
  if ((!str[i]) || (num = atoi(str+i+1)) < 0)
   return(-1);
  e_d_send_cmd("e\n");
  while ((i = e_d_line_read(wfildes[0], str, 256, 0, 0)) ==  2)
   e_d_error(str);
  if (i < 0)
   return(-1);
  for (i = 0; str[i] != '\0' && str[i] != '\"'; i++)
   ;
  for (sp = str + i + 1, i = 0; (file[i] = sp[i]) && file[i] != '\"'; i++)
   ;
  file[i] = '\0';
  if (e_d_dum_read() == -1)
   return(-1);
  return(num);
 }
}

int e_make_line_num2(char *str, char *file)
{
 char *sp;
 int i;

 for (i = 0; str[i] != '[' && str[i] != '\0'; i++)
  ;
 if (!str[i]) return(-1);
 for (sp = str+i+1, i = 0; (file[i] = sp[i]) != ':' && file[i] != '\0'; i++)
  ;
 if (file[i] == '\0') return(-1);
 file[i] = '\0';
 for (i++; isspace(sp[i]); i++)
  ;
 return(atoi(sp+i));
}

int e_d_goto_break(char *file, int line, FENSTER *f)
{
 ECNT *cn = f->ed;
 BUFFER *b;
 SCHIRM *s;
 FENSTER ftmp;
 int i;
 char str[120];

/*   if(schirm != e_d_save_schirm) e_d_switch_out(0);  */
 e_d_switch_out(0);
 ftmp.ed = cn;
 ftmp.fb = f->fb;
 WpeFilenameToPathFile(file, &ftmp.dirct, &ftmp.datnam);
 for (i = 0; i < SVLINES; i++)
  e_d_out_str[i][0] = '\0';
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
   {
    e_d_send_cmd("tbreak main\n");
    if (e_d_dum_read() == -1) { e_d_quit(f); return(-1); }
    e_d_send_cmd("c\n");
    return(e_read_output(f));
   }
   sprintf(str, e_d_msg[ERR_CANTFILE], file);
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
 e_d_pty_flush_to_messages(f);
#ifndef NO_XWINDOWS
 if (WpeIsXwin())
  e_x_reclaim_focus();
#endif
 return(0);
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

int e_d_putchar(int c)
{
 if (!WpeIsXwin()) c = fk_putchar(c);
 else
 {
  char cc = c;
  c = write(wfildes[1], &cc, 1);
 }
 return(c);
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
   /* Map e_deb_type to radio button index:
      0=Gdb, 1=Sdb, 2=Dbx, 3=Jdb, 4=Pdb, 5=A68g (radio index)
      e_deb_type: 0=gdb, 1=sdb, 2=dbx, 3=xdb, 4=jdb, 5=pdb, 6=a68g */
   int _sel = (e_deb_type == DEB_A68G) ? 5 :
              (e_deb_type == DEB_PDB) ? 4 :
              (e_deb_type == DEB_JDB) ? 3 :
              (e_deb_type == DEB_XDB) ? 2 : e_deb_type;
 e_add_pswstr(0, 5, 3, 0, AltG, 0, "Gdb    ", o);
 e_add_pswstr(0, 5, 4, 0, AltS, 0, "Sdb    ", o);
#ifdef XDB
 e_add_pswstr(0, 5, 5, 0, AltX, 0, "Xdb    ", o);
#else
 e_add_pswstr(0, 5, 5, 0, AltD, 0, "Dbx    ", o);
#endif
 e_add_pswstr(0, 5, 6, 0, AltJ, 0, "Jdb    ", o);
 e_add_pswstr(0, 5, 7, 0, AltP, 0, "Pdb    ", o);
 e_add_pswstr(0, 5, 8, 0, AltA, _sel, "A68g   ", o);
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
  { int sel = o->pstr[0]->num;
#ifdef XDB
    if (sel == 2) e_deb_type = DEB_XDB;
    else if (sel == 3) e_deb_type = DEB_JDB;
    else if (sel == 4) e_deb_type = DEB_PDB;
    else if (sel == 5) e_deb_type = DEB_A68G;
    else e_deb_type = sel;
#else
    if (sel == 3) e_deb_type = DEB_JDB;
    else if (sel == 4) e_deb_type = DEB_PDB;
    else if (sel == 5) e_deb_type = DEB_A68G;
    else e_deb_type = sel;
#endif
  }
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
static int            g_lsp_quiet = 0;     /* suppress per-diagnostic Messages
                                              output (live polls show a summary) */
static int            g_lsp_last_err = -1; /* last reported diagnostic totals,  */
static int            g_lsp_last_warn = -1;/* so the live status is not spammed  */
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
typedef struct { int line, c0, c1, sev; } e_lsp_diag_mark;
static e_lsp_diag_mark g_diag_active[LSP_MAX_DIAG];   /* what the renderer shows */
static int             g_diag_nactive = 0;
static e_lsp_diag_mark g_diag_pending[LSP_MAX_DIAG];  /* filling this batch      */
static int             g_diag_npending = 0;

/* document-highlight spans: occurrences of the symbol under the cursor, marked
   on demand (Alt-Q L) in the found-word colour -- separate from diagnostics so
   the two can coexist (a highlighted name that also has a warning). */
static e_lsp_diag_mark g_hl_active[LSP_MAX_DIAG];
static int             g_hl_nactive = 0;

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

/* Called once per line by the renderer: are there any LSP decorations (problem
   marks or highlight spans) to draw for the file in `f`?  Keeps the
   per-character query (below) free of path work. */
int e_lsp_decor_active_for(FENSTER *f)
{
 char path[1200];
 if (!g_lsp || (g_diag_nactive == 0 && g_hl_nactive == 0))
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
 return(base);
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
 * drawn dimly after the line's text (the renderer calls e_lsp_inlay_eol_text per
 * line).  Unlike true IDE inlay hints we do NOT insert mid-line virtual text --
 * that would need a buffer<->screen column remap the 1993 cell grid has no layer
 * for -- so the hint sits at the line end, conveying the same information (": T")
 * without disturbing the cursor/editing/scroll invariants.  Off by default. */
#define LSP_MAX_INLAY 1024
static e_lsp_inlay_hint g_inlay_active[LSP_MAX_INLAY];
static int             g_inlay_nactive = 0;
static int             g_inlay_on = 0;    /* the toggle (per open file)         */

static void e_lsp_inlay_clear(void)
{
 int i;
 for (i = 0; i < g_inlay_nactive; i++)
  free(g_inlay_active[i].label);
 g_inlay_nactive = 0;
}

/* Snapshot the file's inferred-type hints into the renderer-owned overlay.  The
   engine's labels live in its session cache (clobbered by the next request), so
   copy them. */
static void e_lsp_inlay_fetch(FENSTER *f)
{
 static e_lsp_inlay_hint tmp[LSP_MAX_INLAY];
 int n, i, last;

 e_lsp_inlay_clear();
 if (!g_lsp || !f || !f->b)
  return;
 last = f->b->mxlines - 1;
 if (last < 0)
  last = 0;
 n = e_lsp_inlay_hints(g_lsp, g_lsp_file, 0, last, tmp, LSP_MAX_INLAY);
 for (i = 0; i < n && g_inlay_nactive < LSP_MAX_INLAY; i++)
 {
  if (!tmp[i].label || !tmp[i].label[0])
   continue;
  g_inlay_active[g_inlay_nactive].label = strdup(tmp[i].label);
  g_inlay_active[g_inlay_nactive].line = tmp[i].line;
  g_inlay_active[g_inlay_nactive].character = tmp[i].character;
  g_inlay_active[g_inlay_nactive].kind = tmp[i].kind;
  g_inlay_nactive++;
 }
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

/* The dim cell attribute for inlay text: grey foreground on the line's own
   background where colour is available; the base attribute under mono ncurses. */
int e_lsp_inlay_color(SCHIRM *s, int base)
{
 (void)s;
 if (WpeIsXwin() || col_num > 0)
  return(16 * (base / 16) + 3);   /* cyan fg on the line's bg: a readable,
                                     secondary "annotation" colour (dark grey
                                     was too faint on the Borland blue) */
 return(base);
}

/* The LSP languageId for this window's file, or NULL if none is wired. */
static const char *e_lsp_lang_for(FENSTER *f)
{
 int n;
 if (!f || !f->datnam)
  return(NULL);
 n = strlen(f->datnam);
 if (n > 6 && !strcmp(f->datnam + n - 6, ".scala"))
  return("scala");
 return(NULL);
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
  g_diag_npending++;
 }

 if (g_lsp_quiet)                   /* live poll: only the summary line shows */
  return;
 snprintf(buf, sizeof(buf), "%s:%d:%d: %s: %s",
          e_d_dap_basename(g_lsp_file), line + 1, ch + 1, kind, msg ? msg : "");
 if (g_lsp_fenster)
  e_d_p_message(buf, g_lsp_fenster, 1);
}

/* End of a publishDiagnostics batch: swap the freshly-collected marks in
   (atomic replace -- even when the totals are unchanged the positions may have
   moved), repaint the source window so the marks update live, and update the
   non-spammy "N errors, M warnings" status line. */
static void e_lsp_on_diag_summary(const char *path, int errors, int warnings,
                                  void *ud)
{
 char buf[120];
 (void)path; (void)ud;

 memcpy(g_diag_active, g_diag_pending,
        (size_t)g_diag_npending * sizeof(g_diag_active[0]));
 g_diag_nactive = g_diag_npending;
 g_diag_npending = 0;
 if (g_lsp_fenster && DTMD_ISTEXT(g_lsp_fenster->dtmd))
 {
  e_schirm(g_lsp_fenster, 1);       /* redraw text so marks appear/clear */
  e_cursor(g_lsp_fenster, 0);       /* keep the caret where it was */
 }

 if (errors == g_lsp_last_err && warnings == g_lsp_last_warn)
  return;                           /* totals unchanged -- skip the status line */
 g_lsp_last_err = errors;
 g_lsp_last_warn = warnings;
 if (!g_lsp_fenster)
  return;
 if (errors == 0 && warnings == 0)
  snprintf(buf, sizeof(buf), "LSP: no problems.");
 else
  snprintf(buf, sizeof(buf), "LSP: %d error(s), %d warning(s).", errors, warnings);
 e_d_p_message(buf, g_lsp_fenster, 0);   /* sw=0: do NOT steal focus from the
                                            editor -- the user keeps typing */
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
 if (!g_lsp_busy || !g_lsp_fenster)
  return;
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
 e_d_p_message("Metals Doctor updated (open its window to read it).", home, 0);
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

/* Major feature version of the JDK at `java_bin` (e.g. 21, 26, or 8 for a "1.8.0"
   string), or -1 if it cannot be determined.  Runs `java -version` and parses the
   first version token. */
static int e_jdk_major(const char *java_bin)
{
 char cmd[1200], line[256];
 FILE *fp;
 int major = -1;

 snprintf(cmd, sizeof(cmd), "'%s' -version 2>&1", java_bin);
 fp = popen(cmd, "r");
 if (!fp)
  return(-1);
 while (fgets(line, sizeof(line), fp))
 {
  char *v = strstr(line, "version \"");
  if (v)
  {
   v += 9;                                   /* past: version "            */
   if (strncmp(v, "1.", 2) == 0)
    major = atoi(v + 2);                      /* 1.8.0 -> 8                 */
   else
    major = atoi(v);                          /* 21.0.1 -> 21, 26.0.1 -> 26 */
   break;
  }
 }
 pclose(fp);
 return(major);
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

/* Metals' presentation compiler (hover / completion / go-to-definition) runs on
   the JVM Metals itself uses -- NOT the project's pinned build JVM.  The Scala 3
   compiler crashes at start-up on a too-new JDK (>= 24: "asTerm called on
   not-a-Term"), which makes every PC-driven action silently return empty (hover
   always "No hover information") while definition/diagnostics still work -- a
   confusing split.  So, unless the user already pinned a supported JAVA_HOME,
   detect a too-new default and repoint JAVA_HOME (and PATH) at an LTS JDK for the
   Metals child, and say what was done.  No-op when the default JDK is already
   fine or no LTS JDK is installed. */
static void e_lsp_pin_jdk(FENSTER *f)
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
  snprintf(msg, sizeof(msg), "Metals: default JDK %d is too new for the Scala "
           "compiler and no LTS JDK (17/21) was found -- set JAVA_HOME to one.",
           major);
  e_d_p_message(msg, f, 1);
  return;
 }
 setenv("JAVA_HOME", jdk, 1);
 oldpath = getenv("PATH");
 snprintf(newpath, sizeof(newpath), "%s/bin:%s", jdk, oldpath ? oldpath : "");
 setenv("PATH", newpath, 1);
 snprintf(msg, sizeof(msg), "Metals: pinned JAVA_HOME=%s (default JDK %d is too "
          "new for the Scala compiler).", jdk, major);
 e_d_p_message(msg, f, 1);
}

/* Lazily start the language server for f's file and surface its diagnostics.
   For Scala, `scala-cli setup-ide` is run first so Metals auto-connects to the
   build server.  The buffer is read from disk (save before invoking for the
   freshest view).  Returns 0 when a session is ready, -1 otherwise. */
static int e_lsp_ensure(FENSTER *f)
{
 const char *lang = e_lsp_lang_for(f);
 static e_lsp_host host;
 char dir[1024], path[1200], cmd[1400], *text;
 char *const argv[] = { "metals", NULL };
 int dl;

 if (!lang)
 {  e_error("Language server: unsupported file type.", 0, f->fb);  return(-1);  }
 snprintf(path, sizeof(path), "%s%s", f->dirct ? f->dirct : "./", f->datnam);
 if (g_lsp && !strcmp(g_lsp_file, path))
  return(0);                                   /* already open for this file */
 if (g_lsp)
 {  e_lsp_close(g_lsp);  g_lsp = NULL;  e_lsp_diag_clear();
    e_lsp_inlay_clear();  g_inlay_on = 0;  }  /* switched files: drop overlays */
 if (e_test_command(argv[0]))
 {  e_error("Metals not in PATH (cs install metals).", 0, f->fb);  return(-1);  }

 snprintf(dir, sizeof(dir), "%s", f->dirct ? f->dirct : ".");
 dl = strlen(dir);
 while (dl > 1 && dir[dl - 1] == DIRC)
  dir[--dl] = '\0';
 g_lsp_fenster = f;
 strncpy(g_lsp_file, path, sizeof(g_lsp_file) - 1);
 g_lsp_file[sizeof(g_lsp_file) - 1] = '\0';

 e_d_p_message("Starting language server (Metals)...", f, 1);
 if (!strcmp(lang, "scala"))
  e_lsp_pin_jdk(f);              /* keep Metals' Scala PC off a too-new JDK */
 /* </dev/null: never let the child share xwpe's interactive terminal stdin --
    it would consume/scramble the user's keystrokes typed during this blocking
    setup, leaving stray bytes that the editor later reads as commands. */
 snprintf(cmd, sizeof(cmd), "scala-cli setup-ide '%s' </dev/null >/dev/null 2>&1", dir);
 if (system(cmd) != 0)
  { /* non-fatal: Metals can still import the build */ }

 host.on_diagnostic = e_lsp_on_diag;
 host.on_diagnostics_summary = e_lsp_on_diag_summary;
 host.on_show_text = e_lsp_on_show_text;
 host.on_status = e_lsp_on_status;
 host.ud = NULL;
 g_lsp_last_err = g_lsp_last_warn = -1;   /* fresh session: force first report */
 free(g_lsp_doctor_last);                 /* fresh session: show its Doctor even */
 g_lsp_doctor_last = NULL;                 /* if identical to a previous session's */
 free(g_lsp_status_last);                 /* fresh session: status starts clean   */
 g_lsp_status_last = NULL;
 g_lsp_busy = 0;
 g_lsp = e_lsp_open(argv, dir, lang, &host);
 if (!g_lsp)
 {
  e_error("Could not start the language server.", 0, f->fb);
  g_lsp_file[0] = '\0';
  return(-1);
 }
 text = e_lsp_buffer_text(f);
 e_lsp_did_open(g_lsp, path, text ? text : "");
 e_lsp_did_focus(g_lsp, path);                 /* warm the PC so hover/completion work */
 e_lsp_synced_set(text ? text : strdup(""));   /* baseline = what we opened with */
 e_lsp_wait_diagnostics(g_lsp, path, 240000);  /* compile -> diags to Messages */
 /* The first start froze the UI for seconds while the JVM booted; discard any
    keys the user mashed during the freeze so they are not replayed afterwards as
    stray actions (e.g. a queued key landing on Run -> "not a C file"). */
 {
  extern void e_t_flush_input(void);
#ifndef NO_XWINDOWS
  extern void e_x_flush_input(void);
  if (WpeIsXwin())
   e_x_flush_input();
  else
#endif
   e_t_flush_input();
 }
 e_lsp_raise_source(f);   /* keep the code focused -- do not strand the user in Messages */
 return(0);
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

/* The three "jump to a location" actions (definition / implementation / type
   definition) differ only in the engine call and the not-found wording, so they
   share this driver.  `locate` is one of the e_lsp_* locators; `what` names the
   target for the error message. */
typedef int (*e_lsp_locate_fn)(e_lsp_session *, const char *, int, int,
                               char *, size_t, int *, int *);

static int e_lsp_ui_jump(FENSTER *f, e_lsp_locate_fn locate, const char *what)
{
 BUFFER *b = f->b;
 char outpath[1024], msg[64];
 int oline = -1, ochar = -1;

 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_sync(f);
 { int rc0 = locate(g_lsp, g_lsp_file, b->b.y, e_lsp_symbol_col(f),
                    outpath, sizeof(outpath), &oline, &ochar);
   if (rc0 != 0)
   {
    snprintf(msg, sizeof(msg), "No %s found.", what);
    e_error(msg, 0, f->fb);
    return(0);
   }
 }
 e_d_goto_break(outpath, oline + 1, f);        /* engine 0-based -> 1-based */
 return(0);
}

/* AltQ D -- jump to the definition of the symbol under the cursor. */
static int e_lsp_ui_definition(FENSTER *f)
{
 return(e_lsp_ui_jump(f, e_lsp_definition, "definition"));
}

/* AltQ I -- jump to the implementation of the symbol (concrete override of an
   abstract def / trait member). */
static int e_lsp_ui_implementation(FENSTER *f)
{
 return(e_lsp_ui_jump(f, e_lsp_implementation, "implementation"));
}

/* AltQ T -- jump to the TYPE of the symbol (the class/trait of a val's type,
   not the val's own definition). */
static int e_lsp_ui_type_definition(FENSTER *f)
{
 return(e_lsp_ui_jump(f, e_lsp_type_definition, "type definition"));
}

/* AltQ H -- show the type/documentation of the symbol under the cursor. */
static int e_lsp_ui_hover(FENSTER *f)
{
 BUFFER *b = f->b;
 char *hov;

 { const char *tp = getenv("XWPE_UI_TRACE");
   if (tp) { FILE *tf = fopen(tp, "a");
     if (tf) { fprintf(tf, "e_lsp_ui_hover ENTER\n"); fclose(tf); } } }
 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_sync(f);
 hov = e_lsp_hover(g_lsp, g_lsp_file, b->b.y, e_lsp_symbol_col(f));
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
 e_message(0, hov, f);                         /* one-button info popup */
 free(hov);
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
static int e_lsp_pick(FENSTER *f, const char *title, const char *const *labels,
                      int n)
{
 W_OPTSTR *o;
 char name[80];
 static char rows[16][64];     /* truncated rows, persist through e_opt_kst */
 const int maxw = 56;          /* longest row we render; longer is ...-clipped */
 int i, sel = -1, vis, mxlen = 0, titlen, w;

 vis = n < 16 ? n : 16;
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
 o->xa = 8;
 o->ya = 3;
 o->xe = o->xa + w + 4;
 o->ye = o->ya + vis + 3;
 o->bgsw = 0;
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

/* AltQ C -- offer completion candidates for the word under the cursor in a
   navigable popup (the dialog radio list); insert the chosen one, replacing the
   partial word.  Reuses the dialog widget system (arrows navigate, Enter
   selects, Esc cancels) -- no bespoke popup. */
static int e_lsp_ui_complete(FENSTER *f)
{
 BUFFER *b = f->b;
 SCHIRM *s = f->s;
 e_lsp_completion_item items[64];
 const char *labels[64];
 char *line, insbuf[256];
 const char *ins;
 int n, i, sel, prefix = 0, inslen;

 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_sync(f);
 n = e_lsp_completion(g_lsp, g_lsp_file, b->b.y, b->b.x, items, 64);
 if (n <= 0)
 {  e_error("No completions.", 0, f->fb);  return(0);  }

 /* the identifier already typed before the cursor (to be replaced) */
 line = b->bf[b->b.y].s;
 while (prefix < b->b.x && e_lsp_is_ident((unsigned char)line[b->b.x - 1 - prefix]))
  prefix++;

 for (i = 0; i < n; i++)
  labels[i] = items[i].label;
 sel = e_lsp_pick(f, "Completion - Enter to insert", labels, n);
 if (sel < 0)
  return(0);

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
 return(0);
}

/* AltQ S -- show the signature of the call the cursor is inside. */
static int e_lsp_ui_signature(FENSTER *f)
{
 BUFFER *b = f->b;
 char *sig;

 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_sync(f);
 sig = e_lsp_signature_help(g_lsp, g_lsp_file, b->b.y, b->b.x);
 if (!sig || !*sig)
 {  if (sig) free(sig);  e_error("No signature here.", 0, f->fb);  return(0);  }
 e_message(0, sig, f);
 free(sig);
 return(0);
}

/* AltQ R -- list every reference to the symbol under the cursor in Messages. */
static int e_lsp_ui_references(FENSTER *f)
{
 BUFFER *b = f->b;
 e_lsp_location locs[128];
 char line[700];
 int n, i;

 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_sync(f);
 n = e_lsp_references(g_lsp, g_lsp_file, b->b.y, e_lsp_symbol_col(f), locs, 128);
 if (n <= 0)
 {  e_error("No references found.", 0, f->fb);  return(0);  }
 snprintf(line, sizeof(line), "%d reference(s):", n);
 e_d_p_message(line, f, 1);
 for (i = 0; i < n; i++)
 {
  snprintf(line, sizeof(line), "%s:%d:%d", e_d_dap_basename(locs[i].path),
           locs[i].line + 1, locs[i].character + 1);
  e_d_p_message(line, f, 1);
 }
 return(0);
}

/* Call hierarchy for the symbol under the cursor, listed in Messages as
   "name  file:line:col": who calls it (outgoing == 0) or what it calls
   (outgoing != 0).  Lets the user trace a call graph without leaving xwpe --
   put the cursor on a def/method and ask "who reaches this?" or "what does this
   reach?".  Shared body for the two menu actions below. */
static int e_lsp_ui_call_hierarchy(FENSTER *f, int outgoing)
{
 BUFFER *b = f->b;
 e_lsp_symbol items[128];
 char line[700];
 int n, i;

 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_sync(f);
 n = e_lsp_call_hierarchy(g_lsp, g_lsp_file, b->b.y, e_lsp_symbol_col(f),
                          outgoing, items, 128);
 if (n <= 0)
 {
  e_error(outgoing ? "No outgoing calls found." : "No callers found.", 0, f->fb);
  return(0);
 }
 if (outgoing)
  snprintf(line, sizeof(line), "%d outgoing call(s):", n);
 else
  snprintf(line, sizeof(line), "%d caller(s):", n);
 e_d_p_message(line, f, 1);
 for (i = 0; i < n; i++)
 {
  snprintf(line, sizeof(line), "%s  %s:%d:%d",
           items[i].name ? items[i].name : "?",
           items[i].path ? e_d_dap_basename(items[i].path) : "?",
           items[i].line + 1, items[i].character + 1);
  e_d_p_message(line, f, 1);
 }
 return(0);
}

/* AltQ B -- callers (incoming calls): who calls the symbol under the cursor. */
static int e_lsp_ui_callers(FENSTER *f)  {  return(e_lsp_ui_call_hierarchy(f, 0));  }
/* AltQ G -- callees (outgoing calls): what the symbol under the cursor calls. */
static int e_lsp_ui_callees(FENSTER *f)  {  return(e_lsp_ui_call_hierarchy(f, 1));  }

/* Type hierarchy for the type under the cursor, listed in Messages as
   "name  file:line:col": its supertypes (subtypes == 0: what it
   extends/implements) or its subtypes (subtypes != 0: what extends it).  Put
   the cursor on a class/trait name and walk the inheritance graph up or down
   without leaving xwpe.  Shared body for the two menu actions below. */
static int e_lsp_ui_type_hierarchy(FENSTER *f, int subtypes)
{
 BUFFER *b = f->b;
 e_lsp_symbol items[128];
 char line[700];
 int n, i;

 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_sync(f);
 n = e_lsp_type_hierarchy(g_lsp, g_lsp_file, b->b.y, e_lsp_symbol_col(f),
                          subtypes, items, 128);
 if (n <= 0)
 {
  e_error(subtypes ? "No subtypes found." : "No supertypes found.", 0, f->fb);
  return(0);
 }
 if (subtypes)
  snprintf(line, sizeof(line), "%d subtype(s):", n);
 else
  snprintf(line, sizeof(line), "%d supertype(s):", n);
 e_d_p_message(line, f, 1);
 for (i = 0; i < n; i++)
 {
  snprintf(line, sizeof(line), "%s  %s:%d:%d",
           items[i].name ? items[i].name : "?",
           items[i].path ? e_d_dap_basename(items[i].path) : "?",
           items[i].line + 1, items[i].character + 1);
  e_d_p_message(line, f, 1);
 }
 return(0);
}

/* AltQ K -- supertypes (vim K = up = parents): what this type extends. */
static int e_lsp_ui_supertypes(FENSTER *f) {  return(e_lsp_ui_type_hierarchy(f, 0));  }
/* AltQ J -- subtypes (vim J = down = children): what extends this type. */
static int e_lsp_ui_subtypes(FENSTER *f)   {  return(e_lsp_ui_type_hierarchy(f, 1));  }

/* AltQ L -- highlight every occurrence of the symbol under the cursor in this
   file (the found-word colour, like a search highlight that follows meaning,
   not text).  Pressing it off any identifier clears the highlight. */
static int e_lsp_ui_highlight(FENSTER *f)
{
 BUFFER *b = f->b;
 e_lsp_location locs[256];
 char word[256];
 int n, i, wlen;

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
 n = e_lsp_document_highlight(g_lsp, g_lsp_file, b->b.y, e_lsp_symbol_col(f), locs, 256);
 if (n <= 0)
 {  e_error("No occurrences.", 0, f->fb);  return(0);  }
 /* every hit is the same symbol, so they share the identifier's length */
 wlen = strlen(word);
 g_hl_nactive = 0;
 for (i = 0; i < n && g_hl_nactive < LSP_MAX_DIAG; i++)
 {
  g_hl_active[g_hl_nactive].line = locs[i].line;
  g_hl_active[g_hl_nactive].c0 = locs[i].character;
  g_hl_active[g_hl_nactive].c1 = locs[i].character + wlen;
  g_hl_active[g_hl_nactive].sev = 0;
  g_hl_nactive++;
 }
 e_schirm(f, 1);
 e_cursor(f, 0);
 return(0);
}

/* AltQ Y -- toggle the end-of-line inlay-hint overlay (inferred types).  Turning
   it on starts/syncs the server and snapshots the file's hints; off clears them.
   If the server has nothing yet, it stays off and says so.  Off by default. */
static int e_lsp_ui_inlay(FENSTER *f)
{
 char msg[128];

 if (g_inlay_on)                        /* currently on -> turn off */
 {
  g_inlay_on = 0;
  e_lsp_inlay_clear();
  e_d_p_message("Inlay hints: OFF.", f, 1);   /* sw=1: repaints Messages + editor */
  e_cursor(f, 0);
  return(0);
 }
 if (e_lsp_ensure(f) < 0)               /* turning on: start + sync + snapshot */
  return(-1);
 e_lsp_sync(f);
 e_lsp_inlay_fetch(f);
 if (g_inlay_nactive == 0)
 {
  /* On a cold start the first compile can finish before the workspace is fully
     indexed, so the snapshot is empty.  Wait for the next diagnostics publish
     (an event, bounded -- not a fixed sleep) and try once more, so a cold
     Alt-Q Y usually works on the FIRST press instead of needing a second. */
  e_lsp_wait_diagnostics(g_lsp, g_lsp_file, 8000);
  e_lsp_inlay_fetch(f);
 }
 { const char *tp = getenv("XWPE_UI_TRACE");   /* XWPE_UI_TRACE: diag only */
   if (tp) { FILE *tf = fopen(tp, "a");
     if (tf) { fprintf(tf, "inlay toggle: fetched nactive=%d (e.g. line=%d [%s])\n",
       g_inlay_nactive, g_inlay_nactive ? g_inlay_active[0].line : -1,
       g_inlay_nactive ? g_inlay_active[0].label : "-"); fclose(tf); } } }
 if (g_inlay_nactive == 0)
 {
  e_d_p_message("Inlay hints: none for this file yet "
                "(the server may still be indexing -- try Alt-Q Y again).", f, 1);
  return(0);                            /* leave the overlay off */
 }
 g_inlay_on = 1;
 snprintf(msg, sizeof(msg),
          "Inlay hints: ON -- %d inferred type(s) shown dim at end of line. "
          "Alt-Q Y to hide.", g_inlay_nactive);
 e_d_p_message(msg, f, 1);              /* sw=1: repaints Messages + editor (hints) */
 e_cursor(f, 0);
 return(0);
}

/* AltQ K -- code lenses: the run/test/reference annotations Metals attaches to
   definitions, listed in a popup as "<label>  (line N)"; selecting one jumps to
   that definition.  To actually run/test a Scala main, use the debugger
   (Ctrl-G T) which drives the same BSP/DAP path Metals' "run" lens would. */
static int e_lsp_ui_codelens(FENSTER *f)
{
 static e_lsp_code_lens lenses[64];
 static char rows[64][160];
 const char *labels[64];
 int n, i, sel;

 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_sync(f);
 n = e_lsp_code_lenses(g_lsp, g_lsp_file, lenses, 64);
 if (n <= 0)
 {  e_error("No code lenses here.", 0, f->fb);  return(0);  }
 for (i = 0; i < n; i++)
 {
  snprintf(rows[i], sizeof(rows[i]), "%s  (line %d)",
           lenses[i].title, lenses[i].line + 1);
  labels[i] = rows[i];
 }
 sel = e_lsp_pick(f, "Code lenses - Enter to go", labels, n);
 if (sel < 0)
  return(0);
 e_d_goto_break(g_lsp_file, lenses[sel].line + 1, f);
 return(0);
}

/* AltQ O -- the file outline: pick a symbol from a popup and jump to it. */
static int e_lsp_ui_outline(FENSTER *f)
{
 e_lsp_symbol syms[128];
 const char *labels[128];
 int n, i, sel;

 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_sync(f);
 n = e_lsp_document_symbols(g_lsp, g_lsp_file, syms, 128);
 if (n <= 0)
 {  e_error("No symbols.", 0, f->fb);  return(0);  }
 for (i = 0; i < n; i++)
  labels[i] = syms[i].name;
 sel = e_lsp_pick(f, "File symbols - Enter to go", labels, n);
 if (sel < 0)
  return(0);
 e_d_goto_break(g_lsp_file, syms[sel].line + 1, f);
 return(0);
}

/* A workspace/symbol "result" Metals returns that is not a real code symbol but
   a help/command hint (e.g. "Add ';' to search library dependencies"), which it
   points at a documentation page like workspace-symbol.md.  Those must not show
   up as jump targets -- jumping would open a Markdown help file, not code. */
static int e_lsp_symbol_is_help(const e_lsp_symbol *sym)
{
 const char *path = sym->path;
 size_t len;

 if (!path)
  return(0);
 len = strlen(path);
 if (len >= 3 && strcmp(path + len - 3, ".md") == 0)
  return(1);
 return(0);
}

/* AltQ W -- workspace symbol search: prompt for a query, list matching symbols
   from across the whole project (each shown with its file), and jump to the one
   chosen (opening its file if it is not the current one).  IntelliJ's "Go to
   Symbol" / Search Everywhere. */
static int e_lsp_ui_workspace_symbols(FENSTER *f)
{
 static e_lsp_symbol syms[128];
 static char rows[128][160];
 const char *labels[128];
 int keep[128];
 char query[160];
 const char *jumpfile;
 int n, i, m, sel;

 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_word_at_cursor(f, query, sizeof(query));
 if (!e_add_arguments(query, "Search symbol", f, 0, AltS, NULL) || !query[0])
  return(0);                        /* cancelled or empty */
 e_lsp_sync(f);
 n = e_lsp_workspace_symbols(g_lsp, query, syms, 128);
 if (n <= 0)
 {  e_error("No matching symbols.", 0, f->fb);  return(0);  }
 for (i = 0, m = 0; i < n; i++)
 {
  const char *base;
  if (e_lsp_symbol_is_help(&syms[i]))
   continue;                        /* skip Metals help/command hints */
  base = syms[i].path ? e_d_dap_basename(syms[i].path) : "(this file)";
  snprintf(rows[m], sizeof(rows[m]), "%s  -  %s:%d",
           syms[i].name, base, syms[i].line + 1);
  labels[m] = rows[m];
  keep[m] = i;
  m++;
 }
 if (m <= 0)
 {  e_error("No matching symbols.", 0, f->fb);  return(0);  }
 sel = e_lsp_pick(f, "Workspace symbols - Enter to go", labels, m);
 if (sel < 0)
  return(0);
 sel = keep[sel];
 jumpfile = syms[sel].path ? syms[sel].path : g_lsp_file;
 e_d_goto_break((char *)jumpfile, syms[sel].line + 1, f);
 return(0);
}

/* AltQ A -- code actions / quick-fixes at the cursor (organize imports, import
   missing symbol, ...).  Lists what the server offers; applying a direct-edit
   action rewrites the buffer.  Command-based actions (which need executeCommand)
   are listed but reported as not-yet-runnable.  IntelliJ's Alt-Enter. */
static int e_lsp_ui_code_actions(FENSTER *f)
{
 static e_lsp_code_action acts[64];
 const char *labels[64];
 char *text, *newtext;
 int n, i, sel, others = 0;

 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_sync(f);
 n = e_lsp_code_actions(g_lsp, g_lsp_file, f->b->b.y, f->b->b.x, acts, 64);
 if (n <= 0)
 {  e_error("No code actions here.", 0, f->fb);  return(0);  }
 for (i = 0; i < n; i++)
  labels[i] = acts[i].title;
 sel = e_lsp_pick(f, "Code actions - Enter to apply", labels, n);
 if (sel < 0)
  return(0);
 if (!acts[sel].has_edit)
 {
  e_error("That action runs a server command xwpe does not execute yet.",
          -1, f->fb);
  return(0);
 }
 text = e_lsp_buffer_text(f);
 if (!text)
  return(-1);
 newtext = e_lsp_apply_code_action(g_lsp, sel, g_lsp_file, text, &others);
 if (!newtext)
 {  free(text);  e_error("Could not apply that action.", 0, f->fb);  return(0);  }
 if (strcmp(newtext, text) != 0)
  e_lsp_replace_buffer(f, newtext);
 if (others > 0)
 {
  char m[120];
  snprintf(m, sizeof(m), "Note: this action also changes %d other file(s) -- "
           "not applied here.", others);
  e_error(m, -1, f->fb);
 }
 free(text);
 free(newtext);
 return(0);
}

/* Replace the whole editor buffer with `newtext` (lines split on '\n'),
   reusing the proven buffer-rebuild path (e_p_red_buffer + print_to_end_of_
   buffer, as the Watches/Messages windows do).  Empty lines are preserved.
   Used to apply server-side reformat/rename results. */
static void e_lsp_replace_buffer(FENSTER *f, const char *newtext)
{
 BUFFER *b = f->b;
 char *copy = strdup(newtext ? newtext : ""), *p, *nl;

 if (!copy)
  return;
 e_p_red_buffer(b);
 FREE(b->bf[0].s);
 b->mxlines = 0;
 p = copy;
 for (;;)
 {
  nl = strchr(p, '\n');
  if (nl)
   *nl = '\0';
  print_to_end_of_buffer(b, p, b->mx.x);
  if (!nl)
   break;
  p = nl + 1;
  if (!*p)
   break;                          /* trailing '\n' terminates the last line */
 }
 e_new_line(b->mxlines, b);
 free(copy);
 b->b.x = 0;
 b->b.y = 0;
 f->save++;                         /* mark the window modified */
 e_firstl(f, 1);                    /* re-open the view over the new buffer */
 e_schirm(f, 1);
 e_rep_win_tree(f->ed);             /* full repaint (as the Watches rebuild) */
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
static int e_lsp_ui_format(FENSTER *f)
{
 char *text, *formatted;

 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_sync(f);
 text = e_lsp_buffer_text(f);
 if (!text)
  return(-1);
 formatted = e_lsp_format(g_lsp, g_lsp_file, text);
 if (!formatted)
 {  free(text);  e_error("Nothing to format (or no formatter).", 0, f->fb);  return(0);  }
 if (strcmp(formatted, text) == 0)
  e_d_p_message("Already formatted.", f, 1);
 else
  e_lsp_replace_buffer(f, formatted);
 free(text);
 free(formatted);
 return(0);
}

/* AltQ N -- rename the symbol under the cursor across the workspace. */
static int e_lsp_ui_rename(FENSTER *f)
{
 BUFFER *b = f->b;
 char newname[256], *text, *renamed;
 int others = 0;

 e_lsp_word_at_cursor(f, newname, sizeof(newname));
 if (!e_add_arguments(newname, "Rename to", f, 0, AltR, NULL) || !newname[0])
  return(0);                        /* cancelled or empty */
 if (e_lsp_ensure(f) < 0)
  return(-1);
 e_lsp_sync(f);
 text = e_lsp_buffer_text(f);
 if (!text)
  return(-1);
 renamed = e_lsp_rename(g_lsp, g_lsp_file, b->b.y, e_lsp_symbol_col(f), newname, text, &others);
 if (!renamed)
 {  free(text);  e_error("Rename failed (cannot rename here).", 0, f->fb);  return(0);  }
 if (strcmp(renamed, text) != 0)
  e_lsp_replace_buffer(f, renamed);
 if (others > 0)
 {
  char m[120];
  snprintf(m, sizeof(m), "Note: rename also affects %d other file(s) -- not "
           "applied (open them and rename there).", others);
  e_error(m, -1, f->fb);
 }
 free(text);
 free(renamed);
 return(0);
}

/* Called from the editor loop after a keystroke.  Cheap: drains any diagnostics
   the server already sent (non-blocking) and, when a line is finished, pushes
   the buffer so the server recompiles -- live "as you type" diagnostics without
   blocking, and no save needed.  Only acts on the file the server is open for. */
void e_lsp_on_edit(FENSTER *f, int c)
{
 char path[1200];

 if (!g_lsp || !f || !f->datnam)
  return;
 snprintf(path, sizeof(path), "%s%s", f->dirct ? f->dirct : "./", f->datnam);
 if (strcmp(path, g_lsp_file) != 0)
  return;
 g_lsp_quiet = 1;                   /* show only the summary, not each line */
 e_lsp_poll(g_lsp);
 if (c == WPE_CR)                   /* a line was completed -> recompile */
  e_lsp_sync(f);
 g_lsp_quiet = 0;
}

/* Disconnect the language server (called on editor exit). */
void e_lsp_ui_shutdown(void)
{
 if (g_lsp)
 {  e_lsp_close(g_lsp);  g_lsp = NULL;  }
 e_lsp_diag_clear();
 e_lsp_inlay_clear();
 g_inlay_on = 0;
 e_lsp_synced_set(NULL);
 free(g_lsp_doctor_last);
 g_lsp_doctor_last = NULL;
 free(g_lsp_status_last);
 g_lsp_status_last = NULL;
 g_lsp_busy = 0;
 g_lsp_file[0] = '\0';
 g_lsp_fenster = NULL;
}

/* Human-readable name of the language server wired for f's file, for menu and
   dialog titles -- "Metals" for Scala today; as clangd / pyright / ... are added
   this adapts so the menu is always named after the server it actually drives.
   Returns NULL when the file's language has no server (so callers can hide the
   menu). */
const char *e_lsp_server_label(FENSTER *f)
{
 const char *lang = e_lsp_lang_for(f);

 if (!lang)
  return(NULL);
 if (!strcmp(lang, "scala"))
  return("Metals");
 return("Language server");
}

/* One row of the discoverable LSP action menu: the label shown in the popup
   (with its Alt-Q letter as a cue) and the handler it runs. */
/* The top-menu dropdown engine (we_menue.c): draws a bordered, arrow-navigable
   list and runs the chosen item's function -- the same widget the File/Edit menus
   use, so the language-server menu looks and behaves identically. */
extern int WpeHandleSubmenu(int xa, int ya, int xe, int ye, int nm,
                            OPTK *fopt, FENSTER *f);
extern OPTK WpeFillSubmenuItem(char *t, int x, char o, int (*fkt)());

/* Inner text width of the dropdown rows: the action name is left-justified and
   its "Alt-Q X" accelerator right-justified within this, exactly like the top
   menus right-align "F2" / "Shift Del / ^X". */
#define LSP_MENU_TEXTW 27

/* Persistent storage for the formatted "name        Alt-Q X" labels (OPTK keeps
   a pointer, so the strings must outlive the call). */
static char g_lsp_menu_label[24][48];

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
  { "Highlight uses",       'U', e_lsp_ui_highlight         },
  { "Inlay hints (toggle)", 'Y', e_lsp_ui_inlay             },
  { "Outline",              'O', e_lsp_ui_outline           },
  { "Code lenses",          'L', e_lsp_ui_codelens          },
  { "Workspace symbols",    'W', e_lsp_ui_workspace_symbols },
  { "Code actions",         'A', e_lsp_ui_code_actions      },
  { "Signature help",       'S', e_lsp_ui_signature         },
  { "Rename",               'N', e_lsp_ui_rename            },
  { "Format",               'F', e_lsp_ui_format            }
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
 OPTK items[24];
 int n, xa, xe, ya, ye, w, mx;

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
 WpeHandleSubmenu(xa, ya, xe, ye, 0, items, f);
 return(0);
}

/* Alt-Q: the language-server command prefix, in the Borland spirit of Ctrl-K for
   blocks -- the menu (Alt-B / a click) is the discoverable path, the prefix is
   the silent fast one.  Read the next key and run the matching action DIRECTLY,
   drawing no menu (so a fluent user gets no flicker).  '?' or F1 or any key we
   do not recognise opens the action menu for discovery; ESC cancels. */
int e_lsp_ui_key(FENSTER *f)
{
 int c = e_getch();

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
  case 'u': case ('u' - 'a' + 1):
   return(e_lsp_ui_highlight(f));      /* U = Uses */
  case 'y': case ('y' - 'a' + 1):
   return(e_lsp_ui_inlay(f));          /* Y = inlaY hints */
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
  case WPE_ESC:
   return(0);                        /* cancel -- no action, no menu        */
  default:
   return(e_lsp_ui_menu(f));         /* '?', F1, anything else -> the menu  */
 }
}

#endif

