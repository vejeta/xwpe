/* test_shtok.c -- golden-text unit for the five token walkers the
 * definition finder uses to read C text (we_shtok.c).  Always runs; run it
 * under ASan+UBSan too.
 *
 * What the user sees behind each group: "Find definition" and the project's
 * symbol scan read a C source one line at a time and must step over
 * blanks, line continuations, comments, quoted text and nested braces or
 * parentheses exactly as before, or a function is found on the wrong line
 * or not found at all.  Each case below freezes what one walker returns for
 * one kind of input: where it stopped, how many extra lines it read, and
 * where the file read is left.
 *
 *  - walker 1 (E_SH_NEXT_TOKEN): next token, a backslash joins the next
 *    line, comments;
 *  - walker 2 (E_SH_NEXT_TOKEN_EOL): the same, and the end of a line also
 *    joins the next one;
 *  - walker 3 (E_SH_CLOSE_BRACE): the matching close brace, over nesting,
 *    quotes, comments and the unused side of an #else;
 *  - walker 5 (E_SH_CLOSE_PAREN): the same for the close parenthesis;
 *  - walker 4 (E_SH_LIST_END): the next top-level comma, semicolon or open
 *    parenthesis.
 *
 * Each case is: the walker, the text of the file (its first line is the
 * line buffer the walker starts from), the index of the character the walker
 * starts at (it reads from the next one), and the frozen result.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../edit.h"

#define LINE_SIZE 256   /* the size of the caller's line buffer */
#define OUT_SIZE  160
#define AT_SHOWN  8     /* characters of the stop point written to the result */

/* 300 x's: a line longer than the caller's line buffer. */
#define X10  "xxxxxxxxxx"
#define X100 X10 X10 X10 X10 X10 X10 X10 X10 X10 X10
#define X300 X100 X100 X100

struct golden {
  int walker;           /* 1 to 5: the kind E_SH_* is walker - 1 */
  const char *name;
  const char *text;
  int start;
  const char *want;
};

