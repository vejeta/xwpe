/* regex-start-probe.c - Print what this system's regexec does with the
 * calls the search owner (we_rowfind.c) makes, one line per measurement.
 *
 * Standalone: only <regex.h>.  It uses the owner's regcomp flags (the
 * library's flag for the word escapes, as configure.ac chooses it) and the
 * owner's ctype (UTF-8).  Every measurement runs in a child process, so a
 * crashing engine is a "crash signal N" line and not the end of the run.
 *
 * This is evidence for a regex start-context decision, not a verdict: it
 * always exits 0 when it ran. */
#include <sys/types.h>
#include <sys/wait.h>
#include <locale.h>
#include <regex.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if defined(REG_ENHANCED)
# define WORD_FLAG REG_ENHANCED
# define WORD_FLAG_NAME "REG_ENHANCED"
#elif defined(REG_GNU)
# define WORD_FLAG REG_GNU
# define WORD_FLAG_NAME "REG_GNU"
#else
# define WORD_FLAG 0
# define WORD_FLAG_NAME "none"
#endif

#ifndef REG_STARTEND
# define REG_STARTEND 0
# define HAVE_STARTEND 0
#else
# define HAVE_STARTEND 1
#endif

#define ROW4 "\xC3\x83 b"

/** measure - Compile @pat, run it once over @text and print the outcome.
 * @shape: the name that prefixes the line.
 * @so: where the call starts (REG_STARTEND rm_so when declared).
 * @groups: how many sub-expression slots to print besides group 0.
 * Runs in a child so that a crash in the engine is reported, not fatal. */
static void measure(const char *shape, const char *pat, int icase,
                    const char *text, int len, int so, int eflags,
                    int groups)
{
 pid_t pid;
 int status;

 fflush(stdout);
 pid = fork();
 if (pid < 0) {
  printf("%s: fork failed\n", shape);
  return;
 }
 if (pid == 0) {
  regex_t re;
  regmatch_t m[8];
  int rc, i;
  int cflags = REG_EXTENDED | WORD_FLAG | (icase ? REG_ICASE : 0);

  rc = regcomp(&re, pat, cflags);
  if (rc != 0) {
   printf("%s: compile=%d\n", shape, rc);
   fflush(stdout);
   _exit(0);
  }
  memset(m, 0, sizeof m);
  if (HAVE_STARTEND) {
   m[0].rm_so = so;
   m[0].rm_eo = len;
   rc = regexec(&re, text, 8, m, eflags | REG_STARTEND);
  } else {
   rc = regexec(&re, text + so, 8, m, eflags);
  }
  printf("%s: regexec=%d", shape, rc);
  if (rc == 0)
   for (i = 0; i <= groups; i++)
    printf(" g%d=%d..%d", i, (int) m[i].rm_so, (int) m[i].rm_eo);
  printf("\n");
  fflush(stdout);
  _exit(0);
 }
 waitpid(pid, &status, 0);
 if (WIFSIGNALED(status))
  printf("%s: crash signal %d\n", shape, WTERMSIG(status));
 else if (WEXITSTATUS(status) != 0)
  printf("%s: child exit %d\n", shape, WEXITSTATUS(status));
}

/** compiles - Whether regcomp accepts @pat with the owner's flags. */
static int compiles(const char *pat)
{
 regex_t re;
 int rc = regcomp(&re, pat, REG_EXTENDED | WORD_FLAG);

 if (rc == 0)
  regfree(&re);
 return rc == 0;
}

/** probe_boundary_rows - The rows that need \b \B \< \> to compile. */
static void probe_boundary_rows(void)
{
 measure("gt-dot-1", "\\>.", 0, ROW4, 4, 1, 0, 0);
 measure("gt-dot-2", "\\>.", 0, ROW4, 4, 2, 0, 0);
 measure("b-a-from1", "\\ba", 0, "xab", 3, 1, REG_NOTBOL, 0);
 measure("B-a-from1", "\\Ba", 0, "xab", 3, 1, REG_NOTBOL, 0);
 measure("bracket-lf", "[^\n](\\ba)", 0, "xab", 3, 0, 0, 1);
 measure("icase-b", "\\bA", 1, "xab", 3, 1, 0, 0);
}

/** probe_bracket_forms - The same rows with [[:<:]] and [[:>:]], for a
 * system where \b does not compile (R8). */
static void probe_bracket_forms(void)
{
 measure("br-gt-dot-1", "[[:>:]].", 0, ROW4, 4, 1, 0, 0);
 measure("br-gt-dot-2", "[[:>:]].", 0, ROW4, 4, 2, 0, 0);
 measure("br-lt-a-from1", "[[:<:]]a", 0, "xab", 3, 1, REG_NOTBOL, 0);
 measure("br-bracket-lf", "[^\n]([[:<:]]a)", 0, "xab", 3, 0, 0, 1);
 measure("br-icase", "[[:<:]]A", 1, "xab", 3, 1, 0, 0);
}

int main(void)
{
 const char *loc = "none";

 if (setlocale(LC_CTYPE, "C.UTF-8"))
  loc = "C.UTF-8";
 else if (setlocale(LC_CTYPE, "en_US.UTF-8"))
  loc = "en_US.UTF-8";
 setvbuf(stdout, NULL, _IOLBF, 0);

 printf("env: ctype=%s word_flag=%s startend=%s\n", loc, WORD_FLAG_NAME,
        HAVE_STARTEND ? "declared" : "undeclared");
#ifdef REG_ILLSEQ
 printf("env: REG_ILLSEQ=%d\n", (int) REG_ILLSEQ);
#else
 printf("env: REG_ILLSEQ=undeclared\n");
#endif
 printf("env: REG_NOMATCH=%d REG_ESPACE=%d\n", (int) REG_NOMATCH,
        (int) REG_ESPACE);

 if (compiles("\\ba")) {
  printf("env: backslash-b compiles=yes\n");
  probe_boundary_rows();
 } else {
  printf("env: backslash-b compiles=no\n");
  probe_bracket_forms();
 }

 measure("bracket-nul", "[^\n](a)", 0, "\0a", 2, 0, 0, 1);
 measure("group-backref", ".((a)\\2)", 0, "xaab", 4, 0, 0, 2);
 measure("unmatched-paren", "a)", 0, "xa)b", 4, 0, 0, 0);
 printf("done\n");
 return 0;
}
