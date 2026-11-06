/* The reader of gdb's machine interface: a line of output into a record, and
 * the bytes of a read into lines.  See we_gdbmi.h.
 *
 * A record is a bounded arena (nodes and text inside the record itself), so
 * reading a line allocates nothing and a line that does not fit is refused
 * with an error code.  Every read of the line is checked against its end:
 * the parser never looks at a byte past the terminator.
 */
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "we_gdbmi.h"

/* gdb's prompt line, with or without its trailing space. */
#define E_GDBMI_PROMPT      "(gdb)"
#define E_GDBMI_PROMPT_LEN  5
/* What opens a record, after its token. */
#define E_GDBMI_C_RESULT    '^'
#define E_GDBMI_C_EXEC      '*'
#define E_GDBMI_C_STATUS    '+'
#define E_GDBMI_C_NOTIFY    '='
#define E_GDBMI_C_CONSOLE   '~'
#define E_GDBMI_C_TARGET    '@'
#define E_GDBMI_C_LOG       '&'
/* The punctuation of values. */
#define E_GDBMI_C_QUOTE     '"'
#define E_GDBMI_C_ESCAPE    '\\'
#define E_GDBMI_C_SEP       ','
#define E_GDBMI_C_EQUALS    '='
#define E_GDBMI_C_TUPLE_OPEN  '{'
#define E_GDBMI_C_TUPLE_CLOSE '}'
#define E_GDBMI_C_LIST_OPEN   '['
#define E_GDBMI_C_LIST_CLOSE  ']'
#define E_GDBMI_C_PATH_SEP  '.'
/* The octal escape: up to three digits, a byte. */
#define E_GDBMI_OCTAL_DIGITS 3
#define E_GDBMI_OCTAL_BASE   8
#define E_GDBMI_DECIMAL_BASE 10
#define E_GDBMI_BYTE_MASK    0xff

/* Where the parser is in one line. */
typedef struct
{
 const char *p;             /* the next byte to read */
 const char *end;           /* the line's '\0': never read at or past it */
 E_GDBMI_RECORD *r;
} E_GDBMI_PARSE;

/* ---- the record ---- */

/**
 * e_gdbmi_clear - Leave @r holding no record.
 */
static void e_gdbmi_clear(E_GDBMI_RECORD *r)
{
 r->kind = E_GDBMI_K_NONE;
 r->token = E_GDBMI_NONE;
 r->klass[0] = '\0';
 r->root = E_GDBMI_NONE;
 r->stream = E_GDBMI_NONE;
 r->nnodes = 0;
 r->ntext = 0;
 r->text[0] = '\0';
}

/**
 * e_gdbmi_text_put - Add one byte to the record's text.
 * Return: E_GDBMI_OK, or E_GDBMI_ERR_TOO_BIG when the text is full.  One
 * byte is always kept for the terminator of the string being written.
 */
static int e_gdbmi_text_put(E_GDBMI_RECORD *r, char c)
{
 if (r->ntext >= E_GDBMI_TEXT_MAX - 1)
  return(E_GDBMI_ERR_TOO_BIG);
 r->text[r->ntext++] = c;
 return(E_GDBMI_OK);
}

/**
 * e_gdbmi_text_end - Close the string being written with its terminator.
 * Return: E_GDBMI_OK, or E_GDBMI_ERR_TOO_BIG when the text is full (an
 * empty string right after a full one has no byte reserved for it).
 */
static int e_gdbmi_text_end(E_GDBMI_RECORD *r)
{
 if (r->ntext >= E_GDBMI_TEXT_MAX)
  return(E_GDBMI_ERR_TOO_BIG);
 r->text[r->ntext++] = '\0';
 return(E_GDBMI_OK);
}

/**
 * e_gdbmi_new_node - Take the next node of the record.
 * @kind: E_GDBMI_V_*; @str: the offset of its text, for a string.
 * Return: its index, or E_GDBMI_ERR_TOO_BIG.
 */
