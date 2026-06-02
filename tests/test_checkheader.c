#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT_EQ(got, expect, msg) do { \
    tests_run++; \
    if ((got) == (expect)) { tests_passed++; printf("  PASS: %s\n", msg); } \
    else { printf("  FAIL: %s (got %d, expected %d)\n", msg, got, expect); } \
} while (0)

#define TDIR "/tmp/xwpe_chktest"

static char *e_chk_skip_block_comment(char *p, char *buf, FILE *fp)
{
 p++;
 for (;;)
 {
  for (p++; *p && *p != '*'; p++)
   ;
  if (*p == '*' && *(p + 1) == '/')
   return p + 2;
  if (!*p && !fgets((p = buf), 120, fp))
   return NULL;
 }
}

static char *e_chk_skip_whitespace_and_comments(char *p, char *buf, FILE *fp)
{
 for (;;)
 {
  while (isspace((unsigned char)*p))
   p++;
  if (*p == '/' && *(p + 1) == '*')
  {
   p = e_chk_skip_block_comment(p, buf, fp);
   if (!p) return NULL;
   continue;
  }
  if (*p == '/' && *(p + 1) == '/')
   return NULL;
  return p;
 }
}

static int e_chk_is_directive(char *p, const char *name)
{
 int len = strlen(name);
 if (strncmp(p, name, len) != 0)
  return 0;
 return !isalnum((unsigned char)p[len]) && p[len] != '_';
}

static void e_chk_track_conditional(char *p, int *if_depth, int *skip_depth)
{
 if (e_chk_is_directive(p, "ifdef") || e_chk_is_directive(p, "ifndef")
     || e_chk_is_directive(p, "if"))
 {
  (*if_depth)++;
  if (*skip_depth == 0 && e_chk_is_directive(p, "if")
      && *(p + 2) == ' ' && *(p + 3) == '0')
   *skip_depth = *if_depth;
 }
 else if (e_chk_is_directive(p, "else"))
 {
  if (*skip_depth == *if_depth)
   *skip_depth = 0;
 }
 else if (e_chk_is_directive(p, "endif"))
 {
  if (*skip_depth == *if_depth)
   *skip_depth = 0;
  if (*if_depth > 0)
   (*if_depth)--;
 }
}

static int e_chk_extract_include(char *p, char *out, int outsz)
{
 int i;
 for (p += 7; isspace((unsigned char)*p); p++)
  ;
 if (*p != '\"')
  return 0;
 for (p++, i = 0; p[i] != '\"' && p[i] != '\0' && p[i] != '\n'
      && i < outsz - 1; i++)
  out[i] = p[i];
 out[i] = '\0';
 return 1;
}

static char *e_chk_next_directive(char *str, FILE *fp)
{
 char *p = e_chk_skip_whitespace_and_comments(str, str, fp);
 if (!p || *p != '#')
  return NULL;
 for (p++; isspace((unsigned char)*p); p++)
  ;
 return p;
}

typedef unsigned long M_TIME;

static int e_check_header_test(char *file, M_TIME otime, int sw)
{
 struct stat cbuf[1];
 FILE *fp;
 char *p, str[120], str2[120];
 int if_depth = 0, skip_depth = 0;

 if ((fp = fopen(file, "r")) == NULL)
  return(sw);
 stat(file, cbuf);
 if (otime < (unsigned long)cbuf->st_mtime)
  sw++;
 while (fgets(str, 120, fp))
 {
  p = e_chk_next_directive(str, fp);
  if (!p)
   continue;
  e_chk_track_conditional(p, &if_depth, &skip_depth);
  if (skip_depth == 0 && e_chk_is_directive(p, "include")
      && e_chk_extract_include(p, str2, sizeof(str2)))
   sw = e_check_header_test(str2, otime, sw);
 }
 fclose(fp);
 return(sw);
}

static void write_file(const char *path, const char *content)
{
 FILE *f = fopen(path, "w");
 fputs(content, f);
 fclose(f);
}

static void setup_fixtures(void)
{
 mkdir(TDIR, 0755);
 write_file(TDIR "/real.h", "int x;\n");
 write_file(TDIR "/fake.h", "int fake;\n");
 write_file(TDIR "/guarded.h",
  "#ifndef GUARDED_H\n#define GUARDED_H\nint g;\n#endif\n");
}

static void test_line_comment_ignored(void)
{
 printf("test_line_comment_ignored:\n");
 write_file(TDIR "/t1.c",
  "#include \"" TDIR "/real.h\"\n"
  "// #include \"" TDIR "/fake.h\"\n");
 int sw = e_check_header_test(TDIR "/t1.c", 0, 0);
 ASSERT_EQ(sw, 2, "real.h followed (2), fake.h in // comment skipped");
}

