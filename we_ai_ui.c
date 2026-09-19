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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <poll.h>
#include <unistd.h>
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

static void ai_pane(FENSTER *f, const char *line, int surface)
{
 if (g_ai_chat_focus) {                 /* keep the "> " input row at the bottom */
  FENSTER *wf = ai_pane_win(f);
  if (wf) { ai_tr_line(wf, line); ai_pane_paint(wf); }
  return;
 }
 e_d_p_named(AI_PANE_NAME, (char *)line, f, surface ? 1 : 0);
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
 int line_idx = 0, col = 0, i;
 for (i = 0; i < g_ai_input_pos && i < g_ai_input_len; i++) {
  if (g_ai_input[i] == '\n') { line_idx++; col = 0; }
  else if (((unsigned char)g_ai_input[i] & 0xC0) != 0x80) col++;  /* one per glyph */
 }
 *cy = wf->b->mxlines - rows + line_idx;
 *cx = 2 + col;
}

/* Repaint the pane window and keep the caret in view.  The pane is an ordinary
 * window, so the caret belongs to whichever window is FOCUSED: it sits in the
 * chat input row only while the pane itself is focused; when the user has
 * switched to their file (or an async op runs), the pane repaints in the
 * background and the caret stays in the focused/user window. */
static void ai_pane_paint(FENSTER *wf)
{
 FENSTER *act = wf->ed->f[wf->ed->mxedt];       /* the focused window */
 FENSTER *caret = g_ai_bg_win ? g_ai_bg_win : act;
 int pane_focused = (act == wf && !g_ai_bg_win);

 if (pane_focused && g_ai_chat_focus) {         /* caret sits in the input region */
  int cy, cx;
  ai_input_caret(wf, &cy, &cx);
  wf->b->b.y = cy;
  wf->b->b.x = cx;
 } else if (pane_focused) {
  int y = wf->b->mxlines - 1;
  wf->b->b.y = y;
  wf->b->b.x = (y >= 0 && wf->b->bf[y].s) ? wf->b->bf[y].len : 0;
 }
 if (g_ai_scroll_lock) {
  /* user scrolled up to browse -- leave the view where they left it */
 } else if (g_ai_chat_focus) {
  int vh = wf->e.y - wf->a.y - 1;               /* pin the input row to the bottom */
  int bottom = wf->b->mxlines - vh;
  wf->s->c.y = bottom > 0 ? bottom : 0;
 } else {
  e_messages_scroll_to_bottom(wf);
 }
 e_schirm(wf, 0);
 e_cursor(caret, 0);
 e_refresh();
}

/* Scroll the transcript view by `delta` lines to peek at history.  The view
 * holds until the next key: any edit key repaints and snaps back to the input
 * (ai_pane_paint scrolls to the bottom), so PgUp/PgDn browse, then editing
 * returns to the prompt. */
static void ai_pane_scroll(FENSTER *wf, int delta)
{
 SCHIRM *s = wf->s;
 int visible_h = wf->e.y - wf->a.y - 1;
 int maxtop = wf->b->mxlines - visible_h;
 if (maxtop < 0) maxtop = 0;
 s->c.y += delta;
 if (s->c.y > maxtop) s->c.y = maxtop;
 if (s->c.y < 0) s->c.y = 0;
 g_ai_scroll_lock = (s->c.y < maxtop);          /* released once back at the bottom */
 e_schirm(wf, 0);
 e_cursor(wf, 0);
 e_refresh();
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

/* Columns of reply text the pane can show on one line: inside its two borders,
 * never wider than the line buffer, and never absurdly small. */
static int ai_pane_width(FENSTER *wf)
{
 int w = wf->e.x - wf->a.x - 2;
 if (w > wf->b->mx.x - 1) w = wf->b->mx.x - 1;
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
 wf = ai_pane_win(s->ref);
 if (!wf) return;
 width = ai_pane_width(wf);
 if (!s->started) {          /* open a reply line under the "AI:" header once */
  ai_pane_commit(wf);
  s->started = 1;
 }

 for (i = 0; i < dl; i++) {
  char c = delta[i];
  if (c == '\r')
   continue;
  if (c == '\n') {
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
  if (((unsigned char)c & 0xC0) != 0x80)   /* not a UTF-8 continuation byte */
   s->pcols++;
  if (s->pcols >= width)                    /* soft-wrap at the pane width */
   ai_stream_wrap(s, wf);
 }
 ai_pane_set_last(wf, s->pending ? s->pending : "");   /* live partial line */
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
   /* nothing streamed: replace the "gathering..." placeholder. */
   FENSTER *wf = ai_pane_win(s->ref);
   if (wf) ai_pane_set_last(wf, AI_REPLY_PREFIX "(no answer)");
  }
  wpe_ai_session_save(s->ref);
  wpe_ai_trace("chat done");
  s->active = 0;
  ai_chat_finish(s);
 }
}

