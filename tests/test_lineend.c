/* test_lineend.c -- unit test for the owner of a file's line endings
 * (we_lineend.c) on the buffer's owner (we_buffer.c).  The editor's side of
 * the buffer's seam is played by the recorders below.  Always runs.
 *
 * What the user would see go wrong if a rule broke: a CRLF file saved with
 * LF or an LF file saved with CRLF, a line the user never touched rewritten
 * because another line of the file had a carriage return, a stray carriage
 * return deleted from the text, a file that gains or loses its last line
 * break unasked, two lines merged by a NUL byte, a long line cut where its
 * carriage return was, text the buffer made coming back changed from a
 * whole-text replace.
 */
/* @scenario SC-line-endings-17 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "../we_buffer.h"
#include "../we_lineend.h"

#define WIDTH   80
#define ROWS_MAX 16

static int fails;

static void chk(const char *what, int got, int want)
{
 if (got != want) {
  printf("FAIL: %s: got %d want %d\n", what, got, want);
  fails++;
 }
}

/* ---- the editor's side of the buffer's seam, as no-ops ---- */

void wpe_buffer_syntax_changed(BUFFER *b, int y) { (void) b; (void) y; }

void wpe_buffer_lines_moved(BUFFER *b, int y, int n_del, int n_ins,
                            const int *rows_removed, int n_removed,
                            const int *rows_added, int n_added)
{
 (void) b; (void) y; (void) n_del; (void) n_ins; (void) rows_removed;
 (void) n_removed; (void) rows_added; (void) n_added;
}

void wpe_buffer_say_nomem(BUFFER *b, const char *what)
{
 (void) b;
 printf("FAIL: refused for memory: %s\n", what);
 fails++;
}

void wpe_buffer_say_shape(BUFFER *b, const char *what) { (void) b; (void) what; }
void wpe_buffer_say_partial(BUFFER *b) { (void) b; }
void wpe_buffer_say_read_only(BUFFER *b) { (void) b; }
int wpe_buffer_replay_kind(BUFFER *b, SCHIRM *s, Undo *rec)
{
 (void) b; (void) s; (void) rec;
 return(E_BUFFER_OK);
}
void wpe_buffer_replay_on_shell(BUFFER *shell, SCHIRM *s, const Undo *rec, int y0)
{
 (void) shell; (void) s; (void) rec; (void) y0;
}
void wpe_buffer_trace(const char *fmt, ...) { (void) fmt; }

/* ---- helpers ---- */

/* An editor whose options file set nothing: new files are LF. */
static ECNT no_options;

/* A buffer holding the lines of @bytes as a file read into a fresh window,
   with what the reader found in @info. */
static BUFFER *read_bytes(const char *bytes, size_t n, int mx_x, WpeEolInfo *info)
{
 BUFFER *b = calloc(1, sizeof(BUFFER));
 FILE *fp = tmpfile();
 POINT end;

 e_buffer_init(b, NULL, NULL, mx_x, E_BUFFER_NO_HISTORY);
 if (n > 0)
  fwrite(bytes, 1, n, fp);
 rewind(fp);
 chk("the file is read", e_eol_read_file_into(fp, b, 0, 0, 0, info, &end), E_BUFFER_OK);
 fclose(fp);
 /* as a window does: the fresh empty line the reader pushed to the end goes */
 if (b->mxlines > 1 && b->bf[b->mxlines - 1].s[0] == '\0')
  e_buffer_delete_lines(b, b->mxlines - 1, 1, E_BUFFER_STEP_COVERED);
 e_eol_buffer_adopt(b, &no_options, info);
 return(b);
}

/* What a save of the whole buffer writes. */
static char *written(BUFFER *b, int keep_final, size_t *n)
{
 char *out;

 *n = e_eol_text(b, 0, 0, b->mxlines - 1, E_BUFFER_ROW_END(b), b->eol, keep_final, &out);
 return(out);
}

