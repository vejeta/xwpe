/* we_unix.c                                             */
/* Copyright (C) 1993 Fred Kruse                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey              */
/* This is free software; you can redistribute it and/or  */
/* modify it under the terms of the                       */
/* GNU General Public License, see the file COPYING.      */

#include "model.h"		/* exchange for D.S.  */

#ifdef XWPE_DLL
#include <dlfcn.h>
#else
int WpeXtermInit(int *argc, char **argv);
int WpeTermInit(int *argc, char **argv);
#endif
#ifdef HAVE_WAYLAND
int WpeWaylandInit(int *argc, char **argv);
#endif
#ifdef HAVE_PANGO
void wpe_render_cairo_start_threads(void);
#endif

#include <termios.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <signal.h>
#include <pthread.h>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#include <locale.h>
#include <errno.h>
#include <poll.h>
#ifndef TERMCAP
#ifndef DJGPP
#include<curses.h>
#endif
#endif

#include "edit.h"		/*   exchange for D.S.  */
#include "attrb.h"
#include "progr.h"
#include "we_fdloop.h"
#include "we_dap.h"

#ifdef NOSYMLINKS
#define lstat(x,y)  stat(x,y)
#undef S_ISLNK
#define S_ISLNK(x)  0
#endif


/* e_lib_dir - the directory xwpe loads its data files from (syntax_def, the
   in-app help, the option file).  Normally the compiled-in LIBRARY_DIR (set by
   configure to the install prefix), but $XWPE_LIB overrides it so you can run
   straight from a build / source checkout without `make install` -- e.g.
   `XWPE_LIB=$(pwd) ./wpe foo.c` picks up the in-tree syntax_def. */
const char *e_lib_dir(void)
{
 const char *d = getenv("XWPE_LIB");
 return (d && *d) ? d : LIBRARY_DIR;
}

/* e_lib_file - resolve a data-file name under e_lib_dir(), falling back to
   "<name>_eng" when the bare name is missing.  The English defaults ship in
   the source tree with the "_eng" suffix (help.xwpe_eng, help.key_eng) and
   are renamed by `make install`; this fallback lets `wpe` run from a build
   tree (XWPE_LIB pointing at the checkout) without an install step, instead
   of silently rendering an empty Help window.  Returns a malloc'd path the
   caller must FREE -- the bare path even when neither variant exists, so
   the existing fopen()-failure error path is preserved. */
char *e_lib_file(const char *name)
{
 char *p = e_mkfilename((char *)e_lib_dir(), (char *)name);
 struct stat st;
 char *alt, *q;
 size_t n;

 if (!p || stat(p, &st) == 0)
  return p;
 n = strlen(name) + 5;                       /* + "_eng" + NUL */
 alt = MALLOC(n);
 if (!alt)
  return p;
 snprintf(alt, n, "%s_eng", name);
 q = e_mkfilename((char *)e_lib_dir(), alt);
 FREE(alt);
 if (q && stat(q, &st) == 0)
 {
  FREE(p);
  return q;
 }
 if (q)
  FREE(q);
 return p;
}

SCREENCELL *schirm = NULL;
char e_we_sw = 0;
WpeGfxBackend e_gfx_backend = WPE_GFX_X11;

void WpeSignalUnknown(int sig);
void WpeSignalChild(int sig);
void WpeSignalTerm(int sig);
static void wpe_end_request_open(void);

void (*WpeMouseChangeShape)(WpeMouseShape new_shape);
void (*WpeMouseRestoreShape)(void);
void (*WpeDisplayEnd)(void);
int (*fk_u_locate)(int x, int y);
int (*fk_u_cursor)(int x);
int (*e_u_initscr)(int argc, char *argv[]);
int (*fk_u_putchar)(int c);
int (*u_bioskey)(void);
int (*e_frb_u_menue)(int sw, int xa, int ya, FENSTER *f, int md);
COLOR (*e_s_u_clr)(int f, int b);
COLOR (*e_n_u_clr)(int fb);
void (*e_pr_u_col_kasten)(int xa, int ya, int x, int y, FENSTER *f, int sw);
int (*fk_mouse)(int g[]);
int (*fk_u_grab_pointer)(int on);
int (*fk_u_drag_next)(int *px, int *py);
int (*e_u_refresh)(void);
int (*e_u_getch)(void);
int (*e_u_sys_ini)(void);
int (*e_u_sys_end)(void);
int (*e_u_system)(const char *exe);
int (*e_make_urect)(int xa, int ya, int xe, int ye, int sw);
int (*e_make_urect_abs)(int xa, int ya, int xe, int ye, int sw);
int (*e_u_d_switch_out)(int sw);
int (*e_u_switch_screen)(int sw);
int (*e_u_deb_out)(struct FNST *f);
int (*e_u_cp_X_to_buffer)(struct FNST *f);
int (*e_u_copy_X_buffer)(struct FNST *f);
int (*e_u_paste_X_buffer)(struct FNST *f);
int (*e_u_key_take)(int want);
int (*e_u_change)(PIC *pic);
void (*e_u_serve_display)(void);
int (*e_u_ini_size)(void);
int (*e_get_pic_urect)(int xa, int ya, int xe, int ye, struct PICSTR *pic);
int (*e_u_s_sys_end)(void);
int (*e_u_s_sys_ini)(void);
void (*e_u_setlastpic)(PIC *pic);

FARBE *u_fb, *x_fb;

char MCI, MCA, RD1, RD2, RD3, RD4, RD5, RD6, WBT;
char RE1, RE2, RE3, RE4, RE5, RE6;
char *ctree[5];
int MENOPT = 8;
int e_mn_men = 3;

int MAXSLNS = 24;
int MAXSCOL = 80;
int col_num = 0;
char *att_no;
struct termios otermio, ntermio, ttermio;
int cur_x = -1, cur_y = -1;
void *libxwpe;

#ifdef NEWSTYLE
char *extbyte = NULL, *altextbyte = NULL;
#endif
SCREENCELL *altschirm = NULL;
PIC *e_X_l_pic = NULL;

void WpeNullFunction(void)
{
}

int WpeZeroFunction(void)
{
 return(0);
}

#if !defined(NO_XWINDOWS)
/* e_pick_gfx_backend - choose the graphical backend for `xwpe` at startup.
   Honours an explicit XWPE_BACKEND=x11|wayland|auto override; with no override
   (or "auto") it prefers the native Wayland surface whenever the session
   exposes WAYLAND_DISPLAY, otherwise X11.  X11 also covers XWayland, so the
   user running `xwpe` on any desktop gets a window with no flag.  Console mode
   (`wpe`) never reaches here.  When xwpe was built without the Wayland backend
   this always returns X11, so a forced "wayland" still opens via XWayland. */
