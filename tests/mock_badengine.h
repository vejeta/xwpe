/* mock_badengine.h -- force-included into the test_rowfind_badengine build.
 *
 * NetBSD 10's regexec faults in mbrtowc on a word-boundary escape beside an
 * alternation over a row that holds a multibyte character, and answers a
 * match far outside the row for the same pattern with REG_ICASE.  glibc does
 * neither, so the owner's refusal and its check of the engine's answer would
 * never run on Linux.  This build puts mock_regcomp, mock_regexec and
 * mock_regfree (mock_badengine.c) between the owner, the test's reference
 * and the C library: the mock crashes on that shape and answers outside the
 * range on rows that carry a marker, and everything else goes to the C
 * library.  The build also forces the owner's refusal on
 * (WPE_ROWFIND_UNSAFE_BOUNDARY_GROUP), so the crash must never be reached. */
#ifndef MOCK_BADENGINE_H
#define MOCK_BADENGINE_H

#include <sys/types.h>
#include <regex.h>

int mock_regcomp(regex_t *re, const char *pattern, int cflags);
int mock_regexec(const regex_t *re, const char *text, size_t nmatch,
                 regmatch_t pmatch[], int eflags);
void mock_regfree(regex_t *re);

#define regcomp mock_regcomp
#define regexec mock_regexec
#define regfree mock_regfree

#endif
