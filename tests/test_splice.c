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

 (void) b;
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

void wpe_buffer_say_nomem(BUFFER *b, const char *what)
{
 (void) b;
 (void) what;
 nomem_calls++;
}

int wpe_buffer_replay_kind(BUFFER *b, SCHIRM *s, Undo *rec)
{
 (void) b;
 (void) s;
 (void) rec;
 return(E_BUFFER_OK);
}

void wpe_buffer_trace(const char *fmt, ...)
{
 if (strstr(fmt, "depth leak") != NULL)
  leak_traced++;
}

static void hooks_reset(void)
{
 syntax_calls = moved_calls = nomem_calls = 0;
 moved_nremoved = moved_nadded = 0;
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

int main(void)
{
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
 case_syntax_states_move();
 case_a_view();
 case_no_window();
 case_no_memory_changes_nothing();
 case_out_of_range();
 case_zero_splice();
 case_emptied_buffer();
 case_refused_undo_keeps_the_cursor();
 case_the_undo_bound();
 case_append_text();
 case_many_appends();
 case_depth_guard();
 if (fails) {
  printf("test_splice: %d failure(s)\n", fails);
  return(1);
 }
 printf("test_splice: ok\n");
 return(0);
}
