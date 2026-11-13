/* test_splice.c -- unit test for the buffer owner (we_buffer.c).
 * Links we_buffer.c only; the editor's side of the seam is played by the
 * recorders below.  Always runs (no skip).
 *
 * What the user would see go wrong if a rule broke: a block mark left on
 * the wrong line or past the end of the file, a breakpoint left under other
 * code or lost for good after Undo, Undo giving back different text or
 * needing two keys for one action, a window that reads as saved after Undo
 * changed it, colours of the old text after a change, a change half made
 * when memory ran out, or a view (Messages, Help) collecting undo.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "../we_buffer.h"

#define WIDTH          80
#define NUMUNDO        10
#define MANY_APPENDS   10000
#define MAX_ROWS       16
#define STATE_PLAIN    0
#define STATE_COMMENT  5

static int fails;

static void chk(const char *what, int got, int want)
{
 if (got != want) {
  printf("FAIL: %s: got %d want %d\n", what, got, want);
  fails++;
 }
}

static void chk_str(const char *what, const char *got, const char *want)
{
 if (strcmp(got, want) != 0) {
  printf("FAIL: %s: got \"%s\" want \"%s\"\n", what, got, want);
  fails++;
 }
}

/* ---- the editor's side of the seam, as recorders ---- */

static int syntax_calls, syntax_y;
static int moved_calls, moved_y, moved_del, moved_ins;
static int moved_removed[MAX_ROWS], moved_nremoved, moved_added[MAX_ROWS], moved_nadded;
static int nomem_calls;
static int leak_traced;
static int fault_traced;
static int overrun_traced;
static int move_view_traced;
static int row_wider_traced;
static int block_in_step_traced;
static int rows_past_traced;     /* an action named rows the text does not have */
static int inside_traced;        /* a change named a place inside a character */
static int shell_record_traced;  /* a record kept on a shell */
static int user_record_traced;   /* a record kept on the user's list */

void wpe_buffer_syntax_changed(BUFFER *b, int y)
{
 (void) b;
 syntax_calls++;
 syntax_y = y;
}

void wpe_buffer_lines_moved(BUFFER *b, int y, int n_del, int n_ins,
                            const int *rows_removed, int n_removed,
                            const int *rows_added, int n_added)
{
 int i;

 if (b->f == NULL || b->f->datnam == NULL)   /* as the editor's: no file, no list */
  return;
 moved_calls++;
 moved_y = y;
 moved_del = n_del;
 moved_ins = n_ins;
 moved_nremoved = n_removed;
 moved_nadded = n_added;
 for (i = 0; i < n_removed && i < MAX_ROWS; i++)
  moved_removed[i] = rows_removed[i];
 for (i = 0; i < n_added && i < MAX_ROWS; i++)
  moved_added[i] = rows_added[i];
}

/* As the editor's: a refusal inside a shell counts there and says nothing. */
void wpe_buffer_say_nomem(BUFFER *b, const char *what)
{
 (void) what;
 if (e_buffer_is_shell(b)) {
  e_buffer_shell_refused(b);
  return;
 }
 nomem_calls++;
}

static int shape_calls;          /* refusals said for the shape of the text */

/* As the editor's: a refusal inside a shell counts there and says nothing. */
void wpe_buffer_say_shape(BUFFER *b, const char *what)
{
 (void) what;
 if (e_buffer_is_shell(b)) {
  e_buffer_shell_refused(b);
  return;
 }
 shape_calls++;
}

static int read_only_calls;

/* As the editor's: the status row says a read-only window kept its text. */
void wpe_buffer_say_read_only(BUFFER *b)
{
 (void) b;
 read_only_calls++;
}

static int replay_calls;        /* character records the editor's side replayed */
static int replay_row_missing;  /* ... handed a row the text does not have */
static int block_calls;         /* block records the editor's side replayed */
static int stub_marks_past;     /* the replay sets the block past the text, as
                                   a record of a longer text would */

/* The block a replay sets past the end of the text it works on. */
static void stub_marks_past_the_text(BUFFER *b, SCHIRM *s)
{
 if (!stub_marks_past || s == NULL)
  return;
 s->mark_begin.x = 0;
 s->mark_begin.y = b->mxlines + 2;
 s->mark_end.x = 7;
 s->mark_end.y = b->mxlines + 3;
}

/* As the editor's block replays: only the block kinds come here, the
   character kinds are replayed on a shell. */
int wpe_buffer_replay_kind(BUFFER *b, SCHIRM *s, Undo *rec)
{
 if (rec->type == 'c' || rec->type == 'v' || rec->type == 'd')
  block_calls++;
 stub_marks_past_the_text(b, s);
 return(E_BUFFER_OK);
}

/* ---- the editor's character primitives on a shell, played by a stub ----

   'a' (the Undo of a typed line break) joins: the line below the record's
   goes, its breakpoint parked in the 'r' inverse, as e_del_nchar does.
   'r' (the Undo of a join) splits the line again, recording an 'a'; an 'r'
   whose text holds no line break puts the text back in its row.  Each
   records its inverse first, as the primitives do.  A replay works on the
   row its record names, so that row must be one of the shell's: counted
   here rather than read, so a row past the text fails the case instead of
   reading a line a splice freed. */

static int stub_refuses;      /* the primitive is refused for memory on the shell */
static int stub_overruns;     /* the join takes the shell's last row: past its span */

static Undo *stub_record(BUFFER *shell, int kind, int x, int y)
{
 Undo *inv = e_buffer_record_new(kind);

 if (inv == NULL)
  return(NULL);
 inv->b.x = x;
 inv->b.y = y;
 inv->a.x = 1;
 if (kind == 'r' && e_buffer_record_text(inv, (const unsigned char *) "\n", 1) != E_BUFFER_OK) {
  e_buffer_record_free(inv);
  return(NULL);
 }
 return(e_buffer_record_push(shell, inv));
}

static void stub_join(BUFFER *shell, int x, int y)
{
 Undo *inv = stub_record(shell, 'r', x, y);
 int gone = y + 1;

 if (inv == NULL) {
  wpe_buffer_say_nomem(shell, "the undo record");
  return;
 }
 if (stub_overruns)
  gone = shell->mxlines - 1;
 e_buffer_cover_begin(shell, inv);
 if (e_buffer_delete_lines(shell, gone, 1, E_BUFFER_STEP_NEW) != E_BUFFER_OK)
  wpe_buffer_say_nomem(shell, "the line deletion");
 e_buffer_cover_end(shell);
}

static void stub_split(BUFFER *shell, int x, int y)
{
 if (stub_record(shell, 'a', x, y) == NULL) {
  wpe_buffer_say_nomem(shell, "the undo record");
  return;
 }
 if (e_buffer_split_line(shell, y, x, E_BUFFER_STEP_COVERED, E_BUFFER_HEAD_WR) != E_BUFFER_OK)
  wpe_buffer_say_nomem(shell, "a line break");
}

/* The bytes of an 'r' record without a line break, back at its place. */
static void stub_put_back(BUFFER *shell, const Undo *rec, int y)
{
 STRING *l = &shell->bf[y];
 Undo *inv = stub_record(shell, 'a', rec->b.x, y);
 int n = rec->a.x;

 if (inv == NULL) {
  wpe_buffer_say_nomem(shell, "the undo record");
  return;
 }
 inv->a.x = n;
 memmove(l->s + rec->b.x + n, l->s + rec->b.x, (size_t)(l->nrc - rec->b.x + 1));
 memcpy(l->s + rec->b.x, rec->u.pt, (size_t) n);
 l->len += n;
 l->nrc += n;
}

/* Does an 'r' record's text end with a line break?  The join records the
   cases build on their own stack keep no text: theirs is the line break. */
static int stub_text_breaks(const Undo *rec)
{
 if (rec->u.pt == NULL)
  return(1);
 return(rec->a.x > 0 && ((const char *) rec->u.pt)[rec->a.x - 1] == '\n');
}

void wpe_buffer_replay_on_shell(BUFFER *shell, SCHIRM *s, const Undo *rec, int y0)
{
 int y = rec->b.y - y0;

 if (stub_refuses) {
  wpe_buffer_say_nomem(shell, "the stub");
  return;
 }
 replay_calls++;
 if (y < 0 || y >= shell->mxlines) {
  replay_row_missing++;
  return;
 }
 if (rec->type == 'a')
  stub_join(shell, rec->b.x, y);
 else if (rec->type == 'r' && stub_text_breaks(rec))
  stub_split(shell, rec->b.x, y);
 else if (rec->type == 'r')
  stub_put_back(shell, rec, y);
 stub_marks_past_the_text(shell, s);
}

void wpe_buffer_trace(const char *fmt, ...)
{
 if (strstr(fmt, "depth leak") != NULL)
  leak_traced++;
 if (strstr(fmt, "buffer fault") != NULL)
  fault_traced++;
 if (strstr(fmt, "span overrun") != NULL)
  overrun_traced++;
 if (strstr(fmt, "view meets a move") != NULL)
  move_view_traced++;
 if (strstr(fmt, "row wider") != NULL)
  row_wider_traced++;
 if (strstr(fmt, "holds a block record") != NULL)
  block_in_step_traced++;
 if (strstr(fmt, "names rows past") != NULL)
  rows_past_traced++;
 if (strstr(fmt, "position inside a character") != NULL)
  inside_traced++;
 if (strstr(fmt, "buffer shell record kind=") != NULL)
  shell_record_traced++;
 if (strstr(fmt, "buffer record kind=") != NULL)
  user_record_traced++;
}

static void hooks_reset(void)
{
 syntax_calls = moved_calls = nomem_calls = 0;
 moved_nremoved = moved_nadded = 0;
 overrun_traced = move_view_traced = row_wider_traced = block_in_step_traced = 0;
 rows_past_traced = shell_record_traced = user_record_traced = read_only_calls = 0;
 inside_traced = 0;
 shape_calls = 0;
 stub_refuses = stub_overruns = 0;
 replay_calls = replay_row_missing = stub_marks_past = block_calls = 0;
}

/* ---- the fixture: a window with a buffer of numbered lines ---- */

static ECNT cn;
static FENSTER win;
static SCHIRM scr;
static BUFFER buf;

static STRING line_of(BUFFER *b, const char *text)
{
 STRING l;

 if (e_buffer_line_new(b, &l) != E_BUFFER_OK)
  exit(2);
 strcpy((char *) l.s, text);
 l.len = (int) strlen(text);
 l.s[l.len] = WPE_WR;
 l.s[l.len + 1] = '\0';
 l.nrc = l.len + 1;
 return(l);
}

/* A window holding lines "L0".."L<n-1>", no breakpoints, no marks, nothing
   to undo, saved. */
static void fixture(int n, int history)
{
 STRING lines[64];
 int i;
 char name[16];

 memset(&cn, 0, sizeof cn);
 memset(&win, 0, sizeof win);
 memset(&scr, 0, sizeof scr);
 cn.numundo = NUMUNDO;
 win.s = &scr;
 win.datnam = "t.c";
 scr.brp = NULL;
 e_buffer_init(&buf, &cn, &win, WIDTH, history);
 win.b = &buf;
 for (i = 0; i < n; i++) {
  snprintf(name, sizeof name, "L%d", i);
  lines[i] = line_of(&buf, name);
 }
 e_buffer_splice(&buf, 0, 1, lines, n, NULL, 0, E_BUFFER_STEP_COVERED);
 e_buffer_history_drop(&buf);
 win.save = 0;
 hooks_reset();
}

static void fixture_free(void)
{
 int i;

 e_buffer_history_drop(&buf);
 for (i = 0; i < buf.mxlines; i++)
  free(buf.bf[i].s);
 free(buf.bf);
 free(scr.brp);
 free(win.c_sw);
 win.c_sw = NULL;
}

static void brp_set(int n, const int *rows)
{
 int i;

 scr.brp = realloc(scr.brp, (size_t)(n + 1) * sizeof(int));
 scr.brp[0] = n;
 for (i = 0; i < n; i++)
  scr.brp[i + 1] = rows[i];
}

static int brp_has(int row)
{
 int i;

 for (i = 1; i <= scr.brp[0]; i++)
  if (scr.brp[i] == row)
   return(1);
 return(0);
}

static const char *text_of(void)
{
 static char out[4096];
 char *t = e_buffer_to_text(&buf);

 snprintf(out, sizeof out, "%s", t ? t : "(null)");
 free(t);
 return(out);
}

static int list_len(Undo *u)
{
 int n = 0;

 for (; u != NULL; u = u->next)
  n++;
 return(n);
}

static POINT pnt(int x, int y)
{
 POINT p;

 p.x = x;
 p.y = y;
 return(p);
}

/* ---- the cases ---- */

static void case_insert_above_a_mark(void)
{
 STRING two[2];
 int row = 8;

 fixture(10, E_BUFFER_HISTORY_FULL);
 scr.mark_begin = pnt(1, 3);
 brp_set(1, &row);
 two[0] = line_of(&buf, "new0");
 two[1] = line_of(&buf, "new1");
 chk("1 insert", e_buffer_splice(&buf, 2, 0, two, 2, NULL, 0, E_BUFFER_STEP_NEW), E_BUFFER_OK);
 chk("1 mark line", scr.mark_begin.y, 5);
 chk("1 mark column", scr.mark_begin.x, 1);
 chk("1 breakpoint row", scr.brp[1], 10);
 chk("1 debugger told once", moved_calls, 1);
 chk("1 told y", moved_y, 2);
 chk("1 told counts", moved_del * 10 + moved_ins, 2);
 chk("1 nothing removed or added", moved_nremoved + moved_nadded, 0);
 fixture_free();
}

static void case_delete_above(void)
{
 int row = 7;

 fixture(10, E_BUFFER_HISTORY_FULL);
 scr.mark_begin = pnt(1, 6);
 brp_set(1, &row);
 chk("2 delete", e_buffer_delete_lines(&buf, 0, 3, E_BUFFER_STEP_NEW), E_BUFFER_OK);
 chk("2 mark line", scr.mark_begin.y, 3);
 chk("2 breakpoint row", scr.brp[1], 4);
 fixture_free();
}

static void case_delete_across_a_mark(void)
{
 fixture(10, E_BUFFER_HISTORY_FULL);
 scr.mark_begin = pnt(1, 4);
 scr.mark_end = pnt(2, 9);
 e_buffer_delete_lines(&buf, 3, 3, E_BUFFER_STEP_NEW);
 chk("3 mark on a removed line goes to the next line", scr.mark_begin.y, 3);
 chk("3 ... at column 0", scr.mark_begin.x, 0);
 e_buffer_delete_lines(&buf, 5, 2, E_BUFFER_STEP_NEW);
 chk("3 mark on the removed last line goes to the line above", scr.mark_end.y, 4);
 chk("3 ... at its end", scr.mark_end.x, buf.bf[4].len);
 fixture_free();
}

static void case_breakpoint_on_a_deleted_line(void)
{
 int row = 5;

 fixture(10, E_BUFFER_HISTORY_FULL);
 brp_set(1, &row);
 e_buffer_delete_lines(&buf, 5, 1, E_BUFFER_STEP_NEW);
 chk("4 the row left the margin", scr.brp[0], 0);
 chk("4 the record holds it", buf.ud->nbrk == 1 && buf.ud->brk[0] == 0, 1);
 chk("4 the debugger was told it went", moved_nremoved == 1 && moved_removed[0] == 5, 1);
 hooks_reset();
 chk("4 undo", e_buffer_undo(&buf, &scr), E_BUFFER_OK);
 chk_str("4 the line is back", (char *) buf.bf[5].s, "L5\n");
 chk("4 the row is back", brp_has(5), 1);
 chk("4 the debugger was told it came", moved_nadded == 1 && moved_added[0] == 5, 1);
 fixture_free();
}

static void case_changed_lines_keep_rows_and_marks(void)
{
 STRING two[2];
 int row = 4;

 fixture(10, E_BUFFER_HISTORY_FULL);
 brp_set(1, &row);
 strcpy((char *) buf.bf[4].s, "a long line\n");
 buf.bf[4].len = 11;
 scr.mark_begin = pnt(9, 4);
 two[0] = line_of(&buf, "x3");
 two[1] = line_of(&buf, "y4");
 e_buffer_splice(&buf, 3, 2, two, 2, NULL, 0, E_BUFFER_STEP_NEW);
 chk("5 the row stays on its changed line", brp_has(4), 1);
 chk("5 the mark stays on its line", scr.mark_begin.y, 4);
 chk("5 its column is clamped to the new line", scr.mark_begin.x, 2);
 fixture_free();
}

static void case_line_record_round_trip(void)
{
 char before[4096], after[4096];
 Undo first;
 int rows = 4;

 fixture(8, E_BUFFER_HISTORY_FULL);
 brp_set(1, &rows);
 snprintf(before, sizeof before, "%s", text_of());
 e_buffer_delete_lines(&buf, 3, 2, E_BUFFER_STEP_NEW);
 snprintf(after, sizeof after, "%s", text_of());
 first = *buf.ud;
 chk("6 one record", list_len(buf.ud) * 10 + list_len(buf.rd), 10);
 e_buffer_undo(&buf, &scr);
 chk_str("6 Undo gives the text back", text_of(), before);
 chk("6 lines", buf.mxlines, 8);
 chk("6 one record on redo", list_len(buf.ud) * 10 + list_len(buf.rd), 1);
 win.save = 0;
 e_buffer_redo(&buf, &scr);
 chk_str("6 Redo makes the change again", text_of(), after);
 chk("6 one record on undo", list_len(buf.ud) * 10 + list_len(buf.rd), 10);
 chk("6 same span", buf.ud->span, first.span);
 chk("6 same counts", buf.ud->n_old * 10 + buf.ud->n_new, first.n_old * 10 + first.n_new);
 chk("6 same breakpoint rows", buf.ud->nbrk == 1 && buf.ud->brk[0] == 1, 1);
 chk("21 a replay counts toward the modified mark", win.save > 0, 1);
 fixture_free();
}

static void case_a_step_is_one_undo(void)
{
 char before[4096], after[4096];
 Undo *u;
 int more = 0, n = 0;

 fixture(8, E_BUFFER_HISTORY_FULL);
 snprintf(before, sizeof before, "%s", text_of());
 e_buffer_step_begin(&buf);
 e_buffer_delete_lines(&buf, 1, 1, E_BUFFER_STEP_NEW);
 e_buffer_insert_blank(&buf, 4, E_BUFFER_STEP_NEW);
 e_buffer_delete_lines(&buf, 6, 1, E_BUFFER_STEP_NEW);
 e_buffer_step_end(&buf);
 snprintf(after, sizeof after, "%s", text_of());
 e_buffer_undo(&buf, &scr);
 chk_str("7 one Undo takes back the step", text_of(), before);
 chk("7 nothing left to undo", buf.ud == NULL, 1);
 for (u = buf.rd; u != NULL; u = u->next, n++)
  more += (u->flags & E_UNDO_MORE) != 0;
 chk("7 three inverses", n, 3);
 chk("7 MORE on all but the first pushed", more, 2);
 chk("7 the first pushed lacks MORE", (buf.rd->next->next->flags & E_UNDO_MORE) == 0, 1);
 e_buffer_redo(&buf, &scr);
 chk_str("7 one Redo gives the step back", text_of(), after);
 chk("7 nothing left to redo", buf.rd == NULL, 1);
 fixture_free();
}

static void case_covered_deletion_and_the_pocket(void)
{
 Undo *rec;
 int row = 5;

 fixture(8, E_BUFFER_HISTORY_FULL);
 brp_set(1, &row);
 rec = e_buffer_record_push(&buf, e_buffer_record_new('r'));
 rec->b = pnt(3, 4);
 rec->u.pt = malloc(1);
 e_buffer_cover_begin(&buf, rec);
 e_buffer_cover_begin(&buf, NULL);          /* a nested cover keeps the pocket */
 e_buffer_delete_lines(&buf, 5, 1, E_BUFFER_STEP_NEW);
 e_buffer_cover_end(&buf);
 e_buffer_cover_end(&buf);
 chk("8 nothing recorded", list_len(buf.ud), 1);
 chk("8 the pocket holds the row at its offset", rec->nbrk == 1 && rec->brk[0] == 1, 1);
 fixture_free();

 fixture(8, E_BUFFER_HISTORY_FULL);
 brp_set(1, &row);
 e_buffer_cover_begin(&buf, NULL);
 e_buffer_delete_lines(&buf, 5, 1, E_BUFFER_STEP_NEW);
 e_buffer_cover_end(&buf);
 chk("8 no pocket: no record", list_len(buf.ud), 0);
 chk("8 no pocket: the row is gone", scr.brp[0], 0);
 chk("8 no pocket: the debugger was told", moved_nremoved == 1 && moved_removed[0] == 5, 1);
 fixture_free();
}

