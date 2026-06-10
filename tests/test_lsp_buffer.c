/* test_lsp_buffer.c -- unit test for e_lsp_join_lines (we_lsp.c): the buffer
 * serialization sent to the language server must emit EXACTLY ONE '\n' per
 * editor line.  Each editor line is stored with its in-buffer WPE_WR ('\n',
 * 0x0A) terminator; the serializer must copy the content up to that terminator
 * and add a single '\n', NOT keep the WPE_WR and add another -- doubling shifts
 * every (line,character) position and makes hover/definition land on the wrong
 * token (the 1.6.x buffer-newline regression).  Pure, no server, no skip. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../we_lsp.h"

static int fails;

static void check(const char *name, char *got, const char *want)
{
 if (got && strcmp(got, want) == 0)
  printf("ok   - %s\n", name);
 else
 {
  printf("FAIL - %s\n      want: \"%s\"\n      got : \"%s\"\n",
         name, want, got ? got : "(null)");
  fails++;
 }
 free(got);
}

int main(void)
{
 /* 1. plain lines (no terminator): one '\n' each, including a trailing one. */
 {
  char *l[] = { "object Demo", "  def x = 1" };
  check("plain lines -> single newline each",
        e_lsp_join_lines(l, 2), "object Demo\n  def x = 1\n");
 }

 /* 2. THE REGRESSION: each line still carries its WPE_WR ('\n') terminator --
       it must be trimmed, not doubled. */
 {
  char *l[] = { "object Demo\n", "  def x = 1\n" };
  check("trailing WPE_WR is not doubled",
        e_lsp_join_lines(l, 2), "object Demo\n  def x = 1\n");
 }

 /* 3. blank lines: empty string AND NULL both count as one empty line. */
 {
  char *l[] = { "a", "", NULL, "b" };
  check("empty and NULL lines are blank lines",
        e_lsp_join_lines(l, 4), "a\n\n\nb\n");
 }

 /* 4. an all-blank buffer keeps its line count (N newlines). */
 {
  char *l[] = { "\n", "\n", "\n" };
  check("three blank lines -> three newlines",
        e_lsp_join_lines(l, 3), "\n\n\n");
 }

 /* 5. zero lines -> empty string (not NULL). */
 check("zero lines -> empty string", e_lsp_join_lines(NULL, 0), "");

 /* 6. only content up to the first terminator is taken (defensive). */
 {
  char *l[] = { "keep\nDROP", "keep2\nDROP2" };
  check("content after the terminator is ignored",
        e_lsp_join_lines(l, 2), "keep\nkeep2\n");
 }

 if (fails)
 {
  printf("%d test(s) FAILED\n", fails);
  return(1);
 }
 printf("all tests passed\n");
 return(0);
}
