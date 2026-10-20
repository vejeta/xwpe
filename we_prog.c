/* we_prog.c                                             */
/* Copyright (C) 1993 Fred Kruse                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

#include "messages.h"
#include "edit.h"
#include "WeExpArr.h"
#include "we_ai.h"       /* AI assistant entry point (guarded by WPE_AI) */

#ifdef PROG

#include <time.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>     /* TIOCSCTTY: pulled in transitively on glibc, explicit on the BSDs */
#include <signal.h>
#include <poll.h>
#include <errno.h>
#if defined(HAVE_PTY_H)
# include <pty.h>          /* glibc: openpty/forkpty */
#elif defined(HAVE_UTIL_H)
# include <util.h>         /* macOS, *BSD */
#elif defined(HAVE_LIBUTIL_H)
# include <libutil.h>      /* some BSDs */
#endif
#include "we_fdloop.h"
#include <stdint.h>
#ifdef __linux__
# include <sys/timerfd.h>  /* the waiting line's heartbeat while a build runs */
#endif

/* A path handed to a build command, or a directory a build runs in. */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define WPE_BUILD_PATH_MAX PATH_MAX

int e_run_sh(FENSTER *f);
char *e_prj_dir(void);
static void e_prj_anchor_path(void);
int e_make_library(char *library, char *ofile, FENSTER *f,
                   e_build_cont done, void *data);
int e_p_exec(FENSTER *f, PIC *pic, e_build_cont done, void *data);
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

/* One row of the error list.  x is the compiler's 1-based column (0 when it
   names none), y the Messages row Enter on which selects the entry (the
   first row of an include chain that leads to it), row the Messages row the
   entry was read from, kind what the compiler called it. */
struct ERR_LI  {  char *file, *text, *srch;  int x, y, line, row, kind;  } *err_li = NULL;
/* What a row of the error list is; both are counted when a build ends. */
#define E_ROW_ERROR   0
#define E_ROW_WARNING 1
/* A row make wrote about its own recipe ("make: *** [Makefile:3: all] Error
   1"): a stop of the walk, since it names the recipe's line, but not an
   error of the user's code, so the count leaves it out. */
#define E_ROW_MAKE    2
int err_no, err_num;
/* Whether a row of the current error list has been shown yet.  A new list
   (a compile, an agent changeset, a Multi-file accept) starts unwalked: the
   first Alt-T then shows its first row and the first Alt-V its last, in the
   list's order, wherever the cursor happens to be. */
static int err_walked;

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
/* a68g's row: the echoed source line, which starts with its number. */
#define A68G_INTSTR "${LINE} *"
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
 "no compiler entry for %s; add one in Options > Compiler",
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
#ifdef WPE_AI
  case AltG:   /*  Alt-G  AI assistant prefix (Alt-B is the Block menu; Alt-G is a
                   free Alt-letter, chosen like the LSP's Alt-Q so the assistant
                   never shadows an existing key).  The bar entry reads "Alt-G AI". */
   e_ai_ui_key(f);
   break;
  case WPE_AI_MENU:   /*  click on the "Alt-G AI" bottom-bar entry: the same door as
                          Alt-G, so the first-use notice is shown once, whichever
                          way the user came in */
   e_ai_ui_key(f);
   break;
#endif
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

/* e_build_dir - the directory a build/run/debug action must execute in, as a
   malloc'd ABSOLUTE path the caller FREEs (or NULL = keep the current CWD).
   xwpe historically never chdir()'d, so a forked compiler/make/program/debugger
   inherited the directory the shell launched wpe from; opening a file elsewhere
   (`cd /x; wpe sub/main.c`) then built/ran/debugged the wrong thing.  The action
   must happen where the code lives:
     - project mode (.prj open): the directory holding the .prj, so the project's
       Makefile and project-relative flags resolve;
     - otherwise:                the active file's own directory (f->dirct).
   A relative .prj path is taken from the current directory (e_prj_dir).
   Returns NULL when no target can be derived (no file dir) -- the caller then
   leaves the CWD untouched, exactly the pre-1.6.5 behaviour.
   Exposed (non-static) so the debugger launcher in we_debug.c shares the same
   directory policy. */
char *e_build_dir(FENSTER *f)
{
 if (e_project_is_open())
 {
  char *d = e_prj_dir();

  if (d)
   return(d);
 }
 if (f && f->dirct && f->dirct[0])
  return(WpeStrdup(f->dirct));
 return(NULL);
}

/**
 * e_prj_dir - The directory that holds the open project's .prj file.
 *
 * A project builds there whatever directory the editor was started from:
 * "wpe proj/p.prj" from the parent finds the project's sources, because a
 * relative .prj path is taken from the current directory.
 * Return: a new absolute path the caller frees, or NULL.
 */
char *e_prj_dir(void)
{
 char cwd[WPE_BUILD_PATH_MAX], *path, *slash, *dir;

 if (!e_prog.project || !e_prog.project[0])
  return(NULL);
 if (e_prog.project[0] == DIRC)
  path = WpeStrdup(e_prog.project);
 else if (!getcwd(cwd, sizeof cwd))
  return(NULL);
 else
  path = e_mkfilename(cwd, e_prog.project);
 slash = strrchr(path, DIRC);
 if (!slash)
 {
  FREE(path);
  return(NULL);
 }
 if (slash == path)
  slash++;                      /* a project in the root directory */
 *slash = '\0';
 dir = WpeStrdup(path);
 FREE(path);
 return(dir);
}

/**
 * e_prj_anchor_path - Make the open project's path absolute.
 *
 * A project build changes into the project's directory before it reads
 * the .prj file; a path relative to the directory the editor was started
 * in would no longer lead to it there.
 */
static void e_prj_anchor_path(void)
{
 char cwd[WPE_BUILD_PATH_MAX], *path;

 if (!e_prog.project || !e_prog.project[0] || e_prog.project[0] == DIRC)
  return;
 if (!getcwd(cwd, sizeof cwd))
  return;
 path = e_mkfilename(cwd, e_prog.project);
 if (!path)
  return;
 FREE(e_prog.project);
 e_prog.project = path;
}

/* e_build_pushd - switch the process CWD to e_build_dir(f) before forking a
   build/run tool, stashing the old CWD in `saved` (size n).  The CWD is kept
   changed across the whole action (compile/make/run) on purpose: relative
   output paths (e.g. exedir="."), the up-to-date stat() checks, and the error
   paths the tool reports all resolve against it.  Returns 1 if it changed the
   CWD (pair with e_build_popd), 0 if it left it untouched (no target dir, or
   getcwd/chdir failed -> the tool runs in the inherited CWD, as before). */
static int e_build_pushd(FENSTER *f, char *saved, size_t n)
{
 char *dir = e_build_dir(f);
 int ok;

 if (!dir)
  return(0);
 if (!getcwd(saved, n))
 {
  FREE(dir);
  return(0);
 }
 ok = (chdir(dir) == 0);
 FREE(dir);
 return(ok ? 1 : 0);
}

/* e_build_popd - restore the CWD saved by e_build_pushd once the action and its
   error handling are done. */
static void e_build_popd(char *saved)
{
 if (chdir(saved) != 0)
  return; /* the saved directory vanished mid-action; nothing safe to do */
}

static void e_make_multifile_hint(FENSTER *f);
static void e_p_set_compiled_source(const char *name);
static int e_build_chain_begin(FENSTER *f, e_build_cont done, void *data);
static void e_build_chain_end(FENSTER *f, int result, void *data);

/**
 * e_p_source_window - The window a build is about: the topmost one that is
 * not Messages.
 * @cn: the desktop.
 *
 * A build step ends with Messages on top, and the user may have moved
 * between windows while it ran, so each step finds the source again rather
 * than keeping a window that may have been closed meanwhile.
 * Return: the window.
 */
static FENSTER *e_p_source_window(ECNT *cn)
{
 int i;

 for (i = cn->mxedt; i > 0; i--)
  if (strcmp(cn->f[i]->datnam, "Messages"))
   break;
 if (i > 0)
  return(cn->f[i]);
 return(cn->f[cn->mxedt-1]);
}

/**
 * e_compile - Alt-C: compile the current file without linking, in the
 * background.
 */
int e_compile(FENSTER *f)
{
 if (e_build_chain_begin(f, NULL, NULL))
  return(-1);
 WpeMouseChangeShape(WpeWorkingShape);
 efildes[0] = efildes[1] = -1;
 wfildes[0] = wfildes[1] = -1;
 e_comp(f, e_build_chain_end, NULL);
 WpeMouseRestoreShape();
 return(0);
}

static void e_p_make_link(ECNT *cn);

/**
 * e_p_make_after_compile - The compile of F9 has ended: link, or stop.
 * @f: the top window; @result: the compile's result; @data: unused.
 */
static void e_p_make_after_compile(FENSTER *f, int result, void *data)
{
 (void)data;
 if (result)
 {
  e_build_chain_end(f, result, NULL);
  return;
 }
 e_p_make_link(f->ed);
}

/**
 * e_p_make_after_link - The link of F9 has ended: end the build.
 * @f: the top window; @result: the link's result; @data: unused.
 *
 * A link that fails on a lone file of a program with a makefile also says
 * how to build the whole program, after the jump to the error.
 */
static void e_p_make_after_link(FENSTER *f, int result, void *data)
{
 ECNT *cn = f->ed;

 (void)data;
 e_build_chain_end(f, result, NULL);
 if (result)
  e_make_multifile_hint(e_p_source_window(cn));
}

/**
 * e_p_make_then - F9: compile and link in the background, then run @done.
 * @f: the window F9 was pressed in.
 * @done: what runs once the build has ended (Ctrl-F9 runs the program, the
 * debugger starts), or NULL; it gets 0 for a success.
 * @data: handed to @done.
 *
 * Return: 0 when the build started, -1 when another build is running.
 */
int e_p_make_then(FENSTER *f, e_build_cont done, void *data)
{
 { const char *tp = getenv("XWPE_UI_TRACE");
   if (tp) { FILE *tf = fopen(tp, "a");
     if (tf) { fprintf(tf, "e_p_make ENTER\n"); fclose(tf); } } }
 if (e_build_chain_begin(f, done, data))
  return(-1);
 WpeMouseChangeShape(WpeWorkingShape);
 efildes[0] = efildes[1] = -1;
 wfildes[0] = wfildes[1] = -1;
 e_comp(f, e_p_make_after_compile, NULL);
 WpeMouseRestoreShape();
 return(0);
}

/**
 * e_p_make - F9 and Alt-M: build the current file or project.
 */
int e_p_make(FENSTER *f)
{
 return(e_p_make_then(f, NULL, NULL));
}

static void e_build_note_up_to_date(const char *target);

/**
 * e_p_make_note_program_current - The link found the program current.
 * @estr: the program of a single-file build.
 *
 * A compiler that writes no separate program (javac, fpc, scalac) has no
 * link step: what it found current is its own output, noted by the
 * compile.
 */
static void e_p_make_note_program_current(const char *estr)
{
 if (e_s_prog.comp_sw & 1)
  return;
 if (e__project)
  e_build_note_up_to_date(e_s_prog.exe_name);
 else
  e_build_note_up_to_date(estr);
}

/**
 * e_p_make_link - The link step of F9, once the compile succeeded.
 * @cn: the desktop.
 *
 * Skipped when the program is newer than its objects; otherwise the linker
 * runs as the next step of the same build.
 */
static void e_p_make_link(ECNT *cn)
{
 FENSTER *f;
 char ostr[128], estr[128], mstr[80];
 char cwd_saved[1024];
 int len, file = -1, cwd_chg = 0;
 struct stat cbuf[1], obuf[1];
 int linkRequest = 1; /* assume linking has to be done */

 f = e_p_source_window(cn);
 /* Link/up-to-date checks below use exedir="." and bare exe/object names; run
    them and the linker in the build directory so they resolve next to the
    code, not in wpe's launch directory. */
 cwd_chg = e_build_pushd(f, cwd_saved, sizeof(cwd_saved));
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
   e_p_exec(f, NULL, e_p_make_after_link, NULL);
  else
   e_p_make_after_link(f, WPE_ESC, NULL);
  if (cwd_chg)
   e_build_popd(cwd_saved);
  return;
 }
 /* Executable is up to date -- no linking needed */
 e_p_make_note_program_current(estr);
 if (!e__project)
  e_free_arg(e_arg, e_argc);
 if (cwd_chg)
  e_build_popd(cwd_saved);
 e_build_chain_end(f, 0, NULL);
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

/* The source Ctrl-F9 was pressed on, kept by value: the program runs after
   the build, and the window may have been closed while it built. */
static char e_run_src_dirct[256], e_run_src_datnam[256];

static int e_run_program(ECNT *cn);

/**
 * e_run_after_make - The build of Ctrl-F9 has ended: run the program if it
 * succeeded.
 * @f: the top window; @result: the build's result; @data: unused.
 */
static void e_run_after_make(FENSTER *f, int result, void *data)
{
 (void)data;
 if (result)
  return;
 e_run_program(f->ed);
}

/**
 * e_run - Ctrl-F9: build in the background, then run the program.
 */
int e_run(FENSTER *f)
{
 { const char *tp = getenv("XWPE_UI_TRACE");
   if (tp) { FILE *tf = fopen(tp, "a");
     if (tf) { fprintf(tf, "e_run ENTER datnam=%s\n",
              f && f->datnam ? f->datnam : "?"); fclose(tf); } } }
 if (e_build_busy())
 {
  e_build_refuse(f);
  return(-1);
 }
 efildes[0] = efildes[1] = -1;
 wfildes[0] = wfildes[1] = -1;
 if (!e_run_sh(f))
  return(0);
 strncpy(e_run_src_dirct, f->dirct, sizeof(e_run_src_dirct) - 1);
 e_run_src_dirct[sizeof(e_run_src_dirct) - 1] = '\0';
 strncpy(e_run_src_datnam, f->datnam, sizeof(e_run_src_datnam) - 1);
 e_run_src_datnam[sizeof(e_run_src_datnam) - 1] = '\0';
 return(e_p_make_then(f, e_run_after_make, NULL));
}