static void case_detach_then_splice(void)
{
 STRING out[3];
 int rows[2] = {2, 3}, got[3], ngot = 0, i;

 fixture(8, E_BUFFER_HISTORY_FULL);
 brp_set(2, rows);
 chk("9 detach", e_buffer_detach_lines(&buf, 2, 2, out, got, &ngot), E_BUFFER_OK);
 chk("9 nothing recorded", list_len(buf.ud), 0);
 chk("9 the rows came out", ngot == 2 && got[0] == 0 && got[1] == 1, 1);
 chk("9 the margin has none", scr.brp[0], 0);
 chk_str("9 the lines are in hand", (char *) out[0].s, "L2\n");
 hooks_reset();
 chk("9 splice them back lower", e_buffer_splice(&buf, 4, 0, out, 2, got, ngot,
                                                  E_BUFFER_STEP_COVERED), E_BUFFER_OK);
 chk("9 the rows arrived", brp_has(4) && brp_has(5), 1);
 chk("9 the debugger was told", moved_nadded, 2);
 for (i = 0; i < buf.mxlines; i++)
  chk("9 every line is still one allocation", buf.bf[i].s != NULL, 1);
 chk_str("9 the order", text_of(), "L0\nL1\nL4\nL5\nL2\nL3\nL6\nL7\n");
 fixture_free();
}

static void case_replace_all_changes_only_the_middle(void)
{
 int rows[2] = {1, 6};

 fixture(8, E_BUFFER_HISTORY_FULL);
 brp_set(2, rows);
 scr.mark_begin = pnt(1, 0);
 scr.mark_end = pnt(1, 7);
 chk("10 replace", e_buffer_replace_all(&buf, "L0\nL1\nL2\nnew\nL5\nL6\nL7\n",
                                         E_BUFFER_STEP_NEW), E_BUFFER_OK);
 chk("10 the debugger told once", moved_calls, 1);
 chk("10 at the middle", moved_y, 3);
 chk("10 rows outside kept", brp_has(1), 1);
 chk("10 rows below moved by one", brp_has(5), 1);
 chk("10 the mark below moved", scr.mark_end.y, 6);
 chk("10 one record", list_len(buf.ud), 1);
 hooks_reset();
 chk("10 same text", e_buffer_replace_all(&buf, "L0\nL1\nL2\nnew\nL5\nL6\nL7\n",
                                           E_BUFFER_STEP_NEW), E_BUFFER_OK);
 chk("10 same text records nothing", list_len(buf.ud), 1);
 chk("10 same text tells nobody", moved_calls, 0);
 fixture_free();
}

/* The buffer holds @text, saved, with nothing to undo. */
static void load_text(const char *text)
{
 e_buffer_replace_all(&buf, text, E_BUFFER_STEP_COVERED);
 e_buffer_history_drop(&buf);
 win.save = 0;
 hooks_reset();
}

static void case_a_format_is_cut_into_hunks(void)
{
 static const char old_text[] = "int f()\n{\n\ta = 1;\n\tfoo(a);\n\tbar(a);\n\tbaz(a);\n\treturn;\n}\n";
 static const char new_text[] =
  "int f()\n{\n\n    a = 1;\n    foo(a);\n    bar(a);\n    baz(a);\n    return;\n}\n";
 int rows[2] = {3, 5};

 fixture(1, E_BUFFER_HISTORY_FULL);
 load_text(old_text);
 brp_set(2, rows);
 scr.mark_begin = pnt(1, 4);
 chk("11 format", e_buffer_replace_all(&buf, new_text, E_BUFFER_STEP_NEW), E_BUFFER_OK);
 chk_str("11 the new text", text_of(), new_text);
 chk("11 two hunks", moved_calls, 2);
 chk("11 one record per hunk", list_len(buf.ud), 2);
 chk("11 the row below the insertion moved", brp_has(4), 1);
 chk("11 the other row moved", brp_has(6), 1);
 chk("11 no row left on the old lines", brp_has(3) + brp_has(5), 0);
 chk("11 the mark followed its line", scr.mark_begin.y, 5);
 chk("11 the dirty mark counts both hunks", win.save, E_BUFFER_SAVE_PER_LINE * (1 + 10));
 chk("11 one Undo takes both back", e_buffer_undo(&buf, &scr), E_BUFFER_OK);
 chk_str("11 the old text", text_of(), old_text);
 chk("11 the rows are back", brp_has(3) && brp_has(5) && scr.brp[0] == 2, 1);
 chk("11 one Redo gives both", e_buffer_redo(&buf, &scr), E_BUFFER_OK);
 chk_str("11 the new text again", text_of(), new_text);
 chk("11 the rows follow again", brp_has(4) && brp_has(6) && scr.brp[0] == 2, 1);
 fixture_free();
}

/* Memory refused at any point of a swap of several hunks: either the swap
   is complete, or nothing changed (text, breakpoints, dirty mark, undo); no
   line is lost or freed twice (the sanitizer build proves the second). */
static void case_a_refused_swap_changes_nothing(void)
{
 static const char old_text[] = "int f()\n{\n\ta = 1;\n\tfoo(a);\n\tbar(a);\n\treturn;\n}\n";
 static const char new_text[] = "int f()\n{\n\n    a = 1;\n    foo(a);\n    bar(a);\n    return;\n}\n";
 int rows[2] = {3, 6};
 int n, r, bad = 0, refused = 0, whole = 0;

 for (n = 1; n < 80; n++) {
  fixture(1, E_BUFFER_HISTORY_FULL);
  load_text(old_text);
  brp_set(2, rows);
  e_buffer_test_fail_allocs(n);
  r = e_buffer_replace_all(&buf, new_text, E_BUFFER_STEP_NEW);
  e_buffer_test_fail_allocs(0);
  if (r == E_BUFFER_OK) {
   whole++;
   if (strcmp(text_of(), new_text) != 0 || !brp_has(4) || !brp_has(7))
    bad++;
  }
  if (r == E_BUFFER_NOMEM) {
   refused++;
   if (strcmp(text_of(), old_text) != 0 || buf.ud != NULL || win.save != 0 ||
       !brp_has(3) || !brp_has(6) || scr.brp[0] != 2 || moved_calls != 0)
    bad++;
  }
  fixture_free();
 }
 chk("11 a refused swap is whole or changes nothing", bad, 0);
 chk("11 some allocation was refused", refused > 0, 1);
 chk("11 and some swap went through", whole > 0, 1);
}

/* A swap made inside a block operation's cover: every covered splice moves
   the rows below it, so the swap is cut into its two hunks (a line put in
   above, a line taken out), each measured where it stands, and the
   breakpoint stays on its line; nothing is recorded under the cover. */
static void case_a_swap_inside_a_block_operation(void)
{
 int rows[1] = {4};
 int r;

 fixture(6, E_BUFFER_HISTORY_FULL);
 brp_set(1, rows);
 e_buffer_cover_begin(&buf, NULL);
 r = e_buffer_replace_all(&buf, "X\nL0\nL1\nL2\nL4\nL5\n", E_BUFFER_STEP_NEW);
 e_buffer_cover_end(&buf);
 chk("11 a swap inside a block operation", r, E_BUFFER_OK);
 chk_str("11 its text", text_of(), "X\nL0\nL1\nL2\nL4\nL5\n");
 chk("11 two hunks", moved_calls, 2);
 chk("11 nothing recorded under the cover", list_len(buf.ud), 0);
 chk("11 the breakpoint stays on L4", brp_has(4) && scr.brp[0] == 1, 1);
 fixture_free();
}

/* A swap of more lines than the diff will search for is one hunk, never a
   refusal and never a different text. */
static void case_a_swap_beyond_the_bound_is_one_hunk(void)
{
 enum { LINES = E_BUFFER_DIFF_MAX / 2 + 10 };
 char *old_text = malloc((size_t) LINES * 12), *new_text = malloc((size_t) LINES * 12);
 size_t po = 0, pn = 0;
 char last[32];
 int i;

 for (i = 0; i < LINES; i++) {
  po += (size_t) sprintf(old_text + po, "old%d\n", i);
  pn += (size_t) sprintf(new_text + pn, "new%d\n", i);
 }
 fixture(1, E_BUFFER_HISTORY_FULL);
 load_text(old_text);
 chk("11 big swap", e_buffer_replace_all(&buf, new_text, E_BUFFER_STEP_NEW), E_BUFFER_OK);
 chk("11 big swap: every line is the new one", buf.mxlines, LINES);
 chk_str("11 big swap: first line", (char *) buf.bf[0].s, "new0\n");
 snprintf(last, sizeof last, "new%d\n", LINES - 1);
 chk_str("11 big swap: last line", (char *) buf.bf[LINES - 1].s, last);
 chk("11 big swap: one hunk", moved_calls, 1);
 chk("11 big swap: one record", list_len(buf.ud), 1);
 fixture_free();
 free(old_text);
 free(new_text);
}

static unsigned fuzz_state = 12345u;

static unsigned fuzz_next(unsigned bound)
{
 fuzz_state = fuzz_state * 1103515245u + 12345u;
 return((fuzz_state >> 16) % bound);
}

/* Random text of a few lines from a small stock, the kind a formatter
   shuffles: repeated lines, the same line with other blanks, empty lines. */
static void fuzz_text(char *out, size_t size)
{
 static const char *const stock[] = {"a", "b", "c", "  a", "\tb", "", "x y", "x  y", "{", "}"};
 unsigned lines = 1 + fuzz_next(24), i;
 size_t used = 0;

 for (i = 0; i < lines && used + 16 < size; i++)
  used += (size_t) snprintf(out + used, size - used, "%s\n", stock[fuzz_next(10)]);
}

/* Whatever two texts are, the swap gives the new one, one Undo gives the
   old one back and one Redo the new one again. */
static void case_the_swap_is_exact_for_any_texts(void)
{
 char old_text[1024], new_text[1024], before[1024];
 int round, bad = 0;

 for (round = 0; round < 400; round++) {
  fuzz_text(old_text, sizeof old_text);
  fuzz_text(new_text, sizeof new_text);
  fixture(1, E_BUFFER_HISTORY_FULL);
  load_text(old_text);
  snprintf(before, sizeof before, "%s", text_of());
  if (e_buffer_replace_all(&buf, new_text, E_BUFFER_STEP_NEW) != E_BUFFER_OK)
   bad++;
  if (strcmp(text_of(), new_text) != 0)
   bad++;
  if (e_buffer_undo(&buf, &scr) != E_BUFFER_OK && strcmp(before, new_text) != 0)
   bad++;
  if (strcmp(text_of(), before) != 0)
   bad++;
  if (buf.ud != NULL)
   bad++;
  if (list_len(buf.rd) > 0 && e_buffer_redo(&buf, &scr) != E_BUFFER_OK)
   bad++;
  if (strcmp(text_of(), new_text) != 0)
   bad++;
  fixture_free();
 }
 chk("11 400 random swaps, undone and redone, are exact", bad, 0);
}

static void case_syntax_states_move(void)
{
 static const int want[6] = {STATE_PLAIN, STATE_COMMENT, STATE_COMMENT, STATE_COMMENT,
                             STATE_COMMENT, STATE_PLAIN};
 int i;

 fixture(4, E_BUFFER_HISTORY_FULL);
 win.c_sw = calloc((size_t) buf.mx.y, sizeof(int));
 win.c_sw[0] = STATE_PLAIN;
 win.c_sw[1] = STATE_COMMENT;
 win.c_sw[2] = STATE_COMMENT;
 win.c_sw[3] = STATE_PLAIN;
 e_buffer_insert_blank(&buf, 1, E_BUFFER_STEP_NEW);
 e_buffer_insert_blank(&buf, 1, E_BUFFER_STEP_NEW);
 for (i = 0; i < 6; i++)
  chk("12 state moved with its line, new lines start as the old span did",
      win.c_sw[i], want[i]);
 chk("12 recomputed after each change", syntax_calls, 2);
 syntax_calls = 0;
 e_buffer_cover_begin(&buf, NULL);
 e_buffer_insert_blank(&buf, 3, E_BUFFER_STEP_NEW);
 e_buffer_insert_blank(&buf, 2, E_BUFFER_STEP_NEW);
 chk("12 not per splice inside a cover", syntax_calls, 0);
 e_buffer_cover_end(&buf);
 chk("12 once at depth 0", syntax_calls, 1);
 chk("12 from the lowest line touched", syntax_y, 2);
 fixture_free();
}

static void case_a_view(void)
{
 int row = 3;

 fixture(6, E_BUFFER_HISTORY_FULL);
 brp_set(1, &row);
 e_buffer_mark_view(&buf);
 scr.mark_end = pnt(1, 5);
 e_buffer_delete_lines(&buf, 0, 3, E_BUFFER_STEP_NEW);
 e_buffer_set_line(&buf, 0, "x", 1);
 chk("13 no record", list_len(buf.ud), 0);
 chk("13 no modified mark", win.save, 0);
 chk("13 the margin untouched", scr.brp[0] == 1 && scr.brp[1] == 3, 1);
 chk("13 the mark still moves", scr.mark_end.y, 2);
 chk("13 the debugger is not told", moved_calls, 0);
 fixture_free();
}

static void case_no_window(void)
{
 fixture(5, E_BUFFER_HISTORY_FULL);
 buf.f = NULL;
 scr.mark_begin = pnt(0, 4);
 chk("14 splice", e_buffer_delete_lines(&buf, 0, 2, E_BUFFER_STEP_NEW), E_BUFFER_OK);
 chk("14 no screen touched", scr.mark_begin.y, 4);
 chk("14 no debugger told", moved_calls, 0);
 chk("14 lines", buf.mxlines, 3);
 buf.f = &win;
 fixture_free();
}

static void case_no_memory_changes_nothing(void)
{
 char before[4096];
 STRING two[2];
 int rows[1] = {2}, k, save, n, r;
 POINT mark;

 for (k = 1; k <= 5; k++) {
  fixture(MAXLINES - 2, E_BUFFER_HISTORY_FULL);   /* a full table: the next splice grows */
  brp_set(1, rows);
  win.c_sw = calloc((size_t) buf.mx.y, sizeof(int));
  scr.mark_begin = mark = pnt(1, 5);
  e_buffer_delete_lines(&buf, 0, 1, E_BUFFER_STEP_NEW);
  e_buffer_insert_blank(&buf, 0, E_BUFFER_STEP_NEW);
  snprintf(before, sizeof before, "%s", text_of());
  save = win.save;
  n = list_len(buf.ud);
  mark = scr.mark_begin;
  two[0] = line_of(&buf, "more0");
  two[1] = line_of(&buf, "more1");
  e_buffer_test_fail_allocs(k);
  r = e_buffer_splice(&buf, 1, 1, two, 2, NULL, 0, E_BUFFER_STEP_NEW);
  e_buffer_test_fail_allocs(0);
  if (r == E_BUFFER_OK) {
   chk("15 the splice takes four allocations", k, 5);
   fixture_free();
   continue;
  }
  chk("15 refused", r, E_BUFFER_NOMEM);
  free(two[0].s);
  free(two[1].s);
  chk_str("15 text unchanged", text_of(), before);
  chk("15 marks unchanged", scr.mark_begin.y * 100 + scr.mark_begin.x, mark.y * 100 + mark.x);
  chk("15 margin unchanged", scr.brp[0] == 1 && scr.brp[1] == 2, 1);
  chk("15 lists unchanged", list_len(buf.ud) * 10 + list_len(buf.rd), n * 10);
  chk("15 modified mark unchanged", win.save, save);
  fixture_free();
 }
 fixture(3, E_BUFFER_HISTORY_FULL);
 e_buffer_delete_lines(&buf, 1, 1, E_BUFFER_STEP_NEW);
 e_buffer_test_fail_allocs(1);
 chk("15 a refused Undo", e_buffer_undo(&buf, &scr), E_BUFFER_NOMEM);
 e_buffer_test_fail_allocs(0);
 chk("15 keeps its record", list_len(buf.ud), 1);
 chk("15 and says so", nomem_calls, 1);
 chk("15 a later Undo works", e_buffer_undo(&buf, &scr), E_BUFFER_OK);
 chk_str("15 the line is back", text_of(), "L0\nL1\nL2\n");
 fixture_free();
}

static void case_out_of_range(void)
{
 fixture(4, E_BUFFER_HISTORY_FULL);
 chk("16 past the end", e_buffer_splice(&buf, 5, 0, NULL, 0, NULL, 0, E_BUFFER_STEP_NEW),
     E_BUFFER_RANGE);
 chk("16 negative count", e_buffer_splice(&buf, 0, -1, NULL, 0, NULL, 0, E_BUFFER_STEP_NEW),
     E_BUFFER_RANGE);
 chk("16 nothing changed", buf.mxlines * 10 + list_len(buf.ud), 40);
 fixture_free();
}

static void case_zero_splice(void)
{
 fixture(4, E_BUFFER_HISTORY_FULL);
 chk("17 zero", e_buffer_splice(&buf, 2, 0, NULL, 0, NULL, 0, E_BUFFER_STEP_NEW), E_BUFFER_OK);
 chk("17 no record, no hook", list_len(buf.ud) + moved_calls + syntax_calls, 0);
 fixture_free();
}

static void case_emptied_buffer(void)
{
 int row = 0;

 fixture(1, E_BUFFER_HISTORY_FULL);
 brp_set(1, &row);
 e_buffer_delete_lines(&buf, 0, 1, E_BUFFER_STEP_NEW);
 chk("18 one empty line", buf.mxlines == 1 && buf.bf[0].len == 0, 1);
 chk("18 its breakpoint went", scr.brp[0], 0);
 e_buffer_undo(&buf, &scr);
 chk_str("18 Undo gives back exactly the line", text_of(), "L0\n");
 chk("18 with its breakpoint", brp_has(0), 1);
 e_buffer_redo(&buf, &scr);
 chk("18 Redo empties it again, no extra blank", buf.mxlines == 1 && buf.bf[0].len == 0, 1);
 chk("18 and the breakpoint goes again", scr.brp[0], 0);
 fixture_free();
}

/* Undo of a buffer emptied, refused at each of its allocations in turn:
   the cursor stays where the user had it, with the text and both lists. */
static void case_refused_undo_keeps_the_cursor(void)
{
 int k, r = E_BUFFER_NOMEM;

 for (k = 1; k <= 8 && r == E_BUFFER_NOMEM; k++) {
  fixture(1, E_BUFFER_HISTORY_FULL);
  buf.b = pnt(2, 0);
  e_buffer_delete_lines(&buf, 0, 1, E_BUFFER_STEP_NEW);
  buf.b = pnt(0, 0);
  e_buffer_test_fail_allocs(k);
  r = e_buffer_undo(&buf, &scr);
  e_buffer_test_fail_allocs(0);
  if (r == E_BUFFER_NOMEM) {
   chk("22 a refused Undo leaves the cursor", buf.b.y * 100 + buf.b.x, 0);
   chk_str("22 and the text", text_of(), "\n");
   chk("22 and the lists", list_len(buf.ud) * 10 + list_len(buf.rd), 10);
  }
  fixture_free();
 }
 chk("22 an Undo with memory works", r, E_BUFFER_OK);
 chk("22 after at least one refusal", k > 2, 1);
}

/* Undo of a covered deletion whose lines held a breakpoint, refused at each
   of its allocations in turn: the Undo is refused whole, with the breakpoint
   still parked in the record, or it succeeds and the breakpoint is back on
   its line.  Never an Undo that succeeds and loses the breakpoint. */
/* What the Undo of case 24 takes, measured: the shell (6), the fill over
   the record's row, the line below and a guard on each side (the lines
   array and four lines), the split on the shell (the 'a' inverse, the new
   line and its splice), the rewrite item (the rows the replay keeps, the
   transaction's copy of them, its removed and added rows) and the room on
   the window's margin. */
#define POCKET_UNDO_ALLOCATIONS 19