static int e_gdbmi_new_node(E_GDBMI_RECORD *r, int kind, int str)
{
 E_GDBMI_NODE *n;

 if (r->nnodes >= E_GDBMI_NODES_MAX)
  return(E_GDBMI_ERR_TOO_BIG);
 n = &r->nodes[r->nnodes];
 n->kind = kind;
 n->name = E_GDBMI_NONE;
 n->str = str;
 n->first = E_GDBMI_NONE;
 n->next = E_GDBMI_NONE;
 return(r->nnodes++);
}

/**
 * e_gdbmi_append - Make node @idx the next item of the tuple or list @parent.
 * @last: the item added before it, updated.
 */
static void e_gdbmi_append(E_GDBMI_RECORD *r, int parent, int *last, int idx)
{
 if (*last == E_GDBMI_NONE)
  r->nodes[parent].first = idx;
 else
  r->nodes[*last].next = idx;
 *last = idx;
}

/* ---- characters ---- */

static int e_gdbmi_is_digit(int c)
{
 return(c >= '0' && c <= '9');
}

static int e_gdbmi_is_octal(int c)
{
 return(c >= '0' && c <= '7');
}

/**
 * e_gdbmi_is_name_char - A character of a class or a field name:
 * "breakpoint-modified", "thread-groups", "original-location".
 */
