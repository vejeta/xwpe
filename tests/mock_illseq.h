/* mock_illseq.h -- force-included into the test_rowfind_illseq build.
 *
 * glibc matches an invalid byte as itself, so the owner's two fallbacks to
 * bytes (the plain text its engine refuses at compile time, the row it
 * refuses at exec time) never run on Linux.  FreeBSD and macOS refuse with
 * REG_ILLSEQ.  This build puts mock_regcomp and mock_regexec (mock_illseq.c)
 * between the owner, the test's reference and the C library, and REG_ILLSEQ
 * below is an error code glibc's regerror knows, so both fallbacks run here
 * and the reference is compared with the owner while they do. */
#ifndef MOCK_ILLSEQ_H
#define MOCK_ILLSEQ_H

#include <sys/types.h>
#include <regex.h>

/* REG_ILLSEQ is an error code glibc's regerror knows (REG_ECTYPE), so a
   refusal can be reported; a system that has its own REG_ILLSEQ gets this
   one in its place for this build. */
#undef REG_ILLSEQ
#define REG_ILLSEQ REG_ECTYPE

int mock_regcomp(regex_t *re, const char *pattern, int cflags);
int mock_regexec(const regex_t *re, const char *text, size_t nmatch,
                 regmatch_t pmatch[], int eflags);

#define regcomp mock_regcomp
#define regexec mock_regexec

#endif