static void case_a_refused_undo_keeps_a_pocketed_breakpoint(void)
{
 int k, r = E_BUFFER_NOMEM, row = 5;
 Undo *rec;

 for (k = 1; k <= POCKET_UNDO_ALLOCATIONS + 1 && r == E_BUFFER_NOMEM; k++) {
  fixture(8, E_BUFFER_HISTORY_FULL);
  brp_set(1, &row);
  rec = e_buffer_record_push(&buf, e_buffer_record_new('r'));
  rec->b = pnt(2, 4);
  rec->a.x = 1;
  if (e_buffer_record_text(rec, (const unsigned char *) "\n", 1) != E_BUFFER_OK)
   exit(2);
  e_buffer_cover_begin(&buf, rec);
  e_buffer_delete_lines(&buf, 5, 1, E_BUFFER_STEP_NEW);
  e_buffer_cover_end(&buf);
  e_buffer_test_fail_allocs(k);
  r = e_buffer_undo(&buf, &scr);
  e_buffer_test_fail_allocs(0);
  if (r == E_BUFFER_NOMEM) {
   chk("24 a refused Undo keeps its record", list_len(buf.ud), 1);
   chk("24 and the breakpoint stays parked", scr.brp[0] == 0 && rec->nbrk == 1, 1);
  } else {
   chk("24 an Undo that succeeds brings the breakpoint back", brp_has(5), 1);
  }
  fixture_free();
 }
 chk("24 an Undo with memory works", r, E_BUFFER_OK);
 chk("24 after at least one refusal", k > 2, 1);
 chk("24 the Undo takes this many allocations", k - 2, POCKET_UNDO_ALLOCATIONS);
}

/* A row of a view may be wider than the buffer and is kept whole, with the
   line break it had; a row set as the open end of a wrapped line has none. */
static void case_a_view_row_may_exceed_the_width(void)
{
 char wide[2 * WIDTH + 1];

 memset(wide, 'x', sizeof wide - 1);
 wide[sizeof wide - 1] = '\0';
 fixture(3, E_BUFFER_HISTORY_FULL);
 e_buffer_mark_view(&buf);
 chk("25 a wide row is taken", e_buffer_set_line(&buf, 0, wide, 2 * WIDTH), E_BUFFER_OK);
 chk("25 whole", buf.bf[0].len, 2 * WIDTH);
 chk("25 bytes kept", memcmp(buf.bf[0].s, wide, 2 * WIDTH), 0);
 chk("25 with its line break", buf.bf[0].s[2 * WIDTH], WPE_WR);
 chk("25 the width is the buffer's still", buf.mx.x, WIDTH);
 chk("25 an open end is taken",
     e_buffer_set_line_ended(&buf, 1, "ab", 2, '\0'), E_BUFFER_OK);
 chk("25 open: its length", buf.bf[1].len, 2);
 chk("25 open: no line break", buf.bf[1].s[2], '\0');
 chk("25 open: counted without one", buf.bf[1].nrc, 2);
 chk("25 a closed end is taken",
     e_buffer_set_line_ended(&buf, 1, "ab", 2, WPE_WR), E_BUFFER_OK);
 chk("25 closed: counted with one", buf.bf[1].nrc, 3);
 chk("25 a view records nothing", list_len(buf.ud), 0);
 fixture_free();
}

/* The user's number of undo steps counts the actions before the newest,
   as the original did: a setting of NUMUNDO undoes NUMUNDO + 1 actions. */
static void case_the_undo_bound(void)
{
 int i;

 fixture(30, E_BUFFER_HISTORY_FULL);
 for (i = 0; i < NUMUNDO + 3; i++)
  e_buffer_delete_lines(&buf, 0, 1, E_BUFFER_STEP_NEW);
 chk("23 actions Ctrl-U can take back", list_len(buf.ud), NUMUNDO + 1);
 fixture_free();
}

static void case_append_text(void)
{
 fixture(1, E_BUFFER_HISTORY_FULL);
 e_buffer_mark_view(&buf);
 e_buffer_append_text(&buf, "a\n\nb\n", 1);
 chk_str("19 blank lines dropped", text_of(), "L0\na\nb\n");
 e_buffer_append_text(&buf, "c\n\nd\n", 0);
 chk_str("19 blank lines kept", text_of(), "L0\na\nb\nc\n\nd\n");
 fixture_free();
}

/* A compiler's line wider than the window stays one row, so the error walk
   reads its whole path; memory refused at any allocation changes nothing. */
static void case_append_rows(void)
{
 char wide[3 * WIDTH + 1], text[3 * WIDTH + 16], want[3 * WIDTH + 32];
 int k, r = E_BUFFER_NOMEM;

 memset(wide, 'p', sizeof wide - 1);
 wide[WIDTH] = '-';
 wide[2 * WIDTH] = ' ';
 wide[sizeof wide - 1] = '\0';
 snprintf(text, sizeof text, "a\n\n%s\nb", wide);
 snprintf(want, sizeof want, "L0\na\n%s\nb\n", wide);
 fixture(1, E_BUFFER_HISTORY_FULL);
 e_buffer_mark_view(&buf);
 chk("24 taken", e_buffer_append_rows(&buf, text), E_BUFFER_OK);
 chk_str("24 one row per line, the wide one whole", text_of(), want);
 chk("24 the wide row's length", buf.bf[2].len, 3 * WIDTH);
 chk("24 a view keeps no undo", list_len(buf.ud), 0);
 fixture_free();
 for (k = 1; k <= 8 && r == E_BUFFER_NOMEM; k++) {
  fixture(1, E_BUFFER_HISTORY_FULL);
  e_buffer_mark_view(&buf);
  e_buffer_test_fail_allocs(k);
  r = e_buffer_append_rows(&buf, text);
  e_buffer_test_fail_allocs(0);
  if (r == E_BUFFER_NOMEM)
   chk_str("24 a refused append changes nothing", text_of(), "L0\n");
  fixture_free();
 }
 chk("24 an append with memory works", r, E_BUFFER_OK);
 chk("24 after at least one refusal", k > 2, 1);
}

static void case_many_appends(void)
{
 STRING l;
 char want[16];
 int i, ok = 1;

 fixture(1, E_BUFFER_HISTORY_FULL);
 e_buffer_mark_view(&buf);
 for (i = 0; i < MANY_APPENDS; i++) {
  snprintf(want, sizeof want, "%d", i);
  l = line_of(&buf, want);
  if (e_buffer_splice(&buf, buf.mxlines, 0, &l, 1, NULL, 0, E_BUFFER_STEP_NEW) != E_BUFFER_OK)
   ok = 0;
 }
 chk("20 every append taken", ok, 1);
 chk("20 lines", buf.mxlines, MANY_APPENDS + 1);
 chk("20 the table grew in steps", buf.mx.y % MAXLINES == 0 && buf.mx.y >= buf.mxlines + 2, 1);
 for (i = 0; i < MANY_APPENDS; i += 997) {
  snprintf(want, sizeof want, "%d\n", i);
  chk_str("20 the line is where it was put", (char *) buf.bf[i + 1].s, want);
 }
 chk("20 a view keeps no undo", list_len(buf.ud), 0);
 fixture_free();
}

static void case_depth_guard(void)
{
 fixture(3, E_BUFFER_HISTORY_FULL);
 chk("C7 nothing open", e_buffer_depth_check(&buf), 0);
 e_buffer_cover_begin(&buf, NULL);
 e_buffer_step_begin(&buf);
 chk("C7 a leak is found", e_buffer_depth_check(&buf), 1);
 chk("C7 and traced", leak_traced, 1);
 chk("C7 and closed", buf.covered + buf.step_depth, 0);
 e_buffer_delete_lines(&buf, 0, 1, E_BUFFER_STEP_NEW);
 chk("C7 the next change is recorded again", list_len(buf.ud), 1);
 fixture_free();
}

/* A run of the bytes of a C string, for e_buffer_lines_from_parts. */
static WpeBufferPart part_of(const char *s)
{
 WpeBufferPart p;

 p.s = (const unsigned char *) s;
 p.n = (int) strlen(s);
 return(p);
}

/* The lines a block move builds where two texts meet: wrapped where the
   file reader wraps, the last ending as the second run ends. */
static void case_lines_from_parts(void)
{
 static const char long_text[] =
  "one two three four five six seven eight nine ten eleven twelve thirteen "
  "fourteen fifteen sixteen seventeen eighteen nineteen twenty twenty-one";
 WpeBufferPart parts[2];
 STRING *joined, *whole;
 char head[96];
 int nj, nw, i, same = 1;

 parts[0] = part_of("abc");
 parts[1] = part_of("def\n");
 joined = e_buffer_lines_from_parts(WIDTH, parts, 2, &nj);
 chk("26 one line", nj, 1);
 chk_str("26 ends with the second run's line break", (char *) joined[0].s, "abc" "def\n");
 chk("26 measured", joined[0].len * 10 + joined[0].nrc, 6 * 10 + 7);
 e_buffer_lines_free(joined, nj);
 parts[1] = part_of("def");
 joined = e_buffer_lines_from_parts(WIDTH, parts, 2, &nj);
 chk_str("26 a run with no line break leaves the line open", (char *) joined[0].s, "abcdef");
 chk("26 ... measured", joined[0].len * 10 + joined[0].nrc, 6 * 10 + 6);
 e_buffer_lines_free(joined, nj);
 joined = e_buffer_lines_from_parts(WIDTH, parts, 0, &nj);
 chk("26 no runs make one empty line", nj == 1 && joined[0].len == 0, 1);
 e_buffer_lines_free(joined, nj);
 snprintf(head, sizeof head, "%.70s", long_text);
 parts[0] = part_of(head);
 parts[1] = part_of(long_text + 70);
 joined = e_buffer_lines_from_parts(WIDTH, parts, 2, &nj);
 whole = e_buffer_lines_from_text(long_text, WIDTH, &nw);
 chk("26 a wide join wraps into as many lines as the reader's split", nj, nw);
 for (i = 0; i < nj && i < nw; i++)
  if (strcmp((char *) joined[i].s, (char *) whole[i].s) != 0 && i < nw - 1)
   same = 0;
 chk("26 ... at the same columns", same, 1);
 chk("26 the last piece is open: the run had no line break", joined[nj - 1].nrc,
     joined[nj - 1].len);
 e_buffer_lines_free(joined, nj);
 e_buffer_lines_free(whole, nw);
}

/* The text of the move fixture after the block (1,2)-(1,4) went to (1,6). */
#define MOVE_BEFORE "L0\nL1\nL2\nL3\nL4\nL5\nL6\nL7\n"
#define MOVE_AFTER  "L0\nL1\nL4\nL5\nL2\nL3\nL6\nL7\n"
/* What the move below takes from the owner: the record, four for the line
   that moves whole, three or four for each line built, the margin's room,
   and the table's growth for the line that stands there for a moment
   before the two ends join (the fixture's table holds MAXLINES - 2). */
#define MOVE_ALLOCATIONS 18

/* A block move as the block code builds it for a block from inside one
   line to inside another, dropped inside a line below it in the same
   buffer: the middle line moves whole with its breakpoint, the line the
   block is dropped into takes the block's first part, the block's last
   part takes the rest of that line, and the block's two ends join. */
static int a_move_of_the_block(void)
{
 WpeBufferTxn t;
 WpeBufferPart parts[2];
 Undo *rec;

 e_buffer_txn_begin(&t);
 rec = e_buffer_txn_record(&t, 'v');
 e_buffer_txn_move_lines(&t, &buf, 3, 1, &buf, 6);
 parts[0] = part_of("L");
 parts[1] = part_of("2\n");
 e_buffer_txn_splice_parts(&t, &buf, 5, 1, parts, 2, E_BUFFER_STEP_COVERED);
 parts[1] = part_of("6\n");
 e_buffer_txn_splice_parts(&t, &buf, 7, 0, parts, 2, E_BUFFER_STEP_COVERED);
 parts[1] = part_of("4\n");
 e_buffer_txn_splice_parts(&t, &buf, 2, 2, parts, 2, E_BUFFER_STEP_COVERED);
 if (e_buffer_txn_commit(&t) != E_BUFFER_OK)
  return(E_BUFFER_NOMEM);
 e_buffer_record_push(&buf, rec);
 return(E_BUFFER_OK);
}

/* Memory refused at every allocation of a block move in turn: the move is
   made whole, or the text, breakpoints, marks, modified mark and both undo
   lists are as they were and nobody was told of a line that moved. */
static void case_a_refused_move_changes_nothing(void)
{
 int rows[2] = {3, 4};
 int n, r, bad = 0, refused = 0, whole_from = 0;

 for (n = 1; n <= MOVE_ALLOCATIONS + 1; n++) {
  fixture(8, E_BUFFER_HISTORY_FULL);
  brp_set(2, rows);
  scr.mark_begin = pnt(1, 2);
  scr.mark_end = pnt(1, 4);
  e_buffer_cover_begin(&buf, NULL);
  e_buffer_test_fail_allocs(n);
  r = a_move_of_the_block();
  e_buffer_test_fail_allocs(0);
  e_buffer_cover_end(&buf);
  if (r == E_BUFFER_OK) {
   if (whole_from == 0)
    whole_from = n;
   if (strcmp(text_of(), MOVE_AFTER) != 0 || scr.brp[0] != 1 || !brp_has(5))
    bad++;
  } else {
   refused++;
   if (strcmp(text_of(), MOVE_BEFORE) != 0 || scr.brp[0] != 2 || !brp_has(3) || !brp_has(4) ||
       scr.mark_begin.x != 1 || scr.mark_begin.y != 2 || scr.mark_end.x != 1 ||
       scr.mark_end.y != 4 || win.save != 0 || buf.ud != NULL || buf.rd != NULL ||
       moved_calls != 0)
    bad++;
  }
  fixture_free();
 }
 chk("27 a refused move is whole or changes nothing", bad, 0);
 chk("27 every cut before the last allocation refuses it", refused, MOVE_ALLOCATIONS);
 chk("27 the move takes this many allocations", whole_from - 1, MOVE_ALLOCATIONS);
}

/* Whether one line can be made now, the line freed again. */
static int a_line_can_be_made(void)
{
 STRING l;

 if (e_buffer_line_new(&buf, &l) != E_BUFFER_OK)
  return(0);
 free(l.s);
 return(1);
}

/* The fault a test of the running editor arms fires in the first action
   that enters its point, from the armed allocation on, for that action
   only, and once: memory is back when the action ends, a nested entry of
   the same point keeps it, and no later action meets it again. */
static void case_the_fault_hook_fires_once(void)
{
 int outer, inner;

 fixture(3, E_BUFFER_HISTORY_FULL);
 fault_traced = 0;
 chk("25 a name it knows", e_buffer_test_fault_point("block-move"), E_BUFFER_FAULT_BLOCK_MOVE);
 chk("25 a name it does not know", e_buffer_test_fault_point("blockmove"), E_BUFFER_FAULT_NONE);
 e_buffer_test_fault_arm(E_BUFFER_FAULT_BLOCK_MOVE, 2);
 outer = e_buffer_test_fault_enter(E_BUFFER_FAULT_UNDO);
 chk("25 another point does not fire it", outer, 0);
 chk("25 ... and memory is there", a_line_can_be_made(), 1);
 e_buffer_test_fault_leave(outer);
 outer = e_buffer_test_fault_enter(E_BUFFER_FAULT_BLOCK_MOVE);
 chk("25 its point fires it", outer, E_BUFFER_FAULT_FIRED);
 chk("25 and the trace says so", fault_traced, 1);
 chk("25 the allocations before the armed one are made", a_line_can_be_made(), 1);
 chk("25 the armed one is refused", a_line_can_be_made(), 0);
 inner = e_buffer_test_fault_enter(E_BUFFER_FAULT_BLOCK_MOVE);
 chk("25 a nested entry does not fire it again", inner, 0);
 e_buffer_test_fault_leave(inner);
 chk("25 a nested leave keeps it", a_line_can_be_made(), 0);
 e_buffer_test_fault_leave(outer);
 chk("25 the action's end gives memory back", a_line_can_be_made(), 1);
 outer = e_buffer_test_fault_enter(E_BUFFER_FAULT_BLOCK_MOVE);
 chk("25 it fires once", outer, 0);
 chk("25 ... and the next action has memory", a_line_can_be_made(), 1);
 e_buffer_test_fault_leave(outer);
 chk("25 traced once", fault_traced, 1);
 fixture_free();
}

/* ---- a step of Undo as one transaction: the pieces ---- */

/* A copy of a line's text without its line break, for a comparison. */
static const char *line_text(const STRING *l)
{
 static char out[WIDTH + 2];

 snprintf(out, sizeof out, "%.*s", l->len, (const char *) l->s);
 return(out);
}

/* A fresh array of one line of the fixture's width, holding text. */
static STRING *one_line(const char *text)
{
 STRING *l = malloc(sizeof(STRING));

 if (l == NULL)
  exit(2);
 *l = line_of(&buf, text);
 return(l);
}

/* What the view says of every row, and whether the buffer says the same
   once the change is made: the view is what the next record's replay
   reads before anything is made. */
#define VIEW_ROWS 16
static int view_texts(WpeBufferTxn *t, char texts[][WIDTH + 2], int *rows, int n)
{
 const STRING *l;
 int y, row;

 for (y = 0; y < n; y++) {
  if (e_buffer_txn_line(t, &buf, y, &l, &row) != E_BUFFER_OK)
   return(y);
  snprintf(texts[y], WIDTH + 2, "%s", line_text(l));
  rows[y] = row;
 }
 return(n);
}

static int view_matches_buffer(char texts[][WIDTH + 2], const int *rows, int n)
{
 int y, bad = 0;

 if (n != buf.mxlines)
  return(0);
 for (y = 0; y < n; y++)
  if (strcmp(texts[y], line_text(&buf.bf[y])) != 0 || rows[y] != brp_has(y))
   bad++;
 return(bad == 0);
}

/* The view reads lines the items so far would put in, with the breakpoints
   they would leave, before anything is made: a changed row keeps the one
   below it, an inserted row has none, a replayed line has the record's. */
static void case_the_view_reads_pending_lines(void)
{
 char texts[VIEW_ROWS][WIDTH + 2];
 int rows[2] = {2, 5}, got[VIEW_ROWS], n, row;
 STRING *two;
 const STRING *l;
 WpeBufferTxn t;
 Undo *rec;

 fixture(8, E_BUFFER_HISTORY_FULL);
 brp_set(2, rows);
 e_buffer_txn_begin(&t);
 e_buffer_txn_splice(&t, &buf, 3, 1, one_line("x3"), 1, E_BUFFER_STEP_COVERED);
 two = malloc(2 * sizeof(STRING));
 two[0] = line_of(&buf, "n1a");
 two[1] = line_of(&buf, "n1b");
 e_buffer_txn_splice(&t, &buf, 1, 0, two, 2, E_BUFFER_STEP_COVERED);
 e_buffer_txn_splice(&t, &buf, 7, 1, one_line("x5"), 1, E_BUFFER_STEP_COVERED);
 n = view_texts(&t, texts, got, VIEW_ROWS);
 chk("28 the view has the lines the items leave", n, 10);
 chk_str("28 an inserted line", texts[1], "n1a");
 chk("28 ... has no breakpoint", got[1], 0);
 chk_str("28 a line below it, moved down", texts[4], "L2");
 chk("28 ... with its breakpoint", got[4], 1);
 chk_str("28 a changed line", texts[5], "x3");
 chk_str("28 a changed line over a breakpoint", texts[7], "x5");
 chk("28 ... keeps it", got[7], 1);
 chk("28 a row past the end is refused", e_buffer_txn_line(&t, &buf, 10, &l, &row), E_BUFFER_RANGE);
 chk_str("28 nothing is made yet", text_of(), "L0\nL1\nL2\nL3\nL4\nL5\nL6\nL7\n");
 chk("28 the change is made", e_buffer_txn_commit(&t), E_BUFFER_OK);
 chk("28 as the view said", view_matches_buffer(texts, got, n), 1);
 fixture_free();

 fixture(8, E_BUFFER_HISTORY_FULL);
 row = 4;
 brp_set(1, &row);
 e_buffer_delete_lines(&buf, 4, 1, E_BUFFER_STEP_NEW);
 rec = buf.ud;
 e_buffer_txn_begin(&t);
 chk("28 a replay item", e_buffer_txn_replay_lines(&t, &buf, rec), E_BUFFER_OK);
 n = view_texts(&t, texts, got, VIEW_ROWS);
 chk_str("28 the view reads the record's line", texts[4], "L4");
 chk("28 ... with the breakpoint it parked", got[4], 1);
 e_buffer_txn_abort(&t);
 chk("28 an abort leaves the record its line", rec->n_old == 1 && rec->old != NULL, 1);
 chk_str("28 ... and the text as it was", text_of(), "L0\nL1\nL2\nL3\nL5\nL6\nL7\n");
 e_buffer_txn_begin(&t);
 e_buffer_txn_replay_lines(&t, &buf, rec);
 chk("28 a replay item commits", e_buffer_txn_commit(&t), E_BUFFER_OK);
 chk("28 as the view said", view_matches_buffer(texts, got, n), 1);
 chk("28 its inverse is on the list", buf.ud != rec && buf.ud->type == E_UNDO_LINES &&
     buf.ud->n_old == 0 && buf.ud->n_new == 1, 1);
 fixture_free();

 fixture(8, E_BUFFER_HISTORY_FULL);
 e_buffer_txn_begin(&t);
 e_buffer_txn_move_lines(&t, &buf, 1, 1, &buf, 4);
 chk("28 the view does not read through a move",
     e_buffer_txn_line(&t, &buf, 5, &l, &row), E_BUFFER_RANGE);
 chk("28 ... and says so", move_view_traced, 1);
 e_buffer_txn_abort(&t);
 fixture_free();
}

