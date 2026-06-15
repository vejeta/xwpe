/* test_clipboard.c -- unit test for the pure clipboard helpers:
 * RFC 4648 base64 vectors and the OSC 52 framing written to an fd.
 * Links we_clip.c only; no editor, no X11 -- always runs (no skip). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "../we_clip.h"

static int fails;

static void chk_b64(const char *in, const char *want)
{
 char out[256];
 int n = e_b64_encode((const unsigned char *)in, (int)strlen(in), out,
                      (int)sizeof(out));
 if (n < 0 || strcmp(out, want) != 0)
 {
  printf("FAIL: b64(\"%s\") = \"%s\" want \"%s\"\n",
         in, n < 0 ? "(err)" : out, want);
  fails++;
 }
}

static void test_osc52_framing(void)
{
 /* "Hi" -> base64 "SGk=" -> ESC ] 52 ; c ; SGk= BEL */
 const char *want = "\033]52;c;SGk=\a";
 char buf[256];
 int p[2];
 ssize_t r;

 if (pipe(p) != 0) { printf("FAIL: pipe\n"); fails++; return; }
 if (e_clip_osc52_write(p[1], "Hi", 2) != 0)
 { printf("FAIL: e_clip_osc52_write returned -1\n"); fails++; }
 close(p[1]);
 r = read(p[0], buf, sizeof(buf) - 1);
 close(p[0]);
 if (r < 0) { printf("FAIL: read\n"); fails++; return; }
 buf[r] = '\0';
 if ((size_t)r != strlen(want) || memcmp(buf, want, (size_t)r) != 0)
 {
  printf("FAIL: OSC 52 framing: got %zd bytes \"%s\"\n", r, buf);
  fails++;
 }
}

static void test_osc52_cap(void)
{
 /* Oversized payload must be refused, not silently truncated. */
 int big = E_CLIP_OSC52_MAX + 1;
 char *s = malloc((size_t)big);
 if (!s) { printf("FAIL: malloc\n"); fails++; return; }
 memset(s, 'x', (size_t)big);
 if (e_clip_osc52_write(STDOUT_FILENO, s, big) != -1)
 { printf("FAIL: oversized OSC 52 not rejected\n"); fails++; }
 free(s);
}

int main(void)
{
 /* RFC 4648 section 10 test vectors. */
 chk_b64("",       "");
 chk_b64("f",      "Zg==");
 chk_b64("fo",     "Zm8=");
 chk_b64("foo",    "Zm9v");
 chk_b64("foob",   "Zm9vYg==");
 chk_b64("fooba",  "Zm9vYmE=");
 chk_b64("foobar", "Zm9vYmFy");
 /* UTF-8: "Héllo" = 48 C3 A9 6C 6C 6F -> "SMOpbGxv". */
 chk_b64("H\303\251llo", "SMOpbGxv");

 test_osc52_framing();
 test_osc52_cap();

 if (fails)
 {
  printf("FAIL: %d clipboard check(s)\n", fails);
  return 1;
 }
 printf("PASS: clipboard base64 + OSC 52 framing\n");
 return 0;
}