static void chk_bytes(const char *what, const char *got, size_t gn, const char *want, size_t wn)
{
 if (gn != wn || memcmp(got, want, wn) != 0) {
  size_t i;

  printf("FAIL: %s: got %zu bytes \"", what, gn);
  for (i = 0; i < gn; i++)
   printf(got[i] >= 32 && got[i] < 127 ? "%c" : "\\x%02x", (unsigned char) got[i]);
  printf("\" want \"");
  for (i = 0; i < wn; i++)
   printf(want[i] >= 32 && want[i] < 127 ? "%c" : "\\x%02x", (unsigned char) want[i]);
  printf("\"\n");
  fails++;
 }
}

static void free_buffer(BUFFER *b)
{
 e_buffer_lines_free(b->bf, b->mxlines);
 free(b);
}

#define LIT(s)  (s), (sizeof(s) - 1)

/* A file read and written back with the final break as it was: the bytes
   are the bytes, the style and the final break are what the file said. */
static void round_trip(const char *what, const char *bytes, size_t n, int eol, int final_break,
                       int breaks)
{
 WpeEolInfo info;
 BUFFER *b = read_bytes(bytes, n, WIDTH, &info);
 size_t wn;
 char *out = written(b, 1, &wn);

 chk(what, b->eol, eol);
 chk(what, b->final_break, final_break);
 chk(what, info.breaks, breaks);
 chk_bytes(what, out, wn, bytes, n);
 free(out);
 free_buffer(b);
}

/* ---- cases ---- */

static void test_styles_and_round_trips(void)
{
 round_trip("crlf file", LIT("a\r\nb\r\n"), E_EOL_CRLF, E_FINAL_BREAK_HAD, 2);
 round_trip("lf file", LIT("a\nb\n"), E_EOL_LF, E_FINAL_BREAK_HAD, 2);
 round_trip("mixed file", LIT("a\r\nb\nc\r\n"), E_EOL_LF, E_FINAL_BREAK_HAD, 3);
 round_trip("lone cr", LIT("a\rb\n"), E_EOL_LF, E_FINAL_BREAK_HAD, 1);
 round_trip("crlf without a final break", LIT("a\r\nb"), E_EOL_CRLF, E_FINAL_BREAK_NONE, 1);
 round_trip("lf without a final break", LIT("a\nb"), E_EOL_LF, E_FINAL_BREAK_NONE, 1);
 round_trip("empty file", "", 0, E_EOL_LF, E_FINAL_BREAK_NONE, 0);
 round_trip("one line, no break", LIT("abc"), E_EOL_LF, E_FINAL_BREAK_NONE, 0);
 round_trip("cr cr lf", LIT("a\r\r\nb\r\n"), E_EOL_CRLF, E_FINAL_BREAK_HAD, 2);
 round_trip("cr at the end of an lf file", LIT("a\nb\r"), E_EOL_LF, E_FINAL_BREAK_NONE, 1);
 round_trip("a byte order mark stays", LIT("\xef\xbb\xbf" "a\r\nb\r\n"), E_EOL_CRLF,
            E_FINAL_BREAK_HAD, 2);
 round_trip("blank lines", LIT("\r\n\r\na\r\n"), E_EOL_CRLF, E_FINAL_BREAK_HAD, 3);
}

static void test_the_carriage_return_is_text_in_an_lf_file(void)
{
 WpeEolInfo info;
 BUFFER *b = read_bytes(LIT("a\r\nb\nc\r\n"), WIDTH, &info);

 chk("a mixed file is LF", info.eol, E_EOL_LF);
 chk("a mixed file says so", info.mixed, 1);
 chk("row 0 keeps its carriage return", b->bf[0].len, 2);
 chk("row 0 byte", b->bf[0].s[1], '\r');
 chk("row 1 has none", b->bf[1].len, 1);
 free_buffer(b);

 b = read_bytes(LIT("a\rb\n"), WIDTH, &info);
 chk("a lone carriage return is counted", info.lone_cr, 1);
 chk("one row", b->mxlines, 1);
 chk("the row is a^Mb", b->bf[0].len, 3);
 free_buffer(b);
}

