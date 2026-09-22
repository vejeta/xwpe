/* we_ai_ui.c - AI assistant UI: the docked chat pane, the prompt input, and the
 * asynchronous token streaming driven off the editor's fd-loop.  The inline
 * diff/preview (Edit mode) and the agent approval prompt live here too.
 * Compiled only under --enable-ai (WPE_AI). */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef WPE_AI

#include "messages.h"
#include "edit.h"
#include "WeExpArr.h"
#include "progr.h"
#include "we_fdloop.h"
#include "we_ai.h"
#include "we_ai_host.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <poll.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdint.h>
#include <time.h>
#ifdef __linux__
#include <sys/timerfd.h>
#endif

/* e_d_p_named is defined in we_prog.c; declared here in case progr.h predates it. */
int e_d_p_named(char *winname, char *str, FENSTER *f, int sw);

#define AI_PANE_NAME  "AI"
#define AI_CTX_MAX    16000     /* cap the current-file context we send        */
#define AI_PROMPT_MAX 2000      /* room for a real instruction, not 128 chars   */

/* ----- one active chat at a time (MVP) ----------------------------------- */
typedef struct {
 wpe_ai_stream *st;
 FENSTER       *ref;           /* anchor window (gives us cn = ref->ed)        */
 char          *pending;       /* incomplete trailing line being streamed      */
 size_t         plen, pcap;
 int            pcols;         /* display columns in `pending` (for word-wrap)   */
 char          *full;          /* whole reply so far (for the session log)     */
 size_t         flen, fcap;
 int            fd;
 int            active;
 int            started;       /* a fresh reply line has been opened under "AI:" */
 int            turns;         /* read-only tool turns taken before answering    */
 int            decided;       /* this turn: 0 undecided, 1 = answer (stream it),
                                  2 = a TOOL investigation (shown as a dim status,
                                  the raw protocol line is NOT echoed as "AI:")  */
 int            hide_tool;     /* a TOOL line begun after a preamble is being
                                  hidden: show its dim status, not the raw line   */
 size_t         shown;         /* bytes of `full` already painted (decided==1)   */
} ai_chat_session;

#define AI_CHAT_MAX_TOOL_TURNS 6   /* cap chat's read-only investigation loop */
#define AI_REPLY_PREFIX "AI: "     /* speaker label kept on the reply's first line */

static ai_chat_session *g_ai_chat = NULL;

/* Chat "focus mode": the pane keeps a live "> " input row as its LAST line, and
 * the streamed reply is inserted ABOVE that row, so the conversation reads down
 * the window with a fixed input at the bottom (like a modern chat).  The input
 * row is an ordinary buffer line, so every backend paints it for free.  While
 * focus mode is on, the whole-line and streaming pane writers target the line
 * ABOVE the input row instead of the last line. */
static int  g_ai_chat_focus = 0;
static char g_ai_input[AI_PROMPT_MAX];  /* may hold '\n' -- a multi-line prompt   */
static int  g_ai_input_len = 0;
static int  g_ai_input_rows = 0;        /* pane lines the input region occupies    */
static int  g_ai_input_pos = 0;         /* caret byte offset within g_ai_input     */
static int  g_ai_scroll_lock = 0;       /* user scrolled up: hold the view there   */
static FENSTER *g_ai_chat_pane = NULL;  /* the armed chat pane (NULL = no chat)     */
static int  g_ai_home_edt = -1;         /* window to refocus when the chat closes   */

/* First pane line the stream writes into: the last line normally, or the line
 * just above the input region when focus mode owns the bottom rows. */
static int ai_stream_y(FENSTER *wf)
{
 int y = wf->b->mxlines - 1;
 if (g_ai_chat_focus) y -= g_ai_input_rows;
 if (y < 0) y = 0;
 return y;
}

/* Seed the streaming line buffer with `seed` so the reply keeps its "AI: "
 * speaker label on the first line while tokens append after it (instead of the
 * label sitting alone on its own line above the answer). */
static void ai_chat_set_pending(ai_chat_session *s, const char *seed)
{
 size_t n = strlen(seed);
 if (n + 1 > s->pcap) {
  size_t nc = s->pcap ? s->pcap : 256;
  char *nb;
  while (n + 1 > nc) nc *= 2;
  nb = realloc(s->pending, nc);
  if (!nb) return;
  s->pending = nb; s->pcap = nc;
 }
 memcpy(s->pending, seed, n + 1);
 s->plen = n;
 s->pcols = (int)n;              /* the "AI: " seed is plain ASCII */
}

/* read-only tool helpers (defined in the Agent section) + the turn starter */
static char *ai_run_capture(const char *cmd);
static char *ai_read_file_bounded(const char *path);
static void  ai_chat_next_turn(ai_chat_session *s);
static char *ai_chat_tool_result(FENSTER *f, const char *reply);
static FENSTER *ai_pane_win(FENSTER *f);
static void  ai_tr_line(FENSTER *wf, const char *str);
static void  ai_pane_paint(FENSTER *wf);
static void  ai_pane_hl_attention(FENSTER *wf);

static int ai_pane_width(FENSTER *wf);          /* defined below */

/* Append ONE ready line to the pane (no wrapping): into the transcript above the
   input row in chat-focus, otherwise to the end of the docked pane. */
static void ai_pane_put1(FENSTER *f, const char *line, int surface)
{
 if (g_ai_chat_focus) {                 /* keep the "> " input row at the bottom */
  FENSTER *wf = ai_pane_win(f);
  if (wf) { ai_tr_line(wf, line); ai_pane_paint(wf); }
  return;
 }
 /* Ensure the AI pane is created AND docked at the bottom (beside Messages)
    before appending -- the same placement as the chat pane.  Without this the
    Edit/Plan/Agent output opened as a full window over the file being edited. */
 ai_pane_win(f);
 e_d_p_named(AI_PANE_NAME, (char *)line, f, surface ? 1 : 0);
 /* e_d_p_named draws in the normal colour; repaint any attention line red on
    top (agent approval, the first-use notice run outside the chat-input path). */
 { FENSTER *wf = ai_pane_win(f);
   if (wf) { ai_pane_hl_attention(wf); e_refresh(); } }
}

/* Append `line` to the pane, word-wrapped to the pane's CURRENT width, so a long
   answer or summary reads DOWN the window instead of running off to the right --
   the same width the streamed chat reply wraps to, now applied to every pane
   message (agent / multi-file answers and status lines).  Display-only wrapping
   of our own transcript window; it never touches a source file.  Column counting
   skips UTF-8 continuation bytes so multibyte text wraps by character. */
static void ai_pane(FENSTER *f, const char *line, int surface)
{
 FENSTER *wf = ai_pane_win(f);
 int width = wf ? ai_pane_width(wf) : 76;
 const char *p = line;
 int first = 1;

 if (!line || !*line) { ai_pane_put1(f, "", surface); return; }
 while (*p) {
  int col = 0, i = 0, lastspace = -1, cut;
  char seg[1200];
  while (p[i] && col < width) {                  /* consume up to `width` columns */
   if (p[i] == ' ') lastspace = i;
   if (((unsigned char)p[i] & 0xC0) != 0x80) col++;
   i++;
  }
  while (p[i] && ((unsigned char)p[i] & 0xC0) == 0x80) i++;   /* trailing cont bytes */
  if (!p[i]) { ai_pane_put1(f, p, first ? surface : 0); break; }  /* the rest fits */
  cut = (lastspace > 0) ? lastspace : i;         /* break at a space, else hard */
  if (cut >= (int)sizeof seg) cut = (int)sizeof seg - 1;
  memcpy(seg, p, (size_t)cut); seg[cut] = '\0';
  ai_pane_put1(f, seg, first ? surface : 0);
  p += cut;
  while (*p == ' ') p++;                          /* drop the break space */
  first = 0;
 }
}

/* Lines that ASK the user to act (approve a step, the first-use notice, "type a
   follow-up") carry this marker and are drawn in a distinct colour, so a prompt
   never blends into the agent's ordinary output.  Content-based, so it survives
   scrolling and line insertion. */
#define AI_ATTN_MARK    ">> "
#define AI_ATTN_MARKLEN 3

/** ai_pane_attn - Add an attention line (marked + highlighted) to the pane. */
static void ai_pane_attn(FENSTER *f, const char *text)
{
 char buf[1300];
 snprintf(buf, sizeof buf, "%s%s", AI_ATTN_MARK, text);
 ai_pane(f, buf, 1);
}

/* Repaint the pane's visible attention lines in the highlight colour, on top of
   what e_schirm just drew in the normal colour. */
static void ai_pane_hl_attention(FENSTER *wf)
{
 BUFFER *b = wf->b;
 SCHIRM *s = wf->s;
 int rows = wf->e.y - wf->a.y;                    /* interior rows e_schirm draws */
 /* Red foreground on the pane's own background ("this needs your input").  The
    attribute is 16*bg + fg; keep the pane's text background and set a red
    foreground (ANSI colour index 1, as init_pair maps them). */
 int color = 16 * wf->fb->nt.b + 1;
 int y;
 for (y = s->c.y; y < b->mxlines && y < s->c.y + rows; y++) {
  const char *ls = b->bf[y].s;
  int row, n;
  char tmp[1300];
  if (!ls || strncmp(ls, AI_ATTN_MARK, AI_ATTN_MARKLEN)) continue;
  n = b->bf[y].len;
  if (n >= (int)sizeof tmp) n = (int)sizeof tmp - 1;
  memcpy(tmp, ls, (size_t)n); tmp[n] = '\0';
  row = wf->a.y + 1 + (y - s->c.y);
  e_pr_str(wf->a.x + 1, row, tmp, color, 0, 0, 0, 0);
 }
}

/* Append a possibly multi-line string to the pane, one pane line per '\n', so a
   multi-line answer or summary is shown in full instead of only its first line. */
static void ai_pane_multiline(FENSTER *f, const char *text, int surface)
{
 const char *p = text;
 if (!p) return;
 while (*p) {
  const char *nl = strchr(p, '\n');
  size_t l = nl ? (size_t)(nl - p) : strlen(p);
  char buf[1024];
  if (l >= sizeof buf) l = sizeof buf - 1;
  memcpy(buf, p, l); buf[l] = '\0';
  ai_pane(f, buf, surface);
  if (!nl) break;
  p = nl + 1;
 }
}

/* Join the current file's lines into a single (bounded) text block. */
static char *ai_current_file_text(FENSTER *f)
{
 BUFFER *b = f->b;
 size_t cap = 4096, len = 0;
 char *t = malloc(cap);
 int y;
 if (!t) return NULL;
 for (y = 0; y < b->mxlines; y++) {
  const char *ls = (const char *)b->bf[y].s;
  int ll = b->bf[y].len;
  if (ll < 0) ll = 0;
  if (len + (size_t)ll + 2 > cap) {
   char *nt;
   while (len + (size_t)ll + 2 > cap) cap *= 2;
   nt = realloc(t, cap);
   if (!nt) { free(t); return NULL; }
   t = nt;
  }
  if (ls && ll > 0) { memcpy(t + len, ls, (size_t)ll); len += (size_t)ll; }
  t[len++] = '\n';
  if (len > AI_CTX_MAX) break;
 }
 t[len] = '\0';
 return t;
}

/* ai_block_range - the marked block as a whole-line range [*y0..*y1], or 0 if
   there is no active selection.  Edit works line-granular (it diffs and splices
   whole lines), so a partial-line mark is widened to the lines it touches; a
   block that ends at column 0 does not pull in that trailing line. */
static int ai_block_range(FENSTER *f, int *y0, int *y1)
{
 SCHIRM *s = f->s;
 int a = s->mark_begin.y, b = s->mark_end.y;
 if (b < a || (a == b && s->mark_end.x <= s->mark_begin.x))
  return 0;                                  /* empty / inverted -> no selection */
 if (s->mark_end.x == 0 && b > a) b--;       /* mark at col 0 excludes that line */
 if (b < a) b = a;
 if (a < 0) a = 0;
 if (b > f->b->mxlines - 1) b = f->b->mxlines - 1;
 *y0 = a; *y1 = b;
 return 1;
}

/* ai_lines_text - lines [y0..y1] of the file joined with '\n', malloc'd.  Used to
   send only the selected region to the model for a scoped Edit. */
static char *ai_lines_text(FENSTER *f, int y0, int y1)
{
 BUFFER *b = f->b;
 size_t cap = 1024, len = 0;
 char *t = malloc(cap);
 int y;
 if (!t) return NULL;
 for (y = y0; y <= y1 && y < b->mxlines; y++) {
  const char *ls = (const char *)b->bf[y].s;
  int ll = b->bf[y].len;
  if (ll < 0) ll = 0;
  if (len + (size_t)ll + 2 > cap) {
   char *nt;
   while (len + (size_t)ll + 2 > cap) cap *= 2;
   nt = realloc(t, cap);
   if (!nt) { free(t); return NULL; }
   t = nt;
  }
  if (ls && ll > 0) { memcpy(t + len, ls, (size_t)ll); len += (size_t)ll; }
  t[len++] = '\n';
 }
 t[len] = '\0';
 return t;
}

/* ai_splice_lines - the whole file with lines [y0..y1] replaced by `snippet`, so
   a selection edit reuses the proven whole-file diff+apply path while confining
   the change to the selected region.  A trailing '\n' on `snippet` is dropped so
   the join does not introduce a blank line. */
static char *ai_splice_lines(FENSTER *f, int y0, int y1, const char *snippet)
{
 BUFFER *b = f->b;
 size_t cap = 4096, len = 0, sl = strlen(snippet);
 char *t = malloc(cap);
 int y;
 if (!t) return NULL;
 while (sl > 0 && snippet[sl - 1] == '\n') sl--;   /* one join newline, not two */
 for (y = 0; y < b->mxlines; y++) {
  const char *ls;
  int ll;
  if (y == y0) {                                    /* splice the new region in  */
   if (len + sl + 2 > cap) {
    char *nt; while (len + sl + 2 > cap) cap *= 2;
    nt = realloc(t, cap); if (!nt) { free(t); return NULL; }
    t = nt;
   }
   memcpy(t + len, snippet, sl); len += sl; t[len++] = '\n';
  }
  if (y >= y0 && y <= y1) continue;                 /* drop the old region       */
  ls = (const char *)b->bf[y].s; ll = b->bf[y].len;
  if (ll < 0) ll = 0;
  if (len + (size_t)ll + 2 > cap) {
   char *nt; while (len + (size_t)ll + 2 > cap) cap *= 2;
   nt = realloc(t, cap); if (!nt) { free(t); return NULL; }
   t = nt;
  }
  if (ls && ll > 0) { memcpy(t + len, ls, (size_t)ll); len += (size_t)ll; }
  t[len++] = '\n';
 }
 t[len] = '\0';
 return t;
}

static void ai_chat_finish(ai_chat_session *s)
{
 if (!s) return;
 wpe_ai_trace("chat finish begin fd=%d", s->fd);
 if (s->fd >= 0) wpe_fd_del(s->fd);
 wpe_ai_stream_free(s->st);
 free(s->pending);
 free(s->full);
 if (g_ai_chat == s) g_ai_chat = NULL;
 free(s);
 wpe_ai_trace("chat finish end");
}

/* Locate the "AI" output pane (creating it if needed), so streaming can paint
 * directly into its buffer rather than only appending whole lines. */
static FENSTER *ai_pane_win(FENSTER *f)
{
 ECNT *cn = f->ed;
 int i;
 for (i = cn->mxedt; i > 0 && strcmp(cn->f[i]->datnam, AI_PANE_NAME); i--)
  ;
 if (i == 0) {
  if (e_edit(cn, AI_PANE_NAME))
   return NULL;
  i = cn->mxedt;
  /* Mark it a tool/output pane like Messages (ins == 8, backs no file): it is
     never offered for saving on quit and wears the gear marker, not the padlock.
     The chat input still works -- e_ai_chat_key consumes those keys before the
     ins-gated editing path (we_edit.c). */
  cn->f[i]->ins = 8;
  e_position_ai_window(cn->f[i], cn);   /* dock at the bottom, not over the editor */
  /* Free every window's save-under before the relayout repaint: docking moves
     the editor's bottom edge, and a stale save-under would restore an old strip
     (a black column down the left edge).  This is what e_switch_window does. */
  e_free_all_pics(cn);
  e_rep_win_tree(cn);
 }
 return cn->f[i];
}

/* While a background op runs, the caret belongs in the USER'S window, not the
 * pane -- so they keep typing in their code while the AI streams/spins. */
static FENSTER *g_ai_bg_win = NULL;

/* Place the pane caret at the input cursor: its row within the input region and
 * its display column (past the "> "/"  " 2-column prefix). */
static void ai_input_caret(FENSTER *wf, int *cy, int *cx)
{
 int rows = g_ai_input_rows > 0 ? g_ai_input_rows : 1;
 int line_idx = 0, bcol = 0, i;
 /* The caret x is a BYTE offset into the rendered line: e_cursor walks the line
    bytes and converts a multi-byte glyph to its single display column itself
    (e_utf8_visual_step).  So count every byte on the current line here, NOT one
    per glyph -- returning a glyph count would drop the caret onto the trailing
    byte of an accented character (the cursor "sticks" on the e-acute). */
 for (i = 0; i < g_ai_input_pos && i < g_ai_input_len; i++) {
  if (g_ai_input[i] == '\n') { line_idx++; bcol = 0; }
  else bcol++;
 }
 *cy = wf->b->mxlines - rows + line_idx;
 *cx = 2 + bcol;                       /* 2 = the "> "/"  " prefix bytes */
}

/* Repaint the pane window and keep the caret in view.  The pane is an ordinary
 * window, so the caret belongs to whichever window is FOCUSED: it sits in the
 * chat input row only while the pane itself is focused; when the user has
 * switched to their file (or an async op runs), the pane repaints in the
 * background and the caret stays in the focused/user window. */
static void ai_pane_paint(FENSTER *wf)
{
 extern int wpe_modal_active;                    /* a dropdown/dialog box is open */
 FENSTER *act = wf->ed->f[wf->ed->mxedt];       /* the focused window */
 FENSTER *caret = g_ai_bg_win ? g_ai_bg_win : act;
 int pane_focused = (act == wf && !g_ai_bg_win);

 /* A menu or dialog is up on top of us (its whole lifetime sets this flag): do
    NOT repaint under it, or a streaming token / the working spinner draws over
    the box and corrupts it -- the same guard the async language-server painter
    uses.  The buffer already holds the new text; the next paint once the box
    closes shows it. */
 if (wpe_modal_active)
  return;

 if (pane_focused && g_ai_scroll_lock) {
  /* Browsing the transcript: the caret rests on a history line and the view
     follows it.  Do NOT move the caret back to the input or re-pin the view --
     a focused window always scrolls to keep its caret visible (e_cursor), so
     touching either here is exactly what would undo the browse. */
  e_cursor(wf, 0);
 } else {
  if (pane_focused && g_ai_chat_focus) {        /* caret sits in the input region */
   int cy, cx;
   ai_input_caret(wf, &cy, &cx);
   wf->b->b.y = cy;
   wf->b->b.x = cx;
  } else if (pane_focused) {
   int y = wf->b->mxlines - 1;
   wf->b->b.y = y;
   wf->b->b.x = (y >= 0 && wf->b->bf[y].s) ? wf->b->bf[y].len : 0;
  }
  if (g_ai_chat_focus) {
   int vh = wf->e.y - wf->a.y - 1;              /* pin the input row to the bottom */
   int bottom = wf->b->mxlines - vh;
   wf->s->c.y = bottom > 0 ? bottom : 0;
  } else {
   e_messages_scroll_to_bottom(wf);
  }
 }
 e_schirm(wf, 0);
 ai_pane_hl_attention(wf);          /* colour the "needs your input" lines */
 e_cursor(caret, 0);
 e_refresh();
}

/* Scroll the transcript view by `delta` lines to peek at history.  The view
 * holds until the next key: any edit key repaints and snaps back to the input
 * (ai_pane_paint scrolls to the bottom), so PgUp/PgDn browse, then editing
 * returns to the prompt. */
static void ai_pane_scroll(FENSTER *wf, int delta)
{
 BUFFER *b = wf->b;
 SCHIRM *s = wf->s;
 int vh = wf->e.y - wf->a.y - 1;
 int bottom = b->mxlines - vh;                    /* view offset that pins the input */

 if (bottom < 0) bottom = 0;

 /* A focused window always re-pins its view to keep the caret visible
    (e_cursor), so moving the view offset alone is undone at once.  Scroll the
    transcript one/page line at a time AND park the caret on the top visible
    line, so e_cursor is already satisfied and leaves the offset where we put it
    -- the pager feel a chat wants.  Reaching the bottom pin leaves browse mode
    and snaps back to the prompt. */
 s->c.y += delta;
 if (s->c.y < 0) s->c.y = 0;
 if (s->c.y >= bottom) {
  g_ai_scroll_lock = 0;
  ai_pane_paint(wf);
  return;
 }
 g_ai_scroll_lock = 1;
 b->b.y = s->c.y;
 b->b.x = 0;
 ai_pane_paint(wf);
}

/* Overwrite pane line `y` with `text` in place (no new line added). */
static void ai_pane_set_line(FENSTER *wf, int y, const char *text)
{
 BUFFER *b = wf->b;
 size_t L = strlen(text);
 if (b->mxlines == 0)
  e_new_line(0, b);
 if (y < 0) y = 0;
 if (y >= b->mxlines) y = b->mxlines - 1;
 b->bf[y].s = REALLOC(b->bf[y].s, L + 2);
 memcpy(b->bf[y].s, text, L);
 b->bf[y].s[L] = '\n';
 b->bf[y].s[L + 1] = '\0';
 b->bf[y].len = (int)L;
 b->bf[y].nrc = (int)L + 1;
}

/* Replace the text of the pane's current stream line in place (the last line, or
 * the line above the input row in focus mode), so a partial line grows
 * token-by-token as the model streams -- the "live typing" effect a real chat
 * has, instead of a whole line appearing at once. */
static void ai_pane_set_last(FENSTER *wf, const char *text)
{
 ai_pane_set_line(wf, ai_stream_y(wf), text);
 ai_pane_paint(wf);
}

/* Redraw the input region (the pane's last g_ai_input_rows lines) from the input
 * buffer, which may hold '\n'-separated lines.  The first line shows "> ", the
 * rest are indented so a multi-line prompt aligns under it; the region grows and
 * shrinks with the number of lines. */
static void ai_input_render(FENSTER *wf)
{
 BUFFER *b = wf->b;
 int want = 1, i;
 const char *p;
 char row[AI_PROMPT_MAX + 8];

 for (p = g_ai_input; *p; p++)
  if (*p == '\n') want++;

 if (b->mxlines == 0) e_new_line(0, b);
 while (g_ai_input_rows < want) { e_new_line(b->mxlines, b); g_ai_input_rows++; }
 while (g_ai_input_rows > want && g_ai_input_rows > 1) {
  int y = b->mxlines - 1;
  FREE(b->bf[y].s);
  b->mxlines--;
  g_ai_input_rows--;
 }
 p = g_ai_input;
 for (i = 0; i < want; i++) {
  const char *nl = strchr(p, '\n');
  int seglen = nl ? (int)(nl - p) : (int)strlen(p);
  if (seglen > (int)sizeof row - 4) seglen = (int)sizeof row - 4;
  snprintf(row, sizeof row, "%s%.*s", i == 0 ? "> " : "  ", seglen, p);
  ai_pane_set_line(wf, b->mxlines - want + i, row);
  if (!nl) break;
  p = nl + 1;
 }
 ai_pane_paint(wf);
}

