/* The state of a debugging session, and the readers of what the debuggers
 * print, apart from drawing.
 *
 * Each reader here was a part of we_debug.c, moved as it was: the editor
 * calls these functions, and tests/test_debsess.c feeds them the words real
 * debuggers print.  Nothing here reads a descriptor, draws, or touches the
 * editor's globals; the one question about the file system (does pdb's
 * named file exist?) is asked through a probe the caller hands in.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "we_debsess.h"

/* jdb groups the digits of a line number past 999: "line=1,004". */
#define E_DS_JDB_DIGIT_GROUP ','
#define E_DS_DECIMAL_BASE    10
/* pdb marks each frame of "where" with "> " (selected) or "  ". */
#define E_DS_PDB_FRAME_LEAD  2
#define E_DS_PDB_STOP_LEAD   "> "
#define E_DS_PDB_SOURCE_LEAD "-> "
#define E_DS_PDB_ERROR_LEAD  "*** "
/* a68g's thread line, its column mark and its terminate question. */
#define E_DS_A68G_MAIN_THREAD  "This is the main thread"
#define E_DS_A68G_OTHER_THREAD "This is not the main thread"
#define E_DS_A68G_MARK_CHAR    '-'
#define E_DS_A68G_PROC_WORD    "PROC "
#define E_DS_A68G_NOT_FOUND    "cannot find identifier"
#define E_DS_A68G_MONITOR_ERR  "monitor error"
/* gdb's row of "where" starts "#N". */
#define E_DS_GDB_ROW_MARK      '#'
/* dbx names the line of a frame or a stop after this, the file in quotes. */
#define E_DS_DBX_LINE_WORD     " line "
/* What the debugger says a stop or a breakpoint is. */
#define E_DS_GDB_BREAKPOINT    "Breakpoint "
/* A DAP "reason" that is a breakpoint, a step, a pause. */
#define E_DS_DAP_BREAKPOINT    "breakpoint"
#define E_DS_DAP_STEP          "step"
#define E_DS_DAP_PAUSE         "pause"
#define E_DS_DAP_EXCEPTION     "exception"
/* gdb's MI: the classes of its records, the "reason" of a stop, the signal
   the editor sends to interrupt, and the exit code's base (gdb writes
   exit-code="012" for 10). */
#define E_DS_MI_CLASS_RUNNING  "running"
#define E_DS_MI_CLASS_STOPPED  "stopped"
#define E_DS_MI_CLASS_ERROR    "error"
#define E_DS_MI_CLASS_EXIT     "exit"
#define E_DS_MI_CLASS_DONE     "done"
#define E_DS_MI_SIGINT         "SIGINT"
#define E_DS_MI_EXIT_BASE      8
#define E_DS_MI_DECIMAL        10
/* What shows on the status row when gdb holds a breakpoint pending, and when
   it refuses one without saying why. */
#define E_DS_MI_PENDING_WORDS  "gdb holds it pending: "
#define E_DS_MI_REFUSED_WORDS  "gdb refused the breakpoint"
/* A watch's value and the Stack window's rows, written the way gdb's line
   interface prints them, so both interfaces show the same lines. */
#define E_DS_MI_WATCH_LEAD     "= "
#define E_DS_MI_ARG_SEP        ", "
#define E_DS_MI_NO_FUNC        "??"
#define E_DS_MI_ROW_AT_FMT     "#%d  %s%s (%s) at %s:%s\n"
#define E_DS_MI_ROW_FROM_FMT   "#%d  %s%s (%s) from %s\n"
#define E_DS_MI_ROW_FMT        "#%d  %s%s (%s)\n"
#define E_DS_MI_ADDR_FMT       "%s in "
#define E_DS_MI_ADDR_MAX       64
/* A terminal's styling: ESC [ ... a final byte from @ to ~. */
#define E_DS_ESC               '\033'
#define E_DS_CSI               '['
#define E_DS_CSI_FINAL_FIRST   '@'
#define E_DS_CSI_FINAL_LAST    '~'
/* The parameter and intermediate bytes a styling sequence holds before its
   final byte. */
#define E_DS_CSI_INNER_FIRST   ' '
#define E_DS_CSI_INNER_LAST    '?'
/* UTF-8: a continuation byte is 10xxxxxx; a lead byte from 0xc0 opens a
   character of two bytes, from 0xe0 of three, from 0xf0 of four. */
#define E_DS_UTF8_TOP_BITS     0xc0u
#define E_DS_UTF8_CONT         0x80u
#define E_DS_UTF8_LEAD2        0xc0u
#define E_DS_UTF8_LEAD3        0xe0u
#define E_DS_UTF8_LEAD4        0xf0u
#define E_DS_UTF8_BYTES2       2
#define E_DS_UTF8_BYTES3       3
#define E_DS_UTF8_BYTES4       4

/* ================================================================== */
/* The readers                                                         */
/* ================================================================== */

/**
 * e_ds_copy_name - Copy a file name out of a debugger line, up to a stop
 * character.
 * @from: where the name starts; @stop: the character that ends it (the end
 * of the line ends it too); @file, @n: where it goes.
 *
 * The line may be of any length, since frames are read whole; a "name"
 * longer than any path is no file, and is refused rather than copied.
 * Return: the name's length, or -1 when it does not end before @stop.
 */
static int e_ds_copy_name(const char *from, char stop, char *file, size_t n)
{
 int i;

 for (i = 0; from[i] != '\0' && from[i] != stop; i++)
 {
  if ((size_t)i >= n - 1)
  {
   file[i] = '\0';
   return(-1);
  }
  file[i] = from[i];
 }
 file[i] = '\0';
 if (from[i] != stop)
  return(-1);
 return(i);
}

/**
 * e_ds_line_num_gdb - gdb's "... at file:line": the last colon ends the file,
 * the blank before it starts it.
 */
static int e_ds_line_num_gdb(const char *str, char *file, size_t n)
{
 int i, k;

 for (k = (int)strlen(str); k >= 0 && str[k] != ':'; k--)
  ;
 if (k < 0)
  return(-1);
 for (i = k - 1; i >= 0 && !isspace((unsigned char)str[i]); i--)
  ;
 if ((k = e_ds_copy_name(str + i + 1, ':', file, n)) < 0)
  return(-1);
 return(atoi(str + i + 1 + k + 1));
}

/**
 * e_ds_line_num_dbx - dbx's "... line 7 in \"p.c\"".
 */
static int e_ds_line_num_dbx(const char *str, char *file, size_t n)
{
 const char *sp;
 int i, num;

 if (!(sp = strstr(str, E_DS_DBX_LINE_WORD)))
  return(-1);
 if (!(num = atoi(sp + strlen(E_DS_DBX_LINE_WORD))))
  return(-1);
 for (i = (int)strlen(E_DS_DBX_LINE_WORD); sp[i] && sp[i] != '\"'; i++)
  ;
 if (!sp[i])
  return(-1);
 sp += (i + 1);
 if (e_ds_copy_name(sp, '\"', file, n) < 0)
  return(-1);
 return(num);
}

/**
 * e_ds_line_num_xdb - xdb's "p.c:main:7".
 */
static int e_ds_line_num_xdb(const char *str, char *file, size_t n)
{
 int i;

 if ((i = e_ds_copy_name(str, ':', file, n)) < 0)
  return(-1);
 for (i++; str[i] && str[i] != ':'; i++)
  ;
 if (!str[i])
  return(-1);
 for (i++; str[i] && isspace((unsigned char)str[i]); i++)
  ;
 if (!isdigit((unsigned char)str[i]))
  return(-1);
 return(atoi(str + i));
}

/**
 * e_ds_line_num_sdb - sdb's "p.c:7": only the line; the file comes from
 * the answer to "e" (e_ds_sdb_file_of).
 */
static int e_ds_line_num_sdb(const char *str)
{
 int i, num;

 for (i = 0; str[i] != '\0' && str[i] != ':'; i++)
  ;
 if ((!str[i]) || (num = atoi(str + i + 1)) < 0)
  return(-1);
 return(num);
}

int e_ds_line_num(int backend, const char *str, char *file, size_t n)
{
 if (backend == E_DS_GDB)
  return(e_ds_line_num_gdb(str, file, n));
 if (backend == E_DS_DBX)
  return(e_ds_line_num_dbx(str, file, n));
 if (backend == E_DS_XDB)
  return(e_ds_line_num_xdb(str, file, n));
 return(e_ds_line_num_sdb(str));
}