/* Build the chat system prompt: the assistant may INVESTIGATE the workspace
 * with read-only tools before answering, so questions about other files (not
 * just the open one) work.  Includes the workspace file listing and the current
 * file for immediate context. */
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
   "I will reply with the result; then either use another tool or give your "
   "answer.  When you can answer, reply with the answer directly (no TOOL "
   "line).  Do not guess about files you have not read.\n\n";
 if (!sys) { free(ctx); return NULL; }
 len += (size_t)snprintf(sys + len, cap - len, "%s", head);
 nsc = wpe_ai_scope_files(f, e_project_is_open(), 1, &scope);
 if (len < cap - 32)
  len += (size_t)snprintf(sys + len, cap - len, "WORKSPACE FILES:\n");
 for (i = 0; i < nsc && len < cap - 256; i++)
  len += (size_t)snprintf(sys + len, cap - len, "  %s\n", scope[i]);
 wpe_ai_free_list(scope, nsc);
 if (ctx && len < cap - 512)
  snprintf(sys + len, cap - len, "\n--- current file ---\n%.*s",
           (int)(cap - len - 32), ctx);
 free(ctx);
 return sys;
}

/* If the reply is a read-only tool call, run it and return the result (malloc'd,
 * to be fed back as the next turn's input); else return NULL so the reply is
 * treated as the final answer.  Chat only ever runs read-only tools. */