static WpeGfxBackend e_pick_gfx_backend(void)
{
#ifdef HAVE_WAYLAND
 const char *force = getenv("XWPE_BACKEND");

 if (force && !strcmp(force, "wayland"))
  return WPE_GFX_WAYLAND;
 if (force && !strcmp(force, "x11"))
  return WPE_GFX_X11;
 /* "auto", empty, or anything else: detect from the session. */
 if (getenv("WAYLAND_DISPLAY"))
  return WPE_GFX_WAYLAND;
#endif
 return WPE_GFX_X11;
}
#endif /* !NO_XWINDOWS */

/* The thread that runs the editor's loop, and the process it belongs to.
   The graphical libraries start threads of their own (Pango's font loader;
   SDL's under the macOS X11 shim), and the kernel hands a process-wide
   signal to any thread that does not block it.  The SIGCHLD handler keeps
   the end of the programs the editor started in tables that the loop reads
   with SIGCHLD blocked; that block only holds the signal off the thread
   that set it, so the handler must never do its work on another thread.
   wpe_main_pid stays 0 until the editor is set up. */
static pthread_t wpe_main_thread;
static pid_t wpe_main_pid;

/**
 * wpe_signal_main_thread_note - Remember which thread runs the editor.
 *
 * Called first thing at start-up, before any signal handler is installed
 * and before any library can start a thread, so every later signal can be
 * told apart: on the editor's thread or on a library's.
 */
static void wpe_signal_main_thread_note(void)
{
 wpe_main_thread = pthread_self();
 wpe_main_pid = getpid();
}

/**
 * wpe_signal_on_main_thread - In a signal handler: is this the editor's own
 * thread?
 *
 * A child forked from the editor (the jdb VM before it runs java) has one
 * thread and a new PID; the thread remembered above does not exist there,
 * so in a child the answer is always yes.  getpid and pthread_self are
 * async-signal-safe; pthread_equal is not on the POSIX list, but every
 * supported libc implements it as a plain comparison.
 * Return: 1 on the editor's thread (or before start-up noted it), else 0.
 */
static int wpe_signal_on_main_thread(void)
{
 if (wpe_main_pid == 0 || getpid() != wpe_main_pid)
  return(1);
 return(pthread_equal(pthread_self(), wpe_main_thread) != 0);
}

/**
 * wpe_signal_forward_to_main - In a signal handler running on a library's
 * thread: hand the signal to the editor's thread instead.
 * @sig: the signal.
 *
 * The signal is then delivered on the editor's thread, at once or, while
 * that thread holds it blocked (the moment a child is forked and put on
 * the watch list), as soon as it unblocks it.  So a program that ends at
 * any moment is recorded once, by the editor's thread, and the user still
 * sees how it ended.
 *
 * pthread_kill is async-signal-safe since POSIX.1-2008 TC1.  On macOS and
 * NetBSD it may take the library's thread-list lock when the target is
 * another thread, so a library thread interrupted while holding that lock
 * could stall here; this is why wpe_library_threads_start keeps the known
 * library threads from taking SIGCHLD at all.
 */
static void wpe_signal_forward_to_main(int sig)
{
 (void)pthread_kill(wpe_main_thread, sig);
}

/**
 * wpe_library_threads_start - Let the graphical libraries start their
 * threads now, with SIGCHLD blocked, so the threads keep it blocked.
 *
 * A thread starts with the signal mask of the thread that created it, so
 * every thread made here never takes SIGCHLD at all, and the editor's own
 * thread is the only one the kernel can hand it to.  Done before the
 * window opens, when no program the editor runs has been forked yet; a
 * thread a library starts later is covered by wpe_signal_forward_to_main.
 */
#ifndef NO_XWINDOWS
static void wpe_library_threads_start(void)
{
#ifdef HAVE_PANGO
 sigset_t saved;

 WpeChildSignalBlock(&saved);
 wpe_render_cairo_start_threads();
 WpeChildSignalRestore(&saved);
#endif
}
#endif

/**
 * wpe_child_say_on_status_row - Where the children's owner and the loop's
 * writer speak: the status row of the window on top, so the user reads why
 * a program did not start, that one did not end, or that one stopped
 * reading its input.  Said while a box or a command the editor waits for
 * has the screen, the line waits for it and is kept until the next key.
 * @line: the line.
 */
static void wpe_child_say_on_status_row(const char *line)
{
 wpe_paint_trace_say(line);
 if (WpeEditor == NULL || WpeEditor->mxedt < 0)
  return;
 if (e_status_keep_until_free(line))
  return;
 e_status_say(WpeEditor->f[WpeEditor->mxedt], line);
}

/**
 * e_ini_children - Make the editor ready to start programs and to be told
 * when they end.
 *
 * Done before the display is opened: the graphical editor asks the desktop
 * for its font while it starts, a command the editor waits for, and a
 * command's end is only ever told by the SIGCHLD handler through the wake
 * pipe.  The pipe exists before the handler can write to it.
 */
static void e_ini_children(void)
{
 WpeChildWakeOpen();
 wpe_child_say_set(wpe_child_say_on_status_row);
 wpe_fd_write_say_set(wpe_child_say_on_status_row);
 wpe_child_sigchld_install(WpeSignalChild);
}

/**
 * e_capture_quiet_fd - While the editor waits for a short command: the
 * console's keyboard, whose keys stay queued for the editor; none for a
 * window, whose keys arrive with its other events.
 */
static int e_capture_quiet_fd(void)
{
 if (WpeIsXwin())
  return(-1);
 return(e_t_keyboard_fd());
}

/**
 * e_capture_serve_display - While the editor waits for a short command:
 * answer the window's repaint and resize events, keys and clicks left
 * queued.
 */
static void e_capture_serve_display(void)
{
 if (e_u_serve_display)
  (*e_u_serve_display)();
}

/**
 * e_capture_cancel_asked - While the editor waits for a short command: did
 * the user press Esc in the window?  The Esc is taken; the keys typed
 * before it stay queued for the editor.
 */
static int e_capture_cancel_asked(void)
{
 return(wpe_paint_take_key(WPE_ESC) == WPE_ESC);
}

/**
 * e_ini_children_display - Once the display is open: serve it while the
 * editor waits for a short command (the print command, a manual page, an
 * unpack), so a window covered or resized meanwhile is drawn again at once
 * and Esc ends the command.  The console's keyboard waits for the editor
 * meanwhile, as it always has.
 */
static void e_ini_children_display(void)
{
 if (WpeIsXwin())
  wpe_child_capture_display_set(e_capture_quiet_fd, e_capture_serve_display,
                                e_capture_cancel_asked);
 else
  wpe_child_capture_display_set(e_capture_quiet_fd, NULL, NULL);
}

