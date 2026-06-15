/* we_prog.c                                             */
/* Copyright (C) 1993 Fred Kruse                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

#include "messages.h"
#include "edit.h"
#include "WeExpArr.h"

#ifdef PROG

#include <time.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#if defined(HAVE_PTY_H)
# include <pty.h>          /* glibc: openpty/forkpty */
#elif defined(HAVE_UTIL_H)
# include <util.h>         /* macOS, *BSD */
#elif defined(HAVE_LIBUTIL_H)
# include <libutil.h>      /* some BSDs */
#endif
#include "we_fdloop.h"

int e_run_sh(FENSTER *f);
int e_make_library(char *library, char *ofile, FENSTER *f);
int e_p_exec(int file, FENSTER *f, PIC *pic);
struct dirfile **e_make_prj_opt(FENSTER *f);
int print_to_end_of_buffer(BUFFER * b,char * str,int wrap_limit);

int wfildes[2], efildes[2];
char *wfile = NULL, *efile = NULL;

struct e_s_prog e_s_prog = {  NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL, NULL, 0, 0};

struct e_prog e_prog = {  0, NULL, NULL, NULL, NULL, NULL  };

const char *e_get_start_symbol(void)
{
 if (e_s_prog.start_symbol && e_s_prog.start_symbol[0])
  return e_s_prog.start_symbol;
 return "main";
}

struct ERR_LI  {  char *file, *text, *srch;  int x, y, line;  } *err_li = NULL;
int err_no, err_num;

int e__project = 0, e_argc, e_p_l_comp;
int e_prj_select_pending = 0;
int e_prj_new_pending = 0;
char **e_arg = NULL;
char *e__arguments = NULL;
M_TIME last_time;
char library[80];
int e_save_pid;
struct dirfile **e_p_df;

extern BUFFER *e_p_m_buffer;
extern char *e_tmp_dir;
#ifdef DEBUGGER
extern int e_d_swtch;
#endif

/********************************************************/   
/**** defs for breakpoints and watches manipulations ****/
/*** breakpoints ***/
extern int e_d_nbrpts;
extern char ** e_d_sbrpts;
extern int * e_d_ybrpts;

/*** watches ***/
extern int e_d_nwtchs;
extern char ** e_d_swtchs;
extern int * e_d_nrwtchs;

/* Shared program-output capture (we_debug.c) -- feeds the Alt-F5 User Screen. */
extern void e_d_prog_output_add(char *, int);
extern void e_d_prog_output_reset(void);

/********************************************************/   

char *gnu_intstr = "${?*:warning:}${FILE}:${LINE}:${COLUMN}:*";
char *cc_intstr = "${?*:warning:}\"${FILE}\", line ${LINE}:* at or near * \"${COLUMN=AFTER}\"";
char *pc_intstr = "${?0:e}${?0:w}${?0:s}*:*:* * ${FILE}:\n\n* ${LINE}  ${CMPTEXT}\n*-------\
${COLUMN=PREVIOUS?^+14}\n[EWew] * line*";


char *e_prj_window_title(void)
{
 char buf[128];

 if (e_prog.project && e_prog.project[0])
 {
  const char *base = strrchr(e_prog.project, '/');
  base = base ? base + 1 : e_prog.project;
  snprintf(buf, sizeof(buf), "Project: %s", base);
  return WpeStrdup(buf);
 }
 return WpeStrdup("Project");
}

const char *e_prj_status_label(void)
{
 static char sbuf[128];

 if (e__project && e_prog.project && e_prog.project[0])
 {
  const char *base = strrchr(e_prog.project, '/');
  base = base ? base + 1 : e_prog.project;
  snprintf(sbuf, sizeof(sbuf), "Project: %s", base);
  return sbuf;
 }
 return NULL;
}

char *e_p_msg[] = {
 "Not a C - File",
 "Can\'t open Pipe",
 "Error in Process",
 "Error in Pipe\n Pipe Nr.: %d\n",
 "Error at Command: ",
 "Return-Code: %d",      /*   Number 5   */
 "%s is not a C - File",
 "No Compiler specified",
 "No Files to compile",
 "No Project-Window",
};


extern int e_lsp_ui_key(FENSTER *f);   /* Alt-Q prefix (direct/menu), in we_debug.c */
extern int e_lsp_ui_menu(FENSTER *f);  /* LSP action menu, defined in we_debug.c */

int e_prog_switch(FENSTER *f, int c)
{
 switch(c)
 {
  case AltU:
  case CF9:
   e_run(f);
   break;
  case AltQ:           /*  Alt-Q  language-server prefix: Alt-Q+letter runs the
                           action directly (silent, no flicker); Alt-Q ? (or any
                           unknown key) opens the menu; ESC cancels. */
   e_lsp_ui_key(f);
   break;
  case WPE_LSP_MENU:   /*  click on the bottom-bar entry -> open the menu */
   e_lsp_ui_menu(f);
   break;
  case AltM:   /*  Alt M  Make */
  case F9:
   e_p_make(f);
   break;
  case AltC:   /*  Alt C  Compile */
  case AF9:
   e_compile(f);
   break;
  case AltL:   /*  Alt L  InstaLl */
   e_install(f);
   break;
  case AltA:   /*  Alt A  Execute MAke */
   e_exec_make(f);
   break;
  case AltT:   /*  Alt T  NexT Error */
  case AF8:
   e_next_error(f);
   break;
  case AltV:   /*  Alt V  PreVious Error  */
  case AF7:
   e_previous_error(f);
   break;
#ifdef DEBUGGER
  case CtrlG:   /*  Ctrl G DebuG - Modus */
   e_deb_inp(f);
   break;
  default:
   return(e_debug_switch(f, c));
#else
  default:
   return(c);
#endif
 }
 return(0);
}

int e_compile(FENSTER *f)
{
 int ret;

 WpeMouseChangeShape(WpeWorkingShape);
 efildes[0] = efildes[1] = -1;
 wfildes[0] = wfildes[1] = -1;
 ret = e_comp(f, 1);
 WpeMouseRestoreShape();
 return(ret);
}

int e_p_make(FENSTER *f)
{
 ECNT *cn = f->ed;
 char ostr[128], estr[128], mstr[80];
 int len, i, file = -1;
 struct stat cbuf[1], obuf[1];
 { const char *tp = getenv("XWPE_UI_TRACE");
   if (tp) { FILE *tf = fopen(tp, "a");
     if (tf) { fprintf(tf, "e_p_make ENTER\n"); fclose(tf); } } }
 PIC *pic = NULL;
 int linkRequest = 1; /* assume linking has to be done */

 WpeMouseChangeShape(WpeWorkingShape);
 efildes[0] = efildes[1] = -1;
 wfildes[0] = wfildes[1] = -1;
 if (e_comp(f, 0))
 {
  WpeMouseRestoreShape();
  return(-1);
 }
 /* Find the source file, skipping Messages which e_comp may have
    placed at mxedt or mxedt-1 */
 for (i = cn->mxedt; i > 0; i--)
  if (strcmp(cn->f[i]->datnam, "Messages"))
   break;
 f = (i > 0) ? cn->f[i] : cn->f[cn->mxedt-1];
 if (!e__project)
 {
  e_arg = MALLOC(6 * sizeof(char *));
  e_argc = e_make_arg(&e_arg, e_s_prog.libraries);
  e_arg[1] = MALLOC(3);
  strcpy(e_arg[1], "-o");
  snprintf(mstr, sizeof(mstr), "%s", f->datnam);
  WpeStringCutChar(mstr, '.');
  len = strlen(e_prog.exedir) - 1;
  if (e_s_prog.exe_name && e_s_prog.exe_name[0])
  {
   if (e_prog.exedir[len] == DIRC)
    snprintf(estr, sizeof(estr), "%s%s", e_prog.exedir, e_s_prog.exe_name);
   else
    snprintf(estr, sizeof(estr), "%s%c%s", e_prog.exedir, DIRC, e_s_prog.exe_name);
  }
  else
  {
   if (e_prog.exedir[len] == DIRC)
    snprintf(estr, sizeof(estr), "%s%s.e", e_prog.exedir, mstr);
   else
    snprintf(estr, sizeof(estr), "%s%c%s.e", e_prog.exedir, DIRC, mstr);
  }
  if (e_prog.exedir[len] == DIRC)
   snprintf(ostr, sizeof(ostr), (e_s_prog.comp_sw & 1) ? "%s%s.class" : "%s%s.o",
            e_prog.exedir, mstr);
  else
   snprintf(ostr, sizeof(ostr), (e_s_prog.comp_sw & 1) ? "%s%c%s.class" : "%s%c%s.o",
            e_prog.exedir, DIRC, mstr);
  e_argc = e_add_arg(&e_arg, estr, 2, e_argc);
  e_argc = e_add_arg(&e_arg, ostr, 3, e_argc);
  stat(ostr, cbuf);
  if (!stat(estr, obuf) && obuf->st_mtime >= cbuf->st_mtime)
   linkRequest = 0;
 }
 else
 {
  if (!stat(e_s_prog.exe_name, obuf) && !e_p_l_comp &&
    obuf->st_mtime >= last_time)
   linkRequest = 0;
 }
 /* Non-GNU compilers (fpc, javac) handle linking internally during
    compilation.  Skip the separate link step to avoid passing
    gcc-style -o flags that they don't understand. */
 if (linkRequest && (e_s_prog.comp_sw & 1))
  linkRequest = 0;
 if (linkRequest)
 {
#ifdef DEBUGGER
  if (e_d_swtch > 0)
   e_d_quit(f);
#endif
  e_sys_ini();
  file = e_exec_inf(f, e_arg, e_argc);
  e_sys_end();
  if (!e__project)
   e_free_arg(e_arg, e_argc);
  if (file != 0)
   i = e_p_exec(file, f, pic);
  else
  {
   i = WPE_ESC;
   if (pic)
    e_close_view(pic, 1);
  }
  WpeMouseRestoreShape();
  return(i);
 }
 /* Executable is up to date -- no linking needed */
 if (!e__project)
  e_free_arg(e_arg, e_argc);
 WpeMouseRestoreShape();
 return(0);
}

/* Fold one raw output byte from the running program into the Messages buffer:
   newline opens a new line, CR is ignored, backspace/DEL deletes one (possibly
   multi-byte UTF-8) character off the current line, and any other printable
   byte is appended. */
static void e_run_pty_apply_byte(BUFFER *b, unsigned char c)
{
 int line = b->mxlines - 1;
 int len;

 if (c == '\n')
  e_new_line(b->mxlines, b);
 else if (c == '\r')
  ;
 else if (c == '\b' || c == 127)
 {
  len = b->bf[line].len;
  if (len > 0)
  {
   len--;
   while (len > 0 && ((unsigned char)b->bf[line].s[len] & 0xC0) == 0x80)
    len--;
   b->bf[line].s[len] = '\0';
   b->bf[line].len = len;
  }
 }
 else if (c >= 32)
 {
  len = b->bf[line].len;
  b->bf[line].s = REALLOC(b->bf[line].s, len + 2);
  b->bf[line].s[len] = c;
  b->bf[line].s[len + 1] = '\0';
  b->bf[line].len = len + 1;
 }
}

static void e_run_drain_pty(int pty_fd, BUFFER *b, FENSTER *mf)
{
 char buf[512];
 int n, flags;
 flags = fcntl(pty_fd, F_GETFL, 0);
 fcntl(pty_fd, F_SETFL, flags | O_NONBLOCK);
 while ((n = read(pty_fd, buf, sizeof(buf))) > 0)
 {
  int k;
  /* Capture the raw byte stream verbatim for the Alt-F5 User Screen, in
     addition to the line-parsed copy that goes to the Messages window. */
  e_d_prog_output_add(buf, n);
  for (k = 0; k < n; k++)
   e_run_pty_apply_byte(b, (unsigned char)buf[k]);
 }
 fcntl(pty_fd, F_SETFL, flags & ~O_NONBLOCK);
 b->b.y = b->mxlines - 1;
 b->b.x = b->bf[b->b.y].len;
 e_cursor(mf, 1);
 e_schirm(mf, 1);
 e_refresh();
}

/**
 * e_user_screen - Borland "User Screen" (Alt-F5): leave the editor and show
 * the running program's own full screen.
 *
 * Used after Ctrl-F9 Run (or a debug session) when the program PAINTS its
 * output -- cursor positioning, ANSI colour, a TUI, a progress bar -- which
 * the line-oriented Messages window cannot represent.  On the console (wpe)
 * it drops out of ncurses and replays the program's raw output verbatim
 * (e_t_user_screen).  In X11 (xwpe) there is no real terminal to drop to, so
 * when libvterm is available (HAVE_VTERM) the captured output is interpreted by
 * an embedded VT terminal and painted full-window (e_x_vterm_user_screen);
 * without it Alt-F5 falls back to showing the output in the Messages window.
 *
 * Return: 0.
 */
int e_user_screen(FENSTER *f)
{
 extern int e_t_user_screen(FENSTER *);

 if (!WpeIsXwin())
  return e_t_user_screen(f);
#ifdef HAVE_VTERM
 {
  extern int e_x_vterm_user_screen(FENSTER *);
  return e_x_vterm_user_screen(f);
 }
#else
 return e_deb_out(f);
#endif
}

/* Read one key the user typed while their program runs under the pty.  On the
   console it pulls a byte from ncurses and assembles a multi-byte UTF-8
   sequence into a single codepoint; under X11 it pulls the key from
   e_x_kbhit().  Returns the key, or 0 if none is pending.  (Special-key
   translation and Ctrl-C handling stay in the caller's loop.) */
static int e_run_read_user_key(void)
{
 int c = 0;
#ifdef NCURSES
 if (!WpeIsXwin())
 {
  timeout(0);
  c = getch();
  timeout(-1);
  if (c == ERR) c = 0;
  else if ((unsigned int)c >= 0xC0 && (unsigned int)c <= 0xF7)
  {
   int cp, i, expect, cont;
   if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; expect = 1; }
   else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; expect = 2; }
   else { cp = c & 0x07; expect = 3; }
   for (i = 0; i < expect; i++)
   {
    timeout(50);
    cont = getch();
    timeout(-1);
    if (cont == ERR || (cont & 0xC0) != 0x80) { cp = c; break; }
    cp = (cp << 6) | (cont & 0x3F);
   }
   c = cp;
  }
 }
#endif
#ifndef NO_XWINDOWS
 if (WpeIsXwin())
 { extern int e_x_kbhit(void);
   c = e_x_kbhit();
 }
#endif
 return c;
}

static int e_run_with_pty(char *cmd, BUFFER *b, FENSTER *mf)
{
 int pty_master, pty_slave, status, ret = -1;
 char slave_name[80];
 pid_t child;

 if (openpty(&pty_master, &pty_slave, slave_name, NULL, NULL) < 0)
  return -1;

 /* Start a fresh capture so the Alt-F5 User Screen shows only this run. */
 e_d_prog_output_reset();

 child = fork();
 if (child < 0)
 {
  close(pty_master);
  close(pty_slave);
  return -1;
 }
 if (child == 0)
 {
  close(pty_master);
  setsid();
  ioctl(pty_slave, TIOCSCTTY, 0);
  dup2(pty_slave, 0);
  dup2(pty_slave, 1);
  dup2(pty_slave, 2);
  if (pty_slave > 2) close(pty_slave);
  execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
  _exit(127);
 }
 close(pty_slave);

 print_to_end_of_buffer(b, "--- Run output (type in Messages, Ctrl-C to stop) ---", 0);
 e_new_line(b->mxlines, b);
 b->b.y = b->mxlines - 1;
 if (!WpeIsXwin())
  e_mouse_tracking_disable();
 fk_cursor(1);
 e_cursor(mf, 1);
 e_schirm(mf, 1);
 e_refresh();

 wpe_fd_add(pty_master, POLLIN, NULL, NULL);

 for (;;)
 {
  int ws;
  int wp = waitpid(child, &ws, WNOHANG);
  if (wp > 0)
  {
   e_run_drain_pty(pty_master, b, mf);
   ret = WIFEXITED(ws) ? WEXITSTATUS(ws) : -1;
   break;
  }
  e_run_drain_pty(pty_master, b, mf);
  wpe_fd_poll(WPE_FD_POLL_MS);
  {
   int c = e_run_read_user_key();
    if (c == CtrlC)
    { kill(child, SIGINT); continue; }
#ifdef NCURSES
    if (c == KEY_BACKSPACE || c == 127 || c == 8)
     c = WPE_DC;
    else if (c == KEY_ENTER || c == '\n' || c == '\r')
     c = WPE_CR;
    else if (c > KEY_CODE_YES)
     c = 0;
#endif
    if (c > 0)
     e_pty_send_key(pty_master, c);
  }
 }

 wpe_fd_del(pty_master);
 close(pty_master);
 if (!WpeIsXwin())
  e_mouse_tracking_enable();
 return ret;
}

/* e_strlcat - append src to dst within a cap-byte buffer, never overrunning
   and always NUL-terminating (BSD strlcat semantics).  Used to assemble the
   run/make command line safely from project paths, exe names and arguments,
   any of which can be long. */
static void e_strlcat(char *dst, const char *src, size_t cap)
{
 size_t dl = strlen(dst);
 if (dl + 1 >= cap)
  return;
 snprintf(dst + dl, cap - dl, "%s", src);
}