int e_ds_line_num2(const char *str, char *file, size_t n)
{
 const char *sp;
 int i;

 for (i = 0; str[i] != '[' && str[i] != '\0'; i++)
  ;
 if (!str[i])
  return(-1);
 sp = str + i + 1;
 if ((i = e_ds_copy_name(sp, ':', file, n)) < 0)
  return(-1);
 for (i++; isspace((unsigned char)sp[i]); i++)
  ;
 return(atoi(sp + i));
}

void e_ds_sdb_file_of(const char *answer, char *file, size_t n)
{
 int i;

 for (i = 0; answer[i] != '\0' && answer[i] != '\"'; i++)
  ;
 /* sdb's answer has always been taken up to its closing quote or its end */
 if (answer[i] == '\"')
  e_ds_copy_name(answer + i + 1, '\"', file, n);
 else
  file[0] = '\0';
}

int e_ds_where_row_frame(const char *row)
{
 if (row[0] == E_DS_GDB_ROW_MARK && isdigit((unsigned char)row[1]))
  return(atoi(row + 1));
 return(E_DS_NOT_FOUND);
}

/**
 * e_ds_watch_after_equals - Where the value starts in an answer that gives
 * it after "=" (gdb's "$1 = 3", dbx's and jdb's "x = 3"), or after the
 * first blank of xdb's answers that start with an address.
 * @backend: E_DS_*; @str: the answer; @k: set to the value's start.
 * Return: 1, or 0 when the answer gives no value.
 */
static int e_ds_watch_after_equals(int backend, const char *str, int *k)
{
 int i;

 if (backend == E_DS_XDB && str[0] == '0')
 {
  for (i = 1; str[i] != '\0' && !isspace((unsigned char)str[i]); i++)
   ;
 }
 else
  for (i = 0; str[i] != '\0' && str[i] != '='; i++)
   ;
 if (str[i] == '\0')
  return(0);
 for (i++; str[i] != '\0' && isspace((unsigned char)str[i]); i++)
  ;
 *k = i;
 return(1);
}

/**
 * e_ds_watch_after_colon - Where sdb's value starts: after "x:", or at the
 * start of an answer that has no colon.
 */
static int e_ds_watch_after_colon(const char *str)
{
 int k;

 for (k = 0; str[k] != '\0' && str[k] != ':'; k++)
  ;
 if (str[k] == '\0')
  return(0);
 return(k + 1);
}

/**
 * e_ds_skip_blanks - The first character of @s that is not a blank.
 */
static const char *e_ds_skip_blanks(const char *s)
{
 while (*s != '\0' && isspace((unsigned char)*s))
  s++;
 return(s);
}

/**
 * e_ds_watch_answer_state - Is what follows the expression a value, or the
 * debugger's word that it has none?
 */
static int e_ds_watch_answer_state(int backend, const char *text)
{
 if (backend == E_DS_PDB &&
     !strncmp(text, E_DS_PDB_ERROR_LEAD, strlen(E_DS_PDB_ERROR_LEAD)))
  return(E_DS_WATCH_ERROR);
 return(E_DS_WATCH_VALUE);
}

int e_ds_watch_value(int backend, const char *answer, const char **text)
{
 const char *t;
 int k = 0;

 if (!answer)
 {
  *text = E_DS_NO_SYMBOL;
  return(E_DS_WATCH_ERROR);
 }
 if (backend == E_DS_GDB || backend == E_DS_DBX || backend == E_DS_XDB ||
     backend == E_DS_JDB)    /* jdb answers " x = 3", as gdb "$1 = 3" */
 {
  if (!e_ds_watch_after_equals(backend, answer, &k))
  {
   *text = E_DS_NO_SYMBOL;
   return(E_DS_WATCH_ERROR);
  }
 }
 else if (backend == E_DS_SDB)
  k = e_ds_watch_after_colon(answer);
 t = e_ds_skip_blanks(answer + k);
 /* a68g reports an out-of-scope variable with a verbose monitor error
    ("a68g: monitor error: cannot find identifier (n)").  This is normal once
    execution leaves the procedure where the variable lives (e.g. after the
    recursion unwinds); a short note is shown instead. */
 if (backend == E_DS_A68G &&
     (strstr(t, E_DS_A68G_NOT_FOUND) || strstr(t, E_DS_A68G_MONITOR_ERR)))
 {
  *text = E_DS_NOT_IN_SCOPE;
  return(E_DS_WATCH_ERROR);
 }
 *text = t;
 return(e_ds_watch_answer_state(backend, t));
}

int e_ds_brk_is_number_line(int backend, const char *line)
{
 if (backend == E_DS_GDB)
  return(!strncmp(line, E_D_GDB_BRK_ANSWER, strlen(E_D_GDB_BRK_ANSWER)));
 if (backend == E_DS_DBX)
  return(line[0] == E_D_DBX_BRK_OPEN);
 return(!strncmp(line, E_D_XDB_BRK_ADDED, strlen(E_D_XDB_BRK_ADDED)));
}

int e_ds_brk_number(int backend, const char *line)
{
 if (backend == E_DS_GDB)
  return(atoi(line + strlen(E_D_GDB_BRK_ANSWER)));
 return(atoi(line + 1));
}

void e_ds_brk_words(char *out, size_t size, const char *line)
{
 size_t len;

 while (*line && ((unsigned char)*line >= E_D_FIRST_NON_ASCII ||
                  *line == '*' || *line == ' ' || *line == '\t'))
  line++;
 snprintf(out, size, "%s", line);
 len = strlen(out);
 while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' ||
                    out[len - 1] == ' '))
  out[--len] = '\0';
}

/* ---- pdb ---- */

/**
 * e_ds_pdb_line_end - Say whether a character ends a line of pdb's answer.
 */
static int e_ds_pdb_line_end(char c)
{
 return(c == '\0' || c == '\n' || c == '\r');
}

/**
 * e_ds_pdb_has_file - Ask the probe whether a file pdb named exists; with
 * no probe, every name counts.
 */
static int e_ds_pdb_has_file(const char *file, E_DS_FILE_PROBE is_file,
                             void *ctx)
{
 if (!is_file)
  return(1);
 return(is_file(file, ctx));
}

/**
 * e_ds_pdb_stop_number - Find the next "(digits)" of a stop line, one of
 * which follows its file name as "(LINENO)".
 * @from: where to look from.
 * Return: its "(", or NULL when the line has no more.
 */
static const char *e_ds_pdb_stop_number(const char *from)
{
 const char *q, *d;

 for (q = from; !e_ds_pdb_line_end(*q); q++)
 {
  if (*q != '(' || !isdigit((unsigned char)q[1]))
   continue;
  for (d = q + 1; isdigit((unsigned char)*d); d++)
   ;
  if (*d == ')')
   return(q);
 }
 return(NULL);
}

/**
 * e_ds_pdb_stop_tail - Say whether a stop line goes on, after its function
 * name, as pdb ends one: "()", then the line's end or "->" and a value.
 */
static int e_ds_pdb_stop_tail(const char *after)
{
 if (strncmp(after, "()", 2))
  return(0);
 after += 2;
 return(e_ds_pdb_line_end(*after) || !strncmp(after, "->", 2));
}

/**
 * e_ds_pdb_stop_at_number - Read a stop line taking the "(digits)" at @open
 * for its "(LINENO)": what follows must be FUNCNAME "()" and then the
 * line's end or "->REPR".
 * @name: where the file name starts; @open: a "(digits)" after it; @file,
 * @n: receives the file; @line: the line, which may be 0 (pdb 3.14 names
 * exec()'d code "<string>(0)").
 * Return: 1 when the line reads that way, else 0.
 */
static int e_ds_pdb_stop_at_number(const char *name, const char *open,
                                   char *file, size_t n, int *line)
{
 const char *func = strchr(open, ')') + 1, *after;
 size_t len = open - name;

 for (after = func; !e_ds_pdb_line_end(*after) && *after != '(' &&
      *after != ' '; after++)
  ;
 if (after == func)
  return(0);
 /* A part that ends inside the function name is the head of a stop line
    whose rest comes in the next part (a name longer than the buffer). */
 if (*after != '\0' && !e_ds_pdb_stop_tail(after))
  return(0);
 *line = atoi(open + 1);
 if (len == 0 || len >= n || *line < 0)
  return(0);
 memcpy(file, name, len);
 file[len] = '\0';
 return(1);
}