static void test_the_final_break_is_kept_or_added(void)
{
 WpeEolInfo info;
 BUFFER *b = read_bytes(LIT("a\r\nb"), WIDTH, &info);
 size_t n;
 char *out;

 out = written(b, 0, &n);
 chk_bytes("a save adds the final break", out, n, LIT("a\r\nb\r\n"));
 free(out);
 out = written(b, 1, &n);
 chk_bytes("a save that keeps the file's ending", out, n, LIT("a\r\nb"));
 free(out);
 free_buffer(b);

 b = read_bytes("", 0, WIDTH, &info);
 out = written(b, 0, &n);
 chk_bytes("an empty file saved the old way is one line", out, n, LIT("\n"));
 free(out);
 out = written(b, 1, &n);
 chk_bytes("an empty file stays empty when the ending is kept", out, n, "", 0);
 free(out);
 free_buffer(b);
}

static void test_an_empty_or_unbroken_file_takes_the_default(void)
{
 ECNT cn;
 WpeEolInfo info;
 BUFFER *b;

 memset(&cn, 0, sizeof(cn));
 e_eol_set_default(&cn, 109);
 chk("Data 109 is CRLF", e_eol_default(&cn), E_EOL_CRLF);
 chk("Data 109 is written back as 109", e_eol_data_key(&cn), 109);

 b = read_bytes("", 0, WIDTH, &info);
 e_eol_buffer_adopt(b, &cn, &info);
 chk("an empty file takes the default", b->eol, E_EOL_CRLF);
 free_buffer(b);

 b = read_bytes(LIT("one line"), WIDTH, &info);
 e_eol_buffer_adopt(b, &cn, &info);
 chk("a file with no break takes the default", b->eol, E_EOL_CRLF);
 free_buffer(b);

 b = read_bytes(LIT("a\nb\n"), WIDTH, &info);
 e_eol_buffer_adopt(b, &cn, &info);
 chk("a file with breaks keeps its own", b->eol, E_EOL_LF);
 free_buffer(b);

 b = read_bytes(LIT("a\r\n"), WIDTH, &info);
 e_eol_set_default(&cn, 110);
 e_eol_buffer_adopt(b, &cn, &info);
 chk("a CRLF file keeps its own against an LF default", b->eol, E_EOL_CRLF);
 free_buffer(b);

 /* the key is a style, never a window kind: nothing but 109 is CRLF */
 e_eol_set_default(&cn, 70);
 chk("Data 70 is LF", e_eol_default(&cn), E_EOL_LF);
 chk("Data 70 is written back as 110", e_eol_data_key(&cn), 110);
 e_eol_set_default(&cn, 0);
 chk("Data 0 is LF", e_eol_default(&cn), E_EOL_LF);
 e_eol_set_default(&cn, 68);
 chk("Data 68 is LF", e_eol_default(&cn), E_EOL_LF);
 memset(&cn, 0, sizeof(cn));
 chk("a zeroed editor is LF", e_eol_default(&cn), E_EOL_LF);
}

static void test_zero_values_are_lf_with_no_break(void)
{
 BUFFER b;

 memset(&b, 0, sizeof(b));
 chk("a buffer nobody set is LF", b.eol, E_EOL_LF);
 chk("a buffer nobody set has recorded no break", b.final_break, E_FINAL_BREAK_NONE);
 chk("LF is zero", E_EOL_LF, 0);
 chk("no final break is zero", E_FINAL_BREAK_NONE, 0);
}