int e_run(FENSTER *f)
{
 ECNT *cn = f->ed;
 BUFFER *b;
 FENSTER *mf;
 char estr[256];
 char src_dirct[256], src_datnam[256];
 int len, ret, i;

 { const char *tp = getenv("XWPE_UI_TRACE");
   if (tp) { FILE *tf = fopen(tp, "a");
     if (tf) { fprintf(tf, "e_run ENTER datnam=%s\n",
              f && f->datnam ? f->datnam : "?"); fclose(tf); } } }
 efildes[0] = efildes[1] = -1;
 wfildes[0] = wfildes[1] = -1;
 if (!e_run_sh(f))
  return(0);
 strncpy(src_dirct, f->dirct, sizeof(src_dirct) - 1);
 src_dirct[sizeof(src_dirct) - 1] = '\0';
 strncpy(src_datnam, f->datnam, sizeof(src_datnam) - 1);
 src_datnam[sizeof(src_datnam) - 1] = '\0';
 if (e_p_make(f))
  return(-1);
 WpeMouseChangeShape(WpeWorkingShape);
 /* Find source file, skipping Messages */
 for (i = cn->mxedt; i > 0; i--)
  if (strcmp(cn->f[i]->datnam, "Messages"))
   break;
 f = (i > 0) ? cn->f[i] : cn->f[cn->mxedt-1];
#ifdef DEBUGGER
 if (e_d_swtch > 0)
  e_d_quit(f);
#endif
 estr[0] = '\0';
 if (e_s_prog.comp_sw & 1)
 {
  snprintf(estr, sizeof(estr), "%s %s%s", e_s_prog.compiler, src_dirct, src_datnam);
 }
 else
 {
  if ((!e_s_prog.exe_name) || (e_s_prog.exe_name[0]!=DIRC))
  {
   e_strlcat(estr, e_prog.exedir, sizeof(estr));
   len = strlen(estr) - 1;
   if (len >= 0 && estr[len] != DIRC)
   {
    char dc[2] = { DIRC, '\0' };
    e_strlcat(estr, dc, sizeof(estr));
   }
  }
  if (e_s_prog.exe_name && e_s_prog.exe_name[0])
  {
   e_strlcat(estr, e_s_prog.exe_name, sizeof(estr));
  }
  else if (!e__project)
  {
   e_strlcat(estr, f->datnam, sizeof(estr));
   WpeStringCutChar(estr, '.');
   e_strlcat(estr, ".e", sizeof(estr));
  }
  else
   e_strlcat(estr, "a.out", sizeof(estr));
 }
 e_strlcat(estr, " ", sizeof(estr));
 if (e_prog.arguments)
  e_strlcat(estr, e_prog.arguments, sizeof(estr));

 for (i = cn->mxedt; i > 0 && strcmp(cn->f[i]->datnam, "Messages"); i--)
  ;
 if (i <= 0)
 {
  e_edit(cn, "Messages");
  i = cn->mxedt;
 }
 mf = cn->f[i];
 b = mf->b;

 if (b->bf[b->mxlines-1].len != 0)
  e_new_line(b->mxlines, b);
 /* Restore the DEFAULT SIGCHLD disposition for the duration of the run so
    e_run_with_pty's own waitpid()/pclose() reaps the child; xwpe's normal
    SIGCHLD handler is put back afterwards.  (_dfl = SIG_DFL, not "ignore".) */
 { struct sigaction _old, _dfl;
   _dfl.sa_handler = SIG_DFL;
   sigemptyset(&_dfl.sa_mask);
   _dfl.sa_flags = 0;
   sigaction(SIGCHLD, &_dfl, &_old);
   ret = e_run_with_pty(estr, b, mf);
   if (ret < 0)
   {
    FILE *pp;
    char line[1024];
    print_to_end_of_buffer(b, "--- Run output ---", 0);
    e_strlcat(estr, " </dev/null 2>&1", sizeof(estr));
    pp = popen(estr, "r");
    if (pp)
    {
     while (fgets(line, sizeof(line), pp))
     {
      len = strlen(line);
      if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
      print_to_end_of_buffer(b, line, 0);
     }
     /* WEXITSTATUS must take an lvalue: macOS expands it to *(int*)&(x), so a
        bare pclose(pp) rvalue does not compile.  Store the status first. */
     { int status = pclose(pp);  ret = WEXITSTATUS(status);  }
    }
    else
     ret = -1;
   }
   sigaction(SIGCHLD, &_old, NULL);
 }
#ifdef NCURSES
 clearok(stdscr, TRUE);
#endif
 e_repaint_desk(cn->f[cn->mxedt]);

 print_to_end_of_buffer(b, "--- End output ---", 0);
 sprintf(estr, e_p_msg[ERR_RETCODE], ret);
 print_to_end_of_buffer(b, estr, b->mx.x);

 b->b.y = b->mxlines-1;
 e_cursor(mf, 1);
 e_schirm(mf, 1);
 e_refresh();
 WpeMouseRestoreShape();
 return(0);
}

/* e_check_c_file_w - e_check_c_file for an open editor window, passing the
   file's FULL path (dir + name).  e_check_c_file's Algol 68 dialect sniff opens
   the source to read its stropping, which fails on a bare window name when the
   file lives outside the current directory (e.g. "wpe sub/dir/foo.a68") -- so
   it would misdetect the dialect and pick the wrong compiler.  Returns the
   matching compiler index (1-based) or 0.  Shared with the debugger start
   (we_debug.c), which must set e_s_prog from the same correct dialect. */
int e_check_c_file_w(FENSTER *fw)
{
 char *full = e_mkfilename(fw->dirct, fw->datnam);
 int matched = e_check_c_file(full ? full : fw->datnam);
 if (full)
  FREE(full);
 return matched;
}

/**
 * e_compile_announce_uptodate - Tell the user that a lone Compile (Alt-C) had
 * nothing to do because the object file is already current.
 *
 * Alt-C compiles the active source to a .o without linking.  When that .o is
 * newer than the source, xwpe skips the compiler and produces no Messages
 * output -- which is indistinguishable from "Alt-C did nothing" and reads as a
 * bug (e.g. right after a debug/Make has already built the .o).  This pops a
 * short confirmation so the user knows the build is up to date, not broken.
 * Only the single-file Compile path calls it; Make (Alt-M) stays silent and
 * goes on to link.
 *
 * @param f  the active editor window (used to place the message box).
 */
static void e_compile_announce_uptodate(FENSTER *f)
{
 e_message(0, "Already compiled (object file is up to date).", f);
}

int e_comp(FENSTER *f, int announce)
{
 ECNT *cn = f->ed;
 PIC *pic = NULL;
 char **arg = NULL, fstr[128], ostr[128];
 int i, file = -1, len, argc;
 { const char *tp = getenv("XWPE_UI_TRACE");
   if (tp) { FILE *tf = fopen(tp, "a");
     if (tf) { fprintf(tf, "e_comp ENTER datnam=%s\n",
              f && f->datnam ? f->datnam : "?"); fclose(tf); } } }
#ifdef CHECKHEADER
 struct stat obuf[1];
#else
 struct stat cbuf[1], obuf[1];
#endif

#ifdef DEBUGGER
 if (e_d_swtch > 0)
 {
  i = e_message(1, "The Debugger is Running\nDo You want to Quit Debugging ?",f);
  if (i == 'Y')
   e_d_quit(f);
  else
   return(-1);
  WpeMouseChangeShape(WpeWorkingShape);
 }
#endif
 e__project = e_project_is_open();
 if (e__project)
  return(e_c_project(f));
 for (i = cn->mxedt; i > 0; i--)
 {
  if (e_check_c_file_w(cn->f[i]))
   break;
 }
 if (i == 0)
 {
  snprintf(ostr, sizeof(ostr), e_p_msg[ERR_S_NO_CFILE], f->datnam);
  e_error(ostr, 0, f->fb);
  return(WPE_ESC);
 }
 else if (cn->f[i]->save)
  e_save(cn->f[i]);
 f = cn->f[i];
 e_switch_window(cn->edt[i], cn->f[cn->mxedt]);
 argc = e_make_arg(&arg, e_s_prog.comp_str);
 if (!(e_s_prog.comp_sw & 1))
 {
  /* GNU compilers (gcc, g++, gfortran): add -c flag */
  arg[1] = MALLOC(3);
  strcpy(arg[1], "-c");
 }
 else
 {
  /* Non-GNU compilers (fpc, javac): no -c, shift args down */
  int k;
  for (k = 1; k < argc; k++)
   arg[k] = arg[k + 1];
  argc--;
 }
 len = strlen(f->dirct) - 1;
 if (!strcmp(f->ed->dirct, f->dirct))
  strcpy(fstr, f->datnam);
 if (f->dirct[len] == DIRC)
  snprintf(fstr, sizeof(fstr), "%s%s", f->dirct, f->datnam);
 else
  snprintf(fstr, sizeof(fstr), "%s%c%s", f->dirct, DIRC, f->datnam);
 argc = e_add_arg(&arg, fstr, argc, argc);
 if (e_prog.exedir[strlen(e_prog.exedir)-1] == DIRC)
  snprintf(ostr, sizeof(ostr), "%s%s", e_prog.exedir, f->datnam);
 else
  snprintf(ostr, sizeof(ostr), "%s%c%s", e_prog.exedir, DIRC, f->datnam);
 WpeStringCutChar(ostr, '.');
 /* javac produces .class files, not .o -- check the right extension
    so the up-to-date test works and we don't recompile every time */
 if (e_s_prog.comp_sw & 1)
  strcat(ostr, ".class");
 else
  strcat(ostr, ".o");
#ifndef NO_MINUS_C_MINUS_O
 if (!(e_s_prog.comp_sw & 1))
 {
  /* GNU compilers: add -o output.o */
  argc = e_add_arg(&arg, "-o", argc, argc);
  argc = e_add_arg(&arg, ostr, argc, argc);
 }
#endif
 e_sys_ini();
#ifdef CHECKHEADER
 if ((stat(ostr, obuf) || e_check_header(fstr, obuf->st_mtime, cn, 0)))
#else
 stat(f->datnam, cbuf);
 if ((stat(ostr, obuf) || obuf->st_mtime < cbuf->st_mtime))
#endif
 {
  if (e_new_message(f))
  {
   e_sys_end();
   e_free_arg(arg, argc);
   return(WPE_ESC);
  }
  remove(ostr);
  if ((file = e_exec_inf(f, arg, argc)) == 0)
  {
   e_sys_end();
   e_free_arg(arg, argc);
   return(WPE_ESC);
  }
  e_sys_end();
  e_free_arg(arg, argc);
  i = e_p_exec(file, f, pic);
  return(i);
 }
 /* Object file is up to date -- no recompilation needed */
 e_sys_end();
 e_free_arg(arg, argc);
 if (announce)
  e_compile_announce_uptodate(f);
 return(0);
}

int e_exec_inf(FENSTER *f, char **argv, int n)
{
 int pid;
 char tstr[128];
#ifdef DEBUGGER
 if (e_d_swtch > 0)
  e_d_quit(f);
#endif
 fflush(stdout);
 sprintf(tstr, "%s/we_111", e_tmp_dir);
 if((efildes[1] = creat(tstr, 0777)) < 0)
 {
  e_error(e_p_msg[ERR_PIPEOPEN], 0, f->fb);
  return(0);
 }
 if((efildes[0] = open(tstr, O_RDONLY)) < 0 )
 {
  e_error(e_p_msg[ERR_PIPEOPEN], 0, f->fb);
  return(0);
 }
 efile = MALLOC((strlen(tstr)+1)*sizeof(char));
 strcpy(efile, tstr);
 sprintf(tstr, "%s/we_112", e_tmp_dir);
 if((wfildes[1] = creat(tstr, 0777)) < 0)
 {
  e_error(e_p_msg[ERR_PIPEOPEN], 0, f->fb);
  return(0);
 }
 if((wfildes[0] = open(tstr, O_RDONLY)) < 0 )
 {
  e_error(e_p_msg[ERR_PIPEOPEN], 0, f->fb);
  return(0);
 }
 wfile = MALLOC((strlen(tstr)+1)*sizeof(char));
 strcpy(wfile, tstr);

 if((e_save_pid = pid = fork()) > 0)
  return(efildes[1]);
 else if(pid < 0)
 {
  e_error(e_p_msg[ERR_PROCESS], 0, f->fb);
  return(0);
 }

 close(2);   /*  new process   */
 if(fcntl(efildes[1], F_DUPFD, 2) != 2)
 {
  fprintf(stderr, e_p_msg[ERR_PIPEEXEC], efildes[1]);
  exit(1);
 }
 close(1);
 if(fcntl(wfildes[1], F_DUPFD, 1) != 1)
 {
  fprintf(stderr, e_p_msg[ERR_PIPEEXEC], wfildes[1]);
  exit(1);
 }
 e_print_arg(stderr, "", argv, n);
 execvp(argv[0], argv);
 e_print_arg(stderr, e_p_msg[ERR_IN_COMMAND], argv, n);
 exit(1);
 /* Can never get here */
 return 0;
}

int e_print_arg(FILE *fp, char *s, char **argv, int n)
{
 int i;

 if ((s) && (s[0]))
  fprintf(fp,"%s ", s);
 for (i = 0; i < n && argv[i] != NULL; i++)
  fprintf(fp,"%s ", argv[i]);
 fprintf(fp,"\n");
 return(n);
}

static FENSTER *e_find_or_create_messages(ECNT *cn)
{
 int i;
 for (i = cn->mxedt; i > 0; i--)
  if (!strcmp(cn->f[i]->datnam, "Messages"))
   return cn->f[i];
 if (e_edit(cn, "Messages"))
  return cn->f[cn->mxedt];
 return cn->f[cn->mxedt];
}

int e_p_exec(int file, FENSTER *f, PIC *pic)
{
 ECNT *cn = f->ed;
 FENSTER *mf = e_find_or_create_messages(cn);
 BUFFER *b = mf->b;
 int ret = 0, i = 0, is, fd, stat_loc = 0;
 char str[128];
 char *buff;

 f = mf;
 while ((ret = wait(&stat_loc)) >= 0 && ret != e_save_pid)
  ;
 /* Only trust stat_loc when we actually reaped our own child: wait() can
    return -1 (e.g. ECHILD if the child was already reaped) and then leave
    stat_loc untouched, so WIFEXITED() would read an undefined value. */
 ret = (ret == e_save_pid && WIFEXITED(stat_loc)) ? WEXITSTATUS(stat_loc) : 1;
 for (is = b->mxlines-1, fd = efildes[0]; fd > 0; fd = wfildes[0])
 {
  buff=MALLOC(1);
  buff[0]='\0';
  while( e_line_read(fd, str, 128) == 0 )
  {
   buff=REALLOC(buff, strlen(buff) + strlen(str) + 1);
   strcat(buff, str);

   fflush(stdout);
  }
  /* Don't wrap compiler output lines -- long paths break error
     patterns when the File/line info is split across two lines.
     The Messages window scrolls horizontally instead. */
  print_to_end_of_buffer(b, buff, 0);
  FREE(buff);

  if( fd == wfildes[0] )
    break;
 }
 b->b.y = b->mxlines-1;
 if (efildes[0] >= 0)
  close(efildes[0]);
 if (wfildes[0] >= 0)
  close(wfildes[0]);
 if (efildes[1] >= 0)
  close(efildes[1]);
 if (wfildes[1] >= 0)
  close(wfildes[1]);
 if (wfile)
 {
  remove(wfile);
  FREE(wfile);
  wfile = NULL;
 }
 if (efile)
 {
  remove(efile);
  FREE(efile);
  efile = NULL;
 }
 efildes[0] = efildes[1] = -1;
 wfildes[0] = wfildes[1] = -1;
 if (pic)
  e_close_view(pic, 1);
 if (b->mxlines - is > 2)
  i = e_make_error_list(f);
 if (ret || i)
 {
  if (i != -2)
   e_show_error(err_no = 0, f);
  return(-1);
 }

 print_to_end_of_buffer(b, "Success", b->mx.x);

 /* Switch back to the source file instead of staying on Messages */
 for (i = cn->mxedt; i > 0; i--)
  if (strcmp(cn->f[i]->datnam, "Messages"))
   break;
 if (i > 0 && i != cn->mxedt)
  e_switch_window(cn->edt[i], cn->f[cn->mxedt]);
 else
 {
  e_cursor(f, 1);
  e_schirm(f, 1);
 }
 e_refresh();
 return(0);
}

/* show source-position of error number "n" from actual errorlist */
int e_show_error(int n, FENSTER *f)
{
 ECNT *cn = f->ed;
 BUFFER *b = cn->f[cn->mxedt]->b;
 int i, j, bg = 0;
 char *filename;
 unsigned char *cp;

 if (!err_li || n >= err_num || n < 0)
  return(1);
 f = cn->f[cn->mxedt];
 if (err_li[n].file[0] == '.' && err_li[n].file[1] == DIRC)
  bg = 2;
 if (err_li[n].file[0] == DIRC)
 {
  filename = e_mkfilename(f->dirct, f->datnam);
 }
 else
  filename = f->datnam;
 if (strcmp(err_li[n].file+bg, filename))
 {
  for (i = cn->mxedt - 1; i > 0; i--)
  {
   if (filename != cn->f[i+1]->datnam)
   {
    FREE(filename);
    filename = e_mkfilename(cn->f[i]->dirct, cn->f[i]->datnam);
   }
   else
    filename = cn->f[i]->datnam;
   if (!strcmp(err_li[n].file+bg, filename))
   {
    if (filename != cn->f[i]->datnam)
     FREE(filename);
    e_switch_window(cn->edt[i], cn->f[cn->mxedt]);
    break;  
   }
  }
  if (i <= 0)
  {
   /* Fallback: try matching by basename only.  Compilers like pdflatex
      and py_compile report relative paths (e.g. "docs/examples/file.tex")
      that don't match the open file's datnam ("file.tex"). */
   char *_bn = strrchr(err_li[n].file + bg, '/');
   if (_bn)
   {
    _bn++;  /* skip the '/' */
    for (i = cn->mxedt; i > 0; i--)
     if (!strcmp(_bn, cn->f[i]->datnam))
     {
      e_switch_window(cn->edt[i], cn->f[cn->mxedt]);
      break;
     }
   }
   if (i <= 0)
   {
    if (filename != cn->f[1]->datnam)
     FREE(filename);
    if (e_edit(cn, err_li[n].file))
     return(WPE_ESC);
   }
  }
 }
 else if (filename != f->datnam)
  FREE(filename);
 b = cn->f[cn->mxedt]->b;
 b->b.y = err_li[n].line > b->mxlines ? b->mxlines - 1 : err_li[n].line - 1;
 if (!err_li[n].srch)
 {
  for(i = j = 0; i + j < err_li[n].x && i < b->bf[b->b.y].len; i++)
  {
   if (*(b->bf[b->b.y].s + i) == WPE_TAB)
    j += (f->ed->tabn - ((j + i) % f->ed->tabn) - 1);
#ifdef UNIX
   else if (*(b->bf[b->b.y].s + i) < ' ')
    j++;
#endif
  }
  b->b.x = i;
 }
 else
 {
  cp = strstr(b->bf[b->b.y].s, err_li[n].srch+1);
  for (i = 0; b->bf[b->b.y].s + i < cp; i++);
  if (err_li[n].srch[0] == 'B')
  {
   for (i--; i >= 0 && isspace(b->bf[b->b.y].s[i]); i--);
   if (i < 0 && b->b.y > 0)
   {
    (b->b.y)--;
    i = b->bf[b->b.y].len+1;
   }
   else
    i++;
  }
/*      else if(err_li[n].x < -1) i++;    */
  b->b.x = i +  err_li[n].x;
 }
 e_schirm(cn->f[cn->mxedt], 1);
 e_cursor(cn->f[cn->mxedt], 1);
 return(0);
}

int e_pure_bin(char *str, int ch)
{
 int i;

 for (i = 0; isspace(str[i]); i++)
  ;
 for (; str[i] && str[i] != ch; i++)
  ;
 for(; i >= 0 && str[i] != DIRC; i--)
  ;
 return(i+1);
}