static int e_gdbmi_is_name_char(int c)
{
 return((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || e_gdbmi_is_digit(c) || c == '-' || c == '_');
}

/**
 * e_gdbmi_simple_escape - The byte a one-letter escape stands for.
 * Return: it; an escape gdb does not write stands for itself.
 */
static char e_gdbmi_simple_escape(char c)
{
 switch (c)
 {
  case 'n': return('\n');
  case 't': return('\t');
  case 'r': return('\r');
  case 'a': return('\a');
  case 'b': return('\b');
  case 'f': return('\f');
  case 'v': return('\v');
  case 'e': return('\033');
  default: return(c);
 }
}

/* ---- the line ---- */

/**
 * e_gdbmi_line_length - The length of @line, looking no further than the
 * longest line a record takes.
 * Return: it, or E_GDBMI_LINE_MAX when @line is not shorter than that.
 */
static size_t e_gdbmi_line_length(const char *line)
{
 size_t n = 0;

 while (n < E_GDBMI_LINE_MAX && line[n])
  n++;
 return(n);
}

/**
 * e_gdbmi_at_end - Has the parser read the whole line?
 */
static int e_gdbmi_at_end(const E_GDBMI_PARSE *ps)
{
 return(ps->p >= ps->end);
}

/**
 * e_gdbmi_peek - The next byte, or '\0' at the end of the line.
 */
static char e_gdbmi_peek(const E_GDBMI_PARSE *ps)
{
 if (e_gdbmi_at_end(ps))
  return('\0');
 return(*ps->p);
}

/**
 * e_gdbmi_accept - Read @c if it is next.
 * Return: 1 when it was.
 */
static int e_gdbmi_accept(E_GDBMI_PARSE *ps, char c)
{
 if (e_gdbmi_at_end(ps) || *ps->p != c)
  return(0);
 ps->p++;
 return(1);
}

/**
 * e_gdbmi_read_octal - Read the digits of an octal escape (the first is
 * @first) and add the byte they stand for.  A byte of 0 is dropped: a
 * string with a NUL in it would end early for every reader of the text.
 */
static int e_gdbmi_read_octal(E_GDBMI_PARSE *ps, char first)
{
 int value = first - '0', digits = 1;

 while (digits < E_GDBMI_OCTAL_DIGITS && !e_gdbmi_at_end(ps)
        && e_gdbmi_is_octal(*ps->p))
 {
  value = value * E_GDBMI_OCTAL_BASE + (*ps->p - '0');
  ps->p++;
  digits++;
 }
 value &= E_GDBMI_BYTE_MASK;
 if (value == 0)
  return(E_GDBMI_OK);
 return(e_gdbmi_text_put(ps->r, (char)value));
}

/**
 * e_gdbmi_read_escape - Read the escape after a backslash and add what it
 * stands for.
 */
static int e_gdbmi_read_escape(E_GDBMI_PARSE *ps)
{
 char c;

 if (e_gdbmi_at_end(ps))
  return(E_GDBMI_ERR_SYNTAX);
 c = *ps->p++;
 if (e_gdbmi_is_octal(c))
  return(e_gdbmi_read_octal(ps, c));
 return(e_gdbmi_text_put(ps->r, e_gdbmi_simple_escape(c)));
}

/**
 * e_gdbmi_read_cstring - Read "..." and keep its decoded text.
 * @off: receives the offset of the text.
 */
static int e_gdbmi_read_cstring(E_GDBMI_PARSE *ps, int *off)
{
 int rc;

 if (!e_gdbmi_accept(ps, E_GDBMI_C_QUOTE))
  return(E_GDBMI_ERR_SYNTAX);
 *off = ps->r->ntext;
 for (;;)
 {
  if (e_gdbmi_at_end(ps))
   return(E_GDBMI_ERR_SYNTAX);
  if (e_gdbmi_accept(ps, E_GDBMI_C_QUOTE))
   break;
  if (e_gdbmi_accept(ps, E_GDBMI_C_ESCAPE))
   rc = e_gdbmi_read_escape(ps);
  else
   rc = e_gdbmi_text_put(ps->r, *ps->p++);
  if (rc != E_GDBMI_OK)
   return(rc);
 }
 return(e_gdbmi_text_end(ps->r));
}

/**
 * e_gdbmi_read_name - Read a class or field name and keep it.
 * @off: receives the offset of the text.
 * Return: E_GDBMI_OK, or an error when no name is next.
 */
static int e_gdbmi_read_name(E_GDBMI_PARSE *ps, int *off)
{
 int rc;

 if (!e_gdbmi_is_name_char(e_gdbmi_peek(ps)))
  return(E_GDBMI_ERR_SYNTAX);
 *off = ps->r->ntext;
 while (e_gdbmi_is_name_char(e_gdbmi_peek(ps)))
 {
  rc = e_gdbmi_text_put(ps->r, *ps->p++);
  if (rc != E_GDBMI_OK)
   return(rc);
 }
 return(e_gdbmi_text_end(ps->r));
}

static int e_gdbmi_read_value(E_GDBMI_PARSE *ps, int depth, int *node);
static int e_gdbmi_read_tuple(E_GDBMI_PARSE *ps, int depth, int *node);

/**
 * e_gdbmi_read_unnamed_tuple - Read the {...} gdb puts after a result with
 * no name of its own: a breakpoint with several places answers
 * bkpt={...},{number="1.1",...},{number="1.2",...}.  Such an item has no
 * name and is reached by walking the items.
 */
static int e_gdbmi_read_unnamed_tuple(E_GDBMI_PARSE *ps, int depth,
                                      int parent, int *last)
{
 int node, rc;

 rc = e_gdbmi_read_tuple(ps, depth, &node);
 if (rc != E_GDBMI_OK)
  return(rc);
 e_gdbmi_append(ps->r, parent, last, node);
 return(E_GDBMI_OK);
}

/**
 * e_gdbmi_read_result - Read "name=value" and add it to @parent.
 * @depth: how many tuples and lists @parent is inside of; @last: the item
 * added before this one, E_GDBMI_NONE for the first.
 */
static int e_gdbmi_read_result(E_GDBMI_PARSE *ps, int depth, int parent,
                               int *last)
{
 int name, node, rc;

 if (*last != E_GDBMI_NONE && e_gdbmi_peek(ps) == E_GDBMI_C_TUPLE_OPEN)
  return(e_gdbmi_read_unnamed_tuple(ps, depth, parent, last));
 rc = e_gdbmi_read_name(ps, &name);
 if (rc != E_GDBMI_OK)
  return(rc);
 if (!e_gdbmi_accept(ps, E_GDBMI_C_EQUALS))
  return(E_GDBMI_ERR_SYNTAX);
 rc = e_gdbmi_read_value(ps, depth, &node);
 if (rc != E_GDBMI_OK)
  return(rc);
 ps->r->nodes[node].name = name;
 e_gdbmi_append(ps->r, parent, last, node);
 return(E_GDBMI_OK);
}

/**
 * e_gdbmi_read_results - Read "name=value,name=value" up to the closing
 * character of the tuple or list @parent.
 */
static int e_gdbmi_read_results(E_GDBMI_PARSE *ps, int depth, int parent)
{
 int last = E_GDBMI_NONE, rc;

 do
 {
  rc = e_gdbmi_read_result(ps, depth, parent, &last);
  if (rc != E_GDBMI_OK)
   return(rc);
 } while (e_gdbmi_accept(ps, E_GDBMI_C_SEP));
 return(E_GDBMI_OK);
}

/**
 * e_gdbmi_read_values - Read "value,value" up to the closing character of
 * the list @parent.
 */
static int e_gdbmi_read_values(E_GDBMI_PARSE *ps, int depth, int parent)
{
 int last = E_GDBMI_NONE, node, rc;

 do
 {
  rc = e_gdbmi_read_value(ps, depth, &node);
  if (rc != E_GDBMI_OK)
   return(rc);
  e_gdbmi_append(ps->r, parent, &last, node);
 } while (e_gdbmi_accept(ps, E_GDBMI_C_SEP));
 return(E_GDBMI_OK);
}

/**
 * e_gdbmi_read_tuple - Read {name=value,...}.
 * @depth: how many tuples and lists the tuple is inside of.
 */
static int e_gdbmi_read_tuple(E_GDBMI_PARSE *ps, int depth, int *node)
{
 int rc;

 if (depth + 1 > E_GDBMI_DEPTH_MAX)
  return(E_GDBMI_ERR_DEPTH);
 *node = e_gdbmi_new_node(ps->r, E_GDBMI_V_TUPLE, E_GDBMI_NONE);
 if (*node < 0)
  return(*node);
 if (!e_gdbmi_accept(ps, E_GDBMI_C_TUPLE_OPEN))
  return(E_GDBMI_ERR_SYNTAX);
 if (e_gdbmi_accept(ps, E_GDBMI_C_TUPLE_CLOSE))
  return(E_GDBMI_OK);
 rc = e_gdbmi_read_results(ps, depth + 1, *node);
 if (rc != E_GDBMI_OK)
  return(rc);
 if (!e_gdbmi_accept(ps, E_GDBMI_C_TUPLE_CLOSE))
  return(E_GDBMI_ERR_SYNTAX);
 return(E_GDBMI_OK);
}

/**
 * e_gdbmi_read_list - Read [value,...] or [name=value,...]: gdb writes
 * either, never both in one list.
 */
static int e_gdbmi_read_list(E_GDBMI_PARSE *ps, int depth, int *node)
{
 int rc;

 if (depth + 1 > E_GDBMI_DEPTH_MAX)
  return(E_GDBMI_ERR_DEPTH);
 *node = e_gdbmi_new_node(ps->r, E_GDBMI_V_LIST, E_GDBMI_NONE);
 if (*node < 0)
  return(*node);
 if (!e_gdbmi_accept(ps, E_GDBMI_C_LIST_OPEN))
  return(E_GDBMI_ERR_SYNTAX);
 if (e_gdbmi_accept(ps, E_GDBMI_C_LIST_CLOSE))
  return(E_GDBMI_OK);
 if (e_gdbmi_is_name_char(e_gdbmi_peek(ps)))
  rc = e_gdbmi_read_results(ps, depth + 1, *node);
 else
  rc = e_gdbmi_read_values(ps, depth + 1, *node);
 if (rc != E_GDBMI_OK)
  return(rc);
 if (!e_gdbmi_accept(ps, E_GDBMI_C_LIST_CLOSE))
  return(E_GDBMI_ERR_SYNTAX);
 return(E_GDBMI_OK);
}

/**
 * e_gdbmi_read_string_value - Read "..." as a value.
 */
static int e_gdbmi_read_string_value(E_GDBMI_PARSE *ps, int *node)
{
 int off, rc;

 rc = e_gdbmi_read_cstring(ps, &off);
 if (rc != E_GDBMI_OK)
  return(rc);
 *node = e_gdbmi_new_node(ps->r, E_GDBMI_V_STRING, off);
 if (*node < 0)
  return(*node);
 return(E_GDBMI_OK);
}

/**
 * e_gdbmi_read_value - Read a string, a tuple or a list.
 * @depth: how many tuples and lists the value is inside of.
 */
static int e_gdbmi_read_value(E_GDBMI_PARSE *ps, int depth, int *node)
{
 switch (e_gdbmi_peek(ps))
 {
  case E_GDBMI_C_QUOTE:
   return(e_gdbmi_read_string_value(ps, node));
  case E_GDBMI_C_TUPLE_OPEN:
   return(e_gdbmi_read_tuple(ps, depth, node));
  case E_GDBMI_C_LIST_OPEN:
   return(e_gdbmi_read_list(ps, depth, node));
  default:
   return(E_GDBMI_ERR_SYNTAX);
 }
}

/**
 * e_gdbmi_read_token - Read the number a command was given, if the line
 * starts with one.  A number that does not fit an int is not a token.
 */
static int e_gdbmi_read_token(E_GDBMI_PARSE *ps)
{
 int value = 0, digit;
 int any = 0;

 while (e_gdbmi_is_digit(e_gdbmi_peek(ps)))
 {
  digit = *ps->p++ - '0';
  /* Checked before the multiplication, so the sum never overflows: a
     test on a long afterwards is always false where long is as narrow
     as int (i386, armhf). */
  if (value > (INT_MAX - digit) / E_GDBMI_DECIMAL_BASE)
   return(E_GDBMI_ERR_SYNTAX);
  value = value * E_GDBMI_DECIMAL_BASE + digit;
  any = 1;
 }
 if (any)
  ps->r->token = value;
 return(E_GDBMI_OK);
}

/**
 * e_gdbmi_read_class - Read the class after ^ * + or = into the record.
 */
static int e_gdbmi_read_class(E_GDBMI_PARSE *ps)
{
 size_t n = 0;

 while (e_gdbmi_is_name_char(e_gdbmi_peek(ps)))
 {
  if (n >= E_GDBMI_CLASS_MAX - 1)
   return(E_GDBMI_ERR_SYNTAX);
  ps->r->klass[n++] = *ps->p++;
 }
 ps->r->klass[n] = '\0';
 if (n == 0)
  return(E_GDBMI_ERR_SYNTAX);
 return(E_GDBMI_OK);
}

/**
 * e_gdbmi_read_class_and_results - Read what follows ^ * + or =: the class
 * and ",name=value" for each result, kept as a tuple at the root.
 */
static int e_gdbmi_read_class_and_results(E_GDBMI_PARSE *ps)
{
 int rc;

 rc = e_gdbmi_read_class(ps);
 if (rc != E_GDBMI_OK)
  return(rc);
 ps->r->root = e_gdbmi_new_node(ps->r, E_GDBMI_V_TUPLE, E_GDBMI_NONE);
 if (ps->r->root < 0)
 {
  rc = ps->r->root;
  ps->r->root = E_GDBMI_NONE;
  return(rc);
 }
 if (e_gdbmi_accept(ps, E_GDBMI_C_SEP))
  rc = e_gdbmi_read_results(ps, 0, ps->r->root);
 return(rc);
}

/**
 * e_gdbmi_read_stream - Read the "..." of a ~, @ or & record.
 */
static int e_gdbmi_read_stream(E_GDBMI_PARSE *ps)
{
 return(e_gdbmi_read_cstring(ps, &ps->r->stream));
}

/**
 * e_gdbmi_read_prompt - Is the line gdb's prompt?  The space after it is
 * optional.
 * Return: 1 when the line is nothing but the prompt.
 */
static int e_gdbmi_read_prompt(E_GDBMI_PARSE *ps)
{
 if ((size_t)(ps->end - ps->p) < E_GDBMI_PROMPT_LEN
     || strncmp(ps->p, E_GDBMI_PROMPT, E_GDBMI_PROMPT_LEN) != 0)
  return(0);
 ps->p += E_GDBMI_PROMPT_LEN;
 while (e_gdbmi_peek(ps) == ' ')
  ps->p++;
 return(e_gdbmi_at_end(ps));
}

/**
 * e_gdbmi_record_kind_of - The record kind a line's opening character
 * stands for.
 * Return: E_GDBMI_K_*, E_GDBMI_K_NONE for a character that opens none.
 */
static int e_gdbmi_record_kind_of(char c)
{
 switch (c)
 {
  case E_GDBMI_C_RESULT: return(E_GDBMI_K_RESULT);
  case E_GDBMI_C_EXEC: return(E_GDBMI_K_EXEC);
  case E_GDBMI_C_STATUS: return(E_GDBMI_K_STATUS);
  case E_GDBMI_C_NOTIFY: return(E_GDBMI_K_NOTIFY);
  case E_GDBMI_C_CONSOLE: return(E_GDBMI_K_CONSOLE);
  case E_GDBMI_C_TARGET: return(E_GDBMI_K_TARGET);
  case E_GDBMI_C_LOG: return(E_GDBMI_K_LOG);
  default: return(E_GDBMI_K_NONE);
 }
}

/**
 * e_gdbmi_read_record - Read the record the line holds, after its token.
 */
static int e_gdbmi_read_record(E_GDBMI_PARSE *ps)
{
 int kind = e_gdbmi_record_kind_of(e_gdbmi_peek(ps));

 if (kind == E_GDBMI_K_NONE)
  return(E_GDBMI_ERR_SYNTAX);
 ps->p++;
 ps->r->kind = kind;
 if (kind == E_GDBMI_K_CONSOLE || kind == E_GDBMI_K_TARGET
     || kind == E_GDBMI_K_LOG)
 {
  if (ps->r->token != E_GDBMI_NONE)
   return(E_GDBMI_ERR_SYNTAX);
  return(e_gdbmi_read_stream(ps));
 }
 return(e_gdbmi_read_class_and_results(ps));
}

int e_gdbmi_parse_line(const char *line, E_GDBMI_RECORD *r)
{
 E_GDBMI_PARSE ps;
 size_t len;
 int rc;

 e_gdbmi_clear(r);
 if (!line)
  return(E_GDBMI_ERR_SYNTAX);
 len = e_gdbmi_line_length(line);
 if (len >= E_GDBMI_LINE_MAX)
  return(E_GDBMI_ERR_LONG);
 ps.p = line;
 ps.end = line + len;
 ps.r = r;
 if (e_gdbmi_read_prompt(&ps))
 {
  r->kind = E_GDBMI_K_PROMPT;
  return(E_GDBMI_OK);
 }
 ps.p = line;
 rc = e_gdbmi_read_token(&ps);
 if (rc == E_GDBMI_OK)
  rc = e_gdbmi_read_record(&ps);
 if (rc == E_GDBMI_OK && !e_gdbmi_at_end(&ps))
  rc = E_GDBMI_ERR_SYNTAX;
 if (rc != E_GDBMI_OK)
  e_gdbmi_clear(r);
 return(rc);
}

const char *e_gdbmi_stream_text(const E_GDBMI_RECORD *r)
{
 if (r->stream == E_GDBMI_NONE)
  return(NULL);
 return(r->text + r->stream);
}

/* ---- reading the results ---- */

/**
 * e_gdbmi_is_container - Is @n a tuple or a list?
 */
static int e_gdbmi_is_container(const E_GDBMI_NODE *n)
{
 return(n && (n->kind == E_GDBMI_V_TUPLE || n->kind == E_GDBMI_V_LIST));
}

/**
 * e_gdbmi_child_named - The first item of @n called @name (of @len bytes).
 */
static const E_GDBMI_NODE *e_gdbmi_child_named(const E_GDBMI_RECORD *r,
                                               const E_GDBMI_NODE *n,
                                               const char *name, size_t len)
{
 const E_GDBMI_NODE *c;

 for (c = e_gdbmi_first_item(r, n); c; c = e_gdbmi_next_item(r, c))
 {
  if (c->name == E_GDBMI_NONE)
   continue;
  if (strncmp(r->text + c->name, name, len) == 0
      && r->text[c->name + len] == '\0')
   return(c);
 }
 return(NULL);
}

const E_GDBMI_NODE *e_gdbmi_find_in(const E_GDBMI_RECORD *r,
                                    const E_GDBMI_NODE *n, const char *path)
{
 const char *seg = path;
 const char *dot;
 size_t len;

 if (!n || !path || !path[0])
  return(NULL);
 for (;;)
 {
  dot = strchr(seg, E_GDBMI_C_PATH_SEP);
  len = dot ? (size_t)(dot - seg) : strlen(seg);
  if (len == 0 || len >= E_GDBMI_PATH_MAX)
   return(NULL);
  n = e_gdbmi_child_named(r, n, seg, len);
  if (!n || !dot)
   return(n);
  seg = dot + 1;
 }
}

const E_GDBMI_NODE *e_gdbmi_find(const E_GDBMI_RECORD *r, const char *path)
{
 if (r->root == E_GDBMI_NONE)
  return(NULL);
 return(e_gdbmi_find_in(r, &r->nodes[r->root], path));
}

const char *e_gdbmi_text_of(const E_GDBMI_RECORD *r, const E_GDBMI_NODE *n)
{
 if (!n || n->kind != E_GDBMI_V_STRING)
  return(NULL);
 return(r->text + n->str);
}

const char *e_gdbmi_get(const E_GDBMI_RECORD *r, const char *path)
{
 return(e_gdbmi_text_of(r, e_gdbmi_find(r, path)));
}

const char *e_gdbmi_get_in(const E_GDBMI_RECORD *r, const E_GDBMI_NODE *n,
                           const char *path)
{
 return(e_gdbmi_text_of(r, e_gdbmi_find_in(r, n, path)));
}

const E_GDBMI_NODE *e_gdbmi_first_item(const E_GDBMI_RECORD *r,
                                       const E_GDBMI_NODE *n)
{
 if (!e_gdbmi_is_container(n) || n->first == E_GDBMI_NONE)
  return(NULL);
 return(&r->nodes[n->first]);
}

const E_GDBMI_NODE *e_gdbmi_next_item(const E_GDBMI_RECORD *r,
                                      const E_GDBMI_NODE *n)
{
 if (!n || n->next == E_GDBMI_NONE)
  return(NULL);
 return(&r->nodes[n->next]);
}

const char *e_gdbmi_item_name(const E_GDBMI_RECORD *r, const E_GDBMI_NODE *n)
{
 if (!n || n->name == E_GDBMI_NONE)
  return("");
 return(r->text + n->name);
}

int e_gdbmi_number(const char *text, int base, int fallback)
{
 char *end;
 long value;

 if (!text || !text[0])
  return(fallback);
 errno = 0;
 value = strtol(text, &end, base);
 if (*end != '\0' || errno == ERANGE || value > INT_MAX || value < INT_MIN)
  return(fallback);
 return((int)value);
}

/* ---- bytes into lines ---- */

void e_gdbmi_split_init(E_GDBMI_SPLIT *sp)
{
 sp->len = 0;
 sp->dropping = 0;
}

/**
 * e_gdbmi_split_finish - The line in the buffer is complete: drop the
 * carriage return gdb may put before the line end, close the string and
 * start the next line.
 * Return: E_GDBMI_SPLIT_LINE, or E_GDBMI_SPLIT_OVERLONG when its tail was
 * thrown away.
 */
static int e_gdbmi_split_finish(E_GDBMI_SPLIT *sp)
{
 int rc = sp->dropping ? E_GDBMI_SPLIT_OVERLONG : E_GDBMI_SPLIT_LINE;

 if (!sp->dropping && sp->len > 0 && sp->buf[sp->len - 1] == '\r')
  sp->len--;
 sp->buf[sp->len] = '\0';
 sp->len = 0;
 sp->dropping = 0;
 return(rc);
}

/**
 * e_gdbmi_split_add - Add one byte of a line, or note that the line has
 * outgrown the buffer: its head stays, the rest is dropped.
 */
static void e_gdbmi_split_add(E_GDBMI_SPLIT *sp, char c)
{
 if (sp->dropping)
  return;
 if (sp->len >= E_GDBMI_LINE_MAX - 1)
 {
  sp->dropping = 1;
  return;
 }
 sp->buf[sp->len++] = c;
}

int e_gdbmi_split_next(E_GDBMI_SPLIT *sp, const char *data, size_t n,
                       size_t *used, const char **line)
{
 char c;

 while (*used < n)
 {
  c = data[(*used)++];
  if (c != '\n')
  {
   e_gdbmi_split_add(sp, c);
   continue;
  }
  *line = sp->buf;
  return(e_gdbmi_split_finish(sp));
 }
 return(E_GDBMI_SPLIT_NEED_MORE);
}