/* Splices held in a transaction itself, and the ones past them that take
   memory: one each for the first growth, then one each item. */
#define GROW_ITEMS        (E_BUFFER_TXN_SPLICES + 8)
#define GROW_ALLOCATIONS  (GROW_ITEMS + 1)

/* A change of more splices than a transaction holds inline (a format's
   Undo has one per hunk) grows, and is still whole or not at all. */
static void case_a_transaction_grows_past_its_inline_slots(void)
{
 STRING *lines[GROW_ITEMS];
 char name[16], want[GROW_ITEMS * 8 + 64];
 WpeBufferTxn t;
 int n, i, r, bad = 0, refused = 0, whole_from = 0, inline_held = 0;

 for (n = 1; n <= GROW_ALLOCATIONS + 1; n++) {
  fixture(GROW_ITEMS + 2, E_BUFFER_HISTORY_FULL);
  for (i = 0; i < GROW_ITEMS; i++) {
   snprintf(name, sizeof name, "X%d", i);
   lines[i] = one_line(name);
  }
  e_buffer_test_fail_allocs(n);
  e_buffer_txn_begin(&t);
  for (i = 0; i < GROW_ITEMS; i++) {
   e_buffer_txn_splice(&t, &buf, i, 1, lines[i], 1, E_BUFFER_STEP_COVERED);
   if (i == E_BUFFER_TXN_SPLICES - 1 && t.sp == t.sp_inline)
    inline_held++;
  }
  r = e_buffer_txn_commit(&t);
  e_buffer_test_fail_allocs(0);
  if (r == E_BUFFER_OK) {
   if (whole_from == 0)
    whole_from = n;
   want[0] = '\0';
   for (i = 0; i < GROW_ITEMS; i++)
    snprintf(want + strlen(want), sizeof want - strlen(want), "X%d\n", i);
   snprintf(want + strlen(want), sizeof want - strlen(want), "L%d\nL%d\n", GROW_ITEMS,
            GROW_ITEMS + 1);
   if (strcmp(text_of(), want) != 0)
    bad++;
  } else {
   refused++;
   if (buf.bf[0].s[0] != 'L' || buf.bf[GROW_ITEMS - 1].s[0] != 'L' || moved_calls != 0)
    bad++;
  }
  chk("29 the transaction is back on its inline slots", t.sp == t.sp_inline, 1);
  fixture_free();
 }
 chk("29 a grown change is whole or changes nothing", bad, 0);
 chk("29 every cut before the last allocation refuses it", refused, GROW_ALLOCATIONS);
 chk("29 the change takes this many allocations", whole_from - 1, GROW_ALLOCATIONS);
 chk("29 the inline slots hold a block move's splices without memory",
     inline_held, GROW_ALLOCATIONS + 1);
}

/* The pieces of a shell, all the owner's: the buffer, its window, its
   screen, its margin, its table and its one line. */
#define SHELL_ALLOCATIONS 6

/* A shell is made of the owner's memory, so a test that starves the owner
   reaches it, and a record's text is too. */
static void case_a_shell_is_an_owner_allocation(void)
{
 BUFFER *shell = NULL;
 Undo *rec;
 int n;

 fixture(3, E_BUFFER_HISTORY_FULL);
 for (n = 1; n <= SHELL_ALLOCATIONS + 1; n++) {
  e_buffer_test_fail_allocs(n);
  shell = e_buffer_shell_new(&buf, WIDTH);
  e_buffer_test_fail_allocs(0);
  if (shell != NULL)
   break;
 }
 chk("30 a shell takes this many allocations", n - 1, SHELL_ALLOCATIONS);
 chk("30 it is one", shell != NULL && e_buffer_is_shell(shell), 1);
 chk("30 the window's buffer is not", e_buffer_is_shell(&buf), 0);
 if (shell != NULL) {
  chk("30 one empty line", shell->mxlines == 1 && shell->bf[0].len == 0, 1);
  chk("30 as wide as asked", shell->mx.x, WIDTH);
  chk("30 a margin of its own, empty", shell->f->s->brp != NULL && shell->f->s->brp[0] == 0, 1);
  chk("30 no file name: no debugger hears of it", shell->f->datnam == NULL, 1);
  chk("30 no editor window", shell->f->ed == NULL, 1);
  chk("30 nothing refused yet", shell->refusals, 0);
  e_buffer_shell_free(shell);
 }
 rec = e_buffer_record_new('r');
 e_buffer_test_fail_allocs(1);
 chk("30 a record's text is the owner's memory",
     e_buffer_record_text(rec, (const unsigned char *) "ab", 2), E_BUFFER_NOMEM);
 e_buffer_test_fail_allocs(0);
 chk("30 ... and copied when there is memory",
     e_buffer_record_text(rec, (const unsigned char *) "ab", 2) == E_BUFFER_OK &&
     memcmp(rec->u.pt, "ab", 2) == 0, 1);
 e_buffer_record_free(rec);
 fixture_free();
}

/* A change refused inside a shell is counted there and said nowhere: the
   step it belongs to is refused whole and says so once. */
static void case_a_shell_counts_its_refusals(void)
{
 BUFFER *shell;
 STRING l;
 int zero = 0;
 Undo rec;

 fixture(3, E_BUFFER_HISTORY_FULL);
 shell = e_buffer_shell_new(&buf, WIDTH);
 l = line_of(shell, "s");
 e_buffer_test_fail_allocs(1);
 chk("31 a splice refused in a shell",
     e_buffer_splice(shell, 0, 0, &l, 1, &zero, 1, E_BUFFER_STEP_COVERED), E_BUFFER_NOMEM);
 e_buffer_test_fail_allocs(0);
 free(l.s);
 chk("31 counts there", shell->refusals, 1);
 memset(&rec, 0, sizeof rec);
 rec.type = 'a';
 stub_refuses = 1;
 wpe_buffer_replay_on_shell(shell, shell->f->s, &rec, 0);
 stub_refuses = 0;
 chk("31 a primitive refused on a shell counts there", shell->refusals, 2);
 chk("31 and is not said", nomem_calls, 0);
 e_buffer_shell_refused(&buf);
 chk("31 the window's buffer counts nothing", buf.refusals, 0);
 e_buffer_shell_free(shell);
 fixture_free();
}

/* The Undo of a typed line break at the end of L3 as the owner's replay
   will build it: rows 3 and 4 are the span (L3 and the line that joins it),
   L2 and L5 its guards. */
#define JOIN_Y0       3
#define JOIN_SPAN     2
#define JOIN_GUARD    1
#define JOIN_BEFORE   "L0\nL1\nL2\nL3\nL4\nL5\nL6\nL7\n"
#define JOIN_AFTER    "L0\nL1\nL2\nL3\nL5\nL6\nL7\n"

/* The rows of the shell's span that carry a breakpoint, relative to the
   span: what the rewrite item takes as brk[]. */
static int shell_span_rows(BUFFER *shell, int g_top, int n_ins, int *out)
{
 int i, rel, n = 0;

 for (i = 1; i <= shell->f->s->brp[0]; i++) {
  rel = shell->f->s->brp[i] - g_top;
  if (rel >= 0 && rel < n_ins && n < MAX_ROWS)
   out[n++] = rel;
 }
 return(n);
}

/* A shell over the join's span and guards, the primitive played on it in a
   step of its own, one rewrite item, the commit.  Return: what the change
   returned. */
static int a_join_replayed_on_a_shell(const Undo *rec)
{
 WpeBufferTxn t;
 BUFFER *shell;
 int rows[MAX_ROWS], nrows, top = JOIN_Y0 - JOIN_GUARD, n_ins, r;

 e_buffer_txn_begin(&t);
 if ((shell = e_buffer_shell_new(&buf, buf.mx.x)) == NULL)
  return(E_BUFFER_NOMEM);
 if ((r = e_buffer_shell_fill(shell, &t, &buf, top, JOIN_SPAN + 2 * JOIN_GUARD)) != E_BUFFER_OK) {
  e_buffer_shell_free(shell);
  return(r);
 }
 shell->b = pnt(rec->b.x, rec->b.y - top);
 e_buffer_step_begin(shell);
 wpe_buffer_replay_on_shell(shell, shell->f->s, rec, top);
 e_buffer_step_end(shell);
 if (shell->refusals > 0) {
  e_buffer_shell_free(shell);
  return(E_BUFFER_NOMEM);
 }
 n_ins = shell->mxlines - 2 * JOIN_GUARD;
 nrows = shell_span_rows(shell, JOIN_GUARD, n_ins, rows);
 r = e_buffer_txn_rewrite(&t, &buf, JOIN_Y0, JOIN_SPAN, shell, JOIN_GUARD, JOIN_GUARD, rows,
                          nrows);
 if (r != E_BUFFER_OK) {
  e_buffer_txn_abort(&t);
  return(r);
 }
 return(e_buffer_txn_commit(&t));
}

/* The 'a' record of a line break typed at the end of L3. */
static Undo a_line_break_record(void)
{
 Undo rec;

 memset(&rec, 0, sizeof rec);
 rec.type = 'a';
 rec.b = pnt(2, JOIN_Y0);
 rec.a.x = 1;
 return(rec);
}

/* What the owner of a join takes: the shell (6), the fill (the lines and
   rows arrays, four lines, the splice's added rows and room on the shell's
   margin: 8), the primitive on the shell (the 'r' inverse and its text,
   the deletion's removed rows, the pocket's room: 4), the rewrite item (its
   rows, removed, added: 3) and the room on the window's margin (1). */
#define JOIN_ALLOCATIONS 22

static int join_is_whole(void)
{
 Undo *u = buf.ud;

 return(strcmp(text_of(), JOIN_AFTER) == 0 && scr.brp[0] == 2 && brp_has(3) && brp_has(4) &&
        u != NULL && u->next == NULL && u->type == 'r' && u->b.x == 2 && u->b.y == JOIN_Y0 &&
        (u->flags & E_UNDO_MORE) == 0 && u->nbrk == 1 && u->brk[0] == 1 &&
        ((char *) u->u.pt)[0] == '\n' && buf.rd == NULL && buf.b.x == 2 && buf.b.y == JOIN_Y0 &&
        scr.mark_begin.x == 1 && scr.mark_begin.y == 5 && moved_calls == 1 &&
        moved_del == JOIN_SPAN && moved_ins == 1 && moved_nremoved == 0 && moved_nadded == 0);
}

static int join_changed_nothing(void)
{
 return(strcmp(text_of(), JOIN_BEFORE) == 0 && scr.brp[0] == 3 && brp_has(3) && brp_has(4) &&
        brp_has(5) && buf.ud == NULL && buf.rd == NULL && buf.b.x == 0 && buf.b.y == 7 &&
        scr.mark_begin.x == 1 && scr.mark_begin.y == 6 && moved_calls == 0 && win.save == 0);
}

static void join_fixture(void)
{
 int rows[3] = {3, 4, 5};

 fixture(8, E_BUFFER_HISTORY_FULL);
 brp_set(3, rows);
 scr.mark_begin = pnt(1, 6);
 buf.b = pnt(0, 7);
}

/* A rewrite item built by hand as the owner's replay will build it, with
   memory refused at each allocation in turn: the join is made whole, its
   inverse on the list with the parked breakpoint, or nothing changed. */
static void case_a_rewrite_item_is_whole_or_nothing(void)
{
 Undo rec = a_line_break_record();
 int n, r, bad = 0, refused = 0, whole_from = 0;

 for (n = 1; n <= JOIN_ALLOCATIONS + 1; n++) {
  join_fixture();
  e_buffer_test_fail_allocs(n);
  r = a_join_replayed_on_a_shell(&rec);
  e_buffer_test_fail_allocs(0);
  if (r == E_BUFFER_OK) {
   if (whole_from == 0)
    whole_from = n;
   if (!join_is_whole())
    bad++;
  } else {
   refused++;
   if (r != E_BUFFER_NOMEM || !join_changed_nothing())
    bad++;
  }
  fixture_free();
 }
 chk("32 a rewrite is whole or changes nothing", bad, 0);
 chk("32 every cut before the last allocation refuses it", refused, JOIN_ALLOCATIONS);
 chk("32 the join takes this many allocations", whole_from - 1, JOIN_ALLOCATIONS);

 join_fixture();
 stub_overruns = 1;
 r = a_join_replayed_on_a_shell(&rec);
 stub_overruns = 0;
 chk("32 a replay that reached a guard is refused", r, E_BUFFER_RANGE);
 chk("32 ... traced", overrun_traced, 1);
 chk("32 ... and changes nothing", join_changed_nothing(), 1);
 fixture_free();
}

/* The rows a rewrite tells the debugger of: a row of a changed line that
   loses its breakpoint is removed, a row that gains one is added, and a
   line split at column 0 that took its breakpoint down is both. */
static void case_a_rewrite_reports_rows_removed_and_added(void)
{
 WpeBufferTxn t;
 BUFFER *shell;
 int row = 3, one = 1;

 fixture(8, E_BUFFER_HISTORY_FULL);
 brp_set(1, &row);
 e_buffer_txn_begin(&t);
 shell = e_buffer_shell_new(&buf, WIDTH);
 e_buffer_shell_fill(shell, &t, &buf, 2, 4);
 chk("33 the fill copies the rows", shell->f->s->brp[0] == 1 && shell->f->s->brp[1] == 1, 1);
 e_buffer_txn_rewrite(&t, &buf, 3, 2, shell, 1, 1, &one, 1);
 chk("33 a rewrite moving a row commits", e_buffer_txn_commit(&t), E_BUFFER_OK);
 chk("33 the row moved", scr.brp[0] == 1 && brp_has(4), 1);
 chk("33 its old row is removed", moved_nremoved == 1 && moved_removed[0] == 3, 1);
 chk("33 its new row is added", moved_nadded == 1 && moved_added[0] == 4, 1);
 chk_str("33 the text is the shell's", text_of(), "L0\nL1\nL2\nL3\nL4\nL5\nL6\nL7\n");
 fixture_free();

 fixture(8, E_BUFFER_HISTORY_FULL);
 brp_set(1, &row);
 e_buffer_txn_begin(&t);
 shell = e_buffer_shell_new(&buf, WIDTH);
 e_buffer_shell_fill(shell, &t, &buf, 2, 3);
 e_buffer_insert_blank(shell, 1, E_BUFFER_STEP_COVERED);
 e_buffer_txn_rewrite(&t, &buf, 3, 1, shell, 1, 1, &one, 1);
 hooks_reset();
 chk("33 a split at column 0 commits", e_buffer_txn_commit(&t), E_BUFFER_OK);
 chk_str("33 an empty line above", text_of(), "L0\nL1\nL2\n\nL3\nL4\nL5\nL6\nL7\n");
 chk("33 the breakpoint went down with its line", scr.brp[0] == 1 && brp_has(4), 1);
 chk("33 removed at its old row", moved_nremoved == 1 && moved_removed[0] == 3, 1);
 chk("33 added at the new one", moved_nadded == 1 && moved_added[0] == 4, 1);
 chk("33 the debugger hears one line in, one out", moved_del * 10 + moved_ins, 12);
 fixture_free();
}

/* The replay of an 'r' record whose text lands on the last row of a file
   when that row is full and ends soft (what Ctrl-Y on the hard row under a
   wrapped one leaves).  e_ins_nchar sends the overflow of a soft row down
   into the row below unless the row is the last, which then gets a row of
   its own for its tail (we_edit.c, `ya == b->mxlines - 1`; it once tested
   `ya == b->mxlines`, never true, and read bf[mxlines], past the lines).
   This case reports what the owner leaves there; the primitive's rows are
   the editor's (test_buffer_owner.py, test_block_asan.py). */
static void case_a_char_replay_on_a_full_soft_last_row(void)
{
 WpeBufferTxn t;
 BUFFER *shell;
 int last, reaches;

 fixture(8, E_BUFFER_HISTORY_FULL);
 memset(buf.bf[6].s, 'x', WIDTH - 1);
 buf.bf[6].s[WIDTH - 1] = '\0';
 buf.bf[6].len = buf.bf[6].nrc = WIDTH - 1;
 e_buffer_delete_lines(&buf, 7, 1, E_BUFFER_STEP_NEW);
 last = buf.mxlines - 1;
 chk("34 the last row ends soft", buf.bf[last].s[buf.bf[last].len], '\0');
 chk("34 ... and is full", buf.bf[last].len + 1 >= buf.mx.x - 1, 1);
 e_buffer_txn_begin(&t);
 shell = e_buffer_shell_new(&buf, WIDTH);
 chk("34 a shell over the span to the last row, a guard above",
     e_buffer_shell_fill(shell, &t, &buf, last - 1, 2), E_BUFFER_OK);
 reaches = (shell->bf[shell->mxlines - 1].s[shell->bf[shell->mxlines - 1].len] == '\0' &&
            shell->mxlines < shell->mx.y);
 printf("NOTE 34: an 'r' replay on the shell meets a full soft last row with "
        "nothing below it: %s\n", reaches ? "yes" : "no");
 chk("34 the shell's last row ends soft too, nothing below it", reaches, 1);
 e_buffer_shell_free(shell);
 fixture_free();
}

/* Undo of a join's record of kind 'r' at (@x, @y): the stub splits the line
   there again and records the 'a' that joins it. */
static Undo a_join_record(int x, int y)
{
 Undo rec;

 memset(&rec, 0, sizeof rec);
 rec.type = 'r';
 rec.b = pnt(x, y);
 rec.a.x = 1;
 return(rec);
}

/* One record replayed on a shell over rows [@y, @y + @n_del) of the
   fixture with @g_top and @g_bot guard rows, added to @t as one rewrite
   item, as the owner's replay will do it.  Return: what adding it
   returned; on a refusal the shell is gone and @t is left to its caller. */
static int a_record_replayed_into(WpeBufferTxn *t, const Undo *rec, int y, int n_del,
                                  int g_top, int g_bot)
{
 BUFFER *shell;
 int rows[MAX_ROWS], nrows, top = y - g_top, r;

 if ((shell = e_buffer_shell_new(&buf, buf.mx.x)) == NULL)
  return(E_BUFFER_NOMEM);
 if ((r = e_buffer_shell_fill(shell, t, &buf, top, n_del + g_top + g_bot)) != E_BUFFER_OK) {
  e_buffer_shell_free(shell);
  return(r);
 }
 shell->b = pnt(rec->b.x, rec->b.y - top);
 e_buffer_step_begin(shell);
 wpe_buffer_replay_on_shell(shell, shell->f->s, rec, top);
 e_buffer_step_end(shell);
 if (shell->refusals > 0) {
  e_buffer_shell_free(shell);
  return(E_BUFFER_NOMEM);
 }
 nrows = shell_span_rows(shell, g_top, shell->mxlines - g_top - g_bot, rows);
 return(e_buffer_txn_rewrite(t, &buf, y, n_del, shell, g_top, g_bot, rows, nrows));
}

/* The most allocations the whole-or-nothing loops below try. */
#define SWEEP_LIMIT 80

/* The undo of a join on the file's last row: a shell with a guard above
   and none below, the line split again with its breakpoint on the head. */
#define LAST_BEFORE "L0\nL1\nL2\nL3\nL4\nL5\nL6\nL7\n"
#define LAST_AFTER  "L0\nL1\nL2\nL3\nL4\nL5\nL6\nL\n7\n"

static int last_row_changed_nothing(void)
{
 return(strcmp(text_of(), LAST_BEFORE) == 0 && scr.brp[0] == 1 && brp_has(7) &&
        buf.ud == NULL && buf.rd == NULL && moved_calls == 0 && buf.b.x == 0 && buf.b.y == 2);
}

static int last_row_is_whole(void)
{
 Undo *u = buf.ud;

 return(strcmp(text_of(), LAST_AFTER) == 0 && scr.brp[0] == 1 && brp_has(7) &&
        u != NULL && u->next == NULL && u->type == 'a' && u->b.x == 1 && u->b.y == 7 &&
        buf.rd == NULL && moved_calls == 1 && moved_del == 1 && moved_ins == 2 &&
        moved_nremoved == 0 && moved_nadded == 0);
}

