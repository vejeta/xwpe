/* test_lsp_positions.c -- unit test of the owner of the language server's
 * columns (we_lsp_pos.c), alone: the units of every glyph shape in both
 * encodings, the two conversions and their clamps, the rows of a text, the
 * offer and the answer, the wire's helpers and the semantic decoder.
 *
 * The expected numbers are written here by hand from the rule (a glyph of n
 * bytes is n units in utf-8; in utf-16 two for a code point above the BMP and
 * one for anything else, a stray byte included), never computed by the
 * owner. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <json-c/json.h>

#include "../we_lsp_pos.h"

static int fails;

static void chk(const char *name, int got, int want)
{
 if (got == want)
  printf("ok   - %s\n", name);
 else
 {  printf("FAIL - %s (want %d, got %d)\n", name, want, got);  fails++;  }
}

static void chk_str(const char *name, const char *got, const char *want)
{
 if (got && !strcmp(got, want))
  printf("ok   - %s\n", name);
 else
 {  printf("FAIL - %s (want %s, got %s)\n", name, want, got ? got : "(null)");
    fails++;  }
}

/* A column in the server's units against the number it should be. */
static void chku(const char *name, e_lsp_units got, int want)
{
 chk(name, e_lsp_units_eq(got, e_lsp_units_make(want)), 1);
}

/* Every shape a row can hold: ASCII, 2, 3 and 4 byte characters, and the
   bytes that are no character (a stray continuation, a cut lead, a Latin-1
   letter, an overlong form, a surrogate, a lead above F4, an F8 and its
   continuation).  Glyph starts: 0,1,3,6,10,11,12,13,14,16,19,23,24; len 25. */
#define ROW_A "a" "\xC3\xA9" "\xE6\xBC\xA2" "\xF0\x9F\x98\x80" "b" "\x80" "\xC3" \
              "\xE9" "\xC0\x80" "\xED\xA0\x80" "\xF5\x80\x80\x80" "\xF8" "\x80"
#define ROW_A_LEN 25
#define NSTARTS 13