/**
 * e_run_program - Run the program that was just built, with its output in
 * Messages and on the user screen (Alt-F5).
 * @cn: the desktop.
 */
static int e_run_program(ECNT *cn)
{
 BUFFER *b;
 FENSTER *mf, *f;
 char estr[256];
 char *src_dirct = e_run_src_dirct, *src_datnam = e_run_src_datnam;
 char cwd_saved[1024];
 int len, ret, i, cwd_chg = 0;

 WpeMouseChangeShape(WpeWorkingShape);
 f = e_p_source_window(cn);
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
 /* Run the program IN ITS OWN directory: the exe is named relative ("./prog",
    "a.out") and, more importantly, the program itself should see its own
    directory as CWD so it finds/writes its data files there, not wherever wpe
    was launched from. */
 cwd_chg = e_build_pushd(f, cwd_saved, sizeof(cwd_saved));
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
 if (cwd_chg)
  e_build_popd(cwd_saved);
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
 * e_p_file_type - The extension that picks a file's compiler entry.
 * @name: the file name.
 *
 * Named when F9 finds no entry, so the user knows which suffix to add in
 * Options > Compiler.  A name with no extension is named whole.
 * Return: a pointer into @name.
 */
static const char *e_p_file_type(const char *name)
{
 const char *dot = strrchr(name, '.');

 if (dot)
  return(dot);
 return(name);
}

/* The last thing a build found already current: the object of a compile,
   or the program of a link.  Empty while every step had work to do. */
static char g_build_up_to_date[WPE_BUILD_PATH_MAX];

/**
 * e_build_note_up_to_date - A step of the build found @target current and
 * ran nothing.
 * @target: the object or program, as the build names it.
 *
 * When no step of the build ran a command, Messages ends the build with
 * "nothing to build: <target> is up to date", for F9, Alt-M and Alt-C
 * alike, so a key that had nothing to do never looks like a key that did
 * nothing.
 */
static void e_build_note_up_to_date(const char *target)
{
 if (!strncmp(target, "./", 2))
  target += 2;
 snprintf(g_build_up_to_date, sizeof g_build_up_to_date, "%s", target);
}

/* A source or object path handed to a build command.  The 128 bytes this
   replaces cut the path mid-word at 127 characters and handed the compiler the
   piece that was left, which then reported a linker input file it could not
   find: the message blamed the linker for a name the editor had truncated.
   A build path is a path, so it is sized like one (WPE_BUILD_PATH_MAX, at
   the top of this file). */

/**
 * e_build_path - Join a directory and a file name into a build command's path.
 * @out: where the path goes; @n: its size.
 * @dir: the directory, with or without a trailing separator.
 * @name: the file name.
 *
 * Both the source handed to the compiler and the object file it writes are
 * built this way, and both used to be formatted inline with the separator
 * decided twice.  A path that does not fit is refused here rather than cut,
 * because a cut path reaches the compiler as a different file and the error it
 * produces then points at the wrong thing.
 *
 * Return: 0, or -1 when the result would not fit, which the caller reports.
 */
static int e_build_path(char *out, size_t n, const char *dir, const char *name)
{
 size_t len = dir ? strlen(dir) : 0;
 int wrote;

 if (!out || !n || !dir || !name)
  return (-1);
 if (len > 0 && dir[len - 1] == DIRC)
  wrote = snprintf(out, n, "%s%s", dir, name);
 else
  wrote = snprintf(out, n, "%s%c%s", dir, DIRC, name);
 if (wrote < 0 || (size_t)wrote >= n)
  return (-1);
 return (0);
}

/**
 * e_build_path_too_long - Say which path did not fit, instead of cutting it.
 * @f: the window the message box belongs to; @what: "source" or "object".
 *
 * The user can act on this: shorten the path, or work nearer the root.  A
 * silent truncation cannot be acted on at all, because the name in the
 * compiler's complaint is not a name the user ever typed.
 */
static void e_build_path_too_long(FENSTER *f, const char *what)
{
 char msg[160];

 snprintf(msg, sizeof msg,
          "The %s path is longer than %d characters.\nCompile from a shorter path.",
          what, WPE_BUILD_PATH_MAX - 1);
 e_message(0, msg, f);
}

/**
 * e_comp_start - Start the compile of the current file (or the project).
 * @f: the window the key was pressed in; @done, @data: what runs when the
 * compile has ended.
 * @handed: set to 1 when @done now belongs to a running step.
 *
 * An object that is already current is noted, and the end of the build
 * says "nothing to build" when no step ran.
 * Return: the result, when @done was not handed on.
 */
static int e_comp_start(FENSTER *f, e_build_cont done, void *data,
                        int *handed)
{
 ECNT *cn = f->ed;
 PIC *pic = NULL;
 char **arg = NULL, fstr[WPE_BUILD_PATH_MAX], ostr[WPE_BUILD_PATH_MAX];
 char cwd_saved[1024];
 int i, file = -1, len, argc, cwd_chg = 0;
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
 {
  *handed = 1;
  return(e_c_project(f, done, data));
 }
 for (i = cn->mxedt; i > 0; i--)
 {
  if (e_check_c_file_w(cn->f[i]))
   break;
 }
 if (i == 0)
 {
  snprintf(ostr, sizeof(ostr), e_p_msg[ERR_S_NO_CFILE], e_p_file_type(f->datnam));
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
 /* The unconditional strcpy that stood here wrote f->datnam into fstr and was
    then overwritten by the join below, so it did nothing except risk its own
    overflow on a long name. */
 if (e_build_path(fstr, sizeof(fstr), f->dirct, f->datnam))
 {
  e_build_path_too_long(f, "source");
  if (arg) FREE(arg);
  return (-1);
 }
 argc = e_add_arg(&arg, fstr, argc, argc);
 e_p_set_compiled_source(fstr);
 if (e_build_path(ostr, sizeof(ostr), e_prog.exedir, f->datnam))
 {
  e_build_path_too_long(f, "object");
  if (arg) FREE(arg);
  return (-1);
 }
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
 /* Compile in the file's directory: the source is passed absolute, but the
    object path (exedir=".") and the stat() up-to-date checks below are
    relative, so without this they would land in / be looked for in wpe's
    launch directory when it differs from the file's. */
 cwd_chg = e_build_pushd(f, cwd_saved, sizeof(cwd_saved));
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
   if (cwd_chg)
    e_build_popd(cwd_saved);
   return(WPE_ESC);
  }
  remove(ostr);
  if ((file = e_exec_inf(f, arg, argc)) == 0)
  {
   e_sys_end();
   e_free_arg(arg, argc);
   if (cwd_chg)
    e_build_popd(cwd_saved);
   return(WPE_ESC);
  }
  e_sys_end();
  e_free_arg(arg, argc);
  *handed = 1;
  e_p_exec(f, pic, done, data);
  if (cwd_chg)
   e_build_popd(cwd_saved);
  return(0);
 }
 /* Object file is up to date -- no recompilation needed */
 e_sys_end();
 e_free_arg(arg, argc);
 if (cwd_chg)
  e_build_popd(cwd_saved);
 e_build_note_up_to_date(ostr);
 return(0);
}

static void e_build_continue(e_build_cont cont, FENSTER *f, int result, void *data);

/**
 * e_comp - Compile the current file, or build the open project, as a step
 * of a build.
 * @f: the window the key was pressed in; @done: what runs when the compile
 * has ended, exactly once, also when nothing needed compiling or the
 * compile could not start; @data: handed to @done.
 * Return: 0.
 */
int e_comp(FENSTER *f, e_build_cont done, void *data)
{
 int handed = 0;
 int ret = e_comp_start(f, done, data, &handed);

 if (!handed)
  e_build_continue(done, f, ret, data);
 return(0);
}