static char *ai_chat_tool_result(FENSTER *f, const char *reply)
{
 char action[1100], *nl, *arg;
 (void)f;
 nl = strchr(reply, '\n');
 { size_t l = nl ? (size_t)(nl - reply) : strlen(reply);
   if (l >= sizeof action) l = sizeof action - 1;
   memcpy(action, reply, l); action[l] = '\0'; }
 /* trim trailing spaces */
 { size_t l = strlen(action); while (l && (action[l-1]==' '||action[l-1]=='\r')) action[--l]='\0'; }
 if (strncmp(action, "TOOL ", 5)) return NULL;
 arg = strchr(action + 5, ' ');
 if (arg) { *arg = '\0'; arg++; } else arg = (char *)"";
 if (!strcmp(action + 5, "read_file"))
  return ai_read_file_bounded(arg);
 if (!strcmp(action + 5, "grep")) {
  char cmd[1300]; snprintf(cmd, sizeof cmd, "grep -rn -- %s .", arg); return ai_run_capture(cmd);
 }
 if (!strcmp(action + 5, "list_dir")) {
  char cmd[1200]; snprintf(cmd, sizeof cmd, "ls -la %s", arg[0] ? arg : "."); return ai_run_capture(cmd);
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
 ret = e_opt_kst(o);
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
 if (wf->b->mxlines <= 1)                        /* one-time hint on a fresh chat */
  ai_tr_line(wf, "[Enter=send  Ctrl-J=newline  arrows move  PgUp/PgDn scroll  Esc leaves]");
 ai_input_render(wf);                            /* creates the input region */
}

/* Handle one key while the AI chat pane is the FOCUSED window.  Returns 1 if the
 * key was an input action (consumed), 0 to let the editor handle it normally
 * (window switch, function keys, ...).  Mouse is handled by the editor before
 * this is reached, so dragging/resizing/switching windows all keep working. */
int e_ai_chat_key(FENSTER *f, int c)
{
 FENSTER *wf = f;

 if (!g_ai_chat_focus || f != g_ai_chat_pane)
  return 0;
 if (c == WPE_ESC) { e_ai_chat_close(); return 1; }
 if (c == BUP) { ai_pane_scroll(wf, -(wf->e.y - wf->a.y - 2)); return 1; }
 if (c == BDO) { ai_pane_scroll(wf, +(wf->e.y - wf->a.y - 2)); return 1; }
 g_ai_scroll_lock = 0;                           /* any edit key returns to the input */
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
 if (c == CUP)      { ai_in_vmove(-1);                             ai_input_render(wf); return 1; }
 if (c == CDO)      { ai_in_vmove(1);                              ai_input_render(wf); return 1; }
 if ((c >= 32 && c < 255) || c > WPE_AI_MENU) { /* a printable char (ASCII/Unicode) */
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
static void e_ai_apply_text(FENSTER *f, const char *newtext)
{
 BUFFER *b = f->b;
 e_add_undo('B', b, b->b.x, b->b.y, 0);
 e_buffer_set_text(b, newtext);
 if (b->b.y >= b->mxlines) b->b.y = b->mxlines ? b->mxlines - 1 : 0;
 if (b->b.y < 0) b->b.y = 0;
 if (b->b.x > b->bf[b->b.y].len) b->b.x = b->bf[b->b.y].len;
 if (b->b.x < 0) b->b.x = 0;
 f->save++;
 e_firstl(f, 1);
 e_schirm(f, 1);
 e_rep_win_tree(f->ed);
 e_refresh();
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
static char *ai_hunk_apply(FENSTER *f, wpe_ai_seg *segs, int nseg)
{
 int i, k, total = 0, hunk = 0, any = 0, all = 0, cancel = 0;
 int *acc = calloc(nseg > 0 ? nseg : 1, sizeof *acc);
 size_t cap = 1024, len = 0;
 char *out;
 if (!acc) return NULL;
 for (i = 0; i < nseg; i++) if (segs[i].is_change) total++;

 ai_pane(f, "--- proposed changes (per hunk) ---", 1);
 for (i = 0; i < nseg && !cancel; i++) {
  if (!segs[i].is_change) continue;
  hunk++;
  if (all) { acc[i] = 1; any = 1; continue; }
  { char hdr[80]; snprintf(hdr, sizeof hdr, "--- hunk %d/%d ---", hunk, total); ai_pane(f, hdr, 0); }
  for (k = 0; k < segs[i].an; k++) { char ln[540]; snprintf(ln, sizeof ln, "-%.520s", segs[i].a[k]); ai_pane(f, ln, 0); }
  for (k = 0; k < segs[i].bn; k++) { char ln[540]; snprintf(ln, sizeof ln, "+%.520s", segs[i].b[k]); ai_pane(f, ln, 0); }
  ai_pane(f, "   y=apply  n=skip  a=all  q=cancel", 0);
  for (;;) {
   int c = e_toupper(e_getch());
   if (c == 'Y' || c == 13 || c == '\r' || c == '\n') { acc[i] = 1; any = 1; break; }
   if (c == 'N') { acc[i] = 0; break; }
   if (c == 'A') { acc[i] = 1; any = 1; all = 1; break; }
   if (c == 'Q' || c == WPE_ESC) { cancel = 1; break; }
  }
 }
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

/* Cancel whatever background op is running (user asked, or a new action starts).*/
void wpe_ai_cancel(void)
{
 ai_async_op *op = g_ai_op;
 if (!op) return;
 ai_op_detach(op);
 if (ai_window_alive(op->cn, op->f))
  ai_pane(op->f, "[AI] cancelled", 0);
 ai_op_free(op);
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
 if (op->acc_len == 0) {
  ai_pane(f, "[AI edit] no response", 0);
  ai_op_free(op);
  return;
 }
 clean = ai_strip_fences(op->acc);
 if (!clean) { ai_op_free(op); return; }

 now = ai_current_file_text(f);
 nseg = wpe_ai_diff_segments(now ? now : "", clean, &segs);
 free(now);
 for (i = 0; i < nseg; i++) if (segs[i].is_change) { has_change = 1; break; }
 if (!has_change) {
  ai_pane(f, "[AI edit] no change", 0);
  wpe_ai_trace("edit no-change");
 } else {
  char *result = ai_hunk_apply(f, segs, nseg);
  if (result) {
   e_ai_apply_text(f, result);
   ai_pane(f, "[AI edit] applied - Ctrl-U to undo", 0);
   wpe_ai_trace("edit applied");
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
  return;
 }
 if (done)
  ai_edit_done(op);
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
 ai_ml_add(&op->ml, "assistant", op->acc ? op->acc : "");
 r = op->process ? op->process(op, op->acc ? op->acc : "") : 1;
 op->iter++;
 if (r != 0 || op->iter >= op->max_iter) {
  if (op->iter >= op->max_iter && r == 0 && ai_window_alive(op->cn, op->f))
   ai_pane(op->f, "[AI] stopped (max steps)", 0);
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
 if (done)
  ai_conv_on_reply(op);
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
 const char *sys =
   "You are a precise code editor. Apply the user's instruction to the file "
   "below and return ONLY the complete modified file content - no markdown "
   "fences, no commentary, no explanation.";
 wpe_ai_msg msgs[2];
 wpe_ai_req req;
 ECNT *cn = f->ed;
 int save_id = -1, wi;
 ai_async_op *op;

 if (wpe_ai_busy()) {
  ai_pane(f, "[AI] a task is already running - press Alt-G to cancel it first", 1);
  return 0;
 }
 instr[0] = '\0';
 if (!e_ai_prompt1(instr, "AI edit instruction", f) || !instr[0])
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

 cur = ai_current_file_text(f);
 { size_t n = strlen(instr) + (cur ? strlen(cur) : 0) + 64;
   user = malloc(n);
   if (user) snprintf(user, n, "%s\n\n--- file ---\n%s", instr, cur ? cur : ""); }
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
 op->fd = wpe_ai_stream_fd(op->st);
 op->start = time(NULL);
 op->spin = ai_spin_begin(f, "[AI edit] working");
 g_ai_op = op;
 g_ai_bg_win = f;
 ai_hb_acquire(ai_op_tick);
 wpe_fd_add(op->fd, POLLIN, ai_edit_fd_cb, op);
 /* Hand focus back to the file so the user keeps editing while the model works;
    the pane spins in the background (its paints leave the caret here). */
 if (save_id >= 0)
  e_switch_window(save_id, cn->f[cn->mxedt]);
 wpe_ai_trace("edit stream fd=%d (async)", op->fd);
 return 0;
}

/* ======================= model picker =================================== */

/* A navigable radio list (arrows move, Enter confirms, Esc cancels) built on the
 * standard dialog widgets -- the same interaction as the LSP pickers, so choosing
 * a model feels like every other list in the editor rather than a blind prompt.
 * Returns the chosen index in labels[0..n), or -1 if cancelled.  Mirrors the LSP
 * picker's structure; the sw ids MUST be unique and non-zero or the modal dialog
 * cannot take initial focus and would spin (see the LSP picker for the why). */
#define AI_PICK_MAXW 44
static int e_ai_pick(FENSTER *f, const char *title, const char *const *labels,
                     int n)
{
 W_OPTSTR *o;
 static char rows[16][AI_PICK_MAXW + 4];
 static char name[80];
 int i, sel = -1, vis, mxlen = 0, w, bw, bh;

 vis = n < 16 ? n : 16;
 for (i = 0; i < vis; i++) {
  snprintf(rows[i], sizeof rows[i], "%.*s", AI_PICK_MAXW, labels[i]);
  if ((int)strlen(rows[i]) > mxlen) mxlen = strlen(rows[i]);
 }
 snprintf(name, sizeof name, "%.60s", title);
 w = mxlen + 4;
 if ((int)strlen(name) + 2 > w) w = strlen(name) + 2;
 o = e_init_opt_kst(f);
 if (!o) return -1;
 bw = w + 4;
 bh = vis + 3;
 o->xa = 8;
 o->ya = 3;
 o->xe = o->xa + bw;
 o->ye = o->ya + bh;
 o->bgsw = 0;
 o->crsw = AltO;                        /* Enter on a radio confirms via Ok      */
 o->name = name;
 for (i = 0; i < vis; i++)
  e_add_pswstr(0, 3, 1 + i, -1, 10001 + i, 0, rows[i], o);
 e_add_bttstr((o->xe - o->xa - 4) / 2, o->ye - o->ya - 1, 0, AltO, "Ok", NULL, o);
 if (e_opt_kst(o) != WPE_ESC)
  sel = o->pstr[0]->num;
 freeostr(o);
 if (sel < 0 || sel >= vis) return -1;
 return sel;
}

static int e_ai_pick_model(FENSTER *f)
{
 char *names[32];
 char title[80], line[220];
 int n, i, sel;

 title[0] = '\0';
 n = wpe_ai_list_models(e_ai_backend, names, 32, title, sizeof title);
 if (n <= 0) { ai_pane(f, title[0] ? title : "no models found", 1); return 0; }
 snprintf(title, sizeof title, "Model (%d available)", n);
 sel = e_ai_pick(f, title, (const char *const *)names, n);
 if (sel >= 0) {
  free(e_ai_model);
  e_ai_model = strdup(names[sel]);
  snprintf(line, sizeof line, "[AI] model = %s (Save Options to persist)",
           names[sel]);
  ai_pane(f, line, 1);
  wpe_ai_trace("model set %s", names[sel]);
 }
 for (i = 0; i < n; i++) free(names[i]);
 return 0;
}

int e_ai_agent(FENSTER *f);         /* defined in the Agent section below */
static int e_ai_plan(FENSTER *f);   /* defined in the PLAN section below  */
static void e_ai_cycle_policy(FENSTER *f);  /* defined below e_ai_ui_key    */

/* ======================= Alt-G prefix dispatch ========================== */
int e_ai_ui_key(FENSTER *f)
{
 if (!wpe_ai_enabled()) {
  ai_pane(f, "AI assistant is off - enable it in Options > Editor "
             "(the \"Ai assistant\" box), then press Alt-G again.", 1);
  return 0;
 }
 /* A background task is running (async Edit): Alt-G cancels it rather than
    opening the menu, so there is a one-key way out and no modal stacks on top of
    the in-flight work. */
 if (wpe_ai_busy()) {
  wpe_ai_cancel();
  return 0;
 }
 /* Alt-G shows the action menu straight away -- the way Alt-F shows the File
    menu -- instead of an invisible "press another key" prefix.  The menu takes
    the item's letter as a shortcut (a=Ask e=Edit p=Plan g=Agent m=Model
    y=policY n=New d=Disable), so a quick Alt-G a still jumps straight to Ask;
    pausing just leaves the menu on screen to pick from. */
 return e_ai_menu(f);
}

/* ======================= Agent mode ==================================== */

#define AI_AGENT_MAX_ITERS 8
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
 snprintf(line, sizeof line, "[agent] APPROVE?  %s", what);
 ai_pane(f, line, 1);
 ai_pane(f, "   y = allow    n / Esc = deny", 0);
 for (;;) {
  int c = e_getch();
  if (c == WPE_ESC || e_toupper(c) == 'N') return 0;
  if (e_toupper(c) == 'Y' || c == 13 || c == '\r' || c == '\n') return 1;
 }
}

/* Alt-G y: cycle the permission dial ask -> edits -> auto.  Also settable from
 * the AI menu (radio) and persisted as AIPolicy via Save Options. */
static void e_ai_cycle_policy(FENSTER *f)
{
 char line[140];
 e_ai_policy = (e_ai_policy + 1) % 3;
 snprintf(line, sizeof line,
   "[AI] permission policy = %s   (ask -> edits -> auto; Save Options to keep)",
   wpe_ai_policy_name(e_ai_policy));
 ai_pane(f, line, 1);
 wpe_ai_trace("policy set %s", wpe_ai_policy_name(e_ai_policy));
}

/* One agent turn: parse the action, run the tool (write/run ask for approval),
 * feed the result back.  Returns 1 when the agent is done, 0 to keep going.
 * Runs from the conversation driver with the spinner paused, so its approval
 * prompt (ai_agent_approve -> e_getch) is safe. */
static int ai_agent_process(ai_async_op *op, char *reply)
{
 FENSTER *f = op->f;
 char *firstnl, action[1100];

 if (!ai_window_alive(op->cn, f)) return 1;
 firstnl = strchr(reply, '\n');
 { size_t l = firstnl ? (size_t)(firstnl - reply) : strlen(reply);
   if (l >= sizeof action) l = sizeof action - 1;
   memcpy(action, reply, l); action[l] = '\0'; }

 if (!strncmp(action, "DONE", 4)) {
  ai_pane(f, action[0] ? action : "[agent] done", 0);
  wpe_ai_trace("agent done");
  return 1;
 }
 if (strncmp(action, "TOOL ", 5)) {            /* not a tool call = final answer */
  ai_pane(f, action, 0);
  return 1;
 }
 {
  char *tool = action + 5, *arg = strchr(tool, ' '), *result = NULL, paneln[640];
  if (arg) { *arg = '\0'; arg++; } else arg = (char *)"";
  snprintf(paneln, sizeof paneln, "[agent] %s %s", tool, arg);
  ai_pane(f, paneln, 0);
  wpe_ai_trace("agent tool=%s arg=%s", tool, arg);

  if (!strcmp(tool, "list_dir")) {
   char cmd[1200]; snprintf(cmd, sizeof cmd, "ls -la %s", arg[0] ? arg : "."); result = ai_run_capture(cmd);
  } else if (!strcmp(tool, "read_file")) {
   result = ai_read_file_bounded(arg);
  } else if (!strcmp(tool, "grep")) {
   char cmd[1300]; snprintf(cmd, sizeof cmd, "grep -rn -- %s .", arg); result = ai_run_capture(cmd);
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
   { char what[720]; snprintf(what, sizeof what, "write_file %s (%zu bytes)", arg, content ? strlen(content) : 0);
     if (content && ai_agent_approve(f, what, 0)) {
      FILE *w = fopen(arg, "wb");
      if (w) { fwrite(content, 1, strlen(content), w); fclose(w); result = strdup("(written)"); }
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
}

int e_ai_agent(FENSTER *f)
{
 static char goal[AI_PROMPT_MAX];
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
   "  TOOL run_command <shell command>\n"
   "  TOOL write_file <path>\n"
   "For write_file, put the new file content on the following lines, ending "
   "with a line that is exactly @@END .\n"
   "When the task is complete, reply with a line beginning DONE and a short "
   "summary. Output nothing else; wait for each tool result before continuing.";

 if (wpe_ai_busy()) {
  ai_pane(f, "[AI] a task is already running - press Alt-G to cancel it first", 1);
  return 0;
 }
 goal[0] = '\0';
 if (!e_ai_prompt1(goal, "AI agent task", f) || !goal[0])
  return 0;
 err[0] = '\0';
 if (wpe_ai_preflight(e_ai_backend, err, sizeof err)) { ai_pane(f, err, 1); return 0; }
 err[0] = '\0';
 if (wpe_ai_ensure_model(err, sizeof err)) {
  ai_pane(f, err[0] ? err : "no model set", 1);
  return 0;
 }

 { char line[1100]; snprintf(line, sizeof line, "[agent] task: %s", goal); ai_pane(f, line, 1); }
 wpe_ai_trace("agent task=%s", goal);

 wpe_ai_trace("agent policy=%s", wpe_ai_policy_name(e_ai_policy));
 wpe_ai_session_load(f);
 /* claudecli: pre-grant per the dial (it cannot prompt in -p mode). */
 e_ai_cli_mode = e_ai_policy == WPE_AI_POLICY_AUTO  ? WPE_AI_CLI_AUTO
               : e_ai_policy == WPE_AI_POLICY_EDITS ? WPE_AI_CLI_EDITS
               : WPE_AI_CLI_TEXTONLY;
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
 { wpe_ai_msg prior[12]; int np = wpe_ai_session_messages(prior, 12), i;
   for (i = 0; i < np; i++) ai_ml_add(&op->ml, prior[i].role, prior[i].content); }
 ai_ml_add(&op->ml, "user", goal);
 wpe_ai_session_append("user", goal);
 ai_conv_start(op, "[agent] working");
 return 0;
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
 snprintf(line, sizeof line, "[plan] the AI proposes to change %d file%s:", np, np == 1 ? "" : "s");
 ai_pane(f, line, 1);
 for (i = 0; i < np; i++) {
  char *now = wpe_ai_read_scope_file(f, pd->props[i].path);
  wpe_ai_seg *segs; int ns, k, plus = 0, minus = 0;
  ns = wpe_ai_diff_segments(now ? now : "", pd->props[i].text, &segs);
  for (k = 0; k < ns; k++) if (segs[k].is_change) { plus += segs[k].bn; minus += segs[k].an; }
  wpe_ai_segs_free(segs, ns);
  snprintf(line, sizeof line, "   %s  (+%d -%d)%s", pd->props[i].path, plus, minus, now ? "" : "  [new file]");
  ai_pane(f, line, 0);
  free(now);
 }
 wpe_ai_trace("plan proposals=%d", np);
 ai_pane(f, "   a = apply all    f = review file by file    q = cancel", 0);
 {
  int mode = 0;
  for (;;) {
   int c = e_toupper(e_getch());
   if (c == 'A') { mode = 1; break; }
   if (c == 'F' || c == 13 || c == '\r' || c == '\n') { mode = 2; break; }
   if (c == 'Q' || c == WPE_ESC) { mode = 0; break; }
  }
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
      char *result = ai_hunk_apply(w, segs, ns);
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
  ai_pane(f, "[AI] a task is already running - press Alt-G to cancel it first", 1);
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

/* ======================= Bottom-bar action menu ========================= */
/* The "Alt-G AI" entry on the editor's bottom bar (mouse-clickable) and any
 * unrecognised Alt-G letter open this popup so every AI action is discoverable
 * without memorising the prefix letters -- the same role e_lsp_ui_menu plays
 * for the language server. */

#define AI_MENU_TEXTW 25

/* Cycle the permission dial (ask -> edits -> auto) from the menu. */
static int e_ai_menu_policy(FENSTER *f)
{
 e_ai_cycle_policy(f);
 return 0;
}

/* Forget the workspace conversation so the next request starts fresh. */
static int e_ai_menu_new_session(FENSTER *f)
{
 wpe_ai_session_reset(f);
 ai_pane(f, "[AI] session reset for this workspace", 1);
 return 0;
}

/* Turn the assistant off (clears the runtime ED_AI_ENABLE toggle).  The bar
 * loses its "Alt-G AI" entry the next time this window is drawn; Options >
 * Editor turns it back on. */
static int e_ai_menu_disable(FENSTER *f)
{
 if (WpeEditor)
  WpeEditor->edopt &= ~ED_AI_ENABLE;
 ai_pane(f, "[AI] assistant disabled - re-enable it in Options > Editor", 1);
 return 0;
}

/* Fill `it` with the menu rows (name left, "Alt-G <key>" right-aligned so the
 * keyboard shortcut lines up like the LSP menu).  Returns the row count. */
static int e_ai_menu_items(OPTK *it)
{
 static char label[8][AI_MENU_TEXTW + 4];
 static const struct { const char *name; char key; int (*fkt)(FENSTER *); } a[] = {
  { "Ask (chat)",        'A', e_ai_chat            },
  { "Edit current file", 'E', e_ai_edit            },
  { "Plan (multi-file)", 'P', e_ai_plan            },
  { "Agent (tools)",     'G', e_ai_agent           },
  { "Pick model",        'M', e_ai_pick_model      },
  { "Policy dial",       'Y', e_ai_menu_policy     },
  { "New session",       'N', e_ai_menu_new_session},
  { "Disable",           'D', e_ai_menu_disable    }
 };
 int i, n = (int)(sizeof(a) / sizeof(a[0]));

 for (i = 0; i < n; i++)
 {
  char code[12];
  int pad, hl;
  snprintf(code, sizeof code, "Alt-G %c", a[i].key);            /* 7 chars */
  pad = AI_MENU_TEXTW - (int)strlen(a[i].name) - (int)strlen(code);
  if (pad < 1)
   pad = 1;
  snprintf(label[i], sizeof label[i], "%s%*s%s", a[i].name, pad, "", code);
  hl = (int)strlen(label[i]) - 1;                /* the letter in "Alt-G X" */
  it[i] = WpeFillSubmenuItem(label[i], hl, a[i].key, a[i].fkt);
 }
 return n;
}

int e_ai_menu(FENSTER *f)
{
 OPTK items[8];
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
 WpeHandleSubmenu(xa, ya, xe, ye, 0, items, f);
 return 0;
}

#endif /* WPE_AI */

typedef int wpe_ai_ui_translation_unit;