int e_make_error_list(FENSTER *f)
{
 char file[256];
 ECNT *cn = f->ed;
 BUFFER *b = cn->f[cn->mxedt]->b;
 int i, j, k = 0, ret = 0;
 char *spt;

 if (err_li)
 {
  for (i = 0; i < err_num; i++)
  {
   if(err_li[i].file) FREE(err_li[i].file);
   if(err_li[i].text) FREE(err_li[i].text);
   if(err_li[i].srch) FREE(err_li[i].srch);
  }
  FREE(err_li);
 }
 err_li = MALLOC(sizeof(struct ERR_LI) * b->mxlines);
 err_num = 0;
 for (i = 0; i < b->mxlines; i++)
 {
  if (!strncmp((char *)b->bf[i].s, "Error at Command:", 17)) 
   return(!ret ? -2 : ret);
  if ((!strncmp((char *)b->bf[i].s, "ld", 2) &&
    (b->bf[i].s[2] == ' '  || b->bf[i].s[2] == ':')) ||
    !strncmp((char *)b->bf[i].s, "collect:", 8))
   ret = -2;
  else if (!strncmp((char *)b->bf[i].s, "makefile:", 9) ||
    !strncmp((char *)b->bf[i].s, "Makefile:", 9))
  {
   err_li[k].file = MALLOC(9);
   for (j = 0; j < 8; j++)
    err_li[k].file[j] = b->bf[i].s[j];
   err_li[k].file[8] = '\0';
   err_li[k].line = atoi((char *)b->bf[i].s+9);
   err_li[k].y = i;
   err_li[k].x = 0;
   err_li[k].srch = NULL;
   err_li[k].text = MALLOC(strlen((char *)b->bf[i].s) + 1);
   strcpy(err_li[k].text, (char *)b->bf[i].s);
   err_li[k].text[b->bf[i].len] = '\0';
   k++;
   err_num++;
   ret = -1;
   continue;
  }
  else if (!strncmp((char *)b->bf[i].s, "make:", 5) &&
    ((spt = strstr((char *)b->bf[i].s, "makefile")) ||
    (spt = strstr((char *)b->bf[i].s, "Makefile")) ) &&
    (err_li[k].line = atoi(spt+14)) > 0 )
  {
   err_li[k].file = MALLOC(9);
   for (j = 0; j < 8; j++)
    err_li[k].file[j] = spt[j];
   err_li[k].file[8] = '\0';
   err_li[k].y = i;
   err_li[k].x = 0;
   err_li[k].srch = NULL;
   err_li[k].text = MALLOC(strlen((char *)b->bf[i].s) + 1);
   strcpy(err_li[k].text, (char *)b->bf[i].s);
   err_li[k].text[b->bf[i].len] = '\0';
   k++;
   err_num++;
   continue;
  }
  else
  {
   char *tststr = e_s_prog.comp_sw ? e_s_prog.intstr : gnu_intstr;
   if (!(ret = e_p_cmp_mess(tststr, b, &i, &k, ret)))
   {
    int ip, in;
    ip = e_pure_bin(e_s_prog.compiler, ' ');
    in = e_pure_bin(b->bf[i].s, ':');
    sprintf(file, "%s:", e_s_prog.compiler+ip);
    if (!strncmp(file, b->bf[i].s+in, strlen(file)))
     ret = -2;
    else if (!strncmp("ld:", b->bf[i].s+in, 3))
     ret = -2;
    else if (!strncmp("as:", b->bf[i].s+in, 3))
     ret = -2;
   }
  }
 }
 if (!(f->ed->edopt & (ED_ERRORS_STOP_AT | ED_MESSAGES_STOP_AT)) &&
   ret == -1)
  ret = 0;
 return(ret);
}

int e_previous_error(FENSTER *f)
{
 int i, cur_line;

 if (err_num <= 0)
 {
  e_pr_uul(f->fb);
  return(0);
 }
 cur_line = f->b->b.y + 1;
 for (i = err_num - 1; i >= 0; i--)
  if (err_li[i].line < cur_line)
   return(e_show_error(err_no = i, f));
 return(e_show_error(err_no = err_num - 1, f));
}

int e_next_error(FENSTER *f)
{
 int i, cur_line;

 if (err_num <= 0)
 {
  e_pr_uul(f->fb);
  return(0);
 }
 cur_line = f->b->b.y + 1;
 for (i = 0; i < err_num; i++)
  if (err_li[i].line > cur_line)
   return(e_show_error(err_no = i, f));
 return(e_show_error(err_no = 0, f));
}

int e_cur_error(int y, FENSTER *f)
{
 int i;

 if(err_num)
 {
  for(i = 1; i < err_num && err_li[i].y <= y; i++);
  return(e_show_error(err_no = i - 1, f));
 }
 e_pr_uul(f->fb);
 return(0);
}

int e_d_car_ret(FENSTER *f)
{
 if (!strcmp(f->datnam, "Messages"))
  return(e_cur_error(f->ed->f[f->ed->mxedt]->b->b.y, f));
#ifdef DEBUGGER
 if (!strcmp(f->datnam, "Watches"))
  return(e_edit_watches(f));
 if(!strcmp(f->datnam, "Stack"))
  return(e_make_stack(f));
#endif
 return(0);
}

int e_line_read(int n, char *s, int max)
{
 int i, ret = 0;

 for (i = 0; i < max - 1; i++)
  if ((ret = read(n, s + i, 1)) != 1 || s[i] == '\n'|| s[i] == '\0')
   break;
 if (ret != 1 && i == 0)
  return(-1);
 if (i == max - 1)
  i--;
 s[i+1] = '\0';
 return(0);
}

int e_arguments(FENSTER *f)
{
 char str[80];

 if (!e_prog.arguments)
 {
  e_prog.arguments = MALLOC(1);
  e_prog.arguments[0] = '\0';
 }
 strcpy(str, e_prog.arguments);
 if (e_add_arguments(str, "Arguments", f, 0 , AltA, NULL))
 {
  e_prog.arguments = REALLOC(e_prog.arguments, strlen(str) + 1);
  strcpy(e_prog.arguments, str);
 }
 return(0);
}

/* Algol 68 dialect-aware toolchain selection (defined further below). */
static void e_algol68_apply(struct e_s_prog *cp, int use_ga68);
int e_algol68_use_ga68(const char *filename);

int e_check_c_file(char *name)
{
 int i, j;
 char *postfix;

 postfix = strrchr(name, '.');
 if (postfix)
 {
  for (i = 0; i < e_prog.num; i++)
   for (j = WpeExpArrayGetSize(e_prog.comp[i]->filepostfix); j; j--)
    if(!strcmp(e_prog.comp[i]->filepostfix[j - 1], postfix))
    {
     e_copy_prog(&e_s_prog, e_prog.comp[i]);
     /* Algol 68: drive the compiler that matches THIS file's dialect (a68g
        vs ga68), detected from its content -- the two are incompatible. */
     if (e_prog.comp[i]->language &&
         !strcmp(e_prog.comp[i]->language, "Algol68"))
      e_algol68_apply(&e_s_prog, e_algol68_use_ga68(name));
     return(i+1);
    }
 }
 return(0);
}

#ifdef CHECKHEADER

static void e_chk_save_if_open(char *file, ECNT *cn)
{
 int i;
 char *p;

 for (i = cn->mxedt; i > 0; i--)
 {
  if (file[0] == DIRC)
   p = e_mkfilename(cn->f[i]->dirct, cn->f[i]->datnam);
  else
   p = cn->f[i]->datnam;
  if (!strcmp(p, file) && cn->f[i]->save)
  {
   e_save(cn->f[i]);
   if (p != cn->f[i]->datnam) FREE(p);
   break;
  }
  if (p != cn->f[i]->datnam)
   FREE(p);
 }
}

static char *e_chk_skip_block_comment(char *p, char *buf, FILE *fp)
{
 p++;
 for (;;)
 {
  for (p++; *p && *p != '*'; p++)
   ;
  if (*p == '*' && *(p + 1) == '/')
   return p + 2;
  if (!*p && !fgets((p = buf), 120, fp))
   return NULL;
 }
}

static char *e_chk_skip_whitespace_and_comments(char *p, char *buf, FILE *fp)
{
 for (;;)
 {
  while (isspace((unsigned char)*p))
   p++;
  if (*p == '/' && *(p + 1) == '*')
  {
   p = e_chk_skip_block_comment(p, buf, fp);
   if (!p) return NULL;
   continue;
  }
  if (*p == '/' && *(p + 1) == '/')
   return NULL;
  return p;
 }
}

static int e_chk_is_directive(char *p, const char *name)
{
 int len = strlen(name);
 if (strncmp(p, name, len) != 0)
  return 0;
 return !isalnum((unsigned char)p[len]) && p[len] != '_';
}

static void e_chk_track_conditional(char *p, int *if_depth, int *skip_depth)
{
 if (e_chk_is_directive(p, "ifdef") || e_chk_is_directive(p, "ifndef")
     || e_chk_is_directive(p, "if"))
 {
  (*if_depth)++;
  if (*skip_depth == 0 && e_chk_is_directive(p, "if")
      && *(p + 2) == ' ' && *(p + 3) == '0')
   *skip_depth = *if_depth;
 }
 else if (e_chk_is_directive(p, "else"))
 {
  if (*skip_depth == *if_depth)
   *skip_depth = 0;
 }
 else if (e_chk_is_directive(p, "endif"))
 {
  if (*skip_depth == *if_depth)
   *skip_depth = 0;
  if (*if_depth > 0)
   (*if_depth)--;
 }
}

static int e_chk_extract_include(char *p, char *out, int outsz)
{
 int i;

 for (p += 7; isspace((unsigned char)*p); p++)
  ;
 if (*p != '\"')
  return 0;
 for (p++, i = 0; p[i] != '\"' && p[i] != '\0' && p[i] != '\n'
      && i < outsz - 1; i++)
  out[i] = p[i];
 out[i] = '\0';
 return 1;
}

static char *e_chk_next_directive(char *str, FILE *fp)
{
 char *p = e_chk_skip_whitespace_and_comments(str, str, fp);

 if (!p || *p != '#')
  return NULL;
 for (p++; isspace((unsigned char)*p); p++)
  ;
 return p;
}

int e_check_header(char *file, M_TIME otime, ECNT *cn, int sw)
{
 struct stat cbuf[1];
 FILE *fp;
 char *p, str[120], str2[120];
 int if_depth = 0, skip_depth = 0;

 e_chk_save_if_open(file, cn);

 if ((fp = fopen(file, "r")) == NULL)
  return(sw);
 stat(file, cbuf);
 if (otime < cbuf->st_mtime)
  sw++;
 while (fgets(str, 120, fp))
 {
  p = e_chk_next_directive(str, fp);
  if (!p)
   continue;
  e_chk_track_conditional(p, &if_depth, &skip_depth);
  if (skip_depth == 0 && e_chk_is_directive(p, "include")
      && e_chk_extract_include(p, str2, sizeof(str2)))
   sw = e_check_header(str2, otime, cn, sw);
 }
 fclose(fp);
 return(sw);
}
#endif

char *e_cat_string(char *p, char *str)
{
 if(str == NULL) return(p = NULL);
 if(p == NULL)
 {
  if((p = MALLOC(strlen(str)+2)) == NULL) return(NULL);
  p[0] = '\0';
 }
 else if ((p = REALLOC(p, strlen(p) + strlen(str)+2)) == NULL)
  return(NULL);
 strcat(p, " ");
 strcat(p, str);
 return(p);
}

int e_make_arg(char ***arg, char *str)
{
 int i, j;
 char tmp[128], *p = tmp;

 if (!(*arg))
  *arg = (char **) MALLOC(4*sizeof(char *));
 else
  *arg = (char **) REALLOC(*arg, 4*sizeof(char *));
 (*arg)[0] = MALLOC(strlen(e_s_prog.compiler) + 1);
 strcpy((*arg)[0], e_s_prog.compiler);
 if (!str)
 {
  (*arg)[1] = NULL;
  (*arg)[2] = NULL;
  return(2);
 }
 strcpy(tmp, str);
 for (j = 2, i = 0; p[i] != '\0'; j++)
 {
  for (; p[i] != '\0' && p[i] != ' '; i++)
   ;
  (*arg)[j] = MALLOC(i + 1);
  strncpy((*arg)[j], p, i);
  (*arg)[j][i] = '\0';
  *arg = (char **) REALLOC(*arg, (j + 3)*sizeof(char *));
  if (p[i] != '\0')
  {
   p += (i + 1);
   i = 0;
  }
 }
 (*arg)[j] = NULL;
 return(j);
}

int e_add_arg(char ***arg, char *str, int n, int argc)
{
 int i;

 argc++;
 *arg = (char **) REALLOC(*arg, (argc+1)*sizeof(char *));
 for(i = argc; i > n; i--)
  (*arg)[i] = (*arg)[i-1];
 (*arg)[n] = MALLOC(strlen(str) + 1);
 strcpy((*arg)[n], str);
 return(argc);
}

/* e_cmd_in_path - True if executable CMD is found on $PATH (access X_OK).
   Used to enable a tool-specific default only when the user has it installed,
   e.g. prefer the ga68 compiler when present. */
static int e_cmd_in_path(const char *cmd)
{
 char *path = getenv("PATH"), *copy, *dir, buf[1056];

 if (!path || !*path)
  return 0;
 copy = WpeStrdup(path);
 for (dir = strtok(copy, ":"); dir; dir = strtok(NULL, ":"))
 {
  snprintf(buf, sizeof(buf), "%s/%s", dir, cmd);
  if (access(buf, X_OK) == 0)
  {
   FREE(copy);
   return 1;
  }
 }
 FREE(copy);
 return 0;
}

/* ------------------------------------------------------------ Algol 68 ----
   Algol 68 has two live, source-INCOMPATIBLE compilers: ga68 (the GNU GCC
   front-end -> native binary, gdb) and a68g (Algol 68 Genie -> interpreter,
   own monitor).  They use opposite stropping regimes -- ga68 the modern one
   (lowercase bold words, { } comments), a68g the classic one (UPPER BEGIN/END,
   # ... # or CO comments) -- so a given .a68 file only builds with the right
   one.  xwpe therefore detects each file's dialect from its content and drives
   the matching compiler AND debugger, with no manual switch. */

static int e_a68_isword(int c)
{
 return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_';
}

/* e_a68_word_in_line - whole-word, case-sensitive search of WORD in LINE. */
static int e_a68_word_in_line(const char *line, const char *word)
{
 const char *p = line;
 size_t wl = strlen(word);

 while ((p = strstr(p, word)))
 {
  if ((p == line || !e_a68_isword((unsigned char)p[-1])) &&
      !e_a68_isword((unsigned char)p[wl]))
   return 1;
  p += wl;
 }
 return 0;
}

/* e_algol68_apply - Configure compiler entry CP for one toolchain.  ga68: GCC
   front-end -- native compile+link to a .e (comp_sw 0), GNU diagnostics, and
   the binary is gdb-debugged breaking at __algol68_main.  a68g: the Genie
   interpreter -- --norun syntax check (F9), interpret to run, its own --monitor
   to debug; its diagnostics carry only a line number. */
static void e_algol68_apply(struct e_s_prog *cp, int use_ga68)
{
 if (cp->compiler)     FREE(cp->compiler);
 if (cp->comp_str)     FREE(cp->comp_str);
 if (cp->intstr)       FREE(cp->intstr);
 if (cp->start_symbol) FREE(cp->start_symbol);
 if (use_ga68)
 {
  cp->compiler     = WpeStrdup("ga68");
  cp->comp_str     = WpeStrdup("-g");
  cp->intstr       = WpeStrdup(cc_intstr);
  cp->start_symbol = WpeStrdup("__algol68_main");
  cp->comp_sw      = 0;
 }
 else
 {
  cp->compiler     = WpeStrdup("a68g");
  cp->comp_str     = WpeStrdup("--norun");
  cp->intstr       = WpeStrdup("*in line ${LINE}*");
  cp->start_symbol = NULL;
  cp->comp_sw      = 1;
 }
}

/* e_algol68_sniff - Guess FILE's dialect from its first lines.  The regimes are
   mutually exclusive, so one strong marker decides: a { } comment or a lowercase
   bold word means ga68; a # comment or an UPPER bold word means a68g.  Returns 1
   for ga68, 0 for a68g, -1 if undecided. */
static int e_algol68_sniff(const char *filename)
{
 FILE *fp = fopen(filename, "r");
 char line[512];
 int n = 0, verdict = -1;

 if (!fp)
  return -1;
 while (verdict < 0 && n++ < 200 && fgets(line, sizeof(line), fp))
 {
  if (strchr(line, '{')) { verdict = 1; break; }   /* brace comment -> ga68 */
  if (strchr(line, '#')) { verdict = 0; break; }   /* hash comment  -> a68g */
  if (e_a68_word_in_line(line, "begin") || e_a68_word_in_line(line, "end") ||
      e_a68_word_in_line(line, "mode")  || e_a68_word_in_line(line, "proc"))
   { verdict = 1; break; }
  if (e_a68_word_in_line(line, "BEGIN") || e_a68_word_in_line(line, "END") ||
      e_a68_word_in_line(line, "MODE")  || e_a68_word_in_line(line, "PROC") ||
      e_a68_word_in_line(line, "CO")    || e_a68_word_in_line(line, "COMMENT"))
   { verdict = 0; break; }
 }
 fclose(fp);
 return verdict;
}

/* e_algol68_use_ga68 - Decide which Algol 68 toolchain to drive for FILE: the
   one matching its detected dialect, constrained to what is installed.  Shared
   by compile (e_check_c_file) and debug (e_start_debug) so a file is built and
   debugged by the same correct compiler. */
int e_algol68_use_ga68(const char *filename)
{
 int have_ga68 = e_cmd_in_path("ga68");
 int have_a68g = e_cmd_in_path("a68g");
 int d;

 if (have_ga68 && !have_a68g) return 1;
 if (have_a68g && !have_ga68) return 0;
 if (!have_ga68 && !have_a68g) return 0;   /* neither: default a68g */
 d = e_algol68_sniff(filename);            /* both present: pick by dialect */
 return (d == 1) ? 1 : 0;                  /* undecided -> classic a68g */
}

/* e_algol68_use_ga68_in - dialect decision for a file named NAME in directory
   DIR.  Assembles the full path the content sniff must open, so callers that
   only hold the split window form -- the debugger (e_d_file / datnam) -- get
   the same answer as the compiler.  Returns 1 for ga68, 0 for a68g. */
int e_algol68_use_ga68_in(const char *dir, const char *name)
{
 char *full = e_mkfilename((char *)dir, (char *)name);
 int use = e_algol68_use_ga68(full ? full : name);
 if (full)
  FREE(full);
 return use;
}

