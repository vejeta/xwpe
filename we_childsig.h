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

#include <signal.h>

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

#endif /* WE_CHILDSIG_H */