static void case_a_rewrite_on_the_last_row(void)
{
 Undo rec = a_join_record(1, 7);
 WpeBufferTxn t;
 int n, r, row = 7, bad = 0, whole_from = 0;

 for (n = 1; n <= SWEEP_LIMIT && whole_from == 0; n++) {
  fixture(8, E_BUFFER_HISTORY_FULL);
  brp_set(1, &row);
  buf.b = pnt(0, 2);
  e_buffer_test_fail_allocs(n);
  e_buffer_txn_begin(&t);
  r = a_record_replayed_into(&t, &rec, 7, 1, 1, 0);
  if (r == E_BUFFER_OK)
   r = e_buffer_txn_commit(&t);
  else
   e_buffer_txn_abort(&t);
  e_buffer_test_fail_allocs(0);
  if (r == E_BUFFER_OK) {
   whole_from = n;
   if (!last_row_is_whole())
    bad++;
  } else if (r != E_BUFFER_NOMEM || !last_row_changed_nothing())
   bad++;
  fixture_free();
 }
 chk("35 a rewrite on the last row is whole or changes nothing", bad, 0);
 chk("35 ... and is made once memory suffices", whole_from > 1, 1);

 fixture(8, E_BUFFER_HISTORY_FULL);
 brp_set(1, &row);
 e_buffer_txn_begin(&t);
 rec = a_join_record(1, 7);
 chk("35 a guard below the last row is refused",
     a_record_replayed_into(&t, &rec, 7, 1, 1, 1), E_BUFFER_RANGE);
 e_buffer_txn_abort(&t);
 chk("35 ... and changes nothing", strcmp(text_of(), LAST_BEFORE) == 0 && buf.ud == NULL, 1);
 fixture_free();
}

/* A shell of one row holding a line just like the one a fresh shell starts
   with: the fill must not take the two for the same and drop the row's
   breakpoint. */
static void case_a_shell_of_one_row_keeps_its_breakpoint(void)
{
 WpeBufferTxn t;
 BUFFER *shell;
 int row = 1, zero = 0;

 fixture(3, E_BUFFER_HISTORY_FULL);
 free(buf.bf[1].s);
 e_buffer_line_new(&buf, &buf.bf[1]);
 brp_set(1, &row);
 e_buffer_txn_begin(&t);
 shell = e_buffer_shell_new(&buf, buf.mx.x);
 chk("36 a shell of one row is filled", e_buffer_shell_fill(shell, &t, &buf, 1, 1), E_BUFFER_OK);
 chk("36 ... with the row's breakpoint",
     shell->mxlines == 1 && shell->f->s->brp[0] == 1 && shell->f->s->brp[1] == 0, 1);
 chk("36 the rewrite of it is taken",
     e_buffer_txn_rewrite(&t, &buf, 1, 1, shell, 0, 0, &zero, 1), E_BUFFER_OK);
 chk("36 ... and made", e_buffer_txn_commit(&t), E_BUFFER_OK);
 chk("36 the breakpoint is where it was", scr.brp[0] == 1 && brp_has(1), 1);
 chk("36 the debugger hears of no row", moved_nremoved + moved_nadded, 0);
 chk("36 three lines still", buf.mxlines, 3);
 fixture_free();
}

/* Two records of one step, each on its shell: the second reads what the
   first leaves (the join's line, split again), and the step is made whole
   or not at all. */
#define TWO_BEFORE JOIN_BEFORE
#define TWO_AFTER  "L0\nL1\nL2\nL\n3\nL5\nL6\nL7\n"

static int two_rewrites_changed_nothing(void)
{
 return(strcmp(text_of(), TWO_BEFORE) == 0 && scr.brp[0] == 3 && brp_has(3) && brp_has(4) &&
        brp_has(5) && buf.ud == NULL && buf.rd == NULL && moved_calls == 0);
}

static int two_rewrites_are_whole(void)
{
 Undo *u = buf.ud;

 return(strcmp(text_of(), TWO_AFTER) == 0 && scr.brp[0] == 2 && brp_has(3) && brp_has(5) &&
        u != NULL && u->type == 'a' && u->b.x == 1 && u->b.y == 3 && u->next != NULL &&
        u->next->type == 'r' && u->next->b.y == JOIN_Y0 && u->next->nbrk == 1 &&
        u->next->brk[0] == 1 && u->next->next == NULL && buf.rd == NULL && moved_calls == 2);
}

/* The two items; the view is read before the commit.  Return: the commit's
   result, or the first refusal (the change then aborted). */
static int two_rewrites(char texts[][WIDTH + 2], int *rows, int *n)
{
 Undo join = a_line_break_record(), split = a_join_record(1, JOIN_Y0);
 WpeBufferTxn t;
 int r;

 e_buffer_txn_begin(&t);
 r = a_record_replayed_into(&t, &join, JOIN_Y0, JOIN_SPAN, JOIN_GUARD, JOIN_GUARD);
 if (r == E_BUFFER_OK)
  r = a_record_replayed_into(&t, &split, JOIN_Y0, 1, JOIN_GUARD, JOIN_GUARD);
 if (r != E_BUFFER_OK) {
  e_buffer_txn_abort(&t);
  return(r);
 }
 *n = view_texts(&t, texts, rows, VIEW_ROWS);
 return(e_buffer_txn_commit(&t));
}

static void case_two_rewrites_in_one_transaction(void)
{
 char texts[VIEW_ROWS][WIDTH + 2];
 int got[VIEW_ROWS], n = 0, k, r, bad = 0, whole_from = 0, view_ok = 0;

 for (k = 1; k <= SWEEP_LIMIT && whole_from == 0; k++) {
  join_fixture();
  e_buffer_test_fail_allocs(k);
  r = two_rewrites(texts, got, &n);
  e_buffer_test_fail_allocs(0);
  if (r == E_BUFFER_OK) {
   whole_from = k;
   if (!two_rewrites_are_whole())
    bad++;
   view_ok = view_matches_buffer(texts, got, n);
  } else if (r != E_BUFFER_NOMEM || !two_rewrites_changed_nothing())
   bad++;
  fixture_free();
 }
 chk("37 two rewrites are whole or change nothing", bad, 0);
 chk("37 ... and are made once memory suffices", whole_from > 1, 1);
 chk("37 the view read through both before the commit", view_ok, 1);
}

/* Two items over the same row: the view gives the newer line, with the
   breakpoint the row had before either. */
static void case_the_view_reads_two_items_on_one_row(void)
{
 char texts[VIEW_ROWS][WIDTH + 2];
 int row = 3, got[VIEW_ROWS], n;
 WpeBufferTxn t;

 fixture(8, E_BUFFER_HISTORY_FULL);
 brp_set(1, &row);
 e_buffer_txn_begin(&t);
 e_buffer_txn_splice(&t, &buf, 3, 1, one_line("x3"), 1, E_BUFFER_STEP_COVERED);
 e_buffer_txn_splice(&t, &buf, 3, 1, one_line("y3"), 1, E_BUFFER_STEP_COVERED);
 n = view_texts(&t, texts, got, VIEW_ROWS);
 chk_str("38 the newer line", texts[3], "y3");
 chk("38 ... with the row's breakpoint", got[3], 1);
 chk("38 the change is made", e_buffer_txn_commit(&t), E_BUFFER_OK);
 chk("38 as the view said", view_matches_buffer(texts, got, n), 1);
 fixture_free();
}

/* ---- text between buffers of different widths ---- */

#define NARROW 20
/* A line wider than NARROW and narrower than WIDTH, with blanks to wrap at. */
#define WIDE_TEXT "aaaa bbbb cccc dddd eeee ffff gggg hhhh"

/* The text rows y0.. of b hold, as a file would be written: a row that ends
   without a line break goes on in the next one. */
static const char *rows_text(BUFFER *b, int y0, int n)
{
 static char out[512];
 size_t used = 0;
 int y;

 out[0] = '\0';
 for (y = y0; y < y0 + n && y < b->mxlines && used + b->bf[y].len + 2 < sizeof out; y++) {
  memcpy(out + used, b->bf[y].s, (size_t) b->bf[y].len);
  used += (size_t) b->bf[y].len;
  if (b->bf[y].s[b->bf[y].len] == WPE_WR)
   out[used++] = '\n';
  out[used] = '\0';
 }
 return(out);
}

/* The shape every row of a file has, written from the rule and not from
   the owner: at most Max. Columns (b->mx.x) bytes of text; then the line
   break or the zero of a row whose line goes on below; the zero right after
   the break; the last byte of the row's room (Max. Columns + 2 bytes) zero.
   A row of exactly Max. Columns bytes is one row with its break, which the
   old rule (a byte always left free) did not allow. */
static int row_obeys(const BUFFER *b, const STRING *l)
{
 int hard;

 if (l->s == NULL || l->len < 0 || l->len > b->mx.x)
  return(0);
 if (l->s[l->len] == WPE_WR)
  hard = 1;
 else if (l->s[l->len] == '\0')
  hard = 0;
 else
  return(0);
 if (l->nrc != l->len + hard || l->s[l->nrc] != '\0')
  return(0);
 return(l->s[b->mx.x + 1] == '\0');
}

/* Does every row of b keep the row shape (row_obeys)? */
static int rows_fit(BUFFER *b)
{
 int y;

 for (y = 0; y < b->mxlines; y++)
  if (!row_obeys(b, &b->bf[y]))
   return(0);
 return(1);
}

/* A narrow buffer: a shell NARROW wide, as a window opened after Max.
   Columns was lowered. */
static BUFFER *narrow_buffer(void)
{
 BUFFER *nb = e_buffer_shell_new(&buf, NARROW);

 if (nb == NULL)
  exit(2);
 return(nb);
}

/* A copy into a buffer narrower than the line: every byte and the line break
   arrive, on rows of the narrow width (a paste into a window opened after
   Max. Columns was lowered once dropped the bytes past it). */
static void case_a_copy_into_a_narrower_buffer_keeps_every_byte(void)
{
 WpeBufferTxn t;
 BUFFER *nb;

 fixture(3, E_BUFFER_HISTORY_FULL);
 e_buffer_set_line(&buf, 1, WIDE_TEXT, (int) strlen(WIDE_TEXT));
 nb = narrow_buffer();
 e_buffer_txn_begin(&t);
 e_buffer_txn_copy_lines(&t, nb, 0, &buf, 1, 1, E_BUFFER_STEP_COVERED);
 chk("39 the copy is made", e_buffer_txn_commit(&t), E_BUFFER_OK);
 chk("39 it took more than one row", nb->mxlines > 2, 1);
 chk("39 every row fits the narrow width", rows_fit(nb), 1);
 chk_str("39 every byte and the line break", rows_text(nb, 0, nb->mxlines - 1), WIDE_TEXT "\n");
 chk_str("39 the original is as it was", rows_text(&buf, 1, 1), WIDE_TEXT "\n");
 e_buffer_shell_free(nb);
 fixture_free();
}

/* A move into a buffer narrower than the line: the line is built again at
   the narrow width, with its breakpoint, and leaves the wide buffer (a line
   kept at the wide width would be written past by the first character typed
   into it). */
static void case_a_move_into_a_narrower_buffer_builds_its_line_again(void)
{
 WpeBufferTxn t;
 BUFFER *nb;
 int rows[1] = {1};

 fixture(3, E_BUFFER_HISTORY_FULL);
 e_buffer_set_line(&buf, 1, WIDE_TEXT, (int) strlen(WIDE_TEXT));
 brp_set(1, rows);
 nb = narrow_buffer();
 e_buffer_txn_begin(&t);
 e_buffer_txn_move_lines(&t, &buf, 1, 1, nb, 0);
 chk("40 the move is made", e_buffer_txn_commit(&t), E_BUFFER_OK);
 chk_str("40 the line left", text_of(), "L0\nL2\n");
 chk("40 its breakpoint left with it", scr.brp[0], 0);
 chk("40 it took more than one row", nb->mxlines > 2, 1);
 chk("40 every row fits the narrow width", rows_fit(nb), 1);
 chk_str("40 every byte and the line break", rows_text(nb, 0, nb->mxlines - 1), WIDE_TEXT "\n");
 chk("40 the breakpoint is on its first row",
     nb->f->s->brp != NULL && nb->f->s->brp[0] == 1 && nb->f->s->brp[1] == 0, 1);
 e_buffer_shell_free(nb);
 fixture_free();
}

/* The record of a deletion (as e_del_nchar keeps it, with the deletion
   covered by it) on row @y of the fixture: its text is the row's. */
static Undo *a_deletion_record(int y)
{
 Undo *rec = e_buffer_record_push(&buf, e_buffer_record_new('r'));

 rec->b = pnt(0, y);
 rec->a.x = buf.bf[y].len;
 if (e_buffer_record_text(rec, buf.bf[y].s, buf.bf[y].len) != E_BUFFER_OK)
  exit(2);
 return(rec);
}

/* A character record of @kind on the list, naming (0, @y). */
static Undo *a_char_record_at(int kind, int y)
{
 Undo *rec = e_buffer_record_push(&buf, e_buffer_record_new(kind));

 rec->b = pnt(0, y);
 rec->a.x = 1;
 if (kind == 'r' && e_buffer_record_text(rec, (const unsigned char *) "x", 1) != E_BUFFER_OK)
  exit(2);
 return(rec);
}

/* A character record names the row its text goes back to, and the
   deletion it records may take that row: Ctrl-T on the only word of the
   last line.  After that splice the record names the row just past the
   text and never further; Undo hands its replay a row of the text (the
   owner opens it again on the replay's shell, the text goes back on it and
   the parked breakpoint with it), never the line the splice freed.  A record naming any row further
   out, or one that takes text from the row past the text, is refused and
   nothing is replayed. */
static void case_a_record_never_names_a_freed_row(void)
{
 Undo *rec;
 int row = 3;

 fixture(4, E_BUFFER_HISTORY_FULL);
 brp_set(1, &row);
 rec = a_deletion_record(3);
 e_buffer_cover_begin(&buf, rec);
 e_buffer_delete_lines(&buf, 3, 1, E_BUFFER_STEP_NEW);
 e_buffer_cover_end(&buf);
 chk("row past: the last row went", buf.mxlines, 3);
 chk("row past: the record names the row just past the text", rec->b.y, buf.mxlines);
 chk("row past: the record holds the breakpoint", rec->nbrk == 1 && rec->brk[0] == 0, 1);
 replay_calls = replay_row_missing = 0;
 chk("row past: Undo", e_buffer_undo(&buf, &scr), E_BUFFER_OK);
 chk("row past: the replay ran once", replay_calls, 1);
 chk("row past: on a row of the text", replay_row_missing, 0);
 chk("row past: the row is there again", buf.mxlines, 4);
 chk_str("row past: with the text the replay put back", text_of(), "L0\nL1\nL2\nL3\n");
 chk("row past: its breakpoint is back on it", brp_has(3), 1);
 chk("row past: the record is used up", list_len(buf.ud), 0);
 fixture_free();

 fixture(4, E_BUFFER_HISTORY_FULL);
 a_char_record_at('r', 6);
 chk("row further out: Undo refused", e_buffer_undo(&buf, &scr), E_BUFFER_RANGE);
 chk("row further out: nothing replayed", replay_calls, 0);
 chk("row further out: the text as it was", buf.mxlines, 4);
 chk("row further out: the record stays", list_len(buf.ud), 1);
 fixture_free();

 fixture(4, E_BUFFER_HISTORY_FULL);
 a_char_record_at('a', 4);
 chk("text taken from past the text: Undo refused", e_buffer_undo(&buf, &scr), E_BUFFER_RANGE);
 chk("text taken from past the text: nothing replayed", replay_calls, 0);
 chk("text taken from past the text: the text as it was", buf.mxlines, 4);
 fixture_free();
}

/* A replay that sets the block from a record of a longer text leaves it
   past the end; the owner pulls it back, so the next block command meets
   rows the text has. */
static void case_marks_stand_in_the_text_after_an_undo(void)
{
 fixture(4, E_BUFFER_HISTORY_FULL);
 a_char_record_at('r', 1);
 stub_marks_past = 1;
 chk("marks: Undo", e_buffer_undo(&buf, &scr), E_BUFFER_OK);
 chk("marks: the block begins on a row of the text", scr.mark_begin.y < buf.mxlines, 1);
 chk("marks: and ends on one", scr.mark_end.y, buf.mxlines - 1);
 chk("marks: at the end of the text", scr.mark_end.x, buf.bf[buf.mxlines - 1].len);
 fixture_free();
}

/* ---- Undo and Redo: one step, one change ---- */

/* A join as Backspace at the start of row @y + 1 makes it, played by the
   owner's calls: the 'r' record of the line break at (@x, @y) goes on the
   list, and the line below goes under its cover, its breakpoint parked in
   the record.  (The stub's Undo of it splits row @y at @x again.) */
static Undo *a_join_made(int x, int y)
{
 Undo *rec = e_buffer_record_push(&buf, e_buffer_record_new('r'));

 rec->b = pnt(x, y);
 rec->a.x = 1;
 if (e_buffer_record_text(rec, (const unsigned char *) "\n", 1) != E_BUFFER_OK)
  exit(2);
 e_buffer_cover_begin(&buf, rec);
 e_buffer_delete_lines(&buf, y + 1, 1, E_BUFFER_STEP_NEW);
 e_buffer_cover_end(&buf);
 return(rec);
}

/* What the Undo of one join takes: as case 24's. */
#define JOIN_UNDO_ALLOCATIONS POCKET_UNDO_ALLOCATIONS

/* Undo of a joined line whose breakpoint the join parked in its record,
   with memory refused at each allocation in turn: refused, the line stays
   joined, the record keeps the breakpoint, Redo has nothing and the cursor
   is where the user had it (an Undo once used up its record, left the line
   joined and put the breakpoint on a line the user never marked); whole,
   the line is back with its breakpoint and Redo has the join again. */
static void case_a_refused_undo_of_a_join_keeps_the_breakpoint_parked(void)
{
 char before[4096];
 int k, r, row = 4, bad = 0, whole_from = 0;
 Undo *rec;

 for (k = 1; k <= SWEEP_LIMIT && whole_from == 0; k++) {
  fixture(8, E_BUFFER_HISTORY_FULL);
  brp_set(1, &row);
  rec = a_join_made(2, 3);
  buf.b = pnt(1, 6);
  snprintf(before, sizeof before, "%s", text_of());
  e_buffer_test_fail_allocs(k);
  r = e_buffer_undo(&buf, &scr);
  e_buffer_test_fail_allocs(0);
  if (r == E_BUFFER_OK) {
   whole_from = k;
   if (!(buf.mxlines == 8 && scr.brp[0] == 1 && brp_has(4) && buf.ud == NULL &&
         buf.rd != NULL && buf.rd->type == 'a' && buf.rd->b.y == 3 && buf.rd->next == NULL))
    bad++;
  } else if (r != E_BUFFER_NOMEM || strcmp(text_of(), before) != 0 || buf.mxlines != 7 ||
             scr.brp[0] != 0 || buf.ud != rec || rec->nbrk != 1 || rec->brk[0] != 1 ||
             buf.rd != NULL || buf.b.x != 1 || buf.b.y != 6 || nomem_calls != 1)
   bad++;
  fixture_free();
 }
 chk("47 an Undo of a join is whole or changes nothing", bad, 0);
 chk("47 ... and is made once memory suffices", whole_from - 1, JOIN_UNDO_ALLOCATIONS);

 fixture(8, E_BUFFER_HISTORY_FULL);           /* memory short for the primitive alone */
 brp_set(1, &row);
 rec = a_join_made(2, 3);
 snprintf(before, sizeof before, "%s", text_of());
 stub_refuses = 1;
 r = e_buffer_undo(&buf, &scr);
 stub_refuses = 0;
 chk("47 a primitive refused on its shell refuses the Undo", r, E_BUFFER_NOMEM);
 chk("47 ... which changes nothing", strcmp(text_of(), before) == 0 && buf.ud == rec &&
     rec->nbrk == 1 && buf.rd == NULL && scr.brp[0] == 0, 1);
 chk("47 ... and says so once", nomem_calls, 1);
 fixture_free();
}

/* A step of two joins (two Backspaces kept as one action), its Undo refused
   at each allocation in turn: refused, neither line comes back (an Undo
   once made the first and said the text was unchanged); whole, both are
   back with their breakpoints and Redo has both joins, one step. */
#define TWO_JOINS_ALLOCATIONS (2 * POCKET_UNDO_ALLOCATIONS - 1)