/* Finalise the current line and open a fresh (empty) one after it -- inserted
 * BEFORE the input region when focus mode owns the bottom rows. */
static void ai_pane_commit(FENSTER *wf)
{
 BUFFER *b = wf->b;
 if (g_ai_chat_focus && b->mxlines > g_ai_input_rows)
  e_new_line(b->mxlines - g_ai_input_rows, b);
 else
  e_new_line(b->mxlines, b);
}

/* Append one finished transcript line, keeping the input region (if any) at the
 * very bottom by inserting just above it. */
static void ai_tr_line(FENSTER *wf, const char *str)
{
 BUFFER *b = wf->b;
 int at;
 if (!g_ai_chat_focus) {
  print_to_end_of_buffer(b, (char *)str, b->mx.x);
  return;
 }
 at = b->mxlines - g_ai_input_rows;      /* first input-region line */
 if (at < 0) at = 0;
 e_new_line(at, b);                      /* insert a blank line before the region */
 ai_pane_set_line(wf, at, str);
}

/* Columns the pane wraps reply text to: inside its two borders, never wider than
 * the line buffer, capped at a readable prose measure even when the pane is much
 * wider (long unbroken lines of prose are hard to read -- editors and chat UIs
 * cap the measure rather than fill the whole width), and never absurdly small.
 * Chat streaming and every pane message share this, so wrapping is consistent. */
#define AI_PANE_MAX_COLS 80
static int ai_pane_width(FENSTER *wf)
{
 int w = wf->e.x - wf->a.x - 2;
 if (w > wf->b->mx.x - 1) w = wf->b->mx.x - 1;
 if (w > AI_PANE_MAX_COLS) w = AI_PANE_MAX_COLS;
 if (w < 16) w = 16;
 return w;
}

/* The streamed line has reached the pane width: emit it up to the last space
 * (word wrap; a hard break if the run has no space) and carry the remainder to a
 * fresh line, so a long reply reads DOWN the window instead of scrolling off to
 * the right.  Continuation lines carry no "AI:" prefix, matching a real chat. */
static void ai_stream_wrap(ai_chat_session *s, FENSTER *wf)
{
 int brk = (int)s->plen;                 /* default: hard break at the end */
 int k, start, rem, col, m;
 for (k = (int)s->plen - 1; k > 0; k--)
  if (s->pending[k] == ' ') { brk = k; break; }
 { char saved = s->pending[brk];         /* emit pending[0..brk) as one line */
   s->pending[brk] = '\0';
   ai_pane_set_last(wf, s->pending);
   ai_pane_commit(wf);
   s->pending[brk] = saved; }
 start = (brk < (int)s->plen && s->pending[brk] == ' ') ? brk + 1 : brk;
 rem = (int)s->plen - start;
 if (rem < 0) rem = 0;
 memmove(s->pending, s->pending + start, (size_t)rem);
 s->pending[rem] = '\0';
 s->plen = rem;
 for (col = 0, m = 0; m < rem; m++)
  if (((unsigned char)s->pending[m] & 0xC0) != 0x80) col++;
 s->pcols = col;
}

/* A "working" indicator for the blocking Edit/Plan/Agent waits: a rotating
 * -\|/ plus the seconds elapsed, redrawn in place on one pane line so a slow
 * local model looks alive instead of frozen.  wpe_ai_complete calls ai_spin_cb
 * ~8x/second (see wpe_ai_progress_cb). */
typedef struct { FENSTER *f; const char *label; int i; } ai_spin;

static void ai_spin_cb(void *ud, int elapsed_s)
{
 ai_spin *sp = ud;
 FENSTER *wf = ai_pane_win(sp->f);
 static const char frames[] = "-\\|/";
 char line[120];
 if (!wf) return;
 snprintf(line, sizeof line, "%s %c  %ds  (Esc cancels)",
          sp->label, frames[sp->i & 3], elapsed_s);
 sp->i++;
 ai_pane_set_last(wf, line);
}

/* Open a fresh pane line and return a spinner bound to `label`, ready to hand to
 * wpe_ai_complete as its progress callback. */
static ai_spin ai_spin_begin(FENSTER *f, const char *label)
{
 ai_spin sp;
 FENSTER *wf = ai_pane_win(f);
 sp.f = f; sp.label = label; sp.i = 0;
 if (wf) { ai_pane_commit(wf); ai_pane_set_last(wf, label); }
 return sp;
}

static void ai_split_tool(char *action, char **tool, char **arg);   /* defined below */

/* Format a "TOOL <name> <arg>" protocol line as a dim one-line status
 * ("  . reading <arg>"), so the raw protocol is never shown as the assistant's
 * answer -- for both a leading tool call and one that follows a preamble.
 * `line` need not be NUL-terminated at `len`. */
static void ai_tool_status(const char *line, size_t len, char *out, size_t n)
{
 char first[600], *tool, *arg;
 const char *pretty;
 size_t l;
 if (len >= sizeof first) len = sizeof first - 1;
 memcpy(first, line, len); first[len] = '\0';
 l = strlen(first);
 while (l && (first[l-1] == ' ' || first[l-1] == '\r' || first[l-1] == '\n'))
  first[--l] = '\0';
 ai_split_tool(first, &tool, &arg);
 pretty = !strcmp(tool, "read_file") ? "reading"
        : !strcmp(tool, "grep")      ? "searching for"
        : !strcmp(tool, "list_dir")  ? "listing"
        : tool;
 snprintf(out, n, "  . %s %.560s", pretty, arg);
}

/* Stream a delta into the pane: append its characters to the current line and
 * start a new line at each '\n', repainting so tokens appear as they arrive.
 * The whole reply is also accumulated for the session log. */
static void ai_delta_cb(const char *delta, void *ud)
{
 ai_chat_session *s = ud;
 size_t dl = strlen(delta), i;
 FENSTER *wf;

 if (s->flen + dl + 1 > s->fcap) {            /* keep the whole reply too */
  size_t nc = s->fcap ? s->fcap : 512;
  char *nb;
  while (s->flen + dl + 1 > nc) nc *= 2;
  nb = realloc(s->full, nc);
  if (nb) { s->full = nb; s->fcap = nc; }
 }
 if (s->full && s->flen + dl + 1 <= s->fcap) {
  memcpy(s->full + s->flen, delta, dl);
  s->flen += dl;
  s->full[s->flen] = '\0';
 }

 int width;
 /* Decide from the reply's first line: a "TOOL ..." line is the model asking to
    INVESTIGATE (read a file, grep, ...), not its answer -- echoing it as "AI:
    TOOL read_file /path" reads like a broken reply, so show a dim status line
    and suppress the raw protocol; anything else is the answer and streams. */
 if (!s->decided && s->full) {
  const char *nl = memchr(s->full, '\n', s->flen);
  if (s->flen < 5 && !nl) return;                    /* wait: could be "TOOL " */
  if (!strncmp(s->full, "TOOL ", 5)) {
   char status[640];
   size_t fl = nl ? (size_t)(nl - s->full) : s->flen;
   ai_tool_status(s->full, fl, status, sizeof status);
   wf = ai_pane_win(s->ref);
   if (wf) ai_pane_set_last(wf, status);              /* replace "(gathering...)" */
   s->decided = 2;
   return;
  }
  s->decided = 1;                                     /* an answer -- stream it */
 }
 if (s->decided == 2) return;                         /* keep hiding the investigation */

 wf = ai_pane_win(s->ref);
 if (!wf) return;
 width = ai_pane_width(wf);
 if (!s->started) {          /* open a reply line under the "AI:" header once */
  ai_pane_commit(wf);
  s->started = 1;
 }

 /* Paint the not-yet-shown portion of `full` (this covers both the delta just
    arrived and anything buffered while we were deciding).  A model may put a
    reasoning preamble on the first line(s) and its TOOL call on a later line; a
    line that begins "TOOL " is hidden the same way a leading one is -- its dim
    status replaces the raw protocol, and nothing after it is echoed. */
 for (i = s->shown; i < s->flen; i++) {
  char c = s->full[i];
  if (c == '\r')
   continue;
  if (c == '\n') {
   if (s->hide_tool) {                     /* the hidden TOOL line just ended */
    char status[640];
    ai_tool_status(s->pending, s->plen, status, sizeof status);
    ai_pane_set_last(wf, status);
    s->decided = 2;                        /* suppress anything after the tool */
    s->plen = 0; s->pcols = 0; if (s->pending) s->pending[0] = '\0';
    s->shown = i + 1;
    return;
   }
   ai_pane_set_last(wf, s->pending ? s->pending : "");
   ai_pane_commit(wf);
   s->plen = 0;
   s->pcols = 0;
   if (s->pending) s->pending[0] = '\0';
   continue;
  }
  if (s->plen + 2 > s->pcap) {
   size_t nc = s->pcap ? s->pcap : 256;
   char *nb;
   while (s->plen + 2 > nc) nc *= 2;
   nb = realloc(s->pending, nc);
   if (!nb) return;
   s->pending = nb;
   s->pcap = nc;
  }
  s->pending[s->plen++] = c;
  s->pending[s->plen] = '\0';
  if (!s->hide_tool && s->plen == 5 && !strncmp(s->pending, "TOOL ", 5))
   s->hide_tool = 1;                        /* this fresh line is a tool call   */
  if (((unsigned char)c & 0xC0) != 0x80)   /* not a UTF-8 continuation byte */
   s->pcols++;
  if (!s->hide_tool && s->pcols >= width)   /* soft-wrap at the pane width */
   ai_stream_wrap(s, wf);
 }
 s->shown = s->flen;                                   /* painted up to here */
 if (s->hide_tool) {                                   /* tool line still arriving */
  char status[640];
  ai_tool_status(s->pending, s->plen, status, sizeof status);
  ai_pane_set_last(wf, status);
 } else
  ai_pane_set_last(wf, s->pending ? s->pending : "");  /* live partial line */
}

/* fd-loop callback: drain readable bytes, stream deltas into the pane. */
static void ai_fd_cb(int fd, void *data)
{
 ai_chat_session *s = data;
 int done = 0;
 (void)fd;
 if (!s || !s->active) return;
 if (wpe_ai_stream_pump(s->st, ai_delta_cb, s, &done) < 0) {
  ai_pane(s->ref, "[AI: transport error]", 0);
  s->active = 0;
  ai_chat_finish(s);
  return;
 }
 if (done) {
  /* the trailing partial is already on screen (painted live in ai_delta_cb) */
  if (s->full && s->flen) {
   char *tr = (s->turns < AI_CHAT_MAX_TOOL_TURNS)
              ? ai_chat_tool_result(s->ref, s->full) : NULL;
   wpe_ai_session_append("assistant", s->full);
   if (tr) {
    /* the model asked to investigate: feed the tool result and take another
       turn -- the editor never blocked, this just continues in the background */
    { size_t n = strlen(tr) + 32; char *m = malloc(n);
      if (m) { snprintf(m, n, "TOOL RESULT:\n%s", tr); wpe_ai_session_append("user", m); free(m); } }
    free(tr);
    s->turns++;
    wpe_fd_del(s->fd);                 /* close this turn's stream */
    wpe_ai_stream_free(s->st); s->st = NULL;
    ai_chat_next_turn(s);              /* stream the next turn (answer or tool) */
    return;
   }
  } else {
   /* nothing streamed: show the server's error if the request failed (e.g. a
      model the key cannot access -> "(no answer)" told the user nothing), else
      the plain empty-answer placeholder. */
   FENSTER *wf = ai_pane_win(s->ref);
   char *emsg = wpe_ai_stream_error_message(s->st);
   if (wf) {
    if (emsg) {
     char line[440];
     snprintf(line, sizeof line, AI_REPLY_PREFIX "error: %s", emsg);
     ai_pane_set_last(wf, line);
    } else {
     ai_pane_set_last(wf, AI_REPLY_PREFIX "(no answer)");
    }
   }
   free(emsg);
  }
  wpe_ai_session_save(s->ref);
  wpe_ai_trace("chat done");
  s->active = 0;
  ai_chat_finish(s);
 }
}

/* Tell the model which backend and model are actually serving THIS turn, so an
 * identity question ("which model are you?") is answered from the truth of the
 * moment rather than from an earlier turn in the shared conversation -- after
 * the user switches backends mid-chat, the history still holds the previous
 * backend's self-description, and without this line the new backend just parrots
 * it.  Rebuilt every turn, so it always names the backend now in use. */
static void ai_identity_text(char *buf, size_t n)
{
 const char *be = wpe_ai_backend_name(e_ai_backend);
 const char *tail =
   ". If you are asked which model, backend, or maker you are, answer from "
   "this line and ignore any earlier turn in the conversation that names a "
   "different model or maker -- earlier turns may come from another backend.";
 if (e_ai_model && *e_ai_model && strcmp(e_ai_model, "default"))
  snprintf(buf, n, "You are served by the \"%s\" backend, model \"%s\"%s",
           be, e_ai_model, tail);
 else
  snprintf(buf, n, "You are served by the \"%s\" backend%s", be, tail);
}

/**
 * ai_status_text - One-line "who am I talking to" indicator for the pane header:
 * the active backend, model and permission policy.
 * @buf: destination; @n: its size.
 *
 * Shown at the top of a fresh AI pane so the working setup is visible without
 * opening Options (the bottom bar is a fixed 80-column layout with no room for a
 * variable model name).
 */
static void ai_status_text(char *buf, size_t n)
{
 const char *be = wpe_ai_backend_name(e_ai_backend);
 const char *pol = wpe_ai_policy_name(e_ai_policy);
 const char *model = (e_ai_model && *e_ai_model) ? e_ai_model : "(default)";
 snprintf(buf, n, "[AI: %s | model: %s | policy: %s]", be, model, pol);
}

static size_t ai_append_identity(char *sys, size_t cap, size_t len)
{
 char id[512];
 wpe_ai_trace("chat identity backend=%s model=%s",
              wpe_ai_backend_name(e_ai_backend), e_ai_model ? e_ai_model : "");
 if (len >= cap - 32) return len;
 ai_identity_text(id, sizeof id);
 len += (size_t)snprintf(sys + len, cap - len, "%s\n\n", id);
 return len;
}

/* The project's standing AI instructions, capped so a large file cannot crowd
   out the rest of the prompt.  Kept modest on purpose. */
#define AI_PROJECT_MEMORY_MAX 4096

/**
 * ai_project_memory - Read the project's AI instructions file into `out`.
 * @out: buffer to fill (always NUL-terminated); @n: its size.
 * Return: bytes read (0 when there is no project instructions file).
 *
 * Looks for AGENTS.md (the emerging cross-tool convention), then the
 * xwpe-specific .xwpe-ai.md, in the working directory -- which is xwpe's cwd,
 * i.e. where it was launched: the project root.  Lets a repo carry standing
 * instructions (build command, code style, "do not touch X") that every AI turn
 * follows -- the cross-tool per-repo agent-instructions convention -- without the
 * user restating them each turn.
 */
static size_t ai_project_memory(char *out, size_t n)
{
 static const char *names[] = { "AGENTS.md", ".xwpe-ai.md" };
 size_t i, got = 0;
 out[0] = '\0';
 for (i = 0; i < sizeof names / sizeof names[0]; i++) {
  FILE *fp = fopen(names[i], "rb");
  if (!fp) continue;
  got = fread(out, 1, n - 1, fp);
  fclose(fp);
  out[got] = '\0';
  wpe_ai_trace("project memory %s bytes=%zu", names[i], got);
  break;
 }
 return got;
}

/**
 * ai_append_project_memory - Add the project instructions to a text prompt.
 * @sys/@cap/@len: prompt buffer being grown.  Return: the new length.
 * A no-op when there is no AGENTS.md / .xwpe-ai.md, or no room left.
 */
static size_t ai_append_project_memory(char *sys, size_t cap, size_t len)
{
 char mem[AI_PROJECT_MEMORY_MAX];
 size_t got = ai_project_memory(mem, sizeof mem);
 if (!got || len >= cap - got - 96) return len;
 len += (size_t)snprintf(sys + len, cap - len,
   "PROJECT INSTRUCTIONS (from the project's AGENTS.md -- follow these):\n%s\n\n",
   mem);
 return len;
}

/**
 * ai_append_open_files - Append the editor's open-files block to a system prompt
 * being built, so a text-assembled prompt (chat) carries the working set too.
 * @sys: prompt buffer being grown; @cap: its size; @len: current length.
 * @f:   the window the mode was invoked from (marks the focused file).
 * Return: the new length (unchanged when there is nothing to add or no room).
 */
static size_t ai_append_open_files(char *sys, size_t cap, size_t len, FENSTER *f)
{
 char ow[1400];
 if (!wpe_ai_open_windows_block(f, ow, sizeof ow)[0]) return len;
 if (len >= cap - strlen(ow) - 4) return len;
 len += (size_t)snprintf(sys + len, cap - len, "%s\n", ow);
 return len;
}

#ifdef DEBUGGER
extern int e_lsp_diag_snapshot(char *out, size_t sz);   /* we_debug.c */
#endif

/* The current file's live diagnostics as a prompt block (or "" if none), so the
 * assistant sees the same errors/warnings the user does and "fix this" works
 * without pasting them.  Shared by chat, Edit and Agent. */
static const char *ai_diag_block(char *buf, size_t sz)
{
 buf[0] = '\0';
#ifdef DEBUGGER
 { char d[1600]; int nd = e_lsp_diag_snapshot(d, sizeof d);
   if (nd > 0) {
    snprintf(buf, sz, "\nDIAGNOSTICS reported for the current file "
                      "(from the language server):\n%s", d);
    wpe_ai_trace("prompt diagnostics=%d", nd);
   } }
#endif
 return buf;
}

/* Build the chat system prompt: the assistant may INVESTIGATE the workspace
 * with read-only tools before answering, so questions about other files (not
 * just the open one) work.  Includes the workspace file listing, any live
 * diagnostics, and the current file for immediate context. */
static char *ai_build_system(FENSTER *f)
{
 char *ctx = ai_current_file_text(f);
 char **scope;
 int nsc, i;
 size_t cap = 8192, len = 0;
 char *sys = malloc(cap);
 const char *head =
   "You are an AI assistant embedded in the xwpe console editor. Answer "
   "concisely in plain text.\n"
   "You may INVESTIGATE the workspace before answering.  To read a file or "
   "search, reply with EXACTLY ONE line and nothing else:\n"
   "  TOOL read_file <path>\n"
   "  TOOL grep <pattern>\n"
   "  TOOL list_dir <path>\n"
   "  TOOL glob <name-pattern>   (e.g. *.c -- find files by name across the tree)\n"
   "I will reply with the result; then either use another tool or give your "
   "answer.  When you can answer, reply with the answer directly (no TOOL "
   "line).  Do not guess about files you have not read.\n\n";
 /* Capability manifest: tell the model what the xwpe AI integration ALREADY
    does, so it neither re-proposes built features as new ideas nor prints code
    for the user to retype -- it should point them to the shortcut that does the
    job.  Grounds every meta-question ("how could we improve this?") in reality. */
 const char *caps =
   "XWPE AI CAPABILITIES that already exist -- do NOT propose these as new; when "
   "the user would benefit, point them to the shortcut:\n"
   "- Edit the current file or selection: Alt-G e (applies your instruction; the "
   "change previews as a diff and Ctrl-U reverts it as one undo step).\n"
   "- Multi-file edit: Alt-G f.  Autonomous Agent (the read tools above PLUS "
   "run_command and write_file, gated by the permission dial ask/edits/auto): "
   "Alt-G g.  Build & fix until the compile passes: Alt-G b.\n"
   "- Replies stream token by token; Esc cancels a run mid-generation.\n"
   "- Proposed edits are reviewed as a diff changeset (Alt-T / Alt-V) and are "
   "revertible; a checkpoint is taken before a non-interactive run.\n"
   "- You already receive the open files, the workspace file list, the current "
   "file, live language-server diagnostics, and the project's AGENTS.md "
   "instructions when present (all below).\n"
   "- Backend and model are user-selectable (Ollama, OpenAI-compatible, Claude) "
   "in Options > AI.\n"
   "So when the user asks to CHANGE code, tell them to use Edit (Alt-G e) or the "
   "Agent (Alt-G g) rather than printing code for them to retype.\n\n";
 if (!sys) { free(ctx); return NULL; }
 len += (size_t)snprintf(sys + len, cap - len, "%s", head);
 len += (size_t)snprintf(sys + len, cap - len, "%s", caps);
 len = ai_append_project_memory(sys, cap, len);
 len = ai_append_identity(sys, cap, len);
 len = ai_append_open_files(sys, cap, len, f);
 nsc = wpe_ai_scope_files(f, e_project_is_open(), 1, &scope);
 if (len < cap - 32)
  len += (size_t)snprintf(sys + len, cap - len, "WORKSPACE FILES:\n");
 for (i = 0; i < nsc && len < cap - 256; i++)
  len += (size_t)snprintf(sys + len, cap - len, "  %s\n", scope[i]);
 wpe_ai_free_list(scope, nsc);
 { char diag[1700];
   if (ai_diag_block(diag, sizeof diag)[0] && len < cap - 1800)
    len += (size_t)snprintf(sys + len, cap - len, "%s", diag); }
 if (ctx && len < cap - 512)
  snprintf(sys + len, cap - len, "\n--- current file ---\n%.*s",
           (int)(cap - len - 32), ctx);
 free(ctx);
 return sys;
}

/**
 * ai_scan_tool_line - Find the model's "TOOL ..." action line within a reply.
 * @reply: the full assistant reply.
 * Return: a pointer to the start of the first line beginning with "TOOL ", or
 *         NULL when the reply has none.
 *
 * A tool-using model -- a local one especially -- often prefaces the tool line
 * with a sentence of reasoning ("Let me look at the current code..."), so the
 * action is not on the reply's first line.  Judging only the first line then
 * mistakes the preamble for the answer and the tool is never run.  Scan the line
 * starts instead (the same rule the agent loop uses).
 */
static const char *ai_scan_tool_line(const char *reply)
{
 const char *scan;
 for (scan = reply; scan; ) {
  if (!strncmp(scan, "TOOL ", 5)) return scan;
  scan = strchr(scan, '\n');
  if (scan) scan++;
 }
 return NULL;
}

/**
 * ai_split_tool - Split a "TOOL <name> <arg>" line into its name and argument.
 * @action: a mutable copy of the tool line (modified in place).
 * @tool:   out; the tool name, with a trailing ':' stripped.
 * @arg:    out; the argument text (empty string when absent).
 *
 * Tolerates the "TOOL read_file: /path" colon that models commonly write, so the
 * name still matches "read_file" instead of "read_file:".
 */
static void ai_split_tool(char *action, char **tool, char **arg)
{
 char *nm = action + 5, *sp = strchr(nm, ' ');
 size_t l;
 if (sp) { *sp = '\0'; *arg = sp + 1; } else *arg = (char *)"";
 l = strlen(nm);
 if (l && nm[l - 1] == ':') nm[l - 1] = '\0';
 *tool = nm;
}

/* If the reply contains a read-only tool call, run it and return the result
 * (malloc'd, to be fed back as the next turn's input); else return NULL so the
 * reply is treated as the final answer.  Chat only ever runs read-only tools.
 * The tool line is found past any reasoning preamble (ai_scan_tool_line). */
