/* we_child.h -- the editor's children: started, watched and ended in one place.
 *
 * A process the editor starts is forked, signalled and ended here.  Its end
 * is reported once, from the fd loop, to the callback given when it was
 * started; nobody waits for a child, and nobody asks waitpid whether one
 * ended.  The one waitpid stays in the SIGCHLD handler (we_unix.c), which
 * hands each reaped child to wpe_child_note_ended.
 *
 * Pure C, no display header: the C tests link it with we_fdloop.c alone.
 */
#ifndef WE_CHILD_H
#define WE_CHILD_H

#include <signal.h>
#include <stddef.h>
#include <sys/types.h>

struct WpeStartPlan;

/* At most sixteen at once: the debugger or an adapter, jdb's program, four
   adapters closing, the build, two language servers (one ending), the build
   server and its setup, the AI host, its tool command and the CLI, a Run and
   a short capture.  Twice that, so a start is never refused in practice. */
#define WPE_CHILD_MAX            32
#define WPE_CHILD_NO_FD          (-1)
#define WPE_CHILD_END_GRACE_MS   2000    /* after the polite signal, before SIGKILL */
#define WPE_CHILD_END_KILL_MS    1000    /* after SIGKILL, before "did not end" */
#define WPE_CHILD_NO_SIGNAL      0       /* wpe_child_end: the caller already said "quit" on the pipe */
#define WPE_CHILD_NAMED_POLL_MS  100     /* wpe_child_end_named: how often a named process is looked at */
#define WPE_CHILD_INIT_PID       1       /* wpe_child_end_named: init, and every lower number, is never ended */

/* How a child ended, for WpeChildEnded and the callbacks. */
#define WPE_CHILD_RUNS          0   /* still running */
#define WPE_CHILD_ENDED         1   /* ended, status known */
#define WPE_CHILD_ENDED_UNKNOWN 2   /* ended, status lost, or given up on */

/* The ladder of wpe_child_end. */
#define WPE_CHILD_END_NONE      0
#define WPE_CHILD_END_POLITE    1   /* the polite signal went out; the grace runs */
#define WPE_CHILD_END_KILL      2   /* SIGKILL went out; the kill grace runs */
#define WPE_CHILD_END_GIVEN_UP  3   /* it did not end; reported as such */

typedef enum {
 WPE_CHILD_IO_PIPES,             /* stdin/stdout pipes, stderr as the spec says */
 WPE_CHILD_IO_PTY,               /* one pty on 0/1/2, its controlling terminal:
                                    dlv, whose program runs on it */
 WPE_CHILD_IO_PIPES_AND_PTY,     /* command pipes, and a pty beside them for the
                                    program the child starts: a line debugger,
                                    which names the pty to its program */
 WPE_CHILD_IO_INHERIT_AND_PTY,   /* the editor's own 0/1/2, and the same pty:
                                    the graphical editors' debugger script */
 WPE_CHILD_IO_TTY_PATH,          /* a terminal the editor has, on 0/1/2: jdb's
                                    program, on the session's pty */
 WPE_CHILD_IO_INHERIT            /* the editor's own 0/1/2, no pipe and no pty:
                                    a command the user watches on the terminal
                                    the editor handed over, or in a window of
                                    its own */
} WpeChildIo;

typedef enum {
 WPE_CHILD_OWN_SESSION,          /* setsid: a signal to -pid ends the pipeline */
 WPE_CHILD_OWN_GROUP,            /* setpgid on both sides: the build */
 WPE_CHILD_EDITORS_GROUP         /* stays in the editor's group and session: the
                                    graphical editors' debugger script, which
                                    reads the terminal the editor came from */
} WpeChildGroup;

typedef struct {
 char *const *argv;              /* execvp */
 const char *cwd;                /* chdir in the child, or NULL */
 WpeChildIo io;
 WpeChildGroup group;
 int stdin_null;                 /* stdin from /dev/null, no pipe: the build */
 int stdout_to_null;             /* stdout to /dev/null, no pipe: a command
                                    run only for what it leaves on disk */
 const char *stdout_path;        /* stdout into this file, created or emptied
                                    (mode 0600), no pipe: a command run for
                                    the file it writes; NULL for none */
 int stderr_to_null;             /* servers are chatty */
 int stderr_to_stdout;           /* the build: one merged stream, in order */
 int stderr_inherit;             /* the editor's own stderr: a stdio debug adapter */
 int pty_raw;                    /* WPE_CHILD_IO_PTY: no output processing, so
                                    a program's "\n" is not read as "\r\n" */
 int die_with_editor;            /* the system ends it if the editor is killed */
 int signal_leader_only;         /* signals reach the child alone, never its
                                    group: the build server's launcher, whose
                                    group may hold a build server other editors
                                    share */
 int stderr_nonblocking;         /* the child's stderr pipe never makes it wait:
                                    a debugger whose errors the editor reads
                                    only between its answers */
 int side_pty_slave;             /* _PIPES_AND_PTY, _INHERIT_AND_PTY (set by
                                    their callers): the program's side of a pty
                                    from wpe_child_pty_make, kept open across
                                    exec for the program the child starts; or
                                    WPE_CHILD_NO_FD (jdb, whose program the
                                    editor starts itself) */
 int tty_fd;                     /* _TTY_PATH (set by its callers): a pty
                                    slave of the editor's, made the child's
                                    controlling terminal; or WPE_CHILD_NO_FD,
                                    and then tty_path */
 const char *tty_path;           /* _TTY_PATH without tty_fd: a terminal opened
                                    by name, not made the controlling one */
 int say_failed;                 /* a terminal, directory or program that fails
                                    is named on the child's stderr, "cannot run
                                    java: ...", where the user reads its output */
 int echo_argv;                  /* the command line first on stdout (the build's Messages) */
 const char *exec_failed;        /* written before argv when exec fails, or NULL */
 const char *what;               /* "the build": every line the user reads names it */
 const struct WpeStartPlan *plan; /* what the editor decided this child is (we_startplan.c),
                                    or NULL for a start not converted yet */
 int origin;                     /* WPE_CHILD_ORIGIN_*: who asked for the child */
} WpeChildSpec;