static void case_a_refused_undo_of_a_two_record_step_changes_nothing(void)
{
 char before[4096];
 int k, r, rows[2] = {2, 5}, bad = 0, whole_from = 0, redone = 0;

 for (k = 1; k <= SWEEP_LIMIT && whole_from == 0; k++) {
  fixture(8, E_BUFFER_HISTORY_FULL);
  brp_set(2, rows);
  e_buffer_step_begin(&buf);
  a_join_made(2, 1);
  a_join_made(2, 3);
  e_buffer_step_end(&buf);
  snprintf(before, sizeof before, "%s", text_of());
  e_buffer_test_fail_allocs(k);
  r = e_buffer_undo(&buf, &scr);
  e_buffer_test_fail_allocs(0);
  if (r == E_BUFFER_OK) {
   whole_from = k;
   if (!(buf.mxlines == 8 && scr.brp[0] == 2 && brp_has(2) && brp_has(5) && buf.ud == NULL &&
         list_len(buf.rd) == 2 && (buf.rd->flags & E_UNDO_MORE) != 0 &&
         (buf.rd->next->flags & E_UNDO_MORE) == 0))
    bad++;
   redone = (e_buffer_redo(&buf, &scr) == E_BUFFER_OK && buf.mxlines == 6 &&
             scr.brp[0] == 0 && buf.rd == NULL && list_len(buf.ud) == 2);
  } else if (r != E_BUFFER_NOMEM || strcmp(text_of(), before) != 0 || buf.mxlines != 6 ||
             scr.brp[0] != 0 || list_len(buf.ud) != 2 || buf.rd != NULL || nomem_calls != 1)
   bad++;
  fixture_free();
 }
 chk("48 an Undo of a step of two joins is whole or changes nothing", bad, 0);
 chk("48 ... and is made once memory suffices", whole_from - 1, TWO_JOINS_ALLOCATIONS);
 chk("48 one Redo makes both joins again", redone, 1);
}

/* Case 7's step of three line records, its Undo refused at each allocation
   in turn: all three come back, or none. */
static void case_a_step_of_line_records_is_one_transaction(void)
{
 char before[4096], after[4096];
 int k, r, bad = 0, whole_from = 0;

 for (k = 1; k <= SWEEP_LIMIT && whole_from == 0; k++) {
  fixture(8, E_BUFFER_HISTORY_FULL);
  snprintf(before, sizeof before, "%s", text_of());
  e_buffer_step_begin(&buf);
  e_buffer_delete_lines(&buf, 1, 1, E_BUFFER_STEP_NEW);
  e_buffer_insert_blank(&buf, 4, E_BUFFER_STEP_NEW);
  e_buffer_delete_lines(&buf, 6, 1, E_BUFFER_STEP_NEW);
  e_buffer_step_end(&buf);
  snprintf(after, sizeof after, "%s", text_of());
  e_buffer_test_fail_allocs(k);
  r = e_buffer_undo(&buf, &scr);
  e_buffer_test_fail_allocs(0);
  if (r == E_BUFFER_OK) {
   whole_from = k;
   if (strcmp(text_of(), before) != 0 || buf.ud != NULL || list_len(buf.rd) != 3)
    bad++;
  } else if (r != E_BUFFER_NOMEM || strcmp(text_of(), after) != 0 || list_len(buf.ud) != 3 ||
             buf.rd != NULL)
   bad++;
  fixture_free();
 }
 chk("49 a step of line records is undone whole or not at all", bad, 0);
 chk("49 ... and once memory suffices", whole_from > 1, 1);
}

/* A character change run through the owner's entry (what a block deleted
   inside a line goes through), memory refused at each allocation in turn:
   refused, the text, the breakpoint and both lists are as they were and the
   cursor stays; whole, the line is joined, its breakpoint is parked in the
   one record the change leaves on the Undo list, and Redo has nothing. */
static void case_a_character_change_through_the_owner_is_whole_or_nothing(void)
{
 char before[4096];
 int k, r, row = 4, bad = 0, whole_from = 0;
 Undo as_if;

 memset(&as_if, 0, sizeof(as_if));
 as_if.type = 'a';
 as_if.b = pnt(2, 3);
 as_if.a.x = 1;
 for (k = 1; k <= SWEEP_LIMIT && whole_from == 0; k++) {
  fixture(8, E_BUFFER_HISTORY_FULL);
  brp_set(1, &row);
  buf.b = pnt(1, 6);
  snprintf(before, sizeof before, "%s", text_of());
  e_buffer_test_fail_allocs(k);
  r = e_buffer_apply_chars(&buf, &scr, &as_if);
  e_buffer_test_fail_allocs(0);
  if (r == E_BUFFER_OK) {
   whole_from = k;
   if (!(buf.mxlines == 7 && scr.brp[0] == 0 && list_len(buf.ud) == 1 &&
         buf.ud->type == 'r' && buf.ud->nbrk == 1 && buf.rd == NULL))
    bad++;
  } else if (r != E_BUFFER_NOMEM || strcmp(text_of(), before) != 0 || buf.mxlines != 8 ||
             scr.brp[0] != 1 || !brp_has(4) || buf.ud != NULL || buf.rd != NULL ||
             buf.b.x != 1 || buf.b.y != 6)
   bad++;
  fixture_free();
 }
 chk("51 a character change through the owner is whole or changes nothing", bad, 0);
 chk("51 ... and is made once memory suffices", whole_from > 1, 1);

 fixture(8, E_BUFFER_HISTORY_FULL);              /* refused on the shell alone */
 snprintf(before, sizeof before, "%s", text_of());
 stub_refuses = 1;
 r = e_buffer_apply_chars(&buf, &scr, &as_if);
 stub_refuses = 0;
 chk("51 a primitive refused on its shell refuses the change", r, E_BUFFER_NOMEM);
 chk("51 ... which changes nothing", strcmp(text_of(), before) == 0 && buf.ud == NULL, 1);
 as_if.b = pnt(0, 20);
 replay_calls = 0;
 chk("51 a row past the text is refused", e_buffer_apply_chars(&buf, &scr, &as_if),
     E_BUFFER_RANGE);
 chk("51 ... without running the primitive", replay_calls, 0);
 fixture_free();
}

/* A block record inside a step (no command makes one; a hook could): the
   step ends before it, traced, and the block record is a step of its own. */
static void case_a_block_record_is_alone_in_its_step(void)
{
 fixture(8, E_BUFFER_HISTORY_FULL);
 e_buffer_step_begin(&buf);
 e_buffer_record_push(&buf, e_buffer_record_new('c'));
 e_buffer_delete_lines(&buf, 2, 1, E_BUFFER_STEP_NEW);
 e_buffer_step_end(&buf);
 chk("50 the line record is in the block's step",
     (buf.ud->flags & E_UNDO_MORE) != 0 && buf.ud->next->type == 'c', 1);
 chk("50 Undo", e_buffer_undo(&buf, &scr), E_BUFFER_OK);
 chk("50 takes the line record back", buf.mxlines, 8);
 chk("50 ... alone, traced", block_in_step_traced, 1);
 chk("50 the block record stays", list_len(buf.ud) == 1 && buf.ud->type == 'c', 1);
 chk("50 not replayed yet", block_calls, 0);
 chk("50 the next Undo", e_buffer_undo(&buf, &scr), E_BUFFER_OK);
 chk("50 replays the block record", block_calls, 1);
 chk("50 nothing left", buf.ud == NULL, 1);
 fixture_free();
}

/* ---- the width of a row ---- */

#define HALF_WIDTH (WIDTH / 2)
#define SPLIT_TEXT_MAX (4 * WIDTH + 8)

/* @n bytes of @c, then @tail: a line built for a row-width case. */
static const char *repeated(int c, int n, const char *tail)
{
 static char out[SPLIT_TEXT_MAX];

 memset(out, c, (size_t) n);
 snprintf(out + n, sizeof out - (size_t) n, "%s", tail);
 return(out);
}

/* The buffer holds @text now; every row keeps the row shape, by the rule
   and by the owner's own check, and the text saved is @text again.
   @what: the case's name; @rows: the rows the text must take. */
static void chk_text_rows(const char *what, const char *text, int rows)
{
 char name[128];

 snprintf(name, sizeof name, "%s: taken", what);
 chk(name, e_buffer_replace_all(&buf, text, E_BUFFER_STEP_NEW), E_BUFFER_OK);
 snprintf(name, sizeof name, "%s: rows", what);
 chk(name, buf.mxlines, rows);
 snprintf(name, sizeof name, "%s: every row fits", what);
 chk(name, rows_fit(&buf), 1);
 snprintf(name, sizeof name, "%s: the owner agrees", what);
 chk(name, e_buffer_rows_check(&buf), -1);
 snprintf(name, sizeof name, "%s: every byte", what);
 chk_str(name, rows_text(&buf, 0, buf.mxlines), text);
}

/* Lines read or pasted, as the one wrap rule cuts them. */
static void case_text_cut_into_rows_fits_the_width(void)
{
 char astride[WIDTH + 8];

 fixture(1, E_BUFFER_HISTORY_FULL);
 chk_text_rows("41 a line one short of Max. Columns", repeated('x', WIDTH - 1, "\n"), 1);
 chk_text_rows("41 a line of Max. Columns is one row", repeated('x', WIDTH, "\n"), 1);
 chk("41 ... with its line break", buf.bf[0].s[WIDTH], WPE_WR);
 chk_text_rows("41 one byte over wraps that byte", repeated('x', WIDTH + 1, "\n"), 2);
 chk("41 ... the first row full", buf.bf[0].len, WIDTH);
 chk_text_rows("41 two full rows", repeated('x', 2 * WIDTH, "\n"), 2);
 chk_text_rows("41 a blank to wrap at",
               repeated('a', WIDTH - 10, " bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"), 2);
 chk("41 ... cut after the blank", buf.bf[0].len, WIDTH - 9);
 memset(astride, 'x', WIDTH - 1);
 snprintf(astride + WIDTH - 1, sizeof astride - (WIDTH - 1), "\xc3\xa9y\n");
 chk_text_rows("41 a two-byte character astride the margin", astride, 2);
 chk("41 ... the character whole on the second row", buf.bf[1].s[0], 0xc3);
 fixture_free();
}

/* The other makers of rows: parts joined, Enter, a copy, a shell. */
static void case_every_row_a_maker_makes_fits_the_width(void)
{
 WpeBufferPart parts[2];
 STRING *lines;
 BUFFER *other;
 WpeBufferTxn t;
 int n, x, xs[4] = {0, 1, WIDTH - 1, WIDTH}, i;
 char joined[WIDTH + 2];

 memset(joined, 'j', WIDTH);
 joined[WIDTH] = WPE_WR;
 parts[0].s = (const unsigned char *) joined;
 parts[0].n = HALF_WIDTH;
 parts[1].s = (const unsigned char *) joined + HALF_WIDTH;
 parts[1].n = WIDTH - HALF_WIDTH + 1;
 lines = e_buffer_lines_from_parts(WIDTH, parts, 2, &n);
 chk("42 parts joined to exactly Max. Columns are one row", n, 1);
 if (n == 1) {
  chk("42 ... of Max. Columns", lines[0].len, WIDTH);
  chk("42 ... with its break", lines[0].s[WIDTH], WPE_WR);
 }
 e_buffer_lines_free(lines, n);

 for (i = 0; i < 4; i++) {
  x = xs[i];
  fixture(1, E_BUFFER_HISTORY_FULL);
  e_buffer_replace_all(&buf, repeated('s', 2 * WIDTH, "\n"), E_BUFFER_STEP_NEW);
  chk("42 a split of a full soft row is made",
      e_buffer_split_line(&buf, 0, x, E_BUFFER_STEP_NEW, E_BUFFER_HEAD_WR), E_BUFFER_OK);
  chk("42 ... every row fits", rows_fit(&buf), 1);
  chk("42 ... the owner agrees", e_buffer_rows_check(&buf), -1);
  fixture_free();
  fixture(1, E_BUFFER_HISTORY_FULL);
  e_buffer_replace_all(&buf, repeated('h', WIDTH, "\ntail\n"), E_BUFFER_STEP_NEW);
  chk("42 a split of a full hard row is made",
      e_buffer_split_line(&buf, 0, x, E_BUFFER_STEP_NEW, E_BUFFER_HEAD_WR), E_BUFFER_OK);
  chk("42 ... every row fits", rows_fit(&buf), 1);
  chk("42 ... the line break went with the tail", buf.mxlines, 3);
  fixture_free();
 }

 fixture(1, E_BUFFER_HISTORY_FULL);
 e_buffer_replace_all(&buf, repeated('c', WIDTH, "\n"), E_BUFFER_STEP_NEW);
 other = e_buffer_shell_new(&buf, WIDTH);
 e_buffer_txn_begin(&t);
 e_buffer_txn_copy_lines(&t, other, 0, &buf, 0, 1, E_BUFFER_STEP_COVERED);
 chk("42 a full row copied into the same width", e_buffer_txn_commit(&t), E_BUFFER_OK);
 chk("42 ... every row fits", rows_fit(other), 1);
 chk_str("42 ... every byte and the break", rows_text(other, 0, 1), repeated('c', WIDTH, "\n"));
 e_buffer_shell_free(other);
 other = e_buffer_shell_new(&buf, HALF_WIDTH);
 e_buffer_txn_begin(&t);
 e_buffer_txn_copy_lines(&t, other, 0, &buf, 0, 1, E_BUFFER_STEP_COVERED);
 chk("42 a full row copied into half the width", e_buffer_txn_commit(&t), E_BUFFER_OK);
 chk("42 ... every row fits", rows_fit(other), 1);
 chk_str("42 ... every byte and the break", rows_text(other, 0, 2), repeated('c', WIDTH, "\n"));
 e_buffer_shell_free(other);
 other = e_buffer_shell_new(&buf, WIDTH);
 e_buffer_txn_begin(&t);
 chk("42 a shell filled over a full row",
     e_buffer_shell_fill(other, &t, &buf, 0, 1), E_BUFFER_OK);
 e_buffer_txn_abort(&t);
 chk("42 ... every row fits", rows_fit(other), 1);
 e_buffer_shell_free(other);
 chk("42 a row one byte too wide is refused",
     e_buffer_set_line_ended(&buf, 0, repeated('w', WIDTH + 1, ""), WIDTH + 1, WPE_WR),
     E_BUFFER_RANGE);
 chk_str("42 ... the text unchanged", text_of(), repeated('c', WIDTH, "\n"));
 e_buffer_mark_view(&buf);
 chk("42 ... a view takes it",
     e_buffer_set_line_ended(&buf, 0, repeated('w', WIDTH + 1, ""), WIDTH + 1, WPE_WR),
     E_BUFFER_OK);
 fixture_free();
}

/* A row the owner did not make, one byte past Max. Columns: refused whole,
   and the trace names it, so the check has teeth. */
static STRING *a_row_one_byte_too_wide(void)
{
 STRING *wide = malloc(sizeof(STRING));

 if (wide == NULL || (wide->s = calloc(WIDTH + 4, 1)) == NULL)
  exit(2);
 memset(wide->s, 'w', WIDTH + 1);
 wide->s[WIDTH + 1] = WPE_WR;
 wide->len = WIDTH + 1;
 wide->nrc = WIDTH + 2;
 return(wide);
}

static void case_a_row_that_does_not_fit_is_refused(void)
{
 STRING *wide;
 WpeBufferTxn t;
 int lines, r;

 fixture(3, E_BUFFER_HISTORY_FULL);
 wide = a_row_one_byte_too_wide();
 lines = buf.mxlines;
 r = e_buffer_splice(&buf, 1, 0, wide, 1, NULL, 0, E_BUFFER_STEP_NEW);
 chk("43 a row too wide is refused", r, E_BUFFER_RANGE);
 if (r != E_BUFFER_OK)
  free(wide->s);
 free(wide);
 chk("43 ... no line added", buf.mxlines, lines);
 chk_str("43 ... the text as it was", text_of(), "L0\nL1\nL2\n");
 chk("43 ... nothing to undo", list_len(buf.ud), 0);
 chk("43 ... the trace says why", row_wider_traced, 1);
 fixture_free();
 fixture(3, E_BUFFER_HISTORY_FULL);
 e_buffer_txn_begin(&t);
 wide = a_row_one_byte_too_wide();
 chk("43 a transaction refuses it too",
     e_buffer_txn_splice(&t, &buf, 1, 1, wide, 1, E_BUFFER_STEP_COVERED), E_BUFFER_RANGE);
 chk("43 ... and is refused whole", e_buffer_txn_commit(&t), E_BUFFER_RANGE);
 chk_str("43 ... the text as it was", text_of(), "L0\nL1\nL2\n");
 fixture_free();
}

/* Enter at the end of a full row without a line break: the row takes the
   break inside its own room, and the row below is the rest of the line. */
static void case_enter_at_the_end_of_a_full_soft_row_gives_it_its_break(void)
{
 fixture(1, E_BUFFER_HISTORY_FULL);
 e_buffer_replace_all(&buf, repeated('a', WIDTH, "tail\n"), E_BUFFER_STEP_NEW);
 chk("44 a full soft row and its rest", buf.mxlines, 2);
 chk("44 Enter at its end",
     e_buffer_split_line(&buf, 0, WIDTH, E_BUFFER_STEP_NEW, E_BUFFER_HEAD_WR), E_BUFFER_OK);
 chk("44 ... no row added", buf.mxlines, 2);
 chk("44 ... the row hard", buf.bf[0].s[WIDTH], WPE_WR);
 chk("44 ... every byte on it", buf.bf[0].len, WIDTH);
 chk("44 ... counted with its break", buf.bf[0].nrc, WIDTH + 1);
 chk("44 ... the zero after the break", buf.bf[0].s[WIDTH + 1], '\0');
 chk_str("44 ... the text", text_of(), repeated('a', WIDTH, "\ntail\n"));
 fixture_free();
 fixture(1, E_BUFFER_HISTORY_FULL);
 e_buffer_replace_all(&buf, repeated('a', WIDTH, "\n"), E_BUFFER_STEP_NEW);
 chk("44 Enter at the end of a full hard row",
     e_buffer_split_line(&buf, 0, WIDTH, E_BUFFER_STEP_NEW, E_BUFFER_HEAD_WR), E_BUFFER_OK);
 chk("44 ... a row added", buf.mxlines, 2);
 chk("44 ... empty", buf.bf[1].len, 0);
 chk("44 ... and hard", buf.bf[1].s[0], WPE_WR);
 chk("44 ... every row fits", rows_fit(&buf), 1);
 fixture_free();
}

/* A row of a view wider than the window, copied into a file's buffer of
   the same width: wrapped again, every byte and the break kept. */
static void case_a_wide_view_row_copied_into_a_file_keeps_every_byte(void)
{
 WpeBufferTxn t;
 BUFFER *file;
 const char *wide = repeated('v', 2 * WIDTH, "");
 char want[2 * WIDTH + 2];

 snprintf(want, sizeof want, "%s\n", wide);
 fixture(2, E_BUFFER_HISTORY_FULL);
 e_buffer_mark_view(&buf);
 chk("45 a view holds a row twice the width",
     e_buffer_set_line(&buf, 0, wide, 2 * WIDTH), E_BUFFER_OK);
 file = e_buffer_shell_new(&buf, WIDTH);
 e_buffer_txn_begin(&t);
 e_buffer_txn_copy_lines(&t, file, 0, &buf, 0, 1, E_BUFFER_STEP_COVERED);
 chk("45 the copy is made", e_buffer_txn_commit(&t), E_BUFFER_OK);
 chk("45 ... every row fits", rows_fit(file), 1);
 chk("45 ... the owner agrees", e_buffer_rows_check(file), -1);
 chk_str("45 ... every byte and the break", rows_text(file, 0, file->mxlines - 1), want);
 e_buffer_shell_free(file);
 fixture_free();
}

/* The owner's check and the rule above say the same of a good buffer and
   of a bad one. */
static void case_the_owners_check_agrees_with_the_rule(void)
{
 fixture(3, E_BUFFER_HISTORY_FULL);
 chk("46 a good buffer: the rule", rows_fit(&buf), 1);
 chk("46 a good buffer: the owner", e_buffer_rows_check(&buf), -1);
 memset(buf.bf[1].s, 'z', WIDTH + 1);
 buf.bf[1].s[WIDTH + 1] = '\0';
 buf.bf[1].len = WIDTH + 1;
 buf.bf[1].nrc = WIDTH + 1;
 chk("46 a row past the width: the rule", row_obeys(&buf, &buf.bf[1]), 0);
 chk("46 a row past the width: the owner", e_buffer_rows_check(&buf), 1);
 buf.bf[1].s[2] = WPE_WR;
 buf.bf[1].s[3] = 'z';
 buf.bf[1].len = 2;
 buf.bf[1].nrc = 3;
 chk("46 bytes after the break: the rule", row_obeys(&buf, &buf.bf[1]), 0);
 chk("46 bytes after the break: the owner", e_buffer_rows_check(&buf), 1);
 e_buffer_mark_view(&buf);
 chk("46 a view is not checked", e_buffer_rows_check(&buf), -1);
 fixture_free();
}

/* ---- an editing action of several pieces ---- */