static void test_a_nul_byte_loses_nothing_but_itself(void)
{
 WpeEolInfo info;
 BUFFER *b = read_bytes(LIT("a\0b\nc\n"), WIDTH, &info);
 size_t n;
 char *out = written(b, 1, &n);

 chk("the NUL is counted", info.nul, 1);
 chk("two lines, none merged", b->mxlines, 2);
 chk_bytes("the rest of the row and its break survive", out, n, LIT("ab\nc\n"));
 free(out);
 free_buffer(b);

 b = read_bytes(LIT("a\0b\r\nc\r\n"), WIDTH, &info);
 out = written(b, 1, &n);
 chk_bytes("the same in a CRLF file", out, n, LIT("ab\r\nc\r\n"));
 free(out);
 free_buffer(b);
}

static void test_a_long_line_is_cut_before_its_break(void)
{
 char bytes[256];
 WpeEolInfo info;
 BUFFER *b;
 size_t n, i;
 char *out;

 for (i = 0; i < 200; i++)
  bytes[i] = 'a';
 memcpy(bytes + 200, "\r\nb\r\n", 5);
 n = 205;
 b = read_bytes(bytes, n, WIDTH, &info);
 chk("a CRLF line is the same however long", info.eol, E_EOL_CRLF);
 chk("it is wrapped into rows", b->mxlines > 2, 1);
 chk("no row holds a carriage return", b->bf[0].s[b->bf[0].len - 1], 'a');
 out = written(b, 1, &i);
 chk_bytes("and written back as one line", out, i, bytes, n);
 free(out);
 free_buffer(b);

 memcpy(bytes + 200, "\r\nb\n", 4);
 n = 204;
 b = read_bytes(bytes, n, WIDTH, &info);
 chk("an LF file is LF", info.eol, E_EOL_LF);
 out = written(b, 1, &i);
 chk_bytes("a carriage return at a wrap stays text", out, i, bytes, n);
 free(out);
 free_buffer(b);
}

/* Rows the buffer made, as text, read back by E_EOL_ROWS: the same rows. */
static void rows_round_trip(const char *what, const char *bytes, size_t n, int width)
{
 WpeEolInfo info;
 BUFFER *b = read_bytes(bytes, n, width, &info);
 char *text;
 size_t tn;
 STRING *rows;
 int count, y;

 tn = e_eol_text(b, 0, 0, b->mxlines - 1, E_BUFFER_ROW_END(b), E_EOL_LF, 0, &text);
 rows = e_eol_lines((const unsigned char *) text, tn, width, E_EOL_ROWS, NULL, &count);
 chk(what, count, b->mxlines);
 for (y = 0; y < count && y < b->mxlines; y++) {
  if (rows[y].len != b->bf[y].len ||
      memcmp(rows[y].s, b->bf[y].s, (size_t) b->bf[y].len) != 0 ||
      rows[y].s[rows[y].len] != b->bf[y].s[b->bf[y].len]) {
   printf("FAIL: %s: row %d differs\n", what, y);
   fails++;
  }
 }
 e_buffer_lines_free(rows, count);
 free(text);
 free_buffer(b);
}

static void test_text_the_buffer_made_goes_back_as_it_was(void)
{
 char wide[300];
 size_t i;

 rows_round_trip("a file", LIT("a\nb\n"), WIDTH);
 rows_round_trip("a carriage return at a row's end", LIT("a\r\nb\nc\r\n"), WIDTH);
 rows_round_trip("a lone carriage return", LIT("a\rb\n"), WIDTH);
 rows_round_trip("a carriage return at the buffer's end", LIT("a\nb\r"), WIDTH);
 rows_round_trip("a CRLF file, whose rows hold no carriage return", LIT("a\r\nb\r\n"), WIDTH);
 rows_round_trip("a blank line", LIT("a\n\nb\n"), WIDTH);
 for (i = 0; i < 200; i++)
  wide[i] = (i % 7 == 6) ? ' ' : 'x';
 memcpy(wide + 200, "\nend\n", 5);
 rows_round_trip("a wrapped line", wide, 205, 40);
}

