/* we_lsp_pos.h -- the language server's columns: the one owner.

   A language server counts the characters of a line in the units it
   negotiated (UTF-16 code units unless it agreed to UTF-8); the editor
   counts bytes.  Every column that crosses between the two goes through
   this file, so a highlight, a problem mark, a selection, a rename or a
   quick-fix lands on the symbol and not a few letters to its left on a line
   that has an accented letter, a CJK character or an emoji before it.

   The file is pure over bytes: no editor globals, no session.  It is the
   only file that makes a unit count or opens one (the struct below keeps
   an int from being handed where units are meant, and units from being
   used as a byte column); tests/check-one-owner.sh fails the build when
   another file does.

   One server line is one buffer row: the document the server has is the
   rows joined by '\n' (e_lsp_join_lines), so a row of the buffer and a line
   of the server's text are the same bytes.

   What a character is comes from we_utf8.h, nothing else. */

#ifndef WE_LSP_POS_H
#define WE_LSP_POS_H

#include <stddef.h>

struct json_object;

/* A column on the wire, in the units of the session's encoding.  Written
   and read only by we_lsp_pos.c. */
typedef struct { int units_on_wire; } e_lsp_units;

#define E_LSP_ENC_UTF16 0               /* the protocol's default and mandatory encoding */
#define E_LSP_ENC_UTF8  1
#define E_LSP_UTF16_PAIR_FROM 0x10000   /* the first code point UTF-16 writes as two units */

/* One semantic token: the half-open span [start, start+length) on `line`
 * (0-based), whose meaning is the legend index `type`.  `modifiers` is the
 * raw modifier bitset.  start and length are in the session's units. */
typedef struct {
 int         line;
 e_lsp_units start;
 e_lsp_units length;
 int         type;
 int         modifiers;
} e_lsp_sem_token;

/* The encodings the editor offers at start-up: ["utf-8", "utf-16"], utf-8
   first, for general.positionEncodings.  The caller owns the array. */
struct json_object *e_lsp_encodings_offered(void);
/* The server's answer: "utf-8" gives UTF8; NULL, "utf-16" or anything else
   gives UTF16, the encoding every server must support. */
int e_lsp_encoding_parse(const char *answer);
/* 1 when @answer names an encoding that was offered or is absent, 0 when
   the server named one that was not (the caller says so once in its trace). */
int e_lsp_encoding_answer_is_known(const char *answer);
const char *e_lsp_encoding_name(int enc);

/* The wire's number as units: for the owner and the tests only. */
e_lsp_units e_lsp_units_make(int on_wire);
/* Column 0, for a position not answered yet. */
e_lsp_units e_lsp_units_zero(void);
/* A token's end: start + length. */
e_lsp_units e_lsp_units_sum(e_lsp_units a, e_lsp_units b);
int e_lsp_units_eq(e_lsp_units a, e_lsp_units b);

/* The units of the row's byte column @x, over one row of @len bytes (no
   terminator needed).  @x < 0 is 0, @x > len is len, an @x inside a glyph
   counts as the glyph's start. */
e_lsp_units e_lsp_units_of(const unsigned char *row, int len, int x, int enc);
/* The byte column of @u over the same row: the glyph that holds the units
   (a unit inside a surrogate pair goes back to the glyph's start), or @len
   when the units reach or pass the row's end. */
int e_lsp_col_of(const unsigned char *row, int len, e_lsp_units u, int enc);
/* The byte column of @u when the row is not available (a file that cannot
   be read): the units taken as bytes, as for ASCII text. */
int e_lsp_col_unread_row(e_lsp_units u);

/* pos.character into *out; 0 when the key is absent (then *out is 0). */
int e_lsp_units_read(struct json_object *pos, e_lsp_units *out);
/* {line, character}; the caller owns it. */
struct json_object *e_lsp_position_json(int line, e_lsp_units u);
/* {start: (start_line, 0), end: (end_line, past any line's end)}. */
struct json_object *e_lsp_line_range(int start_line, int end_line);

/* Decode an LSP semantic-tokens `data` array (@n ints, groups of 5:
 * deltaLine, deltaStartChar, length, tokenType, tokenModifiers) into absolute
 * tokens.  Fills up to @max into @out and returns the count. */
int e_lsp_semantic_decode(const int *data, int n, e_lsp_sem_token *out, int max);

/* A copy of @text with each '\r' written as a space, for the text sent to a
   server (the protocol counts a CR as a line end; a space has the same bytes
   and units in every encoding, so no position moves).  NULL for NULL or when
   out of memory.  The caller frees it. */
char *e_lsp_wire_text(const char *text);

/* Row @line of a '\n'-joined document: its first byte, its length in *len;
   NULL when the text has no such row. */
const char *e_lsp_text_row(const char *text, int line, int *len);
/* The byte offset of (@line, @u) in @text: the row found, then the glyph.
   A line past the text is the text's end; units past the row are the row's
   end, never the next row. */
size_t e_lsp_text_offset(const char *text, int line, e_lsp_units u, int enc);

/* The rows of one text, found once: every later conversion is O(row).
   Built per answer or per synced text, not per conversion. */
typedef struct e_lsp_text_index e_lsp_text_index;
/* An index over @text (kept by reference: it must outlive the index);
   a NULL text is an index with no rows.  NULL on out of memory. */
e_lsp_text_index *e_lsp_text_index_new(const char *text);
void e_lsp_text_index_free(e_lsp_text_index *ix);
/* Row @line of the indexed text: its first byte and its length in *len, or
   NULL when the index has no text or no such row. */
const char *e_lsp_text_index_row(const e_lsp_text_index *ix, int line, int *len);
/* The byte column of (@line, @u): @len of the row when the units reach its
   end, 0 for a line the text does not have, e_lsp_col_unread_row() when the
   index has no text. */
int e_lsp_text_index_col(const e_lsp_text_index *ix, int line, e_lsp_units u,
                         int enc);
/* The byte offset of (@line, @u) as e_lsp_text_offset does, over the index. */
size_t e_lsp_text_index_offset(const e_lsp_text_index *ix, int line,
                               e_lsp_units u, int enc);

#endif /* WE_LSP_POS_H */
