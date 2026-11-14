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
#include "we_buffer.h"
#include "we_lineend.h"
#include "we_progname.h"
#include "we_startcmd.h"
#include "we_startplan.h"

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
#include "we_childsig.h"
#include <stdint.h>

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
/* The project's library, kept whole on the heap: LIBNAME is a path, and the
   80 bytes that held it cut it on Save. */
static char *e_prj_library_text;

/**
 * e_prj_library - The project's library, "" when it has none.
 *
 * Project-Options shows it and Save writes it back to the .prj, so what the
 * user typed or the file held is what comes back, at any length.
 * Return: the library's path; never NULL.
 */
static char *e_prj_library(void)
{
 if (!e_prj_library_text)
  e_prj_library_text = WpeStrdup("");
 return(e_prj_library_text ? e_prj_library_text : (char *)"");
}

/**
 * e_prj_library_set - Keep @path as the project's library.
 * @path: its path, or "" for none.
 *
 * Without memory the library stays as it was.
 */
static void e_prj_library_set(const char *path)
{
 char *copy = wpe_path_dup(path);

 if (!copy)
  return;
 free(e_prj_library_text);
 e_prj_library_text = copy;
}
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
extern int e_lsp_ui_direct(FENSTER *f, int action);   /* a language-server action on its own key */