static const int starts[NSTARTS] = { 0, 1, 3, 6, 10, 11, 12, 13, 14, 16, 19, 23, 24 };
static const int units16[NSTARTS] = { 0, 1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
#define UNITS16_AT_END 14
#define EMOJI_START 6
#define EMOJI_SECOND_UNIT 4          /* units 3 and 4 are the emoji's pair */

static const unsigned char *row_a(void)
{
 return (const unsigned char *)ROW_A;
}

static void case_units_of_every_glyph_start(void)
{
 int i;
 char name[64];

 for (i = 0; i < NSTARTS; i++)
 {
  snprintf(name, sizeof name, "utf-16 units at byte %d", starts[i]);
  chku(name, e_lsp_units_of(row_a(), ROW_A_LEN, starts[i], E_LSP_ENC_UTF16),
       units16[i]);
  snprintf(name, sizeof name, "utf-8 units at byte %d", starts[i]);
  chku(name, e_lsp_units_of(row_a(), ROW_A_LEN, starts[i], E_LSP_ENC_UTF8),
       starts[i]);
 }
 chku("utf-16 units at the row's end",
      e_lsp_units_of(row_a(), ROW_A_LEN, ROW_A_LEN, E_LSP_ENC_UTF16),
      UNITS16_AT_END);
 chku("utf-8 units at the row's end",
      e_lsp_units_of(row_a(), ROW_A_LEN, ROW_A_LEN, E_LSP_ENC_UTF8), ROW_A_LEN);
}

static void case_col_of_round_trips(void)
{
 int i;
 char name[64];

 for (i = 0; i < NSTARTS; i++)
 {
  e_lsp_units u16 = e_lsp_units_of(row_a(), ROW_A_LEN, starts[i], E_LSP_ENC_UTF16);
  e_lsp_units u8 = e_lsp_units_of(row_a(), ROW_A_LEN, starts[i], E_LSP_ENC_UTF8);

  snprintf(name, sizeof name, "utf-16 round trip at byte %d", starts[i]);
  chk(name, e_lsp_col_of(row_a(), ROW_A_LEN, u16, E_LSP_ENC_UTF16), starts[i]);
  snprintf(name, sizeof name, "utf-8 round trip at byte %d", starts[i]);
  chk(name, e_lsp_col_of(row_a(), ROW_A_LEN, u8, E_LSP_ENC_UTF8), starts[i]);
 }
}

static void case_units_inside_a_pair_go_back(void)
{
 chk("utf-16 unit 4 (the emoji's second) is the emoji's start",
     e_lsp_col_of(row_a(), ROW_A_LEN, e_lsp_units_make(EMOJI_SECOND_UNIT),
                  E_LSP_ENC_UTF16), EMOJI_START);
 chk("utf-8 byte 8 (inside the emoji) is the emoji's start",
     e_lsp_col_of(row_a(), ROW_A_LEN, e_lsp_units_make(8), E_LSP_ENC_UTF8),
     EMOJI_START);
 chk("utf-8 byte 2 (inside the e acute) is its start",
     e_lsp_col_of(row_a(), ROW_A_LEN, e_lsp_units_make(2), E_LSP_ENC_UTF8), 1);
}

static void case_units_past_the_row_clamp(void)
{
 chk("unit 99 is the row's end",
     e_lsp_col_of(row_a(), ROW_A_LEN, e_lsp_units_make(99), E_LSP_ENC_UTF16),
     ROW_A_LEN);
 chk("unit 99 in utf-8 is the row's end",
     e_lsp_col_of(row_a(), ROW_A_LEN, e_lsp_units_make(99), E_LSP_ENC_UTF8),
     ROW_A_LEN);
 chk("unit -1 is the row's start",
     e_lsp_col_of(row_a(), ROW_A_LEN, e_lsp_units_make(-1), E_LSP_ENC_UTF16), 0);
 chku("byte 99 counts the units of the row's end",
      e_lsp_units_of(row_a(), ROW_A_LEN, 99, E_LSP_ENC_UTF16), UNITS16_AT_END);
 chku("byte -3 is unit 0",
      e_lsp_units_of(row_a(), ROW_A_LEN, -3, E_LSP_ENC_UTF16), 0);
 chk("an empty row has column 0 for any unit",
     e_lsp_col_of(row_a(), 0, e_lsp_units_make(5), E_LSP_ENC_UTF16), 0);
 chk("a NULL row has column 0",
     e_lsp_col_of(NULL, 0, e_lsp_units_make(5), E_LSP_ENC_UTF16), 0);
 chku("a NULL row has units 0",
      e_lsp_units_of(NULL, 0, 5, E_LSP_ENC_UTF16), 0);
}

static void case_a_byte_inside_a_glyph_counts_its_start(void)
{
 chku("byte 2 (inside the e acute) counts as byte 1",
      e_lsp_units_of(row_a(), ROW_A_LEN, 2, E_LSP_ENC_UTF16), 1);
 chku("byte 8 (inside the emoji) counts as byte 6",
      e_lsp_units_of(row_a(), ROW_A_LEN, 8, E_LSP_ENC_UTF16), 3);
 chku("byte 8 in utf-8 counts as byte 6",
      e_lsp_units_of(row_a(), ROW_A_LEN, 8, E_LSP_ENC_UTF8), 6);
}

static void case_the_rows_of_a_text(void)
{
 const char *text = "ab\n" ROW_A "\ncd";
 const char *row;
 int len = -1;

 row = e_lsp_text_row(text, 0, &len);
 chk("row 0 starts the text", row == text, 1);
 chk("row 0 has two bytes", len, 2);
 row = e_lsp_text_row(text, 1, &len);
 chk("row 1 starts after the first newline", row == text + 3, 1);
 chk("row 1 is row A", len, ROW_A_LEN);
 row = e_lsp_text_row(text, 2, &len);
 chk("the last row has no terminator", row == text + 3 + ROW_A_LEN + 1, 1);
 chk("the last row has two bytes", len, 2);
 chk("a row past the text is NULL", e_lsp_text_row(text, 3, &len) == NULL, 1);
 chk("a negative row is NULL", e_lsp_text_row(text, -1, &len) == NULL, 1);
 chk("no text is NULL", e_lsp_text_row(NULL, 0, &len) == NULL, 1);
 row = e_lsp_text_row("x\n", 1, &len);
 chk("the row after a final newline exists and is empty", row != NULL && len == 0, 1);
}

static void case_text_offset_finds_the_row(void)
{
 const char *text = "ab\n" ROW_A "\ncd";

 chk("line 1 unit 5 is byte 10 of row A",
     (int)e_lsp_text_offset(text, 1, e_lsp_units_make(5), E_LSP_ENC_UTF16),
     3 + 10);
 chk("a line past the text is the text's end",
     (int)e_lsp_text_offset(text, 9, e_lsp_units_make(0), E_LSP_ENC_UTF16),
     (int)strlen(text));
 chk("units past the row are the row's end, not the next row",
     (int)e_lsp_text_offset(text, 1, e_lsp_units_make(99), E_LSP_ENC_UTF16),
     3 + ROW_A_LEN);
 chk("line 2 unit 1 is the second byte of the last row",
     (int)e_lsp_text_offset(text, 2, e_lsp_units_make(1), E_LSP_ENC_UTF16),
     3 + ROW_A_LEN + 1 + 1);
}

static void case_the_index_matches_the_scan(void)
{
 const char *text = "ab\n" ROW_A "\ncd";
 e_lsp_text_index *ix = e_lsp_text_index_new(text);
 int len = -1;
 const char *row;

 chk("the index is built", ix != NULL, 1);
 chk("index col, line 1 unit 5",
     e_lsp_text_index_col(ix, 1, e_lsp_units_make(5), E_LSP_ENC_UTF16), 10);
 chk("index col, line 1 unit 4 goes back to the emoji",
     e_lsp_text_index_col(ix, 1, e_lsp_units_make(4), E_LSP_ENC_UTF16), 6);
 chk("index col, line 1 units past the row",
     e_lsp_text_index_col(ix, 1, e_lsp_units_make(99), E_LSP_ENC_UTF16),
     ROW_A_LEN);
 chk("index col, a line the text does not have, is 0",
     e_lsp_text_index_col(ix, 7, e_lsp_units_make(3), E_LSP_ENC_UTF16), 0);
 chk("index offset equals the scan's offset",
     (int)e_lsp_text_index_offset(ix, 1, e_lsp_units_make(5), E_LSP_ENC_UTF16),
     (int)e_lsp_text_offset(text, 1, e_lsp_units_make(5), E_LSP_ENC_UTF16));
 chk("index offset of a line past the text is the text's end",
     (int)e_lsp_text_index_offset(ix, 9, e_lsp_units_make(0), E_LSP_ENC_UTF16),
     (int)strlen(text));
 row = e_lsp_text_index_row(ix, 2, &len);
 chk("index row 2 is the last row", row == text + 3 + ROW_A_LEN + 1 && len == 2, 1);
 chk("index row past the text is NULL",
     e_lsp_text_index_row(ix, 3, &len) == NULL, 1);
 e_lsp_text_index_free(ix);

 ix = e_lsp_text_index_new(NULL);
 chk("an index over no text is built", ix != NULL, 1);
 chk("no text: the units are taken as bytes",
     e_lsp_text_index_col(ix, 1, e_lsp_units_make(7), E_LSP_ENC_UTF16), 7);
 chk("no text: no row", e_lsp_text_index_row(ix, 0, &len) == NULL, 1);
 e_lsp_text_index_free(ix);
 chk("a NULL index takes the units as bytes",
     e_lsp_text_index_col(NULL, 1, e_lsp_units_make(7), E_LSP_ENC_UTF16), 7);
 e_lsp_text_index_free(NULL);

 ix = e_lsp_text_index_new("");
 chk("the empty text has one empty row",
     e_lsp_text_index_col(ix, 0, e_lsp_units_make(4), E_LSP_ENC_UTF16), 0);
 e_lsp_text_index_free(ix);
}

static void case_unread_row_takes_units_as_bytes(void)
{
 chk("units are bytes when the row cannot be read",
     e_lsp_col_unread_row(e_lsp_units_make(12)), 12);
}

static void case_encoding_parse(void)
{
 chk("NULL is utf-16", e_lsp_encoding_parse(NULL), E_LSP_ENC_UTF16);
 chk("utf-16 is utf-16", e_lsp_encoding_parse("utf-16"), E_LSP_ENC_UTF16);
 chk("utf-8 is utf-8", e_lsp_encoding_parse("utf-8"), E_LSP_ENC_UTF8);
 chk("utf-32 is read as utf-16", e_lsp_encoding_parse("utf-32"), E_LSP_ENC_UTF16);
 chk("UTF-8 in capitals is read as utf-16",
     e_lsp_encoding_parse("UTF-8"), E_LSP_ENC_UTF16);
 chk("the empty answer is utf-16", e_lsp_encoding_parse(""), E_LSP_ENC_UTF16);
 chk("an absent answer is a known one", e_lsp_encoding_answer_is_known(NULL), 1);
 chk("utf-8 is a known answer", e_lsp_encoding_answer_is_known("utf-8"), 1);
 chk("utf-32 is not a known answer", e_lsp_encoding_answer_is_known("utf-32"), 0);
 chk_str("name of utf-8", e_lsp_encoding_name(E_LSP_ENC_UTF8), "utf-8");
 chk_str("name of utf-16", e_lsp_encoding_name(E_LSP_ENC_UTF16), "utf-16");
}

static void case_offered_is_utf8_then_utf16(void)
{
 struct json_object *o = e_lsp_encodings_offered();

 chk("the offer is an array of two", json_object_array_length(o), 2);
 chk_str("first is utf-8",
         json_object_get_string(json_object_array_get_idx(o, 0)), "utf-8");
 chk_str("second is utf-16",
         json_object_get_string(json_object_array_get_idx(o, 1)), "utf-16");
 json_object_put(o);
}

static void case_semantic_decode_typed(void)
{
 e_lsp_sem_token t[8];
 int n;

 {
  int data[] = { 0,0,5,15,0,   0,6,3,8,0,   2,4,4,13,0 };
  n = e_lsp_semantic_decode(data, (int)(sizeof(data) / sizeof(data[0])), t, 8);
  chk("decode count", n, 3);
  chk("t0.line", t[0].line, 0);
  chku("t0.start", t[0].start, 0);
  chku("t0.length", t[0].length, 5);
  chk("t0.type", t[0].type, 15);
  chk("t1 same line", t[1].line, 0);
  chku("t1.start += dchar", t[1].start, 6);
  chk("t2 new line", t[2].line, 2);
  chku("t2.start absolute", t[2].start, 4);
  chku("a token's end is start + length",
       e_lsp_units_sum(t[1].start, t[1].length), 9);
 }
 {
  int data[] = { 0,0,3,1,0,   0,2 };
  chk("short tail ignored", e_lsp_semantic_decode(data, 7, t, 8), 1);
 }
 {
  int data[] = { 5,0,1,1,0,   0,3,1,2,0,   0,3,1,3,0 };
  n = e_lsp_semantic_decode(data, 15, t, 2);
  chk("count capped at max", n, 2);
  chku("t1.start within cap", t[1].start, 3);
 }
 chk("NULL data", e_lsp_semantic_decode(NULL, 5, t, 8), 0);
}

static void case_units_sum_saturates(void)
{
 e_lsp_units big = e_lsp_units_make(2147483647);

 chku("a sum that would overflow stays at the largest",
      e_lsp_units_sum(big, e_lsp_units_make(5)), 2147483647);
 chku("a negative count from the wire is 0", e_lsp_units_make(-4), 0);
}

static void case_position_json_round_trip(void)
{
 struct json_object *pos = e_lsp_position_json(7, e_lsp_units_make(25));
 struct json_object *line = NULL;
 e_lsp_units u;

 json_object_object_get_ex(pos, "line", &line);
 chk("position line", json_object_get_int(line), 7);
 chk("units read back from a position", e_lsp_units_read(pos, &u), 1);
 chku("units value", u, 25);
 json_object_put(pos);

 pos = json_object_new_object();
 chk("an absent character reads as not present", e_lsp_units_read(pos, &u), 0);
 chku("an absent character is 0", u, 0);
 chk("a NULL position reads as not present", e_lsp_units_read(NULL, &u), 0);
 json_object_put(pos);
}

static void case_line_range_runs_past_any_line(void)
{
 struct json_object *r = e_lsp_line_range(2, 9);
 struct json_object *st = NULL, *en = NULL, *c = NULL, *l = NULL;
 e_lsp_units u;

 json_object_object_get_ex(r, "start", &st);
 json_object_object_get_ex(r, "end", &en);
 e_lsp_units_read(st, &u);
 chku("start character is 0", u, 0);
 json_object_object_get_ex(st, "line", &l);
 chk("start line", json_object_get_int(l), 2);
 json_object_object_get_ex(en, "line", &l);
 chk("end line", json_object_get_int(l), 9);
 json_object_object_get_ex(en, "character", &c);
 chk("end character is past any line", json_object_get_int(c) > 100000, 1);
 json_object_put(r);
}

int main(void)
{
 case_units_of_every_glyph_start();
 case_col_of_round_trips();
 case_units_inside_a_pair_go_back();
 case_units_past_the_row_clamp();
 case_a_byte_inside_a_glyph_counts_its_start();
 case_the_rows_of_a_text();
 case_text_offset_finds_the_row();
 case_the_index_matches_the_scan();
 case_unread_row_takes_units_as_bytes();
 case_encoding_parse();
 case_offered_is_utf8_then_utf16();
 case_semantic_decode_typed();
 case_units_sum_saturates();
 case_position_json_round_trip();
 case_line_range_runs_past_any_line();

 if (fails)
 {  printf("%d failure(s)\n", fails);  return(1);  }
 printf("all position checks passed\n");
 return(0);
}