int e_ini_prog(ECNT *cn)
{
 int i;

 e_prog.num = 12;
 if (e_prog.arguments) FREE(e_prog.arguments);
 e_prog.arguments = WpeStrdup("");
 if (e_prog.project) FREE(e_prog.project);
 e_prog.project = WpeStrdup("");
 if (e_prog.exedir) FREE(e_prog.exedir);
 e_prog.exedir = WpeStrdup(".");
 if (e_prog.sys_include) FREE(e_prog.sys_include);
 e_prog.sys_include =
   WpeStrdup("/usr/include:/usr/local/include:/usr/include/X11");
 if (e_prog.comp == NULL)
  e_prog.comp = MALLOC(e_prog.num * sizeof(struct e_s_prog *));
 else
  e_prog.comp = REALLOC(e_prog.comp, e_prog.num * sizeof(struct e_s_prog *));
 for (i = 0; i < e_prog.num; i++)
  e_prog.comp[i] = calloc(1, sizeof(struct e_s_prog));
 e_prog.comp[0]->compiler = WpeStrdup("gcc");
 e_prog.comp[0]->language = WpeStrdup("C");
 e_prog.comp[0]->filepostfix = (char **)WpeExpArrayCreate(1, sizeof(char *), 1);
 e_prog.comp[0]->filepostfix[0] = WpeStrdup(".c");
 e_prog.comp[0]->key = 'C';
 e_prog.comp[0]->x = 0;
 e_prog.comp[0]->intstr = WpeStrdup(cc_intstr);
 e_prog.comp[1]->compiler = WpeStrdup("g++");
 e_prog.comp[1]->language = WpeStrdup("C++");
 e_prog.comp[1]->filepostfix = (char **)WpeExpArrayCreate(4, sizeof(char *), 1);
 e_prog.comp[1]->filepostfix[0] = WpeStrdup(".C");
 e_prog.comp[1]->filepostfix[1] = WpeStrdup(".cc");
 e_prog.comp[1]->filepostfix[2] = WpeStrdup(".cpp");
 e_prog.comp[1]->filepostfix[3] = WpeStrdup(".cxx");
 e_prog.comp[1]->key = '+';
 e_prog.comp[1]->x = 1;
 e_prog.comp[1]->intstr = WpeStrdup(cc_intstr);
 e_prog.comp[2]->compiler = WpeStrdup("gfortran");
 e_prog.comp[2]->language = WpeStrdup("Fortran");
 e_prog.comp[2]->filepostfix = (char **)WpeExpArrayCreate(5, sizeof(char *), 1);
 e_prog.comp[2]->filepostfix[0] = WpeStrdup(".f");
 e_prog.comp[2]->filepostfix[1] = WpeStrdup(".f90");
 e_prog.comp[2]->filepostfix[2] = WpeStrdup(".f95");
 e_prog.comp[2]->filepostfix[3] = WpeStrdup(".f03");
 e_prog.comp[2]->filepostfix[4] = WpeStrdup(".f08");
 e_prog.comp[2]->key = 'F';
 e_prog.comp[2]->x = 0;
 e_prog.comp[2]->intstr = WpeStrdup(cc_intstr);
 e_prog.comp[3]->compiler = WpeStrdup("fpc");
 e_prog.comp[3]->language = WpeStrdup("Pascal");
 e_prog.comp[3]->filepostfix = (char **)WpeExpArrayCreate(3, sizeof(char *), 1);
 e_prog.comp[3]->filepostfix[0] = WpeStrdup(".p");
 e_prog.comp[3]->filepostfix[1] = WpeStrdup(".pas");
 e_prog.comp[3]->filepostfix[2] = WpeStrdup(".pp");
 e_prog.comp[3]->key = 'P';
 e_prog.comp[3]->x = 0;
 e_prog.comp[3]->intstr = WpeStrdup("${?*:Warning:}${FILE}(${LINE},${COLUMN})*");
 e_prog.comp[4]->compiler = WpeStrdup("javac");
 e_prog.comp[4]->language = WpeStrdup("Java");
 e_prog.comp[4]->filepostfix = (char **)WpeExpArrayCreate(1, sizeof(char *), 1);
 e_prog.comp[4]->filepostfix[0] = WpeStrdup(".java");
 e_prog.comp[4]->key = 'J';
 e_prog.comp[4]->x = 0;
 e_prog.comp[4]->intstr = WpeStrdup("${?*:warning:}${FILE}:${LINE}:*");
 e_prog.comp[5]->compiler = WpeStrdup("python3");
 e_prog.comp[5]->language = WpeStrdup("Python");
 e_prog.comp[5]->filepostfix = (char **)WpeExpArrayCreate(1, sizeof(char *), 1);
 e_prog.comp[5]->filepostfix[0] = WpeStrdup(".py");
 e_prog.comp[5]->key = 'Y';
 e_prog.comp[5]->x = 0;
 e_prog.comp[5]->intstr = WpeStrdup("*${FILE}, line ${LINE}");
 e_prog.comp[6]->compiler = WpeStrdup("pdflatex");
 e_prog.comp[6]->language = WpeStrdup("LaTeX");
 e_prog.comp[6]->filepostfix = (char **)WpeExpArrayCreate(1, sizeof(char *), 1);
 e_prog.comp[6]->filepostfix[0] = WpeStrdup(".tex");
 e_prog.comp[6]->key = 'L';
 e_prog.comp[6]->x = 0;
 e_prog.comp[6]->intstr = WpeStrdup("${FILE}:${LINE}:");
 e_prog.comp[7]->compiler = WpeStrdup("perl");
 e_prog.comp[7]->language = WpeStrdup("Perl");
 e_prog.comp[7]->filepostfix = (char **)WpeExpArrayCreate(2, sizeof(char *), 1);
 e_prog.comp[7]->filepostfix[0] = WpeStrdup(".pl");
 e_prog.comp[7]->filepostfix[1] = WpeStrdup(".pm");
 e_prog.comp[7]->key = 'E';
 e_prog.comp[7]->x = 0;
 e_prog.comp[7]->intstr = WpeStrdup("*at ${FILE} line ${LINE}");
 e_prog.comp[8]->compiler = WpeStrdup("cobc");
 e_prog.comp[8]->language = WpeStrdup("COBOL");
 e_prog.comp[8]->filepostfix = (char **)WpeExpArrayCreate(2, sizeof(char *), 1);
 e_prog.comp[8]->filepostfix[0] = WpeStrdup(".cob");
 e_prog.comp[8]->filepostfix[1] = WpeStrdup(".cbl");
 e_prog.comp[8]->key = 'O';
 e_prog.comp[8]->x = 0;
 e_prog.comp[8]->intstr = WpeStrdup("${FILE}:${LINE}:*");
 /* Algol 68 entry: only the extension and menu key are fixed here.  Its
    compiler / comp_str / intstr / comp_sw / start_symbol are toolchain-specific
    (a68g vs ga68) and owned entirely by e_algol68_apply -- the single source of
    truth -- applied below for the default and per file in e_check_c_file. */
 e_prog.comp[9]->language = WpeStrdup("Algol68");
 e_prog.comp[9]->filepostfix = (char **)WpeExpArrayCreate(2, sizeof(char *), 1);
 e_prog.comp[9]->filepostfix[0] = WpeStrdup(".a68");
 e_prog.comp[9]->filepostfix[1] = WpeStrdup(".alg");
 e_prog.comp[9]->key = 'A';
 e_prog.comp[9]->x = 0;
 /* Go: compiled and debugged through the Debug Adapter Protocol (dlv dap), not
    a gdb pipe.  The compiler entry exists so e_check_c_file recognises ".go"
    (e_breakpoint, e_d_quit) and F9 can `go build`; the debugger path is routed
    to the DAP bridge in we_debug.c by file extension. */
 e_prog.comp[10]->compiler = WpeStrdup("go");
 e_prog.comp[10]->language = WpeStrdup("Go");
 e_prog.comp[10]->filepostfix = (char **)WpeExpArrayCreate(1, sizeof(char *), 1);
 e_prog.comp[10]->filepostfix[0] = WpeStrdup(".go");
 e_prog.comp[10]->key = 'G';
 e_prog.comp[10]->x = 0;
 e_prog.comp[10]->intstr = WpeStrdup("${FILE}:${LINE}:*");
 /* Rust: compiled with rustc to a native binary and debugged through the DAP
    bridge (gdb --interpreter=dap), like Go via dlv.  The compiler entry exists
    so e_check_c_file recognises ".rs" (e_breakpoint, e_d_quit) and supplies the
    compiler/flags the DAP bridge builds with; the debugger path is routed to the
    bridge by extension.  rustc diagnostics put the location on a "--> file:line:col"
    line. */
 e_prog.comp[11]->compiler = WpeStrdup("rustc");
 e_prog.comp[11]->language = WpeStrdup("Rust");
 e_prog.comp[11]->filepostfix = (char **)WpeExpArrayCreate(1, sizeof(char *), 1);
 e_prog.comp[11]->filepostfix[0] = WpeStrdup(".rs");
 e_prog.comp[11]->key = 'R';
 e_prog.comp[11]->x = 0;
 e_prog.comp[11]->intstr = WpeStrdup("*--> ${FILE}:${LINE}:${COLUMN}*");
 for (i = 0; i < e_prog.num; i++)
 {
  if (i == 5) e_prog.comp[i]->comp_str = WpeStrdup("-m py_compile");
  else if (i == 6) e_prog.comp[i]->comp_str = WpeStrdup("-interaction=nonstopmode -file-line-error");
  else if (i == 7) e_prog.comp[i]->comp_str = WpeStrdup("-c");
  else if (i == 10) e_prog.comp[i]->comp_str = WpeStrdup("build");
  else e_prog.comp[i]->comp_str = WpeStrdup("-g");   /* comp[9] (Algol 68) overwritten by e_algol68_apply */
  e_prog.comp[i]->libraries = WpeStrdup("");
  e_prog.comp[i]->exe_name = WpeStrdup("");
  e_prog.comp[i]->comp_sw = (i < 3 || i == 8) ? 0 : 1;  /* GNU for gcc/g++/gfortran/cobc, other for rest */
 }
 /* Default the Algol 68 entry to whichever compiler is installed (ga68 when it
    is the only one).  Each .a68 file's actual compiler is then chosen by its
    detected dialect at compile/debug time (e_check_c_file and e_start_debug via
    e_algol68_use_ga68), so both dialects build with no manual switch. */
 e_algol68_apply(e_prog.comp[9], e_cmd_in_path("ga68") && !e_cmd_in_path("a68g"));
 e_copy_prog(&e_s_prog, e_prog.comp[0]);
 return(0);
}

int e_copy_prog(struct e_s_prog *out, struct e_s_prog *in)
{
 int i;

 if (out->language) FREE(out->language);
 out->language = WpeStrdup(in->language);
 if (out->filepostfix)
 {
  for (i = WpeExpArrayGetSize(out->filepostfix); i; i--)
   WpeFree(out->filepostfix[i - 1]);
  WpeExpArrayDestroy(out->filepostfix);
 }
 out->filepostfix = (char **)WpeExpArrayCreate(WpeExpArrayGetSize(in->filepostfix), sizeof(char *), 1);
 for (i = WpeExpArrayGetSize(out->filepostfix); i; i--)
  out->filepostfix[i - 1] = WpeStrdup(in->filepostfix[i - 1]);
 if (out->compiler) FREE(out->compiler);
 out->compiler = WpeStrdup(in->compiler);
 if (out->comp_str) FREE(out->comp_str);
 out->comp_str = WpeStrdup(in->comp_str);
 if (out->libraries) FREE(out->libraries);
 out->libraries = WpeStrdup(in->libraries);
 if (out->exe_name) FREE(out->exe_name);
 out->exe_name = WpeStrdup(in->exe_name);
 if (out->intstr) FREE(out->intstr);
 out->intstr = WpeStrdup(in->intstr);
 if (out->start_symbol) FREE(out->start_symbol);
 out->start_symbol = in->start_symbol ? WpeStrdup(in->start_symbol) : NULL;
 out->key = in->key;
 out->comp_sw = in->comp_sw;
 return(0);
}

int e_prj_ob_btt(FENSTER *f, int sw)
{
 FLWND *fw;

 e_data_first(sw+4, f->ed, f->ed->dirct);
 if (sw == 0)
 {
  FENSTER *pf = f->ed->f[f->ed->mxedt];
  FREE(pf->datnam);
  pf->datnam = e_prj_window_title();
  e_ed_rahmen(pf, 1);
 }
 if (sw > 0)
 {
  if (!(f->ed->edopt & ED_CUA_STYLE))
   while (e_data_eingabe(f->ed) != AF3)
    ;
  else
   while (e_data_eingabe(f->ed) != CF4)
    ;
  fw = (FLWND *)f->ed->f[f->ed->mxedt]->b;
  fw->df = NULL;
  e_close_window(f->ed->f[f->ed->mxedt]);
 }
 return(0);
}

int e_prj_ob_file(FENSTER *f)
{
 return(e_prj_ob_btt(f, 0));
}

int e_prj_ob_varb(FENSTER *f)
{
 return(e_prj_ob_btt(f, 1));
}

int e_prj_ob_inst(FENSTER *f)
{
 return(e_prj_ob_btt(f, 2));
}

int e_prj_ob_svas(FENSTER *f)
{
 return(e_project_name(f) ? 0 : AltS);
}

int e_project_options(FENSTER *f)
{
 int ret;
 W_OPTSTR *o = e_init_opt_kst(f);
 char *messagestring;

 if (!o)
  return(-1);
 if (!(e_make_prj_opt(f)))
 {
  freeostr(o);
  return(-1);
 }
 o->xa = 8;  o->ya = 1;  o->xe = 68;  o->ye = 23;
 o->bgsw = 0;
 o->name = "Project-Options";
 o->crsw = AltS;
 e_add_txtstr(4, 14, "Compiler-Style:", o);
 e_add_wrstr(4, 2, 22, 2, 36, 128, 0, AltC, "Compiler:", e_s_prog.compiler, NULL, o);
 e_add_wrstr(4, 4, 22, 4, 36, 256, 3, AltP, "ComPiler-Options:", e_s_prog.comp_str, NULL, o);
 e_add_wrstr(4, 6, 22, 6, 36, 256, 0, AltL, "Loader-Options:", e_s_prog.libraries, NULL, o);
 e_add_wrstr(4, 8, 22, 8, 36, 128, 0, AltE, "Executable:", e_s_prog.exe_name, NULL, o);
 e_add_wrstr(4, 10, 22, 10, 36, 128, 2, AltB, "LiBrary:", library, NULL, o);
 { const char *sym = e_s_prog.start_symbol ? e_s_prog.start_symbol : "main";
   e_add_wrstr(4, 12, 22, 12, 36, 128, 0, AltY, "Start sYmbol:", (char *)sym, NULL, o);
 }
 messagestring = WpeStringToValue(e_s_prog.intstr);
 e_add_wrstr(22, 14, 22, 15, 36, 256, 0, AltM, "Message-String:", messagestring, NULL, o);
 WpeFree(messagestring);
 e_add_pswstr(0, 5, 15, 0, AltG, 0, "GNU       ", o);
 e_add_pswstr(0, 5, 16, 1, AltT, e_s_prog.comp_sw, "OTher     ", o);
 e_add_bttstr(9, 19, 0, AltS, "Save", NULL, o);
 e_add_bttstr(44, 19, -1, WPE_ESC, "Cancel", NULL, o);
 e_add_bttstr(26, 19, 5, AltA, "Save As", e_prj_ob_svas, o);
 e_add_bttstr(12, 17, 0, AltV, "Variables ...", e_prj_ob_varb, o);
 e_add_bttstr(35, 17, 0, AltI, "Install ...", e_prj_ob_inst, o);
 ret = e_opt_kst(o);
 if (ret != WPE_ESC)
 {
  if (e_s_prog.compiler) FREE(e_s_prog.compiler);
  e_s_prog.compiler = WpeStrdup(o->wstr[0]->txt);
  if (e_s_prog.comp_str) FREE(e_s_prog.comp_str);
  e_s_prog.comp_str = WpeStrdup(o->wstr[1]->txt);
  if (e_s_prog.libraries) FREE(e_s_prog.libraries);
  e_s_prog.libraries = WpeStrdup(o->wstr[2]->txt);
  if (e_s_prog.exe_name) FREE(e_s_prog.exe_name);
  e_s_prog.exe_name = WpeStrdup(o->wstr[3]->txt);
  if (e_s_prog.start_symbol) FREE(e_s_prog.start_symbol);
  e_s_prog.start_symbol = WpeStrdup(o->wstr[5]->txt);
  if (e_s_prog.intstr) FREE(e_s_prog.intstr);
  e_s_prog.intstr = WpeValueToString(o->wstr[6]->txt);
  /* Bounded: the LiBrary dialog field accepts more than sizeof(library). */
  snprintf(library, sizeof(library), "%s", o->wstr[4]->txt);
  e_s_prog.comp_sw = o->pstr[0]->num;
  e_wrt_prj_fl(f);
 }
 freeostr(o);
 if (f->ed->mxedt > 0)
  e_ed_rahmen(f, 1);
 return(0);
}

int e_run_c_options(FENSTER *f)
{
 int i, j, ret;
 W_OPTSTR *o = e_init_opt_kst(f);
 char filepostfix[128];
 char *newpostfix;
 char *messagestring;

 if (!o)
  return(-1);
 o->xa = 8;  o->ya = 2;  o->xe = 68;  o->ye = 22;
 o->bgsw = 0;
 o->name = "Compiler-Options";
 o->crsw = AltO;
 e_add_txtstr(4, 14, "Compiler-Style:", o);
 e_add_wrstr(4, 2, 22, 2, 36, 128, 1, AltA, "LAnguage:", e_s_prog.language, NULL, o);
 e_add_wrstr(4, 4, 22, 4, 36, 128, 0, AltC, "Compiler:", e_s_prog.compiler, NULL, o);
 e_add_wrstr(4, 6, 22, 6, 36, 128, 3, AltP, "ComPiler-Options:", e_s_prog.comp_str, NULL, o);
 e_add_wrstr(4, 8, 22, 8, 36, 128, 0, AltL, "Loader-Options:", e_s_prog.libraries, NULL, o);
 e_add_wrstr(4, 10, 22, 10, 36, 128, 0, AltE, "Executable:", e_s_prog.exe_name, NULL, o);
 filepostfix[0] = 0;
 if ((j = WpeExpArrayGetSize(e_s_prog.filepostfix)))
 {
  strcpy(filepostfix, e_s_prog.filepostfix[0]);
  for (i = 1; i < j; i++)
  {
   strcat(filepostfix, " ");
   strcat(filepostfix, e_s_prog.filepostfix[i]);
  }
 }
 e_add_wrstr(4, 12, 22, 12, 36, 128, 0, AltF, "File-Postfix:", filepostfix, NULL, o);
 { const char *sym = e_s_prog.start_symbol ? e_s_prog.start_symbol : "main";
   e_add_wrstr(4, 14, 22, 14, 36, 128, 7, AltY, "Start sYmbol:", (char *)sym, NULL, o);
 }
 messagestring = WpeStringToValue(e_s_prog.intstr);
 e_add_wrstr(22, 15, 22, 16, 36, 128, 0, AltM, "Message-String:", messagestring, NULL, o);
 WpeFree(messagestring);
 e_add_pswstr(0, 5, 16, 0, AltG, 0, "GNU      ", o);
 e_add_pswstr(0, 5, 17, 1, AltT, e_s_prog.comp_sw, "OTher    ", o);
 e_add_bttstr(16, 19, 1, AltO, " Ok ", NULL, o);
 e_add_bttstr(37, 19, -1, WPE_ESC, "Cancel", NULL, o);
 ret = e_opt_kst(o);
 if (ret != WPE_ESC)
 {
  if (e_s_prog.language) FREE(e_s_prog.language);
  e_s_prog.language = WpeStrdup(o->wstr[0]->txt);
  if (e_s_prog.compiler) FREE(e_s_prog.compiler);
  e_s_prog.compiler = WpeStrdup(o->wstr[1]->txt);
  if (e_s_prog.comp_str) FREE(e_s_prog.comp_str);
  e_s_prog.comp_str = WpeStrdup(o->wstr[2]->txt);
  if (e_s_prog.libraries) FREE(e_s_prog.libraries);
  e_s_prog.libraries = WpeStrdup(o->wstr[3]->txt);
  if (e_s_prog.exe_name) FREE(e_s_prog.exe_name);
  e_s_prog.exe_name = WpeStrdup(o->wstr[4]->txt);
  for (i = 0; i < j; i++)
   WpeFree(e_s_prog.filepostfix[i]);
  WpeExpArrayDestroy(e_s_prog.filepostfix);
  e_s_prog.filepostfix = (char **)WpeExpArrayCreate(0, sizeof(char *), 1);
  for (i = 0; o->wstr[5]->txt[i]; i++)
  {
   if (isspace(o->wstr[5]->txt[i]))
    continue;
   for (j = i; (o->wstr[5]->txt[j]) && (!isspace(o->wstr[5]->txt[j])); j++)
    ;
   newpostfix = (char *)WpeMalloc(sizeof(char) * j - i + 1);
   strncpy(newpostfix, o->wstr[5]->txt + i, j - i);
   newpostfix[j - i] = 0;
   WpeExpArrayAdd((void **)&e_s_prog.filepostfix, &newpostfix);
   i = j - 1;
  }
  if (e_s_prog.start_symbol) FREE(e_s_prog.start_symbol);
  e_s_prog.start_symbol = WpeStrdup(o->wstr[6]->txt);
  if (e_s_prog.intstr) FREE(e_s_prog.intstr);
  e_s_prog.intstr = WpeValueToString(o->wstr[7]->txt);
  e_s_prog.comp_sw = o->pstr[0]->num;
 }
 freeostr(o);
 return(0);
}