/* The action of these cases works on rows 3 and 4 of "L0".."L7": its span
   runs to the first hard row after its last (5, every row here is hard), so
   the shell holds L2 (a guard), L3, L4, L5 and L6 (a guard).  Its pieces
   split L3 after its first byte and then take L5 out under the cover of an
   'r' record, as a join does, each pushing its record on the shell. */
#define ACTION_Y0      3
#define ACTION_Y1      4
#define ACTION_TOP     2                    /* the shell's row 0 on the user's rows */
#define ACTION_SPAN    3
#define ACTION_WHAT    "the test action"
#define ACTION_BEFORE  "L0\nL1\nL2\nL3\nL4\nL5\nL6\nL7\n"
#define ACTION_AFTER   "L0\nL1\nL2\nL\n3\nL4\nL6\nL7\n"

/* What a whole action takes from the owner: the shell (6), its fill (the
   lines and rows arrays, five lines, the splice's added rows and room on
   its margin: 9), the split (its 'a' record, its new line and the rows it
   moves: 3), the join (its 'r' record and text, the deletion's removed rows
   and the pocket's room: 4), the rows of the rewrite (1), the rewrite item
   (its rows, removed, added: 3) and the room on the window's margin (1). */
#define ACTION_ALLOCATIONS 27

static void action_fixture(void)
{
 int rows[2] = {4, 5};

 fixture(8, E_BUFFER_HISTORY_FULL);
 brp_set(2, rows);
 buf.b = pnt(1, 3);
}

/* The pieces, on the shell's rows: they leave the cursor at the start of
   "3", the second half of the split line. */
static void action_pieces(BUFFER *shell)
{
 stub_split(shell, 1, ACTION_Y0 - ACTION_TOP);
 stub_join(shell, 0, 3);
 shell->b = pnt(0, 2);
}

/* The action as an editing command runs it: inside a step of the user's
   buffer, begun, its pieces on the shell, committed.  Return: what the
   action returned. */
static int action_run(void)
{
 WpeBufferAction a;
 int r;

 e_buffer_step_begin(&buf);
 r = e_buffer_action_begin(&a, &buf, ACTION_Y0, ACTION_Y1, E_BUFFER_FAULT_NONE, ACTION_WHAT);
 if (r == E_BUFFER_OK) {
  action_pieces(a.shell);
  r = e_buffer_action_commit(&a);
 }
 e_buffer_step_end(&buf);
 return(r);
}

static int action_is_whole(void)
{
 Undo *u = buf.ud;

 return(strcmp(text_of(), ACTION_AFTER) == 0 && scr.brp[0] == 1 && brp_has(5) &&
        list_len(u) == 2 && u->type == 'r' && u->b.y == 5 && (u->flags & E_UNDO_MORE) != 0 &&
        u->nbrk == 1 && u->next->type == 'a' && u->next->b.x == 1 && u->next->b.y == 3 &&
        (u->next->flags & E_UNDO_MORE) == 0 && buf.rd == NULL && buf.b.x == 0 &&
        buf.b.y == 4 && win.save > 0 && moved_calls == 1);
}

static int action_changed_nothing(void)
{
 return(strcmp(text_of(), ACTION_BEFORE) == 0 && scr.brp[0] == 2 && brp_has(4) &&
        brp_has(5) && buf.ud == NULL && buf.rd == NULL && buf.b.x == 1 && buf.b.y == 3 &&
        moved_calls == 0 && win.save == 0);
}

/* The rows, the breakpoints, both records (oldest first, MORE on the
   newer: one Ctrl-U), the cursor and the modified count arrive together. */
static void case_an_action_runs_on_a_shell_and_commits_whole(void)
{
 action_fixture();
 chk("52 an action of two pieces is made", action_run(), E_BUFFER_OK);
 chk("52 ... whole: rows, breakpoint, records, cursor", action_is_whole(), 1);
 chk("52 ... nothing said", nomem_calls, 0);
 chk("52 ... the shell's records traced there, then once on the user's list",
     shell_record_traced * 10 + user_record_traced, 22);
 fixture_free();
}

/* A piece refused on the shell refuses the whole action: nothing of the
   user's changes and the status row says so once. */
static void case_an_action_refused_on_its_shell_changes_nothing_and_says_so_once(void)
{
 WpeBufferAction a;

 action_fixture();
 e_buffer_step_begin(&buf);
 chk("53 begun", e_buffer_action_begin(&a, &buf, ACTION_Y0, ACTION_Y1, E_BUFFER_FAULT_NONE,
                                         ACTION_WHAT), E_BUFFER_OK);
 action_pieces(a.shell);
 e_buffer_shell_refused(a.shell);
 chk("53 a piece refused on the shell refuses the action", e_buffer_action_commit(&a),
     E_BUFFER_NOMEM);
 e_buffer_step_end(&buf);
 chk("53 ... which changes nothing", action_changed_nothing(), 1);
 chk("53 ... and says so once", nomem_calls, 1);
 chk("53 ... with no shell left", a.shell == NULL, 1);
 fixture_free();
}

/* Memory refused at each allocation of the action in turn: refused, the
   user's text, breakpoints, lists and cursor as they were and said once;
   from the last allocation on, whole. */
static void case_an_action_refused_for_memory_at_each_allocation(void)
{
 int k, r, bad = 0, refused = 0, whole_from = 0;

 for (k = 1; k <= SWEEP_LIMIT && whole_from == 0; k++) {
  action_fixture();
  e_buffer_test_fail_allocs(k);
  r = action_run();
  e_buffer_test_fail_allocs(0);
  if (r == E_BUFFER_OK) {
   whole_from = k;
   if (!action_is_whole() || nomem_calls != 0)
    bad++;
  } else {
   refused++;
   if (r != E_BUFFER_NOMEM || !action_changed_nothing() || nomem_calls != 1)
    bad++;
  }
  fixture_free();
 }
 chk("54 an action is whole or changes nothing, said once", bad, 0);
 chk("54 every cut before its last allocation refuses it", refused, ACTION_ALLOCATIONS);
 chk("54 the action takes this many allocations", whole_from - 1, ACTION_ALLOCATIONS);
}

/* The span of an action whose last row goes on without a line break runs
   to the row that ends the line, with a guard on each side. */
static void case_an_action_span_runs_to_the_first_hard_row_after_its_last(void)
{
 WpeBufferAction a;
 char text[SPLIT_TEXT_MAX + 16], before[4096];

 fixture(1, E_BUFFER_HISTORY_FULL);
 snprintf(text, sizeof text, "L0\n%sL4\nL5\n", repeated('s', 2 * WIDTH + 5, "\n"));
 e_buffer_replace_all(&buf, text, E_BUFFER_STEP_NEW);
 e_buffer_history_drop(&buf);
 snprintf(before, sizeof before, "%s", text_of());
 chk("55 a line of three rows between two lines", buf.mxlines, 6);
 chk("55 begun on its first row", e_buffer_action_begin(&a, &buf, 1, 1, E_BUFFER_FAULT_NONE,
                                                         ACTION_WHAT), E_BUFFER_OK);
 chk("55 the span runs to the row that ends the line", a.y0 * 10 + a.n_del, 13);
 chk("55 ... a guard above and below", a.g_top * 10 + a.g_bot, 11);
 chk("55 the shell holds the span and its guards", a.shell->mxlines, 5);
 e_buffer_action_abort(&a);
 chk("55 an abort leaves no shell", a.shell == NULL, 1);
 chk_str("55 ... and the text as it was", text_of(), before);
 chk("55 ... nothing to undo, nothing said", list_len(buf.ud) * 10 + nomem_calls, 0);
 chk("55 rows past the text are refused", e_buffer_action_begin(&a, &buf, 4, 6,
     E_BUFFER_FAULT_NONE, ACTION_WHAT), E_BUFFER_RANGE);
 chk("55 ... traced", rows_past_traced, 1);
 chk("55 ... with no shell", a.shell == NULL, 1);
 fixture_free();
}

/* An action over every row of the text has no guard rows: its rewrite
   covers the whole text. */
static void case_an_action_over_the_whole_text_has_no_guards(void)
{
 WpeBufferAction a;

 fixture(3, E_BUFFER_HISTORY_FULL);
 chk("56 begun over the whole text", e_buffer_action_begin(&a, &buf, 0, 2, E_BUFFER_FAULT_NONE,
                                                            ACTION_WHAT), E_BUFFER_OK);
 chk("56 no guard", a.g_top * 10 + a.g_bot, 0);
 chk("56 the span is every row", a.y0 * 10 + a.n_del, 3);
 stub_split(a.shell, 1, 0);
 chk("56 committed", e_buffer_action_commit(&a), E_BUFFER_OK);
 chk_str("56 the text is the shell's", text_of(), "L\n0\nL1\nL2\n");
 chk("56 its record on the list", list_len(buf.ud) == 1 && buf.ud->type == 'a', 1);
 fixture_free();
}

/* A piece that puts in more than a row (a long New Text) records an 'R' on
   the shell: it arrives on the user's list on the user's rows, traced once
   there, and its Ctrl-U puts the old rows back.  @a_y, @e_y: the marks the
   window has before the action; the record keeps them, moved, or unset. */
static void action_with_a_line_record(int a_y, int e_y, int want_a_y, int want_e_y,
                                      const char *what)
{
 WpeBufferAction a;
 STRING two[2];
 char name[128];

 fixture(8, E_BUFFER_HISTORY_FULL);
 scr.mark_begin = pnt(0, a_y);
 scr.mark_end = pnt(1, e_y);
 e_buffer_step_begin(&buf);
 e_buffer_action_begin(&a, &buf, 2, 2, E_BUFFER_FAULT_NONE, ACTION_WHAT);
 two[0] = line_of(a.shell, "long1");
 two[1] = line_of(a.shell, "long2");
 snprintf(name, sizeof name, "%s: the splice on the shell records", what);
 chk(name, e_buffer_splice(a.shell, 1, 1, two, 2, NULL, 0, E_BUFFER_STEP_NEW), E_BUFFER_OK);
 snprintf(name, sizeof name, "%s: traced as a shell's record", what);
 chk(name, shell_record_traced * 10 + user_record_traced, 10);
 snprintf(name, sizeof name, "%s: committed", what);
 chk(name, e_buffer_action_commit(&a), E_BUFFER_OK);
 e_buffer_step_end(&buf);
 snprintf(name, sizeof name, "%s: traced once on the user's list", what);
 chk(name, user_record_traced, 1);
 snprintf(name, sizeof name, "%s: the text", what);
 chk_str(name, text_of(), "L0\nL1\nlong1\nlong2\nL3\nL4\nL5\nL6\nL7\n");
 snprintf(name, sizeof name, "%s: one line record on the user's rows", what);
 chk(name, list_len(buf.ud) == 1 && buf.ud->type == E_UNDO_LINES && buf.ud->span == 2 &&
     buf.ud->n_old == 1 && buf.ud->n_new == 2, 1);
 snprintf(name, sizeof name, "%s: the block it saw", what);
 chk(name, buf.ud->a.y * 100 + buf.ud->e.y, want_a_y * 100 + want_e_y);
 snprintf(name, sizeof name, "%s: Ctrl-U", what);
 chk(name, e_buffer_undo(&buf, &scr), E_BUFFER_OK);
 snprintf(name, sizeof name, "%s: puts the old row back", what);
 chk_str(name, text_of(), ACTION_BEFORE);
 fixture_free();
}

static void case_an_action_keeps_a_line_record_of_its_shell(void)
{
 action_with_a_line_record(2, 3, 2, 3, "57 a block on the span");
}

static void case_a_line_record_of_a_shell_keeps_unset_marks_unset(void)
{
 action_with_a_line_record(-1, -1, -1, -1, "58 no block");
}

/* A window the user cannot change refuses the action before it takes any
   memory: said once, nothing begun. */
static void case_a_read_only_buffer_refuses_an_action_before_its_shell(void)
{
 WpeBufferAction a;

 fixture(8, E_BUFFER_HISTORY_FULL);
 win.ins = WPE_INS_READONLY;
 e_buffer_test_fail_allocs(1);            /* any owner allocation would refuse it for memory */
 chk("59 a read-only window refuses the action",
     e_buffer_action_begin(&a, &buf, 2, 3, E_BUFFER_FAULT_NONE, ACTION_WHAT), E_BUFFER_LOCKED);
 e_buffer_test_fail_allocs(0);
 chk("59 ... says so once", read_only_calls, 1);
 chk("59 ... not as memory", nomem_calls, 0);
 chk("59 ... with no shell", a.shell == NULL, 1);
 chk_str("59 ... the text as it was", text_of(), ACTION_BEFORE);
 win.ins = 0;
 fixture_free();
}

/* Ctrl-K I or U over a block inside one row shifts no line: nothing is
   begun, no memory is taken, no fault fires, nothing is traced. */
static void case_an_empty_shift_begins_nothing(void)
{
 WpeBufferAction a;
 int token;

 fixture(8, E_BUFFER_HISTORY_FULL);
 e_buffer_test_fault_arm(E_BUFFER_FAULT_SHIFT, 1);
 fault_traced = 0;
 chk("60 an empty loop begins nothing",
     e_buffer_action_begin(&a, &buf, 4, 3, E_BUFFER_FAULT_SHIFT, ACTION_WHAT), E_BUFFER_NOTHING);
 chk("60 ... takes no memory and fires no fault", fault_traced, 0);
 chk("60 ... names no row", rows_past_traced, 0);
 chk("60 ... with no shell", a.shell == NULL, 1);
 e_buffer_action_abort(&a);
 chk_str("60 ... the text as it was", text_of(), ACTION_BEFORE);
 token = e_buffer_test_fault_enter(E_BUFFER_FAULT_SHIFT);   /* the armed point fires here */
 chk("60 the point was still armed", token, E_BUFFER_FAULT_FIRED);
 e_buffer_test_fault_leave(token);
 fixture_free();
}

/* An action refused for the shape of the text (rows the text does not
   have) is said once on the status row, with that reason and not as memory;
   an action with nothing to do says nothing. */
static void case_an_action_refused_for_the_shape_of_the_text_says_so(void)
{
 WpeBufferAction a;

 fixture(8, E_BUFFER_HISTORY_FULL);
 chk("61 rows past the text are refused", e_buffer_action_begin(&a, &buf, 9, 10,
     E_BUFFER_FAULT_NONE, ACTION_WHAT), E_BUFFER_RANGE);
 chk("61 ... said once", shape_calls, 1);
 chk("61 ... not as memory", nomem_calls, 0);
 chk_str("61 ... the text as it was", text_of(), ACTION_BEFORE);
 chk("61 nothing to do is not a refusal", e_buffer_action_begin(&a, &buf, 4, 3,
     E_BUFFER_FAULT_NONE, ACTION_WHAT), E_BUFFER_NOTHING);
 e_buffer_action_abort(&a);
 chk("61 ... and says nothing more", shape_calls, 1);
 fixture_free();
}

/* ---- positions: the cursor and the marks stand on a glyph ---- */

/* A row of every shape a glyph takes: ASCII, a two byte character, a stray
   continuation byte, a three byte one, a cut lead, ASCII, a four byte one,
   an overlong form, a surrogate, a Latin-1 letter, a lead above F4 with its
   continuation bytes (a glyph of four), and a byte above F7 with a
   continuation byte (two glyphs of one byte).  Separate literals, so no hex
   escape swallows the letter after it. */
static const char SHAPES[] = "a" "\xC3\xA9" "\x80" "\xE2\x82\xAC" "\xC3" "b"
                             "\xF0\x9F\x98\x80" "\xC0\x80" "\xED\xA0\x80"
                             "\xE9" "\xF5\x80\x80\x80" "\xF8\x80";
#define SHAPES_LEN ((int) sizeof SHAPES - 1)
#define TAB_WIDTH 8

/* A window with one row of @n bytes of @text, an editor behind it that
   gives the tab width, and no marks set. */
static void pos_fixture(const char *text, int n)
{
 fixture(1, E_BUFFER_HISTORY_FULL);
 cn.tabn = TAB_WIDTH;
 win.ed = &cn;
 e_buffer_set_line(&buf, 0, text, n);
 scr.mark_begin.x = scr.mark_begin.y = -1;
 scr.mark_end.x = scr.mark_end.y = -1;
 scr.fa.x = scr.fa.y = -1;
 scr.fe.x = scr.fe.y = -1;
 scr.ks.x = scr.ks.y = -1;
 buf.b.x = buf.b.y = 0;
}

/* Is column @x of the row a glyph start, by the forward walk from byte 0?
   Written from the rule, not from the owner. */
static int pos_obeys(const BUFFER *b, int y, int x)
{
 const STRING *l = &b->bf[y];
 int pos = 0;

 while (pos < x && pos < l->len)
  pos += e_utf8_seq_len(l->s + pos, l->len - pos);
 return(pos == x);
}

/* The glyph start at or before @x, by the same walk. */
static int pos_floor(const BUFFER *b, int y, int x)
{
 const STRING *l = &b->bf[y];
 int pos = 0, next;

 if (x > l->len)
  x = l->len;
 for (;;) {
  if (pos >= l->len)
   return(l->len);
  next = pos + e_utf8_seq_len(l->s + pos, l->len - pos);
  if (next > x)
   return(pos);
  pos = next;
 }
}

/* @scenario SC-edit-model-53
   A position is a glyph start, or the end of its row. */
static void case_a_position_is_a_glyph_start(void)
{
 POINT p;
 int x, want;
 char what[80];

 pos_fixture(SHAPES, SHAPES_LEN);
 for (x = 0; x <= SHAPES_LEN + 2; x++) {
  p.y = 0;
  p.x = x;
  snprintf(what, sizeof what, "62 fits at x=%d", x);
  chk(what, e_buffer_pos_fits(&buf, &p), x <= SHAPES_LEN && pos_obeys(&buf, 0, x));
  want = pos_floor(&buf, 0, x);
  e_buffer_pos_set(&buf, &p, 0, x);
  snprintf(what, sizeof what, "62 set at x=%d lands on the glyph at or before it", x);
  chk(what, p.x, want);
  snprintf(what, sizeof what, "62 set at x=%d stays on row 0", x);
  chk(what, p.y, 0);
  snprintf(what, sizeof what, "62 what set gives fits (x=%d)", x);
  chk(what, e_buffer_pos_fits(&buf, &p), 1);
 }
 e_buffer_pos_set(&buf, &p, -4, 3);
 chk("62 a row above the text is the first row", p.y, 0);
 e_buffer_pos_set(&buf, &p, 9, 3);
 chk("62 a row past the text is the last row", p.y, buf.mxlines - 1);
 e_buffer_pos_set(&buf, &p, 0, -3);
 chk("62 a column below 0 is 0", p.x, 0);
 e_buffer_cursor_set(&buf, 0, 2);
 chk("62 the cursor set inside an e-acute goes before it", buf.b.x, 1);
 chk("62 ... on its row", buf.b.y, 0);
 e_buffer_cursor_set(&buf, 0, SHAPES_LEN + 40);
 chk("62 the cursor set past the row goes to its end", buf.b.x, SHAPES_LEN);
 fixture_free();
}

/* @scenario SC-edit-model-53
   The position of an empty text reads no row, and an unset mark stays unset
   whatever the owner does to the marks around it. */
static void case_a_position_in_an_empty_or_unset_place(void)
{
 POINT p;
 STRING *rows;

 pos_fixture("ab", 2);
 rows = buf.bf;
 buf.bf = NULL;
 buf.mxlines = 0;
 p.y = 0;
 p.x = 0;
 chk("63 an empty text holds the position 0,0", e_buffer_pos_fits(&buf, &p), 1);
 p.x = 1;
 chk("63 ... and no other column", e_buffer_pos_fits(&buf, &p), 0);
 p.x = 0;
 p.y = 1;
 chk("63 ... and no other row", e_buffer_pos_fits(&buf, &p), 0);
 e_buffer_pos_set(&buf, &p, 3, 5);
 chk("63 set in an empty text gives 0,0", p.y == 0 && p.x == 0, 1);
 buf.bf = rows;
 buf.mxlines = 1;
 e_buffer_marks_inside(&buf);
 chk("63 a mark never set stays unset", scr.mark_begin.y, -1);
 e_buffer_mark_set(&buf, E_BUFFER_MARK_BEGIN, 0, 1);
 chk("63 a mark set stands where it was put", scr.mark_begin.y == 0 && scr.mark_begin.x == 1, 1);
 e_buffer_mark_unset(&buf, E_BUFFER_MARK_BEGIN);
 chk("63 a mark unset is unset again", scr.mark_begin.y < 0 && scr.mark_begin.x < 0, 1);
 e_buffer_mark_set(&buf, E_BUFFER_MARK_END, -1, -1);
 chk("63 a mark set to nowhere is unset", scr.mark_end.y < 0, 1);
 fixture_free();
}