static const struct golden cases[] = {
  /* E_SH_NEXT_TOKEN: blanks, continuation, comments */
  {1, "blank run", "a   b\n", 0, "ret=4 n=0 next=6 at=[b\\n]"},
  {1, "nothing to skip", "ab\n", 0, "ret=1 n=0 next=3 at=[b\\n]"},
  {1, "end of line is blank", "a\n  b\n", 0, "ret=2 n=0 next=2 at=[]"},
  {1, "backslash joins next line", "a \\\nnext   x\n", 0,
   "ret=0 n=1 next=13 at=[next   x]"},
  {1, "backslash at end of file", "a \\\n", 0, "ret=-1 n=1 next=4 at=[]"},
  {1, "comment on the line", "a /* c */ b\n", 0, "ret=10 n=0 next=12 at=[b\\n]"},
  {1, "comment over lines", "a /* c\nstill\nend */ z\n", 0,
   "ret=7 n=2 next=22 at=[z\\n]"},
  {1, "comment never closed", "a /* c\nstill\n", 0, "ret=-1 n=2 next=13 at=[]"},
  {1, "slash not a comment", "a /b\n", 0, "ret=2 n=0 next=5 at=[/b\\n]"},

  /* E_SH_NEXT_TOKEN_EOL: as E_SH_NEXT_TOKEN, and the end of a line joins the next */
  {2, "blank run", "a   b\n", 0, "ret=4 n=0 next=6 at=[b\\n]"},
  {2, "end of line joins next", "a\nb\n", 0, "ret=0 n=1 next=4 at=[b\\n]"},
  {2, "blank lines join", "a\n\n\nc\n", 0, "ret=0 n=3 next=6 at=[c\\n]"},
  {2, "backslash joins next line", "a \\\nnext x\n", 0,
   "ret=0 n=1 next=11 at=[next x\\n]"},
  {2, "end of file after line", "a\n", 0, "ret=-1 n=1 next=2 at=[]"},
  {2, "comment on the line", "a /* c */ b\n", 0, "ret=10 n=0 next=12 at=[b\\n]"},
  {2, "comment over lines", "a /* c\nend */ z\n", 0,
   "ret=7 n=1 next=16 at=[z\\n]"},
  {2, "comment never closed", "a /* c\n", 0, "ret=-1 n=1 next=7 at=[]"},
  {2, "slash not a comment", "a /b\n", 0, "ret=2 n=0 next=5 at=[/b\\n]"},

  /* E_SH_CLOSE_BRACE: the matching close brace */
  {3, "plain body", "{ a }\n", 0, "ret=4 n=0 next=6 at=[}\\n]"},
  {3, "nested braces", "{ { x } y }\n", 0, "ret=10 n=0 next=12 at=[}\\n]"},
  {3, "brace in char", "{ '}' }\n", 0, "ret=6 n=0 next=8 at=[}\\n]"},
  {3, "escaped quote char", "{ '\\'' }\n", 0, "ret=7 n=0 next=9 at=[}\\n]"},
  {3, "brace in string", "{ \"}\" }\n", 0, "ret=6 n=0 next=8 at=[}\\n]"},
  {3, "escaped quote in string", "{ \"a\\\"}\" }\n", 0,
   "ret=9 n=0 next=11 at=[}\\n]"},
  {3, "escaped backslash ends string", "{ \"a\\\\\" }\n", 0,
   "ret=8 n=0 next=10 at=[}\\n]"},
  {3, "brace in comment", "{ /* } */ }\n", 0, "ret=10 n=0 next=12 at=[}\\n]"},
  {3, "comment over lines", "{ /* a\n} */ }\n", 0, "ret=5 n=1 next=14 at=[}\\n]"},
  {3, "body over lines", "{ a\n   b }\n", 0, "ret=5 n=1 next=11 at=[}\\n]"},
  {3, "line starting with close", "{ a\n}\n", 0, "ret=0 n=1 next=6 at=[}\\n]"},
  {3, "else side skipped", "{ a\n#else\n x }\n#endif\n}\n", 0,
   "ret=0 n=4 next=24 at=[}\\n]"},
  {3, "elif side skipped", "{ a\n#elif B\n x }\n#endif\n }\n", 0,
   "ret=1 n=4 next=27 at=[}\\n]"},
  {3, "if line kept", "{ a\n#if B\n x }\n", 0, "ret=3 n=2 next=15 at=[}\\n]"},
  {3, "unterminated char", "{ 'a\n}\n", 0, "ret=0 n=1 next=7 at=[}\\n]"},
  {3, "unterminated string", "{ \"a\n}\n", 0, "ret=0 n=1 next=7 at=[}\\n]"},
  {3, "end of file", "{ a\n b\n", 0, "ret=-1 n=2 next=7 at=[]"},
  {3, "else never ends", "{ a\n#else\n b\n", 0, "ret=-1 n=3 next=13 at=[]"},
  {3, "comment never closed", "{ /* a\n", 0, "ret=-1 n=1 next=7 at=[]"},

  /* E_SH_CLOSE_PAREN: the matching close parenthesis */
  {5, "plain list", "( a )\n", 0, "ret=4 n=0 next=6 at=[)\\n]"},
  {5, "nested parentheses", "( ( x ) y )\n", 0, "ret=10 n=0 next=12 at=[)\\n]"},
  {5, "paren in char", "( ')' )\n", 0, "ret=6 n=0 next=8 at=[)\\n]"},
  {5, "escaped quote char", "( '\\'' )\n", 0, "ret=7 n=0 next=9 at=[)\\n]"},
  {5, "paren in string", "( \")\" )\n", 0, "ret=6 n=0 next=8 at=[)\\n]"},
  {5, "escaped quote in string", "( \"a\\\")\" )\n", 0,
   "ret=9 n=0 next=11 at=[)\\n]"},
  {5, "paren in comment", "( /* ) */ )\n", 0, "ret=10 n=0 next=12 at=[)\\n]"},
  {5, "comment over lines", "( /* a\n) */ )\n", 0, "ret=5 n=1 next=14 at=[)\\n]"},
  {5, "braces ignored", "( { ) }\n", 0, "ret=4 n=0 next=8 at=[) }\\n]"},
  {5, "list over lines", "( a,\n   b )\n", 0, "ret=5 n=1 next=12 at=[)\\n]"},
  {5, "else side skipped", "( a\n#else\n x )\n#endif\n)\n", 0,
   "ret=0 n=4 next=24 at=[)\\n]"},
  {5, "unterminated string", "( \"a\n)\n", 0, "ret=0 n=1 next=7 at=[)\\n]"},
  {5, "unterminated char", "( 'a\n)\n", 0, "ret=0 n=1 next=7 at=[)\\n]"},
  {5, "end of file", "( a\n b\n", 0, "ret=-1 n=2 next=7 at=[]"},
  {5, "comment never closed", "( /* a\n", 0, "ret=-1 n=1 next=7 at=[]"},

  /* E_SH_LIST_END: the next top-level comma, semicolon or open parenthesis */
  {4, "comma", "a, b\n", 0, "ret=1 n=0 next=5 at=[, b\\n]"},
  {4, "semicolon", "a; b\n", 0, "ret=1 n=0 next=5 at=[; b\\n]"},
  {4, "open parenthesis", "a(b)\n", 0, "ret=1 n=0 next=5 at=[(b)\\n]"},
  {4, "stops inside braces ignored", "a { b; c, d( } e,\n", 0,
   "ret=16 n=0 next=18 at=[,\\n]"},
  {4, "stray close brace", "a } b, c;\n", 0, "ret=-1 n=1 next=10 at=[]"},
  {4, "comma in string", "a \",\" b;\n", 0, "ret=7 n=0 next=9 at=[;\\n]"},
  {4, "escaped quote in string", "a \"\\\",\" b;\n", 0,
   "ret=9 n=0 next=11 at=[;\\n]"},
  {4, "semicolon in char", "a ';' b,\n", 0, "ret=7 n=0 next=9 at=[,\\n]"},
  {4, "escaped quote char", "a '\\'' b;\n", 0, "ret=8 n=0 next=10 at=[;\\n]"},
  {4, "semicolon in comment", "a /* ; */ b,\n", 0, "ret=11 n=0 next=13 at=[,\\n]"},
  {4, "comment over lines", "a /* ;\n; */ b,\n", 0, "ret=6 n=1 next=15 at=[,\\n]"},
  {4, "over lines", "a\n b;\n", 0, "ret=2 n=1 next=6 at=[;\\n]"},
  {4, "line start is not skipped", "a\n#else\n;\n", 0, "ret=0 n=2 next=10 at=[;\\n]"},
  {4, "unterminated string", "a \"b\n;\n", 0, "ret=0 n=1 next=7 at=[;\\n]"},
  {4, "unterminated char", "a 'b\n;\n", 0, "ret=0 n=1 next=7 at=[;\\n]"},
  {4, "end of file", "a\n", 0, "ret=-1 n=1 next=2 at=[]"},
  {4, "comment never closed", "a /* b\n", 0, "ret=-1 n=1 next=7 at=[]"},

  /* the unused side of an #if, and the line buffer */
  {3, "indented else side skipped", "{ a\n   #else\n x }\n#endif\n}\n", 0,
   "ret=0 n=4 next=27 at=[}\\n]"},
  {3, "indented endif ends skip", "{ a\n#else\n x }\n   #endif\n}\n", 0,
   "ret=0 n=4 next=27 at=[}\\n]"},
  {3, "else side with error line", "{ a\n#else\n#error }\n }\n#endif\n}\n", 0,
   "ret=0 n=5 next=31 at=[}\\n]"},
  {5, "indented elif side skipped", "( a\n\t#elif B\n x )\n #endif\n)\n", 0,
   "ret=0 n=4 next=28 at=[)\\n]"},
  {3, "stale bytes after short string line", "{ /*   }}}}}} */\n\"a\n}\n", 0,
   "ret=0 n=2 next=22 at=[}\\n]"},
  {4, "stale bytes after short char line", "a /*   ;;;;;; */\n'a\n;\n", 0,
   "ret=0 n=2 next=22 at=[;\\n]"},
  {2, "long line read in two", "a\n" X300 "\n", 0,
   "ret=0 n=1 next=257 at=[xxxxxxxx]"},
};