static void test_the_rows_policy_splits_on_lf_only(void)
{
 int count;
 STRING *rows = e_eol_lines((const unsigned char *) "a\r\nb\rc", 6, WIDTH, E_EOL_ROWS, NULL,
                            &count);

 chk("two rows", count, 2);
 chk("row 0 keeps its CR", rows[0].len, 2);
 chk("row 1 keeps its lone CR", rows[1].len, 3);
 e_buffer_lines_free(rows, count);
 rows = e_eol_lines((const unsigned char *) "", 0, WIDTH, E_EOL_ROWS, NULL, &count);
 chk("empty text has no row", count, 0);
}

static void test_the_buffer_remembers_the_nul_bytes_the_reader_left_out(void)
{
 WpeEolInfo info;
 BUFFER *b = read_bytes(LIT("a\0b\0c\nd\n"), WIDTH, &info);
 char line[E_NUL_SAY_MAX];

 chk("the reader counts them", info.nul, 2);
 chk("the buffer keeps the count for the first Save", b->nul_dropped, 2);
 e_eol_nul_sentence(b->nul_dropped, line, sizeof line);
 chk("the sentence says how many", strcmp(line, "2 NUL bytes of the file were left out "
                                                 "when they were read"), 0);
 e_eol_nul_sentence(1, line, sizeof line);
 chk("and the singular", strcmp(line, "1 NUL byte of the file was left out when it was read"), 0);
 free_buffer(b);
 b = read_bytes(LIT("a\nb\n"), WIDTH, &info);
 chk("a file without any records none", b->nul_dropped, 0);
 free_buffer(b);
}

static void test_the_normalize_policy_reads_a_cr_lf_as_one_break(void)
{
 int count;
 STRING *rows = e_eol_lines((const unsigned char *) "a\r\nb\rc\r\n", 8, WIDTH, E_EOL_NORMALIZE,
                            NULL, &count);
 char buf[16];
 size_t n;

 chk("two rows", count, 2);
 chk("row 0 has no carriage return", rows[0].len, 1);
 chk("row 1 keeps its lone carriage return", rows[1].len, 3);
 chk("row 1 is b CR c", memcmp(rows[1].s, "b\rc", 3), 0);
 e_buffer_lines_free(rows, count);

 n = e_eol_normalize(buf, "x\r\ny\rz\r", 7);
 chk_bytes("a CR before an LF goes, the others stay", buf, n, LIT("x\ny\rz\r"));
 memcpy(buf, "x\r\r\ny\r\n", 8);
 n = e_eol_normalize(buf, buf, 7);
 chk_bytes("in place, and only the CR next to the LF goes", buf, n, LIT("x\r\ny\n"));
 n = e_eol_normalize(buf, "", 0);
 chk("empty text", (int) n, 0);
}

static void test_appended_program_output_has_no_carriage_returns(void)
{
 WpeEolInfo info;
 BUFFER *b = read_bytes(LIT("first\n"), WIDTH, &info);
 int before = b->mxlines;

 chk("the text is appended",
     e_eol_append_text(b, "one\r\n\r\ntwo\r\n", 1, E_EOL_NORMALIZE), E_BUFFER_OK);
 chk("two rows were added, the blank one left out", b->mxlines, before + 2);
 chk("the first has no carriage return", b->bf[before].len, 3);
 chk("nor the second", b->bf[before + 1].len, 3);
 free_buffer(b);
}

