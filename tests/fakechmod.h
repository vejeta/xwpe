/* fakechmod.h -- a test's way to make this user unable to keep a setgid bit.
 *
 * The BSDs refuse a chmod that sets S_ISGID for a user who is not in the
 * file's group (EPERM, the whole call); Linux accepts the call and clears the
 * bit.  A Linux run cannot reach either case without a group the user is not
 * in, so this header defines fchmod and chmod in the test program, where they
 * take the place of the C library's for every caller linked into it (the
 * editor's files compiled into the test included), and reach the library's
 * through dlsym(RTLD_NEXT).  Both faces are the system's own:
 *
 *	refuse-sgid	a call that asks for S_ISGID fails with EPERM, nothing changes
 *	drop-sgid	the call is made without S_ISGID and returns 0
 *
 * XWPE_TEST_FAKE_FCHMOD names the face for a whole run; a test that needs a
 * face for one row sets it with fakechmod_set and puts it back to
 * FAKECHMOD_OFF.  Off, both functions are transparent.
 *
 * It defines functions with external linkage: include it from exactly one
 * file of a program.  An unknown face, or a C library that has no fchmod to
 * pass on to, ends the program with a message: a run that silently did
 * nothing would show a green row that tested nothing.
 */
#ifndef TEST_FAKECHMOD_H
#define TEST_FAKECHMOD_H

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define FAKECHMOD_ENV "XWPE_TEST_FAKE_FCHMOD"

typedef enum { FAKECHMOD_OFF, FAKECHMOD_REFUSE, FAKECHMOD_DROP } FakeChmodFace;

typedef int (*fakechmod_fchmod_fn)(int, mode_t);
typedef int (*fakechmod_chmod_fn)(const char *, mode_t);

static FakeChmodFace fakechmod_face = FAKECHMOD_OFF;
static mode_t fakechmod_bits = 0;
static int fakechmod_ready;

/**
 * fakechmod_set - Make this user unable to keep the given special bits.
 *
 * A row that proves what the editor does when the system refuses or drops a
 * bit calls this after its setup and puts FAKECHMOD_OFF back at its end.
 */
static void fakechmod_set(FakeChmodFace face, mode_t bits)
{
 fakechmod_face = face;
 fakechmod_bits = bits;
 fakechmod_ready = 1;
}

/**
 * fakechmod_arm_from_env - Take the face a whole run asks for, once.
 *
 * Lets one command line run a test as the user of a BSD would meet it:
 * XWPE_TEST_FAKE_FCHMOD=refuse-sgid ./test_fileop.
 */
static void fakechmod_arm_from_env(void)
{
 const char *v;

 if (fakechmod_ready)
  return;
 fakechmod_ready = 1;
 v = getenv(FAKECHMOD_ENV);
 if (v == NULL || v[0] == '\0')
  return;
 if (strcmp(v, "refuse-sgid") == 0)
  fakechmod_set(FAKECHMOD_REFUSE, S_ISGID);
 else if (strcmp(v, "drop-sgid") == 0)
  fakechmod_set(FAKECHMOD_DROP, S_ISGID);
 else
 {
  fprintf(stderr, "%s=%s: not a face (refuse-sgid or drop-sgid)\n", FAKECHMOD_ENV, v);
  abort();
 }
}

static void *fakechmod_next(const char *name)
{
 void *fn = dlsym(RTLD_NEXT, name);

 if (fn == NULL)
 {
  fprintf(stderr, "fakechmod: the C library has no %s to pass on to\n", name);
  abort();
 }
 return(fn);
}

int fchmod(int fd, mode_t mode)
{
 static fakechmod_fchmod_fn real;

 fakechmod_arm_from_env();
 if (real == NULL)
  *(void **)&real = fakechmod_next("fchmod");
 if (fakechmod_face == FAKECHMOD_REFUSE && (mode & fakechmod_bits))
 {
  errno = EPERM;
  return(-1);
 }
 if (fakechmod_face == FAKECHMOD_DROP)
  mode &= ~fakechmod_bits;
 return(real(fd, mode));
}

int chmod(const char *path, mode_t mode)
{
 static fakechmod_chmod_fn real;

 fakechmod_arm_from_env();
 if (real == NULL)
  *(void **)&real = fakechmod_next("chmod");
 if (fakechmod_face == FAKECHMOD_REFUSE && (mode & fakechmod_bits))
 {
  errno = EPERM;
  return(-1);
 }
 if (fakechmod_face == FAKECHMOD_DROP)
  mode &= ~fakechmod_bits;
 return(real(path, mode));
}

#endif