int e_exec_inf(FENSTER *f, char **argv, int n)
{
 int pid, nul;
#ifdef DEBUGGER
 if (e_d_swtch > 0)
  e_d_quit(f);
#endif
 fflush(stdout);
 /* One pipe for the child's stdout and stderr together: the lines reach
    Messages in the order the build wrote them (a make's echoed command
    before the compiler's errors).  Two pipes kept the two streams apart and
    the loop could read the later one first.  efildes carries the pipe;
    wfildes stays unused and closed, as the callers expect. */
 if (pipe(efildes) < 0)
 {
  e_error(e_p_msg[ERR_PIPEOPEN], 0, f->fb);
  return(0);
 }
 wfildes[0] = wfildes[1] = -1;

 if ((e_save_pid = pid = fork()) > 0)
 {
  /* The build leads its own process group, so Esc can end make together
     with the compiler it started.  Set on both sides of the fork: whichever
     runs first, the group exists before the parent may signal it. */
  setpgid(pid, pid);
  /* Parent: keep the read ends; close the write ends so read() sees EOF when
     the child exits, and CLOEXEC so a later child does not inherit them. */
  close(efildes[1]);
  efildes[1] = -1;
  fcntl(efildes[0], F_SETFD, FD_CLOEXEC);
  return(1);
 }
 if (pid < 0)
 {
  e_error(e_p_msg[ERR_PROCESS], 0, f->fb);
  close(efildes[0]); close(efildes[1]);
  efildes[0] = efildes[1] = -1;
  return(0);
 }

 /* Child: stdout and stderr -> the pipe's write end, and stdin -> /dev/null
    so the command can never grab the controlling tty and corrupt the
    parent's curses screen.  Then close every pipe fd (the dup2'd copies on
    0/1/2 survive). */
 setpgid(0, 0);
 dup2(efildes[1], 2);
 dup2(efildes[1], 1);
 if ((nul = open("/dev/null", O_RDONLY)) >= 0)
 {
  dup2(nul, 0);
  if (nul > 2)
   close(nul);
 }
 close(efildes[0]); close(efildes[1]);
 e_print_arg(stderr, "", argv, n);
 execvp(argv[0], argv);
 e_print_arg(stderr, e_p_msg[ERR_IN_COMMAND], argv, n);
 _exit(1);
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

/* e_make_multifile_hint - turn a confusing single-file link failure into
   guidance.  When F9 (Make) or Ctrl-F9 (Run) links the active file ALONE and it
   fails (typically "undefined symbols" because the program spans several .c/.cpp
   files), and the file's directory has a Makefile but no project is open, tell
   the user the two ways to build it for real: open a Project (.prj) listing all
   sources, or use Run -> Execute Make (Alt-A) to run the Makefile. */
static void e_make_multifile_hint(FENSTER *f)
{
 char path[1024];
 struct stat sb;

 if (e__project || !f || !f->dirct || !f->dirct[0])
  return;
 snprintf(path, sizeof(path), "%sMakefile", f->dirct);
 if (stat(path, &sb) != 0)
 {
  snprintf(path, sizeof(path), "%smakefile", f->dirct);
  if (stat(path, &sb) != 0)
   return;                       /* no Makefile -> nothing useful to suggest */
 }
 /* A modal info box (like the "Already compiled" hint) so it is seen even after
    e_show_error has jumped the view to the linker-error line. */
 e_message(0,
   "Linking this file alone failed -- it looks like a multi-file program.\n\n"
   "Open a Project (.prj) that lists every source, or use\n"
   "Run -> Execute Make (Alt-A) to build with the Makefile.", f);
}

/**
 * e_p_pump_complete_lines - Hand the complete lines of an accumulator to
 * Messages and keep the partial line that follows them intact.
 * @acc: the accumulator, NUL-terminated at *@acclen.
 * @acclen: its length; on return, the length of the partial line kept.
 * @last: the index of the last '\n' in @acc.
 * @b: the Messages buffer.
 *
 * A build writes in pieces of any size, so a read usually ends inside a
 * line.  The first byte of that partial line is saved before the complete
 * lines are cut off with a NUL and restored after, so the next read appends
 * to the whole line: every line the build prints reaches Messages, in order,
 * however the compiler chops its output.
 */
static void e_p_pump_complete_lines(char *acc, int *acclen, int last, BUFFER *b)
{
 int keep = *acclen - (last + 1);
 char first_kept = acc[last + 1];

 acc[last + 1] = '\0';
 print_to_end_of_buffer(b, acc, 0);
 acc[last + 1] = first_kept;
 memmove(acc, acc + last + 1, keep + 1);
 *acclen = keep;
}

/* e_p_pump_fd - read whatever is available on fd into the per-fd line
   accumulator (*acc / *acclen), append every COMPLETE line (through its '\n')
   to the Messages buffer b, and keep the trailing partial for next time.
   Returns the number of bytes read (>0) while the fd is open, 0 on EOF/error
   (the caller then closes it).  The byte count feeds the runaway cap in
   e_p_exec so a flooding child cannot pin xwpe at 100% CPU forever.
   Replaces the byte-at-a-time e_line_read, which also dropped the final
   fragment that had no trailing '\n'. */
/* e_p_pump_fd's answer when the pipe has nothing to read right now. */
#define E_PUMP_AGAIN (-1)

static int e_p_pump_fd(int fd, char **acc, int *acclen, BUFFER *b)
{
 char rbuf[4096];
 int r, last;

 r = read(fd, rbuf, sizeof(rbuf));
 if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
  return(E_PUMP_AGAIN);                 /* nothing yet: the loop wakes us again */
 if (r <= 0)
  return(0);                            /* 0 = EOF, <0 = error */
 *acc = REALLOC(*acc, *acclen + r + 1);
 memcpy(*acc + *acclen, rbuf, r);
 *acclen += r;
 (*acc)[*acclen] = '\0';
 if (!b)                                /* Messages is closed: keep it all */
  return(r);
 for (last = *acclen - 1; last >= 0 && (*acc)[last] != '\n'; last--)
  ;
 if (last >= 0)                         /* at least one complete line */
  e_p_pump_complete_lines(*acc, acclen, last, b);
 return(r);
}

/* ======================= the build on the editor's fd loop ===================
 * The compiler, the linker and make run as a background task, the way the
 * assistant's requests do: the child's stdout and stderr are registered with
 * the fd loop the editor already waits on for keys, each line lands in
 * Messages from that loop's callback, a heartbeat timer turns the waiting line
 * at the foot of Messages, and Esc ends the child.  The editor keeps taking
 * keys the whole time.  When the child is gone, what the old blocking reader
 * ran after its loop runs here, and then the step's continuation: the next
 * step of the build (the link after the compile, the next file of a project),
 * the program after a build (Ctrl-F9), the debugger after a build, or the end
 * of the build (the count row and the jump to the first error).
 */

/* The waiting line turns this often: the assistant's heartbeat cadence. */
#define E_BUILD_TICK_NS 120000000L
/* After Esc sends SIGTERM, this many ticks (about two seconds) pass before
   SIGKILL: a build that traps or ignores SIGTERM still ends. */
#define E_BUILD_KILL_TICKS 17
/* A child that prints more than this in one step is malfunctioning (a68g's
   abend recursion pinned the editor at 100% CPU draining it); it is killed
   at the source. */
#define E_BUILD_RUNAWAY_MAX (16UL * 1024 * 1024)
#define E_BUILD_WAIT_FMT "building ... %lds  (Esc cancels)"
#define E_BUILD_CANCELLED_LINE "build cancelled"
#define E_BUILD_RUNAWAY_LINE "xwpe: child output exceeded runaway cap, killed."
#define E_BUILD_SUCCESS_LINE "Success"
#define E_BUILD_BUSY_LINE " A build is running: Esc cancels it"
#define E_BUILD_NO_FD (-1)
/* e_make_error_list's verdict for output that is not a compiler's (a linker
   or a command that could not start): no row to jump to. */
#define E_LIST_NO_JUMP (-2)
/* Its verdict for a failed build whose rows may still be walked. */
#define E_LIST_COMPILER_FAILED (-1)

/* The one build step that may run at a time. */
typedef struct {
 int active;            /* a step owns a child (or is finishing)           */
 int pid;               /* the child, leader of its own process group      */
 int efd, wfd;          /* read ends of its stderr and stdout              */
 char *eacc, *wacc;     /* the partial line kept from each stream          */
 int eacclen, wacclen;
 unsigned long drained; /* bytes read in this step (the runaway cap)       */
 int runaway;
 int cancelled;         /* Esc was pressed                                 */
 int term_ticks;        /* heartbeat ticks since SIGTERM                   */
 int reaped;            /* the child has been waited for                   */
 int status;            /* its wait status                                 */
 int held;              /* it ended while a box was up                     */
 int is;                /* rows Messages had when the step began           */
 int wait_row;          /* the waiting line is Messages' last row          */
 time_t start;
 int hb_fd;             /* the heartbeat timer                             */
 sigset_t old_mask;     /* the signal mask before SIGCHLD was blocked      */
 ECNT *cn;
 PIC *pic;
 char dir[WPE_BUILD_PATH_MAX];   /* where the child runs; its paths resolve there */
 e_build_cont cont;
 void *data;
} e_build_step;

static e_build_step g_build;

/* The build as a whole: open from F9 (or Alt-C, Ctrl-F9, Execute Make, the
   debugger's build) until its last step has ended. */
static int g_build_chain_open;
static int g_build_chain_ran;          /* at least one step ran a child      */
static int g_build_chain_cancelled;
static int g_build_chain_list;         /* the last step's list verdict       */
static int g_build_chain_exit;         /* the last step's exit status        */
static e_build_cont g_build_final;
static void *g_build_final_data;

static void e_build_step_end(void);

/**
 * e_build_busy - Is a build running, or between two of its steps?
 *
 * F9, Alt-C, Ctrl-F9, Execute Make and the debugger's build ask this first,
 * so a second build never resets Messages under the first one; the user is
 * told on the status row that Esc cancels the running build instead.
 * Return: 1 while a build owns Messages.
 */
int e_build_busy(void)
{
 if (g_build.active)
  return(1);
 return(g_build_chain_open);
}

/**
 * e_build_refuse - Say why a second build did not start.
 * @f: the window the key was pressed in.
 *
 * The bottom row names the running build and the key that ends it; the
 * next repaint of the hint row puts the key hints back.
 */
void e_build_refuse(FENSTER *f)
{
 e_status_say(f, E_BUILD_BUSY_LINE);
}

/* What a status line that does not fit the screen starts with: it keeps
   its end, where the part of a long file name the user knows is. */
#define E_STATUS_CUT_MARK "..."
/* Columns the bottom row keeps free: the one it is drawn from, and the
   last one. */
#define E_STATUS_MARGIN 2

/**
 * e_status_say - Answer a key on the bottom row.
 * @f: the window the key was pressed in; @text: what to say.
 *
 * For a key that cannot do what it says right now (a second build, a walk
 * with no messages, a row naming a file that is not there): the user reads
 * why at once, where Borland's status line said it, instead of seeing
 * nothing happen.  The next repaint of the hint row puts the key hints
 * back.
 */
void e_status_say(FENSTER *f, const char *text)
{
 int room = MAXSCOL - E_STATUS_MARGIN, len = (int)strlen(text);
 char *shown;

 if (!f || !f->fb || room <= (int)strlen(E_STATUS_CUT_MARK))
  return;
 shown = MALLOC(room + 1);
 if (len <= room)
  strcpy(shown, text);
 else
 {
  strcpy(shown, E_STATUS_CUT_MARK);
  strcat(shown, text + len - (room - (int)strlen(E_STATUS_CUT_MARK)));
 }
 e_blk(MAXSCOL, 0, MAXSLNS - 1, f->fb->mt.fb);
 e_pr_str(1, MAXSLNS - 1, shown, f->fb->ms.fb, -1, -1,
          f->fb->ms.fb, f->fb->mt.fb);
 e_refresh();
 FREE(shown);
}

/**
 * e_build_messages - The Messages window, if it is open.
 * @cn: the desktop.
 *
 * Looked up afresh on every use: the user may close Messages while the
 * build runs, and its output is then kept until the window is back.
 * Return: the window, or NULL.
 */
static FENSTER *e_build_messages(ECNT *cn)
{
 int i;

 if (!cn)
  return(NULL);
 for (i = cn->mxedt; i > 0; i--)
  if (cn->f[i] && cn->f[i]->datnam && !strcmp(cn->f[i]->datnam, "Messages"))
   return(cn->f[i]);
 return(NULL);
}

/**
 * e_build_ui_idle - May the build paint, or end, right now?
 * @cn: the desktop.
 *
 * True when no dialog, menu or picker owns the screen and a text window is
 * on top: the same test the assistant applies before it shows a reply.  A
 * build that ends while a dialog is open waits for the dialog to close, so
 * the jump to the first error never lands under it.
 */
static int e_build_ui_idle(ECNT *cn)
{
 extern int wpe_modal_active;
 FENSTER *top;

 if (wpe_modal_active || !cn || cn->mxedt < 1)
  return(0);
 top = cn->f[cn->mxedt];
 if (!top)
  return(0);
 return(DTMD_ISTEXT(top->dtmd));
}

/**
 * e_build_drop_last_row - Take the last row out of the Messages buffer.
 * @b: the Messages buffer.
 */
static void e_build_drop_last_row(BUFFER *b)
{
 int last = b->mxlines - 1;

 if (last < 0)
  return;
 if (last == 0)
 {
  b->bf[0].s[0] = '\0';
  b->bf[0].len = b->bf[0].nrc = 0;
  return;
 }
 FREE(b->bf[last].s);
 b->mxlines--;
}

/**
 * e_build_wait_line_drop - Remove the waiting line before output is added.
 *
 * The line always sits below the build's own output, so it is taken out
 * before new lines land and written again after them.
 */
static void e_build_wait_line_drop(void)
{
 FENSTER *mf = e_build_messages(g_build.cn);

 if (!g_build.wait_row)
  return;
 g_build.wait_row = 0;
 if (mf)
  e_build_drop_last_row(mf->b);
 else if (e_p_m_buffer)
  e_build_drop_last_row(e_p_m_buffer);   /* Messages closed: its buffer stays */
}

/**
 * e_build_wait_line_put - Write "building ... Ns (Esc cancels)" at the foot
 * of Messages.
 *
 * What the user sees while a build runs: that it runs, for how long, and
 * that Esc ends it -- where the assistant shows its own waiting line.
 */
static void e_build_wait_line_put(void)
{
 FENSTER *mf = e_build_messages(g_build.cn);
 char line[80];

 if (!mf || g_build.wait_row)
  return;
 snprintf(line, sizeof line, E_BUILD_WAIT_FMT, (long)(time(NULL) - g_build.start));
 print_to_end_of_buffer(mf->b, line, 0);
 g_build.wait_row = 1;
}

/**
 * e_build_paint - Show the latest Messages rows, when nothing is in the way.
 *
 * Messages follows the output to its last row; the cursor goes back to the
 * window the user is typing in, so typing during a build is undisturbed.
 */
static void e_build_paint(void)
{
 ECNT *cn = g_build.cn;
 FENSTER *mf = e_build_messages(cn);

 if (!mf || !e_build_ui_idle(cn))
  return;
 mf->b->b.y = mf->b->mxlines - 1;
 mf->b->b.x = 0;
 e_messages_scroll_to_bottom(mf);
 e_schirm(mf, 0);
 if (mf != cn->f[cn->mxedt])
  e_cursor(cn->f[cn->mxedt], 0);
 else
  e_cursor(mf, 0);
 e_refresh();
}

/**
 * e_build_signal - Send @sig to the build and every process it started.
 * @sig: SIGTERM on Esc, SIGKILL when that was not enough or on a runaway.
 *
 * make runs the compiler as its own child; ending make alone would leave
 * the compiler writing into the pipe, so the whole process group is hit.
 */
static void e_build_signal(int sig)
{
 if (g_build.pid <= 0)
  return;
 if (kill(-g_build.pid, sig) < 0)
  kill(g_build.pid, sig);
}

/**
 * e_build_try_reap - Collect the child's exit status if it has exited.
 * Return: 1 once it is reaped.
 */
static int e_build_try_reap(void)
{
 int r;

 if (g_build.reaped)
  return(1);
 if (g_build.pid <= 0)
 {
  g_build.reaped = 1;
  return(1);
 }
 r = waitpid(g_build.pid, &g_build.status, WNOHANG);
 if (r == g_build.pid || (r < 0 && errno == ECHILD))
  g_build.reaped = 1;
 if (r < 0 && !WpeReapedStatus(g_build.pid, &g_build.status))
  g_build.status = 0;      /* reaped and not kept: judge by the parsed rows */
 return(g_build.reaped);
}

/**
 * e_build_streams_open - Is either output pipe of the child still open?
 */
static int e_build_streams_open(void)
{
 if (g_build.efd != E_BUILD_NO_FD)
  return(1);
 return(g_build.wfd != E_BUILD_NO_FD);
}

/**
 * e_build_close_stream - Stop watching one output pipe and close it.
 * @fd: the pipe's read end.
 */
static void e_build_close_stream(int fd)
{
 if (fd == E_BUILD_NO_FD)
  return;
 wpe_fd_del(fd);
 close(fd);
 if (fd == g_build.efd)
  g_build.efd = E_BUILD_NO_FD;
 if (fd == g_build.wfd)
  g_build.wfd = E_BUILD_NO_FD;
}

/**
 * e_build_finish_or_hold - The child is gone: end the step now, or as soon
 * as the dialog on screen closes.
 */
static void e_build_finish_or_hold(void)
{
 if (e_build_ui_idle(g_build.cn))
 {
  e_build_step_end();
  return;
 }
 g_build.held = 1;
}

/**
 * e_build_after_streams_closed - Both pipes are at EOF: reap the child.
 *
 * A child closes its pipes just before it exits, so it is normally reaped
 * at once; otherwise the heartbeat looks again on its next tick.  Without a
 * heartbeat timer the wait is the short one between the close and the exit.
 */
static void e_build_after_streams_closed(void)
{
 if (!e_build_try_reap() && g_build.hb_fd == E_BUILD_NO_FD)
 {
  while (waitpid(g_build.pid, &g_build.status, 0) < 0 && errno == EINTR)
   ;
  g_build.reaped = 1;
 }
 if (g_build.reaped)
  e_build_finish_or_hold();
}

/**
 * e_build_stop_runaway - Kill a build that floods Messages.
 */
static void e_build_stop_runaway(void)
{
 g_build.runaway = 1;
 e_build_signal(SIGKILL);
 e_build_close_stream(g_build.efd);
 e_build_close_stream(g_build.wfd);
}

/**
 * e_build_pump_stream - Read what one pipe has into Messages.
 * @fd: the pipe that woke.
 * Return: the bytes read, 0 at EOF, E_PUMP_AGAIN when there was nothing.
 */
static int e_build_pump_stream(int fd)
{
 FENSTER *mf = e_build_messages(g_build.cn);
 BUFFER *b = NULL;

 if (mf)
  b = mf->b;
 if (fd == g_build.efd)
  return(e_p_pump_fd(fd, &g_build.eacc, &g_build.eacclen, b));
 return(e_p_pump_fd(fd, &g_build.wacc, &g_build.wacclen, b));
}

/**
 * e_build_on_output - The fd loop's callback for the child's stdout and
 * stderr: every line lands in Messages as it arrives.
 * @fd: the pipe that woke; @data: unused.
 */
static void e_build_on_output(int fd, void *data)
{
 int n;

 (void)data;
 if (!g_build.active)
  return;
 e_build_wait_line_drop();
 n = e_build_pump_stream(fd);
 if (n == 0)
  e_build_close_stream(fd);
 if (n > 0)
  g_build.drained += (unsigned long)n;
 if (g_build.drained >= E_BUILD_RUNAWAY_MAX)
  e_build_stop_runaway();
 if (e_build_streams_open())
 {
  e_build_wait_line_put();
  e_build_paint();
  return;
 }
 e_build_after_streams_closed();
}

/**
 * e_build_escalate - A cancelled build that is still alive after the grace
 * ticks is killed outright.
 */
static void e_build_escalate(void)
{
 if (!g_build.cancelled || g_build.reaped)
  return;
 g_build.term_ticks++;
 if (g_build.term_ticks == E_BUILD_KILL_TICKS)
  e_build_signal(SIGKILL);
}

/**
 * e_build_on_tick - The heartbeat: turn the waiting line, finish a child
 * that has closed its pipes, and escalate a cancel.
 * @fd: the timer; @data: unused.
 */
static void e_build_on_tick(int fd, void *data)
{
 uint64_t expirations;

 (void)data;
 if (read(fd, &expirations, sizeof expirations) != (ssize_t)sizeof expirations)
  return;
 if (!g_build.active || g_build.held)
  return;
 e_build_escalate();
 if (!e_build_streams_open())
 {
  if (e_build_try_reap())
   e_build_finish_or_hold();
  return;
 }
 e_build_wait_line_drop();
 e_build_wait_line_put();
 e_build_paint();
}

/**
 * e_build_heartbeat_start - Start the timer that turns the waiting line.
 *
 * Present only while a build runs, so the idle editor still sleeps until a
 * key.  Without timerfd the line still moves whenever output arrives.
 */
static void e_build_heartbeat_start(void)
{
 g_build.hb_fd = E_BUILD_NO_FD;
#ifdef __linux__
 g_build.hb_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
 if (g_build.hb_fd >= 0)
 {
  struct itimerspec its;

  its.it_value.tv_sec = 0;
  its.it_value.tv_nsec = E_BUILD_TICK_NS;
  its.it_interval = its.it_value;
  timerfd_settime(g_build.hb_fd, 0, &its, NULL);
  wpe_fd_add(g_build.hb_fd, POLLIN, e_build_on_tick, NULL);
 }
 else
  g_build.hb_fd = E_BUILD_NO_FD;
#endif
}

/**
 * e_build_heartbeat_stop - Stop the waiting line's timer.
 */
static void e_build_heartbeat_stop(void)
{
 if (g_build.hb_fd == E_BUILD_NO_FD)
  return;
 wpe_fd_del(g_build.hb_fd);
 close(g_build.hb_fd);
 g_build.hb_fd = E_BUILD_NO_FD;
}

/**
 * e_build_cancel - Esc while a build runs: end it.
 *
 * The child and everything it started get SIGTERM; if they are still there
 * about two seconds later, SIGKILL.  The step then ends with "build
 * cancelled" in Messages and no later step runs, so no program is linked or
 * started from a build the user stopped.
 */
void e_build_cancel(void)
{
 if (g_build.held)
 {
  e_build_deliver_held();
  return;
 }
 if (!g_build.active || g_build.cancelled)
  return;
 g_build.cancelled = 1;
 g_build.term_ticks = 0;
 e_build_signal(SIGTERM);
 if (g_build.hb_fd == E_BUILD_NO_FD)
  e_build_signal(SIGKILL);       /* no timer to wait on: end it now */
}

/**
 * e_build_deliver_held - End a build step that finished behind a dialog.
 *
 * The editor's key loop calls this before it waits for a key, the point
 * that is certainly outside every dialog: the jump to the first error and
 * the next step of the build happen there, never under an open box.
 */
void e_build_deliver_held(void)
{
 if (!g_build.held)
  return;
 if (!e_build_ui_idle(g_build.cn))
  return;
 g_build.held = 0;
 e_build_step_end();
}

/**
 * e_build_watch_child - Hand the running child's pipes to the fd loop.
 *
 * From here the function returns to the editor at once; the build goes on
 * in e_build_on_output and e_build_on_tick.
 */
static void e_build_watch_child(void)
{
 int flags;

 g_build.efd = efildes[0];
 g_build.wfd = wfildes[0];
 efildes[0] = wfildes[0] = E_BUILD_NO_FD;
 if (g_build.efd != E_BUILD_NO_FD)
 {
  flags = fcntl(g_build.efd, F_GETFL, 0);
  fcntl(g_build.efd, F_SETFL, flags | O_NONBLOCK);
  wpe_fd_add(g_build.efd, POLLIN, e_build_on_output, NULL);
 }
 if (g_build.wfd != E_BUILD_NO_FD)
 {
  flags = fcntl(g_build.wfd, F_GETFL, 0);
  fcntl(g_build.wfd, F_SETFL, flags | O_NONBLOCK);
  wpe_fd_add(g_build.wfd, POLLIN, e_build_on_output, NULL);
 }
 e_build_heartbeat_start();
 e_build_wait_line_put();
 e_build_paint();
}

/**
 * e_build_flush_fragments - The last bytes of each stream, with no final
 * newline, still go to Messages.
 * @b: the Messages buffer.
 */
static void e_build_flush_fragments(BUFFER *b)
{
 if (g_build.eacclen > 0)
  print_to_end_of_buffer(b, g_build.eacc, 0);
 if (g_build.wacclen > 0)
  print_to_end_of_buffer(b, g_build.wacc, 0);
 if (g_build.eacc)
  FREE(g_build.eacc);
 if (g_build.wacc)
  FREE(g_build.wacc);
 g_build.eacc = g_build.wacc = NULL;
 g_build.eacclen = g_build.wacclen = 0;
}

/**
 * e_build_exit_code - The child's exit code, 0 when it cannot be known.
 *
 * The editor's SIGCHLD handler may reap the child first; it keeps the status
 * for us (WpeReapedStatus).  Only a status lost to a full ring leaves the
 * parsed rows to decide success or failure, as they always did.
 */
static int e_build_exit_code(void)
{
 if (g_build.pid > 0 && WIFEXITED(g_build.status))
  return(WEXITSTATUS(g_build.status));
 return(0);
}

/**
 * e_build_raise_messages - Bring Messages to the top for the step's end.
 * @cn: the desktop.
 *
 * The error list is read from the top window and the jump to an error
 * starts from it; the user may have moved to another window, or closed
 * Messages, while the build ran.
 * Return: the Messages window.
 */
static FENSTER *e_build_raise_messages(ECNT *cn)
{
 FENSTER *mf = e_build_messages(cn);
 int i;

 if (!mf)
  return(e_find_or_create_messages(cn));
 for (i = cn->mxedt; i > 0; i--)
  if (cn->f[i] == mf)
   break;
 if (i > 0 && i != cn->mxedt)
  e_switch_window(cn->edt[i], cn->f[cn->mxedt]);
 return(cn->f[cn->mxedt]);
}

/**
 * e_build_step_end_window - The Messages window a finished step reports in.
 * @cn: the desktop.
 *
 * A cancelled build leaves the user in the window they moved to; any other
 * step raises Messages, because its rows are read from the top window.
 * Return: the Messages window.
 */
static FENSTER *e_build_step_end_window(ECNT *cn)
{
 FENSTER *mf = e_build_messages(cn);

 if (g_build.cancelled && mf)
  return(mf);
 return(e_build_raise_messages(cn));
}

/**
 * e_build_enter_dir - Work in the directory the child ran in.
 * @saved: where the editor's own directory is kept; @n: its size.
 *
 * The rows a compiler prints name files relative to where it ran, so the
 * error list and the jump read them from there, as they did when the
 * build held the editor in that directory.
 * Return: 1 when the directory was changed (pair with e_build_popd).
 */
static int e_build_enter_dir(char *saved, size_t n)
{
 if (!g_build.dir[0] || !getcwd(saved, n))
  return(0);
 return(chdir(g_build.dir) == 0);
}

/**
 * e_build_step_verdict - Read the finished step's rows and say how it went.
 * @mf: the Messages window, on top.
 * @ret: the child's exit code.
 * Return: 0 for a success, -1 for a failure, WPE_ESC when cancelled.
 */
static int e_build_step_verdict(FENSTER *mf, int ret)
{
 BUFFER *b = mf->b;
 int list = 0;

 if (g_build.cancelled)
 {
  print_to_end_of_buffer(b, E_BUILD_CANCELLED_LINE, 0);
  g_build_chain_cancelled = 1;
  g_build_chain_list = E_LIST_NO_JUMP;
  return(WPE_ESC);
 }
 if (g_build.runaway)
  print_to_end_of_buffer(b, E_BUILD_RUNAWAY_LINE, 0);
 if (b->mxlines - g_build.is > 2)
  list = e_make_error_list(mf);
 g_build_chain_list = list;
 g_build_chain_exit = ret;
 if (ret || list)
  return(-1);
 print_to_end_of_buffer(b, E_BUILD_SUCCESS_LINE, b->mx.x);
 return(0);
}

/**
 * e_build_continue - Run what follows a step: the next step, or the end.
 * @cont: the continuation (may be NULL); @f: the top window;
 * @result: the step's result; @data: the continuation's own data.
 */
static void e_build_continue(e_build_cont cont, FENSTER *f, int result, void *data)
{
 if (cont)
  cont(f, result, data);
}

/**
 * e_build_step_end - The child is gone: what the old blocking reader ran
 * after its loop, then the continuation.
 */
static void e_build_step_end(void)
{
 ECNT *cn = g_build.cn;
 e_build_cont cont = g_build.cont;
 void *data = g_build.data;
 char cwd_saved[WPE_BUILD_PATH_MAX];
 FENSTER *mf;
 int result, cwd_chg;

 e_build_heartbeat_stop();
 e_build_close_stream(g_build.efd);
 e_build_close_stream(g_build.wfd);
 e_build_wait_line_drop();
 mf = e_build_step_end_window(cn);
 e_build_flush_fragments(mf->b);
 if (g_build.pic)
  e_close_view(g_build.pic, 1);
 cwd_chg = e_build_enter_dir(cwd_saved, sizeof cwd_saved);
 if (g_build.pid > 0)
  g_build_chain_ran = 1;
 result = e_build_step_verdict(mf, e_build_exit_code());
 mf->b->b.y = mf->b->mxlines - 1;
 g_build.active = 0;
 e_build_continue(cont, cn->f[cn->mxedt], result, data);
 if (cwd_chg)
  e_build_popd(cwd_saved);
}

/**
 * e_build_step_begin - Set up the state of a new step.
 * @cn: the desktop; @mf: Messages; @pic: the old status box, if any;
 * @cont, @data: what runs when the step ends.
 */
static void e_build_step_begin(ECNT *cn, FENSTER *mf, PIC *pic,
                               e_build_cont cont, void *data)
{
 memset(&g_build, 0, sizeof g_build);
 g_build.active = 1;
 g_build.pid = e_save_pid;
 g_build.efd = g_build.wfd = g_build.hb_fd = E_BUILD_NO_FD;
 g_build.cn = cn;
 g_build.pic = pic;
 g_build.cont = cont;
 g_build.data = data;
 g_build.start = time(NULL);
 g_build.is = mf->b->mxlines - 1;
 if (!getcwd(g_build.dir, sizeof g_build.dir))
  g_build.dir[0] = '\0';
}

/**
 * e_p_exec - Run the child e_exec_inf started as one step of a build.
 * @f: the window the build belongs to; @pic: an old status box, or NULL;
 * @cont: what runs when the step has ended (NULL for nothing);
 * @data: handed to @cont.
 *
 * Returns to the editor at once: the step goes on in the background and
 * @cont is called exactly once, with 0 for a success, -1 for a failure and
 * WPE_ESC for a cancelled build.  With no child to run (a project whose
 * objects are all current) the step ends at once, as the old reader did.
 * Return: 0.
 */
int e_p_exec(FENSTER *f, PIC *pic, e_build_cont cont, void *data)
{
 ECNT *cn = f->ed;
 FENSTER *mf = e_find_or_create_messages(cn);

 e_build_step_begin(cn, mf, pic, cont, data);
 if (efildes[0] < 0 && wfildes[0] < 0)
 {
  g_build.pid = 0;
  g_build.reaped = 1;
  e_build_step_end();
  return(0);
 }
 if (efildes[1] >= 0) close(efildes[1]);
 if (wfildes[1] >= 0) close(wfildes[1]);
 efildes[1] = wfildes[1] = E_BUILD_NO_FD;
 e_build_watch_child();
 return(0);
}

/**
 * e_build_chain_begin - Open a build: F9, Alt-C, Ctrl-F9, Execute Make or
 * the debugger's build.
 * @f: the window the key was pressed in.
 * @done: what runs once the whole build has ended, or NULL; @data: its data.
 *
 * Only one build runs at a time; a second one is refused with a line that
 * says Esc cancels the first.
 * Return: 0 when the build may start, -1 when another one runs.
 */
static int e_build_chain_begin(FENSTER *f, e_build_cont done, void *data)
{
 if (e_build_busy())
 {
  e_build_refuse(f);
  return(-1);
 }
 g_build_chain_open = 1;
 g_build_chain_ran = 0;
 g_build_chain_cancelled = 0;
 g_build_chain_list = 0;
 g_build_chain_exit = 0;
 g_build_up_to_date[0] = '\0';
 g_build_final = done;
 g_build_final_data = data;
 return(0);
}

/* What a warning row of Messages starts with: U+26A0 WARNING SIGN and a
   blank, from the same symbol block as the window's other glyphs. */
#define E_BUILD_WARNING_MARK "\342\232\240 "

/**
 * e_build_mark_row - Put the warning mark in front of one Messages row.
 * @b: the Messages buffer; @y: the row.
 */
static void e_build_mark_row(BUFFER *b, int y)
{
 size_t m = strlen(E_BUILD_WARNING_MARK), total;
 char *s;

 if (y < 0 || y >= b->mxlines)
  return;
 s = (char *)b->bf[y].s;
 if (!strncmp(s, E_BUILD_WARNING_MARK, m))
  return;                                 /* marked by an earlier step */
 total = strlen(s) + 1;
 s = REALLOC(s, total + m);
 memmove(s + m, s, total);
 memcpy(s, E_BUILD_WARNING_MARK, m);
 b->bf[y].s = (void *)s;
 b->bf[y].len += m;
 b->bf[y].nrc += m;
}

/**
 * e_build_mark_warnings - Mark every warning row of the error list.
 * @b: the Messages buffer.
 *
 * The user tells at a glance which rows only advise and which stopped the
 * build; Alt-T and Alt-V still walk both.
 */
static void e_build_mark_warnings(BUFFER *b)
{
 int i;

 for (i = 0; i < err_num; i++)
  if (err_li[i].kind == E_ROW_WARNING)
   e_build_mark_row(b, err_li[i].row);
}

/**
 * e_build_plural - The "s" of "2 errors", none for "1 error".
 * @n: the count.
 */
static const char *e_build_plural(int n)
{
 if (n == 1)
  return("");
 return("s");
}

/* The count line of the last build, said again on the status row: a short
   Messages window scrolls to the error row and may not show its last row. */
static char g_build_count_line[160];

/**
 * e_build_print_count - End Messages with "N errors, M warnings".
 * @b: the Messages buffer; @result: the build's result.
 *
 * The count Borland showed when a build ended.  A failed build that left
 * no row to walk (a linker error, a command that could not run) says so
 * with its exit status, so "0 errors" never reads as a success.
 */
static void e_build_print_count(BUFFER *b, int result)
{
 char line[160];
 int i, errors = 0, warnings = 0;

 for (i = 0; i < err_num; i++)
 {
  if (err_li[i].kind == E_ROW_WARNING)
   warnings++;
  if (err_li[i].kind == E_ROW_ERROR)
   errors++;
 }
 snprintf(line, sizeof line, "%d error%s, %d warning%s",
          errors, e_build_plural(errors), warnings, e_build_plural(warnings));
 if (result && errors == 0)
  snprintf(line + strlen(line), sizeof line - strlen(line),
           "; the build failed (exit status %d)", g_build_chain_exit);
 print_to_end_of_buffer(b, line, 0);
 snprintf(g_build_count_line, sizeof g_build_count_line, "%s", line);
}

/**
 * e_build_tally - Read the whole build's rows again and end Messages with
 * their count, every warning row marked.
 * @mf: the Messages window, on top; @result: the build's result.
 */
static void e_build_tally(FENSTER *mf, int result)
{
 e_make_error_list(mf);
 e_build_mark_warnings(mf->b);
 e_build_print_count(mf->b, result);
 mf->b->b.y = mf->b->mxlines - 1;
 e_messages_scroll_to_bottom(mf);
}

/**
 * e_build_return_to_source - After a build that succeeded, give the keys
 * back to the window the user was in instead of leaving them in Messages.
 * @cn: the desktop, Messages on top.
 */
static void e_build_return_to_source(ECNT *cn)
{
 FENSTER *mf = cn->f[cn->mxedt];
 int i;

 for (i = cn->mxedt; i > 0; i--)
  if (strcmp(cn->f[i]->datnam, "Messages"))
   break;
 if (i > 0 && i != cn->mxedt)
 {
  e_switch_window(cn->edt[i], mf);
  return;
 }
 e_cursor(mf, 1);
 e_schirm(mf, 1);
}

/**
 * e_build_settle_top - Paint the cursor and the line:column of the window
 * now on top.
 * @cn: the desktop.
 *
 * A build ends from the fd loop, not from a key, so the key loop's own
 * repaint of the status row does not follow it: without this the window
 * the jump landed in still showed the old line and column.
 */
static void e_build_settle_top(ECNT *cn)
{
 FENSTER *top = cn->f[cn->mxedt];

 e_cursor(top, 1);
 e_zlsplt(top);
 e_refresh();
}

/* What Messages says when a build found everything current. */
#define E_BUILD_UP_TO_DATE_FMT "nothing to build: %s is up to date"

/**
 * e_build_report_up_to_date - A build that ran no command says so.
 * @cn: the desktop; @result: the build's result.
 *
 * Messages is started afresh with one line naming what is current, the
 * same words for F9 and Alt-C, and the keys go back to the source window:
 * the old run's lines no longer stand there as if the key had redone them.
 */
static void e_build_report_up_to_date(ECNT *cn, int result)
{
 FENSTER *mf;
 size_t n;
 char *line;

 if (result || !g_build_up_to_date[0])
  return;
 if (e_new_message(e_p_source_window(cn)))
  return;
 e_set_error_list(0, NULL, NULL, NULL, NULL);
 mf = e_build_raise_messages(cn);
 n = strlen(E_BUILD_UP_TO_DATE_FMT) + strlen(g_build_up_to_date) + 1;
 line = MALLOC(n);
 snprintf(line, n, E_BUILD_UP_TO_DATE_FMT, g_build_up_to_date);
 print_to_end_of_buffer(mf->b, line, 0);
 FREE(line);
 e_messages_scroll_to_bottom(mf);
 e_build_return_to_source(cn);
 e_build_settle_top(cn);
}

/**
 * e_build_chain_report - What the user reads when a build has ended.
 * @cn: the desktop; @result: the build's result.
 *
 * A failed build puts the cursor on its first error, a successful one
 * gives the keys back to the source; a cancelled one stays where it was,
 * with "build cancelled" as the last row of Messages.
 */
static void e_build_chain_report(ECNT *cn, int result)
{
 FENSTER *mf;
 int jumped = 0;                 /* e_show_error's answer: 0 when it landed */

 if (!g_build_chain_ran)
 {
  e_build_report_up_to_date(cn, result);
  return;
 }
 if (g_build_chain_cancelled)
 {
  e_build_paint();
  return;
 }
 mf = e_build_raise_messages(cn);
 e_build_tally(mf, result);
 if (result && g_build_chain_list != E_LIST_NO_JUMP)
  jumped = e_show_error(err_no = 0, mf);
 else if (!result)
  e_build_return_to_source(cn);
 else
 {
  e_cursor(mf, 1);
  e_schirm(mf, 1);
 }
 e_build_settle_top(cn);
 /* Where Borland showed "Errors: N" when a build ended: the user reads the
    count whatever the height of Messages.  A jump that could not happen
    has already said why on that row ("cannot find <name>"), and that word
    matters more than the count, so it stays. */
 if (jumped == 0)
  e_status_say(cn->f[cn->mxedt], g_build_count_line);
}

/**
 * e_build_chain_end - The last step of a build has ended: report it, then
 * run what was waiting for the build.
 * @f: the top window; @result: the build's result; @data: unused.
 */
static void e_build_chain_end(FENSTER *f, int result, void *data)
{
 e_build_cont done = g_build_final;
 void *done_data = g_build_final_data;
 ECNT *cn = f->ed;

 (void)data;
 g_build_chain_open = 0;
 g_build_final = NULL;
 g_build_final_data = NULL;
 e_build_chain_report(cn, result);
 e_build_continue(done, cn->f[cn->mxedt], result, done_data);
}

/**
 * e_err_column_index - The 0-based screen column of row @n's column.
 * @n: a row of the error list.
 *
 * Compilers count columns from 1 ("t.c:3:5" is the fifth character); the
 * cursor counts from 0.  Without this the jump landed one character to the
 * right of the one the compiler names, which at the end of a line was not
 * even visible.  A row with no column lands on the start of the line.
 * Return: the column to put the cursor on.
 */
static int e_err_column_index(int n)
{
 if (err_li[n].x <= 0)
  return(0);
 return(err_li[n].x - 1);
}

/* Rows of context kept above the current error row when Messages scrolls
   to it. */
#define E_ERR_ROW_CONTEXT 1

/**
 * e_err_show_row_in_messages - Put the Messages cursor on row @n's line
 * and scroll Messages so it can be read.
 * @cn: the desktop; @n: a row of the error list.
 *
 * The jump moves the source cursor; the message that sent it there stays
 * on screen beside it, as Borland's Messages window highlighted the
 * message of the current error.  Messages scrolls only when the row is
 * out of sight, so the rows around it (and the count at the end of a
 * build) stay where the user saw them.
 */
static void e_err_show_row_in_messages(ECNT *cn, int n)
{
 FENSTER *mf = e_build_messages(cn);
 int row = err_li[n].row, visible_h;

 if (!mf || row < 0 || row >= mf->b->mxlines)
  return;
 visible_h = mf->e.y - mf->a.y - 1;
 mf->b->b.y = row;
 mf->b->b.x = 0;
 if (row < mf->s->c.y || row >= mf->s->c.y + visible_h)
  mf->s->c.y = row - E_ERR_ROW_CONTEXT;
 if (mf->s->c.y < 0)
  mf->s->c.y = 0;
 e_schirm(mf, 0);
}

static int e_p_file_exists(const char *name);

/* What the status row says for a row naming a file that is not there. */
#define E_ERR_MISSING_FMT "cannot find %s"

/**
 * e_err_say_missing - A row names a file the editor cannot find.
 * @f: the window on top; @name: the name the row gives.
 *
 * The status row says so and nothing is opened: a jump used to open a new,
 * empty window under that name, which a later save would have written to
 * disk as a file nobody asked for.
 * Return: 1, the answer of a jump that did not happen.
 */
static int e_err_say_missing(FENSTER *f, const char *name)
{
 size_t n = strlen(E_ERR_MISSING_FMT) + strlen(name) + 1;
 char *text = MALLOC(n);

 snprintf(text, n, E_ERR_MISSING_FMT, name);
 e_status_say(f, text);
 FREE(text);
 return(1);
}

static FENSTER *e_p_source_window(ECNT *cn);

/**
 * e_err_locate_file - Where the file an error row names is to be found.
 * @cn: the desktop; @name: the name as the compiler printed it.
 *
 * A compiler names a file relative to the directory it ran in, and the
 * first jump happens there; a later Alt-T or Enter runs from the key loop
 * in the directory the editor was started in, which may be another one
 * ("wpe sub/t.c" from the parent, a make that failed in two files).  The
 * name is tried as it stands, then from the directory the build runs in,
 * so the second file of such a build still opens where the compiler said.
 * Return: a new path the caller frees, or NULL when neither place has it.
 */
static char *e_err_locate_file(ECNT *cn, const char *name)
{
 char *dir, *path;

 if (e_p_file_exists(name))
  return(WpeStrdup(name));
 if (name[0] == DIRC)
  return(NULL);
 dir = e_build_dir(e_p_source_window(cn));
 if (!dir)
  return(NULL);
 path = e_mkfilename(dir, (char *)name);
 FREE(dir);
 if (path && e_p_file_exists(path))
  return(path);
 if (path)
  FREE(path);
 return(NULL);
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
 err_walked = 1;
 e_err_show_row_in_messages(cn, n);
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
    char *where = e_err_locate_file(cn, err_li[n].file);

    if (!where)
     return(e_err_say_missing(f, err_li[n].file));
    if (e_edit(cn, where))
    {
     FREE(where);
     return(WPE_ESC);
    }
    FREE(where);
   }
  }
 }
 else if (filename != f->datnam)
  FREE(filename);
 b = cn->f[cn->mxedt]->b;
 b->b.y = err_li[n].line > b->mxlines ? b->mxlines - 1 : err_li[n].line - 1;
 if (!err_li[n].srch)
 {
  int col = e_err_column_index(n);

  for(i = j = 0; i + j < col && i < b->bf[b->b.y].len; i++)
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

/* How gcc and clang print the chain of includes that leads to an error in a
   header: "In file included from a.c:2:", then "                 from b.c:7,"
   for each further level.  Such a row names a place but is not an error. */
#define E_P_INCLUDED_FROM "In file included from "
#define E_P_INCLUDED_MORE "from "
/* How they print a note that belongs to the error above it. */
#define E_P_NOTE_TAG ": note:"

/**
 * e_p_is_context_row - Is this row part of an include chain?
 * @row: a Messages row.
 *
 * The walk with Alt-T and Alt-V stops on errors and warnings only; the
 * chain that leads to an error in a header belongs to that error, so Enter
 * on any of its rows goes to the error after it.
 */
static int e_p_is_context_row(const char *row)
{
 const char *p = row;

 if (!strncmp(row, E_P_INCLUDED_FROM, strlen(E_P_INCLUDED_FROM)))
  return(1);
 if (*p != ' ')
  return(0);
 while (*p == ' ')
  p++;
 return(!strncmp(p, E_P_INCLUDED_MORE, strlen(E_P_INCLUDED_MORE)));
}

/**
 * e_p_is_note_row - Is this row a note on the error above it?
 * @row: a Messages row.
 *
 * gcc repeats the error's own place in its notes ("each undeclared
 * identifier is reported only once"); as a stop of its own it would make
 * Alt-T stay on the same line, and it is not an error to count.
 */
static int e_p_is_note_row(const char *row)
{
 return(strstr(row, E_P_NOTE_TAG) != NULL);
}

/**
 * e_p_attach_context - Give the include chain before a new row to that row.
 * @k_before: the list's size before the row was read; @k: after.
 * @ctx_y: the first Messages row of a pending include chain, or -1; reset
 * once it is attached.
 */
static void e_p_attach_context(int k_before, int k, int *ctx_y)
{
 if (k <= k_before || *ctx_y < 0)
  return;
 err_li[k_before].y = *ctx_y;
 *ctx_y = -1;
}

static size_t e_p_longest_row(BUFFER *b);

/**
 * e_p_row_starts_with - Does @row start, after a directory, with @word
 * and a colon?
 * @row: a Messages row; @word: a program name such as "ld" or "a68g".
 */
static int e_p_row_starts_with(const char *row, const char *word)
{
 size_t n = strlen(word);
 int at = e_pure_bin((char *)row, ':');

 if (!n)
  return(0);
 return(!strncmp(row + at, word, n) && row[at + n] == ':');
}

/**
 * e_p_unmatched_row_verdict - What a row the compiler's pattern did not
 * take says about the build.
 * @row: the Messages row.
 *
 * A row that starts with the compiler's own name ("a68g: error: ...",
 * "gcc: error: ...") is the compiler talking: the build failed and the
 * rows the pattern did make are still walked, starting with the first.
 * A row from the assembler or the linker names no source line, so the
 * build failed with nothing to jump to.
 * Return: 0 for an ordinary row, E_LIST_COMPILER_FAILED or E_LIST_NO_JUMP.
 */
static int e_p_unmatched_row_verdict(const char *row)
{
 int ip = e_pure_bin(e_s_prog.compiler, ' ');
 char *name = WpeStrdup(e_s_prog.compiler + ip);
 int verdict = 0;

 name[strcspn(name, " \t")] = '\0';
 if (e_p_row_starts_with(row, name))
  verdict = E_LIST_COMPILER_FAILED;
 else if (e_p_row_starts_with(row, "ld") || e_p_row_starts_with(row, "as"))
  verdict = E_LIST_NO_JUMP;
 FREE(name);
 return(verdict);
}

int e_make_error_list(FENSTER *f)
{
 ECNT *cn = f->ed;
 BUFFER *b = cn->f[cn->mxedt]->b;
 int i, j, k = 0, ret = 0, ctx_y = -1;
 size_t row_max = e_p_longest_row(b);
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
 err_walked = 0;
 for (i = 0; i < b->mxlines; i++)
 {
  if (!strncmp((char *)b->bf[i].s, "Error at Command:", 17))
   return(!ret ? -2 : ret);
  if (e_p_is_context_row((char *)b->bf[i].s))
  {
   if (ctx_y < 0)
    ctx_y = i;
   continue;
  }
  if (e_p_is_note_row((char *)b->bf[i].s))
   continue;
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
   err_li[k].y = err_li[k].row = i;
   err_li[k].kind = E_ROW_MAKE;
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
  /* make's own line about a failed recipe, "make: *** [Makefile:2: all]
     Error 1": the makefile's line follows "Makefile:".  An offset written
     for an older format read 0 here, and the row then fell through to the
     compiler's pattern and was counted as an error of the user's code. */
  else if (!strncmp((char *)b->bf[i].s, "make", 4) &&
    ((spt = strstr((char *)b->bf[i].s, "makefile:")) ||
    (spt = strstr((char *)b->bf[i].s, "Makefile:")) ) &&
    (err_li[k].line = atoi(spt + 9)) > 0 )
  {
   err_li[k].file = MALLOC(9);
   for (j = 0; j < 8; j++)
    err_li[k].file[j] = spt[j];
   err_li[k].file[8] = '\0';
   err_li[k].y = err_li[k].row = i;
   err_li[k].kind = E_ROW_MAKE;
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
   int k_before = k;

   ret = e_p_cmp_mess(tststr, b, &i, &k, ret, row_max);
   e_p_attach_context(k_before, k, &ctx_y);
   if (!ret)
    ret = e_p_unmatched_row_verdict((char *)b->bf[i].s);
  }
 }
 if (!(f->ed->edopt & (ED_ERRORS_STOP_AT | ED_MESSAGES_STOP_AT)) &&
   ret == -1)
  ret = 0;
 return(ret);
}

/* e_set_error_list - replace the Next/Prev-Error list with explicit locations.
 *
 * Lets a tool other than the compiler feed the Alt-T / Alt-V navigation: the
 * AI changeset review writes one line per changed hunk into Messages and then
 * registers each (file, line) here, so the user steps change-to-change with
 * the same keys used to step compile-error-to-error.  ys[i] is the Messages
 * line each entry points back to (0 if unknown). */
int e_set_error_list(int n, char **files, int *lines, int *ys, char **texts)
{
 int i;
 if (err_li)
 {
  for (i = 0; i < err_num; i++)
  {
   if(err_li[i].file) FREE(err_li[i].file);
   if(err_li[i].text) FREE(err_li[i].text);
   if(err_li[i].srch) FREE(err_li[i].srch);
  }
  FREE(err_li);
  err_li = NULL;
 }
 err_num = 0;
 err_no = 0;
 err_walked = 0;
 if (n <= 0) return(0);
 err_li = MALLOC(sizeof(struct ERR_LI) * n);
 if (!err_li) return(-1);
 for (i = 0; i < n; i++)
 {
  err_li[i].file = WpeStrdup(files[i] ? files[i] : "");
  err_li[i].text = WpeStrdup(texts && texts[i] ? texts[i] : "");
  err_li[i].srch = NULL;
  err_li[i].x = 0;
  err_li[i].y = ys ? ys[i] : 0;
  err_li[i].row = err_li[i].y;
  err_li[i].kind = E_ROW_ERROR;
  err_li[i].line = lines ? lines[i] : 1;
 }
 err_num = n;
 return(0);
}

/**
 * e_err_row_in_window - Does row @n of the error list name the file in @f?
 * @n: a row of the list; @f: a window.
 *
 * The same rule e_show_error applies when it opens a row: a row written as
 * "name" or "./name" is compared to the window's file name, a row written
 * with a directory to the window's full name.  Lets Alt-T and Alt-V step
 * within the file the user is in before moving to the next file's rows.
 *
 * Return: 1 when the row belongs to the window's file, else 0.
 */
static int e_err_row_in_window(int n, FENSTER *f)
{
 int bg = 0, same;
 char *filename;

 if (err_li[n].file[0] == '.' && err_li[n].file[1] == DIRC)
  bg = 2;
 if (err_li[n].file[0] == DIRC)
 {
  filename = e_mkfilename(f->dirct, f->datnam);
  same = !strcmp(err_li[n].file + bg, filename);
  FREE(filename);
 }
 else
  same = !strcmp(err_li[n].file + bg, f->datnam);
 return(same);
}

/**
 * e_err_list_names_window - Does any row of the list name @f's file?
 * @f: a window.
 */
static int e_err_list_names_window(FENSTER *f)
{
 int i;

 for (i = 0; i < err_num; i++)
  if (e_err_row_in_window(i, f))
   return(1);
 return(0);
}

/**
 * e_err_walk_restarts - Does Alt-T or Alt-V start the walk again from an
 * end of the list?
 * @f: the window the user is in.
 *
 * On a list nothing has been shown from yet, and from a window the list
 * does not name, the walk starts at the first row (Alt-T) or the last
 * (Alt-V).
 */
static int e_err_walk_restarts(FENSTER *f)
{
 if (!err_walked)
  return(1);
 if (err_no < 0 || err_no >= err_num)
  return(1);
 return(!e_err_list_names_window(f));
}

/* What the status row says for Alt-T or Alt-V with no list to walk. */
#define E_ERR_NOTHING_TO_WALK "no messages to walk"

/**
 * e_err_say_nothing_to_walk - Alt-T or Alt-V before any build, or after
 * one that left no row.
 * @f: the window the user is in.
 *
 * The status row says there is nothing to walk; the keys used to repaint
 * the hint row and nothing else, which read as a key that did not work.
 * Return: 0.
 */
static int e_err_say_nothing_to_walk(FENSTER *f)
{
 e_status_say(f, E_ERR_NOTHING_TO_WALK);
 return(0);
}

/**
 * e_previous_error - Alt-V: the row before the last one shown, in the
 * order the compiler printed them.
 * @f: the window the user is in.
 *
 * The walk goes through the list as it stands in Messages, crossing from
 * file to file: compiler errors, the agent's changeset or a Multi-file edit
 * are visited whole, wrapping at the ends, whatever the line numbers.
 */
int e_previous_error(FENSTER *f)
{
 if (err_num <= 0)
  return(e_err_say_nothing_to_walk(f));
 if (e_err_walk_restarts(f))
  return(e_show_error(err_no = err_num - 1, f));
 return(e_show_error(err_no = (err_no + err_num - 1) % err_num, f));
}

/**
 * e_next_error - Alt-T: the row after the last one shown, in the order the
 * compiler printed them.
 * @f: the window the user is in.
 *
 * A row in another file that the compiler printed between two rows of
 * this one is visited in its turn, never skipped.
 */
int e_next_error(FENSTER *f)
{
 if (err_num <= 0)
  return(e_err_say_nothing_to_walk(f));
 if (e_err_walk_restarts(f))
  return(e_show_error(err_no = 0, f));
 return(e_show_error(err_no = (err_no + 1) % err_num, f));
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
  /* a68g 3 echoes the offending source line with its number ("3       print
     ((x, yy))"), marks the spot below it and then says "a68g: error: 1: ...
     in this line."; the file is the one being compiled. */
  cp->intstr       = WpeStrdup(A68G_INTSTR);
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

 e_prog.num = 13;
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
 /* Scala: scalac writes the class files itself, like javac, so there is no
    link step.  Scala 2 prints "t.scala:3: error: type mismatch;"; Scala 3
    heads its message "-- [E007] Type Mismatch Error: t.scala:3:17", whose
    file name the row lookup finds after the heading's last blank. */
 e_prog.comp[12]->compiler = WpeStrdup("scalac");
 e_prog.comp[12]->language = WpeStrdup("Scala");
 e_prog.comp[12]->filepostfix = (char **)WpeExpArrayCreate(1, sizeof(char *), 1);
 e_prog.comp[12]->filepostfix[0] = WpeStrdup(".scala");
 e_prog.comp[12]->key = 'S';
 e_prog.comp[12]->x = 0;
 e_prog.comp[12]->intstr = WpeStrdup("${?*:warning:}${FILE}:${LINE}:*");
 for (i = 0; i < e_prog.num; i++)
 {
  if (i == 5) e_prog.comp[i]->comp_str = WpeStrdup("-m py_compile");
  else if (i == 6) e_prog.comp[i]->comp_str = WpeStrdup("-interaction=nonstopmode -file-line-error");
  else if (i == 7) e_prog.comp[i]->comp_str = WpeStrdup("-c");
  else if (i == 10) e_prog.comp[i]->comp_str = WpeStrdup("build");
  else if (i == 12) e_prog.comp[i]->comp_str = WpeStrdup("-deprecation");
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
 /* The radios' caption has its own row, right above them and beside the
    Message-String caption: on the Start sYmbol row that field painted
    over it. */
 e_add_txtstr(4, 15, "Compiler-Style:", o);
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

/**
 * e_make_library - Insert an object into the project's library, as a step
 * of the build.
 * @library: the archive; @ofile: the object; @f: a window of the desktop.
 * @done: what runs when ar has ended, exactly once (0 for a success);
 * @data: handed to @done.
 * Return: 0 when ar started, -1 when it could not.
 */
int e_make_library(char *library, char *ofile, FENSTER *f,
                   e_build_cont done, void *data)
{
 char *ar_arg[5] = {  NULL, NULL, NULL, NULL, NULL  };
 int file;
 PIC *pic = NULL;

 ar_arg[0] = "ar";
 if (access(library, 0))
  ar_arg[1] = "-cr";
 else
  ar_arg[1] = "-r";
 ar_arg[2] = library;
 ar_arg[3] = ofile;
 /* The child prints its command line itself (e_exec_inf). */
 e_sys_ini();
 file = e_exec_inf(f, ar_arg, 4);
 e_sys_end();
 if (!file)
 {
  e_build_continue(done, f, -1, data);
  return(-1);
 }
 e_p_exec(f, pic, done, data);
 return(0);
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
 else
 {
  /* Repaint just this window: async writers (LSP fd-loop callbacks, the
     debugger's e_d_pty_flush_line) call us with sw=0 from outside the input
     loop, so without a paint here the buffer update is invisible until the
     next keystroke.  The previous WpeIsXwin() gate only redrew under X11 --
     ncurses (the test harness, the common shell case) silently swallowed the
     update.  Always paint; e_schirm + e_refresh are cheap and safe in both
     modes, mirroring the debugger's e_d_messages_redraw pattern. */
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
 else
 {
  e_schirm(wf, 0);
  e_cursor(wf, 0);
  e_refresh();
 }
 return(0);
}

#if MOUSE
/* Two presses on the same Messages row closer together than this are one
   double click, the usual desktop interval. */
#define E_MSG_DCLICK_MS 500L

/* The last press on a Messages row: which window, which row, and when. */
static struct {
 FENSTER *f;
 int row;
 long ms;
} g_msg_press;

/**
 * e_msg_now_ms - A monotonic clock in milliseconds, for the double click.
 */
static long e_msg_now_ms(void)
{
 struct timespec ts;

 clock_gettime(CLOCK_MONOTONIC, &ts);
 return((long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

/**
 * e_msg_row_at_mouse - The Messages row under the pointer.
 * @f: the Messages window.
 * Return: the row, clamped to the rows the buffer has.
 */
static int e_msg_row_at_mouse(FENSTER *f)
{
 extern struct mouse e_mouse;
 int row = e_mouse.y - f->a.y + f->s->c.y - 1;

 if (row >= f->b->mxlines)
  row = f->b->mxlines - 1;
 if (row < 0)
  row = 0;
 return(row);
}

/**
 * e_msg_press - Note a press on a Messages row; is it the second of a
 * double click?
 * @f: the Messages window; @row: the row pressed.
 *
 * Called for the press that brings Messages to the front as well, so a
 * double click from the source window jumps like one inside Messages.
 * Return: 1 for the second press of a double click on the same row.
 */
static int e_msg_press(FENSTER *f, int row)
{
 long now = e_msg_now_ms();
 int second = (g_msg_press.f == f && g_msg_press.row == row &&
               now - g_msg_press.ms <= E_MSG_DCLICK_MS);

 g_msg_press.f = f;
 g_msg_press.row = row;
 g_msg_press.ms = now;
 if (second)
  g_msg_press.f = NULL;     /* a third press starts a new click */
 return(second);
}

/**
 * e_d_note_messages_focus_click - The press that brings Messages to the
 * front counts as the first click of a double click on its row.
 * @f: the window that was clicked.
 */
void e_d_note_messages_focus_click(FENSTER *f)
{
 if (!f || !f->datnam || strcmp(f->datnam, "Messages"))
  return;
 e_msg_press(f, e_msg_row_at_mouse(f));
}

/**
 * e_msg_car_mouse - A click in Messages: one click puts its cursor on the
 * row, a double click jumps to the place the row names, like Enter.
 * @f: the Messages window, on top.
 * Return: WPE_CR for a double click, else 0.
 */
static int e_msg_car_mouse(FENSTER *f)
{
 extern struct mouse e_mouse;
 BUFFER *b = f->b;
 int row = e_msg_row_at_mouse(f);

 b->b.y = row;
 b->b.x = e_mouse.x - f->a.x + f->s->c.x - 1;
 if (b->b.x < 0)
  b->b.x = 0;
 if (e_msg_press(f, row))
  return(WPE_CR);
 return(0);
}

int e_d_car_mouse(FENSTER *f)
{
 extern struct mouse e_mouse;
 BUFFER *b = f->ed->f[f->ed->mxedt]->b;
 SCHIRM *s = f->ed->f[f->ed->mxedt]->s;

 if (!strcmp(f->datnam, "Messages"))
  return(e_msg_car_mouse(f));
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

/**
 * e_messages_return - Esc in Messages: back to the window the user came
 * from, its cursor where it was left.
 * @f: the window on top.
 *
 * Borland's Messages window let go the same way; before this Esc did
 * nothing there and the user had to find the source window with F6.
 * Return: 1 when Esc was taken, 0 when @f is not Messages or no other
 * window is open.
 */
int e_messages_return(FENSTER *f)
{
 ECNT *cn = f->ed;
 int i;

 if (!f->datnam || strcmp(f->datnam, "Messages") || f != cn->f[cn->mxedt])
  return(0);
 for (i = cn->mxedt - 1; i > 0; i--)
  if (DTMD_ISTEXT(cn->f[i]->dtmd) && strcmp(cn->f[i]->datnam, "Messages"))
   break;
 if (i <= 0)
  return(0);
 e_switch_window(cn->edt[i], f);
 return(1);
}

int e_exec_make(FENSTER *f)
{
 ECNT *cn = f->ed;
 char **arg = NULL;
 char saved_cwd[1024];
 int i, file, argc, cwd_changed;

 if (e_build_chain_begin(f, NULL, NULL))
  return(-1);
 WpeMouseChangeShape(WpeWorkingShape);
 efildes[0] = efildes[1] = -1;
 wfildes[0] = wfildes[1] = -1;
 /* Capture the active file's directory BEFORE e_new_message makes Messages the
    top window -- after that cn->f[cn->mxedt]->dirct is the Messages dir. */
 cwd_changed = e_build_pushd(f, saved_cwd, sizeof(saved_cwd));
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
 {
  if (cwd_changed)
   e_build_popd(saved_cwd);
  WpeMouseRestoreShape();
  e_build_chain_end(f, WPE_ESC, NULL);
  return(WPE_ESC);
 }
 f = cn->f[cn->mxedt];
 e_sys_ini();
 e_p_set_compiled_source(NULL);
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
  if (cwd_changed)
   e_build_popd(saved_cwd);
  WpeMouseRestoreShape();
  e_build_chain_end(f, WPE_ESC, NULL);
  return(WPE_ESC);
 }
 e_sys_end();
 e_free_arg(arg, argc - 1);
 e_p_exec(f, NULL, e_build_chain_end, NULL);
 if (cwd_changed)
  e_build_popd(saved_cwd);
 WpeMouseRestoreShape();
 return(0);
}

int e_run_sh(FENSTER *f)
{
 int ret, len = strlen(f->datnam), cwd_chg = 0;
 char estr[128];
 char cwd_saved[1024];

 if (strcmp(f->datnam+len-3, ".sh"))
  return(1);

 WpeMouseChangeShape(WpeWorkingShape);
 f->filemode |= 0100;
 if (f->save)
  e_save(f);
 /* Run the script in its own directory, not wherever wpe was launched from,
    and invoke it as "./name.sh" so the shell finds it there ("." is not
    normally on PATH, so a bare "name.sh" would fail even in the right dir). */
 cwd_chg = e_build_pushd(f, cwd_saved, sizeof(cwd_saved));
 /* Make the script executable on disk: the in-memory filemode |= 0100 above
    only reaches disk on save, so a freshly-opened unmodified .sh would be
    non-executable and "./name.sh" would fail with EACCES. */
 { struct stat _sb;
   if (!stat(f->datnam, &_sb))
    chmod(f->datnam, _sb.st_mode | S_IXUSR); }
 snprintf(estr, sizeof(estr), "./%s ", f->datnam);
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
 if (cwd_chg)
  e_build_popd(cwd_saved);
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

static int e_install_run(FENSTER *f);

/**
 * e_install_after_make - The build of Install has ended: run the project's
 * install commands if it succeeded.
 * @f: the top window; @result: the build's result; @data: unused.
 */
static void e_install_after_make(FENSTER *f, int result, void *data)
{
 (void)data;
 if (result)
  return;
 e_install_run(f);
}

/**
 * e_install - Alt-L: build in the background, then install the project.
 */
int e_install(FENSTER *f)
{
 return(e_p_make_then(f, e_install_after_make, NULL));
}

/**
 * e_install_run - Run the "install:" commands of the open project.
 * @f: a window of the desktop.
 */
static int e_install_run(FENSTER *f)
{
 char *tp, *sp, *string, *tmp, text[256];
 FILE *fp;
 int i, j;

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

/* The project build runs one compile (or ar, or ranlib) at a time in the
   background, so its loop over the project's files is kept here between
   steps, as phases of a small state machine. */
enum e_prj_phase {
 PRJ_FILE,          /* look at file k: current, or compile it            */
 PRJ_COMPILED,      /* file k compiled: into the library at once?         */
 PRJ_INSERTED,      /* take file k off the compile command                */
 PRJ_LINK_ARG,      /* file k's object onto the link line, or the library */
 PRJ_NEXT,          /* on to file k + 1                                   */
 PRJ_RANLIB,        /* every file built: index the library                */
 PRJ_FINISH,        /* hand the link command to F9's link step            */
 PRJ_DONE
};
/* A phase's answer: go on with the next phase, or wait for a step. */
#define E_PRJ_GO_ON 0
#define E_PRJ_WAIT  1

static struct {
 ECNT *cn;
 char **arg;               /* the compile command                       */
 int argc;
 struct dirfile *df;       /* the project's FILES                        */
 int k;                    /* the file being built                       */
 int libsw, exlib, sccs;
 char ofile[128];
 struct stat lbuf, obuf;
 enum e_prj_phase phase;
 e_build_cont done;
 void *data;
} g_prj;

static void e_prj_run(void);

/**
 * e_prj_setup - Read the project and prepare its compile command.
 * @f: the window the key was pressed in.
 * Return: 0, or the result of a project that cannot be built (said in a box).
 */
static int e_prj_setup(FENSTER *f)
{
 ECNT *cn = f->ed;
 struct dirfile *df = NULL;
 char **arg;
 int i, j, k, elen, argc, exlib = 0;
 char ofile[128];
 struct stat lbuf[1];

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
 /* MALLOC leaves slot 0 uninitialised.  The CMP/FILES lookups below can bail
    out early (e.g. a project whose variables did not load) straight into
    e_free_arg, which FREEs every non-NULL slot -- so an unset slot must read
    NULL or it frees a wild pointer and glibc aborts.  The build loops set
    slot 0 on their first iteration, so this only matters on the early exits. */
 if (e_arg) e_arg[0] = NULL;
 if (arg) arg[0] = NULL;
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
 memset(&g_prj, 0, sizeof g_prj);
 g_prj.cn = cn;
 g_prj.arg = arg;
 g_prj.argc = argc;
 g_prj.df = df;
 g_prj.exlib = exlib;
 g_prj.lbuf = lbuf[0];
 return(0);
}

/**
 * e_prj_top - The window the project build's steps report to.
 *
 * Looked up for each step: the user may move between windows while the
 * build runs.
 */
static FENSTER *e_prj_top(void)
{
 return(g_prj.cn->f[g_prj.cn->mxedt]);
}

/**
 * e_prj_end - The project build is over: free its state, run what waits.
 * @result: 0 when every file was built, else the failing step's result.
 */
static void e_prj_end(int result)
{
 e_build_cont done = g_prj.done;
 void *data = g_prj.data;

 if (g_prj.arg)
  e_free_arg(g_prj.arg, g_prj.argc);
 g_prj.arg = NULL;
 if (g_prj.df)
  freedf(g_prj.df);
 g_prj.df = NULL;
 if (result)
  e_free_arg(e_arg, e_argc);
 e_build_continue(done, e_prj_top(), result, data);
}

/**
 * e_prj_step_done - A compile, ar or ranlib of the project has ended: go on
 * with the next file, or stop at the first file with errors.
 * @f: the top window; @result: the step's result; @data: unused.
 */
static void e_prj_step_done(FENSTER *f, int result, void *data)
{
 (void)f;
 (void)data;
 if (result)
 {
  e_prj_end(result);
  return;
 }
 e_prj_run();
}

/**
 * e_prj_object_name - The object file of project file @k, into g_prj.ofile.
 */
static void e_prj_object_name(int k)
{
 struct dirfile *df = g_prj.df;
 int j, elen = strlen(e_prog.exedir)-1;

 for (j = strlen(df->name[k])-1; j >= 0 && df->name[k][j] != DIRC; j--)
  ;
 if (e_prog.exedir[elen] == '/')
  sprintf(g_prj.ofile, "%s%s ", e_prog.exedir, df->name[k]+j+1);
 else sprintf(g_prj.ofile, "%s/%s ", e_prog.exedir, df->name[k]+j+1);
 for (j = strlen(g_prj.ofile); j > 0 && g_prj.ofile[j] != '.'; j--)
  ;
 g_prj.ofile[j+1] = 'o';
 g_prj.ofile[j+2] = '\0';
}

/**
 * e_prj_object_current - Is the object of project file @k newer than its
 * source?  Current objects are not compiled again.
 */
static int e_prj_object_current(int k)
{
#ifndef CHECKHEADER
 struct stat cbuf;
#endif

 if (stat(g_prj.ofile, &g_prj.obuf))
  return(0);
 if (g_prj.obuf.st_mtime > last_time) last_time = g_prj.obuf.st_mtime;
#ifdef CHECKHEADER
 return(!e_check_header(g_prj.df->name[k], g_prj.obuf.st_mtime, g_prj.cn, 0));
#else
 stat(g_prj.df->name[k], &cbuf);
 return(g_prj.obuf.st_mtime >= cbuf.st_mtime);
#endif
}

/**
 * e_prj_save_file - Save the window of project file @k when it has edits,
 * so the compile sees what the user typed.
 */
static void e_prj_save_file(int k)
{
 ECNT *cn = g_prj.cn;
 int j;

 for (j = cn->mxedt; j > 0; j--)
  if (!strcmp(cn->f[j]->datnam, g_prj.df->name[k]) && cn->f[j]->save)
   e_save(cn->f[j]);
}

/**
 * e_prj_compile_file - Start the compile of project file @k.
 * Return: E_PRJ_WAIT: the machine goes on when the compile has ended.
 */
static int e_prj_compile_file(int k)
{
 FENSTER *f = e_prj_top();
 PIC *pic = NULL;
 int file = 0;

 g_prj.argc = e_add_arg(&g_prj.arg, g_prj.df->name[k], g_prj.argc, g_prj.argc);
#ifndef NO_MINUS_C_MINUS_O
 g_prj.argc = e_add_arg(&g_prj.arg, "-o", g_prj.argc, g_prj.argc);
 g_prj.argc = e_add_arg(&g_prj.arg, g_prj.ofile, g_prj.argc, g_prj.argc);
#endif
 g_prj.arg[g_prj.argc] = NULL;
 remove(g_prj.ofile);
 g_prj.sccs = 1;
 e_p_set_compiled_source(g_prj.df->name[k]);
 /* The child prints its own command line first (e_exec_inf), as every
    single-file build does; writing it here as well showed each command
    twice. */
 e_sys_ini();
 file = e_exec_inf(f, g_prj.arg, g_prj.argc);
 e_sys_end();
 if (!file)
 {
  if (pic) e_close_view(pic, 1);
  e_prj_end(WPE_ESC);
  return(E_PRJ_WAIT);
 }
 e_p_l_comp = 1;
 g_prj.phase = PRJ_COMPILED;
 e_p_exec(f, pic, e_prj_step_done, NULL);
 return(E_PRJ_WAIT);
}

/**
 * e_prj_object_base - The object's file name without its directory.
 */
static char *e_prj_object_base(void)
{
 int j;

 for (j = strlen(g_prj.ofile); j >= 0 && g_prj.ofile[j] != '/'; j--)
  ;
 return(g_prj.ofile + j + 1);
}

/**
 * e_prj_is_program_object - Does the object go on the link line itself
 * (main.o, or one named after the program) rather than into the library?
 */
static int e_prj_is_program_object(void)
{
 char *base = e_prj_object_base();
 int len = strlen(e_s_prog.exe_name);

 if (!strcmp(base, "main.o"))
  return(1);
 return(!strncmp(e_s_prog.exe_name, base, len) && g_prj.ofile[len] == '.');
}

/**
 * e_prj_inserted_after_compile - Is a freshly compiled object put into the
 * library at once?  The test the project build has always made.
 */
static int e_prj_inserted_after_compile(void)
{
 char *base = e_prj_object_base();
 int len = strlen(e_s_prog.exe_name);

 if (g_prj.exlib || library[0] == '\0' || !strcmp(base, "main.o"))
  return(0);
 return(strncmp(e_s_prog.exe_name, base, len) || g_prj.ofile[len] == '.');
}

/**
 * e_prj_insert - Put the object into the project's library as a step of
 * the build.
 * @next: the phase after the insert.
 */
static int e_prj_insert(enum e_prj_phase next)
{
 g_prj.phase = next;
 g_prj.libsw = 1;
 e_make_library(library, g_prj.ofile, e_prj_top(), e_prj_step_done, NULL);
 return(E_PRJ_WAIT);
}

/**
 * e_prj_after_compile - File k compiled: insert it into the library when it
 * is a library object that the archive does not already take.
 */
static int e_prj_after_compile(void)
{
 g_prj.phase = PRJ_INSERTED;
 if (e_prj_inserted_after_compile())
  return(e_prj_insert(PRJ_INSERTED));
 return(E_PRJ_GO_ON);
}

/**
 * e_prj_drop_compile_args - Take file k's source and object off the
 * compile command, ready for the next file.
 */
static int e_prj_drop_compile_args(void)
{
 int j;

 for (j = 0; j < 3; j++) FREE(g_prj.arg[g_prj.argc-j-1])
  ;
 g_prj.argc -= 3;
 g_prj.phase = PRJ_LINK_ARG;
 return(E_PRJ_GO_ON);
}

/**
 * e_prj_link_arg - Add file k's object to the link, or to the library.
 */
static int e_prj_link_arg(void)
{
 g_prj.phase = PRJ_NEXT;
 if (library[0] == '\0' || e_prj_is_program_object())
 {
  e_argc = e_add_arg(&e_arg, g_prj.ofile, e_argc, e_argc);
  return(E_PRJ_GO_ON);
 }
 if (g_prj.exlib || g_prj.obuf.st_mtime >= g_prj.lbuf.st_mtime)
  return(e_prj_insert(PRJ_NEXT));
 return(E_PRJ_GO_ON);
}

/**
 * e_prj_look_at_file - The next project file: compile it, or skip to its
 * place in the link when its object is current.
 */
static int e_prj_look_at_file(void)
{
 int k = g_prj.k;

 if (k >= g_prj.df->anz)
 {
  g_prj.phase = PRJ_RANLIB;
  return(E_PRJ_GO_ON);
 }
 e_prj_save_file(k);
 e_prj_object_name(k);
 if (e_prj_object_current(k))
 {
  g_prj.phase = PRJ_LINK_ARG;
  return(E_PRJ_GO_ON);
 }
 return(e_prj_compile_file(k));
}

/**
 * e_prj_ranlib - Every file is built: index the library if it changed.
 */
static int e_prj_ranlib(void)
{
 g_prj.phase = PRJ_FINISH;
#ifdef RANLIB
 if (g_prj.libsw && library[0] != '\0')
 {
  FENSTER *f = e_prj_top();
  PIC *pic = NULL;
  char *ar_arg[3];
  int file = 0;

  ar_arg[0] = "ranlib";
  ar_arg[1] = library;
  ar_arg[2] = NULL;
  /* The child prints its command line itself (e_exec_inf). */
  e_sys_ini();
  file = e_exec_inf(f, ar_arg, 2);
  e_sys_end();
  if (!file)
  {
   e_prj_end(-1);
   return(E_PRJ_WAIT);
  }
  e_p_exec(f, pic, e_prj_step_done, NULL);
  return(E_PRJ_WAIT);
 }
#endif
 return(E_PRJ_GO_ON);
}

/**
 * e_prj_finish - Hand the link command to F9's link step.
 */
static int e_prj_finish(void)
{
 struct dirfile *df;
 e_build_cont done = g_prj.done;
 void *data = g_prj.data;
 int k;

 if (library[0] != '\0')
  e_argc = e_add_arg(&e_arg, library, e_argc, e_argc);
 freedf(g_prj.df);
 g_prj.df = NULL;
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
 e_free_arg(g_prj.arg, g_prj.argc);
 g_prj.arg = NULL;
 g_prj.phase = PRJ_DONE;
 if (!g_prj.sccs)
 {
  e_p_exec(e_prj_top(), NULL, done, data);   /* nothing compiled: "Success" */
  return(E_PRJ_WAIT);
 }
 e_build_continue(done, e_prj_top(), 0, data);
 return(E_PRJ_WAIT);
}

/**
 * e_prj_run - Drive the project build until it waits for a step or ends.
 *
 * Each compile, ar and ranlib runs in the background; its end calls
 * e_prj_step_done, which comes back here for the next phase.
 */
static void e_prj_run(void)
{
 int wait = E_PRJ_GO_ON;

 while (wait == E_PRJ_GO_ON)
 {
  switch (g_prj.phase)
  {
   case PRJ_FILE:     wait = e_prj_look_at_file();       break;
   case PRJ_COMPILED: wait = e_prj_after_compile();      break;
   case PRJ_INSERTED: wait = e_prj_drop_compile_args();  break;
   case PRJ_LINK_ARG: wait = e_prj_link_arg();           break;
   case PRJ_NEXT:     g_prj.k++;  g_prj.phase = PRJ_FILE; break;
   case PRJ_RANLIB:   wait = e_prj_ranlib();             break;
   case PRJ_FINISH:   wait = e_prj_finish();             break;
   default:           wait = E_PRJ_WAIT;                 break;
  }
 }
}

/**
 * e_c_project - Build the open project's files, one compile at a time, in
 * the background.
 * @f: the window the key was pressed in; @done: what runs when every file
 * is built or the first one fails, exactly once; @data: handed to @done.
 * Return: 0.
 */
int e_c_project(FENSTER *f, e_build_cont done, void *data)
{
 char cwd_saved[WPE_BUILD_PATH_MAX];
 int ret, cwd_chg;

 /* The project builds in its own directory, whichever one the editor was
    started from; every later step runs where the first one did. */
 e_prj_anchor_path();
 cwd_chg = e_build_pushd(f, cwd_saved, sizeof cwd_saved);
 ret = e_prj_setup(f);
 if (ret)
 {
  if (cwd_chg)
   e_build_popd(cwd_saved);
  e_build_continue(done, f, ret, data);
  return(0);
 }
 g_prj.done = done;
 g_prj.data = data;
 g_prj.k = 0;
 g_prj.phase = PRJ_FILE;
 e_prj_run();
 if (cwd_chg)
  e_build_popd(cwd_saved);
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

/**
 * e_new_message - Start Messages afresh for a new build.
 * @f: the window the build was asked from; it may be Messages itself (F6 to
 * Messages, then Alt-A).
 *
 * The old Messages window is closed and a new one opened.  The desktop is
 * taken before the close: when @f is the Messages window, closing it frees
 * @f, and reading the desktop through it afterwards was a use after free.
 * Return: 0, or WPE_ESC when the new window could not be opened.
 */
int e_new_message(FENSTER *f)
{
 ECNT *cn = f->ed;
 int i;

 if (e_p_m_buffer)
  e_p_red_buffer(e_p_m_buffer);
 for (i = cn->mxedt; i > 0; i--)
  if (cn->f[i] && cn->f[i]->datnam &&
      !strcmp(cn->f[i]->datnam, "Messages"))
  {
   e_switch_window(cn->edt[i], cn->f[cn->mxedt]);
   e_close_window(cn->f[cn->mxedt]);
   break;
  }
 if (access("Messages", 0) == 0)
  remove("Messages");
 if (e_edit(cn, "Messages"))
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

/**
 * e_p_take_file_token - Copy the text a pattern's ${FILE} delimits.
 * @file: where the name goes, at least as long as @str plus one;
 * @str: the text between the pattern's delimiters.
 *
 * The whole token is kept, blanks included, so a row naming "my file.c"
 * jumps into that file instead of a new, empty "file.c".  Only the blanks
 * around the token and the quotes some compilers put around a name are
 * taken off.  e_p_row_file decides later which part names a real file.
 */
static void e_p_take_file_token(char *file, const char *str)
{
 size_t len;

 while (isspace((unsigned char)*str))
  str++;
 len = strlen(str);
 while (len > 0 && isspace((unsigned char)str[len - 1]))
  len--;
 memcpy(file, str, len);
 file[len] = '\0';
 e_strip_quotes(file);
}

int e_p_konv_mess(char *var, char *str, char *txt, char *file, char *cmp,
  int *y, int *x)
{
 int i;
 char *cp;

 if (!strncmp(var, "FILE", 4) && !isalnum(var[4]))
  e_p_take_file_token(file, str);
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

/* The word a compiler's row says when the compiler's own pattern carries no
   warning marker. */
#define E_P_WARNING_WORD "warning"

/**
 * e_p_row_is_warning - Did the compiler call this row a warning?
 * @row: the Messages row; @wtxt, @wn, @wnum: the warning markers of the
 * compiler's pattern (the text, and the column it must start at, -1 for
 * anywhere).
 *
 * The count at the end of a build and the mark on each warning row come
 * from this, so the user sees at once which rows stop the build and which
 * only advise.  A pattern with no marker falls back to the word itself, in
 * any case.
 * Return: 1 for a warning row.
 */
static int e_p_row_is_warning(char *row, char **wtxt, int *wn, int wnum)
{
 int m;
 size_t n = strlen(E_P_WARNING_WORD);
 char *p;

 for (m = 0; m < wnum; m++)
 {
  if (wn[m] == -1 && strstr(row, wtxt[m]))
   return(1);
  if (wn[m] > -1 && (int)strlen(row) > wn[m] &&
      !strncmp(row + wn[m], wtxt[m], strlen(wtxt[m])))
   return(1);
 }
 if (wnum > 0)
  return(0);
 for (p = row; *p; p++)
  if (!strncasecmp(p, E_P_WARNING_WORD, n))
   return(1);
 return(0);
}

/**
 * e_p_row_kind - Is this row an error or a warning?
 * @row: the Messages row; @wtxt, @wn, @wnum: the warning markers of the
 * compiler's pattern.
 * Return: E_ROW_ERROR or E_ROW_WARNING.
 */
static int e_p_row_kind(char *row, char **wtxt, int *wn, int wnum)
{
 if (e_p_row_is_warning(row, wtxt, wn, wnum))
  return(E_ROW_WARNING);
 return(E_ROW_ERROR);
}

/**
 * e_p_file_exists - Is there a file by this name where the build ran?
 * @name: a name as a compiler printed it.
 */
static int e_p_file_exists(const char *name)
{
 struct stat st;

 if (!name || !name[0])
  return(0);
 return(stat(name, &st) == 0);
}

/**
 * e_p_unquoted_copy - A copy of @s without the blanks around it and the
 * quotes a compiler may put around a file name.
 * Return: a new string the caller frees.
 */
static char *e_p_unquoted_copy(const char *s)
{
 char *copy = MALLOC(strlen(s) + 1);

 e_p_take_file_token(copy, s);
 return(copy);
}

/* make writes the makefile of a failed recipe inside brackets. */
#define E_P_NAME_OPENER '['

/**
 * e_p_row_file - The file name an error row is filed under.
 * @token: the whole text the pattern's ${FILE} delimited.
 *
 * The whole token wins when it names a file, so "my file.c" and a path of
 * any length jump where the compiler said.  A pattern that lets ${FILE}
 * swallow a lead-in, as Python's "  File "t.py"", a Scala 3 heading or
 * make's "make: *** [Makefile:3: all]" do, still finds the file: the token
 * is tried from each blank or opening bracket onwards, longest first.
 * When nothing exists the whole token is kept, so the jump can say which
 * name it could not find.
 * Return: a new string the caller frees.
 */
static char *e_p_row_file(const char *token)
{
 const char *p;
 char *candidate;

 if (e_p_file_exists(token))
  return(WpeStrdup(token));
 for (p = token; *p; p++)
 {
  if (!isspace((unsigned char)*p) && *p != E_P_NAME_OPENER)
   continue;
  candidate = e_p_unquoted_copy(p + 1);
  if (e_p_file_exists(candidate))
   return(candidate);
  FREE(candidate);
 }
 return(WpeStrdup(token));
}

/* The file a pattern without ${FILE} names: the source being compiled.  A
   compiler that checks one file at a time (a68g) prints only the line. */
static char *e_p_compiled_source = NULL;

/**
 * e_p_set_compiled_source - Remember which source the next rows are about.
 * @name: the file handed to the compiler, or NULL when a step compiles no
 * single file (a link, a make).
 *
 * Lets an error row of a compiler that never repeats the file name still
 * take the user to the right file.
 */
static void e_p_set_compiled_source(const char *name)
{
 if (e_p_compiled_source)
  FREE(e_p_compiled_source);
 e_p_compiled_source = NULL;
 if (name && name[0])
  e_p_compiled_source = WpeStrdup(name);
}

/**
 * e_p_pattern_names_file - Does the message pattern carry ${FILE}?
 * @srch: the pattern.
 */
static int e_p_pattern_names_file(const char *srch)
{
 return(strstr(srch, "${FILE}") != NULL);
}

/**
 * e_p_longest_row - The length of the longest row of @b.
 *
 * Every name, column text and compiler text a pattern takes from a row is
 * a piece of that row, so a buffer of this size (plus the pattern's own
 * length) holds any of them, however long the path a compiler printed.
 */
static size_t e_p_longest_row(BUFFER *b)
{
 size_t longest = 0;
 int i;

 for (i = 0; i < b->mxlines; i++)
  if ((size_t)b->bf[i].len > longest)
   longest = b->bf[i].len;
 return(longest);
}

/* A pattern is cut into at most this many rows, one per line of output. */
#define E_P_PATTERN_ROWS 4

int e_p_cmp_mess(char *srch, BUFFER *b, int *ii, int *kk, int ret,
                 size_t row_max)
{
 char *cp, *cmp, *file, *search, *tmp[E_P_PATTERN_ROWS], **wtxt = NULL;
 int j, l, m, n, iy, iorig, i = *ii, k = *kk, x = 0, y = -1, wnum = 0;
 int *wn = NULL;
 size_t cap = row_max + strlen(srch) + 2;

 if (e_p_compiled_source)
  cap += strlen(e_p_compiled_source);
 cmp = MALLOC(cap);
 file = MALLOC(cap);
 search = MALLOC(cap);
 for (n = 0; n < E_P_PATTERN_ROWS; n++)
  tmp[n] = MALLOC(strlen(srch) + 2);
 cmp[0] = search[0] = file[0] = '\0';
 if (!e_p_pattern_names_file(srch) && e_p_compiled_source)
  strcpy(file, e_p_compiled_source);
 wtxt = MALLOC(1);
 wn = MALLOC(1);
 for (j = 0, n = 0; n < E_P_PATTERN_ROWS && srch[j]; n++)
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
    err_li[k].file = e_p_row_file(file);
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
    err_li[k].y = err_li[k].row = iorig;
    err_li[k].kind = e_p_row_kind((char *)b->bf[i].s, wtxt, wn, wnum);
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
 for (m = 0; m < E_P_PATTERN_ROWS; m++)
  FREE(tmp[m]);
 FREE(cmp);
 FREE(file);
 FREE(search);
 return(ret);
}

#endif