int e_run_options(FENSTER *f)
{
 int i, n, xa = 48, ya = 2, num = 2 + e_prog.num;
 OPTK *opt = MALLOC(num * sizeof(OPTK));
 char tmp[80];

 tmp[0] = '\0';
 opt[0].t = "Add Compiler   ";     opt[0].x = 0;  opt[0].o = 'A';
 opt[1].t = "Remove Compiler";     opt[1].x = 0;  opt[1].o = 'R';

 for (i = 0; i < e_prog.num; i++)
 {
  opt[i+2].t = e_prog.comp[i]->language;  opt[i+2].x = e_prog.comp[i]->x;
  opt[i+2].o = e_prog.comp[i]->key;
 }
 n = e_opt_sec_box(xa, ya, num, opt, f, 1);

 if (n == 0)
 {
  if (!e_run_c_options(f))
  {
   e_prog.num++;
   e_prog.comp = REALLOC(e_prog.comp, e_prog.num * sizeof(struct e_s_prog *));
   e_prog.comp[e_prog.num - 1] = calloc(1, sizeof(struct e_s_prog));
   e_prog.comp[e_prog.num - 1]->language = (char *)WpeMalloc(1);
   e_prog.comp[e_prog.num - 1]->language[0] = 0;
   e_prog.comp[e_prog.num - 1]->compiler = (char *)WpeMalloc(1);
   e_prog.comp[e_prog.num - 1]->compiler[0] = 0;
   e_prog.comp[e_prog.num - 1]->comp_str = (char *)WpeMalloc(1);
   e_prog.comp[e_prog.num - 1]->comp_str[0] = 0;
   e_prog.comp[e_prog.num - 1]->libraries = (char *)WpeMalloc(1);
   e_prog.comp[e_prog.num - 1]->libraries[0] = 0;
   e_prog.comp[e_prog.num - 1]->exe_name = (char *)WpeMalloc(1);
   e_prog.comp[e_prog.num - 1]->exe_name[0] = 0;
   e_prog.comp[e_prog.num - 1]->intstr = (char *)WpeMalloc(1);
   e_prog.comp[e_prog.num - 1]->intstr[0] = 0;
   e_prog.comp[e_prog.num - 1]->filepostfix =
     (char **)WpeExpArrayCreate(0, sizeof(char *), 1);
   e_copy_prog(e_prog.comp[e_prog.num-1], &e_s_prog);
   for (n = 0; e_prog.comp[e_prog.num-1]->language[n]; n++)
   {
    for (i = 0; i <= e_prog.num &&
      toupper(e_prog.comp[e_prog.num-1]->language[n]) != opt[i].o; i++)
     ;
    if (i > e_prog.num)
     break;
   }
   e_prog.comp[e_prog.num-1]->key =
    toupper(e_prog.comp[e_prog.num-1]->language[n]);
   e_prog.comp[e_prog.num-1]->x = n;
  }
 }
 else if (n == 1)
 {
  if(e_add_arguments(tmp, "Remove Compiler", f, 0, AltR, NULL))
  {
   for (i = 0; i < e_prog.num && strcmp(e_prog.comp[i]->language, tmp); i++)
    ;
   if (i >= e_prog.num)
   {
    e_error(e_p_msg[ERR_NO_COMPILER], 0, f->fb);
    FREE(opt);
    return(0);
   }
   FREE(e_prog.comp[i]);
   for(; i < e_prog.num-1; i++) e_prog.comp[i] = e_prog.comp[i+1];
    e_prog.num--;
  }
 }
 else if (n > 1)
 {
  e_copy_prog(&e_s_prog, e_prog.comp[n-2]);
  e_run_c_options(f);
  e_copy_prog(e_prog.comp[n-2], &e_s_prog);
 }
 FREE(opt);
 return(n < 0 ? WPE_ESC : 0);
}

int e_project_name(FENSTER *f)
{
 char str[80];

 if (!e_prog.project)
 {
  e_prog.project = MALLOC(1);
  e_prog.project[0] = '\0';
 }
 strcpy(str, e_prog.project);
 if (e_add_arguments(str, "Project", f, 0, AltP, NULL))
 {
  e_prog.project = REALLOC(e_prog.project, strlen(str) + 1);
  strcpy(e_prog.project, str);
  return(0);
 }
 return(WPE_ESC);
}

/**
 * e_find_project_window - Locate the user's open project window.
 *
 * Used whenever an action targets "the project" rather than a single file:
 * switching back to the project's file list, adding or deleting a member,
 * saving the .prj, or building the whole project. The project window is the
 * panel that lists the project's source files -- the one titled
 * "Project: <name>.prj".
 *
 * @cn: editor context.
 *
 * Return: index into @cn->f[] of the project window, or 0 when the user has no
 *         project window open.
 */
int e_find_project_window(ECNT *cn)
{
 int i;

 for (i = cn->mxedt;
   i > 0 && (cn->f[i]->dtmd != DTMD_DATA || cn->f[i]->ins != 4); i--)
  ;
 return(i);
}

/**
 * e_find_dirty_project_window - Locate the project window with unsaved edits.
 *
 * Used when xwpe is about to re-read or rewrite the .prj from disk. If the user
 * has changed the project since it was last saved (added or removed files), the
 * in-memory file list must be preserved across the re-parse instead of being
 * overwritten by the older on-disk copy. A "dirty" project window is one the
 * user has edited but not yet saved.
 *
 * @cn: editor context.
 *
 * Return: index into @cn->f[] of the unsaved project window, or 0 when no open
 *         project has pending changes.
 */
int e_find_dirty_project_window(ECNT *cn)
{
 int i;

 for (i = cn->mxedt; i > 0 && (cn->f[i]->dtmd != DTMD_DATA
   || cn->f[i]->ins != 4 || !cn->f[i]->save); i--)
  ;
 return(i);
}

/**
 * e_project_is_open - Tell whether the user currently has a project loaded.
 *
 * Used to gate project-only actions (Window->Project, Add/Delete member,
 * building against the project's compiler settings). When it returns false the
 * editor is acting on plain files, so callers report "No project open" rather
 * than inventing an empty project.
 *
 * Return: non-zero when a project is loaded -- its .prj path is set and the
 *         file still exists on disk -- and 0 otherwise.
 */
int e_project_is_open(void)
{
 return(e_prog.project[0] && access(e_prog.project, F_OK) == 0);
}

/* Open the project named in e_prog.project: validate, replace any open
   project window, parse the .prj, and show the file list. Shared by the
   File-Manager selection path and any caller that has set e_prog.project. */
int e_open_project_file(ECNT *cn)
{
 FENSTER *f;
 int i;

 if (!e_project_is_open())
 {
  e_error("Project file not found", 0, cn->fb);
  e_prog.project[0] = '\0';
  return(WPE_ESC);
 }
 e__project = 1;
 i = e_find_project_window(cn);
 if (i > 0)
 {
  e_switch_window(cn->edt[i], cn->f[cn->mxedt]);
  e_close_window(cn->f[cn->mxedt]);
 }
 f = cn->f[cn->mxedt];
 e_make_prj_opt(f);
 e_rel_brkwtch(f);
 e_prj_ob_file(f);
 return(0);
}

/* Write a minimal .prj so a freshly named project can be opened.
   e_make_prj_opt() fills in the compiler defaults (gcc, -g). */
static void e_write_default_prj(char *path)
{
 FILE *fp = fopen(path, "w");
 char exe[128];
 const char *base;
 int i;

 if (!fp)
  return;
 /* Executable name = project basename without directory or .prj suffix
    (e.g. /tmp/nuevo.prj -> nuevo). */
 base = strrchr(path, DIRC);
 base = base ? base + 1 : path;
 for (i = 0; base[i] && base[i] != '.' && i < (int)sizeof(exe) - 1; i++)
  exe[i] = base[i];
 exe[i] = '\0';

 fprintf(fp, "#\n# xwpe - project-file: %s\n", path);
 fprintf(fp, "# created by xwpe version %s\n#\n\n", VERSION);
 fprintf(fp, "CMP=\tgcc\n");
 fprintf(fp, "CMPFLAGS=\t-g\n");
 fprintf(fp, "LDFLAGS=\t\n");
 fprintf(fp, "EXENAME=\t%s\n", exe);
 fprintf(fp, "CMPSWTCH=\tgnu\n\n");
 fprintf(fp, "FILES=\t\n");
 fclose(fp);
}

/**
 * e_select_project - Open an EXISTING project the user picked.
 *
 * Used by the "Open Project" picker. Strictly opens what the user chose: if the
 * file does not exist (e.g. a typo), e_open_project_file reports "Project file
 * not found" rather than silently creating an empty project. Use
 * e_create_project for the "New Project" path.
 *
 * @cn:   editor context.
 * @path: chosen .prj path.
 */
int e_select_project(ECNT *cn, char *path)
{
 e_prog.project = REALLOC(e_prog.project, strlen(path) + 1);
 strcpy(e_prog.project, path);
 return(e_open_project_file(cn));
}

/**
 * e_create_project - Create a NEW project at the name the user typed and open it.
 *
 * Used by the "New Project" picker. Writes a fresh skeleton .prj (compiler
 * defaults + EXENAME from the name) when the file does not yet exist, then opens
 * it. If the user types the name of an existing project it is opened as-is, so
 * "New Project" never destroys an existing one.
 *
 * @cn:   editor context.
 * @path: project path to create/open.
 */
int e_create_project(ECNT *cn, char *path)
{
 e_prog.project = REALLOC(e_prog.project, strlen(path) + 1);
 strcpy(e_prog.project, path);
 if (access(path, F_OK) != 0)
  e_write_default_prj(path);
 return(e_open_project_file(cn));
}

/**
 * e_project - Menu "Open Project": pick an existing .prj to open.
 *
 * Opens the project picker in open mode (see e_select_project).
 */
int e_project(FENSTER *f)
{
 e_prj_select_pending = 1;
 WpeCreateFileManager(0, f->ed, "");
 return(0);
}

/**
 * e_new_project - Menu "New Project": type a name to create a new project.
 *
 * Opens the project picker in new mode; the typed name is created as a fresh
 * project (see e_create_project) and opened.
 */
int e_new_project(FENSTER *f)
{
 e_prj_new_pending = 1;
 WpeCreateFileManager(0, f->ed, "");
 return(0);
}

int e_show_project(FENSTER *f)
{
 ECNT *cn = f->ed;
 int i;

 /* Window->Project only makes sense with a project open; without one,
    do not synthesise an empty project window. */
 if (!e_project_is_open())
 {
  e_error("No project open", 0, f->fb);
  return(0);
 }
 i = e_find_project_window(cn);
 if (i > 0)
  e_switch_window(cn->edt[i], cn->f[cn->mxedt]);
 else
 {
  e_make_prj_opt(f);
  e_prj_ob_file(f);
 }
 return(0);
}

int e_cl_project(FENSTER *f)
{
 ECNT *cn = f->ed;
 int i;

 e__project = 0;
 i = e_find_project_window(cn);
 if (i > 0)
 {
  e_switch_window(cn->edt[i], cn->f[cn->mxedt]);
  e_close_window(cn->f[cn->mxedt]);
 }
 e_pr_uul(cn->fb);
 return(0);
}

int e_p_add_item(FENSTER *f)
{
 ECNT *cn = f->ed;
 int i;

 if (!e_project_is_open())
 {
  e_error("No project open", 0, f->fb);
  return(0);
 }
 i = e_find_project_window(cn);
 if (i > 0)
  e_switch_window(cn->edt[i], cn->f[cn->mxedt]);
 else
 {
  FLWND *fw;

  e_make_prj_opt(f);
  e_prj_ob_file(f);
  fw = (FLWND *) cn->f[cn->mxedt]->b;
  fw->nf = fw->df->anz - 1;
 }
 cn->f[cn->mxedt]->save = 1;
 WpeCreateFileManager(5, cn, NULL);
 WpeHandleFileManager(cn);
 return(0);
}

int e_p_del_item(FENSTER *f)
{
 ECNT *cn = f->ed;
 int i;

 i = e_find_project_window(cn);
 if (i > 0)
  e_switch_window(cn->edt[i], cn->f[cn->mxedt]);
 else
  return(e_error(e_p_msg[ERR_NOPROJECT], 0, f->fb));
 f = cn->f[cn->mxedt];
 f->save = 1;
 e_p_del_df((FLWND *) f->b, f->ins);
 /* Persist immediately, symmetrically with Add (#137: keep disk in sync). */
 e_wrt_prj_fl(f);
 return 0;
}

int e_make_library(char *library, char *ofile, FENSTER *f)
{
 char *ar_arg[5] = {  NULL, NULL, NULL, NULL, NULL  };
 int ret = 0, file = -1;
 PIC *pic = NULL;

 ar_arg[0] = "ar";
 if (access(library, 0))
  ar_arg[1] = "-cr";
 else
  ar_arg[1] = "-r";
 ar_arg[2] = library;
 ar_arg[3] = ofile;
 if ((ret = e_p_mess_win("Insert into Archive", 4, ar_arg, &pic, f)) == 0)
 {
  e_sys_ini();
  file = e_exec_inf(f, ar_arg, 4);
  e_sys_end();
  if ((file) && ((ret = e_p_exec(file, f, pic)) == 0))
  {
   pic = NULL;
/*
#ifdef RANLIB
    ar_arg[0] = "ranlib";
    ar_arg[1] = library;
    ar_arg[2] = NULL;
    if(ret = e_p_mess_win("Convert Archive", 2, ar_arg, &pic, f)) goto m_l_ende;
    e_sys_ini();
    file = e_exec_inf(f, ar_arg, 2);
    e_sys_end();
    if(file) ret = e_p_exec(file, f, pic);
#endif
*/
  }
 }
 return((!ret && file) ? 0 : -1);
}

int e_system(char *estr, ECNT *cn)
{
#if MOUSE
 int g[4];
#endif
 int ret;
 PIC *outp;
 FENSTER *f;

#if  MOUSE
 g[0] = 2;
 fk_mouse(g);
#endif
 f = cn->f[cn->mxedt-1];
 outp = e_open_view(0,0,MAXSCOL-1,MAXSLNS-1,cn->fb->ws,1);
 fk_locate(0,0);
 fk_cursor(1);
 (*e_u_s_sys_ini)();
 ret = system(estr);
 if (!WpeIsXwin())
 {
  printf("%s",e_msg[ERR_HITCR]);
  fflush(stdout);
  fk_getch();
 }
 (*e_u_s_sys_end)();
 e_close_view(outp, 1);
 fk_cursor(0);
#if  MOUSE
 g[0] = 1;
 fk_mouse(g);
#endif
 return(ret);
}

/* arranges string str into buffer b and eventually wrappes string around
 wrap_limit columns */
int print_to_end_of_buffer(BUFFER * b,char * str,int wrap_limit)
{
 int i,k,j;

 k = 0;
 do
 {
  if (wrap_limit != 0)
   for (j = 0;
     ((j < wrap_limit) && (!((str[j + k] == '\n') || (str[j + k] == '\0'))));
     j++)
    ;
  else
   for (j = 0; (!((str[j + k] == '\n') || (str[j + k] == '\0'))); j++)
    ;
  /* Skip blank lines but continue processing */
  if (j == 0)
  {
   if (str[k] == '\n')
    k++;
   else
    break;
   continue;
  }

/* b->mxlines - count of lines in b
   so add one more line at the end of buffer */
  e_new_line(b->mxlines, b);
  i = b->mxlines-1;

/* copy char from string (str) to buffer */

  if (str[j + k]!='\0')
   b->bf[i].s = REALLOC(b->bf[i].s, j + 2);
  else
   b->bf[i].s = REALLOC(b->bf[i].s, j + 1);
  strncpy(b->bf[i].s,str + k,j);

/* if this is not end of string, then we created substring
 if *(b->bf[i].s+j) is not '\0' then it is soft break is not written to file */

  if (str[j + k]!='\0')
  {
   *(b->bf[i].s + j) = '\n';
   *(b->bf[i].s + j + 1) = '\0';
  }
  else
  {
   *(b->bf[i].s + j) = '\0';
  }
/* update len of line in buffer */
  b->bf[i].len = j;
  b->bf[i].nrc = j + 1;

  if (str[j + k]=='\n')
  {
   j++;
  }

  k += j;

/* loop until end of string
 } while (str[k] != '\n' && str[k] != '\0');*/
 } while (str[k] != '\0');

 return 0;
}

void e_messages_scroll_to_bottom(FENSTER *f)
{
 SCHIRM *s = f->s;
 BUFFER *b = f->b;
 int visible_h = f->e.y - f->a.y - 1;
 int margin = visible_h > 4 ? 2 : 0;

 if (b->b.y >= s->c.y + visible_h - margin)
  s->c.y = b->b.y - visible_h + margin + 1;
 if (s->c.y < 0) s->c.y = 0;
}

int e_d_p_message(char *str, FENSTER *f, int sw)
{
 ECNT *cn = f->ed;
 BUFFER *b;
 SCHIRM *s;
 int i;

 if (str[0] == '\0' || str[0] == '\n')
  return(0);
 for (i = cn->mxedt; i > 0 && strcmp(cn->f[i]->datnam, "Messages"); i--)
  ;
 if (i == 0)
 {
  if (e_edit(cn, "Messages"))
   return(-1);
  else
   i = cn->mxedt;
 }
 else
  e_position_messages_window(cn->f[i], cn);

 f = cn->f[i];

/* b - buffer */
 b = cn->f[i]->b;

/* s - content of window */
 s = cn->f[i]->s;

 print_to_end_of_buffer(b, str, b->mx.x);

 b->b.y = b->mxlines-1;
 e_messages_scroll_to_bottom(f);

 if (sw)
  e_rep_win_tree(cn);
 else if (WpeIsXwin())
 {
  e_schirm(f, 0);
  e_cursor(f, 0);
  e_refresh();
 }
 return(0);
}

/* Append a line to a window identified by NAME (creating it the first time),
   like e_d_p_message but for any window -- e.g. a dedicated "Metals Doctor"
   window, so the server's Doctor report does not pollute "Messages".  Unlike
   the Messages window this is a plain editor window (no bottom-dock).  sw=1
   surfaces it; sw=0 writes in the background. */
int e_d_p_named(char *winname, char *str, FENSTER *f, int sw)
{
 ECNT *cn = f->ed;
 FENSTER *wf;
 BUFFER *b;
 int i;

 if (str[0] == '\0')
  return(0);
 for (i = cn->mxedt; i > 0 && strcmp(cn->f[i]->datnam, winname); i--)
  ;
 if (i == 0)
 {
  if (e_edit(cn, winname))
   return(-1);
  i = cn->mxedt;
 }
 wf = cn->f[i];
 b = wf->b;
 print_to_end_of_buffer(b, str, b->mx.x);
 b->b.y = b->mxlines - 1;
 e_messages_scroll_to_bottom(wf);
 if (sw)
  e_rep_win_tree(cn);
 else if (WpeIsXwin())
 {
  e_schirm(wf, 0);
  e_cursor(wf, 0);
  e_refresh();
 }
 return(0);
}

#if MOUSE
int e_d_car_mouse(FENSTER *f)
{
 extern struct mouse e_mouse;
 BUFFER *b = f->ed->f[f->ed->mxedt]->b;
 SCHIRM *s = f->ed->f[f->ed->mxedt]->s;

 if (e_mouse.y-f->a.y+s->c.y-1 == b->b.y)
  return(WPE_CR);
 else
 {
  b->b.y = e_mouse.y-f->a.y+s->c.y-1;
  b->b.x = e_mouse.x-f->a.x+s->c.x-1;
 }
 return(0);
}
#endif