int e_ini_unix(int *argc, char **argv)
{
 extern OPT opt[];
 int i, debug;
 struct sigaction act;
 int (*initfunc)(int *argc, char **argv);

 wpe_signal_main_thread_note();
 setlocale(LC_ALL, "");
 tcgetattr(0, &otermio);
 /* Arm the SIGTERM/SIGHUP handler HERE -- before WpeTermInit() below, whose
    e_t_initscr() ends in tcsetattr(0, TCSADRAIN, ...) and can BLOCK until the
    pty's output buffer drains.  If the handler were installed only after that
    (as it used to be), a `kill PID` arriving while init blocks would take the
    default disposition and leave the controlling terminal in raw mode with
    mouse tracking still on.  Until the editor has opened its windows there is
    nothing to save, so the handler ends the editor at once, restoring the
    terminal from `otermio` (read just above); afterwards it asks the editor's
    loop to save the unsaved windows and end.  (Crash signals are installed
    lower down; they have no such ordering dependency.  SIGCHLD is installed
    next, before the display opens, because opening it runs a command.)  The
    pipe that carries the request to the loop exists before the handler can
    write to it. */
 wpe_end_request_open();
 act.sa_handler = WpeSignalTerm;
 sigfillset(&act.sa_mask);
 act.sa_flags = 0;
 sigaction(SIGTERM, &act, NULL);
#ifdef SIGHUP
 sigaction(SIGHUP, &act, NULL);
#endif
 e_ini_children();
 u_fb = NULL;
 x_fb = NULL;
 debug = 0;
 for (i = 1; i < *argc; i++)
 {
  if (strcmp("--debug", argv[i]) == 0)
  {
   debug = 1;
  }
 }
 for (i = strlen(argv[0])-1; i >= 0 && *(argv[0] + i) != DIRC; i--)
  ;
#ifndef NO_XWINDOWS
 if (*(argv[0]+i+1) == 'x')
  e_we_sw = 1;
 else
  e_we_sw = 0;
#endif
#ifdef PROG
 if (!strncmp("wpe", (argv[0] + i + e_we_sw + 1), 3))
  e_we_sw |= 2;
#endif
#ifdef XWPE_DLL
 if (WpeIsXwin())
 {
  libxwpe = dlopen(LIBRARY_DIR"/libxwpe-x11.so", RTLD_NOW);
 }
 else
 {
  libxwpe = dlopen(LIBRARY_DIR"/libxwpe-term.so",RTLD_NOW);
 }
 if (!libxwpe)
 {
  printf("%s\n", dlerror());
  exit(0);
 }
 initfunc = dlsym(libxwpe, "WpeDllInit");
 if (initfunc)
 {
  (*initfunc)(argc, argv);
 }
 else
 {
  printf("%s\n", dlerror());
  exit(0);
 }
#else
#ifndef NO_XWINDOWS
 if (WpeIsXwin())
 {
  wpe_library_threads_start();
  e_gfx_backend = e_pick_gfx_backend();
#ifdef HAVE_WAYLAND
  /* Native Wayland first; if no compositor is reachable (or the native
     backend is not yet up for this build), WpeWaylandInit() returns non-zero
     and we fall back to X11 -- which transparently uses XWayland on a Wayland
     session -- so `xwpe` always opens a window. */
  if (e_gfx_backend == WPE_GFX_WAYLAND && WpeWaylandInit(argc, argv) == 0)
   ;
  else
  {
   e_gfx_backend = WPE_GFX_X11;
   WpeXtermInit(argc, argv);
  }
#else
  WpeXtermInit(argc, argv);
#endif
 }
 else
#endif
 {
  WpeTermInit(argc, argv);
 }
#endif
 e_ini_children_display();
 if (WpeIsProg())
 {
#ifndef DEBUGGER
  opt[0].x = 2, opt[1].x = 7, opt[2].x = 14; opt[3].x = 21; opt[4].x = 30;
  opt[9].t = opt[7].t; opt[9].x = 74; opt[9].s = opt[7].s; opt[9].as = opt[7].as;
  opt[8].t = opt[6].t; opt[8].x = 65; opt[8].s = opt[6].s; opt[8].as = opt[6].as;
  opt[7].t = opt[5].t; opt[7].x = 55; opt[7].s = opt[5].s; opt[7].as = opt[5].as;
  opt[6].t = "Project"; opt[6].x = 45; opt[6].s = 'P'; opt[6].as = AltP;
  opt[5].t = "Run"; opt[5].x = 38; opt[5].s = 'R'; opt[5].as = AltR;
  MENOPT = 10;
  e_mn_men = 2;
#else
  opt[0].x = 2, opt[1].x = 6, opt[2].x = 12; opt[3].x = 18; opt[4].x = 26;
  opt[10].t = opt[7].t; opt[10].x = 74; opt[10].s = opt[7].s; opt[10].as = opt[7].as;
  opt[9].t = opt[6].t; opt[9].x = 65; opt[9].s = opt[6].s; opt[9].as = opt[6].as;
  opt[8].t = opt[5].t; opt[8].x = 56; opt[8].s = opt[5].s; opt[8].as = opt[5].as;
  opt[7].t = "Project"; opt[7].x = 47; opt[7].s = 'P'; opt[7].as = AltP;
  opt[6].t = "Debug"; opt[6].x = 40; opt[6].s = 'D'; opt[6].as = AltD;
  opt[5].t = "Run"; opt[5].x = 34; opt[5].s = 'R'; opt[5].as = AltR;
  MENOPT = 11;
  e_mn_men = 1;
#endif
 }

 /* Unknown error signal handling */
 act.sa_handler = WpeSignalUnknown;
 sigfillset(&act.sa_mask); /* Mask all signals while running */
 act.sa_flags = 0;
 if (!debug)
 {
  sigaction(SIGQUIT, &act, NULL);
  sigaction(SIGILL, &act, NULL);
  sigaction(SIGABRT, &act, NULL);
  sigaction(SIGFPE, &act, NULL);
  sigaction(SIGSEGV, &act, NULL);
#ifdef SIGTRAP
  sigaction(SIGTRAP, &act, NULL);
#endif
#ifdef SIGIOT
  sigaction(SIGIOT, &act, NULL);
#endif
#ifdef SIGBUS
  sigaction(SIGBUS, &act, NULL);
#endif
 }
 /* Ignore SIGINT */
 act.sa_handler = SIG_IGN;
 sigaction(SIGINT, &act, NULL);
 /* Ignore SIGPIPE process-wide: writes to a child that has exited (a crashed
    or fake LSP server, a debugger that quit) return -1 with errno=EPIPE,
    which the loop's writer reports as the child's input closed (the fd loop
    reaps the dead session).  Without this xwpe gets SIGPIPE-killed -- on
    macOS the default action is termination, which made
    test_dead_server_does_not_spin show up as the editor itself dying
    instead of cleanly tearing the session down. */
 sigaction(SIGPIPE, &act, NULL);
 return(*argc);
}