/* The display column onto a row: the loop of the cursor's own Up and Down,
   kept here as it was, which the owner's copy must agree with byte for byte. */
static int ref_col_to_byte(int col, BUFFER *b, FENSTER *f, int y)
{
 int i, j;

 for (i = j = 0; i + j < col && i < b->bf[y].len; i++) {
  unsigned char uc = (unsigned char) *(b->bf[y].s + i);

  if (uc == WPE_TAB)
   j += (f->ed->tabn - ((j + i) % f->ed->tabn) - 1);
  else if (uc < ' ')
   j++;
  else if (UTF8_IS_LEAD(uc))
   i += e_utf8_visual_step((unsigned char *)b->bf[y].s, i, b->bf[y].len, &j);
 }
 return(i);
}

/* @scenario SC-edit-model-53
   A display column lands on a glyph, as Up and Down did. */
static void case_a_display_column_lands_on_a_glyph(void)
{
 static const char ROW[] = "a\tb" "\xC3\xA9" "\xE4\xB8\xAD" "\x01" "\xE2\x82\xAC" "z\t\t" "\xF0\x9F\x98\x80" "end";
 POINT p;
 int col;
 char what[80];

 pos_fixture(ROW, (int) sizeof ROW - 1);
 for (col = 0; col <= 120; col++) {
  e_buffer_pos_set_col(&buf, &p, 0, col);
  snprintf(what, sizeof what, "64 column %d: the byte Up and Down gave", col);
  chk(what, p.x, ref_col_to_byte(col, &buf, &win, 0));
  snprintf(what, sizeof what, "64 column %d: a glyph start", col);
  chk(what, e_buffer_pos_fits(&buf, &p), 1);
 }
 buf.b.x = 5;
 e_buffer_cursor_set_col(&buf, 0, 3);
 chk("64 the cursor lands by display column", buf.b.x, ref_col_to_byte(3, &buf, &win, 0));
 fixture_free();
}

/* @scenario SC-edit-model-53
   A search hit is shown whole: its start goes back, its
   end goes forward, to the edge of the glyph it falls in. */
static void case_a_find_mark_snaps_outward(void)
{
 static const int CASES[][2] = { {1, 1}, {0, 1}, {1, 2} };
 size_t i;
 char what[60];

 pos_fixture("\xC3\xA9" "a", 3);
 for (i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
  e_buffer_find_set(&buf, 0, CASES[i][0], CASES[i][1]);
  snprintf(what, sizeof what, "65 hit %d,%d starts at 0", CASES[i][0], CASES[i][1]);
  chk(what, scr.fa.x, 0);
  snprintf(what, sizeof what, "65 hit %d,%d ends at 2", CASES[i][0], CASES[i][1]);
  chk(what, scr.fe.x, 2);
  chk("65 the hit is on its row", scr.fa.y == 0 && scr.fe.y == 0, 1);
 }
 e_buffer_find_set(&buf, 0, 2, 3);
 chk("65 a hit on whole glyphs stays: start", scr.fa.x, 2);
 chk("65 a hit on whole glyphs stays: end", scr.fe.x, 3);
 fixture_free();
}

/* @scenario SC-edit-model-53
   Marks behind a change in their row follow the text. */
static void case_marks_move_with_an_in_row_change(void)
{
 STRING below;

 pos_fixture("abcdefgh", 8);
 scr.mark_begin.y = scr.mark_end.y = scr.ks.y = 0;
 scr.mark_begin.x = 1;
 scr.mark_end.x = 5;
 scr.ks.x = 7;
 /* "cd" (2 bytes at 2) taken out: the bytes are gone before the call */
 memmove(buf.bf[0].s + 2, buf.bf[0].s + 4, 5);
 buf.bf[0].len = 6;
 e_buffer_text_removed(&buf, 0, 2, 2);
 chk("66 removed: a mark before the change stays", scr.mark_begin.x, 1);
 chk("66 removed: a mark behind it moves left", scr.mark_end.x, 3);
 chk("66 removed: so does the anchor", scr.ks.x, 5);
 scr.mark_end.x = 3;
 scr.mark_begin.x = 3;
 memmove(buf.bf[0].s + 1, buf.bf[0].s + 4, 3);
 buf.bf[0].len = 3;
 e_buffer_text_removed(&buf, 0, 1, 3);
 chk("66 removed: a mark inside the cut goes to its start", scr.mark_begin.x, 1);
 fixture_free();
 pos_fixture("abcdefgh", 8);
 scr.mark_begin.y = scr.mark_end.y = scr.ks.y = 0;
 scr.mark_begin.x = 1;
 scr.mark_end.x = 2;
 scr.ks.x = 6;
 memmove(buf.bf[0].s + 4, buf.bf[0].s + 2, 7);
 buf.bf[0].len = 10;
 e_buffer_text_inserted(&buf, 0, 2, 2);
 chk("66 inserted: a mark before the place stays", scr.mark_begin.x, 1);
 chk("66 inserted: a mark at the place moves right", scr.mark_end.x, 4);
 chk("66 inserted: so does the anchor behind it", scr.ks.x, 8);
 fixture_free();
 pos_fixture("abcdefgh", 8);
 below = line_of(&buf, "wxyz");
 e_buffer_splice(&buf, 1, 0, &below, 1, NULL, 0, E_BUFFER_STEP_COVERED);
 scr.mark_begin.y = scr.mark_end.y = 0;
 scr.mark_begin.x = 2;
 scr.mark_end.x = 6;
 e_buffer_row_cut(&buf, 0, 4);
 chk("66 cut: a mark before the cut stays on its row", scr.mark_begin.y == 0 && scr.mark_begin.x == 2, 1);
 chk("66 cut: a mark behind it goes to the row below", scr.mark_end.y == 1 && scr.mark_end.x == 2, 1);
 fixture_free();
}

/* @scenario SC-edit-model-53
   A change that makes one character of stray bytes
   leaves the cursor and the marks before it, not inside. */
static void case_a_glyph_merge_resnaps_the_cursor(void)
{
 pos_fixture("\xC3" "x" "\xA9", 3);
 buf.b.x = 1;
 scr.mark_begin.y = scr.mark_end.y = 0;
 scr.mark_begin.x = 1;
 scr.mark_end.x = 3;
 scr.pt[2].y = 0;
 scr.pt[2].x = 1;
 memmove(buf.bf[0].s + 1, buf.bf[0].s + 2, 2);
 buf.bf[0].len = 2;
 e_buffer_text_removed(&buf, 0, 1, 1);
 chk("67 the cursor that was between them is before the character", buf.b.x, 0);
 chk("67 a mark that was between them is too", scr.mark_begin.x, 0);
 chk("67 the end of the row stays the end", scr.mark_end.x, 2);
 chk("67 a marker the same", scr.pt[2].x, 0);
 chk("67 the position is a place", e_buffer_pos_fits(&buf, &buf.b), 1);
 fixture_free();
}

/* @scenario SC-edit-model-53
   A splice keeps the cursor on its row and inside the row's text. */
static void case_a_splice_keeps_the_cursor_inside_its_row(void)
{
 STRING l;

 pos_fixture("abcdef", 6);
 buf.b.y = 0;
 buf.b.x = 5;
 l = line_of(&buf, "\xC3\xA9" "z");
 chk("68 a shorter row put under the cursor", e_buffer_splice(&buf, 0, 1, &l, 1, NULL, 0,
     E_BUFFER_STEP_COVERED), E_BUFFER_OK);
 chk("68 keeps the cursor's row", buf.b.y, 0);
 chk("68 and takes it to the row's end", buf.b.x, 3);
 fixture_free();
}

/* @scenario SC-edit-model-53
   The cursor and the block that a view keeps and the
   marker and step calls write are places. */
static void case_the_named_positions_are_places(void)
{
 WpeReplayView v;
 Undo rec;
 POINT from;

 pos_fixture("\xC3\xA9" "ab" "\xE2\x82\xAC" "c", 8);
 buf.b.y = 0;
 buf.b.x = 3;
 e_buffer_mark_set(&buf, E_BUFFER_MARK_BEGIN, 0, 3);
 e_buffer_mark_set(&buf, E_BUFFER_MARK_END, 0, 6);
 e_buffer_view_keep(&buf, &v);
 chk("69 the view keeps the cursor", v.cursor.x == 3 && v.cursor.y == 0, 1);
 chk("69 ... and the block, its end back at the start of the euro sign",
     v.mark_begin.x == 3 && v.mark_end.x == 4, 1);
 e_buffer_mark_copy(&buf, E_BUFFER_MARK_ANCHOR, &buf.b);
 chk("69 the anchor copies the cursor", scr.ks.x == 3 && scr.ks.y == 0, 1);
 from.y = 0;
 from.x = 6;
 e_buffer_mark_copy(&buf, E_BUFFER_MARK_ANCHOR, &from);
 chk("69 ... and goes to the start of a glyph it fell in", scr.ks.x, 4);
 e_buffer_block_set(&buf, &scr.mark_end, &scr.mark_begin);
 chk("69 a block is set begin before end, from its own marks",
     scr.mark_begin.x == 3 && scr.mark_end.x == 4, 1);
 e_buffer_cursor_set(&buf, 0, 0);
 e_buffer_cursor_step(&buf, 1);
 chk("69 a step right takes the whole e-acute", buf.b.x, 2);
 e_buffer_cursor_step(&buf, 1);
 e_buffer_cursor_step(&buf, 1);
 chk("69 ... and then one byte letters", buf.b.x, 4);
 e_buffer_cursor_step(&buf, 1);
 chk("69 ... and the whole euro sign", buf.b.x, 7);
 e_buffer_cursor_step(&buf, -1);
 chk("69 a step left takes the whole euro sign", buf.b.x, 4);
 e_buffer_cursor_set(&buf, 0, 0);
 e_buffer_cursor_step(&buf, -1);
 chk("69 a step left at the row's start stays", buf.b.x, 0);
 e_buffer_cursor_set(&buf, 0, buf.bf[0].len);
 e_buffer_cursor_step(&buf, 1);
 chk("69 a step right at the row's end stays", buf.b.x, buf.bf[0].len);
 e_buffer_cursor_set(&buf, 0, 3);
 e_buffer_marker_set(&buf, 4);
 chk("69 a marker is the cursor", scr.pt[4].x == 3 && scr.pt[4].y == 0, 1);
 chk("69 its highlight is one glyph: start", scr.fa.x == 3 && scr.fa.y == 0, 1);
 chk("69 its highlight is one glyph: end", scr.fe.x, 4);
 e_buffer_cursor_set(&buf, 0, 0);
 e_buffer_marker_goto(&buf, 4);
 chk("69 the marker's place is the cursor again", buf.b.x == 3 && buf.b.y == 0, 1);
 memset(&rec, 0, sizeof rec);
 e_buffer_record_at(&rec, 0, 5);
 chk("69 a record is given its place", rec.b.y == 0 && rec.b.x == 5, 1);
 e_buffer_pos_from_offset(&buf, &from, 0, 2);
 chk("69 an offset into the line is a place", from.y == 0 && from.x == 2, 1);
 e_buffer_pos_from_offset(&buf, &from, 0, 600);
 chk("69 ... clamped to the row's end", from.x, buf.bf[0].len);
 fixture_free();
}

/* @scenario SC-edit-model-53
   Every text primitive refuses a start or an end between the bytes of a
   character, says why in the trace, and leaves the text as it was. */
static void case_a_change_inside_a_glyph_is_refused(void)
{
 Undo as_if;
 char before[256];
 int x, n, want, bad = 0, traced, kept;

 pos_fixture(SHAPES, SHAPES_LEN);
 for (x = 0; x <= SHAPES_LEN; x++)
  for (n = 0; x + n <= SHAPES_LEN; n++) {
   want = pos_obeys(&buf, 0, x) && pos_obeys(&buf, 0, x + n);
   if (e_buffer_change_fits(&buf, 0, x, n, "sweep") != want)
    bad++;
  }
 chk("70 a change fits exactly when both its ends are places, for every x and n", bad, 0);
 chk("70 ... and no change past the row's end fits",
     e_buffer_change_fits(&buf, 0, 2, SHAPES_LEN, "past"), 0);
 chk("70 ... nor a row that is not there", e_buffer_change_fits(&buf, 5, 0, 0, "row"), 0);
 fixture_free();

 pos_fixture("a" "\xC3\xA9" "b", 4);
 buf.bf[0].s[4] = WPE_WR;
 chk("70 the row's line break is one more byte a delete may take",
     e_buffer_change_fits(&buf, 0, 3, 2, "break"), 1);
 chk("70 ... but not two", e_buffer_change_fits(&buf, 0, 3, 3, "break"), 0);
 fixture_free();

 pos_fixture("a" "\xC3\xA9" "b", 4);
 snprintf(before, sizeof before, "%s", text_of());
 hooks_reset();
 chk("70 a split between the bytes of a character is refused",
     e_buffer_split_line(&buf, 0, 2, E_BUFFER_STEP_NEW, E_BUFFER_HEAD_WR), E_BUFFER_RANGE);
 chk("70 ... traced", inside_traced, 1);
 chk("70 ... and the text is as it was", strcmp(text_of(), before) == 0, 1);
 chk("70 ... on one row", buf.mxlines, 1);
 chk("70 a split before the character is made",
     e_buffer_split_line(&buf, 0, 1, E_BUFFER_STEP_NEW, E_BUFFER_HEAD_WR), E_BUFFER_OK);
 chk("70 ... with no refusal traced", inside_traced, 1);
 fixture_free();

 pos_fixture("a" "\xC3\xA9" "b", 4);
 memset(&as_if, 0, sizeof as_if);
 as_if.type = 'a';
 as_if.b = pnt(2, 0);
 as_if.a.x = 1;
 snprintf(before, sizeof before, "%s", text_of());
 kept = list_len(buf.ud);
 hooks_reset();
 chk("70 an Undo that deletes from inside a character is refused",
     e_buffer_apply_chars(&buf, &scr, &as_if), E_BUFFER_RANGE);
 traced = inside_traced;
 chk("70 ... traced", traced, 1);
 chk("70 ... without running the primitive", replay_calls, 0);
 chk("70 ... and the text is as it was", strcmp(text_of(), before) == 0, 1);
 chk("70 ... with no record kept", list_len(buf.ud), kept);
 as_if.b = pnt(1, 0);
 as_if.a.x = 1;
 chk("70 ... also when it ends inside one",
     e_buffer_apply_chars(&buf, &scr, &as_if), E_BUFFER_RANGE);
 as_if.type = 's';
 as_if.b = pnt(0, 0);
 as_if.a.x = 1;
 as_if.a.y = 2;
 hooks_reset();
 chk("70 an Undo of a replace whose text ends inside a character is refused",
     e_buffer_apply_chars(&buf, &scr, &as_if), E_BUFFER_RANGE);
 chk("70 ... without running the primitive", replay_calls, 0);
 chk("70 ... and the text is as it was", strcmp(text_of(), before) == 0, 1);
 fixture_free();
}

/* @scenario SC-edit-model-53
   The find mark follows the text it marks: bytes put in before it push it
   right, bytes taken out pull it left, and a row put in above moves its row,
   so the highlight and Search Again stay on the hit after an edit. */
static void case_the_find_mark_follows_an_edit(void)
{
 STRING l;

 pos_fixture("abcdefgh", 8);
 e_buffer_find_set(&buf, 0, 4, 6);
 memmove(buf.bf[0].s + 2, buf.bf[0].s, 9);
 buf.bf[0].len = 10;
 e_buffer_text_inserted(&buf, 0, 1, 2);
 chk("71 an insert before the hit moves its start", scr.fa.x, 6);
 chk("71 ... and its end", scr.fe.x, 8);
 memmove(buf.bf[0].s, buf.bf[0].s + 1, 10);
 buf.bf[0].len = 9;
 e_buffer_text_removed(&buf, 0, 0, 1);
 chk("71 a delete before the hit pulls its start back", scr.fa.x, 5);
 chk("71 ... and its end", scr.fe.x, 7);
 e_buffer_text_inserted(&buf, 0, 8, 1);
 chk("71 an insert behind the hit leaves its start", scr.fa.x, 5);
 chk("71 ... and its end", scr.fe.x, 7);
 l = line_of(&buf, "new");
 chk("71 a row put in above", e_buffer_splice(&buf, 0, 0, &l, 1, NULL, 0, E_BUFFER_STEP_COVERED),
     E_BUFFER_OK);
 chk("71 moves the hit's row down", scr.fa.y == 1 && scr.fe.y == 1, 1);
 chk("71 ... and keeps its columns", scr.fa.x == 5 && scr.fe.x == 7, 1);
 fixture_free();
}

int main(void)
{
 case_the_find_mark_follows_an_edit();
 case_a_change_inside_a_glyph_is_refused();
 case_a_position_is_a_glyph_start();
 case_a_position_in_an_empty_or_unset_place();
 case_a_display_column_lands_on_a_glyph();
 case_a_find_mark_snaps_outward();
 case_marks_move_with_an_in_row_change();
 case_a_glyph_merge_resnaps_the_cursor();
 case_a_splice_keeps_the_cursor_inside_its_row();
 case_the_named_positions_are_places();
 case_insert_above_a_mark();
 case_delete_above();
 case_delete_across_a_mark();
 case_breakpoint_on_a_deleted_line();
 case_changed_lines_keep_rows_and_marks();
 case_line_record_round_trip();
 case_a_step_is_one_undo();
 case_covered_deletion_and_the_pocket();
 case_detach_then_splice();
 case_replace_all_changes_only_the_middle();
 case_a_format_is_cut_into_hunks();
 case_a_refused_swap_changes_nothing();
 case_a_swap_inside_a_block_operation();
 case_a_swap_beyond_the_bound_is_one_hunk();
 case_the_swap_is_exact_for_any_texts();
 case_syntax_states_move();
 case_a_view();
 case_no_window();
 case_no_memory_changes_nothing();
 case_out_of_range();
 case_zero_splice();
 case_emptied_buffer();
 case_refused_undo_keeps_the_cursor();
 case_a_refused_undo_keeps_a_pocketed_breakpoint();
 case_a_view_row_may_exceed_the_width();
 case_the_undo_bound();
 case_append_text();
 case_append_rows();
 case_many_appends();
 case_depth_guard();
 case_the_fault_hook_fires_once();
 case_lines_from_parts();
 case_a_refused_move_changes_nothing();
 case_the_view_reads_pending_lines();
 case_a_transaction_grows_past_its_inline_slots();
 case_a_shell_is_an_owner_allocation();
 case_a_shell_counts_its_refusals();
 case_a_rewrite_item_is_whole_or_nothing();
 case_a_rewrite_reports_rows_removed_and_added();
 case_a_char_replay_on_a_full_soft_last_row();
 case_a_rewrite_on_the_last_row();
 case_a_shell_of_one_row_keeps_its_breakpoint();
 case_two_rewrites_in_one_transaction();
 case_the_view_reads_two_items_on_one_row();
 case_a_copy_into_a_narrower_buffer_keeps_every_byte();
 case_a_move_into_a_narrower_buffer_builds_its_line_again();
 case_a_record_never_names_a_freed_row();
 case_marks_stand_in_the_text_after_an_undo();
 case_a_refused_undo_of_a_join_keeps_the_breakpoint_parked();
 case_a_refused_undo_of_a_two_record_step_changes_nothing();
 case_a_step_of_line_records_is_one_transaction();
 case_a_block_record_is_alone_in_its_step();
 case_a_character_change_through_the_owner_is_whole_or_nothing();
 case_text_cut_into_rows_fits_the_width();
 case_every_row_a_maker_makes_fits_the_width();
 case_a_row_that_does_not_fit_is_refused();
 case_enter_at_the_end_of_a_full_soft_row_gives_it_its_break();
 case_a_wide_view_row_copied_into_a_file_keeps_every_byte();
 case_the_owners_check_agrees_with_the_rule();
 case_an_action_runs_on_a_shell_and_commits_whole();
 case_an_action_refused_on_its_shell_changes_nothing_and_says_so_once();
 case_an_action_refused_for_memory_at_each_allocation();
 case_an_action_span_runs_to_the_first_hard_row_after_its_last();
 case_an_action_over_the_whole_text_has_no_guards();
 case_an_action_keeps_a_line_record_of_its_shell();
 case_a_line_record_of_a_shell_keeps_unset_marks_unset();
 case_a_read_only_buffer_refuses_an_action_before_its_shell();
 case_an_empty_shift_begins_nothing();
 case_an_action_refused_for_the_shape_of_the_text_says_so();
 if (fails) {
  printf("test_splice: %d failure(s)\n", fails);
  return(1);
 }
 printf("test_splice: ok\n");
 return(0);
}