int e_ds_pdb_stop_parse(const char *p, char *file, size_t n, int *line,
                        E_DS_FILE_PROBE is_file, void *ctx)
{
 const char *name = p + strlen(E_DS_PDB_STOP_LEAD), *open;
 char first_file[E_DS_PATH_MAX];
 int first_line = -1;

 if (strncmp(p, E_DS_PDB_STOP_LEAD, strlen(E_DS_PDB_STOP_LEAD)))
  return(0);
 for (open = e_ds_pdb_stop_number(name); open;
      open = e_ds_pdb_stop_number(open + 1))
 {
  if (!e_ds_pdb_stop_at_number(name, open, file, n, line))
   continue;
  if (e_ds_pdb_has_file(file, is_file, ctx))
   return(1);
  if (first_line < 0)
  {
   snprintf(first_file, sizeof first_file, "%s", file);
   first_line = *line;
  }
 }
 if (first_line < 0)
  return(0);
 snprintf(file, n, "%s", first_file);
 *line = first_line;
 return(1);
}

/**
 * e_ds_pdb_best_stop - The stop line that starts between @from and @to in
 * pdb's answer @text, or NULL.
 *
 * The first one that names an existing file wins; then the first one, which
 * a value pdb prints after the stop ("->'x > g(5)'") can never come before.
 * A "> " of pdb's "-> source" line is not a stop.
 */
static const char *e_ds_pdb_best_stop(const char *text, const char *from,
                                      const char *to, E_DS_FILE_PROBE is_file,
                                      void *ctx)
{
 char file[E_DS_PATH_MAX];
 const char *p = from, *first = NULL;
 int line;

 while ((p = strstr(p, E_DS_PDB_STOP_LEAD)) != NULL && p < to)
 {
  if ((p == text || p[-1] != '-') &&
      e_ds_pdb_stop_parse(p, file, sizeof file, &line, is_file, ctx))
  {
   if (e_ds_pdb_has_file(file, is_file, ctx))
    return(p);
   if (!first)
    first = p;
  }
  p += strlen(E_DS_PDB_STOP_LEAD);
 }
 return(first);
}

/**
 * e_ds_pdb_source_line - Where pdb's "-> source" line starts in a part of
 * its answer: the line it prints right after the stop line.
 * Return: its start, or NULL when the part holds none.
 */
static const char *e_ds_pdb_source_line(const char *text)
{
 const char *p = text, *found = NULL;

 if (!strncmp(text, E_DS_PDB_SOURCE_LEAD, strlen(E_DS_PDB_SOURCE_LEAD)))
  found = text;
 while ((p = strstr(p, "\n" E_DS_PDB_SOURCE_LEAD)) != NULL)
 {
  found = p + 1;
  p += 1;
 }
 return(found);
}

const char *e_ds_pdb_find_stop(const char *text, E_DS_FILE_PROBE is_file,
                               void *ctx)
{
 const char *source = e_ds_pdb_source_line(text);
 const char *limit = source ? source : text + strlen(text);
 const char *before, *found;

 if (source && source > text)
 {
  for (before = source - 1; before > text && before[-1] != '\n'; before--)
   ;
  if ((found = e_ds_pdb_best_stop(text, before, source, is_file, ctx)) != NULL)
   return(found);
 }
 return(e_ds_pdb_best_stop(text, text, limit, is_file, ctx));
}

int e_ds_pdb_stop_place(const char *text, char *file, size_t n, int *line,
                        E_DS_FILE_PROBE is_file, void *ctx)
{
 const char *stop = e_ds_pdb_find_stop(text, is_file, ctx);

 if (!stop)
  return(0);
 return(e_ds_pdb_stop_parse(stop, file, n, line, is_file, ctx));
}

int e_ds_pdb_frame_parse(const char *row, E_DS_FRAME *fr)
{
 const char *text, *open, *close, *call;
 size_t len;

 if (strlen(row) <= E_DS_PDB_FRAME_LEAD ||
     (strncmp(row, "> ", E_DS_PDB_FRAME_LEAD) &&
      strncmp(row, "  ", E_DS_PDB_FRAME_LEAD)))
  return(0);
 text = row + E_DS_PDB_FRAME_LEAD;
 open = strchr(text, '(');
 close = open ? strchr(open, ')') : NULL;
 if (!close || open == text || atoi(open + 1) <= 0)
  return(0);
 len = open - text;
 if (len >= sizeof fr->file)
  return(0);
 memcpy(fr->file, text, len);
 fr->file[len] = '\0';
 fr->line = atoi(open + 1);
 call = strchr(close + 1, '(');
 len = call ? (size_t)(call - (close + 1)) : strlen(close + 1);
 if (len >= sizeof fr->func)
  len = sizeof fr->func - 1;
 memcpy(fr->func, close + 1, len);
 fr->func[len] = '\0';
 return(1);
}

/* ---- jdb ---- */

int e_ds_jdb_stop_line(const char *stop)
{
 const char *lp = strstr(stop, E_D_JDB_LINE_MARK), *p;
 int line = 0;

 if (!lp)
  return(0);
 for (p = lp + strlen(E_D_JDB_LINE_MARK); *p; p++)
 {
  if (*p == E_DS_JDB_DIGIT_GROUP && isdigit((unsigned char)p[1]) &&
      p > lp + strlen(E_D_JDB_LINE_MARK))
   continue;
  if (!isdigit((unsigned char)*p))
   break;
  line = line * E_DS_DECIMAL_BASE + (*p - '0');
 }
 return(line);
}

int e_ds_jdb_stop_class(const char *stop, char *out, size_t n)
{
 const char *lp = strstr(stop, E_D_JDB_LINE_MARK);
 const char *paren, *start, *method_dot;
 char *nested;
 size_t len;

 if (!lp)
  return(0);
 for (paren = lp; paren > stop && *paren != '('; paren--)
  ;
 for (start = paren; start > stop && !isspace((unsigned char)start[-1]); start--)
  ;
 for (method_dot = paren; method_dot > start && *method_dot != '.'; method_dot--)
  ;
 if (*paren != '(' || method_dot == start)
  return(0);
 len = method_dot - start;
 if (len >= n)
  return(0);
 memcpy(out, start, len);
 out[len] = '\0';
 nested = strchr(out, E_D_JDB_NESTED_SEP);
 if (nested)
  *nested = '\0';
 return(1);
}

const char *e_ds_jdb_line_starts_with_mark(const char *line, const char *mark)
{
 while (strncmp(line, E_D_JDB_ECHO, strlen(E_D_JDB_ECHO)) == 0)
  line += strlen(E_D_JDB_ECHO);
 if (strncmp(line, mark, strlen(mark)) == 0)
  return(line);
 return(NULL);
}

/**
 * e_ds_jdb_line_names_stop - The stop mark the line at @line starts with
 * (a breakpoint, a step or an uncaught exception), or NULL.
 */
static const char *e_ds_jdb_line_names_stop(const char *line)
{
 const char *at = e_ds_jdb_line_starts_with_mark(line, E_D_JDB_STOP_MARK);

 if (!at)
  at = e_ds_jdb_line_starts_with_mark(line, E_D_JDB_STEP_MARK);
 if (!at)
  at = e_ds_jdb_line_starts_with_mark(line, E_D_JDB_EXCEPTION_MARK);
 return(at);
}

const char *e_ds_jdb_stop_line_in(const char *text)
{
 const char *line = text, *found = NULL, *at;

 while (line)
 {
  at = e_ds_jdb_line_names_stop(line);
  if (at)
   found = at;
  line = strchr(line, '\n');
  if (line)
   line++;
 }
 return(found);
}

int e_ds_jdb_says(const char *text, const char *mark)
{
 const char *line = text;

 while (line)
 {
  if (e_ds_jdb_line_starts_with_mark(line, mark))
   return(1);
  line = strchr(line, '\n');
  if (line)
   line++;
 }
 return(0);
}

int e_ds_jdb_says_end(const char *text)
{
 return(e_ds_jdb_says(text, E_D_JDB_EXIT) ||
        e_ds_jdb_says(text, E_D_JDB_DISCONNECTED));
}

int e_ds_jdb_asks_command(const char *text)
{
 size_t len = strlen(text), tail = strlen(E_D_JDB_PROMPT_TAIL);

 return(len >= tail && strcmp(text + len - tail, E_D_JDB_PROMPT_TAIL) == 0);
}

int e_ds_jdb_reply_is_stop(const char *text)
{
 const char *stop;

 if (!e_ds_jdb_asks_command(text))
  return(0);
 stop = e_ds_jdb_stop_line_in(text);
 if (!stop)
  return(0);
 return(strstr(stop, E_D_JDB_LINE_MARK) != NULL);
}