int e_abs_refr()
{
 memset(altschirm, 0, sizeof(SCREENCELL) * MAXSCOL * MAXSLNS);
 return(0);
}

void e_refresh_area(int x, int y, int width, int height)
{
 int i,j;

 if (width + x > MAXSCOL)
  width = MAXSCOL - x;
 if (height + y > MAXSLNS)
  height = MAXSLNS - y;
 for (j = 0; j < height; j++)
  for (i = 0; i < width; i++)
   memset(&altschirm[(y+j) * MAXSCOL + (x+i)], 0, sizeof(SCREENCELL));
}

int e_tast_sim(int c)
{
 if (c >= 'A' && c <= 'Z')
  return(c + 1024 - 'A');
 switch(c)
 {
  case 'a':  return(AltA);
  case 'b':  return(AltB);
  case 'c':  return(AltC);
  case 'd':  return(AltD);
  case 'e':  return(AltE);
  case 'f':  return(AltF);
  case 'g':  return(AltG);
  case 'h':  return(AltH);
  case 'i':  return(AltI);
  case 'j':  return(AltJ);
  case 'k':  return(AltK);
  case 'l':  return(AltL);
  case 'm':  return(AltM);
  case 'n':  return(AltN);
  case 'o':  return(AltO);
  case 'p':  return(AltP);
  case 'q':  return(AltQ);
  case 'r':  return(AltR);
  case 's':  return(AltS);
  case 't':  return(AltT);
  case 'u':  return(AltU);
  case 'v':  return(AltV);
  case 'w':  return(AltW);
  case 'x':  return(AltX);
  case 'y':  return(AltY);
  case 'z':  return(AltZ);
  case '1':  return(Alt1);
  case '2':  return(Alt2);
  case '3':  return(Alt3);
  case '4':  return(Alt4);
  case '5':  return(Alt5);
  case '6':  return(Alt6);
  case '7':  return(Alt7);
  case '8':  return(Alt8);
  case '9':  return(Alt9);
  case '0':  return(Alt0);
  case ' ':  return(AltBl);
  case '#':  return(AltSYS);
  case CtrlA:  return(CBUP);
  case CtrlE:  return(CBDO);
  case CtrlB:  return(CCLE);
  case CtrlF:  return(CCRI);
  case CtrlP:  return(CPS1);
  case CtrlN:  return(CEND);
  case CtrlH:  return(AltBS);
  default:  return(0);
 }
}

/* Every child the editor started leads a group or a session of its own, so a
   signal that ends the editor does not reach it: the handlers below end
   them all (wpe_child_end_from_handler), as a crash, a SIGTERM or a closed
   terminal would otherwise leave a build, a debugger or a server running
   unseen. */

/**
 * wpe_end_children_from_handler - From a handler: end the editor's
 * children, unless this is a child forked from the editor that has not run
 * its program yet (the table is the editor's, and its children are not this
 * process's to end).  Async-signal-safe.
 */
static void wpe_end_children_from_handler(void)
{
 if (wpe_main_pid != 0 && getpid() != wpe_main_pid)
  return;
 wpe_child_end_from_handler();
}

void WpeSignalUnknown(int sig)
{
 wpe_end_children_from_handler();
 int fd = open("/tmp/xwpe-crash.txt", O_WRONLY|O_CREAT|O_TRUNC, 0644);
 if (fd >= 0)
 {
  char line[256];
  int len;
  void *bt[32];
  int n;

  len = snprintf(line, sizeof(line), "CRASH: signal %d\n", sig);
  write(fd, line, len);
  n = backtrace(bt, 32);
  backtrace_symbols_fd(bt, n, fd);
  len = snprintf(line, sizeof(line), "MAXSLNS=%d MAXSCOL=%d\n", MAXSLNS, MAXSCOL);
  write(fd, line, len);
  close(fd);
 }
 write(STDERR_FILENO, "\033[?1002l\033[?1006l\033[?1000l", 24);
 tcsetattr(0, TCSANOW, &otermio);
 signal(sig, SIG_DFL);
 raise(sig);
}

/* The children's owner is we_child.c: their slots, the pipe that wakes the
   loop when one ends, and the ring for the unwatched.  The handler below is
   the one place that reaps, and hands each reaped child to it. */

/* The end-request pipe below: no descriptor yet, and what one read takes. */
#define WPE_CHILD_WAKE_NONE (-1)
#define WPE_CHILD_WAKE_DRAIN 64

void WpeSignalChild(int sig)
{
 int statloc, saved_errno = errno;
 pid_t pid;

 if (!wpe_signal_on_main_thread())
 {
  /* A library's thread took it: the tables below belong to the editor's
     thread, which may hold SIGCHLD blocked right now to keep a new child
     out of them until it is watched. */
  wpe_signal_forward_to_main(sig);
  errno = saved_errno;
  return;
 }
 /* Use WNOHANG to avoid blocking, and loop to reap all zombies.
    The old wait() call would steal children from pclose/waitpid,
    which made a pclose of a popen'ed command hang.
    errno is kept for the code this signal interrupted.  A watched child
    that ended wakes the loop, which calls its owner back. */
 while ((pid = waitpid(-1, &statloc, WNOHANG)) > 0)
  wpe_child_note_ended(pid, statloc);
 errno = saved_errno;
}

/**
 * wpe_end_now - In a signal handler: end the editor at once, keeping nothing.
 * @sig: the SIGTERM or SIGHUP that asked for the end.
 *
 * The way out when the editor cannot take its time: the signal came before
 * the editor was set up, in a forked child before it ran its program, or a
 * second time while the first one's save was still running (a save that
 * hangs must never leave the editor impossible to end).  Only
 * async-signal-safe work: every child is killed, the mouse tracking turned
 * off and the terminal modes restored, so the shell is usable again, and
 * the signal is raised again with its default action, so the parent sees
 * the usual "killed by SIGTERM".
 */
static void wpe_end_now(int sig)
{
 const char *mouse_off = "\033[?1002l\033[?1006l\033[?1000l";
 ssize_t n;

 wpe_end_children_from_handler();
 n = write(STDERR_FILENO, mouse_off, 24);
 (void)n;
 tcsetattr(0, TCSANOW, &otermio);
 signal(sig, SIG_DFL);
 raise(sig);
}

/* A SIGTERM or SIGHUP asks the editor to end.  The handler only records
   which signal came and writes one byte to this pipe.  The fd loop watches
   the read end from start-up to exit; the key read of all three displays, a
   dialog, the debugger's answer and a running program or build all wait in
   that loop, so the editor wakes at once there and ends with the emergency
   save.  A debug adapter's answers and its Quit are read on the loop too,
   so the save comes first, and the editor's end then keeps the loop
   running, bounded, until the adapter has ended its program;
   the assistant's synchronous wait asks wpe_end_requested
   on every slice.  The other waits outside the loop are bounded by a
   deadline (language server and build-server replies, web fetches, tool
   commands) or by the user (a program run in the foreground ends the
   editor once it ends).  The pipe never fires otherwise, so it never wakes
   an idle editor. */
