#ifndef WPE_GDBMI_H
#define WPE_GDBMI_H

/* gdb's machine interface (MI), the half the editor reads: one line of gdb's
   output made into a record, and the bytes of a read made into lines.

   With MI the editor learns what gdb did from named fields (the file and
   line of a stop, the number of a breakpoint, the exit code) instead of
   from sentences gdb words differently from one version and one language
   to the next.  Nothing here is localised: record kinds, classes, field
   names and "reason" values are fixed by gdb; only msg= and the console
   records carry words, and those are shown, never matched.

   Plain C over strings: no curses, no X11, no Wayland, no editor header, no
   descriptor and no allocation.  A record is a bounded arena, so a line
   too large for it is refused, never overflowed.  tests/test_gdbmi.c feeds
   it what the real gdb printed (tests/transcripts/gdb-mi-*.txt). */

#include <stddef.h>

/* Sizes.  The longest real line seen is a stop inside a library function,
   about 1000 bytes; a stack of 64 frames is about 21000. */
#define E_GDBMI_LINE_MAX    32768   /* a line past this is reported, not read */
#define E_GDBMI_NODES_MAX   1024    /* values (tuples, lists, strings) in one record */
#define E_GDBMI_TEXT_MAX    32768   /* bytes of names and strings in one record */
#define E_GDBMI_CLASS_MAX   64      /* "stopped", "breakpoint-modified" */
#define E_GDBMI_DEPTH_MAX   32      /* tuples and lists inside each other */
#define E_GDBMI_PATH_MAX    128     /* one name in a path */

/* No token, no node, no text. */
#define E_GDBMI_NONE        (-1)

/* What a line is: the character that opens it, or the prompt. */
#define E_GDBMI_K_NONE      0   /* not a record (a failed parse) */
#define E_GDBMI_K_RESULT    1   /* ^ the answer to a command */
#define E_GDBMI_K_EXEC      2   /* * the program ran or stopped */
#define E_GDBMI_K_STATUS    3   /* + progress of a slow command */
#define E_GDBMI_K_NOTIFY    4   /* = something changed in gdb */
#define E_GDBMI_K_CONSOLE   5   /* ~ gdb's own words, for the user */
#define E_GDBMI_K_TARGET    6   /* @ the program's output, remote targets */
#define E_GDBMI_K_LOG       7   /* & gdb's log, its stderr */
#define E_GDBMI_K_PROMPT    8   /* (gdb) closes every answer */

/* What a value is. */
#define E_GDBMI_V_STRING    1   /* "..." */
#define E_GDBMI_V_TUPLE     2   /* {name=value,...} */
#define E_GDBMI_V_LIST      3   /* [value,...] or [name=value,...] */

/* What a call returns: 0 is good, a negative number says why not. */
#define E_GDBMI_OK          0
#define E_GDBMI_ERR_SYNTAX  (-1)    /* not a line of MI */
#define E_GDBMI_ERR_TOO_BIG (-2)    /* more values or text than a record holds */
#define E_GDBMI_ERR_LONG    (-3)    /* a line past E_GDBMI_LINE_MAX */
#define E_GDBMI_ERR_DEPTH   (-4)    /* tuples and lists nested past E_GDBMI_DEPTH_MAX */

typedef struct
{
 int kind;      /* E_GDBMI_V_* */
 int name;      /* offset in the record's text, E_GDBMI_NONE for a list's bare value */
 int str;       /* a string: offset of its decoded text */
 int first;     /* a tuple or list: its first value, or E_GDBMI_NONE */
 int next;      /* the next value of the same tuple or list, or E_GDBMI_NONE */
} E_GDBMI_NODE;

typedef struct
{
 int kind;                          /* E_GDBMI_K_* */
 int token;                         /* the number before the line, or E_GDBMI_NONE */
 char klass[E_GDBMI_CLASS_MAX];     /* "done", "error", "stopped", "breakpoint-modified" */
 int root;                          /* node of the results as a tuple, or E_GDBMI_NONE */
 int stream;                        /* ~ @ &: offset of the text, else E_GDBMI_NONE */
 int nnodes;
 int ntext;
 E_GDBMI_NODE nodes[E_GDBMI_NODES_MAX];
 char text[E_GDBMI_TEXT_MAX];
} E_GDBMI_RECORD;

/* ---- one line ---- */

/**
 * e_gdbmi_parse_line - Read one line of gdb's MI output.
 * @line: the line, '\0'-terminated, without its line end.
 * @r: receives the record; every earlier content is dropped.
 *
 * The editor can tell what gdb said (a stop, an answer, a notice, words for
 * the user) and read the places and numbers in it, without ever matching
 * gdb's sentences.  The strings are decoded: \n, \t, \e, \", \\ and the
 * octal \NNN gdb writes for bytes outside ASCII.
 * Return: E_GDBMI_OK, or why the line is not a record (the record is then
 * of kind E_GDBMI_K_NONE and the caller goes on with the next line).
 */
