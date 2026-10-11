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
 int            fell_back;     /* this chat already retried with the fallback model */
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

/**
 * ai_full_file_text - The whole current file joined with '\n', never capped.
 * @f: the file window.
 *
 * The text a reply is diffed against and the buffer is rebuilt from.  It has
 * to be the complete file, not the capped copy that goes to the model: diffing
 * against a truncated copy showed the file's tail as a deletion and accepting
 * the edit wrote a shortened file to disk.  Return: malloc'd text, or NULL.
 */
static char *ai_full_file_text(FENSTER *f)
{
 return ai_lines_text(f, 0, f->b->mxlines - 1);
}

/* ai_line_bytes - what line y costs in the context sent to the model: the
   line and its '\n'. */
static size_t ai_line_bytes(BUFFER *b, int y)
{
 int ll = b->bf[y].len;
 if (ll < 0) ll = 0;
 return (size_t)ll + 1;
}

/**
 * ai_file_fits_cap - Whether the whole file can be sent to the model.
 * @f: the file window.
 *
 * Return: 1 when the file is within AI_CTX_MAX and an Edit may send it whole,
 * 0 when only a window of it can go.
 */
static int ai_file_fits_cap(FENSTER *f)
{
 BUFFER *b = f->b;
 size_t used = 0;
 int y;
 for (y = 0; y < b->mxlines; y++) {
  used += ai_line_bytes(b, y);
  if (used > AI_CTX_MAX) return 0;
 }
 return 1;
}

/**
 * ai_context_window - Pick the lines around the cursor that fit the cap.
 * @f: the file window.
 * @y0: receives the first line of the window (0-based).
 * @y1: receives the last line of the window (inclusive).
 *
 * On a file larger than the context cap an Edit sends only a window of whole
 * lines: it starts at the cursor line and grows one line downwards, then one
 * upwards, while the next line still fits, so the user's cursor sits in the
 * middle of what the model sees and the reply is spliced over exactly these
 * lines.  A single line longer than the cap is still sent on its own; the
 * window is never empty.
 */
static void ai_context_window(FENSTER *f, int *y0, int *y1)
{
 BUFFER *b = f->b;
 int last = b->mxlines - 1;
 int cur = b->b.y;
 int lo, hi, grew;
 size_t used;
 if (cur < 0) cur = 0;
 if (cur > last) cur = last;
 lo = hi = cur;
 used = ai_line_bytes(b, cur);
 do {
  grew = 0;
  if (hi < last && used + ai_line_bytes(b, hi + 1) <= AI_CTX_MAX) {
   hi++;
   used += ai_line_bytes(b, hi);
   grew = 1;
  }
  if (lo > 0 && used + ai_line_bytes(b, lo - 1) <= AI_CTX_MAX) {
   lo--;
   used += ai_line_bytes(b, lo);
   grew = 1;
  }
 } while (grew);
 *y0 = lo;
 *y1 = hi;
}

/* What an Edit sends to the model and splices the reply over. */
enum ai_edit_scope {
 AI_SCOPE_FILE,     /* the whole file: it fits the cap, the reply is the whole file */
 AI_SCOPE_BLOCK,    /* the marked block: the reply replaces those lines            */
 AI_SCOPE_WINDOW    /* a window around the cursor: the file is over the cap        */
};

/**
 * ai_edit_scope_pick - Decide how much of the file an Edit sends.
 * @f: the file window.
 * @y0: receives the first line for BLOCK and WINDOW, -1 for FILE.
 * @y1: receives the last line for BLOCK and WINDOW, -1 for FILE.
 *
 * A marked block wins.  Otherwise a file within the cap goes whole, and a
 * larger one goes as a window around the cursor so that it is edited the way
 * a block is: the reply replaces the window and every line outside it stays
 * untouched, instead of the file being rebuilt from a truncated copy.
 */
static enum ai_edit_scope ai_edit_scope_pick(FENSTER *f, int *y0, int *y1)
{
 if (ai_block_range(f, y0, y1)) return AI_SCOPE_BLOCK;
 if (ai_file_fits_cap(f)) {
  *y0 = -1;
  *y1 = -1;
  return AI_SCOPE_FILE;
 }
 ai_context_window(f, y0, y1);
 return AI_SCOPE_WINDOW;
}

/* ai_edit_scope_header - the line that introduces the text in the prompt, so
   the model knows whether it sees the file, a marked region or a window and,
   for a window, where in the file that window sits. */
static void ai_edit_scope_header(enum ai_edit_scope scope, int y0, int y1,
                                 int nlines, char *hdr, size_t size)
{
 if (scope == AI_SCOPE_BLOCK)
  snprintf(hdr, size, "--- selected region ---");
 else if (scope == AI_SCOPE_WINDOW)
  snprintf(hdr, size, "--- lines %d-%d of %d ---", y0 + 1, y1 + 1, nlines);
 else
  snprintf(hdr, size, "--- file ---");
}

/* ai_edit_spin_label - the pane label while the model works, naming the scope
   so the user knows a selection or a window edit is in flight. */
static const char *ai_edit_spin_label(enum ai_edit_scope scope)
{
 if (scope == AI_SCOPE_BLOCK)  return "[AI edit] working (selection)";
 if (scope == AI_SCOPE_WINDOW) return "[AI edit] working (window)";
 return "[AI edit] working";
}

/**
 * ai_edit_announce_window - Tell the user which lines of a large file went out.
 * @f: the file window.
 * @y0: first line sent (0-based).
 * @y1: last line sent (inclusive).
 *
 * The pane line "[AI edit] lines 431-770 of 1200 sent" is the user's only hint
 * that the model did not see the whole file, so the instruction may need to be
 * repeated with the cursor elsewhere for the rest of it.
 */