#define WPE_END_NONE 0
static volatile sig_atomic_t wpe_end_signal = WPE_END_NONE;
static int wpe_end_wake_rd = WPE_CHILD_WAKE_NONE;
static int wpe_end_wake_wr = WPE_CHILD_WAKE_NONE;

static void wpe_end_on_wake(int fd, void *data);

/**
 * wpe_end_request_open - Make the pipe a SIGTERM or SIGHUP wakes the editor
 * through, and put it on the fd loop.
 *
 * Called once at start-up, before the handler is installed.  Without a
 * pipe (none could be made, or the loop is full) the handler ends the
 * editor at once as it did before, without the save, so the editor still
 * starts and still ends on the signal.
 */
static void wpe_end_request_open(void)
{
 int fds[2];

 if (wpe_end_wake_rd != WPE_CHILD_WAKE_NONE || pipe(fds) < 0)
  return;
 if (wpe_child_fd_private(fds[0]) < 0 || wpe_child_fd_private(fds[1]) < 0
   || wpe_fd_add(fds[0], POLLIN, wpe_end_on_wake, NULL) < 0)
 {
  close(fds[0]);
  close(fds[1]);
  return;
 }
 wpe_end_wake_rd = fds[0];
 wpe_end_wake_wr = fds[1];
}

/**
 * wpe_end_requested - Has a SIGTERM or SIGHUP asked the editor to end?
 *
 * For a wait outside the fd loop that could otherwise hold the editor (an
 * assistant reply, a debug adapter that never answers): it gives up when
 * this says yes, and the next fd-loop wait saves the user's work and ends.
 * Return: 1 once the end was asked for, else 0.
 */
int wpe_end_requested(void)
{
 return(wpe_end_signal != WPE_END_NONE);
}

/**
 * wpe_end_wake - In the handler: wake the editor's loop for the end request.
 *
 * One write() to a non-blocking pipe.  When it fails the pipe is full, so
 * a wake-up is waiting in it already.
 */
static void wpe_end_wake(void)
{
 static const char wake = 1;

 if (write(wpe_end_wake_wr, &wake, 1) < 0)
  return;
}

/**
 * wpe_editor_is_up - Has the editor opened its windows yet?
 *
 * Before that there is nothing to save, and the editor may be waiting
 * outside its loop (the terminal set-up, the Wayland window's first
 * configure), so an end asked for then takes the immediate way out.  Safe
 * in the handler: two plain reads of values the start-up writes once.
 */
static int wpe_editor_is_up(void)
{
 return(WpeEditor != NULL && WpeEditor->mxedt >= 0);
}

/**
 * wpe_end_can_wait - In the handler: may this signal be left to the loop?
 *
 * Not in a child forked from the editor before it runs its program (the
 * pipe is still open there, and the parent must not end for the child's
 * signal), not without a pipe, not before the editor is up, and not when
 * an end was asked for already: the user or the system that sends a second
 * signal wants the editor gone now, even if the save of the first one
 * hangs.
 * Return: 1 when the loop will end the editor, 0 when the handler must.
 */
static int wpe_end_can_wait(void)
{
 if (wpe_main_pid != 0 && getpid() != wpe_main_pid)
  return(0);
 if (wpe_end_wake_wr == WPE_CHILD_WAKE_NONE)
  return(0);
 if (!wpe_editor_is_up())
  return(0);
 return(wpe_end_signal == WPE_END_NONE);
}

void WpeSignalTerm(int sig)
{
 int saved_errno = errno;

 if (!wpe_signal_on_main_thread())
 {
  /* A library's thread took it: the editor's own thread ends the editor. */
  wpe_signal_forward_to_main(sig);
  errno = saved_errno;
  return;
 }
 if (!wpe_end_can_wait())
 {
  wpe_end_now(sig);
  return;
 }
 wpe_end_signal = sig;
 wpe_end_wake();
 errno = saved_errno;
}

/* The emergency save writes each window with unsaved changes to its .ESV
   file.  What it did is kept here and told once the terminal is restored,
   where the user can still read it after the editor is gone. */
#define E_ESV_SUFFIX ".ESV"
/* Shortest file-name limit that can hold an .ESV name; below it the file
   system is a DOS one, where the suffix would replace the extension. */
#define E_ESV_MIN_NAME_MAX 12
static struct {
 const char *name;
 int saved;
} e_esv_result[MAXEDT + 1];
static int e_esv_results = 0;
static int e_esv_started = 0;
static int e_esv_running = 0;

/**
 * e_emergency_save_running - Is the editor writing its .ESV files?
 *
 * While it is, nobody is there to answer a box: the editor is ending on an
 * error or a signal, and the terminal may be gone.  An error box then
 * neither paints nor waits for a key, so one file that cannot be written
 * does not stop the others from being saved, nor the editor from ending.
 * Return: 1 during the save, else 0.
 */
int e_emergency_save_running(void)
{
 return(e_esv_running);
}

/**
 * e_err_save_window - Write one window with unsaved changes to its .ESV
 * file, beside its own file.
 * @f: a text window with unsaved changes.
 *
 * The user's work survives the end of the editor, and "wpe -r" brings it
 * back.  No backup is made: the .ESV is not the user's file, and the
 * backup of an .ESV would be named like the backup of the file itself.
 * Return: 0 when it was written, -1 when it could not be.
 */
static int e_err_save_window(FENSTER *f)
{
 long maxname = pathconf(f->dirct, _PC_NAME_MAX);
 char *esv;

 if (maxname < (long)(strlen(f->datnam) + strlen(E_ESV_SUFFIX)))
  return(-1);
 if (maxname <= E_ESV_MIN_NAME_MAX)
  return(-1);
 esv = MALLOC(strlen(f->datnam) + strlen(E_ESV_SUFFIX) + 1);
 if (!esv)
  return(-1);
 sprintf(esv, "%s%s", f->datnam, E_ESV_SUFFIX);
 FREE(f->datnam);
 f->datnam = esv;
 if (e_write(0, 0, f->b->mx.x, f->b->mxlines - 1, f, WPE_NOBACKUP) == WPE_ESC)
  return(-1);
 return(0);
}

/**
 * e_err_save_wanted - Does this window hold work the emergency save keeps?
 * @f: a window of the editor.
 * Return: 1 for a text window with unsaved changes and some text, else 0.
 */
static int e_err_save_wanted(FENSTER *f)
{
 BUFFER *b = f->b;

 if (!DTMD_ISTEXT(f->dtmd) || !f->save)
  return(0);
 return(b->mxlines > 1 || b->bf[0].len > 0);
}