#define NCASES ((int)(sizeof cases / sizeof cases[0]))

static int fails;

/* The line reader the walkers call.  The editor's own (we_progn.c) also
   reads from a window's text; the walkers do not care which, and the unit
   reads from a file. */
char *e_agets(char *str, int n, E_AFILE *ep)
{
  return fgets(str, n, ep->fp);
}

/* Write @s into @out with a newline shown as the two characters \n. */
static void show(char *out, size_t size, const char *s, int max)
{
  size_t o = 0;
  int i;

  for (i = 0; s[i] && i < max && o + 3 < size; i++)
  {
    if (s[i] == '\n')
    {
      out[o++] = '\\';
      out[o++] = 'n';
    }
    else
      out[o++] = s[i];
  }
  out[o] = '\0';
}

/* Run one walker over @text and describe where it stopped. */
static void run_case(const struct golden *g, char *out, size_t size)
{
  char str[LINE_SIZE], at[OUT_SIZE], *sp, *ret;
  E_AFILE af;
  int n = 0;
  FILE *fp = tmpfile();

  memset(&af, 0, sizeof af);
  af.fp = fp;
  fputs(g->text, fp);
  rewind(fp);
  memset(str, 0, sizeof str);
  if (!fgets(str, sizeof str, fp))
    str[0] = '\0';
  sp = str + g->start;
  ret = e_sh_walk(g->walker - 1, sp, str, &af, &n);
  at[0] = '\0';
  if (ret)
    show(at, sizeof at, ret, AT_SHOWN);
  snprintf(out, size, "ret=%ld n=%d next=%ld at=[%s]",
           ret ? (long)(ret - str) : -1L, n, ftell(fp), at);
  fclose(fp);
}

int main(int argc, char **argv)
{
  char out[OUT_SIZE * 2];
  int i, print = argc > 1 && !strcmp(argv[1], "--print");

  for (i = 0; i < NCASES; i++)
  {
    run_case(&cases[i], out, sizeof out);
    if (print)
      printf("  {%d, \"%s\", ... \"%s\"},\n", cases[i].walker, cases[i].name,
             out);
    else if (strcmp(out, cases[i].want) != 0)
    {
      printf("FAIL: spl%d %s: got [%s] want [%s]\n", cases[i].walker,
             cases[i].name, out, cases[i].want);
      fails++;
    }
  }
  if (fails)
    return 1;
  if (!print)
    printf("test_shtok: %d cases OK\n", NCASES);
  return 0;
}