static void test_block_comment_ignored(void)
{
 printf("test_block_comment_ignored:\n");
 write_file(TDIR "/t2.c",
  "/* #include \"" TDIR "/fake.h\" */\n"
  "#include \"" TDIR "/real.h\"\n");
 int sw = e_check_header_test(TDIR "/t2.c", 0, 0);
 ASSERT_EQ(sw, 2, "block comment skipped, real.h followed");
}

static void test_multiline_block_comment(void)
{
 printf("test_multiline_block_comment:\n");
 write_file(TDIR "/t3.c",
  "/*\n"
  " * #include \"" TDIR "/fake.h\"\n"
  " */\n"
  "#include \"" TDIR "/real.h\"\n");
 int sw = e_check_header_test(TDIR "/t3.c", 0, 0);
 ASSERT_EQ(sw, 2, "multiline block comment skipped");
}

static void test_if_zero_skips(void)
{
 printf("test_if_zero_skips:\n");
 write_file(TDIR "/t4.c",
  "#if 0\n"
  "#include \"" TDIR "/fake.h\"\n"
  "#endif\n"
  "#include \"" TDIR "/real.h\"\n");
 int sw = e_check_header_test(TDIR "/t4.c", 0, 0);
 ASSERT_EQ(sw, 2, "#if 0 skips fake.h, real.h followed");
}

static void test_if_zero_else(void)
{
 printf("test_if_zero_else:\n");
 write_file(TDIR "/t5.c",
  "#if 0\n"
  "#include \"" TDIR "/fake.h\"\n"
  "#else\n"
  "#include \"" TDIR "/real.h\"\n"
  "#endif\n");
 int sw = e_check_header_test(TDIR "/t5.c", 0, 0);
 ASSERT_EQ(sw, 2, "#if 0 skips fake.h, #else follows real.h");
}

static void test_nested_if_zero(void)
{
 printf("test_nested_if_zero:\n");
 write_file(TDIR "/t6.c",
  "#if 0\n"
  "#ifdef SOMETHING\n"
  "#include \"" TDIR "/fake.h\"\n"
  "#endif\n"
  "#endif\n"
  "#include \"" TDIR "/real.h\"\n");
 int sw = e_check_header_test(TDIR "/t6.c", 0, 0);
 ASSERT_EQ(sw, 2, "nested #if 0 skips all inner includes");
}

static void test_ifdef_conservative(void)
{
 printf("test_ifdef_conservative:\n");
 write_file(TDIR "/t7.c",
  "#ifdef UNDEFINED_SYMBOL\n"
  "#include \"" TDIR "/fake.h\"\n"
  "#endif\n"
  "#include \"" TDIR "/real.h\"\n");
 int sw = e_check_header_test(TDIR "/t7.c", 0, 0);
 ASSERT_EQ(sw, 3, "#ifdef follows conservatively (no symbol table)");
}

static void test_ifndef_guard_transparent(void)
{
 printf("test_ifndef_guard_transparent:\n");
 write_file(TDIR "/t8.c",
  "#include \"" TDIR "/guarded.h\"\n");
 int sw = e_check_header_test(TDIR "/t8.c", 0, 0);
 ASSERT_EQ(sw, 2, "ifndef guard does not block header check");
}

static void test_system_include_not_followed(void)
{
 printf("test_system_include_not_followed:\n");
 write_file(TDIR "/t9.c",
  "#include <stdio.h>\n"
  "#include <stdlib.h>\n");
 int sw = e_check_header_test(TDIR "/t9.c", 0, 0);
 ASSERT_EQ(sw, 1, "system <> includes not followed (only file counted)");
}

static void test_no_includes(void)
{
 printf("test_no_includes:\n");
 write_file(TDIR "/t10.c", "int main(void) { return 0; }\n");
 int sw = e_check_header_test(TDIR "/t10.c", 0, 0);
 ASSERT_EQ(sw, 1, "no includes: only file itself counted");
}

int main(void)
{
 setup_fixtures();

 test_line_comment_ignored();
 test_block_comment_ignored();
 test_multiline_block_comment();
 test_if_zero_skips();
 test_if_zero_else();
 test_nested_if_zero();
 test_ifdef_conservative();
 test_ifndef_guard_transparent();
 test_system_include_not_followed();
 test_no_includes();

 printf("\n%d/%d tests passed\n", tests_passed, tests_run);
 return tests_passed == tests_run ? 0 : 1;
}