int e_ds_jdb_says_suspended_text(const char *text)
{
 size_t len = strlen(text), tail = strlen(E_D_JDB_BARE_PROMPT);

 if (len < tail || strcmp(text + len - tail, E_D_JDB_BARE_PROMPT) != 0)
  return(0);
 return(e_ds_jdb_says(text, E_D_JDB_SUSPENDED));
}

/**
 * e_ds_jdb_spec_of - Cut "P:7" out of jdb's answer after one of its words.
 * @after: the text that follows the words; @spec, @n: receives it, without
 * the full stop jdb ends "Deferring breakpoint P:7." with.
 */
static void e_ds_jdb_spec_of(const char *after, char *spec, size_t n)
{
 size_t len = strcspn(after, " \t\r\n");

 if (len >= n)
  len = n - 1;
 memcpy(spec, after, len);
 spec[len] = '\0';
 if (len > 0 && spec[len - 1] == '.')
  spec[len - 1] = '\0';
}

int e_ds_jdb_live_word(const char *line, char *spec, size_t n,
                       const char **words)
{
 const char *at;

 if ((at = strstr(line, E_D_JDB_SET_NOW)) != NULL)
 {
  e_ds_jdb_spec_of(at + strlen(E_D_JDB_SET_NOW), spec, n);
  *words = at;
  return(E_D_BRK_VERIFIED);
 }
 if ((at = strstr(line, E_D_JDB_DEFERS)) != NULL)
 {
  e_ds_jdb_spec_of(at + strlen(E_D_JDB_DEFERS), spec, n);
  *words = at;
  return(E_D_BRK_PENDING);
 }
 if ((at = strstr(line, E_D_JDB_UNABLE)) != NULL)
 {
  e_ds_jdb_spec_of(at + strlen(E_D_JDB_UNABLE), spec, n);
  *words = at;
  return(E_D_BRK_REFUSED);
 }
 return(E_DS_NOT_FOUND);
}

/**
 * e_ds_jdb_frame_line - A line number as jdb writes it, "1,620" included.
 */
static int e_ds_jdb_frame_line(const char *s)
{
 int n = 0;

 for (; isdigit((unsigned char)*s) || *s == E_DS_JDB_DIGIT_GROUP; s++)
  if (*s != E_DS_JDB_DIGIT_GROUP)
   n = n * E_DS_DECIMAL_BASE + (*s - '0');
 return(n);
}

int e_ds_jdb_frame_parse(const char *row, E_DS_FRAME *fr)
{
 const char *p, *open, *close, *colon;
 size_t len;

 for (p = strchr(row, '['); p; p = strchr(p + 1, '['))
  if ((p == row || isspace((unsigned char)p[-1])) && isdigit((unsigned char)p[1]))
   break;
 if (!p || !(p = strstr(p, "] ")))
  return(0);
 p += 2;
 open = strstr(p, " (");
 close = open ? strchr(open, ')') : NULL;
 if (!close)
  return(0);
 len = open - p;
 if (len == 0 || len >= sizeof fr->func)
  return(0);
 memcpy(fr->func, p, len);
 fr->func[len] = '\0';
 colon = strchr(open, ':');
 fr->file[0] = '\0';
 fr->line = E_DS_NO_LINE;
 if (!colon || colon > close)
  return(1);
 len = colon - (open + 2);
 if (len < sizeof fr->file)
 {
  memcpy(fr->file, open + 2, len);
  fr->file[len] = '\0';
 }
 fr->line = e_ds_jdb_frame_line(colon + 1);
 return(1);
}

/* ---- a68g ---- */

int e_ds_a68g_parse_line(const char *s)
{
 int n = 0, k;

 if (!isdigit((unsigned char)s[0]))
  return(-1);
 for (k = 0; isdigit((unsigned char)s[k]); k++)
  n = n * E_DS_DECIMAL_BASE + (s[k] - '0');
 if (s[k] != ' ' && s[k] != '\t')
  return(-1);
 return(n > 0 ? n : -1);
}

/**
 * e_ds_a68g_is_thread_line - Is this the line a68g writes just before its
 * prompt at a stop?
 */
static int e_ds_a68g_is_thread_line(const char *s, int len)
{
 if (len == (int)sizeof E_DS_A68G_MAIN_THREAD - 1 &&
     !strncmp(s, E_DS_A68G_MAIN_THREAD, len))
  return(1);
 if (len == (int)sizeof E_DS_A68G_OTHER_THREAD - 1 &&
     !strncmp(s, E_DS_A68G_OTHER_THREAD, len))
  return(1);
 return(0);
}

/**
 * e_ds_a68g_is_column_marker - Is this the line a68g writes under a source
 * line to point at the column it stopped in?
 */
static int e_ds_a68g_is_column_marker(const char *s, int len)
{
 int i, marks = 0;

 for (i = 0; i < len; i++)
 {
  if (s[i] == E_DS_A68G_MARK_CHAR)
   marks++;
  else if (s[i] != ' ')
   return(0);
 }
 return(marks > 0);
}

/**
 * e_ds_a68g_is_source_line - Is this a numbered source line, as a68g shows
 * the line it stopped on?
 */
static int e_ds_a68g_is_source_line(const char *s, int len)
{
 char line[E_DS_PATH_MAX + E_DS_LINE_MAX];

 if (len >= (int)sizeof line)
  len = sizeof line - 1;
 memcpy(line, s, len);
 line[len] = '\0';
 return(e_ds_a68g_parse_line(line) > 0);
}

int e_ds_a68g_line_kind(const char *s, int len)
{
 if (len >= (int)sizeof E_D_A68G_TERMINATE - 1 &&
     !strncmp(s, E_D_A68G_TERMINATE, sizeof E_D_A68G_TERMINATE - 1))
  return(E_D_A68G_LINE_QUESTION);
 if (e_ds_a68g_is_thread_line(s, len))
  return(E_D_A68G_LINE_THREAD);
 if (e_ds_a68g_is_column_marker(s, len))
  return(E_D_A68G_LINE_MARKER);
 if (e_ds_a68g_is_source_line(s, len))
  return(E_D_A68G_LINE_SOURCE);
 return(E_D_A68G_LINE_OTHER);
}

int e_ds_a68g_lines_make_a_stop(const int *kinds)
{
 return(kinds[0] == E_D_A68G_LINE_THREAD &&
        kinds[1] == E_D_A68G_LINE_MARKER &&
        kinds[2] == E_D_A68G_LINE_SOURCE);
}

void e_ds_a68g_proc_name(const char *text, char *out, size_t n)
{
 size_t len;

 while (isspace((unsigned char)*text))
  text++;
 if (!strncmp(text, E_DS_A68G_PROC_WORD, strlen(E_DS_A68G_PROC_WORD)))
  text += strlen(E_DS_A68G_PROC_WORD);
 for (len = 0; text[len] && !isspace((unsigned char)text[len]) &&
      text[len] != '=' && text[len] != '('; len++)
  ;
 if (len >= n)
  len = n - 1;
 memcpy(out, text, len);
 out[len] = '\0';
}

/* ================================================================== */
/* The session                                                         */
/* ================================================================== */

void e_ds_reset(E_D_SESSION *s)
{
 E_DS_FILE_PROBE probe = s->is_file;
 void *ctx = s->is_file_ctx;

 memset(s, 0, sizeof *s);
 s->phase = E_DS_STARTING;
 s->stop_reason = E_DS_STOP_UNKNOWN;
 s->marked_frame = E_DS_NOT_FOUND;
 s->is_file = probe;
 s->is_file_ctx = ctx;
}

void e_ds_set_file_probe(E_D_SESSION *s, E_DS_FILE_PROBE probe, void *ctx)
{
 s->is_file = probe;
 s->is_file_ctx = ctx;
}

void e_ds_set_watch(E_D_SESSION *s, int i, const char *expr)
{
 if (i < 0 || i >= E_DS_WATCHES_MAX)
  return;
 snprintf(s->watches[i].expr, sizeof s->watches[i].expr, "%s", expr);
 s->watches[i].value[0] = '\0';
 s->watches[i].state = E_DS_WATCH_WAITING;
 if (i >= s->nwatches)
  s->nwatches = i + 1;
 s->changed |= E_DS_CHANGED_WATCHES;
}

