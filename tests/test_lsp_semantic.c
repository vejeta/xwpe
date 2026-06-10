/* test_lsp_semantic.c -- unit test for e_lsp_semantic_decode (we_lsp.c): the LSP
 * semantic-tokens wire format is a flat int array, groups of 5
 * (deltaLine, deltaStartChar, length, tokenType, tokenModifiers), where each
 * group is relative to the PREVIOUS token -- same line: char delta adds to the
 * previous start; new line: char delta is absolute and start resets.  Getting
 * that delta accumulation wrong shifts every highlight, so it is decoded by a
 * pure function and checked here without a server. */
#include <stdio.h>
#include <string.h>

#include "../we_lsp.h"

static int fails;

static void chk(const char *name, int got, int want)
{
 if (got == want)
  printf("ok   - %s\n", name);
 else
 {  printf("FAIL - %s (want %d, got %d)\n", name, want, got);  fails++;  }
}

int main(void)
{
 e_lsp_sem_token t[8];
 int n;

 /* three tokens: keyword(0,0,5), variable on the same line (+6 -> 6,3),
    method two lines down (absolute start 4). */
 {
  int data[] = { 0,0,5,15,0,   0,6,3,8,0,   2,4,4,13,0 };
  n = e_lsp_semantic_decode(data, (int)(sizeof(data)/sizeof(data[0])), t, 8);
  chk("count", n, 3);
  chk("t0.line", t[0].line, 0);   chk("t0.start", t[0].start, 0);
  chk("t0.length", t[0].length, 5); chk("t0.type", t[0].type, 15);
  chk("t1 same line", t[1].line, 0); chk("t1.start += dchar", t[1].start, 6);
  chk("t1.type", t[1].type, 8);
  chk("t2 new line", t[2].line, 2); chk("t2.start absolute", t[2].start, 4);
  chk("t2.type", t[2].type, 13);
 }

 /* a short tail (not a whole group of 5) is ignored, not misread. */
 {
  int data[] = { 0,0,3,1,0,   0,2 };          /* one token + 2 dangling ints */
  chk("short tail ignored", e_lsp_semantic_decode(data, 7, t, 8), 1);
 }

 /* max caps the OUTPUT but line/start tracking still advances, so a later token
    that does fit is still placed correctly relative to skipped ones. */
 {
  int data[] = { 5,0,1,1,0,   0,3,1,2,0,   0,3,1,3,0 };
  n = e_lsp_semantic_decode(data, 15, t, 2);  /* room for 2 of the 3 */
  chk("count capped at max", n, 2);
  chk("t1.start within cap", t[1].start, 3);
 }

 /* defensive: NULL data / negative n -> 0, no crash. */
 chk("NULL data", e_lsp_semantic_decode(NULL, 5, t, 8), 0);
 chk("negative n", e_lsp_semantic_decode((int[]){0}, -1, t, 8), 0);

 if (fails)
 {  printf("%d failure(s)\n", fails);  return(1);  }
 printf("all semantic-decode checks passed\n");
 return(0);
}