static void test_a_text_the_buffer_made_is_not_normalized(void)
{
 WpeEolInfo info;
 BUFFER *b = read_bytes(LIT("a\r\nb\nc\r\n"), WIDTH, &info);
 char *text;
 size_t tn;
 STRING *rows;
 int count;

 tn = e_eol_text(b, 0, 0, b->mxlines - 1, E_BUFFER_ROW_END(b), E_EOL_LF, 0, &text);
 chk_bytes("an LF file with carriage returns in two rows", text, tn, LIT("a\r\nb\nc\r\n"));
 rows = e_eol_lines((const unsigned char *) text, tn, WIDTH, E_EOL_NORMALIZE, NULL, &count);
 chk("normalizing it loses the carriage return of row 0", rows[0].len, 1);
 e_buffer_lines_free(rows, count);
 rows = e_eol_lines((const unsigned char *) text, tn, WIDTH, E_EOL_ROWS, NULL, &count);
 chk("reading it as the buffer's rows keeps it", rows[0].len, 2);
 e_buffer_lines_free(rows, count);
 free(text);
 free_buffer(b);
}

/* A file read as a manual page: its backspace sequences make bold runs and
   every carriage return is dropped, in a CRLF file and in an LF one. */
static BUFFER *read_manpage(const char *bytes, size_t n, WpeEolInfo *info)
{
 BUFFER *b = calloc(1, sizeof(BUFFER));
 FILE *fp = tmpfile();
 POINT end;

 e_buffer_init(b, NULL, NULL, WIDTH, E_BUFFER_NO_HISTORY);
 fwrite(bytes, 1, n, fp);
 rewind(fp);
 chk("the manual page is read", e_eol_read_file_into(fp, b, 0, 0, 1, info, &end), E_BUFFER_OK);
 fclose(fp);
 return(b);
}

static void test_the_manual_page_filter_is_the_readers_own(void)
{
 WpeEolInfo info;
 BUFFER *b = read_manpage(LIT("abc\r\ndef\rg\n"), &info);

 chk("two rows and the fresh line", b->mxlines, 3);
 chk("row 0 has no carriage return", b->bf[0].len, 3);
 chk("row 1 has none either, lone or not", b->bf[1].len, 4);
 chk("row 1 is d e f g", memcmp(b->bf[1].s, "defg", 4), 0);
 free_buffer(b);

 b = read_manpage(LIT("N\bNAME\nplain\n"), &info);
 chk_bytes("the bold run is marked", b->bf[0].s, (size_t) b->bf[0].len, LIT("\x0f" "N\x05" "AME"));
 chk("the plain line follows", memcmp(b->bf[1].s, "plain", 5), 0);
 free_buffer(b);
}

static void test_a_range_is_written_as_e_write_took_it(void)
{
 WpeEolInfo info;
 BUFFER *b = read_bytes(LIT("one\r\ntwo\r\nthree\r\n"), WIDTH, &info);
 size_t n;
 char *out;

 n = e_eol_text(b, 0, 0, 1, E_BUFFER_ROW_END(b), b->eol, 0, &out);
 chk_bytes("rows 0 and 1", out, n, LIT("one\r\ntwo\r\n"));
 free(out);
 n = e_eol_text(b, 1, 1, 2, 2, b->eol, 0, &out);
 chk_bytes("from a column to a column", out, n, LIT("wo\r\nth"));
 free(out);
 free_buffer(b);
}

int main(void)
{
 test_styles_and_round_trips();
 test_the_carriage_return_is_text_in_an_lf_file();
 test_the_final_break_is_kept_or_added();
 test_an_empty_or_unbroken_file_takes_the_default();
 test_zero_values_are_lf_with_no_break();
 test_a_nul_byte_loses_nothing_but_itself();
 test_a_long_line_is_cut_before_its_break();
 test_text_the_buffer_made_goes_back_as_it_was();
 test_the_rows_policy_splits_on_lf_only();
 test_the_buffer_remembers_the_nul_bytes_the_reader_left_out();
 test_the_normalize_policy_reads_a_cr_lf_as_one_break();
 test_appended_program_output_has_no_carriage_returns();
 test_a_text_the_buffer_made_is_not_normalized();
 test_the_manual_page_filter_is_the_readers_own();
 test_a_range_is_written_as_e_write_took_it();
 if (fails) {
  printf("%d failure(s)\n", fails);
  return(1);
 }
 printf("test_lineend: ok\n");
 return(0);
}