/**
 * e_err_save_note - Keep what the save did with one window for the report.
 * @f: the window; @saved: whether its .ESV was written.
 */
static void e_err_save_note(FENSTER *f, int saved)
{
 if (e_esv_results > MAXEDT)
  return;
 e_esv_result[e_esv_results].name = f->datnam;
 e_esv_result[e_esv_results].saved = saved;
 e_esv_results++;
}

void e_err_save()
{
 ECNT *cn = WpeEditor;
 int i;

 /* Once per run: the error exit and a signal can both reach it. */
 if (e_esv_started || !cn)
  return;
 e_esv_started = 1;
 e_esv_running = 1;
 for (i = 0; i <= cn->mxedt; i++)
 {
  if (!e_err_save_wanted(cn->f[i]))
   continue;
  e_err_save_note(cn->f[i], e_err_save_window(cn->f[i]) == 0);
 }
 e_esv_running = 0;
}

/**
 * e_err_save_report - Tell the user which .ESV files were written.
 *
 * Printed after the terminal is restored, so it stays on the screen the
 * user returns to; a file that could not be saved is named too.
 */
static void e_err_save_report(void)
{
 int i;

 for (i = 0; i < e_esv_results; i++)
 {
  if (e_esv_result[i].saved)
   printf("File %s saved!\n", e_esv_result[i].name);
  else
   printf("File %s could not be saved!\n", e_esv_result[i].name);
 }
}

#ifdef DEBUGGER
/* The longest the editor's end waits for the children it is ending.  An
   adapter's way out is the longest (no answer to `disconnect`, the grace,
   SIGKILL and its kill grace); a language server's, the build server's and
   a setup command's are the owner's ladder alone, shorter, and all of them
   run at once. */
#define E_EXIT_CHILDREN_MAX_MS E_DAP_CLOSE_MAX_MS

/**
 * e_exit_children_gone - Have the children the editor's end is ending gone?
 *
 * Every debug adapter Quit ended, and every child whose end the owner's
 * ladder still runs (a language server, the build server, a setup command).
 * Return: 1 once none is left.
 */
static int e_exit_children_gone(void)
{
 extern int e_d_dap_all_closed(void);

 return(e_d_dap_all_closed() && wpe_child_ending() == 0);
}
#endif

/**
 * e_exit_teardown - Stop what the editor started and give the terminal back.
 *
 * The debugger, the language server and a running build are ended, so none
 * of them outlives the editor, and the display is closed and the terminal
 * modes restored.
 */
static void e_exit_teardown(void)
{
#ifdef DEBUGGER
 extern void e_lsp_ui_shutdown(void);
 extern void e_d_dap_end_at_exit(void);
 extern void e_d_end_at_exit(void);

 /* A debug adapter is told to end its program, a line debugger to quit
    (so it ends its program itself), and the language server (Metals etc.)
    to shut down and then ended, so its JVM does not linger after xwpe
    quits; the work was saved before this.  All of them then get one bounded
    time to go, on the loop: one that ignores what it was told is killed
    after the grace of the editor's children's owner, well within it. */
 e_d_dap_end_at_exit();
 e_d_end_at_exit();
 e_lsp_ui_shutdown();
 if (!e_exit_children_gone())
  wpe_fd_end_wait(e_exit_children_gone, E_EXIT_CHILDREN_MAX_MS);
#endif
#ifdef PROG
 /* A build in its own process group would outlive the editor. */
 e_build_end_at_exit();
#endif
 /* Whatever is left (a child that did not go in time, one nothing above
    knows how to ask) is killed with its group, without a wait. */
 wpe_child_end_from_handler();
 (*WpeDisplayEnd)();
 e_switch_screen(0);
 tcsetattr(0, TCSADRAIN, &otermio);
 fputs("\033c", stderr);
}

static int e_exit_started = 0;

/**
 * e_exit_session - The one way the editor ends.
 * @keep_work: write the windows with unsaved changes to .ESV files first.
 *
 * The save comes before anything that can take time (the language server
 * gets seconds to stop) or fail (a display that is gone), so the user's
 * work is on disk even if the end is cut short.
 */
static void e_exit_session(int keep_work)
{
 e_exit_started = 1;
 if (keep_work)
  e_err_save();
 e_exit_teardown();
}

void e_exit(int n)
{
 e_exit_session(n != 0);
 if (n != 0)
 {
  printf("\nError-Exit!   Code: %d!\n", n);
  e_err_save_report();
 }
 exit(n);
}

/**
 * wpe_end_by_default - Finish ending the editor the way the signal would.
 * @sig: the SIGTERM or SIGHUP that asked for the end.
 *
 * The parent (a shell, a session manager) then sees the editor killed by
 * that signal, as it did before the editor learned to save on it.
 */
static void wpe_end_by_default(int sig)
{
 sigset_t one;

 fflush(NULL);
 signal(sig, SIG_DFL);
 sigemptyset(&one);
 sigaddset(&one, sig);
 pthread_sigmask(SIG_UNBLOCK, &one, NULL);
 raise(sig);
 _exit(1);
}

/**
 * wpe_end_on_wake - The fd loop's callback for the end request.
 * @fd: the pipe's read end; @data: unused.
 *
 * Runs wherever the editor waits, a dialog or the debugger included, so a
 * "kill PID", a logout or a closed terminal window ends the editor at once
 * with the windows that had unsaved changes written to .ESV files, which
 * "wpe -r" reopens.  An end already under way (an error exit, the input
 * gone) is left to finish its own save.
 */
static void wpe_end_on_wake(int fd, void *data)
{
 char buf[WPE_CHILD_WAKE_DRAIN];
 int sig;

 (void)data;
 while (read(fd, buf, sizeof buf) > 0)
  ;
 sig = wpe_end_signal;
 if (sig == WPE_END_NONE || e_exit_started)
  return;
 e_exit_session(1);
 e_err_save_report();
 wpe_end_by_default(sig);
}

char *e_mkfilepath(char *dr, char *fn, char *fl)
{
 strcpy(fl, dr);
 if (dr[strlen(dr)-1] != DIRC)
 {
  strcat(fl, DIRS);
 }
 strcat(fl, fn);
 return(fl);
}