void e_ds_set_brk(E_D_SESSION *s, int i, const char *file, int line)
{
 if (i < 0 || i >= E_DS_BRKS_MAX)
  return;
 snprintf(s->brks[i].file, sizeof s->brks[i].file, "%s", file);
 s->brks[i].line = line;
 s->brks[i].id = 0;
 s->brks[i].state = E_D_BRK_PENDING;
 s->brks[i].msg[0] = '\0';
 if (i >= s->nbrks)
  s->nbrks = i + 1;
 s->changed |= E_DS_CHANGED_BRKS;
}

/**
 * e_ds_put_stop - The program stopped at @file:@line: one frame, marked.
 * @s: the session; @reason: E_DS_STOP_*; @func: the function, or "".
 *
 * The stop's own report names one place; the stack, when asked for, comes
 * after it through e_ds_take_where_row.
 */
static void e_ds_put_stop(E_D_SESSION *s, int reason, const char *file,
                          int line, const char *func)
{
 E_DS_FRAME *fr = &s->frames[0];

 snprintf(fr->file, sizeof fr->file, "%s", file);
 snprintf(fr->func, sizeof fr->func, "%s", func);
 fr->line = line;
 fr->level = 0;
 s->nframes = 1;
 s->marked_frame = 0;
 s->phase = E_DS_STOPPED;
 s->stop_reason = reason;
 s->changed |= E_DS_CHANGED_MARK | E_DS_CHANGED_STACK | E_DS_CHANGED_STATUS;
}

/**
 * e_ds_take_pdb_stop - pdb's answer names its stop "> file(line)func()".
 */
static int e_ds_take_pdb_stop(E_D_SESSION *s, const char *text)
{
 char file[E_DS_PATH_MAX];
 E_DS_FRAME fr;
 const char *stop = e_ds_pdb_find_stop(text, s->is_file, s->is_file_ctx);
 int line;

 if (!stop || !e_ds_pdb_stop_parse(stop, file, sizeof file, &line,
                                   s->is_file, s->is_file_ctx))
  return(0);
 fr.func[0] = '\0';
 e_ds_pdb_frame_parse(stop, &fr);
 e_ds_put_stop(s, E_DS_STOP_STEP, file, line, fr.func);
 return(1);
}

/**
 * e_ds_jdb_stop_reason - A breakpoint for "Breakpoint hit:", a step for
 * the rest.
 */
static int e_ds_jdb_stop_reason(const char *stop)
{
 if (!strncmp(stop, E_D_JDB_STOP_MARK, strlen(E_D_JDB_STOP_MARK)))
  return(E_DS_STOP_BREAKPOINT);
 return(E_DS_STOP_STEP);
}

/**
 * e_ds_take_jdb_stop - jdb's answer names a class and its "line=".  The
 * frame's file is the class: finding its source is the editor's business.
 */
static int e_ds_take_jdb_stop(E_D_SESSION *s, const char *text)
{
 char cls[E_DS_PATH_MAX];
 const char *stop = e_ds_jdb_stop_line_in(text);
 int line;

 if (!stop)
  return(0);
 line = e_ds_jdb_stop_line(stop);
 if (line <= 0 || !e_ds_jdb_stop_class(stop, cls, sizeof cls))
  return(0);
 e_ds_put_stop(s, e_ds_jdb_stop_reason(stop), cls, line, "");
 return(1);
}

/**
 * e_ds_take_a68g_stop - a68g's stop is its numbered source line; the file
 * is the one the program was started on, which the editor knows.
 */
static int e_ds_take_a68g_stop(E_D_SESSION *s, const char *text)
{
 const char *line = text;
 int num;

 while (line)
 {
  num = e_ds_a68g_parse_line(line);
  if (num > 0)
  {
   e_ds_put_stop(s, E_DS_STOP_STEP, "", num, "");
   return(1);
  }
  line = strchr(line, '\n');
  if (line)
   line++;
 }
 return(0);
}

/**
 * e_ds_take_named_stop - gdb's, dbx's or xdb's line names file and line.
 */
static int e_ds_take_named_stop(E_D_SESSION *s, int backend, const char *text)
{
 char file[E_DS_PATH_MAX];
 int line = e_ds_line_num(backend, text, file, sizeof file);
 int reason = E_DS_STOP_STEP;

 if (line < 0)
  return(0);
 if (!strncmp(text, E_DS_GDB_BREAKPOINT, strlen(E_DS_GDB_BREAKPOINT)))
  reason = E_DS_STOP_BREAKPOINT;
 e_ds_put_stop(s, reason, file, line, "");
 return(1);
}

int e_ds_take_cli_stop_line(E_D_SESSION *s, int backend, const char *text)
{
 if (backend == E_DS_PDB)
  return(e_ds_take_pdb_stop(s, text));
 if (backend == E_DS_JDB)
  return(e_ds_take_jdb_stop(s, text));
 if (backend == E_DS_A68G)
  return(e_ds_take_a68g_stop(s, text));
 if (backend == E_DS_GDB || backend == E_DS_DBX || backend == E_DS_XDB)
  return(e_ds_take_named_stop(s, backend, text));
 return(0);
}

void e_ds_begin_stack(E_D_SESSION *s)
{
 s->nframes = 0;
 s->marked_frame = E_DS_NOT_FOUND;
 s->changed |= E_DS_CHANGED_STACK;
}

/**
 * e_ds_add_frame - Append @fr as the next frame out, if there is room.
 */
static int e_ds_add_frame(E_D_SESSION *s, const E_DS_FRAME *fr)
{
 if (s->nframes >= E_DS_FRAMES_MAX)
  return(0);
 s->frames[s->nframes] = *fr;
 s->frames[s->nframes].level = s->nframes;
 s->nframes++;
 s->changed |= E_DS_CHANGED_STACK;
 return(1);
}

/**
 * e_ds_named_row_frame - A row of gdb's or dbx's "where", or of sdb's and
 * xdb's "t": a frame when it names a line, or (gdb) starts "#N".
 */
static int e_ds_named_row_frame(int backend, const char *row, E_DS_FRAME *fr)
{
 int line;

 fr->func[0] = '\0';
 if (backend == E_DS_SDB || backend == E_DS_XDB)
  line = e_ds_line_num2(row, fr->file, sizeof fr->file);
 else
  line = e_ds_line_num(backend, row, fr->file, sizeof fr->file);
 if (line >= 0)
 {
  fr->line = line;
  return(1);
 }
 fr->file[0] = '\0';
 fr->line = E_DS_NO_LINE;
 return(backend == E_DS_GDB &&
        e_ds_where_row_frame(row) != E_DS_NOT_FOUND);
}

/**
 * e_ds_a68g_row_frame - A row of a68g's "calls": "2   PROC sq = ...".
 */
static int e_ds_a68g_row_frame(const char *row, E_DS_FRAME *fr)
{
 int line = e_ds_a68g_parse_line(row);

 if (line <= 0)
  return(0);
 while (isdigit((unsigned char)*row))
  row++;
 e_ds_a68g_proc_name(row, fr->func, sizeof fr->func);
 fr->file[0] = '\0';
 fr->line = line;
 return(1);
}

int e_ds_take_where_row(E_D_SESSION *s, int backend, const char *row)
{
 E_DS_FRAME fr;
 int is_frame;

 if (backend == E_DS_PDB)
  is_frame = e_ds_pdb_frame_parse(row, &fr);
 else if (backend == E_DS_JDB)
  is_frame = e_ds_jdb_frame_parse(row, &fr);
 else if (backend == E_DS_A68G)
  is_frame = e_ds_a68g_row_frame(row, &fr);
 else
  is_frame = e_ds_named_row_frame(backend, row, &fr);
 if (!is_frame)
  return(0);
 e_ds_add_frame(s, &fr);
 return(1);
}

void e_ds_answer_borrow(E_D_ANSWER *a, const char *text, int error)
{
 /* The reader only reads: the cast lends the text, it never writes it. */
 a->text = (char *)text;
 a->len = 0;
 if (text)
  a->len = strlen(text);
 a->size = 0;
 a->error = error;
}

/**
 * e_ds_answer_text - The answer's text, or NULL when the debugger gave
 * nothing before its prompt (no answer, or an empty one).
 */
static const char *e_ds_answer_text(const E_D_ANSWER *a)
{
 if (!a || !a->text || a->len == 0)
  return(NULL);
 return(a->text);
}