static char *ai_chat_tool_result(FENSTER *f, const char *reply)
{
 char action[1100], *tool, *arg;
 const char *line, *nl;
 (void)f;
 line = ai_scan_tool_line(reply);
 if (!line) return NULL;
 nl = strchr(line, '\n');
 { size_t l = nl ? (size_t)(nl - line) : strlen(line);
   if (l >= sizeof action) l = sizeof action - 1;
   memcpy(action, line, l); action[l] = '\0'; }
 /* trim trailing spaces */
 { size_t l = strlen(action); while (l && (action[l-1]==' '||action[l-1]=='\r')) action[--l]='\0'; }
 ai_split_tool(action, &tool, &arg);
 if (!strcmp(tool, "read_file"))
  return ai_read_file_bounded(arg);
 if (!strcmp(tool, "grep")) {
  char cmd[1300]; snprintf(cmd, sizeof cmd, "grep -rn -- %s .", arg); return ai_run_capture(cmd);
 }
 if (!strcmp(tool, "list_dir")) {
  char cmd[1200]; snprintf(cmd, sizeof cmd, "ls -la %s", arg[0] ? arg : "."); return ai_run_capture(cmd);
 }
 if (!strcmp(tool, "glob")) {
  char cmd[1300];
  snprintf(cmd, sizeof cmd,
    "find . -name %s -not -path '*/.*' 2>/dev/null | head -200",
    arg[0] ? arg : "*");
  return ai_run_capture(cmd);
 }
 return NULL;   /* unknown/non-read-only: treat the line as a normal answer */
}

/* Start (or continue) a chat turn: rebuild the request from the workspace
 * conversation and stream the reply into the pane.  Reused for the first turn
 * and for each read-only investigation turn. */
static void ai_chat_next_turn(ai_chat_session *s)
{
 FENSTER *f = s->ref;
 wpe_ai_msg msgs[16];
 wpe_ai_req req;
 char *sys, err[320];
 int nm = 0, np, i;
 FENSTER *wf;

 sys = ai_build_system(f);
 msgs[nm].role = "system"; msgs[nm].content = sys ? sys : ""; nm++;
 np = wpe_ai_session_messages(msgs + nm, 12);
 for (i = 0; i < np; i++) nm++;
 req.model = NULL; req.msgs = msgs; req.nmsgs = nm;
 err[0] = '\0';
 s->st = wpe_ai_stream_start(&req, err, sizeof err);
 free(sys);
 if (!s->st) {
  ai_pane(f, err[0] ? err : "[AI error] could not start", 0);
  s->active = 0;
  ai_chat_finish(s);
  return;
 }
 s->fd = wpe_ai_stream_fd(s->st);
 s->active = 1;
 s->started = 0;
 s->decided = 0;
 s->hide_tool = 0;
 s->shown = 0;
 s->flen = 0; if (s->full) s->full[0] = '\0';
 ai_chat_set_pending(s, AI_REPLY_PREFIX);     /* reply streams after "AI: " */
 wf = ai_pane_win(f);
 if (wf) {
  ai_pane_commit(wf);
  ai_pane_set_last(wf, AI_REPLY_PREFIX "(gathering the answer...)");
  s->started = 1;
 }
 wpe_fd_add(s->fd, POLLIN, ai_fd_cb, s);
 wpe_ai_trace("chat stream fd=%d turn=%d", s->fd, s->turns);
}

/* Alt-G: prompt for a question and start an asynchronous streaming reply. */
/* Multi-line composer: open a scratch editor window so the user writes the
 * prompt with the FULL editor (many lines, cut/paste, arrows, ...), finish with
 * @key{Esc}, and confirm Send.  Reuses the editor itself as the text area.
 * Returns 1 and fills `out` on Send, 0 otherwise. */
static int e_ai_compose(char *out, size_t outsz, const char *title,
                        const char *seed, FENSTER *f)
{
 ECNT *cn = f->ed;
 FENSTER *w;
 char *text;
 size_t l;

 if (e_edit(cn, (char *)title)) return 0;    /* scratch window, empty buffer   */
 w = cn->f[cn->mxedt];
 if (seed && *seed) {                        /* carry over what was already typed */
  e_buffer_set_text(w->b, seed);
  w->b->b.y = w->b->mxlines ? w->b->mxlines - 1 : 0;   /* caret after the text */
  w->b->b.x = w->b->bf[w->b->b.y].len;
  e_firstl(w, 1);
 }
 e_eingabe(cn);                              /* the real editor, until Esc     */
 text = ai_current_file_text(w);             /* join the typed lines           */
 w->save = 0;                                /* skip the "save changes?" prompt */
 e_close_window(w);
 if (!text) return 0;
 l = strlen(text);
 while (l && (text[l-1] == '\n' || text[l-1] == '\r' ||
              text[l-1] == ' '  || text[l-1] == '\t')) text[--l] = '\0';
 if (!text[0]) { free(text); return 0; }     /* empty -> cancel                */
 if (e_message(1, "Send this prompt to the AI?", f) != 'Y') { free(text); return 0; }
 strncpy(out, text, outsz - 1);
 out[outsz - 1] = '\0';
 free(text);
 return 1;
}

/* A roomier prompt than e_add_arguments, which caps input at 128 characters.
 * xwpe's dialog widgets are single-line, so the field holds up to AI_PROMPT_MAX
 * and scrolls; a @samp{Multi-line} button opens the full editor composer for a
 * long, multi-line prompt.  Returns 1 and fills `out` on Send, 0 on Cancel. */
static int e_ai_prompt(char *out, const char *title, FENSTER *f)
{
 W_OPTSTR *o = e_init_opt_kst(f);
 static char head[80];
 int ret;

 if (!o) return 0;
 o->xa = 6;  o->ya = 4;  o->xe = 73;  o->ye = 11;
 o->bgsw = 0;
 snprintf(head, sizeof head, "%.70s", title);
 o->name = head;
 o->crsw = AltO;
 out[0] = '\0';
 e_add_wrstr(3, 2, 3, 3, 62, AI_PROMPT_MAX - 1, 0, AltT, "Prompt (Enter=Send, Alt-M=multi-line, Esc=Cancel):", out, NULL, o);
 e_add_bttstr(18, 6, 1, AltO, " Send ", NULL, o);
 e_add_bttstr(32, 6, 4, AltM, "Multi-line", NULL, o);
 e_add_bttstr(50, 6, -1, WPE_ESC, "Cancel", NULL, o);
 /* A background task may be animating its spinner: mark a modal so the AI
    heartbeat's repaint (which moves the caret to the pane) is suppressed while
    this dialog owns the input -- otherwise it steals focus and eats keystrokes. */
 { extern int wpe_modal_active; int sv = wpe_modal_active; wpe_modal_active = 1;
   ret = e_opt_kst(o); wpe_modal_active = sv; }
 if (ret != WPE_ESC) {                        /* keep what was typed either way */
  strncpy(out, o->wstr[0]->txt, AI_PROMPT_MAX - 1);
  out[AI_PROMPT_MAX - 1] = '\0';
 }
 freeostr(o);
 if (ret == WPE_ESC) return 0;                /* cancel   */
 if (ret == AltM)    return 2;                /* Multi-line: caller decides where */
 return 1;                                    /* Send     */
}

/* One-shot prompt for Edit/Plan/Agent, which have no persistent input row: the
 * Multi-line button opens the full-editor composer, as before.  (Chat instead
 * carries the text into its fixed input row -- see e_ai_chat.) */
static int e_ai_prompt1(char *out, const char *title, FENSTER *f)
{
 int r = e_ai_prompt(out, title, f);
 if (r == 2) {
  char seed[AI_PROMPT_MAX], ct[96];
  strncpy(seed, out, sizeof seed - 1);
  seed[sizeof seed - 1] = '\0';
  snprintf(ct, sizeof ct, "%.48s  (Esc finishes, then Y sends)", title);
  return e_ai_compose(out, AI_PROMPT_MAX, ct, seed, f);
 }
 return r;
}

/* Send `text` as one chat turn: echo "You: ..." above the input row, add it to
 * the workspace conversation (context preserved) and start the async stream. */
static void e_ai_chat_send(FENSTER *f, const char *text)
{
 ai_chat_session *s;
 char line[AI_PROMPT_MAX + 8];

 const char *p = text;
 int first = 1;

 wpe_ai_trace("chat prompt=%s", text);
 if (g_ai_chat) { g_ai_chat->active = 0; ai_chat_finish(g_ai_chat); }
 /* Echo the prompt one transcript line per input line, so a multi-line prompt
    reads cleanly ("You: ..." then indented continuations) instead of showing an
    embedded newline as a control glyph. */
 while (first || *p) {
  const char *nl = strchr(p, '\n');
  int seg = nl ? (int)(nl - p) : (int)strlen(p);
  snprintf(line, sizeof line, "%s%.*s", first ? "You: " : "     ", seg, p);
  ai_pane(f, line, 0);                         /* inserts above the input region */
  first = 0;
  if (!nl) break;
  p = nl + 1;
 }
 wpe_ai_session_append("user", text);
 s = calloc(1, sizeof *s);
 if (!s) return;
 s->ref = f;
 g_ai_chat = s;
 /* Stream the reply; the model may take a few read-only tool turns first to
    investigate the workspace, all in the background (see ai_fd_cb). */
 ai_chat_next_turn(s);
}

/* ----- input-row editing: a small line editor over g_ai_input ------------- */

static int ai_in_prev(int p)            /* byte index of the char before p */
{
 if (p <= 0) return 0;
 p--;
 while (p > 0 && ((unsigned char)g_ai_input[p] & 0xC0) == 0x80) p--;
 return p;
}
static int ai_in_next(int p)            /* byte index of the char after p */
{
 if (p >= g_ai_input_len) return g_ai_input_len;
 p++;
 while (p < g_ai_input_len && ((unsigned char)g_ai_input[p] & 0xC0) == 0x80) p++;
 return p;
}
static int ai_in_bol(int p)             /* start of the input line holding p */
{
 while (p > 0 && g_ai_input[p - 1] != '\n') p--;
 return p;
}
static int ai_in_eol(int p)             /* end of the input line holding p */
{
 while (p < g_ai_input_len && g_ai_input[p] != '\n') p++;
 return p;
}
static void ai_in_insert(const char *bytes, int n)   /* insert at the caret */
{
 int i;
 if (n <= 0 || g_ai_input_len + n >= (int)sizeof g_ai_input) return;
 memmove(g_ai_input + g_ai_input_pos + n, g_ai_input + g_ai_input_pos,
         (size_t)(g_ai_input_len - g_ai_input_pos));
 for (i = 0; i < n; i++) g_ai_input[g_ai_input_pos + i] = bytes[i];
 g_ai_input_len += n;
 g_ai_input_pos += n;
 g_ai_input[g_ai_input_len] = '\0';
}
static void ai_in_backspace(void)       /* delete the char before the caret */
{
 int q;
 if (g_ai_input_pos <= 0) return;
 q = ai_in_prev(g_ai_input_pos);
 memmove(g_ai_input + q, g_ai_input + g_ai_input_pos,
         (size_t)(g_ai_input_len - g_ai_input_pos));
 g_ai_input_len -= (g_ai_input_pos - q);
 g_ai_input_pos = q;
 g_ai_input[g_ai_input_len] = '\0';
}
static void ai_in_delete(void)          /* delete the char at the caret */
{
 int q;
 if (g_ai_input_pos >= g_ai_input_len) return;
 q = ai_in_next(g_ai_input_pos);
 memmove(g_ai_input + g_ai_input_pos, g_ai_input + q,
         (size_t)(g_ai_input_len - q));
 g_ai_input_len -= (q - g_ai_input_pos);
 g_ai_input[g_ai_input_len] = '\0';
}
static void ai_in_vmove(int dir)        /* caret up/down one line, keep column */
{
 int bol = ai_in_bol(g_ai_input_pos);
 int col = g_ai_input_pos - bol;
 if (dir < 0) {
  if (bol == 0) return;
  { int pbol = ai_in_bol(bol - 1), peol = bol - 1;
    g_ai_input_pos = pbol + col;
    if (g_ai_input_pos > peol) g_ai_input_pos = peol; }
 } else {
  int eol = ai_in_eol(g_ai_input_pos);
  if (eol >= g_ai_input_len) return;
  { int nbol = eol + 1, neol = ai_in_eol(nbol);
    g_ai_input_pos = nbol + col;
    if (g_ai_input_pos > neol) g_ai_input_pos = neol; }
 }
}

/* Close the chat: drop the "> " input region and hand focus back to the file the
 * chat was opened from.  The transcript stays in the (now ordinary) pane. */
static void e_ai_chat_close(void)
{
 FENSTER *wf = g_ai_chat_pane;
 if (!wf) return;
 g_ai_chat_focus = 0;
 g_ai_chat_pane = NULL;
 { BUFFER *b = wf->b;
   while (g_ai_input_rows > 0 && b->mxlines > 0) {
    int y = b->mxlines - 1;
    FREE(b->bf[y].s);
    b->mxlines--;
    g_ai_input_rows--;
   } }
 ai_pane_paint(wf);
 if (g_ai_home_edt >= 0)
  e_switch_window(g_ai_home_edt, wf);
}

/* Arm the chat: dock and focus the pane, add the "> " input row, and remember it
 * as the active chat pane.  From here the pane is an ORDINARY window -- the main
 * editor loop drives it, so mouse, scrolling, resizing and switching to other
 * windows all work; only the input keys are intercepted (see e_ai_chat_key). */
static void e_ai_chat_arm(FENSTER *f)
{
 FENSTER *wf;
 int wi;

 g_ai_home_edt = -1;
 for (wi = 1; wi <= f->ed->mxedt; wi++)         /* remember the caller's window */
  if (f->ed->f[wi] == f) { g_ai_home_edt = f->ed->edt[wi]; break; }
 wf = ai_pane_win(f);
 if (!wf) return;
 for (wi = 1; wi <= f->ed->mxedt; wi++)         /* bring the pane to the front */
  if (f->ed->f[wi] == wf) { e_switch_window(f->ed->edt[wi], wf); break; }
 wf = ai_pane_win(f);
 if (!wf) return;

 g_ai_chat_pane = wf;
 g_ai_chat_focus = 1;
 g_ai_scroll_lock = 0;
 g_ai_input[0] = '\0';
 g_ai_input_len = 0;
 g_ai_input_pos = 0;
 g_ai_input_rows = 0;
 if (wf->b->mxlines == 0) e_new_line(0, wf->b);
 if (wf->b->mxlines <= 1) {                       /* one-time header on a fresh chat */
  char st[200];
  ai_status_text(st, sizeof st);
  ai_tr_line(wf, st);
  ai_tr_line(wf, "[Enter=send  Ctrl-J=newline  arrows move  PgUp/PgDn scroll  Esc leaves]");
 }
 ai_input_render(wf);                            /* creates the input region */
}

/* Handle one key while the AI chat pane is the FOCUSED window.  Returns 1 if the
 * key was an input action (consumed), 0 to let the editor handle it normally
 * (window switch, function keys, ...).  Mouse is handled by the editor before
 * this is reached, so dragging/resizing/switching windows all keep working. */
int e_ai_chat_key(FENSTER *f, int c)
{
 FENSTER *wf;

 if (!g_ai_chat_focus || !g_ai_chat_pane)
  return 0;
 /* The editor's key loop refreshes its own `f` only at the END of an iteration,
    so right after the chat was armed from an ASYNC callback (the agent finishing
    and arming a follow-up), the first key arrives with a STALE `f` -- the window
    that was focused before the arm.  If the chat pane is actually the current top
    window, the key is the pane's; treat it so, instead of leaking that first key
    into the old file.  If the user really switched to another window, let the
    editor have it. */
 if (f != g_ai_chat_pane) {
  if (f->ed->f[f->ed->mxedt] != g_ai_chat_pane)
   return 0;
  f = g_ai_chat_pane;
 }
 wf = f;
 if (c == WPE_ESC) { e_ai_chat_close(); return 1; }
 if (c == BUP) { ai_pane_scroll(wf, -(wf->e.y - wf->a.y - 2)); return 1; }
 if (c == BDO) { ai_pane_scroll(wf, +(wf->e.y - wf->a.y - 2)); return 1; }
 /* Up/Down move within a multi-line input; at its edges (or a single-line input)
    they browse the transcript, so you can read back the history with the arrows.
    Handled before the scroll-lock reset below so they keep the browsed view. */
 if (c == CUP) {
  if (g_ai_scroll_lock) { ai_pane_scroll(wf, -1); return 1; }   /* browse older */
  { int before = g_ai_input_pos; ai_in_vmove(-1);
    if (g_ai_input_pos != before) { ai_input_render(wf); return 1; } }
  ai_pane_scroll(wf, -1);                                       /* top of input: older */
  return 1;
 }
 if (c == CDO) {
  if (g_ai_scroll_lock) { ai_pane_scroll(wf, +1); return 1; }   /* reading back: newer */
  { int before = g_ai_input_pos; ai_in_vmove(1);
    if (g_ai_input_pos != before) ai_input_render(wf); }
  return 1;
 }
 g_ai_scroll_lock = 0;                           /* any other edit key returns to the input */
 if (c == WPE_CR) {                             /* Enter: send the whole input */
  if (g_ai_input_len > 0) {
   char sb[AI_PROMPT_MAX];
   strncpy(sb, g_ai_input, sizeof sb - 1);
   sb[sizeof sb - 1] = '\0';
   g_ai_input[0] = '\0';
   g_ai_input_len = 0;
   g_ai_input_pos = 0;
   ai_input_render(wf);
   e_ai_chat_send(f, sb);
  }
  return 1;
 }
 if (c == WPE_WR)   { ai_in_insert("\n", 1);                       ai_input_render(wf); return 1; }
 if (c == WPE_DC)   { ai_in_backspace();                           ai_input_render(wf); return 1; }
 if (c == ENTF)     { ai_in_delete();                              ai_input_render(wf); return 1; }
 if (c == CLE)      { g_ai_input_pos = ai_in_prev(g_ai_input_pos); ai_input_render(wf); return 1; }
 if (c == CRI)      { g_ai_input_pos = ai_in_next(g_ai_input_pos); ai_input_render(wf); return 1; }
 if (c == POS1)     { g_ai_input_pos = ai_in_bol(g_ai_input_pos);  ai_input_render(wf); return 1; }
 if (c == ENDE)     { g_ai_input_pos = ai_in_eol(g_ai_input_pos);  ai_input_render(wf); return 1; }
 if ((c >= 32 && c < 255) || e_input_was_char) { /* a printable char (ASCII/Unicode) */
  unsigned char u8[4];
  int n = (c >= 0x80) ? e_codepoint_to_utf8(c, u8) : (u8[0] = (unsigned char)c, 1);
  ai_in_insert((char *)u8, n);
  ai_input_render(wf);
  return 1;
 }
 return 0;                                        /* not an input key: editor handles it */
}

static int e_ai_chat(FENSTER *f)
{
 char err[320], line[400];

 if (g_ai_chat_pane) {                            /* already open: just refocus it */
  int wi;
  for (wi = 1; wi <= f->ed->mxedt; wi++)
   if (f->ed->f[wi] == g_ai_chat_pane) { e_switch_window(f->ed->edt[wi], f); return 0; }
 }
 err[0] = '\0';
 if (wpe_ai_preflight(e_ai_backend, err, sizeof err)) {
  snprintf(line, sizeof line, "[AI unavailable] %s", err);
  ai_pane(f, line, 1);
  return 0;
 }
 err[0] = '\0';
 if (wpe_ai_ensure_model(err, sizeof err)) {
  snprintf(line, sizeof line, "[AI] %s", err[0] ? err : "no model set (set AIModel)");
  ai_pane(f, line, 1);
  return 0;
 }

 e_ai_cli_mode = WPE_AI_CLI_TEXTONLY;
 wpe_ai_session_load(f);                         /* resume this workspace's talk */
 /* Open straight into the pane's input row -- no popup.  The pane is now a normal
    window: type and Enter to send, Ctrl-J for a newline, Esc to leave. */
 e_ai_chat_arm(f);
 return 0;
}

/* ======================= Edit mode ====================================== */

/* Apply new whole-file text with one undo snapshot (mirrors the LSP apply). */
/* Apply an accepted AI edit: the same one-undo whole-buffer swap the LSP apply
   path uses, so an AI edit reverts with a single Ctrl-U exactly like a code
   action.  Shared spine lives in we_edit.c. */
static void e_ai_apply_text(FENSTER *f, const char *newtext)
{
 e_replace_buffer_undoable(f, newtext);
}

/* Strip a leading ```lang fence and a trailing ``` fence, if present.  Always
 * returns a fresh malloc'd copy. */
