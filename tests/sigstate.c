/* sigstate: print the name of each signal this process ignores and each it
   blocks, two lines "IGNORED: XFSZ PIPE ..." and "BLOCKED: CHLD ..." (names
   without the SIG prefix).

   Tests that ask whether a program the editor started inherited an ignored
   signal run this as the program.  It replaces reading the SigIgn line of
   /proc, which only Linux has, and it names signals instead of bits, whose
   numbers differ between systems. */

#include <signal.h>
#include <stdio.h>

typedef struct
{
 int number;
 const char *name;
} SignalName;

static const SignalName signals_of_interest[] = {
 {SIGHUP, "HUP"},   {SIGINT, "INT"},   {SIGQUIT, "QUIT"}, {SIGPIPE, "PIPE"},
 {SIGALRM, "ALRM"}, {SIGTERM, "TERM"}, {SIGCHLD, "CHLD"}, {SIGXFSZ, "XFSZ"},
 {SIGXCPU, "XCPU"}, {SIGUSR1, "USR1"}, {SIGUSR2, "USR2"}, {SIGWINCH, "WINCH"},
};

/* Whether signal @number is ignored by this process. */
static int is_ignored(int number)
{
 struct sigaction act;

 if (sigaction(number, NULL, &act) != 0)
  return 0;
 return act.sa_handler == SIG_IGN;
}

/* Whether signal @number is blocked in this process's signal mask. */
static int is_blocked(int number)
{
 sigset_t mask;

 if (sigprocmask(SIG_BLOCK, NULL, &mask) != 0)
  return 0;
 return sigismember(&mask, number) == 1;
}

int main(void)
{
 size_t i;
 const size_t count = sizeof signals_of_interest / sizeof signals_of_interest[0];

 printf("IGNORED:");
 for (i = 0; i < count; i++)
  if (is_ignored(signals_of_interest[i].number))
   printf(" %s", signals_of_interest[i].name);
 printf("\nBLOCKED:");
 for (i = 0; i < count; i++)
  if (is_blocked(signals_of_interest[i].number))
   printf(" %s", signals_of_interest[i].name);
 printf("\n");
 return 0;
}