void e_ds_take_watch_answer(E_D_SESSION *s, int backend, int i,
                            const E_D_ANSWER *a)
{
 const char *text;

 if (i < 0 || i >= E_DS_WATCHES_MAX)
  return;
 s->watches[i].state = e_ds_watch_value(backend, e_ds_answer_text(a), &text);
 if (a && a->error)
  s->watches[i].state = E_DS_WATCH_ERROR;
 snprintf(s->watches[i].value, sizeof s->watches[i].value, "%s", text);
 if (i >= s->nwatches)
  s->nwatches = i + 1;
 s->changed |= E_DS_CHANGED_WATCHES;
}

/**
 * e_ds_brk_put - Record what the debugger made of breakpoint @i.
 */
static void e_ds_brk_put(E_D_SESSION *s, int i, int state, int id,
                         const char *words)
{
 s->brks[i].state = state;
 s->brks[i].id = id;
 s->brks[i].msg[0] = '\0';
 if (words)
  e_ds_brk_words(s->brks[i].msg, sizeof s->brks[i].msg, words);
 if (i >= s->nbrks)
  s->nbrks = i + 1;
 s->changed |= E_DS_CHANGED_BRKS;
}

/**
 * e_ds_take_numbered_brk - gdb's, dbx's or xdb's answer: the number line,
 * the error line, or nothing.
 */
static void e_ds_take_numbered_brk(E_D_SESSION *s, int backend, int i,
                                   const E_D_ANSWER *a)
{
 const char *text = e_ds_answer_text(a);

 if (a->error && text)
  e_ds_brk_put(s, i, E_D_BRK_REFUSED, 0, text);
 else if (!text)
  e_ds_brk_put(s, i, E_D_BRK_REFUSED, 0, E_D_BRK_NO_NUMBER);
 else
  e_ds_brk_put(s, i, E_D_BRK_VERIFIED, e_ds_brk_number(backend, text), NULL);
}

/**
 * e_ds_take_jdb_brk - jdb's word on a breakpoint: set, deferred, refused.
 */
static void e_ds_take_jdb_brk(E_D_SESSION *s, int i, const E_D_ANSWER *a)
{
 char spec[E_DS_PATH_MAX];
 const char *words = NULL, *text = e_ds_answer_text(a);
 int state;

 if (!text)
  return;
 state = e_ds_jdb_live_word(text, spec, sizeof spec, &words);
 if (state == E_DS_NOT_FOUND)
  return;
 if (state == E_D_BRK_VERIFIED)
  words = NULL;
 e_ds_brk_put(s, i, state, 0, words);
}

/**
 * e_ds_take_line_brk - pdb's or a68g's answer: an error line refuses it.
 */
static void e_ds_take_line_brk(E_D_SESSION *s, int i, const E_D_ANSWER *a)
{
 const char *text = e_ds_answer_text(a);

 if (a->error && text)
  e_ds_brk_put(s, i, E_D_BRK_REFUSED, 0, text);
 else
  e_ds_brk_put(s, i, E_D_BRK_VERIFIED, 0, NULL);
}

void e_ds_take_brk_answer(E_D_SESSION *s, int backend, int i,
                          const E_D_ANSWER *a)
{
 if (i < 0 || i >= E_DS_BRKS_MAX || !a)
  return;
 if (backend == E_DS_GDB || backend == E_DS_DBX || backend == E_DS_XDB)
  e_ds_take_numbered_brk(s, backend, i, a);
 else if (backend == E_DS_JDB)
  e_ds_take_jdb_brk(s, i, a);
 else
  e_ds_take_line_brk(s, i, a);
}

/**
 * e_ds_dap_reason - E_DS_STOP_* for a DAP "reason".
 */
static int e_ds_dap_reason(const char *reason)
{
 if (!reason)
  return(E_DS_STOP_UNKNOWN);
 if (!strcmp(reason, E_DS_DAP_BREAKPOINT))
  return(E_DS_STOP_BREAKPOINT);
 if (!strcmp(reason, E_DS_DAP_STEP))
  return(E_DS_STOP_STEP);
 if (!strcmp(reason, E_DS_DAP_PAUSE))
  return(E_DS_STOP_PAUSE);
 if (!strcmp(reason, E_DS_DAP_EXCEPTION))
  return(E_DS_STOP_SIGNAL);
 return(E_DS_STOP_UNKNOWN);
}

void e_ds_take_dap_stopped(E_D_SESSION *s, const char *reason,
                           const E_DS_FRAME *fr, int n)
{
 int k;

 e_ds_begin_stack(s);
 for (k = 0; k < n; k++)
  e_ds_add_frame(s, &fr[k]);
 s->phase = E_DS_STOPPED;
 s->stop_reason = e_ds_dap_reason(reason);
 s->marked_frame = e_ds_frame_to_mark(s, NULL, NULL);
 s->changed |= E_DS_CHANGED_MARK | E_DS_CHANGED_STATUS;
}

int e_ds_frame_to_mark(const E_D_SESSION *s,
                       int (*has_source)(const char *file, void *ctx),
                       void *ctx)
{
 int k;

 for (k = 0; k < s->nframes; k++)
 {
  if (s->frames[k].line <= E_DS_NO_LINE || !s->frames[k].file[0])
   continue;
  if (!has_source || has_source(s->frames[k].file, ctx))
   return(k);
 }
 return(E_DS_NOT_FOUND);
}

/* ================================================================== */
/* gdb over MI                                                         */
/* ================================================================== */

void e_ds_set_interrupt(E_D_SESSION *s, int why)
{
 s->interrupt_for = why;
}

void e_ds_console_clear(E_D_SESSION *s)
{
 s->console[0] = '\0';
 s->console_len = 0;
 s->console_lost = 0;
}

/**
 * e_ds_skip_style - Step over a terminal styling sequence (ESC [ ... final)
 * that starts at @in.  A byte that cannot be inside such a sequence ends
 * it unread, so a broken sequence never swallows the words after it.
 * Return: where the text goes on.
 */
static const char *e_ds_skip_style(const char *in)
{
 unsigned char c;

 in++;
 if (*in != E_DS_CSI)
  return(in);
 in++;
 for (;;)
 {
  c = (unsigned char)*in;
  if (c >= E_DS_CSI_FINAL_FIRST && c <= E_DS_CSI_FINAL_LAST)
   return(in + 1);
  if (c < E_DS_CSI_INNER_FIRST || c > E_DS_CSI_INNER_LAST)
   return(in);
  in++;
 }
}

/**
 * e_ds_utf8_cut - Where to cut @len bytes of @s so no character is split.
 * Return: @len, or less when the last character would be cut short.
 *
 * Messages and the status row then never show half a character when gdb's
 * words are longer than the room for them.
 */
static size_t e_ds_utf8_cut(const char *s, size_t len)
{
 size_t start = len, need;
 unsigned char lead;

 while (start > 0 && ((unsigned char)s[start - 1] & E_DS_UTF8_TOP_BITS)
                     == E_DS_UTF8_CONT)
  start--;
 if (start == 0)
  return(len);
 lead = (unsigned char)s[start - 1];
 if (lead < E_DS_UTF8_LEAD2)
  return(len);
 if (lead >= E_DS_UTF8_LEAD4)
  need = E_DS_UTF8_BYTES4;
 else if (lead >= E_DS_UTF8_LEAD3)
  need = E_DS_UTF8_BYTES3;
 else
  need = E_DS_UTF8_BYTES2;
 if (len - (start - 1) < need)
  return(start - 1);
 return(len);
}

/**
 * e_ds_strip_style - Copy @in to @out without terminal styling.
 * @out, @n: where it goes, cut to fit at a character's end; @in: gdb's words.
 *
 * gdb 18 styles the file name in an error message even when it writes to a
 * pipe.  The status row and Messages show the words, not the colour codes.
 * Return: the length the whole text has once stripped, as snprintf does.
 */
static size_t e_ds_strip_style(char *out, size_t n, const char *in)
{
 size_t total = 0;

 while (*in)
 {
  if (*in == E_DS_ESC)
  {
   in = e_ds_skip_style(in);
   continue;
  }
  if (total + 1 < n)
   out[total] = *in;
  total++;
  in++;
 }
 if (n == 0)
  return(total);
 if (total < n)
  out[total] = '\0';
 else
  out[e_ds_utf8_cut(out, n - 1)] = '\0';
 return(total);
}

/**
 * e_ds_console_add - Keep gdb's words until Messages shows them.
 * @s: the session; @text: a console, target or log record's text.
 *
 * Records come in pieces ("\nProgram", " received signal SIGINT"), so the
 * text is joined as it arrives.  What does not fit is counted, not kept.
 */