int e_prog_switch(FENSTER *f, int c)
{
 int action = wpe_binding_action(c, wpe_binding_style(f->ed->edopt), WPE_CTX_PROGRAM);

 switch (action)
 {
  case WPE_ACT_RUN:
   e_run(f);
   break;
  case WPE_ACT_LSP_DEFINITION:
  case WPE_ACT_LSP_REFERENCES:
  case WPE_ACT_LSP_RENAME:
  case WPE_ACT_LSP_COMPLETE:
  case WPE_ACT_LSP_FORMAT:
  case WPE_ACT_LSP_OUTLINE:
  case WPE_ACT_LSP_WORKSPACE_SYMBOLS:
   e_lsp_ui_direct(f, action);   /* the Modern style's single keys for what Alt-Q letters do */
   break;
  case WPE_ACT_LSP_PREFIX:   /*  language-server prefix: the letter after it runs
                                 the action directly (silent, no flicker); ? (or any
                                 unknown key) opens the menu; ESC cancels. */
   e_lsp_ui_key(f);
   break;
  case WPE_ACT_LSP_MENU:     /*  click on the bottom-bar entry -> open the menu */
   e_lsp_ui_menu(f);
   break;
  case WPE_ACT_MAKE:
   e_p_make(f);
   break;
  case WPE_ACT_COMPILE:
   e_compile(f);
   break;
  case WPE_ACT_INSTALL:
   e_install(f);
   break;
  case WPE_ACT_EXEC_MAKE:
   e_exec_make(f);
   break;
#ifdef WPE_AI
  case WPE_ACT_AI_PREFIX:    /*  AI assistant prefix (Alt-B is the Block menu; Alt-G is a
                                 free Alt-letter, chosen like the LSP's Alt-Q so the assistant
                                 never shadows an existing key).  The bar entry reads "Alt-G AI". */
   e_ai_ui_key(f);
   break;
  case WPE_ACT_AI_MENU:      /*  click on the "Alt-G AI" bottom-bar entry: the same door as
                                 Alt-G, so the first-use notice is shown once, whichever
                                 way the user came in */
   e_ai_ui_key(f);
   break;
#endif
  case WPE_ACT_NEXT_ERROR:
   e_next_error(f);
   break;
  case WPE_ACT_PREV_ERROR:
   e_previous_error(f);
   break;
#ifdef DEBUGGER
  case WPE_ACT_DEBUG_PREFIX:   /*  the debugger's prefix */
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
 /* A project the user chose himself stays his once its path is absolute. */
 if (e_opt_value_from(E_OPT_SCOPE_PROGRAMMING, "Project", e_prog.project) == WPE_FROM_USER)
  e_opt_value_typed(E_OPT_SCOPE_PROGRAMMING, "Project", path);
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

static void e_build_step_end_of(FENSTER *f, int result);

/**
 * e_compile_after - The compile of Alt-C has ended: note its object, end the
 * build.
 * @f: the top window; @result: the compile's result; @data: unused.
 */
static void e_compile_after(FENSTER *f, int result, void *data)
{
 (void)data;
 e_build_step_end_of(f, result);
 e_build_chain_end(f, result, NULL);
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
 e_comp(f, e_compile_after, NULL);
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
 e_build_step_end_of(f, result);
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
 e_build_step_end_of(f, result);
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
 * Return: 0 when the build started, -1 when another build is running or
 * the user kept the debug session.
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

/* What this editor built here.  An object, a program or a library counts as
   up to date only when this editor wrote that very file in this folder, so a
   built file a clone ships never makes a build skip a step and never reaches
   Ctrl-F9.  The records live in the user's state folder (we_startcmd.h); one
   set, of the build's folder, is read when a build is planned and written
   once per step. */
#define E_BUILT_STEP_MAX 2   /* the most outputs one step writes: object and program */

static WpeBuiltSet g_built;     /* the records of the build's folder             */
static int g_built_ready;       /* g_built is loaded for the build in progress    */
static struct {
 char path[WPE_BUILD_PATH_MAX];
 int existed;                   /* the output was there before the step           */
 struct stat st;                /* and this is what it was                        */
} g_built_before[E_BUILT_STEP_MAX];
static int g_built_nbefore;     /* outputs of the step in progress                */

/**
 * e_build_record_begin - Read the records of the folder a build runs in.
 * @f: the window the build was asked from.
 *
 * Called once when a build is planned, after the user answered whatever the
 * plan asked.  A store that cannot be believed is said once and nothing
 * counts as built here: every F9 builds.
 */
static void e_build_record_begin(FENSTER *f)
{
 char *dir = e_build_dir(f);

 if (g_built_ready)
  wpe_startcmd_built_free(&g_built);
 g_built_ready = 0;
 g_built_nbefore = 0;
 if (!dir)
  return;
 wpe_startcmd_built_load(dir, &g_built);
 FREE(dir);
 g_built_ready = 1;
 if (g_built.state == WPE_STARTCMD_STORE_BAD)
  e_build_record_say(f, E_BUILT_NOT_READ);
}

/**
 * e_build_stat - The stat of a build output, taken in the build's folder.
 * @output: its path as the build names it; @st: receives the stat.
 *
 * Resolved against the folder the plan runs in, never against the process's
 * folder, so the record and the lookup name one file whatever the editor's
 * working folder is.
 * Return: 0 when it exists, else -1.
 */
static int e_build_stat(const char *output, struct stat *st)
{
 char path[WPE_BUILD_PATH_MAX];
 int n;

 if (!g_built.folder[0])
  return(-1);
 if (output[0] == DIRC)
  n = snprintf(path, sizeof path, "%s", output);
 else
  n = snprintf(path, sizeof path, "%s/%s", g_built.folder, output);
 if (n < 0 || (size_t)n >= sizeof path)
  return(-1);
 return(stat(path, st));
}

/**
 * e_build_current - Is an output up to date: built here, and no older than
 * what it is made of?
 * @output: the output; @source: the file it is made of, or NULL;
 * @since: it must be no older than this time, or 0; @newest: raised to the
 * output's time when it exists, or NULL; @out_st: receives its stat when it
 * exists, or NULL.
 *
 * The build's one up-to-date rule.  A time stamp alone never makes a shipped
 * file current: the record of what this editor built must hold this very
 * file (device, inode, times, size).  So the first F9 in a fresh clone
 * builds once, then "nothing to build" works as it always did.
 * Return: 1 when the output is current.
 */
static int e_build_current(const char *output, const char *source, M_TIME since, M_TIME *newest,
                           struct stat *out_st)
{
 struct stat st, sb;

 if (e_build_stat(output, &st))
  return(0);
 if (out_st)
  *out_st = st;
 if (newest && st.st_mtime > *newest)
  *newest = st.st_mtime;
 if (source && (e_build_stat(source, &sb) || st.st_mtime < sb.st_mtime))
  return(0);
 if (st.st_mtime < since)
  return(0);
 return(wpe_startcmd_built_here(&g_built, output, &st));
}

/**
 * e_built_step_begin - Remember the outputs of a step before it runs.
 * @first/@second: the outputs the step writes (a compile: the object; a
 * link: the program; a compiler that writes its own program: both; ar and
 * ranlib: the library), or NULL.
 *
 * What a clone shipped is there before the step; only a file the step made
 * or changed is the step's work.
 */
static void e_built_step_begin(const char *first, const char *second)
{
 const char *out[E_BUILT_STEP_MAX];
 int i, n = 0;

 out[0] = first;
 out[1] = second;
 for (i = 0; i < E_BUILT_STEP_MAX; i++)
 {
  if (!out[i] || !out[i][0])
   continue;
  snprintf(g_built_before[n].path, sizeof g_built_before[n].path, "%s", out[i]);
  g_built_before[n].existed = (e_build_stat(out[i], &g_built_before[n].st) == 0);
  n++;
 }
 g_built_nbefore = n;
}

/**
 * e_build_step_note - A step succeeded: its work is this editor's.
 * @f: the window.
 *
 * Notes only the outputs of the step that ran, and only a file that was
 * absent before it or whose identity changed during it, then writes the
 * records once.  A step that changed nothing notes nothing.
 */
static void e_build_step_note(FENSTER *f)
{
 struct stat now;
 int i;

 for (i = 0; i < g_built_nbefore && g_built_ready; i++)
 {
  if (e_build_stat(g_built_before[i].path, &now))
   continue;
  if (g_built_before[i].existed && wpe_startcmd_same_file(&g_built_before[i].st, &now))
   continue;
  wpe_startcmd_note_built(&g_built, g_built_before[i].path, &now);
 }
 g_built_nbefore = 0;
 if (g_built_ready && g_built.dirty && wpe_startcmd_built_save(&g_built))
  e_build_record_say(f, E_BUILT_NOT_SAVED);
}

/**
 * e_build_step_end_of - A step ended: note its work, or forget it failed.
 * @f: the window; @result: the step's result.
 */
static void e_build_step_end_of(FENSTER *f, int result)
{
 if (result)
  g_built_nbefore = 0;
 else
  e_build_step_note(f);
}

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
  e_build_note_up_to_date(e_prj_program());
 else
  e_build_note_up_to_date(estr);
}

static void e_build_path_too_long(FENSTER *f, const char *what);

/**
 * e_prog_source_of - The open file as the program-name owner reads it.
 * @s: filled in; @f: the file's window; @entry: the language's entry whose
 * compiler style names the files (the one in force, or the user's own for
 * "Use mine"); @executable: its Executable, or NULL; @exedir: the output
 * folder.
 *
 * The one place the compiler's style is read to name something: a compiler
 * of the "other" style (fpc, javac, rustc) writes its own program, which is
 * therefore never placed under Exedir, and its object is a .class.  Every
 * name the editor gives to what a build makes goes through the owner from
 * here, so F9, Alt-C and Ctrl-F9 name the same files.
 */
static void e_prog_source_of(WpeProgramSource *s, FENSTER *f, const struct e_s_prog *entry,
                             const char *executable, const char *exedir)
{
 memset(s, 0, sizeof *s);
 s->name = f->datnam;
 s->exedir = exedir;
 s->executable = executable;
 s->self_linked = entry->comp_sw & 1;
 s->class_object = entry->comp_sw & 1;
}

/**
 * e_prog_switch_text - A language's compiler style, as its xwperc writes it.
 * @entry: the language's entry; @out/@n: receive "0" or "1".
 *
 * Run compares the style with the answers a folder's xwperc is checked
 * against, which hold it as this text (CompilerSwitch).
 */
static void e_prog_switch_text(const struct e_s_prog *entry, char *out, size_t n)
{
 snprintf(out, n, "%d", entry->comp_sw);
}

/**
 * e_p_link_operands_of - The program and the object file of F9's link step.
 * @f: the source's window; @exe_name: the language's Executable, or NULL;
 * @exedir: the output folder; @estr/@ostr: receive them; @sz: the room of
 * each.
 *
 * Named by the owner, which joins an Exedir a folder sets ("@r") through the
 * start plan's operand, so it makes a path ("./@r/t.o") and never a response
 * file the linker reads.  The build's plan and its link step both take the
 * names from here, so a source with a long name is linked under the name
 * the plan judged.
 * Return: 0, or -1 when one does not fit.
 */
static int e_p_link_operands_of(FENSTER *f, const char *exe_name, const char *exedir,
                                char *estr, char *ostr, size_t sz)
{
 WpeProgramSource s;

 e_prog_source_of(&s, f, &e_s_prog, exe_name, exedir);
 if (wpe_progname_program(estr, sz, &s))
  return(-1);
 return(wpe_progname_object(ostr, sz, &s));
}

static int e_exec_planned(FENSTER *f, char **argv);

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
 char ostr[WPE_BUILD_PATH_MAX], estr[WPE_BUILD_PATH_MAX];
 char cwd_saved[WPE_PATH_SIZE];
 int file = -1, cwd_chg = 0;
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
  if (e_p_link_operands_of(f, e_s_prog.exe_name, e_prog.exedir,
                           estr, ostr, sizeof estr))
  {
   e_free_arg(e_arg, e_argc);
   if (cwd_chg)
    e_build_popd(cwd_saved);
   e_build_path_too_long(f, "program");
   e_build_chain_end(f, -1, NULL);
   return;
  }
  e_argc = e_add_arg(&e_arg, estr, 2, e_argc);
  e_argc = e_add_arg(&e_arg, ostr, 3, e_argc);
  if (e_build_current(estr, ostr, 0, NULL, NULL))
   linkRequest = 0;
 }
 else
 {
  if (!e_p_l_comp && e_build_current(e_prj_program(), NULL, last_time, NULL, NULL))
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
  if (e__project)
   e_built_step_begin(e_prj_program(), NULL);
  else
   e_built_step_begin(estr, NULL);
  e_sys_ini();
  file = e_exec_planned(f, e_arg);
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

/**
 * e_view_add_row - Put an empty row at the foot of a view the editor writes.
 * @b: the buffer of Messages or of an output window.
 *
 * What the user reads grows a row at a time as a build or a program prints.
 * Nothing in it is the user's to undo, and without memory the view stays as
 * it was and the status line says so.
 * Return: E_BUFFER_OK, or E_BUFFER_NOMEM.
 */
int e_view_add_row(BUFFER *b)
{
 int r = e_buffer_insert_blank(b, b->mxlines, E_BUFFER_STEP_NEW);

 if (r != E_BUFFER_OK)
  wpe_buffer_say_nomem(b, "a row of output");
 return(r);
}

/* What the status row says when a line of output was refused because the
   window it was for is not a view and holds no row wider than its width. */
#define E_VIEW_SAY_TOO_WIDE "A line of output is wider than the window and was not shown"

/**
 * e_view_say_refused - Say on the status row why output did not reach a
 * window, when it did not.
 * @b: the window's buffer; @r: what the owner answered.
 *
 * Output never goes missing in silence: for want of memory the status row
 * says so, and a line refused for its width says that instead.
 */
static void e_view_say_refused(BUFFER *b, int r)
{
 if (r == E_BUFFER_NOMEM)
  wpe_buffer_say_nomem(b, "the output");
 else if (r == E_BUFFER_RANGE && WpeEditor != NULL && WpeEditor->mxedt >= 0)
  e_status_say(WpeEditor->f[WpeEditor->mxedt], E_VIEW_SAY_TOO_WIDE);
}

/**
 * e_view_append - Add text at the foot of a view the editor writes.
 * @b: the buffer of Messages or of an output window; @text: one or more
 * lines, the empty ones left out, each one row however wide.
 *
 * What a build or a program prints reaches the window row by row, each
 * line whole, so Alt-T and Enter on a compiler's row find the file it
 * names however long its path; without memory the view stays as it was
 * and the status line says so.
 */
void e_view_append(BUFFER *b, const char *text)
{
 e_view_say_refused(b, e_eol_append_rows(b, text, E_EOL_NORMALIZE));
}

/**
 * e_view_append_wrapped - Add text at the foot of a view, wider lines
 * wrapped onto the next rows.
 * @b: the buffer of Messages or of a named output window; @text: one or
 * more lines, the empty ones left out.
 *
 * What the editor itself says there (a debugger's or a language server's
 * news) is read, not walked, so it wraps to stay inside the window; without
 * memory the view stays as it was and the status line says so.
 */
static void e_view_append_wrapped(BUFFER *b, const char *text)
{
 e_view_say_refused(b, e_eol_append_text(b, text, 1, E_EOL_NORMALIZE));
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
  e_view_add_row(b);
 else if (c == '\r')
  ;
 else if (c == '\b' || c == 127)
 {
  len = b->bf[line].len;
  if (len > 0)
  {
   len = e_utf8_char_before(b->bf[line].s, len, len);
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

/**
 * e_run_drain_pty - Take what the running program has written and show it.
 * @pty_fd: the program's terminal, which is non-blocking: the read stops
 * when nothing is waiting; @b, @mf: Messages.
 */
static void e_run_drain_pty(int pty_fd, BUFFER *b, FENSTER *mf)
{
 char buf[512];
 int n;

 while ((n = read(pty_fd, buf, sizeof(buf))) > 0)
 {
  int k;
  /* Capture the raw byte stream verbatim for the Alt-F5 User Screen, in
     addition to the line-parsed copy that goes to the Messages window. */
  e_d_prog_output_add(buf, n);
  for (k = 0; k < n; k++)
   e_run_pty_apply_byte(b, (unsigned char)buf[k]);
 }
 e_buffer_cursor_set(b, b->mxlines - 1, b->bf[b->mxlines - 1].len);
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
 * (e_t_user_screen).  In X11 and Wayland (xwpe) there is no real terminal to
 * drop to, so when libvterm is available (HAVE_VTERM) the captured output is
 * interpreted by an embedded VT terminal and painted full-window
 * (e_vterm_user_screen); without it Alt-F5 falls back to showing the output
 * in the Messages window.
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
  extern int e_vterm_user_screen(FENSTER *);
  return e_vterm_user_screen(f);
 }
#else
 return e_deb_out(f);
#endif
}

/* Set by the editor's SIGINT handler while a program runs: with the terminal
   in cbreak mode Ctrl-C arrives as the signal, not as the key. */
static volatile sig_atomic_t e_run_interrupted;

/**
 * e_run_on_sigint - The editor's SIGINT handler while a program runs.
 * @sig: SIGINT.
 *
 * Only notes that Ctrl-C came; the run's loop then stops the program
 * (e_run_interrupt_program), so a Ctrl-C typed where the terminal sends a
 * signal ends the program as one that arrives as a key does.
 */
static void e_run_on_sigint(int sig)
{
 (void)sig;
 e_run_interrupted = 1;
}

/**
 * e_run_catch_sigint - For the length of a run: take SIGINT as a request
 * to stop the program.
 * @old: where the editor's own disposition is kept, for
 * e_run_release_sigint.
 *
 * Outside a run the editor ignores SIGINT; during one, the user's Ctrl-C
 * must stop the program instead of being lost.
 */
static void e_run_catch_sigint(struct sigaction *old)
{
 struct sigaction on_int;

 e_run_interrupted = 0;
 memset(&on_int, 0, sizeof on_int);
 on_int.sa_handler = e_run_on_sigint;
 sigemptyset(&on_int.sa_mask);
 sigaction(SIGINT, &on_int, old);
}

/**
 * e_run_release_sigint - After a run: give SIGINT back the disposition it
 * had before, so a Ctrl-C while editing does nothing again.
 * @old: what e_run_catch_sigint kept.
 */
static void e_run_release_sigint(const struct sigaction *old)
{
 sigaction(SIGINT, old, NULL);
}

/* The program Run started: the owner's record of it, and how it ended.  One
   program runs at a time (Run does not return until it has ended). */
static struct {
 WpeChild child;
 int ended;
 int status;
} e_run_prog;

/* What Run reports for an end whose status the owner could not give: the
   shell's own code for a status it could not read. */
#define E_RUN_LOST_CODE 255

/**
 * e_run_on_end - The owner of the editor's children reports that the program
 * Run started has ended.
 * @c: its record; @status: the wait status; @how: WPE_CHILD_ENDED, or
 * WPE_CHILD_ENDED_UNKNOWN when the status is lost; @data: unused.
 *
 * Reported from the loop, once, with the program's own status however many
 * other children ended at the same moment, so the Run loop learns of the end
 * without asking the system.
 */
static void e_run_on_end(WpeChild *c, int status, int how, void *data)
{
 (void)c;
 (void)data;
 e_run_prog.status = status;
 if (how != WPE_CHILD_ENDED)
  e_run_prog.status = -1;
 e_run_prog.ended = 1;
}

/**
 * e_run_interrupt_program - Ctrl-C while a program runs: stop it.
 *
 * The whole session is signalled, as the terminal's Ctrl-C would: the shell
 * may be waiting for the program, or a pipeline of programs, which a signal
 * to the shell alone would not reach.  The program, which got SIGINT as the
 * shell gives it (see wpe_child_default_signals), ends unless it handles
 * the signal itself.  Nothing is sent once the program has ended.
 */
static void e_run_interrupt_program(void)
{
 wpe_child_signal(&e_run_prog.child, SIGINT);
}

/**
 * e_run_status_code - The code Run reports for a program's end.
 * @ws: the status waitpid gave, or -1 when it was lost.
 *
 * A program killed by a signal reports 128 plus the signal, as a shell
 * does; a negative code would make the caller run it again another way.
 */
static int e_run_status_code(int ws)
{
 if (ws < 0)
  return(E_RUN_LOST_CODE);
 if (WIFEXITED(ws))
  return(WEXITSTATUS(ws));
 return(128 + WTERMSIG(ws));
}

/* What every line about the program Run started calls it. */
#define E_RUN_WHAT "the program"

/**
 * e_run_start - Start the user's program on a terminal of its own.
 * @plan: Run's start plan, judged and answered.
 *
 * The program leads a session with the terminal as its controlling one, so
 * it reads what the user types in Messages and Ctrl-C reaches all of it.
 * Return: the editor's end of the terminal, non-blocking, or -1 when the
 * program could not be started.
 */
static int e_run_start(WpeStartPlan *plan)
{
 WpeStartIo io;

 memset(&io, 0, sizeof io);
 io.io = WPE_CHILD_IO_PTY;
 io.group = WPE_CHILD_OWN_SESSION;
 io.side_pty_slave = WPE_CHILD_NO_FD;
 io.tty_fd = WPE_CHILD_NO_FD;
 io.what = E_RUN_WHAT;
 e_run_prog.ended = 0;
 e_run_prog.status = 0;
 if (wpe_startplan_start_words(plan, &io, &e_run_prog.child, e_run_on_end, NULL) != 0)
  return(-1);
 return(e_run_prog.child.pty_fd);
}

/**
 * e_run_program_key - What the program is sent for a key the user typed.
 * @c: the key, as the display's reader decoded it.
 *
 * Every display hands over decoded keys (the terminal's reader decodes the
 * keyboard protocols too), so Backspace and Enter are already the codes the
 * program's terminal reads; Ctrl-J is the line end Enter is.  What the
 * program receives for the key (control bytes, text, the xterm sequences of
 * the cursor and function keys) is e_pty_key_bytes' encoding.
 * Return: the key to send, or 0 for none.
 */
static int e_run_program_key(int c)
{
 if (c == '\n')
  return(WPE_CR);
 return(c);
}

/**
 * e_run_serve_key - Hand the user's typing to the running program, or stop
 * it on Ctrl-C.
 * @pty_master: the program's terminal.
 *
 * Never waits: what the program's terminal does not take now (a program
 * that stopped reading) is queued on the loop and the editor's keys go on
 * working.
 */
static void e_run_serve_key(int pty_master)
{
 int c = wpe_paint_take_key(WPE_PAINT_ANY_KEY);

 if (c == CtrlC || e_run_interrupted)
 {
  e_run_interrupted = 0;
  e_run_interrupt_program();
  return;
 }
 c = e_run_program_key(c);
 if (c > 0)
  e_pty_send_key(pty_master, c);
}

/* What the screen's owner calls the program's run in its trace: while it
   runs, Messages and the keyboard are the program's. */
#define E_RUN_OWNS_SCREEN "the program's run"

/**
 * e_run_loop - Show the program's output in Messages and pass it the user's
 * typing until it ends.
 * @pty_master: the program's terminal; @b, @mf: Messages.
 *
 * Each pass reads the terminal and waits at most WPE_FD_POLL_MS, so a
 * program whose output cannot be watched still shows it, within that wait.
 * The end is the owner's report, which comes from the loop; the output the
 * program wrote before it ended is read before the loop returns.
 * Return: the program's exit code (see e_run_status_code).
 */
static int e_run_loop(int pty_master, BUFFER *b, FENSTER *mf)
{
 int code;

 wpe_paint_modal_enter(E_RUN_OWNS_SCREEN);
 for (;;)
 {
  e_run_drain_pty(pty_master, b, mf);
  if (e_run_prog.ended)
   break;
  wpe_fd_poll(WPE_FD_POLL_MS);
  if (e_run_prog.ended)
   continue;
  e_run_serve_key(pty_master);
 }
 wpe_paint_modal_leave();
 code = e_run_status_code(e_run_prog.status);
 return(code);
}

/**
 * e_run_with_pty - Run the user's program with its output in Messages.
 * @plan: Run's start plan; @b, @mf: Messages.
 *
 * Return: the program's exit code, or -1 when it could not be started on a
 * terminal (the caller then runs it another way).
 */
static int e_run_with_pty(WpeStartPlan *plan, BUFFER *b, FENSTER *mf)
{
 int pty_master, ret;
 struct sigaction old_int;

 /* Start a fresh capture so the Alt-F5 User Screen shows only this run. */
 e_d_prog_output_reset();
 pty_master = e_run_start(plan);
 if (pty_master < 0)
  return -1;

 e_view_append(b, "--- Run output (type in Messages, Ctrl-C to stop) ---");
 e_view_add_row(b);
 e_buffer_cursor_set(b, b->mxlines - 1, b->b.x);
 if (!WpeIsXwin())
  e_t_mouse_tracking_pause();
 fk_cursor(1);
 e_cursor(mf, 1);
 e_schirm(mf, 1);
 e_refresh();

 e_fd_watch(pty_master, POLLIN, NULL, NULL, "the program's output");

 e_run_catch_sigint(&old_int);
 ret = e_run_loop(pty_master, b, mf);
 e_run_release_sigint(&old_int);
 wpe_fd_close(pty_master);
 if (!WpeIsXwin())
  e_t_mouse_tracking_resume();
 return ret;
}

/* The source Ctrl-F9 was pressed on, kept by value: the program runs after
   the build, and the window may have been closed while it built. */
static char *e_run_src_dirct, *e_run_src_datnam;

static int e_run_program(ECNT *cn);

/**
 * e_run_src_keep - Remember the source Ctrl-F9 runs, whole, on the heap.
 * @f: the window of the source.
 *
 * A folder as deep as the system allows still runs: the build gets the
 * window's folder and the run gets the same one, never a prefix of it that
 * names a file which is not there.
 * Return: 0, or -1 when there is no memory.
 */
static int e_run_src_keep(FENSTER *f)
{
 char *dirct = wpe_path_dup(f->dirct);
 char *datnam = wpe_path_dup(f->datnam);

 if (!dirct || !datnam)
 {
  free(dirct);
  free(datnam);
  return(-1);
 }
 free(e_run_src_dirct);
 free(e_run_src_datnam);
 e_run_src_dirct = dirct;
 e_run_src_datnam = datnam;
 return(0);
}

/* The no-terminal run's output, cut into lines: what the program has written
   since its last newline waits here, and goes to Messages as one line when
   the newline comes, when it is as long as a line can be, or at the end. */
#define E_RUN_LINE_MAX 1024
#define E_RUN_READ_MAX 512
static struct {
 BUFFER *b;
 char line[E_RUN_LINE_MAX];
 int len;
} e_run_text;

/** e_run_text_flush - Send the line gathered so far to Messages. */
static void e_run_text_flush(void)
{
 e_run_text.line[e_run_text.len] = '\0';
 e_view_append(e_run_text.b, e_run_text.line);
 e_run_text.len = 0;
}

/** e_run_text_feed - Take one byte of the program's output. */
static void e_run_text_feed(char c)
{
 if (c == '\n')
 {
  e_run_text_flush();
  return;
 }
 e_run_text.line[e_run_text.len++] = c;
 if (e_run_text.len == E_RUN_LINE_MAX - 1)
  e_run_text_flush();
}

/**
 * e_run_text_drain - Take everything the program has written so far.
 * @fd: its output, non-blocking.
 * Return: 1 when the program closed its output, else 0.
 */
static int e_run_text_drain(int fd)
{
 char buf[E_RUN_READ_MAX];
 ssize_t n, k;

 for (;;)
 {
  n = read(fd, buf, sizeof buf);
  if (n < 0 && errno == EINTR)
   continue;
  if (n <= 0)
   return(n == 0);
  for (k = 0; k < n; k++)
   e_run_text_feed(buf[k]);
 }
}

/** e_run_text_on_readable - The loop: the program wrote or closed its output. */
static void e_run_text_on_readable(int fd, void *data)
{
 (void)data;
 if (e_run_text_drain(fd))
  wpe_fd_del(fd);
}

/**
 * e_run_text_serve_key - Stop the program on Ctrl-C; the other keys are
 * dropped, since a program with no terminal has no input to give them to.
 */
static void e_run_text_serve_key(void)
{
 int c = wpe_paint_take_key(WPE_PAINT_ANY_KEY);

 if (c == CtrlC || e_run_interrupted)
 {
  e_run_interrupted = 0;
  e_run_interrupt_program();
 }
}

/**
 * e_run_text_loop - Wait for the program with no terminal to end, its
 * output going to Messages as the loop reads it.
 *
 * The same loop as the run on a terminal: each pass waits at most
 * WPE_FD_POLL_MS, then takes the user's keys, so Ctrl-C stops the program
 * here too, and the end is the owner's report.
 */
static void e_run_text_loop(void)
{
 wpe_paint_modal_enter(E_RUN_OWNS_SCREEN);
 while (!e_run_prog.ended)
 {
  wpe_fd_poll(WPE_FD_POLL_MS);
  if (e_run_prog.ended)
   break;
  e_run_text_serve_key();
 }
 wpe_paint_modal_leave();
}

/**
 * e_run_start_text - Start the user's program with its output on a pipe and
 * no input, its errors with its output.
 * @plan: Run's start plan, judged and answered.
 * Return: the editor's end of the pipe, non-blocking, or -1 when the program
 * could not be started.
 */
static int e_run_start_text(WpeStartPlan *plan)
{
 WpeStartIo io;

 memset(&io, 0, sizeof io);
 io.io = WPE_CHILD_IO_PIPES;
 io.group = WPE_CHILD_OWN_SESSION;
 io.stdin_null = 1;
 io.stderr_to_stdout = 1;
 io.side_pty_slave = WPE_CHILD_NO_FD;
 io.tty_fd = WPE_CHILD_NO_FD;
 io.what = E_RUN_WHAT;
 e_run_prog.ended = 0;
 e_run_prog.status = 0;
 if (wpe_startplan_start_words(plan, &io, &e_run_prog.child, e_run_on_end, NULL) != 0)
  return(-1);
 return(e_run_prog.child.out_fd);
}

/**
 * e_run_without_pty - Run the program when no terminal could be made for it:
 * its output goes to Messages line by line, with no input.
 * @plan: Run's start plan; @b: Messages.
 *
 * The program is a child of the editor's owner and its output is read on the
 * loop as it is written, so a program that runs for a while shows its lines
 * as they come, the build and the servers are served meanwhile, and Ctrl-C
 * stops it as on a terminal; no wait of the system's holds the editor or
 * the reaper.
 * Return: the program's exit code, or -1 when it could not be started.
 */
static int e_run_without_pty(WpeStartPlan *plan, BUFFER *b)
{
 int out_fd;
 struct sigaction old_int;

 e_view_append(b, "--- Run output ---");
 e_run_text.b = b;
 e_run_text.len = 0;
 out_fd = e_run_start_text(plan);
 if (out_fd < 0)
  return(-1);
 if (e_fd_watch(out_fd, POLLIN, e_run_text_on_readable, NULL,
                "the program's output") != 0)
 {
  wpe_child_end(&e_run_prog.child, SIGKILL);
  wpe_child_forget(&e_run_prog.child);
  wpe_fd_close(out_fd);
  return(-1);
 }
 e_run_catch_sigint(&old_int);
 e_run_text_loop();
 e_run_release_sigint(&old_int);
 e_run_text_drain(out_fd);   /* all it wrote before it ended is in the pipe */
 if (e_run_text.len > 0)
  e_run_text_flush();
 wpe_fd_close(out_fd);
 return(e_run_status_code(e_run_prog.status));
}

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
 /* The pipes are reset by e_p_make_then once it may build: a debug session
    the user keeps still answers on them. */
 if (!e_run_sh(f))
  return(0);
 if (e_run_src_keep(f))
 {
  e_error("Out of memory", 0, f->fb);
  return(0);
 }
 return(e_p_make_then(f, e_run_after_make, NULL));
}

/* Run's shell line.  What the kind starts is its words "$1" ... "$k" (the
   program; the file an interpreter runs; the java and the class of a VM);
   the user's own Arguments follow as his own shell text (a "< input" of his
   keeps working), a folder's follow as more words, never as shell text. */
#define E_RUN_LINE_ALL     "\"$@\""
#define E_RUN_WORD_REF_ROOM 12      /* room of one "$N" and the blank after it */
#define E_RUN_WORDS_FILE   1        /* an interpreter or a program: one word, the file */
#define E_RUN_WORDS_VM     2        /* a VM: the java and the class */
#define E_RUN_REFUSED_FMT  "not run: the class %s would be read by %s as an option"
#define E_RUN_NONE_FMT     "%s makes no program to run"
#define E_JAVA_NAME        "java"
#define E_SCALA_NAME       "scala"   /* Scala's runner: it puts scala-library on the class path */
#define E_SCALA_LANGUAGE   "Scala"
#define E_RUN_HERE        "."       /* the program's folder when Exedir is empty */
#define E_RUN_NAME         "program" /* the key's name when no language is known */
#define E_RUN_WHAT_ASKED   "program to run"
#define E_RUN_SRC_WINDOW   "the window's file"
#define E_ARGS_KEY         "Arguments"
#define E_ARGS_SOURCE      "./xwperc: Arguments"
#define E_RUN_INTERP_SOURCE "./xwperc: Compiler"
#define E_RUN_PROG_SOURCE  "./xwperc: Exedir, Executable"

static char *e_prog_text_add(char *text, const char *part);

/**
 * e_java_find_in_path - Where @name is found on PATH, as the shell finds it.
 * @name: a command; @out/@n: receive its path.
 *
 * An empty element of PATH means the current folder, as it does for the
 * shell.
 * Return: 1 when an executable file of that name is on PATH, else 0.
 */
static int e_java_find_in_path(const char *name, char *out, size_t n)
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
 * e_java_tool_path - Where a tool is, as it is named.
 * @tool: a command or a path; @out/@n: receive the path.
 * Return: 1 when it names an executable file, else 0.
 */
static int e_java_tool_path(const char *tool, char *out, size_t n)
{
 if (!strchr(tool, DIRC))
  return(e_java_find_in_path(tool, out, n));
 if (snprintf(out, n, "%s", tool) >= (int)n)
  return(0);
 return(access(out, X_OK) == 0);
}

/**
 * e_java_vm_path - The java a class is run with.
 * @beside: the tool the class was made with (javac, scalac) or the
 * debugger (jdb), as a command or a path; NULL for none.
 * @out/@n: receive the java.
 *
 * The java next to that tool: a class made by that JDK's compiler runs
 * there, while the java first on PATH may be another release.  Otherwise
 * java on PATH, else "java".  The tool is an argument, never the debugger
 * in force: at Run time that names whatever debugger the user last chose.
 * Return: 0, or -1 when no path fits @n (@out is then empty).
 */
static int e_vm_path_named(const char *beside, const char *name, char *out, size_t n)
{
 char tool[WPE_PATH_SIZE], real[WPE_PATH_SIZE], *slash;

 if (beside && beside[0] && e_java_tool_path(beside, tool, sizeof tool) &&
     realpath(tool, real) && (slash = strrchr(real, DIRC)))
 {
  slash[1] = '\0';
  /* A path that does not fit is no runner found: PATH is tried next. */
  if (snprintf(out, n, "%s%s", real, name) < (int)n && access(out, X_OK) == 0)
   return(0);
 }
 if (e_java_find_in_path(name, out, n))
  return(0);
 if (snprintf(out, n, "%s", name) >= (int)n)
 {
  out[0] = '\0';
  return(-1);
 }
 return(0);
}

/* The java beside @beside, as above (the debugger's start and Java's Run).
   e_vm_path_named does the same for another runner, as Scala's scala. */
int e_java_vm_path(const char *beside, char *out, size_t n)
{
 return(e_vm_path_named(beside, E_JAVA_NAME, out, n));
}


/**
 * e_java_vm_words - The words of a VM start, added to a plan.
 * @p: the plan; @java: the java; @agent: an option the editor puts between
 * the java and the class (jdb's agent), or NULL; @cls: the class;
 * @from: whose the java and the class are (WPE_FROM_*).
 *
 * Run and the Java debugger start the VM through here, with the class path
 * java reads from the user's own CLASSPATH and the build folder as the
 * working folder.  A class named like an option is no class java could
 * run, and is never handed to it.
 * Return: 0, -1 for such a class (no word is added), -2 out of memory.
 */
int e_java_vm_words(WpeStartPlan *p, const char *java, const char *agent, const char *cls,
                    int from)
{
 if (cls[0] == '-' || cls[0] == '@')
  return(-1);
 if (wpe_startplan_word(p, java, WPE_WORD_PROGRAM, from, NULL) < 0)
  return(-2);
 if (agent && wpe_startplan_word(p, agent, WPE_WORD_OPTION, WPE_FROM_EDITOR, NULL) < 0)
  return(-2);
 if (wpe_startplan_word(p, cls, WPE_WORD_TEXT, from, NULL) < 0)
  return(-2);
 return(0);
}

/* The values one Run takes, and whose they are. */
typedef struct {
 const char *language;
 const struct e_s_prog *entry; /* the entry whose switch and kind it follows   */
 int kind;               /* E_RUN_*: what it starts                        */
 const char *runner;     /* the Compiler: the interpreter, or the tool the
                            VM's java sits beside; NULL when there is none */
 const char *exedir;     /* the folder the program was built into          */
 const char *exe_name;   /* its name (Executable), or NULL for the default */
 const char *args;       /* Arguments                                      */
 int own;                /* 1: the user's own plan, every value his        */
} e_run_values;

/**
 * e_run_vm_name - The command that runs the class a language's build made.
 * @v: the values.
 *
 * java for Java.  A Scala object needs scala-library on the class path, so
 * Scala's own scala runner is used: java on its own fails with
 * NoClassDefFoundError scala/Predef$.
 * Return: the command's name.
 */
static const char *e_run_vm_name(const e_run_values *v)
{
 if (v->language && !strcmp(v->language, E_SCALA_LANGUAGE))
  return(E_SCALA_NAME);
 return(E_JAVA_NAME);
}

/**
 * e_run_from - Whose a value of a Run is.
 * @v: the values; @scope/@key: the key, as e_opt_value_from names it;
 * @value: the value.
 * Return: WPE_FROM_USER for every value of his own plan; else as
 * e_opt_value_from.
 */
static int e_run_from(const e_run_values *v, const char *scope, const char *key,
                      const char *value)
{
 if (v->own || !scope)
  return(WPE_FROM_USER);
 return(e_opt_value_from(scope, key, value));
}

/**
 * e_run_kind_in_force - What Ctrl-F9 starts for the open file.
 * @p: the language's entry.
 *
 * A project's Run starts the project's program, whatever its compiler
 * style says; a file's Run starts what its language's entry says.
 * Return: E_RUN_*.
 */
static int e_run_kind_in_force(const struct e_s_prog *p)
{
 if (e__project)
  return(E_RUN_PROGRAM);
 return(e_run_kind_of(p));
}

/**
 * e_run_values_of - The values a Run takes from a language's entry.
 * @v: filled in; @p: the entry, or NULL for a user who has none for the
 * language (the open file's entry then gives the kind and the style, and
 * nothing else of it is his); @exedir/@args: the output folder and the
 * Arguments; @own: 1 when they are the user's own.
 */
static void e_run_values_of(e_run_values *v, const struct e_s_prog *p, const char *exedir,
                            const char *args, int own)
{
 memset(v, 0, sizeof *v);
 v->language = e_s_prog.language;
 v->entry = p ? p : &e_s_prog;
 v->kind = e_run_kind_in_force(v->entry);
 if (p)
 {
  v->runner = p->compiler;
  v->exe_name = p->exe_name;
 }
 v->exedir = exedir;
 v->args = args;
 v->own = own;
}

/**
 * e_run_program_placed - The program the build made, as the build names it.
 * @f: the source window; @v: the values; @out/@sz: receive it.
 *
 * The owner's name for a single file (under Exedir, an absolute Executable
 * where the link put it) or the project's program: the file F9 wrote, so
 * Ctrl-F9 starts the file the build made and no other.
 * Return: 0, or -1 when it does not fit.
 */
static int e_run_program_placed(FENSTER *f, const e_run_values *v, char *out, size_t sz)
{
 WpeProgramSource s;

 if (e__project)
  return(wpe_path_copy(out, sz, e_prj_program()));
 e_prog_source_of(&s, f, v->entry, v->exe_name, v->exedir);
 return(wpe_progname_program(out, sz, &s));
}

/**
 * e_run_vm_class - The class a VM runs: the name of what the build made.
 * @f: the source window; @v: the values; @out/@sz: receive it.
 *
 * The same name the Java debugger starts: the file's name without its
 * extension, never placed under Exedir, since the compiler wrote the class
 * where it ran.
 * Return: 0, or -1 when it does not fit.
 */
static int e_run_vm_class(FENSTER *f, const e_run_values *v, char *out, size_t sz)
{
 WpeProgramSource s;

 e_prog_source_of(&s, f, v->entry, v->exe_name, v->exedir);
 return(wpe_progname_program_name(out, sz, &s));
}

/**
 * e_run_self_linked - Does the compiler write the program itself?
 * @f: the source window; @v: the values.
 *
 * Then the build never writes the language's Executable, and a folder that
 * sets one names a file the folder may ship.
 * Return: 1 when it does.
 */
static int e_run_self_linked(FENSTER *f, const e_run_values *v)
{
 WpeProgramSource s;

 e_prog_source_of(&s, f, v->entry, v->exe_name, v->exedir);
 return(s.self_linked);
}

/**
 * e_run_program_path - The program Run starts, as a path.
 * @f: the source window; @v: the values; @out/@sz: receive it.
 *
 * The program the build made, with a folder in front when its name has none,
 * so the shell never looks the name up on PATH, and through the operand rule,
 * so a name starting with '-' is only ever that file.  A path the build
 * already placed (under Exedir, or absolute) is the word as it is.
 * Return: 0, or -1 when it does not fit.
 */
static int e_run_program_path(FENSTER *f, const e_run_values *v, char *out, size_t sz)
{
 char placed[WPE_PATH_SIZE];

 if (e_run_program_placed(f, v, placed, sizeof placed))
  return(-1);
 if (strchr(placed, DIRC))
  return(wpe_path_copy(out, sz, placed));
 return(wpe_startplan_operand_in(out, sz, E_RUN_HERE, placed));
}

/**
 * e_run_word_count - How many words the editor puts before the user's text.
 * @kind: E_RUN_*.
 * Return: the file an interpreter runs or the program, 1; the java and the
 * class of a VM, 2.
 */
static int e_run_word_count(int kind)
{
 if (kind == E_RUN_VM)
  return(E_RUN_WORDS_VM);
 return(E_RUN_WORDS_FILE);
}

/**
 * e_run_line_words - The shell's references to the first @k words of a Run.
 * @k: how many.
 * Return: the malloc'd text "$1" "$2" ... "$k", sized from @k; NULL out of
 * memory.
 */
static char *e_run_line_words(int k)
{
 char *text = MALLOC((size_t)k * E_RUN_WORD_REF_ROOM + 1);
 size_t used = 0;
 int i;

 if (!text)
  return(NULL);
 text[0] = '\0';
 for (i = 1; i <= k; i++)
 {
  if (i > 1)
   used += (size_t)sprintf(text + used, " \"$%d\"", i);
  else
   used += (size_t)sprintf(text + used, "\"$%d\"", i);
 }
 return(text);
}

/**
 * e_run_line - The shell line of a Run.
 * @v: the values; @args_as_text: 1 when the user's own Arguments follow as
 * his shell text.
 *
 * An interpreter's line starts with the interpreter, which is the plan's
 * own text; every other kind's words are the plan's words.
 * Return: the malloc'd line, or NULL out of memory.
 */
static char *e_run_line(const e_run_values *v, int args_as_text)
{
 char *words = NULL, *line;
 const char *first = E_RUN_LINE_ALL;
 size_t size;

 if (args_as_text)
 {
  if (!(words = e_run_line_words(e_run_word_count(v->kind))))
   return(NULL);
  first = words;
 }
 size = strlen(first) + 1;
 if (v->kind == E_RUN_INTERPRETER && v->runner)
  size += strlen(v->runner) + 1;
 if (args_as_text)
  size += strlen(v->args) + 1;
 if (!(line = MALLOC(size)))
 {
  FREE(words);
  return(NULL);
 }
 line[0] = '\0';
 if (v->kind == E_RUN_INTERPRETER && v->runner)
 {
  strcat(line, v->runner);
  strcat(line, " ");
 }
 strcat(line, first);
 if (args_as_text)
 {
  strcat(line, " ");
  strcat(line, v->args);
 }
 FREE(words);
 return(line);
}

/** The keys of a language's entry that decide what Run starts. */
#define E_RUN_KEY_RUN        "Run"
#define E_RUN_KEY_SWITCH     "CompilerSwitch"
#define E_RUN_KEY_COMPILER   "Compiler"
#define E_RUN_KEY_EXECUTABLE "Executable"
#define E_RUN_SWITCH_ROOM    16
#define E_RUN_CHOICE_ROOM    (WPE_PATH_SIZE + 32)

/**
 * e_run_kind_from - Whose the kind of a Run is.
 * @v: the values.
 * Return: WPE_FROM_FOLDER when a folder's xwperc named it, else
 * WPE_FROM_USER.
 */
static int e_run_kind_from(const e_run_values *v)
{
 return(e_run_from(v, v->language, E_RUN_KEY_RUN, e_run_kind_name(v->kind)));
}

/**
 * e_run_interp_from - Whose the interpreter's shell line is.
 * @v: the values.
 *
 * The line starts with the Compiler, which the kind turns into the
 * interpreter, so the folder's Compiler or its Run makes it the folder's.
 * Return: WPE_FROM_FOLDER or WPE_FROM_USER.
 */
static int e_run_interp_from(const e_run_values *v)
{
 if (e_run_from(v, v->language, E_RUN_KEY_COMPILER, v->runner) == WPE_FROM_FOLDER)
  return(WPE_FROM_FOLDER);
 return(e_run_kind_from(v));
}

/**
 * e_run_switch_from - Whose the compiler style of a Run is.
 * @v: the values.
 *
 * The style decides whether the compiler writes its own program, so it
 * decides which file a program kind starts.
 * Return: WPE_FROM_FOLDER or WPE_FROM_USER.
 */
static int e_run_switch_from(const e_run_values *v)
{
 char text[E_RUN_SWITCH_ROOM];

 e_prog_switch_text(v->entry, text, sizeof text);
 return(e_run_from(v, v->language, E_RUN_KEY_SWITCH, text));
}

/**
 * e_run_vm_from - Whose the java and the class of a VM run are.
 * @f: the source window; @v: the values.
 *
 * The java sits beside the Compiler, and the class is named by the style
 * and the Executable, so a folder that set any of them chose what starts.
 * Return: WPE_FROM_FOLDER or WPE_FROM_USER.
 */
static int e_run_vm_from(FENSTER *f, const e_run_values *v)
{
 if (e_run_from(v, v->language, E_RUN_KEY_COMPILER, v->runner) == WPE_FROM_FOLDER ||
     e_run_kind_from(v) == WPE_FROM_FOLDER || e_run_switch_from(v) == WPE_FROM_FOLDER)
  return(WPE_FROM_FOLDER);
 if (e_run_self_linked(f, v) &&
     e_run_from(v, v->language, E_RUN_KEY_EXECUTABLE, v->exe_name) == WPE_FROM_FOLDER)
  return(WPE_FROM_FOLDER);
 return(WPE_FROM_USER);
}

/**
 * e_run_note_choice - Say a key a folder set that chose what Run starts.
 * @plan: the plan; @key/@value: the key and its value.
 *
 * It makes the plan ask, and the box names the key and the program it
 * picked: without it a folder's CompilerSwitch or Run would turn Run's
 * program into a file the folder ships and nothing would say so.
 */
static void e_run_note_choice(WpeStartPlan *plan, const char *key, const char *value)
{
 char what[E_RUN_CHOICE_ROOM];

 wpe_startplan_value(plan, E_STARTCMD_XWPERC, key, value, NULL);
 snprintf(what, sizeof what, "%s = %s", key, value);
 wpe_startplan_folder_chose(plan, what);
}

/** A project's compiler style, as its file writes it. */
#define E_RUN_KEY_PRJ_SWITCH "CMPSWTCH"
#define E_RUN_PRJ_OTHER      "other"

/**
 * e_run_note_project_style - Say that a project's style left its program
 * unbuilt.
 * @plan: the plan; @f: the source window; @v: the values.
 *
 * A project of style other (CMPSWTCH=other) links nothing, so the program
 * Run starts is a file the folder holds, never one this build wrote.  When
 * that style is not the user's own, the plan asks and the box names it.
 */
static void e_run_note_project_style(WpeStartPlan *plan, FENSTER *f, const e_run_values *v)
{
 if (e_run_self_linked(f, v) && e_run_switch_from(v) == WPE_FROM_FOLDER)
  e_run_note_choice(plan, E_RUN_KEY_PRJ_SWITCH, E_RUN_PRJ_OTHER);
}

/**
 * e_run_note_choices - Say every key of a folder that chose Run's program.
 * @plan: the plan; @f: the source window; @v: the values.
 *
 * Run (the kind), CompilerSwitch (it makes the compiler write its own
 * program, so the word is another file) and, for a compiler that writes its
 * own program, Executable (the build never writes that file).  A linked
 * program under a folder's Exedir or Executable is not one of them: the
 * link writes that file in this very build.  A project's program is the
 * project's, unless its style links nothing.
 */
static void e_run_note_choices(WpeStartPlan *plan, FENSTER *f, const e_run_values *v)
{
 char text[E_RUN_SWITCH_ROOM];

 if (e__project)
 {
  e_run_note_project_style(plan, f, v);
  return;
 }
 if (e_run_kind_from(v) == WPE_FROM_FOLDER)
  e_run_note_choice(plan, E_RUN_KEY_RUN, e_run_kind_name(v->kind));
 if (v->kind == E_RUN_INTERPRETER)
  return;
 if (e_run_switch_from(v) == WPE_FROM_FOLDER)
 {
  e_prog_switch_text(v->entry, text, sizeof text);
  e_run_note_choice(plan, E_RUN_KEY_SWITCH, text);
 }
 if (v->exe_name && v->exe_name[0] && e_run_self_linked(f, v) &&
     e_run_from(v, v->language, E_RUN_KEY_EXECUTABLE, v->exe_name) == WPE_FROM_FOLDER)
  e_run_note_choice(plan, E_RUN_KEY_EXECUTABLE, v->exe_name);
}

/**
 * e_run_plan_source - The file an interpreter runs: the source, as "$1".
 * @plan: the plan; @f: the source window.
 * Return: 0, or -1 when the path does not fit.
 */
static int e_run_plan_source(WpeStartPlan *plan, FENSTER *f)
{
 char path[WPE_BUILD_PATH_MAX];

 if (wpe_startplan_operand(path, sizeof path, e_run_src_dirct, e_run_src_datnam))
 {
  e_build_path_too_long(f, "source");
  return(-1);
 }
 wpe_startplan_word(plan, path, WPE_WORD_OPERAND, WPE_FROM_USER, E_RUN_SRC_WINDOW);
 wpe_startplan_path(plan, path, WPE_PATH_INPUT, WPE_FROM_USER, E_RUN_SRC_WINDOW, NULL);
 return(0);
}

/**
 * e_run_plan_program - The program the build made: "$1".
 * @plan: the plan; @f: the source window; @v: the values.
 *
 * A folder's Exedir or Executable that chose the program is the folder's,
 * and asks when it puts the program outside the folder.
 * Return: 0, or -1 when the path does not fit.
 */
static int e_run_plan_program(WpeStartPlan *plan, FENSTER *f, const e_run_values *v)
{
 char path[WPE_BUILD_PATH_MAX];
 int from_dir, from_exe, from = WPE_FROM_USER;

 if (e_run_program_path(f, v, path, sizeof path))
 {
  e_build_path_too_long(f, "program");
  return(-1);
 }
 from_dir = e_run_from(v, E_OPT_SCOPE_PROGRAMMING, "Exedir", v->exedir);
 from_exe = e_run_from(v, v->language, E_RUN_KEY_EXECUTABLE, v->exe_name);
 if (from_dir == WPE_FROM_FOLDER || from_exe == WPE_FROM_FOLDER)
  from = WPE_FROM_FOLDER;
 wpe_startplan_word(plan, path, WPE_WORD_OPERAND, from, E_RUN_PROG_SOURCE);
 wpe_startplan_path(plan, path, WPE_PATH_INPUT, from, E_RUN_PROG_SOURCE, NULL);
 if (from_dir == WPE_FROM_FOLDER)
  wpe_startplan_value(plan, E_STARTCMD_XWPERC, "Exedir", v->exedir, NULL);
 if (from_exe == WPE_FROM_FOLDER)
  wpe_startplan_value(plan, E_STARTCMD_XWPERC, E_RUN_KEY_EXECUTABLE, v->exe_name, NULL);
 return(0);
}

/**
 * e_run_vm_parts - The java and the class a VM run starts.
 * @f: the source window; @v: the values; @java/@jsz and @cls/@csz: receive
 * them.
 * Return: 0, or -1 when one does not fit (the user is told which).
 */
static int e_run_vm_parts(FENSTER *f, const e_run_values *v, char *java, size_t jsz,
                          char *cls, size_t csz)
{
 if (e_run_vm_class(f, v, cls, csz))
 {
  e_build_path_too_long(f, "program");
  return(-1);
 }
 if (e_vm_path_named(v->runner, e_run_vm_name(v), java, jsz))
 {
  e_build_path_too_long(f, "program");
  return(-1);
 }
 return(0);
}

/**
 * e_run_plan_vm - The java and the class a build made: "$1" and "$2".
 * @plan: the plan; @f: the source window; @v: the values.
 *
 * The same start the Java debugger makes, in the build's folder, so the
 * class path is the user's own CLASSPATH.  A class named like an option is
 * refused and said, never handed to java.
 * Return: 0, or -1 when nothing may start.
 */
static int e_run_plan_vm(WpeStartPlan *plan, FENSTER *f, const e_run_values *v)
{
 char java[WPE_PATH_SIZE], cls[WPE_BUILD_PATH_MAX], msg[WPE_BUILD_PATH_MAX + 64];
 int rc;

 if (e_run_vm_parts(f, v, java, sizeof java, cls, sizeof cls))
  return(-1);
 rc = e_java_vm_words(plan, java, NULL, cls, e_run_vm_from(f, v));
 if (rc == -1)
 {
  snprintf(msg, sizeof msg, E_RUN_REFUSED_FMT, cls, e_run_vm_name(v));
  e_status_say(f, msg);
 }
 return(rc);
}

/**
 * e_run_plan_first - What Run starts, as the first words of its shell line.
 * @plan: the plan; @f: the source window; @v: the values.
 *
 * By kind: the file an interpreter runs, the program, or the java and the
 * class of a VM.  The keys of a folder's xwperc that chose it are noted.
 * Return: 0, or -1 when it does not fit or may not start.
 */
static int e_run_plan_first(WpeStartPlan *plan, FENSTER *f, const e_run_values *v)
{
 int rc;

 if (v->kind == E_RUN_INTERPRETER)
  rc = e_run_plan_source(plan, f);
 else if (v->kind == E_RUN_VM)
  rc = e_run_plan_vm(plan, f, v);
 else
  rc = e_run_plan_program(plan, f, v);
 if (rc == 0)
  e_run_note_choices(plan, f, v);
 return(rc);
}

/**
 * e_args_plan_words - A folder's Arguments, as words of a plan.
 * @plan: the plan; @args: the value; @role: what the words are to the
 * program (text for Run, options or text for make).
 *
 * Cut at each blank as the build cuts a value, said in the box as "From
 * ./xwperc: Arguments = ...", and asked about.
 */
static void e_args_plan_words(WpeStartPlan *plan, const char *args, int role)
{
 wpe_startplan_words(plan, args, role, WPE_FROM_FOLDER, E_ARGS_SOURCE);
 wpe_startplan_value(plan, E_STARTCMD_XWPERC, E_ARGS_KEY, args, NULL);
}

/**
 * e_run_plan_make - The plan of a Run (Ctrl-F9).
 * @f: the source window; @v: the values, the folder's or the user's own.
 *
 * The program runs in the source's folder, as "$1" of a shell line built
 * from the editor's own words and the user's own text only: the program's
 * path and a folder's Arguments are words of the plan, never shell text.
 * Return: the plan, or NULL when a path does not fit or out of memory.
 */
static WpeStartPlan *e_run_plan_make(FENSTER *f, const e_run_values *v)
{
 const char *name = v->language ? v->language : E_RUN_NAME;
 WpeStartPlan *plan = wpe_startplan_new(WPE_START_RUN, name);
 int from_args = e_run_from(v, E_OPT_SCOPE_PROGRAMMING, E_ARGS_KEY, v->args);
 int from_interp = WPE_FROM_USER;
 int args_as_text = v->args && v->args[0] && from_args == WPE_FROM_USER;
 char *line, *dir;

 if (!plan)
  return(NULL);
 if (e_opt_start_folder())
  wpe_startplan_set_file(plan, E_STARTCMD_XWPERC, e_opt_start_folder());
 dir = e_build_dir(f);
 wpe_startplan_set_cwd(plan, dir ? dir : E_RUN_HERE);
 if (dir)
  FREE(dir);
 if (v->kind == E_RUN_INTERPRETER)
  from_interp = e_run_interp_from(v);
 line = e_run_line(v, args_as_text);
 wpe_startplan_shell(plan, line ? line : "", from_interp, E_RUN_INTERP_SOURCE);
 if (line)
  FREE(line);
 if (e_run_from(v, v->language, E_RUN_KEY_COMPILER, v->runner) == WPE_FROM_FOLDER &&
     v->kind == E_RUN_INTERPRETER)
  wpe_startplan_value(plan, E_STARTCMD_XWPERC, E_RUN_KEY_COMPILER, v->runner, NULL);
 if (e_run_plan_first(plan, f, v))
 {
  wpe_startplan_free(plan);
  return(NULL);
 }
 if (!args_as_text && v->args && v->args[0])
  e_args_plan_words(plan, v->args, WPE_WORD_TEXT);
 plan->honest = 1;
 return(plan);
}

/**
 * e_run_shown - A Run as the box shows it: the interpreter, the program
 * and its arguments, as the user wrote them.
 * @f: the source window; @v: the values.
 * Return: the malloc'd text, or NULL.
 */
static char *e_run_shown(FENSTER *f, const e_run_values *v)
{
 char path[WPE_BUILD_PATH_MAX], java[WPE_PATH_SIZE];
 char *text = NULL;

 if (v->kind == E_RUN_INTERPRETER)
 {
  text = e_prog_text_add(text, v->runner);
  if (wpe_startplan_operand(path, sizeof path, e_run_src_dirct, e_run_src_datnam) == 0)
   text = e_prog_text_add(text, path);
 }
 else if (v->kind == E_RUN_VM)
 {
  if (e_run_vm_parts(f, v, java, sizeof java, path, sizeof path) == 0)
  {
   text = e_prog_text_add(text, java);
   text = e_prog_text_add(text, path);
  }
 }
 else if (e_run_program_path(f, v, path, sizeof path) == 0)
  text = e_prog_text_add(text, path);
 text = e_prog_text_add(text, v->args);
 if (!text)
  text = WpeStrdup("");
 return(text);
}

/**
 * e_run_say_nothing - Say that the language makes no program to run.
 * @f: the source window; e_s_prog holds its language's entry.
 *
 * Ctrl-F9 on a LaTeX file builds the document with F9's own step and then
 * tells the user there is nothing to start, instead of running the build
 * again and saying nothing.
 */
static void e_run_say_nothing(FENSTER *f)
{
 char msg[WPE_PATH_MSG_MAX];

 snprintf(msg, sizeof msg, E_RUN_NONE_FMT, e_s_prog.language ? e_s_prog.language : E_RUN_NAME);
 e_status_say(f, msg);
}

/**
 * e_run_own_plan_offered - May the Run box offer "Use mine"?
 * @f: the source window; @mine: the values of the user's own plan;
 * @has_own: 1 when he has an entry of his own for the language.
 *
 * With an entry of his, his plan starts what his entry says.  With none (a
 * language only the folder's xwperc makes), the kind and the compiler style
 * are the folder's, so "Use mine" is offered only for a program this build
 * links: an interpreter, a class or a program the compiler writes itself
 * would be a file the folder chose, started as if it were his.
 * Return: 1 when it may be offered.
 */
static int e_run_own_plan_offered(FENSTER *f, const e_run_values *mine, int has_own)
{
 if (mine->kind == E_RUN_NONE)
  return(0);
 if (has_own)
  return(1);
 if (mine->kind != E_RUN_PROGRAM)
  return(0);
 if (e__project)
  return(1);
 return(!e_run_self_linked(f, mine));
}

/**
 * e_run_plan_approve - Judge Run's plan, and ask when a start-folder xwperc
 * chose its program, its interpreter or its Arguments.
 * @f: the source window; e_s_prog holds its language's entry.
 *
 * Run runs the folder's own program, which the box says; what it asks
 * about is what the editor puts on the program's line from a folder file.
 * "Use mine" runs the program with the user's own values.
 * Return: the answered plan (the caller frees it), or NULL when nothing runs.
 */
static WpeStartPlan *e_run_plan_approve(FENSTER *f)
{
 struct e_s_prog *own = NULL;
 char *shown, *own_shown = NULL;
 E_STARTPLAN_SHOW show;
 e_run_values v;
 WpeStartPlan *plan;
 int verdict;

 e_run_values_of(&v, &e_s_prog, e_prog.exedir, e_prog.arguments, 0);
 if (v.kind == E_RUN_NONE)
 {
  e_run_say_nothing(f);
  return(NULL);
 }
 if (!(plan = e_run_plan_make(f, &v)))
  return(NULL);
 shown = e_run_shown(f, &v);
 if (e_opt_start_folder())
 {
  if (e_s_prog.language)
   own = e_opt_own_language(e_s_prog.language);
  e_run_values_of(&v, own, e_opt_own_value("Exedir"), e_opt_own_value(E_ARGS_KEY), 1);
  if (e_run_own_plan_offered(f, &v, own != NULL))
  {
   wpe_startplan_set_own(plan, e_run_plan_make(f, &v));
   own_shown = e_run_shown(f, &v);
  }
 }
 show.who = E_STARTCMD_XWPERC_WHO;
 show.what = E_RUN_WHAT_ASKED;
 show.shown = shown ? shown : "";
 show.own = own_shown;
 verdict = e_startplan_approve(f, plan, &show);
 if (shown) FREE(shown);
 if (own_shown) FREE(own_shown);
 if (verdict == WPE_STARTCMD_CANCEL || !wpe_startplan_running(plan))
 {
  wpe_startplan_free(plan);
  return(NULL);
 }
 return(plan);
}

/**
 * e_run_program - Run the program that was just built, with its output in
 * Messages and on the user screen (Alt-F5).
 * @cn: the desktop.
 *
 * The program runs in its own folder (the source's), so it finds and writes
 * its data files there, not wherever the editor was started.
 */
static int e_run_program(ECNT *cn)
{
 BUFFER *b;
 FENSTER *mf, *f;
 char estr[256];
 WpeStartPlan *plan;
 int ret, i;

 WpeMouseChangeShape(WpeWorkingShape);
 f = e_p_source_window(cn);
#ifdef DEBUGGER
 if (e_d_swtch > 0)
  e_d_quit(f);
#endif
 if (!(plan = e_run_plan_approve(f)))
 {
  WpeMouseRestoreShape();
  return(0);
 }
 for (i = cn->mxedt; i > 0 && strcmp(cn->f[i]->datnam, "Messages"); i--)
  ;
 if (i <= 0 && e_edit(cn, "Messages") != 0)
 {
  wpe_startplan_free(plan);
  WpeMouseRestoreShape();       /* no Messages: the output has nowhere to go */
  return(0);
 }
 if (i <= 0)
  i = cn->mxedt;
 mf = cn->f[i];
 b = mf->b;

 if (b->bf[b->mxlines-1].len != 0)
  e_view_add_row(b);
 ret = e_run_with_pty(plan, b, mf);
 if (ret < 0)
  ret = e_run_without_pty(plan, b);
 wpe_startplan_free(plan);
#ifdef NCURSES
 clearok(stdscr, TRUE);
#endif
 e_repaint_desk(cn->f[cn->mxedt]);

 e_view_append(b, "--- End output ---");
 sprintf(estr, e_p_msg[ERR_RETCODE], ret);
 e_view_append(b, estr);

 e_buffer_cursor_set(b, b->mxlines - 1, b->b.x);
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
 const char *ext = wpe_progname_extension(name);

 if (ext[0])
  return(ext);
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

/* The room wpe_startplan_operand may add in front of a path that would read
   as an option or a response file: "./". */
#define E_OPERAND_PREFIX_ROOM 2

/* The plan of the build that runs (F9, Alt-C, a project's build), from its
   judge to its last step; NULL between builds.  Every step of the build
   starts through it (e_exec_planned) and removes its stale outputs through
   it. */
static WpeStartPlan *e_build_plan;

/**
 * e_build_plan_drop - Forget the last build's plan, as a new build opens.
 */
static void e_build_plan_drop(void)
{
 wpe_startplan_free(e_build_plan);
 e_build_plan = NULL;
}

/**
 * e_build_remove_stale - Remove a stale output before its step rebuilds it.
 * @f: the window; @path: the output.
 *
 * Only through the plan: an output outside the folder is left in place
 * (the compiler writes it anyway) and the status row says so.
 */
static void e_build_remove_stale(FENSTER *f, const char *path)
{
 if (wpe_startplan_remove(e_build_plan, path) == WPE_STARTPLAN_NOT_REMOVED)
  e_startplan_say_not_removed(f, e_build_plan, path);
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
 char msg[WPE_PATH_MSG_MAX];

 snprintf(msg, sizeof msg, WPE_PATH_TOO_LONG_FMT, what, WPE_PATH_MAX);
 e_message(0, msg, f);
}

/* The option that names the program a link writes, in a shown command. */
#define E_PROG_OUTPUT_OPTION "-o"
/* Room for what the question calls a language's compiler. */
#define E_PROG_WHAT_MAX 160
#define E_PROG_WHAT_FMT "%s compiler"

/**
 * e_prog_text_add - Append one part of a command, a space before it.
 * @text: the command so far (malloc'd, may be NULL); @part: the part.
 * Return: the longer command; an empty part leaves it as it is.
 */
static char *e_prog_text_add(char *text, const char *part)
{
 size_t had, len;
 char *grown;

 if (!part || !*part)
  return(text);
 had = text ? strlen(text) : 0;
 len = strlen(part);
 if (!(grown = REALLOC(text, had + len + 2)))
  return(text);
 if (had)
  grown[had++] = ' ';
 memcpy(grown + had, part, len + 1);
 return(grown);
}

/**
 * e_prog_command_text - A language's compile and link line, as one text.
 * @p: the language's entry.
 *
 * The program, its options, the link's libraries and the program the link
 * writes, in the order the build hands them over; the source file is left
 * out.  The options are part of it because they can load code into the
 * compiler (-fplugin, -B) as surely as the program name can name one.
 * Return: the malloc'd text, or NULL when the entry names nothing.
 */
char *e_prog_command_text(const struct e_s_prog *p)
{
 char *text = NULL;

 if (!p)
  return(NULL);
 text = e_prog_text_add(text, p->compiler);
 text = e_prog_text_add(text, p->comp_str);
 text = e_prog_text_add(text, p->libraries);
 if (p->exe_name && p->exe_name[0])
 {
  text = e_prog_text_add(text, E_PROG_OUTPUT_OPTION);
  text = e_prog_text_add(text, p->exe_name);
 }
 if (!text)
  text = WpeStrdup("");
 return(text);
}

/**
 * e_prog_take_own - Put the user's own compile line in force for a language.
 * @own: his entry.
 *
 * After "Use mine" (or a remembered one) his line replaces the folder's in
 * the language's entry, so every later compile, link and run of this
 * session uses it, whichever path copies the entry.
 */
static void e_prog_take_own(const struct e_s_prog *own)
{
 int i;

 for (i = 0; i < e_prog.num; i++)
 {
  struct e_s_prog *p = e_prog.comp[i];
  if (p == own || WpeStrccmp(p->language, own->language) != 0)
   continue;
  if (p->compiler) FREE(p->compiler);
  if (p->comp_str) FREE(p->comp_str);
  if (p->libraries) FREE(p->libraries);
  if (p->exe_name) FREE(p->exe_name);
  p->compiler = WpeStrdup(own->compiler);
  p->comp_str = WpeStrdup(own->comp_str);
  p->libraries = WpeStrdup(own->libraries);
  p->exe_name = WpeStrdup(own->exe_name);
 }
 if (e_s_prog.compiler) FREE(e_s_prog.compiler);
 if (e_s_prog.comp_str) FREE(e_s_prog.comp_str);
 if (e_s_prog.libraries) FREE(e_s_prog.libraries);
 if (e_s_prog.exe_name) FREE(e_s_prog.exe_name);
 e_s_prog.compiler = WpeStrdup(own->compiler);
 e_s_prog.comp_str = WpeStrdup(own->comp_str);
 e_s_prog.libraries = WpeStrdup(own->libraries);
 e_s_prog.exe_name = WpeStrdup(own->exe_name);
}

/**
 * e_prog_approve_compile - Ask before the compile line a start-folder xwperc names runs.
 * @f: the window the build was asked from; e_s_prog holds the language's
 * entry for the file about to be compiled.
 *
 * The user's own line, the built-in one and a line answered for in this
 * folder run as they are; any other line is shown first.  "Use mine" puts
 * the user's own line in force.
 * Return: 0 to go on with e_s_prog, -1 when the user cancelled.
 */
int e_prog_approve_compile(FENSTER *f)
{
 char key[WPE_STARTCMD_KEY_MAX], what[E_PROG_WHAT_MAX];
 struct e_s_prog *own;
 char *text, *own_text;
 E_STARTCMD c;
 int verdict;

 if (!e_opt_start_folder() || !e_s_prog.language)
  return(0);
 own = e_opt_own_language(e_s_prog.language);
 text = e_prog_command_text(&e_s_prog);
 own_text = e_prog_command_text(own);
 snprintf(key, sizeof key, "%s%s", WPE_STARTCMD_KEY_COMPILER, e_s_prog.language);
 snprintf(what, sizeof what, E_PROG_WHAT_FMT, e_s_prog.language);
 c.folder = e_opt_start_folder();
 c.source = E_STARTCMD_XWPERC;
 c.who = E_STARTCMD_XWPERC_WHO;
 c.key = key;
 c.what = what;
 c.text = text;
 c.own = own_text;
 verdict = e_startcmd_approve(f, &c);
 if (verdict == WPE_STARTCMD_USE_OWN && own)
  e_prog_take_own(own);
 if (text) FREE(text);
 if (own_text) FREE(own_text);
 if (verdict == WPE_STARTCMD_CANCEL)
  return(-1);
 if (verdict == WPE_STARTCMD_USE_OWN && !own)
  return(-1);              /* a language only the folder knows: none of his to run */
 return(0);
}

/* How the box and the plan name a start-folder xwperc's values. */
#define E_BUILD_SRC_FMT "./xwperc: %s"
#define E_BUILD_SRC_MAX 128
/* What the plan calls the window's own file, which the user opened. */
#define E_BUILD_SRC_WINDOW "the window's file"
/* The literal options a build adds. */
#define E_BUILD_OPT_COMPILE "-c"
#define E_BUILD_OPT_OUTPUT  "-o"

/* The values one single-file build takes, and whose they are. */
typedef struct {
 const char *language;
 const char *compiler, *comp_str, *libraries, *exe_name;
 const char *exedir;
 int own;            /* 1: the user's own plan, every value his          */
} e_build_values;

/**
 * e_build_values_of - The values of a language's entry, for a plan.
 * @v: filled in; @p: the entry; @exedir: the output folder; @own: 1 when
 * they are the user's own.
 */
static void e_build_values_of(e_build_values *v, const struct e_s_prog *p,
                              const char *exedir, int own)
{
 v->language = e_s_prog.language;
 v->compiler = p->compiler;
 v->comp_str = p->comp_str;
 v->libraries = p->libraries;
 v->exe_name = p->exe_name;
 v->exedir = exedir;
 v->own = own;
}

/**
 * e_build_from - Whose a value of a build is.
 * @v: the values; @scope/@key: the key, as e_opt_value_from names it;
 * @value: the value.
 * Return: WPE_FROM_USER for every value of his own plan; else as
 * e_opt_value_from.
 */
static int e_build_from(const e_build_values *v, const char *scope, const char *key,
                        const char *value)
{
 if (v->own)
  return(WPE_FROM_USER);
 return(e_opt_value_from(scope, key, value));
}

/**
 * e_build_plan_value - A folder's value of a build, said in the box.
 * @plan: the plan; @key: the key; @value: the value; @from: whose it is.
 */
static void e_build_plan_value(WpeStartPlan *plan, const char *key, const char *value,
                               int from)
{
 if (from == WPE_FROM_FOLDER)
  wpe_startplan_value(plan, E_STARTCMD_XWPERC, key, value, NULL);
}

/**
 * e_build_plan_options - Add an option string of a build, word by word.
 * @plan: the plan; @v: the values; @key: "CompilerOptions" or "Libraries";
 * @value: the string.
 *
 * The user's own options are options; a folder's are text the compiler
 * reads as it likes, which is what the box asks about.
 */
static void e_build_plan_options(WpeStartPlan *plan, const e_build_values *v,
                                 const char *key, const char *value)
{
 char source[E_BUILD_SRC_MAX];
 int from = e_build_from(v, v->language, key, value);

 snprintf(source, sizeof source, E_BUILD_SRC_FMT, key);
 if (from == WPE_FROM_FOLDER)
  wpe_startplan_words(plan, value, WPE_WORD_TEXT, from, source);
 else
  wpe_startplan_words(plan, value, WPE_WORD_OPTION, from, source);
 e_build_plan_value(plan, key, value, from);
}

/**
 * e_build_plan_words - The words of a file's compile and link.
 * @plan: the plan; @v: the values.
 */
static void e_build_plan_words(WpeStartPlan *plan, const e_build_values *v)
{
 char source[E_BUILD_SRC_MAX];
 int from = e_build_from(v, v->language, "Compiler", v->compiler);
 const char *compiler = v->compiler;

 if (!compiler)
  compiler = "";
 snprintf(source, sizeof source, E_BUILD_SRC_FMT, "Compiler");
 wpe_startplan_word(plan, compiler, WPE_WORD_PROGRAM, from, source);
 e_build_plan_value(plan, "Compiler", v->compiler, from);
 e_build_plan_options(plan, v, "CompilerOptions", v->comp_str);
 e_build_plan_options(plan, v, "Libraries", v->libraries);
 wpe_startplan_word(plan, E_BUILD_OPT_COMPILE, WPE_WORD_OPTION, WPE_FROM_EDITOR, NULL);
 wpe_startplan_word(plan, E_BUILD_OPT_OUTPUT, WPE_WORD_OPTION, WPE_FROM_EDITOR, NULL);
}

/**
 * e_comp_operands - The source and the object of a file's compile.
 * @f: the file's window; @exedir: the output folder; @fstr/@ostr: receive
 * them; @sz: the room of each.
 *
 * The compile and the build's plan both take their paths from here, so
 * the compile names exactly the paths the plan judged.  The object is the
 * owner's: the file's name without its extension, under Exedir, so an
 * Exedir with a dot in it never changes the object's name.
 * Return: 0, -1 when the source does not fit, -2 when the object does not.
 */
static int e_comp_operands(FENSTER *f, const char *exedir, char *fstr, char *ostr, size_t sz)
{
 WpeProgramSource s;

 if (wpe_startplan_operand(fstr, sz, f->dirct, f->datnam))
  return(-1);
 e_prog_source_of(&s, f, &e_s_prog, NULL, exedir);
 if (wpe_progname_object(ostr, sz, &s))
  return(-2);
 return(0);
}

/**
 * e_comp_step_begin - Remember what the compile of the open file is about to
 * write.
 * @f: the file's window; @ostr: its object.
 *
 * The object; and, for a compiler that writes its own program (javac, fpc,
 * rustc), the program too, since that compile is the whole build.
 */
static void e_comp_step_begin(FENSTER *f, const char *ostr)
{
 WpeProgramSource s;
 char estr[WPE_BUILD_PATH_MAX], lostr[WPE_BUILD_PATH_MAX];

 e_prog_source_of(&s, f, &e_s_prog, e_s_prog.exe_name, e_prog.exedir);
 if (s.self_linked &&
     !e_p_link_operands_of(f, e_s_prog.exe_name, e_prog.exedir, estr, lostr, sizeof estr))
 {
  e_built_step_begin(ostr, estr);
  return;
 }
 e_built_step_begin(ostr, NULL);
}

/**
 * e_build_plan_paths - The paths a file's compile and link touch.
 * @plan: the plan; @f: the file's window; @v: the values.
 *
 * The source (the user's own file), the object the compile writes and
 * removes when it is stale, and the program the link writes: a folder's
 * Exedir or Executable that puts them outside the folder is what the box
 * shows under "Writes outside this folder".
 * Return: 0, or -1 when a path does not fit.
 */
static int e_build_plan_paths(WpeStartPlan *plan, FENSTER *f, const e_build_values *v)
{
 char fstr[WPE_BUILD_PATH_MAX], ostr[WPE_BUILD_PATH_MAX];
 char estr[WPE_BUILD_PATH_MAX], lostr[WPE_BUILD_PATH_MAX];
 int from_dir = e_build_from(v, E_OPT_SCOPE_PROGRAMMING, "Exedir", v->exedir);
 int from_exe = e_build_from(v, v->language, "Executable", v->exe_name);
 int from_prog = WPE_FROM_USER;

 if (from_dir == WPE_FROM_FOLDER || from_exe == WPE_FROM_FOLDER)
  from_prog = WPE_FROM_FOLDER;
 if (e_comp_operands(f, v->exedir, fstr, ostr, sizeof fstr))
  return(-1);
 wpe_startplan_path(plan, fstr, WPE_PATH_INPUT, WPE_FROM_USER, E_BUILD_SRC_WINDOW, NULL);
 wpe_startplan_path(plan, ostr, WPE_PATH_WRITE, from_dir, "./xwperc: Exedir", NULL);
 wpe_startplan_mark_output(plan, WPE_OUTPUT_OBJECT);
 wpe_startplan_path(plan, ostr, WPE_PATH_REMOVE, from_dir, "./xwperc: Exedir", NULL);
 wpe_startplan_mark_output(plan, WPE_OUTPUT_OBJECT);
 if (e_p_link_operands_of(f, v->exe_name, v->exedir, estr, lostr, sizeof estr))
  return(-1);
 wpe_startplan_path(plan, estr, WPE_PATH_WRITE, from_prog, "./xwperc: Exedir, Executable", NULL);
 wpe_startplan_mark_output(plan, WPE_OUTPUT_PRODUCT);
 if (strcmp(lostr, ostr) != 0)
 {
  wpe_startplan_path(plan, lostr, WPE_PATH_WRITE, from_dir, "./xwperc: Exedir", NULL);
  wpe_startplan_mark_output(plan, WPE_OUTPUT_OBJECT);
 }
 e_build_plan_value(plan, "Exedir", v->exedir, from_dir);
 e_build_plan_value(plan, "Executable", v->exe_name, from_exe);
 return(0);
}

/**
 * e_build_plan_make - The plan of a file's compile and link (F9, Alt-C).
 * @f: the file's window; @v: the values, the folder's or the user's own.
 *
 * The children run in the file's own folder; the values a start-folder
 * xwperc set are judged against that xwperc's folder.
 * Return: the plan, or NULL when a path does not fit or out of memory.
 */
static WpeStartPlan *e_build_plan_make(FENSTER *f, const e_build_values *v)
{
 WpeStartPlan *plan = wpe_startplan_new(WPE_START_BUILD, v->language);
 char *dir;

 if (!plan)
  return(NULL);
 if (e_opt_start_folder())
  wpe_startplan_set_file(plan, E_STARTCMD_XWPERC, e_opt_start_folder());
 if ((dir = e_build_dir(f)) != NULL)
 {
  wpe_startplan_set_cwd(plan, dir);
  FREE(dir);
 }
 e_build_plan_words(plan, v);
 if (e_build_plan_paths(plan, f, v))
 {
  wpe_startplan_free(plan);
  return(NULL);
 }
 return(plan);
}

/**
 * e_build_take_own_exedir - Put the user's own output folder in force.
 *
 * After "Use mine" his Exedir replaces the folder's for this session, so the
 * build writes where his own plan, the one he chose, says.
 */
static void e_build_take_own_exedir(void)
{
 const char *own = e_opt_own_value("Exedir");

 if (e_prog.exedir && strcmp(e_prog.exedir, own) == 0)
  return;
 if (e_prog.exedir) FREE(e_prog.exedir);
 e_prog.exedir = WpeStrdup(own);
}

/**
 * e_build_plan_approve - Judge the plan of a file's build, and ask when a
 * start-folder xwperc decided something the user has not seen.
 * @f: the file's window; e_s_prog holds its language's entry.
 *
 * One plan for the compile and the link: the compiler, its options, and
 * where the object and the program go.  The user's own values run as they
 * are; a folder's compiler or options, or an output folder outside the
 * folder, is shown first.  "Use mine" puts his own line and his own output
 * folder in force.
 * Return: 0 to go on with e_s_prog, -1 when the user cancelled or nothing
 * may run.
 */
static int e_build_plan_approve(FENSTER *f)
{
 struct e_s_prog *own = NULL;
 char what[E_PROG_WHAT_MAX];
 char *shown, *own_text = NULL;
 E_STARTPLAN_SHOW show;
 e_build_values v;
 int verdict;

 if (e_s_prog.language)
  own = e_opt_own_language(e_s_prog.language);
 e_build_plan_drop();
 e_build_values_of(&v, &e_s_prog, e_prog.exedir, 0);
 if (!(e_build_plan = e_build_plan_make(f, &v)))
  return(-1);
 if (own)
 {
  e_build_values_of(&v, own, e_opt_own_value("Exedir"), 1);
  wpe_startplan_set_own(e_build_plan, e_build_plan_make(f, &v));
  own_text = e_prog_command_text(own);
 }
 shown = e_prog_command_text(&e_s_prog);
 what[0] = '\0';
 if (e_s_prog.language)
  snprintf(what, sizeof what, E_PROG_WHAT_FMT, e_s_prog.language);
 show.who = E_STARTCMD_XWPERC_WHO;
 show.what = what;
 show.shown = "";
 if (shown)
  show.shown = shown;
 show.own = own_text;
 verdict = e_startplan_approve(f, e_build_plan, &show);
 if (verdict == WPE_STARTCMD_USE_OWN && own)
 {
  e_prog_take_own(own);
  e_build_take_own_exedir();
 }
 if (shown) FREE(shown);
 if (own_text) FREE(own_text);
 if (verdict == WPE_STARTCMD_CANCEL)
  return(-1);
 if (verdict == WPE_STARTCMD_USE_OWN && !own)
  return(-1);
 return(0);
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
 char cwd_saved[WPE_PATH_SIZE];
 int i, file = -1, len, argc, cwd_chg = 0;
 { const char *tp = getenv("XWPE_UI_TRACE");
   if (tp) { FILE *tf = fopen(tp, "a");
     if (tf) { fprintf(tf, "e_comp ENTER datnam=%s\n",
              f && f->datnam ? f->datnam : "?"); fclose(tf); } } }
 struct stat obuf[1];

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
 /* A path that does not fit is said before anything is asked or run. */
 len = e_comp_operands(f, e_prog.exedir, fstr, ostr, sizeof fstr);
 if (len == -1)
 {
  e_build_path_too_long(f, "source");
  return (-1);
 }
 if (len == -2)
 {
  e_build_path_too_long(f, "object");
  return (-1);
 }
 if (e_build_plan_approve(f))
  return(WPE_ESC);
 /* "Use mine" may have put the user's own output folder in force. */
 if (e_comp_operands(f, e_prog.exedir, fstr, ostr, sizeof fstr))
 {
  e_build_path_too_long(f, "object");
  return (-1);
 }
 e_build_record_begin(f);
 argc = e_make_arg(&arg, e_s_prog.comp_str);
 if (!(e_s_prog.comp_sw & 1))
 {
  /* GNU compilers (gcc, g++, gfortran): add -c flag */
  arg[1] = MALLOC(3);
  strcpy(arg[1], E_BUILD_OPT_COMPILE);
 }
 else
 {
  /* Non-GNU compilers (fpc, javac): no -c, shift args down */
  int k;
  for (k = 1; k < argc; k++)
   arg[k] = arg[k + 1];
  argc--;
 }
 argc = e_add_arg(&arg, fstr, argc, argc);
 e_p_set_compiled_source(fstr);
#ifndef NO_MINUS_C_MINUS_O
 if (!(e_s_prog.comp_sw & 1))
 {
  /* GNU compilers: add -o output.o */
  argc = e_add_arg(&arg, E_BUILD_OPT_OUTPUT, argc, argc);
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
 if (!e_build_current(ostr, NULL, 0, NULL, obuf) || e_check_header(fstr, obuf->st_mtime, cn, 0))
#else
 if (!e_build_current(ostr, f->datnam, 0, NULL, obuf))
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
  e_build_remove_stale(f, ostr);
  e_comp_step_begin(f, ostr);
  if ((file = e_exec_planned(f, arg)) == 0)
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

/* How the build's child ended, as its watch reported it.  Kept apart from
   the step's state, which is reset when the step begins: the end may be
   reported between the fork and that point. */
typedef struct {
 int pid;        /* the child whose end this is, or 0 */
 int ended;      /* its end was reported */
 int status;     /* its wait status, once ended */
} e_build_child_end_t;

static e_build_child_end_t e_build_child_end;
static WpeChild e_build_child;     /* the step's process, while it runs */

#define E_BUILD_WHAT "the build"

static void e_build_child_ended(WpeChild *c, int status, int how, void *data);

/**
 * e_build_forget_previous - A step's process whose end was never reported
 * (a step given up on) is let go before the next one starts, so the new
 * step's end is never taken for the old one's.
 */
static void e_build_forget_previous(void)
{
 if (e_build_child.pid > 0)
  wpe_child_forget(&e_build_child);
}

/**
 * e_build_step_prepare - Make ready for one step of a build to start.
 * @f: the window.
 *
 * A debug session ends first (the step may rebuild its program), and a
 * step given up on earlier is let go, so its end is never taken for this
 * one's.
 */
static void e_build_step_prepare(FENSTER *f)
{
#ifdef DEBUGGER
 if (e_d_swtch > 0)
  e_d_quit(f);
#else
 (void)f;
#endif
 fflush(stdout);
 /* efildes carries the one output pipe; wfildes stays unused, as the
    callers expect. */
 efildes[0] = efildes[1] = wfildes[0] = wfildes[1] = -1;
 e_build_forget_previous();
}

/**
 * e_build_step_started - Note a step that started, or say that it did not.
 * @f: the window; @rc: what the start returned.
 * Return: 1 when it runs, 0 when it did not start.
 */
static int e_build_step_started(FENSTER *f, int rc)
{
 if (rc < 0)
 {
  e_save_pid = 0;
  e_error(e_p_msg[ERR_PROCESS], 0, f->fb);
  return(0);
 }
 e_save_pid = e_build_child.pid;
 e_build_child_end.pid = e_build_child.pid;
 e_build_child_end.ended = 0;
 e_build_child_end.status = 0;
 efildes[0] = e_build_child.out_fd;
 return(1);
}

/**
 * e_build_io - How the children's owner runs one step of a planned build
 * (a compile, the link, ar, ranlib, make).
 * @io: filled in.
 *
 * Its own process group, so Esc ends make together with the compiler it
 * started; stdin from /dev/null, so the command can never take the terminal
 * from the editor; stdout and stderr in one pipe, so the lines reach
 * Messages in the order the build wrote them (a make's echoed command before
 * the compiler's errors), the command line first.
 */
static void e_build_io(WpeStartIo *io)
{
 memset(io, 0, sizeof *io);
 io->io = WPE_CHILD_IO_PIPES;
 io->group = WPE_CHILD_OWN_GROUP;
 io->stdin_null = 1;
 io->stderr_to_stdout = 1;
 io->echo_argv = 1;
 io->exec_failed = e_p_msg[ERR_IN_COMMAND];
 io->what = E_BUILD_WHAT;
}

/**
 * e_exec_planned - Start one step of a planned build (a compile, the link,
 * ar, ranlib) in the background.
 * @f: the window; @argv: the step's command line, NULL-ended.
 *
 * The step starts through the build's plan, in its working folder, and
 * only when every word of its command line is one the plan the user saw
 * holds.
 * Return: 1 when it runs, 0 when it did not start (said in a box).
 */
static int e_exec_planned(FENSTER *f, char **argv)
{
 WpeStartIo io;

 e_build_step_prepare(f);
 e_build_io(&io);
 return(e_build_step_started(f, wpe_startplan_start(e_build_plan, argv, &io, &e_build_child,
                                                    e_build_child_ended, NULL)));
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
 char path[WPE_PATH_SIZE];
 struct stat sb;

 if (e__project || !f || !f->dirct || !f->dirct[0])
  return;
 /* A hint only: a name that does not fit names no Makefile to suggest. */
 if (wpe_path_join(path, sizeof(path), f->dirct, "Makefile") != 0)
  return;
 if (stat(path, &sb) != 0)
 {
  if (wpe_path_join(path, sizeof(path), f->dirct, "makefile") != 0)
   return;
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
 e_view_append(b, acc);
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
 int reaped;            /* the child has been waited for                   */
 int status;            /* its wait status                                 */
 int held;              /* it ended while a box was up                     */
 int is;                /* rows Messages had when the step began           */
 int wait_row;          /* the waiting line is Messages' last row          */
 time_t start;
 int hb_timer;          /* the heartbeat timer, or WPE_TIMER_NONE          */
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
static void e_build_deliver_held(void);
static void e_build_deliver_held_replay(void *data);

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

/**
 * e_status_say - Answer a key on the bottom row.
 * @f: the window the key was pressed in; @text: what to say.
 *
 * For a key that cannot do what it says right now (a second build, a walk
 * with no messages, a row naming a file that is not there): the user reads
 * why at once, where Borland's status line said it, instead of seeing
 * nothing happen.  The next repaint of the hint row puts the key hints
 * back.  A line kept until the next key gives way to this newer one.
 */
void e_status_say(FENSTER *f, const char *text)
{
 if (!f || !f->fb)
  return;
 e_status_drop_held();
 e_status_paint(f->fb, text);
 e_refresh();
}

/**
 * e_status_say_until_key - Say a line on the bottom row and keep it there
 * until the user presses a key.
 * @f: the window on top; @text: what to say.
 *
 * For a line said while the editor goes on working by itself, such as the
 * debugger it picked for the file's language before that debugger starts:
 * the windows the start repaints would otherwise put the key hints back
 * before the user could read it.
 */
void e_status_say_until_key(FENSTER *f, const char *text)
{
 if (!f || !f->fb)
  return;
 e_status_hold(text);
 e_status_paint(f->fb, text);
 e_refresh();
}

/**
 * e_build_messages - The Messages window, if it is open.
 * @cn: the desktop.
 *
 * Looked up afresh on every use: the user may close Messages while the
 * build runs, and its output is then kept until the window is back.
 * Return: the window, or NULL.
 */
FENSTER *e_build_messages(ECNT *cn)
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
 * e_build_drop_last_row - Take the last row out of the Messages buffer.
 * @b: the Messages buffer.
 */
static void e_build_drop_last_row(BUFFER *b)
{
 if (b->mxlines > 0)
  e_buffer_delete_lines(b, b->mxlines - 1, 1, E_BUFFER_STEP_NEW);
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
 e_view_append(mf->b, line);
 g_build.wait_row = 1;
}

/* What the screen's owner calls the Messages rows kept behind a box in its
   trace. */
#define E_MESSAGES_TAIL_WHAT "the Messages rows"

static void e_messages_show_tail_replay(void *cn);

/**
 * e_messages_show_tail - Show the latest Messages rows, now or when nothing
 * is in the way.
 * @cn: the desktop.
 *
 * Messages follows a background task's output (a build, a starting
 * debugger) to its last row; the cursor goes back to the window the user
 * is typing in, so typing while the task runs is undisturbed.  Under a box,
 * a menu or a run the rows wait and are drawn when the editor is its own
 * again.
 */
void e_messages_show_tail(ECNT *cn)
{
 FENSTER *mf = e_build_messages(cn);

 if (!mf)
  return;
 if (!wpe_paint_idle())
 {
  wpe_paint_request(e_messages_show_tail_replay, cn, E_MESSAGES_TAIL_WHAT);
  return;
 }
 e_buffer_cursor_set(mf->b, mf->b->mxlines - 1, 0);
 e_messages_scroll_to_bottom(mf);
 e_schirm(mf, 0);
 if (mf != cn->f[cn->mxedt])
  e_cursor(cn->f[cn->mxedt], 0);
 else
  e_cursor(mf, 0);
 e_refresh();
}

/**
 * e_messages_show_tail_replay - The screen's owner's turn for Messages rows
 * that waited behind a box.
 * @cn: the desktop.
 */
static void e_messages_show_tail_replay(void *cn)
{
 e_messages_show_tail((ECNT *)cn);
}

/* What the screen's owner calls the build's Messages rows in its trace. */
#define E_BUILD_PAINT_WHAT "the build's output"

/**
 * e_build_paint_rows - Draw the build's latest Messages rows.
 * @data: unused.
 */
static void e_build_paint_rows(void *data)
{
 (void)data;
 e_messages_show_tail(g_build.cn);
}

/**
 * e_build_paint - Show the build's latest Messages rows, or when the editor
 * is its own again.
 *
 * Under a dialog, a menu or a run the rows wait; the key loop draws them
 * once, with everything the build wrote meanwhile.
 */
static void e_build_paint(void)
{
 wpe_paint_request(e_build_paint_rows, NULL, E_BUILD_PAINT_WHAT);
}

/**
 * e_build_signal - Send @sig to the build and every process it started.
 * @sig: SIGKILL, on a runaway or when the editor ends.
 *
 * make runs the compiler as its own child; ending make alone would leave
 * the compiler writing into the pipe, so the whole process group is hit.
 * Nothing is sent once the build has ended.
 */
static void e_build_signal(int sig)
{
 wpe_child_signal(&e_build_child, sig);
}

/**
 * e_build_mark_reaped - The build's child has been waited for.
 *
 * Its slot in the owner of the editor's children is free from then on, so
 * a signal that ends the editor never reaches a later process that reused
 * the number.
 */
static void e_build_mark_reaped(void)
{
 g_build.reaped = 1;
}

/**
 * e_build_try_reap - Has the step's child ended?  Take its status if so.
 *
 * Read from what the child's watch reported; nothing here asks waitpid.
 * Return: 1 once the child is gone.
 */
static int e_build_try_reap(void)
{
 if (g_build.reaped)
  return(1);
 if (g_build.pid <= 0)
 {
  e_build_mark_reaped();
  return(1);
 }
 if (e_build_child_end.pid != g_build.pid || !e_build_child_end.ended)
  return(0);
 g_build.status = e_build_child_end.status;
 e_build_mark_reaped();
 return(1);
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
 if (wpe_paint_idle())
 {
  e_build_step_end();
  return;
 }
 g_build.held = 1;
 wpe_paint_request(e_build_deliver_held_replay, NULL, "the build's end");
}

/**
 * e_build_after_streams_closed - Both pipes are at EOF: end the step once
 * the child has ended too.
 *
 * A child closes its pipes just before it exits; when its end was not
 * reported yet, e_build_child_ended ends the step the moment it is.
 */
static void e_build_after_streams_closed(void)
{
 if (e_build_try_reap())
  e_build_finish_or_hold();
}

/**
 * e_build_child_ended - The build's child ended (the children's owner, from
 * the loop).
 * @c: the child; @status: its wait status; @how: WPE_CHILD_ENDED, or
 * WPE_CHILD_ENDED_UNKNOWN for one that did not end even after SIGKILL;
 * @data: unused.
 *
 * The status is kept for the step's verdict, so the user reads the build's
 * own exit code even when many processes ended at the same moment.  When
 * the output already ended, the step ends now.
 */
static void e_build_child_ended(WpeChild *c, int status, int how, void *data)
{
 int pid = e_build_child_end.pid;

 (void)c;
 (void)how;
 (void)data;
 e_build_child_end.ended = 1;
 e_build_child_end.status = status;
 if (!g_build.active || g_build.held || g_build.pid != pid)
  return;
 if (!e_build_streams_open())
  e_build_after_streams_closed();
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
 * e_build_on_tick - The heartbeat: turn the waiting line.
 * @data: unused.
 *
 * It runs while the build is silent too, so the seconds on the waiting line
 * keep counting during a long compile or link that prints nothing.
 */
static void e_build_on_tick(void *data)
{
 (void)data;
 if (!g_build.active || g_build.held)
  return;
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
 * key.
 */
static void e_build_heartbeat_start(void)
{
 g_build.hb_timer = wpe_timer_add(E_BUILD_TICK_NS / WPE_TIMER_NS_PER_MS, 1,
                                  e_build_on_tick, NULL);
 if (g_build.hb_timer < 0)
  g_build.hb_timer = WPE_TIMER_NONE;
}

/**
 * e_build_heartbeat_stop - Stop the waiting line's timer.
 */
static void e_build_heartbeat_stop(void)
{
 wpe_timer_del(g_build.hb_timer);
 g_build.hb_timer = WPE_TIMER_NONE;
}

/**
 * e_build_cancel - Esc while a build runs: end it.
 *
 * The child and everything it started get SIGTERM; if they are still there
 * WPE_CHILD_END_GRACE_MS later, SIGKILL (the children's owner's ladder, on
 * the loop).  The step then ends with "build cancelled" in Messages and no
 * later step runs, so no program is linked or started from a build the
 * user stopped.
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
 wpe_child_end(&e_build_child, SIGTERM);
}

/**
 * e_build_end_at_exit - The editor is leaving: end a build that still runs.
 *
 * The build runs in its own process group, so nothing else ends it when the
 * editor goes; a compiler or an interpreter that loops would be left
 * running, unseen, for good.  It and everything it started are killed and
 * nothing waits for it: the editor is ending, and the killed child is then
 * reaped by init, so a wait could only delay the exit.
 */
void e_build_end_at_exit(void)
{
 if (!g_build.active || g_build.reaped || g_build.pid <= 0)
  return;
 e_build_signal(SIGKILL);
}

/**
 * e_build_deliver_held - End a build step that finished behind a dialog.
 *
 * Run once the editor is back in its key loop, the point that is certainly
 * outside every dialog: the jump to the first error and the next step of
 * the build happen there, never under an open box.
 */
static void e_build_deliver_held(void)
{
 if (!g_build.held)
  return;
 if (!wpe_paint_idle())
  return;
 g_build.held = 0;
 e_build_step_end();
}

/**
 * e_build_deliver_held_replay - The screen's owner's turn for a build that
 * ended behind a dialog.
 * @data: unused.
 */
static void e_build_deliver_held_replay(void *data)
{
 (void)data;
 e_build_deliver_held();
}

/**
 * e_build_watch_stream - Hand one output pipe of the child to the fd loop.
 * @fd: the pipe's read end, or E_BUILD_NO_FD.
 *
 * A pipe the loop cannot watch is closed at once: its lines would never
 * reach Messages, and the child would block on it once it filled.
 * Return: 1 when the pipe was refused, else 0.
 */
static int e_build_watch_stream(int fd)
{
 int flags;

 if (fd == E_BUILD_NO_FD)
  return(0);
 flags = fcntl(fd, F_GETFL, 0);
 fcntl(fd, F_SETFL, flags | O_NONBLOCK);
 if (e_fd_watch(fd, POLLIN, e_build_on_output, NULL, "the build output") == 0)
  return(0);
 e_build_close_stream(fd);
 return(1);
}

/**
 * e_build_give_up_unwatched - End a build whose output cannot be read.
 *
 * The bottom row already says why; the build is then cancelled as Esc would
 * cancel it, so the user is not left with a waiting line that never ends,
 * and no later step links or runs a program from a build nobody saw.
 */
static void e_build_give_up_unwatched(void)
{
 e_build_cancel();
 if (!e_build_streams_open())
  e_build_after_streams_closed();   /* no output callback will come */
}

/**
 * e_build_watch_child - Hand the running child's pipes to the fd loop.
 *
 * From here the function returns to the editor at once; the build goes on
 * in e_build_on_output and e_build_on_tick.
 */
static void e_build_watch_child(void)
{
 int refused;

 g_build.efd = efildes[0];
 g_build.wfd = wfildes[0];
 efildes[0] = wfildes[0] = E_BUILD_NO_FD;
 e_build_heartbeat_start();
 refused = e_build_watch_stream(g_build.efd);
 refused |= e_build_watch_stream(g_build.wfd);
 e_build_wait_line_put();
 e_build_paint();
 if (refused)
  e_build_give_up_unwatched();
}

/**
 * e_build_flush_fragments - The last bytes of each stream, with no final
 * newline, still go to Messages.
 * @b: the Messages buffer.
 */
static void e_build_flush_fragments(BUFFER *b)
{
 if (g_build.eacclen > 0)
  e_view_append(b, g_build.eacc);
 if (g_build.wacclen > 0)
  e_view_append(b, g_build.wacc);
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
 * The child's watch reports its status (e_build_child_ended), so a build
 * that failed is reported as failed even when its last command printed
 * nothing the error parser knows.
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
  e_view_append(b, E_BUILD_CANCELLED_LINE);
  g_build_chain_cancelled = 1;
  g_build_chain_list = E_LIST_NO_JUMP;
  return(WPE_ESC);
 }
 if (g_build.runaway)
  e_view_append(b, E_BUILD_RUNAWAY_LINE);
 if (b->mxlines - g_build.is > 2)
  list = e_make_error_list(mf);
 g_build_chain_list = list;
 g_build_chain_exit = ret;
 if (ret || list)
  return(-1);
 e_view_append(b, E_BUILD_SUCCESS_LINE);
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
 e_buffer_cursor_set(mf->b, mf->b->mxlines - 1, mf->b->b.x);
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
 g_build.efd = g_build.wfd = E_BUILD_NO_FD;
 g_build.hb_timer = WPE_TIMER_NONE;
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
 * e_p_exec - Run the child e_exec_planned started as one step of a build.
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
  e_build_mark_reaped();
  e_build_step_end();
  return(0);
 }
 if (efildes[1] >= 0) close(efildes[1]);
 if (wfildes[1] >= 0) close(wfildes[1]);
 efildes[1] = wfildes[1] = E_BUILD_NO_FD;
 e_build_watch_child();
 return(0);
}

#ifdef DEBUGGER
#define E_BUILD_QUIT_DEBUG_QUESTION \
 "The Debugger is Running\nDo You want to Quit Debugging ?"
#define E_BUILD_ANSWER_YES 'Y'

/**
 * e_build_ends_debug_session - Ask whether a build may end the debug session.
 * @f: the window the build key was pressed in.
 *
 * A build replaces the program being debugged, so, as Turbo C asked to
 * reset the program before a rebuild, F9, Alt-C, Ctrl-F9, Install and
 * Execute Make first ask whether to quit debugging.  Yes ends the session
 * and the build goes on.  No or Esc leaves the session exactly as it was,
 * stopped or running: the question comes before the build claims Messages
 * or the pipes the debugger answers on, so the next stop is still marked
 * and the debugger keys keep working.
 * Return: 0 when the build may go on, -1 when the user kept the session.
 */
static int e_build_ends_debug_session(FENSTER *f)
{
 if (e_d_swtch <= 0)
  return(0);
 if (e_message(1, E_BUILD_QUIT_DEBUG_QUESTION, f) != E_BUILD_ANSWER_YES)
  return(-1);
 e_d_quit(f);
 return(0);
}
#else
/* Without the debugger there is no session for a build to end. */
static int e_build_ends_debug_session(FENSTER *f)
{
 (void)f;
 return(0);
}
#endif

/**
 * e_build_chain_begin - Open a build: F9, Alt-C, Ctrl-F9, Execute Make or
 * the debugger's build.
 * @f: the window the key was pressed in.
 * @done: what runs once the whole build has ended, or NULL; @data: its data.
 *
 * Only one build runs at a time; a second one is refused with a line that
 * says Esc cancels the first.  During a debug session the user is asked
 * first whether to end it, before anything of the build is set up.
 * Return: 0 when the build may start, -1 when another one runs or the user
 * kept the debug session.
 */
static int e_build_chain_begin(FENSTER *f, e_build_cont done, void *data)
{
 if (e_build_busy())
 {
  e_build_refuse(f);
  return(-1);
 }
 if (e_build_ends_debug_session(f))
  return(-1);
 e_build_plan_drop();
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
 * The count Borland showed when a build ended.  A failed build that
 * counted no error (a command that could not run, a recipe that failed
 * with only make's own row) says so with its exit status, so "0 errors"
 * never reads as a success.
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
 e_view_append(b, line);
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
 e_buffer_cursor_set(mf->b, mf->b->mxlines - 1, mf->b->b.x);
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
 e_view_append(mf->b, line);
 FREE(line);
 e_messages_scroll_to_bottom(mf);
 e_build_return_to_source(cn);
 e_build_settle_top(cn);
}

/**
 * e_build_has_error_to_jump_to - Did the failed build leave a row to go to?
 * @result: the build's result.
 *
 * A build can fail with nothing the error walk understands, such as a
 * tool's one-line message with no file and line.  The report then shows
 * Messages itself, with that line and the count, instead of trying a jump
 * that returns at once and leaves the screen as the waiting line left it.
 * Return: 1 when the first error row can be jumped to.
 */
static int e_build_has_error_to_jump_to(int result)
{
 if (!result || g_build_chain_list == E_LIST_NO_JUMP)
  return(0);
 return(err_num > 0);
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
 if (e_build_has_error_to_jump_to(result))
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
 e_buffer_cursor_set(mf->b, row, 0);
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
 int i, bg = 0;
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
 /* a line past the end of the file lands on its last row: the owner clamps */
 e_buffer_cursor_set(b, err_li[n].line - 1, 0);
 if (!err_li[n].srch)
 {
  /* the compiler's column is a screen column: the owner walks tabs,
     control and wide characters and lands on the character that has it */
  e_buffer_cursor_set_col(b, b->b.y, e_err_column_index(n));
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
    e_buffer_cursor_set(b, b->b.y - 1, 0);
    i = b->bf[b->b.y].len+1;
   }
   else
    i++;
  }
/*      else if(err_li[n].x < -1) i++;    */
  e_buffer_cursor_set(b, b->b.y, i + err_li[n].x);
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
 char str[E_ARG_FIELD_MAX + 1];  /* the dialog's field writes back this much */

 if (!e_prog.arguments)
 {
  e_prog.arguments = MALLOC(1);
  e_prog.arguments[0] = '\0';
 }
 /* An xwperc can make the arguments longer than the field: show what fits. */
 snprintf(str, sizeof str, "%s", e_prog.arguments);
 if (e_add_arguments(str, sizeof str, "Arguments", f, 0 , AltA, NULL))
 {
  /* Only a value he changed is his: OK on a folder's value cut to the
     field's room approves nothing. */
  if (strcmp(str, e_prog.arguments) != 0)
   e_opt_value_typed(E_OPT_SCOPE_PROGRAMMING, "Arguments", str);
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
 const char *postfix;

 postfix = wpe_progname_extension(name);
 if (postfix[0])
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
 const char *p = str;     /* read in place: options, libraries and Run arguments
                             come from an xwperc and have no length limit */

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
  cp->run_kind     = E_RUN_PROGRAM;
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
  cp->run_kind     = E_RUN_INTERPRETER;
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

/* The built-in languages in the order e_ini_prog makes them, and what
   Ctrl-F9 starts for each: C, C++, Fortran, Pascal, Java, Python, LaTeX,
   Perl, COBOL, Algol 68 (e_algol68_apply chooses per toolchain), Go, Rust,
   Scala. */
static const int e_ini_run_kinds[] = {
 E_RUN_PROGRAM, E_RUN_PROGRAM, E_RUN_PROGRAM, E_RUN_PROGRAM, E_RUN_VM,
 E_RUN_INTERPRETER, E_RUN_NONE, E_RUN_INTERPRETER, E_RUN_PROGRAM,
 E_RUN_PROGRAM, E_RUN_PROGRAM, E_RUN_PROGRAM, E_RUN_VM
};

/**
 * e_run_kind_of - What Ctrl-F9 starts for a language.
 * @p: the language's entry.
 *
 * Run reads the kind here and nowhere else, so a language that is not built
 * in says what it runs in its own xwperc, and the compiler style
 * (CompilerSwitch) keeps to the build steps and the message format it was
 * made for.
 * Return: E_RUN_*.
 */
int e_run_kind_of(const struct e_s_prog *p)
{
 return(p->run_kind);
}

/** The words an xwperc writes after "Run :", indexed by E_RUN_* - 1. */
static const char *const e_run_kind_names[] = { "interpreter", "program", "vm", "none" };

/**
 * e_run_kind_name - The word an xwperc writes for a kind.
 * @kind: E_RUN_*.
 * Return: "interpreter", "program", "vm" or "none"; "" for UNSET.
 */
const char *e_run_kind_name(int kind)
{
 if (kind < E_RUN_INTERPRETER || kind > E_RUN_NONE)
  return("");
 return(e_run_kind_names[kind - E_RUN_INTERPRETER]);
}

/**
 * e_run_kind_parse - The kind an xwperc's "Run" value means.
 * @name: the value, in any case.
 * Return: E_RUN_*, or E_RUN_KIND_UNSET for a word that names none (the
 * entry then keeps the kind it has).
 */
int e_run_kind_parse(const char *name)
{
 int kind;

 for (kind = E_RUN_INTERPRETER; kind <= E_RUN_NONE; kind++)
  if (WpeStrccmp(e_run_kind_names[kind - E_RUN_INTERPRETER], name) == 0)
   return(kind);
 return(E_RUN_KIND_UNSET);
}

/**
 * e_run_kind_default - The kind of an entry whose xwperc gives no Run.
 * @p: the entry.
 *
 * A compiler of the "other" style used to be run as an interpreter, so an
 * entry a user made before the key existed (a Lua with CompilerSwitch 1)
 * keeps running as one; any other entry runs the program it builds.
 * Return: E_RUN_INTERPRETER or E_RUN_PROGRAM.
 */
int e_run_kind_default(const struct e_s_prog *p)
{
 if (p->comp_sw & 1)
  return(E_RUN_INTERPRETER);
 return(E_RUN_PROGRAM);
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
  e_prog.comp[i]->run_kind = e_ini_run_kinds[i];
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
 out->run_kind = in->run_kind;
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
  while (wpe_binding_action(e_data_eingabe(f->ed),
                            wpe_binding_style(f->ed->edopt),
                            WPE_CTX_LIST) != WPE_ACT_CLOSE_LIST)
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

static char *e_prj_line_text(char *cmp, char *flags, char *ldflags);
static void e_prj_note_typed(int his);
static int e_prj_fields_are_his(W_OPTSTR *o);

int e_project_options(FENSTER *f)
{
 int ret, his;
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
 e_add_wrstr(4, 8, 22, 8, 36, WPE_PATH_FIELD_MAX, 0, AltE, "Executable:", e_s_prog.exe_name, NULL, o);
 e_add_wrstr(4, 10, 22, 10, 36, WPE_PATH_FIELD_MAX, 2, AltB, "LiBrary:", e_prj_library(), NULL, o);
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
  his = e_prj_fields_are_his(o);
  if (e_s_prog.compiler) FREE(e_s_prog.compiler);
  e_s_prog.compiler = WpeStrdup(o->wstr[0]->txt);
  if (e_s_prog.comp_str) FREE(e_s_prog.comp_str);
  e_s_prog.comp_str = WpeStrdup(o->wstr[1]->txt);
  if (e_s_prog.libraries) FREE(e_s_prog.libraries);
  e_s_prog.libraries = WpeStrdup(o->wstr[2]->txt);
  e_prj_note_typed(his);
  if (e_s_prog.exe_name) FREE(e_s_prog.exe_name);
  e_s_prog.exe_name = WpeStrdup(o->wstr[3]->txt);
  if (e_s_prog.start_symbol) FREE(e_s_prog.start_symbol);
  e_s_prog.start_symbol = WpeStrdup(o->wstr[5]->txt);
  if (e_s_prog.intstr) FREE(e_s_prog.intstr);
  e_s_prog.intstr = WpeValueToString(o->wstr[6]->txt);
  e_prj_library_set(o->wstr[4]->txt);
  e_s_prog.comp_sw = o->pstr[0]->num;
  e_wrt_prj_fl(f);
 }
 freeostr(o);
 if (f->ed->mxedt > 0)
  e_ed_rahmen(f, 1);
 return(0);
}

/* The room of the dialogs' command fields (e_add_wrstr's wmx). */
#define E_PROG_FIELD_ROOM     128
#define E_PROG_PRJ_FIELD_ROOM 256
/* The fields a compile line is made of: compiler, options, libraries,
   program; a project's line has the first three. */
#define E_PROG_LINE_FIELDS 4
#define E_PRJ_LINE_FIELDS  3

/* One command field of a dialog: as it opened, as the user left it, the
   user's own value for it (or NULL) and the field's room. */
typedef struct {
 const char *before;
 const char *after;
 const char *own;
 size_t room;
} e_prog_field;

/**
 * e_prog_fields_are_his - Did the user write this command line himself?
 * @fld/@n: the line's fields.
 *
 * Only then does OK count it as his own.  A field he left alone counts only
 * when it already held his own value; a field whose value was cut to fit the
 * dialog never counts, so a folder's command that does not fit is never
 * approved through the dialog; and at least one field must have changed.
 * Return: 1 when the line is his.
 */
static int e_prog_fields_are_his(const e_prog_field *fld, int n)
{
 int i, changed = 0;

 for (i = 0; i < n; i++)
 {
  const char *before = fld[i].before ? fld[i].before : "";
  if (strlen(before) > fld[i].room)
   return(0);
  if (strcmp(before, fld[i].after) != 0)
   changed = 1;
  else if (!fld[i].own || strcmp(before, fld[i].own) != 0)
   return(0);
 }
 return(changed);
}

/**
 * e_prog_fields_of - The compile-line fields of the Compiler dialog.
 * @fld: receives four fields; @o: the dialog, after OK; @own: the user's
 * own entry for the language, or NULL.
 */
static void e_prog_fields_of(e_prog_field fld[4], W_OPTSTR *o, const struct e_s_prog *own)
{
 fld[0].before = e_s_prog.compiler;  fld[0].after = o->wstr[1]->txt;
 fld[1].before = e_s_prog.comp_str;  fld[1].after = o->wstr[2]->txt;
 fld[2].before = e_s_prog.libraries; fld[2].after = o->wstr[3]->txt;
 fld[3].before = e_s_prog.exe_name;  fld[3].after = o->wstr[4]->txt;
 fld[0].own = own ? own->compiler : NULL;
 fld[1].own = own ? own->comp_str : NULL;
 fld[2].own = own ? own->libraries : NULL;
 fld[3].own = own ? own->exe_name : NULL;
 fld[0].room = fld[1].room = fld[2].room = E_PROG_FIELD_ROOM;
 fld[3].room = WPE_PATH_FIELD_MAX;   /* the program is a path */
}

/**
 * e_prog_note_typed_compile - A compile line the user set in Options is his own.
 * @his: e_prog_fields_are_his for the dialog's fields.
 *
 * When he wrote it, the line in e_s_prog now is what he typed: building
 * does not ask about it in this folder, now or after the next start.
 */
static void e_prog_note_typed_compile(int his)
{
 char key[WPE_STARTCMD_KEY_MAX];
 char *after;

 if (!his || !e_s_prog.language)
  return;
 after = e_prog_command_text(&e_s_prog);
 snprintf(key, sizeof key, "%s%s", WPE_STARTCMD_KEY_COMPILER, e_s_prog.language);
 if (after)
  e_startcmd_typed(e_opt_start_folder(), key, after);
 if (after) FREE(after);
 e_opt_value_typed(e_s_prog.language, "Compiler", e_s_prog.compiler);
 e_opt_value_typed(e_s_prog.language, "CompilerOptions", e_s_prog.comp_str);
 e_opt_value_typed(e_s_prog.language, "Libraries", e_s_prog.libraries);
 e_opt_value_typed(e_s_prog.language, "Executable", e_s_prog.exe_name);
}

/**
 * e_prog_postfix_list - A language's file extensions as one line, for its field.
 * @out/@sz: receive them, space separated, as many as fit.
 */
static void e_prog_postfix_list(char *out, size_t sz)
{
 int i, j = WpeExpArrayGetSize(e_s_prog.filepostfix);
 size_t used = 0;

 out[0] = '\0';
 for (i = 0; i < j; i++)
 {
  size_t len = strlen(e_s_prog.filepostfix[i]);
  if (i)
   len++;
  if (used + len >= sz)
   break;
  if (i)
   snprintf(out + used, sz - used, " %s", e_s_prog.filepostfix[i]);
  else
   snprintf(out + used, sz - used, "%s", e_s_prog.filepostfix[i]);
  used += len;
 }
}

/**
 * e_prog_postfix_word - One file extension of the File-Postfix field, on the
 * heap.
 * @list: where the word starts in the field's text; @len: its bytes.
 *
 * The field is a line of words; each becomes a language's extension.
 * Return: the word, or NULL when there is no memory.
 */
static char *e_prog_postfix_word(const char *list, size_t len)
{
 char *word = (char *)WpeMalloc(len + 1);

 if (!word)
  return(NULL);
 memcpy(word, list, len);
 word[len] = '\0';
 return(word);
}

int e_run_c_options(FENSTER *f)
{
 int i, j, ret, his;
 W_OPTSTR *o = e_init_opt_kst(f);
 char filepostfix[E_PROG_FIELD_ROOM + 1];
 char *newpostfix;
 char *messagestring;
 e_prog_field fld[E_PROG_LINE_FIELDS];

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
 e_add_wrstr(4, 10, 22, 10, 36, WPE_PATH_FIELD_MAX, 0, AltE, "Executable:", e_s_prog.exe_name, NULL, o);
 j = WpeExpArrayGetSize(e_s_prog.filepostfix);
 e_prog_postfix_list(filepostfix, sizeof filepostfix);
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
  e_prog_fields_of(fld, o, e_opt_own_language(e_s_prog.language));
  his = e_prog_fields_are_his(fld, E_PROG_LINE_FIELDS);
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
   newpostfix = e_prog_postfix_word(o->wstr[5]->txt + i, j - i);
   WpeExpArrayAdd((void **)&e_s_prog.filepostfix, &newpostfix);
   i = j - 1;
  }
  if (e_s_prog.start_symbol) FREE(e_s_prog.start_symbol);
  e_s_prog.start_symbol = WpeStrdup(o->wstr[6]->txt);
  if (e_s_prog.intstr) FREE(e_s_prog.intstr);
  e_s_prog.intstr = WpeValueToString(o->wstr[7]->txt);
  e_s_prog.comp_sw = o->pstr[0]->num;
  e_prog_note_typed_compile(his);
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
   /* A new language is what its own style says, not what the language the
      dialog was opened from runs. */
   e_prog.comp[e_prog.num-1]->run_kind = e_run_kind_default(e_prog.comp[e_prog.num-1]);
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
  if(e_add_arguments(tmp, sizeof tmp, "Remove Compiler", f, 0, AltR, NULL))
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
 char str[WPE_PATH_SIZE];  /* the dialog's field writes back this much */

 if (!e_prog.project)
 {
  e_prog.project = MALLOC(1);
  e_prog.project[0] = '\0';
 }
 /* A project the system could not open is not offered for editing: a cut
    name would be written back in its place. */
 if (wpe_path_copy(str, sizeof str, e_prog.project) != 0)
 {
  e_input_limit_say(WPE_PATH_FIELD_MAX);
  return(WPE_ESC);
 }
 if (e_add_arguments(str, sizeof str, "Project", f, 0, AltP, NULL))
 {
  /* Only a value he changed is his: OK on a folder's value cut to the
     field's room approves nothing. */
  if (strcmp(str, e_prog.project) != 0)
   e_opt_value_typed(E_OPT_SCOPE_PROGRAMMING, "Project", str);
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
   e_make_prj_opt() fills in the compiler defaults (gcc, -g).  Its program is
   named after the project by the owner, as a source's is: "my.app.prj"
   builds "my.app".  A name too long for the room would leave EXENAME empty,
   never cut. */
static void e_write_default_prj(char *path)
{
 FILE *fp = fopen(path, "w");
 char exe[WPE_NAME_SIZE];

 if (!fp)
  return;
 wpe_progname_project_default(exe, sizeof exe, path);

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
 e_opt_value_typed(E_OPT_SCOPE_PROGRAMMING, "Project", e_prog.project);
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
 e_opt_value_typed(E_OPT_SCOPE_PROGRAMMING, "Project", e_prog.project);
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
  fw->nf = wpe_listsel_sentinel_row(fw->df);
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
 /* The child prints its command line itself (e_exec_planned). */
 e_sys_ini();
 file = e_exec_planned(f, ar_arg);
 e_sys_end();
 if (!file)
 {
  e_build_continue(done, f, -1, data);
  return(-1);
 }
 e_p_exec(f, pic, done, data);
 return(0);
}

/* The shell command e_shell_run is waiting for, and how it ended.  One at a
   time: the wait takes no key, so no second command can be asked for. */
static struct {
 WpeChild child;
 int ended;
 int status;
} e_shell;

#define E_SHELL_WHAT    "the command"
/* What e_shell_run returns when the command could not be run. */
#define E_SHELL_FAILED  (-1)

/**
 * e_shell_on_end - The owner of the editor's children reports that the
 * command e_shell_run waits for has ended.
 * @status: its wait status; @how: WPE_CHILD_ENDED, or
 * WPE_CHILD_ENDED_UNKNOWN when the status is lost.
 */
static void e_shell_on_end(WpeChild *c, int status, int how, void *data)
{
 (void)c;
 (void)data;
 e_shell.status = status;
 if (how != WPE_CHILD_ENDED)
  e_shell.status = E_SHELL_FAILED;
 e_shell.ended = 1;
}

/** e_shell_has_ended - The wait's condition: the command is over. */
static int e_shell_has_ended(void)
{
 return(e_shell.ended);
}

/**
 * e_shell_io - How a command the user watches runs: in the editor's own
 * terminal or window group, with the signals a shell run from a terminal
 * has.
 * @io: filled in.
 */
static void e_shell_io(WpeStartIo *io)
{
 memset(io, 0, sizeof *io);
 io->io = WPE_CHILD_IO_INHERIT;
 io->group = WPE_CHILD_EDITORS_GROUP;
 io->side_pty_slave = WPE_CHILD_NO_FD;
 io->tty_fd = WPE_CHILD_NO_FD;
 io->what = E_SHELL_WHAT;
}

/**
 * e_shell_start - Start a command the user watches, through its plan.
 * @plan: the plan, judged and answered; @argv: the one step to run (an
 * install line), or NULL for the plan's own words behind its carrier.
 * Return: 0, or -1 when it could not be started.
 */
static int e_shell_start(WpeStartPlan *plan, char *const argv[])
{
 WpeStartIo io;

 e_shell_io(&io);
 e_shell.ended = 0;
 e_shell.status = 0;
 if (argv)
  return(wpe_startplan_start(plan, argv, &io, &e_shell.child, e_shell_on_end, NULL));
 return(wpe_startplan_start_carried(plan, &io, &e_shell.child, e_shell_on_end, NULL));
}

/**
 * e_shell_serve_display - Answer what the editor's own window was asked
 * meanwhile (a repaint after another window covered it, a new size).
 *
 * A graphical editor whose window took no event until its command ended
 * would show a blank or half-drawn window to anyone who moved another one
 * over it.  The console has no such event.
 */
static void e_shell_serve_display(void)
{
 (*e_u_serve_display)();
}

/**
 * e_shell_keyboard_fd - The descriptor the loop must not wake for while the
 * command runs.
 *
 * On the console the command has the keyboard: the keys the user types are
 * its, and a loop that woke for them with no one in the editor to read them
 * would spin.  A graphical editor's keys arrive on its window's connection,
 * which the editor goes on serving.
 * Return: the console's keyboard, or -1.
 */
static int e_shell_keyboard_fd(void)
{
 if (WpeIsXwin())
  return(-1);
 return(e_t_keyboard_fd());
}

/**
 * e_shell_wait - Wait on the loop until the command has ended.
 *
 * Everything else the editor does on the loop goes on meanwhile, and the
 * window's events are answered.
 */
static void e_shell_wait(void)
{
 wpe_fd_wait_serving(e_shell_has_ended, E_SHELL_WHAT, e_shell_keyboard_fd(),
                     e_shell_serve_display, WPE_WAIT_FOREVER);
}

/**
 * e_shell_run - Run a command the user watches and wait for it, like
 * system(), but on the editor's loop, from its start plan, and with the
 * command's signals at their defaults.
 * @plan: the command's plan, judged and answered; @argv: the one step to
 * run, or NULL for the plan's own words.
 *
 * system() hands the command the editor's ignored SIGINT and SIGPIPE, which
 * a shell can never undo: a script or program run from the editor then could
 * not be stopped with Ctrl-C and would keep writing into a closed pipe.  The
 * wait is the loop's, not a call that blocks the editor: the build's output,
 * the servers and the window's repaints are served meanwhile.  Ctrl-C and
 * Ctrl-backslash typed for the command are the command's, not the editor's.
 * No text is ever handed to a shell here: a file name goes to the command as
 * a word of its plan.
 * Return: the wait status, or -1 when the command could not be started.
 */
int e_shell_run(struct WpeStartPlan *plan, char *const argv[])
{
 struct sigaction ign, old_int, old_quit;
 int started;

 memset(&ign, 0, sizeof ign);
 ign.sa_handler = SIG_IGN;
 sigemptyset(&ign.sa_mask);
 sigaction(SIGINT, &ign, &old_int);
 sigaction(SIGQUIT, &ign, &old_quit);
 started = e_shell_start(plan, argv);
 if (started == 0)
  e_shell_wait();
 sigaction(SIGINT, &old_int, NULL);
 sigaction(SIGQUIT, &old_quit, NULL);
 if (started != 0)
  return(E_SHELL_FAILED);
 if (!e_shell.ended)
 {
  wpe_child_forget(&e_shell.child);
  return(E_SHELL_FAILED);
 }
 return(e_shell.status);
}

/**
 * e_system - Run a command the user watches on the console, the editor's
 * screen stepped aside, and wait for Return when it ends.
 * @plan: the command's start plan, judged and answered; @cn: the desktop.
 * Return: the command's wait status, or -1 when it could not be started.
 */
int e_system(struct WpeStartPlan *plan, ECNT *cn)
{
#if MOUSE
 int g[4];
#endif
 int ret;
 PIC *outp;

#if  MOUSE
 g[0] = 2;
 fk_mouse(g);
#endif
 outp = e_open_view(0,0,MAXSCOL-1,MAXSLNS-1,cn->fb->ws,1);
 fk_locate(0,0);
 fk_cursor(1);
 (*e_u_s_sys_ini)();
 ret = e_shell_run(plan, NULL);
 if (!WpeIsXwin())
 {
  printf("%s",e_msg[ERR_HITCR]);
  fflush(stdout);
  e_t_wait_for_enter_on_raw_terminal();
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

/* Lines said from a callback while a box owns the screen, kept for when it
   closes: how many, and how long a kept line may be (a longer one is cut). */
#define E_PMSG_HELD_MAX  32
#define E_PMSG_LINE_MAX  1024
#define E_PMSG_HELD_WHAT "a message for Messages"

static struct {
 char *line[E_PMSG_HELD_MAX];
 int   sw[E_PMSG_HELD_MAX];
 ECNT *cn;
 int   count;
 int   lost;
} e_pmsg_held;

static int e_p_message_now(char *str, FENSTER *f, int sw);

/**
 * e_p_message_replay - Put the lines kept behind a box into Messages, in the
 * order they were said.
 * @data: unused.
 *
 * The screen's owner runs this once no box owns the screen.  Lines that did
 * not fit the keeping are counted and named in one last line, so the user
 * knows the list is not whole.
 */
static void e_p_message_replay(void *data)
{
 ECNT *cn = e_pmsg_held.cn;
 FENSTER *top;
 char tail[E_PMSG_LINE_MAX];
 int i, count = e_pmsg_held.count, lost = e_pmsg_held.lost;
 char *lines[E_PMSG_HELD_MAX];
 int sw[E_PMSG_HELD_MAX];

 (void)data;
 if (!cn || cn->mxedt < 1)
  return;
 for (i = 0; i < count; i++)
 {
  lines[i] = e_pmsg_held.line[i];
  sw[i] = e_pmsg_held.sw[i];
 }
 e_pmsg_held.count = 0;
 e_pmsg_held.lost = 0;
 top = cn->f[cn->mxedt];
 for (i = 0; i < count; i++)
 {
  e_p_message_now(lines[i], top, sw[i]);
  FREE(lines[i]);
 }
 if (lost > 0)
 {
  snprintf(tail, sizeof tail, "(%d more messages were not shown)", lost);
  e_p_message_now(tail, top, 0);
 }
}

/**
 * e_p_message_hold - Keep a line said from a callback until no box owns the
 * screen.
 * @str: the line; @f: a window of the desktop; @sw: as e_d_p_message.
 *
 * What a language server or a debugger tells while a dialog or a menu is up
 * is not lost and not drawn under it: it is in Messages when the box
 * closes.  A full keeping drops the newest lines and counts them.
 * Return: 0.
 */
static int e_p_message_hold(char *str, FENSTER *f, int sw)
{
 char *copy;
 size_t room = strlen(str) + 1;

 if (e_pmsg_held.count >= E_PMSG_HELD_MAX)
 {
  e_pmsg_held.lost++;
  return(0);
 }
 if (room > E_PMSG_LINE_MAX)
  room = E_PMSG_LINE_MAX;
 copy = MALLOC(room);
 snprintf(copy, room, "%s", str);
 e_pmsg_held.cn = f->ed;
 e_pmsg_held.line[e_pmsg_held.count] = copy;
 e_pmsg_held.sw[e_pmsg_held.count] = sw;
 e_pmsg_held.count++;
 wpe_paint_request(e_p_message_replay, NULL, E_PMSG_HELD_WHAT);
 return(0);
}

/**
 * e_d_p_message - Add a line to Messages, raising it if it is not open.
 * @str: the line; @f: a window of the desktop; @sw: 1 to bring Messages
 * forward, 0 to write in the background.
 *
 * Said from the user's own key it is drawn at once.  Said from a callback
 * (a language server's news, a debugger's word) while a box, a menu or a
 * run owns the screen, it waits for the screen's owner, in order.
 * Return: 0, or -1 when Messages could not be opened.
 */
int e_d_p_message(char *str, FENSTER *f, int sw)
{
 if (str[0] == '\0' || str[0] == '\n')
  return(0);
 if (wpe_paint_in_callback() && !wpe_paint_idle())
  return(e_p_message_hold(str, f, sw));
 if (e_pmsg_held.count > 0 && wpe_paint_idle())
  e_p_message_replay(NULL);
 return(e_p_message_now(str, f, sw));
}

static int e_p_message_now(char *str, FENSTER *f, int sw)
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

 e_view_append_wrapped(b, str);

 e_buffer_cursor_set(b, b->mxlines - 1, b->b.x);
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
  e_buffer_mark_view(cn->f[i]->b);
 }
 wf = cn->f[i];
 b = wf->b;
 e_view_append_wrapped(b, str);
 e_buffer_cursor_set(b, b->mxlines - 1, b->b.x);
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

 e_buffer_cursor_set_col(b, row, e_mouse.x - f->a.x + f->s->c.x - 1);
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
  e_buffer_cursor_set_col(b, e_mouse.y-f->a.y+s->c.y-1,
                          e_mouse.x-f->a.x+s->c.x-1);
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

/* What Execute Make starts, and what its plan and question call it. */
#define E_MAKE_PROGRAM    "make"
#define E_MAKE_WHAT_ASKED "make arguments"

/**
 * e_make_plan_make - The plan of Execute Make.
 * @f: the active window; @args: the Arguments make gets; @own: 1 when they
 * are the user's own.
 *
 * make runs in the active file's folder (the project's, when one is open)
 * and runs the folder's Makefile, which the box says.  The user's own
 * Arguments are options of his; a folder's are its text, shown and asked
 * about once.
 * Return: the plan, or NULL out of memory.
 */
static WpeStartPlan *e_make_plan_make(FENSTER *f, const char *args, int own)
{
 WpeStartPlan *plan = wpe_startplan_new(WPE_START_MAKE, E_MAKE_PROGRAM);
 int from = WPE_FROM_USER;
 char *dir;

 if (!plan)
  return(NULL);
 if (e_opt_start_folder())
  wpe_startplan_set_file(plan, E_STARTCMD_XWPERC, e_opt_start_folder());
 dir = e_build_dir(f);
 wpe_startplan_set_cwd(plan, dir ? dir : E_RUN_HERE);
 if (dir)
  FREE(dir);
 wpe_startplan_word(plan, E_MAKE_PROGRAM, WPE_WORD_PROGRAM, WPE_FROM_EDITOR, NULL);
 if (!own)
  from = e_opt_value_from(E_OPT_SCOPE_PROGRAMMING, E_ARGS_KEY, args);
 if (from == WPE_FROM_FOLDER)
  e_args_plan_words(plan, args, WPE_WORD_TEXT);
 else
  wpe_startplan_words(plan, args, WPE_WORD_OPTION, WPE_FROM_USER, E_ARGS_SOURCE);
 plan->honest = 1;
 return(plan);
}

/**
 * e_make_shown - make and its Arguments, as the box shows them.
 * @args: the Arguments.
 * Return: the malloc'd text.
 */
static char *e_make_shown(const char *args)
{
 char *text = e_prog_text_add(NULL, E_MAKE_PROGRAM);

 text = e_prog_text_add(text, args);
 if (!text)
  text = WpeStrdup(E_MAKE_PROGRAM);
 return(text);
}

/**
 * e_make_plan_approve - Judge Execute Make's plan, and ask when a
 * start-folder xwperc set the Arguments make gets.
 * @f: the active window.
 *
 * The plan becomes the build's plan, so make starts through it like every
 * step of a build.  "Use mine" runs make with the user's own Arguments.
 * Return: 0 to run make, -1 when nothing runs.
 */
static int e_make_plan_approve(FENSTER *f)
{
 const char *own_args = e_opt_own_value(E_ARGS_KEY);
 char *shown, *own_shown = NULL;
 E_STARTPLAN_SHOW show;
 int verdict;

 e_build_plan_drop();
 if (!(e_build_plan = e_make_plan_make(f, e_prog.arguments, 0)))
  return(-1);
 if (e_opt_start_folder())
 {
  wpe_startplan_set_own(e_build_plan, e_make_plan_make(f, own_args, 1));
  own_shown = e_make_shown(own_args);
 }
 shown = e_make_shown(e_prog.arguments);
 show.who = E_STARTCMD_XWPERC_WHO;
 show.what = E_MAKE_WHAT_ASKED;
 show.shown = shown ? shown : "";
 show.own = own_shown;
 verdict = e_startplan_approve(f, e_build_plan, &show);
 if (shown) FREE(shown);
 if (own_shown) FREE(own_shown);
 if (verdict == WPE_STARTCMD_CANCEL || !wpe_startplan_running(e_build_plan))
  return(-1);
 return(0);
}

/**
 * e_make_start - Start make through the build's plan, with the words of
 * the plan that runs (the user's own after "Use mine").
 * @f: Messages.
 * Return: 1 when it runs, 0 when it did not start.
 */
static int e_make_start(FENSTER *f)
{
 char **argv = wpe_startplan_argv(e_build_plan);
 int file;

 if (!argv)
  return(0);
 file = e_exec_planned(f, argv);
 free(argv);
 return(file);
}

int e_exec_make(FENSTER *f)
{
 ECNT *cn = f->ed;
 char saved_cwd[WPE_PATH_SIZE];
 int i, file, cwd_changed;

 if (e_build_chain_begin(f, NULL, NULL))
  return(-1);
 if (e_make_plan_approve(f))
 {
  e_build_chain_end(f, WPE_ESC, NULL);
  return(WPE_ESC);
 }
 WpeMouseChangeShape(WpeWorkingShape);
 efildes[0] = efildes[1] = -1;
 wfildes[0] = wfildes[1] = -1;
 /* Capture the active file's directory BEFORE e_new_message makes Messages the
    top window -- after that cn->f[cn->mxedt]->dirct is the Messages dir.  make
    itself runs in the plan's folder, the same one; the editor's own folder
    follows it so Messages finds the files make names. */
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
 e_s_prog.compiler = WpeStrdup(E_MAKE_PROGRAM);
 if ((file = e_make_start(f)) == 0)
 {
  e_sys_end();
  if (cwd_changed)
   e_build_popd(saved_cwd);
  WpeMouseRestoreShape();
  e_build_chain_end(f, WPE_ESC, NULL);
  return(WPE_ESC);
 }
 e_sys_end();
 e_p_exec(f, NULL, e_build_chain_end, NULL);
 if (cwd_changed)
  e_build_popd(saved_cwd);
 WpeMouseRestoreShape();
 return(0);
}

/* What a shell script's Run is called in its key and its question. */
#define E_RUN_SH_SUFFIX     ".sh"
#define E_RUN_SH_SUFFIX_LEN 3
#define E_RUN_SH_NAME       "sh"
#define E_RUN_SH_WHAT_ASKED "script's arguments"

/**
 * e_run_sh_plan_make - The plan of Run on a shell script.
 * @f: the script's window; @args: the Arguments; @own: 1 when they are the
 * user's own.
 *
 * The script runs in its own folder as "./name.sh", the "$1" of a shell
 * line, so a script named "a';touch x;'.sh" is only ever that file; the
 * user's own Arguments follow as his shell text, a folder's as words, shown
 * and asked about once.
 * Return: the plan, or NULL when the name does not fit or out of memory.
 */
static WpeStartPlan *e_run_sh_plan_make(FENSTER *f, const char *args, int own)
{
 WpeStartPlan *plan = wpe_startplan_new(WPE_START_SCRIPT, E_RUN_SH_NAME);
 e_run_values v;
 char path[WPE_BUILD_PATH_MAX];
 int from = WPE_FROM_USER, args_as_text;
 char *line;

 if (!plan)
  return(NULL);
 if (e_opt_start_folder())
  wpe_startplan_set_file(plan, E_STARTCMD_XWPERC, e_opt_start_folder());
 wpe_startplan_set_cwd(plan, f->dirct && f->dirct[0] ? f->dirct : E_RUN_HERE);
 if (!own)
  from = e_opt_value_from(E_OPT_SCOPE_PROGRAMMING, E_ARGS_KEY, args);
 args_as_text = args && args[0] && from == WPE_FROM_USER;
 memset(&v, 0, sizeof v);
 v.args = args;
 line = e_run_line(&v, args_as_text);
 wpe_startplan_shell(plan, line ? line : "", WPE_FROM_USER, NULL);
 if (line)
  FREE(line);
 if (wpe_startplan_operand(path, sizeof path, E_RUN_HERE, f->datnam))
 {
  wpe_startplan_free(plan);
  return(NULL);
 }
 wpe_startplan_word(plan, path, WPE_WORD_OPERAND, WPE_FROM_USER, E_RUN_SRC_WINDOW);
 wpe_startplan_path(plan, path, WPE_PATH_INPUT, WPE_FROM_USER, E_RUN_SRC_WINDOW, NULL);
 if (!args_as_text && args && args[0])
  e_args_plan_words(plan, args, WPE_WORD_TEXT);
 plan->honest = 1;
 return(plan);
}

/**
 * e_run_sh_plan_approve - Judge the plan of Run on a shell script, and ask
 * when a start-folder xwperc set its Arguments.
 * @f: the script's window.
 * Return: the answered plan (the caller frees it), or NULL when nothing runs.
 */
static WpeStartPlan *e_run_sh_plan_approve(FENSTER *f)
{
 const char *own_args = e_opt_own_value(E_ARGS_KEY);
 char *shown, *own_shown = NULL;
 E_STARTPLAN_SHOW show;
 WpeStartPlan *plan;
 int verdict;

 if (!(plan = e_run_sh_plan_make(f, e_prog.arguments, 0)))
  return(NULL);
 shown = e_prog_text_add(e_prog_text_add(NULL, f->datnam), e_prog.arguments);
 if (e_opt_start_folder())
 {
  wpe_startplan_set_own(plan, e_run_sh_plan_make(f, own_args, 1));
  own_shown = e_prog_text_add(e_prog_text_add(NULL, f->datnam), own_args);
 }
 show.who = E_STARTCMD_XWPERC_WHO;
 show.what = E_RUN_SH_WHAT_ASKED;
 show.shown = shown ? shown : "";
 show.own = own_shown;
 verdict = e_startplan_approve(f, plan, &show);
 if (shown) FREE(shown);
 if (own_shown) FREE(own_shown);
 if (verdict == WPE_STARTCMD_CANCEL || !wpe_startplan_running(plan))
 {
  wpe_startplan_free(plan);
  return(NULL);
 }
 return(plan);
}

/**
 * e_run_sh_make_runnable - Make the script executable on disk.
 * @f: the script's window.
 *
 * The window's executable bit reaches the disk only on save, so a freshly
 * opened unmodified script would not run as "./name.sh".
 */
static void e_run_sh_make_runnable(FENSTER *f)
{
 char *full = e_mkfilename(f->dirct, f->datnam);
 struct stat sb;

 if (!full)
  return;
 if (!stat(full, &sb))
  chmod(full, sb.st_mode | S_IXUSR);
 FREE(full);
}

/**
 * e_run_sh - Ctrl-F9 on a shell script: run it, in its own folder, where
 * the user watches it.
 * @f: the window.
 * Return: 1 when the window is no script (Run goes on to build), else 0.
 */
int e_run_sh(FENSTER *f)
{
 int len = strlen(f->datnam);
 WpeStartPlan *plan;

 if (len < E_RUN_SH_SUFFIX_LEN || strcmp(f->datnam + len - E_RUN_SH_SUFFIX_LEN, E_RUN_SH_SUFFIX))
  return(1);

 WpeMouseChangeShape(WpeWorkingShape);
 f->filemode |= 0100;
 if (f->save)
  e_save(f);
 e_run_sh_make_runnable(f);
 if ((plan = e_run_sh_plan_approve(f)) != NULL)
 {
#ifndef NO_XWINDOWS
  if (WpeIsXwin())
   (*e_u_system)(plan);
  else
#endif
   e_system(plan, f->ed);
  wpe_startplan_free(plan);
 }
 WpeMouseRestoreShape();
 return(0);
}

/*  new project   */

/* A project file's variable: its name, its value as the build uses it, and
   its value as the file writes it (the box shows both). */
struct proj_var {  char *var, *string, *raw;  }  **p_v = NULL;
int p_v_n = 0;

/**
 * e_prj_var_value - The value a $(NAME) in a project file stands for.
 * @var: the name; @before: how many of the project's variables are read
 * so far (only those can be named).
 *
 * The project's own variables come first.  From the editor's environment a
 * project file may take only where the user is (HOME, USER, LOGNAME, PWD,
 * TMPDIR), never a key or a token he holds, which would otherwise reach
 * the compiler's command line, the question's box and the object file.
 * Return: the value, or NULL when the name is not one a project may use.
 */
static char *e_prj_var_value(const char *var, int before)
{
 int k;

 for (k = 0; k < before; k++)
  if (!strcmp(p_v[k]->var, var))
   return(p_v[k]->string);
 if (!wpe_startplan_env_name_allowed(var))
  return(NULL);
 return(getenv(var));
}

/**
 * e_prj_var_refused - Is $(NAME) one a project file may not expand at all?
 * @var: the name; @before: as e_prj_var_value.
 * Return: 1 for a name that is neither the project's own variable nor one
 * the environment may give it: it stays as written, so the box shows it.
 */
static int e_prj_var_refused(const char *var, int before)
{
 int k;

 for (k = 0; k < before; k++)
  if (!strcmp(p_v[k]->var, var))
   return(0);
 return(getenv(var) != NULL && !wpe_startplan_env_name_allowed(var));
}


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
   if (e_prj_var_refused(var, p_v_n - 1))
   {
    FREE(var);
    continue;                        /* left as written: the box shows the name */
   }
   v_string = e_prj_var_value(var, p_v_n - 1);
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
    if (p_v[i]->raw) FREE(p_v[i]->raw);
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
  p_v[p_v_n-1]->raw = NULL;
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
  p_v[p_v_n-1]->raw = WpeStrdup(p_v[p_v_n-1]->string);
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
 * e_prj_say_open_failed - Say that the open project file could not be opened.
 * @f: the window the box belongs to.
 *
 * The message names the project's path whatever its length: a folder's
 * Project key can make it far longer than a line buffer.
 */
static void e_prj_say_open_failed(FENSTER *f)
{
 size_t size = strlen(e_msg[ERR_FOPEN]) + strlen(e_prog.project) + 1;
 char *text = MALLOC(size);

 if (!text)
  return;
 snprintf(text, size, e_msg[ERR_FOPEN], e_prog.project);
 e_error(text, 0, f->fb);
 FREE(text);
}

/* The "install:" lines of a project, each expanded, in order. */
typedef struct {
 char **line;
 int n;
} e_install_list;

/* What the question calls a project's install lines, and their key. */
#define E_INSTALL_WHAT "install commands"
#define E_INSTALL_WHO  "The project file"


/**
 * e_install_add - Append an install line, taking it.
 * Return: 0, or -1 out of memory (the line is then freed).
 */
static int e_install_add(e_install_list *l, char *text)
{
 char **grown = REALLOC(l->line, (l->n + 1) * sizeof(char *));

 if (!grown)
 {
  FREE(text);
  return(-1);
 }
 l->line = grown;
 l->line[l->n++] = text;
 return(0);
}

/** e_install_free - Free the install lines. */
static void e_install_free(e_install_list *l)
{
 int i;

 for (i = 0; i < l->n; i++)
  FREE(l->line[i]);
 if (l->line)
  FREE(l->line);
 l->line = NULL;
 l->n = 0;
}

/**
 * e_install_text - Every install line as one text, a newline between them.
 * Return: the malloc'd text.
 */
static char *e_install_text(const e_install_list *l)
{
 size_t size = 1;
 char *text;
 int i;

 for (i = 0; i < l->n; i++)
  size += strlen(l->line[i]) + 1;
 if (!(text = MALLOC(size)))
  return(NULL);
 text[0] = '\0';
 for (i = 0; i < l->n; i++)
 {
  if (i)
   strcat(text, "\n");
  strcat(text, l->line[i]);
 }
 return(text);
}

/* Where the plan says an install line came from. */
#define E_INSTALL_SOURCE "install:"

/**
 * e_install_plan_make - The plan of a project's install lines.
 * @l: the lines, expanded as they will run.
 *
 * One plan for every line, so Alt-L asks once.  Each line is the project
 * file's own shell text, run by "/bin/sh -c" in the editor's folder, as
 * the original did; the box shows them whole, and that nothing the lines
 * then do is vetted.
 * Return: the plan, or NULL out of memory.
 */
static WpeStartPlan *e_install_plan_make(const e_install_list *l)
{
 WpeStartPlan *plan = wpe_startplan_new(WPE_START_INSTALL, e_prj_file_name());
 char folder[WPE_BUILD_PATH_MAX];
 int i;

 if (!plan)
  return(NULL);
 e_prj_folder(folder, sizeof folder);
 wpe_startplan_set_file(plan, e_prj_file_name(), folder);
 wpe_startplan_set_cwd(plan, E_RUN_HERE);
 wpe_startplan_word(plan, WPE_STARTPLAN_SHELL, WPE_WORD_PROGRAM, WPE_FROM_EDITOR, NULL);
 wpe_startplan_word(plan, WPE_STARTPLAN_SHELL_OPT, WPE_WORD_OPTION, WPE_FROM_EDITOR, NULL);
 for (i = 0; i < l->n; i++)
  wpe_startplan_word(plan, l->line[i], WPE_WORD_TEXT, WPE_FROM_FOLDER, E_INSTALL_SOURCE);
 plan->honest = 1;
 return(plan);
}

/**
 * e_install_approve - Ask before a project file's install lines run.
 * @f: a window; @l: the lines, expanded as they will run.
 *
 * They go to the shell, so a project file from a clone could run anything
 * at Alt-L.  They are shown first, once per folder and text, as a folder's
 * compiler is; there is no command of the user's own to run instead, so
 * "Use mine" is not offered.
 * Return: the answered plan (the caller frees it), or NULL to run nothing.
 */
static WpeStartPlan *e_install_approve(FENSTER *f, const e_install_list *l)
{
 char *text = e_install_text(l);
 E_STARTPLAN_SHOW show;
 WpeStartPlan *plan;
 int verdict;

 if (!text)
  return(NULL);
 if (!(plan = e_install_plan_make(l)))
 {
  FREE(text);
  return(NULL);
 }
 show.who = E_INSTALL_WHO;
 show.what = E_INSTALL_WHAT;
 show.shown = text;
 show.own = NULL;
 verdict = e_startplan_approve(f, plan, &show);
 FREE(text);
 if (verdict != WPE_STARTCMD_RUN || !wpe_startplan_running(plan))
 {
  wpe_startplan_free(plan);
  return(NULL);
 }
 return(plan);
}

/**
 * e_install_lines_run - Run the install lines, once the user agreed.
 * Return: 0, or WPE_ESC when nothing ran.
 */
static int e_install_lines_run(FENSTER *f, const e_install_list *l)
{
 char *argv[] = { WPE_STARTPLAN_SHELL, WPE_STARTPLAN_SHELL_OPT, NULL, NULL };
 WpeStartPlan *plan;
 int i;

 if (l->n == 0)
  return(0);
 if (!(plan = e_install_approve(f, l)))
  return(WPE_ESC);
 for (i = 0; i < l->n; i++)
 {
  e_d_p_message(l->line[i], f, 1);
  argv[2] = l->line[i];
  e_shell_run(plan, argv);
 }
 wpe_startplan_free(plan);
 return(0);
}

/**
 * e_install_run - Run the "install:" commands of the open project.
 * @f: a window of the desktop.
 */
static int e_install_run(FENSTER *f)
{
 char *tp, *sp, *string, *tmp, text[256];
 FILE *fp;
 int i, j, ret;
 e_install_list lines = {NULL, 0};

 if (!e__project) return(0);
 if ((fp = fopen(e_prog.project, "r")) == NULL)
 {
  e_prj_say_open_failed(f);
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
  /* A blank install line is no command: a project saved by 1.5.30a ends
     its install section with one, and Install must not ask to run it. */
  if (sp[0] == '\0')
   continue;
  if (!(string = MALLOC(strlen(sp) + 1)))
  {
   fclose(fp);
   e_install_free(&lines);
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
     e_install_free(&lines);
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
  if (e_install_add(&lines, string))
  {
   fclose(fp);
   e_install_free(&lines);
   e_error(e_msg[ERR_LOWMEM], 0, f->fb);
   return(-1);
  }
 }
 fclose(fp);
 ret = e_install_lines_run(f, &lines);
 e_install_free(&lines);
 return(ret);
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
 char ofile[WPE_BUILD_PATH_MAX];
 char program[WPE_BUILD_PATH_MAX]; /* the project's program, placed (e_prj_program) */
 struct stat obuf;
 int lib_remove;           /* a library this editor did not build is in the way */
 enum e_prj_phase phase;
 e_build_cont done;
 void *data;
} g_prj;

/**
 * e_prj_program - The program the open project links, as a command names it.
 *
 * Worked out once per build by e_prj_setup from the project's EXENAME and
 * Exedir, so the link, the up-to-date test, the "nothing to build" line, Run
 * and the debugger all name the file the build writes.  The Executable field
 * of Project-Options keeps the word the project file holds: a build never
 * writes its placed path into the user's EXENAME.
 * Return: the path; before the first build of a session, the one that word
 * and Exedir give.
 */
const char *e_prj_program(void)
{
 static char fallback[WPE_BUILD_PATH_MAX];

 if (g_prj.program[0])
  return(g_prj.program);
 wpe_progname_project_program(fallback, sizeof fallback, e_s_prog.exe_name, e_prog.exedir);
 return(fallback);
}

static void e_prj_run(void);

/* The project variables that name what a project build runs. */
#define E_PRJ_VAR_CMP      "CMP"
#define E_PRJ_VAR_CMPFLAGS "CMPFLAGS"
#define E_PRJ_VAR_LDFLAGS  "LDFLAGS"
/* The language whose compile line "Use mine" runs for a project. */
#define E_PRJ_OWN_LANGUAGE "C"
#define E_PRJ_WHO          "The project file"
#define E_PRJ_WHAT         "compiler"

/* The user's own C compile line, while he answered "Use mine" for the
   project being built; NULL while the project's own line runs. */
static struct e_s_prog *e_prj_own;

/**
 * e_prj_text_add_words - Append a project variable's words to a command.
 * @text: the command so far (malloc'd, may be NULL); @df: the words, freed.
 * Return: the longer command.
 */
static char *e_prj_text_add_words(char *text, struct dirfile *df)
{
 int k;

 if (!df)
  return(text);
 for (k = 0; k < df->anz; k++)
  text = e_prog_text_add(text, df->name[k]);
 freedf(df);
 return(text);
}

/**
 * e_prj_line_text - A project's compile and link line from its three parts.
 * @cmp/@flags/@ldflags: the program, its options and the link's options.
 *
 * Each part is taken word by word, the project's variables filled in, as
 * the build itself reads them; the program's output name is left out, as
 * every project names one.
 * Return: the malloc'd text.
 */
static char *e_prj_line_text(char *cmp, char *flags, char *ldflags)
{
 char *text = NULL;

 if (cmp) text = e_prj_text_add_words(text, e_p_get_args(cmp));
 if (flags) text = e_prj_text_add_words(text, e_p_get_args(flags));
 if (ldflags) text = e_prj_text_add_words(text, e_p_get_args(ldflags));
 if (!text)
  text = WpeStrdup("");
 return(text);
}

/**
 * e_prj_var - A variable of the project as the build uses it.
 * @name: the variable.
 *
 * After "Use mine" the project's program, options and link options are the
 * user's own C line; every other variable is the project's.
 * Return: its words (freed by the caller), or NULL when it is not set.
 */
static struct dirfile *e_prj_var(char *name)
{
 if (e_prj_own && strcmp(name, E_PRJ_VAR_CMP) == 0)
  return(e_p_get_args(e_prj_own->compiler));
 if (e_prj_own && strcmp(name, E_PRJ_VAR_CMPFLAGS) == 0)
  return(e_p_get_args(e_prj_own->comp_str));
 if (e_prj_own && strcmp(name, E_PRJ_VAR_LDFLAGS) == 0)
  return(e_p_get_args(e_prj_own->libraries));
 return(e_p_get_var(name));
}

/**
 * e_prj_folder - The folder that holds the open project file.
 * @out/@sz: receive it.
 *
 * The build's and the debugger's start plans keep the user's answer for
 * this folder.
 */
void e_prj_folder(char *out, size_t sz)
{
 char *slash;

 snprintf(out, sz, "%s", e_prog.project);
 slash = strrchr(out, DIRC);
 if (slash == out) slash[1] = '\0';
 else if (slash) *slash = '\0';
 else snprintf(out, sz, ".");
}

/**
 * e_prj_file_name - The open project file's name, without its folder, as
 * the box's title names it.
 */
const char *e_prj_file_name(void)
{
 const char *slash = strrchr(e_prog.project, DIRC);

 if (slash)
  return(slash + 1);
 return(e_prog.project);
}

/* The literal words a project build adds: the compile's and the link's,
   ar's (a new archive, an existing one) and ranlib. */
#define E_PRJ_AR        "ar"
#define E_PRJ_AR_NEW    "-cr"
#define E_PRJ_AR_ADD    "-r"
#define E_PRJ_RANLIB    "ranlib"
#define E_PRJ_SRC_FMT   "%s: %s"
/* The scope under which a project's line the user typed counts as his. */
#define E_PRJ_SCOPE_FMT "Project/%s"

/**
 * e_prj_typed_scope - The scope a project's typed values are noted under.
 * @out/@sz: receive "Project/<file name>".
 */
static void e_prj_typed_scope(char *out, size_t sz)
{
 snprintf(out, sz, E_PRJ_SCOPE_FMT, e_prj_file_name());
}

/**
 * e_prj_part_from - Whose a part of the project's compile line is.
 * @name: "CMP", "CMPFLAGS" or "LDFLAGS"; @value: the part as the build
 * uses it; @own_value: the matching part of the user's own C line.
 *
 * A project file is the folder's, whether or not a start-folder xwperc
 * exists.  A part that is empty, equal to the user's own, or one he typed
 * in Project-Options this session is his; any other is the folder's.
 * Return: WPE_FROM_USER or WPE_FROM_FOLDER.
 */
static int e_prj_part_from(const char *name, const char *value, const char *own_value)
{
 char scope[WPE_STARTCMD_KEY_MAX], folder[WPE_BUILD_PATH_MAX];

 if (!value || !*value)
  return(WPE_FROM_USER);
 if (own_value && strcmp(value, own_value) == 0)
  return(WPE_FROM_USER);
 e_prj_typed_scope(scope, sizeof scope);
 e_prj_folder(folder, sizeof folder);
 if (e_opt_field_is_his(scope, name, value, folder))
  return(WPE_FROM_USER);
 return(WPE_FROM_FOLDER);
}

/**
 * e_prj_plan_part - Add one part of a project's compile line to its plan.
 * @plan: the plan; @name: the part's variable; @value: the part, as the
 * build uses it; @own_value: the user's own; @first_is_program: 1 for CMP,
 * whose first word is the program; @mine: 1 for the user's own plan.
 */
static void e_prj_plan_part(WpeStartPlan *plan, const char *name, const char *value,
                            const char *own_value, int first_is_program, int mine)
{
 char source[E_BUILD_SRC_MAX];
 struct dirfile *df;
 int k, from = WPE_FROM_USER, role;

 if (!value || !(df = e_p_get_args((char *)value)))
  return;
 if (!mine)
  from = e_prj_part_from(name, value, own_value);
 snprintf(source, sizeof source, E_PRJ_SRC_FMT, e_prj_file_name(), name);
 for (k = 0; k < df->anz; k++)
 {
  if (k == 0 && first_is_program)
   role = WPE_WORD_PROGRAM;
  else if (from == WPE_FROM_FOLDER)
   role = WPE_WORD_TEXT;
  else
   role = WPE_WORD_OPTION;
  wpe_startplan_word(plan, df->name[k], role, from, source);
 }
 freedf(df);
 if (from == WPE_FROM_FOLDER && !mine)
  wpe_startplan_value(plan, e_prj_file_name(), name, e_find_var_raw((char *)name), value);
}

/**
 * e_prj_plan_line - The project's compile line in its plan: the project's
 * own, or the user's own C line for "Use mine".
 * @plan: the plan; @own: the user's own C entry, or NULL; @mine: 1 for
 * his own plan.
 */
static void e_prj_plan_line(WpeStartPlan *plan, const struct e_s_prog *own, int mine)
{
 const char *own_cmp = own ? own->compiler : NULL;
 const char *own_flags = own ? own->comp_str : NULL;
 const char *own_ld = own ? own->libraries : NULL;

 if (mine)
 {
  e_prj_plan_part(plan, E_PRJ_VAR_CMP, own_cmp, own_cmp, 1, 1);
  e_prj_plan_part(plan, E_PRJ_VAR_CMPFLAGS, own_flags, own_flags, 0, 1);
  e_prj_plan_part(plan, E_PRJ_VAR_LDFLAGS, own_ld, own_ld, 0, 1);
 }
 else
 {
  e_prj_plan_part(plan, E_PRJ_VAR_CMP, e_find_var(E_PRJ_VAR_CMP), own_cmp, 1, 0);
  e_prj_plan_part(plan, E_PRJ_VAR_CMPFLAGS, e_find_var(E_PRJ_VAR_CMPFLAGS), own_flags, 0, 0);
  e_prj_plan_part(plan, E_PRJ_VAR_LDFLAGS, e_find_var(E_PRJ_VAR_LDFLAGS), own_ld, 0, 0);
 }
 wpe_startplan_word(plan, E_BUILD_OPT_COMPILE, WPE_WORD_OPTION, WPE_FROM_EDITOR, NULL);
 wpe_startplan_word(plan, E_BUILD_OPT_OUTPUT, WPE_WORD_OPTION, WPE_FROM_EDITOR, NULL);
 wpe_startplan_word(plan, E_PRJ_AR, WPE_WORD_PROGRAM, WPE_FROM_EDITOR, NULL);
 wpe_startplan_word(plan, E_PRJ_AR_NEW, WPE_WORD_OPTION, WPE_FROM_EDITOR, NULL);
 wpe_startplan_word(plan, E_PRJ_AR_ADD, WPE_WORD_OPTION, WPE_FROM_EDITOR, NULL);
 wpe_startplan_word(plan, E_PRJ_RANLIB, WPE_WORD_PROGRAM, WPE_FROM_EDITOR, NULL);
}

static int e_prj_program_path(char *out, size_t sz, struct dirfile *df, const char *exedir);
static int e_prj_object_of(const char *file, const char *exedir, char *out, size_t sz);

/**
 * e_prj_plan_program - The program the project writes, in its plan.
 * @plan: the plan; @exedir: the output folder; @from_dir: whose it is.
 * Return: 0, or -1 when the path does not fit.
 */
static int e_prj_plan_program(WpeStartPlan *plan, const char *exedir, int from_dir)
{
 char ofile[WPE_BUILD_PATH_MAX], source[E_BUILD_SRC_MAX];
 struct dirfile *df = e_p_get_var("EXENAME");
 int from = from_dir, rc;

 if (df && df->anz > 0)
 {
  from = WPE_FROM_FOLDER;
  wpe_startplan_value(plan, e_prj_file_name(), "EXENAME", e_find_var_raw("EXENAME"),
                      e_find_var("EXENAME"));
 }
 rc = e_prj_program_path(ofile, sizeof ofile, df, exedir);
 if (df) freedf(df);
 if (rc)
  return(-1);
 snprintf(source, sizeof source, E_PRJ_SRC_FMT, e_prj_file_name(), "EXENAME");
 wpe_startplan_path(plan, ofile, WPE_PATH_WRITE, from, source, NULL);
 wpe_startplan_mark_output(plan, WPE_OUTPUT_PRODUCT);
 return(0);
}

/**
 * e_prj_plan_files - The project's files, and the objects made of them.
 * @plan: the plan; @exedir: the output folder.
 *
 * Each file is handed to the compiler as a path; each object is written,
 * and removed when it is stale.  Inside the project's folder they are its
 * own business and the box only counts them.
 * Return: 0, or -1 when a path does not fit.
 */
static int e_prj_plan_files(WpeStartPlan *plan, const char *exedir)
{
 char named[WPE_BUILD_PATH_MAX], object[WPE_BUILD_PATH_MAX], source[E_BUILD_SRC_MAX];
 struct dirfile *df = e_p_get_var("FILES");
 int k;

 if (!df)
  return(0);
 snprintf(source, sizeof source, E_PRJ_SRC_FMT, e_prj_file_name(), "FILES");
 for (k = 0; k < df->anz; k++)
 {
  if (wpe_startplan_operand(named, sizeof named, NULL, df->name[k]) ||
      e_prj_object_of(named, exedir, object, sizeof object))
  {
   freedf(df);
   return(-1);
  }
  wpe_startplan_path(plan, named, WPE_PATH_INPUT, WPE_FROM_FOLDER, source, NULL);
  wpe_startplan_mark_output(plan, WPE_OUTPUT_SOURCE);
  wpe_startplan_path(plan, object, WPE_PATH_WRITE, WPE_FROM_FOLDER, source, NULL);
  wpe_startplan_mark_output(plan, WPE_OUTPUT_OBJECT);
  wpe_startplan_path(plan, object, WPE_PATH_REMOVE, WPE_FROM_FOLDER, source, NULL);
  wpe_startplan_mark_output(plan, WPE_OUTPUT_OBJECT);
 }
 freedf(df);
 return(0);
}

/**
 * e_prj_plan_library - The project's library, in its plan.
 * @plan: the plan.
 */
static void e_prj_plan_library(WpeStartPlan *plan)
{
 char lib[WPE_BUILD_PATH_MAX], source[E_BUILD_SRC_MAX];
 struct dirfile *df = e_p_get_var("LIBNAME");

 if (df && df->anz > 0 && wpe_startplan_operand(lib, sizeof lib, NULL, df->name[0]) == 0)
 {
  snprintf(source, sizeof source, E_PRJ_SRC_FMT, e_prj_file_name(), "LIBNAME");
  wpe_startplan_path(plan, lib, WPE_PATH_WRITE, WPE_FROM_FOLDER, source, NULL);
  wpe_startplan_mark_output(plan, WPE_OUTPUT_PRODUCT);
  /* A library the editor did not build is removed before ar makes it anew. */
  wpe_startplan_path(plan, lib, WPE_PATH_REMOVE, WPE_FROM_FOLDER, source, NULL);
  wpe_startplan_mark_output(plan, WPE_OUTPUT_PRODUCT);
  wpe_startplan_value(plan, e_prj_file_name(), "LIBNAME", e_find_var_raw("LIBNAME"),
                      e_find_var("LIBNAME"));
 }
 if (df) freedf(df);
}

/**
 * e_prj_plan_project_file - The project file itself, when a start-folder
 * xwperc named it.
 * @plan: the plan.
 *
 * A folder's Project key can name a project file anywhere; one outside
 * the start folder is a file the build reads from outside, and asks.
 */
static void e_prj_plan_project_file(WpeStartPlan *plan)
{
 int from = e_opt_value_from(E_OPT_SCOPE_PROGRAMMING, "Project", e_prog.project);

 if (from != WPE_FROM_FOLDER)
  return;
 wpe_startplan_path(plan, e_prog.project, WPE_PATH_INPUT, from, "./xwperc: Project",
                    e_opt_start_folder());
 wpe_startplan_value(plan, E_STARTCMD_XWPERC, "Project", e_prog.project, NULL);
}

/**
 * e_prj_plan_make - The plan of a project's build: every compile, the
 * library, ranlib and the link, judged once.
 * @own: the user's own C entry, or NULL; @mine: 1 for the plan "Use mine"
 * starts (his own line and output folder, the project's files);
 * @exedir: the output folder.
 * Return: the plan, or NULL when a path does not fit or out of memory.
 */
static WpeStartPlan *e_prj_plan_make(const struct e_s_prog *own, int mine, const char *exedir)
{
 char folder[WPE_BUILD_PATH_MAX];
 WpeStartPlan *plan = wpe_startplan_new(WPE_START_PROJECT, e_prj_file_name());
 int from_dir = WPE_FROM_USER;
 char *dir;

 if (!plan)
  return(NULL);
 e_prj_folder(folder, sizeof folder);
 wpe_startplan_set_file(plan, e_prj_file_name(), folder);
 if ((dir = e_prj_dir()) != NULL)
 {
  wpe_startplan_set_cwd(plan, dir);
  FREE(dir);
 }
 plan->list_inside = 1;
 if (!mine)
  from_dir = e_opt_value_from(E_OPT_SCOPE_PROGRAMMING, "Exedir", exedir);
 e_prj_plan_line(plan, own, mine);
 if (from_dir == WPE_FROM_FOLDER)
  wpe_startplan_value(plan, E_STARTCMD_XWPERC, "Exedir", exedir, NULL);
 if (e_prj_plan_program(plan, exedir, from_dir) || e_prj_plan_files(plan, exedir))
 {
  wpe_startplan_free(plan);
  return(NULL);
 }
 e_prj_plan_library(plan);
 if (!mine)
  e_prj_plan_project_file(plan);
 return(plan);
}

/**
 * e_prj_approve - Judge the plan of a project's build, and ask when the
 * project file or a start-folder xwperc decided something the user has
 * not seen.
 * @f: the window the build was asked from; the project's variables are read.
 *
 * A project file comes with the folder it sits in, a clone included.  Its
 * compile line, where it puts the program and its objects, and the project
 * file a start-folder xwperc names are one plan, judged once for the whole
 * build: a line equal to the user's own C line, inside the project's
 * folder, runs as it is; anything else is shown first, once per folder and
 * plan.  "Use mine" builds with his own C line and output folder.
 * Return: 0 to build, -1 when the user cancelled or nothing may run.
 */
static int e_prj_approve(FENSTER *f)
{
 struct e_s_prog *own = e_opt_own_language(E_PRJ_OWN_LANGUAGE);
 char *text, *own_text = NULL;
 E_STARTPLAN_SHOW show;
 int verdict;

 e_prj_own = NULL;
 e_build_plan_drop();
 if (!e_prog.project || !e_prog.project[0])
  return(-1);
 if (!(e_build_plan = e_prj_plan_make(own, 0, e_prog.exedir)))
 {
  e_build_path_too_long(f, "program");
  return(-1);
 }
 if (own)
 {
  wpe_startplan_set_own(e_build_plan, e_prj_plan_make(own, 1, e_opt_own_value("Exedir")));
  own_text = e_prj_line_text(own->compiler, own->comp_str, own->libraries);
 }
 text = e_prj_line_text(e_find_var(E_PRJ_VAR_CMP), e_find_var(E_PRJ_VAR_CMPFLAGS),
                        e_find_var(E_PRJ_VAR_LDFLAGS));
 show.who = E_PRJ_WHO;
 show.what = E_PRJ_WHAT;
 show.shown = text;
 show.own = own_text;
 verdict = e_startplan_approve(f, e_build_plan, &show);
 if (verdict == WPE_STARTCMD_USE_OWN && own)
 {
  e_prj_own = own;
  e_build_take_own_exedir();
 }
 FREE(text);
 if (own_text) FREE(own_text);
 if (verdict == WPE_STARTCMD_CANCEL)
  return(-1);
 if (verdict == WPE_STARTCMD_USE_OWN && !own)
  return(-1);
 return(0);
}

/**
 * e_prj_fields_are_his - Did the user write the project's line in Project-Options?
 * @o: the dialog, after OK; e_s_prog still holds the line it opened with.
 * Return: as e_prog_fields_are_his, against his own C line.
 */
static int e_prj_fields_are_his(W_OPTSTR *o)
{
 struct e_s_prog *own = e_opt_own_language(E_PRJ_OWN_LANGUAGE);
 e_prog_field fld[E_PRJ_LINE_FIELDS];

 fld[0].before = e_s_prog.compiler;  fld[0].after = o->wstr[0]->txt;
 fld[1].before = e_s_prog.comp_str;  fld[1].after = o->wstr[1]->txt;
 fld[2].before = e_s_prog.libraries; fld[2].after = o->wstr[2]->txt;
 fld[0].room = E_PROG_FIELD_ROOM;
 fld[1].room = fld[2].room = E_PROG_PRJ_FIELD_ROOM;
 fld[0].own = fld[1].own = fld[2].own = NULL;
 if (own)
 {
  fld[0].own = own->compiler;
  fld[1].own = own->comp_str;
  fld[2].own = own->libraries;
 }
 return(e_prog_fields_are_his(fld, E_PRJ_LINE_FIELDS));
}

/**
 * e_prj_note_typed - The project line the user set in Project-Options is his own.
 * @his: e_prj_fields_are_his for the dialog.
 */
static void e_prj_note_typed(int his)
{
 char scope[WPE_STARTCMD_KEY_MAX], folder[WPE_BUILD_PATH_MAX];

 if (!his || !e_prog.project || !e_prog.project[0])
  return;
 e_prj_typed_scope(scope, sizeof scope);
 e_prj_folder(folder, sizeof folder);
 e_opt_value_typed_in(scope, E_PRJ_VAR_CMP, e_s_prog.compiler, folder);
 e_opt_value_typed_in(scope, E_PRJ_VAR_CMPFLAGS, e_s_prog.comp_str, folder);
 e_opt_value_typed_in(scope, E_PRJ_VAR_LDFLAGS, e_s_prog.libraries, folder);
}

/**
 * e_prj_files_as_paths - Make every FILES word one that names a file.
 * @df: the project's FILES, changed in place, each through
 * wpe_startplan_operand, as the build's plan names them.
 * Return: 0, or -1 out of memory.
 */
static int e_prj_files_as_paths(struct dirfile *df)
{
 size_t len;
 char *named;
 int k;

 for (k = 0; k < df->anz; k++)
 {
  len = E_OPERAND_PREFIX_ROOM + strlen(df->name[k]) + 1;
  if (!(named = MALLOC(len)))
   return(-1);
  if (wpe_startplan_operand(named, len, NULL, df->name[k]))
  {
   FREE(named);
   return(-1);
  }
  FREE(df->name[k]);
  df->name[k] = named;
 }
 return(0);
}

/**
 * e_prj_program_path - The program a project build writes.
 * @out/@sz: receive it, in the program folder; @df: EXENAME's words, or NULL;
 * @exedir: the output folder.
 * Return: 0, or -1 when the path does not fit (the build then stops and
 * says so, instead of writing past the buffer).
 */
static int e_prj_program_path(char *out, size_t sz, struct dirfile *df, const char *exedir)
{
 const char *word = NULL;

 if (df && df->anz > 0)
  word = df->name[0];
 return(wpe_progname_project_program(out, sz, word, exedir));
}

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
 int i, j, k, argc, exlib = 0, lib_remove = 0;
 char ofile[WPE_BUILD_PATH_MAX];
 struct stat lbuf;

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
  e_prj_say_open_failed(f);
  return(-1);
 }
 if (e_prj_approve(f))
  return(WPE_ESC);
 e_build_record_begin(f);
 e_arg = (char **) MALLOC(e_argc*sizeof(char *));
 arg = (char **) MALLOC(argc*sizeof(char *));
 /* MALLOC leaves slot 0 uninitialised.  The CMP/FILES lookups below can bail
    out early (e.g. a project whose variables did not load) straight into
    e_free_arg, which FREEs every non-NULL slot -- so an unset slot must read
    NULL or it frees a wild pointer and glibc aborts.  The build loops set
    slot 0 on their first iteration, so this only matters on the early exits. */
 if (e_arg) e_arg[0] = NULL;
 if (arg) arg[0] = NULL;
 df = e_prj_var(E_PRJ_VAR_CMP);
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
 df = e_prj_var(E_PRJ_VAR_CMPFLAGS);
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
 if (e_prj_program_path(ofile, sizeof ofile, df, e_prog.exedir))
 {
  if (df) freedf(df);
  e_build_path_too_long(f, "program");
  e_free_arg(arg, argc);  e_free_arg(e_arg, e_argc);
  return(-1);
 }
 if (df) freedf(df);
 e_argc = e_add_arg(&e_arg, ofile, 2, e_argc);
 df = e_p_get_var("LIBNAME");
 e_prj_library_set("");
 if (df && df->anz > 0)
 {
  char libpath[WPE_PATH_SIZE];

  if (wpe_startplan_operand(libpath, sizeof libpath, NULL, df->name[0]))
  {
   freedf(df);
   e_build_path_too_long(f, "library");
   e_free_arg(arg, argc);  e_free_arg(e_arg, e_argc);
   return(-1);
  }
  e_prj_library_set(libpath);
  /* A library this editor did not build here is no library of the build:
     every object goes into one made anew, and the shipped one, with any
     member it carries, is removed before the first insert. */
  if (e_build_stat(libpath, &lbuf))
   exlib = 1;
  else if (!e_build_current(libpath, NULL, 0, NULL, &lbuf))
   exlib = lib_remove = 1;
 }
 if (df) freedf(df);
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
 if (e_prj_files_as_paths(df))
 {
  freedf(df);
  e_error(e_msg[ERR_LOWMEM], 0, cn->fb);
  e_free_arg(arg, argc);  e_free_arg(e_arg, e_argc);
  return(-1);
 }
 arg[argc] = NULL;
 memset(&g_prj, 0, sizeof g_prj);
 snprintf(g_prj.program, sizeof g_prj.program, "%s", ofile);
 g_prj.cn = cn;
 g_prj.arg = arg;
 g_prj.argc = argc;
 g_prj.df = df;
 g_prj.exlib = exlib;
 g_prj.lib_remove = lib_remove;
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
 (void)data;
 e_build_step_end_of(f, result);
 if (result)
 {
  e_prj_end(result);
  return;
 }
 e_prj_run();
}

/**
 * e_prj_object_of - The object file a project file is compiled into.
 * @file: the project file, as the build names it; @exedir: the output
 * folder; @out/@sz: receive the object.
 *
 * The build and its plan both name objects through here, so a compile
 * writes exactly the object the plan judged.
 * Return: 0, or -1 when it does not fit.
 */
static int e_prj_object_of(const char *file, const char *exedir, char *out, size_t sz)
{
 WpeProgramSource s;

 memset(&s, 0, sizeof s);
 s.name = file;
 s.exedir = exedir;
 return(wpe_progname_object(out, sz, &s));
}

/**
 * e_prj_object_name - The object file of project file @k, into g_prj.ofile.
 */
static void e_prj_object_name(int k)
{
 if (e_prj_object_of(g_prj.df->name[k], e_prog.exedir, g_prj.ofile, sizeof g_prj.ofile))
  g_prj.ofile[0] = '\0';
}

/**
 * e_prj_object_current - Is the object of project file @k newer than its
 * source?  Current objects are not compiled again.
 */
static int e_prj_object_current(int k)
{
#ifdef CHECKHEADER
 if (!e_build_current(g_prj.ofile, NULL, 0, &last_time, &g_prj.obuf))
  return(0);
 return(!e_check_header(g_prj.df->name[k], g_prj.obuf.st_mtime, g_prj.cn, 0));
#else
 return(e_build_current(g_prj.ofile, g_prj.df->name[k], 0, &last_time, &g_prj.obuf));
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
 e_build_remove_stale(f, g_prj.ofile);
 e_built_step_begin(g_prj.ofile, NULL);
 g_prj.sccs = 1;
 e_p_set_compiled_source(g_prj.df->name[k]);
 /* The child prints its own command line first (e_exec_planned), as every
    single-file build does; writing it here as well showed each command
    twice. */
 e_sys_ini();
 file = e_exec_planned(f, g_prj.arg);
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
 const char *program = e_prj_program();
 int len = strlen(program);

 if (!strcmp(base, "main.o"))
  return(1);
 return(!strncmp(program, base, len) && g_prj.ofile[len] == '.');
}

/**
 * e_prj_inserted_after_compile - Is a freshly compiled object put into the
 * library at once?  The test the project build has always made.
 */
static int e_prj_inserted_after_compile(void)
{
 char *base = e_prj_object_base();
 const char *program = e_prj_program();
 int len = strlen(program);

 if (g_prj.exlib || e_prj_library()[0] == '\0' || !strcmp(base, "main.o"))
  return(0);
 return(strncmp(program, base, len) || g_prj.ofile[len] == '.');
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
 if (g_prj.lib_remove)
 {
  /* The library in the way is not one this editor built: ar makes it anew
     from the editor's own objects, so no member of the clone's survives. */
  e_build_remove_stale(e_prj_top(), e_prj_library());
  g_prj.lib_remove = 0;
 }
 e_built_step_begin(e_prj_library(), NULL);
 e_make_library(e_prj_library(), g_prj.ofile, e_prj_top(), e_prj_step_done, NULL);
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
 if (e_prj_library()[0] == '\0' || e_prj_is_program_object())
 {
  e_argc = e_add_arg(&e_arg, g_prj.ofile, e_argc, e_argc);
  return(E_PRJ_GO_ON);
 }
 if (g_prj.exlib || !e_build_current(e_prj_library(), g_prj.ofile, 0, NULL, NULL))
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
 if (g_prj.libsw && e_prj_library()[0] != '\0')
 {
  FENSTER *f = e_prj_top();
  PIC *pic = NULL;
  char *ar_arg[3];
  int file = 0;

  ar_arg[0] = E_PRJ_RANLIB;
  ar_arg[1] = e_prj_library();
  ar_arg[2] = NULL;
  e_built_step_begin(e_prj_library(), NULL);
  /* The child prints its command line itself (e_exec_planned). */
  e_sys_ini();
  file = e_exec_planned(f, ar_arg);
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

 if (e_prj_library()[0] != '\0' && !g_prj.lib_remove)
  e_argc = e_add_arg(&e_arg, e_prj_library(), e_argc, e_argc);
 freedf(g_prj.df);
 g_prj.df = NULL;
 df = e_prj_var(E_PRJ_VAR_LDFLAGS);
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
 ECNT *cn = f->ed;
 int ret, cwd_chg;

 /* The project builds in its own directory, whichever one the editor was
    started from; every later step runs where the first one did. */
 e_prj_anchor_path();
 cwd_chg = e_build_pushd(f, cwd_saved, sizeof cwd_saved);
 ret = e_prj_setup(f);
 if (ret)
 {
  /* e_prj_setup opens a fresh Messages window, closing the old one, which
     may be the window the key was pressed in: the build ends on the window
     now on top. */
  f = cn->f[cn->mxedt];
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

/**
 * e_find_var_raw - A project variable as the project file writes it.
 * @var: its name.
 *
 * The box shows what the file says ("$(HOME)/.local/bin/git") beside what
 * the build makes of it, so the user sees a variable's effect.
 * Return: the text before expansion, or "" when the variable is not set.
 */
const char *e_find_var_raw(const char *var)
{
 int i;

 for (i = 0; i < p_v_n; i++)
  if (!strcmp(p_v[i]->var, var) && p_v[i]->raw)
   return(p_v[i]->raw);
 return("");
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
/**
 * e_make_prj_opt_finish - The one way out of e_make_prj_opt.
 * @save_df: the Files list of a Project window with unsaved edits, or NULL.
 *
 * Every way e_make_prj_opt can end, including the ones that stop early
 * because the project file is short or unreadable, goes through here. The
 * window the user is editing gets its own list back, and the three project
 * lists (Files, Variables, Install) are sealed whatever the file held: a
 * project with no FILES line or no install section still opens, adds,
 * edits and deletes without a list that is missing or half built.
 *
 * Return: the three project lists.
 */
static struct dirfile **e_make_prj_opt_finish(struct dirfile *save_df)
{
 int i;

 if (save_df)
 {
  if (e_p_df[0] && e_p_df[0] != save_df)
   freedf(e_p_df[0]);
  e_p_df[0] = save_df;
 }
 for (i = 0; i < 3; i++)
  wpe_listsel_seal(&e_p_df[i]);
 return(e_p_df);
}

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
  e_s_prog.exe_name = WpeStrdup(WPE_PROJECT_DEFAULT_PROGRAM);
  if (e_s_prog.intstr) FREE(e_s_prog.intstr);
  e_s_prog.intstr = WpeStrdup(cc_intstr);
  e_prj_library_set("");
  return(e_make_prj_opt_finish(save_df));
 }
 if (!(e_p_df[1] = wpe_listsel_new_empty())) return(e_make_prj_opt_finish(save_df));
 if (!(e_p_df[2] = wpe_listsel_new_empty())) return(e_make_prj_opt_finish(save_df));
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
   e_prj_library_set(p_v[i]->string);
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
   {  e_p_df[1]->anz--;  e_p_df[1]->name = tmp;  return(e_make_prj_opt_finish(save_df));  }
   if (!(e_p_df[1]->name[e_p_df[1]->anz-1] = MALLOC((strlen(p_v[i]->var)
			+ strlen(p_v[i]->string) + 2)*sizeof(char))))
   {  e_p_df[1]->anz--;  return(e_make_prj_opt_finish(save_df));  }
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
 if (!e_s_prog.exe_name || !e_s_prog.exe_name[0])
 {
  /* A project that names no program builds a.out; Project-Options offers it */
  if (e_s_prog.exe_name) FREE(e_s_prog.exe_name);
  e_s_prog.exe_name = WpeStrdup(WPE_PROJECT_DEFAULT_PROGRAM);
 }
 if (!e_s_prog.intstr)
  e_s_prog.intstr = WpeStrdup(cc_intstr);
 if ((fp = fopen(e_prog.project, "r")) == NULL)
 {
  e_prj_say_open_failed(f);
  return(e_make_prj_opt_finish(save_df));
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
 if (!tp) {  fclose(fp);  return(e_make_prj_opt_finish(save_df));  }
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
  {  e_p_df[2]->anz--;  e_p_df[2]->name = tmp;  fclose(fp);  return(e_make_prj_opt_finish(save_df));  }
  if (!(e_p_df[2]->name[e_p_df[2]->anz-1] = MALLOC((strlen(sp) + 1))))
  {  e_p_df[2]->anz--;  fclose(fp);  return(e_make_prj_opt_finish(save_df));  }

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
	       return(e_make_prj_opt_finish(save_df));
    }
    strcat(e_p_df[2]->name[e_p_df[2]->anz-1], text);
   }
  }
  j = strlen(e_p_df[2]->name[e_p_df[2]->anz-1]);
  if (*(e_p_df[2]->name[e_p_df[2]->anz-1]+j-1) == '\n')
   *(e_p_df[2]->name[e_p_df[2]->anz-1]+j-1) = '\0';
 }
 fclose(fp);
 return(e_make_prj_opt_finish(save_df));
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
 char *path;

 i = e_find_project_window(f->ed);
 if (i == 0 || e_prog.project[0] == DIRC)
  path = WpeStrdup(e_prog.project);
 else
  path = e_mkfilename(f->ed->f[i]->dirct, e_prog.project);
 fp = path ? fopen(path, "w") : NULL;
 if (path)
  FREE(path);
 if (fp == NULL)
 {
  e_prj_say_open_failed(f);
  return(-1);
 }
 fprintf(fp, "#\n# xwpe - project-file: %s\n", e_prog.project);
 fprintf(fp, "# created by xwpe version %s\n#\n", VERSION);
 for (i = 0; i < wpe_listsel_count(e_p_df[1], 1); i++)
  fprintf(fp, "%s\n", wpe_listsel_entry(e_p_df[1], i, 1));
 fprintf(fp, "\nCMP=\t%s\n", e_s_prog.compiler);
 fprintf(fp, "CMPFLAGS=\t%s\n", e_s_prog.comp_str);
 fprintf(fp, "LDFLAGS=\t%s\n", e_s_prog.libraries);
 fprintf(fp, "EXENAME=\t%s\n", e_s_prog.exe_name);
 if (e_prj_library()[0])
  fprintf(fp, "LIBNAME=\t%s\n", e_prj_library());
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
 for (i = 0, len = 8; i < wpe_listsel_count(e_p_df[0], 1); i++)
 {
  len += strlen(wpe_listsel_entry(e_p_df[0], i, 1));
  if (len > 80)
  {
   fprintf(fp, " \\\n\t");
   len = 1;
  }
  fprintf(fp, "%s ", wpe_listsel_entry(e_p_df[0], i, 1));
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

 if (wpe_listsel_count(e_p_df[2], 1) > 0)
  fprintf(fp, "\ninstall:\n");
 for (i = 0; i < wpe_listsel_count(e_p_df[2], 1); i++)
  fprintf(fp, "\t%s\n", wpe_listsel_entry(e_p_df[2], i, 1));
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
 char *title = NULL, str[WPE_PATH_SIZE];

 if (sw == 4)
  title = "Add File";
 else if (sw == 5)
  title = "Add Variable";
 else if (sw == 6)
  title = "Add Command";
 str[0] = '\0'; /* terminate new string to prevent garbage in display */
 if (e_add_arguments(str, sizeof str, title, fw->f, 0, AltA, NULL))
  wpe_listsel_insert(fw->df, fw->nf, str);
 return(0);
}

int e_p_edit_df(FLWND *fw, int sw)
{
 char *title = NULL, str[WPE_PATH_SIZE];
 char *entry;
 int row;

 if (sw == 4)
  title = "Change Filename";
 else if (sw == 5)
  title = "Change Variable";
 else if (sw == 6)
  title = "Change Command";
 entry = wpe_listsel_name(fw->df, fw->nf, 1);
 if (entry)
 {
  /* An entry the dialog's field cannot hold whole is not offered for
     editing: a cut entry would be written back in its place. */
  if (wpe_field_take(entry, str, sizeof str) != 0)
  {
   e_input_limit_say(WPE_PATH_FIELD_MAX);
   return(0);
  }
 }
 else
  str[0] = '\0';
 if (e_add_arguments(str, sizeof str, title, fw->f, 0, AltA, NULL))
 {
  row = wpe_listsel_set(fw->df, fw->nf, str);
  if (row >= 0)
   fw->nf = row;
 }
 return(0);
}

int e_p_del_df(FLWND *fw, int sw)
{
 /* Delete removes an entry, never the blank row that ends the list, and the
    last real file stays deletable because the list is sealed. */
 wpe_listsel_remove(fw->df, &fw->nf);
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

 if (e_p_m_buffer && e_buffer_clear(e_p_m_buffer) != E_BUFFER_OK)
  wpe_buffer_say_nomem(e_p_m_buffer, "the new build's messages");
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
 if (f->b->mxlines == 1 && f->b->bf[0].len == 0)
 {
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
  /* Sit at the stream write position -- the end of the last line, like a
     tailing terminal -- so output continues (and interactive input goes)
     right after the last character, not on top of it.  For a line ending
     in a newline the last line is empty, so this is column 0. */
  e_buffer_cursor_set(mf->b, last, mf->b->bf[last].len);
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

/**
 * e_p_alloc_pattern_rows - Allocate the row buffers of one error pattern.
 *
 * A language set up without a MessageString has an empty pattern, and a
 * pattern may have fewer rows than E_P_PATTERN_ROWS. Every row starts as an
 * empty string, so F9 on such a language reports "no error rows recognised"
 * instead of reading memory nobody filled.
 */
static void e_p_alloc_pattern_rows(char *rows[E_P_PATTERN_ROWS], char *srch)
{
 int n;

 for (n = 0; n < E_P_PATTERN_ROWS; n++)
 {
  rows[n] = MALLOC(strlen(srch) + 2);
  rows[n][0] = '\0';
 }
}

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
 e_p_alloc_pattern_rows(tmp, srch);
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
     tmp[n][l] = '\0';
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