int e_exec_make(FENSTER *f)
{
 ECNT *cn = f->ed;
 char **arg = NULL;
 int i, file, argc;

 WpeMouseChangeShape(WpeWorkingShape);
 efildes[0] = efildes[1] = -1;
 wfildes[0] = wfildes[1] = -1;
 for (i = cn->mxedt; i > 0; i--)
  if (cn->f[i] && cn->f[i]->datnam &&
      (!strcmp(cn->f[i]->datnam, "Makefile") ||
       !strcmp(cn->f[i]->datnam, "makefile")))
  {
   e_switch_window(cn->edt[i], cn->f[cn->mxedt]);
   e_save(cn->f[cn->mxedt]);
   break;
  }
 if (e_new_message(f))
  return(WPE_ESC);
 f = cn->f[cn->mxedt];
 e_sys_ini();
 if (e_s_prog.compiler)
  FREE(e_s_prog.compiler);
 e_s_prog.compiler = MALLOC(5*sizeof(char));
 strcpy(e_s_prog.compiler, "make");
 argc = e_make_arg(&arg, e_prog.arguments);
 if (argc == 0)
 {
  arg[1] = NULL;
  argc = 2;
 }
 else
 {
  for (i = 1; i < argc; i++)
   arg[i] = arg[i+1];
 }
 if ((file = e_exec_inf(f, arg, argc)) == 0)
 {
  e_sys_end();
  WpeMouseRestoreShape();
  return(WPE_ESC);
 }
 e_sys_end();
 e_free_arg(arg, argc - 1);
 i = e_p_exec(file, f, NULL);
 WpeMouseRestoreShape();
 return(i);
}

int e_run_sh(FENSTER *f)
{
 int ret, len = strlen(f->datnam);
 char estr[128];

 if (strcmp(f->datnam+len-3, ".sh"))
  return(1);

 WpeMouseChangeShape(WpeWorkingShape);   
 f->filemode |= 0100;
 if (f->save)
  e_save(f);
 strcpy(estr, f->datnam);
 strcat(estr, " ");
 if (e_prog.arguments)
  strcat(estr, e_prog.arguments);
#ifndef NO_XWINDOWS
 if (WpeIsXwin())
 {
  ret = (*e_u_system)(estr);
 }
 else
#endif
 ret = e_system(estr, f->ed);
 WpeMouseRestoreShape();
 return(0);
}

/*  new project   */

struct proj_var {  char *var, *string;  }  **p_v = NULL;
int p_v_n = 0;


char *e_interpr_var(char *string)
{
 int i, j;

 for (i = 0; string[i]; i++)
 {
  if (string[i] == '\\') 
   for (j = i; (string[j] = string[j+1]) != '\0'; j++)
    ;
  else if (string[i] == '\'' || string[i] == '\"')
  {
   for(j = i; (string[j] = string[j+1]) != '\0'; j++)
    ;
   i--;
  }
 }
 return(string);
}

char *e_expand_var(char *string, FENSTER *f)
{
 int i, j = 0, k, len, kl = 0;
 char *var = NULL, *v_string, *tmp;

 for (i = 0; string[i]; i++)
 {
  if (string[i] == '\'')
  {
   kl = kl ? 0 : 1;
   for (j = i; (string[j] = string[j+1]) != '\0'; j++);
   i--;
   continue;
  }
  if (string[i] == '\\' && (string[i+1] == 'n' || string[i+1] == 'r'))
  {
   string[i] = string[i+1] == 'n' ? '\n' : '\r';
   for (j = i+1; (string[j] = string[j+1]) != '\0'; j++);
   continue;
  }
  if (string[i] == '$' && !kl && (!i || string[i-1] != '\\'))
  {
   if (string[i+1] == '(')
   {
    for (j = i+2; string[j] && string[j] != ')'; j++);
    if (!string[j]) continue;
   }
   else if (string[i+1] == '{')
   {
    for (j = i+2; string[j] && string[j] != '}'; j++);
    if (!string[j]) continue;
   }
   if (string[i+1] == '(' || string[i+1] == '{')
   {
    if (!(var = MALLOC((j-i-1) * sizeof(char))))
    {
     e_error(e_msg[ERR_LOWMEM], 0, f->fb);
     return(string);
    }
    for (k = i+2; k < j; k++) var[k-i-2] = string[k];
    var[k-i-2] = '\0';
   }
   else
   {
    if (!(var = MALLOC(2 * sizeof(char))))
    {  e_error(e_msg[ERR_LOWMEM], 0, f->fb);  return(string);  }
    var[0] = string[i+1];  var[1] = '\0';
   }
   if (!(v_string = getenv(var)))
   {
    for (k = 0; k < p_v_n - 1; k++)
    {
     if (!strcmp(p_v[k]->var, var))
     {  v_string = p_v[k]->string;  break;  }
    }
   }
   if (string[i+1] == '(' || string[i+1] == '{') len = (j-i+1);
   else len = 2;
   if (!v_string)
   {
    for(k = i; (string[k] = string[k+len]) != '\0'; k++);
    if (!(string = REALLOC(tmp = string, (strlen(string) + 1) * sizeof(char))))
    {  FREE(var);  e_error(e_msg[ERR_LOWMEM], 0, f->fb);  return(tmp);  }
   }
   else
   {
    len = strlen(v_string) - len;
    if (len >= 0)
    {
     if (!(string = REALLOC(tmp = string, (k = strlen(string) + len + 1) * sizeof(char))))
     {  FREE(var);  e_error(e_msg[ERR_LOWMEM], 0, f->fb);  return(tmp);  }
     for (k--; k > j + len; k--) string[k] = string[k-len];
     for (k = i; v_string[k-i]; k++) string[k] = v_string[k-i];
    }
    else
    {
     for (k = i; (string[k] = string[k-len]) != '\0'; k++);
     for (k = i; v_string[k-i]; k++) string[k] = v_string[k-i];
     if (!(string = REALLOC(tmp = string, (strlen(string) + 1) * sizeof(char))))
     {  FREE(var);  e_error(e_msg[ERR_LOWMEM], 0, f->fb);  return(tmp);  }
    }
   }
   FREE(var);
  }
 }
 return(string);
}

int e_read_var(FENSTER *f)
{
 struct proj_var **tmp;
 FILE *fp;
 char str[256], *sp1, *sp2, *stmp;
 int i;

 if ((fp = fopen(e_prog.project, "r")) == NULL) return(-1);
 if (p_v)
 {
  for (i = 0; i < p_v_n; i++)
  {
   if (p_v[i])
   {
    if (p_v[i]->var) FREE(p_v[i]->var);
    if (p_v[i]->string) FREE(p_v[i]->string);
    FREE(p_v[i]);
   }
  }
  FREE(p_v);
 }
 p_v_n = 0;
 if (!(p_v = MALLOC(sizeof(struct proj_var *))))
 {  fclose(fp);  e_error(e_msg[ERR_LOWMEM], 0, f->fb);  return(-1);  }
 while (fgets(str, 256, fp))
 {
  for (i = 0; isspace(str[i]); i++);
  if (!str[i]) continue;
  else if (str[i] == '#')
  {
   while (str[strlen(str)-1] != '\n')
   {  fgets(str, 256, fp);  }
   continue;
  }
  sp1 = str + i;
  sp2 = strchr(sp1, '=');
  if (sp2 == NULL) continue;
  for (stmp = sp1; !((isspace(*stmp)) || (*stmp == '=')) && *stmp; stmp++)
   ;
  *stmp = 0;
  for (sp2++; isspace(*sp2) && *sp2 != '\n'; sp2++);
  p_v_n++;
  if (!(p_v = REALLOC(tmp = p_v, sizeof(struct proj_var *) * p_v_n)))
  {  p_v = tmp;  fclose(fp);  e_error(e_msg[ERR_LOWMEM], 0, f->fb);  return(-1);  }
  if (!(p_v[p_v_n-1] = MALLOC(sizeof(struct proj_var))))
  {  fclose(fp);  e_error(e_msg[ERR_LOWMEM], 0, f->fb);  return(-1);  }
  if (!(p_v[p_v_n-1]->var = MALLOC((strlen(sp1)+1) * sizeof(char))))
  {  fclose(fp);  e_error(e_msg[ERR_LOWMEM], 0, f->fb);  return(-1);  }
  strcpy(p_v[p_v_n-1]->var, sp1);
  if (!(p_v[p_v_n-1]->string = MALLOC((strlen(sp2)+1) * sizeof(char))))
  {  fclose(fp);  e_error(e_msg[ERR_LOWMEM], 0, f->fb);  return(-1);  }
  strcpy(p_v[p_v_n-1]->string, sp2);
  while (p_v[p_v_n-1]->string[i = strlen(p_v[p_v_n-1]->string) - 1] != '\n' || (i && p_v[p_v_n-1]->string[i-1] == '\\'))
  {
   if (p_v[p_v_n-1]->string[i-1] == '\\') p_v[p_v_n-1]->string[i-1] = '\0';
   if (!fgets(str, 256, fp)) break;
   if (!(p_v[p_v_n-1]->string = REALLOC(stmp = p_v[p_v_n-1]->string,
		(strlen(p_v[p_v_n-1]->string)+strlen(str)+1) * sizeof(char))))
   {
    p_v[p_v_n-1]->string = stmp;
    fclose(fp);  e_error(e_msg[ERR_LOWMEM], 0, f->fb);  return(-1);
   }
   strcat(p_v[p_v_n-1]->string, str);
  }
  p_v[p_v_n-1]->string[strlen(p_v[p_v_n-1]->string) - 1] = '\0';
  for (i = 0; p_v[p_v_n-1]->string[i]; i++)
   if (p_v[p_v_n-1]->string[i] == '\t') p_v[p_v_n-1]->string[i] = ' ';
  p_v[p_v_n-1]->string = e_expand_var(p_v[p_v_n-1]->string, f);
 }
 fclose(fp);
 return(0);
}

int e_install(FENSTER *f)
{
 char *tp, *sp, *string, *tmp, text[256];
 FILE *fp;
 int i, j;

 if (e_p_make(f)) return(-1);
 if (!e__project) return(0);
 if ((fp = fopen(e_prog.project, "r")) == NULL)
 {
  sprintf(text, e_msg[ERR_FOPEN], e_prog.project);
  e_error(text, 0, f->fb);
  return(WPE_ESC);
 }
 while ((tp = fgets(text, 256, fp)))
 {
  if (text[0] == '\t') continue;
  for (i = 0; isspace(text[i]); i++)
   ;
  if (!strncmp(text+i, "install:", 8))
  {
   while (tp && (text[j = strlen(text)-1] != '\n' || text[j-1] == '\\'))
    tp = fgets(text, 256, fp);
   break;
  }
 }
 if (!tp)
 {
  fclose(fp);
  return(1);
 }
 while (tp && (tp = fgets(text, 256, fp)))
 {
  for (i = 0; isspace(text[i]); i++)
   ;
  sp = text+i;
  if (sp[0] == '#')
  {
   while (tp && (text[j = strlen(text)-1] != '\n' || text[j-1] == '\\'))
    tp = fgets(text, 256, fp);
   continue;
  }
  if (text[0] != '\t')
   break;
  if (!(string = MALLOC(strlen(sp) + 1)))
  {
   fclose(fp);
   e_error(e_msg[ERR_LOWMEM], 0, f->fb);
   return(-1);
  }
  strcpy(string, sp);
  while (tp && (text[j = strlen(text)-1] != '\n' || text[j-1] == '\\'))
  {
   tp = fgets(text, 256, fp);
   if (tp)
   {
    if (!(string = REALLOC(tmp = string, strlen(string) + strlen(text) + 1)))
    {
     fclose(fp);
     FREE(tmp);
     e_error(e_msg[ERR_LOWMEM], 0, f->fb);
     return(-1);
    }
    strcat(string, text);
   }
  }
  if (p_v_n)
   p_v_n++;
  string = e_expand_var(string, f);
  if (p_v_n) p_v_n--;
  e_d_p_message(string, f, 1);
  system(string);
  FREE(string);
 }
 fclose(fp);
 return(0);
}

struct dirfile *e_p_get_args(char *string)
{
 struct dirfile *df = MALLOC(sizeof(struct dirfile));
 char **tmp;
 int i, j, k;

 if (!df)
  return(NULL);
 if (!(df->name = MALLOC(sizeof(char *))))
 {
  FREE(df);
  return(NULL);
 }
 df->anz = 0;
 for (i = 0; string[i]; )
 {
  for (; isspace(string[i]); i++)
   ;
  for (j = i; string[j] && !isspace(string[j]); j++)
   ;
  if (j == i)
   break;
  df->anz++;
  if (!(df->name = REALLOC(tmp = df->name, df->anz * sizeof(char *))))
  {
   df->anz--;
   df->name = tmp;
   return(df);
  }
  if (!(df->name[df->anz-1] = MALLOC((j-i+1)*sizeof(char))))
  {
   df->anz--;
   return(df);
  }
  for (k = i; k < j; k++)
   *(df->name[df->anz-1] + k - i) = string[k];
  *(df->name[df->anz-1] + k - i) = '\0';
  e_interpr_var(df->name[df->anz-1]);
  i = j;
 }
 return(df);
}

struct dirfile *e_p_get_var(char *string)
{
 int i;

 for (i = 0; i < p_v_n; i++)
 {
  if(!strcmp(p_v[i]->var, string))
   return(e_p_get_args(p_v[i]->string));
 }
 return(NULL);
}

int e_c_project(FENSTER *f)
{
 ECNT *cn = f->ed;
 struct dirfile *df = NULL;
 char **arg;
 int i, j, k, file= -1, len, elen, argc, libsw = 0, exlib = 0, sccs = 0;
 char ofile[128];
#ifdef CHECKHEADER
 struct stat lbuf[1], obuf[1];
#else
 struct stat lbuf[1], cbuf[1], obuf[1];
#endif
 PIC *pic = NULL;

 last_time = (M_TIME) 0;
 e_p_l_comp = 0;
 if (e_new_message(f)) return(WPE_ESC);
 f = cn->f[cn->mxedt];
 if (e_s_prog.comp_str)
 {
  FREE(e_s_prog.comp_str);
  e_s_prog.comp_str = NULL;
 }
 e_s_prog.comp_sw &= ~1;
 e_argc = 1;
 argc = 1;
 i = e_find_dirty_project_window(f->ed);
 if (i > 0) e_p_update_prj_fl(f);
 if (e_read_var(f))
 {
  sprintf(ofile, e_msg[ERR_FOPEN], e_prog.project);
  e_error(ofile, 0, f->fb);
  return(-1);
 }
 e_arg = (char **) MALLOC(e_argc*sizeof(char *));
 arg = (char **) MALLOC(argc*sizeof(char *));
 df = e_p_get_var("CMP");
 if (!df)
 {
  e_error(e_p_msg[ERR_NOTHING], 0, f->fb);
  e_free_arg(arg, argc);  e_free_arg(e_arg, e_argc);
  return(-1);
 }
 for (k = 0; k < df->anz; k++, e_argc++, argc++)
 {
  j = e_argc == 1 ? 1 : 0;
  e_arg = REALLOC(e_arg, (e_argc+2)*sizeof(char *));
  e_arg[e_argc-j] = MALLOC(strlen(df->name[k]) + 1);
  strcpy(e_arg[e_argc-j], df->name[k]);
  arg = REALLOC(arg, (argc+2)*sizeof(char *));
  arg[argc-j] = MALLOC(strlen(df->name[k]) + 1);
  strcpy(arg[argc-j], df->name[k]);
  if (e_argc > 1)
   e_s_prog.comp_str = e_cat_string(e_s_prog.comp_str, e_arg[e_argc-j]);
 }
 freedf(df);
 arg[1] = MALLOC(3);
 strcpy(arg[1], "-c");
 e_arg[1] = MALLOC(3);
 strcpy(e_arg[1], "-o");
 df = e_p_get_var("CMPFLAGS");
 if (df)
 {
  for (k = 0; k < df->anz; k++, e_argc++, argc++)
  {
   j = e_argc == 1 ? 1 : 0;
   e_arg = REALLOC(e_arg, (e_argc+2)*sizeof(char *));
   e_arg[e_argc-j] = MALLOC(strlen(df->name[k]) + 1);
   strcpy(e_arg[e_argc-j], df->name[k]);
   arg = REALLOC(arg, (argc+2)*sizeof(char *));
   arg[argc-j] = MALLOC(strlen(df->name[k]) + 1);
   strcpy(arg[argc-j], df->name[k]);
   if (e_argc > 1)
    e_s_prog.comp_str = e_cat_string(e_s_prog.comp_str, e_arg[e_argc-j]);
  }
  freedf(df);
 }
 df = e_p_get_var("EXENAME");
 elen = strlen(e_prog.exedir)-1;
 if (e_prog.exedir[elen] == '/')
  sprintf(ofile, "%s%s", e_prog.exedir,
    (df && df->anz > 0 && df->name[0][0]) ? df->name[0] : "a.out");
 else
  sprintf(ofile, "%s/%s", e_prog.exedir,
    (df && df->anz > 0 && df->name[0][0]) ? df->name[0] : "a.out");
 if (df) freedf(df);
 if (e_s_prog.exe_name) FREE(e_s_prog.exe_name);
 e_s_prog.exe_name = WpeStrdup(ofile);
 e_argc = e_add_arg(&e_arg, e_s_prog.exe_name, 2, e_argc);
 df = e_p_get_var("LIBNAME");
 if (df)
 {
  snprintf(library, sizeof(library), "%s", df->name[0]);
  if (access(library, 0)) exlib = 1;
  else stat(library, lbuf);
  freedf(df);
 }
 else
  library[0] = '\0';
 df = e_p_get_var("CMPSWTCH");
 if (df)
 {
  if (!strcmp(df->name[0], "other"))
   e_s_prog.comp_sw = 1;
  freedf(df);
 }
 df = e_p_get_var("CMPMESSAGE");
 if (df)
 {
  char *tmpstr = MALLOC(1);
  tmpstr[0] = '\0';
  for (k = 0; k < df->anz; k++)
  {
   tmpstr = REALLOC(tmpstr,
     (strlen(tmpstr)+strlen(df->name[k])+2)*sizeof(char));
   if (k) strcat(tmpstr, " ");
   strcat(tmpstr, df->name[k]);
  }
  if (e_s_prog.intstr) FREE(e_s_prog.intstr);
  e_s_prog.intstr = WpeStrdup(tmpstr);
  FREE(tmpstr);
  freedf(df);
 }
 else
 {
  if (e_s_prog.intstr) FREE(e_s_prog.intstr);
  e_s_prog.intstr = WpeStrdup(cc_intstr);
 }
 df = e_p_get_var("FILES");
 if (!df)
 {
  e_error(e_p_msg[ERR_NOTHING], 0, cn->fb);
  e_free_arg(arg, argc);  e_free_arg(e_arg, e_argc);
  return(-1);
 }
 arg[argc] = NULL;
 elen = strlen(e_prog.exedir)-1;
 for (k = 0; k < df->anz; k++)
 {
  for (j = cn->mxedt; j > 0; j--)
   if (!strcmp(cn->f[j]->datnam, df->name[k]) && cn->f[j]->save)
    e_save(cn->f[j]);
  for (j = strlen(df->name[k])-1; j >= 0 && df->name[k][j] != DIRC; j--)
   ;
  if (e_prog.exedir[elen] == '/')
   sprintf(ofile, "%s%s ", e_prog.exedir, df->name[k]+j+1);
  else sprintf(ofile, "%s/%s ", e_prog.exedir, df->name[k]+j+1);
  for (j = strlen(ofile); j > 0 && ofile[j] != '.'; j--)
   ;
  ofile[j+1] = 'o';
  ofile[j+2] = '\0';
  if (!stat(ofile, obuf))
  {
   if (obuf->st_mtime > last_time) last_time = obuf->st_mtime;
#ifdef CHECKHEADER
   if (!e_check_header(df->name[k], obuf->st_mtime, cn, 0)) goto gt_library;
#else
   stat(df->name[k], cbuf);
   if (obuf->st_mtime >= cbuf->st_mtime) goto gt_library;
#endif
  }
  argc = e_add_arg(&arg, df->name[k], argc, argc);
#ifndef NO_MINUS_C_MINUS_O
  argc = e_add_arg(&arg, "-o", argc, argc);
  argc = e_add_arg(&arg, ofile, argc, argc);
#endif
  arg[argc] = NULL;
  remove(ofile);
  sccs = 1;
  j = e_p_mess_win("Compiling", argc, arg, &pic, f);
  e_sys_ini();
  if (j != 0 || (file = e_exec_inf(f, arg, argc)) == 0)
  {
   e_sys_end();
   e_free_arg(arg, argc);
   freedf(df);
   e_free_arg(e_arg, e_argc);
   if (pic) e_close_view(pic, 1);
   return(WPE_ESC);
  }
  e_sys_end();
  e_p_l_comp = 1;
  if (e_p_exec(file, f, pic))
  {
   e_free_arg(arg, argc);
   e_free_arg(e_arg, e_argc);
   freedf(df);
   return(-1);
  }
  pic = NULL;
  for (j = strlen(ofile); j >= 0 && ofile[j] != '/'; j--)
   ;
  if (!exlib && library[0] != '\0' && strcmp(ofile+j+1, "main.o") &&
    (strncmp(e_s_prog.exe_name, ofile+j+1,(len = strlen(e_s_prog.exe_name))) ||
    ofile[len] == '.'))
  {
   if(e_make_library(library, ofile, f))
   {
    e_free_arg(arg, argc);
    e_free_arg(e_arg, e_argc);
    freedf(df);
    return(-1);  
   }
   else libsw = 1;
  }
  for (j = 0; j < 3; j++) FREE(arg[argc-j-1])
   ;
  argc -= 3;
gt_library:
  for (j = strlen(ofile); j >= 0 && ofile[j] != '/'; j--)
   ;
  if (library[0] == '\0' || !strcmp(ofile+j+1, "main.o") ||
    (!strncmp(e_s_prog.exe_name, ofile+j+1,(len = strlen(e_s_prog.exe_name))) &&
    ofile[len] == '.'))
   e_argc = e_add_arg(&e_arg, ofile, e_argc, e_argc);
  else if (exlib || obuf->st_mtime >= lbuf->st_mtime)
  {
   if (e_make_library(library, ofile, f))
   {
    e_free_arg(arg, argc);
    e_free_arg(e_arg, e_argc);
    freedf(df);
    return(-1);
   }
   else libsw = 1;
  }
 }
#ifdef RANLIB
 if (libsw && library[0] != '\0')
 {
  char *ar_arg[3];
  ar_arg[0] = "ranlib";
  ar_arg[1] = library;
  ar_arg[2] = NULL;
  if (!(j = e_p_mess_win("Convert Archive", 2, ar_arg, &pic, f)))
  {
   e_sys_ini();
   file = e_exec_inf(f, ar_arg, 2);
   e_sys_end();
   if (file) j = e_p_exec(file, f, pic);
  }
  if (j || !file)
  {
   e_free_arg(arg, argc);
   e_free_arg(e_arg, e_argc);
   freedf(df);
   return(-1);
  }
 }
#endif
 if (library[0] != '\0')
  e_argc = e_add_arg(&e_arg, library, e_argc, e_argc);
 freedf(df);
 df = e_p_get_var("LDFLAGS");
 if (df)
 {
  FREE(e_s_prog.libraries);
  e_s_prog.libraries = NULL;
  for (k = 0; k < df->anz; k++, e_argc++)
  {
   e_arg = REALLOC(e_arg, (e_argc+2)*sizeof(char *));
   e_arg[e_argc] = MALLOC(strlen(df->name[k]) + 1);
   strcpy(e_arg[e_argc], df->name[k]);
   e_s_prog.libraries = e_cat_string(e_s_prog.libraries, e_arg[e_argc]);
  }
  freedf(df);
 }
 e_arg[e_argc] = NULL;
 e_free_arg(arg, argc);
 if (!sccs) e_p_exec(file, f, pic);
 return(0);
}