static void e_ds_console_add(E_D_SESSION *s, const char *text)
{
 size_t room = E_DS_CONSOLE_MAX - (size_t)s->console_len;
 size_t total, kept;

 total = e_ds_strip_style(s->console + s->console_len, room, text);
 kept = strlen(s->console + s->console_len);
 s->console_len += (int)kept;
 s->console_lost += (int)(total - kept);
 if (total > 0)
  s->changed |= E_DS_CHANGED_CONSOLE;
}

/**
 * e_ds_mi_frame - Read a frame tuple of a stop or of a stack listing.
 * @fn: the tuple; @fr: receives the frame.
 *
 * The file is the full path when gdb gives one, so the user's bar finds the
 * window whatever the directory the editor runs in.
 */
static void e_ds_mi_frame(const E_GDBMI_RECORD *r, const E_GDBMI_NODE *fn,
                          E_DS_FRAME *fr)
{
 const char *text;

 memset(fr, 0, sizeof *fr);
 text = e_gdbmi_get_in(r, fn, "fullname");
 if (!text || !text[0])
  text = e_gdbmi_get_in(r, fn, "file");
 if (text)
  snprintf(fr->file, sizeof fr->file, "%s", text);
 text = e_gdbmi_get_in(r, fn, "func");
 if (text)
  snprintf(fr->func, sizeof fr->func, "%s", text);
 fr->line = e_gdbmi_number(e_gdbmi_get_in(r, fn, "line"), E_DS_MI_DECIMAL,
                           E_DS_NO_LINE);
 if (fr->line < E_DS_NO_LINE)
  fr->line = E_DS_NO_LINE;
 fr->level = e_gdbmi_number(e_gdbmi_get_in(r, fn, "level"), E_DS_MI_DECIMAL,
                            0);
}

/**
 * e_ds_mi_put_stop - The program stopped, where the record's frame says.
 * @reason: E_DS_STOP_*.
 */
static void e_ds_mi_put_stop(E_D_SESSION *s, int reason,
                             const E_GDBMI_RECORD *r)
{
 const E_GDBMI_NODE *fn = e_gdbmi_find(r, "frame");
 E_DS_FRAME fr;

 if (!fn)
 {
  s->phase = E_DS_STOPPED;
  s->stop_reason = reason;
  s->changed |= E_DS_CHANGED_STATUS;
  return;
 }
 e_ds_mi_frame(r, fn, &fr);
 e_ds_put_stop(s, reason, fr.file, fr.line, fr.func);
}

/**
 * e_ds_mi_reason - E_DS_STOP_* for a stop's "reason".
 */
static int e_ds_mi_reason(const char *reason)
{
 static const struct
 {
  const char *name;
  int stop;
 } known[] = {
  { "breakpoint-hit", E_DS_STOP_BREAKPOINT },
  { "watchpoint-trigger", E_DS_STOP_BREAKPOINT },
  { "read-watchpoint-trigger", E_DS_STOP_BREAKPOINT },
  { "access-watchpoint-trigger", E_DS_STOP_BREAKPOINT },
  { "end-stepping-range", E_DS_STOP_STEP },
  { "function-finished", E_DS_STOP_STEP },
  { "location-reached", E_DS_STOP_STEP },
  { "signal-received", E_DS_STOP_SIGNAL },
 };
 size_t k;

 for (k = 0; k < sizeof known / sizeof known[0]; k++)
  if (!strcmp(reason, known[k].name))
   return(known[k].stop);
 return(E_DS_STOP_UNKNOWN);
}

/**
 * e_ds_mi_is_exit_reason - Does this "reason" say the program is gone?
 */
static int e_ds_mi_is_exit_reason(const char *reason)
{
 return(!strcmp(reason, "exited") || !strcmp(reason, "exited-normally")
        || !strcmp(reason, "exited-signalled"));
}

/**
 * e_ds_mi_exited - The program ended: its exit code, or the signal that
 * killed it, for the status row.
 */
static int e_ds_mi_exited(E_D_SESSION *s, const E_GDBMI_RECORD *r,
                          const char *reason)
{
 const char *sig = e_gdbmi_get(r, "signal-name");

 s->phase = E_DS_ENDED;
 s->stop_reason = E_DS_STOP_EXITED;
 s->exit_code = e_gdbmi_number(e_gdbmi_get(r, "exit-code"), E_DS_MI_EXIT_BASE,
                               0);
 if (sig && !strcmp(reason, "exited-signalled"))
  snprintf(s->signal, sizeof s->signal, "%s", sig);
 s->changed |= E_DS_CHANGED_ENDED | E_DS_CHANGED_STATUS;
 return(1);
}

/**
 * e_ds_mi_own_stop - The stop is the editor's own interrupt: the program is
 * stopped, and nothing of it is for the user to see.
 */
static int e_ds_mi_own_stop(E_D_SESSION *s)
{
 s->phase = E_DS_STOPPED;
 s->stop_reason = E_DS_STOP_PAUSE;
 s->own_stop = 1;
 return(1);
}

/**
 * e_ds_mi_signal_stop - The program got a signal.  When it is the SIGINT
 * the editor sent, it is the user's Pause (shown) or the editor's own stop
 * to apply a breakpoint change (not shown), as e_ds_set_interrupt said;
 * any other SIGINT is the program's.
 */
static int e_ds_mi_signal_stop(E_D_SESSION *s, const E_GDBMI_RECORD *r)
{
 const char *name = e_gdbmi_get(r, "signal-name");
 int why = E_DS_INT_NONE;

 if (name)
  snprintf(s->signal, sizeof s->signal, "%s", name);
 if (name && !strcmp(name, E_DS_MI_SIGINT))
  why = s->interrupt_for;
 if (why == E_DS_INT_NONE)
 {
  e_ds_mi_put_stop(s, E_DS_STOP_SIGNAL, r);
  return(1);
 }
 s->interrupt_for = E_DS_INT_NONE;
 s->signal[0] = '\0';
 if (why == E_DS_INT_SYNC)
  return(e_ds_mi_own_stop(s));
 e_ds_mi_put_stop(s, E_DS_STOP_PAUSE, r);
 return(1);
}

/**
 * e_ds_mi_stopped - Take *stopped.
 */
static int e_ds_mi_stopped(E_D_SESSION *s, const E_GDBMI_RECORD *r)
{
 const char *reason = e_gdbmi_get(r, "reason");

 if (!reason)
  reason = "";
 s->own_stop = 0;
 s->signal[0] = '\0';
 if (e_ds_mi_is_exit_reason(reason))
  return(e_ds_mi_exited(s, r, reason));
 if (!strcmp(reason, "signal-received"))
  return(e_ds_mi_signal_stop(s, r));
 e_ds_mi_put_stop(s, e_ds_mi_reason(reason), r);
 return(1);
}

/**
 * e_ds_mi_running - gdb says the program runs.
 */
static int e_ds_mi_running(E_D_SESSION *s)
{
 s->phase = E_DS_RUNNING;
 s->changed |= E_DS_CHANGED_STATUS;
 return(1);
}

/**
 * e_ds_mi_stack - Take the frames of -stack-list-frames' answer.
 * Return: 1 when the record holds a stack.
 */
static int e_ds_mi_stack(E_D_SESSION *s, const E_GDBMI_RECORD *r)
{
 const E_GDBMI_NODE *list = e_gdbmi_find(r, "stack");
 const E_GDBMI_NODE *item;
 E_DS_FRAME fr;

 if (!list)
  return(0);
 e_ds_begin_stack(s);
 for (item = e_gdbmi_first_item(r, list); item;
      item = e_gdbmi_next_item(r, item))
 {
  e_ds_mi_frame(r, item, &fr);
  if (!e_ds_add_frame(s, &fr))
   break;
 }
 /* gdb names a library's frames by its build tree ("./nptl/..."), so only
    a frame whose file is here takes the mark. */
 s->marked_frame = e_ds_frame_to_mark(s, s->is_file, s->is_file_ctx);
 return(1);
}

/**
 * e_ds_mi_result - Take a result record no request owns: an error for the
 * status row, the end of gdb, the start of a run, a stack.
 */
static int e_ds_mi_result(E_D_SESSION *s, const E_GDBMI_RECORD *r)
{
 const char *msg;

 if (!strcmp(r->klass, E_DS_MI_CLASS_ERROR))
 {
  msg = e_gdbmi_get(r, "msg");
  e_ds_strip_style(s->last_error, sizeof s->last_error, msg ? msg : "");
  s->changed |= E_DS_CHANGED_STATUS;
  return(1);
 }
 if (!strcmp(r->klass, E_DS_MI_CLASS_EXIT))
 {
  s->phase = E_DS_ENDED;
  s->changed |= E_DS_CHANGED_ENDED | E_DS_CHANGED_STATUS;
  return(1);
 }
 if (!strcmp(r->klass, E_DS_MI_CLASS_RUNNING))
  return(e_ds_mi_running(s));
 if (!strcmp(r->klass, E_DS_MI_CLASS_DONE))
  return(e_ds_mi_stack(s, r));
 return(0);
}

