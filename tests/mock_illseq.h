/* mock_illseq.h -- force-included into the test_rowfind_illseq builds.
 *
 * glibc matches an invalid byte as itself, so the owner handing the engine a
 * byte that is not UTF-8 goes unseen on Linux.  FreeBSD and macOS refuse with
 * REG_ILLSEQ, and NetBSD faults.  These builds put mock_regcomp and
 * mock_regexec (mock_illseq.c) between the owner, the test's reference and
 * the C library: every call that hands the engine a byte that is not UTF-8 is
 * refused and counted in mock_illseq_refusals, and the test fails on a count
 * above zero.  REG_ILLSEQ below is an error code glibc's regerror knows, so a
 * refusal can be reported. */
#ifndef MOCK_ILLSEQ_H
#define MOCK_ILLSEQ_H

#include <sys/types.h>
#include <regex.h>

/* REG_ILLSEQ is an error code glibc's regerror knows (REG_ECTYPE), so a
   refusal can be reported; a system that has its own REG_ILLSEQ gets this
   one in its place for this build. */
#undef REG_ILLSEQ
#define REG_ILLSEQ REG_ECTYPE

/* How many calls handed the engine text that is not UTF-8, since the start. */
extern int mock_illseq_refusals;

int mock_regcomp(regex_t *re, const char *pattern, int cflags);
int mock_regexec(const regex_t *re, const char *text, size_t nmatch,
                 regmatch_t pmatch[], int eflags);

#define regcomp mock_regcomp
#define regexec mock_regexec

#endif