int e_compstr(char *a, char *b)
{
 int n, k;
 char *ctmp, *cp;

 if (a[0] == '*' && !a[1])
  return(0);
 if (!a[0] || !b[0])
  return(a[0] - b[0]);
 if (a[0] == '*' && a[1] == '*')
  return(e_compstr(++a, b));
 for (n = a[0] == '*' ? 2 : 1;
   a[n] != '*' && a[n] != '?' && a[n] != '[' && a[n]; n++)
  ;
 if (a[0] == '*')
 {
  n--;
  a++;
  if (a[0] == '?')
  {
   cp = MALLOC((strlen(a)+1)*sizeof(char));
   strcpy(cp, a);
   cp[0] = '*';
   n = e_compstr(cp, ++b);
   FREE(cp);
   return(n);
  }
  else if (a[0] == '[')
  {
   while (*b && (n = e_compstr(a, b)))
    b++;
   return(n);
  }
  ctmp = MALLOC(n+1);
  for (k = 0; k < n; k++)
   ctmp[k] = a[k];
  ctmp[n] = '\0';
  cp = strstr(b, ctmp);
  FREE(ctmp);
  if (cp == NULL)
   return((a[0] - b[0]) ? a[0] - b[0] : -1);
  if (!a[n] && !cp[n])
   return(0);
  if (!a[n])
   return(e_compstr(a-1, cp+1));
  if (!(k = e_compstr(a+n, cp+n)))
   return(0);
  return(e_compstr(a-1, cp+1));
 }
 else if (a[0] == '?')
 {
  n--;  a++;  b++;
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
   n-=(k+1);  a+=(k+1);  b++;
  }
  else
  {
   for (k = 1; a[k] && (a[k] != ']' || k == 1) && a[k] != b[0]; k++)
    if (a[k+1] == '-' && b[0] >= a[k] && b[0] <= a[k+2])
     break;
   if (a[k] == ']' || a[k] == '\0')
    return(-b[0]);
   for (; a[k] && (a[k] != ']'); k++)
    ;
   n-=(k+1);  a+=(k+1);  b++;
  }
 }
 if (n <= 0)
  return(e_compstr(a, b));
 if ((k = strncmp(a, b, n)) != 0)
  return(k);
 return(e_compstr(a+n, b+n));
}

struct dirfile *e_find_files(char *sufile, int sw)
{
 char           *stmp, *tmpst, *sfile, *sdir;
 struct dirfile *df = MALLOC(sizeof(struct dirfile));
 DIR            *dirp;
 struct dirent  *dp;
 struct stat     buf;
 struct stat     lbuf;
 int             i, n, cexist, sizeStmp, sizeSdir;

 df->name = NULL;
 df->anz = 0;
 for (n = strlen(sufile); n >= 0 && sufile[n] != DIRC; n--);
 sfile = sufile + 1 + n;
 if (n <= 0)
 {
  sizeSdir = 2;
  sdir = (char *)WpeMalloc(2 * sizeof(char));
  sdir[0] = n ? '.' : DIRC;
  sdir[1] = '\0';
 }
 else
 {
  sizeSdir = n + 1;
  sdir = (char *)WpeMalloc((n + 1) * sizeof(char));
  for (i = 0; i < n; i++)
   sdir[i] = sufile[i];
  sdir[n] = '\0';
 }
 if (!(dirp = opendir(sdir)))
 {
  FREE(sdir);
  return(df);
 }
 sizeStmp = 256;
 stmp = (char *)WpeMalloc(sizeStmp);
 while((dp = readdir(dirp)) != NULL)
 {
  if (!(sw & 1) && dp->d_name[0] == '.' && sfile[0] != '.')
   continue;
  if (!e_compstr(sfile, dp->d_name))
  {
   if (sizeSdir + strlen(dp->d_name) + 10 > sizeStmp)
   {
    while (sizeSdir + strlen(dp->d_name) + 10 > sizeStmp)
     sizeStmp <<= 1;
    stmp = (char *)WpeRealloc(stmp, sizeStmp);
   }

   e_mkfilepath(sdir, dp->d_name, stmp);
   lstat(stmp, &lbuf);
   stat(stmp, &buf);

   /* check existence of the file */
   cexist = access(stmp, F_OK);

   /* we accept it as a file if
      - a regular file
      - link and it does not point to anything
      - link and it points to a non-directory */
   if ((S_ISREG(buf.st_mode) ||
     (S_ISLNK(lbuf.st_mode) &&
     (cexist || (cexist == 0 && !S_ISDIR(buf.st_mode))))) &&
     (!(sw & 2) || (buf.st_mode & 0111)) )
   {
    if (df->anz == 0)
     df->name = MALLOC((df->anz + 1) * sizeof(char *));
    else
     df->name = REALLOC(df->name, (df->anz + 1) * sizeof(char *));
    if (df->name == NULL || !(tmpst = MALLOC(strlen(dp->d_name) + 1)))
    {
     df->anz = 0;
     closedir(dirp);
     WpeFree(stmp);
     WpeFree(sdir);
     return(df);
    }
    strcpy(tmpst, dp->d_name);
    for (n = df->anz; n > 0 && strcmp(*(df->name + n - 1), tmpst) > 0; n--)
     *(df->name + n) = *(df->name + n - 1);
    *(df->name + n) = tmpst;
    (df->anz)++;
   }
  }
 }
 closedir(dirp);
 WpeFree(stmp);
 WpeFree(sdir);
 return(df);
}

struct dirfile *e_find_dir(char *sufile, int sw)
{
 char           *stmp, *tmpst, *sfile, *sdir;
 struct dirfile *df = MALLOC(sizeof(struct dirfile));
 DIR            *dirp;
 struct dirent  *dp;
 struct stat     buf;
 int             i, n, sizeStmp, sizeSdir;

 df->name = NULL;
 df->anz = 0;
 for (n = strlen(sufile); n >= 0 && sufile[n] != DIRC; n--);
 sfile = sufile + 1;
 sfile = sfile + n;
 if (n <= 0)
 {
  sizeSdir = 2;
  sdir = MALLOC(2 * sizeof(char));
  sdir[0] = n ? '.' : DIRC;
  sdir[1] = '\0';
 }
 else
 {
  sizeSdir = n + 1;
  sdir = MALLOC((n + 1) * sizeof(char));
  for (i = 0; i < n; i++)
   sdir[i] = sufile[i];
  sdir[n] = '\0';
 }
 if (!(dirp = opendir(sdir)))
 {
  FREE(sdir);
  return(df);
 }
 sizeStmp = 256;
 stmp = (char *)WpeMalloc(sizeStmp);
 while ((dp = readdir(dirp)) != NULL)
 {
  if (!sw && dp->d_name[0] == '.' && sfile[0] != '.')
   continue;
  if (!e_compstr(sfile, dp->d_name) && strcmp(dp->d_name, ".") &&
    strcmp(dp->d_name, ".."))
  {
   if (sizeSdir + strlen(dp->d_name) + 10 > sizeStmp)
   {
    while (sizeSdir + strlen(dp->d_name) + 10 > sizeStmp)
     sizeStmp <<= 1;
    stmp = (char *)WpeRealloc(stmp, sizeStmp);
   }
   stat(e_mkfilepath(sdir, dp->d_name, stmp), &buf);

   /* we accept _only_ real, existing directories */
   if (S_ISDIR(buf.st_mode))
   {

    if (df->anz == 0)
     df->name = MALLOC((df->anz + 1) * sizeof(char *));
    else
     df->name = REALLOC(df->name, (df->anz + 1) * sizeof(char *));
    if (df->name == NULL || !(tmpst = MALLOC(strlen(dp->d_name) + 1)))
    {
     df->anz = 0;
     closedir(dirp);
     FREE(sdir);
     WpeFree(stmp);
     return(df);
    }
    strcpy(tmpst, dp->d_name);
    for (n = df->anz; n > 0 && strcmp(*(df->name + n - 1), tmpst) > 0; n--)
     *(df->name + n) = *(df->name + n - 1);
    *(df->name + n) = tmpst;
    (df->anz)++;
   }
  }
 }
 closedir(dirp);
 FREE(sdir);
 WpeFree(stmp);
 return(df);
}