static void ai_edit_announce_window(FENSTER *f, int y0, int y1)
{
 char line[120];
 snprintf(line, sizeof line, "[AI edit] lines %d-%d of %d sent",
          y0 + 1, y1 + 1, f->b->mxlines);
 ai_pane(f, line, 0);
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

/**
 * ai_pane_existing - Find the AI pane only if it is already open.
 * @cn: the desktop.  Return: the pane window, or NULL when none is open.
 *
 * ai_pane_win opens the pane when it is missing, which pushes a window onto the
 * desktop stack.  Callers that run while the stack is being changed underneath
 * them (a window close in progress) must only ever paint into a pane that
 * already exists, so they look it up with this and leave creation alone.
 */
static FENSTER *ai_pane_existing(ECNT *cn)
{
 int i;
 for (i = cn->mxedt; i > 0; i--)
  if (!strcmp(cn->f[i]->datnam, AI_PANE_NAME))
   return cn->f[i];
 return NULL;
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

/**
 * ai_tool_result_nonempty - Make an empty tool result explicit.
 * @result: a malloc'd tool output (or NULL).  Return: a malloc'd result.
 *
 * A blank or whitespace-only result (grep found nothing, an empty directory, a
 * command that printed nothing) reads to the model like a broken tool, so it may
 * stall or repeat.  Turn it into an explicit note so it learns "nothing matched"
 * and tries elsewhere.  Frees the input.
 */
static char *ai_tool_result_nonempty(char *result)
{
 const char *p = result;
 while (p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
 if (p && *p) return result;
 free(result);
 return strdup("(no output -- nothing matched; try a different path or pattern)");
}

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
    tr = ai_tool_result_nonempty(tr);   /* an empty result becomes an explicit note */
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
   /* Model fallback: a real failure (an error, not just an empty answer) retries
      this turn ONCE on the configured fallback model, so a primary that is down
      or too slow does not dead-end the chat. */
   if (emsg && !s->fell_back && e_ai_model_fallback && *e_ai_model_fallback &&
       (!e_ai_model || strcmp(e_ai_model, e_ai_model_fallback))) {
    char line[440];
    snprintf(line, sizeof line,
             AI_REPLY_PREFIX "primary model failed (%s) - retrying with %s",
             emsg, e_ai_model_fallback);
    if (wf) ai_pane_set_last(wf, line);
    free(emsg);
    s->fell_back = 1;
    wpe_fd_del(s->fd);
    wpe_ai_stream_free(s->st); s->st = NULL;
    ai_chat_next_turn(s);            /* re-run this turn on the fallback model */
    return;
   }
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
   "web_search and web_fetch to use the web, run_command and write_file, gated "
   "by the permission dial ask/edits/auto): Alt-G g.  Build & fix until the "
   "compile passes: Alt-G b.\n"
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
 /* After a primary-model failure this turn runs on the configured fallback
    model (req.model overrides e_ai_model for this request only). */
 req.model = s->fell_back ? e_ai_model_fallback : NULL;
 req.msgs = msgs; req.nmsgs = nm;
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
 o->crsw = AltS;   /* Enter confirms via the Send button (sw=AltS), same as Alt-S */
 out[0] = '\0';
 e_add_wrstr(3, 2, 3, 3, 62, AI_PROMPT_MAX - 1, 0, AltT, "Prompt (Enter or Alt-S=Send, Alt-M=Multi-line, Alt-C/Esc=Cancel):", out, NULL, o);
 /* Each button's underlined letter (nc) must match the key that fires it (sw)
    so the accelerator shown is the accelerator that works: Alt-S sends, Alt-M
    opens the multi-line composer, Alt-C cancels.  Enter still sends via crsw
    (AltO) and Esc still cancels via the engine, so both plain keys keep working
    alongside the Alt accelerators. */
 e_add_bttstr(18, 6, 1, AltS, " Send ", NULL, o);
 e_add_bttstr(32, 6, 0, AltM, "Multi-line", NULL, o);
 e_add_bttstr(50, 6, 0, AltC, "Cancel", NULL, o);
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
 if (ret == WPE_ESC || ret == AltC) return 0; /* cancel (Esc or the Cancel button) */
 if (ret == AltM)    return 2;                /* Multi-line: caller decides where */
 return 1;                                    /* Send (Enter/crsw=AltO, or Alt-S)  */
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
  /* Spell out the two-step exit: in a full editor Esc reads as "cancel", but
     here it FINISHES the prompt and a Send? Y/N confirm follows -- say so. */
  snprintf(ct, sizeof ct, "%.40s  [Esc = done, then Y sends / N cancels]", title);
  return e_ai_compose(out, AI_PROMPT_MAX, ct, seed, f);
 }
 return r;
}

/* Send `text` as one chat turn: echo "You: ..." above the input row, add it to
 * the workspace conversation (context preserved) and start the async stream. */
/**
 * ai_slash_expand - Expand a leading /command in the chat input to a full,
 * file-scoped instruction, so a common ask is one token.
 * @text: the user's raw input; @out/@n: expansion buffer.
 * Return: 1 and fill @out for a known slash command, else 0 (send verbatim).
 *
 * Any text after the command is kept as a specific focus ("/fix the off-by-one").
 * The transcript still echoes what the user typed; only the model sees the
 * expansion.
 */
static int ai_slash_expand(const char *text, char *out, size_t n)
{
 static const struct { const char *cmd, *tmpl; } t[] = {
  { "/explain", "Explain what the current file (or the selected code) does, concisely." },
  { "/fix",     "Find and fix the problem in the current file; use the diagnostics shown, if any." },
  { "/test",    "Write a focused unit test for the main logic of the current file." },
  { "/doc",     "Add clear doc comments to the functions in the current file." },
  { "/review",  "Review the current file for bugs, edge cases and style; list concrete issues." }
 };
 size_t i;
 if (*text != '/') return 0;
 for (i = 0; i < sizeof t / sizeof t[0]; i++) {
  size_t cl = strlen(t[i].cmd);
  const char *rest;
  if (strncmp(text, t[i].cmd, cl) || (text[cl] && text[cl] != ' ')) continue;
  rest = text + cl;
  while (*rest == ' ') rest++;
  if (*rest) snprintf(out, n, "%s  (focus: %s)", t[i].tmpl, rest);
  else       snprintf(out, n, "%s", t[i].tmpl);
  return 1;
 }
 return 0;
}

static void e_ai_chat_send(FENSTER *f, const char *text)
{
 ai_chat_session *s;
 char line[AI_PROMPT_MAX + 8];
 char expanded[AI_PROMPT_MAX];
 const char *tosend = text;

 const char *p = text;
 int first = 1;

 if (ai_slash_expand(text, expanded, sizeof expanded))
  tosend = expanded;              /* echo what was typed; send the expansion */
 wpe_ai_trace("chat prompt=%s", tosend);
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
 wpe_ai_session_append("user", tosend);
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
  ai_tr_line(wf, "[Shortcuts: /explain  /fix  /test  /doc  /review]");
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
 if (c == POS1 || c == CtrlA) { g_ai_input_pos = ai_in_bol(g_ai_input_pos); ai_input_render(wf); return 1; }
 if (c == ENDE || c == CtrlE) { g_ai_input_pos = ai_in_eol(g_ai_input_pos); ai_input_render(wf); return 1; }
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

/* ======================= Post-edit hook ================================= */

/* The exit status a hook returns when its work is done; any other status is a
   failure the user is told about, and the buffer is left as it was. */
#define AI_HOOK_OK 0

/**
 * ai_hook_configured - Whether the user set a post-edit hook (AIEditHook).
 *
 * The hook is optional: with none set every assistant change lands as it is
 * and no "[hook]" line appears in the pane.
 */
static int ai_hook_configured(void)
{
 return e_ai_edit_hook && *e_ai_edit_hook;
}

/**
 * ai_hook_command - Build the hook's command line with the path quoted.
 * @cmd/@sz: the command buffer; @path: the file the hook works on.
 *
 * The path is single-quoted for /bin/sh, so a file called "a b.c", or one
 * with a quote in its name, reaches the hook as one argument instead of
 * being split into two files the user never asked for.
 */
static void ai_hook_command(char *cmd, size_t sz, const char *path)
{
 char quoted[1200];
 wpe_ai_shell_quote(quoted, sizeof quoted, path);
 snprintf(cmd, sz, "%s %s", e_ai_edit_hook, quoted);
}

/**
 * ai_hook_announce - Tell the user which hook runs on which file.
 * @f: the window the pane belongs to; @shown: the file as the user knows it.
 */
static void ai_hook_announce(FENSTER *f, const char *shown)
{
 char line[720];
 snprintf(line, sizeof line, "[hook] %s %s", e_ai_edit_hook, shown);
 ai_pane(f, line, 0);
}

/**
 * ai_hook_report_failure - Say that the hook failed, and with what status.
 * @f: the window the pane belongs to; @shown: the file as the user knows it;
 * @status: the hook's exit status; @out: its merged output, shown when it said
 * anything.
 *
 * A formatter that refuses a file (a syntax error, a missing configuration)
 * exits non-zero: the user sees the status and the tool's own words in the
 * pane, and knows the text on screen is the assistant's, not the hook's.
 */
static void ai_hook_report_failure(FENSTER *f, const char *shown, int status,
                                   const char *out)
{
 char line[720];
 snprintf(line, sizeof line, "[hook] failed, exit status %d: %s "
          "(buffer kept as the assistant left it)", status, shown);
 ai_pane(f, line, 1);
 if (out && *out) ai_pane_multiline(f, out, 0);
}

/**
 * ai_hook_run_on_file - Run the hook on one file on disk.
 * @f: the window the pane belongs to; @path: the file handed to the hook;
 * @shown: the name the pane shows for it (the real file when the hook works
 * on a temporary copy).
 *
 * Return: the hook's exit status; AI_HOOK_OK when its result may be taken.
 */
static int ai_hook_run_on_file(FENSTER *f, const char *path, const char *shown)
{
 char cmd[2000], *out;
 int status = AI_HOOK_OK;
 ai_hook_announce(f, shown);
 ai_hook_command(cmd, sizeof cmd, path);
 out = wpe_ai_run_capture_status(cmd, &status);
 if (status != AI_HOOK_OK) ai_hook_report_failure(f, shown, status, out);
 free(out);
 wpe_ai_trace("edit hook ran on %s status %d", shown, status);
 return status;
}

/**
 * ai_run_edit_hook - Run the hook on a file the agent just wrote or patched.
 * @f: the window the pane belongs to; @path: the file, as the agent named it.
 *
 * The file is already on disk, so the hook works on it in place, and the open
 * window then takes the hook's result inside the undo step the write took: a
 * write and its reformatting are one change to the user, and one Ctrl-U puts
 * back the text from before the write.  A failed hook leaves the buffer
 * showing the agent's text.  A no-op without a hook.
 */
static void ai_run_edit_hook(FENSTER *f, const char *path)
{
 if (!ai_hook_configured()) return;
 if (ai_hook_run_on_file(f, path, path) != AI_HOOK_OK) return;
 wpe_ai_refresh_open_window(f, path);
}

/**
 * ai_hook_after_accept - Run the hook on an accepted change, leaving disk alone.
 * @f: the window holding the accepted text.
 *
 * An accepted Edit, completion or multi-file change lives in the buffer until
 * the user saves, and that holds with a hook set: the buffer's text goes to a
 * temporary copy beside the file, the hook runs on the copy, and the copy's
 * text replaces the buffer within the undo step the accept took, so one Ctrl-U
 * reverts both.  The copy is removed either way; a failed hook changes nothing.
 */
static void ai_hook_after_accept(FENSTER *f)
{
 char tmp[1400], *shown;
 if (!ai_hook_configured()) return;
 if (!wpe_ai_window_hook_copy(f, tmp, sizeof tmp)) return;
 shown = e_mkfilename(f->dirct, f->datnam);
 if (ai_hook_run_on_file(f, tmp, shown ? shown : tmp) == AI_HOOK_OK)
  wpe_ai_window_take_file(f, tmp);
 unlink(tmp);
 free(shown);
}

/* ======================= Edit mode ====================================== */

/**
 * e_ai_apply_text - Put an accepted AI change into the window as one undo step.
 * @f: the window; @newtext: the whole file as accepted.
 *
 * The same one-undo whole-buffer swap the LSP apply path uses (the shared
 * spine is in we_edit.c), so an Edit, a completion or a multi-file change
 * reverts with a single Ctrl-U exactly like a code action.  With a post-edit
 * hook set, its reformatting lands inside that same step and the file on disk
 * stays as it was until the user saves.
 */
static void e_ai_apply_text(FENSTER *f, const char *newtext)
{
 e_replace_buffer_undoable(f, newtext);
 ai_hook_after_accept(f);
}

/* Reply bytes a fence may be wrapped in: blanks and line breaks around the
   fence are the fence's, the ones inside the reply belong to the user. */
#define AI_FENCE "```"
#define AI_FENCE_LEN 3

/* An Edit reply taken apart at its outermost fence pair.  The code is the
   file; the text before the opening fence and after the closing one is the
   model's commentary, which the user reads in the pane.  All pointers point
   into the reply. */
typedef struct {
 const char *code;    size_t code_len;
 const char *before;  size_t before_len;
 const char *after;   size_t after_len;
 int fenced;
} ai_reply_parts;

/**
 * ai_fence_run_len - How many backticks open a fence on this line.
 * @line: the first byte of a line.
 *
 * A fence is a line whose first non-blank characters are three or more
 * backticks.  The run length matters because a longer run opens a fence that
 * only an equally long run closes, which is how a reply quotes a fence inside
 * a fence without ending the block early.
 *
 * Return: the run length, or 0 when the line is not a fence.
 */
static int ai_fence_run_len(const char *line)
{
 const char *p = line;
 int n = 0;
 while (*p == ' ' || *p == '\t') p++;
 while (*p == '`') { n++; p++; }
 if (n < AI_FENCE_LEN) return 0;
 return n;
}

/**
 * ai_fence_closes - Does this line close a fence opened with @run backticks?
 * @line: the first byte of a line; @run: the opening fence's run length.
 *
 * A closing fence is a run at least as long as the opening one with nothing
 * but blanks after it: "```c" opens, a bare "```" closes, and a "```" met
 * inside a "````" block is content of the block.
 *
 * Return: 1 when the line closes the fence, 0 otherwise.
 */
static int ai_fence_closes(const char *line, int run)
{
 const char *p = line;
 int n = ai_fence_run_len(line);
 if (n < run) return 0;
 while (*p == ' ' || *p == '\t') p++;
 p += n;
 while (*p == ' ' || *p == '\t' || *p == '\r') p++;
 return *p == '\n' || *p == '\0';
}

/**
 * ai_next_line - The first byte of the line after the one @p is on.
 * @p: any byte of a line.
 *
 * Return: the byte after the next newline, or the end of the text.
 */
static const char *ai_next_line(const char *p)
{
 const char *nl = strchr(p, '\n');
 if (!nl) return p + strlen(p);
 return nl + 1;
}

/**
 * ai_find_opening_fence - The first fence line of a reply.
 * @reply: the whole reply.
 *
 * Return: that line's first byte, or NULL when the reply has no fence.
 */
static const char *ai_find_opening_fence(const char *reply)
{
 const char *line = reply;
 while (*line) {
  if (ai_fence_run_len(line)) return line;
  line = ai_next_line(line);
 }
 return NULL;
}

/**
 * ai_find_closing_fence - The line that closes the fence opened at @open.
 * @open: the opening fence line.
 *
 * Return: that line's first byte, or NULL when the model never closed the
 * fence (the code then runs to the end of the reply).
 */
static const char *ai_find_closing_fence(const char *open)
{
 int run = ai_fence_run_len(open);
 const char *line = ai_next_line(open);
 while (*line) {
  if (ai_fence_closes(line, run)) return line;
  line = ai_next_line(line);
 }
 return NULL;
}

/**
 * ai_skip_blanks - The first byte of a text that is not blank.
 * @s: the text.
 *
 * Return: a pointer into @s.
 */
static const char *ai_skip_blanks(const char *s)
{
 while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') s++;
 return s;
}

/**
 * ai_reply_split - Take an Edit reply apart into the file and the commentary.
 * @reply: the whole reply; @parts: receives the pieces, pointing into @reply.
 *
 * What the user gains: a model that writes "Here is the updated file:", wraps
 * the code in ```c fences and signs off with "Let me know if you need more."
 * no longer gets that prose into the file.  Only the content of the first
 * fence pair is the file, its language tag dropped; everything around it,
 * a second fenced block included, is commentary for the pane.  A reply with
 * no fence at all is the file whole, leading blank lines dropped, as before.
 */
static void ai_reply_split(const char *reply, ai_reply_parts *parts)
{
 const char *open, *close;
 memset(parts, 0, sizeof *parts);
 open = ai_find_opening_fence(reply);
 if (!open) {
  parts->code = ai_skip_blanks(reply);
  parts->code_len = strlen(parts->code);
  return;
 }
 parts->fenced = 1;
 parts->before = reply;
 parts->before_len = (size_t)(open - reply);
 parts->code = ai_next_line(open);
 close = ai_find_closing_fence(open);
 if (!close) {
  parts->code_len = strlen(parts->code);
  return;
 }
 parts->code_len = (size_t)(close - parts->code);
 parts->after = ai_next_line(close);
 parts->after_len = strlen(parts->after);
}

/**
 * ai_copy_lf_only - Copy reply bytes, turning every CRLF line ending into LF.
 * @out: room for @len bytes; @in: the reply bytes; @len: how many.
 *
 * What the user gains: a model that answers with Windows line endings no
 * longer turns every line of the file into a change.  The buffer holds lines
 * without carriage returns (the editor strips them on load and writes them
 * back on save for a CRLF file), so a reply is diffed and spliced on the same
 * terms: the review shows only the lines that really changed, and no stray
 * carriage return reaches the buffer or the disk.  A carriage return that is
 * not followed by a line feed is content and is kept.
 *
 * Return: the number of bytes written to @out.
 */
static size_t ai_copy_lf_only(char *out, const char *in, size_t len)
{
 size_t i, n = 0;
 for (i = 0; i < len; i++) {
  if (in[i] == '\r' && i + 1 < len && in[i + 1] == '\n')
   continue;
  out[n++] = in[i];
 }
 return n;
}

/**
 * ai_file_text - The text a file reply lands as: LF endings, trimmed, one final newline.
 * @code: the first byte of the code; @len: how many bytes of it.
 *
 * A file ends with a newline however the model closed its answer, so blank
 * lines and spaces at the end are cut and exactly one newline is added; CRLF
 * line endings become LF (see ai_copy_lf_only) before anything is diffed.
 * This is the cleaner for a reply that is a FILE (Edit, Multi-file); a
 * completion is cleaned by ai_completion_text instead, which keeps the
 * reply's own leading and trailing bytes.
 *
 * Return: a fresh malloc'd copy, or NULL when out of memory.
 */
static char *ai_file_text(const char *code, size_t len)
{
 const char *end = code + len;
 char *out;
 size_t n;
 while (end > code && (end[-1] == '\n' || end[-1] == '\r' ||
                       end[-1] == ' ' || end[-1] == '\t'))
  end--;
 len = (size_t)(end - code);
 out = malloc(len + 2);
 if (!out) return NULL;
 n = ai_copy_lf_only(out, code, len);
 out[n] = '\n';
 out[n + 1] = '\0';
 return out;
}

/**
 * ai_bytes_have_ink - Is there a printable character among these bytes?
 * @s: the bytes; @len: how many.
 *
 * The blank lines a model leaves around a fence are the fence's spacing, not
 * commentary; only text with a visible character is worth a pane line.
 *
 * Return: 1 when a non-blank byte is present, 0 otherwise.
 */
static int ai_bytes_have_ink(const char *s, size_t len)
{
 size_t i;
 if (!s) return 0;
 for (i = 0; i < len; i++)
  if (s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r') return 1;
 return 0;
}

/**
 * ai_pane_bytes - Show a piece of a reply in the pane, line by line.
 * @f: the file window; @s: the bytes; @len: how many.
 *
 * Blank lines at either end are dropped so the commentary starts with its
 * first word; the lines between are shown as they were written.
 */
static void ai_pane_bytes(FENSTER *f, const char *s, size_t len)
{
 const char *start = s, *end = s + len;
 char *copy;
 while (start < end && (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r'))
  start++;
 while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r'))
  end--;
 len = (size_t)(end - start);
 copy = malloc(len + 1);
 if (!copy) return;
 memcpy(copy, start, len);
 copy[len] = '\0';
 ai_pane_multiline(f, copy, 0);
 free(copy);
}

/* The pane line that introduces what the model wrote around the code. */
#define AI_COMMENTARY_LINE "[AI edit] the model's commentary (not part of the file):"

/**
 * ai_edit_show_commentary - Put the prose around the fences in the pane.
 * @f: the file window; @parts: the reply taken apart.
 *
 * What the user gains: the sentences the model wrote around the code are
 * neither lost nor mistaken for code.  They appear under one line that says
 * what they are, so a note such as "I also removed the unused include" is
 * read, while the file receives only the code.  Nothing is shown when the
 * reply had no prose.
 */
static void ai_edit_show_commentary(FENSTER *f, const ai_reply_parts *parts)
{
 int before = ai_bytes_have_ink(parts->before, parts->before_len);
 int after = ai_bytes_have_ink(parts->after, parts->after_len);
 if (!before && !after) return;
 ai_pane(f, AI_COMMENTARY_LINE, 0);
 if (before) ai_pane_bytes(f, parts->before, parts->before_len);
 if (after) ai_pane_bytes(f, parts->after, parts->after_len);
 wpe_ai_trace("edit commentary shown (%lu bytes before, %lu after)",
              (unsigned long)parts->before_len, (unsigned long)parts->after_len);
}

/**
 * ai_edit_file_from_reply - The file an Edit reply proposes; the rest to the pane.
 * @f: the file window the edit is for; @reply: the whole reply.
 *
 * The one entry point of the Edit cleaner: the reply is split at its outermost
 * fence pair, the commentary is shown, and the code comes back as file text
 * ending in one newline.  The guard that questions a reply much shorter than
 * the file runs on this result, so a long preamble cannot make a one-line
 * refusal look like a file.
 *
 * Return: malloc'd file text, or NULL when out of memory.
 */
static int ai_file_has_fence_lines(FENSTER *f);

static char *ai_edit_file_from_reply(FENSTER *f, const char *reply)
{
 ai_reply_parts parts;
 /* A file that carries fence lines itself (Markdown, a README with code
    samples) must not be split at its own first fence: for such a file the
    reply is the file, whole, as every reply was before fences were cleaned. */
 if (ai_file_has_fence_lines(f))
  return ai_file_text(reply, strlen(reply));
 ai_reply_split(reply, &parts);
 ai_edit_show_commentary(f, &parts);
 return ai_file_text(parts.code, parts.code_len);
}

/**
 * ai_file_has_fence_lines - Does the file being edited carry fence lines of its own?
 * @f: the file window the edit is for.
 *
 * A Markdown page or a README with code samples has lines of three backticks
 * as content.  A reply that returns such a file unfenced would otherwise be cut
 * at the file's own first fence and most of it shown as commentary: the user
 * would see their document offered back as deletions.  For these files the
 * cleaner steps aside and the reply is taken whole; a wrapper of four or more
 * backticks around such a file is still recognised by the normal path.
 *
 * Return: 1 when a line of the file starts with a fence, 0 otherwise.
 */
static int ai_file_has_fence_lines(FENSTER *f)
{
 char *text = ai_full_file_text(f);
 int found;
 if (!text)
  return 0;
 found = ai_find_opening_fence(text) != NULL;
 free(text);
 return found;
}

/**
 * ai_after_opening_fence - Where a reply's text starts once a leading fence is skipped.
 * @s: the whole reply.
 *
 * A model told not to use fences sometimes does anyway ("```c" on a line of
 * its own); the user wants the code, not the fence line.  A reply with no
 * fence is returned from its first byte, leading blanks included, because at
 * a cursor in the middle of a line those blanks may be the completion.
 *
 * Return: a pointer into @s.
 */
static const char *ai_after_opening_fence(const char *s)
{
 const char *p = s;
 const char *nl;
 while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
  p++;
 if (strncmp(p, AI_FENCE, AI_FENCE_LEN))
  return s;
 nl = strchr(p, '\n');
 if (!nl)
  return p + AI_FENCE_LEN;
 return nl + 1;
}

/**
 * ai_before_closing_fence - Where a reply's text ends once a trailing fence is cut.
 * @start: the text's first byte; @end: one past its last byte.
 *
 * Cuts a closing "```" together with the blanks after it and the line break
 * that put the fence on a line of its own; a reply with no fence keeps every
 * byte, a final newline included, since the model chose to end the completion
 * with a line break.
 *
 * Return: the new end, a pointer into the text.
 */
static const char *ai_before_closing_fence(const char *start, const char *end)
{
 const char *e = end;
 while (e > start && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
  e--;
 if (e - start < AI_FENCE_LEN || strncmp(e - AI_FENCE_LEN, AI_FENCE, AI_FENCE_LEN))
  return end;
 e -= AI_FENCE_LEN;
 if (e > start && e[-1] == '\n')
  e--;
 if (e > start && e[-1] == '\r')
  e--;
 return e;
}

/**
 * ai_completion_text - The text a completion inserts, as the model returned it.
 * @reply: the whole reply.
 *
 * Only markdown fences are removed and CRLF line endings become LF (see
 * ai_copy_lf_only), so a multi-line completion from a model that answers with
 * Windows line endings does not leave a carriage return at the end of each
 * line.  No newline is added or taken away: a completion goes in at the
 * cursor, which may be in the middle of a line, so "foo(" completed with
 * "bar)" must read "foo(bar)" and not break the line.  (The Edit cleaner,
 * ai_edit_file_from_reply, normalises to one final newline because its reply
 * is a whole file, and sends the prose around the fences to the pane; a
 * completion is a fragment at the cursor, where the bytes before a fence may
 * be the completion itself, so it keeps this simpler edge-only finder.)
 *
 * Return: a fresh malloc'd copy, or NULL when out of memory.
 */
static char *ai_completion_text(const char *reply)
{
 const char *start = ai_after_opening_fence(reply);
 const char *end = ai_before_closing_fence(start, start + strlen(start));
 size_t len = (size_t)(end - start);
 size_t n;
 char *out = malloc(len + 1);
 if (!out) return NULL;
 n = ai_copy_lf_only(out, start, len);
 out[n] = '\0';
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

/* The glyph that ends a review row too wide for the box.  Plain ASCII, so it
   reads the same in a monochrome terminal as under colours: the reviewer sees
   that the row goes on past the edge and that a `-`/`+` pair which looks the
   same here differs further right. */
#define AI_CLIP_MARK '>'

/**
 * ai_box_row_text - Lay one review row out at the box's inner width.
 * @out: the buffer the row is drawn from; @n: its size;
 * @row: the row as formatted (gutter, number, text); @width: columns inside the box.
 *
 * What the user gains: a change beyond the box's edge is never shown as two
 * rows that read identical.  A row that fits is padded to the width so the
 * box background is solid; a row wider than the box is cut and its last
 * column becomes AI_CLIP_MARK, the sign that there is more to the right.
 */
static void ai_box_row_text(char *out, size_t n, const char *row, int width)
{
 int clipped;
 if (width >= (int)n) width = (int)n - 1;
 if (width < 1) { out[0] = '\0'; return; }
 clipped = (int)strlen(row) > width;
 snprintf(out, n, "%-*.*s", width, width, row);
 if (clipped) out[width - 1] = AI_CLIP_MARK;
}

/* The rows a box needs besides its body: the title row and the key-hint row. */
#define AI_BOX_FRAME_ROWS 2
/* The fewest body rows worth placing beside the hunk; with less room on either
   side the box covers the window body from its top instead, and PgDn scrolls. */
#define AI_BOX_MIN_BODY_ROWS 3

/**
 * ai_window_first_row - The first screen row of a window's text area.
 * @f: the window.
 *
 * Return: the row below the window's top border.
 */
static int ai_window_first_row(const FENSTER *f)
{
 return f->a.y + 1;
}

/**
 * ai_window_last_row - The last screen row of a window's text area.
 * @f: the window.
 *
 * Return: the row above the window's status row, which the box never covers.
 */
static int ai_window_last_row(const FENSTER *f)
{
 return f->e.y - 1;
}

/**
 * ai_box_room_below - Body rows a box can have under a line and stay in the window.
 * @f: the window; @hunk_row: the screen row of the hunk's first line.
 *
 * Return: the room, possibly negative when the line is at the bottom.
 */
static int ai_box_room_below(const FENSTER *f, int hunk_row)
{
 return ai_window_last_row(f) - hunk_row - AI_BOX_FRAME_ROWS;
}

/**
 * ai_box_room_above - Body rows a box can have over a line and stay in the window.
 * @f: the window; @hunk_row: the screen row of the hunk's first line.
 *
 * Return: the room, possibly negative when the line is at the top.
 */
static int ai_box_room_above(const FENSTER *f, int hunk_row)
{
 return hunk_row - ai_window_first_row(f) - AI_BOX_FRAME_ROWS;
}

/**
 * ai_box_place_in_window - Choose where the hunk box goes and how tall it is.
 * @f: the window being edited; @hunk_row: the screen row of the hunk's first
 * line, already scrolled into view; @nrows: the rows the box has to show;
 * @ya: receives the title row; @vis: receives the number of body rows.
 *
 * What the user gains: the box sits one row below the line it is about, so the
 * change reads in place, and it always stays inside the file window: never on
 * the status row, never over the assistant pane under it.  When the box does
 * not fit below it opens above the line; when it fits whole on neither side it
 * takes the side with more room and PgDn scrolls the rest; when neither side
 * has even AI_BOX_MIN_BODY_ROWS it covers the window body from the top, which
 * is the one case where it hides the line itself.
 */
static void ai_box_place_in_window(const FENSTER *f, int hunk_row, int nrows,
                                   int *ya, int *vis)
{
 int below = ai_box_room_below(f, hunk_row);
 int above = ai_box_room_above(f, hunk_row);
 int body = ai_window_last_row(f) - ai_window_first_row(f) + 1 - AI_BOX_FRAME_ROWS;
 if (nrows <= below) {
  *vis = nrows;
  *ya = hunk_row + 1;
  return;
 }
 if (nrows <= above) {
  *vis = nrows;
  *ya = hunk_row - nrows - AI_BOX_FRAME_ROWS;
  return;
 }
 if (below >= above && below >= AI_BOX_MIN_BODY_ROWS) {
  *vis = below;
  *ya = hunk_row + 1;
  return;
 }
 if (above >= AI_BOX_MIN_BODY_ROWS) {
  *vis = above;
  *ya = hunk_row - above - AI_BOX_FRAME_ROWS;
  return;
 }
 *vis = nrows < body ? nrows : body;
 if (*vis < 1) *vis = 1;
 *ya = ai_window_first_row(f);
}

/**
 * ai_box_place_at_top - Place a box that is about no particular line.
 * @nrows: the rows the box has to show; @ya: receives the title row;
 * @vis: receives the number of body rows.
 *
 * The agent's write preview is about a file, not a line of the window, so it
 * opens under the menu bar and may be as tall as the screen allows.
 */
static void ai_box_place_at_top(int nrows, int *ya, int *vis)
{
 *vis = nrows;
 if (*vis > MAXSLNS - 8) *vis = MAXSLNS - 8;
 if (*vis < 1) *vis = 1;
 *ya = 2;
}

/* ai_diff_box_show - Render rows[] (each with its own colour attr) in a boxed,
   scrollable overlay and return the key the reviewer pressed.  PgUp/PgDn scroll
   when the content is taller than the box, so nothing is lost off the top; a key
   in `accept` (case-insensitive), or Enter/Esc, ends it -- any other key is
   ignored.  Returns 13 for Enter, WPE_ESC for Esc, else the uppercased key.  The
   shared preview surface for the Edit review and the agent write confirmation.
   ya_pref > 0 is the screen row of the hunk's first line, which the caller has
   scrolled into view: the box then stays inside the window f (see
   ai_box_place_in_window); ya_pref <= 0 opens it under the menu bar. */
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
 xa = (MAXSCOL - boxw) / 2; if (xa < 1) xa = 1;
 xe = xa + boxw;
 if (ya_pref > 0)
  ai_box_place_in_window(f, ya_pref, nrows, &ya, &vis);
 else
  ai_box_place_at_top(nrows, &ya, &vis);
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
   ai_box_row_text(line, sizeof line, rt[row], boxw - 2);
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

/**
 * ai_hunk_deletes_more_than_it_adds - Does this hunk remove more lines than it
 * puts back?
 * @seg: the change segment about to be reviewed.
 *
 * This is the shape in which code disappears: the model dropped a block instead
 * of rewriting it.  "Apply the rest" stops on one of these, so the user sees the
 * deletion while it is still a proposal rather than discovering the lines gone.
 *
 * Return: 1 when the deletions outnumber the additions, 0 otherwise.
 */
static int ai_hunk_deletes_more_than_it_adds(const wpe_ai_seg *seg)
{
 return seg->an > seg->bn;
}

/**
 * ai_diff_hunk_hint - Write the key line shown under one hunk.
 * @out: buffer for the hint.
 * @n:   its size.
 * @seg: the change segment being shown.
 *
 * A hunk that deletes more than it adds carries its line tally in front of the
 * keys, so a reviewer who pressed "apply the rest" reads why the box came back
 * and how much text would go, before deciding again.
 */
static void ai_diff_hunk_hint(char *out, size_t n, const wpe_ai_seg *seg)
{
 if (!ai_hunk_deletes_more_than_it_adds(seg)) {
  snprintf(out, n, " y apply  n skip  a all  q cancel  PgUp/PgDn ");
  return;
 }
 snprintf(out, n, " deletes %d adds %d:  y apply  n skip  a all  q cancel  PgUp/PgDn ",
          seg->an, seg->bn);
}

/**
 * ai_review_caption - How the hunk review box is captioned.
 * @title: the box title, or NULL for the Edit review's numbered
 *         " Proposed change n/total ".
 * @hint:  the key line under the box, or NULL for the Edit review's keys
 *         (y/n/a/q, with the deletion tally when a hunk removes more than it adds).
 *
 * The Edit review numbers its hunks; a review that is by nature one hunk, the
 * completion at the cursor, names what the hunk is instead, so the user reads
 * "Proposed completion" and not "change 1/1".
 */
typedef struct {
 const char *title;
 const char *hint;
} ai_review_caption;

/**
 * ai_review_title - Write the title of the hunk being reviewed.
 * @out/@n: the buffer; @caption: the review's caption, NULL for the Edit default;
 * @hunk/@total: this hunk's place among the review's hunks.
 */
static void ai_review_title(char *out, size_t n, const ai_review_caption *caption,
                            int hunk, int total)
{
 if (caption && caption->title) {
  snprintf(out, n, "%s", caption->title);
  return;
 }
 snprintf(out, n, " Proposed change %d/%d ", hunk, total);
}

/**
 * ai_review_hint - Write the key line shown under the hunk being reviewed.
 * @out/@n: the buffer; @caption: the review's caption, NULL for the Edit default;
 * @seg: the hunk, whose line tally the Edit default may mention.
 */
static void ai_review_hint(char *out, size_t n, const ai_review_caption *caption,
                           const wpe_ai_seg *seg)
{
 if (caption && caption->hint) {
  snprintf(out, n, "%s", caption->hint);
  return;
 }
 ai_diff_hunk_hint(out, n, seg);
}

/**
 * ai_diff_context_lines - How many unchanged lines of a neighbouring segment to show.
 * @segs/@nseg: the review's segments; @j: the neighbour's index (may be out of range).
 *
 * Return: up to AI_DIFF_CTX lines when @j is a kept-lines segment, else 0, so
 * a hunk at the very start or end of the file simply has no context on that
 * side instead of reading past the segment array.
 */
static int ai_diff_context_lines(const wpe_ai_seg *segs, int nseg, int j)
{
 if (j < 0 || j >= nseg)
  return 0;
 if (segs[j].is_change)
  return 0;
 if (segs[j].an < AI_DIFF_CTX)
  return segs[j].an;
 return AI_DIFF_CTX;
}

/**
 * ai_hunk_old_start - The line, in the file as it is now, where a hunk begins.
 * @segs: the review's segments; @i: the hunk's index.
 *
 * Return: the 1-based line; one past the last line for an insertion at the end.
 */
static int ai_hunk_old_start(const wpe_ai_seg *segs, int i)
{
 int j, line = 1;
 for (j = 0; j < i; j++) line += segs[j].an;
 return line;
}

/**
 * ai_scroll_line_into_view - Bring a line of the file on screen, cursor on it.
 * @f: the window; @line: the 1-based line.
 *
 * What the user gains: the hunk about to be reviewed is visible in the file
 * before its box opens, however far from the cursor the model's change is.
 * The cursor moves to the line and keeps its column (clamped to the line's
 * length), and the window scrolls exactly as it does for Search and Goto Line:
 * the editor's one scroll-into-view path, e_cursor, so the view behaves as the
 * user already knows it.  A line past the end of the file lands on the last
 * line.  The status row is repainted so it names the line the cursor is on:
 * the review runs outside the key loop, which is what normally repaints it.
 */
static void ai_scroll_line_into_view(FENSTER *f, int line)
{
 BUFFER *b = f->b;
 int y = line - 1;
 if (y > b->mxlines - 1) y = b->mxlines - 1;
 if (y < 0) y = 0;
 b->b.y = y;
 e_cursor(f, 1);
 e_zlsplt(f);
}

/**
 * ai_line_screen_row - The screen row a line of the file is drawn on.
 * @f: the window; @line: the 1-based line, already scrolled into view.
 *
 * Return: the row inside the window's text area, clamped to it.
 */
static int ai_line_screen_row(const FENSTER *f, int line)
{
 int row = f->a.y + (line - 1) - f->s->c.y + 1;
 if (row < ai_window_first_row(f)) row = ai_window_first_row(f);
 if (row > ai_window_last_row(f)) row = ai_window_last_row(f);
 return row;
}

/**
 * ai_review_hunk_in_place - Open the hunk box next to the lines it changes.
 * @f: the window; @segs/@i: the review's segments and this hunk's index;
 * @rt/@ra/@nrows: the box rows and their colours; @title/@hint: its caption.
 *
 * What the user gains: the file scrolls to the hunk's first line first, so the
 * old text is in view, and the box opens one row below that line inside the
 * window (above when there is no room), never on the status row or over the
 * pane.  The file still shows the OLD text while the box is up, so the line is
 * counted in old-file lines.
 *
 * Return: the key the reviewer pressed, as ai_diff_box_show reports it.
 */
static int ai_review_hunk_in_place(FENSTER *f, const wpe_ai_seg *segs, int i,
                                   char **rt, int *ra, int nrows,
                                   const char *title, const char *hint)
{
 int line = ai_hunk_old_start(segs, i);
 int row;
 ai_scroll_line_into_view(f, line);
 row = ai_line_screen_row(f, line);
 return ai_diff_box_show(f, rt, ra, nrows, title, hint, "YNAQ", row);
}

/* ai_diff_review_hunk - Show one proposed hunk in the colored diff box and read
   the reviewer's decision: 'Y' apply, 'N' skip, 'A' apply the rest, 'Q'/Esc
   cancel.  Deleted lines are red, added lines green, context normal.  The
   caption names the review (NULL: the Edit review's numbered hunks). */
static int ai_diff_review_hunk(FENSTER *f, wpe_ai_seg *segs, int nseg,
                               int i, int hunk, int total,
                               const ai_review_caption *caption)
{
 int red = f->fb->db.fb, green = f->fb->dy.fb, ctx = f->fb->nt.fb;
 int pc = ai_diff_context_lines(segs, nseg, i - 1);
 int nc = ai_diff_context_lines(segs, nseg, i + 1);
 int nrows = pc + segs[i].an + segs[i].bn + nc;
 char **rt;
 int   *ra;
 int    idx = 0, k, key, ret;
 char   title[80], hint[80];

 if (nrows <= 0) return 'Y';                    /* nothing to show -> accept */
 rt = malloc((size_t)nrows * sizeof *rt);
 ra = malloc((size_t)nrows * sizeof *ra);
 if (!rt || !ra) { free(rt); free(ra); return 'Q'; }

 { int j, newstart = 1, ln;
   for (j = 0; j < i; j++) newstart += segs[j].is_change ? segs[j].bn : segs[j].an;
   ln = newstart - pc;                          /* context above the change */
   if (pc) {
    for (k = segs[i-1].an - pc; k < segs[i-1].an; k++)
     { rt[idx] = ai_diff_fmt_n(ln++, ' ', segs[i-1].a[k]); ra[idx++] = ctx; }
   }
   for (k = 0; k < segs[i].an; k++)              /* deleted: no new-file number */
    { rt[idx] = ai_diff_fmt_n(0, '-', segs[i].a[k]); ra[idx++] = red; }
   ln = newstart;
   for (k = 0; k < segs[i].bn; k++)              /* added: numbered in the new file */
    { rt[idx] = ai_diff_fmt_n(ln++, '+', segs[i].b[k]); ra[idx++] = green; }
   ln = newstart + segs[i].bn;                   /* context below the change */
   for (k = 0; nc && k < nc; k++)
    { rt[idx] = ai_diff_fmt_n(ln++, ' ', segs[i+1].a[k]); ra[idx++] = ctx; }
 }

 ai_review_title(title, sizeof title, caption, hunk, total);
 ai_review_hint(hint, sizeof hint, caption, &segs[i]);
 key = ai_review_hunk_in_place(f, segs, i, rt, ra, nrows, title, hint);
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

/**
 * ai_cursor_to_first_accepted - Park the cursor where the first accepted hunk lands.
 * @f: the window; @segs/@nseg: the review's segments; @acc: which were accepted.
 *
 * What the user gains: when the review ends, the first change they said yes to
 * is on screen with the cursor on its first line, column kept, instead of the
 * view resting wherever the last hunk was reviewed.  Every segment before that
 * hunk keeps its old lines, so its line is the same in the old and the new
 * file; the whole-buffer swap that follows keeps the cursor's row and column,
 * clamped to the new text.  Nothing moves when no hunk was accepted.
 */
static void ai_cursor_to_first_accepted(FENSTER *f, const wpe_ai_seg *segs, int nseg,
                                        const int *acc)
{
 int i, line = 1;
 for (i = 0; i < nseg; i++) {
  if (segs[i].is_change && acc[i]) break;
  line += segs[i].an;
 }
 if (i >= nseg) return;
 ai_scroll_line_into_view(f, line);
}

static char *ai_hunk_apply(FENSTER *f, wpe_ai_seg *segs, int nseg,
                           const ai_review_caption *caption,
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
  /* "Apply the rest" never removes text unseen: a hunk that deletes more than
     it adds ends the blanket accept and is shown for its own decision. */
  if (all && ai_hunk_deletes_more_than_it_adds(&segs[i]))
   all = 0;
  if (all) { acc[i] = 1; any = 1; nacc++; continue; }
  decision = ai_diff_review_hunk(f, segs, nseg, i, hunk, total, caption);
  if (decision == 'Y')      { acc[i] = 1; any = 1; nacc++; }
  else if (decision == 'A') { acc[i] = 1; any = 1; all = 1; nacc++; }
  else if (decision == 'N') { acc[i] = 0; }
  else                      { cancel = 1; }        /* 'Q' / Esc */
 }
 if (ntotal) *ntotal = total;
 if (naccepted) *naccepted = nacc;
 if (cancel || !any) { free(acc); return NULL; }
 ai_cursor_to_first_accepted(f, segs, nseg, acc);

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

/* The kinds of background task, named so the pane text, the traces and the
 * "does this task lock its window" test all agree on one spelling. */
#define AI_KIND_EDIT     "edit"
#define AI_KIND_COMPLETE "complete"
#define AI_KIND_AGENT    "agent"
#define AI_KIND_PLAN     "plan"

/* --- one background operation at a time.  A single-turn op (Edit) leaves
 * `process` NULL; a multi-turn op (Agent, Plan) supplies process()/finish() and
 * the driver keeps taking turns until process() says stop. */
typedef struct ai_async_op ai_async_op;
struct ai_async_op {
 wpe_ai_stream *st;
 int      fd;
 FENSTER *f;                 /* target window -- validated before applying     */
 ECNT    *cn;
 const char *kind;           /* "edit", "complete", "agent", "plan": names the
                                task in traces and in the pane when it is dropped */
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
 int      ready;             /* the reply is complete but a dialog was open when
                                it landed: it waits for the editor's key loop   */
 void   (*deliver)(ai_async_op *op);  /* the done path to run once it is idle    */
 int    (*process)(ai_async_op *op, char *reply);  /* 0=continue 1=done -1=abort */
 void   (*finish)(ai_async_op *op);                /* finalize (apply, save)     */
 void    *ud;               /* op-specific payload (proposals, scope, ...)      */
 void   (*free_ud)(void *ud);                      /* free ud on finish/cancel   */
};

static ai_async_op *g_ai_op = NULL;

/* True while a background op is running -- a new AI action is queued behind it
 * until it ends (or the user cancels it). */
int wpe_ai_busy(void) { return g_ai_op != NULL; }

/**
 * ai_op_splices_its_window - Does this task write back over the lines it sent?
 * @op: the running task.  Return: 1 for Edit and Complete, 0 otherwise.
 *
 * Edit and Complete rebuild the file from text captured when the request left
 * and splice the reply at the line numbers captured with it.  Agent and Plan
 * read the buffer again at the moment they write, so they keep the file
 * editable while they think.
 */
static int ai_op_splices_its_window(const ai_async_op *op)
{
 if (!op->kind)
  return 0;
 if (!strcmp(op->kind, AI_KIND_EDIT))
  return 1;
 if (!strcmp(op->kind, AI_KIND_COMPLETE))
  return 1;
 return 0;
}

/**
 * wpe_ai_window_locked - Is this window waiting for a reply of its own?
 * @f: the window the editor is about to change.  Return: 1 while it waits.
 *
 * What the user gains: the text an Edit or a Complete is about to splice back
 * is still the text that was sent, so the change lands where it was meant to
 * and the review shows the model's work only -- never a hunk that undoes what
 * was typed during the wait.  Every other window stays fully editable.
 */
int wpe_ai_window_locked(FENSTER *f)
{
 ai_async_op *op = g_ai_op;
 if (!op || !f || op->f != f)
  return 0;
 return ai_op_splices_its_window(op);
}

/**
 * wpe_ai_report_window_locked - Say on the status row why the key was refused.
 * @f: the locked window.
 *
 * What the user gains: a refused keystroke is never silent.  The bottom row
 * names the reason and the way out (Esc cancels the task and frees the
 * window); the next keystroke restores the ordinary key hints.
 */
void wpe_ai_report_window_locked(FENSTER *f)
{
 wpe_ai_flash(f, " The assistant is working on this window (Esc cancels)");
}

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

/* ---- a reply that lands behind a dialog waits for the editor ----------------
 * The fd-loop that completes a reply is pumped from inside the key read of
 * every dialog, prompt, picker and the file manager, not only from the
 * editor's own key loop.  Running the review or the apply from there stacked
 * the hunk box over the open dialog and read the next key the user typed into
 * that dialog as a review key (an "a" meant for a task description applied
 * the whole edit unseen).  So a finished reply is handed over only when the
 * editor is idle; otherwise it is held, stream closed, and the editor's key
 * loop delivers it before it waits for the next key. */

/**
 * ai_editor_idle - Is the editor in its own key loop, free to open a review box?
 * @cn: the desktop.
 *
 * True when no transient box (dialog, prompt, picker, pulldown, a review of
 * its own) owns the screen and the window on top is a text window: the file
 * manager is a window of its own type with its own key loop, so a reply
 * landing while it is up must wait as well.  The same two checks the
 * language-server painter uses to keep its output from drawing under a box.
 */
static int ai_editor_idle(ECNT *cn)
{
 extern int wpe_modal_active;
 FENSTER *top;
 if (wpe_modal_active)
  return 0;
 if (!cn || cn->mxedt < 1)
  return 0;
 top = cn->f[cn->mxedt];
 if (!top)
  return 0;
 return DTMD_ISTEXT(top->dtmd);
}

/**
 * ai_op_hold_reply - Keep a finished reply until the editor is idle again.
 * @op: the task whose reply is complete; @deliver: its done path.
 *
 * The user sees the spinner line change to "reply ready" and keeps using the
 * dialog undisturbed; the review opens by itself once the dialog closes.  The
 * stream is closed here, so the done path later finds nothing left to read,
 * and Esc-cancel or closing the target window meanwhile drops the reply the
 * usual way, since the task stays the running one until it is delivered.
 */
static void ai_op_hold_reply(ai_async_op *op, void (*deliver)(ai_async_op *op))
{
 FENSTER *wf = ai_pane_existing(op->cn);   /* never open the pane under a dialog */
 char line[120];
 const char *kind = op->kind ? op->kind : "task";
 ai_op_stream_close(op);
 op->ready = 1;
 op->deliver = deliver;
 op->paused = 1;                        /* no spinner: the wait is not the model's */
 snprintf(line, sizeof line, "[AI %s] reply ready - shown when the dialog closes", kind);
 if (wf)
  ai_pane_set_last(wf, line);
 wpe_ai_trace("%s reply held (dialog open)", kind);
}

/**
 * ai_op_deliver_held - Run the done path of a reply that was held.
 * @op: a task with a held reply, now that the editor is idle.
 *
 * From here on the task behaves exactly as if its reply had just landed: the
 * review box opens (Edit, Multi-file, Complete), the next agent turn runs.
 */
static void ai_op_deliver_held(ai_async_op *op)
{
 void (*deliver)(ai_async_op *op) = op->deliver;
 op->ready = 0;
 op->deliver = NULL;
 wpe_ai_trace("%s reply delivered", op->kind ? op->kind : "task");
 deliver(op);
}

/**
 * ai_op_reply_complete - A task's reply is complete: finish now or hold it.
 * @op: the task; @deliver: its done path (review, apply, next turn).
 *
 * Called from the stream callback the moment the backend is done.  With the
 * editor idle this is the ordinary immediate finish; with a dialog open the
 * reply is held and the key loop delivers it when the dialog closes.
 */
static void ai_op_reply_complete(ai_async_op *op, void (*deliver)(ai_async_op *op))
{
 if (ai_editor_idle(op->cn)) {
  deliver(op);
  return;
 }
 ai_op_hold_reply(op, deliver);
}

/**
 * wpe_ai_deliver_held_reply - Deliver a held reply now that the editor is idle.
 *
 * The editor's key loop calls this just before it waits for a key, which is
 * the one point that is certainly outside every dialog's own key loop.  A
 * reply held behind a dialog opens its review here, before the user's next
 * keystroke, so that keystroke can never be mistaken for a review key.
 */
void wpe_ai_deliver_held_reply(void)
{
 ai_async_op *op = g_ai_op;
 if (!op || !op->ready)
  return;
 if (!ai_editor_idle(op->cn))
  return;
 ai_op_deliver_held(op);
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
 * ai_pane_open_spinner_line - Give the progress indicator a line of its own.
 * @f: the window the pane belongs to.
 *
 * The indicator rewrites the pane's LAST line on every tick, so a notice
 * appended while a task runs is erased within a fraction of a second and the
 * user never reads it.  Opening a fresh line after the notice leaves the
 * indicator that empty line to live on and the notice stays where it was
 * written, scrolling up with the rest of the transcript.
 */
static void ai_pane_open_spinner_line(FENSTER *f)
{
 FENSTER *wf = ai_pane_win(f);
 if (wf)
  ai_pane_commit(wf);
}

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
 if (goal[0])
  snprintf(line, sizeof line,
           "[AI] queued \"%s\" - it starts when the running one ends", goal);
 else
  snprintf(line, sizeof line, "[AI] queued - it starts when the running one ends");
 ai_pane(f, line, 1);
 ai_pane_open_spinner_line(f);
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

/* Share of the file's lines a reply must keep to be taken at face value as the
   new file.  Under it the reply is a refusal ("I cannot do that"), a stray
   sentence or a generation that stopped early, so it is questioned before
   anything is reviewed instead of being diffed as if it were code. */
#define AI_REPLY_KEEP_PCT 60
/* Denominator of the share above, named so the comparison holds no bare number. */
#define AI_PCT_WHOLE 100

/**
 * ai_text_line_count - How many lines this text holds.
 * @text: the text to measure, or NULL.
 *
 * Both line counts the confirmation names come from here, so the question the
 * user reads matches what the editor compared.
 *
 * Return: the number of lines, counting a last line that has no newline.
 */
static int ai_text_line_count(const char *text)
{
 const char *p;
 int n = 0;
 if (!text) return 0;
 for (p = text; *p; p++)
  if (*p == '\n') n++;
 if (p > text && p[-1] != '\n') n++;
 return n;
}

/**
 * ai_text_is_blank - Is there nothing but whitespace in this text?
 * @text: the text to inspect, or NULL.
 *
 * Catches the replies that would leave an empty buffer: a whitespace-only answer
 * and a bare fence pair, which the fence stripper reduces to nothing.
 *
 * Return: 1 when no printable character is present, 0 otherwise.
 */
static int ai_text_is_blank(const char *text)
{
 const char *p;
 if (!text) return 1;
 for (p = text; *p; p++)
  if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') return 0;
 return 1;
}

/**
 * ai_reply_truncates_file - Would taking this reply drop much of the file?
 * @reply_lines: lines the reply would leave.
 * @file_lines:  lines the file has now.
 *
 * The one test that covers both a prose refusal (a sentence where a file was
 * asked for) and a generation that stopped halfway: either way the user would
 * be asked to review a file that is mostly gone.
 *
 * Return: 1 when the reply keeps less than AI_REPLY_KEEP_PCT of the lines.
 */
static int ai_reply_truncates_file(int reply_lines, int file_lines)
{
 return reply_lines * AI_PCT_WHOLE < file_lines * AI_REPLY_KEEP_PCT;
}

/**
 * ai_reply_would_destroy_file - Is this reply too small to be the new file?
 * @filetext: the file as it stands.
 * @reply:    the cleaned reply, spliced into place already.
 *
 * Return: 1 when the reply is blank or a truncation of the file, 0 otherwise.
 */
static int ai_reply_would_destroy_file(const char *filetext, const char *reply)
{
 if (ai_text_is_blank(reply) && !ai_text_is_blank(filetext))
  return 1;
 return ai_reply_truncates_file(ai_text_line_count(reply),
                                ai_text_line_count(filetext));
}

/**
 * ai_line_word - "line" or "lines", for a count the user reads.
 * @n: the count the word belongs to.
 *
 * Return: the singular form for one line, the plural otherwise.
 */
static const char *ai_line_word(int n)
{
 if (n == 1) return "line";
 return "lines";
}

/**
 * ai_reply_confirm_question - Write the question that names both line counts.
 * @out:         buffer for the question.
 * @n:           its size.
 * @reply_lines: lines the reply would leave.
 * @file_lines:  lines the file has now.
 *
 * The numbers are what makes the question answerable: "reply has 1 line, file
 * has 3" tells the user it is a refusal, not a rewrite, without reading a diff.
 */
static void ai_reply_confirm_question(char *out, size_t n,
                                      int reply_lines, int file_lines)
{
 snprintf(out, n, "reply has %d %s, file has %d: review anyway?",
          reply_lines, ai_line_word(reply_lines), file_lines);
}

/**
 * ai_edit_reply_trusted - Let a reply reach the review, asking first when it
 * would destroy the file.
 * @f:        the file window the edit is for.
 * @filetext: the file as it stands.
 * @reply:    the cleaned reply, spliced into place already.
 *
 * A reply that is prose, blank or half a file used to be diffed like any other,
 * and a single Enter then replaced the file with it.  Such a reply now stops at
 * the editor's own question box, which names both line counts; answering No or
 * Esc leaves buffer and disk exactly as they were.  A reply of a normal size
 * reaches the review untouched, so ordinary edits are no slower.
 *
 * Return: 1 to go on to the review, 0 when the user declined.
 */
static int ai_edit_reply_trusted(FENSTER *f, const char *filetext, const char *reply)
{
 extern int wpe_modal_active;
 char question[160];
 int modal_save, answer;

 if (!ai_reply_would_destroy_file(filetext, reply))
  return 1;
 ai_reply_confirm_question(question, sizeof question,
                           ai_text_line_count(reply),
                           ai_text_line_count(filetext));
 modal_save = wpe_modal_active;
 wpe_modal_active = 1;            /* a question is up: async paints defer */
 answer = e_message(1, question, f);
 wpe_modal_active = modal_save;
 return answer == 'Y';
}

/**
 * ai_edit_report_discarded - Say that the review ended with nothing applied.
 * @f:          the file window the edit was for.
 * @naccepted:  how many hunks had been accepted before the cancel.
 *
 * Cancelling drops the hunks already accepted as well, which is invisible unless
 * it is said: the pane names the count so the user knows what to redo rather
 * than assuming those hunks landed.
 */
static void ai_edit_report_discarded(FENSTER *f, int naccepted)
{
 char msg[80];

 if (naccepted <= 0) {
  ai_pane(f, "[AI edit] discarded", 0);
  wpe_ai_trace("edit discarded");
  return;
 }
 snprintf(msg, sizeof msg, "[AI edit] discarded (%d accepted %s dropped)",
          naccepted, naccepted == 1 ? "hunk" : "hunks");
 ai_pane(f, msg, 0);
 wpe_ai_trace("edit discarded (%d accepted dropped)", naccepted);
}

/**
 * ai_edit_show_applied - Repaint the edited window with the accepted change in view.
 * @f: the window the edit landed in.
 *
 * The apply ran from the fd loop with the focus on the pane, so the window is
 * redrawn here the way a completion's is; e_cursor keeps the first accepted
 * hunk, where the review left the cursor, inside the view, and e_zlsplt
 * repaints the status row's line and column, which only the key loop would
 * otherwise do.
 */
static void ai_edit_show_applied(FENSTER *f)
{
 e_schirm(f, 1);
 e_cursor(f, 1);
 e_zlsplt(f);
 e_refresh();
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
 clean = ai_edit_file_from_reply(f, op->acc);
 if (!clean) { ai_op_free(op); return; }

 /* A block or window edit returns only the modified region; splice it back
    into the current file at those lines so the diff below shows just that hunk
    and the proven whole-file apply path is reused. */
 if (op->sel_y0 >= 0) {
  int y0 = op->sel_y0, y1 = op->sel_y1, last = f->b->mxlines - 1;
  char *full;
  if (y0 > last) y0 = last;
  if (y1 > last) y1 = last;
  full = ai_splice_lines(f, y0, y1, clean);
  if (full) { free(clean); clean = full; }
 }

 /* Diff against the complete file, never the capped copy sent to the model:
    the lines beyond the cap must not surface as a phantom deletion hunk. */
 now = ai_full_file_text(f);
 /* A reply that would leave almost nothing of the file is questioned before the
    diff opens, so one keystroke cannot replace the file with a refusal. */
 if (!ai_edit_reply_trusted(f, now ? now : "", clean)) {
  ai_pane(f, "[AI edit] reply declined (nothing changed)", 0);
  wpe_ai_trace("edit reply declined");
  free(now);
  free(clean);
  ai_op_free(op);
  return;
 }
 nseg = wpe_ai_diff_segments(now ? now : "", clean, &segs);
 free(now);
 for (i = 0; i < nseg; i++) if (segs[i].is_change) { has_change = 1; break; }
 if (!has_change) {
  ai_pane(f, "[AI edit] no change", 0);
  wpe_ai_trace("edit no-change");
 } else {
  int nacc = 0, ntot = 0;
  char *result = ai_hunk_apply(f, segs, nseg, NULL, &nacc, &ntot);
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
   ai_edit_show_applied(f);
  } else {
   ai_edit_report_discarded(f, nacc);
  }
 }
 wpe_ai_segs_free(segs, nseg);
 free(clean);
 ai_op_free(op);
}

/**
 * ai_edit_deliver - The Edit done path: review and apply, then the queue.
 * @op: the finished Edit task.
 *
 * Run at once when the reply lands with the editor idle, or later from the
 * key loop when it landed behind a dialog; either way the queued follow-up
 * request (if any) starts only after the review has ended.
 */
static void ai_edit_deliver(ai_async_op *op)
{
 ai_edit_done(op);
 ai_queue_run();                    /* start the next queued request, if any */
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
  op->had_error = wpe_ai_stream_had_error(op->st);   /* capture before the stream goes */
  ai_op_reply_complete(op, ai_edit_deliver);
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
  ai_op_reply_complete(op, ai_conv_on_reply);
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

/**
 * ai_edit_launch - Send an Edit instruction and leave the editor live.
 * @f: the file window the change belongs to; @instr: what the user asked for.
 *
 * Split from the prompt so the same launch serves Alt-G e and a request that
 * was queued behind a running task: the user types the instruction once, and
 * it starts here either immediately or when the queue reaches it.
 */
static void ai_edit_launch(FENSTER *f, const char *instr)
{
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
 static const char sys_window[] =
   "You are a precise code editor. The file is too large to send whole, so you "
   "see only a window of consecutive lines around the cursor; the header gives "
   "their position in the file. Apply the instruction to the WINDOW below and "
   "return ONLY the modified window, keeping its indentation - no markdown "
   "fences, no commentary, and do NOT include the rest of the file.";
 const char *sys;
 char hdr[80];
 wpe_ai_msg msgs[2];
 wpe_ai_req req;
 ECNT *cn = f->ed;
 int save_id = -1, wi;
 int sy0 = -1, sy1 = -1;
 enum ai_edit_scope scope;
 ai_async_op *op;

 /* Pick the scope now, not when the instruction was typed: a queued request
    sends the block that is marked when it actually starts. */
 scope = ai_edit_scope_pick(f, &sy0, &sy1);
 /* Remember the edited window so focus returns to it after the pane work. */
 for (wi = 1; wi <= cn->mxedt; wi++)
  if (cn->f[wi] == f) { save_id = cn->edt[wi]; break; }
 err[0] = '\0';
 if (wpe_ai_preflight(e_ai_backend, err, sizeof err)) { ai_pane(f, err, 1); return; }
 err[0] = '\0';
 if (wpe_ai_ensure_model(err, sizeof err)) {
  ai_pane(f, err[0] ? err : "no model set", 1);
  return;
 }

 /* The text for the model: whole file, marked block, or the window around the
    cursor on a file over the cap.  The reply is diffed and applied against the
    full buffer in ai_edit_done, never against this prompt text. */
 if (scope == AI_SCOPE_FILE)       { cur = ai_current_file_text(f);   sys = sys_file;   }
 else if (scope == AI_SCOPE_BLOCK) { cur = ai_lines_text(f, sy0, sy1); sys = sys_sel;    }
 else                              { cur = ai_lines_text(f, sy0, sy1); sys = sys_window; }
 ai_edit_scope_header(scope, sy0, sy1, f->b->mxlines, hdr, sizeof hdr);
 if (scope == AI_SCOPE_WINDOW) ai_edit_announce_window(f, sy0, sy1);
 { char diag[1700], ow[1400]; ai_diag_block(diag, sizeof diag);
   wpe_ai_open_windows_block(f, ow, sizeof ow);
   size_t n = strlen(instr) + (cur ? strlen(cur) : 0) + strlen(diag) + strlen(ow) + sizeof hdr + 64;
   user = malloc(n);
   if (user) snprintf(user, n, "%s%s\n%s\n%s\n%s",
                      instr, diag, ow, hdr, cur ? cur : ""); }
 msgs[0].role = "system"; msgs[0].content = sys;
 msgs[1].role = "user";   msgs[1].content = user ? user : instr;
 req.model = NULL; req.msgs = msgs; req.nmsgs = 2;

 wpe_ai_trace("edit instr=%s", instr);
 err[0] = '\0';
 op = calloc(1, sizeof *op);
 if (!op) { free(user); free(cur); return; }
 op->st = wpe_ai_stream_start(&req, err, sizeof err);
 free(user);
 free(cur);
 if (!op->st) {
  snprintf(line, sizeof line, "[AI edit] %s", err[0] ? err : "could not start");
  ai_pane(f, line, 1);
  free(op);
  return;
 }
 /* Register the stream on the shared fd-loop and RETURN -- the editor stays
    interactive; ai_edit_fd_cb finishes (diff + apply) when the reply lands. */
 op->f = f; op->cn = cn; op->save_id = save_id;
 op->kind = AI_KIND_EDIT;
 op->sel_y0 = sy0;                  /* -1 for a whole-file edit, else the lines sent */
 op->sel_y1 = sy1;
 op->fd = wpe_ai_stream_fd(op->st);
 op->start = time(NULL);
 op->spin = ai_spin_begin(f, ai_edit_spin_label(scope));
 g_ai_op = op;
 g_ai_bg_win = f;
 ai_hb_acquire(ai_op_tick);
 wpe_fd_add(op->fd, POLLIN, ai_edit_fd_cb, op);
 /* Hand focus back to the file so the user watches the change land there and
    can cancel with Esc; the pane spins in the background (its paints leave the
    caret here).  Its text is held until the reply is spliced in. */
 if (save_id >= 0)
  e_switch_window(save_id, cn->f[cn->mxedt]);
 wpe_ai_report_window_locked(f);
 wpe_ai_trace("edit stream fd=%d (async) sel=%d..%d", op->fd, op->sel_y0, op->sel_y1);
}

/**
 * e_ai_edit - Change the current file by instruction (Alt-G e).
 * @f: the file window.  Return: 0, the editor-action convention.
 *
 * Asks for the instruction first and only then looks at what is running, so a
 * second request can be typed while the first is still generating: it is
 * queued and starts by itself when that one ends.
 */
static int e_ai_edit(FENSTER *f)
{
 static char instr[AI_PROMPT_MAX];
 int sy0 = -1, sy1 = -1;
 int have_sel = (ai_edit_scope_pick(f, &sy0, &sy1) == AI_SCOPE_BLOCK);

 instr[0] = '\0';
 if (!e_ai_prompt1(instr, have_sel ? "AI edit instruction (selection)"
                                   : "AI edit instruction", f) || !instr[0])
  return 0;
 if (wpe_ai_busy()) {
  ai_enqueue(f, ai_edit_launch, instr);
  return 0;
 }
 ai_edit_launch(f, instr);
 return 0;
}

/* ================= inline completion at the cursor (Alt-G c) ============= */

/* Payload for a completion op: the file text on each side of the cursor, kept so
   the reply can be spliced back in exactly where it was requested. */
typedef struct { char *before, *after; } ai_complete_ud;

static void ai_complete_ud_free(void *ud)
{
 ai_complete_ud *c = ud;
 if (!c) return;
 free(c->before); free(c->after); free(c);
}

/* The completion review is one hunk by nature: the box says what it is instead
   of numbering it, and lists the keys that make sense for a single hunk (the
   same keys the Edit review reads: y/Enter take it, n, q and Esc leave it). */
static const ai_review_caption ai_complete_caption = {
 " Proposed completion ",
 " y insert  n / q / Esc discard  PgUp/PgDn "
};

/* A completion review has at most three segments: the lines above the cursor
   line, the cursor line itself (the hunk), the lines below it. */
#define AI_COMPLETE_SEGS_MAX 3

/**
 * ai_copy_span - A malloc'd, NUL-terminated copy of the first @len bytes of @s.
 */
static char *ai_copy_span(const char *s, size_t len)
{
 char *out = malloc(len + 1);
 if (!out) return NULL;
 memcpy(out, s, len);
 out[len] = '\0';
 return out;
}

/**
 * ai_cursor_line_start - Where the cursor line begins inside the text before the cursor.
 * @before: the file up to the cursor.  Return: the line's first byte.
 */
static const char *ai_cursor_line_start(const char *before)
{
 const char *nl = strrchr(before, '\n');
 if (!nl) return before;
 return nl + 1;
}

/**
 * ai_cursor_line_end - Where the cursor line ends inside the text after the cursor.
 * @after: the file from the cursor on.  Return: its '\n', or the end of the text.
 */
static const char *ai_cursor_line_end(const char *after)
{
 const char *nl = strchr(after, '\n');
 if (!nl) return after + strlen(after);
 return nl;
}

/**
 * ai_lines_below_cursor - The text that follows the cursor line.
 * @line_end: where the cursor line ends (ai_cursor_line_end).
 * Return: the first byte after the line break, or "" when the cursor line is
 * the last.
 */
static const char *ai_lines_below_cursor(const char *line_end)
{
 if (*line_end == '\n') return line_end + 1;
 return line_end;
}

/**
 * ai_cursor_line_with - The cursor line with @insert placed at the cursor.
 * @prefix/@plen: the line up to the cursor; @insert: the text at the cursor,
 * "" for the line as it is; @suffix/@slen: the line from the cursor on.
 *
 * Newline-terminated, so a suggestion with line breaks of its own splits into
 * exactly the lines the file will have once it is in.  Return: malloc'd text,
 * or NULL.
 */
static char *ai_cursor_line_with(const char *prefix, size_t plen, const char *insert,
                                 const char *suffix, size_t slen)
{
 size_t ilen = strlen(insert);
 char *out = malloc(plen + ilen + slen + 2);
 if (!out) return NULL;
 memcpy(out, prefix, plen);
 memcpy(out + plen, insert, ilen);
 memcpy(out + plen + ilen, suffix, slen);
 out[plen + ilen + slen] = '\n';
 out[plen + ilen + slen + 1] = '\0';
 return out;
}

/**
 * ai_segs_add_context - Append the lines of @text as a kept-lines segment.
 * @segs/@n: the segment array and its count.
 *
 * Nothing is appended for empty text, so a completion on the first or last
 * line has no empty context segment above or below it.
 */
static void ai_segs_add_context(wpe_ai_seg *segs, int *n, const char *text)
{
 wpe_ai_seg *seg = &segs[*n];
 memset(seg, 0, sizeof *seg);
 seg->is_change = 0;
 seg->an = wpe_ai_split_lines(text, &seg->a);
 if (seg->an > 0) {
  (*n)++;
  return;
 }
 wpe_ai_free_lines(seg->a, 0);
 seg->a = NULL;
}

/**
 * ai_segs_add_change - Append the hunk in which @old_text becomes @new_text.
 * @segs/@n: the segment array and its count.
 */
static void ai_segs_add_change(wpe_ai_seg *segs, int *n, const char *old_text,
                               const char *new_text)
{
 wpe_ai_seg *seg = &segs[*n];
 memset(seg, 0, sizeof *seg);
 seg->is_change = 1;
 seg->an = wpe_ai_split_lines(old_text, &seg->a);
 seg->bn = wpe_ai_split_lines(new_text, &seg->b);
 (*n)++;
}

/**
 * ai_complete_segments - Lay a completion out as one reviewable hunk.
 * @c:     the file before and after the cursor, captured when the request left.
 * @clean: the suggestion, fences removed.
 * @out:   receives the segments (free with wpe_ai_segs_free).
 *
 * The hunk is the cursor line as it is against the lines it becomes with the
 * suggestion spliced in at the cursor; everything above and below is kept as
 * context.  The review box then shows the completion in place, with the line
 * numbers of the real file, and the accept path rebuilds the file from the very
 * segments the user looked at: no second splice, so what was shown is what
 * lands.  Return: the segment count, 0 when out of memory.
 */
static int ai_complete_segments(const ai_complete_ud *c, const char *clean,
                                wpe_ai_seg **out)
{
 const char *prefix = ai_cursor_line_start(c->before);
 const char *line_end = ai_cursor_line_end(c->after);
 const char *below = ai_lines_below_cursor(line_end);
 size_t above_len = (size_t)(prefix - c->before);
 size_t slen = (size_t)(line_end - c->after);
 char *above = ai_copy_span(c->before, above_len);
 char *old_line = ai_cursor_line_with(prefix, strlen(prefix), "", c->after, slen);
 char *new_lines = ai_cursor_line_with(prefix, strlen(prefix), clean, c->after, slen);
 wpe_ai_seg *segs = calloc(AI_COMPLETE_SEGS_MAX, sizeof *segs);
 int n = 0;

 *out = NULL;
 if (above && old_line && new_lines && segs) {
  ai_segs_add_context(segs, &n, above);
  ai_segs_add_change(segs, &n, old_line, new_lines);
  ai_segs_add_context(segs, &n, below);
  *out = segs;
 } else {
  free(segs);
 }
 free(above);
 free(old_line);
 free(new_lines);
 return n;
}

/**
 * ai_complete_report_discarded - Say that the completion was left out.
 * @f: the window the completion was for.
 *
 * The user pressed n, q or Esc on the box; the pane records it so the quiet
 * screen is not mistaken for a reply that never came.
 */
static void ai_complete_report_discarded(FENSTER *f)
{
 ai_pane(f, "[AI complete] discarded", 0);
 wpe_ai_trace("complete discarded");
}

/**
 * ai_complete_insert - Put the accepted completion into the file.
 * @f: the window; @op: the finished task; @newtext: the file with the
 * completion in, as rebuilt from the reviewed hunk.
 *
 * Goes through the proven whole-file path, so the insertion is one Ctrl-U undo
 * step and never a partial write, and the pane says so.
 */
static void ai_complete_insert(FENSTER *f, ai_async_op *op, const char *newtext)
{
 e_ai_apply_text(f, newtext);
 ai_pane(f, "[AI] completed at cursor - Ctrl-U to undo", 0);
 wpe_ai_trace("complete applied");
 if (op->save_id >= 0) e_switch_window(op->save_id, f);
 e_schirm(f, 1);          /* repaint the file window: the apply ran from the */
 e_cursor(f, 1);          /* fd-loop with focus on the pane, so force a redraw */
 e_refresh();
}

/**
 * ai_complete_review - Show the completion in the review box and act on the answer.
 * @f: the window; @op: the finished task; @segs/@nseg: the completion as one hunk.
 *
 * The user sees the suggestion at its place, in the same box, colours and keys
 * as an Edit hunk, and the file changes only on y or Enter; n, q and Esc leave
 * it as it was.
 */
static void ai_complete_review(FENSTER *f, ai_async_op *op, wpe_ai_seg *segs, int nseg)
{
 char *result = ai_hunk_apply(f, segs, nseg, &ai_complete_caption, NULL, NULL);
 if (!result) {
  ai_complete_report_discarded(f);
  return;
 }
 ai_complete_insert(f, op, result);
 free(result);
}

/**
 * ai_complete_done - A completion has arrived: review it, then insert or drop it.
 * @op: the finished Complete task.
 *
 * Cleans the reply of fences only, lays it out as the one hunk the cursor line
 * becomes, and opens the review box.  A backend error or an empty suggestion
 * changes nothing and says so.
 */
static void ai_complete_done(ai_async_op *op)
{
 FENSTER *f = op->f;
 ai_complete_ud *c = op->ud;
 char *clean;
 wpe_ai_seg *segs = NULL;
 int nseg;

 ai_op_detach(op);
 if (!ai_window_alive(op->cn, f)) { ai_op_free(op); return; }
 if (op->had_error) { ai_pane(f, "[AI complete] backend error", 1); ai_op_free(op); return; }
 if (op->acc_len == 0) { ai_pane(f, "[AI complete] no suggestion", 0); ai_op_free(op); return; }
 clean = ai_completion_text(op->acc);
 if (!clean || !c) { free(clean); ai_op_free(op); return; }
 if (!*clean) {
  ai_pane(f, "[AI complete] no suggestion", 0);
  free(clean);
  ai_op_free(op);
  return;
 }
 nseg = ai_complete_segments(c, clean, &segs);
 if (nseg > 0)
  ai_complete_review(f, op, segs, nseg);
 wpe_ai_segs_free(segs, nseg);
 free(clean);
 ai_op_free(op);
}

/**
 * ai_complete_deliver - The Complete done path: review and insert, then the queue.
 * @op: the finished Complete task.
 *
 * Run at once when the reply lands with the editor idle, or later from the
 * key loop when it landed behind a dialog, so the review box never opens over
 * a dialog and a key typed into the dialog is never read as its answer.
 */
static void ai_complete_deliver(ai_async_op *op)
{
 ai_complete_done(op);
 ai_queue_run();
}

static void ai_complete_fd_cb(int fd, void *data)
{
 ai_async_op *op = data;
 int done = 0;
 (void)fd;
 if (wpe_ai_stream_pump(op->st, ai_op_collect, op, &done) < 0) {
  FENSTER *f = op->f; ECNT *cn = op->cn;
  ai_op_detach(op);
  if (ai_window_alive(cn, f)) ai_pane(f, "[AI complete] transport error", 0);
  ai_op_free(op); ai_queue_run(); return;
 }
 if (done) {
  op->had_error = wpe_ai_stream_had_error(op->st);
  ai_op_reply_complete(op, ai_complete_deliver);
 }
}

/**
 * e_ai_complete - Fill in code at the cursor (Alt-G c).
 * Sends the code around the cursor to the model; when the reply arrives the
 * suggestion is shown at its place in the review box, and y or Enter inserts
 * it exactly at the cursor as one undo step (Ctrl-U reverts) -- a local,
 * low-ceremony alternative to chat/edit for "finish this line/block".  The
 * editor stays interactive while the model works.
 */
static void ai_launch_complete(FENSTER *f, const char *goal);

static int e_ai_complete(FENSTER *f)
{
 char err[320], line[360];
 char *text, *before, *after, *user = NULL;
 const char *pbz;
 ai_complete_ud *ud;
 ai_async_op *op;
 BUFFER *b = f->b;
 ECNT *cn = f->ed;
 int save_id = -1, wi, y, i, x, ll;
 size_t off = 0, bl;
 static const char sys[] =
   "You are a code completion engine. Complete the code at the <CURSOR> marker. "
   "Return ONLY the text to insert at the cursor -- no markdown fences, no "
   "commentary, and do not repeat the surrounding code.";
 wpe_ai_msg msgs[2];
 wpe_ai_req req;

 if (!DTMD_ISTEXT(f->dtmd)) { ai_pane(f, "[AI] no file to complete here", 1); return 0; }
 if (wpe_ai_busy()) { ai_enqueue(f, ai_launch_complete, ""); return 0; }
 err[0] = '\0';
 if (wpe_ai_preflight(e_ai_backend, err, sizeof err)) { ai_pane(f, err, 1); return 0; }
 err[0] = '\0';
 if (wpe_ai_ensure_model(err, sizeof err)) { ai_pane(f, err[0] ? err : "no model set", 1); return 0; }

 /* Split the COMPLETE file at the cursor: the reply is spliced back as
    before + suggestion + after, so a capped copy here would drop the tail.
    Only the prompt below is trimmed to the context around the cursor. */
 text = ai_full_file_text(f);
 if (!text) return 0;
 y = b->b.y; if (y < 0) y = 0; if (y > b->mxlines - 1) y = b->mxlines - 1;
 for (i = 0; i < y && i < b->mxlines; i++)
  off += (size_t)(b->bf[i].len < 0 ? 0 : b->bf[i].len) + 1;
 x = b->b.x; ll = b->bf[y].len < 0 ? 0 : b->bf[y].len;
 if (x > ll) x = ll; if (x < 0) x = 0;
 off += (size_t)x;
 if (off > strlen(text)) off = strlen(text);
 before = malloc(off + 1);
 after = strdup(text + off);
 if (!before || !after) { free(before); free(after); free(text); return 0; }
 memcpy(before, text, off); before[off] = '\0';
 free(text);

 bl = strlen(before);
 pbz = bl > 2000 ? before + (bl - 2000) : before;      /* last ~2000 chars of context */
 { size_t n = strlen(pbz) + 900;
   user = malloc(n);
   if (user) snprintf(user, n, "%s<CURSOR>%.800s", pbz, after); }
 if (!user) { free(before); free(after); return 0; }

 msgs[0].role = "system"; msgs[0].content = sys;
 msgs[1].role = "user";   msgs[1].content = user;
 req.model = NULL; req.msgs = msgs; req.nmsgs = 2;

 for (wi = 1; wi <= cn->mxedt; wi++) if (cn->f[wi] == f) { save_id = cn->edt[wi]; break; }

 op = calloc(1, sizeof *op);
 ud = calloc(1, sizeof *ud);
 if (!op || !ud) { free(op); free(ud); free(before); free(after); free(user); return 0; }
 ud->before = before; ud->after = after;
 op->st = wpe_ai_stream_start(&req, err, sizeof err);
 free(user);
 if (!op->st) {
  snprintf(line, sizeof line, "[AI complete] %s", err[0] ? err : "could not start");
  ai_pane(f, line, 1);
  ai_complete_ud_free(ud); free(op);
  return 0;
 }
 op->f = f; op->cn = cn; op->save_id = save_id;
 op->kind = AI_KIND_COMPLETE;
 op->sel_y0 = op->sel_y1 = -1;
 op->ud = ud; op->free_ud = ai_complete_ud_free;
 op->fd = wpe_ai_stream_fd(op->st);
 op->start = time(NULL);
 op->spin = ai_spin_begin(f, "[AI] completing");
 g_ai_op = op;
 g_ai_bg_win = f;
 ai_hb_acquire(ai_op_tick);
 wpe_fd_add(op->fd, POLLIN, ai_complete_fd_cb, op);
 if (save_id >= 0) e_switch_window(save_id, cn->f[cn->mxedt]);
 wpe_ai_report_window_locked(f);
 wpe_ai_trace("complete stream fd=%d off=%zu", op->fd, off);
 return 0;
}

/**
 * ai_launch_complete - Queue hook: run the completion now that nothing else is.
 * @f: the window the user asked to complete in; @goal: unused, Complete takes
 * no instruction.
 *
 * What the user gains: Alt-G c pressed during another task is not lost; the
 * suggestion arrives at the cursor as soon as the running task is done.
 */
static void ai_launch_complete(FENSTER *f, const char *goal)
{
 (void)goal;
 e_ai_complete(f);
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

/* Apply a CA file path typed in the settings dialog's CA file field (trimmed);
 * a blank field means the system CA store only.  Shared by the Ok handler and
 * the provider operations so a self-signed bridge works before Ok. */
static void ai_set_cafile(const char *text)
{
 const char *ca = text ? text : "";
 while (*ca == ' ' || *ca == '\t') ca++;
 free(e_ai_cafile);
 e_ai_cafile = (*ca) ? strdup(ca) : NULL;
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

/**
 * ai_opt_commit_connection_fields - Make the Connection fields the session's values.
 * @o: the open AI settings dialog (NULL: nothing to commit)
 *
 * What the user sees in Endpoint, CA file and API key is what every provider
 * operation works with: the Model listing (Alt-M), a provider switch or
 * "Save current as..." (Alt-V), and Ok all commit the fields first, so a URL
 * or a key typed but not yet confirmed is never silently replaced by an older
 * value.  Returns 1 when the key field carried a key (so the caller can
 * persist it to the right key file), 0 otherwise.
 */
static int ai_opt_commit_connection_fields(W_OPTSTR *o)
{
 int key_typed = 0;
 if (!o) return 0;
 if (o->wn > AI_OPT_ENDPOINT_WSTR)
  ai_set_endpoint(o->wstr[AI_OPT_ENDPOINT_WSTR]->txt);
 if (o->wn > AI_OPT_CAFILE_WSTR)
  ai_set_cafile(o->wstr[AI_OPT_CAFILE_WSTR]->txt);
 if (o->wn > AI_OPT_KEY_WSTR)
  key_typed = ai_set_typed_key(o->wstr[AI_OPT_KEY_WSTR]->txt);
 return key_typed;
}

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
 /* List from the URL, CA file and key the user just typed, not the ones a
    previous Ok saved -- so Alt-M works before Ok, also against an authenticated
    endpoint (Groq, a Lumo bridge, ...) or a self-signed HTTPS bridge. */
 ai_opt_commit_connection_fields(g_ai_opt_dlg);
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

/**
 * ai_opt_switch_provider - Make a saved profile the active one.
 * @p: the picked profile
 * @key_typed: the key field carried a key for the profile being left
 *
 * A key typed for the current profile is written to that profile's key file
 * BEFORE the field reloads with the picked profile's own key, so switching
 * never silently drops a key entered but not yet confirmed with Ok.  The
 * backend becomes OpenAI-compatible and the profile's endpoint, model, CA file
 * and name are applied, so its model list and its key file are the ones used.
 */
static void ai_opt_switch_provider(const struct wpe_ai_provider *p, int key_typed)
{
 if (key_typed)
  wpe_ai_write_openai_key(e_ai_provider, e_ai_key);
 e_ai_backend = WPE_AI_OPENAI;
 free(e_ai_endpoint); e_ai_endpoint = strdup(p->endpoint);
 free(e_ai_model);    e_ai_model    = strdup((p->model && *p->model) ? p->model : "");
 free(e_ai_cafile);   e_ai_cafile   = (p->cafile && *p->cafile) ? strdup(p->cafile) : NULL;
 free(e_ai_provider); e_ai_provider = strdup(p->name);
}

/**
 * ai_strip_blanks - Drop the blanks around a typed name, in place.
 */
static void ai_strip_blanks(char *s)
{
 char *first = s;
 size_t n;
 while (*first == ' ' || *first == '\t') first++;
 memmove(s, first, strlen(first) + 1);
 n = strlen(s);
 while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = '\0';
}

/**
 * ai_opt_refuse_provider_name - Say why a typed provider name cannot be saved.
 *
 * Shown instead of silently dropping the name, so the user knows what to
 * retype: the length limit, and that '|' (the field separator of the
 * AIProvider line in xwperc) and '/' (a path separator in the key file's name)
 * cannot be part of a name.
 */
static void ai_opt_refuse_provider_name(FENSTER *f)
{
 static char msg[96];
 snprintf(msg, sizeof msg, "Provider name: up to %d characters, without '|' or '/'",
          WPE_AI_PROVIDER_NAME_MAX);
 e_error(msg, 0, f->fb);
}

/**
 * ai_opt_save_provider_as - Store the Connection fields as a new named profile.
 *
 * Asks for a name and saves the endpoint, model and CA file the dialog shows
 * under it, making it the active profile; the session key (typed in the field
 * or set by Alt-M) goes to that name's own key file, so the profile is
 * complete on the next launch and the key field keeps showing it.  A name
 * that cannot be stored is refused with a message and nothing changes.
 */
static void ai_opt_save_provider_as(FENSTER *f)
{
 char name[E_ARG_FIELD_MAX + 1] = "";
 /* field sw must differ from the dialog's confirm key (AltO) or Ok is shadowed */
 if (!e_add_arguments(name, "Save provider as", f, 0, AltN, NULL))
  return;
 ai_strip_blanks(name);
 if (!name[0])
  return;
 if (!wpe_ai_provider_name_usable(name)) {
  ai_opt_refuse_provider_name(f);
  return;
 }
 wpe_ai_provider_set(name, e_ai_endpoint ? e_ai_endpoint : "", e_ai_model, e_ai_cafile);
 free(e_ai_provider); e_ai_provider = strdup(name);
 if (e_ai_key && *e_ai_key)
  wpe_ai_write_openai_key(name, e_ai_key);
}

/* Provider button (Alt-V): pick a saved OpenAI-compatible provider profile, or
 * save the current endpoint/model/CA as a new named one.  Either operation
 * works with what the Connection fields show (committed first), so a typed
 * URL or key is part of it.  Returns -1 to repaint the dialog in place. */
static int e_ai_opt_pick_provider(FENSTER *f)
{
 int np = wpe_ai_provider_count();
 const char *labels[34];
 char names[32][WPE_AI_PROVIDER_NAME_MAX + 1];
 int i, n = 0, sel, key_typed;
 for (i = 0; i < np && n < 32; i++) {
  snprintf(names[n], sizeof names[n], "%s", wpe_ai_provider_get(i)->name);
  labels[n] = names[n];
  n++;
 }
 labels[n++] = "[ + Save current as... ]";
 sel = e_ai_pick(f, "AI providers", labels, n, 0);
 if (sel < 0) { fk_cursor(0); return -1; }
 key_typed = ai_opt_commit_connection_fields(g_ai_opt_dlg);
 if (sel < np)
  ai_opt_switch_provider(wpe_ai_provider_get(sel), key_typed);
 else
  ai_opt_save_provider_as(f);
 ai_opt_sync_widgets();
 fk_cursor(0);
 return -1;
}

/* Advanced AI settings (the "Advanced..." button in the AI dialog): the less
 * common knobs -- the fallback model, the post-edit hook, and (host builds) the
 * agent-host command -- kept in their own dialog so the main one stays within a
 * 24-line terminal.  Each is also a plain xwperc key; this is discoverability.
 * Returns -1 whichever way it closes: the sub-dialog's own close repaints the
 * desktop under the main dialog, so the engine must paint the main box again
 * in place (its pending edits intact), as it does after a picker. */
static int e_ai_opt_advanced(FENSTER *f)
{
 W_OPTSTR *o = e_init_opt_kst(f);
 /* Wide enough for the longest help line below (67 cells at column 3 --
    the engine draws a line only when it fits inside the frame) and for the
    23-cell field labels to end before their fields start.  Still inside an
    80-column terminal. */
 int ret, w = 72, h = 12;
 const char *v;
 if (!o) return 0;
 o->xa = (MAXSCOL - w) / 2; if (o->xa < 1) o->xa = 1; o->xe = o->xa + w;
 o->ya = (MAXSLNS - h) / 2; if (o->ya < 1) o->ya = 1; o->ye = o->ya + h;
 o->bgsw = 0; o->crsw = AltO;
 o->name = "Advanced AI settings";
 e_add_wrstr(3, 2, 27, 2, 38, 255, -1, AltM, "Model fallback (Alt-M):",
             e_ai_model_fallback ? e_ai_model_fallback : "", NULL, o);   /* wstr[0] */
 e_add_wrstr(3, 4, 27, 4, 38, 511, -1, AltH, "Edit hook (Alt-H):",
             e_ai_edit_hook ? e_ai_edit_hook : "", NULL, o);             /* wstr[1] */
#ifdef WPE_AI_AGENT_HOST
 e_add_wrstr(3, 6, 27, 6, 38, 511, -1, AltD, "Host command (Alt-D):",
             e_ai_host_command ? e_ai_host_command : "", NULL, o);       /* wstr[2] */
#endif
 e_add_txtstr(3, 8, "Fallback: a model retried once when the primary fails a turn.", o);
 e_add_txtstr(3, 9, "Edit hook: a command run on a file after an AI edit (clang-format).", o);
 e_add_bttstr(24, 11, 1, AltO, "  Ok  ", NULL, o);
 e_add_bttstr(40, 11, -1, WPE_ESC, "Cancel", NULL, o);
 ret = e_opt_kst(o);
 if (ret == WPE_ESC) { freeostr(o); return -1; }
 v = o->wstr[0]->txt; while (v && (*v == ' ' || *v == '\t')) v++;
 free(e_ai_model_fallback); e_ai_model_fallback = (v && *v) ? strdup(v) : NULL;
 v = o->wstr[1]->txt; while (v && (*v == ' ' || *v == '\t')) v++;
 free(e_ai_edit_hook); e_ai_edit_hook = (v && *v) ? strdup(v) : NULL;
#ifdef WPE_AI_AGENT_HOST
 v = o->wstr[2]->txt; while (v && (*v == ' ' || *v == '\t')) v++;
 free(e_ai_host_command); e_ai_host_command = (v && *v) ? strdup(v) : NULL;
#endif
 e_save_opt(f);
 freeostr(o);
 return -1;
}

/* The session values the AI dialog opened with.  Compared with the values Ok
 * applied, they tell which keys the USER changed: only those replace an
 * XWPE_AI_* override in xwperc (wpe_ai_cfg_user_changed); the others keep the
 * file's own value, so a one-session override never becomes configuration. */
struct ai_opt_opened {
 int   backend, policy, agent_engine;
 char *endpoint, *model, *cafile;
};

/**
 * ai_opt_opened_take - Remember the values shown when the dialog opens.
 */
static void ai_opt_opened_take(struct ai_opt_opened *was)
{
 was->backend      = e_ai_backend;
 was->policy       = e_ai_policy;
 was->agent_engine = e_ai_agent_engine;
 was->endpoint     = strdup(e_ai_endpoint ? e_ai_endpoint : "");
 was->model        = strdup(e_ai_model ? e_ai_model : "");
 was->cafile       = strdup(e_ai_cafile ? e_ai_cafile : "");
}

/**
 * ai_opt_opened_drop - Release what ai_opt_opened_take kept.
 */
static void ai_opt_opened_drop(struct ai_opt_opened *was)
{
 free(was->endpoint);
 free(was->model);
 free(was->cafile);
}

/**
 * ai_str_changed - True when a session string no longer reads as it did.
 */
static int ai_str_changed(const char *was, const char *now)
{
 return strcmp(was ? was : "", now ? now : "") != 0;
}

/**
 * ai_opt_report_user_changes - Tell the config layer which keys Ok changed.
 *
 * Called once Ok has applied every field, right before the options are saved.
 * A key whose value differs from the one the dialog opened with was changed by
 * the user (typed, picked with Alt-M or Alt-V, or reset by a backend switch),
 * so it is what the user wants in xwperc even when an XWPE_AI_* variable was
 * overriding it; a key left alone keeps the file's own value.
 */
static void ai_opt_report_user_changes(const struct ai_opt_opened *was)
{
 if (was->backend != e_ai_backend)
  wpe_ai_cfg_user_changed(WPE_AI_CFG_BACKEND);
 if (was->policy != e_ai_policy)
  wpe_ai_cfg_user_changed(WPE_AI_CFG_POLICY);
 if (was->agent_engine != e_ai_agent_engine)
  wpe_ai_cfg_user_changed(WPE_AI_CFG_AGENT_ENGINE);
 if (ai_str_changed(was->endpoint, e_ai_endpoint))
  wpe_ai_cfg_user_changed(WPE_AI_CFG_ENDPOINT);
 if (ai_str_changed(was->model, e_ai_model))
  wpe_ai_cfg_user_changed(WPE_AI_CFG_MODEL);
 if (ai_str_changed(was->cafile, e_ai_cafile))
  wpe_ai_cfg_user_changed(WPE_AI_CFG_CAFILE);
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
 struct ai_opt_opened opened;
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
 ai_opt_opened_take(&opened);
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

 /* The box shows the EFFECTIVE state (XWPE_AI_ENABLE=1 keeps the assistant on
    whatever the saved bit says), so it never contradicts what Alt-G does; Ok
    writes the saved bit from the box. */
 e_add_sswstr(3, 2, 0, AltE, wpe_ai_enabled() ? 1 : 0, "Enable AI assistant", o);

 /* --- Backend (left) + Permission (right), side by side.  Every radio needs a
        UNIQUE non-zero sw (sw==0 means "no field", unreachable).  Add order sets
        the group index, so backend (0) precedes permission (1) precedes agent. */
 e_add_txtstr(3, 4, "Backend:", o);
 e_add_txtstr(36, 4, "Permission:", o);
 for (i = 0; i < 4; i++)
  e_add_pswstr(0, 5, 5 + i, i, 4000 + i, (i == 3) ? bcur : 0, (char *)bklab[i], o);
 for (i = 0; i < 3; i++)
  e_add_pswstr(1, 38, 5 + i, i, 4200 + i, (i == 2) ? e_ai_policy : 0, (char *)pol[i], o);

 /* Model sits with Backend/Permission as a general choice -- every backend but
    Claude CLI uses it -- NOT inside the Connection block below, which is the
    HTTP-only (Ollama / OpenAI-compatible) endpoint/CA/key that Provider switches
    among saved profiles. */
 e_add_txtstr(3, 9, "Model (Alt-M):", o);
 e_add_bttstr(20, 9, 0, AltM, g_ai_opt_mlabel, e_ai_opt_pick_model, o);

 /* Say plainly that the Connection block is for the HTTP backends only, so a
    populated Endpoint/CA does not look "mixed" when Claude CLI/API (which ignore
    them) is the selected backend. */
 e_add_txtstr(3, 10, "Connection - Ollama / OpenAI-compatible backends ONLY:", o);
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
 e_add_bttstr(5, 16, 0, AltA, "Advanced (Alt-A)...", e_ai_opt_advanced, o);

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
 if (ret == WPE_ESC) {
  ai_opt_opened_drop(&opened);
  g_ai_opt_dlg = NULL;
  freeostr(o);
  return 0;
 }

 /* Ok read: apply EVERY field, then save once.  (A Model pick happens in place:
    the Model button floats the picker over the dialog and never returns here.) */
 f->ed->edopt = (f->ed->edopt & ~ED_AI_ENABLE) | (o->sstr[0]->num ? ED_AI_ENABLE : 0);
 e_ai_policy  = (o->pstr[1]->num >= 0 && o->pstr[1]->num < 3) ? o->pstr[1]->num : 0;
 new_be = bk[(o->pstr[0]->num >= 0 && o->pstr[0]->num < 4) ? o->pstr[0]->num : 0];
 if (ai_opt_commit_connection_fields(o))
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
 ai_opt_report_user_changes(&opened);
 ai_opt_opened_drop(&opened);

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

/* ---------------- agent plan / live TODO list (Messages window) ------------ */

#define AI_PLAN_MAX 32
static char g_ai_plan[AI_PLAN_MAX][200];  /* the tasks the agent laid out */
static int  g_ai_plan_done[AI_PLAN_MAX];  /* per-task: completed?         */
static int  g_ai_plan_n;

/**
 * ai_plan_show - Draw the plan as a live checklist in the Messages window.
 *
 * A long agent run is far more legible with a dockable to-do list than a
 * scrolling pane log: the plan sits in the Messages window and is REDRAWN IN
 * PLACE (sw=1 clears and restarts) each time a step completes, so its status --
 * "3/7 done", "[x]" per finished task -- updates live instead of scrolling away.
 */
static void ai_plan_show(FENSTER *f)
{
 char line[260];
 int i, ndone = 0;
 for (i = 0; i < g_ai_plan_n; i++) if (g_ai_plan_done[i]) ndone++;
 snprintf(line, sizeof line, "=== Agent plan: %d/%d done ===", ndone, g_ai_plan_n);
 e_d_p_message(line, f, 1);
 for (i = 0; i < g_ai_plan_n; i++) {
  snprintf(line, sizeof line, "  [%c] %s", g_ai_plan_done[i] ? 'x' : ' ', g_ai_plan[i]);
  e_d_p_message(line, f, 0);
 }
 if (g_ai_plan_n == 0) e_d_p_message("  (no steps given)", f, 0);
}

/**
 * ai_todo_load - Load the agent's plan body (task lines up to @@END) into the
 * checklist, all pending, and show it before the agent acts so the user can
 * course-correct.
 */
static void ai_todo_load(FENSTER *f, const char *body)
{
 const char *p = body, *stop = strstr(body, "\n@@END");
 g_ai_plan_n = 0;
 while (p && *p && (!stop || p < stop) && g_ai_plan_n < AI_PLAN_MAX) {
  const char *nl = strchr(p, '\n');
  int len = nl ? (int)(nl - p) : (int)strlen(p);
  if (stop && (!nl || nl > stop)) len = (int)(stop - p);
  while (len && (*p == ' ' || *p == '\t' || *p == '-' || *p == '*')) { p++; len--; }
  if (len > 0) {
   int k = len < (int)sizeof g_ai_plan[0] - 1 ? len : (int)sizeof g_ai_plan[0] - 1;
   memcpy(g_ai_plan[g_ai_plan_n], p, (size_t)k);
   g_ai_plan[g_ai_plan_n][k] = '\0';
   g_ai_plan_done[g_ai_plan_n] = 0;
   g_ai_plan_n++;
  }
  if (!nl) break;
  p = nl + 1;
 }
 ai_plan_show(f);
}

/**
 * ai_plan_mark_done - Tick off a finished step and redraw the checklist.
 * Match the reported text to a pending task (substring either way); fall back to
 * the earliest pending task, since an agent usually finishes them in order.
 */
static void ai_plan_progress(FENSTER *f, const char *task)
{
 char line[280];
 int i, ndone = 0;
 for (i = 0; i < g_ai_plan_n; i++) if (g_ai_plan_done[i]) ndone++;
 snprintf(line, sizeof line, "[plan] %d/%d done  ok: %.200s",
          ndone, g_ai_plan_n, task && *task ? task : "(step)");
 ai_pane(f, line, 0);            /* visible progress in the pane during the run */
}

static void ai_plan_mark_done(FENSTER *f, const char *arg)
{
 int i, first_pending = -1;
 for (i = 0; i < g_ai_plan_n; i++) {
  if (g_ai_plan_done[i]) continue;
  if (first_pending < 0) first_pending = i;
  if (arg && *arg && (strstr(g_ai_plan[i], arg) || strstr(arg, g_ai_plan[i]))) {
   g_ai_plan_done[i] = 1;
   ai_plan_show(f);                          /* update the Messages-window list */
   ai_plan_progress(f, g_ai_plan[i]);        /* and a compact pane line         */
   return;
  }
 }
 if (first_pending >= 0) {
  g_ai_plan_done[first_pending] = 1;
  ai_plan_show(f);
  ai_plan_progress(f, arg);
 }
}

/**
 * ai_agent_write_file - Put the agent's content on disk.
 * @path: the file, as the agent named it; @content: its complete new text.
 *
 * Return: 1 when written, 0 when the file could not be opened for writing.
 */
static int ai_agent_write_file(const char *path, const char *content)
{
 FILE *w = fopen(path, "wb");
 if (!w) return 0;
 fwrite(content, 1, strlen(content), w);
 fclose(w);
 return 1;
}

/**
 * ai_agent_report_undo_hint - Tell the user how far Ctrl-U reaches after a write.
 * @f: the window the pane belongs to; @path: the file, as the agent named it.
 *
 * The agent's text is already on disk, so Ctrl-U in the file window restores
 * the buffer only; the file keeps the agent's version until the user saves or
 * reverts it in the changeset review.  Said once per write so nobody expects
 * Ctrl-U to have undone the write on disk.
 */
static void ai_agent_report_undo_hint(FENSTER *f, const char *path)
{
 char line[720];
 snprintf(line, sizeof line, "[agent] wrote %s - Ctrl-U to undo (buffer only)", path);
 ai_pane(f, line, 0);
}

/**
 * ai_agent_show_written - Bring a written file into its window as one change.
 * @f: the window the pane belongs to; @path: the file, as the agent named it.
 *
 * The open window reloads as one undo step, the post-edit hook (if any)
 * reformats inside that same step, and the pane says what Ctrl-U will do, so
 * a write and its reformatting read as one change the user can take back with
 * one key.  A file that is not open is only written and run through the hook.
 */
static void ai_agent_show_written(FENSTER *f, const char *path)
{
 int shown = wpe_ai_reload_open_window(f, path);
 ai_run_edit_hook(f, path);
 if (shown) ai_agent_report_undo_hint(f, path);
}

/**
 * ai_agent_commit_write - Approve (per the permission dial) and write `content`
 * to `path`, reloading it if open.  Shared by write_file and apply_patch so both
 * go through the same review: auto/edits ask ai_agent_approve, ASK shows the
 * change as a diff (ai_diff_confirm_write) first.  Runs the post-edit hook after
 * a successful write, within the write's undo step.  Returns a malloc'd TOOL
 * RESULT.
 */
static char *ai_agent_commit_write(FENSTER *f, const char *path, const char *content)
{
 char what[720];
 int allow;
 if (!content) return strdup("(no content)");
 snprintf(what, sizeof what, "write %s (%zu bytes)", path, strlen(content));
 if (e_ai_policy != WPE_AI_POLICY_ASK) {
  allow = ai_agent_approve(f, what, 0);
 } else {
  char *old = ai_slurp_file(path);
  allow = ai_diff_confirm_write(f, path, old ? old : "", content);
  wpe_ai_trace("agent write confirm %s -> %s", path, allow ? "allow" : "deny");
  free(old);
 }
 if (!allow) return strdup("(denied by user)");
 if (!ai_agent_write_file(path, content)) return strdup("(write failed)");
 ai_agent_show_written(f, path);
 return strdup("(written)");
}

/**
 * ai_apply_search_replace - Apply SEARCH/REPLACE blocks to a file's text.
 * @old:  the current file content.
 * @body: the patch text after the TOOL line, up to a "@@END" line.
 * @out:  set to the malloc'd new content on success.
 * @err:  filled with a reason on failure.
 * Return: 1 on success, 0 on failure (unmatched search, malformed, no block).
 *
 * Each block is
 *     <<<<<<< SEARCH
 *     <exact lines to replace>
 *     =======
 *     <new lines>
 *     >>>>>>> REPLACE
 * and its SEARCH text must occur verbatim (the first match is replaced), so an
 * edit is precise and self-checking -- a mismatch returns an error the agent
 * loop hands back as "TOOL ERROR" for the model to retry, instead of silently
 * rewriting the wrong place.
 */
static int ai_apply_search_replace(const char *old, const char *body,
                                   char **out, char *err, size_t errn)
{
 static const char *S = "<<<<<<< SEARCH";
 static const char *M = "=======";
 static const char *R = ">>>>>>> REPLACE";
 char *cur = strdup(old ? old : "");
 const char *p = body;
 const char *stop = strstr(body, "\n@@END");
 int blocks = 0;

 if (!cur) { snprintf(err, errn, "out of memory"); return 0; }
 for (;;) {
  const char *sb = strstr(p, S), *mid, *re, *send, *rend;
  char *search, *replace, *hit, *nc;
  size_t sl, rl, pre, post;

  if (!sb || (stop && sb >= stop)) break;          /* no more blocks */
  sb += strlen(S);
  if (*sb == '\n') sb++;                            /* to the line after the marker */
  mid = strstr(sb, M);
  if (!mid) { snprintf(err, errn, "block missing the ======= divider"); free(cur); return 0; }
  send = mid;
  if (send > sb && send[-1] == '\n') send--;        /* drop the newline before ======= */
  sl = (size_t)(send - sb);
  re = mid + strlen(M);
  if (*re == '\n') re++;
  rend = strstr(re, R);
  if (!rend) { snprintf(err, errn, "block missing >>>>>>> REPLACE"); free(cur); return 0; }
  { const char *e = rend; if (e > re && e[-1] == '\n') e--; rl = (size_t)(e - re); }

  search = malloc(sl + 1);
  replace = malloc(rl + 1);
  if (!search || !replace) { free(search); free(replace); free(cur);
    snprintf(err, errn, "out of memory"); return 0; }
  memcpy(search, sb, sl);  search[sl] = '\0';
  memcpy(replace, re, rl);  replace[rl] = '\0';

  hit = sl ? strstr(cur, search) : NULL;
  if (!hit) {
   char snip[80];
   size_t k = sl < sizeof snip - 1 ? sl : sizeof snip - 1;
   char *nlp;
   memcpy(snip, search, k); snip[k] = '\0';
   if ((nlp = strchr(snip, '\n'))) *nlp = '\0';
   snprintf(err, errn, "SEARCH text not found: \"%s\"", snip);
   free(search); free(replace); free(cur); return 0;
  }
  pre = (size_t)(hit - cur);
  post = strlen(hit + sl);
  nc = malloc(pre + rl + post + 1);
  if (!nc) { free(search); free(replace); free(cur);
    snprintf(err, errn, "out of memory"); return 0; }
  memcpy(nc, cur, pre);
  memcpy(nc + pre, replace, rl);
  memcpy(nc + pre + rl, hit + sl, post);
  nc[pre + rl + post] = '\0';

  free(cur); cur = nc;
  free(search); free(replace);
  p = rend + strlen(R);
  blocks++;
 }
 if (!blocks) { snprintf(err, errn, "no SEARCH/REPLACE block found"); free(cur); return 0; }
 *out = cur;
 return 1;
}

/* Reduce fetched HTML to readable text for the model: drop <script>/<style>
 * bodies and every tag, decode the few common entities, and cap the length.
 * Not a real parser -- just enough that a page reads as text, not markup, and
 * does not flood the context.  Returns a malloc'd string (caller frees). */
static char *ai_html_to_text(const char *html)
{
 size_t n = html ? strlen(html) : 0, o = 0, i = 0;
 const size_t CAP = 12000;            /* keep a page down to a summarisable size */
 char *out = malloc(n + 1);
 if (!out) return NULL;
 while (html[i] && o < CAP) {
  if (html[i] == '<') {
   /* Skip the entire body of <script>/<style>, not just the tag. */
   if (!strncasecmp(html + i, "<script", 7) || !strncasecmp(html + i, "<style", 6)) {
    const char *close = (html[i + 1] == 's' || html[i + 1] == 'S') &&
                        (html[i + 3] == 'r' || html[i + 3] == 'R')
                        ? "</script" : "</style";
    size_t cl = strlen(close);
    i += 6;
    while (html[i] && strncasecmp(html + i, close, cl)) i++;
    while (html[i] && html[i] != '>') i++;
    if (html[i]) i++;
    continue;
   }
   while (html[i] && html[i] != '>') i++;   /* drop the tag */
   if (html[i]) i++;
   if (o && out[o - 1] != '\n' && out[o - 1] != ' ') out[o++] = ' ';
   continue;
  }
  if (html[i] == '&') {                      /* decode a handful of entities */
   static const struct { const char *e; char c; } ent[] = {
    {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'},
    {"&#39;", '\''}, {"&apos;", '\''}, {"&nbsp;", ' '}
   };
   int k, matched = 0;
   for (k = 0; k < (int)(sizeof ent / sizeof ent[0]); k++) {
    size_t el = strlen(ent[k].e);
    if (!strncmp(html + i, ent[k].e, el)) { out[o++] = ent[k].c; i += el; matched = 1; break; }
   }
   if (matched) continue;
  }
  out[o++] = html[i++];
 }
 if (o >= CAP) {                             /* mark that the page was clipped */
  const char *mark = "\n...(truncated)";
  size_t ml = strlen(mark);
  if (o + ml < n + 1) { memcpy(out + o, mark, ml); o += ml; }
 }
 out[o] = '\0';
 return out;
}

/* Percent-encode a query for a URL, leaving RFC 3986 unreserved bytes as-is. */
static void ai_url_encode(const char *s, char *out, size_t outsz)
{
 static const char hex[] = "0123456789ABCDEF";
 size_t o = 0;
 for (; s && *s && o + 4 < outsz; s++) {
  unsigned char c = (unsigned char)*s;
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
      (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
   out[o++] = (char)c;
  else {
   out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15];
  }
 }
 out[o] = '\0';
}

/* Build the search URL from the template: the first "%s" is replaced with the
 * (already-encoded) query; a template with no "%s" gets the query appended.  The
 * template is passed as DATA (a %-arg), so a stray % in it is never a format. */
static void ai_search_url_build(const char *tmpl, const char *q, char *out, size_t outsz)
{
 const char *pct = strstr(tmpl, "%s");
 if (pct)
  snprintf(out, outsz, "%.*s%s%s", (int)(pct - tmpl), tmpl, q, pct + 2);
 else
  snprintf(out, outsz, "%s%s", tmpl, q);
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
  } else if (!strcmp(tool, "web_fetch")) {
   /* Read a web page.  Network egress, so it goes through the same approval as
      run_command; the body is reduced to text so the model reads content, not
      markup.  This is the built-in agent's only route onto the web, for models
      that have no web tool of their own (Ollama, a Groq/Lumo OpenAI endpoint). */
   if (!ai_agent_approve(f, arg, 1)) {
    result = strdup("(denied by user)");
   } else {
    char *raw = NULL, werr[200];
    werr[0] = '\0';
    if (wpe_ai_web_fetch(arg, 65536, &raw, werr, sizeof werr) == 0 && raw) {
     result = ai_html_to_text(raw);
     free(raw);
    } else {
     size_t n = strlen(werr) + 20;
     result = malloc(n);
     if (result) snprintf(result, n, "TOOL ERROR: %s", werr[0] ? werr : "fetch failed");
    }
   }
  } else if (!strcmp(tool, "web_search")) {
   /* Search the web for the model: build a query URL from a configurable
      template (AISearchURL, default a keyless engine) and fetch it as text.
      The model reads the result links, then web_fetch's the promising one. */
   if (!ai_agent_approve(f, arg, 1)) {
    result = strdup("(denied by user)");
   } else {
    char enc[1024], surl[1400], *raw = NULL, werr[200];
    const char *tmpl = (e_ai_search_url && *e_ai_search_url) ? e_ai_search_url
                       : "https://lite.duckduckgo.com/lite/?q=%s";
    werr[0] = '\0';
    ai_url_encode(arg, enc, sizeof enc);
    ai_search_url_build(tmpl, enc, surl, sizeof surl);
    if (wpe_ai_web_fetch(surl, 65536, &raw, werr, sizeof werr) == 0 && raw) {
     result = ai_html_to_text(raw);
     free(raw);
    } else {
     size_t n = strlen(werr) + 20;
     result = malloc(n);
     if (result) snprintf(result, n, "TOOL ERROR: %s", werr[0] ? werr : "search failed");
    }
   }
  } else if (!strcmp(tool, "write_file")) {
   char *content = NULL;
   if (firstnl) {
    char *body = firstnl + 1, *endm = strstr(body, "\n@@END");
    size_t cl = endm ? (size_t)(endm - body) : strlen(body);
    content = malloc(cl + 1);
    if (content) { memcpy(content, body, cl); content[cl] = '\0'; }
   }
   result = ai_agent_commit_write(f, arg, content);   /* shared review + write */
   free(content);
  } else if (!strcmp(tool, "apply_patch")) {
   /* Precise edit: apply SEARCH/REPLACE blocks against the current file, then
      write the result through the same review as write_file.  An unmatched
      SEARCH becomes a TOOL ERROR so the model retries with better context. */
   char *old = ai_slurp_file(arg);
   char *newc = NULL, perr[300];
   if (!old) {
    result = strdup("TOOL ERROR: file not found (use write_file to create it)");
   } else if (!firstnl) {
    result = strdup("TOOL ERROR: apply_patch needs SEARCH/REPLACE blocks");
   } else if (!ai_apply_search_replace(old, firstnl + 1, &newc, perr, sizeof perr)) {
    char *r = malloc(strlen(perr) + 16);
    if (r) { snprintf(r, strlen(perr) + 16, "TOOL ERROR: %s", perr); result = r; }
   } else {
    result = ai_agent_commit_write(f, arg, newc);
   }
   free(old);
   free(newc);
  } else if (!strcmp(tool, "plan")) {
   ai_todo_load(f, firstnl ? firstnl + 1 : "");
   ai_pane(f, "[plan] laid out - the checklist is in the Messages window "
              "(updates as steps finish)", 0);
   result = strdup("(plan shown -- now do the steps, one tool per turn, "
                   "reporting each with TOOL step_done <task>)");
  } else if (!strcmp(tool, "step_done")) {
   ai_plan_mark_done(f, arg);
   result = strdup("(recorded)");
  } else {
   result = strdup("(unknown tool)");
  }
  result = ai_tool_result_nonempty(result);   /* empty output -> explicit note */
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
   "  TOOL plan          (for a multi-step task: follow with one task per line, "
   "then a line that is exactly @@END; shown to the user before you act)\n"
   "  TOOL step_done <task>   (mark a planned step complete as you finish it)\n"
   "  TOOL list_dir <path>\n"
   "  TOOL read_file <path>\n"
   "  TOOL grep <pattern>\n"
   "  TOOL glob <name-pattern>\n"
   "  TOOL run_command <shell command>\n"
   "  TOOL web_search <query>   (search the web; returns result links to then "
   "read with web_fetch)\n"
   "  TOOL web_fetch <http(s) URL>   (read a web page when you need up-to-date "
   "or external information)\n"
   "  TOOL write_file <path>\n"
   "  TOOL apply_patch <path>\n"
   "For write_file, put the new file content on the following lines, ending "
   "with a line that is exactly @@END .\n"
   "PREFER apply_patch to change an EXISTING file (write_file is for a new file "
   "or a full rewrite).  For apply_patch, follow the TOOL line with one or more "
   "blocks and then a line that is exactly @@END :\n"
   "<<<<<<< SEARCH\n"
   "<exact lines from the file to replace>\n"
   "=======\n"
   "<the new lines>\n"
   ">>>>>>> REPLACE\n"
   "The SEARCH text must match the file verbatim; include enough surrounding "
   "lines to be unique.\n"
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
 op->kind = AI_KIND_AGENT;
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
#ifdef WPE_AI_AGENT_HOST
 /* One-time nudge for the Claude CLI backend: the built-in agent runs the model
    text-only and applies each tool itself under the dial -- it never hands the
    model --dangerously-skip-permissions, so "Auto" does not make Claude act on
    its own here.  Full autonomy (the model driving its own tools) is the Claude
    Code agent engine, which is where the dial reaches the model's own tools. */
 {
  static int nudged = 0;
  if (!nudged && e_ai_backend == WPE_AI_CLAUDECLI && e_ai_policy != WPE_AI_POLICY_ASK) {
   nudged = 1;
   ai_pane(f, "[tip] For full autonomy (the model runs its own tools/edits), set "
              "Options > AI: Agent = Claude Code -- the built-in agent keeps every "
              "tool under xwpe's review.", 0);
  }
 }
#endif
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
static void ai_launch_fix_build(FENSTER *f, const char *goal);

static int e_ai_fix_build(FENSTER *f)
{
 char cmd[1024], extra[1700];
 if (wpe_ai_busy()) {
  ai_enqueue(f, ai_launch_fix_build, "");
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

/**
 * ai_launch_fix_build - Queue hook: start the build-and-fix loop when free.
 * @f: the window it was asked from; @goal: unused, the goal is fixed.
 *
 * What the user gains: asking for a build fix during another task behaves
 * like every other action -- it waits its turn instead of being refused.
 */
static void ai_launch_fix_build(FENSTER *f, const char *goal)
{
 (void)goal;
 e_ai_fix_build(f);
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
    /* The same cleaner as an Edit reply: one final newline, CRLF made LF, so
       a proposal with Windows line endings is diffed against the buffer on
       equal terms and shows only its real changes. */
    props[*np].text = ai_file_text(body, cl);
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
  result = ai_tool_result_nonempty(result);   /* empty output -> explicit note */
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

/* Mark in the approval box's gutter for a file the plan would write outside
   the workspace; the row says so in words too, and the pane repeats it when
   the file is applied. */
#define AI_PLAN_OUTSIDE_MARK "!"
#define AI_PLAN_OUTSIDE_WORD "outside the workspace"

/* What the user chose in the approval box. */
#define AI_PLAN_CANCEL    0
#define AI_PLAN_APPLY_ALL 1
#define AI_PLAN_REVIEW    2

/**
 * ai_plan_count_change - Lines a proposal adds and removes against the live file.
 * @f: the window the plan was asked from; @p: the proposal;
 * @plus: receives the lines added; @minus: receives the lines removed.
 *
 * The file is read as the editor has it now, unsaved edits included, so the
 * numbers in the box are the numbers Apply all acts on: an edit typed before
 * the plan is counted, not overwritten in silence.
 *
 * Return: 1 when the file exists (as a buffer or on disk), 0 for a new file.
 */
static int ai_plan_count_change(FENSTER *f, const ai_proposal *p, int *plus, int *minus)
{
 char *now = wpe_ai_read_scope_file(f, p->path);
 wpe_ai_seg *segs;
 int ns, k;
 *plus = 0;
 *minus = 0;
 ns = wpe_ai_diff_segments(now ? now : "", p->text, &segs);
 for (k = 0; k < ns; k++) {
  if (!segs[k].is_change) continue;
  *plus += segs[k].bn;
  *minus += segs[k].an;
 }
 wpe_ai_segs_free(segs, ns);
 if (!now) return 0;
 free(now);
 return 1;
}

/**
 * ai_row_append - Add a piece of text to a box row, within the row's size.
 * @row: the row so far; @sz: its size; @text: what to add.
 */
static void ai_row_append(char *row, size_t sz, const char *text)
{
 size_t used = strlen(row);
 if (used >= sz) return;
 snprintf(row + used, sz - used, "%s", text);
}

/**
 * ai_plan_row - One line of the approval box for a proposal.
 * @f: the window the plan was asked from; @p: the proposal;
 * @row: receives the text; @sz: its size; @attr: receives the row's colour.
 *
 * The row reads "  path  (+a -b)", with "[new file]" when nothing exists
 * under that name yet.  A file the plan would write outside the workspace
 * carries the gutter mark and the words "outside the workspace", in the
 * colour of a deletion, so the user sees before approving that the model is
 * reaching beyond the tree.  It is not refused: the model may be right that
 * the file belongs there, but the user must see it.
 */
static void ai_plan_row(FENSTER *f, const ai_proposal *p, char *row, size_t sz, int *attr)
{
 int plus, minus, exists, outside;
 char counts[64];
 exists = ai_plan_count_change(f, p, &plus, &minus);
 outside = wpe_ai_path_outside_workspace(f, p->path);
 snprintf(counts, sizeof counts, "  (+%d -%d)", plus, minus);
 if (outside) snprintf(row, sz, "%s %s", AI_PLAN_OUTSIDE_MARK, p->path);
 else snprintf(row, sz, "  %s", p->path);
 ai_row_append(row, sz, counts);
 if (!exists) ai_row_append(row, sz, "  [new file]");
 if (outside) ai_row_append(row, sz, "  " AI_PLAN_OUTSIDE_WORD);
 if (outside) *attr = f->fb->db.fb;      /* the deletion colour: look twice   */
 else *attr = f->fb->dy.fb;             /* green: a proposed change          */
}

/**
 * ai_plan_report_outside - Record in the pane that an applied file lies outside the workspace.
 * @f: the window the plan was asked from; @path: the file, as the model named it.
 *
 * The box warned before the choice; this line is the record after it, so a
 * user reading the pane later knows which file to look for beyond the tree.
 * Says nothing for a file inside the workspace.
 */
static void ai_plan_report_outside(FENSTER *f, const char *path)
{
 char line[1400];
 if (!wpe_ai_path_outside_workspace(f, path)) return;
 snprintf(line, sizeof line, "[plan] applied %s, %s", path, AI_PLAN_OUTSIDE_WORD);
 ai_pane(f, line, 0);
 wpe_ai_trace("plan applied outside workspace %s", path);
}

/**
 * ai_plan_choice - Show the approval box and return what the user chose.
 * @f: the window the plan was asked from; @pd: the proposals; @title: the box title.
 *
 * The plan is a modal confirmation popup, the same boxed overlay the Edit
 * review uses, so the choice is an explicit dialog that grabs the focus and
 * nothing paints under it, not a log line the user might miss.  Its rows are
 * the preview of a Multi-file change: live counts per file, the new-file and
 * outside-the-workspace marks.
 *
 * Return: AI_PLAN_APPLY_ALL, AI_PLAN_REVIEW or AI_PLAN_CANCEL.
 */
static int ai_plan_choice(FENSTER *f, ai_plan_data *pd, const char *title)
{
 char *rt[AI_PLAN_MAX];
 int ra[AI_PLAN_MAX];
 int nr = 0, i, key;
 for (i = 0; i < pd->np && nr < AI_PLAN_MAX; i++) {
  char row[600];
  ai_plan_row(f, &pd->props[i], row, sizeof row, &ra[nr]);
  rt[nr] = strdup(row);
  nr++;
 }
 key = ai_diff_box_show(f, rt, ra, nr, title,
       " Enter/A = Apply all    F = Review file-by-file    Q/Esc = Cancel ", "AFQ", 0);
 for (i = 0; i < nr; i++) free(rt[i]);
 if (key == 13 || key == 'A') return AI_PLAN_APPLY_ALL;
 if (key == 'F') return AI_PLAN_REVIEW;
 return AI_PLAN_CANCEL;
}

/**
 * ai_plan_review_file - Offer one proposal hunk by hunk in the open window.
 * @w: the window holding the file; @p: the proposal.
 *
 * The same diff box as an Edit, against the whole file as the window has it.
 *
 * Return: 1 when the user accepted something and it was applied, 0 otherwise.
 */
static int ai_plan_review_file(FENSTER *w, const ai_proposal *p)
{
 char *now = ai_full_file_text(w);        /* the diff needs the whole file */
 wpe_ai_seg *segs;
 int ns, applied = 0;
 char *result;
 ns = wpe_ai_diff_segments(now ? now : "", p->text, &segs);
 free(now);
 result = ai_hunk_apply(w, segs, ns, NULL, NULL, NULL);
 if (result) {
  e_ai_apply_text(w, result);
  free(result);
  applied = 1;
 }
 wpe_ai_segs_free(segs, ns);
 return applied;
}

/* Plan wrap-up: persist, then the permission moment -- list the proposed files
 * with +/- counts and apply-all / file-by-file / cancel. */
static void ai_plan_finish(ai_async_op *op)
{
 ai_plan_data *pd = op->ud;
 FENSTER *f = op->f;
 char line[1400], title[80];
 int i, np = pd->np, mode, applied = 0;

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
 snprintf(title, sizeof title, "Apply AI plan - %d file%s", np, np == 1 ? "" : "s");
 mode = ai_plan_choice(f, pd, title);
 if (mode == AI_PLAN_CANCEL) {
  ai_pane(f, "[plan] cancelled - nothing changed", 0);
  wpe_ai_trace("plan cancelled");
  return;
 }
 for (i = 0; i < np; i++) {
  ECNT *cn = f->ed;
  FENSTER *w;
  const char *path = pd->props[i].path;
  int done;
  if (e_edit(cn, pd->props[i].path) != 0) {
   snprintf(line, sizeof line, "[plan] could not open %s", path);
   ai_pane(f, line, 0);
   continue;
  }
  w = cn->f[cn->mxedt];
  if (mode == AI_PLAN_APPLY_ALL) {
   e_ai_apply_text(w, pd->props[i].text);
   done = 1;
  } else {
   done = ai_plan_review_file(w, &pd->props[i]);
  }
  if (!done) {
   wpe_ai_trace("plan skipped %s", path);
   continue;
  }
  applied++;
  wpe_ai_trace("plan applied %s", path);
  ai_plan_report_outside(f, path);
 }
 snprintf(line, sizeof line, "[plan] applied %d of %d file%s - each is open (Ctrl-U undoes per window)", applied, np, np == 1 ? "" : "s");
 ai_pane(f, line, 0);
 wpe_ai_trace("plan done applied=%d", applied);
}

/**
 * ai_plan_launch - Start a multi-file study-and-propose run on `task`.
 * @f: the window it was asked from; @task: what the user wants changed.
 *
 * Split from the prompt so Alt-G f and a queued request share one launch: the
 * task is typed once and runs either now or when the running task ends.
 */
static void ai_plan_launch(FENSTER *f, const char *task)
{
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

 err[0] = '\0';
 if (wpe_ai_preflight(e_ai_backend, err, sizeof err)) { ai_pane(f, err, 1); return; }
 err[0] = '\0';
 if (wpe_ai_ensure_model(err, sizeof err)) { ai_pane(f, err[0] ? err : "no model set", 1); return; }

 e_ai_cli_mode = WPE_AI_CLI_TEXTONLY;               /* xwpe owns every edit */
 wpe_ai_session_load(f);
 pd = calloc(1, sizeof *pd);
 if (!pd) return;
 pd->nsc = wpe_ai_scope_files(f, e_project_is_open(), 1, &pd->scope);
 snprintf(line, sizeof line, "[plan] task: %s  (scope: %d files)", task, pd->nsc);
 ai_pane(f, line, 1);
 wpe_ai_trace("plan task=%s scope=%d", task, pd->nsc);

 for (wi = 1; wi <= cn->mxedt; wi++)
  if (cn->f[wi] == f) { save_id = cn->edt[wi]; break; }
 op = calloc(1, sizeof *op);
 if (!op) { ai_plan_free_data(pd); return; }
 op->f = f; op->cn = cn; op->save_id = save_id;
 op->kind = AI_KIND_PLAN;
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
}

/**
 * e_ai_plan - Change several files at once (Alt-G f, "Multi-file edit").
 * @f: the window it was asked from.  Return: 0, the editor-action convention.
 *
 * Asks for the task first and only then looks at what is running, so the task
 * can be typed while another one finishes; it is then queued and starts by
 * itself, like every other assistant action.
 */
static int e_ai_plan(FENSTER *f)
{
 static char task[AI_PROMPT_MAX];

 task[0] = '\0';
 if (!e_ai_prompt1(task, "AI plan: task", f) || !task[0])
  return 0;
 if (wpe_ai_busy()) {
  ai_enqueue(f, ai_plan_launch, task);
  return 0;
 }
 ai_plan_launch(f, task);
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

/* A short name for the hosted engine, for the pane messages: the configured
   command, else "Claude Code" (the built-in default). */
static const char *host_engine_name(void)
{
 if (e_ai_host_command && *e_ai_host_command) return e_ai_host_command;
 return "Claude Code";
}

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
 if (hup) {
  char l[120];
  snprintf(l, sizeof l, "[agent] %.80s session ended", host_engine_name());
  e_ai_host_end(l);
 }
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
 { char title[120];
   snprintf(title, sizeof title, "%.80s: %s", host_engine_name(),
            g_host ? "next (Esc ends)" : "task");
   if (!e_ai_prompt1(task, title, f) || !task[0]) {
    if (g_host) {                        /* empty/Esc ends a live session */
     /* Safe to go modal here (user context, not the fd callback): let the user
        review/revert what the agent changed against the start-of-session
        checkpoint, then tear down. */
     char l[120];
     if (wpe_ai_checkpoint_active()) wpe_ai_changeset_review(f);
     snprintf(l, sizeof l, "[agent] %.80s session ended", host_engine_name());
     e_ai_host_end(l);
    }
    return 0;
   }
 }
 if (!g_host) {
  err[0] = '\0';
  g_host = wpe_host_start(e_ai_model, "", e_ai_policy, err, sizeof err);
  if (!g_host) {
   char l[360];
   snprintf(l, sizeof l, "[agent] could not start %.80s: %.240s",
            host_engine_name(), err[0] ? err : "?");
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
  { char l[160];
    snprintf(l, sizeof l, "[agent] engine: %.80s - session started (Esc at the prompt ends it)",
             host_engine_name());
    ai_pane(f, l, 1); }
  wpe_ai_trace("host start policy=%s adapter=%d", wpe_ai_policy_name(e_ai_policy), e_ai_host_adapter);
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
 { "Complete at cursor",  'C', e_ai_complete         },
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

/* ================= a window closes: let go of it ==========================
 * Everything asynchronous in this file remembers the window it was started
 * from: the background op (Edit, Complete, Agent, Plan) repaints its spinner
 * through it on every heartbeat and applies the reply to it, a chat stream
 * paints into the pane found from it, the queued follow-up launches on it, the
 * armed chat input lives in it, and the hosted agent anchors its pane to it.
 * e_close_window tells us here, before it frees the window, so none of those
 * is left holding freed memory. */

/**
 * ai_op_note_window_gone - Tell the user the task went away with its window.
 * @cn: the desktop; @closing: the window on its way out; @kind: "edit", ...
 *
 * The spinner line would otherwise sit frozen at "[AI edit] working" with no
 * reply ever coming.  Writes into the pane that is already open and nowhere
 * else: opening the pane here would push a window onto the stack while
 * e_close_window is in the middle of popping one, and painting into the pane
 * when the pane is the window being closed would paint into freed memory.
 */
static void ai_op_note_window_gone(ECNT *cn, FENSTER *closing, const char *kind)
{
 FENSTER *wf = ai_pane_existing(cn);
 char line[120];
 if (!wf || wf == closing)
  return;
 snprintf(line, sizeof line, "[AI %s] dropped - its window was closed", kind);
 ai_pane_set_last(wf, line);
}

/**
 * ai_op_drop_for_window - End the background op if it targets the closing window.
 * @closing: the window e_close_window is about to free.
 *
 * Leaves the fd-loop (stream and heartbeat) first, so no spinner tick or reply
 * callback can run against the window after this returns, then frees the op
 * and notes it in the pane.  The reply is never applied anywhere: its target
 * file is gone.  The trace line ("edit target gone") is the same one the late
 * reply path prints, so a trace reader sees one event whichever side caught it.
 */
static void ai_op_drop_for_window(FENSTER *closing)
{
 ai_async_op *op = g_ai_op;
 ECNT *cn;
 const char *kind;
 if (!op || op->f != closing)
  return;
 cn = op->cn;
 kind = op->kind ? op->kind : "task";
 wpe_ai_trace("%s target gone", kind);
 ai_op_detach(op);
 ai_op_free(op);
 ai_op_note_window_gone(cn, closing, kind);
}

/**
 * ai_queue_drop_for_window - Forget a queued request whose window is closing.
 * @closing: the window e_close_window is about to free.
 *
 * A request queued behind the running task would launch on that window when
 * the task ends; with the window gone there is nothing to launch it on.
 */
static void ai_queue_drop_for_window(FENSTER *closing)
{
 if (!g_ai_queue.pending || g_ai_queue.f != closing)
  return;
 wpe_ai_trace("queued request dropped (window closed)");
 ai_queue_clear();
}

/**
 * ai_chat_drop_for_window - End a streaming chat anchored to the closing window.
 * @closing: the window e_close_window is about to free.
 *
 * The chat finds the pane and the workspace through its anchor window on every
 * delta; once that window is gone the stream is closed and the partial answer
 * stays in the pane as far as it got.
 */
static void ai_chat_drop_for_window(FENSTER *closing)
{
 ai_chat_session *s = g_ai_chat;
 if (!s || s->ref != closing)
  return;
 wpe_ai_trace("chat target gone");
 s->active = 0;
 ai_chat_finish(s);
}

/**
 * ai_chat_input_drop_for_window - Disarm the chat input when its pane closes.
 * @closing: the window e_close_window is about to free.
 *
 * The "> " input rows live in the pane's own buffer, which is freed with the
 * pane; forgetting the armed pane here keeps the next keystroke from being
 * routed into it.
 */
static void ai_chat_input_drop_for_window(FENSTER *closing)
{
 if (!g_ai_chat_pane || g_ai_chat_pane != closing)
  return;
 g_ai_chat_focus = 0;
 g_ai_chat_pane = NULL;
 g_ai_input_rows = 0;
}

#ifdef WPE_AI_AGENT_HOST
/**
 * ai_host_drop_for_window - End the hosted agent session anchored to the window.
 * @closing: the window e_close_window is about to free.
 *
 * The host renders its events through its anchor window; ending the session
 * here (silently, the pane message would need the window) stops the child and
 * its fd-loop callback before the window is freed.
 */
static void ai_host_drop_for_window(FENSTER *closing)
{
 if (!g_host || g_host_win != closing)
  return;
 wpe_ai_trace("host target gone");
 e_ai_host_end(NULL);
}
#endif

/**
 * wpe_ai_window_closing - Let go of every pending AI reference to a window.
 * @closing: the window e_close_window is about to free.
 *
 * Called from e_close_window once the close is certain (after the "save?"
 * question, before the window is freed).  Until now a background Edit, Agent or
 * Plan kept the window it was started from and repainted its spinner through it
 * on every heartbeat, so closing that window with Ctrl-W while "[AI edit]
 * working" was spinning crashed the editor on the next tick (a read through the
 * freed window).  For the user: Ctrl-W on a file during a pending AI task just
 * closes the file; the pane reports the task as dropped, nothing is applied
 * anywhere, and the editor keeps taking keys.
 *
 * The close notifies the AI layer rather than having every heartbeat re-check
 * the window stack: a liveness scan after the free can only compare addresses,
 * and a freed window's address is reused by the next window opened, so a late
 * check can mistake a new file for the old target; the stream and the
 * heartbeat would also keep running for a reply nobody can apply.  Dropping at
 * close time is exact, happens once, and is the same answer for every kind of
 * pending work: the op, the queued request, the chat stream, the armed chat
 * input, the hosted agent and the background caret window.
 */
void wpe_ai_window_closing(FENSTER *closing)
{
 ai_queue_drop_for_window(closing);
 ai_op_drop_for_window(closing);
 ai_chat_drop_for_window(closing);
 ai_chat_input_drop_for_window(closing);
#ifdef WPE_AI_AGENT_HOST
 ai_host_drop_for_window(closing);
#endif
 if (g_ai_bg_win == closing)
  g_ai_bg_win = NULL;
}

#endif /* WPE_AI */

typedef int wpe_ai_ui_translation_unit;