static char *ai_strip_fences(const char *s)
{
 const char *start = s, *end;
 size_t len;
 char *out;
 while (*start == ' ' || *start == '\n' || *start == '\r' || *start == '\t')
  start++;
 if (!strncmp(start, "```", 3)) {
  const char *nl = strchr(start, '\n');
  if (nl) start = nl + 1;
 }
 len = strlen(start);
 end = start + len;
 while (end > start && (end[-1] == '\n' || end[-1] == '\r' ||
                        end[-1] == ' ' || end[-1] == '\t'))
  end--;
 if (end - start >= 3 && !strncmp(end - 3, "```", 3)) {
  end -= 3;
  while (end > start && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '))
   end--;
 }
 len = (size_t)(end - start);
 out = malloc(len + 2);
 if (!out) return NULL;
 memcpy(out, start, len);
 out[len] = '\n';
 out[len + 1] = '\0';
 return out;
}

/* Per-hunk accept/reject.  Shows each change hunk and asks y/n/a/q; returns the
 * reconstructed file text (malloc'd) built from the accepted hunks, or NULL if
 * the user cancelled or accepted nothing. */
/* Unchanged lines shown on each side of a hunk, so the reviewer sees WHERE the
   change lands (like a unified diff's context). */
#define AI_DIFF_CTX 2

/* Format one overlay row: a one-char gutter (' ' context, '-' delete, '+' add)
   plus the line text, clipped so it never spills past the box border. */
static char *ai_diff_fmt(char gutter, const char *text)
{
 char *s = malloc(544);
 if (s) snprintf(s, 544, "%c%.520s", gutter, text ? text : "");
 return s;
}

/* Like ai_diff_fmt but prefixed with the new-file line number (blank for a
   deleted line, which has none), so the reviewer can point at "line 42". */
static char *ai_diff_fmt_n(int lineno, char gutter, const char *text)
{
 char *s = malloc(560);
 if (!s) return NULL;
 if (lineno > 0) snprintf(s, 560, "%4d %c%.512s", lineno, gutter, text ? text : "");
 else            snprintf(s, 560, "     %c%.512s", gutter, text ? text : "");
 return s;
}

/* ai_diff_box_show - Render rows[] (each with its own colour attr) in a boxed,
   scrollable overlay and return the key the reviewer pressed.  PgUp/PgDn scroll
   when the content is taller than the box, so nothing is lost off the top; a key
   in `accept` (case-insensitive), or Enter/Esc, ends it -- any other key is
   ignored.  Returns 13 for Enter, WPE_ESC for Esc, else the uppercased key.  The
   shared preview surface for the Edit review and the agent write confirmation. */
static int ai_diff_box_show(FENSTER *f, char **rt, int *ra, int nrows,
                            const char *title, const char *hint, const char *accept,
                            int ya_pref)
{
 extern int wpe_modal_active;
 int maxw = 0, boxw, vis, xa, ya, xe, ye, top = 0, k, ret = WPE_ESC;
 int modal_save = wpe_modal_active;
 wpe_modal_active = 1;              /* a review/confirm box is up: async paints defer */

 for (k = 0; k < nrows; k++)
  if (rt[k] && (int)strlen(rt[k]) > maxw) maxw = (int)strlen(rt[k]);
 if (maxw < (int)strlen(title)) maxw = (int)strlen(title);
 if (maxw < (int)strlen(hint)) maxw = (int)strlen(hint);
 boxw = maxw + 3;
 if (boxw > MAXSCOL - 4) boxw = MAXSCOL - 4;
 vis = nrows;
 if (vis > MAXSLNS - 8) vis = MAXSLNS - 8;
 if (vis < 1) vis = 1;
 xa = (MAXSCOL - boxw) / 2; if (xa < 1) xa = 1;
 xe = xa + boxw;
 /* Place the box next to the lines it is about (ya_pref = their screen row) so
    the change reads in place; drop it just below, or flip above when there is no
    room below, and clamp to the screen.  ya_pref <= 0 keeps it near the top. */
 if (ya_pref > 0) {
  ya = ya_pref + 1;
  if (ya + vis + 1 > MAXSLNS - 2) ya = ya_pref - vis - 2;   /* flip above */
  if (ya < 1) ya = 1;
  if (ya + vis + 1 > MAXSLNS - 2) ya = 2;                   /* still no fit: top */
 } else {
  ya = 2;
 }
 ye = ya + vis + 1;

 fk_cursor(0);
 for (;;) {
  PIC *pic = e_std_kst(xa, ya, xe, ye, (char *)title, 1,
                       f->fb->nr.fb, f->fb->nt.fb, f->fb->ne.fb);
  int c, j;
  if (!pic) break;
  for (j = 0; j < vis; j++) {
   int row = top + j;
   char line[600];
   if (row >= nrows) break;
   snprintf(line, sizeof line, "%-*.*s", boxw - 2, boxw - 2, rt[row]);
   e_pr_str(xa + 1, ya + 1 + j, line, ra[row], 0, 0, 0, 0);
  }
  { int hl = (int)strlen(hint), hx = xa + (xe - xa - hl) / 2;
    if (hx < xa + 1) hx = xa + 1;
    e_pr_str(hx, ye, (char *)hint, f->fb->nr.fb, 0, 0, 0, 0); }
  e_refresh();

  c = e_getch();
  if (c == BUP)      { top -= vis; if (top < 0) top = 0; e_close_view(pic, 1); continue; }
  if (c == BDO)      { top += vis; if (top > nrows - vis) top = nrows - vis; if (top < 0) top = 0;
                       e_close_view(pic, 1); continue; }
  if (c == 13 || c == '\r' || c == '\n') ret = 13;
  else if (c == WPE_ESC)                 ret = WPE_ESC;
  else {
   int uc = e_toupper(c);
   if (accept && strchr(accept, uc)) ret = uc;
   else { e_close_view(pic, 1); continue; }           /* ignore other keys */
  }
  e_close_view(pic, 1);
  break;
 }
 fk_cursor(1);
 e_cursor(f, 0);
 wpe_modal_active = modal_save;
 return ret;
}

/* Read a whole file, or NULL if it does not exist / is too big -- used to show
   what an agent write_file would replace. */
static char *ai_slurp_file(const char *path)
{
 FILE *fp = fopen(path, "rb");
 long n;
 char *b;
 if (!fp) return NULL;
 fseek(fp, 0, SEEK_END); n = ftell(fp); fseek(fp, 0, SEEK_SET);
 if (n < 0 || n > 4000000) { fclose(fp); return NULL; }
 b = malloc((size_t)n + 1);
 if (b) { size_t r = fread(b, 1, (size_t)n, fp); b[r] = '\0'; }
 fclose(fp);
 return b;
}

/* ai_diff_review_hunk - Show one proposed hunk in the colored diff box and read
   the reviewer's decision: 'Y' apply, 'N' skip, 'A' apply the rest, 'Q'/Esc
   cancel.  Deleted lines are red, added lines green, context normal. */
static int ai_diff_review_hunk(FENSTER *f, wpe_ai_seg *segs, int nseg,
                               int i, int hunk, int total)
{
 int red = f->fb->db.fb, green = f->fb->dy.fb, ctx = f->fb->nt.fb;
 int pc = (i > 0 && !segs[i-1].is_change)
          ? (segs[i-1].an < AI_DIFF_CTX ? segs[i-1].an : AI_DIFF_CTX) : 0;
 int nc = (i+1 < nseg && !segs[i+1].is_change)
          ? (segs[i+1].an < AI_DIFF_CTX ? segs[i+1].an : AI_DIFF_CTX) : 0;
 int nrows = pc + segs[i].an + segs[i].bn + nc;
 char **rt;
 int   *ra;
 int    idx = 0, k, key, ret;
 char   title[48], hint[80];

 if (nrows <= 0) return 'Y';                    /* nothing to show -> accept */
 rt = malloc((size_t)nrows * sizeof *rt);
 ra = malloc((size_t)nrows * sizeof *ra);
 if (!rt || !ra) { free(rt); free(ra); return 'Q'; }

 { int j, newstart = 1, ln;
   for (j = 0; j < i; j++) newstart += segs[j].is_change ? segs[j].bn : segs[j].an;
   ln = newstart - pc;                          /* context above the change */
   for (k = segs[i-1].an - pc; pc && k < segs[i-1].an; k++)
    { rt[idx] = ai_diff_fmt_n(ln++, ' ', segs[i-1].a[k]); ra[idx++] = ctx; }
   for (k = 0; k < segs[i].an; k++)              /* deleted: no new-file number */
    { rt[idx] = ai_diff_fmt_n(0, '-', segs[i].a[k]); ra[idx++] = red; }
   ln = newstart;
   for (k = 0; k < segs[i].bn; k++)              /* added: numbered in the new file */
    { rt[idx] = ai_diff_fmt_n(ln++, '+', segs[i].b[k]); ra[idx++] = green; }
   ln = newstart + segs[i].bn;                   /* context below the change */
   for (k = 0; nc && k < nc; k++)
    { rt[idx] = ai_diff_fmt_n(ln++, ' ', segs[i+1].a[k]); ra[idx++] = ctx; }
 }

 snprintf(title, sizeof title, " Proposed change %d/%d ", hunk, total);
 snprintf(hint, sizeof hint, " y apply  n skip  a all  q cancel  PgUp/PgDn ");
 /* Screen row of this hunk in the file on screen (which still shows the OLD
    text): count old-file lines before it, then map through the window's scroll
    so the box appears over the lines being changed, not floating at the top. */
 { int j, oldstart = 1, srow;
   for (j = 0; j < i; j++) oldstart += segs[j].an;
   srow = f->a.y + (oldstart - 1) - f->s->c.y + 1;
   if (srow < f->a.y + 1) srow = f->a.y + 1;
   if (srow > f->e.y - 1) srow = f->e.y - 1;
   key = ai_diff_box_show(f, rt, ra, nrows, title, hint, "YNAQ", srow); }
 if (key == 13 || key == 'Y')      ret = 'Y';
 else if (key == 'N')              ret = 'N';
 else if (key == 'A')              ret = 'A';
 else                              ret = 'Q';   /* 'Q' / Esc / closed */
 for (k = 0; k < nrows; k++) free(rt[k]);
 free(rt); free(ra);
 return ret;
}

/* ai_diff_confirm_write - Preview what the agent's write_file would do to `path`
   as a colored diff (new file: all-green additions; overwrite: a real diff with
   context) and ask a single allow/deny.  So the user SEES the change before it
   lands, instead of approving a blind "write_file x (N bytes)".  Returns 1 to
   allow, 0 to deny.  Long unchanged runs collapse to a few context lines. */
static int ai_diff_confirm_write(FENSTER *f, const char *path,
                                 const char *oldtext, const char *newtext)
{
 int red = f->fb->db.fb, green = f->fb->dy.fb, ctx = f->fb->nt.fb;
 wpe_ai_seg *segs;
 int nseg = wpe_ai_diff_segments(oldtext ? oldtext : "", newtext ? newtext : "", &segs);
 int i, k, cap = 0, nrows = 0, key, allow;
 char **rt = NULL;
 int   *ra = NULL;
 char   title[80], hint[64];

 for (i = 0; i < nseg; i++)            /* upper bound on the rows we may emit */
  cap += segs[i].an + segs[i].bn + 1;
 if (cap <= 0) { wpe_ai_segs_free(segs, nseg); return 1; }   /* no change -> allow */
 rt = malloc((size_t)cap * sizeof *rt);
 ra = malloc((size_t)cap * sizeof *ra);
 if (!rt || !ra) { free(rt); free(ra); wpe_ai_segs_free(segs, nseg); return 1; }

 { int nl = 1;                                   /* running new-file line number */
 for (i = 0; i < nseg; i++) {
  if (segs[i].is_change) {
   for (k = 0; k < segs[i].an && nrows < cap; k++)      /* deleted: no new number */
    { rt[nrows] = ai_diff_fmt_n(0, '-', segs[i].a[k]); ra[nrows++] = red; }
   for (k = 0; k < segs[i].bn && nrows < cap; k++)
    { rt[nrows] = ai_diff_fmt_n(nl++, '+', segs[i].b[k]); ra[nrows++] = green; }
  } else {
   int an = segs[i].an;
   int head = (i > 0) ? AI_DIFF_CTX : 0;          /* context after a change */
   int tail = (i + 1 < nseg) ? AI_DIFF_CTX : 0;   /* context before the next */
   if (an <= head + tail) {
    for (k = 0; k < an && nrows < cap; k++)
     { rt[nrows] = ai_diff_fmt_n(nl++, ' ', segs[i].a[k]); ra[nrows++] = ctx; }
   } else {
    for (k = 0; k < head && nrows < cap; k++)
     { rt[nrows] = ai_diff_fmt_n(nl++, ' ', segs[i].a[k]); ra[nrows++] = ctx; }
    if (nrows < cap) { rt[nrows] = ai_diff_fmt_n(0, ' ', "..."); ra[nrows++] = ctx; }
    nl += an - head - tail;                       /* the hidden lines still count */
    for (k = an - tail; k < an && nrows < cap; k++)
     { rt[nrows] = ai_diff_fmt_n(nl++, ' ', segs[i].a[k]); ra[nrows++] = ctx; }
   }
  }
 }
 }
 snprintf(title, sizeof title, " Write %.48s ? ", path ? path : "file");
 snprintf(hint, sizeof hint, " y allow  n / Esc deny  PgUp/PgDn ");
 key = ai_diff_box_show(f, rt, ra, nrows, title, hint, "YN", -1);
 allow = (key == 13 || key == 'Y');
 for (k = 0; k < nrows; k++) free(rt[k]);
 free(rt); free(ra);
 wpe_ai_segs_free(segs, nseg);
 return allow;
}

static char *ai_hunk_apply(FENSTER *f, wpe_ai_seg *segs, int nseg,
                           int *naccepted, int *ntotal)
{
 int i, total = 0, hunk = 0, any = 0, all = 0, cancel = 0, k, nacc = 0;
 int *acc = calloc(nseg > 0 ? nseg : 1, sizeof *acc);
 size_t cap = 1024, len = 0;
 char *out;
 if (naccepted) *naccepted = 0;
 if (ntotal) *ntotal = 0;
 if (!acc) return NULL;
 for (i = 0; i < nseg; i++) if (segs[i].is_change) total++;

 for (i = 0; i < nseg && !cancel; i++) {
  int decision;
  if (!segs[i].is_change) continue;
  hunk++;
  if (all) { acc[i] = 1; any = 1; nacc++; continue; }
  decision = ai_diff_review_hunk(f, segs, nseg, i, hunk, total);
  if (decision == 'Y')      { acc[i] = 1; any = 1; nacc++; }
  else if (decision == 'A') { acc[i] = 1; any = 1; all = 1; nacc++; }
  else if (decision == 'N') { acc[i] = 0; }
  else                      { cancel = 1; }        /* 'Q' / Esc */
 }
 if (ntotal) *ntotal = total;
 if (naccepted) *naccepted = nacc;
 if (cancel || !any) { free(acc); return NULL; }

 out = malloc(cap);
 if (!out) { free(acc); return NULL; }
 out[0] = '\0';
 for (i = 0; i < nseg; i++) {
  char **lines;
  int n2;
  if (!segs[i].is_change) { lines = segs[i].a; n2 = segs[i].an; }
  else if (acc[i])        { lines = segs[i].b; n2 = segs[i].bn; }
  else                    { lines = segs[i].a; n2 = segs[i].an; }
  for (k = 0; k < n2; k++) {
   size_t ll = strlen(lines[k]);
   if (len + ll + 2 > cap) { while (len + ll + 2 > cap) cap *= 2; out = realloc(out, cap); }
   memcpy(out + len, lines[k], ll); len += ll; out[len++] = '\n';
  }
 }
 out[len] = '\0';
 free(acc);
 return out;
}

/* ================= asynchronous operations (non-blocking) =================
 * Edit/Plan/Agent used to block the editor in a nested poll loop while the model
 * generated.  Instead they now run on the shared fd-loop like chat: start the
 * stream, return control to the editor at once, and finish (show the diff, run
 * the next agent turn) from the fd callback.  A periodic timerfd "heartbeat"
 * animates the spinner during model silence -- the editor stays fully live.
 */

/* --- heartbeat: a ~120ms periodic timer, present only while AI work is in
 * flight, so the idle wpe_fd_poll(-1) gets the wakeups it needs to spin.
 * Mirrors the Wayland key-repeat timerfd idiom. */
static int  g_ai_hb_fd = -1;
static int  g_ai_hb_refs = 0;
static void (*g_ai_hb_tick)(void) = NULL;   /* animates the active spinner */

static void ai_hb_fire(int fd, void *data)
{
 uint64_t exp;
 (void)data;
 if (read(fd, &exp, sizeof exp) != (ssize_t)sizeof exp)
  return;
 if (g_ai_hb_tick)
  g_ai_hb_tick();
}

static void ai_hb_acquire(void (*tick)(void))
{
 g_ai_hb_tick = tick;
 if (g_ai_hb_refs++ > 0)
  return;
#ifdef __linux__
 g_ai_hb_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
 if (g_ai_hb_fd >= 0) {
  struct itimerspec its;
  its.it_value.tv_sec = 0;    its.it_value.tv_nsec = 120000000L;
  its.it_interval.tv_sec = 0; its.it_interval.tv_nsec = 120000000L;
  timerfd_settime(g_ai_hb_fd, 0, &its, NULL);
  wpe_fd_add(g_ai_hb_fd, POLLIN, ai_hb_fire, NULL);
 }
#endif
 /* Without timerfd the spinner still ticks when tokens arrive; only the silent
    gaps go unanimated.  Async operation itself does not depend on the timer. */
}

static void ai_hb_release(void)
{
 if (--g_ai_hb_refs > 0)
  return;
 g_ai_hb_refs = 0;
 g_ai_hb_tick = NULL;
#ifdef __linux__
 if (g_ai_hb_fd >= 0) {
  wpe_fd_del(g_ai_hb_fd);
  close(g_ai_hb_fd);
  g_ai_hb_fd = -1;
 }
#endif
}

/* growable conversation (message list) -- shared by the multi-turn ops */
struct ai_mlist { char **role; char **content; int n, cap; };
static void ai_ml_add(struct ai_mlist *m, const char *role, const char *content)
{
 if (m->n == m->cap) {
  m->cap = m->cap ? m->cap * 2 : 8;
  m->role = realloc(m->role, m->cap * sizeof *m->role);
  m->content = realloc(m->content, m->cap * sizeof *m->content);
 }
 m->role[m->n] = strdup(role);
 m->content[m->n] = strdup(content ? content : "");
 m->n++;
}
static void ai_ml_free(struct ai_mlist *m)
{
 int i;
 for (i = 0; i < m->n; i++) { free(m->role[i]); free(m->content[i]); }
 free(m->role); free(m->content);
 m->role = NULL; m->content = NULL; m->n = m->cap = 0;
}

/**
 * ai_ml_add_identity - State which backend/model is answering, as a system turn.
 * @m: the conversation being seeded.
 *
 * Keeps the assistant from claiming a different maker when the history was built
 * by another backend (see ai_identity_text).
 */
static void ai_ml_add_identity(struct ai_mlist *m)
{
 char id[512];
 ai_identity_text(id, sizeof id);
 ai_ml_add(m, "system", id);
}

/**
 * ai_ml_add_open_files - Hand the editor's open files to the model as a system
 * turn, so a mode knows the user's working set without having to discover it.
 * @m: the conversation being seeded.
 * @f: the window the mode was invoked from (used to mark the focused file).
 *
 * A no-op when nothing qualifies (no real file windows open).
 */
static void ai_ml_add_open_files(struct ai_mlist *m, FENSTER *f)
{
 char ow[1400];
 if (wpe_ai_open_windows_block(f, ow, sizeof ow)[0])
  ai_ml_add(m, "system", ow);
}

/**
 * ai_ml_add_project_memory - Seed the conversation with the project's standing
 * AI instructions (AGENTS.md), as a system turn.  A no-op when there is none.
 * The message-list peer of ai_append_project_memory, so the agent and the other
 * async modes follow the same repo conventions the chat does.
 */
static void ai_ml_add_project_memory(struct ai_mlist *m)
{
 char mem[AI_PROJECT_MEMORY_MAX + 96];
 char body[AI_PROJECT_MEMORY_MAX];
 if (!ai_project_memory(body, sizeof body)) return;
 snprintf(mem, sizeof mem,
   "PROJECT INSTRUCTIONS (from the project's AGENTS.md -- follow these):\n%s", body);
 ai_ml_add(m, "system", mem);
}

/**
 * ai_ml_add_diagnostics - Fold the current file's live language-server
 * diagnostics into the conversation as a system turn, so "fix this" needs no
 * pasting.
 * @m: the conversation being seeded.
 *
 * A no-op when the language server has reported nothing.
 */
static void ai_ml_add_diagnostics(struct ai_mlist *m)
{
 char diag[1700];
 if (ai_diag_block(diag, sizeof diag)[0])
  ai_ml_add(m, "system", diag);
}

/* --- one background operation at a time.  A single-turn op (Edit) leaves
 * `process` NULL; a multi-turn op (Agent, Plan) supplies process()/finish() and
 * the driver keeps taking turns until process() says stop. */
typedef struct ai_async_op ai_async_op;
struct ai_async_op {
 wpe_ai_stream *st;
 int      fd;
 FENSTER *f;                 /* target window -- validated before applying     */
 ECNT    *cn;
 int      save_id;
 ai_spin  spin;
 time_t   start;
 char    *acc;               /* current turn's reply collected from the stream */
 size_t   acc_len, acc_cap;
 int      had_error;          /* the finished turn was a backend failure         */
 int      sel_y0, sel_y1;     /* Edit scope: selected line range, or -1 = whole file */
 /* multi-turn state (unused by single-turn Edit) */
 struct ai_mlist ml;         /* the running conversation                        */
 const char *label;          /* spinner label, e.g. "[agent] working"          */
 int      iter, max_iter;
 int      paused;            /* skip the spinner while a modal (approval) is up */
 int    (*process)(ai_async_op *op, char *reply);  /* 0=continue 1=done -1=abort */
 void   (*finish)(ai_async_op *op);                /* finalize (apply, save)     */
 void    *ud;               /* op-specific payload (proposals, scope, ...)      */
 void   (*free_ud)(void *ud);                      /* free ud on finish/cancel   */
};

static ai_async_op *g_ai_op = NULL;

/* True while a background op is running -- new AI actions refuse until it ends
 * (or the user cancels it). */
int wpe_ai_busy(void) { return g_ai_op != NULL; }

static void ai_op_collect(const char *delta, void *ud)
{
 ai_async_op *op = ud;
 size_t dl = strlen(delta);
 if (op->acc_len + dl + 1 > op->acc_cap) {
  size_t nc = op->acc_cap ? op->acc_cap : 1024;
  char *nb;
  while (op->acc_len + dl + 1 > nc) nc *= 2;
  nb = realloc(op->acc, nc);
  if (!nb) return;
  op->acc = nb; op->acc_cap = nc;
 }
 memcpy(op->acc + op->acc_len, delta, dl);
 op->acc_len += dl;
 op->acc[op->acc_len] = '\0';
}

/* animate the in-flight op's spinner (called from the heartbeat) -- but not
 * while it is paused (a modal approval / review is on the pane). */
static void ai_op_tick(void)
{
 if (g_ai_op && !g_ai_op->paused)
  ai_spin_cb(&g_ai_op->spin, (int)(time(NULL) - g_ai_op->start));
}

/* Close just the stream fd (between turns of a multi-turn op the heartbeat and
 * conversation must survive). */
static void ai_op_stream_close(ai_async_op *op)
{
 if (op->fd >= 0) { wpe_fd_del(op->fd); op->fd = -1; }
 if (op->st) { wpe_ai_stream_free(op->st); op->st = NULL; }
}

/* Detach the op from the fd-loop and free it.  Must run BEFORE any modal review
 * so no AI callbacks (this stream, the heartbeat) fire underneath it. */
static void ai_op_detach(ai_async_op *op)
{
 if (op->fd >= 0) wpe_fd_del(op->fd);
 ai_hb_release();
 wpe_ai_stream_free(op->st);
 op->st = NULL;
 op->fd = -1;
 if (g_ai_op == op) g_ai_op = NULL;
 g_ai_bg_win = NULL;          /* pane caret handling returns to normal */
}

static void ai_op_free(ai_async_op *op)
{
 if (op->free_ud && op->ud) op->free_ud(op->ud);
 ai_ml_free(&op->ml);
 free(op->acc);
 free(op);
}

/* True if window `f` is still one of the desktop's live windows. */
static int ai_window_alive(ECNT *cn, FENSTER *f)
{
 int i;
 for (i = 0; i <= cn->mxedt; i++)
  if (cn->f[i] == f) return 1;
 return 0;
}

/* ---- request queue (stack depth 1) ------------------------------------------
 * A second request started while a task runs is queued rather than cancelling
 * the first (or being refused): it launches automatically when the current task
 * ends.  Depth 1 keeps it predictable -- one running plus one waiting, the
 * newest replacing any already waiting -- and a cancel (Esc) discards it. */
static struct {
 int      pending;
 void   (*launch)(FENSTER *f, const char *goal);
 char     goal[AI_PROMPT_MAX];
 FENSTER *f;
 ECNT    *cn;
} g_ai_queue;

/** ai_queue_clear - Drop any queued request (e.g. on cancel). */
static void ai_queue_clear(void) { g_ai_queue.pending = 0; }

/**
 * ai_enqueue - Queue `goal` to be launched by `launch` when the running task
 * ends.  @f is the window it belongs to.  Replaces an already-queued request.
 */
static void ai_enqueue(FENSTER *f, void (*launch)(FENSTER *, const char *),
                       const char *goal)
{
 char line[AI_PROMPT_MAX + 64];
 if (g_ai_queue.pending)
  ai_pane(f, "[AI] replaced the queued request", 0);
 g_ai_queue.pending = 1;
 g_ai_queue.launch  = launch;
 g_ai_queue.f       = f;
 g_ai_queue.cn      = f->ed;
 strncpy(g_ai_queue.goal, goal, sizeof g_ai_queue.goal - 1);
 g_ai_queue.goal[sizeof g_ai_queue.goal - 1] = '\0';
 snprintf(line, sizeof line, "[AI] queued -- runs after the current task: %s", goal);
 ai_pane(f, line, 1);
}

/** ai_queue_run - Launch the queued request, if any, now nothing is running. */
static void ai_queue_run(void)
{
 void (*launch)(FENSTER *, const char *) = g_ai_queue.launch;
 FENSTER *f = g_ai_queue.f;
 if (!g_ai_queue.pending) return;
 g_ai_queue.pending = 0;                 /* clear before launching (re-entrancy) */
 if (launch && ai_window_alive(g_ai_queue.cn, f))
  launch(f, g_ai_queue.goal);
}

/* Cancel whatever background op is running (user asked, or a new action starts).*/
void wpe_ai_cancel(void)
{
 ai_async_op *op = g_ai_op;
 FENSTER *f;
 int alive;
 ai_queue_clear();                    /* a cancel discards any queued follow-up */
 if (!op) return;
 f = op->f;
 alive = ai_window_alive(op->cn, f);
 ai_op_detach(op);
 if (alive) ai_pane(f, "[AI] cancelled", 0);
 ai_op_free(op);
 /* Leave the pane ready to type in: arm the chat input so the user can continue
    right there (ask a follow-up / a new question) instead of a dead pane. */
 if (alive) e_ai_chat_arm(f);
}

/* Stream done: reconstruct the file, review the diff, apply.  Runs from the fd
 * callback but only after ai_op_detach() has removed the AI fds. */
static void ai_edit_done(ai_async_op *op)
{
 FENSTER *f = op->f;
 char *clean;
 wpe_ai_seg *segs;
 int nseg, has_change = 0, i;
 char *now;

 ai_op_detach(op);                        /* leave the loop before going modal */
 if (!ai_window_alive(op->cn, f)) {       /* the file window was closed meanwhile */
  wpe_ai_trace("edit target gone");
  ai_op_free(op);
  return;
 }
 /* The backend reported a failure (e.g. the claude CLI is not logged in); its
    message is a diagnostic, not new file content.  Show it and apply NOTHING --
    an error must never overwrite the buffer. */
 if (op->had_error) {
  char l[360];
  snprintf(l, sizeof l, "[AI edit] backend error: %.320s",
           op->acc_len ? op->acc : "the backend did not return a result");
  ai_pane(f, l, 1);
  wpe_ai_trace("edit error (not applied)");
  ai_op_free(op);
  return;
 }
 if (op->acc_len == 0) {
  ai_pane(f, "[AI edit] no response", 0);
  ai_op_free(op);
  return;
 }
 clean = ai_strip_fences(op->acc);
 if (!clean) { ai_op_free(op); return; }

 /* A selection edit returns only the modified region; splice it back into the
    current file at the selected lines so the diff below shows just that hunk and
    the proven whole-file apply path is reused. */
 if (op->sel_y0 >= 0) {
  int y0 = op->sel_y0, y1 = op->sel_y1, last = f->b->mxlines - 1;
  char *full;
  if (y0 > last) y0 = last;
  if (y1 > last) y1 = last;
  full = ai_splice_lines(f, y0, y1, clean);
  if (full) { free(clean); clean = full; }
 }

 now = ai_current_file_text(f);
 nseg = wpe_ai_diff_segments(now ? now : "", clean, &segs);
 free(now);
 for (i = 0; i < nseg; i++) if (segs[i].is_change) { has_change = 1; break; }
 if (!has_change) {
  ai_pane(f, "[AI edit] no change", 0);
  wpe_ai_trace("edit no-change");
 } else {
  int nacc = 0, ntot = 0;
  char *result = ai_hunk_apply(f, segs, nseg, &nacc, &ntot);
  if (result) {
   char msg[80];
   e_ai_apply_text(f, result);
   if (ntot > 1)
    snprintf(msg, sizeof msg, "[AI edit] applied %d of %d hunks - Ctrl-U to undo", nacc, ntot);
   else
    snprintf(msg, sizeof msg, "[AI edit] applied - Ctrl-U to undo");
   ai_pane(f, msg, 0);
   wpe_ai_trace("edit applied %d/%d", nacc, ntot);
   free(result);
   if (op->save_id >= 0) e_switch_window(op->save_id, f);
  } else {
   ai_pane(f, "[AI edit] discarded", 0);
   wpe_ai_trace("edit discarded");
  }
 }
 wpe_ai_segs_free(segs, nseg);
 free(clean);
 ai_op_free(op);
}

/* fd-loop callback for the background Edit stream. */
static void ai_edit_fd_cb(int fd, void *data)
{
 ai_async_op *op = data;
 int done = 0;
 (void)fd;
 if (wpe_ai_stream_pump(op->st, ai_op_collect, op, &done) < 0) {
  FENSTER *f = op->f;
  ECNT *cn = op->cn;
  ai_op_detach(op);
  if (ai_window_alive(cn, f)) ai_pane(f, "[AI edit] transport error", 0);
  ai_op_free(op);
  ai_queue_run();
  return;
 }
 if (done) {
  op->had_error = wpe_ai_stream_had_error(op->st);   /* capture before detach frees st */
  ai_edit_done(op);
  ai_queue_run();                    /* start the next queued request, if any */
 }
}

/* ---- generic multi-turn driver (Agent, Plan) -------------------------------
 * Each turn is an async stream.  When it completes, op->process() decides the
 * next move (continue with a tool result, or stop); the driver then either
 * starts the next turn or calls op->finish() to wrap up.  All from the fd
 * callback, at the top-level idle point -- the editor stays interactive across
 * every turn. */
static void ai_conv_finish(ai_async_op *op);
static void ai_conv_fd_cb(int fd, void *data);

static void ai_conv_next_turn(ai_async_op *op)
{
 wpe_ai_msg *msgs;
 wpe_ai_req req;
 char err[320];
 int i, n = op->ml.n;

 msgs = malloc((n ? n : 1) * sizeof *msgs);
 if (!msgs) { ai_conv_finish(op); return; }
 for (i = 0; i < n; i++) { msgs[i].role = op->ml.role[i]; msgs[i].content = op->ml.content[i]; }
 req.model = NULL; req.msgs = msgs; req.nmsgs = n;
 err[0] = '\0';
 op->st = wpe_ai_stream_start(&req, err, sizeof err);
 free(msgs);
 if (!op->st) {
  if (ai_window_alive(op->cn, op->f)) {
   char l[360]; snprintf(l, sizeof l, "%s: %s", op->label, err[0] ? err : "stream failed");
   ai_pane(op->f, l, 0);
  }
  ai_conv_finish(op);
  return;
 }
 op->fd = wpe_ai_stream_fd(op->st);
 op->start = time(NULL);
 op->paused = 0;
 op->acc_len = 0;
 if (op->acc) op->acc[0] = '\0';
 op->spin = ai_spin_begin(op->f, op->label);
 wpe_fd_add(op->fd, POLLIN, ai_conv_fd_cb, op);
}

/* A turn's reply is complete: hand it to process(), then continue or finish. */
static void ai_conv_on_reply(ai_async_op *op)
{
 int r;
 op->paused = 1;                    /* no spinner while process() may go modal   */
 ai_op_stream_close(op);            /* leave the loop before any modal review    */
 /* A backend failure (e.g. the claude CLI not logged in) is not a turn to act
    on: do NOT feed it to process(), which could run a tool call or apply an
    edit parsed from an error string.  Report and stop. */
 if (op->had_error) {
  if (ai_window_alive(op->cn, op->f)) {
   char l[360];
   snprintf(l, sizeof l, "%s: backend error: %.300s", op->label,
            (op->acc && op->acc[0]) ? op->acc : "no result");
   ai_pane(op->f, l, 0);
  }
  wpe_ai_trace("conv error (stopped)");
  ai_conv_finish(op);
  return;
 }
 ai_ml_add(&op->ml, "assistant", op->acc ? op->acc : "");
 r = op->process ? op->process(op, op->acc ? op->acc : "") : 1;
 op->iter++;
 if (r != 0 || op->iter >= op->max_iter) {
  if (op->iter >= op->max_iter && r == 0 && ai_window_alive(op->cn, op->f))
   ai_pane(op->f, "[AI] reached the step limit - Alt-G g to continue the task", 0);
  ai_conv_finish(op);
  return;
 }
 ai_conv_next_turn(op);             /* keep going */
}

static void ai_conv_fd_cb(int fd, void *data)
{
 ai_async_op *op = data;
 int done = 0;
 (void)fd;
 if (wpe_ai_stream_pump(op->st, ai_op_collect, op, &done) < 0) {
  FENSTER *f = op->f;
  ECNT *cn = op->cn;
  ai_op_stream_close(op);
  if (ai_window_alive(cn, f)) ai_pane(f, "[AI] transport error", 0);
  ai_conv_finish(op);
  return;
 }
 if (done) {
  op->had_error = wpe_ai_stream_had_error(op->st);   /* capture before stream_close */
  ai_conv_on_reply(op);
 }
}

static void ai_conv_finish(ai_async_op *op)
{
 ai_op_stream_close(op);
 ai_hb_release();                   /* stop the heartbeat before finish() modals */
 if (g_ai_op == op) g_ai_op = NULL;
 g_ai_bg_win = NULL;
 if (op->finish && ai_window_alive(op->cn, op->f))
  op->finish(op);
 ai_op_free(op);
 ai_queue_run();                    /* start the next queued request, if any */
}

/* Launch a multi-turn op: op->ml is seeded, process()/finish()/max_iter set. */
static void ai_conv_start(ai_async_op *op, const char *label)
{
 op->label = label;
 g_ai_op = op;
 g_ai_bg_win = op->f;
 ai_hb_acquire(ai_op_tick);
 ai_conv_next_turn(op);
 /* keep editing the file while the agent/plan works in the background pane */
 if (g_ai_op == op && op->save_id >= 0)
  e_switch_window(op->save_id, op->cn->f[op->cn->mxedt]);
}

static int e_ai_edit(FENSTER *f)
{
 static char instr[AI_PROMPT_MAX];
 char err[320], line[360];
 char *cur, *user;
 static const char sys_file[] =
   "You are a precise code editor. Apply the user's instruction to the file "
   "below and return ONLY the complete modified file content - no markdown "
   "fences, no commentary, no explanation.";
 static const char sys_sel[] =
   "You are a precise code editor. The user selected a region of a file. Apply "
   "the instruction to the SELECTED REGION below and return ONLY the modified "
   "region, keeping its indentation - no markdown fences, no commentary, and do "
   "NOT include the rest of the file.";
 const char *sys;
 wpe_ai_msg msgs[2];
 wpe_ai_req req;
 ECNT *cn = f->ed;
 int save_id = -1, wi;
 int sy0 = -1, sy1 = -1, have_sel;
 ai_async_op *op;

 if (wpe_ai_busy()) {
  ai_pane(f, "[AI] a task is already running - press Esc to cancel it, or wait", 1);
  return 0;
 }
 have_sel = ai_block_range(f, &sy0, &sy1);
 instr[0] = '\0';
 if (!e_ai_prompt1(instr, have_sel ? "AI edit instruction (selection)"
                                   : "AI edit instruction", f) || !instr[0])
  return 0;
 /* Remember the edited window so focus returns to it after the pane work. */
 for (wi = 1; wi <= cn->mxedt; wi++)
  if (cn->f[wi] == f) { save_id = cn->edt[wi]; break; }
 err[0] = '\0';
 if (wpe_ai_preflight(e_ai_backend, err, sizeof err)) { ai_pane(f, err, 1); return 0; }
 err[0] = '\0';
 if (wpe_ai_ensure_model(err, sizeof err)) {
  ai_pane(f, err[0] ? err : "no model set", 1);
  return 0;
 }

 if (have_sel) { cur = ai_lines_text(f, sy0, sy1); sys = sys_sel; }
 else          { cur = ai_current_file_text(f);    sys = sys_file; }
 { char diag[1700], ow[1400]; ai_diag_block(diag, sizeof diag);
   wpe_ai_open_windows_block(f, ow, sizeof ow);
   const char *hdr = have_sel ? "--- selected region ---" : "--- file ---";
   size_t n = strlen(instr) + (cur ? strlen(cur) : 0) + strlen(diag) + strlen(ow) + 64;
   user = malloc(n);
   if (user) snprintf(user, n, "%s%s\n%s\n%s\n%s",
                      instr, diag, ow, hdr, cur ? cur : ""); }
 msgs[0].role = "system"; msgs[0].content = sys;
 msgs[1].role = "user";   msgs[1].content = user ? user : instr;
 req.model = NULL; req.msgs = msgs; req.nmsgs = 2;

 wpe_ai_trace("edit instr=%s", instr);
 err[0] = '\0';
 op = calloc(1, sizeof *op);
 if (!op) { free(user); free(cur); return 0; }
 op->st = wpe_ai_stream_start(&req, err, sizeof err);
 free(user);
 free(cur);
 if (!op->st) {
  snprintf(line, sizeof line, "[AI edit] %s", err[0] ? err : "could not start");
  ai_pane(f, line, 1);
  free(op);
  return 0;
 }
 /* Register the stream on the shared fd-loop and RETURN -- the editor stays
    interactive; ai_edit_fd_cb finishes (diff + apply) when the reply lands. */
 op->f = f; op->cn = cn; op->save_id = save_id;
 op->sel_y0 = have_sel ? sy0 : -1;
 op->sel_y1 = have_sel ? sy1 : -1;
 op->fd = wpe_ai_stream_fd(op->st);
 op->start = time(NULL);
 op->spin = ai_spin_begin(f, have_sel ? "[AI edit] working (selection)"
                                      : "[AI edit] working");
 g_ai_op = op;
 g_ai_bg_win = f;
 ai_hb_acquire(ai_op_tick);
 wpe_fd_add(op->fd, POLLIN, ai_edit_fd_cb, op);
 /* Hand focus back to the file so the user keeps editing while the model works;
    the pane spins in the background (its paints leave the caret here). */
 if (save_id >= 0)
  e_switch_window(save_id, cn->f[cn->mxedt]);
 wpe_ai_trace("edit stream fd=%d (async) sel=%d..%d", op->fd, op->sel_y0, op->sel_y1);
 return 0;
}

/* ======================= model picker =================================== */

/* e_ai_pick - a SCROLLABLE single-choice list in a boxed overlay.  Up/Down move
 * the selection, PgUp/PgDn page, Enter confirms, Esc cancels; the selected row is
 * highlighted.  Unlike a fixed radio group it has no length cap, so a backend
 * with many models (Ollama tags, the OpenAI catalogue) is fully browsable rather
 * than truncated at the box's height.  Returns the chosen index in labels[0..n),
 * or -1 if cancelled. */
#define AI_PICK_MAXW 44
static int e_ai_pick(FENSTER *f, const char *title, const char *const *labels,
                     int n, int cur)
{
 int sel = (cur >= 0 && cur < n) ? cur : 0, top = 0;
 int i, maxw = 0, boxw, vis, xa, ya, xe, ye, ret = -1;
 const char *hint = " Up/Dn move  PgUp/PgDn page  Enter ok  Esc cancel ";

 if (n <= 0) return -1;
 for (i = 0; i < n; i++)
  if (labels[i] && (int)strlen(labels[i]) > maxw) maxw = (int)strlen(labels[i]);
 if (maxw > AI_PICK_MAXW) maxw = AI_PICK_MAXW;
 if (maxw < (int)strlen(title)) maxw = (int)strlen(title);
 if (maxw < (int)strlen(hint)) maxw = (int)strlen(hint);
 boxw = maxw + 4;
 if (boxw > MAXSCOL - 4) boxw = MAXSCOL - 4;
 vis = n;
 if (vis > MAXSLNS - 8) vis = MAXSLNS - 8;
 if (vis < 1) vis = 1;
 xa = (MAXSCOL - boxw) / 2; if (xa < 1) xa = 1;
 xe = xa + boxw; ya = 2; ye = ya + vis + 1;

 fk_cursor(0);
 for (;;) {
  PIC *pic;
  int c, j;
  if (sel < top) top = sel;                     /* keep the selection on screen */
  if (sel >= top + vis) top = sel - vis + 1;
  if (top > n - vis) top = n - vis;
  if (top < 0) top = 0;
  pic = e_std_kst(xa, ya, xe, ye, (char *)title, 1,
                  f->fb->nr.fb, f->fb->nt.fb, f->fb->ne.fb);
  if (!pic) break;
  for (j = 0; j < vis; j++) {
   int row = top + j, attr;
   char line[600];
   if (row >= n) break;
   attr = (row == sel) ? f->fb->fz.fb : f->fb->nt.fb;   /* highlight the choice */
   snprintf(line, sizeof line, " %-*.*s", boxw - 3, boxw - 3,
            labels[row] ? labels[row] : "");
   e_pr_str(xa + 1, ya + 1 + j, line, attr, 0, 0, 0, 0);
  }
  { int hl = (int)strlen(hint), hx = xa + (xe - xa - hl) / 2;
    if (hx < xa + 1) hx = xa + 1;
    e_pr_str(hx, ye, (char *)hint, f->fb->nr.fb, 0, 0, 0, 0); }
  e_refresh();

  c = e_getch();
  if (c == CUP)      { if (sel > 0) sel--; e_close_view(pic, 1); continue; }
  if (c == CDO)      { if (sel < n - 1) sel++; e_close_view(pic, 1); continue; }
  if (c == BUP)      { sel -= vis; if (sel < 0) sel = 0; e_close_view(pic, 1); continue; }
  if (c == BDO)      { sel += vis; if (sel > n - 1) sel = n - 1; e_close_view(pic, 1); continue; }
  if (c == 13 || c == '\r' || c == '\n') { ret = sel; e_close_view(pic, 1); break; }
  if (c == WPE_ESC)  { ret = -1; e_close_view(pic, 1); break; }
  e_close_view(pic, 1);                          /* ignore other keys, redraw */
 }
 fk_cursor(1);
 e_cursor(f, 0);
 return ret;
}

/* Choose a model for the current backend from the scrollable picker.  When
   `announce` is set the choice is echoed into the pane (the Alt-G menu path);
   the Options dialog passes 0 and refreshes its own Model button instead, so it
   is not disturbed. */
static int e_ai_pick_model_apply(FENSTER *f, int announce)
{
 char *names[32];
 char title[80], line[220];
 int n, i, sel;

 title[0] = '\0';
 n = wpe_ai_list_models(e_ai_backend, names, 32, title, sizeof title);
 if (n <= 0) { ai_pane(f, title[0] ? title : "no models found", 1); return 0; }
 for (i = 0; i < n; i++)                 /* pre-mark the model in use */
  if (e_ai_model && !strcmp(e_ai_model, names[i])) break;
 /* Name the source for the HTTP backends: both read the single AIEndpoint, so
    OpenAI-compatible lists whatever that URL serves.  Left on the Ollama default
    (localhost:11434, which also answers /v1/models) it returns Ollama's models --
    spelling out the endpoint makes that visible instead of surprising. */
 if (e_ai_backend == WPE_AI_OLLAMA || e_ai_backend == WPE_AI_OPENAI)
  snprintf(title, sizeof title, "Model (%d) at %s", n,
           e_ai_endpoint ? e_ai_endpoint : "?");
 else
  snprintf(title, sizeof title, "Model (%d available)", n);
 sel = e_ai_pick(f, title, (const char *const *)names, n, i < n ? i : 0);
 if (sel >= 0) {
  free(e_ai_model);
  e_ai_model = strdup(names[sel]);
  if (announce) {
   snprintf(line, sizeof line, "[AI] model = %s", names[sel]);
   ai_pane(f, line, 1);
  }
  wpe_ai_trace("model set %s", names[sel]);
 }
 for (i = 0; i < n; i++) free(names[i]);
 return 0;
}

/* The Model button's caption (fixed width so an in-place refresh always
   overwrites the previous, possibly longer, name) and the open settings dialog,
   shared with the button's action so the picker can float OVER the dialog and
   update the caption without tearing the dialog down. */
#define AI_OPT_MLABELW 24
static char       g_ai_opt_mlabel[AI_OPT_MLABELW + 1];
static W_OPTSTR  *g_ai_opt_dlg;
/* Backend order of the settings dialog's Backend radio (index -> backend id),
   shared with the Model button's action so it can list the models of the
   backend currently SELECTED in the radio, before Ok has applied it. */
static const int  g_ai_bk_order[4] =
 { WPE_AI_CLAUDECLI, WPE_AI_OLLAMA, WPE_AI_OPENAI, WPE_AI_CLAUDE };

static void ai_opt_mlabel(void)
{
 snprintf(g_ai_opt_mlabel, sizeof g_ai_opt_mlabel, "%-*.*s",
          AI_OPT_MLABELW, AI_OPT_MLABELW,
          (e_ai_model && *e_ai_model) ? e_ai_model : "(backend default)");
}

/* The Provider button's caption: the active profile name, or a hint to pick one. */
static char g_ai_opt_plabel[AI_OPT_MLABELW + 1];
static void ai_opt_plabel(void)
{
 snprintf(g_ai_opt_plabel, sizeof g_ai_opt_plabel, "%-*.*s",
          AI_OPT_MLABELW, AI_OPT_MLABELW,
          (e_ai_provider && *e_ai_provider) ? e_ai_provider : "(none - pick/save)");
}

/* Apply an endpoint URL typed in the settings dialog's Endpoint field (trimmed);
 * a blank field falls back to the local Ollama default so the HTTP backends
 * always have a reachable URL.  Shared by the Ok handler and the Model button so
 * Alt-M lists from a freshly typed URL. */
static void ai_set_endpoint(const char *text)
{
 char buf[512];
 const char *s = text ? text : "";
 size_t n;
 while (*s == ' ' || *s == '\t') s++;
 snprintf(buf, sizeof buf, "%s", s);
 n = strlen(buf);
 while (n && (buf[n - 1] == ' ' || buf[n - 1] == '\t')) buf[--n] = '\0';
 free(e_ai_endpoint);
 e_ai_endpoint = strdup(buf[0] ? buf : "http://localhost:11434");
}

/* Adopt an API key typed in the dialog's key field (trimmed) as the session key,
 * so it is used immediately by Alt-M without waiting for Ok.  A blank field
 * leaves the current key untouched (the field is never pre-filled with the
 * secret).  Returns 1 if a new key was set. */
static int ai_set_typed_key(const char *text)
{
 char buf[600];
 const char *s = text ? text : "";
 size_t n;
 while (*s == ' ' || *s == '\t') s++;
 snprintf(buf, sizeof buf, "%s", s);
 n = strlen(buf);
 while (n && (buf[n - 1] == ' ' || buf[n - 1] == '\t')) buf[--n] = '\0';
 if (!buf[0]) return 0;
 free(e_ai_key);
 e_ai_key = strdup(buf);
 return 1;
}

/* The write-string fields' indices in the dialog (in the order added), so the
 * Model button and Ok handler read the same fields. */
#define AI_OPT_ENDPOINT_WSTR 0
#define AI_OPT_CAFILE_WSTR   1
#define AI_OPT_KEY_WSTR      2

/* Model button action.  The scrollable picker (e_ai_pick) saves the screen it
   covers and restores it on close, so it floats OVER the settings dialog and
   leaves it in place -- no blink to the editor, no re-centre.  Rewrite the Model
   caption in the live dialog and return 0 so the option engine keeps it open. */
static int e_ai_opt_pick_model(FENSTER *f)
{
 int i;

 /* List the models of the backend SELECTED in the radio right now, not the one
    a previous Ok applied: the radio changes e_ai_backend only on Ok, so without
    this Alt-M would offer the old backend's models (e.g. Claude's after picking
    Ollama).  Sync the global from the live radio first (resetting the model, as
    a backend switch does), so one Alt-M shows the right list. */
 if (g_ai_opt_dlg && g_ai_opt_dlg->pn > 0) {
  int idx = g_ai_opt_dlg->pstr[0]->num;
  int be = (idx >= 0 && idx < 4) ? g_ai_bk_order[idx] : e_ai_backend;
  if (be != e_ai_backend) {
   e_ai_backend = be;
   free(e_ai_model);
   e_ai_model = NULL;
  }
 }
 /* Honour a URL the user just typed: list models from the endpoint shown in the
    field, not the one a previous Ok saved -- so Alt-M works before Ok. */
 if (g_ai_opt_dlg && g_ai_opt_dlg->wn > AI_OPT_ENDPOINT_WSTR)
  ai_set_endpoint(g_ai_opt_dlg->wstr[AI_OPT_ENDPOINT_WSTR]->txt);
 /* Trust the CA file shown in the field too, so listing an HTTPS bridge with a
    self-signed cert works from Alt-M before Ok. */
 if (g_ai_opt_dlg && g_ai_opt_dlg->wn > AI_OPT_CAFILE_WSTR) {
  const char *ca = g_ai_opt_dlg->wstr[AI_OPT_CAFILE_WSTR]->txt;
  while (ca && (*ca == ' ' || *ca == '\t')) ca++;
  free(e_ai_cafile);
  e_ai_cafile = (ca && *ca) ? strdup(ca) : NULL;
 }
 /* Use a key typed in the field so an authenticated endpoint (Groq, a Lumo
    bridge, ...) lists its models from Alt-M before the provider is saved. */
 if (g_ai_opt_dlg && g_ai_opt_dlg->wn > AI_OPT_KEY_WSTR)
  ai_set_typed_key(g_ai_opt_dlg->wstr[AI_OPT_KEY_WSTR]->txt);
 /* Switching the Backend radio to Ollama leaves the shared endpoint field on the
    previous (OpenAI) URL; Ollama serves /api/... at the host root, so an
    OpenAI-style base would 404.  Fall back to the local Ollama server and show
    that in the field. */
 if (e_ai_backend == WPE_AI_OLLAMA) {
  const char *fix = wpe_ai_ollama_endpoint_fixup(e_ai_endpoint);
  if (fix) {
   free(e_ai_endpoint); e_ai_endpoint = strdup(fix);
   if (g_ai_opt_dlg && g_ai_opt_dlg->wn > AI_OPT_ENDPOINT_WSTR) {
    W_O_WRSTR *w = g_ai_opt_dlg->wstr[AI_OPT_ENDPOINT_WSTR];
    snprintf(w->txt, (size_t)w->wmx + 1, "%s", fix);
   }
  }
 }
 e_ai_pick_model_apply(f, 0);                    /* announce=0: do not raise the pane */
 ai_opt_mlabel();
 if (g_ai_opt_dlg)
  for (i = 0; i < g_ai_opt_dlg->bn; i++)
   if (g_ai_opt_dlg->bstr[i]->sw == AltM) {
    strcpy(g_ai_opt_dlg->bstr[i]->header, g_ai_opt_mlabel);   /* same width, fits */
    break;
   }
 fk_cursor(0);                                   /* keep the dialog's caret hidden */
 return -1;             /* keep the dialog open and repaint it under the closed picker */
}

/* Refresh the live dialog widgets from the e_ai_* globals after a provider
 * profile is loaded: the Backend radio, the Endpoint field, and the Model and
 * Provider button captions.  The Ok read still reads the widgets, so keeping them
 * in sync means the loaded profile is exactly what Ok saves. */
static void ai_opt_sync_widgets(void)
{
 int i;
 if (!g_ai_opt_dlg) return;
 if (g_ai_opt_dlg->pn > 0)
  for (i = 0; i < 4; i++)
   if (g_ai_bk_order[i] == e_ai_backend) { g_ai_opt_dlg->pstr[0]->num = i; break; }
 if (g_ai_opt_dlg->wn > AI_OPT_ENDPOINT_WSTR) {
  W_O_WRSTR *w = g_ai_opt_dlg->wstr[AI_OPT_ENDPOINT_WSTR];
  snprintf(w->txt, (size_t)w->wmx + 1, "%s", e_ai_endpoint ? e_ai_endpoint : "");
 }
 if (g_ai_opt_dlg->wn > AI_OPT_CAFILE_WSTR) {
  W_O_WRSTR *w = g_ai_opt_dlg->wstr[AI_OPT_CAFILE_WSTR];
  snprintf(w->txt, (size_t)w->wmx + 1, "%s", e_ai_cafile ? e_ai_cafile : "");
 }
 if (g_ai_opt_dlg->wn > AI_OPT_KEY_WSTR) {       /* show the loaded provider's key */
  W_O_WRSTR *w = g_ai_opt_dlg->wstr[AI_OPT_KEY_WSTR];
  char *k = wpe_ai_read_openai_key_file(e_ai_provider);
  snprintf(w->txt, (size_t)w->wmx + 1, "%s", k ? k : "");
  free(k);
 }
 ai_opt_mlabel();
 ai_opt_plabel();
 for (i = 0; i < g_ai_opt_dlg->bn; i++) {
  if (g_ai_opt_dlg->bstr[i]->sw == AltM)
   strcpy(g_ai_opt_dlg->bstr[i]->header, g_ai_opt_mlabel);
  else if (g_ai_opt_dlg->bstr[i]->sw == AltV)
   strcpy(g_ai_opt_dlg->bstr[i]->header, g_ai_opt_plabel);
 }
}

/* Provider button (Alt-V): pick a saved OpenAI-compatible provider profile, or
 * save the current endpoint/model/CA as a new named one.  Loading a profile
 * switches the backend to OpenAI-compatible and applies its endpoint, model, CA
 * file and active name, so its model list and its own key file
 * (openai-api-key-<name>) are used.  Returns -1 to repaint the dialog in place. */
static int e_ai_opt_pick_provider(FENSTER *f)
{
 int np = wpe_ai_provider_count();
 const char *labels[34];
 char names[32][80];
 int i, n = 0, sel;
 for (i = 0; i < np && n < 32; i++) {
  snprintf(names[n], sizeof names[n], "%s", wpe_ai_provider_get(i)->name);
  labels[n] = names[n];
  n++;
 }
 labels[n++] = "[ + Save current as... ]";
 sel = e_ai_pick(f, "AI providers", labels, n, 0);
 if (sel < 0) { fk_cursor(0); return -1; }
 if (sel < np) {
  const struct wpe_ai_provider *p = wpe_ai_provider_get(sel);
  e_ai_backend = WPE_AI_OPENAI;
  free(e_ai_endpoint); e_ai_endpoint = strdup(p->endpoint);
  free(e_ai_model);    e_ai_model    = strdup((p->model && *p->model) ? p->model : "");
  free(e_ai_cafile);   e_ai_cafile   = (p->cafile && *p->cafile) ? strdup(p->cafile) : NULL;
  free(e_ai_provider); e_ai_provider = strdup(p->name);
 } else {
  char name[80] = "";
  /* field sw must differ from the dialog's confirm key (AltO) or Ok is shadowed */
  if (e_add_arguments(name, "Save provider as", f, 0, AltN, NULL) && name[0]) {
   wpe_ai_provider_set(name, e_ai_endpoint ? e_ai_endpoint : "",
                       e_ai_model, e_ai_cafile);
   free(e_ai_provider); e_ai_provider = strdup(name);
   /* Move a key typed this session into the new provider's own key file, so it
      is found as openai-api-key-<name> on the next launch. */
   if (e_ai_key && *e_ai_key)
    wpe_ai_write_openai_key(name, e_ai_key);
  }
 }
 ai_opt_sync_widgets();
 fk_cursor(0);
 return -1;
}

/* Options -> AI...: the settings home.  Enable checkbox, Backend radio, a Model
 * radio populated live from the selected backend, and a Policy radio -- all
 * marking the current choice.  Applies to the running session; Save Options
 * persists it (like the Editor dialog).  Changing the backend re-lists its
 * models on the next open. */
int e_ai_options(FENSTER *f)
{
 const int *bk = g_ai_bk_order;
 static const char *bklab[4] = { "Claude CLI (login)", "Ollama (local)   ",
                                 "OpenAI-compatible", "Claude API (key) " };
 static const char *pol[3] = { "Ask each action ", "Auto-accept edits", "Auto (skip asks)" };
 W_OPTSTR *o;
 int i, bcur, edopt_before, ret, new_be;

 /* The dialog shows the CURRENT model on a button; the (unbounded) model list
    lives in a scrollable picker floated over the dialog by the Model button, so
    the dialog itself stays a fixed size.  The Model button syncs the backend
    from the radio before listing, so a backend switch relists there and the
    dialog never needs to reopen -- Ok applies every field and saves ONCE, so the
    choice the user confirmed with Ok is exactly the one that persists. */
 o = e_init_opt_kst(f);
 if (!o) return 0;
 g_ai_opt_dlg = o;                              /* the Model button's fkt refreshes it */
 bcur = 0;
 for (i = 0; i < 4; i++) if (bk[i] == e_ai_backend) bcur = i;
 ai_opt_mlabel();
 ai_opt_plabel();

 /* Lay the dialog out in labelled SECTIONS with blank rows between them, so the
    grown set of fields reads as groups.  Backend and Permission (short radios)
    sit side by side to save height; the Connection fields are full width because
    URLs are long, and run top-to-bottom in the order you fill them -- Provider,
    Endpoint, CA file, API key, then Model (picked last, once the rest is set).
    Kept within ~22 rows so it fits a 24-line terminal.  e_opt_kst centres it. */
 { int w = 64, h = 20;
#ifdef WPE_AI_AGENT_HOST
   h = 21;                              /* one more row: the Agent engine */
#endif
   o->xa = (MAXSCOL - w) / 2; if (o->xa < 1) o->xa = 1;
   o->xe = o->xa + w;
   o->ya = (MAXSLNS - h) / 2; if (o->ya < 1) o->ya = 1;
   o->ye = o->ya + h; }
 o->bgsw = 0; o->crsw = AltO;
 o->name = "AI settings";

 e_add_sswstr(3, 2, 0, AltE, (f->ed->edopt & ED_AI_ENABLE) ? 1 : 0,
              "Enable AI assistant", o);

 /* --- Backend (left) + Permission (right), side by side.  Every radio needs a
        UNIQUE non-zero sw (sw==0 means "no field", unreachable).  Add order sets
        the group index, so backend (0) precedes permission (1) precedes agent. */
 e_add_txtstr(3, 4, "Backend:", o);
 e_add_txtstr(36, 4, "Permission:", o);
 for (i = 0; i < 4; i++)
  e_add_pswstr(0, 5, 5 + i, i, 4000 + i, (i == 3) ? bcur : 0, (char *)bklab[i], o);
 for (i = 0; i < 3; i++)
  e_add_pswstr(1, 38, 5 + i, i, 4200 + i, (i == 2) ? e_ai_policy : 0, (char *)pol[i], o);

 /* --- Connection: used by the OpenAI-compatible and Ollama backends.  Editable
        here (endpoint/CA/key) so a server or a self-signed local bridge needs no
        hand-edited xwperc; Provider switches saved profiles / saves the current. */
 e_add_txtstr(3, 10, "Connection (OpenAI-compatible / Ollama):", o);
 e_add_txtstr(5, 11, "Provider (Alt-V):", o);
 e_add_bttstr(22, 11, 0, AltV, g_ai_opt_plabel, e_ai_opt_pick_provider, o);
 e_add_wrstr(5, 12, 22, 12, 40, 255, -1, AltU, "Endpoint (Alt-U):",
             e_ai_endpoint ? e_ai_endpoint : "", NULL, o);   /* wstr[0] */
 e_add_wrstr(5, 13, 22, 13, 40, 511, -1, AltC, "CA file (Alt-C):",
             e_ai_cafile ? e_ai_cafile : "", NULL, o);        /* wstr[1] */
 { char *k = wpe_ai_read_openai_key_file(e_ai_provider);      /* show the saved key */
   e_add_wrstr(5, 14, 22, 14, 40, 255, -1, AltK, "API key (Alt-K):",
               k ? k : "", NULL, o);                          /* wstr[2] */
   free(k); }
 e_add_txtstr(5, 15, "Model (Alt-M):", o);
 e_add_bttstr(22, 15, 0, AltM, g_ai_opt_mlabel, e_ai_opt_pick_model, o);

#ifdef WPE_AI_AGENT_HOST
 /* --- Agent engine: the editor's own tool loop, or the Claude Code CLI. */
 e_add_txtstr(3, 17, "Agent (Alt-G g):", o);
 e_add_pswstr(2, 20, 17, 0, 4300, 0, "Built-in", o);
 e_add_pswstr(2, 35, 17, 1, 4301, e_ai_agent_engine, "Claude Code", o);
 e_add_txtstr(3, 18, "Tab/arrows move  Space selects  Alt-U/C/K/M/V edit", o);
 e_add_bttstr(24, 19, 1, AltO, "  Ok  ", NULL, o);
 e_add_bttstr(40, 19, -1, WPE_ESC, "Cancel", NULL, o);
#else
 e_add_txtstr(3, 17, "Tab/arrows move  Space selects  Alt-U/C/K/M/V edit", o);
 e_add_bttstr(24, 18, 1, AltO, "  Ok  ", NULL, o);
 e_add_bttstr(40, 18, -1, WPE_ESC, "Cancel", NULL, o);
#endif

 edopt_before = f->ed->edopt;
 ret = e_opt_kst(o);
 if (ret == WPE_ESC) { g_ai_opt_dlg = NULL; freeostr(o); return 0; }

 /* Ok read: apply EVERY field, then save once.  (A Model pick happens in place:
    the Model button floats the picker over the dialog and never returns here.) */
 f->ed->edopt = (f->ed->edopt & ~ED_AI_ENABLE) | (o->sstr[0]->num ? ED_AI_ENABLE : 0);
 e_ai_policy  = (o->pstr[1]->num >= 0 && o->pstr[1]->num < 3) ? o->pstr[1]->num : 0;
 new_be = bk[(o->pstr[0]->num >= 0 && o->pstr[0]->num < 4) ? o->pstr[0]->num : 0];
 if (o->wn > AI_OPT_ENDPOINT_WSTR)
  ai_set_endpoint(o->wstr[AI_OPT_ENDPOINT_WSTR]->txt);   /* apply the typed URL */
 if (o->wn > AI_OPT_CAFILE_WSTR) {                       /* apply the typed CA file */
  const char *ca = o->wstr[AI_OPT_CAFILE_WSTR]->txt;
  while (ca && (*ca == ' ' || *ca == '\t')) ca++;
  free(e_ai_cafile);
  e_ai_cafile = (ca && *ca) ? strdup(ca) : NULL;
 }
 if (o->wn > AI_OPT_KEY_WSTR && ai_set_typed_key(o->wstr[AI_OPT_KEY_WSTR]->txt))
  /* Persist a newly typed key to the active provider's file (or the generic
     one), so it survives the session; e_ai_key holds it meanwhile. */
  wpe_ai_write_openai_key(e_ai_provider, e_ai_key);
#ifdef WPE_AI_AGENT_HOST
 e_ai_agent_engine = (o->pstr[2]->num == 1) ? WPE_AI_ENGINE_CLAUDE_HOST
                                            : WPE_AI_ENGINE_BUILTIN;
#endif
 if (f->ed->edopt != edopt_before) {
  e_switch_blst(f->ed); e_ai_refresh_bars(f->ed); e_repaint_desk(f);
 }
 if (new_be != e_ai_backend) {
  /* Model names are backend-specific, so a backend switch drops the old model;
     the new backend then auto-picks one (or the user reopens and Alt-M's it). */
  e_ai_backend = new_be;
  free(e_ai_model); e_ai_model = NULL;
 }
 if (e_ai_backend == WPE_AI_OLLAMA) {
  /* Keep Ollama on a root endpoint: a leftover OpenAI-style base (from the shared
     field) would send /api/... under /v1 and fail. */
  const char *fix = wpe_ai_ollama_endpoint_fixup(e_ai_endpoint);
  if (fix) { free(e_ai_endpoint); e_ai_endpoint = strdup(fix); }
 }

 wpe_ai_trace("options backend=%s model=%s policy=%s enable=%d",
              wpe_ai_backend_name(e_ai_backend), e_ai_model ? e_ai_model : "-",
              wpe_ai_policy_name(e_ai_policy), (f->ed->edopt & ED_AI_ENABLE) ? 1 : 0);
 e_save_opt(f);              /* Ok persists the working mode -- no separate save */
 g_ai_opt_dlg = NULL;
 freeostr(o);
 return 0;
}

int e_ai_agent(FENSTER *f);         /* defined in the Agent section below */
#ifdef WPE_AI_AGENT_HOST
int e_ai_host(FENSTER *f);          /* the Claude Code agent engine, below */
#endif
static int e_ai_plan(FENSTER *f);   /* defined in the PLAN section below  */
static void e_ai_cycle_policy(FENSTER *f);  /* defined below e_ai_ui_key    */

/* ======================= first-use consent ============================= */

/**
 * ai_consent_path - Path of the "first-use notice accepted" marker file.
 * @buf: destination; @n: its size.  Overridable with XWPE_AI_CONSENT_FILE.
 * Return: buf.
 */
static const char *ai_consent_path(char *buf, size_t n)
{
 const char *e = getenv("XWPE_AI_CONSENT_FILE");
 const char *xdg = getenv("XDG_CONFIG_HOME");
 const char *home = getenv("HOME");
 if (e && *e)          snprintf(buf, n, "%s", e);
 else if (xdg && *xdg) snprintf(buf, n, "%s/xwpe/ai_consented", xdg);
 else if (home)        snprintf(buf, n, "%s/.config/xwpe/ai_consented", home);
 else                  snprintf(buf, n, ".xwpe_ai_consented");
 return buf;
}

/**
 * ai_consent_given - Whether the first-use notice has been accepted.
 *
 * An explicit XWPE_AI_CONSENTED wins (for tests); otherwise the marker file;
 * otherwise a dev/CI force-enable (XWPE_AI_ENABLE) implies it, so scripted and
 * test runs are never blocked on the prompt.
 */
static int ai_consent_given(void)
{
 char buf[1024];
 const char *e = getenv("XWPE_AI_CONSENTED");
 if (e) return *e == '1';
 if (access(ai_consent_path(buf, sizeof buf), F_OK) == 0) return 1;
 return getenv("XWPE_AI_ENABLE") ? 1 : 0;
}

/** ai_consent_remember - Persist that the first-use notice was accepted. */
static void ai_consent_remember(void)
{
 char buf[1024];
 char *slash;
 ai_consent_path(buf, sizeof buf);
 slash = strrchr(buf, '/');
 if (slash) { *slash = '\0'; mkdir(buf, 0700); *slash = '/'; }  /* best-effort dir */
 { FILE *fp = fopen(buf, "w"); if (fp) { fputs("1\n", fp); fclose(fp); } }
}

/* Shared with the AI action menu below (defined next to e_ai_menu_items):
   whether a letter is an AI menu shortcut, so the consent gate can accept a
   habitual "Alt-G a" as both the go-ahead and the action to run. */
static int ai_menu_has_key(int key);
static int ai_menu_dispatch_key(FENSTER *f, int key);

/**
 * ai_consent_gate - Show the one-time first-use notice before any AI action.
 * @f:        the current window (the notice renders in the AI pane).
 * @pass_key: out; set to the menu letter the user typed (e.g. 'a') when they
 *            answered the notice with a habitual shortcut, else left 0.
 * Return: 1 to proceed (choice remembered), 0 if the user declined.
 *
 * States the trust model up front -- local by default, opt-in, every edit
 * previewed and revertible -- so enabling the assistant is an informed choice.
 * Only the final prompt line is drawn in the attention colour; the notice
 * itself is plain text, so the panel does not look like an error.  Pressing a
 * menu shortcut (a Ask, e Edit, g Agent, s Settings...) counts as the
 * go-ahead AND is reported back so the caller runs that action straight away --
 * so the very first "Alt-G a" reaches Ask instead of appearing to hang.
 */
static int ai_consent_gate(FENSTER *f, int *pass_key)
{
 if (pass_key) *pass_key = 0;
 if (ai_consent_given()) return 1;
 ai_pane(f, "AI assistant -- first use.  It runs LOCALLY by default (Ollama on", 1);
 ai_pane(f, "localhost): nothing leaves your machine unless you point it at a", 1);
 ai_pane(f, "remote backend.  It is opt-in, every edit is previewed, and one", 1);
 ai_pane(f, "Ctrl-U reverts it.", 1);
 ai_pane(f, "After enabling, Alt-G opens a menu: a Ask  e Edit  g Agent  s Settings.", 1);
 ai_pane_attn(f, "Enter / y = enable and continue      n / Esc = not now");
 for (;;) {
  int c = e_getch();
  if (c == WPE_ESC || e_toupper(c) == 'N') {
   ai_pane(f, "[AI] not enabled - press Alt-G again when you want it", 0);
   return 0;
  }
  if (e_toupper(c) == 'Y' || c == 13 || c == '\r' || c == '\n') {
   ai_consent_remember();
   return 1;
  }
  if (ai_menu_has_key(c)) {          /* habitual Alt-G <letter>: enable + do it */
   ai_consent_remember();
   if (pass_key) *pass_key = c;
   return 1;
  }
  /* Any other key just leaves the notice up until the user chooses. */
 }
}

/* ======================= Alt-G prefix dispatch ========================== */
int e_ai_ui_key(FENSTER *f)
{
 if (!wpe_ai_enabled()) {
  ai_pane(f, "AI assistant is off - enable it in Options > Editor "
             "(the \"Ai assistant\" box), then press Alt-G again.", 1);
  return 0;
 }
 /* Alt-G no longer cancels a running task (that surprised users): Esc cancels
    it, and a second action is QUEUED to run after the current one.  Alt-G just
    opens the menu, even while a task runs. */
 /* First use: state the trust model and get an explicit go-ahead once.  If the
    user answered the notice with a menu shortcut, run that action directly. */
 {
  int pass = 0;
  if (!ai_consent_gate(f, &pass))
   return 0;
  if (pass && ai_menu_dispatch_key(f, pass))
   return 0;
 }
 /* Alt-G shows the action menu straight away -- the way Alt-F shows the File
    menu -- instead of an invisible "press another key" prefix.  The menu takes
    the item's letter as a shortcut (a=Ask e=Edit f=multi-File g=Agent b=Build
    y=policY n=New s=Settings), so a quick Alt-G a still jumps straight to Ask;
    pausing just leaves the menu on screen to pick from. */
 return e_ai_menu(f);
}

/* ======================= Agent mode ==================================== */

/* Turns the agent may take.  Each file read/list/grep and each edit/command is
   one turn, so a real task ("look at the project, then improve/build it") spends
   several just investigating -- 8 ran out mid-investigation.  24 leaves room to
   look around AND act while still bounding a runaway. */
#define AI_AGENT_MAX_ITERS 24
#define AI_TOOL_OUT_MAX    6000

/* Run a shell command; capture bounded stdout+stderr (malloc'd). */
static char *ai_run_capture(const char *cmd) { return wpe_ai_run_capture(cmd); }

static char *ai_read_file_bounded(const char *path)
{
 FILE *fp = fopen(path, "rb");
 char *out;
 size_t cap = 4096, len = 0;
 int ch;
 if (!fp) return strdup("(file not found)");
 out = malloc(cap);
 if (!out) { fclose(fp); return NULL; }
 while ((ch = fgetc(fp)) != EOF && len < AI_TOOL_OUT_MAX) {
  if (len + 2 > cap) { char *nb; cap *= 2; nb = realloc(out, cap); if (!nb) break; out = nb; }
  out[len++] = (char)ch;
 }
 out[len] = '\0';
 fclose(fp);
 return out;
}

/* Approve a mutating tool call (write_file / run_command) according to the
 * permission dial: auto passes everything, edits passes writes but asks for
 * commands, ask prompts every time.  1 = approved. */
static int ai_agent_approve(FENSTER *f, const char *what, int is_run)
{
 char line[640];
 if (e_ai_policy == WPE_AI_POLICY_AUTO ||
     (e_ai_policy == WPE_AI_POLICY_EDITS && !is_run)) {
  snprintf(line, sizeof line, "[agent] auto-approved (%s): %s",
           wpe_ai_policy_name(e_ai_policy), what);
  ai_pane(f, line, 0);
  wpe_ai_trace("agent auto-approve %s", what);
  return 1;
 }
 snprintf(line, sizeof line, "APPROVE?  %s", what);
 ai_pane_attn(f, line);
 ai_pane_attn(f, "y = allow    n / Esc = deny");
 for (;;) {
  int c = e_getch();
  if (c == WPE_ESC || e_toupper(c) == 'N') return 0;
  if (e_toupper(c) == 'Y' || c == 13 || c == '\r' || c == '\n') return 1;
 }
}

/* Persist the current settings (the working mode) so choices stick across
 * sessions without a manual "Save Options".  e_save_opt writes the active config
 * -- the same file the option dialog saves to. */
static void ai_persist_settings(FENSTER *f) { e_save_opt(f); }

/* One-shot flash message on the bottom status line: it takes the whole bar row
 * (which is otherwise full of key hints) and the next keystroke restores it.  A
 * quick confirmation that opens neither the pane nor a modal box. */
static int g_ai_flash_active = 0;

void wpe_ai_flash(FENSTER *f, const char *msg)
{
 char buf[MAXSCOL + 1];
 int n;
 if (!f || !f->fb) return;
 n = (int)strlen(msg);
 if (n > MAXSCOL - 2) n = MAXSCOL - 2;
 memcpy(buf, msg, (size_t)n); buf[n] = '\0';
 e_blk(MAXSCOL, 0, MAXSLNS - 1, f->fb->mt.fb);                 /* clear the bar row */
 e_pr_str(1, MAXSLNS - 1, buf, f->fb->ms.fb, -1, -1, f->fb->ms.fb, f->fb->mt.fb);
 e_refresh();
 g_ai_flash_active = 1;
}

int wpe_ai_flash_clear(FENSTER *f)
{
 if (!g_ai_flash_active) return 0;
 g_ai_flash_active = 0;
 if (f && f->fb) { e_pr_uul(f->fb); e_refresh(); }             /* redraw the key hints */
 return 1;
}

/* Mark the bar as flashed by a caller that painted the row itself (e.g. a
 * multi-colour flash), so the next keystroke restores the key hints just like a
 * plain wpe_ai_flash would. */
static void wpe_ai_flash_mark(void) { g_ai_flash_active = 1; }

/* Alt-G y: cycle the permission level ask -> edits -> auto.  Confirmed with a
 * one-shot status-line flash that shows all three levels with the ACTIVE one
 * highlighted (so you see which is selected, and the highlight moves as you
 * cycle) plus a short description of what it does.  Persisted automatically. */
static void e_ai_cycle_policy(FENSTER *f)
{
 static const char *word[3] = { "ask", "edits", "auto" };
 static const char *desc[3] = {
   "ask before each edit and command",
   "auto-accept edits, ask before commands",
   "run edits and commands unattended"
 };
 int col, i, act, base, hi;
 char seg[64];
 if (!f || !f->fb) return;
 e_ai_policy = (e_ai_policy + 1) % 3;
 act  = e_ai_policy;
 base = f->fb->mt.fb;                                   /* bar normal            */
 hi   = f->fb->ms.fb;                                   /* bar shortcut highlight */

 e_blk(MAXSCOL, 0, MAXSLNS - 1, base);                  /* take the whole bar row */
 col = 1;
 e_pr_str(col, MAXSLNS - 1, "Permissions:", base, -1, -1, base, base);
 col += 13;
 for (i = 0; i < 3; i++) {                              /* ask  edits  auto       */
  int c = (i == act) ? hi : base;
  snprintf(seg, sizeof seg, "%s", word[i]);
  e_pr_str(col, MAXSLNS - 1, seg, c, -1, -1, c, base);
  col += (int)strlen(word[i]) + 2;
 }
 snprintf(seg, sizeof seg, "-- %s", desc[act]);
 e_pr_str(col + 1, MAXSLNS - 1, seg, base, -1, -1, base, base);
 e_refresh();
 wpe_ai_flash_mark();                                   /* next key restores the bar */

 ai_persist_settings(f);
 wpe_ai_trace("policy set %s", wpe_ai_policy_name(e_ai_policy));
}

/* One agent turn: parse the action, run the tool (write/run ask for approval),
 * feed the result back.  Returns 1 when the agent is done, 0 to keep going.
 * Runs from the conversation driver with the spinner paused, so its approval
 * prompt (ai_agent_approve -> e_getch) is safe. */
static int ai_agent_process(ai_async_op *op, char *reply)
{
 FENSTER *f = op->f;
 char *firstnl, *act = NULL, *scan, action[1100];

 if (!ai_window_alive(op->cn, f)) return 1;

 /* Locate the protocol action: the first line that begins with "TOOL " or
    "DONE".  A tool-using model -- a local one especially -- often prefaces that
    line with a sentence of reasoning ("let me read the files first..."); judging
    only the reply's first line then mistakes the preamble for the final answer
    and the tool call is never run.  Scan the line starts instead. */
 for (scan = reply; scan; ) {
  if (!strncmp(scan, "TOOL ", 5) || !strncmp(scan, "DONE", 4)) { act = scan; break; }
  scan = strchr(scan, '\n');
  if (scan) scan++;
 }

 if (!act) {                                   /* no marker: the whole reply is the answer */
  ai_pane_multiline(f, reply[0] ? reply : "[agent] done", 0);
  wpe_ai_trace("agent answer (no marker)");
  return 1;
 }
 if (!strncmp(act, "DONE", 4)) {
  /* Show the whole DONE reply, not just its first line: for a question task the
     model's answer follows the DONE marker, and dropping it left the user with
     only "DONE - answering directly" and no answer. */
  ai_pane_multiline(f, reply[0] ? reply : "[agent] done", 0);
  wpe_ai_trace("agent done");
  return 1;
 }
 /* A TOOL line, possibly after a preamble: show the preamble as the agent's
    thought so its reasoning is not lost, then act on the tool line. */
 if (act > reply) {
  char pre[1024];
  size_t pl = (size_t)(act - reply);
  while (pl && (reply[pl-1] == '\n' || reply[pl-1] == ' ' || reply[pl-1] == '\t')) pl--;
  if (pl) {
   if (pl >= sizeof pre) pl = sizeof pre - 1;
   memcpy(pre, reply, pl); pre[pl] = '\0';
   ai_pane_multiline(f, pre, 0);
  }
 }
 firstnl = strchr(act, '\n');
 { size_t l = firstnl ? (size_t)(firstnl - act) : strlen(act);
   if (l >= sizeof action) l = sizeof action - 1;
   memcpy(action, act, l); action[l] = '\0'; }
 {
  char *tool, *arg, *result = NULL, paneln[640];
  ai_split_tool(action, &tool, &arg);   /* shared parse: tolerates "read_file:" */
  snprintf(paneln, sizeof paneln, "[agent] %s %s", tool, arg);
  ai_pane(f, paneln, 0);
  wpe_ai_trace("agent tool=%s arg=%s", tool, arg);

  if (!strcmp(tool, "list_dir")) {
   char cmd[1200]; snprintf(cmd, sizeof cmd, "ls -la %s", arg[0] ? arg : "."); result = ai_run_capture(cmd);
  } else if (!strcmp(tool, "read_file")) {
   result = ai_read_file_bounded(arg);
  } else if (!strcmp(tool, "grep")) {
   char cmd[1300]; snprintf(cmd, sizeof cmd, "grep -rn -- %s .", arg); result = ai_run_capture(cmd);
  } else if (!strcmp(tool, "glob")) {
   char cmd[1300];
   snprintf(cmd, sizeof cmd, "find . -name %s -not -path '*/.*' 2>/dev/null | head -200",
            arg[0] ? arg : "*");
   result = ai_run_capture(cmd);
  } else if (!strcmp(tool, "run_command")) {
   if (ai_agent_approve(f, arg, 1)) result = ai_run_capture(arg);
   else result = strdup("(denied by user)");
  } else if (!strcmp(tool, "write_file")) {
   char *content = NULL;
   if (firstnl) {
    char *body = firstnl + 1, *endm = strstr(body, "\n@@END");
    size_t cl = endm ? (size_t)(endm - body) : strlen(body);
    content = malloc(cl + 1);
    if (content) { memcpy(content, body, cl); content[cl] = '\0'; }
   }
   { char what[720]; int allow;
     snprintf(what, sizeof what, "write_file %s (%zu bytes)", arg, content ? strlen(content) : 0);
     if (!content)                            allow = 0;
     else if (e_ai_policy != WPE_AI_POLICY_ASK) allow = ai_agent_approve(f, what, 0);  /* auto/edits */
     else {
      /* Ask: show WHAT will be written as a diff (new file, or the change to an
         existing one) so the user approves seeing the content, not just a byte
         count. */
      char *old = ai_slurp_file(arg);
      allow = ai_diff_confirm_write(f, arg, old ? old : "", content);
      wpe_ai_trace("agent write confirm %s -> %s", arg, allow ? "allow" : "deny");
      free(old);
     }
     if (content && allow) {
      FILE *w = fopen(arg, "wb");
      if (w) { fwrite(content, 1, strlen(content), w); fclose(w);
               wpe_ai_reload_open_window(f, arg);   /* show the change if the file is open */
               result = strdup("(written)"); }
      else result = strdup("(write failed)");
     } else result = strdup("(denied by user)"); }
   free(content);
  } else {
   result = strdup("(unknown tool)");
  }
  if (!result) result = strdup("(no result)");
  { size_t n = strlen(result) + 32; char *tr = malloc(n);
    if (tr) { snprintf(tr, n, "TOOL RESULT:\n%s", result); ai_ml_add(&op->ml, "user", tr); free(tr); } }
  free(result);
 }
 return 0;                                      /* keep going */
}

/* Agent wrap-up: persist the exchange and, for an unattended run, offer the
 * changeset review (navigable like compile errors). */
static void ai_agent_finish(ai_async_op *op)
{
 if (op->ml.n > 0 && !strcmp(op->ml.role[op->ml.n - 1], "assistant"))
  wpe_ai_session_append("assistant", op->ml.content[op->ml.n - 1]);
 wpe_ai_session_save(op->f);
 if (e_ai_policy != WPE_AI_POLICY_ASK && wpe_ai_checkpoint_active())
  wpe_ai_changeset_review(op->f);
 /* Leave the pane ready for a follow-up instead of a dead log: arm the chat
    input on it (the agent's transcript stays visible), so the user types the
    next instruction right here rather than re-opening Alt-G.  The workspace
    session carries the context forward; Esc leaves. */
 ai_pane_attn(op->f, "done - type a follow-up below (Enter sends, Esc leaves)");
 e_ai_chat_arm(op->f);
}

/* Launch the autonomous agent on an already-formed `goal` (no prompt).  `extra`
   is an optional extra system message appended after the agent's identity (NULL
   for none) -- the build-fix loop uses it to state the exact build command.
   Shared by Alt-G g (prompted) and e_ai_fix_build. */
static int ai_agent_launch(FENSTER *f, const char *goal, const char *extra)
{
 char err[320];
 ECNT *cn = f->ed;
 ai_async_op *op;
 int save_id = -1, wi;
 const char *sys =
   "You are an autonomous coding agent working in the current directory of the "
   "xwpe editor. Reply with EXACTLY ONE action per turn as a single first line:\n"
   "  TOOL list_dir <path>\n"
   "  TOOL read_file <path>\n"
   "  TOOL grep <pattern>\n"
   "  TOOL glob <name-pattern>\n"
   "  TOOL run_command <shell command>\n"
   "  TOOL write_file <path>\n"
   "For write_file, put the new file content on the following lines, ending "
   "with a line that is exactly @@END .\n"
   "When the task is complete, reply with a line beginning DONE; then, on the "
   "following lines, give the user your answer (if they asked a question) or a "
   "short summary of what you changed. Output nothing else; wait for each tool "
   "result before continuing.";

 err[0] = '\0';
 if (wpe_ai_preflight(e_ai_backend, err, sizeof err)) { ai_pane(f, err, 1); return 0; }
 err[0] = '\0';
 if (wpe_ai_ensure_model(err, sizeof err)) {
  ai_pane(f, err[0] ? err : "no model set", 1);
  return 0;
 }

 { char st[200]; ai_status_text(st, sizeof st); ai_pane(f, st, 0); }
 { char line[1100]; snprintf(line, sizeof line, "[agent] task: %s", goal); ai_pane(f, line, 1); }
 wpe_ai_trace("agent task=%s", goal);

 wpe_ai_trace("agent policy=%s", wpe_ai_policy_name(e_ai_policy));
 wpe_ai_session_load(f);
 /* claudecli MUST stay text-only for the agent: our agent drives its OWN tool
    protocol -- the model replies "TOOL read_file x" etc. and WE run each tool
    under the permission dial.  The auto/edits dial used to let the CLI use its
    own Write/Edit/Bash tools (--dangerously-skip-permissions / acceptEdits),
    which bypasses that protocol entirely (the model would edit files itself and
    reply with a summary, not the TOOL lines the agent parses).  Keep it text-
    only; the xwpe policy still gates whether WE run each parsed tool
    (ai_agent_approve), independent of the CLI's own permission mode. */
 e_ai_cli_mode = WPE_AI_CLI_TEXTONLY;
 if (e_ai_policy != WPE_AI_POLICY_ASK) {           /* never unattended without a checkpoint */
  char **scope; int nsc = wpe_ai_scope_files(f, e_project_is_open(), 1, &scope);
  wpe_ai_checkpoint_create(f, scope, nsc);
  wpe_ai_free_list(scope, nsc);
  ai_pane(f, "[agent] checkpoint taken - changes are reviewable/revertible at the end", 0);
 }

 for (wi = 1; wi <= cn->mxedt; wi++)
  if (cn->f[wi] == f) { save_id = cn->edt[wi]; break; }
 op = calloc(1, sizeof *op);
 if (!op) return 0;
 op->f = f; op->cn = cn; op->save_id = save_id;
 op->max_iter = AI_AGENT_MAX_ITERS;
 op->process = ai_agent_process;
 op->finish = ai_agent_finish;
 ai_ml_add(&op->ml, "system", sys);
 ai_ml_add_identity(&op->ml);
 ai_ml_add_open_files(&op->ml, f);
 ai_ml_add_project_memory(&op->ml);
 if (extra && extra[0]) ai_ml_add(&op->ml, "system", extra);
 ai_ml_add_diagnostics(&op->ml);
 { wpe_ai_msg prior[12]; int np = wpe_ai_session_messages(prior, 12), i;
   for (i = 0; i < np; i++) ai_ml_add(&op->ml, prior[i].role, prior[i].content); }
 ai_ml_add(&op->ml, "user", goal);
 wpe_ai_session_append("user", goal);
 ai_conv_start(op, "[agent] working");
 return 0;
}

/* Queue adapter for the agent (matches the void(FENSTER*,const char*) slot). */
static void ai_launch_agent(FENSTER *f, const char *goal)
{
 ai_agent_launch(f, goal, NULL);
}

int e_ai_agent(FENSTER *f)
{
 static char goal[AI_PROMPT_MAX];
#ifdef WPE_AI_AGENT_HOST
 /* When the Agent engine is set to Claude Code (Options > AI), Alt-G g runs the
    hosted CLI with its own tools instead of the built-in tool loop. */
 if (e_ai_agent_engine == WPE_AI_ENGINE_CLAUDE_HOST)
  return e_ai_host(f);
#endif
 /* Prompt first, then decide: if a task is already running, queue this one to
    start when that finishes (instead of cancelling it or refusing). */
 goal[0] = '\0';
 if (!e_ai_prompt1(goal, "AI agent task", f) || !goal[0])
  return 0;
 if (wpe_ai_busy()) {
  ai_enqueue(f, ai_launch_agent, goal);
  return 0;
 }
 return ai_agent_launch(f, goal, NULL);
}

/* ai_build_command - the command that builds the current work, written into
   `out`.  Prefers `make` when a makefile is present (the usual project build);
   otherwise a syntax-only compile of the current file by its language, so a lone
   source file can still be checked.  Falls back to `make`.  The command is shown
   to the user and handed to the agent verbatim -- nothing is hidden. */
static void ai_build_command(FENSTER *f, char *out, size_t sz)
{
 const char *dir = (f->dirct && f->dirct[0]) ? f->dirct : ".";
 const char *mk[] = { "GNUmakefile", "makefile", "Makefile", NULL };
 char path[1200];
 int i;
 struct stat st;
 for (i = 0; mk[i]; i++) {
  snprintf(path, sizeof path, "%s%c%s", dir, DIRC, mk[i]);
  if (!stat(path, &st)) { snprintf(out, sz, "make"); return; }
 }
 if (f->datnam && f->datnam[0]) {
  char *full = e_mkfilename(f->dirct, f->datnam);
  const char *dot = strrchr(f->datnam, '.');
  const char *cc = NULL;
  if (dot) {
   if (!strcmp(dot, ".c")) cc = "gcc";
   else if (!strcmp(dot, ".cc") || !strcmp(dot, ".cpp") ||
            !strcmp(dot, ".cxx") || !strcmp(dot, ".C")) cc = "g++";
  }
  if (cc && full) {
   snprintf(out, sz, "%s -fsyntax-only -Wall '%s'", cc, full);
   free(full);
   return;
  }
  free(full);
 }
 snprintf(out, sz, "make");
}

/* e_ai_fix_build - run the build, and if it fails let the agent fix it in a loop:
   run the build command, read the compiler errors, edit the sources, re-run,
   repeat until it builds.  A specialised agent task -- it reuses the whole agent
   machinery (tools, permission dial, checkpoint/changeset) and just seeds the
   goal and the exact build command, so the user does not have to type either. */
static int e_ai_fix_build(FENSTER *f)
{
 char cmd[1024], extra[1700];
 if (wpe_ai_busy()) {
  ai_pane(f, "[AI] a task is already running - press Esc to cancel it, or wait", 1);
  return 0;
 }
 ai_build_command(f, cmd, sizeof cmd);
 wpe_ai_trace("fix-build cmd=%s", cmd);
 { char line[1200]; snprintf(line, sizeof line, "[fix-build] build command: %s", cmd);
   ai_pane(f, line, 1); }
 snprintf(extra, sizeof extra,
   "You are fixing a failing build. The build command is:\n  %s\n"
   "First run it with `TOOL run_command %s`. If it succeeds (no errors), reply "
   "DONE. Otherwise read the compiler errors, edit the source files to fix the "
   "root cause (not by silencing warnings), then run the build command again. "
   "Repeat until the build succeeds, then reply DONE with a short summary of "
   "what you changed.", cmd, cmd);
 return ai_agent_launch(f, "Fix the failing build.", extra);
}

/* ======================= PLAN mode (multi-file) ========================= */

#define AI_PLAN_MAX 32

typedef struct { char *path; char *text; } ai_proposal;

/* Collect PROPOSE <path> ... @@END blocks from a reply.  Returns 1 if the reply
 * also carried @@PLAN-DONE (or DONE). */
static int ai_plan_parse(const char *reply, ai_proposal *props, int *np)
{
 const char *p = reply;
 int done = 0;
 while (p && *p) {
  const char *nl = strchr(p, '\n');
  size_t l = nl ? (size_t)(nl - p) : strlen(p);
  if (!strncmp(p, "@@PLAN-DONE", 11) || (l >= 4 && !strncmp(p, "DONE", 4))) done = 1;
  else if (!strncmp(p, "PROPOSE ", 8) && *np < AI_PLAN_MAX) {
   char path[1024];
   size_t pl = l - 8;
   const char *body, *endm;
   if (pl >= sizeof path) pl = sizeof path - 1;
   memcpy(path, p + 8, pl); path[pl] = '\0';
   while (pl && (path[pl-1] == ' ' || path[pl-1] == '\r')) path[--pl] = '\0';
   body = nl ? nl + 1 : p + l;
   endm = strstr(body, "\n@@END");
   if (!endm && !strncmp(body, "@@END", 5)) endm = body;
   {
    size_t cl = endm ? (size_t)(endm - body) : strlen(body);
    props[*np].path = strdup(path);
    props[*np].text = malloc(cl + 2);
    if (props[*np].text) { memcpy(props[*np].text, body, cl); props[*np].text[cl] = '\n'; props[*np].text[cl + 1] = '\0'; }
    (*np)++;
   }
   if (endm) { p = strchr(endm + 1, '\n'); if (p) p++; continue; }
   break;
  }
  if (!nl) break;
  p = nl + 1;
 }
 return done;
}

/* Plan carries the accumulated proposals and the study scope across turns. */
typedef struct {
 ai_proposal props[AI_PLAN_MAX];
 int    np;
 char **scope;
 int    nsc;
} ai_plan_data;

static void ai_plan_free_data(void *ud)
{
 ai_plan_data *pd = ud;
 int i;
 for (i = 0; i < pd->np; i++) { free(pd->props[i].path); free(pd->props[i].text); }
 wpe_ai_free_list(pd->scope, pd->nsc);
 free(pd);
}

/* One plan turn: read-only tools while studying, then accumulate PROPOSE blocks
 * until @@PLAN-DONE.  Returns 1 when the plan is complete (or a plain answer). */
static int ai_plan_process(ai_async_op *op, char *reply)
{
 ai_plan_data *pd = op->ud;
 FENSTER *f = op->f;
 char action[1100], *firstnl;
 int done;

 if (!ai_window_alive(op->cn, f)) return 1;
 done = ai_plan_parse(reply, pd->props, &pd->np);
 firstnl = strchr(reply, '\n');
 { size_t l = firstnl ? (size_t)(firstnl - reply) : strlen(reply);
   if (l >= sizeof action) l = sizeof action - 1; memcpy(action, reply, l); action[l] = '\0'; }
 if (!done && !strncmp(action, "TOOL ", 5)) {
  char *tool = action + 5, *arg = strchr(tool, ' '), *result = NULL, paneln[640];
  if (arg) { *arg = '\0'; arg++; } else arg = (char *)"";
  snprintf(paneln, sizeof paneln, "[plan] %s %s", tool, arg);
  ai_pane(f, paneln, 0);
  wpe_ai_trace("plan tool=%s arg=%s", tool, arg);
  if (!strcmp(tool, "read_file"))      result = wpe_ai_read_scope_file(f, arg);
  else if (!strcmp(tool, "list_dir")) { char cmd[1200]; snprintf(cmd, sizeof cmd, "ls -la %s", arg[0] ? arg : "."); result = ai_run_capture(cmd); }
  else if (!strcmp(tool, "grep"))     { char cmd[1300]; snprintf(cmd, sizeof cmd, "grep -rn -- %s .", arg); result = ai_run_capture(cmd); }
  else result = strdup("(only read-only tools are allowed in plan mode)");
  if (!result) result = strdup("(not found)");
  { size_t n2 = strlen(result) + 32; char *tr = malloc(n2);
    if (tr) { snprintf(tr, n2, "TOOL RESULT:\n%s", result); ai_ml_add(&op->ml, "user", tr); free(tr); } }
  free(result);
  return 0;
 }
 if (!done && pd->np == 0) {
  ai_pane(f, action, 0);                /* neither a tool nor a plan: final text */
  return 1;
 }
 return done ? 1 : 0;
}

/* Plan wrap-up: persist, then the permission moment -- list the proposed files
 * with +/- counts and apply-all / file-by-file / cancel. */
static void ai_plan_finish(ai_async_op *op)
{
 ai_plan_data *pd = op->ud;
 FENSTER *f = op->f;
 char line[1400];
 int i, np = pd->np;

 if (op->ml.n > 0 && !strcmp(op->ml.role[op->ml.n - 1], "assistant"))
  wpe_ai_session_append("assistant", op->ml.content[op->ml.n - 1]);
 wpe_ai_session_save(f);

 if (np == 0) {
  ai_pane(f, "[plan] the model proposed no file changes", 0);
  wpe_ai_trace("plan proposals=0");
  return;
 }
 snprintf(line, sizeof line, "[plan] the AI proposes to change %d file%s (review in the dialog):",
          np, np == 1 ? "" : "s");
 ai_pane(f, line, 1);
 wpe_ai_trace("plan proposals=%d", np);
 /* Present the plan as a modal confirmation popup -- the same boxed overlay the
    Edit review uses -- so the choice is an explicit dialog that grabs focus and
    nothing paints under it, not a log line the user might miss.  Enter/A applies
    all; F drops into the per-file diff review (also a popup); Esc cancels. */
 {
  char *rt[AI_PLAN_MAX]; int ra[AI_PLAN_MAX]; int nr = 0, key, mode;
  char title[80];
  snprintf(title, sizeof title, "Apply AI plan - %d file%s", np, np == 1 ? "" : "s");
  for (i = 0; i < np && nr < AI_PLAN_MAX; i++) {
   char *now = wpe_ai_read_scope_file(f, pd->props[i].path);
   wpe_ai_seg *segs; int ns, k, plus = 0, minus = 0;
   char row[600];
   ns = wpe_ai_diff_segments(now ? now : "", pd->props[i].text, &segs);
   for (k = 0; k < ns; k++) if (segs[k].is_change) { plus += segs[k].bn; minus += segs[k].an; }
   wpe_ai_segs_free(segs, ns);
   snprintf(row, sizeof row, "  %s  (+%d -%d)%s", pd->props[i].path, plus, minus, now ? "" : "  [new file]");
   rt[nr] = strdup(row); ra[nr] = f->fb->dy.fb; nr++;      /* green: a proposed change */
   free(now);
  }
  key = ai_diff_box_show(f, rt, ra, nr, title,
        " Enter/A = Apply all    F = Review file-by-file    Q/Esc = Cancel ", "AFQ", 0);
  for (i = 0; i < nr; i++) free(rt[i]);
  if (key == 13 || key == 'A') mode = 1;
  else if (key == 'F')         mode = 2;
  else                         mode = 0;
  if (mode == 0) {
   ai_pane(f, "[plan] cancelled - nothing changed", 0);
   wpe_ai_trace("plan cancelled");
  } else {
   int applied = 0;
   for (i = 0; i < np; i++) {
    ECNT *cn = f->ed;
    FENSTER *w;
    if (e_edit(cn, pd->props[i].path) != 0) {
     snprintf(line, sizeof line, "[plan] could not open %s", pd->props[i].path);
     ai_pane(f, line, 0);
     continue;
    }
    w = cn->f[cn->mxedt];
    if (mode == 1) {
     e_ai_apply_text(w, pd->props[i].text);
     applied++;
     wpe_ai_trace("plan applied %s", pd->props[i].path);
    } else {
     char *now = ai_current_file_text(w);
     wpe_ai_seg *segs; int ns;
     ns = wpe_ai_diff_segments(now ? now : "", pd->props[i].text, &segs);
     free(now);
     {
      char *result = ai_hunk_apply(w, segs, ns, NULL, NULL);
      if (result) { e_ai_apply_text(w, result); free(result); applied++; wpe_ai_trace("plan applied %s", pd->props[i].path); }
      else wpe_ai_trace("plan skipped %s", pd->props[i].path);
     }
     wpe_ai_segs_free(segs, ns);
    }
   }
   snprintf(line, sizeof line, "[plan] applied %d of %d file%s - each is open (Ctrl-U undoes per window)", applied, np, np == 1 ? "" : "s");
   ai_pane(f, line, 0);
   wpe_ai_trace("plan done applied=%d", applied);
  }
 }
}

static int e_ai_plan(FENSTER *f)
{
 static char task[AI_PROMPT_MAX];
 char err[320], line[1400];
 ECNT *cn = f->ed;
 ai_async_op *op;
 ai_plan_data *pd;
 int save_id = -1, wi, i;
 const char *sys =
   "You are a coding agent working on a multi-file workspace inside the xwpe "
   "editor. First STUDY: reply with EXACTLY ONE read-only action per turn as a "
   "single first line:\n"
   "  TOOL read_file <path>\n  TOOL grep <pattern>\n  TOOL list_dir <path>\n"
   "When you know what to change, reply with a PLAN: for each file to change,\n"
   "  PROPOSE <path>\n  <the complete new content of that file>\n  @@END\n"
   "(repeat for every file), then a final line:  @@PLAN-DONE <one-line summary>\n"
   "Do not modify files yourself; the editor applies the plan after the user "
   "approves it. Output nothing else.";

 if (wpe_ai_busy()) {
  ai_pane(f, "[AI] a task is already running - press Esc to cancel it, or wait", 1);
  return 0;
 }
 task[0] = '\0';
 if (!e_ai_prompt1(task, "AI plan: task", f) || !task[0]) return 0;
 err[0] = '\0';
 if (wpe_ai_preflight(e_ai_backend, err, sizeof err)) { ai_pane(f, err, 1); return 0; }
 err[0] = '\0';
 if (wpe_ai_ensure_model(err, sizeof err)) { ai_pane(f, err[0] ? err : "no model set", 1); return 0; }

 e_ai_cli_mode = WPE_AI_CLI_TEXTONLY;               /* xwpe owns every edit */
 wpe_ai_session_load(f);
 pd = calloc(1, sizeof *pd);
 if (!pd) return 0;
 pd->nsc = wpe_ai_scope_files(f, e_project_is_open(), 1, &pd->scope);
 snprintf(line, sizeof line, "[plan] task: %s  (scope: %d files)", task, pd->nsc);
 ai_pane(f, line, 1);
 wpe_ai_trace("plan task=%s scope=%d", task, pd->nsc);

 for (wi = 1; wi <= cn->mxedt; wi++)
  if (cn->f[wi] == f) { save_id = cn->edt[wi]; break; }
 op = calloc(1, sizeof *op);
 if (!op) { ai_plan_free_data(pd); return 0; }
 op->f = f; op->cn = cn; op->save_id = save_id;
 op->max_iter = AI_AGENT_MAX_ITERS + 4;
 op->process = ai_plan_process;
 op->finish = ai_plan_finish;
 op->ud = pd; op->free_ud = ai_plan_free_data;

 ai_ml_add(&op->ml, "system", sys);
 ai_ml_add_open_files(&op->ml, f);
 ai_ml_add_project_memory(&op->ml);
 {
  size_t cap = 4096, len = 0;
  char *u = malloc(cap), *cur;
  if (u) {
   len += (size_t)snprintf(u, cap, "TASK: %s\n\nWORKSPACE FILES:\n", task);
   for (i = 0; i < pd->nsc; i++) {
    size_t need = len + strlen(pd->scope[i]) + 4;
    if (need > cap) { while (need > cap) cap *= 2; u = realloc(u, cap); }
    len += (size_t)snprintf(u + len, cap - len, "  %s\n", pd->scope[i]);
   }
   cur = ai_current_file_text(f);
   if (cur) {
    char *full = e_mkfilename(f->dirct, f->datnam);
    size_t need = len + strlen(cur) + strlen(full ? full : "") + 64;
    if (need > cap) { while (need > cap) cap *= 2; u = realloc(u, cap); }
    len += (size_t)snprintf(u + len, cap - len, "\nCURRENT FILE (%s):\n%s", full ? full : "", cur);
    free(full); free(cur);
   }
   ai_ml_add(&op->ml, "user", u);
   free(u);
  }
 }
 wpe_ai_session_append("user", task);
 ai_conv_start(op, "[plan] working");
 return 0;
}

#ifdef WPE_AI_AGENT_HOST
/* ================= external agent host (Claude Code native) ==============
 * Runs the real `claude` agent as a persistent stream-json session and renders
 * it in the docked pane: its assistant text and tool activity stream in, and a
 * file it edits on disk is reloaded into its open window (one Ctrl-U reverts).
 * The session stays live across turns; Alt-G h feeds the next turn, and Alt-G h
 * with an empty prompt (or Esc) ends it. */

static wpe_host *g_host;                /* the live session, or NULL */
static ECNT     *g_host_cn;             /* container, for liveness checks */
static FENSTER  *g_host_win;            /* pane's anchor window */
static int       g_host_fd = -1;        /* child stdout on the fd-loop */
static int       g_host_turn;           /* a turn is streaming */
static int       g_host_in_prompt;      /* a permission y/n modal is up */

static void e_ai_host_end(const char *why)
{
 if (g_host_fd >= 0) { wpe_fd_del(g_host_fd); g_host_fd = -1; }
 if (g_host) { wpe_host_free(g_host); g_host = NULL; }
 if (g_host_win && why) ai_pane(g_host_win, why, 1);
 g_host_win = NULL; g_host_cn = NULL; g_host_turn = 0;
}

/* Non-host code (e.g. Options > Disable) asks whether a host is up. */
int  wpe_ai_host_active(void) { return g_host != NULL; }
void wpe_ai_host_shutdown(void) { if (g_host) e_ai_host_end(NULL); }

static void host_ev_text(const char *text, void *ud)
{ ai_pane_multiline((FENSTER *)ud, text, 0); }

static void host_ev_tool(const char *tool, const char *arg, void *ud)
{
 char l[720];
 snprintf(l, sizeof l, "[agent] %s %.640s", tool, arg ? arg : "");
 ai_pane((FENSTER *)ud, l, 0);
 wpe_ai_trace("host tool=%s arg=%s", tool, arg ? arg : "");
}

static void host_ev_file(const char *path, void *ud)
{
 FENSTER *f = ud;
 char l[1200];
 wpe_ai_reload_open_window(f, path);         /* refresh the open buffer, one undo */
 snprintf(l, sizeof l, "[agent] edited %.1100s", path ? path : "");
 ai_pane(f, l, 0);
 wpe_ai_trace("host edit=%s", path ? path : "");
}

static void host_ev_notice(const char *text, void *ud)
{ ai_pane((FENSTER *)ud, text ? text : "", 0); }

static void host_ev_result(const char *summary, int is_error, void *ud)
{
 FENSTER *f = ud;
 char l[720];
 /* The assistant text already streamed via on_text; do NOT re-print the result
    field on success (it repeats the whole answer).  Surface only errors. */
 if (is_error)
  snprintf(l, sizeof l, "[agent] error: %.640s", (summary && summary[0]) ? summary : "run failed");
 else
  snprintf(l, sizeof l, "[agent] turn complete - Alt-G g continues, Esc there ends");
 ai_pane(f, l, 0);
 g_host_turn = 0;
 wpe_ai_trace("host result is_error=%d", is_error);
}

/* The agent wants to use a tool: ask y/n (reusing the agent approval dialog),
   returning 1 allow / 0 deny.  g_host_in_prompt stops the fd pump from
   re-entering while the modal reads a key. */
static int host_ev_permission(const char *tool, const char *arg, void *ud)
{
 FENSTER *f = ud;
 char what[720];
 int is_run = tool && !strcmp(tool, "Bash");
 int ok;
 snprintf(what, sizeof what, "%s %.640s", tool ? tool : "tool", arg ? arg : "");
 g_host_in_prompt = 1;
 ok = ai_agent_approve(f, what, is_run);
 g_host_in_prompt = 0;
 wpe_ai_trace("host permission tool=%s allow=%d", tool ? tool : "?", ok);
 return ok;
}

static const wpe_host_events g_host_ev = {
 host_ev_text, host_ev_tool, host_ev_file, host_ev_notice, host_ev_result,
 host_ev_permission
};

/* fd-loop callback: drain and render the session's stream-json events. */
static void host_fd_cb(int fd, void *data)
{
 int turn_done = 0, hup = 0;
 (void)fd; (void)data;
 if (!g_host || g_host_in_prompt) return;   /* not while a permission modal is up */
 if (!ai_window_alive(g_host_cn, g_host_win)) { e_ai_host_end(NULL); return; }
 wpe_host_pump(g_host, &g_host_ev, g_host_win, &turn_done, &hup);
 if (hup) e_ai_host_end("[agent] Claude Code session ended");
}

int e_ai_host(FENSTER *f)
{
 static char task[AI_PROMPT_MAX];
 char err[320];

 if (!wpe_ai_enabled()) {
  ai_pane(f, "AI assistant is off - enable it in Options > Editor.", 1);
  return 0;
 }
 if (wpe_ai_busy()) {
  ai_pane(f, "[AI] a task is already running - press Esc to cancel it, or wait", 1);
  return 0;
 }
 if (g_host && g_host_turn) {
  ai_pane(f, "[agent] still working on the previous turn", 1);
  return 0;
 }
 task[0] = '\0';
 if (!e_ai_prompt1(task, g_host ? "Claude Code: next (Esc ends)" : "Claude Code: task", f)
     || !task[0]) {
  if (g_host) {                          /* empty/Esc ends a live session */
   /* Safe to go modal here (user context, not the fd callback): let the user
      review/revert what the agent changed against the start-of-session
      checkpoint, then tear down. */
   if (wpe_ai_checkpoint_active()) wpe_ai_changeset_review(f);
   e_ai_host_end("[agent] Claude Code session ended");
  }
  return 0;
 }
 if (!g_host) {
  err[0] = '\0';
  g_host = wpe_host_start(e_ai_model, "", e_ai_policy, err, sizeof err);
  if (!g_host) {
   char l[360];
   snprintf(l, sizeof l, "[agent] could not start claude: %.320s", err[0] ? err : "?");
   ai_pane(f, l, 1);
   return 0;
  }
  g_host_win = f; g_host_cn = f->ed;
  g_host_fd = wpe_host_fd(g_host);
  wpe_fd_add(g_host_fd, POLLIN, host_fd_cb, NULL);
  /* The agent edits files on disk with its own tools; take a checkpoint so the
     whole session is reviewable/revertible when it ends (Alt-G g, then Esc). */
  { char **scope; int nsc = wpe_ai_scope_files(f, e_project_is_open(), 1, &scope);
    wpe_ai_checkpoint_create(f, scope, nsc);
    wpe_ai_free_list(scope, nsc); }
  ai_pane(f, "[agent] engine: Claude Code - session started (Esc at the prompt ends it)", 1);
  wpe_ai_trace("host start policy=%s", wpe_ai_policy_name(e_ai_policy));
 }
 { char l[1100]; snprintf(l, sizeof l, "[agent] you: %.1000s", task); ai_pane(f, l, 0); }
 if (wpe_host_send(g_host, task) != 0) {
  e_ai_host_end("[agent] send failed - session ended");
  return 0;
 }
 g_host_turn = 1;
 ai_pane(f, "[agent] working...", 0);
 wpe_ai_trace("host send");
 return 0;
}
#endif /* WPE_AI_AGENT_HOST */

/* ======================= Bottom-bar action menu ========================= */
/* The "Alt-G AI" entry on the editor's bottom bar (mouse-clickable) and any
 * unrecognised Alt-G letter open this popup so every AI action is discoverable
 * without memorising the prefix letters -- the same role e_lsp_ui_menu plays
 * for the language server. */

#define AI_MENU_TEXTW 30

/* Cycle the permission level (ask -> edits -> auto) from the menu.  The menu row
 * shows the CURRENT level ("Permissions: ask"), so this reads like a toggle. */
static int e_ai_menu_policy(FENSTER *f)
{
 e_ai_cycle_policy(f);
 return 0;
}

/* Forget the workspace conversation so the next request starts fresh.  Confirmed
 * with a status-line flash, so it never opens the pane just to report it. */
static int e_ai_menu_new_session(FENSTER *f)
{
 wpe_ai_session_reset(f);
 wpe_ai_flash(f, " AI conversation cleared for this workspace");
 return 0;
}

/* The divider row's action: do nothing (the submenu calls the selected row's
 * fkt on Enter, so it must not be NULL). */
static int e_ai_menu_divider(FENSTER *f) { (void)f; return 0; }

/* Fill `it` with the menu rows (name left, "Alt-G <key>" right-aligned so the
 * keyboard shortcut lines up like the LSP menu).  The Model and Permissions rows
 * show the CURRENT value so the active setup is visible; a blank separator
 * divides the actions from the settings.  Returns the row count. */
/* The AI action menu, as one source of truth shared by three call sites: the
   dropdown built in e_ai_menu_items, the shortcut probe ai_menu_has_key, and
   the direct dispatch ai_menu_dispatch_key.  A NULL name is a divider.  The
   Permissions row shows the live policy, so its caption is filled in at build
   time; only its key and function are constant here. */
static const struct ai_menu_row {
 const char *name; char key; int (*fkt)(FENSTER *);
} ai_menu_rows[] = {
 { "Ask (chat)",          'A', e_ai_chat             },
 { "Edit current file",   'E', e_ai_edit             },
 { "Multi-file edit",     'F', e_ai_plan             },
 { "Agent (tools)",       'G', e_ai_agent            },
 { "Build & fix (agent)", 'B', e_ai_fix_build        },
 { NULL,                  0,   NULL                  },   /* separator */
 { "Permissions",         'Y', e_ai_menu_policy      },
 { "Clear conversation",  'N', e_ai_menu_new_session },
 { "AI settings...",      'S', e_ai_options          }
};
#define AI_MENU_NROWS ((int)(sizeof ai_menu_rows / sizeof ai_menu_rows[0]))

/**
 * ai_menu_has_key - Whether a typed letter is one of the AI menu shortcuts.
 * @key: the character the user pressed (case-insensitive).
 * Return: 1 if a selectable menu row uses that letter, else 0.
 */
static int ai_menu_has_key(int key)
{
 int i, up = e_toupper(key);
 for (i = 0; i < AI_MENU_NROWS; i++)
  if (ai_menu_rows[i].fkt && ai_menu_rows[i].key &&
      e_toupper((int)ai_menu_rows[i].key) == up)
   return 1;
 return 0;
}

/**
 * ai_menu_dispatch_key - Run the AI menu action bound to a shortcut letter.
 * @f:   current window.
 * @key: a letter the user typed (case-insensitive), e.g. 'a' for Ask.
 * Return: 1 if a menu action matched and ran, 0 if no action uses that letter.
 *
 * Lets a habitual "Alt-G a" reach Ask directly -- including right after the
 * one-time consent notice, where the letter would otherwise be swallowed.
 */
static int ai_menu_dispatch_key(FENSTER *f, int key)
{
 int i, up = e_toupper(key);
 for (i = 0; i < AI_MENU_NROWS; i++)
  if (ai_menu_rows[i].fkt && ai_menu_rows[i].key &&
      e_toupper((int)ai_menu_rows[i].key) == up) {
   ai_menu_rows[i].fkt(f);
   return 1;
  }
 return 0;
}

static int e_ai_menu_items(OPTK *it)
{
 static char label[10][AI_MENU_TEXTW + 4];
 static char perm_row[48];
 int i, n = AI_MENU_NROWS;

 snprintf(perm_row, sizeof perm_row, "Permissions: %s",
          wpe_ai_policy_name(e_ai_policy));
 for (i = 0; i < n; i++) {
  const char *name = ai_menu_rows[i].name;
  char code[12];
  int pad, hl, j;
  if (!name) {                                  /* blank, non-selectable divider */
   for (j = 0; j < AI_MENU_TEXTW; j++) label[i][j] = '-';
   label[i][AI_MENU_TEXTW] = '\0';
   it[i] = WpeFillSubmenuItem(label[i], -1, 0, e_ai_menu_divider);
   continue;
  }
  if (ai_menu_rows[i].fkt == e_ai_menu_policy)  /* show the live policy caption */
   name = perm_row;
  snprintf(code, sizeof code, "Alt-G %c", ai_menu_rows[i].key);   /* 7 chars */
  pad = AI_MENU_TEXTW - (int)strlen(name) - (int)strlen(code);
  if (pad < 1)
   pad = 1;
  snprintf(label[i], sizeof label[i], "%s%*s%s", name, pad, "", code);
  hl = (int)strlen(label[i]) - 1;               /* the letter in "Alt-G X" */
  it[i] = WpeFillSubmenuItem(label[i], hl, ai_menu_rows[i].key, ai_menu_rows[i].fkt);
 }
 return n;
}

int e_ai_menu(FENSTER *f)
{
 OPTK items[12];
 int n, xa, xe, ya, ye, w;

 if (!wpe_ai_enabled())
 {
  ai_pane(f, "AI assistant is off - enable it in Options > Editor.", 1);
  return 0;
 }
 n = e_ai_menu_items(items);
 wpe_ai_trace("menu open n=%d", n);
 w = AI_MENU_TEXTW + 5;                   /* box width incl. frame + margins   */
 xa = 54;                                 /* roughly under the "Alt-G AI" entry */
 if (xa + w > MAXSCOL - 1)                /* keep it on screen                  */
  xa = MAXSCOL - 1 - w;
 if (xa < 1)
  xa = 1;
 xe = xa + w;
 ye = MAXSLNS - 2;                        /* bottom edge just above the bar...   */
 ya = ye - (n + 1);                       /* ...so the list opens upward         */
 if (ya < 1)
  ya = 1;
 /* Suppress the AI heartbeat's repaint while the menu owns the input (a task may
    be running underneath): otherwise it steals the caret and drops keystrokes. */
 { extern int wpe_modal_active; int sv = wpe_modal_active; wpe_modal_active = 1;
   WpeHandleSubmenu(xa, ya, xe, ye, 0, items, f); wpe_modal_active = sv; }
 /* A menu action (Ask, or a host follow-up) may have armed the chat input while
    the menu was still up -- ai_pane_paint suppresses painting under an open
    dropdown, so the prompt/hint would not appear until the next keystroke.  The
    menu is closed now, so paint the pane once here to surface it immediately. */
 if (g_ai_chat_focus && g_ai_chat_pane)
  ai_pane_paint(g_ai_chat_pane);
 return 0;
}

#endif /* WPE_AI */

typedef int wpe_ai_ui_translation_unit;