int e_free_arg(char **arg, int argc)
{
 int i;

 for(i = 0; i < argc; i++)
  if(arg[i])
   FREE(arg[i]);
 FREE(arg);
 return(i);
}

char *e_find_var(char *var)
{
 int i;

 for(i = 0; i < p_v_n && strcmp(p_v[i]->var, var); i++);
 if(i >= p_v_n)
  return(NULL);
 else
  return(p_v[i]->string);
}

/****************************************************/
/**** reloading watches and breakpoints from prj ****/
/**** based on p_v variable ****/

/****
  this function is called only on project opening.
  It is used for additional parsing variables from
  project file, ie for loading BREAKPOINTS and WATCHES.
****/  
  
int e_rel_brkwtch(FENSTER *f)
{
 int i;

 for (i = 0; i < p_v_n; i++)
 {
  if (!strcmp(p_v[i]->var, "BREAKPOINTS"))
  {
   e_d_reinit_brks(f,p_v[i]->string);
  }
  else if (!strcmp(p_v[i]->var, "WATCHES"))
  {
   e_d_reinit_watches(f,p_v[i]->string);
  }
 }
 return 0;
}

/****************************************************/

/****
  this function is called each time options window
  is opened. But unfortunately also reloads variables
  from project file, which is not good for WATCHES
  and BREAKPOINTS.
****/  
struct dirfile **e_make_prj_opt(FENSTER *f)
{
 int i, j, ret;
 char **tmp, *sp, *tp, text[256];
 FILE *fp;
 struct dirfile *save_df = NULL;

 i = e_find_dirty_project_window(f->ed);
 if (i > 0) {  save_df = e_p_df[0];  e_p_df[0] = NULL;  }
 if (e_p_df) freedfN(e_p_df, 3);
 e_p_df = MALLOC(3 * sizeof(struct dirfile *));
 if (!e_p_df) return(e_p_df);
 for (i = 0; i < 3; i++) e_p_df[i] = NULL;
 e_s_prog.comp_sw = 0;
 ret = e_read_var(f);
 if (ret)
 {
  if (e_s_prog.compiler) FREE(e_s_prog.compiler);
  e_s_prog.compiler = WpeStrdup("gcc");
  if (e_s_prog.comp_str) FREE(e_s_prog.comp_str);
  e_s_prog.comp_str = WpeStrdup("-g");
  if (e_s_prog.libraries) FREE(e_s_prog.libraries);
  e_s_prog.libraries = WpeStrdup("");
  if (e_s_prog.exe_name) FREE(e_s_prog.exe_name);
  /* Project my_prog.prj defaults to an executable of my_prog BD */
  strcpy(text, e_prog.project);
  e_s_prog.exe_name = WpeStrdup(WpeStringCutChar(text, '.'));
  /*e_s_prog.exe_name = WpeStrdup("a.out");*/
  if (e_s_prog.intstr) FREE(e_s_prog.intstr);
  e_s_prog.intstr = WpeStrdup(cc_intstr);
  strcpy(library, "");
  for (i = !save_df ? 0 : 1; i < 3; i++)
  {
   e_p_df[i] = MALLOC(sizeof(struct dirfile));
   e_p_df[i]->name = MALLOC(sizeof(char *));
   e_p_df[i]->name[0] = MALLOC(2 * sizeof(char));
   *e_p_df[i]->name[0] = ' '; *(e_p_df[i]->name[0] + 1) = '\0';
   e_p_df[i]->anz = 1;
  }
  if (save_df) e_p_df[0] = save_df;
  return(e_p_df);
 }
 if (!(e_p_df[1] = MALLOC(sizeof(struct dirfile)))) return(e_p_df);
 if (!(e_p_df[1]->name = MALLOC(sizeof(char *)))) return(e_p_df);
 e_p_df[1]->anz = 0;
 if (!(e_p_df[2] = MALLOC(sizeof(struct dirfile)))) return(e_p_df);
 if (!(e_p_df[2]->name = MALLOC(sizeof(char *)))) return(e_p_df);
 e_p_df[2]->anz = 0;
 for (i = 0; i < p_v_n; i++)
 {
  if (!strcmp(p_v[i]->var, "CMP"))
  {
   if (e_s_prog.compiler) FREE(e_s_prog.compiler);
   e_s_prog.compiler = WpeStrdup(p_v[i]->string);
  }
  else if (!strcmp(p_v[i]->var, "CMPFLAGS"))
  {
   if (e_s_prog.comp_str) FREE(e_s_prog.comp_str);
   e_s_prog.comp_str = WpeStrdup(p_v[i]->string);
  }
  else if (!strcmp(p_v[i]->var, "LDFLAGS"))
  {
   if (e_s_prog.libraries) FREE(e_s_prog.libraries);
   e_s_prog.libraries = WpeStrdup(p_v[i]->string);
  }
  else if (!strcmp(p_v[i]->var, "EXENAME"))
  {
   if (e_s_prog.exe_name) FREE(e_s_prog.exe_name);
   e_s_prog.exe_name = WpeStrdup(p_v[i]->string);
  }
  else if (!strcmp(p_v[i]->var, "CMPMESSAGE"))
  {
   if (e_s_prog.intstr) FREE(e_s_prog.intstr);
   e_s_prog.intstr = WpeStrdup(e_interpr_var(p_v[i]->string));
  }

/**************************/
/**** 
  this is needed, because this function needs to understand that
  BREAKPOINTS and WATCHES are project variables.
  These variables will be processed later on in e_rel_brkwtch
  function.
****/  
  else if (!strcmp(p_v[i]->var, "BREAKPOINTS"))
  {
  }
  else if (!strcmp(p_v[i]->var, "WATCHES"))
  {
  }
/**************************/

  else if (!strcmp(p_v[i]->var, "LIBNAME"))
   /* Bounded: LIBNAME= from the .prj can be arbitrarily long. */
   snprintf(library, sizeof(library), "%s", p_v[i]->string);
  else if (!strcmp(p_v[i]->var, "CMPSWTCH"))
  {
   if (!strcmp(p_v[i]->string, "other")) e_s_prog.comp_sw = 1;
  }
  else if (!strcmp(p_v[i]->var, "FILES"))
   e_p_df[0] = e_p_get_args(p_v[i]->string);
  else
  {
   e_p_df[1]->anz++;
   if (!(e_p_df[1]->name = REALLOC(tmp =
				e_p_df[1]->name, e_p_df[1]->anz * sizeof(char *))))
   {  e_p_df[1]->anz--;  e_p_df[1]->name = tmp;  return(e_p_df);  }
   if (!(e_p_df[1]->name[e_p_df[1]->anz-1] = MALLOC((strlen(p_v[i]->var)
			+ strlen(p_v[i]->string) + 2)*sizeof(char))))
   {  e_p_df[1]->anz--;  return(e_p_df);  }
   sprintf(e_p_df[1]->name[e_p_df[1]->anz-1], "%s=%s",
					p_v[i]->var, p_v[i]->string);
  }
 }
 if (!e_s_prog.compiler)
  e_s_prog.compiler = WpeStrdup("gcc");
 if (!e_s_prog.comp_str)
  e_s_prog.comp_str = WpeStrdup("-g");
 if (!e_s_prog.libraries)
  e_s_prog.libraries = WpeStrdup("");
 if (!e_s_prog.exe_name)
 {
  /* Project my_prog.prj defaults to an executable of my_prog BD */
  strcpy(text, e_prog.project);
  e_s_prog.exe_name = WpeStrdup(WpeStringCutChar(text, '.'));
  /*e_s_prog.exe_name = WpeStrdup("a.out");*/
 }
 if (!e_s_prog.intstr)
  e_s_prog.intstr = WpeStrdup(cc_intstr);
 if (!e_p_df[0])
 {
  e_p_df[0] = MALLOC(sizeof(struct dirfile));
  e_p_df[0]->anz = 0;
 }
 if ((fp = fopen(e_prog.project, "r")) == NULL)
 {
  sprintf(text, e_msg[ERR_FOPEN], e_prog.project);
  e_error(text, 0, f->fb);
  return(e_p_df);
 }
 while ((tp = fgets(text, 256, fp)))
 {
  if (text[0] == '\t') continue;
  for (i = 0; isspace(text[i]); i++);
  if (!strncmp(text+i, "install:", 8))
  {
   while(tp && (text[j = strlen(text)-1] != '\n' || text[j-1] == '\\'))
    tp = fgets(text, 256, fp);
   break;
  }
 }
 if (!tp) {  fclose(fp);  return(e_p_df);  }
 while(tp && (tp = fgets(text, 256, fp)))
 {
  for (i = 0; isspace(text[i]); i++);
  sp = text+i;
  if (sp[0] == '#')
  {
   while(tp && (text[j = strlen(text)-1] != '\n' || text[j-1] == '\\'))
    tp = fgets(text, 256, fp);
   continue;
  }
  if (text[0] != '\t') break;
  if (sp[0] == '\0') continue;
  e_p_df[2]->anz++;
  if (!(e_p_df[2]->name = REALLOC(tmp =
				e_p_df[2]->name, e_p_df[2]->anz * sizeof(char *))))
  {  e_p_df[2]->anz--;  e_p_df[2]->name = tmp;  fclose(fp);  return(e_p_df);  }
  if (!(e_p_df[2]->name[e_p_df[2]->anz-1] = MALLOC((strlen(sp) + 1))))
  {  e_p_df[2]->anz--;  fclose(fp);  return(e_p_df);  }

  strcpy(e_p_df[2]->name[e_p_df[2]->anz-1], sp);
  while(tp && (text[j = strlen(text)-1] != '\n' || text[j-1] == '\\'))
  {
   tp = fgets(text, 256, fp);
   if (tp)
   {
    j = strlen(e_p_df[2]->name[e_p_df[2]->anz-1]);
    *(e_p_df[2]->name[e_p_df[2]->anz-1]+j-2) = '\0';
    if (!(e_p_df[2]->name[e_p_df[2]->anz-1] =
		    REALLOC(sp = e_p_df[2]->name[e_p_df[2]->anz-1],
			strlen(e_p_df[2]->name[e_p_df[2]->anz-1])
			+ strlen(text) + 1)))
    {  fclose(fp);  FREE(sp);  e_error(e_msg[ERR_LOWMEM], 0, f->fb);
	       return(e_p_df);
    }
    strcat(e_p_df[2]->name[e_p_df[2]->anz-1], text);
   }
  }
  j = strlen(e_p_df[2]->name[e_p_df[2]->anz-1]);
  if (*(e_p_df[2]->name[e_p_df[2]->anz-1]+j-1) == '\n')
   *(e_p_df[2]->name[e_p_df[2]->anz-1]+j-1) = '\0';
 }
 fclose(fp);
 for (i = 0; i < 3; i++)
 {
  if (!e_p_df[i])
  {
   e_p_df[i] = MALLOC(sizeof(struct dirfile));
   e_p_df[i]->name = MALLOC(sizeof(char *));
   e_p_df[i]->anz = 0;
  }
  e_p_df[i]->name = REALLOC(e_p_df[i]->name,
				(e_p_df[i]->anz + 1) * sizeof(char *));
  e_p_df[i]->name[e_p_df[i]->anz] = MALLOC(2*sizeof(char));
  *e_p_df[i]->name[e_p_df[i]->anz] = ' ';
  *(e_p_df[i]->name[e_p_df[i]->anz] + 1) = '\0';
  e_p_df[i]->anz++;
 }
 if (save_df) {  freedf(e_p_df[0]);  e_p_df[0] = save_df;  }
 return(e_p_df);
}

int freedfN(struct dirfile **df, int n)
{
 int i;

 for(i = 0; i < n; i++)
  if(df[i])
   freedf(df[i]);
 FREE(df);
 return(0);
}

int e_wrt_prj_fl(FENSTER *f)
{
 int i, len;
 FILE *fp;
 char text[256];

 i = e_find_project_window(f->ed);
 if (i == 0 || e_prog.project[0] == DIRC)
  strcpy(text, e_prog.project);
 else
  sprintf(text, "%s/%s", f->ed->f[i]->dirct, e_prog.project);
 if ((fp = fopen(text, "w")) == NULL)
 {
  sprintf(text, e_msg[ERR_FOPEN], e_prog.project);
  e_error(text, 0, f->fb);
  return(-1);
 }
 fprintf(fp, "#\n# xwpe - project-file: %s\n", e_prog.project);
 fprintf(fp, "# created by xwpe version %s\n#\n", VERSION);
 for (i = 0; i < e_p_df[1]->anz; i++)
  fprintf(fp, "%s\n", e_p_df[1]->name[i]);
 fprintf(fp, "\nCMP=\t%s\n", e_s_prog.compiler);
 fprintf(fp, "CMPFLAGS=\t%s\n", e_s_prog.comp_str);
 fprintf(fp, "LDFLAGS=\t%s\n", e_s_prog.libraries);
 fprintf(fp, "EXENAME=\t%s\n", e_s_prog.exe_name);
 if (library[0])
  fprintf(fp, "LIBNAME=\t%s\n", library);
 fprintf(fp, "CMPSWTCH=\t%s\n", e_s_prog.comp_sw ? "other" : "gnu");
 fprintf(fp, "CMPMESSAGE=\t\'");
 for (i = 0; e_s_prog.intstr[i]; i++)
 {
  if (e_s_prog.intstr[i] == '\n')
   fprintf(fp, "\\n");
  else if (e_s_prog.intstr[i] == '\r')
   fprintf(fp, "\\r");
  else if (e_s_prog.intstr[i] == '\\' || e_s_prog.intstr[i] == '\'' ||
    e_s_prog.intstr[i] == '\"' )
  {
   fputc('\\', fp);
   fputc(e_s_prog.intstr[i], fp);
  }
  else fputc(e_s_prog.intstr[i], fp);
 }
 fprintf(fp, "\'\n");
 fprintf(fp, "\nFILES=\t");
 for (i = 0, len = 8; i < e_p_df[0]->anz; i++)
 {
  len += strlen(e_p_df[0]->name[i]);
  if (len > 80)
  {
   fprintf(fp, " \\\n\t");
   len = 1;
  }
  fprintf(fp, "%s ", e_p_df[0]->name[i]);
 }
 fprintf(fp, "\n");
   
/*****************************************/   
/****  save WATCHES and BREAKPOINTS   ****/
 if (e_d_nbrpts > 0)
 {
  fprintf(fp, "\nBREAKPOINTS=\t");
  for (i = 0; i < (e_d_nbrpts-1); i++)
  {
   fprintf(fp, "%s:%d;",e_d_sbrpts[i],e_d_ybrpts[i]);
  }
  fprintf(fp, "%s:%d",e_d_sbrpts[e_d_nbrpts-1],e_d_ybrpts[e_d_nbrpts-1]);
 }

 if (e_d_nwtchs > 0)
 {
  fprintf(fp, "\nWATCHES=\t");
  for (i = 0; i < (e_d_nwtchs-1); i++)
  {
   fprintf(fp, "%s;",e_d_swtchs[i]);
  }
  fprintf(fp, "%s",e_d_swtchs[e_d_nwtchs-1]);
 }   
 fprintf(fp, "\n");
/*****************************************/   

 if (e_p_df[2]->anz > 0)
  fprintf(fp, "\ninstall:\n");
 for (i = 0; i < e_p_df[2]->anz; i++)
  fprintf(fp, "\t%s\n", e_p_df[2]->name[i]);
 fclose(fp);
 return(0);
}

