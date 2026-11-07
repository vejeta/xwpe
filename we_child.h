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

/* At most sixteen at once: the debugger or an adapter, jdb's program, four
   adapters closing, the build, two language servers (one ending), the build
   server and its setup, the AI host, its tool command and the CLI, a Run and
   a short capture.  Twice that, so a start is never refused in practice. */
#define WPE_CHILD_MAX            32
#define WPE_CHILD_NO_FD          (-1)
#define WPE_CHILD_END_GRACE_MS   2000    /* after the polite signal, before SIGKILL */
#define WPE_CHILD_END_KILL_MS    1000    /* after SIGKILL, before "did not end" */
#define WPE_CHILD_NO_SIGNAL      0       /* wpe_child_end: the caller already said "quit" on the pipe */

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
 WPE_CHILD_IO_PTY                /* one pty on 0/1/2, its controlling terminal:
                                    dlv, whose program runs on it */
} WpeChildIo;

typedef enum {
 WPE_CHILD_OWN_SESSION,          /* setsid: a signal to -pid ends the pipeline */
 WPE_CHILD_OWN_GROUP             /* setpgid on both sides: the build */
} WpeChildGroup;

typedef struct {
 char *const *argv;              /* execvp */
 const char *cwd;                /* chdir in the child, or NULL */
 WpeChildIo io;
 WpeChildGroup group;
 int stdin_null;                 /* stdin from /dev/null, no pipe: the build */
 int stdout_to_null;             /* stdout to /dev/null, no pipe: a command
                                    run only for what it leaves on disk */
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
 int echo_argv;                  /* the command line first on stdout (the build's Messages) */
 const char *exec_failed;        /* written before argv when exec fails, or NULL */
 const char *what;               /* "the build": every line the user reads names it */
} WpeChildSpec;

typedef struct {
 int pid;                        /* 0 once ended and reported */
 int in_fd, out_fd, err_fd;      /* the editor's ends: O_NONBLOCK, FD_CLOEXEC, or WPE_CHILD_NO_FD */
 int pty_fd;                     /* WPE_CHILD_IO_PTY: the master, or WPE_CHILD_NO_FD */
 int end_stage;                  /* WPE_CHILD_END_*: the ladder, on the owner's one timer */
 long end_due_ms;                /* when the ladder's next step is due */
 int leads_group;                /* signals go to -pid */
} WpeChild;

/* The end of a child started with wpe_child_start: reported once, from the
   fd loop.  @how is WPE_CHILD_ENDED, or WPE_CHILD_ENDED_UNKNOWN for one the
   ladder gave up on.  @c->pid is 0 already. */
typedef void (*WpeChildEndedFn)(WpeChild *c, int status, int how, void *data);

int  wpe_child_start(const WpeChildSpec *spec, WpeChild *c,
                     WpeChildEndedFn ended, void *data);
int  wpe_child_signal(WpeChild *c, int sig);
int  wpe_child_signal_pid(WpeChild *c, int sig);
void wpe_child_end(WpeChild *c, int polite_sig);
void wpe_child_end_detached(WpeChild *c, int polite_sig);
void wpe_child_forget(WpeChild *c);
int  wpe_child_ended(const WpeChild *c, int *status);
int  wpe_child_ending(void);
void wpe_child_say_set(void (*say)(const char *line));

/* For the SIGCHLD handler (async-signal-safe), and the fd that wakes the loop. */
void wpe_child_note_ended(pid_t pid, int status);
int  wpe_child_fd_private(int fd);

/* The older, pid-based watch, for the children not yet started through
   wpe_child_start (the line debuggers, jdb's VM). */
typedef void (*WpeChildEndCallback)(int pid, int status, void *data);
void WpeChildWakeOpen(void);
void WpeChildSignalBlock(sigset_t *saved);
void WpeChildSignalRestore(const sigset_t *saved);
int  WpeChildWatch(int pid, WpeChildEndCallback cb, void *data);
void WpeChildUnwatch(int pid);
int  WpeChildEnded(int pid, int *status);
int  WpeReapedStatus(int pid, int *status);

#endif /* WE_CHILD_H */
