/* we_childsig.h - the signal state of a program the editor starts.
 *
 * One definition for every place that forks a program for the user: the
 * debuggers and their programs, the debug adapters, the Java VM jdb is given,
 * a program run from the Run menu and the build commands.  A static inline in
 * a header, so each of them stays link-independent (the protocol tests link
 * we_dap.c alone) while the rule is written once.
 */
#ifndef WE_CHILDSIG_H
#define WE_CHILDSIG_H

#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#ifdef HAVE_SYS_PRCTL_H
# include <sys/prctl.h>
#endif
#ifdef HAVE_SYS_PROCCTL_H
# include <sys/types.h>
# include <sys/procctl.h>
#endif

/**
 * wpe_child_die_with_editor - In a forked child, before exec: have the
 * system end the child when the editor goes away.
 * @editor: the editor's process ID, read BEFORE the fork.
 *
 * A debugger lives in a session of its own, so the hangup the terminal
 * sends when the editor's session ends no longer reaches it.  When the
 * editor is killed outright (SIGKILL, the out-of-memory killer) nothing
 * else would stop it, and a debugger stuck in its program's loop would keep
 * a core busy until someone found it.  Linux (PR_SET_PDEATHSIG) and FreeBSD
 * (PROC_PDEATHSIG_CTL) send the child SIGKILL then.  Elsewhere nothing is
 * set and the debugger ends only when it sees its input pipe close.
 *
 * The signal only follows a parent that dies after the call, so the editor's
 * ID is checked right after it: a parent that died between fork and here is
 * seen as a changed parent.
 * Return: 0, or -1 when the editor is already gone and the child must not
 * go on.
 */
static inline int wpe_child_die_with_editor(pid_t editor)
{
#if defined(HAVE_SYS_PRCTL_H) && defined(PR_SET_PDEATHSIG)
 prctl(PR_SET_PDEATHSIG, SIGKILL);
#elif defined(HAVE_SYS_PROCCTL_H) && defined(PROC_PDEATHSIG_CTL)
 int sig = SIGKILL;

 procctl(P_PID, 0, PROC_PDEATHSIG_CTL, &sig);
#endif
 return getppid() == editor ? 0 : -1;
}

/**
 * wpe_child_default_signals - In a forked child, before exec: give the
 * program the signal state a program started from a shell has.
 *
 * The editor ignores SIGINT and SIGPIPE (and handles others), and a signal
 * set to ignore stays ignored through exec: without this the program cannot
 * be stopped with Ctrl-C or Pause, and one that relies on SIGPIPE behaves
 * differently than outside the editor.  The signal mask is emptied too, so
 * nothing the editor blocks stays blocked in the program.
 */
static inline void wpe_child_default_signals(void)
{
 static const int sigs[] = { SIGINT, SIGQUIT, SIGPIPE, SIGHUP, SIGTERM,
                             SIGTSTP, SIGTTIN, SIGTTOU, SIGCHLD };
 sigset_t none;
 unsigned int i;

 for (i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
  signal(sigs[i], SIG_DFL);
 sigemptyset(&none);
 sigprocmask(SIG_SETMASK, &none, NULL);
}

/**
 * wpe_child_apart_from_terminal - In a forked child, before exec: leave the
 * editor's terminal, for a server that works for the editor in the
 * background.
 *
 * With its signals at their defaults, a server still in the editor's
 * process group would be ended by the Ctrl-C (or stopped by the Ctrl-Z)
 * the user types to stop a script run from the editor, and completion and
 * diagnostics would be gone without a word.  In a session of its own, as
 * the debuggers are, no key typed in the terminal reaches it; it still ends
 * when the editor closes its pipe or stops it.
 */
static inline void wpe_child_apart_from_terminal(void)
{
 (void)setsid();
}

/**
 * wpe_system_child - In the forked child of wpe_system: start the shell.
 * @cmd: the command line.
 *
 * Does not return.  The command gets the signal state a shell started it
 * from a terminal would have.
 */
static inline void wpe_system_child(const char *cmd)
{
 wpe_child_default_signals();
 execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
 _exit(127);
}

/**
 * wpe_system - Run a shell command and wait for it, like system(), but with
 * the command's signals at their defaults.
 * @cmd: the command line.
 *
 * system() hands the command the editor's ignored SIGINT and SIGPIPE, which
 * a shell can never undo.  A script or program run from the editor then
 * cannot be stopped with Ctrl-C and keeps writing into a closed pipe.  The
 * wait is done here with SIGCHLD held back, as system() does, so the
 * editor's own child reaper cannot take the status first.
 * Return: the wait status, or -1 when the shell could not be started.
 */
static inline int wpe_system(const char *cmd)
{
 sigset_t chld, saved;
 struct sigaction ign, old_int, old_quit;
 pid_t child, got;
 int status = -1;

 sigemptyset(&chld);
 sigaddset(&chld, SIGCHLD);
 sigprocmask(SIG_BLOCK, &chld, &saved);
 child = fork();
 if (child == 0)
  wpe_system_child(cmd);
 if (child < 0)
 {
  sigprocmask(SIG_SETMASK, &saved, NULL);
  return -1;
 }
 ign.sa_handler = SIG_IGN;
 sigemptyset(&ign.sa_mask);
 ign.sa_flags = 0;
 sigaction(SIGINT, &ign, &old_int);
 sigaction(SIGQUIT, &ign, &old_quit);
 do
  got = waitpid(child, &status, 0);
 while (got < 0 && errno == EINTR);
 sigaction(SIGINT, &old_int, NULL);
 sigaction(SIGQUIT, &old_quit, NULL);
 sigprocmask(SIG_SETMASK, &saved, NULL);
 if (got < 0)
  return -1;
 return status;
}

#endif /* WE_CHILDSIG_H */