int e_p_update_prj_fl(FENSTER *f)
{
 if(!e_make_prj_opt(f))
  return(-1);
 if(e_wrt_prj_fl(f))
  return(-1);
 return(0);
}

int e_p_add_df(FLWND *fw, int sw)
{
 char *title = NULL, str[256];
 int i;

 if (sw == 4)
  title = "Add File";
 else if (sw == 5)
  title = "Add Variable";
 else if (sw == 6)
  title = "Add Command";
 str[0] = '\0'; /* terminate new string to prevent garbage in display */
 if (e_add_arguments(str, title, fw->f, 0, AltA, NULL))
 {
  fw->df->anz++;
  fw->df->name = REALLOC(fw->df->name, fw->df->anz * sizeof(char *));
  for (i = fw->df->anz - 1; i > fw->nf; i--)
   fw->df->name[i] = fw->df->name[i-1];
  fw->df->name[i] = MALLOC(strlen(str)+1);
  strcpy(fw->df->name[i], str);
 }
 return(0);
}

int e_p_edit_df(FLWND *fw, int sw)
{
 char *title = NULL, str[256];
 int new = 0;
 if (sw == 4)
  title = "Change Filename";
 else if (sw == 5)
  title = "Change Variable";
 else if (sw == 6)
  title = "Change Command";
 if (fw->nf < fw->df->anz-1 && fw->df->name[fw->nf])
  strcpy(str, fw->df->name[fw->nf]);
 else
 {
  new = 1;
  str[0] = '\0';
 }
 if (e_add_arguments(str, title, fw->f, 0, AltA, NULL))
 {
  if (fw->nf > fw->df->anz-2)
  {
   fw->nf = fw->df->anz-1;
   fw->df->anz++;
   fw->df->name = REALLOC(fw->df->name, fw->df->anz * sizeof(char *));
   fw->df->name[fw->df->anz-1] = fw->df->name[fw->df->anz-2];
  }
  if (!new)
   FREE(fw->df->name[fw->nf]);
  fw->df->name[fw->nf] = MALLOC(strlen(str)+1);
  if (fw->df->name[fw->nf])
   strcpy(fw->df->name[fw->nf], str);
 }
 return(0);
}

int e_p_del_df(FLWND *fw, int sw)
{
 int i;

 /* Nothing to delete only when the list is empty or the selection is out of
    range. The old guard (nf > anz-2) wrongly refused to delete the LAST entry
    (including the single remaining file). */
 if (fw->df->anz <= 0 || fw->nf < 0 || fw->nf >= fw->df->anz)
  return(0);
 fw->df->anz--;
 for (i = fw->nf; i < fw->df->anz; i++)
  fw->df->name[i] = fw->df->name[i+1];
 /* Keep the highlight on a valid row after removing the last one. */
 if (fw->nf >= fw->df->anz && fw->nf > 0)
  fw->nf--;
 return(0);
}

int e_p_mess_win(char *header, int argc, char **argv, PIC **pic, FENSTER *f)
{
 char *tmp = MALLOC(sizeof(char));
 int i;

 tmp[0] = '\0';
 for (i = 0; i < argc && argv[i] != NULL; i++)
 {
  if(!(tmp = REALLOC(tmp, (strlen(tmp)+strlen(argv[i])+2)*sizeof(char))))
   return(-2);
  strcat(tmp, argv[i]);
  strcat(tmp, " ");
 }
 e_d_p_message(tmp, f, 1);
 FREE(tmp);
 return(0);
}

/* After this function b has exactly 1 line allocated (b->mxlines==1).
   This line is initialized to the string WPE_WR,0 */
int e_p_red_buffer(BUFFER *b)
{
 int i;

 for (i = 1; i < b->mxlines; i++)
  if (b->bf[i].s != NULL)
   FREE( b->bf[i].s );
 if (b->mxlines==0) e_new_line(0,b);
 b->bf[0].s[0] = WPE_WR;
 b->bf[0].s[1] = '\0';
 b->bf[0].len = 0;
 b->bf[0].nrc = 1;
 b->mxlines = 1;
 return(0);
}

int e_new_message(FENSTER *f)
{
 int i;

 if (e_p_m_buffer)
  e_p_red_buffer(e_p_m_buffer);
 for (i = f->ed->mxedt; i > 0; i--)
  if (f->ed->f[i] && f->ed->f[i]->datnam &&
      !strcmp(f->ed->f[i]->datnam, "Messages"))
  {
   e_switch_window(f->ed->edt[i], f->ed->f[f->ed->mxedt]);
   e_close_window(f->ed->f[f->ed->mxedt]);
   break;
  }
 if (access("Messages", 0) == 0)
  remove("Messages");
 if (e_edit(f->ed, "Messages"))
  return(WPE_ESC);
 return(0);
}

int e_p_show_messages(FENSTER *f)
{
 int i;

 for (i = f->ed->mxedt; i > 0; i--)
  if (f->ed->f[i] && f->ed->f[i]->datnam &&
      !strcmp(f->ed->f[i]->datnam, "Messages"))
  {
   e_switch_window(f->ed->edt[i], f->ed->f[f->ed->mxedt]);
   break;
  }
 if (i <= 0 && e_edit(f->ed, "Messages"))
 {
  return(-1);
 }
 f = f->ed->f[f->ed->mxedt];
 if (f->b->mxlines == 0)
 {
  e_new_line(0, f->b);
  e_ins_nchar(f->b, f->s, "No Messages", 0, 0, 11);
  e_schirm(f, 1);
 }
 return(0);
}

/**
 * e_p_show_program_output - Reveal the running program's output (Ctrl-G P).
 *
 * Bound to "Output" (Ctrl-G P / Alt-F5).  In 1993 this switched the whole
 * screen to a separate "user screen" because the program shared the one
 * terminal with the editor.  With the pty architecture every byte the
 * program prints is already captured into the Messages window, so in X11
 * mode there is no separate screen to switch to -- the output lives in the
 * integrated panel.  This raises and focuses the Messages window and scrolls
 * it to the latest output, the way a modern IDE focuses its output panel:
 * the user presses Ctrl-G P and immediately sees what the program printed,
 * scrollable and in colour, with no modal popup to dismiss.
 *
 * Return: 0 always (Window-menu callback convention).
 */
int e_p_show_program_output(FENSTER *f)
{
 FENSTER *mf;

 e_p_show_messages(f);                       /* find/create + focus */
 mf = f->ed->f[f->ed->mxedt];                /* now the active window */
 if (strcmp(mf->datnam, "Messages"))
  return(0);
 if (mf->b->mxlines > 0)
 {
  int last = mf->b->mxlines - 1;
  mf->b->b.y = last;
  /* Sit at the stream write position -- the end of the last line, like a
     tailing terminal -- so output continues (and interactive input goes)
     right after the last character, not on top of it.  For a line ending
     in a newline the last line is empty, so this is column 0. */
  mf->b->b.x = mf->b->bf[last].len;
 }
 e_messages_scroll_to_bottom(mf);
 e_schirm(mf, 1);
 return(0);
}

/**
 * e_strip_quotes - Remove surrounding double quotes from a string in place.
 * @s: The string to modify.
 *
 * If @s is surrounded by double quotes (e.g. "filename.py"), they are
 * removed.  Used by the error pattern matcher to handle Python-style
 * error output: File "name.py", line N.
 */
static void e_strip_quotes(char *s)
{
 int len = strlen(s);
 if (len >= 2 && s[0] == '"' && s[len-1] == '"')
 {
  memmove(s, s+1, len-2);
  s[len-2] = '\0';
 }
}

int e_p_konv_mess(char *var, char *str, char *txt, char *file, char *cmp,
  int *y, int *x)
{
 int i;
 char *cp;

 if (!strncmp(var, "FILE", 4) && !isalnum(var[4]))
 {
  for (i = strlen(str) - 1; i >= 0 && !isspace(str[i]); i--)
   ;
  strcpy(file, str+i+1);
  e_strip_quotes(file);
 }
 else if (!strncmp(var, "CMPTEXT", 7) && !isalnum(var[7]))
  strcpy(cmp, str);
 else if (!strncmp(var, "LINE", 4) && !isalnum(var[4]))
 {
  if (!isdigit(str[0]))
   return(1);
  *y = atoi(str);
  if (var[4] == '+')
   *y += atoi(var+5);
  else if (var[4] == '-')
   *y -= atoi(var+5);
 }
 else if (!strncmp(var, "COLUMN", 6) && !isalnum(var[6]))
 {
  if (!strncmp(var+6, "=BEFORE", 7))
  {
   txt[0] = 'B';
   strcpy(txt+1, str);
   *x = 0;
   var += 13;
  }
  else if (!strncmp(var+6, "=AFTER", 6))
  {
   txt[0] = 'A';
   strcpy(txt+1, str);
   *x = strlen(str);
   var += 12;
  }
  else if (!strncmp(var+6, "=PREVIOUS?", 10))
  {
   if (!str[0])
    return(1);
   for (i = 0; (txt[i] = var[16+i]) && txt[i] != '+' && txt[i] != '-'; i++)
    ;
   txt[i] = '\0';
   var += (16+i);
   cp = strstr(str, txt);
   for (i = 0; str+i < cp; i++)
    ;
   *x = i;
   txt[0] = 'P'; txt[1] = '\0';
  }
  else if (!isdigit(str[0]))
   return(1);
  else
  {
   *x = atoi(str);
   txt[0] = '\0';
   var += 6;
  }
  if (var[0] == '+')
   *x += atoi(var+1);
  else if (var[0] == '-')
   *x -= atoi(var+1);
 }
 return(0);
}

int e_p_comp_mess(char *a, char *b, char *c, char *txt, char *file, char *cmp,
  int *y, int *x)
{
 int i, n, k = 0, bsl = 0;
 char *ctmp, *cp, *var = NULL, *str = NULL;

 if (c > b)
  return(0);
 if (a[0] == '*' && !a[1])
  return(2);
 if (!a[0] && !b[0])
  return(2);
 if (!a[0] || !b[0])
  return(0);
 if (a[0] == '*' && (a[1] == '*' || a[1] == '$'))
  return(e_p_comp_mess(++a, b, c, txt, file, cmp, y, x));
 if (a[0] == '$' && a[1] == '{')
 {
  for (k = 2; a[k] && a[k] != '}'; k++);
  var = MALLOC((k-1) * sizeof(char));
  for (i = 2; i < k; i++)
   var[i-2] = a[i];
  var[k-2] = '\0';
  if (a[k])
   k++;
  if (!a[k])
   return(!e_p_konv_mess(var, b, txt, file, cmp, y, x));
  n = a[k] == '\\' ? k : k+1;
 }
 else if (a[0] == '*'&& a[1] != '\\')
 {
  k = 1;
  n = 2;
 }
 else
  n = 1;
 for(; bsl || (a[n] && a[n] != '*' && a[n] != '?' && a[n] != '[' &&
   (a[n] != '$' || a[n+1] != '{' )); n++)
  bsl = a[n] == '\\' ? !bsl : 0;
 if (a[0] == '*' || a[0] == '$')
 {
  if (a[k] == '?')
  {
   cp = MALLOC((strlen(a)+1)*sizeof(char));
   for (i = 0; i < k && (cp[i] = a[i]); i++);
   for (i++; (cp[i-1] = a[i]) != '\0'; i++);
   FREE(var);
   n = e_p_comp_mess(cp, ++b, ++c, txt, file, cmp, y, x);
   FREE(cp);
   return(n);
  }
  if (a[k] == '[')
  {
   for (i = 0; b[i] &&
     !(n = e_p_comp_mess(a+k, b+i, c+i, txt, file, cmp, y, x)); i++)
    ;
   if (!b[i])
    return(0);
   if (a[0] == '$')
   {
    str = MALLOC((i+1)*sizeof(char));
    for (k = 0; k < i; k++)
     str[k] = b[k];
    str[i] = '\0';
    e_p_konv_mess(var, str, txt, file, cmp, y, x);
    FREE(var);
    FREE(str);
   }
   return(n);
  }
  n -= k;
  ctmp = MALLOC(n+1);
  for (i = 0; i < n; i++)
   ctmp[i] = a[i+k];
  ctmp[n] = '\0';
  cp = strstr(b, ctmp);
  FREE(ctmp);
  if (cp == NULL)
   return(0);
  if (a[0] == '$')
  {
   for (i = 0; c + i < cp; i++);
   str = MALLOC((i+1)*sizeof(char));
   for (i = 0; c + i < cp; i++)
    str[i] = c[i];
   str[i] = '\0';
   i = e_p_konv_mess(var, str, txt, file, cmp, y, x);
   FREE(var);
   FREE(str);
   if (i)
    return(0);
  }
  if (!a[k+n] && !cp[n])
   return(2);
  if (!a[k+n])
   return(e_p_comp_mess(a, cp+1, cp+1, txt, file, cmp, y, x));
  if ((i = e_p_comp_mess(a+k+n, cp+n, cp+n, txt, file, cmp, y, x)))
   return(i);
  if (file[0] && *y > -1)
   return(0);
  return(e_p_comp_mess(a, cp+1, a[0] == '$' ? c : cp+1, txt, file, cmp, y, x));
 }
 else if (a[0] == '?')
 {
  n--;
  a++;
  b++;
 }
 else if (a[0] == '[')
 {
  if (a[1] == '!')
  {
   for (k = 2; a[k] && (a[k] != ']' || k == 2) && a[k] != b[0]; k++)
    if (a[k+1] == '-' && b[0] >= a[k] && b[0] <= a[k+2])
     return(-b[0]);
   if (a[k] != ']')
    return(-b[0]);
   n-=(k+1);
   a+=(k+1);
   b++;
  }
  else
  {
   for (k = 1; a[k] && (a[k] != ']' || k == 1) && a[k] != b[0]; k++)
    if (a[k+1] == '-' && b[0] >= a[k] && b[0] <= a[k+2])
     break;
   if (a[k] == ']' || a[k] == '\0')
    return(0);
   for(; a[k] && (a[k] != ']'); k++);
   n-=(k+1);
   a+=(k+1);
   b++;
  }
 }
 if (n <= 0)
  return(e_p_comp_mess(a, b, c, txt, file, cmp, y, x));
 if ((k = strncmp(a, b, n)) != 0)
  return(0);
 return(e_p_comp_mess(a+n, b+n, c+n, txt, file, cmp, y, x));
}

int e_p_cmp_mess(char *srch, BUFFER *b, int *ii, int *kk, int ret)
{
 char *cp, cmp[128], file[128], search[80], tmp[4][128], **wtxt = NULL;
 int j, l, m, n, iy, iorig, i = *ii, k = *kk, x = 0, y = -1, wnum = 0;
 int *wn = NULL;

 cmp[0] = search[0] = file[0] = '\0';
 wtxt = MALLOC(1);
 wn = MALLOC(1);
 for (j = 0, n = 0; n < 4 && srch[j]; n++)
 {
  for (l = 0; (tmp[n][l] = srch[j]); j++, l++)
  {
   if (j > 1 && srch[j] == '?' && srch[j-1] == '{' && srch[j-2] == '$')
   {
    wnum++;
    wn = REALLOC(wn, wnum * sizeof(int));
    wtxt = REALLOC(wtxt, wnum * sizeof(char *));
    if (srch[j+1] == '*')
     wn[wnum-1] = -1;
    else
     wn[wnum-1] = atoi(srch+j+1);
    for (j++; srch[j] && srch[j] != ':'; j++);
    if (!srch[j])
    {
     wnum--;
     break;
    }
    for (m = 0; srch[j+m] && srch[j+m] != '}'; m++);
    wtxt[wnum-1] = MALLOC((m+1) * sizeof(char));
    for (m = 0, j++; (wtxt[wnum-1][m] = srch[j]) && srch[j] != '}'; j++, m++);
    wtxt[wnum-1][m] = '\0';
    l -= 3;
   }
   else if (srch[j] == '\r' || srch[j] == '\n')
   {
    if (srch[j+1] == '\r' || srch[j+1] == '\n')
    {
     tmp[n][l] = '\n';
     tmp[n][l+1] = '\0';
     j++;
    }
    else
     tmp[n][l] = '\0';
    j++;
    break;
   }
  }
 }
 e_p_comp_mess(tmp[0], b->bf[i].s, b->bf[i].s, search, file, cmp, &y, &x);
 iy = i;
 iorig = i;
 do
 {
  if (n > 1 && file[0] && i < b->mxlines-1)
  {
   y = -1;
   while (b->bf[i].s[b->bf[i].len-1] == '\\')
    i++;
   i++;
   e_p_comp_mess(tmp[1], b->bf[i].s, b->bf[i].s, search, file, cmp, &y, &x);
   iy = i;
  }
  do
  {
   if (n > 2 && file[0] && y >= 0 && i < b->mxlines-1)
   {
    while (b->bf[i].s[b->bf[i].len-1] == '\\')
     i++;
    i++;
    l = e_p_comp_mess(tmp[2], b->bf[i].s, b->bf[i].s, search, file, cmp, &y, &x);
    if (!l && n > 3)
     l = e_p_comp_mess(tmp[3], b->bf[i].s, b->bf[i].s, search, file, cmp, &y, &x);
   }
   else
    l = 1;
   if (file[0] && y >= 0 && l != 0)
   {
    err_li[k].file = MALLOC((strlen(file)+1)*sizeof(char));
    strcpy(err_li[k].file, file);
    err_li[k].line = y;
    if (search[0] == 'P')
    {
     cp = strstr(b->bf[iy].s, cmp);
     if (!cp)
      x = 0;
     else
     {
      for (m = 0; b->bf[iy].s + m < (unsigned char *)cp; m++);
      x -= m;
     }
     err_li[k].srch = MALLOC((strlen(cmp)+2)*sizeof(char));
     err_li[k].srch[0] = 'P';
     strcpy(err_li[k].srch+1, cmp);
    }
    else if (search[0])
    {
     err_li[k].srch = MALLOC((strlen(search)+1)*sizeof(char));
     strcpy(err_li[k].srch, search);
    }
    else
     err_li[k].srch = NULL;
    err_li[k].x = x;
    err_li[k].y = iorig;
    err_li[k].text = MALLOC(strlen((char *)b->bf[i].s) + 1);
    strcpy(err_li[k].text, (char *)b->bf[i].s);
    err_li[k].text[b->bf[i].len] = '\0';
    k++;
    err_num++;
    if (!ret)
    {
     for (ret = -1, m = 0; ret && m < wnum; m++)
     {
      if (wn[m] == -1 && !(b->cn->edopt & ED_MESSAGES_STOP_AT) &&
        strstr(b->bf[i].s, wtxt[m]))
       ret = 0;
      else if (wn[m] > -1 && !(b->cn->edopt & ED_MESSAGES_STOP_AT) &&
        !strncmp(b->bf[i].s+wn[m], wtxt[m], strlen(wtxt[m])))
       ret = 0;
     }
    }
    if (!ret && wnum <= 0)
     ret = -1;
    while (b->bf[i].s[b->bf[i].len-1] == '\\')
     i++;
   }
  } while (n > 2 && file[0] && y >= 0 && l != 0 && i < b->mxlines-1);
  if (n > 2 && file[0] && y >= 0 && l == 0)
   i--;
 } while (n > 1 && file[0] && y >= 0 && i < b->mxlines-1);
 if (n > 1 && file[0] && y < 0)
  i--;
 *ii = i;
 *kk = k;
 for (m = 0; m < wnum; m++)
  FREE(wtxt[m]);
 FREE(wn);
 FREE(wtxt);
 return(ret);
}

#endif

