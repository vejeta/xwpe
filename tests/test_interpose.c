/* test_interpose.c -- unit test for tests/interpose.h, the one resolver of
 * "the C library's function behind a test's hook".
 *
 * What goes wrong if it breaks: a hook that passes on to itself (NetBSD's
 * fstat is exported as __fstat50, and the library's symbol named "fstat" is a
 * wrapper that calls it: an endless loop and a stack overflow with no row
 * named), a hook that passes on to a function of another layout (glibc with
 * large files binds fstat to fstat64), or a failure that names nothing.
 *
 * The program hooks fstat the way the write and file tests do, calls it once
 * through the hook and proves the library's answer came back.  The cases that
 * must end the program (a function that leads back into its hook, a hook
 * dladdr cannot name) run in a child and are read from its status and its
 * message.  Always runs.
 */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "interpose.h"
#include "test_watchdog.h"

#define CHILD_WAIT_MS 20000      /* a child that does not end by itself is stopped */
#define WHOLE_RUN_S 60           /* the hang guard of the whole program */
#define MESSAGE_ROOM 512

static int fails;

static void chk_int(const char *what, long got, long want)
{
 if (got != want)
 {
  printf("FAIL: %s: got %ld want %ld\n", what, got, want);
  fails++;
 }
}

static void chk_has(const char *what, const char *got, const char *want)
{
 if (strstr(got, want) == NULL)
 {
  printf("FAIL: %s: \"%s\" lacks \"%s\"\n", what, got, want);
  fails++;
 }
}

/* ---- the hook: fstat, as tests/test_write_owner.c has it ---- */

typedef int (*fstat_fn)(int, struct stat *);

/** fstat - The test's own fstat: counts the entry and passes on to the
    library's, found by the resolver under the name this hook was exported as. */
int fstat(int fd, struct stat *st)
{
 static fstat_fn real;
 WPE_HOOK_GUARD(fstat);

 if (real == NULL)
  real = WPE_REAL(fstat);
 return(real(fd, st));
}

/* ---- the cases that end the program ---- */

/** loop_hook - A hook whose "real" function is itself: what a wrapper that
    calls the renamed symbol gives (NetBSD).  The guard must end it. */
static void loop_hook(void)
{
 WPE_HOOK_GUARD(loop_hook);

 loop_hook();
}

/** hidden_hook - A function dladdr is not asked to find under its own name:
    static, so it is not exported. */
static int hidden_hook(void)
{
 return(0);
}

static void child_loop(void)
{
 loop_hook();
}

static void child_unexported(void)
{
 (void)wpe_interpose_real("hidden_hook", (void *)hidden_hook);
}

/**
 * run_child_that_ends - Run a function in a child and read how it ended.
 * @body: what the child runs.
 * @message: where the child's stderr goes, MESSAGE_ROOM bytes.
 *
 * Return: the signal that ended the child, or 0 when it exited.
 */
static int run_child_that_ends(void (*body)(void), char *message)
{
 int pipefd[2];
 pid_t pid;
 int status = 0;
 ssize_t got;

 message[0] = '\0';
 if (pipe(pipefd) != 0)
  return(-1);
 pid = fork();
 if (pid == 0)
 {
  struct sigaction ign;

  memset(&ign, 0, sizeof ign);
  ign.sa_handler = SIG_DFL;
  sigaction(SIGABRT, &ign, NULL);
  close(pipefd[0]);
  dup2(pipefd[1], STDERR_FILENO);
  body();
  _exit(0);
 }
 close(pipefd[1]);
 got = read(pipefd[0], message, MESSAGE_ROOM - 1);
 close(pipefd[0]);
 message[got > 0 ? got : 0] = '\0';
 status = test_wait_child(pid, CHILD_WAIT_MS);
 if (status == -1)
  return(-1);
 return(WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

/* ---- the rows ---- */

/* The hook ran once, and the library's function answered: the inode is the
   one fstatat gives for the same open file, and the resolver's function is
   not the hook.  Red where the real function is the hook itself (the lookup
   scope RTLD_DEFAULT): the resolver names it and ends the program. */
static void test_the_hook_passes_on_to_the_librarys_function(void)
{
 struct stat through_hook;
 struct stat by_name;
 long before;
 long after;
 int fd = open("/dev/null", O_RDONLY);
 int rc;

 if (fd < 0)
 {
  printf("SKIP: no /dev/null to ask about\n");
  return;
 }
 before = wpe_interpose_entries("fstat");
 rc = fstat(fd, &through_hook);
 after = wpe_interpose_entries("fstat");
 chk_int("the call through the hook answers", rc, 0);
 chk_int("the hook ran once", after - before, 1);
 chk_int("the library's function answered: the same file", fstatat(AT_FDCWD, "/dev/null", &by_name, 0), 0);
 chk_int("the same inode as the library's own answer", (long)through_hook.st_ino, (long)by_name.st_ino);
 chk_int("a character device", S_ISCHR(through_hook.st_mode), 1);
 chk_int("the resolver's function is not the hook", (void *)WPE_REAL(fstat) != (void *)fstat, 1);
 close(fd);
}

/* A hook whose real function leads back into it ends the program, and the
   message names the hook; it is not 20000 frames and a stack overflow.  Red on
   a guard that never counts (the child dies of SIGSEGV, or never ends). */
static void test_a_loop_back_into_the_hook_is_named(void)
{
 char message[MESSAGE_ROOM];
 int sig = run_child_that_ends(child_loop, message);

 chk_int("the child was ended by SIGABRT", sig, SIGABRT);
 chk_has("the message names the hook", message, "loop_hook");
 chk_has("the message says what happened", message, "leads back into its hook");
}

/* A hook dladdr cannot name ends the program with the hook's name and the
   reason; the lookup never falls back to the C spelling. */
static void test_a_hook_that_is_not_exported_is_named(void)
{
 char message[MESSAGE_ROOM];
 int sig = run_child_that_ends(child_unexported, message);

 chk_int("the child was ended by SIGABRT", sig, SIGABRT);
 chk_has("the message names the hook", message, "hidden_hook");
}

int main(void)
{
 test_watchdog_arm("test_interpose", WHOLE_RUN_S);
 test_the_hook_passes_on_to_the_librarys_function();
 test_a_loop_back_into_the_hook_is_named();
 test_a_hook_that_is_not_exported_is_named();
 if (fails != 0)
 {
  printf("test_interpose: %d failed\n", fails);
  return(1);
 }
 printf("test_interpose: ok\n");
 return(0);
}