int e_gdbmi_parse_line(const char *line, E_GDBMI_RECORD *r);

/**
 * e_gdbmi_stream_text - The words of a console, target or log record.
 * @r: a record.
 *
 * The user reads these in Messages.
 * Return: the decoded text, or NULL when @r is not a ~, @ or & record.
 */
const char *e_gdbmi_stream_text(const E_GDBMI_RECORD *r);

/* ---- reading the results ---- */

/**
 * e_gdbmi_find - The value a dotted path names in a record's results.
 * @r: a record; @path: "frame.line", "bkpt.number", "msg".
 *
 * Return: the value, or NULL when the record has none by that path.
 */
const E_GDBMI_NODE *e_gdbmi_find(const E_GDBMI_RECORD *r, const char *path);

/**
 * e_gdbmi_find_in - The value a path names inside @n (a tuple, or one item
 * of a list).
 * @n: where to start; NULL gives NULL.
 */
const E_GDBMI_NODE *e_gdbmi_find_in(const E_GDBMI_RECORD *r,
                                    const E_GDBMI_NODE *n, const char *path);

/**
 * e_gdbmi_text_of - The text of a string value.
 * Return: it, or NULL when @n is NULL or is a tuple or a list.
 */
const char *e_gdbmi_text_of(const E_GDBMI_RECORD *r, const E_GDBMI_NODE *n);

/**
 * e_gdbmi_get - The text of the string a path names: "frame.line" gives
 * "7". The line of a stop, the number of a breakpoint, a message.
 * Return: the text, or NULL when absent or not a string.
 */
const char *e_gdbmi_get(const E_GDBMI_RECORD *r, const char *path);

/**
 * e_gdbmi_get_in - e_gdbmi_get from inside @n.
 */
const char *e_gdbmi_get_in(const E_GDBMI_RECORD *r, const E_GDBMI_NODE *n,
                           const char *path);

/**
 * e_gdbmi_first_item - The first value of a list or tuple, so the Stack
 * window can show "stack=[frame={...},frame={...}]" frame by frame.
 * Return: it, or NULL when @n is NULL, a string, or empty.
 */
const E_GDBMI_NODE *e_gdbmi_first_item(const E_GDBMI_RECORD *r,
                                       const E_GDBMI_NODE *n);

/**
 * e_gdbmi_next_item - The value after @n in its list or tuple, or NULL.
 */
const E_GDBMI_NODE *e_gdbmi_next_item(const E_GDBMI_RECORD *r,
                                      const E_GDBMI_NODE *n);

/**
 * e_gdbmi_item_name - The name of a list's item ("frame" in
 * stack=[frame={...}]), or "" when it has none.
 */
const char *e_gdbmi_item_name(const E_GDBMI_RECORD *r, const E_GDBMI_NODE *n);

/**
 * e_gdbmi_number - A number gdb wrote in a string.
 * @text: the string, may be NULL; @base: 10, or 8 for the exit code (gdb
 * writes exit-code="012" for 10); @fallback: given back when @text is not
 * a number.
 */
int e_gdbmi_number(const char *text, int base, int fallback);

/* ---- bytes into lines ---- */

/* What a splitter says after each call. */
#define E_GDBMI_SPLIT_NEED_MORE 0   /* the data is used up, a partial line waits */
#define E_GDBMI_SPLIT_LINE      1   /* a whole line is ready */
#define E_GDBMI_SPLIT_OVERLONG  2   /* a line was past the limit: its head is given, the rest dropped */

typedef struct
{
 char buf[E_GDBMI_LINE_MAX];    /* the line so far */
 size_t len;
 int dropping;                  /* inside an over-long line, up to its end */
} E_GDBMI_SPLIT;

/**
 * e_gdbmi_split_init - Start with no partial line.
 */
void e_gdbmi_split_init(E_GDBMI_SPLIT *sp);

/**
 * e_gdbmi_split_next - Hand out the next whole line of what a read brought.
 * @sp: the splitter; @data, @n: the bytes read; @used: how many were taken
 * already (start at 0, pass the same variable until NEED_MORE); @line:
 * receives the line, valid until the next call, without its line end.
 *
 * A read may end in the middle of a line, or in the middle of a character:
 * the partial line stays in @sp and the next read finishes it, so the
 * editor never reacts to half a stop.  A line longer than the limit is
 * reported once, at its end, and its tail is thrown away, so one huge line
 * costs one notice and no memory.
 * Return: E_GDBMI_SPLIT_*.
 */
int e_gdbmi_split_next(E_GDBMI_SPLIT *sp, const char *data, size_t n,
                       size_t *used, const char **line);

#endif /* WPE_GDBMI_H */
