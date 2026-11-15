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
#include "we_platform.h"

/**
 * wpe_child_die_with_editor - In a forked child, before exec: have the
 * system end the child when the editor goes away.
 * @editor: the editor's process ID, read BEFORE the fork.
 *
 * A debugger lives in a session of its own, so the hangup the terminal
 * sends when the editor's session ends no longer reaches it.  When the
 * editor is killed outright (SIGKILL, the out-of-memory killer) nothing
 * else would stop it, and a debugger stuck in its program's loop would keep
 * a core busy until someone found it.  Where the system has a parent-death
 * signal (WPE_HAS_PARENT_DEATH_SIGNAL: Linux, FreeBSD) the child gets
 * SIGKILL then.  Elsewhere (macOS, NetBSD, OpenBSD) nothing is set and the
 * debugger ends only when it sees its input pipe close.
 *
 * The signal only follows a parent that dies after the call, so the editor's
 * ID is checked right after it: a parent that died between fork and here is
 * seen as a changed parent.
 * Return: 0, or -1 when the editor is already gone and the child must not
 * go on.
 */
static inline int wpe_child_die_with_editor(pid_t editor)
{
 (void)wpe_platform_die_with_parent(SIGKILL);
 return getppid() == editor ? 0 : -1;
}

/* The child path of the owner (we_child.c) keeps its own symbol, so the
   check of what it may call can read it (an inlined entry would be folded
   into the parent's start function), and it never returns: it ends in
   _exit, or in an exec that replaces it. */
#if defined(__GNUC__)
# define WPE_CHILD_NOINLINE __attribute__((noinline))
# define WPE_CHILD_NORETURN __attribute__((noreturn))
#else
# define WPE_CHILD_NOINLINE
# define WPE_CHILD_NORETURN
#endif

#ifndef NSIG
# define NSIG 65   /* a system that does not say: more than any has */
#endif

/**
 * wpe_child_drop_editor_handlers - In a forked child, before anything else:
 * no handler of the editor's runs in it.
 *
 * The child is a copy of the editor, so every function the editor installed
 * for a signal is still installed in it, and runs on the child's memory: the
 * crash handler would rewrite the editor's crash report and draw, the end
 * handlers would act for an editor that this process is not.  A signal that
 * arrives before the exec, or a fault in the child itself, must end the child
 * the way it ends any program.  Each signal whose action is a function goes
 * back to its default; a signal set to be ignored is left (the next step
 * decides it, as a shell would).
 */
static inline void wpe_child_drop_editor_handlers(void)
{
 struct sigaction old;
 int sig;

 for (sig = 1; sig < NSIG; sig++)
 {
  if (sigaction(sig, NULL, &old) != 0)
   continue;
  if ((old.sa_flags & SA_SIGINFO) ||
      (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN))
   signal(sig, SIG_DFL);
 }
}

/**
 * wpe_child_default_signals - In a forked child, before exec: give the
 * program the signal state a program started from a shell has.
 *
 * No handler of the editor's survives (wpe_child_drop_editor_handlers).  The
 * editor ignores SIGINT and SIGPIPE, and a signal set to ignore stays
 * ignored through exec: without this the program cannot be stopped with
 * Ctrl-C or Pause, and one that relies on SIGPIPE behaves differently than
 * outside the editor.  The signal mask is emptied too, so nothing the editor
 * blocks (the owner blocks every signal around the fork) stays blocked in
 * the program.
 */
static inline void wpe_child_default_signals(void)
{
 static const int sigs[] = { SIGINT, SIGQUIT, SIGPIPE, SIGHUP, SIGTERM,
                             SIGTSTP, SIGTTIN, SIGTTOU, SIGCHLD };
 sigset_t none;
 unsigned int i;

 wpe_child_drop_editor_handlers();
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

#endif /* WE_CHILDSIG_H */