int e_ds_take_mi_record(E_D_SESSION *s, const E_GDBMI_RECORD *r)
{
 switch (r->kind)
 {
  case E_GDBMI_K_EXEC:
   if (!strcmp(r->klass, E_DS_MI_CLASS_STOPPED))
    return(e_ds_mi_stopped(s, r));
   if (!strcmp(r->klass, E_DS_MI_CLASS_RUNNING))
    return(e_ds_mi_running(s));
   return(0);
  case E_GDBMI_K_RESULT:
   return(e_ds_mi_result(s, r));
  case E_GDBMI_K_CONSOLE:
  case E_GDBMI_K_TARGET:
  case E_GDBMI_K_LOG:
   e_ds_console_add(s, e_gdbmi_stream_text(r));
   return(1);
  default:
   return(0);
 }
}

/**
 * e_ds_mi_brk_pending_words - The status row's words for a breakpoint gdb
 * holds pending: what it waits for.
 */
static void e_ds_mi_brk_pending_words(char *out, size_t n,
                                      const char *spec)
{
 snprintf(out, n, "%s%s", E_DS_MI_PENDING_WORDS, spec);
}

/**
 * e_ds_mi_brk_refused - gdb's error answer to a breakpoint set.
 */
static void e_ds_mi_brk_refused(E_D_SESSION *s, int i, const E_GDBMI_RECORD *r)
{
 char words[E_D_BRK_MSG_MAX];
 const char *msg = e_gdbmi_get(r, "msg");

 if (!msg || !msg[0])
  msg = E_DS_MI_REFUSED_WORDS;
 e_ds_strip_style(words, sizeof words, msg);
 e_ds_brk_put(s, i, E_D_BRK_REFUSED, 0, words);
}

/**
 * e_ds_mi_brk_accepted - gdb's answer to a breakpoint set: its number, held
 * pending when the place is not known yet.
 */
static void e_ds_mi_brk_accepted(E_D_SESSION *s, int i, const E_GDBMI_RECORD *r)
{
 char words[E_D_BRK_MSG_MAX];
 const char *pending = e_gdbmi_get(r, "bkpt.pending");
 int id = e_gdbmi_number(e_gdbmi_get(r, "bkpt.number"), E_DS_MI_DECIMAL, 0);

 if (!pending)
 {
  e_ds_brk_put(s, i, E_D_BRK_VERIFIED, id, NULL);
  return;
 }
 e_ds_mi_brk_pending_words(words, sizeof words, pending);
 e_ds_brk_put(s, i, E_D_BRK_PENDING, id, words);
}

int e_ds_mi_watch_text(const E_GDBMI_RECORD *r, char *out, size_t n)
{
 const char *value;

 if (n > 0)
  out[0] = '\0';
 if (r->kind != E_GDBMI_K_RESULT || strcmp(r->klass, E_DS_MI_CLASS_DONE))
  return(0);
 value = e_gdbmi_get(r, "value");
 if (!value)
  return(0);
 snprintf(out, n, "%s%s", E_DS_MI_WATCH_LEAD, value);
 return(1);
}

/**
 * e_ds_mi_args_of - The arguments of frame @level, as "v=3, w=4", from
 * -stack-list-arguments 1's record.
 * @args: that record, or NULL; @out, @n: the words, "" when none.
 */
static void e_ds_mi_args_of(const E_GDBMI_RECORD *args, int level, char *out,
                            size_t n)
{
 const E_GDBMI_NODE *fr, *arg;
 const char *name, *value;
 size_t have;

 out[0] = '\0';
 if (!args)
  return;
 for (fr = e_gdbmi_first_item(args, e_gdbmi_find(args, "stack-args")); fr;
      fr = e_gdbmi_next_item(args, fr))
 {
  if (e_gdbmi_number(e_gdbmi_get_in(args, fr, "level"), E_DS_MI_DECIMAL,
                     E_DS_NOT_FOUND) != level)
   continue;
  for (arg = e_gdbmi_first_item(args, e_gdbmi_find_in(args, fr, "args"));
       arg; arg = e_gdbmi_next_item(args, arg))
  {
   name = e_gdbmi_get_in(args, arg, "name");
   value = e_gdbmi_get_in(args, arg, "value");
   have = strlen(out);
   if (!name || have + 1 >= n)
    continue;
   snprintf(out + have, n - have, "%s%s=%s", have ? E_DS_MI_ARG_SEP : "",
            name, value ? value : "");
  }
  return;
 }
}

/**
 * e_ds_mi_addr_of - The "0x... in " a row of gdb's "bt" starts with: for a
 * caller's frame, whose place is never the start of a line, and for a
 * frame with no source; "" for the innermost frame of the user's code.
 * @out, @n: the words.
 */
static void e_ds_mi_addr_of(const E_GDBMI_RECORD *frames,
                            const E_GDBMI_NODE *fn, int level, int has_line,
                            char *out, size_t n)
{
 const char *addr = e_gdbmi_get_in(frames, fn, "addr");

 out[0] = '\0';
 if (addr && (level > 0 || !has_line))
  snprintf(out, n, E_DS_MI_ADDR_FMT, addr);
}

/**
 * e_ds_mi_row_of - One row of the Stack window for frame tuple @fn.
 * @out, @n: the row with its line end.
 * Return: the bytes the row takes, as snprintf counts them.
 */
static int e_ds_mi_row_of(const E_GDBMI_RECORD *frames,
                          const E_GDBMI_NODE *fn, const E_GDBMI_RECORD *args,
                          char *out, size_t n)
{
 char argtext[E_DS_LINE_MAX];
 char addr[E_DS_MI_ADDR_MAX];
 const char *func = e_gdbmi_get_in(frames, fn, "func");
 const char *file = e_gdbmi_get_in(frames, fn, "file");
 const char *line = e_gdbmi_get_in(frames, fn, "line");
 const char *from = e_gdbmi_get_in(frames, fn, "from");
 int level = e_gdbmi_number(e_gdbmi_get_in(frames, fn, "level"),
                            E_DS_MI_DECIMAL, 0);

 e_ds_mi_args_of(args, level, argtext, sizeof argtext);
 e_ds_mi_addr_of(frames, fn, level, file && line, addr, sizeof addr);
 if (!func)
  func = E_DS_MI_NO_FUNC;
 if (file && line)
  return(snprintf(out, n, E_DS_MI_ROW_AT_FMT, level, addr, func, argtext,
                  file, line));
 if (from)
  return(snprintf(out, n, E_DS_MI_ROW_FROM_FMT, level, addr, func, argtext,
                  from));
 return(snprintf(out, n, E_DS_MI_ROW_FMT, level, addr, func, argtext));
}

int e_ds_mi_where_rows(const E_GDBMI_RECORD *frames,
                       const E_GDBMI_RECORD *args, char *out, size_t n)
{
 const E_GDBMI_NODE *fn;
 size_t have = 0;
 int rows = 0, len;

 if (n > 0)
  out[0] = '\0';
 for (fn = e_gdbmi_first_item(frames, e_gdbmi_find(frames, "stack")); fn;
      fn = e_gdbmi_next_item(frames, fn))
 {
  len = e_ds_mi_row_of(frames, fn, args, out + have, n - have);
  if (len < 0 || have + (size_t)len >= n)
  {
   out[have] = '\0';      /* only whole rows are kept */
   break;
  }
  have += (size_t)len;
  rows++;
 }
 return(rows);
}

int e_ds_take_mi_brk_result(E_D_SESSION *s, int i, const E_GDBMI_RECORD *r)
{
 if (i < 0 || i >= E_DS_BRKS_MAX || r->kind != E_GDBMI_K_RESULT)
  return(0);
 if (!strcmp(r->klass, E_DS_MI_CLASS_ERROR))
 {
  e_ds_mi_brk_refused(s, i, r);
  return(1);
 }
 if (!strcmp(r->klass, E_DS_MI_CLASS_DONE) && e_gdbmi_find(r, "bkpt"))
 {
  e_ds_mi_brk_accepted(s, i, r);
  return(1);
 }
 return(0);
}