/* Who asked for a child.  0 is "not said", so a site that forgot the field
   never picks a side. */
#define WPE_CHILD_ORIGIN_UNSET 0
#define WPE_CHILD_ORIGIN_USER  1   /* a key the user pressed: its start plan is the control */
#define WPE_CHILD_ORIGIN_AI    2   /* the assistant: the containment profile is the control */

/* A user start that does not come from a start plan.  Until the last site is
   converted it is only traced ("child unplanned <what>"); then it is refused
   with this text. */
#define WPE_CHILD_REFUSE_UNPLANNED_TEXT \
 "not started: a program the editor starts must come from a start plan"
void wpe_child_trace_set(void (*trace)(const char *line));

typedef struct {
 int pid;                        /* 0 once ended and reported */
 int in_fd, out_fd, err_fd;      /* the editor's ends: O_NONBLOCK, FD_CLOEXEC, or WPE_CHILD_NO_FD */
 int pty_fd;                     /* WPE_CHILD_IO_PTY: the master, or WPE_CHILD_NO_FD */
 int end_stage;                  /* WPE_CHILD_END_*: the ladder, on the owner's one timer */
 long end_due_ms;                /* when the ladder's next step is due */
 int leads_group;                /* signals go to -pid */
 int killed_by_end;              /* wpe_child_end sent it SIGKILL: an end that
                                    status 137 then reports is the editor's;
                                    kept after the report, until the next
                                    start */
} WpeChild;

/* The end of a child started with wpe_child_start: reported once, from the
   fd loop.  @how is WPE_CHILD_ENDED, or WPE_CHILD_ENDED_UNKNOWN for one the
   ladder gave up on.  @c->pid is 0 already. */
typedef void (*WpeChildEndedFn)(WpeChild *c, int status, int how, void *data);

int  wpe_child_start(const WpeChildSpec *spec, WpeChild *c,
                     WpeChildEndedFn ended, void *data);
int  wpe_child_pty_make(int *master, int *slave, char *name, size_t n);
int  wpe_child_signal(WpeChild *c, int sig);
int  wpe_child_signal_pid(WpeChild *c, int sig);
int  wpe_child_signal_named(WpeChild *c, int pid, int sig);
void wpe_child_end(WpeChild *c, int polite_sig);
void wpe_child_end_detached(WpeChild *c, int polite_sig);
void wpe_child_end_rest(int pgid, int polite_sig);
void wpe_child_end_named(int pid, int polite_sig);
int  wpe_child_wake_fd(void);
void wpe_child_forget(WpeChild *c);
int  wpe_child_ended(const WpeChild *c, int *status);
int  wpe_child_ending(void);
void wpe_child_say_set(void (*say)(const char *line));

/* A short command the editor needs the answer of before it goes on, run and
   waited for on the loop.  The command is an argv, never shell text: what the
   editor adds (a path, a word) is an argument.  Every one has a deadline;
   @what names it on the status row when it takes longer than a moment. */
#define WPE_CAPTURE_ENDED       0    /* the command ended by itself */
#define WPE_CAPTURE_TIMED_OUT   1    /* the deadline came: it was ended */
#define WPE_CAPTURE_CANCELLED   2    /* the user stopped the wait: it was ended */
#define WPE_CAPTURE_NOT_STARTED (-1) /* it could not be started */
int  wpe_child_capture(char *const argv[], char *out, size_t max,
                       long deadline_ms, int *status, const char *what);
int  wpe_child_probe(char *const argv[], char *out, size_t max,
                     long deadline_ms, int *status, const char *what);
int  wpe_child_run_to_file(char *const argv[], const char *path,
                           long deadline_ms, int *status, const char *what);
/* The same, for a command a start plan describes (we_startplan.c calls
   these): the plan travels in the child's spec. */
int  wpe_child_capture_plan(const struct WpeStartPlan *plan, char *const argv[], char *out,
                            size_t max, long deadline_ms, int *status, const char *what);
int  wpe_child_probe_plan(const struct WpeStartPlan *plan, char *const argv[], char *out,
                          size_t max, long deadline_ms, int *status, const char *what);
int  wpe_child_run_to_file_plan(const struct WpeStartPlan *plan, char *const argv[],
                                const char *path, long deadline_ms, int *status,
                                const char *what);
int  wpe_capture_succeeded(int result, int status);
void wpe_child_capture_display_set(int (*quiet_fd)(void), void (*serve)(void),
                                   int (*cancel_asked)(void));

/* For the SIGCHLD handler and the crash and end handlers (async-signal-safe),
   and the fd that wakes the loop. */
void wpe_child_note_ended(pid_t pid, int status);
void wpe_child_end_from_handler(void);
int  wpe_child_fd_private(int fd);

/* The pid-based watch, which wpe_child_start is built on. */
typedef void (*WpeChildEndCallback)(int pid, int status, void *data);
void WpeChildWakeOpen(void);
int  wpe_child_sigchld_install(void (*handler)(int));
void WpeChildSignalBlock(sigset_t *saved);
void WpeChildSignalRestore(const sigset_t *saved);
int  WpeChildWatch(int pid, WpeChildEndCallback cb, void *data);
void WpeChildUnwatch(int pid);
int  WpeChildEnded(int pid, int *status);

#endif /* WE_CHILD_H */