#include <time.h>

char *e_file_info(char *filen, char *str, int *num, int sw)
{
 struct tm *ttm;
 struct stat buf[1];

 stat(filen, buf);
 ttm = localtime(&(buf->st_mtime));
 sprintf(str, "%c%c%c%c%c%c%c%c%c%c  %-13s  %6ld  %2.2u.%2.2u.%4.4u  %2.2u.%2.2u",
   buf->st_mode & 040000 ? 'd' : '-',
   buf->st_mode & 0400 ? 'r' : '-',
   buf->st_mode & 0200 ? 'w' : '-',
   buf->st_mode & 0100 ? 'x' : '-',
   buf->st_mode & 040 ? 'r' : '-',
   buf->st_mode & 020 ? 'w' : '-',
   buf->st_mode & 010 ? 'x' : '-',
   buf->st_mode & 04 ? 'r' : '-',
   buf->st_mode & 02 ? 'w' : '-',
   buf->st_mode & 01 ? 'x' : '-',
   filen,
   buf->st_size, ttm->tm_mday,
   ttm->tm_mon + 1, ttm->tm_year + 1900, ttm->tm_hour, ttm->tm_min);
 if (sw & 1) *num = buf->st_mtime;
 else if (sw & 2) *num = buf->st_size;
 return(str);
}

void ini_repaint(ECNT *cn)
{
 e_cls(cn->fb->df.fb, cn->fb->dc);
 e_ini_desk(cn);
}

void end_repaint()
{
   e_refresh();
}

int e_recover(ECNT *cn)
{
 struct dirfile *files;
 FENSTER *f = NULL;
 BUFFER *b;
 SCHIRM *s;
 int i;

 files = e_find_files("*.ESV", 1);
 for (i = 0; i < files->anz; i++)
 {
  e_edit(cn, files->name[i]);
  f = cn->f[cn->mxedt];
  f->datnam[strlen(f->datnam)-4] = '\0';
  if (!strcmp(f->datnam, BUFFER_NAME))
  {
   s = cn->f[cn->mxedt]->s;
   b = cn->f[cn->mxedt]->b;
   s->mark_end.y = b->mxlines - 1;
   s->mark_end.x = b->bf[b->mxlines-1].len;
   e_edt_copy(f);
   e_close_window(f);
  }
  else f->save = 1;
#ifdef PROG
  if (WpeIsProg()) e_add_synt_tl(f->datnam, f);
#endif
  if ((f->ed->edopt & ED_ALWAYS_AUTO_INDENT) ||
    ((f->ed->edopt & ED_SOURCE_AUTO_INDENT) && f->c_st)) 
   f->flg = 1;
 }
 freedf(files);
 return(0);
}

int e_frb_t_menue(int sw, int xa, int ya, FENSTER *f, int md)
{
 COLOR *frb = &(f->fb->er);
 int i, j, y, c=1, fb, fsv;

 if (md == 1) sw += 11;
 else if (md == 2) sw += 16;
 else if (md == 3) sw += 32;
 else if (md == COLOR_MODE_AI) sw += COLOR_AI_FIRST_ENTRY;
 fsv = fb = frb[sw].fb;
 if (fb == 0) y = 0;
 else
  for (y = 1, j = fb; j > 1; y++)
   j /= 2;
 do
 {
  if (c == CDO) y = y < 6 ? y + 1 : 0;
  else if (c == CUP) y = y > 0 ? y - 1 : 6;
  if (y == 0) fb = 0;
  else
   for (i = 1, fb = 1; i < y; i++)
    fb *= 2;
  frb[sw] = e_n_clr(fb);
  e_pr_t_col_kasten(xa, ya, fb, fb, f, 1);
  e_pr_ed_beispiel(1, 2, f, sw, md);
#if  MOUSE
  if ((c=wpe_paint_modal_getch()) == -1) c = e_opt_ck_mouse(xa, ya, md);
#else
  c = wpe_paint_modal_getch();
#endif
 } while (c != WPE_ESC && c != WPE_CR && c > -2);
 if (c == WPE_ESC || c < -1) frb[sw] = e_n_clr(fsv);
 return(frb[sw].fb);
}

/*   draw colors box  */
void e_pr_t_col_kasten(int xa, int ya, int x, int y, FENSTER * f, int sw)
{
 int rfrb, xe = xa + 14, ye = ya + 8;

 if (x == 0) y = 0;
 else
  for (rfrb = x, y = 1; rfrb > 1; y++)
   rfrb /= 2;
 rfrb = sw == 0 ? f->fb->nt.fb : f->fb->fs.fb;
 e_std_rahmen(xa, ya, xe, ye, "Colors", 0, rfrb, 0);
/*     e_pr_str((xa+xe-8)/2, ya, "Colors", rfrb, 0, 1, 
                                        f->fb->ms.f+16*(rfrb/16), 0);
*/
 e_pr_nstr(xa+2, ya+1, xe-xa-1, "A_NORMAL   ", 0, 0);
 e_pr_nstr(xa+2, ya+2, xe-xa-1, "A_STANDOUT ", A_STANDOUT, A_STANDOUT);
 e_pr_nstr(xa+2, ya+3, xe-xa-1, "A_UNDERLINE", A_UNDERLINE, A_UNDERLINE);
 e_pr_nstr(xa+2, ya+4, xe-xa-1, "A_REVERSE  ", A_REVERSE, A_REVERSE);
 e_pr_nstr(xa+2, ya+5, xe-xa-1, "A_BLINK    ", A_BLINK, A_BLINK);
 e_pr_nstr(xa+2, ya+6, xe-xa-1, "A_DIM      ", A_DIM, A_DIM);
 e_pr_nstr(xa+2, ya+7, xe-xa-1, "A_BOLD     ", A_BOLD, A_BOLD);

 fk_locate(xa+4, ya + y + 1);
}

