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

/* A UTF-8 continuation byte is 10xxxxxx: it adds to the character before it
   and takes no column of its own. */
#define AI_UTF8_CONT_MASK 0xC0
#define AI_UTF8_CONT_BITS 0x80

/**
 * ai_utf8_continues - Is byte @c the continuation of a multibyte character?
 *
 * The pane counts columns and the input row steps the caret by this, so an
 * accented or CJK character wraps, moves and deletes as one character and
 * never leaves half of itself behind.
 */
static int ai_utf8_continues(char c)
{
 return ((unsigned char)c & AI_UTF8_CONT_MASK) == AI_UTF8_CONT_BITS;
}

/* ----- one active chat at a time (MVP) ----------------------------------- */
struct ai_async_op;                 /* the background task a chat turn runs as */
typedef struct {
 wpe_ai_stream *st;
 FENSTER       *ref;           /* anchor window (gives us cn = ref->ed)        */
 struct ai_async_op *op;       /* the task that makes this turn "busy": Esc
                                  cancels it, a second question queues behind
                                  it, the heartbeat spins its waiting line     */
 char          *pending;       /* incomplete trailing line being streamed      */
 size_t         plen, pcap;
 int            pcols;         /* display columns in `pending` (for word-wrap)   */
 char          *full;          /* whole reply so far (for the session log)     */
 size_t         flen, fcap;
 int            fd;
 int            active;
 int            started;       /* a fresh reply line has been opened under "AI:" */
 int            turns;         /* read-only tool turns taken before answering    */
 int            decided;       /* what this turn's reply is (enum ai_reply_kind) */
 int            hide_tool;     /* a TOOL line begun after a preamble is being
                                  hidden: show its dim status, not the raw line   */
 int            fell_back;     /* this chat already retried with the fallback model */
 size_t         shown;         /* bytes of `full` already painted (decided==1)   */
 int            web_declined;  /* the user answered the web notice with "not now":
                                  this question does not ask again              */
 char          *sources;       /* the URLs this question's web tools read, for
                                  the "sources:" line that ends the answer     */
 size_t         srclen, srccap;
} ai_chat_session;

#define AI_CHAT_MAX_TOOL_TURNS 6   /* cap chat's read-only investigation loop */
#define AI_REPLY_PREFIX "AI: "     /* speaker label kept on the reply's first line */
/* The waiting line of a chat turn.  The shared heartbeat appends the spinner
   glyph, the seconds and "(Esc cancels)" to it while nothing has been painted
   in its place yet; the text itself stays what the tests look for. */
#define AI_CHAT_WAIT_LABEL AI_REPLY_PREFIX "(gathering the answer...)"
/* The chat's own word for a peer that hung up without saying anything. */
#define AI_CHAT_TRANSPORT_LINE "[AI: transport error]"

/* What the painter has made of this turn's reply so far. */
enum ai_reply_kind {
 AI_REPLY_UNDECIDED = 0,  /* too few bytes to tell yet                        */
 AI_REPLY_ANSWER,         /* an answer: stream it                             */
 AI_REPLY_TOOL            /* a TOOL investigation: shown as a dim status, the
                             raw protocol line is NOT echoed as "AI:"         */
};
/* The protocol line a model opens a tool call with, and the room the dim
   status line that stands in for it gets. */
#define AI_TOOL_PREFIX     "TOOL "
#define AI_TOOL_PREFIX_LEN (sizeof AI_TOOL_PREFIX - 1)
#define AI_TOOL_STATUS_MAX 640
/* The session-log line a tool's output is fed back under. */
#define AI_TOOL_RESULT_HEAD "TOOL RESULT:\n"
/* First sizes of the reply buffers; each doubles when it fills. */
#define AI_FULL_FIRST    512
#define AI_PENDING_FIRST 256
/* Room for a one-line note that quotes the server's error text. */
#define AI_CHAT_NOTE_MAX 440

static ai_chat_session *g_ai_chat = NULL;

/* The line every entry point writes when the assistant is switched off: the
   box that turns it on lives in Options > AI. */
#define AI_OFF_LINE "AI assistant is off - enable it in Options > AI " \
                    "(the \"Enable AI assistant\" box), then press Alt-G again."

/* ----- who wrote each pane line ----------------------------------------------
 * The pane tells who wrote a line by its colour, from the colour scheme, with
 * the prefix ("You:", "AI:", "[agent]", ">> ") as the second cue.  The code
 * that writes a line names its role, and the role is kept per line here, so
 * the painter never guesses from the text and the colours survive every
 * repaint under every backend.  The assistant's own text paints in the editor
 * text colour and needs no entry of its own. */
enum ai_role {
 AI_ROLE_ASSISTANT = 0,  /* "AI: ..." and the reply: the editor text entry   */
 AI_ROLE_USER,           /* "You: ..." and its continuation rows             */
 AI_ROLE_MACHINERY,      /* "[agent] ...", "[AI edit] ...", "[AI] ...", headers */
 AI_ROLE_ATTENTION,      /* ">> ...": a key is awaited                        */
 AI_ROLE_ERROR           /* "AI: error: ...", transport and backend errors   */
};

/* One byte per pane row: the writer's role in the low bits and, above them,
 * whether the row CONTINUES the previous row's logical line.  The transcript
 * is the list of logical lines; the buffer rows are its rendering at the
 * pane's current width, so a row split off by the wrapper is marked here and
 * can be joined back when the width changes (see ai_pane_reflow). */
#define AI_ROW_ROLE_MASK  0x0F
#define AI_ROW_CONTINUES  0x10
static unsigned char *g_ai_roles = NULL;   /* one role per pane line          */
static int g_ai_roles_n = 0;               /* lines the table covers          */
static int g_ai_roles_cap = 0;
/* The width the transcript rows were last laid out at; the paint re-wraps
 * them when the pane's width differs (a build opened Messages beside the
 * pane, a resize, Messages closed again). */
#define AI_ROWS_WIDTH_UNKNOWN 0
static int g_ai_rows_width = AI_ROWS_WIDTH_UNKNOWN;
/* The stream line was left at the old width because a task was rewriting it
 * in place; it is re-wrapped at the first paint after the task ends. */
static int g_ai_rows_live_stale = 0;
/* A row longer than the width was written whole (a final status on the
 * stream row, a header): the next paint with no task running wraps it. */
static int g_ai_rows_unwrapped = 0;

/**
 * ai_roles_reserve - Make room for `n` roles.  Return: 1 on success.
 */
static int ai_roles_reserve(int n)
{
 unsigned char *grown;
 int cap = g_ai_roles_cap ? g_ai_roles_cap : 64;
 if (n <= g_ai_roles_cap) return 1;
 while (cap < n) cap *= 2;
 grown = realloc(g_ai_roles, (size_t)cap);
 if (!grown) return 0;
 g_ai_roles = grown;
 g_ai_roles_cap = cap;
 return 1;
}

/**
 * ai_roles_sync - Make the table as long as the pane buffer.
 * @b: the pane buffer.
 *
 * Lines the table does not know (rows added at the end: the input row, a
 * fresh buffer's first line) count as the assistant's; rows removed at the
 * end drop their role.  Every writer calls this after it changed the buffer,
 * so the table and the buffer never disagree for long.
 */
static void ai_roles_sync(BUFFER *b)
{
 int n = b->mxlines;
 if (n < 0) n = 0;
 if (!ai_roles_reserve(n)) return;
 while (g_ai_roles_n < n)
  g_ai_roles[g_ai_roles_n++] = AI_ROLE_ASSISTANT;
 g_ai_roles_n = n;
}

/**
 * ai_roles_insert - A line was inserted at `at`: record who wrote it.
 * @b: the pane buffer, already holding the new line.
 * @at: the index of the new line.
 * @role: the writer.
 */
static void ai_roles_insert(BUFFER *b, int at, int role)
{
 int before = b->mxlines - 1;
 if (before < 0) before = 0;
 if (!ai_roles_reserve(b->mxlines)) return;
 while (g_ai_roles_n < before)
  g_ai_roles[g_ai_roles_n++] = AI_ROLE_ASSISTANT;
 g_ai_roles_n = before;
 if (at < 0) at = 0;
 if (at > g_ai_roles_n) at = g_ai_roles_n;
 memmove(g_ai_roles + at + 1, g_ai_roles + at, (size_t)(g_ai_roles_n - at));
 g_ai_roles[at] = (unsigned char)role;
 g_ai_roles_n++;
}

/**
 * ai_roles_set - Change who wrote line `y` (a line rewritten in place).
 */
static void ai_roles_set(BUFFER *b, int y, int role)
{
 ai_roles_sync(b);
 if (y >= 0 && y < g_ai_roles_n)
  g_ai_roles[y] = (unsigned char)((g_ai_roles[y] & ~AI_ROW_ROLE_MASK) | role);
}

/**
 * ai_row_continues - Does row `y` continue the previous row's logical line?
 *
 * A row the wrapper split off a long line says so here, so a narrowed or
 * widened pane can join the line back and wrap it again, and a copy of the
 * rows yields the one line the model wrote.
 */
static int ai_row_continues(int y)
{
 if (y <= 0 || y >= g_ai_roles_n) return 0;
 return (g_ai_roles[y] & AI_ROW_CONTINUES) != 0;
}

/**
 * ai_pane_row_continues_below - Row `y` goes on in the row under it.
 * @b: the pane buffer.
 * @y: the row that was cut by the wrapper.
 *
 * The row loses its line-end marker, the way the editor stores its own
 * soft-wrapped long lines, so the block copy and the OS clipboard join it
 * with the next row instead of handing the user two lines for one.
 */
static void ai_pane_row_continues_below(BUFFER *b, int y)
{
 if (y < 0 || y >= b->mxlines || !b->bf[y].s) return;
 b->bf[y].s[b->bf[y].len] = '\0';
 b->bf[y].nrc = b->bf[y].len;
}

/**
 * ai_pane_mark_continuation - Row `y` continues the row above it.
 * @b: the pane buffer.
 * @y: the row the wrapper split off.
 *
 * Both halves of the record are set together: the flag the reflow reads and
 * the missing line-end marker the copy path reads, so the two never disagree.
 */
static void ai_pane_mark_continuation(BUFFER *b, int y)
{
 ai_roles_sync(b);
 if (y <= 0 || y >= g_ai_roles_n) return;
 g_ai_roles[y] |= AI_ROW_CONTINUES;
 ai_pane_row_continues_below(b, y - 1);
}

/**
 * ai_roles_set_appended - Lines were appended from index `from` on: record
 * their writer.
 */
static void ai_roles_set_appended(BUFFER *b, int from, int role)
{
 int y;
 ai_roles_sync(b);
 for (y = from; y < g_ai_roles_n; y++)
  g_ai_roles[y] = (unsigned char)role;
}

/**
 * ai_roles_forget - The pane is gone: so is every role.
 */
static void ai_roles_forget(void)
{
 g_ai_roles_n = 0;
 g_ai_rows_width = AI_ROWS_WIDTH_UNKNOWN;
 g_ai_rows_live_stale = 0;
 g_ai_rows_unwrapped = 0;
}

/**
 * ai_pane_note_unwrapped - A row was written whole, without the wrapper.
 * @text: the row's text.
 *
 * The stream row's final texts (a tool status, "reply ready", an error) and
 * the lines written straight into the transcript do not pass the wrapper.
 * When such a row is wider than the pane it is noted here, so the next paint
 * with no task rewriting the row wraps it like every other line, and the
 * user reads it whole.
 */
static void ai_pane_note_unwrapped(const char *text)
{
 if ((int)strlen(text) > g_ai_rows_width)
  g_ai_rows_unwrapped = 1;
}

/**
 * ai_role_of - Who wrote line `y`; the assistant when unknown.
 */
static int ai_role_of(int y)
{
 if (y < 0 || y >= g_ai_roles_n) return AI_ROLE_ASSISTANT;
 return g_ai_roles[y] & AI_ROW_ROLE_MASK;
}

/**
 * ai_is_pane - Is this window the AI pane?
 */
static int ai_is_pane(const FENSTER *f)
{
 return f && f->datnam && !strcmp(f->datnam, AI_PANE_NAME);
}

/**
 * ai_role_attr - The scheme colour a role paints in.
 * @wf: the pane (its colour scheme).
 * @role: the writer.
 * @base: the editor text colour, for the assistant.
 *
 * The scheme entries are the user's to change in Options > Colors or in
 * xwperc; a monochrome scheme holds attribute bits in the same slots.
 */
static int ai_role_attr(const FENSTER *wf, int role, int base)
{
 if (role == AI_ROLE_USER) return wf->fb->aiu.fb;
 if (role == AI_ROLE_MACHINERY) return wf->fb->aim.fb;
 if (role == AI_ROLE_ATTENTION) return wf->fb->aia.fb;
 if (role == AI_ROLE_ERROR) return wf->fb->aie.fb;
 return base;
}

/**
 * wpe_ai_pane_line_attr - The colour the row painter paints line `y` in.
 * @f: the window being painted.
 * @y: the buffer line.
 * @base: the editor text colour.
 *
 * Called from the shared row painter for every window, so the pane's roles
 * show on the terminal, X11 and Wayland alike and on every repaint (a menu
 * closing, a window switch, a resize); other windows get `base` back.
 */
int wpe_ai_pane_line_attr(FENSTER *f, int y, int base)
{
 if (!ai_is_pane(f)) return base;
 return ai_role_attr(f, ai_role_of(y), base);
}

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
/* The input region's prefix: "> " on its first row, blanks of the same width
   under it, so a multi-line prompt aligns under its first line. */
#define AI_INPUT_PROMPT     "> "
#define AI_INPUT_INDENT     "  "
#define AI_INPUT_PREFIX_LEN 2

/**
 * ai_stream_y - The pane row the reply streams into.
 * @wf: the pane.
 *
 * The last row normally, or the row just above the input region when the
 * chat owns the bottom rows: the reply grows above the "> " row, never
 * over it.
 */
static int ai_stream_y(FENSTER *wf)
{
 int y = wf->b->mxlines - 1;
 if (g_ai_chat_focus) y -= g_ai_input_rows;
 if (y < 0) y = 0;
 return y;
}

/**
 * ai_chat_buf_reserve - Make @buf hold @need bytes, doubling from @first.
 * @cap: the buffer's present size, updated.
 *
 * Return: 1 when the room is there, 0 when memory ran out and the buffer is
 * as it was.  The reply text and the stream line both grow through here, so
 * a long answer never stops short of the pane for want of room.
 */
static int ai_chat_buf_reserve(char **buf, size_t *cap, size_t need, size_t first)
{
 size_t nc = *cap;
 char *nb;
 if (need <= nc)
  return 1;
 if (nc == 0)
  nc = first;
 while (need > nc)
  nc *= 2;
 nb = realloc(*buf, nc);
 if (!nb)
  return 0;
 *buf = nb;
 *cap = nc;
 return 1;
}

/**
 * ai_chat_set_pending - Seed the stream line with @seed.
 * @s: the chat starting a turn.
 *
 * The reply keeps its "AI: " speaker label on its first line while tokens
 * append after it, instead of the label sitting alone on a line above the
 * answer.
 */
static void ai_chat_set_pending(ai_chat_session *s, const char *seed)
{
 size_t n = strlen(seed);
 if (!ai_chat_buf_reserve(&s->pending, &s->pcap, n + 1, AI_PENDING_FIRST))
  return;
 memcpy(s->pending, seed, n + 1);
 s->plen = n;
 s->pcols = (int)n;              /* the "AI: " seed is plain ASCII */
}

/* read-only tool helpers (defined in the Agent section) + the turn starter */
static char *ai_run_capture(const char *cmd);
static char *ai_read_file_bounded(const char *path);
static void  ai_chat_next_turn(ai_chat_session *s);
static char *ai_chat_tool_result(ai_chat_session *s, const char *reply);
/* The chat's web tools and its refusals live with the agent's web tools,
   which they call; the sources line ends an answer that used them. */
static char *ai_chat_run_web_tool(ai_chat_session *s, const char *tool, const char *arg);
static char *ai_chat_refuse_tool(FENSTER *f, const char *tool, const char *lead,
                                 const char *why);
static void ai_chat_show_sources(ai_chat_session *s);
static FENSTER *ai_pane_win(FENSTER *f);
static void  ai_tr_line(FENSTER *wf, const char *str, int role);
static void  ai_pane_paint(FENSTER *wf);
static void  ai_pane_set_line(FENSTER *wf, int y, const char *text);

static int ai_pane_width(FENSTER *wf);          /* defined below */
static void ai_pane_reflow(FENSTER *wf);        /* defined below */
/* Columns the pane wraps reply text to, at most: a readable prose measure even
 * when the pane is much wider (long unbroken lines of prose are hard to read --
 * editors and chat UIs cap the measure rather than fill the whole width). */
#define AI_PANE_MAX_COLS 80

/* The chat turn as a background task (defined with the task machinery below,
   which its functions need the shape of): registration, the waiting line the
   heartbeat spins, the end of a turn, and the queue a second request joins. */
static void ai_chat_op_begin(ai_chat_session *s);
static void ai_chat_turn_over(ai_chat_session *s);
static void ai_chat_wait_begin(ai_chat_session *s);
static void ai_chat_wait_end(ai_chat_session *s);
static void ai_enqueue(FENSTER *f, void (*launch)(FENSTER *, const char *),
                       const char *goal);

/**
 * ai_pane_show - Repaint the pane after its lines or roles changed.
 * @wf: the pane.
 *
 * The buffer writer painted the new line before its role was recorded; this
 * paints it again in the role's colour, so the user never sees a line change
 * colour on the next keystroke.
 */
static void ai_pane_show(FENSTER *wf)
{
 e_schirm(wf, 0);
 e_refresh();
}

/**
 * ai_pane_row_is_free - Is row @y the pane's untouched opening row?
 * @b: the pane buffer; @y: the row.
 *
 * A fresh window's buffer holds one empty line.  The first line the pane
 * writes takes it, so the pane never opens with a blank row above its first
 * line; every later line is appended as before.
 */
static int ai_pane_row_is_free(BUFFER *b, int y)
{
 return b->mxlines == 1 && y == 0 && b->bf[0].len == 0;
}

/**
 * ai_pane_take_first_row - Let the first line written take the pane's one
 * empty row instead of landing below it.
 * @wf: the pane.
 *
 * A fresh window's buffer holds one empty line, and every writer appended
 * AFTER it, so the pane opened with a blank row above its first line: the
 * header in a chat, the "[AI edit] working" line in an edit.  In a pane that
 * shares the bottom strip with Messages and has ten rows, that row is a tenth
 * of what the user can read.  When the buffer still holds exactly that one
 * empty line, the line is written onto it instead of after it, so the pane's
 * first row is the first line written into it.
 */
static int ai_pane_take_first_row(FENSTER *wf, const char *line, int role)
{
 BUFFER *b = wf->b;
 if (!ai_pane_row_is_free(b, 0))
  return 0;
 ai_pane_set_line(wf, 0, line);
 ai_roles_set(b, 0, role);
 return 1;
}

/**
 * ai_pane_put1 - Append ONE ready line (no wrapping) to the pane as `role`.
 * @f: the window the pane belongs to.
 * @line: the text.
 * @role: who wrote it (enum ai_role).
 * @surface: raise the pane as well.
 *
 * Into the transcript above the input row in chat-focus, otherwise to the end
 * of the docked pane; either way the line is recorded with its writer so it
 * paints in that writer's colour from the first paint on.  The pane's one
 * empty opening row is taken by the first line instead of staying blank above
 * it.
 */
static void ai_pane_put1(FENSTER *f, const char *line, int role, int surface)
{
 FENSTER *wf;
 int before;

 wf = ai_pane_win(f);
 if (!wf) return;

 if (g_ai_chat_focus) {                 /* keep the "> " input row at the bottom */
  if (!ai_pane_take_first_row(wf, line, role))
   ai_tr_line(wf, line, role);
  ai_pane_paint(wf);
  return;
 }
 /* Ensure the AI pane is created AND docked at the bottom (beside Messages)
    before appending -- the same placement as the chat pane.  Without this the
    Edit/Plan/Agent output opened as a full window over the file being edited. */
 if (!ai_pane_take_first_row(wf, line, role)) {
  before = wf->b->mxlines;
  e_d_p_named(AI_PANE_NAME, (char *)line, f, surface ? 1 : 0);
  ai_roles_set_appended(wf->b, before, role);
 }
 ai_pane_show(wf);
}

/**
 * ai_pane_last_transcript_row - The row the last transcript write landed on.
 * @wf: the pane.
 *
 * The last row above the input region in chat-focus, the last row of the
 * buffer otherwise: where ai_pane_put1 has just written.
 */
static int ai_pane_last_transcript_row(FENSTER *wf)
{
 int y = wf->b->mxlines - 1;
 if (g_ai_chat_focus) y -= g_ai_input_rows;
 if (y < 0) y = 0;
 return y;
}

/**
 * ai_pane_put_continuation - Append the rest of a wrapped line as `role`.
 * @f: the window the pane belongs to.
 * @seg: the next row of the line.
 * @role: who wrote it (enum ai_role).
 *
 * The row is recorded as continuing the row above it, so a width change can
 * join the line back together and a copy of the rows yields one line.
 */
static void ai_pane_put_continuation(FENSTER *f, const char *seg, int role)
{
 FENSTER *wf;
 ai_pane_put1(f, seg, role, 0);
 wf = ai_pane_win(f);
 if (!wf) return;
 ai_pane_mark_continuation(wf->b, ai_pane_last_transcript_row(wf));
}

/* The largest row the wrapper hands the pane: the width is capped well below
   this, the cap only bounds the copy. */
#define AI_WRAP_SEG_MAX 1200

/**
 * ai_wrap_cut - Where the next row of `p` ends when wrapped at `width` columns.
 * @p: the text still to be placed.
 * @width: the columns a row may use.
 * @cut: out, bytes of `p` that make the next row.
 *
 * Return: 0 when all of `p` fits on one row (`cut` is then its length), else
 * 1 and `cut` is the row: up to and including the last space that fits (the
 * space stays on the row, so the rows joined again read exactly as written),
 * or a hard cut at the width inside a word longer than the row.  Column
 * counting skips UTF-8 continuation bytes so multibyte text wraps by
 * character.  Every row of the pane goes through here, the first write and
 * every re-wrap, so a line always breaks the same way at a given width.
 */
static int ai_wrap_cut(const char *p, int width, int *cut)
{
 int col = 0, i = 0, lastspace = -1;
 while (p[i] && col < width) {                  /* consume up to `width` columns */
  if (p[i] == ' ') lastspace = i;
  if (!ai_utf8_continues(p[i])) col++;
  i++;
 }
 while (p[i] && ai_utf8_continues(p[i])) i++;   /* trailing cont bytes */
 if (!p[i]) { *cut = i; return 0; }             /* the rest fits */
 if (p[i] == ' ') { *cut = i + 1; return 1; }   /* the break space hangs past the margin */
 if (lastspace > 0) { *cut = lastspace + 1; return 1; }   /* break after a space */
 *cut = i;                                      /* a hard cut inside a long word */
 return 1;
}

/**
 * ai_pane - Append `line` to the pane as `role`, word-wrapped to the pane's
 * current width.
 * @f: the window the pane belongs to.
 * @line: the text.
 * @role: who wrote it (enum ai_role); every row of a wrapped line keeps it.
 * @surface: raise the pane as well.
 *
 * A long answer or summary reads DOWN the window instead of running off to the
 * right -- the same width the streamed chat reply wraps to, applied to every
 * pane message.  Display-only wrapping of our own transcript window; it never
 * touches a source file.  The rows after the first are recorded as the
 * line's continuations, so the line survives a later change of width whole.
 */
static void ai_pane(FENSTER *f, const char *line, int role, int surface)
{
 FENSTER *wf = ai_pane_win(f);
 int width = AI_PANE_MAX_COLS;
 const char *p = line;
 int first = 1;

 if (wf) {
  ai_pane_reflow(wf);          /* the rows above at the width this line gets */
  width = ai_pane_width(wf);
 }
 if (!line || !*line) { ai_pane_put1(f, "", role, surface); return; }
 while (*p) {
  int cut;
  char seg[AI_WRAP_SEG_MAX];
  if (!ai_wrap_cut(p, width, &cut)) {
   if (first) ai_pane_put1(f, p, role, surface);
   else       ai_pane_put_continuation(f, p, role);
   break;
  }
  if (cut >= (int)sizeof seg) cut = (int)sizeof seg - 1;
  memcpy(seg, p, (size_t)cut); seg[cut] = '\0';
  if (first) ai_pane_put1(f, seg, role, surface);
  else       ai_pane_put_continuation(f, seg, role);
  p += cut;
  first = 0;
 }
}

/* Lines that ASK the user to act (approve a step, the first-use notice, "type a
   follow-up") carry this marker as the second cue beside their colour, so a
   prompt never blends into the agent's ordinary output. */
#define AI_ATTN_MARK    ">> "

/**
 * ai_pane_attn - Add an attention line (marked, in the attention colour).
 * @f: the window the pane belongs to.
 * @text: the request, without the marker.
 */
static void ai_pane_attn(FENSTER *f, const char *text)
{
 char buf[1300];
 snprintf(buf, sizeof buf, "%s%s", AI_ATTN_MARK, text);
 ai_pane(f, buf, AI_ROLE_ATTENTION, 1);
}

/**
 * ai_pane_multiline - Append a possibly multi-line string as `role`, one pane
 * line per '\n', so a multi-line answer or summary is shown in full instead
 * of only its first line.
 */
static void ai_pane_multiline(FENSTER *f, const char *text, int role, int surface)
{
 const char *p = text;
 if (!p) return;
 while (*p) {
  const char *nl = strchr(p, '\n');
  size_t l = nl ? (size_t)(nl - p) : strlen(p);
  char buf[1024];
  if (l >= sizeof buf) l = sizeof buf - 1;
  memcpy(buf, p, l); buf[l] = '\0';
  ai_pane(f, buf, role, surface);
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
 ai_pane(f, line, AI_ROLE_MACHINERY, 0);
}

/**
 * ai_chat_finish - Free a chat session and its stream.
 * @s: the session, or NULL.
 *
 * The stream's descriptor leaves the fd loop first, so a late byte from the
 * server can never reach a freed session.
 */
static void ai_chat_finish(ai_chat_session *s)
{
 if (!s) return;
 wpe_ai_trace("chat finish begin fd=%d", s->fd);
 if (s->fd >= 0) wpe_fd_del(s->fd);
 wpe_ai_stream_free(s->st);
 free(s->pending);
 free(s->full);
 free(s->sources);
 if (g_ai_chat == s) g_ai_chat = NULL;
 free(s);
 wpe_ai_trace("chat finish end");
}

/**
 * ai_pane_win - The "AI" output pane, opened and docked when it is missing.
 * @f: the window the request came from.
 *
 * Every writer paints straight into the pane's buffer, so a reply streams
 * token by token instead of appearing a whole line at a time.  Return: the
 * pane, or NULL when a window could not be opened.
 */
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

/**
 * ai_top_window - The window on top of the desktop: the one the keys go to.
 * @f: any window of the same desktop.
 */
static FENSTER *ai_top_window(FENSTER *f)
{
 return f->ed->f[f->ed->mxedt];
}

/**
 * ai_pane_view_rows - Rows of the pane that show buffer lines.
 * @wf: the pane.
 *
 * The frame takes the rest.  The input row is pinned by this count, so it
 * sits on the last visible row whatever height the pane has.
 */
static int ai_pane_view_rows(FENSTER *wf)
{
 return wf->e.y - wf->a.y - 1;
}

/* Rows a page step keeps on screen, so the reader does not lose the thread
   between one page of the transcript and the next. */
#define AI_PAGE_KEEP_ROWS 1

/**
 * ai_pane_page_rows - Lines PgUp and PgDn browse at a time.
 * @wf: the pane.
 */
static int ai_pane_page_rows(FENSTER *wf)
{
 return ai_pane_view_rows(wf) - AI_PAGE_KEEP_ROWS;
}

/**
 * ai_pane_bottom_offset - The view offset that puts the last line on the last row.
 * @wf: the pane.
 *
 * Zero while the transcript is shorter than the pane.  At this offset the
 * input row and the newest reply line are both on screen.
 */
static int ai_pane_bottom_offset(FENSTER *wf)
{
 int bottom = wf->b->mxlines - ai_pane_view_rows(wf);
 if (bottom < 0)
  return 0;
 return bottom;
}

/**
 * ai_input_caret - Where the pane caret goes while the user types.
 * @wf: the pane.
 * @cy: out, the buffer row within the input region.
 * @cx: out, the byte offset in that row, past the "> " prefix.
 *
 * The caret x is a BYTE offset into the rendered line: e_cursor walks the
 * line bytes and converts a multi-byte glyph to its single display column
 * itself (e_utf8_visual_step).  So every byte on the current line counts
 * here, NOT one per glyph: a glyph count would drop the caret onto the
 * trailing byte of an accented character (the cursor "sticks" on the
 * e-acute).
 */
static void ai_input_caret(FENSTER *wf, int *cy, int *cx)
{
 int rows = g_ai_input_rows;
 int line_idx = 0, bcol = 0, i;
 if (rows <= 0)
  rows = 1;
 for (i = 0; i < g_ai_input_pos && i < g_ai_input_len; i++) {
  if (g_ai_input[i] == '\n') {
   line_idx++;
   bcol = 0;
  } else {
   bcol++;
  }
 }
 *cy = wf->b->mxlines - rows + line_idx;
 *cx = AI_INPUT_PREFIX_LEN + bcol;
}

/**
 * ai_pane_keeps_browsed_view - Is the transcript held where the user scrolled it?
 *
 * True from a PgUp, arrow or wheel step away from the bottom until a key
 * snaps the view back (or the user scrolls down to the input row again).
 * The hold does not depend on which window is focused: the user who paged
 * two pages up and then clicked into the file keeps reading the same lines
 * while the reply lands below them.
 */
static int ai_pane_keeps_browsed_view(void)
{
 return g_ai_scroll_lock != 0;
}

/**
 * ai_pane_hold_browsed_view - Leave the view offset alone and park the caret.
 * @wf: the pane.
 * @pane_focused: the pane is the focused window.
 *
 * The caret rests on the top visible line, so neither a focused window's own
 * scroll-to-caret (e_cursor) nor a later switch back to the pane drags the
 * view to the bottom: the lines the user was reading stay on screen while
 * new transcript lines are added below them.
 */
static void ai_pane_hold_browsed_view(FENSTER *wf, int pane_focused)
{
 wf->b->b.y = wf->s->c.y;
 wf->b->b.x = 0;
 if (pane_focused)
  e_cursor(wf, 0);
}

/**
 * ai_pane_is_focused - Does the caret belong to the pane right now?
 * @wf: the pane.
 *
 * Only while the pane is the focused window and no background task holds
 * the caret in the user's own window.
 */
static int ai_pane_is_focused(FENSTER *wf)
{
 return ai_top_window(wf) == wf && !g_ai_bg_win;
}

/**
 * ai_pane_caret_to_input - Put the caret where the user is typing.
 * @wf: the pane, focused with the chat armed.
 */
static void ai_pane_caret_to_input(FENSTER *wf)
{
 int cy, cx;
 ai_input_caret(wf, &cy, &cx);
 wf->b->b.y = cy;
 wf->b->b.x = cx;
}

/**
 * ai_pane_caret_to_end - Put the caret after the last line written.
 * @wf: the pane, focused with no chat armed.
 *
 * The pane is then an ordinary output window and the caret rests where the
 * next line would go, as it does in Messages.
 */
static void ai_pane_caret_to_end(FENSTER *wf)
{
 int y = wf->b->mxlines - 1;
 wf->b->b.y = y;
 wf->b->b.x = 0;
 if (y >= 0 && wf->b->bf[y].s)
  wf->b->b.x = wf->b->bf[y].len;
}

/**
 * ai_pane_pin_input_row - Scroll so the input row is the last row on screen.
 * @wf: the pane with the chat armed.
 *
 * Focused or not, the pane shows the input row and the newest reply line
 * above it; a line added while the user works in the file shows at once.
 */
static void ai_pane_pin_input_row(FENSTER *wf)
{
 wf->s->c.y = ai_pane_bottom_offset(wf);
}

/**
 * ai_pane_place_view - Put the caret and the view where the user expects them.
 * @wf: the pane.
 * @pane_focused: the pane is the focused window.
 *
 * The browsed view holds where the user scrolled it; otherwise the input row
 * (or the last line) is pinned to the bottom, so a new line always shows.
 * Shared by the pane's own paint and the repaint after a width change, so
 * the view is placed the same way whichever repainted the pane.
 */
static void ai_pane_place_view(FENSTER *wf, int pane_focused)
{
 if (ai_pane_keeps_browsed_view()) {
  /* Browsing the transcript, focused or not: the caret rests on a history
     line and the view follows it.  Do NOT move the caret back to the input or
     re-pin the view -- a focused window always scrolls to keep its caret
     visible (e_cursor), so touching either here is exactly what would undo
     the browse. */
  ai_pane_hold_browsed_view(wf, pane_focused);
  return;
 }
 if (pane_focused && g_ai_chat_focus)
  ai_pane_caret_to_input(wf);
 else if (pane_focused)
  ai_pane_caret_to_end(wf);
 if (g_ai_chat_focus)
  ai_pane_pin_input_row(wf);
 else
  e_messages_scroll_to_bottom(wf);
}

/**
 * ai_caret_window - The window whose caret is shown after a pane repaint.
 * @act: the window on top.
 *
 * While a background task runs the caret stays in the user's own window, so
 * they keep typing in their code while the reply streams or the spinner
 * turns; otherwise it is the focused window's.
 */
static FENSTER *ai_caret_window(FENSTER *act)
{
 if (g_ai_bg_win)
  return g_ai_bg_win;
 return act;
}

/**
 * ai_pane_paint - Repaint the pane and keep the caret in view.
 * @wf: the pane.
 *
 * The pane is an ordinary window, so the caret belongs to whichever window
 * is FOCUSED: it sits in the chat input row only while the pane itself is
 * focused; when the user has switched to their file (or a background task
 * runs), the pane repaints in the background and the caret stays in the
 * user's window.
 */
static void ai_pane_paint(FENSTER *wf)
{
 extern int wpe_modal_active;                    /* a dropdown/dialog box is open */
 FENSTER *caret = ai_caret_window(ai_top_window(wf));
 int pane_focused = ai_pane_is_focused(wf);

 /* A menu or dialog is up on top of us (its whole lifetime sets this flag): do
    NOT repaint under it, or a streaming token / the working spinner draws over
    the box and corrupts it -- the same guard the async language-server painter
    uses.  The buffer already holds the new text; the next paint once the box
    closes shows it. */
 if (wpe_modal_active)
  return;

 ai_pane_reflow(wf);                  /* the rows at the pane's present width */
 ai_pane_place_view(wf, pane_focused);
 e_schirm(wf, 0);
 e_cursor(caret, 0);
 e_refresh();
}

/**
 * ai_pane_scroll - Browse the transcript by @delta lines.
 * @wf: the pane.
 *
 * The view holds until the next input key: that key repaints and snaps back
 * to the input (ai_pane_paint pins the bottom), so PgUp/PgDn, the arrows and
 * the mouse wheel browse, then typing returns to the prompt.  Every way of
 * browsing comes through here, so they all lock the view the same way.
 */
static void ai_pane_scroll(FENSTER *wf, int delta)
{
 BUFFER *b = wf->b;
 SCHIRM *s = wf->s;
 int bottom = ai_pane_bottom_offset(wf);          /* view offset that pins the input */

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

/**
 * ai_pane_set_row_bytes - Overwrite pane row `y` with the first `n` bytes of
 * `text`, as a whole line (no new row added).
 * @b: the pane buffer, holding row `y`.
 */
static void ai_pane_set_row_bytes(BUFFER *b, int y, const char *text, size_t n)
{
 b->bf[y].s = REALLOC(b->bf[y].s, n + 2);
 memcpy(b->bf[y].s, text, n);
 b->bf[y].s[n] = '\n';
 b->bf[y].s[n + 1] = '\0';
 b->bf[y].len = (int)n;
 b->bf[y].nrc = (int)n + 1;
}

/**
 * ai_pane_set_line - Overwrite pane row @y with @text in place.
 * @wf: the pane.
 *
 * No row is added: the stream line, the input rows and a status that
 * replaces the waiting line all change under the user's eyes without the
 * transcript moving.
 */
static void ai_pane_set_line(FENSTER *wf, int y, const char *text)
{
 BUFFER *b = wf->b;
 if (b->mxlines == 0) {
  e_new_line(0, b);
  ai_roles_sync(b);
 }
 if (y < 0) y = 0;
 if (y >= b->mxlines) y = b->mxlines - 1;
 ai_pane_set_row_bytes(b, y, text, strlen(text));
}

/**
 * ai_pane_set_last - Replace the text of the stream line in place.
 * @wf: the pane.
 *
 * A partial line grows token by token as the model streams, the "live
 * typing" a real chat has, instead of a whole line appearing at once.
 */
static void ai_pane_set_last(FENSTER *wf, const char *text)
{
 ai_pane_set_line(wf, ai_stream_y(wf), text);
 ai_pane_note_unwrapped(text);
 ai_pane_paint(wf);
}

/**
 * ai_pane_set_last_role - Change who owns the stream line.
 * @wf: the pane.
 * @role: the writer (enum ai_role).
 *
 * A waiting line that becomes an error, a spinner label that the chat's
 * "AI:" line takes over: the colour follows the line's new owner at once.
 */
static void ai_pane_set_last_role(FENSTER *wf, int role)
{
 ai_roles_set(wf->b, ai_stream_y(wf), role);
}

/**
 * ai_pane_set_last_as - Replace the stream line's text and its owner together.
 */
static void ai_pane_set_last_as(FENSTER *wf, const char *text, int role)
{
 ai_pane_set_line(wf, ai_stream_y(wf), text);
 ai_pane_set_last_role(wf, role);
 ai_pane_note_unwrapped(text);
 ai_pane_paint(wf);
}

/**
 * ai_input_line_count - Rows the input region needs for the text in it.
 *
 * One per line break and one more: an empty input still has its "> " row.
 */
static int ai_input_line_count(void)
{
 const char *p;
 int want = 1;
 for (p = g_ai_input; *p; p++)
  if (*p == '\n') want++;
 return want;
}

/**
 * ai_input_drop_last_row - Take the pane's last row out of the input region.
 * @b: the pane buffer, whose last row belongs to the input region.
 */
static void ai_input_drop_last_row(BUFFER *b)
{
 int y = b->mxlines - 1;
 FREE(b->bf[y].s);
 b->mxlines--;
 g_ai_input_rows--;
}

/**
 * ai_input_fit_rows - Give the input region exactly @want rows.
 * @b: the pane buffer.
 *
 * The region grows and shrinks at the end of the pane as line breaks are
 * typed and deleted, never below its one "> " row, and the rows are recorded
 * as plain text so they never take a transcript colour.
 */
static void ai_input_fit_rows(BUFFER *b, int want)
{
 if (b->mxlines == 0)
  e_new_line(0, b);
 while (g_ai_input_rows < want) {
  e_new_line(b->mxlines, b);
  g_ai_input_rows++;
 }
 while (g_ai_input_rows > want && g_ai_input_rows > 1)
  ai_input_drop_last_row(b);
 ai_roles_sync(b);
}

/* Room in a rendered input row beyond the prompt text, and the part of it
   kept free of text: the prefix, the terminator and a margin. */
#define AI_INPUT_ROW_PAD     8
#define AI_INPUT_ROW_RESERVE 4

/**
 * ai_input_put_row - Paint one line of the input text as pane row @y.
 * @wf: the pane.
 * @first: the line is the first of the input.
 * @text: the start of the line; @len: its bytes.
 *
 * The first line shows the "> " prompt, the ones under it are indented by
 * the same width, so a multi-line prompt reads as one block.
 */
static void ai_input_put_row(FENSTER *wf, int y, int first,
                             const char *text, int len)
{
 char row[AI_PROMPT_MAX + AI_INPUT_ROW_PAD];
 const char *prefix;
 if (first)
  prefix = AI_INPUT_PROMPT;
 else
  prefix = AI_INPUT_INDENT;
 if (len > (int)sizeof row - AI_INPUT_ROW_RESERVE)
  len = (int)sizeof row - AI_INPUT_ROW_RESERVE;
 snprintf(row, sizeof row, "%s%.*s", prefix, len, text);
 ai_pane_set_line(wf, y, row);
}

/**
 * ai_input_render - Redraw the input region from the input text.
 * @wf: the pane.
 *
 * The region is the pane's last g_ai_input_rows rows; the text may hold
 * '\n'-separated lines, one row each.  Every change to the input ends here,
 * so what the user sees is always the text that Enter would send.
 */
static void ai_input_render(FENSTER *wf)
{
 BUFFER *b = wf->b;
 int want = ai_input_line_count();
 const char *p = g_ai_input;
 int i;

 ai_input_fit_rows(b, want);
 for (i = 0; i < want; i++) {
  const char *nl = strchr(p, '\n');
  int seglen;
  if (nl)
   seglen = (int)(nl - p);
  else
   seglen = (int)strlen(p);
  ai_input_put_row(wf, b->mxlines - want + i, i == 0, p, seglen);
  if (!nl)
   break;
  p = nl + 1;
 }
 ai_pane_paint(wf);
}

/**
 * ai_pane_commit - Finish the stream line and open an empty one after it.
 * @wf: the pane.
 *
 * The new line goes BEFORE the input region when the chat owns the bottom
 * rows, so the reply keeps reading down towards the "> " row.
 */
static void ai_pane_commit(FENSTER *wf)
{
 BUFFER *b = wf->b;
 int at;
 /* The pane's own opening row: a task that writes its first line through the
    stream line (a spinner, an edit's status) takes that row rather than
    opening a second one under it, so the pane does not start with a blank. */
 if (ai_pane_row_is_free(b, 0))
  return;
 if (g_ai_chat_focus && b->mxlines > g_ai_input_rows)
  at = b->mxlines - g_ai_input_rows;
 else
  at = b->mxlines;
 e_new_line(at, b);
 ai_roles_insert(b, at, AI_ROLE_ASSISTANT);   /* the stream line is the reply's */
}

/**
 * ai_tr_line - Append one finished transcript line as `role`.
 * @wf: the pane.
 * @str: the text.
 * @role: who wrote it (enum ai_role).
 *
 * The input region (if any) stays at the very bottom: the line goes just
 * above it.
 */
static void ai_tr_line(FENSTER *wf, const char *str, int role)
{
 BUFFER *b = wf->b;
 int at;
 ai_pane_note_unwrapped(str);
 if (!g_ai_chat_focus) {
  at = b->mxlines;
  print_to_end_of_buffer(b, (char *)str, b->mx.x);
  ai_roles_set_appended(b, at, role);
  return;
 }
 at = b->mxlines - g_ai_input_rows;      /* first input-region line */
 if (at < 0) at = 0;
 e_new_line(at, b);                      /* insert a blank line before the region */
 ai_roles_insert(b, at, role);
 ai_pane_set_line(wf, at, str);
}

#define AI_PANE_MIN_COLS 16
/**
 * ai_pane_width - Columns the pane wraps reply text to.
 * @wf: the pane.
 *
 * Inside its two borders, never wider than the line buffer, capped at the
 * readable prose measure AI_PANE_MAX_COLS, and never absurdly small.  Chat
 * streaming, every pane message and the re-wrap after a width change share
 * this, so a line always breaks the same way.
 */
static int ai_pane_width(FENSTER *wf)
{
 int w = wf->e.x - wf->a.x - 2;
 if (w > wf->b->mx.x - 1) w = wf->b->mx.x - 1;
 if (w > AI_PANE_MAX_COLS) w = AI_PANE_MAX_COLS;
 if (w < AI_PANE_MIN_COLS) w = AI_PANE_MIN_COLS;
 return w;
}

/**
 * ai_stream_wrap - Break the stream line at the pane width.
 * @s: the chat; @wf: the pane.
 *
 * The line is emitted up to its last space (a hard break when the run has
 * none) and the rest carries over to a fresh row, so a long reply reads
 * DOWN the window instead of scrolling off to the right.  Continuation rows
 * carry no "AI:" prefix, as in a real chat.
 */
static void ai_stream_wrap(ai_chat_session *s, FENSTER *wf)
{
 int brk = (int)s->plen;                 /* default: hard break at the end */
 int k, rem, col, m;
 for (k = (int)s->plen - 1; k > 0; k--)
  if (s->pending[k] == ' ') { brk = k + 1; break; }   /* the space stays on the row */
 { char saved = s->pending[brk];         /* emit pending[0..brk) as one line */
   s->pending[brk] = '\0';
   ai_pane_set_last(wf, s->pending);
   ai_pane_commit(wf);
   s->pending[brk] = saved; }
 /* The fresh row goes on with the same line: recorded so a later change of
    width joins the reply back and wraps it again whole. */
 ai_pane_mark_continuation(wf->b, ai_stream_y(wf));
 rem = (int)s->plen - brk;
 if (rem < 0) rem = 0;
 memmove(s->pending, s->pending + brk, (size_t)rem);
 s->pending[rem] = '\0';
 s->plen = rem;
 for (col = 0, m = 0; m < rem; m++)
  if (!ai_utf8_continues(s->pending[m])) col++;
 s->pcols = col;
}

/* ----- the transcript follows the pane's width ------------------------------
 * The rows were wrapped at the width of the moment they were written.  When
 * Messages opens beside the pane (a build, a changeset review), the terminal
 * is resized or Messages closes again, the pane's width changes and every
 * earlier row would be cut at the new border or leave the new room unused.
 * The functions below join each wrapped line back together from its
 * continuation rows and wrap it again at the present width, before the pane
 * is painted, so the user reads the whole transcript at any width. */

/**
 * ai_pane_transcript_rows - Rows that hold the transcript (not the input).
 * @wf: the pane.
 */
static int ai_pane_transcript_rows(FENSTER *wf)
{
 int n = wf->b->mxlines;
 if (g_ai_chat_focus) n -= g_ai_input_rows;
 if (n < 0) n = 0;
 return n;
}

/**
 * ai_pane_line_start - The first row of the logical line row `y` belongs to.
 */
static int ai_pane_line_start(int y)
{
 while (y > 0 && ai_row_continues(y)) y--;
 return y;
}

/**
 * ai_pane_stream_line_is_live - May a task still rewrite the stream row in place?
 *
 * The spinner, the waiting line and the tool status are written whole onto
 * the stream row by writers that do not wrap.  While one of them may still
 * come, the stream row's line is left as it is: a split row would be half
 * overwritten by the next tick.  It is re-wrapped at the first paint after
 * the task ends (see g_ai_rows_live_stale).
 */
static int ai_pane_stream_line_is_live(void)
{
 if (wpe_ai_busy()) return 1;
 if (g_ai_chat && g_ai_chat->active) return 1;
 return 0;
}

/**
 * ai_pane_logical_line_at - Which logical line row `y` belongs to (0-based).
 *
 * Counted so the line the user was reading can be found again after the
 * rows under it have changed.
 */
static int ai_pane_logical_line_at(int y)
{
 int r, n = 0;
 for (r = 1; r <= y && r < g_ai_roles_n; r++)
  if (!ai_row_continues(r)) n++;
 return n;
}

/**
 * ai_pane_row_of_logical_line - The first row of logical line `n`.
 * @b: the pane buffer.
 *
 * The last line's first row when the transcript is shorter than that.
 */
static int ai_pane_row_of_logical_line(BUFFER *b, int n)
{
 int r, seen = -1, start = 0;
 for (r = 0; r < b->mxlines; r++) {
  if (r > 0 && ai_row_continues(r)) continue;
  seen++;
  start = r;
  if (seen == n) return r;
 }
 return start;
}

/**
 * ai_pane_join_line - The logical line that starts at row `y0`, as one string.
 * @b: the pane buffer.
 * @y0: the first row of the line.
 * @limit: rows beyond this are not part of the transcript being laid out.
 * @rows: out, the rows the line spans.
 *
 * The rows are concatenated as they are: the wrapper left the break space on
 * the row it cut after, and a hard cut inside a word left none, so the join
 * reads exactly as the line was written.  Return: malloc'd text, or NULL.
 */
static char *ai_pane_join_line(BUFFER *b, int y0, int limit, int *rows)
{
 size_t total = 0;
 int y;
 char *t;
 for (y = y0; y < limit; y++) {
  int l;
  if (y > y0 && !ai_row_continues(y)) break;
  l = b->bf[y].s ? b->bf[y].len : 0;
  if (l > 0) total += (size_t)l;
 }
 *rows = y - y0;
 t = malloc(total + 1);
 if (!t) return NULL;
 total = 0;
 for (y = y0; y < y0 + *rows; y++) {
  int l = b->bf[y].s ? b->bf[y].len : 0;
  if (l > 0) { memcpy(t + total, b->bf[y].s, (size_t)l); total += (size_t)l; }
 }
 t[total] = '\0';
 return t;
}

/**
 * ai_wrap_rows - How many rows `text` takes when wrapped at `width`.
 */
static int ai_wrap_rows(const char *text, int width)
{
 const char *p = text;
 int n = 0, cut;
 if (!*p) return 1;
 while (*p) {
  n++;
  if (!ai_wrap_cut(p, width, &cut)) break;
  p += cut;
 }
 return n;
}

/**
 * ai_pane_drop_row - Remove row `y` from the pane and from the role table.
 * @b: the pane buffer.
 */
static void ai_pane_drop_row(BUFFER *b, int y)
{
 int after = b->mxlines - y - 1;
 if (y < 0 || y >= b->mxlines) return;
 FREE(b->bf[y].s);
 if (after > 0)
  memmove(b->bf + y, b->bf + y + 1, (size_t)after * sizeof(STRING));
 b->mxlines--;
 if (y < g_ai_roles_n) {
  if (g_ai_roles_n - y - 1 > 0)
   memmove(g_ai_roles + y, g_ai_roles + y + 1, (size_t)(g_ai_roles_n - y - 1));
  g_ai_roles_n--;
 }
 ai_roles_sync(b);
}

/**
 * ai_pane_rewrap_line - Lay the logical line at row `y0` out again at `width`.
 * @wf: the pane.
 * @y0: the first row of the line.
 * @limit: the end of the rows being laid out.
 * @width: the columns a row may use.
 *
 * The line keeps its writer's role on every row; rows are added or dropped
 * at its end as the new width needs.  Return: the rows the line now spans.
 */
static int ai_pane_rewrap_line(FENSTER *wf, int y0, int limit, int width)
{
 BUFFER *b = wf->b;
 int old_rows, new_rows, role, y;
 const char *p;
 char *text = ai_pane_join_line(b, y0, limit, &old_rows);
 if (!text) return old_rows;
 role = ai_role_of(y0);
 new_rows = ai_wrap_rows(text, width);
 while (old_rows < new_rows) {
  e_new_line(y0 + old_rows, b);
  ai_roles_insert(b, y0 + old_rows, role);
  old_rows++;
 }
 while (old_rows > new_rows) {
  ai_pane_drop_row(b, y0 + old_rows - 1);
  old_rows--;
 }
 p = text;
 for (y = y0; y < y0 + new_rows; y++) {
  int cut;
  int more = ai_wrap_cut(p, width, &cut);
  ai_pane_set_row_bytes(b, y, p, (size_t)cut);
  ai_roles_set(b, y, role);
  if (y > y0) ai_pane_mark_continuation(b, y);
  p += cut;
  if (!more) break;
 }
 free(text);
 return new_rows;
}

/**
 * ai_pane_view_after_reflow - Put the view back where the user was reading.
 * @wf: the pane.
 * @held: the user had scrolled the transcript (the view is held).
 * @top_line: the logical line that was at the top of the held view.
 * @rows_changed: the row count differs from before the re-wrap.
 *
 * A held view shows the same logical line at its top as before, now over
 * its new rows; otherwise the input row (or the last line) is pinned to the
 * bottom as every paint does.  A block mark made on rows that no longer
 * exist as marked is dropped rather than left on other text.
 */
static void ai_pane_view_after_reflow(FENSTER *wf, int held, int top_line,
                                      int rows_changed)
{
 BUFFER *b = wf->b;
 SCHIRM *s = wf->s;
 if (rows_changed) {
  s->mark_begin = e_set_pnt(0, 0);
  s->mark_end = e_set_pnt(0, 0);
 }
 s->c.x = 0;                 /* every row fits the width: no sideways scroll */
 if (held) {
  s->c.y = ai_pane_row_of_logical_line(b, top_line);
 } else {
  b->b.y = b->mxlines - 1;
  b->b.x = 0;
 }
 ai_pane_place_view(wf, ai_pane_is_focused(wf));
}

/**
 * ai_pane_reflow_is_owed - Is a pass owed at the present width?
 * @live: a task may still rewrite the stream row.
 *
 * Yes when a line was left at an old width because a task was rewriting
 * it, or a row was written whole and wider than the pane, and no task is
 * rewriting the stream row now.
 */
static int ai_pane_reflow_is_owed(int live)
{
 if (live) return 0;
 return g_ai_rows_live_stale || g_ai_rows_unwrapped;
}

/**
 * ai_pane_reflow - Lay the transcript out again when the pane's width changed.
 * @wf: the pane.
 *
 * Nothing to do while the rows are already at the pane's width.  Otherwise
 * every logical line above the input region is joined from its rows and
 * wrapped again at the present width: a narrowed pane shows every word of
 * every line over more rows, a widened pane uses its room.  The stream
 * row's line is skipped while a task may still rewrite it and caught up at
 * the first paint after that.  Runs before every paint of the pane, so the
 * user never sees a clipped transcript.
 */
static void ai_pane_reflow(FENSTER *wf)
{
 BUFFER *b = wf->b;
 int width = ai_pane_width(wf);
 int live = ai_pane_stream_line_is_live();
 int held = ai_pane_keeps_browsed_view();
 int top_line = 0, limit, y, rows_before;

 if (width == g_ai_rows_width && !ai_pane_reflow_is_owed(live))
  return;
 wpe_ai_trace("pane reflow: width %d -> %d, %d rows, live %d, held %d",
              g_ai_rows_width, width, b->mxlines, live, held);
 ai_roles_sync(b);
 rows_before = b->mxlines;
 limit = ai_pane_transcript_rows(wf);
 if (live && limit > 0)
  limit = ai_pane_line_start(limit - 1);        /* leave the stream line alone */
 if (held)
  top_line = ai_pane_logical_line_at(wf->s->c.y);
 for (y = 0; y < limit; ) {
  int before = b->mxlines;
  int rows = ai_pane_rewrap_line(wf, y, limit, width);
  limit += b->mxlines - before;
  y += rows;
 }
 g_ai_rows_width = width;
 g_ai_rows_live_stale = live;
 g_ai_rows_unwrapped = 0;
 ai_pane_view_after_reflow(wf, held, top_line, b->mxlines != rows_before);
}

/**
 * wpe_ai_pane_before_paint - The window painter is about to paint `f`.
 * @f: the window being painted.
 *
 * Called from the shared painter for every window, so the pane's rows are
 * laid out at its present width on every repaint under the terminal, X11 and
 * Wayland alike -- after a build opened Messages beside it, a resize, or a
 * window the pane shared the strip with closed; any other window is left
 * alone.
 */
void wpe_ai_pane_before_paint(FENSTER *f)
{
 if (!ai_is_pane(f)) return;
 ai_pane_reflow(f);
}

/* A "working" indicator for the blocking Edit/Plan/Agent waits: a rotating
 * -\|/ plus the seconds elapsed, redrawn in place on one pane line so a slow
 * local model looks alive instead of frozen.  wpe_ai_complete calls ai_spin_cb
 * ~8x/second (see wpe_ai_progress_cb). */
typedef struct {
 FENSTER *f;
 const char *label;
 int i;
 int open;        /* the pane's stream line is this task's waiting line */
} ai_spin;

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
 * wpe_ai_complete as its progress callback.  The line is the task's until
 * ai_spin_end takes it out, or the chat's painter writes the answer over it. */
static ai_spin ai_spin_begin(FENSTER *f, const char *label)
{
 ai_spin sp;
 FENSTER *wf = ai_pane_win(f);
 sp.f = f; sp.label = label; sp.i = 0; sp.open = 0;
 if (wf) {
  ai_pane_commit(wf);
  ai_pane_set_last_as(wf, label, AI_ROLE_MACHINERY);
  sp.open = 1;
 }
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

/**
 * ai_tool_status - Word a "TOOL <name> <arg>" protocol line as a status.
 * @line: the protocol line, not necessarily NUL-terminated at @len.
 * @out: the status, "  . reading <arg>"; @n: its room.
 *
 * The raw protocol is never shown as the assistant's answer, whether the
 * tool call leads the reply or follows a preamble.
 */
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

/**
 * ai_chat_keep_full - Add @delta to the whole reply kept for the session log.
 * @s: the chat; @dl: the delta's length.
 *
 * The conversation resumes later with the complete answer, not just the
 * part that was painted.  When memory runs out the delta is left out.
 */
static void ai_chat_keep_full(ai_chat_session *s, const char *delta, size_t dl)
{
 if (!ai_chat_buf_reserve(&s->full, &s->fcap, s->flen + dl + 1, AI_FULL_FIRST))
  return;
 memcpy(s->full + s->flen, delta, dl);
 s->flen += dl;
 s->full[s->flen] = '\0';
}

/**
 * ai_chat_pending_text - The stream line's text so far, "" before any.
 */
static const char *ai_chat_pending_text(const ai_chat_session *s)
{
 if (s->pending)
  return s->pending;
 return "";
}

/**
 * ai_chat_pending_reset - Start the stream line afresh, empty.
 */
static void ai_chat_pending_reset(ai_chat_session *s)
{
 s->plen = 0;
 s->pcols = 0;
 if (s->pending)
  s->pending[0] = '\0';
}

/**
 * ai_chat_pending_append - Add byte @c to the stream line.
 *
 * Return: 0 when memory ran out and the byte was not added.
 */
static int ai_chat_pending_append(ai_chat_session *s, char c)
{
 if (!ai_chat_buf_reserve(&s->pending, &s->pcap, s->plen + 2, AI_PENDING_FIRST))
  return 0;
 s->pending[s->plen++] = c;
 s->pending[s->plen] = '\0';
 return 1;
}

/**
 * ai_chat_show_tool_status - Put a tool call's dim status on the stream line.
 * @wf: the pane.
 * @line: the "TOOL <name> <arg>" protocol line; @len: its bytes.
 *
 * The user reads "  . reading <path>" where the raw protocol would have
 * looked like a broken reply.
 */
static void ai_chat_show_tool_status(FENSTER *wf, const char *line, size_t len)
{
 char status[AI_TOOL_STATUS_MAX];
 ai_tool_status(line, len, status, sizeof status);
 ai_pane_set_last_as(wf, status, AI_ROLE_MACHINERY);
}

/**
 * ai_chat_show_leading_tool - The reply opens with a tool call: show its status.
 * @s: the chat.
 * @nl: the end of the first line, or NULL while it is still arriving.
 *
 * The status replaces the waiting line, and the turn is marked a tool
 * investigation so nothing of the protocol is echoed as "AI:".
 */
static void ai_chat_show_leading_tool(ai_chat_session *s, const char *nl)
{
 FENSTER *wf = ai_pane_win(s->ref);
 size_t fl = s->flen;
 if (nl)
  fl = (size_t)(nl - s->full);
 ai_chat_wait_end(s);                               /* the status owns the line */
 if (wf)
  ai_chat_show_tool_status(wf, s->full, fl);
 s->decided = AI_REPLY_TOOL;
}

/**
 * ai_chat_decide_first_line - Tell an answer from a tool call by the first line.
 * @s: the chat, its reply so far undecided.
 *
 * A "TOOL ..." line is the model asking to INVESTIGATE (read a file, grep,
 * ...), not its answer; anything else is the answer and streams.  Return: 1
 * once decided, 0 while there are too few bytes to tell "TOOL " from text.
 */
static int ai_chat_decide_first_line(ai_chat_session *s)
{
 const char *nl = memchr(s->full, '\n', s->flen);
 if (s->flen < AI_TOOL_PREFIX_LEN && !nl)
  return 0;                                         /* wait: could be "TOOL " */
 if (!strncmp(s->full, AI_TOOL_PREFIX, AI_TOOL_PREFIX_LEN)) {
  ai_chat_show_leading_tool(s, nl);
  return 1;
 }
 s->decided = AI_REPLY_ANSWER;                      /* an answer -- stream it */
 ai_chat_wait_end(s);                               /* the reply owns the line */
 return 1;
}

/**
 * ai_chat_reply_streams - May the painter put the reply's bytes on the pane?
 * @s: the chat.
 *
 * Return: 0 while the first line is undecided and while a tool investigation
 * is being hidden; 1 for an answer.
 */
static int ai_chat_reply_streams(ai_chat_session *s)
{
 if (s->decided == AI_REPLY_UNDECIDED && s->full && !ai_chat_decide_first_line(s))
  return 0;
 return s->decided != AI_REPLY_TOOL;
}

/**
 * ai_chat_open_reply_line - Open the reply's line under the "AI:" header, once.
 */
static void ai_chat_open_reply_line(ai_chat_session *s, FENSTER *wf)
{
 if (s->started)
  return;
 ai_pane_commit(wf);
 s->started = 1;
}

/**
 * ai_chat_end_hidden_tool_line - The hidden TOOL line ended at byte @i of the reply.
 *
 * Its dim status stands on the stream line and nothing after it is echoed:
 * the turn is a tool investigation from here on.
 */
static void ai_chat_end_hidden_tool_line(ai_chat_session *s, FENSTER *wf, size_t i)
{
 ai_chat_show_tool_status(wf, s->pending, s->plen);
 s->decided = AI_REPLY_TOOL;
 ai_chat_pending_reset(s);
 s->shown = i + 1;
}

/**
 * ai_chat_commit_stream_line - A line of the answer is complete.
 *
 * It is painted whole and a fresh line opens under it for what follows.
 */
static void ai_chat_commit_stream_line(ai_chat_session *s, FENSTER *wf)
{
 ai_pane_set_last(wf, ai_chat_pending_text(s));
 ai_pane_commit(wf);
 ai_chat_pending_reset(s);
}

/**
 * ai_chat_paint_char - Add byte @c of the answer to the stream line.
 * @width: the pane's wrap width.
 *
 * A line that begins "TOOL " after a reasoning preamble is hidden the way a
 * leading one is; an answer line wraps at the pane width.  Return: 0 when
 * memory ran out and painting must stop.
 */
static int ai_chat_paint_char(ai_chat_session *s, FENSTER *wf, char c, int width)
{
 if (!ai_chat_pending_append(s, c))
  return 0;
 if (!s->hide_tool && s->plen == AI_TOOL_PREFIX_LEN &&
     !strncmp(s->pending, AI_TOOL_PREFIX, AI_TOOL_PREFIX_LEN))
  s->hide_tool = 1;                        /* this fresh line is a tool call */
 if (!ai_utf8_continues(c))
  s->pcols++;
 if (!s->hide_tool && s->pcols >= width)   /* soft-wrap at the pane width */
  ai_stream_wrap(s, wf);
 return 1;
}

/**
 * ai_chat_show_partial_line - Paint the line that is still arriving.
 *
 * The answer's live partial line grows token by token; a tool line still
 * arriving shows as its dim status.
 */
static void ai_chat_show_partial_line(ai_chat_session *s, FENSTER *wf)
{
 if (s->hide_tool) {
  ai_chat_show_tool_status(wf, s->pending, s->plen);
  return;
 }
 ai_pane_set_last(wf, ai_chat_pending_text(s));
}

/**
 * ai_delta_cb - Stream a delta of the reply into the pane.
 * @delta: the bytes just arrived; @ud: the chat.
 *
 * Tokens appear as they arrive, a new line opens at each '\n', and the whole
 * reply is kept for the session log.  Everything of the reply not yet shown
 * is painted, which covers the bytes held while the first line was being
 * decided.
 */
static void ai_delta_cb(const char *delta, void *ud)
{
 ai_chat_session *s = ud;
 FENSTER *wf;
 int width;
 size_t i;

 ai_chat_keep_full(s, delta, strlen(delta));
 if (!ai_chat_reply_streams(s))
  return;
 wf = ai_pane_win(s->ref);
 if (!wf)
  return;
 width = ai_pane_width(wf);
 ai_chat_open_reply_line(s, wf);
 for (i = s->shown; i < s->flen; i++) {
  char c = s->full[i];
  if (c == '\r')
   continue;
  if (c == '\n' && s->hide_tool) {
   ai_chat_end_hidden_tool_line(s, wf, i);
   return;
  }
  if (c == '\n') {
   ai_chat_commit_stream_line(s, wf);
   continue;
  }
  if (!ai_chat_paint_char(s, wf, c, width))
   return;
 }
 s->shown = s->flen;                                   /* painted up to here */
 ai_chat_show_partial_line(s, wf);
}

/**
 * ai_chat_paint_held - Paint a reply the painter was still holding when the turn ended.
 * @s: the chat whose stream has just completed.
 *
 * The painter holds the first bytes of a reply until five bytes or a newline
 * tell an answer from a "TOOL ..." line.  A whole reply shorter than that
 * ("ok", "Yes", "42") never reached the decision, and the finish path freed
 * the bytes unpainted: the pane stayed on "(gathering the answer...)" after
 * the turn was over.  Once the stream has ended nothing more can arrive, so
 * what is held is the answer and is painted as such.
 */
static void ai_chat_paint_held(ai_chat_session *s)
{
 if (s->decided != AI_REPLY_UNDECIDED || !s->full || !s->flen) return;
 s->decided = AI_REPLY_ANSWER;
 ai_delta_cb("", s);
}

/**
 * ai_chat_waiting_line_stands - Is the pane's stream line still the waiting line?
 * @s: the chat whose turn is ending.
 *
 * Until the painter decides what the reply is, the line under the user's
 * question is the waiting line and nothing else; once a reply or a tool
 * status has been painted there, that content is the user's and stays.
 * Return: 1 while the waiting line is all that is on the stream line.
 */
static int ai_chat_waiting_line_stands(const ai_chat_session *s)
{
 return s->decided == AI_REPLY_UNDECIDED;
}

/**
 * ai_chat_report_transport_error - Tell the user the peer hung up.
 * @s: the chat whose connection died.
 *
 * A server that closes without a byte, or answers something that is not
 * HTTP, ends the turn with "[AI: transport error]" in place of the waiting
 * line, so "(gathering the answer...)" is never left standing over an answer
 * that cannot come.  A reply that had started to paint is kept and the
 * transport line follows it.
 */
static void ai_chat_report_transport_error(ai_chat_session *s)
{
 FENSTER *wf = ai_pane_win(s->ref);
 if (wf && ai_chat_waiting_line_stands(s)) {
  ai_pane_set_last_as(wf, AI_CHAT_TRANSPORT_LINE, AI_ROLE_ERROR);
  return;
 }
 ai_pane(s->ref, AI_CHAT_TRANSPORT_LINE, AI_ROLE_ERROR, 0);
}

/**
 * ai_chat_record_tool_result - Give the tool's output to the model as the next turn's input.
 * @tr: the tool's output.
 *
 * The model reads what it asked for and answers from it, the way a person
 * would after looking the file up.
 */
static void ai_chat_record_tool_result(const char *tr)
{
 size_t n = strlen(tr) + sizeof AI_TOOL_RESULT_HEAD;
 char *m = malloc(n);
 if (!m)
  return;
 snprintf(m, n, AI_TOOL_RESULT_HEAD "%s", tr);
 wpe_ai_session_append("user", m);
 free(m);
}

/**
 * ai_chat_restart_turn - Close this turn's stream and start the next one.
 * @s: the chat.
 *
 * The editor never blocks: the next turn (an answer, another tool call, the
 * retry on the fallback model) streams in the background like the first.
 */
static void ai_chat_restart_turn(ai_chat_session *s)
{
 wpe_fd_del(s->fd);
 wpe_ai_stream_free(s->st);
 s->st = NULL;
 ai_chat_next_turn(s);
}

/**
 * ai_chat_take_tool_turn - Act on a reply that asks to look at the repository.
 * @s: the chat, its reply complete.
 *
 * The reply joins the session log either way.  A tool call within the turn
 * budget runs read-only and its result feeds the next turn; from here on the
 * chat acts on the repository and the next prompt carries the project
 * instructions under the default scope.  Return: 1 when a tool turn is now
 * streaming, 0 when the reply was the answer.
 */
static int ai_chat_take_tool_turn(ai_chat_session *s)
{
 char *tr = NULL;
 if (s->turns < AI_CHAT_MAX_TOOL_TURNS)
  tr = ai_chat_tool_result(s, s->full);
 wpe_ai_session_append("assistant", s->full);
 if (!tr)
  return 0;
 wpe_ai_session_mark_tool_turn();
 tr = ai_tool_result_nonempty(tr);      /* an empty result becomes an explicit note */
 ai_chat_record_tool_result(tr);
 free(tr);
 s->turns++;
 ai_chat_restart_turn(s);
 return 1;
}

/**
 * ai_chat_fallback_wanted - Should this failed turn be retried on the fallback model?
 * @emsg: the server's error, NULL for a plain empty answer.
 *
 * Once per chat, for a real failure, when a fallback model is set and is not
 * the model that failed: a primary that is down or too slow does not
 * dead-end the chat.
 */
static int ai_chat_fallback_wanted(const ai_chat_session *s, const char *emsg)
{
 if (!emsg || s->fell_back)
  return 0;
 if (!e_ai_model_fallback || !*e_ai_model_fallback)
  return 0;
 if (e_ai_model && !strcmp(e_ai_model, e_ai_model_fallback))
  return 0;
 return 1;
}

/**
 * ai_chat_announce_fallback - Say on the stream line that the turn is retried.
 * @wf: the pane, or NULL when it is gone.
 *
 * The user learns which model failed and why before the retry streams.
 */
static void ai_chat_announce_fallback(FENSTER *wf, const char *emsg)
{
 char line[AI_CHAT_NOTE_MAX];
 snprintf(line, sizeof line,
          AI_REPLY_PREFIX "primary model failed (%s) - retrying with %s",
          emsg, e_ai_model_fallback);
 if (wf)
  ai_pane_set_last_as(wf, line, AI_ROLE_MACHINERY);
}

/**
 * ai_chat_show_empty_reply - Nothing streamed: say why on the stream line.
 * @wf: the pane.
 * @emsg: the server's error, NULL when the answer was simply empty.
 *
 * A request the server refused (a model the key cannot access) shows its
 * error, where "(no answer)" told the user nothing.
 */
static void ai_chat_show_empty_reply(FENSTER *wf, const char *emsg)
{
 char line[AI_CHAT_NOTE_MAX];
 if (!emsg) {
  ai_pane_set_last(wf, AI_REPLY_PREFIX "(no answer)");
  return;
 }
 snprintf(line, sizeof line, AI_REPLY_PREFIX "error: %s", emsg);
 ai_pane_set_last_as(wf, line, AI_ROLE_ERROR);
}

/**
 * ai_chat_end_empty_reply - End a turn that produced no text.
 * @s: the chat.
 *
 * Return: 1 when the turn goes on, retried on the fallback model; 0 when it
 * ends with the error or the placeholder on the stream line.
 */
static int ai_chat_end_empty_reply(ai_chat_session *s)
{
 FENSTER *wf = ai_pane_win(s->ref);
 char *emsg = wpe_ai_stream_error_message(s->st);
 if (ai_chat_fallback_wanted(s, emsg)) {
  ai_chat_announce_fallback(wf, emsg);
  free(emsg);
  s->fell_back = 1;
  ai_chat_restart_turn(s);
  return 1;
 }
 if (wf)
  ai_chat_show_empty_reply(wf, emsg);
 free(emsg);
 return 0;
}

/**
 * ai_chat_reply_done - The stream has ended: close the turn or take the next.
 * @s: the chat.
 *
 * The trailing partial line is already on screen (painted live), except a
 * reply too short for the painter to have decided on, painted here.  The
 * session is saved once the turn really ends, so the talk resumes complete.
 */
static void ai_chat_reply_done(ai_chat_session *s)
{
 int has_text;
 ai_chat_paint_held(s);
 has_text = (s->full != NULL && s->flen > 0);
 if (has_text && ai_chat_take_tool_turn(s))
  return;
 if (!has_text && ai_chat_end_empty_reply(s))
  return;
 ai_chat_show_sources(s);
 wpe_ai_session_save(s->ref);
 wpe_ai_trace("chat done");
 ai_chat_turn_over(s);
}

/**
 * ai_fd_cb - The stream has bytes: drain them into the pane.
 * @fd: the stream's descriptor; @data: the chat.
 *
 * Called from the editor's fd loop, so the reply arrives while the user
 * keeps editing; a peer that hangs up ends the turn with a word about it.
 */
static void ai_fd_cb(int fd, void *data)
{
 ai_chat_session *s = data;
 int done = 0;
 (void)fd;
 if (!s || !s->active) return;
 if (wpe_ai_stream_pump(s->st, ai_delta_cb, s, &done) < 0) {
  ai_chat_report_transport_error(s);
  wpe_ai_trace("chat transport error");
  ai_chat_turn_over(s);
  return;
 }
 if (done)
  ai_chat_reply_done(s);
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
 if (wpe_ai_web_state() == WPE_AI_WEB_ON) {
  snprintf(buf, n, "[AI: %s | model: %s | policy: %s | web]", be, model, pol);
  return;
 }
 snprintf(buf, n, "[AI: %s | model: %s | policy: %s]", be, model, pol);
}

/* ----- the pane header follows the configuration ---------------------------
 * The header is a transcript line, so it cannot be repainted when Options > AI
 * is closed with Ok, Alt-G y cycles the dial or the provider switches.  What
 * the last header said is remembered instead, and the next time the chat is
 * armed or a turn starts, a header is written only if the values changed. */
#define AI_HEADER_MAX 200
static char g_ai_header_shown[AI_HEADER_MAX];   /* "" = no header shown yet     */
static int  g_ai_hints_shown = 0;               /* the two hint lines were shown */

/**
 * ai_header_text_shown - Build the header line and remember it as shown.
 * @buf: destination; @n: its size.
 *
 * Every place that writes the header into the pane builds it here, so the
 * chat knows the backend, model and dial the user last read and does not
 * repeat a header that is still true.
 */
static void ai_header_text_shown(char *buf, size_t n)
{
 ai_status_text(buf, n);
 strncpy(g_ai_header_shown, buf, sizeof g_ai_header_shown - 1);
 g_ai_header_shown[sizeof g_ai_header_shown - 1] = '\0';
}

/**
 * ai_chat_header_due - Does the pane need a header with the current values?
 *
 * True before any header was shown, and whenever the backend, the model or
 * the permission dial differ from the values the last header showed, so the
 * user reads the setup that will serve the next turn, not an earlier one.
 * Return: 1 when a header must be written.
 */
static int ai_chat_header_due(void)
{
 char now[AI_HEADER_MAX];
 ai_status_text(now, sizeof now);
 return strcmp(now, g_ai_header_shown) != 0;
}

/**
 * ai_chat_emit_header_if_due - Write a header line when the setup changed.
 * @wf: the pane window.
 *
 * The line goes above the input row like any transcript line; the lines
 * before it are untouched and the view stays pinned where it was.
 */
static void ai_chat_emit_header_if_due(FENSTER *wf)
{
 char st[AI_HEADER_MAX];
 if (!ai_chat_header_due())
  return;
 ai_header_text_shown(st, sizeof st);
 ai_pane(wf, st, AI_ROLE_MACHINERY, 0);     /* wrapped to the pane's width */
}

/**
 * ai_chat_emit_hints_once - Write the key-hint and slash-hint lines once.
 * @wf: the pane window.
 *
 * Shown the first time the chat is armed in a session, whatever another
 * action (an Edit, the first-use notice, "assistant is off") wrote to the
 * pane before; a user who closes the pane and opens it again reads them
 * again in the fresh pane.
 */
static void ai_chat_emit_hints_once(FENSTER *wf)
{
 if (g_ai_hints_shown)
  return;
 g_ai_hints_shown = 1;
 /* Through the wrapper, so a pane that shares the strip with Messages shows
    the whole hint over two rows instead of a hint cut at the border. */
 ai_pane(wf, "[Enter=send  Ctrl-J=newline  arrows move  PgUp/PgDn scroll  Esc leaves]",
         AI_ROLE_MACHINERY, 0);
 ai_pane(wf, "[Shortcuts: /explain  /fix  /test  /doc  /review]", AI_ROLE_MACHINERY, 0);
}

/**
 * ai_chat_forget_header - The pane is gone: its header and hints with it.
 *
 * Called when the pane window closes, so the next pane opens with the
 * header and the hints again instead of a bare transcript.
 */
static void ai_chat_forget_header(void)
{
 g_ai_header_shown[0] = '\0';
 g_ai_hints_shown = 0;
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

/* The project's standing AI instructions.  The prompt carries at most
   e_ai_project_max bytes of the file (AIProjectInstructionsMax, default
   WPE_AI_PROJECT_MAX_DEFAULT), so a large file cannot crowd out the rest of a
   local model's window; the body and the block are allocated from that bound,
   which is why the key may be raised without touching this file. */

/* The heading of the project block in the prompt, naming the file it came
   from, and the sentence that follows the block in the same system message:
   the instructions say how to use the tools, the editor says which tools there
   are.  A local model took "do not open network connections" in a repository's
   AGENTS.md, written for other coding agents, as an order to refuse the
   editor's own web tools; the sentence is written once and used by the agent,
   the plan and the chat. */
#define AI_PROJECT_BLOCK_HEAD_FMT \
  "PROJECT INSTRUCTIONS (from the project's %s -- follow these):\n"
#define AI_PROJECT_TOOLS_STAND \
  "The tool list above stands: project instructions govern how the tools are " \
  "used, never which tools exist.\nA rule in them that forbids a tool does not " \
  "remove it from this editor's list."
/* Room for the heading with its file name, the two newlines and the sentence
   around the body. */
#define AI_PROJECT_BLOCK_EXTRA 400

/* The pane line that says which project instructions a run folded in.  The
   cut line names the key that raises the bound, as the timeout line names
   AIToolTimeout, so the user learns from the line itself how to change it. */
#define AI_PROJECT_ANNOUNCE_LINE     "[agent] project instructions: %s (%zu bytes)"
#define AI_PROJECT_ANNOUNCE_CUT_LINE \
  "[agent] project instructions: %s (%zu bytes, cut at %d; AIProjectInstructionsMax raises it)"

/* The files looked for, in order: the editor's own file first, so a
   repository whose AGENTS.md serves other coding agents can still give this
   editor its own instructions; AGENTS.md (the cross-tool convention) only when
   .xwpe-ai.md is absent. */
static const char *ai_project_file_names[] = { ".xwpe-ai.md", "AGENTS.md" };

/* Which prompt is being built, for the question of whether the project
   instructions belong in it: a chat turn, or a mode that acts on the
   repository (the Agent, the Multi-file edit, Build & fix). */
enum ai_prompt_mode { AI_PROMPT_CHAT = 0, AI_PROMPT_ACTS = 1 };

/* What ai_project_memory found: the file's name, how much of it the prompt
   carries, and how long it really is (longer than `loaded` means it was cut). */
struct ai_project_memory_info {
 const char *name;
 size_t      loaded;
 size_t      total;
};

/**
 * ai_project_instructions_wanted - Does this prompt carry the project instructions?
 * @mode: what is being built; @session_used_tool: for a chat turn, whether a
 * turn of this session has already answered with a tool line.
 *
 * The one place the AIProjectInstructions key is read, so every builder
 * answers the same way.  Under agent, the default, the modes that act on the
 * repository carry the file, and the chat carries it from the turn in which it
 * first uses a tool: a question answered from the open file would spend a
 * tenth of a local model's window on rules about the repository for nothing.
 * all folds it into every turn; off never does.
 * Return: 1 when the block belongs in the prompt.
 */
static int ai_project_instructions_wanted(enum ai_prompt_mode mode, int session_used_tool)
{
 if (e_ai_project_scope == WPE_AI_PROJECT_OFF) return 0;
 if (e_ai_project_scope == WPE_AI_PROJECT_ALL) return 1;
 if (mode == AI_PROMPT_ACTS) return 1;
 return session_used_tool;
}

/**
 * ai_project_memory_open - Open the instructions file that applies, noting its name and size.
 * @info: receives the file's name and real length.
 * Return: the open file, or NULL when neither file exists.
 */
static FILE *ai_project_memory_open(struct ai_project_memory_info *info)
{
 size_t i;
 struct stat st;
 for (i = 0; i < sizeof ai_project_file_names / sizeof ai_project_file_names[0]; i++) {
  FILE *fp = fopen(ai_project_file_names[i], "rb");
  if (!fp) continue;
  info->name = ai_project_file_names[i];
  if (fstat(fileno(fp), &st) == 0) info->total = (size_t)st.st_size;
  return fp;
 }
 return NULL;
}

/**
 * ai_project_memory - Read the project's AI instructions file, bounded by the key.
 * @info: filled with the file's name and sizes, so the caller can tell the
 *        user what was folded in and whether the file was cut.
 * Return: the first e_ai_project_max bytes of the file, NUL-terminated and
 * malloc'd (the caller frees it), or NULL when there is no instructions file
 * or no memory.
 *
 * Looks for .xwpe-ai.md, then AGENTS.md, in the working directory -- which is
 * xwpe's cwd, i.e. where it was launched: the project root.  Lets a repo carry
 * standing instructions (build command, code style, "do not touch X") that the
 * assistant follows where it acts on the repository, without the user
 * restating them each turn.
 */
static char *ai_project_memory(struct ai_project_memory_info *info)
{
 FILE *fp;
 char *out;
 size_t bound = (size_t)e_ai_project_max;
 info->name = NULL; info->loaded = 0; info->total = 0;
 fp = ai_project_memory_open(info);
 if (!fp) return NULL;
 out = malloc(bound + 1);
 if (!out) { fclose(fp); return NULL; }
 info->loaded = fread(out, 1, bound, fp);
 fclose(fp);
 out[info->loaded] = '\0';
 if (info->total < info->loaded) info->total = info->loaded;
 wpe_ai_trace("project memory %s bytes=%zu", info->name, info->loaded);
 return out;
}

/**
 * ai_project_memory_cut - Was the instructions file longer than the prompt carries?
 * @info: what ai_project_memory found.  Return: 1 when the file was cut.
 */
static int ai_project_memory_cut(const struct ai_project_memory_info *info)
{
 return info->total > info->loaded;
}

/**
 * ai_project_memory_announce - Tell the user which project instructions a run folds in.
 * @f: the window the pane belongs to; @info: what ai_project_memory found.
 *
 * Until now only the trace said that the file was loaded, so when the model
 * followed a rule from it (a repository's "do not open network connections",
 * written for other agents) the user could not see where the refusal came
 * from.  The pane line names the file and its size, and when the file was
 * longer than the prompt carries says "cut at N; AIProjectInstructionsMax
 * raises it", so a rule near its end that the model never saw is not a mystery
 * and the key that lets the user see all of it is on the same line.
 */
static void ai_project_memory_announce(FENSTER *f, const struct ai_project_memory_info *info)
{
 char line[200];
 if (ai_project_memory_cut(info))
  snprintf(line, sizeof line, AI_PROJECT_ANNOUNCE_CUT_LINE, info->name, info->total,
           e_ai_project_max);
 else
  snprintf(line, sizeof line, AI_PROJECT_ANNOUNCE_LINE, info->name, info->loaded);
 ai_pane(f, line, AI_ROLE_MACHINERY, 0);
}

/**
 * ai_project_block_alloc - Form the project block for a prompt: heading, file, precedence.
 * @info: what ai_project_memory found (the heading names the file);
 * @body: the instructions file's text.
 * Return: the malloc'd block (the caller frees it), or NULL out of memory.
 *
 * One builder for the chat and the message-list prompts, so the heading and
 * the tool-list-stands sentence cannot differ between them.
 */
static char *ai_project_block_alloc(const struct ai_project_memory_info *info, const char *body)
{
 size_t n = strlen(body) + strlen(info->name) + AI_PROJECT_BLOCK_EXTRA;
 char *out = malloc(n);
 if (!out) return NULL;
 snprintf(out, n, AI_PROJECT_BLOCK_HEAD_FMT, info->name);
 snprintf(out + strlen(out), n - strlen(out), "%s\n\n%s", body, AI_PROJECT_TOOLS_STAND);
 return out;
}

/**
 * ai_chat_announce_project_memory - Show the project-instructions line once per chat session.
 * @f: the window the pane belongs to; @info: what ai_project_memory found.
 *
 * The chat rebuilds its prompt on every turn, and an answer may take several
 * read-only turns, so the line would otherwise repeat under every question.
 * The session remembers that it was shown (see wpe_ai_session_project_announced).
 */
static void ai_chat_announce_project_memory(FENSTER *f, const struct ai_project_memory_info *info)
{
 if (wpe_ai_session_project_announced()) return;
 ai_project_memory_announce(f, info);
 wpe_ai_session_mark_project_announced();
}

/**
 * ai_chat_wants_project_memory - Does this chat turn carry the project instructions?
 *
 * Asks ai_project_instructions_wanted for a chat turn, with the session's
 * record of whether a turn has used a tool yet; a turn left out is traced, so
 * a user wondering why the model ignored the file can see the scope at work.
 * Return: 1 when the block belongs in this turn's prompt.
 */
static int ai_chat_wants_project_memory(void)
{
 if (ai_project_instructions_wanted(AI_PROMPT_CHAT, wpe_ai_session_tool_turn_opened()))
  return 1;
 wpe_ai_trace("project instructions left out scope=%s mode=chat",
              wpe_ai_project_scope_name(e_ai_project_scope));
 return 0;
}

/**
 * ai_append_project_memory - Add the project instructions to a text prompt.
 * @sys/@cap/@len: prompt buffer being grown; @f: the window the chat pane
 * belongs to, for the once-per-session announcement.  Return: the new length.
 * A no-op when the scope leaves this turn out, when there is no .xwpe-ai.md /
 * AGENTS.md, or when no room is left.
 */
static size_t ai_append_project_memory(char *sys, size_t cap, size_t len, FENSTER *f)
{
 char *mem, *block;
 struct ai_project_memory_info info;
 if (!ai_chat_wants_project_memory()) return len;
 mem = ai_project_memory(&info);
 if (!mem) return len;
 block = ai_project_block_alloc(&info, mem);
 free(mem);
 if (!block) return len;
 if (len + strlen(block) + 4 < cap) {
  len += (size_t)snprintf(sys + len, cap - len, "%s\n\n", block);
  ai_chat_announce_project_memory(f, &info);
 }
 free(block);
 return len;
}

/**
 * ai_project_block_room - Bytes a text prompt must reserve for the project block.
 *
 * The chat's system prompt is one allocated buffer; sizing it with the
 * configured bound in mind means a raised AIProjectInstructionsMax reaches the
 * model instead of being dropped for want of room.
 */
static size_t ai_project_block_room(void)
{
 return (size_t)e_ai_project_max + AI_PROJECT_BLOCK_EXTRA;
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

/* Room for everything in the chat's system prompt but the project block, which
   is sized apart from its configured bound (ai_project_block_room). */
#define AI_CHAT_SYSTEM_BASE 8192

/* Build the chat system prompt: the assistant may INVESTIGATE the workspace
 * with read-only tools before answering, so questions about other files (not
 * just the open one) work.  Includes the workspace file listing, any live
 * diagnostics, and the current file for immediate context. */
/**
 * ai_chat_home_window - The file window the chat was armed from, if open.
 * @cn: the desktop.
 *
 * The chat's questions come through the pane, but the file the user asked
 * about is the window they pressed Alt-G a in; when that window has been
 * closed, or the chat was armed from the pane itself, there is no file.
 * Return: the window, or NULL.
 */
static FENSTER *ai_chat_home_window(ECNT *cn)
{
 int wi;
 if (g_ai_home_edt < 0) return NULL;
 for (wi = 1; wi <= cn->mxedt; wi++) {
  if (cn->edt[wi] != g_ai_home_edt) continue;
  if (ai_is_pane(cn->f[wi])) return NULL;
  return cn->f[wi];
 }
 return NULL;
}

/**
 * ai_append_selection - Add the marked block of the context window, if any.
 * @sys: the prompt; @cap: its size; @len: bytes written so far.
 * @ctx: the file window, or NULL.
 *
 * "What does this do?" about a marked block gets the block spelled out
 * beside the whole file, so the answer is about the lines the user marked.
 * Return: the new length.
 */
static size_t ai_append_selection(char *sys, size_t cap, size_t len, FENSTER *ctx)
{
 int y0, y1;
 char *sel;
 if (!ctx || !ai_block_range(ctx, &y0, &y1)) return len;
 sel = ai_lines_text(ctx, y0, y1);
 if (!sel) return len;
 if (len < cap - 512)
  len += (size_t)snprintf(sys + len, cap - len,
                          "\n--- selection (lines %d-%d) ---\n%.*s",
                          y0 + 1, y1 + 1, (int)(cap - len - 64), sel);
 free(sel);
 return len;
}

/* The chat's tool list, as the model reads it: the workspace tools always,
   the two web tools only while web access is on, so the model is never
   offered a tool the chat would refuse. */
#define AI_CHAT_TOOLS_OPEN \
   "You are an AI assistant embedded in the xwpe console editor. Answer " \
   "concisely in plain text.\n" \
   "You may INVESTIGATE the workspace before answering.  To read a file or " \
   "search, reply with EXACTLY ONE line and nothing else:\n" \
   "  TOOL read_file <path>\n" \
   "  TOOL grep <pattern>\n" \
   "  TOOL list_dir <path>\n" \
   "  TOOL glob <name-pattern>   (e.g. *.c -- find files by name across the tree)\n"
#define AI_CHAT_TOOLS_WEB \
   "  TOOL web_search <query>\n" \
   "  TOOL web_fetch <url>\n" \
   "Use the workspace first; use the web for what changed after you were " \
   "trained, and name the pages you used.\n"
#define AI_CHAT_TOOLS_CLOSE \
   "I will reply with the result; then either use another tool or give your " \
   "answer.  When you can answer, reply with the answer directly (no TOOL " \
   "line).  Do not guess about files you have not read.\n\n"
/* The Agent's line of the capability manifest, by the web switch: with the
   web off it says whose the web tools are, so a model does not ask the chat
   for them (which is what left a turn dead in silence). */
#define AI_CAPS_AGENT_WEB_OFF \
   "Autonomous Agent (the read tools above PLUS web_search and web_fetch -- " \
   "the Agent's tools, not the chat's: the chat has no web tool while web " \
   "access is off in Options > AI -- plus run_command and write_file, gated " \
   "by the permission dial ask/edits/auto): Alt-G g."
#define AI_CAPS_AGENT_WEB_ON \
   "Autonomous Agent (the read tools above PLUS web_search and web_fetch, " \
   "which the chat has too, run_command and write_file, gated by the " \
   "permission dial ask/edits/auto): Alt-G g."

/**
 * ai_append_tool_list - Open the system prompt with the tools the chat runs.
 * @sys: the prompt; @cap: its size; @len: bytes written so far.
 *
 * The model is offered exactly what the chat will run, so a turn never
 * asks for a web tool the switch would refuse.  Return: the new length.
 */
static size_t ai_append_tool_list(char *sys, size_t cap, size_t len)
{
 len += (size_t)snprintf(sys + len, cap - len, "%s", AI_CHAT_TOOLS_OPEN);
 if (wpe_ai_web_state() == WPE_AI_WEB_ON)
  len += (size_t)snprintf(sys + len, cap - len, "%s", AI_CHAT_TOOLS_WEB);
 len += (size_t)snprintf(sys + len, cap - len, "%s", AI_CHAT_TOOLS_CLOSE);
 return len;
}

/**
 * ai_agent_capability_sentence - The manifest's Agent sentence for this session.
 *
 * Names the web tools as the Agent's while the web is off, so the model
 * points the user to Alt-G g or Options > AI instead of asking the chat.
 * Return: the sentence to place in the capability manifest.
 */
static const char *ai_agent_capability_sentence(void)
{
 if (wpe_ai_web_state() == WPE_AI_WEB_ON) return AI_CAPS_AGENT_WEB_ON;
 return AI_CAPS_AGENT_WEB_OFF;
}

/**
 * ai_build_system - The chat's system prompt.
 * @f: the window the pane belongs to (project memory, identity).
 * @ctx: the file window whose text is the context, or NULL for none.
 *
 * The assistant may INVESTIGATE the workspace with read-only tools before
 * answering, so questions about other files (not just the open one) work.
 * Includes the workspace file listing, any live diagnostics, and the current
 * file -- the one the chat was opened from, never the pane's own transcript.
 */
static char *ai_build_system(FENSTER *f, FENSTER *ctx)
{
 char *ctx_text = ctx ? ai_current_file_text(ctx) : NULL;
 FENSTER *focused = ctx ? ctx : f;
 char **scope;
 int nsc, i;
 size_t cap = AI_CHAT_SYSTEM_BASE + ai_project_block_room(), len = 0;
 char *sys = malloc(cap);
 /* Capability manifest: tell the model what the xwpe AI integration ALREADY
    does, so it neither re-proposes built features as new ideas nor prints code
    for the user to retype -- it should point them to the shortcut that does the
    job.  Grounds every meta-question ("how could we improve this?") in reality. */
 const char *caps_open =
   "XWPE AI CAPABILITIES that already exist -- do NOT propose these as new; when "
   "the user would benefit, point them to the shortcut:\n"
   "- Edit the current file or selection: Alt-G e (applies your instruction; the "
   "change previews as a diff and Ctrl-U reverts it as one undo step).\n"
   "- Multi-file edit: Alt-G f.  ";
 const char *caps_rest =
   "  Build & fix until the compile passes: Alt-G b.\n"
   "- Replies stream token by token; Esc cancels a run mid-generation.\n"
   "- Proposed edits are reviewed as a diff changeset (Alt-T / Alt-V) and are "
   "revertible; a checkpoint is taken before a non-interactive run.\n"
   "- You already receive the open files, the workspace file list, the current "
   "file, live language-server diagnostics, and the project's instructions "
   "file (.xwpe-ai.md or AGENTS.md) when one is folded in (all below).\n"
   "- Backend and model are user-selectable (Ollama, OpenAI-compatible, Claude) "
   "in Options > AI.\n"
   "So when the user asks to CHANGE code, tell them to use Edit (Alt-G e) or the "
   "Agent (Alt-G g) rather than printing code for them to retype.\n\n";
 if (!sys) { free(ctx_text); return NULL; }
 len = ai_append_tool_list(sys, cap, len);
 len += (size_t)snprintf(sys + len, cap - len, "%s", caps_open);
 len += (size_t)snprintf(sys + len, cap - len, "%s", ai_agent_capability_sentence());
 len += (size_t)snprintf(sys + len, cap - len, "%s", caps_rest);
 len = ai_append_project_memory(sys, cap, len, f);
 len = ai_append_identity(sys, cap, len);
 len = ai_append_open_files(sys, cap, len, focused);
 nsc = wpe_ai_scope_files(f, e_project_is_open(), 1, &scope);
 if (len < cap - 32)
  len += (size_t)snprintf(sys + len, cap - len, "WORKSPACE FILES:\n");
 for (i = 0; i < nsc && len < cap - 256; i++)
  len += (size_t)snprintf(sys + len, cap - len, "  %s\n", scope[i]);
 wpe_ai_free_list(scope, nsc);
 { char diag[1700];
   if (ai_diag_block(diag, sizeof diag)[0] && len < cap - 1800)
    len += (size_t)snprintf(sys + len, cap - len, "%s", diag); }
 len = ai_append_selection(sys, cap, len, ctx);
 if (ctx_text && len < cap - 512)
  snprintf(sys + len, cap - len, "\n--- current file ---\n%.*s",
           (int)(cap - len - 32), ctx_text);
 free(ctx_text);
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
  if (!strncmp(scan, AI_TOOL_PREFIX, AI_TOOL_PREFIX_LEN)) return scan;
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
 char *nm = action + AI_TOOL_PREFIX_LEN, *sp = strchr(nm, ' ');
 size_t l;
 if (sp) { *sp = '\0'; *arg = sp + 1; } else *arg = (char *)"";
 l = strlen(nm);
 if (l && nm[l - 1] == ':') nm[l - 1] = '\0';
 *tool = nm;
}

/* A TOOL line, and so a tool argument, is at most this long: every loop copies
   the model's line into a buffer of this size before splitting it.  The shell
   quoter writes an embedded quote as four bytes and adds the two enclosing
   quotes, so the quoted form of a full-length argument needs four times the
   line and a little; the command buffer adds room for the fixed command text. */
#define AI_TOOL_LINE_MAX       1100
#define AI_TOOL_ARG_QUOTED_MAX (4 * AI_TOOL_LINE_MAX + 4)
#define AI_TOOL_CMD_MAX        (AI_TOOL_ARG_QUOTED_MAX + 64)

/* The read-only tools that run a shell command (read_file reads the file itself). */
#define AI_TOOL_LIST_DIR "list_dir"
#define AI_TOOL_GREP     "grep"
#define AI_TOOL_GLOB     "glob"
/* The tools the permission dial governs: commands (and the web, which is
   gated like a command) and writes. */
#define AI_TOOL_RUN_COMMAND "run_command"
#define AI_TOOL_WEB_FETCH   "web_fetch"
#define AI_TOOL_WEB_SEARCH  "web_search"
#define AI_TOOL_WRITE_FILE  "write_file"
#define AI_TOOL_APPLY_PATCH "apply_patch"
/* The tools that ask nothing: a bounded file read, and the agent's own
   checklist (laid out, then ticked step by step). */
#define AI_TOOL_READ_FILE   "read_file"
#define AI_TOOL_PLAN        "plan"
#define AI_TOOL_STEP_DONE   "step_done"
/* What the model reads when the user refused a tool, whichever tool it was. */
#define AI_TOOL_DENIED_RESULT "(denied by user)"
/* How the model reads a tool that could not do what it asked. */
#define AI_TOOL_ERROR_HEAD "TOOL ERROR: "
#define AI_TOOL_ERROR_HEAD_LEN (sizeof AI_TOOL_ERROR_HEAD - 1)
/* What the model reads for a tool name it made up. */
#define AI_TOOL_UNKNOWN_RESULT "(unknown tool)"
/* The line that closes a tool body (a write, a patch, a plan), and the same
   marker at the body's start. */
#define AI_TOOL_BODY_END_LINE "\n@@END"
#define AI_TOOL_BODY_END_WORD "@@END"

/**
 * ai_tool_is_shell_readonly - Is this tool one of the read-only shell tools?
 * @tool: the name after "TOOL ".
 *
 * list_dir, grep and glob run without asking under every dial, so they are
 * the tools whose argument must be made harmless before it reaches the shell.
 * Return: 1 for those three, 0 for anything else.
 */
static int ai_tool_is_shell_readonly(const char *tool)
{
 if (!strcmp(tool, AI_TOOL_LIST_DIR)) return 1;
 if (!strcmp(tool, AI_TOOL_GREP)) return 1;
 if (!strcmp(tool, AI_TOOL_GLOB)) return 1;
 return 0;
}

/* What list_dir and glob stand for when the model gives no argument: the
   current directory, and every name. */
#define AI_TOOL_LIST_DIR_DEFAULT "."
#define AI_TOOL_GLOB_DEFAULT     "*"

/**
 * ai_tool_quote_or - Quote the model's argument for the shell, or a default when it gave none.
 * @quoted/@n: receives the one shell word; @arg: the argument as the model
 * wrote it; @dflt: what an empty argument means for this tool.
 *
 * The quoting itself is wpe_ai_shell_quote's, unchanged: this only chooses
 * which text it quotes, so "TOOL list_dir" with nothing after it lists the
 * current directory as it always did.
 */
static void ai_tool_quote_or(char *quoted, size_t n, const char *arg, const char *dflt)
{
 if (arg[0]) wpe_ai_shell_quote(quoted, n, arg);
 else wpe_ai_shell_quote(quoted, n, dflt);
}

/**
 * ai_tool_shell_command - Build the shell command for a read-only tool.
 * @tool: list_dir, grep or glob; @arg: the model's argument, exactly as it
 * wrote it (may be empty); @cmd/@sz: receives the command.
 *
 * The argument reaches /bin/sh as exactly one word, quoted with the same
 * quoter the hook path and the checkpoint use.  So "two words" is one grep
 * pattern, not a pattern and a file; and a line a fetched web page planted in
 * the model's context, such as ". ; touch x", lists an odd name instead of
 * running a second command that no dial was ever asked about.  An empty
 * argument lists the current directory or matches every name, as before.
 * The chat loop, the agent loop and plan mode all build their commands here,
 * so one site cannot be fixed and another left behind.
 *
 * Return: 1 when @tool is one of the three and @cmd is filled, 0 otherwise.
 */
static int ai_tool_shell_command(const char *tool, const char *arg, char *cmd, size_t sz)
{
 char quoted[AI_TOOL_ARG_QUOTED_MAX];
 if (!strcmp(tool, AI_TOOL_LIST_DIR)) {
  ai_tool_quote_or(quoted, sizeof quoted, arg, AI_TOOL_LIST_DIR_DEFAULT);
  snprintf(cmd, sz, "ls -la -- %s", quoted);   /* "--": a name that starts with "-" is a name, not an option */
  return 1;
 }
 if (!strcmp(tool, AI_TOOL_GREP)) {
  wpe_ai_shell_quote(quoted, sizeof quoted, arg);
  snprintf(cmd, sz, "grep -rn -- %s .", quoted);
  return 1;
 }
 if (!strcmp(tool, AI_TOOL_GLOB)) {
  ai_tool_quote_or(quoted, sizeof quoted, arg, AI_TOOL_GLOB_DEFAULT);
  snprintf(cmd, sz, "find . -name %s -not -path '*/.*' 2>/dev/null | head -200", quoted);
  return 1;
 }
 return 0;
}

/**
 * ai_tool_run_readonly - Run list_dir, grep or glob and capture its output.
 * @tool: the tool name; @arg: the model's argument, as it wrote it.
 *
 * The one place the three read-only shell tools are run from, whichever loop
 * asked, so the user gets the same one-word guarantee from a chat
 * investigation, an agent run and a plan study.
 * Return: the malloc'd output (the caller frees), NULL when @tool is not one
 * of the three or the command could not be started.
 */
static char *ai_tool_run_readonly(const char *tool, const char *arg)
{
 char cmd[AI_TOOL_CMD_MAX];
 if (!ai_tool_shell_command(tool, arg, cmd, sizeof cmd)) return NULL;
 return ai_run_capture(cmd);
}

/* The lead of the result a refused tool line gets, and the pane's reason. */
#define AI_CHAT_REFUSED_UNKNOWN "that tool is not available in the chat"
#define AI_CHAT_REFUSED_WHY_UNKNOWN "no such tool"

/**
 * ai_tool_is_web - Is this tool one of the two web tools?
 * @tool: the name after "TOOL ".
 * Return: 1 for web_search and web_fetch, 0 for anything else.
 */
static int ai_tool_is_web(const char *tool)
{
 if (!strcmp(tool, AI_TOOL_WEB_SEARCH)) return 1;
 if (!strcmp(tool, AI_TOOL_WEB_FETCH)) return 1;
 return 0;
}

/**
 * ai_chat_tool_result - Run the tool a chat reply asks for, or refuse it aloud.
 * @s: the chat; @reply: the model's whole reply.
 *
 * The workspace tools run read-only; a web tool goes through the web switch
 * (and its first-use notice) and then the permission dial; a tool the chat
 * cannot run -- a web tool while the web is off, a name the model invented
 * -- is answered with a result naming the tools it may use, so the turn
 * goes on to an answer instead of dying in silence.  The tool line is found
 * past any reasoning preamble (ai_scan_tool_line).
 * Return: the malloc'd result the model reads next, NULL when the reply has
 * no tool line and is the answer.
 */
static char *ai_chat_tool_result(ai_chat_session *s, const char *reply)
{
 char action[AI_TOOL_LINE_MAX], *tool, *arg;
 const char *line, *nl;
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
 if (ai_tool_is_shell_readonly(tool))
  return ai_tool_run_readonly(tool, arg);
 if (ai_tool_is_web(tool))
  return ai_chat_run_web_tool(s, tool, arg);
 return ai_chat_refuse_tool(s->ref, tool, AI_CHAT_REFUSED_UNKNOWN,
                            AI_CHAT_REFUSED_WHY_UNKNOWN);
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

 sys = ai_build_system(f, ai_chat_home_window(f->ed));
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
  ai_pane(f, err[0] ? err : "[AI error] could not start", AI_ROLE_ERROR, 0);
  ai_chat_turn_over(s);
  return;
 }
 s->fd = wpe_ai_stream_fd(s->st);
 s->active = 1;
 s->started = 0;
 s->decided = AI_REPLY_UNDECIDED;
 s->hide_tool = 0;
 s->shown = 0;
 s->flen = 0; if (s->full) s->full[0] = '\0';
 ai_chat_set_pending(s, AI_REPLY_PREFIX);     /* reply streams after "AI: " */
 ai_chat_wait_begin(s);                       /* the waiting line, spun by the heartbeat */
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

/**
 * ai_chat_refresh_header - Precede the turn with a header if the setup changed.
 * @f: the window the pane belongs to.
 *
 * Options > AI closed with Ok, Alt-G y, or a provider switch since the last
 * header: the user reads the backend, model and dial this turn runs under,
 * right above their question, and the transcript above stays as it was.
 */
static void ai_chat_refresh_header(FENSTER *f)
{
 FENSTER *wf = ai_pane_win(f);
 if (wf)
  ai_chat_emit_header_if_due(wf);
}

/**
 * ai_chat_echo_question - Show the user's own line in the transcript.
 * @f: the window the pane belongs to; @text: the question as typed.
 *
 * One transcript line per input line, so a multi-line prompt reads cleanly
 * ("You: ..." then indented continuations) instead of showing an embedded
 * newline as a control glyph.
 */
static void ai_chat_echo_question(FENSTER *f, const char *text)
{
 char line[AI_PROMPT_MAX + 8];
 const char *p = text;
 int first = 1;
 while (first || *p) {
  const char *nl = strchr(p, '\n');
  int seg = nl ? (int)(nl - p) : (int)strlen(p);
  snprintf(line, sizeof line, "%s%.*s", first ? "You: " : "     ", seg, p);
  ai_pane(f, line, AI_ROLE_USER, 0);           /* inserts above the input region */
  first = 0;
  if (!nl) break;
  p = nl + 1;
 }
}

/**
 * ai_chat_launch - Start one chat turn now: echo, record, stream.
 * @f: the window the pane belongs to; @text: the question as typed.
 *
 * The one launch for a question sent while nothing runs and for a question
 * that waited in the queue behind a running task: either way the user sees
 * "You: ..." and the waiting line at the moment the turn actually starts.
 * The turn is registered as the running task first, so from here Esc
 * cancels it and a further question queues behind it.
 */
static void ai_chat_launch(FENSTER *f, const char *text)
{
 ai_chat_session *s;
 char expanded[AI_PROMPT_MAX];
 const char *tosend = text;

 if (ai_slash_expand(text, expanded, sizeof expanded))
  tosend = expanded;              /* echo what was typed; send the expansion */
 wpe_ai_trace("chat prompt=%s", tosend);
 ai_chat_refresh_header(f);
 ai_chat_echo_question(f, text);
 wpe_ai_session_append("user", tosend);
 s = calloc(1, sizeof *s);
 if (!s) return;
 s->ref = f;
 g_ai_chat = s;
 ai_chat_op_begin(s);
 /* Stream the reply; the model may take a few read-only tool turns first to
    investigate the workspace, all in the background (see ai_fd_cb). */
 ai_chat_next_turn(s);
}

/**
 * e_ai_chat_send - Send the input row's text as a chat turn, or queue it.
 * @f: the window the pane belongs to; @text: the question as typed.
 *
 * While a reply is still streaming, or an Edit, Complete, Multi-file or
 * Agent task runs, the question is queued the way a second Edit is: the pane
 * says so, the running turn is not cut, and the question starts by itself
 * when the running one ends.
 */
static void e_ai_chat_send(FENSTER *f, const char *text)
{
 if (wpe_ai_busy()) {
  ai_enqueue(f, ai_chat_launch, text);
  return;
 }
 ai_chat_launch(f, text);
}

/* ----- input-row editing: a small line editor over g_ai_input ------------- */

/**
 * ai_in_prev - Byte index of the character before @p in the input.
 *
 * Steps over a whole multibyte character, so Left and Backspace never land
 * inside one.
 */
static int ai_in_prev(int p)
{
 if (p <= 0) return 0;
 p--;
 while (p > 0 && ai_utf8_continues(g_ai_input[p])) p--;
 return p;
}
/**
 * ai_in_next - Byte index of the character after @p in the input.
 *
 * Steps over a whole multibyte character, so Right and Delete never land
 * inside one.
 */
static int ai_in_next(int p)
{
 if (p >= g_ai_input_len) return g_ai_input_len;
 p++;
 while (p < g_ai_input_len && ai_utf8_continues(g_ai_input[p])) p++;
 return p;
}
/**
 * ai_in_bol - Start of the input line holding byte @p.
 *
 * Home and the arrows work on the line of a multi-line prompt, not on the
 * whole input.
 */
static int ai_in_bol(int p)
{
 while (p > 0 && g_ai_input[p - 1] != '\n') p--;
 return p;
}
/**
 * ai_in_eol - End of the input line holding byte @p.
 */
static int ai_in_eol(int p)
{
 while (p < g_ai_input_len && g_ai_input[p] != '\n') p++;
 return p;
}
/**
 * ai_in_fits - Would @n more bytes keep the input under AI_PROMPT_MAX?
 *
 * The one place that knows the limit, so the key that is refused and the
 * message that says why always agree on where the row ends.
 */
static int ai_in_fits(int n)
{
 return g_ai_input_len + n < (int)sizeof g_ai_input;
}

/**
 * ai_in_insert - Put @n bytes into the input at the caret.
 *
 * Returns 1 when the bytes are in the row, 0 when they would take it past
 * its limit and were left out, so the caller can tell the user instead of
 * letting the row silently stop growing.  A multi-byte character is taken
 * whole or not at all: the row never ends in a broken character.
 */
static int ai_in_insert(const char *bytes, int n)
{
 int i;
 if (n <= 0) return 1;
 if (!ai_in_fits(n)) return 0;
 memmove(g_ai_input + g_ai_input_pos + n, g_ai_input + g_ai_input_pos,
         (size_t)(g_ai_input_len - g_ai_input_pos));
 for (i = 0; i < n; i++) g_ai_input[g_ai_input_pos + i] = bytes[i];
 g_ai_input_len += n;
 g_ai_input_pos += n;
 g_ai_input[g_ai_input_len] = '\0';
 return 1;
}

/**
 * ai_in_refuse_at_limit - Say on the status row that the input is full.
 * @wf: the chat pane (the bar row belongs to its screen).
 *
 * The key that did not fit is dropped; without a word about it the row just
 * stops growing and the user wonders whether the keyboard died.  The bar
 * shows "Input at its limit (N bytes)" with the real limit, and the next key
 * restores the key hints.  A paste that crosses the limit keeps what fitted
 * and leaves this same line on the bar when it ends.
 */
static void ai_in_refuse_at_limit(FENSTER *wf)
{
 char msg[80];
 snprintf(msg, sizeof msg, " Input at its limit (%d bytes)", AI_PROMPT_MAX);
 wpe_ai_flash(wf, msg);
}

/**
 * ai_in_type - Take @n bytes the user typed or pasted and show the result.
 * @wf: the chat pane.
 *
 * The one entry point for text entering the row: the row repaints when the
 * bytes fit, the status row says so when they do not.  A refused key is
 * never a silent nothing.
 */
static void ai_in_type(FENSTER *wf, const char *bytes, int n)
{
 if (!ai_in_insert(bytes, n)) {
  ai_in_refuse_at_limit(wf);
  return;
 }
 ai_input_render(wf);
}

/**
 * ai_in_newline - Break the input line at the caret.
 * @wf: the chat pane.
 *
 * Ctrl-J and a line break inside a paste both land here, so a multi-line
 * prompt looks the same however its lines were entered.
 */
static void ai_in_newline(FENSTER *wf)
{
 ai_in_type(wf, "\n", 1);
}

extern int g_bracketed_paste;           /* we_term.c: set while a paste streams */

/**
 * ai_in_paste_break - Is key @c a line break arriving inside a terminal paste?
 *
 * While a bracketed paste streams, Enter is not the Send command: the line
 * breaks in the pasted text are part of it.  Nothing leaves the row until
 * the user presses Enter after the paste, so a multi-line clipboard lands in
 * the input as it was copied instead of firing one turn per line.  The
 * terminal reader delivers a pasted LF as Enter, and the LF of a CR LF pair
 * not at all, so one key here is one line break.
 */
static int ai_in_paste_break(int c)
{
 if (!g_bracketed_paste) return 0;
 return (c == WPE_CR || c == WPE_WR);
}
/**
 * ai_in_cut - Take the bytes [@from, @to) out of the input.
 *
 * The text after the cut closes up and the row stays terminated; the caret
 * is the caller's to place.
 */
static void ai_in_cut(int from, int to)
{
 memmove(g_ai_input + from, g_ai_input + to, (size_t)(g_ai_input_len - to));
 g_ai_input_len -= (to - from);
 g_ai_input[g_ai_input_len] = '\0';
}

/**
 * ai_in_backspace - Delete the character before the caret, whole.
 */
static void ai_in_backspace(void)
{
 int q;
 if (g_ai_input_pos <= 0) return;
 q = ai_in_prev(g_ai_input_pos);
 ai_in_cut(q, g_ai_input_pos);
 g_ai_input_pos = q;
}

/**
 * ai_in_delete - Delete the character at the caret, whole.
 */
static void ai_in_delete(void)
{
 if (g_ai_input_pos >= g_ai_input_len) return;
 ai_in_cut(g_ai_input_pos, ai_in_next(g_ai_input_pos));
}

/**
 * ai_in_caret_on_line - Put the caret at column @col of the line [@bol, @eol].
 *
 * A shorter line takes the caret at its end, as the arrows do in the editor.
 */
static void ai_in_caret_on_line(int bol, int eol, int col)
{
 g_ai_input_pos = bol + col;
 if (g_ai_input_pos > eol)
  g_ai_input_pos = eol;
}

/**
 * ai_in_vmove - Move the caret a line up (@dir < 0) or down, keeping its column.
 *
 * Return: 1 when the caret moved, 0 at the first or last line of the input.
 * The callers decide what the key does there: Up browses the transcript,
 * Down does nothing.
 */
static int ai_in_vmove(int dir)
{
 int bol = ai_in_bol(g_ai_input_pos);
 int col = g_ai_input_pos - bol;
 int eol;
 if (dir < 0) {
  if (bol == 0)
   return 0;
  ai_in_caret_on_line(ai_in_bol(bol - 1), bol - 1, col);
  return 1;
 }
 eol = ai_in_eol(g_ai_input_pos);
 if (eol >= g_ai_input_len)
  return 0;
 ai_in_caret_on_line(eol + 1, ai_in_eol(eol + 1), col);
 return 1;
}

/**
 * ai_input_clear - Empty the input text and put the caret at its start.
 *
 * What was typed is gone from the row: after Enter sent it, and when the
 * chat is armed afresh.
 */
static void ai_input_clear(void)
{
 g_ai_input[0] = '\0';
 g_ai_input_len = 0;
 g_ai_input_pos = 0;
}

/**
 * ai_input_drop_rows - Remove the whole input region from the pane.
 * @b: the pane buffer.
 *
 * The transcript above it stays; the pane reads as the plain output window
 * it was before the chat.
 */
static void ai_input_drop_rows(BUFFER *b)
{
 while (g_ai_input_rows > 0 && b->mxlines > 0)
  ai_input_drop_last_row(b);
 ai_roles_sync(b);
}

/**
 * ai_window_slot - Where window @w stands in the desktop's window list.
 * @f: any window of the desktop.
 *
 * Return: its index, 0 when it is not on the desktop.  The index names the
 * window to e_switch_window, which brings it to the front.
 */
static int ai_window_slot(FENSTER *f, FENSTER *w)
{
 int wi;
 for (wi = 1; wi <= f->ed->mxedt; wi++)
  if (f->ed->f[wi] == w) return wi;
 return 0;
}

/**
 * e_ai_chat_close - Esc: leave the chat.
 *
 * The "> " input region goes and focus returns to the file the chat was
 * opened from; the transcript stays in the (now ordinary) pane to be read
 * or copied.
 */
static void e_ai_chat_close(void)
{
 FENSTER *wf = g_ai_chat_pane;
 if (!wf) return;
 g_ai_chat_focus = 0;
 g_ai_chat_pane = NULL;
 ai_input_drop_rows(wf->b);
 ai_pane_paint(wf);
 if (g_ai_home_edt >= 0)
  e_switch_window(g_ai_home_edt, wf);
}

/**
 * ai_chat_remember_home - Note the window Esc returns to when the chat closes.
 * @f: the window the chat was opened from.
 */
static void ai_chat_remember_home(FENSTER *f)
{
 int wi = ai_window_slot(f, f);
 g_ai_home_edt = -1;
 if (wi)
  g_ai_home_edt = f->ed->edt[wi];
}

/**
 * ai_chat_raise_pane - Bring the pane in front of the other windows.
 * @f: the window the request came from.
 * @wf: the pane.
 *
 * The user sees the transcript and the input row without hunting for them
 * under the file they were editing.
 */
static void ai_chat_raise_pane(FENSTER *f, FENSTER *wf)
{
 int wi = ai_window_slot(f, wf);
 if (wi)
  e_switch_window(f->ed->edt[wi], wf);
}

/**
 * e_ai_chat_arm - Open the chat: dock and focus the pane, add the "> " row.
 * @f: the window the chat was asked for from.
 *
 * From here the pane is an ORDINARY window the main editor loop drives, so
 * mouse, scrolling, resizing and switching to other windows all work; only
 * the input keys are intercepted (see e_ai_chat_key).
 */
static void e_ai_chat_arm(FENSTER *f)
{
 FENSTER *wf;

 ai_chat_remember_home(f);
 wf = ai_pane_win(f);
 if (!wf) return;
 ai_chat_raise_pane(f, wf);
 wf = ai_pane_win(f);
 if (!wf) return;

 g_ai_chat_pane = wf;
 g_ai_chat_focus = 1;
 g_ai_scroll_lock = 0;
 ai_input_clear();
 g_ai_input_rows = 0;
 if (wf->b->mxlines == 0) e_new_line(0, wf->b);
 ai_roles_sync(wf->b);
 /* The header when the setup changed (or was never shown), the hints once per
    pane: whatever another action wrote to the pane before this arm. */
 ai_chat_emit_header_if_due(wf);
 ai_chat_emit_hints_once(wf);
 ai_input_render(wf);                            /* creates the input region */
}

/**
 * ai_chat_focus_pane - Bring the armed chat pane to the front.
 * @f: the window the request came from.
 *
 * The user's next keystroke lands in the input row; nothing is appended to
 * the transcript.
 */
static void ai_chat_focus_pane(FENSTER *f)
{
 int wi = ai_window_slot(f, g_ai_chat_pane);
 if (wi)
  e_switch_window(f->ed->edt[wi], f);
}

/**
 * ai_chat_offer_input - Leave the pane ready to type in after a cancel.
 * @f: the window the cancelled task belonged to.
 *
 * The user can continue right there (ask a follow-up, a new question)
 * instead of facing a dead pane.  A chat that is already armed keeps its
 * input row and its home window; arming it again would leave a second input
 * row in the transcript and forget the file Esc returns to.
 */
static void ai_chat_offer_input(FENSTER *f)
{
 if (g_ai_chat_pane && g_ai_chat_focus) {
  ai_chat_focus_pane(f);
  return;
 }
 e_ai_chat_arm(f);
}

/**
 * ai_chat_focused_pane - The armed chat pane when it is the window on top.
 * @f: the window the editor loop believes is focused.
 *
 * The editor's key loop refreshes its own `f` only at the END of an iteration,
 * so right after the chat was armed from an ASYNC callback (the agent finishing
 * and arming a follow-up), the first key arrives with a STALE `f` -- the window
 * that was focused before the arm.  Answering from the window stack instead
 * sends that first key (or wheel step) to the pane rather than leaking it into
 * the old file.  NULL when the chat is not armed or the user really switched
 * to another window, so the editor keeps the key.
 */
static FENSTER *ai_chat_focused_pane(FENSTER *f)
{
 if (!g_ai_chat_focus || !g_ai_chat_pane)
  return NULL;
 if (f == g_ai_chat_pane)
  return g_ai_chat_pane;
 if (ai_top_window(f) == g_ai_chat_pane)
  return g_ai_chat_pane;
 return NULL;
}

/**
 * e_ai_chat_wheel - One mouse-wheel step while the chat pane is on top.
 * @f: the window the editor loop believes is focused.
 * @c: the key code (WPE_SCROLL_UP or WPE_SCROLL_DOWN).
 *
 * Browses the transcript three lines per step through the same scroller as
 * PgUp/PgDn and the arrows, so the view locks where the wheel left it and the
 * next reply line does not re-pin it to the input row.  Returns 1 when the
 * step was the pane's; 0 lets the editor scroll the focused window as before.
 */
int e_ai_chat_wheel(FENSTER *f, int c)
{
 FENSTER *wf = ai_chat_focused_pane(f);

 if (!wf)
  return 0;
 if (c == WPE_SCROLL_UP) {
  ai_pane_scroll(wf, -WPE_WHEEL_LINES);
  return 1;
 }
 if (c == WPE_SCROLL_DOWN) {
  ai_pane_scroll(wf, +WPE_WHEEL_LINES);
  return 1;
 }
 return 0;
}

/* What a key does to the input row.  The one list of the keys the chat takes
   from the editor: the key handler reads it to act, and a key not in it goes
   back to the editor with the browsed view left where the user put it. */
enum ai_chat_key_action {
 AI_KEY_NOT_INPUT = 0,   /* the editor's: a window switch, a function key */
 AI_KEY_PASTE_BREAK,     /* a line break inside a terminal paste          */
 AI_KEY_SEND,            /* Enter                                         */
 AI_KEY_NEWLINE,         /* Ctrl-J                                        */
 AI_KEY_BACKSPACE,
 AI_KEY_DELETE,
 AI_KEY_LEFT,
 AI_KEY_RIGHT,
 AI_KEY_BOL,             /* Home, Ctrl-A                                  */
 AI_KEY_EOL,             /* End, Ctrl-E                                   */
 AI_KEY_CHAR             /* a printable character, ASCII or Unicode       */
};

/* The key codes that are a typed character: from the space up to, not
   including, the editor's byte limit; a code point past it arrives with
   e_input_was_char set by the key reader. */
#define AI_PRINTABLE_FIRST 32
#define AI_PRINTABLE_END   255
#define AI_ASCII_END       0x80   /* the first code point that needs UTF-8 */
#define AI_UTF8_MAX        4      /* bytes a code point takes at most      */

/**
 * ai_chat_key_is_printable - Is key @c a character to put in the input?
 */
static int ai_chat_key_is_printable(int c)
{
 if (c >= AI_PRINTABLE_FIRST && c < AI_PRINTABLE_END)
  return 1;
 return e_input_was_char;
}

/**
 * ai_chat_key_action - What key @c does to the input row.
 *
 * Return: the action, AI_KEY_NOT_INPUT for a key that is the editor's.  Only
 * an input key ends a browse of the transcript: a window switch (the key a
 * click on the file window turns into) or a function key leaves the browsed
 * view where the user left it, so clicking into the file to work there does
 * not lose the place being read in the pane.
 */
static enum ai_chat_key_action ai_chat_key_action(int c)
{
 if (ai_in_paste_break(c)) return AI_KEY_PASTE_BREAK;
 if (c == WPE_CR) return AI_KEY_SEND;
 if (c == WPE_WR) return AI_KEY_NEWLINE;
 if (c == WPE_DC) return AI_KEY_BACKSPACE;
 if (c == ENTF) return AI_KEY_DELETE;
 if (c == CLE) return AI_KEY_LEFT;
 if (c == CRI) return AI_KEY_RIGHT;
 if (c == POS1 || c == CtrlA) return AI_KEY_BOL;
 if (c == ENDE || c == CtrlE) return AI_KEY_EOL;
 if (ai_chat_key_is_printable(c)) return AI_KEY_CHAR;
 return AI_KEY_NOT_INPUT;
}

/**
 * ai_chat_send_input - Enter: send the whole input as the next question.
 * @wf: the pane.
 *
 * The row empties as the question goes out, so the echo "You: ..." above
 * and the fresh "> " row tell the user it was taken.  An empty row sends
 * nothing.
 */
static void ai_chat_send_input(FENSTER *wf)
{
 char sb[AI_PROMPT_MAX];
 if (g_ai_input_len <= 0)
  return;
 strncpy(sb, g_ai_input, sizeof sb - 1);
 sb[sizeof sb - 1] = '\0';
 ai_input_clear();
 ai_input_render(wf);
 e_ai_chat_send(wf, sb);
}

/**
 * ai_chat_type_char - Put the character key @c into the input row.
 * @wf: the pane.
 *
 * A code point past ASCII goes in as its UTF-8 bytes, so an accented or
 * CJK character typed on any keyboard layout lands whole.
 */
static void ai_chat_type_char(FENSTER *wf, int c)
{
 unsigned char u8[AI_UTF8_MAX];
 int n;
 if (c >= AI_ASCII_END) {
  n = e_codepoint_to_utf8(c, u8);
 } else {
  u8[0] = (unsigned char)c;
  n = 1;
 }
 ai_in_type(wf, (char *)u8, n);
}

/**
 * ai_chat_input_act - Carry out input @action for key @c and show the result.
 * @wf: the pane.
 *
 * Every edit repaints the row, so the caret and the text never lag behind
 * the key.
 */
static void ai_chat_input_act(FENSTER *wf, enum ai_chat_key_action action, int c)
{
 switch (action) {
 case AI_KEY_PASTE_BREAK:                /* pasted text is literal */
 case AI_KEY_NEWLINE:
  ai_in_newline(wf);
  return;
 case AI_KEY_SEND:
  ai_chat_send_input(wf);
  return;
 case AI_KEY_CHAR:
  ai_chat_type_char(wf, c);
  return;
 case AI_KEY_BACKSPACE:
  ai_in_backspace();
  break;
 case AI_KEY_DELETE:
  ai_in_delete();
  break;
 case AI_KEY_LEFT:
  g_ai_input_pos = ai_in_prev(g_ai_input_pos);
  break;
 case AI_KEY_RIGHT:
  g_ai_input_pos = ai_in_next(g_ai_input_pos);
  break;
 case AI_KEY_BOL:
  g_ai_input_pos = ai_in_bol(g_ai_input_pos);
  break;
 case AI_KEY_EOL:
  g_ai_input_pos = ai_in_eol(g_ai_input_pos);
  break;
 default:
  return;
 }
 ai_input_render(wf);
}

/**
 * ai_chat_key_up - The Up arrow in the chat pane.
 * @wf: the pane.
 *
 * Moves within a multi-line input; at its first line (or a one-line input)
 * it browses the transcript, so the history can be read back with the
 * arrows.  While a browse is on it keeps browsing, older.
 */
static void ai_chat_key_up(FENSTER *wf)
{
 if (g_ai_scroll_lock) {
  ai_pane_scroll(wf, -1);
  return;
 }
 if (ai_in_vmove(-1)) {
  ai_input_render(wf);
  return;
 }
 ai_pane_scroll(wf, -1);                /* top of the input: older */
}

/**
 * ai_chat_key_down - The Down arrow in the chat pane.
 * @wf: the pane.
 *
 * Moves within a multi-line input; while a browse is on it browses newer,
 * back towards the input row.  At the input's last line it does nothing.
 */
static void ai_chat_key_down(FENSTER *wf)
{
 if (g_ai_scroll_lock) {
  ai_pane_scroll(wf, +1);
  return;
 }
 if (ai_in_vmove(1))
  ai_input_render(wf);
}

/**
 * e_ai_chat_key - One key while the chat pane is the focused window.
 * @f: the window the editor loop believes is focused.
 * @c: the key code.
 *
 * Return: 1 when the key was the chat's (Esc, browsing, an input key), 0 to
 * let the editor handle it as usual (a window switch, a function key).  The
 * mouse is the editor's before this is reached, so dragging, resizing and
 * switching windows all keep working.
 */
int e_ai_chat_key(FENSTER *f, int c)
{
 FENSTER *wf = ai_chat_focused_pane(f);
 enum ai_chat_key_action action;

 if (!wf)
  return 0;
 if (c == WPE_ESC) {
  e_ai_chat_close();
  return 1;
 }
 if (c == BUP) {
  ai_pane_scroll(wf, -ai_pane_page_rows(wf));
  return 1;
 }
 if (c == BDO) {
  ai_pane_scroll(wf, +ai_pane_page_rows(wf));
  return 1;
 }
 /* The arrows come before the scroll-lock reset below, so they keep the
    browsed view. */
 if (c == CUP) {
  ai_chat_key_up(wf);
  return 1;
 }
 if (c == CDO) {
  ai_chat_key_down(wf);
  return 1;
 }
 action = ai_chat_key_action(c);
 if (action == AI_KEY_NOT_INPUT)
  return 0;                             /* the editor's key: the browsed view stays */
 g_ai_scroll_lock = 0;                  /* an input key returns to the input */
 ai_chat_input_act(wf, action, c);
 return 1;
}

/**
 * e_ai_chat - Alt-G: open the chat, or bring the open one to the front.
 * @f: the window the request came from.
 *
 * The assistant must be reachable and a model set; otherwise the pane says
 * what is missing and nothing else changes.  The workspace's saved talk is
 * resumed, and the user types straight into the pane's input row: Enter
 * sends, Ctrl-J breaks the line, Esc leaves.
 */
static int e_ai_chat(FENSTER *f)
{
 char err[320], line[400];

 if (g_ai_chat_pane) {                            /* already open: just refocus it */
  ai_chat_focus_pane(f);
  return 0;
 }
 err[0] = '\0';
 if (wpe_ai_preflight(e_ai_backend, err, sizeof err)) {
  snprintf(line, sizeof line, "[AI unavailable] %s", err);
  ai_pane(f, line, AI_ROLE_ERROR, 1);
  return 0;
 }
 err[0] = '\0';
 if (wpe_ai_ensure_model(err, sizeof err)) {
  snprintf(line, sizeof line, "[AI] %s", err[0] ? err : "no model set (set AIModel)");
  ai_pane(f, line, AI_ROLE_ERROR, 1);
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
 ai_pane(f, line, AI_ROLE_MACHINERY, 0);
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
 ai_pane(f, line, AI_ROLE_ERROR, 1);
 if (out && *out) ai_pane_multiline(f, out, AI_ROLE_MACHINERY, 0);
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
 ai_pane_multiline(f, copy, AI_ROLE_ASSISTANT, 0);
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
 ai_pane(f, AI_COMMENTARY_LINE, AI_ROLE_MACHINERY, 0);
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

/* The frame of a review box on the screen: its corners, its width and how many
   body rows it shows.  Laid out when the box opens and again after every
   terminal resize, so the box always fits the screen it is drawn on. */
typedef struct {
 int xa, ya, xe, ye;     /* the border corners */
 int boxw;               /* columns, border included */
 int vis;                /* body rows shown */
} ai_box_frame;

/* The box's close glyph is this many cells left of its right border on the
   title row: e_draw_titlebar_buttons paints every popup's close box at xe-2,
   and the dialog engine hit-tests the same cell. */
#define AI_BOX_CLOSE_FROM_RIGHT 2
/* Columns the box keeps clear of the screen's edges, both sides together. */
#define AI_BOX_SIDE_MARGIN 4
/* Border columns around the rows: the frame on the left and on the right,
   plus the cell the title row's glyphs need. */
#define AI_BOX_BORDER_COLS 3
/* Bit 0 of the mouse layer's button mask is the left button. */
#define AI_MOUSE_LEFT 1

/**
 * ai_box_content_width - The widest line the box has to show.
 * @rt/@nrows: the rows; @title/@hint: the caption lines.
 *
 * Return: columns of the widest row, title or hint; the frame is added by
 * ai_box_frame_fit, and a wider screen never changes it.
 */
static int ai_box_content_width(char **rt, int nrows, const char *title, const char *hint)
{
 int maxw = 0, k;
 for (k = 0; k < nrows; k++)
  if (rt[k] && (int)strlen(rt[k]) > maxw) maxw = (int)strlen(rt[k]);
 if (maxw < (int)strlen(title)) maxw = (int)strlen(title);
 if (maxw < (int)strlen(hint)) maxw = (int)strlen(hint);
 return maxw;
}

/**
 * ai_box_frame_fit - Lay the box out for the screen as it is right now.
 * @f: the window the box is about; @anchor_row: the screen row of the hunk's
 * first line (0 for a box about no line); @nrows: rows to show;
 * @content_width: from ai_box_content_width; @fr: receives the frame.
 *
 * What the user gains: the same layout rule places the box when it opens and
 * after a resize, so a box on a 60-column screen is 60 columns wide at most and
 * one next to a hunk stays inside the file window (ai_box_place_in_window),
 * whatever the screen was when it first opened.
 */
static void ai_box_frame_fit(const FENSTER *f, int anchor_row, int nrows,
                             int content_width, ai_box_frame *fr)
{
 fr->boxw = content_width + AI_BOX_BORDER_COLS;
 if (fr->boxw > MAXSCOL - AI_BOX_SIDE_MARGIN) fr->boxw = MAXSCOL - AI_BOX_SIDE_MARGIN;
 fr->xa = (MAXSCOL - fr->boxw) / 2;
 if (fr->xa < 1) fr->xa = 1;
 fr->xe = fr->xa + fr->boxw;
 if (anchor_row > 0)
  ai_box_place_in_window(f, anchor_row, nrows, &fr->ya, &fr->vis);
 else
  ai_box_place_at_top(nrows, &fr->ya, &fr->vis);
 fr->ye = fr->ya + fr->vis + 1;
}

static void ai_scroll_line_into_view(FENSTER *f, int line);
static int ai_line_screen_row(const FENSTER *f, int line);

/**
 * ai_box_anchor_after_resize - Where the hunk's line is once the windows are relaid.
 * @f: the window the box is about; @anchor_row: the anchor the box opened with.
 *
 * What the user gains: after a resize the box is still next to the hunk it is
 * about.  The terminal layer relays every window and repaints the desk before
 * it reports WPE_RESIZE, so the line the review parked the cursor on may now
 * be drawn on another row or be out of view: it is brought into view again,
 * exactly as when the box opened, and its new row becomes the anchor.  A box
 * about no particular line (the Multi-file approval) keeps its anchor of 0.
 *
 * Return: the screen row to place the box next to, or 0.
 */
static int ai_box_anchor_after_resize(FENSTER *f, int anchor_row)
{
 int line;
 if (anchor_row <= 0)
  return anchor_row;
 line = f->b->b.y + 1;
 ai_scroll_line_into_view(f, line);
 return ai_line_screen_row(f, line);
}

/**
 * ai_box_scrolled_top - The first row shown after a page up or down.
 * @top: the first row shown now; @delta: rows to move, negative for up;
 * @nrows: all rows; @vis: body rows the box shows.
 *
 * Return: the new first row, kept so the box never scrolls past the last row
 * or above the first one.  With @delta 0 it only re-clamps @top, which is
 * what a box that just changed height needs.
 */
static int ai_box_scrolled_top(int top, int delta, int nrows, int vis)
{
 top += delta;
 if (top > nrows - vis) top = nrows - vis;
 if (top < 0) top = 0;
 return top;
}

/**
 * ai_box_paint - Draw the box: its frame and title, the rows from @top, the hint.
 * @f: the window (for the colours); @fr: the frame; @rt/@ra/@nrows: the rows
 * and their colours; @top: the first row shown; @title/@hint: the captions.
 *
 * Return: the saved background to give back to e_close_view, or NULL when the
 * screen could not be saved.
 */
static PIC *ai_box_paint(FENSTER *f, const ai_box_frame *fr, char **rt, int *ra,
                         int nrows, int top, const char *title, const char *hint)
{
 PIC *pic = e_std_kst(fr->xa, fr->ya, fr->xe, fr->ye, (char *)title, 1,
                      f->fb->nr.fb, f->fb->nt.fb, f->fb->ne.fb);
 int j, hint_len, hint_x;
 if (!pic) return NULL;
 for (j = 0; j < fr->vis; j++) {
  int row = top + j;
  char line[600];
  if (row >= nrows) break;
  ai_box_row_text(line, sizeof line, rt[row], fr->boxw - 2);
  e_pr_str(fr->xa + 1, fr->ya + 1 + j, line, ra[row], 0, 0, 0, 0);
 }
 hint_len = (int)strlen(hint);
 hint_x = fr->xa + (fr->xe - fr->xa - hint_len) / 2;
 if (hint_x < fr->xa + 1) hint_x = fr->xa + 1;
 e_pr_str(hint_x, fr->ye, (char *)hint, f->fb->nr.fb, 0, 0, 0, 0);
 e_refresh();
 return pic;
}

/**
 * ai_box_click_closes - Was this mouse report a left click on the box's close glyph?
 * @fr: the box; @c: the negative code e_getch returned for the report.
 *
 * What the user gains: the close glyph on the box's title row does what it does
 * on every other dialog of this editor, it cancels, exactly as q and Esc do
 * (e_opt_mouse and e_error resolve a click on that cell to Esc the same way).
 * A click anywhere else is ignored like an unrelated key.  The rest of the
 * press is swallowed so the release does not reach the editor as a click of
 * its own.  The terminal reports the buttons in e_mouse.k, the graphical
 * backends in the code itself, negated; the mouse layer reads them the same way.
 *
 * Return: 1 for a click on the close glyph, 0 for anything else.
 */
static int ai_box_click_closes(const ai_box_frame *fr, int c)
{
#if MOUSE
 extern struct mouse e_mouse;
 int buttons;
 if (WpeIsXwin()) buttons = -c;
 else buttons = e_mouse.k;
 if (!(buttons & AI_MOUSE_LEFT)) return 0;
 if (e_mouse.y != fr->ya) return 0;
 if (e_mouse.x != fr->xe - AI_BOX_CLOSE_FROM_RIGHT) return 0;
 while (e_mshit() != 0)
  ;
 return 1;
#else
 (void)fr; (void)c;
 return 0;
#endif
}

/**
 * ai_box_key_decision - What a key pressed on the box decides, if anything.
 * @c: the key; @accept: the letters that end the box, upper case.
 *
 * Return: 13 for Enter, WPE_ESC for Esc, the upper-cased letter for one in
 * @accept, 0 for a key the box ignores (it stays open, unchanged).
 */
static int ai_box_key_decision(int c, const char *accept)
{
 int uc;
 if (c == 13 || c == '\r' || c == '\n') return 13;
 if (c == WPE_ESC) return WPE_ESC;
 uc = e_toupper(c);
 if (accept && strchr(accept, uc)) return uc;
 return 0;
}

/* ai_diff_box_show - Render rows[] (each with its own colour attr) in a boxed,
   scrollable overlay and return the key the reviewer pressed.  PgUp/PgDn scroll
   when the content is taller than the box, so nothing is lost off the top; a key
   in `accept` (case-insensitive), Enter, Esc or a click on the close glyph ends
   it -- any other key or click is ignored.  Returns 13 for Enter, WPE_ESC for
   Esc and the close glyph, else the uppercased key.  The shared preview surface
   for the Edit review and the agent write confirmation.  ya_pref > 0 is the
   screen row of the hunk's first line, which the caller has scrolled into view:
   the box then stays inside the window f (see ai_box_place_in_window); ya_pref
   <= 0 opens it under the menu bar.  A terminal resize lays the box out again
   for the new screen, same hunk and same rows shown, and the keys go on
   working. */
static int ai_diff_box_show(FENSTER *f, char **rt, int *ra, int nrows,
                            const char *title, const char *hint, const char *accept,
                            int ya_pref)
{
 extern int wpe_modal_active;
 ai_box_frame fr;
 int width = ai_box_content_width(rt, nrows, title, hint);
 int top = 0, ret = WPE_ESC;
 int modal_save = wpe_modal_active;
 wpe_modal_active = 1;              /* a review/confirm box is up: async paints defer */

 ai_box_frame_fit(f, ya_pref, nrows, width, &fr);
 fk_cursor(0);
 for (;;) {
  PIC *pic = ai_box_paint(f, &fr, rt, ra, nrows, top, title, hint);
  int c, decision;
  if (!pic) break;
  c = e_getch();
  if (c == WPE_RESIZE) {
   /* The terminal layer has relaid the windows and repainted the desk under
      the box, so the background saved at the old size is dropped, not put
      back; the box is then laid out again for the new screen. */
   e_close_view(pic, 0);
   ya_pref = ai_box_anchor_after_resize(f, ya_pref);
   ai_box_frame_fit(f, ya_pref, nrows, width, &fr);
   top = ai_box_scrolled_top(top, 0, nrows, fr.vis);
   continue;
  }
  if (c == BUP) {
   top = ai_box_scrolled_top(top, -fr.vis, nrows, fr.vis);
   e_close_view(pic, 1);
   continue;
  }
  if (c == BDO) {
   top = ai_box_scrolled_top(top, fr.vis, nrows, fr.vis);
   e_close_view(pic, 1);
   continue;
  }
  if (c < 0 && ai_box_click_closes(&fr, c))
   decision = WPE_ESC;
  else if (c < 0)
   decision = 0;                                    /* a click elsewhere */
  else
   decision = ai_box_key_decision(c, accept);
  e_close_view(pic, 1);
  if (decision == 0)
   continue;                                        /* ignore other keys */
  ret = decision;
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

/* Mark in a box's gutter for a file the assistant would write outside the
   workspace; the row says so in words too, and the pane repeats it when the
   file is written.  Shared by the Multi-file plan's approval box and the
   agent's write box so the user learns one sign. */
#define AI_PLAN_OUTSIDE_MARK "!"
#define AI_PLAN_OUTSIDE_WORD "outside the workspace"

/* The Multi-file edit (Alt-G f) has one name wherever the user meets it: the
   menu row, the prompt, every pane line and the approval box's title all say
   "Multi-file edit", so a line in the pane is read as belonging to the action
   the menu offered and not to some other "plan". */
#define AI_MULTIFILE_NAME   "Multi-file edit"
#define AI_MULTIFILE_PREFIX "[" AI_MULTIFILE_NAME "]"

/* A write box for a file outside the workspace carries one more row, the mark. */
#define AI_DIFF_OUTSIDE_ROWS 1

/**
 * ai_diff_add_outside_row - Put the outside-the-workspace mark at the top of a write box.
 * @rt/@ra: the box rows and their colours; @nrows: how many so far, advanced
 * by one; @path: the file, as the model named it; @attr: the deletion colour.
 *
 * The first thing the user reads in the box is "! outside the workspace:
 * <path>" in the colour of a deletion, so a write that reaches beyond the
 * project is seen as such before the diff below it is judged.  The word
 * comes before the path so it stays on screen when the path is too long for
 * the box.
 */
static void ai_diff_add_outside_row(char **rt, int *ra, int *nrows,
                                    const char *path, int attr)
{
 char row[1400];
 snprintf(row, sizeof row, "%s %s: %s", AI_PLAN_OUTSIDE_MARK, AI_PLAN_OUTSIDE_WORD, path);
 rt[*nrows] = strdup(row);
 ra[*nrows] = attr;
 (*nrows)++;
}

/* ai_diff_confirm_write - Preview what the agent's write_file would do to `path`
   as a colored diff (new file: all-green additions; overwrite: a real diff with
   context) and ask a single allow/deny.  So the user SEES the change before it
   lands, instead of approving a blind "write_file x (N bytes)".  Returns 1 to
   allow, 0 to deny.  Long unchanged runs collapse to a few context lines.
   `outside` != 0 says the file lies outside the workspace: the box then opens
   even for an unchanged file and its first row carries the mark. */
static int ai_diff_confirm_write(FENSTER *f, const char *path,
                                 const char *oldtext, const char *newtext,
                                 int outside)
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
 if (cap <= 0 && !outside) { wpe_ai_segs_free(segs, nseg); return 1; }   /* no change -> allow */
 if (outside) cap += AI_DIFF_OUTSIDE_ROWS;
 rt = malloc((size_t)cap * sizeof *rt);
 ra = malloc((size_t)cap * sizeof *ra);
 if (!rt || !ra) { free(rt); free(ra); wpe_ai_segs_free(segs, nseg); return 1; }
 if (outside) ai_diff_add_outside_row(rt, ra, &nrows, path, red);

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
 * ai_ml_wants_project_memory - Does a mode that acts on the repository carry the instructions?
 *
 * Asks ai_project_instructions_wanted for the Agent, the Multi-file edit and
 * Build & fix; only the key set to off leaves them out, and a run left out is
 * traced so the user can see why the model never saw the file.
 * Return: 1 when the block belongs in the conversation.
 */
static int ai_ml_wants_project_memory(void)
{
 if (ai_project_instructions_wanted(AI_PROMPT_ACTS, 0)) return 1;
 wpe_ai_trace("project instructions left out scope=%s mode=acts",
              wpe_ai_project_scope_name(e_ai_project_scope));
 return 0;
}

/**
 * ai_ml_add_project_memory - Seed the conversation with the project's standing
 * AI instructions (.xwpe-ai.md, else AGENTS.md), as a system turn.  A no-op
 * when the key is off or there is no file.
 * @m: the conversation being seeded; @f: the window the pane belongs to.
 *
 * The message-list peer of ai_append_project_memory, so the agent and the other
 * async modes follow the same repo conventions the chat does.  The block ends
 * with the sentence that keeps the tool list in force, and the pane names the
 * file and its size at launch, so the user knows which rules the run follows
 * and whether the file was cut.
 */
static void ai_ml_add_project_memory(struct ai_mlist *m, FENSTER *f)
{
 char *body, *block;
 struct ai_project_memory_info info;
 if (!ai_ml_wants_project_memory()) return;
 body = ai_project_memory(&info);
 if (!body) return;
 block = ai_project_block_alloc(&info, body);
 free(body);
 if (!block) return;
 ai_ml_add(m, "system", block);
 free(block);
 ai_project_memory_announce(f, &info);
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
#define AI_KIND_CHAT     "chat"

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
 char    *err_msg;            /* the backend's own words for that failure, kept
                                from the stream before it is freed               */
 int      transport_failed;   /* the run ended because the connection died: the
                                pane's last word is the error, no follow-up      */
 int      sel_y0, sel_y1;     /* Edit scope: selected line range, or -1 = whole file */
 /* multi-turn state (unused by single-turn Edit) */
 struct ai_mlist ml;         /* the running conversation                        */
 const char *label;          /* spinner label, e.g. "[agent] working"          */
 int      iter, max_iter;
 int      checkpointed;      /* this run took its checkpoint: at launch, or before
                                the first write after the dial was raised        */
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

/**
 * ai_op_capture_failure - Keep what the finished stream says about a failed turn.
 * @op: the task whose stream just completed.
 *
 * The pane line for a backend error is built after the stream is closed, and
 * the stream owns the server's message; read both the flag and the message
 * here, while the stream is still there, so the user reads "model m is on
 * fire" instead of "no result".  Shared by Edit, Complete and the multi-turn
 * driver, so none of them can lose the message again.
 */
static void ai_op_capture_failure(ai_async_op *op)
{
 op->had_error = wpe_ai_stream_had_error(op->st);
 free(op->err_msg);
 op->err_msg = wpe_ai_stream_error_message(op->st);
}

/**
 * ai_op_backend_error_text - The words the pane shows for a failed turn.
 * @op: the task whose turn the backend answered with an error.
 * @none: what to say when the server left no message and streamed no text.
 *
 * The server's own message first ("model m is on fire", "Internal server
 * error", "The model `x` does not exist"), then whatever text the turn
 * streamed (the claude CLI's "not logged in"), and @none only when there is
 * nothing else to say.  Shared by Edit and the multi-turn driver so an
 * error the server sent, by status or by an event inside the stream, reads
 * the same whichever action asked for it.
 * Return: a string owned by the op (or @none), valid until its next turn.
 */
static const char *ai_op_backend_error_text(const ai_async_op *op, const char *none)
{
 if (op->err_msg && op->err_msg[0]) return op->err_msg;
 if (op->acc && op->acc[0]) return op->acc;
 return none;
}

/**
 * ai_spin_end - The task is over: take its waiting line out of the pane.
 * @sp: the task's spinner; @cn: the desktop its pane is on.
 *
 * The result line the task writes next ("[AI edit] applied 2 of 2 hunks",
 * "[AI] cancelled", the agent's first tool line) lands where the waiting
 * line stood, the way the chat's answer takes over "AI: (gathering the
 * answer...)": the user never reads "working - 5s (Esc cancels)" frozen
 * above a result that has already come.  The status row's "working on this
 * window" goes at the same moment.  Edit, Complete, Multi-file, Agent and
 * Plan all end through here, so they all end alike; a second call finds
 * nothing to do.
 */
static int ai_window_alive(ECNT *cn, FENSTER *f);   /* defined below */

/* A pane buffer never goes below one row; a waiting line that is the only
   row is emptied rather than removed. */
#define AI_PANE_LONE_ROW 1

static void ai_spin_end(ai_spin *sp, ECNT *cn)
{
 FENSTER *wf;
 if (!sp->open) return;
 sp->open = 0;
 if (ai_window_alive(cn, sp->f))
  wpe_ai_flash_clear(sp->f);
 wf = ai_pane_existing(cn);
 if (!wf) return;
 if (wf->b->mxlines <= AI_PANE_LONE_ROW) {  /* the buffer keeps one row: empty it */
  ai_pane_set_last(wf, "");
  return;
 }
 ai_pane_drop_row(wf->b, ai_stream_y(wf));
 if (wf->b->b.y >= wf->b->mxlines)
  wf->b->b.y = wf->b->mxlines - 1;
 ai_pane_paint(wf);
}

/* Detach the op from the fd-loop and free it.  Must run BEFORE any modal review
 * so no AI callbacks (this stream, the heartbeat) fire underneath it.  The
 * waiting line goes here too: every result line written after it takes its
 * place. */
static void ai_op_detach(ai_async_op *op)
{
 ai_spin_end(&op->spin, op->cn);
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
 free(op->err_msg);
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
  ai_pane_set_last_as(wf, line, AI_ROLE_MACHINERY);
 /* This text has taken the waiting line's row, so the row is no longer the
    task's to remove when it ends: the transcript keeps the note that the
    reply waited for the user's dialog, as it keeps every other line. */
 op->spin.open = 0;
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
  ai_pane(f, "[AI] replaced the queued request", AI_ROLE_MACHINERY, 0);
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
 ai_pane(f, line, AI_ROLE_MACHINERY, 1);
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
 const char *kind;
 ai_queue_clear();                    /* a cancel discards any queued follow-up */
 if (!op) return;
 f = op->f;
 kind = op->kind ? op->kind : "task";
 alive = ai_window_alive(op->cn, f);
 wpe_ai_trace("%s cancelled", kind);
 ai_op_detach(op);
 if (alive) ai_pane(f, "[AI] cancelled", AI_ROLE_MACHINERY, 0);
 ai_op_free(op);
 /* Leave the pane ready to type in, so the user can continue right there
    (ask a follow-up / a new question) instead of facing a dead pane. */
 if (alive) ai_chat_offer_input(f);
}

/* ---- the chat turn as a background task ------------------------------------
 * A chat reply streams through its own painter (ai_delta_cb), but the turn is
 * owned by the same machinery as Edit, Complete, Multi-file and Agent: while it
 * runs the editor is "busy", so Esc cancels it with the shared line, a second
 * question (or any other action) queues behind it, the heartbeat spins its
 * waiting line, and closing the pane drops it.  The task carries no stream of
 * its own: the session keeps the stream, and the task's payload is the
 * session, so ending the task ends the session. */

/**
 * ai_chat_op_release - End the chat session when its task is dropped.
 * @ud: the chat session the task carried.
 *
 * Runs from the task's free path on Esc and when the pane closes: the stream
 * is closed, nothing more is painted, and the partial answer stays as far as
 * it got.
 */
static void ai_chat_op_release(void *ud)
{
 ai_chat_session *s = ud;
 if (!s) return;
 s->op = NULL;
 s->active = 0;
 ai_chat_finish(s);
}

/**
 * ai_chat_op_begin - Register the chat turn as the running background task.
 * @s: the chat session that is about to stream.
 *
 * From here Esc cancels the turn, a second question is queued, and the
 * heartbeat rewrites the waiting line.  The caret stays in the pane's input
 * row (no background window), so the next question can be typed meanwhile.
 */
static void ai_chat_op_begin(ai_chat_session *s)
{
 ai_async_op *op = calloc(1, sizeof *op);
 if (!op) return;                     /* the turn still streams, just not as a task */
 op->f = s->ref;
 op->cn = s->ref->ed;
 op->kind = AI_KIND_CHAT;
 op->fd = -1;
 op->save_id = -1;
 op->start = time(NULL);
 op->spin.f = s->ref;
 op->spin.label = AI_CHAT_WAIT_LABEL;
 op->spin.i = 0;
 op->ud = s;
 op->free_ud = ai_chat_op_release;
 s->op = op;
 g_ai_op = op;
 ai_hb_acquire(ai_op_tick);
}

/**
 * ai_chat_op_end - Let go of the task when the chat turn ended on its own.
 * @s: the chat session whose stream is complete (or failed).
 *
 * The session ends itself afterwards, so the task must not end it again:
 * the payload is detached before the task is freed.  The chat has written
 * its own ending over the waiting line by now (the answer, "(no answer)",
 * the transport line), so that line is kept, not taken out as a task's is.
 */
static void ai_chat_op_end(ai_chat_session *s)
{
 ai_async_op *op = s->op;
 s->op = NULL;
 if (!op) return;
 op->ud = NULL;
 op->spin.open = 0;
 ai_op_detach(op);
 ai_op_free(op);
}

/**
 * ai_chat_turn_over - The chat turn is over: free everything, run the queue.
 * @s: the chat session that finished, failed or could not start.
 *
 * The one end for every outcome but a cancel, so a question queued behind
 * the turn starts by itself whichever way the turn ended.
 */
static void ai_chat_turn_over(ai_chat_session *s)
{
 s->active = 0;
 ai_chat_op_end(s);
 ai_chat_finish(s);
 ai_queue_run();
}

/**
 * ai_chat_wait_begin - Open the waiting line for a turn and start it spinning.
 * @s: the chat session starting a turn (the first, or a tool turn).
 *
 * The line reads "AI: (gathering the answer...)" and the heartbeat appends
 * the spinner glyph, the seconds and "(Esc cancels)" to it, so a slow model
 * looks alive and the way out is named.  Without a task (allocation failed)
 * the line is written once, as before.
 */
static void ai_chat_wait_begin(ai_chat_session *s)
{
 FENSTER *wf = ai_pane_win(s->ref);
 if (!wf) return;
 if (s->op) {
  s->op->spin = ai_spin_begin(s->ref, AI_CHAT_WAIT_LABEL);
  ai_pane_set_last_role(wf, AI_ROLE_ASSISTANT);   /* the "AI:" line is the reply's */
  s->op->start = time(NULL);
  s->op->paused = 0;
 } else {
  ai_pane_commit(wf);
  ai_pane_set_last(wf, AI_CHAT_WAIT_LABEL);
 }
 s->started = 1;
}

/**
 * ai_chat_wait_end - The stream line now holds content: stop the spinner.
 * @s: the chat session whose painter decided what the reply is.
 *
 * From the first painted byte of an answer, or the dim tool status, the line
 * belongs to the reply; a heartbeat tick must not write the waiting text over
 * it.  The spinner starts again with the next turn's waiting line.
 */
static void ai_chat_wait_end(ai_chat_session *s)
{
 if (!s->op) return;
 s->op->paused = 1;
 s->op->spin.open = 0;             /* the reply owns the line: it is not removed */
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
  ai_pane(f, "[AI edit] discarded", AI_ROLE_MACHINERY, 0);
  wpe_ai_trace("edit discarded");
  return;
 }
 snprintf(msg, sizeof msg, "[AI edit] discarded (%d accepted %s dropped)",
          naccepted, naccepted == 1 ? "hunk" : "hunks");
 ai_pane(f, msg, AI_ROLE_MACHINERY, 0);
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
           ai_op_backend_error_text(op, "the backend did not return a result"));
  ai_pane(f, l, AI_ROLE_ERROR, 1);
  wpe_ai_trace("edit error (not applied)");
  ai_op_free(op);
  return;
 }
 if (op->acc_len == 0) {
  ai_pane(f, "[AI edit] no response", AI_ROLE_ERROR, 0);
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
  ai_pane(f, "[AI edit] reply declined (nothing changed)", AI_ROLE_MACHINERY, 0);
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
  ai_pane(f, "[AI edit] no change", AI_ROLE_MACHINERY, 0);
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
   ai_pane(f, msg, AI_ROLE_MACHINERY, 0);
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
  if (ai_window_alive(cn, f)) ai_pane(f, "[AI edit] transport error", AI_ROLE_ERROR, 0);
  ai_op_free(op);
  ai_queue_run();
  return;
 }
 if (done) {
  ai_op_capture_failure(op);                /* before the stream goes */
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
   ai_pane(op->f, l, AI_ROLE_ERROR, 0);
  }
  ai_conv_finish(op);
  return;
 }
 op->fd = wpe_ai_stream_fd(op->st);
 op->start = time(NULL);
 op->paused = 0;
 op->acc_len = 0;
 if (op->acc) op->acc[0] = '\0';
 free(op->err_msg);
 op->err_msg = NULL;
 op->spin = ai_spin_begin(op->f, op->label);
 wpe_fd_add(op->fd, POLLIN, ai_conv_fd_cb, op);
}

/**
 * ai_conv_report_backend_error - Tell the user a turn failed, with the reason.
 * @op: the task whose turn the backend answered with an error.
 *
 * The line "[agent] working: backend error: <reason>" used to end in "no
 * result" whatever the server had said, because the message lived in the
 * stream that was already freed; the reason is read from the op now.
 */
static void ai_conv_report_backend_error(ai_async_op *op)
{
 char l[360];
 if (!ai_window_alive(op->cn, op->f)) return;
 snprintf(l, sizeof l, "%s: backend error: %.300s", op->label,
          ai_op_backend_error_text(op, "no result"));
 ai_pane(op->f, l, AI_ROLE_ERROR, 0);
}

/* A turn's reply is complete: hand it to process(), then continue or finish. */
static void ai_conv_on_reply(ai_async_op *op)
{
 int r;
 op->paused = 1;                    /* no spinner while process() may go modal   */
 ai_spin_end(&op->spin, op->cn);    /* what process() writes takes the line's place */
 ai_op_stream_close(op);            /* leave the loop before any modal review    */
 /* A backend failure (e.g. the claude CLI not logged in) is not a turn to act
    on: do NOT feed it to process(), which could run a tool call or apply an
    edit parsed from an error string.  Report and stop. */
 if (op->had_error) {
  ai_conv_report_backend_error(op);
  wpe_ai_trace("conv error (stopped)");
  ai_conv_finish(op);
  return;
 }
 ai_ml_add(&op->ml, "assistant", op->acc ? op->acc : "");
 r = op->process ? op->process(op, op->acc ? op->acc : "") : 1;
 op->iter++;
 if (r != 0 || op->iter >= op->max_iter) {
  if (op->iter >= op->max_iter && r == 0 && ai_window_alive(op->cn, op->f))
   ai_pane(op->f, "[AI] reached the step limit - Alt-G g to continue the task",
           AI_ROLE_MACHINERY, 0);
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
  ai_spin_end(&op->spin, cn);       /* the error line takes the waiting line's place */
  if (ai_window_alive(cn, f)) ai_pane(f, "[AI] transport error", AI_ROLE_ERROR, 0);
  op->transport_failed = 1;         /* the run stops on this line, no follow-up */
  wpe_ai_trace("conv transport error (stopped)");
  ai_conv_finish(op);
  return;
 }
 if (done) {
  ai_op_capture_failure(op);        /* before stream_close */
  ai_op_reply_complete(op, ai_conv_on_reply);
 }
}

static void ai_conv_finish(ai_async_op *op)
{
 ai_op_stream_close(op);
 ai_spin_end(&op->spin, op->cn);    /* nothing waits any more: finish() writes here */
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
 if (wpe_ai_preflight(e_ai_backend, err, sizeof err)) { ai_pane(f, err, AI_ROLE_ERROR, 1); return; }
 err[0] = '\0';
 if (wpe_ai_ensure_model(err, sizeof err)) {
  ai_pane(f, err[0] ? err : "no model set", AI_ROLE_ERROR, 1);
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
  ai_pane(f, line, AI_ROLE_ERROR, 1);
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

/* The one pane line for an editing action asked from a window that is not a
   file: the AI pane itself, Messages, a read-only viewer.  Edit, Complete and
   Multi-file all say it, so the user learns one sentence. */
#define AI_NO_FILE_LINE \
 "[AI] no file here - Edit, Complete and Multi-file work on a file window"

/**
 * ai_refuse_non_file_window - Stop an editing action asked from a non-file window.
 * @f: the window Alt-G e, c or f was pressed in.
 *
 * What the user gains: the assistant never diffs or overwrites its own pane,
 * the Messages window or a read-only viewer.  After a chat reply or a cancelled
 * task the focus rests on the pane, and the next Alt-G e used to open its
 * prompt there, diff the transcript and let "y" replace it with the file.  Now
 * nothing is sent, no prompt opens, and the pane says AI_NO_FILE_LINE; the
 * transcript is untouched.  The same question gates the three actions, through
 * wpe_ai_window_is_editable_file.
 *
 * Return: 1 when the action must stop (the pane has been told), 0 to go on.
 */
static int ai_refuse_non_file_window(FENSTER *f)
{
 if (wpe_ai_window_is_editable_file(f))
  return 0;
 ai_pane(f, AI_NO_FILE_LINE, AI_ROLE_MACHINERY, 1);
 wpe_ai_trace("edit refused: not a file window (%s)", f && f->datnam ? f->datnam : "?");
 return 1;
}

/**
 * e_ai_edit - Change the current file by instruction (Alt-G e).
 * @f: the file window.  Return: 0, the editor-action convention.
 *
 * Asks for the instruction first and only then looks at what is running, so a
 * second request can be typed while the first is still generating: it is
 * queued and starts by itself when that one ends.  A window that is not a file
 * is refused before the prompt opens (ai_refuse_non_file_window).
 */
static int e_ai_edit(FENSTER *f)
{
 static char instr[AI_PROMPT_MAX];
 int sy0 = -1, sy1 = -1;
 int have_sel;

 if (ai_refuse_non_file_window(f))
  return 0;
 have_sel = (ai_edit_scope_pick(f, &sy0, &sy1) == AI_SCOPE_BLOCK);
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
 ai_pane(f, "[AI complete] discarded", AI_ROLE_MACHINERY, 0);
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
 ai_pane(f, "[AI] completed at cursor - Ctrl-U to undo", AI_ROLE_MACHINERY, 0);
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
 if (op->had_error) { ai_pane(f, "[AI complete] backend error", AI_ROLE_ERROR, 1); ai_op_free(op); return; }
 if (op->acc_len == 0) { ai_pane(f, "[AI complete] no suggestion", AI_ROLE_MACHINERY, 0); ai_op_free(op); return; }
 clean = ai_completion_text(op->acc);
 if (!clean || !c) { free(clean); ai_op_free(op); return; }
 if (!*clean) {
  ai_pane(f, "[AI complete] no suggestion", AI_ROLE_MACHINERY, 0);
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
  if (ai_window_alive(cn, f)) ai_pane(f, "[AI complete] transport error", AI_ROLE_ERROR, 0);
  ai_op_free(op); ai_queue_run(); return;
 }
 if (done) {
  ai_op_capture_failure(op);
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

 if (ai_refuse_non_file_window(f)) return 0;
 if (wpe_ai_busy()) { ai_enqueue(f, ai_launch_complete, ""); return 0; }
 err[0] = '\0';
 if (wpe_ai_preflight(e_ai_backend, err, sizeof err)) { ai_pane(f, err, AI_ROLE_ERROR, 1); return 0; }
 err[0] = '\0';
 if (wpe_ai_ensure_model(err, sizeof err)) { ai_pane(f, err[0] ? err : "no model set", AI_ROLE_ERROR, 1); return 0; }

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
  ai_pane(f, line, AI_ROLE_ERROR, 1);
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
 if (n <= 0) { ai_pane(f, title[0] ? title : "no models found", AI_ROLE_ERROR, 1); return 0; }
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
   ai_pane(f, line, AI_ROLE_MACHINERY, 1);
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
 /* The web switch sits beside the assistant's own, on the row a user reads
    first: it decides whether anything leaves this machine, which is not a
    setting to hide behind the Advanced button.  Ticked = the web is on for
    this session, the effective state every other field shows, so the box
    never contradicts what the assistant will actually do; whether Ok writes
    it is the config layer's business, not the box's, and the Ok read below
    says why clearing an unanswered box leaves it unanswered. */
 e_add_sswstr(36, 2, 0, AltW,
              wpe_ai_web_state() == WPE_AI_WEB_ON ? 1 : 0, "Web access", o);

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
 /* Ticking the box turns the web on and answers the switch for good.  Clearing
    it turns the web off only when it was on: a user who opens the dialog and
    presses Ok without touching an unanswered box has not refused the web, so
    the first-use notice still gets its one chance to ask. */
 if (o->sstr[1]->num) {
  if (wpe_ai_web_state() != WPE_AI_WEB_ON)
   wpe_ai_cfg_user_changed(WPE_AI_CFG_WEB);  /* the user chose it; it is config now */
  e_ai_web = WPE_AI_WEB_ON;
 } else if (wpe_ai_web_state() == WPE_AI_WEB_ON) {
  wpe_ai_cfg_user_changed(WPE_AI_CFG_WEB);
  e_ai_web = WPE_AI_WEB_OFF;
 }
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
 ai_pane(f, "AI assistant -- first use.  It runs LOCALLY by default (Ollama on",
         AI_ROLE_MACHINERY, 1);
 ai_pane(f, "localhost): nothing leaves your machine unless you point it at a",
         AI_ROLE_MACHINERY, 1);
 ai_pane(f, "remote backend.  It is opt-in, every edit is previewed, and one",
         AI_ROLE_MACHINERY, 1);
 ai_pane(f, "Ctrl-U reverts it.", AI_ROLE_MACHINERY, 1);
 ai_pane(f, "After enabling, Alt-G opens a menu: a Ask  e Edit  g Agent  s Settings.",
         AI_ROLE_MACHINERY, 1);
 ai_pane_attn(f, "Enter / y = enable and continue      n / Esc = not now");
 for (;;) {
  int c = e_getch();
  if (c == WPE_ESC || e_toupper(c) == 'N') {
   ai_pane(f, "[AI] not enabled - press Alt-G again when you want it", AI_ROLE_MACHINERY, 0);
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
  ai_pane(f, AI_OFF_LINE, AI_ROLE_MACHINERY, 1);
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

/* Run a shell command; capture bounded stdout+stderr (malloc'd).  The bound
   on its size and on its time (AIToolTimeout) is the capture function's. */
static char *ai_run_capture(const char *cmd) { return wpe_ai_run_capture(cmd); }

/* What the model reads for a read_file of a path that is not there. */
#define AI_TOOL_NO_FILE_RESULT "(file not found)"
/* The first size of a read's buffer; it doubles when it fills.  A byte is
   stored only when the byte and the NUL after it both fit. */
#define AI_READ_FIRST_CAP 4096
#define AI_READ_TAIL_ROOM 2

/**
 * ai_buf_grow - Double a growing buffer.
 * @buf: the buffer, replaced on success; @cap: its size, doubled on success.
 * Return: 1 when there is room, 0 when the memory was refused and the old
 * buffer stands as it was.
 */
static int ai_buf_grow(char **buf, size_t *cap)
{
 char *nb = realloc(*buf, *cap * 2);
 if (!nb) return 0;
 *buf = nb;
 *cap *= 2;
 return 1;
}

/**
 * ai_read_file_bounded - The first WPE_AI_TOOL_OUT_MAX bytes of a file, for the model.
 * @path: the file, as the model named it.
 *
 * A file longer than the bound is cut and the result ends with the cut note,
 * so the model asks for a narrower read (a grep, a line range) instead of
 * taking the beginning for the whole file.  A missing file is a result, not
 * an error, so the run goes on.
 * Return: the malloc'd result, or NULL out of memory.
 */
static char *ai_read_file_bounded(const char *path)
{
 FILE *fp = fopen(path, "rb");
 char *out;
 size_t cap = AI_READ_FIRST_CAP, len = 0;
 int ch;
 if (!fp) return strdup(AI_TOOL_NO_FILE_RESULT);
 out = malloc(cap);
 if (!out) { fclose(fp); return NULL; }
 while ((ch = fgetc(fp)) != EOF && len < WPE_AI_TOOL_OUT_MAX) {
  if (len + AI_READ_TAIL_ROOM > cap && !ai_buf_grow(&out, &cap)) break;
  out[len++] = (char)ch;
 }
 out[len] = '\0';
 fclose(fp);
 if (ch != EOF) return wpe_ai_tool_result_mark_cut(out, len);   /* more bytes followed */
 return out;
}

/* Room for a pane line that names a tool and its argument. */
#define AI_AGENT_PANE_LINE_MAX 640

/**
 * ai_dial_passes - Does the permission dial let this tool run without asking?
 * @is_run: 1 for a command or a web tool, 0 for a write.
 *
 * auto passes everything, edits passes writes but asks for commands, ask
 * passes nothing.  The dial is read live, so Alt-G y during a run changes the
 * answer for the next tool.
 * Return: 1 when the tool runs unasked.
 */
static int ai_dial_passes(int is_run)
{
 if (e_ai_policy == WPE_AI_POLICY_AUTO) return 1;
 if (e_ai_policy == WPE_AI_POLICY_EDITS && !is_run) return 1;
 return 0;
}

/**
 * ai_agent_note_auto_approved - Say in the pane that the dial passed a tool.
 * @f: the window the pane belongs to; @what: the tool and its argument.
 *
 * Names the dial position, so a user who sees a command run without a
 * question knows which setting allowed it.
 */
static void ai_agent_note_auto_approved(FENSTER *f, const char *what)
{
 char line[AI_AGENT_PANE_LINE_MAX];
 snprintf(line, sizeof line, "[agent] auto-approved (%s): %s",
          wpe_ai_policy_name(e_ai_policy), what);
 ai_pane(f, line, AI_ROLE_MACHINERY, 0);
 wpe_ai_trace("agent auto-approve %s", what);
}

/**
 * ai_agent_read_yes_no - Wait for the user's answer to an approval question.
 *
 * y or Enter allows; n or Esc denies; any other key is ignored, so a stray
 * keystroke cannot approve a command by accident.
 * Return: 1 to allow, 0 to deny.
 */
static int ai_agent_read_yes_no(void)
{
 for (;;) {
  int c = e_getch();
  if (c == WPE_ESC || e_toupper(c) == 'N') return 0;
  if (e_toupper(c) == 'Y' || c == WPE_CR || c == WPE_WR) return 1;
 }
}

/**
 * ai_agent_approve - Let the dial or the user decide a command or a write.
 * @f: the window the pane belongs to; @what: the tool and its argument, for
 * the pane; @is_run: 1 for a command or a web tool, 0 for a write.
 *
 * A tool the dial passes is noted and runs; otherwise the pane asks, with the
 * keys spelled out, and the editor waits for the answer.
 * Return: 1 when approved.
 */
static int ai_agent_approve(FENSTER *f, const char *what, int is_run)
{
 char line[AI_AGENT_PANE_LINE_MAX];
 if (ai_dial_passes(is_run)) {
  ai_agent_note_auto_approved(f, what);
  return 1;
 }
 snprintf(line, sizeof line, "APPROVE?  %s", what);
 ai_pane_attn(f, line);
 ai_pane_attn(f, "y = allow    n / Esc = deny");
 return ai_agent_read_yes_no();
}

/* Room for a pane line that quotes a whole tool argument after its words. */
#define AI_AGENT_ARG_LINE_MAX (AI_TOOL_LINE_MAX + 32)

/**
 * ai_agent_deny - Record a refused tool everywhere it is read: model, pane, trace.
 * @f: the window the pane belongs to; @tool: the tool the user refused;
 * @arg: its argument (a command, a URL or query, a path).
 *
 * A denial used to reach the model alone, so the pane showed the request and
 * then nothing, and a user who pressed n could not tell whether the key was
 * taken.  The pane now says "[agent] denied: <tool> <arg>" and the trace has
 * the same line; the one function serves the command, the two web tools and
 * the writes, so the three records cannot drift apart.
 * Return: the malloc'd result the model reads.
 */
static char *ai_agent_deny(FENSTER *f, const char *tool, const char *arg)
{
 char line[AI_AGENT_ARG_LINE_MAX];
 snprintf(line, sizeof line, "[agent] denied: %s %s", tool, arg);
 ai_pane(f, line, AI_ROLE_MACHINERY, 0);
 wpe_ai_trace("agent deny %s %s", tool, arg);
 return strdup(AI_TOOL_DENIED_RESULT);
}

/* What the model reads when a write could not be made. */
#define AI_TOOL_WRITE_FAILED_RESULT "(write failed)"

/**
 * ai_agent_report_write_failed - Record a write that could not be made: model, pane, trace.
 * @f: the window the pane belongs to; @path: the file the agent named.
 *
 * "(write failed)" used to reach the model alone: the pane's last word was
 * "auto-approved", so a user whose agent wrote into a directory that does not
 * exist believed the file was there.  The pane now says "[agent] write
 * failed: <path>" and the trace has the same fact, so the failure is read
 * where the approval was.
 * Return: the malloc'd result the model reads.
 */
static char *ai_agent_report_write_failed(FENSTER *f, const char *path)
{
 char line[AI_AGENT_ARG_LINE_MAX];
 snprintf(line, sizeof line, "[agent] write failed: %s", path);
 ai_pane(f, line, AI_ROLE_ERROR, 0);
 wpe_ai_trace("agent write failed %s", path);
 return strdup(AI_TOOL_WRITE_FAILED_RESULT);
}

/**
 * ai_agent_show_running - Say in the pane that a tool is running, before it does.
 * @f: the window the pane belongs to; @tool: the tool about to run.
 *
 * The editor reads no keys while a command or a web tool runs, and the working
 * spinner is stopped for the turn, so without this line the pane froze on the
 * tool's request and nothing said an answer would follow.  The line is painted
 * and flushed to the terminal before the blocking call starts, so it is on
 * screen for the whole run.
 */
static void ai_agent_show_running(FENSTER *f, const char *tool)
{
 char line[160];
 snprintf(line, sizeof line, "[agent] running %s ... (result follows)", tool);
 ai_pane(f, line, AI_ROLE_MACHINERY, 0);
}

/**
 * ai_agent_report_timeout - Tell the user a tool was ended at its time limit.
 * @f: the window the pane belongs to; @tool: the tool that ran too long;
 * @result: the timeout result the model gets, traced so a test can read it.
 *
 * The pane names the limit (AIToolTimeout) so the user knows why the command
 * stopped and which key to raise for a command that legitimately needs longer.
 */
static void ai_agent_report_timeout(FENSTER *f, const char *tool, const char *result)
{
 char line[200];
 snprintf(line, sizeof line, "[agent] %s timed out after %d s (AIToolTimeout) "
          "and was ended", tool, e_ai_tool_timeout);
 ai_pane(f, line, AI_ROLE_ERROR, 0);
 wpe_ai_trace("agent tool timed out: %s", result);
}

/* Persist the current settings (the working mode) so choices stick across
 * sessions without a manual "Save Options".  e_save_opt writes the active config
 * -- the same file the option dialog saves to. */
static void ai_persist_settings(FENSTER *f) { e_save_opt(f); }

/* One-shot flash message on the bottom status line: it takes the whole bar row
 * (which is otherwise full of key hints) and the next keystroke restores it.  A
 * quick confirmation that opens neither the pane nor a modal box. */
static int g_ai_flash_active = 0;

/* The bar's first column and the room a flash leaves at the right edge. */
#define AI_FLASH_COL    1
#define AI_FLASH_MARGIN 2

/**
 * wpe_ai_flash - Show a one-line confirmation on the bar, until the next key.
 * @f: the window whose colours the bar takes; @msg: the words.
 *
 * The user gets a quick acknowledgement (a setting changed, a step taken)
 * without a box to dismiss: the next keystroke brings the key hints back.
 */
void wpe_ai_flash(FENSTER *f, const char *msg)
{
 char buf[MAXSCOL + 1];
 int n;
 if (!f || !f->fb) return;
 n = (int)strlen(msg);
 if (n > MAXSCOL - AI_FLASH_MARGIN) n = MAXSCOL - AI_FLASH_MARGIN;
 memcpy(buf, msg, (size_t)n); buf[n] = '\0';
 e_blk(MAXSCOL, 0, MAXSLNS - 1, f->fb->mt.fb);                 /* clear the bar row */
 e_pr_str(AI_FLASH_COL, MAXSLNS - 1, buf, f->fb->ms.fb, -1, -1, f->fb->ms.fb, f->fb->mt.fb);
 e_refresh();
 g_ai_flash_active = 1;
}

/**
 * wpe_ai_flash_clear - Put the key hints back after a flash.
 * @f: the window whose bar was flashed.
 *
 * Called on the next keystroke, so the confirmation never outstays the
 * moment it was for.
 * Return: 1 when a flash was showing and the bar was redrawn, 0 otherwise.
 */
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

/* The dial's three positions in cycling order, each with the words the bar
   shows for it; ask follows auto. */
#define AI_POLICY_COUNT 3
static const char *ai_policy_word[AI_POLICY_COUNT] = { "ask", "edits", "auto" };
static const char *ai_policy_desc[AI_POLICY_COUNT] = {
  "ask before each edit and command",
  "auto-accept edits, ask before commands",
  "run edits and commands unattended"
};
/* The bar's layout: the label, one blank after it, two blanks between the
   positions, one before the description. */
#define AI_POLICY_BAR_LABEL "Permissions:"
#define AI_POLICY_LABEL_GAP 1
#define AI_POLICY_WORD_GAP  2
#define AI_POLICY_DESC_GAP  1
#define AI_POLICY_SEG_MAX   64

/**
 * ai_policy_advance - Turn the dial one position: ask -> edits -> auto -> ask.
 */
static void ai_policy_advance(void)
{
 e_ai_policy = (e_ai_policy + 1) % AI_POLICY_COUNT;
}

/**
 * ai_policy_bar_words - Paint the three positions on the bar, the active one highlighted.
 * @col: where the first word goes; @act: the active position; @base/@hi:
 * the bar's normal and highlight colours.
 *
 * All three are shown so the user sees where the dial stands among its
 * positions and watches the highlight move as Alt-G y cycles.
 * Return: the column after the last word.
 */
static int ai_policy_bar_words(int col, int act, int base, int hi)
{
 char seg[AI_POLICY_SEG_MAX];
 int i, c;
 for (i = 0; i < AI_POLICY_COUNT; i++) {
  if (i == act) c = hi;
  else c = base;
  snprintf(seg, sizeof seg, "%s", ai_policy_word[i]);
  e_pr_str(col, MAXSLNS - 1, seg, c, -1, -1, c, base);
  col += (int)strlen(ai_policy_word[i]) + AI_POLICY_WORD_GAP;
 }
 return col;
}

/**
 * ai_policy_flash_bar - Show the dial's new position on the bar until the next key.
 * @f: the window whose colours the bar takes.
 *
 * The whole bar row is taken for "Permissions: ask  edits  auto -- <what the
 * active one does>", a confirmation that opens neither the pane nor a box.
 */
static void ai_policy_flash_bar(FENSTER *f)
{
 char seg[AI_POLICY_SEG_MAX];
 int col, act = e_ai_policy;
 int base = f->fb->mt.fb;                               /* bar normal             */
 int hi   = f->fb->ms.fb;                               /* bar shortcut highlight */
 e_blk(MAXSCOL, 0, MAXSLNS - 1, base);                  /* take the whole bar row */
 col = AI_FLASH_COL;
 e_pr_str(col, MAXSLNS - 1, AI_POLICY_BAR_LABEL, base, -1, -1, base, base);
 col += (int)strlen(AI_POLICY_BAR_LABEL) + AI_POLICY_LABEL_GAP;
 col = ai_policy_bar_words(col, act, base, hi);
 snprintf(seg, sizeof seg, "-- %s", ai_policy_desc[act]);
 e_pr_str(col + AI_POLICY_DESC_GAP, MAXSLNS - 1, seg, base, -1, -1, base, base);
 e_refresh();
 wpe_ai_flash_mark();                                   /* next key restores the bar */
}

/**
 * e_ai_cycle_policy - Alt-G y: turn the permission dial and confirm it on the bar.
 * @f: the window the key was pressed in.
 *
 * The new position is shown at once with what it means, and saved, so the
 * choice holds for the next session without a visit to the options.
 */
static void e_ai_cycle_policy(FENSTER *f)
{
 if (!f || !f->fb) return;
 ai_policy_advance();
 ai_policy_flash_bar(f);
 ai_persist_settings(f);
 wpe_ai_trace("policy set %s", wpe_ai_policy_name(e_ai_policy));
}

/* ---------------- agent plan / live TODO list (Messages window) ------------ */

/* The checklist: at most this many steps, each cut to this many bytes. */
#define AI_PLAN_MAX      32
#define AI_PLAN_TASK_MAX 200
static char g_ai_plan[AI_PLAN_MAX][AI_PLAN_TASK_MAX];  /* the tasks the agent laid out */
static int  g_ai_plan_done[AI_PLAN_MAX];              /* per-task: completed?         */
static int  g_ai_plan_n;
/* Room for a checklist row and for the pane's progress line. */
#define AI_PLAN_ROW_MAX  260
#define AI_PLAN_NOTE_MAX 280
/* e_d_p_message's last argument: start the Messages window over, or add a row. */
#define AI_PLAN_RESTART 1
#define AI_PLAN_APPEND  0
/* The word for a step the agent reported without naming it. */
#define AI_PLAN_STEP_WORD "(step)"

/**
 * ai_plan_done_count - How many of the plan's steps are ticked.
 * Return: the count, for the "n/m done" the user watches.
 */
static int ai_plan_done_count(void)
{
 int i, ndone = 0;
 for (i = 0; i < g_ai_plan_n; i++) if (g_ai_plan_done[i]) ndone++;
 return ndone;
}

/**
 * ai_plan_tick - The mark a step shows in the checklist.
 * @done: whether the step is finished.
 * Return: 'x' for a finished step, a blank for a pending one.
 */
static char ai_plan_tick(int done)
{
 if (done) return 'x';
 return ' ';
}

/**
 * ai_plan_show - Draw the plan as a live checklist in the Messages window.
 *
 * A long agent run is far more legible with a dockable to-do list than a
 * scrolling pane log: the plan sits in the Messages window and is REDRAWN IN
 * PLACE (the window is restarted) each time a step completes, so its status --
 * "3/7 done", "[x]" per finished task -- updates live instead of scrolling away.
 */
static void ai_plan_show(FENSTER *f)
{
 char line[AI_PLAN_ROW_MAX];
 int i;
 snprintf(line, sizeof line, "=== Agent plan: %d/%d done ===", ai_plan_done_count(), g_ai_plan_n);
 e_d_p_message(line, f, AI_PLAN_RESTART);
 for (i = 0; i < g_ai_plan_n; i++) {
  snprintf(line, sizeof line, "  [%c] %s", ai_plan_tick(g_ai_plan_done[i]), g_ai_plan[i]);
  e_d_p_message(line, f, AI_PLAN_APPEND);
 }
 if (g_ai_plan_n == 0) e_d_p_message("  (no steps given)", f, AI_PLAN_APPEND);
}

/**
 * ai_todo_line_len - How much of a plan line is the task: up to its newline or the end marker.
 * @p: the line's start; @nl: its newline, NULL on the last line; @stop: the
 * "@@END" line, NULL when the body has none.
 * Return: the task's length in bytes.
 */
static int ai_todo_line_len(const char *p, const char *nl, const char *stop)
{
 int len;
 if (nl) len = (int)(nl - p);
 else len = (int)strlen(p);
 if (stop && (!nl || nl > stop)) len = (int)(stop - p);
 return len;
}

/**
 * ai_todo_is_bullet - Is this byte list decoration rather than the task's words?
 * @c: a byte at the start of a plan line.
 * Return: 1 for a blank, a tab, a dash or a star.
 */
static int ai_todo_is_bullet(char c)
{
 return c == ' ' || c == '\t' || c == '-' || c == '*';
}

/**
 * ai_todo_strip_bullet - Skip a plan line's bullet and indentation.
 * @p: the line's start; @len: its length, reduced by what was skipped.
 *
 * "- build it" and "  * build it" both become "build it", so the checklist
 * reads as the user would write it whatever list style the model chose.
 * Return: where the task's words start.
 */
static const char *ai_todo_strip_bullet(const char *p, int *len)
{
 while (*len && ai_todo_is_bullet(*p)) { p++; (*len)--; }
 return p;
}

/**
 * ai_todo_add - Append one pending step to the checklist, cut to the step's room.
 * @p: the task's words; @len: how many bytes of them.
 */
static void ai_todo_add(const char *p, int len)
{
 int k = len;
 if (k > AI_PLAN_TASK_MAX - 1) k = AI_PLAN_TASK_MAX - 1;
 memcpy(g_ai_plan[g_ai_plan_n], p, (size_t)k);
 g_ai_plan[g_ai_plan_n][k] = '\0';
 g_ai_plan_done[g_ai_plan_n] = 0;
 g_ai_plan_n++;
}

/**
 * ai_todo_load - Load the agent's plan body (task lines up to @@END) into the
 * checklist, all pending, and show it before the agent acts so the user can
 * course-correct.
 */
static void ai_todo_load(FENSTER *f, const char *body)
{
 const char *p = body, *stop = strstr(body, AI_TOOL_BODY_END_LINE);
 g_ai_plan_n = 0;
 while (p && *p && (!stop || p < stop) && g_ai_plan_n < AI_PLAN_MAX) {
  const char *nl = strchr(p, '\n');
  int len = ai_todo_line_len(p, nl, stop);
  p = ai_todo_strip_bullet(p, &len);
  if (len > 0) ai_todo_add(p, len);
  if (!nl) break;
  p = nl + 1;
 }
 ai_plan_show(f);
}

/**
 * ai_plan_progress - Note a finished step in the pane, with the running count.
 * @f: the window the pane belongs to; @task: the step's words, may be empty.
 *
 * The Messages window may be hidden behind the file, so the pane carries a
 * compact "[plan] 3/7 done  ok: <step>" the user sees during the run.
 */
static void ai_plan_progress(FENSTER *f, const char *task)
{
 char line[AI_PLAN_NOTE_MAX];
 const char *shown = AI_PLAN_STEP_WORD;
 if (task && *task) shown = task;
 snprintf(line, sizeof line, "[plan] %d/%d done  ok: %.200s",
          ai_plan_done_count(), g_ai_plan_n, shown);
 ai_pane(f, line, AI_ROLE_MACHINERY, 0);            /* visible progress in the pane during the run */
}

/**
 * ai_plan_mark_done - Tick off a finished step and redraw the checklist.
 * Match the reported text to a pending task (substring either way); fall back to
 * the earliest pending task, since an agent usually finishes them in order.
 */
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
 ai_pane(f, line, AI_ROLE_MACHINERY, 0);
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
 * ai_agent_confirm_write - Show the agent's write as a diff and take a y/n.
 * @f: the window the pane belongs to; @path: the file, as the agent named it;
 * @content: the text the agent wants in it; @outside: 1 when the file lies
 * outside the workspace, so the box carries the mark.
 *
 * The user sees what would change before it lands, instead of approving a
 * blind "write x (N bytes)", and the trace records the answer either way.
 * Return: 1 to allow, 0 to deny.
 */
static int ai_agent_confirm_write(FENSTER *f, const char *path,
                                  const char *content, int outside)
{
 char *old = ai_slurp_file(path);
 int allow = ai_diff_confirm_write(f, path, old ? old : "", content, outside);
 wpe_ai_trace("agent write confirm %s -> %s", path, allow ? "allow" : "deny");
 free(old);
 return allow;
}

/**
 * ai_agent_approve_write - Let the dial decide a write inside the workspace.
 * @f: the window the pane belongs to; @path: the file; @content: its new text.
 *
 * Under edits and auto the write is auto-approved and the pane says so; under
 * ask the user sees the diff box.  Only for a file inside the workspace: a
 * write beyond it never reaches this function.
 * Return: 1 to allow, 0 to deny.
 */
static int ai_agent_approve_write(FENSTER *f, const char *path, const char *content)
{
 char what[720];
 if (e_ai_policy == WPE_AI_POLICY_ASK) return ai_agent_confirm_write(f, path, content, 0);
 snprintf(what, sizeof what, "write %s (%zu bytes)", path, strlen(content));
 return ai_agent_approve(f, what, 0);
}

/**
 * ai_agent_ask_outside_write - Ask about a write outside the workspace, whatever the dial.
 * @f: the window the pane belongs to; @path: the file, as the agent named it;
 * @content: its new text.
 *
 * An unattended run (edits or auto) may rewrite the project freely because
 * the checkpoint and the review cover it; a file beyond the working tree is
 * covered by neither, so the user is asked every time and the box says why.
 * Return: 1 to allow, 0 to deny.
 */
static int ai_agent_ask_outside_write(FENSTER *f, const char *path, const char *content)
{
 wpe_ai_trace("agent write outside workspace %s", path);
 return ai_agent_confirm_write(f, path, content, 1);
}

/**
 * ai_agent_report_outside - Record in the pane that a written file lies outside the workspace.
 * @f: the window the pane belongs to; @path: the file, as the agent named it.
 *
 * The box warned before the choice; this line is the record after it, so a
 * user reading the pane later knows which file to look for beyond the tree,
 * and the trace keeps the same fact.
 */
static void ai_agent_report_outside(FENSTER *f, const char *path)
{
 char line[1400];
 snprintf(line, sizeof line, "[agent] wrote %s, %s", path, AI_PLAN_OUTSIDE_WORD);
 ai_pane(f, line, AI_ROLE_MACHINERY, 0);
 wpe_ai_trace("agent wrote outside workspace %s", path);
}

/* The pane line for a checkpoint, at launch and before a write that needs one. */
#define AI_AGENT_CHECKPOINT_LINE \
  "[agent] checkpoint taken - changes are reviewable/revertible at the end"

/**
 * ai_agent_ensure_checkpoint - Take this run's checkpoint if the dial now calls for one.
 * @op: the running agent task.
 *
 * A run that may write without asking (dial at edits or auto) is covered by a
 * checkpoint, so the review at the end can show and revert what it wrote.  The
 * launch takes it when the dial is already raised; the dial is read live at
 * each approval and Alt-G y can raise it while a turn is out, so the write path
 * asks the same question again and takes the checkpoint before the first write
 * that follows.  Under ask every write is approved in its own box and nothing
 * is taken; a run never takes two.
 */
static void ai_agent_ensure_checkpoint(ai_async_op *op)
{
 char **scope;
 int nsc;
 if (e_ai_policy == WPE_AI_POLICY_ASK) return;
 if (op->checkpointed) return;
 nsc = wpe_ai_scope_files(op->f, e_project_is_open(), 1, &scope);
 wpe_ai_checkpoint_create(op->f, scope, nsc);
 wpe_ai_free_list(scope, nsc);
 op->checkpointed = 1;
 ai_pane(op->f, AI_AGENT_CHECKPOINT_LINE, AI_ROLE_MACHINERY, 0);
}

/**
 * ai_agent_commit_write - Approve and write `content` to `path`, reloading it if open.
 * @op: the running agent task (its window owns the pane); @tool: write_file or
 * apply_patch, for the deny line; @path: the file, as the agent named it;
 * @content: its new text, NULL when the model sent none.
 *
 * Shared by write_file and apply_patch so both go through the same review: a
 * file inside the workspace follows the permission dial (ai_agent_approve_write),
 * a file outside it always asks with the marked box (ai_agent_ask_outside_write)
 * and is reported as outside once written.  Before the dial is consulted the
 * run's checkpoint is taken if the dial now calls for one and none exists yet
 * (ai_agent_ensure_checkpoint), so a dial raised mid-run still ends in a
 * review.  A refusal is recorded by ai_agent_deny, as for a command.  Runs the
 * post-edit hook after a successful write, within the write's undo step.
 * Returns a malloc'd TOOL RESULT.
 */
static char *ai_agent_commit_write(ai_async_op *op, const char *tool, const char *path,
                                   const char *content)
{
 FENSTER *f = op->f;
 int outside, allow;
 if (!content) return strdup("(no content)");
 ai_agent_ensure_checkpoint(op);
 outside = wpe_ai_path_outside_workspace(f, path);
 if (outside) allow = ai_agent_ask_outside_write(f, path, content);
 else allow = ai_agent_approve_write(f, path, content);
 if (!allow) return ai_agent_deny(f, tool, path);
 if (!ai_agent_write_file(path, content)) return ai_agent_report_write_failed(f, path);
 ai_agent_show_written(f, path);
 if (outside) ai_agent_report_outside(f, path);
 return strdup("(written)");
}

/**
 * ai_tool_body_content - The file content a write_file body carries.
 * @firstnl: the newline that ends the "TOOL write_file <path>" line.
 *
 * Every body line up to the "@@END" line is content, each with its newline,
 * the last one included: "hello" followed by a newline and the marker is the
 * six bytes "hello\n", so a text file the agent writes ends with a newline as
 * one saved by the editor does.  The cut used to stop before that newline,
 * and every agent-written file lacked its final newline unless the model left
 * a blank line before the marker.  CR bytes are kept, so CRLF content lands
 * verbatim.  A body that is only the marker is empty content; a body with no
 * marker is taken whole.
 * Return: the malloc'd content, NULL out of memory.
 */
static char *ai_tool_body_content(const char *firstnl)
{
 const char *body = firstnl + 1;
 const char *endm;
 size_t len;
 char *content;
 if (!strncmp(body, AI_TOOL_BODY_END_WORD, strlen(AI_TOOL_BODY_END_WORD)))
  len = 0;
 else {
  endm = strstr(body, AI_TOOL_BODY_END_LINE);
  if (endm) len = (size_t)(endm + 1 - body);       /* through the final newline */
  else len = strlen(body);
 }
 content = malloc(len + 1);
 if (!content) return NULL;
 memcpy(content, body, len);
 content[len] = '\0';
 return content;
}

/* The three marker lines of a SEARCH/REPLACE block. */
#define AI_SR_SEARCH  "<<<<<<< SEARCH"
#define AI_SR_DIVIDER "======="
#define AI_SR_REPLACE ">>>>>>> REPLACE"
/* How much of a SEARCH text the file does not hold the error quotes. */
#define AI_SR_SNIP_MAX 80
/* The reason when a copy of a block's text could not be made. */
#define AI_SR_NO_MEMORY "out of memory"

/* What ai_sr_parse_block found at the point it was given. */
enum ai_sr_parse { AI_SR_NO_MORE = 0, AI_SR_BLOCK = 1, AI_SR_MALFORMED = -1 };

/* One parsed block: its two texts as spans of the patch body, and where the
   next block may start. */
struct ai_sr_block {
 const char *search;
 size_t      sl;
 const char *replace;
 size_t      rl;
 const char *next;
};

/**
 * ai_sr_after_marker - The text that follows a marker line.
 * @marker: where the marker was found; @word: the marker's text.
 * Return: the byte after the marker and its newline, when it has one.
 */
static const char *ai_sr_after_marker(const char *marker, const char *word)
{
 const char *p = marker + strlen(word);
 if (*p == '\n') p++;
 return p;
}

/**
 * ai_sr_span_end - Where a block's text ends before the marker that closes it.
 * @start: the text's start; @marker: the closing marker.
 * Return: the marker, or the newline before it when the text has one, so the
 * line break that ends the block is not part of what is matched.
 */
static const char *ai_sr_span_end(const char *start, const char *marker)
{
 if (marker > start && marker[-1] == '\n') return marker - 1;
 return marker;
}

/**
 * ai_sr_malformed - Record why a block could not be read.
 * @err/@errn: where the reason goes; @why: the reason.
 * Return: AI_SR_MALFORMED, so the caller hands it back in one line.
 */
static enum ai_sr_parse ai_sr_malformed(char *err, size_t errn, const char *why)
{
 snprintf(err, errn, "%s", why);
 return AI_SR_MALFORMED;
}

/**
 * ai_sr_parse_block - Read the next SEARCH/REPLACE block of a patch body.
 * @p: where to look from; @stop: the "@@END" line, NULL when the body has
 * none; @b: receives the block; @err/@errn: the reason when it is malformed.
 *
 * A block cut short (no divider, no REPLACE line) is named for what it lacks,
 * so the model's retry can fix the block instead of guessing.
 * Return: AI_SR_BLOCK with @b filled, AI_SR_NO_MORE past the last block,
 * AI_SR_MALFORMED with @err set.
 */
static enum ai_sr_parse ai_sr_parse_block(const char *p, const char *stop,
                                          struct ai_sr_block *b, char *err, size_t errn)
{
 const char *sb = strstr(p, AI_SR_SEARCH), *mid, *re, *rend;
 if (!sb || (stop && sb >= stop)) return AI_SR_NO_MORE;
 sb = ai_sr_after_marker(sb, AI_SR_SEARCH);
 mid = strstr(sb, AI_SR_DIVIDER);
 if (!mid) return ai_sr_malformed(err, errn, "block missing the ======= divider");
 re = ai_sr_after_marker(mid, AI_SR_DIVIDER);
 rend = strstr(re, AI_SR_REPLACE);
 if (!rend) return ai_sr_malformed(err, errn, "block missing >>>>>>> REPLACE");
 b->search  = sb;
 b->sl      = (size_t)(ai_sr_span_end(sb, mid) - sb);
 b->replace = re;
 b->rl      = (size_t)(ai_sr_span_end(re, rend) - re);
 b->next    = rend + strlen(AI_SR_REPLACE);
 return AI_SR_BLOCK;
}

/**
 * ai_span_dup - A NUL-terminated copy of @len bytes of @s.
 * Return: the malloc'd copy, NULL out of memory.
 */
static char *ai_span_dup(const char *s, size_t len)
{
 char *out = malloc(len + 1);
 if (!out) return NULL;
 memcpy(out, s, len);
 out[len] = '\0';
 return out;
}

/**
 * ai_sr_missing - Record that the file does not hold a block's SEARCH text.
 * @search: the text; @sl: its length; @err/@errn: where the reason goes.
 *
 * The reason quotes the text's first line, cut short, so the model sees which
 * block missed and the user reading the pane sees what it looked for.
 * Return: NULL, so the caller hands back "no new text" in one line.
 */
static char *ai_sr_missing(const char *search, size_t sl, char *err, size_t errn)
{
 char snip[AI_SR_SNIP_MAX], *nlp;
 size_t k = sl;
 if (k > sizeof snip - 1) k = sizeof snip - 1;
 memcpy(snip, search, k);
 snip[k] = '\0';
 nlp = strchr(snip, '\n');
 if (nlp) *nlp = '\0';
 snprintf(err, errn, "SEARCH text not found: \"%s\"", snip);
 return NULL;
}

/**
 * ai_sr_splice - The file's text with one hit replaced.
 * @cur: the text; @hit: where the SEARCH text stands in it; @sl: its length;
 * @replace/@rl: the new text and its length.
 * Return: the malloc'd new text, NULL out of memory.
 */
static char *ai_sr_splice(const char *cur, const char *hit, size_t sl,
                          const char *replace, size_t rl)
{
 size_t pre = (size_t)(hit - cur), post = strlen(hit + sl);
 char *nc = malloc(pre + rl + post + 1);
 if (!nc) return NULL;
 memcpy(nc, cur, pre);
 memcpy(nc + pre, replace, rl);
 memcpy(nc + pre + rl, hit + sl, post);
 nc[pre + rl + post] = '\0';
 return nc;
}

/**
 * ai_sr_replace_first - Replace the first occurrence of a block's SEARCH text.
 * @cur: the file's text; @search/@replace: the block's texts, NUL-terminated;
 * @b: the block, for the lengths; @err/@errn: the reason on failure.
 *
 * An empty SEARCH text matches nothing, so a block that forgot its text is
 * an error and not an insertion at the file's start.
 * Return: the malloc'd new text, NULL with @err set.
 */
static char *ai_sr_replace_first(const char *cur, const char *search, const char *replace,
                                 const struct ai_sr_block *b, char *err, size_t errn)
{
 const char *hit = NULL;
 char *nc;
 if (b->sl) hit = strstr(cur, search);
 if (!hit) return ai_sr_missing(search, b->sl, err, errn);
 nc = ai_sr_splice(cur, hit, b->sl, replace, b->rl);
 if (!nc) snprintf(err, errn, "%s", AI_SR_NO_MEMORY);
 return nc;
}

/**
 * ai_sr_apply_block - Apply one parsed block to the file's text.
 * @cur: the text; @b: the block; @err/@errn: the reason on failure.
 * Return: the malloc'd new text, NULL with @err set.
 */
static char *ai_sr_apply_block(const char *cur, const struct ai_sr_block *b,
                               char *err, size_t errn)
{
 char *search = ai_span_dup(b->search, b->sl);
 char *replace = ai_span_dup(b->replace, b->rl);
 char *nc = NULL;
 if (search && replace) nc = ai_sr_replace_first(cur, search, replace, b, err, errn);
 else snprintf(err, errn, "%s", AI_SR_NO_MEMORY);
 free(search);
 free(replace);
 return nc;
}

/**
 * ai_sr_fail - Give up on a patch: drop the text so far and record the reason.
 * @cur: the text built so far, freed; @err/@errn: where the reason goes;
 * @why: the reason, or NULL when a helper already wrote it.
 * Return: 0, the patch's failure, so the caller hands it back in one line.
 */
static int ai_sr_fail(char *cur, char *err, size_t errn, const char *why)
{
 if (why) snprintf(err, errn, "%s", why);
 free(cur);
 return 0;
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
 const char *base = "";
 const char *p = body;
 const char *stop = strstr(body, AI_TOOL_BODY_END_LINE);
 struct ai_sr_block b;
 enum ai_sr_parse got;
 char *cur, *nc;
 int blocks = 0;

 if (old) base = old;
 cur = strdup(base);
 if (!cur) return ai_sr_fail(cur, err, errn, AI_SR_NO_MEMORY);
 for (;;) {
  got = ai_sr_parse_block(p, stop, &b, err, errn);
  if (got == AI_SR_NO_MORE) break;
  if (got == AI_SR_MALFORMED) return ai_sr_fail(cur, err, errn, NULL);
  nc = ai_sr_apply_block(cur, &b, err, errn);
  if (!nc) return ai_sr_fail(cur, err, errn, NULL);
  free(cur);
  cur = nc;
  p = b.next;
  blocks++;
 }
 if (!blocks) return ai_sr_fail(cur, err, errn, "no SEARCH/REPLACE block found");
 *out = cur;
 return 1;
}

/* The most text a fetched page becomes: enough to summarise, not enough to
   flood a model's window; the note that ends a page cut there. */
#define AI_HTML_TEXT_CAP 12000
#define AI_HTML_CUT_MARK "\n...(truncated)"
/* The opening tags whose whole body is dropped, and how far into either the
   scan for its closing tag starts (past "<style", the shorter). */
#define AI_HTML_SCRIPT_OPEN     "<script"
#define AI_HTML_SCRIPT_OPEN_LEN (sizeof AI_HTML_SCRIPT_OPEN - 1)
#define AI_HTML_STYLE_OPEN      "<style"
#define AI_HTML_STYLE_OPEN_LEN  (sizeof AI_HTML_STYLE_OPEN - 1)
#define AI_HTML_SCRIPT_CLOSE    "</script"
#define AI_HTML_STYLE_CLOSE     "</style"
/* The offsets, after '<', of the letters that tell script from style. */
#define AI_HTML_TAG_LETTER_1 1
#define AI_HTML_TAG_LETTER_3 3

/**
 * ai_html_skip_tag - Step past one tag.
 * @html: the page; @i: at the tag's '<'.
 * Return: the index after its '>', or the page's end.
 */
static size_t ai_html_skip_tag(const char *html, size_t i)
{
 while (html[i] && html[i] != '>') i++;
 if (html[i]) i++;
 return i;
}

/**
 * ai_html_is_script_or_style - Does a <script> or <style> element open here?
 * @html: the page; @i: at a '<'.
 * Return: 1 when it does, whatever the letters' case.
 */
static int ai_html_is_script_or_style(const char *html, size_t i)
{
 if (!strncasecmp(html + i, AI_HTML_SCRIPT_OPEN, AI_HTML_SCRIPT_OPEN_LEN)) return 1;
 if (!strncasecmp(html + i, AI_HTML_STYLE_OPEN, AI_HTML_STYLE_OPEN_LEN)) return 1;
 return 0;
}

/**
 * ai_html_close_tag - The closing tag of the element opening here.
 * @html: the page; @i: at the '<' of a script or style tag.
 * Return: "</script" when the letters after '<' spell script, "</style" otherwise.
 */
static const char *ai_html_close_tag(const char *html, size_t i)
{
 int s = html[i + AI_HTML_TAG_LETTER_1] == 's' || html[i + AI_HTML_TAG_LETTER_1] == 'S';
 int r = html[i + AI_HTML_TAG_LETTER_3] == 'r' || html[i + AI_HTML_TAG_LETTER_3] == 'R';
 if (s && r) return AI_HTML_SCRIPT_CLOSE;
 return AI_HTML_STYLE_CLOSE;
}

/**
 * ai_html_skip_element - Step past a <script> or <style> element, body and all.
 * @html: the page; @i: at its '<'.
 *
 * Code and style sheets are not what the model asked to read: dropping only
 * their tags would hand it the page's scripts as text.
 * Return: the index after the closing tag's '>', or the page's end.
 */
static size_t ai_html_skip_element(const char *html, size_t i)
{
 const char *close = ai_html_close_tag(html, i);
 size_t cl = strlen(close);
 i += AI_HTML_STYLE_OPEN_LEN;
 while (html[i] && strncasecmp(html + i, close, cl)) i++;
 return ai_html_skip_tag(html, i);
}

/**
 * ai_html_add_space - Keep the words of two neighbouring tags apart.
 * @out: the text so far; @o: its length, advanced when a blank is added.
 *
 * "<td>a</td><td>b</td>" reads "a b", not "ab"; no blank is added after a
 * line break or another blank, so the text does not fill with them.
 */
static void ai_html_add_space(char *out, size_t *o)
{
 if (*o == 0) return;
 if (out[*o - 1] == '\n' || out[*o - 1] == ' ') return;
 out[(*o)++] = ' ';
}

/**
 * ai_html_skip_markup - Drop the tag or element at @i, keeping the words apart.
 * @html: the page; @i: at a '<'; @out/@o: the text so far.
 * Return: the index of the next byte to read.
 */
static size_t ai_html_skip_markup(const char *html, size_t i, char *out, size_t *o)
{
 if (ai_html_is_script_or_style(html, i)) return ai_html_skip_element(html, i);
 i = ai_html_skip_tag(html, i);
 ai_html_add_space(out, o);
 return i;
}

/**
 * ai_html_decode_entity - Turn one of the common entities into its character.
 * @html: the page; @i: the index read next, advanced past the entity when
 * one is decoded; @out/@o: the text, extended by the character.
 *
 * The seven entities a page of prose uses most are decoded so the model
 * reads "&" and "<" rather than their encodings; any other entity, and any
 * other byte, is left to be copied as it is.
 * Return: 1 when an entity was decoded, 0 when the byte at @i is not one.
 */
static int ai_html_decode_entity(const char *html, size_t *i, char *out, size_t *o)
{
 static const struct { const char *e; char c; } ent[] = {
  {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'},
  {"&#39;", '\''}, {"&apos;", '\''}, {"&nbsp;", ' '}
 };
 int k;
 if (html[*i] != '&') return 0;
 for (k = 0; k < (int)(sizeof ent / sizeof ent[0]); k++) {
  size_t el = strlen(ent[k].e);
  if (strncmp(html + *i, ent[k].e, el)) continue;
  out[(*o)++] = ent[k].c;
  *i += el;
  return 1;
 }
 return 0;
}

/**
 * ai_html_mark_cut - End a page's text with the note that it was cut.
 * @out: the text; @o: its length; @room: the buffer's size.
 *
 * The model reads that the page went on, so it asks for the part it needs
 * instead of taking the first part for the whole.
 * Return: the text's new length (unchanged when the note does not fit).
 */
static size_t ai_html_mark_cut(char *out, size_t o, size_t room)
{
 size_t ml = strlen(AI_HTML_CUT_MARK);
 if (o + ml >= room) return o;
 memcpy(out + o, AI_HTML_CUT_MARK, ml);
 return o + ml;
}

/**
 * ai_html_to_text - Reduce a fetched page to readable text for the model.
 * @html: the page's bytes.
 *
 * Drops script and style bodies and every tag, decodes the common entities
 * and caps the length, so a page reads as text, not markup, and does not
 * flood the model's window.  Not a real parser: just enough for prose.
 * Return: the malloc'd text (the caller frees), NULL out of memory.
 */
static char *ai_html_to_text(const char *html)
{
 size_t n = html ? strlen(html) : 0, o = 0, i = 0;
 char *out = malloc(n + 1);
 if (!out) return NULL;
 while (html[i] && o < AI_HTML_TEXT_CAP) {
  if (html[i] == '<') i = ai_html_skip_markup(html, i, out, &o);
  else if (!ai_html_decode_entity(html, &i, out, &o)) out[o++] = html[i++];
 }
 if (o >= AI_HTML_TEXT_CAP) o = ai_html_mark_cut(out, o, n + 1);
 out[o] = '\0';
 return out;
}

/* A percent-escape is three bytes; a byte is written only when it and the
   NUL after it fit. */
#define AI_URL_ESCAPE_LEN 3
#define AI_URL_HEX_SHIFT  4
#define AI_URL_HEX_MASK   15

/**
 * ai_url_unreserved - May this byte stand in a URL as it is?
 * @c: the byte.
 * Return: 1 for the RFC 3986 unreserved set: letters, digits, "-", "_", ".", "~".
 */
static int ai_url_unreserved(unsigned char c)
{
 if (c >= 'A' && c <= 'Z') return 1;
 if (c >= 'a' && c <= 'z') return 1;
 if (c >= '0' && c <= '9') return 1;
 return c == '-' || c == '_' || c == '.' || c == '~';
}

/**
 * ai_url_encode - Percent-encode a query for a URL.
 * @s: the query as the model wrote it; @out/@outsz: receives the encoding.
 *
 * Every byte outside the unreserved set is escaped, so a query with blanks,
 * quotes or an ampersand reaches the search engine as the query and not as
 * extra parameters.  A query too long for the room is cut at a whole escape.
 */
static void ai_url_encode(const char *s, char *out, size_t outsz)
{
 static const char hex[] = "0123456789ABCDEF";
 size_t o = 0;
 for (; s && *s && o + AI_URL_ESCAPE_LEN + 1 < outsz; s++) {
  unsigned char c = (unsigned char)*s;
  if (ai_url_unreserved(c)) {
   out[o++] = (char)c;
   continue;
  }
  out[o++] = '%';
  out[o++] = hex[c >> AI_URL_HEX_SHIFT];
  out[o++] = hex[c & AI_URL_HEX_MASK];
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

/* The most of a page the web tools read before reducing it to text. */
#define AI_WEB_FETCH_MAX 65536
/* Room a fetch's error line adds to the fetcher's reason for the head. */
#define AI_FETCH_ERROR_ROOM 20
/* Room for the fetcher's reason, and for the encoded query and the URL
   built around it. */
#define AI_FETCH_REASON_MAX 200
#define AI_URL_QUERY_MAX    1024
#define AI_URL_MAX          1400
/* Where a search goes when AISearchURL is not set: a keyless engine. */
#define AI_SEARCH_URL_DEFAULT "https://lite.duckduckgo.com/lite/?q=%s"

/**
 * ai_tool_error_alloc - The "TOOL ERROR: <reason>" result the model reads.
 * @reason: why the tool could not do what was asked.
 * Return: the malloc'd result, NULL out of memory.
 */
static char *ai_tool_error_alloc(const char *reason)
{
 size_t n = sizeof AI_TOOL_ERROR_HEAD + strlen(reason);
 char *out = malloc(n);
 if (out) snprintf(out, n, AI_TOOL_ERROR_HEAD "%s", reason);
 return out;
}

/**
 * ai_agent_tool_run_command - Run the model's command, bounded in size and time.
 * @f: the window the pane belongs to; @arg: the command line, as the model wrote it.
 *
 * The dial decides first; a refusal is recorded by ai_agent_deny.  The pane
 * says the command is running before it starts, and when it outlives
 * AIToolTimeout the command is ended, the pane says so and the model reads
 * "TOOL ERROR: timed out after N s", so a hung command costs the user the
 * configured seconds and not the editor.
 * Return: the malloc'd result the model reads.
 */
static char *ai_agent_tool_run_command(FENSTER *f, const char *arg)
{
 int status = 0, timed_out = 0;
 char *result;
 if (!ai_agent_approve(f, arg, 1)) return ai_agent_deny(f, AI_TOOL_RUN_COMMAND, arg);
 ai_agent_show_running(f, AI_TOOL_RUN_COMMAND);
 result = wpe_ai_run_capture_bounded(arg, e_ai_tool_timeout, &status, &timed_out);
 if (timed_out) ai_agent_report_timeout(f, AI_TOOL_RUN_COMMAND, result);
 return result;
}

/**
 * ai_agent_fetch_as_text - GET a page and reduce it to text for the model.
 * @url: what to fetch; @failed: the words for a failure with no reason.
 *
 * The body is reduced to text so the model reads content, not markup; a
 * failure comes back as a TOOL ERROR line the model can read and act on.
 * Return: the malloc'd result, or NULL out of memory.
 */
static char *ai_agent_fetch_as_text(const char *url, const char *failed)
{
 char *raw = NULL, *result, werr[AI_FETCH_REASON_MAX];
 const char *reason = failed;
 size_t n;
 werr[0] = '\0';
 if (wpe_ai_web_fetch(url, AI_WEB_FETCH_MAX, &raw, werr, sizeof werr) == 0 && raw) {
  result = ai_html_to_text(raw);
  free(raw);
  return result;
 }
 if (werr[0]) reason = werr;
 n = strlen(werr) + AI_FETCH_ERROR_ROOM;
 result = malloc(n);
 if (result) snprintf(result, n, AI_TOOL_ERROR_HEAD "%s", reason);
 return result;
}

/**
 * ai_search_url_template - The search URL the user configured, or the default.
 * Return: the template, with "%s" where the query goes.
 */
static const char *ai_search_url_template(void)
{
 if (e_ai_search_url && *e_ai_search_url) return e_ai_search_url;
 return AI_SEARCH_URL_DEFAULT;
}

/* The web switch's side of the two web tools, defined with the chat's below. */
static int ai_web_is_off(void);
static char *ai_agent_refuse_web(FENSTER *f, const char *tool, const char *arg);
static void ai_agent_note_search_engine(FENSTER *f, const char *surl);

/**
 * ai_agent_tool_web_fetch - Read a web page for the model.
 * @f: the window the pane belongs to; @arg: the URL.
 *
 * Network egress, so it goes through the same approval as run_command, and the
 * pane says the fetch is running while the editor waits on it (the fetch has
 * its own time limit).  This is the built-in agent's only route onto the web,
 * for models that have no web tool of their own.
 * Return: the malloc'd result the model reads.
 */
static char *ai_agent_tool_web_fetch(FENSTER *f, const char *arg)
{
 if (ai_web_is_off()) return ai_agent_refuse_web(f, AI_TOOL_WEB_FETCH, arg);
 if (!ai_agent_approve(f, arg, 1)) return ai_agent_deny(f, AI_TOOL_WEB_FETCH, arg);
 ai_agent_show_running(f, AI_TOOL_WEB_FETCH);
 return ai_agent_fetch_as_text(arg, "fetch failed");
}

/**
 * ai_search_url_for_query - The URL a search for @arg goes to.
 * @arg: the query as the model wrote it; @out: the URL; @n: its room.
 *
 * One place encodes the query into the configured template, so the agent's
 * search and the chat's sources line name the same address.
 */
static void ai_search_url_for_query(const char *arg, char *out, size_t n)
{
 char enc[AI_URL_QUERY_MAX];
 ai_url_encode(arg, enc, sizeof enc);
 ai_search_url_build(ai_search_url_template(), enc, out, n);
}

/**
 * ai_agent_tool_web_search - Search the web for the model.
 * @f: the window the pane belongs to; @arg: the query, as the model wrote it.
 *
 * Builds a query URL from the configurable template (AISearchURL, default a
 * keyless engine) and fetches it as text; the model reads the result links,
 * then web_fetch's the promising one.  Gated and announced like web_fetch.
 * Return: the malloc'd result the model reads.
 */
static char *ai_agent_tool_web_search(FENSTER *f, const char *arg)
{
 char surl[AI_URL_MAX];
 if (ai_web_is_off()) return ai_agent_refuse_web(f, AI_TOOL_WEB_SEARCH, arg);
 if (!ai_agent_approve(f, arg, 1)) return ai_agent_deny(f, AI_TOOL_WEB_SEARCH, arg);
 ai_search_url_for_query(arg, surl, sizeof surl);
 ai_agent_note_search_engine(f, surl);
 ai_agent_show_running(f, AI_TOOL_WEB_SEARCH);
 return ai_agent_fetch_as_text(surl, "search failed");
}

/* ----- the chat's web tools, behind the web switch ------------------------
 * The chat runs the agent's two web functions as they are: the dial, the
 * approval line, the running line and the denial are the agent's.  What is
 * the chat's own is the switch's first-use notice, the refusal of a tool it
 * cannot run, and the sources line that ends an answer built from the web. */

/* The tools the chat offers the model, as the refusal names them. */
#define AI_CHAT_TOOLS_WORKSPACE "read_file, grep, list_dir, glob"
#define AI_CHAT_TOOLS_WITH_WEB  AI_CHAT_TOOLS_WORKSPACE ", web_search, web_fetch"
/* The lead of a refused web tool's result, and the pane's reasons. */
#define AI_CHAT_REFUSED_WEB_OFF     "web access is off"
#define AI_CHAT_REFUSED_WEB_NOT_NOW "web access not enabled"
#define AI_CHAT_WHY_WEB_OFF     "web access is off (Options > AI turns it on)"
#define AI_CHAT_WHY_WEB_NOT_NOW "web access not enabled"
/* Room for a refusal's result and its pane line. */
#define AI_CHAT_REFUSAL_MAX 320
/* The line that ends an answer which read the web, and the glue between URLs. */
#define AI_SOURCES_HEAD "    sources: "
#define AI_SOURCES_SEP  ", "
/* First size of the sources buffer; it doubles when it fills. */
#define AI_SOURCES_FIRST 256
/* The notice's line that names the engine, with room for its host. */
#define AI_URL_HOST_MAX 200
/* The notice's attention line, and the pane line for "not now". */
#define AI_WEB_NOTICE_KEYS "Enter / y = enable web access      n / Esc = not now"
#define AI_WEB_NOT_NOW_LINE \
  "[AI] web access not enabled - the question is answered without it"

/* What the chat decided about reaching the web for this tool line. */
enum ai_web_answer { AI_WEB_ALLOWED = 0, AI_WEB_OFF, AI_WEB_NOT_NOW };

/**
 * ai_url_host - The host part of a URL, for a line that names where a request goes.
 * @url: the address; @out: the host (with its port, if any); @n: its room.
 *
 * The user reads "lite.duckduckgo.com", not a 200-byte query URL.
 */
static void ai_url_host(const char *url, char *out, size_t n)
{
 const char *p = strstr(url, "://");
 size_t l;
 if (p) p += 3; else p = url;
 l = strcspn(p, "/?#");
 if (l >= n) l = n - 1;
 memcpy(out, p, l);
 out[l] = '\0';
}

/**
 * ai_agent_note_search_engine - Name the engine a search goes to, before it goes.
 * @f: the window the pane belongs to; @surl: the query URL.
 *
 * The approval line shows the query; this line shows where it is sent, so
 * the transcript names both halves of what left the machine.
 */
static void ai_agent_note_search_engine(FENSTER *f, const char *surl)
{
 char host[AI_URL_HOST_MAX], line[AI_AGENT_PANE_LINE_MAX];
 ai_url_host(surl, host, sizeof host);
 snprintf(line, sizeof line, "[agent] search engine: %s", host);
 ai_pane(f, line, AI_ROLE_MACHINERY, 0);
}

/**
 * ai_web_is_off - May the agent's web tools NOT run?
 *
 * Only an answered "on" opens the web.  The chat can ask its notice because a
 * question is waiting for an answer; an agent task runs on its own, and at the
 * `auto` dial nothing would ask, so an unanswered switch there would have let
 * the first task a user ever ran reach the network unasked.  Unanswered is
 * therefore treated as off, which is also what the manual promises, and the
 * refusal names Options > AI so the user knows where to say yes.
 * Return: 1 unless AIWeb is on.
 */
static int ai_web_is_off(void)
{
 return wpe_ai_web_state() != WPE_AI_WEB_ON;
}

/**
 * ai_agent_refuse_web - Refuse a web tool while the switch is off: model, pane, trace.
 * @f: the window the pane belongs to; @tool: the web tool; @arg: its argument.
 *
 * The pane names the switch, so a user who turned the web off in Options > AI
 * reads why the agent could not search, and the model reads an error it can
 * act on instead of a page.
 * Return: the malloc'd result the model reads.
 */
static char *ai_agent_refuse_web(FENSTER *f, const char *tool, const char *arg)
{
 char line[AI_AGENT_ARG_LINE_MAX];
 snprintf(line, sizeof line, "[agent] %s refused: %s: %s", tool, AI_CHAT_WHY_WEB_OFF, arg);
 ai_pane(f, line, AI_ROLE_MACHINERY, 0);
 wpe_ai_trace("agent web off %s %s", tool, arg);
 return ai_tool_error_alloc(AI_CHAT_WHY_WEB_OFF);
}

/**
 * ai_chat_tools_offered - The tools the chat may run, as the model is told them.
 * Return: the four workspace tools, plus the two web tools while the web is on.
 */
static const char *ai_chat_tools_offered(void)
{
 if (wpe_ai_web_state() == WPE_AI_WEB_ON) return AI_CHAT_TOOLS_WITH_WEB;
 return AI_CHAT_TOOLS_WORKSPACE;
}

/**
 * ai_chat_refuse_tool - Answer a tool line the chat cannot run: model, pane, trace.
 * @f: the window the pane belongs to; @tool: the tool the model named;
 * @lead: the result's first words; @why: the pane's reason.
 *
 * The painter has already shown the tool line as a dim status, so this is
 * the line the user reads: which tool was not run and why.  The model reads
 * the tools it may use instead, so the turn goes on to an answer.
 * Return: the malloc'd result the model reads.
 */
static char *ai_chat_refuse_tool(FENSTER *f, const char *tool, const char *lead,
                                 const char *why)
{
 char result[AI_CHAT_REFUSAL_MAX], line[AI_CHAT_REFUSAL_MAX];
 snprintf(result, sizeof result, "(%s: you may use %s; answer from what you have)",
          lead, ai_chat_tools_offered());
 snprintf(line, sizeof line, "[AI] %s was not run: %s; answering from what the chat has.",
          tool, why);
 ai_pane(f, line, AI_ROLE_MACHINERY, 0);
 wpe_ai_trace("chat tool refused %s: %s", tool, result);
 return strdup(result);
}

/**
 * ai_web_notice - Ask once, at first need, whether the chat may reach the web.
 * @f: the window the pane belongs to.
 *
 * Shaped like the assistant's own first-use notice: plain lines saying what
 * leaves the machine (the query, to the engine set in Options > AI) and that
 * the model stays where it is, one attention line, a blocking read.  y or
 * Enter saves AIWeb : on at once, so a crash later cannot lose the answer;
 * n or Esc leaves the switch unanswered, so the question comes back at the
 * next need, and the permanent off lives in Options > AI, which the notice
 * names.  Any other key leaves the notice up.
 * Return: 1 when the web is now on, 0 for not now.
 */
static int ai_web_notice(FENSTER *f)
{
 char host[AI_URL_HOST_MAX], line[AI_CHAT_NOTE_MAX];
 ai_url_host(ai_search_url_template(), host, sizeof host);
 ai_pane(f, "Web access -- first use.  To answer this the assistant wants to search the",
         AI_ROLE_MACHINERY, 1);
 snprintf(line, sizeof line,
          "web.  Your query is sent to the search engine set in Options > AI (now %s)", host);
 ai_pane(f, line, AI_ROLE_MACHINERY, 1);
 ai_pane(f, "and the pages it picks are fetched from there; nothing else leaves this",
         AI_ROLE_MACHINERY, 1);
 ai_pane(f, "machine, and the model stays where it is -- local stays local.  Every search",
         AI_ROLE_MACHINERY, 1);
 ai_pane(f, "and every page is shown here, and Options > AI turns this off again.",
         AI_ROLE_MACHINERY, 1);
 ai_pane_attn(f, AI_WEB_NOTICE_KEYS);
 for (;;) {
  int c = e_getch();
  if (c == WPE_ESC || e_toupper(c) == 'N') {
   ai_pane(f, AI_WEB_NOT_NOW_LINE, AI_ROLE_MACHINERY, 0);
   wpe_ai_trace("chat web access not enabled");
   return 0;
  }
  if (e_toupper(c) == 'Y' || c == WPE_CR || c == WPE_WR) {
   e_ai_web = WPE_AI_WEB_ON;
   /* The user answered the question, so this is configuration from now on,
      even if XWPE_AI_WEB was what raised it this session. */
   wpe_ai_cfg_user_changed(WPE_AI_CFG_WEB);
   ai_persist_settings(f);
   wpe_ai_trace("chat web access enabled");
   return 1;
  }
 }
}

/**
 * ai_chat_web_answer - May this turn reach the web?
 * @s: the chat asking.
 *
 * On: yes.  Off: no.  Unanswered: the notice decides, and it is shown at
 * most once per question -- a model that keeps asking after a "not now"
 * reads the refusal again, it does not put the notice back on the screen.
 * Return: AI_WEB_ALLOWED, AI_WEB_OFF or AI_WEB_NOT_NOW.
 */
static enum ai_web_answer ai_chat_web_answer(ai_chat_session *s)
{
 int state = wpe_ai_web_state();
 if (state == WPE_AI_WEB_ON) return AI_WEB_ALLOWED;
 if (state == WPE_AI_WEB_OFF) return AI_WEB_OFF;
 if (s->web_declined) return AI_WEB_NOT_NOW;   /* asked once per question */
 if (ai_web_notice(s->ref)) return AI_WEB_ALLOWED;
 s->web_declined = 1;
 return AI_WEB_NOT_NOW;
}

/**
 * ai_chat_web_result_read - Did a web tool read what it was asked for?
 * @result: what the tool returned.
 *
 * A denial and a TOOL ERROR read nothing, so they name no source.
 * Return: 1 when the result is a page or search results.
 */
static int ai_chat_web_result_read(const char *result)
{
 if (!result) return 0;
 if (!strcmp(result, AI_TOOL_DENIED_RESULT)) return 0;
 if (!strncmp(result, AI_TOOL_ERROR_HEAD, AI_TOOL_ERROR_HEAD_LEN)) return 0;
 return 1;
}

/**
 * ai_chat_sources_append - Add a URL to this question's sources.
 * @s: the chat; @url: the address a web tool read.
 */
static void ai_chat_sources_append(ai_chat_session *s, const char *url)
{
 size_t ul = strlen(url), need = s->srclen + ul + sizeof AI_SOURCES_SEP;
 if (!ai_chat_buf_reserve(&s->sources, &s->srccap, need, AI_SOURCES_FIRST))
  return;
 if (s->srclen) {
  memcpy(s->sources + s->srclen, AI_SOURCES_SEP, sizeof AI_SOURCES_SEP - 1);
  s->srclen += sizeof AI_SOURCES_SEP - 1;
 }
 memcpy(s->sources + s->srclen, url, ul);
 s->srclen += ul;
 s->sources[s->srclen] = '\0';
}

/**
 * ai_chat_note_source - Remember the URL a web tool read, for the sources line.
 * @s: the chat; @tool/@arg: the web tool and its argument; @result: what it got.
 *
 * A search is remembered by the URL its query went to, a fetch by the page,
 * so the answer ends with every address it was built from.
 */
static void ai_chat_note_source(ai_chat_session *s, const char *tool, const char *arg,
                                const char *result)
{
 char url[AI_URL_MAX];
 if (!ai_chat_web_result_read(result)) return;
 if (!strcmp(tool, AI_TOOL_WEB_SEARCH))
  ai_search_url_for_query(arg, url, sizeof url);
 else
  snprintf(url, sizeof url, "%s", arg);
 ai_chat_sources_append(s, url);
}

/**
 * ai_chat_run_web_tool - Run a chat's web tool behind the switch and the dial.
 * @s: the chat; @tool: web_search or web_fetch; @arg: the query or the URL.
 *
 * The switch is asked first (its notice at first need), then the agent's
 * function approves through the dial, prints its lines and runs; a refusal
 * is answered aloud so the turn goes on.
 * Return: the malloc'd result the model reads.
 */
static char *ai_chat_run_web_tool(ai_chat_session *s, const char *tool, const char *arg)
{
 FENSTER *f = s->ref;
 enum ai_web_answer answer = ai_chat_web_answer(s);
 char *result;
 if (answer == AI_WEB_OFF)
  return ai_chat_refuse_tool(f, tool, AI_CHAT_REFUSED_WEB_OFF, AI_CHAT_WHY_WEB_OFF);
 if (answer == AI_WEB_NOT_NOW)
  return ai_chat_refuse_tool(f, tool, AI_CHAT_REFUSED_WEB_NOT_NOW, AI_CHAT_WHY_WEB_NOT_NOW);
 if (!strcmp(tool, AI_TOOL_WEB_SEARCH))
  result = ai_agent_tool_web_search(f, arg);
 else
  result = ai_agent_tool_web_fetch(f, arg);
 ai_chat_note_source(s, tool, arg, result);
 return result;
}

/**
 * ai_chat_show_sources - End an answer that read the web with the URLs it read.
 * @s: the chat, its answer painted.
 *
 * The user can check the answer against the pages it came from; a turn that
 * never reached the web ends as before.
 */
static void ai_chat_show_sources(ai_chat_session *s)
{
 char *line;
 size_t n;
 if (!s->sources || !s->srclen) return;
 if (!ai_pane_win(s->ref)) return;
 n = sizeof AI_SOURCES_HEAD + s->srclen;
 line = malloc(n);
 if (!line) return;
 snprintf(line, n, AI_SOURCES_HEAD "%s", s->sources);
 ai_pane(s->ref, line, AI_ROLE_MACHINERY, 0);
 wpe_ai_trace("chat sources %s", s->sources);
 free(line);
}

/* The line that ends a run: the model's answer or summary follows it. */
#define AI_DONE_MARKER     "DONE"
#define AI_DONE_MARKER_LEN (sizeof AI_DONE_MARKER - 1)
/* The pane line for a run that ended with nothing to say. */
#define AI_AGENT_DONE_LINE "[agent] done"
/* The most of a reasoning preamble the pane shows before a tool line. */
#define AI_AGENT_PREAMBLE_MAX 1024
/* Room for the reason a patch could not be applied. */
#define AI_PATCH_ERR_MAX 300
/* What the model reads for a patch it cannot make: no such file, no blocks. */
#define AI_PATCH_NO_FILE_RESULT   "TOOL ERROR: file not found (use write_file to create it)"
#define AI_PATCH_NO_BLOCKS_RESULT "TOOL ERROR: apply_patch needs SEARCH/REPLACE blocks"
/* What the user and the model read once the checklist is laid out, and what
   the model reads for a ticked step. */
#define AI_PLAN_LAID_OUT_LINE \
  "[plan] laid out - the checklist is in the Messages window (updates as steps finish)"
#define AI_PLAN_LAID_OUT_RESULT \
  "(plan shown -- now do the steps, one tool per turn, reporting each with TOOL step_done <task>)"
#define AI_PLAN_STEP_RESULT "(recorded)"

/* What the scan of a reply found: no protocol line, the DONE line, a TOOL line. */
enum ai_agent_marker { AI_MARK_NONE = 0, AI_MARK_DONE, AI_MARK_TOOL };

/**
 * ai_agent_marker_at - Which protocol line, if any, starts at @line.
 * @line: the start of a line of the reply.
 * Return: AI_MARK_TOOL, AI_MARK_DONE, or AI_MARK_NONE for prose.
 */
static enum ai_agent_marker ai_agent_marker_at(const char *line)
{
 if (!strncmp(line, AI_TOOL_PREFIX, AI_TOOL_PREFIX_LEN)) return AI_MARK_TOOL;
 if (!strncmp(line, AI_DONE_MARKER, AI_DONE_MARKER_LEN)) return AI_MARK_DONE;
 return AI_MARK_NONE;
}

/**
 * ai_agent_find_marker - Find the reply's protocol line, past any reasoning preamble.
 * @reply: the model's reply; @kind: receives what was found.
 *
 * A tool-using model -- a local one especially -- often prefaces its action
 * with a sentence of reasoning ("let me read the files first..."); judging
 * only the reply's first line then mistakes the preamble for the final answer
 * and the tool call is never run.  The line starts are scanned instead.
 * Return: the start of the marker line, NULL (with @kind AI_MARK_NONE) when
 * the reply has none.
 */
static char *ai_agent_find_marker(char *reply, enum ai_agent_marker *kind)
{
 char *scan;
 for (scan = reply; scan; ) {
  *kind = ai_agent_marker_at(scan);
  if (*kind != AI_MARK_NONE) return scan;
  scan = strchr(scan, '\n');
  if (scan) scan++;
 }
 *kind = AI_MARK_NONE;
 return NULL;
}

/**
 * ai_agent_end_with_answer - Show the reply as the agent's answer: the run is over.
 * @f: the window the pane belongs to; @reply: the whole reply; @kind: whether
 * the model said DONE or simply wrote no marker.
 *
 * The whole reply is shown, not its first line: for a question task the
 * answer follows the DONE marker, and dropping it once left the user with
 * "DONE" and no answer.  An empty reply reads "[agent] done".
 * Return: 1, the loop's "finished".
 */
static int ai_agent_end_with_answer(FENSTER *f, const char *reply, enum ai_agent_marker kind)
{
 if (reply[0]) ai_pane_multiline(f, reply, AI_ROLE_ASSISTANT, 0);
 else ai_pane_multiline(f, AI_AGENT_DONE_LINE, AI_ROLE_ASSISTANT, 0);
 if (kind == AI_MARK_DONE) wpe_ai_trace("agent done");
 else wpe_ai_trace("agent answer (no marker)");
 return 1;
}

/**
 * ai_copy_bounded - Copy @len bytes into a buffer, cut to its room, NUL-terminated.
 * @dst/@cap: the buffer and its size; @src/@len: the bytes.
 */
static void ai_copy_bounded(char *dst, size_t cap, const char *src, size_t len)
{
 if (len >= cap) len = cap - 1;
 memcpy(dst, src, len);
 dst[len] = '\0';
}

/**
 * ai_agent_show_preamble - Show the reasoning the model wrote before its tool line.
 * @f: the window the pane belongs to; @reply: the reply; @act: where the
 * tool line starts in it.
 *
 * The thought is shown as the agent's own words, so the user reads why the
 * tool is being called and not only that it is; blank lines before the tool
 * line are not shown, and a thought longer than the room is cut.
 */
static void ai_agent_show_preamble(FENSTER *f, const char *reply, const char *act)
{
 char pre[AI_AGENT_PREAMBLE_MAX];
 size_t pl = (size_t)(act - reply);
 while (pl && (reply[pl-1] == '\n' || reply[pl-1] == ' ' || reply[pl-1] == '\t')) pl--;
 if (!pl) return;
 ai_copy_bounded(pre, sizeof pre, reply, pl);
 ai_pane_multiline(f, pre, AI_ROLE_ASSISTANT, 0);
}

/**
 * ai_agent_show_tool - Record the tool the model asked for: pane and trace.
 * @f: the window the pane belongs to; @tool/@arg: the request.
 *
 * Shown before any approval or run, so the pane carries the request even
 * when the user then denies it or the tool is unknown.
 */
static void ai_agent_show_tool(FENSTER *f, const char *tool, const char *arg)
{
 char line[AI_AGENT_PANE_LINE_MAX];
 snprintf(line, sizeof line, "[agent] %s %s", tool, arg);
 ai_pane(f, line, AI_ROLE_MACHINERY, 0);
 wpe_ai_trace("agent tool=%s arg=%s", tool, arg);
}

/**
 * ai_agent_tool_write_file - Put the body of a write_file request through the write review.
 * @op: the running agent task; @path: the file; @firstnl: the newline that
 * ends the TOOL line, NULL when the request has no body.
 * Return: the malloc'd result the model reads.
 */
static char *ai_agent_tool_write_file(ai_async_op *op, const char *path, const char *firstnl)
{
 char *content = NULL, *result;
 if (firstnl) content = ai_tool_body_content(firstnl);   /* final newline included */
 result = ai_agent_commit_write(op, AI_TOOL_WRITE_FILE, path, content);
 free(content);
 return result;
}

/**
 * ai_agent_apply_patch - Apply the model's SEARCH/REPLACE blocks to a file, through the write review.
 * @op: the running agent task; @path: the file; @firstnl: the newline that
 * ends the TOOL line, NULL when the request has no body.
 *
 * A precise edit: the blocks are applied to the file as it is now and the
 * result goes through the same review as write_file, so the user sees a diff
 * of only what changed.  A missing file, a missing body or an unmatched
 * SEARCH text is a TOOL ERROR the model reads and retries from, so a wrong
 * guess at the file's text never rewrites the wrong place.
 * Return: the malloc'd result the model reads.
 */
static char *ai_agent_apply_patch(ai_async_op *op, const char *path, const char *firstnl)
{
 char *old = ai_slurp_file(path);
 char *newc = NULL, *result, perr[AI_PATCH_ERR_MAX];
 if (!old) return strdup(AI_PATCH_NO_FILE_RESULT);
 if (!firstnl) result = strdup(AI_PATCH_NO_BLOCKS_RESULT);
 else if (!ai_apply_search_replace(old, firstnl + 1, &newc, perr, sizeof perr)) result = ai_tool_error_alloc(perr);
 else result = ai_agent_commit_write(op, AI_TOOL_APPLY_PATCH, path, newc);
 free(old);
 free(newc);
 return result;
}

/**
 * ai_agent_tool_plan - Lay out the model's checklist and tell both sides where it is.
 * @f: the window the pane belongs to; @firstnl: the newline that ends the
 * TOOL line, NULL when the request has no body (an empty plan).
 * Return: the malloc'd result that tells the model how to report each step.
 */
static char *ai_agent_tool_plan(FENSTER *f, const char *firstnl)
{
 if (firstnl) ai_todo_load(f, firstnl + 1);
 else ai_todo_load(f, "");
 ai_pane(f, AI_PLAN_LAID_OUT_LINE, AI_ROLE_MACHINERY, 0);
 return strdup(AI_PLAN_LAID_OUT_RESULT);
}

/**
 * ai_agent_tool_step_done - Tick a step of the checklist.
 * @f: the window the pane belongs to; @arg: the step's words.
 * Return: the malloc'd result the model reads.
 */
static char *ai_agent_tool_step_done(FENSTER *f, const char *arg)
{
 ai_plan_mark_done(f, arg);
 return strdup(AI_PLAN_STEP_RESULT);
}

/**
 * ai_agent_run_tool - Run the tool the model named, under the dial.
 * @op: the running agent task; @tool/@arg: the request; @firstnl: the
 * newline that ends the TOOL line, NULL when the request has no body.
 *
 * One place lists every tool the built-in agent has, so a reader finds in a
 * glance what the model may do and which of it asks: the read-only tools and
 * the checklist run unasked, the command and the web tools are gated as
 * commands, the two writes as writes.
 * Return: the malloc'd result the model reads, NULL out of memory.
 */
static char *ai_agent_run_tool(ai_async_op *op, const char *tool, const char *arg,
                               const char *firstnl)
{
 FENSTER *f = op->f;
 if (ai_tool_is_shell_readonly(tool)) return ai_tool_run_readonly(tool, arg);
 if (!strcmp(tool, AI_TOOL_READ_FILE)) return ai_read_file_bounded(arg);
 if (!strcmp(tool, AI_TOOL_RUN_COMMAND)) return ai_agent_tool_run_command(f, arg);
 if (!strcmp(tool, AI_TOOL_WEB_FETCH)) return ai_agent_tool_web_fetch(f, arg);
 if (!strcmp(tool, AI_TOOL_WEB_SEARCH)) return ai_agent_tool_web_search(f, arg);
 if (!strcmp(tool, AI_TOOL_WRITE_FILE)) return ai_agent_tool_write_file(op, arg, firstnl);
 if (!strcmp(tool, AI_TOOL_APPLY_PATCH)) return ai_agent_apply_patch(op, arg, firstnl);
 if (!strcmp(tool, AI_TOOL_PLAN)) return ai_agent_tool_plan(f, firstnl);
 if (!strcmp(tool, AI_TOOL_STEP_DONE)) return ai_agent_tool_step_done(f, arg);
 return strdup(AI_TOOL_UNKNOWN_RESULT);
}

/**
 * ai_agent_feed_result - Hand a tool's result to the model as the next turn's input.
 * @op: the running agent task; @result: the malloc'd result, taken over.
 *
 * An empty result becomes an explicit note, so the model learns that
 * nothing matched instead of reading a broken tool and stalling.
 */
static void ai_agent_feed_result(ai_async_op *op, char *result)
{
 size_t n;
 char *tr;
 result = ai_tool_result_nonempty(result);
 n = strlen(result) + sizeof AI_TOOL_RESULT_HEAD;
 tr = malloc(n);
 if (tr) snprintf(tr, n, AI_TOOL_RESULT_HEAD "%s", result);
 if (tr) ai_ml_add(&op->ml, "user", tr);
 free(tr);
 free(result);
}

/**
 * ai_agent_process - One agent turn: read the reply, run its tool, feed the result back.
 * @op: the running agent task; @reply: the model's reply, writable.
 *
 * The reply is the agent's answer when it carries no tool line; otherwise
 * the reasoning before the tool line is shown, the tool is run under the
 * dial and its result becomes the next turn.  Runs from the conversation
 * driver with the spinner paused, so an approval question can wait for keys.
 * Return: 1 when the agent is done, 0 to keep going.
 */
static int ai_agent_process(ai_async_op *op, char *reply)
{
 FENSTER *f = op->f;
 enum ai_agent_marker kind;
 char *act, *firstnl, *tool, *arg, *result, action[AI_TOOL_LINE_MAX];
 size_t len;

 if (!ai_window_alive(op->cn, f)) return 1;
 act = ai_agent_find_marker(reply, &kind);
 if (kind != AI_MARK_TOOL) return ai_agent_end_with_answer(f, reply, kind);
 if (act > reply) ai_agent_show_preamble(f, reply, act);
 firstnl = strchr(act, '\n');
 if (firstnl) len = (size_t)(firstnl - act);
 else len = strlen(act);
 ai_copy_bounded(action, sizeof action, act, len);
 ai_split_tool(action, &tool, &arg);   /* shared parse: tolerates "read_file:" */
 ai_agent_show_tool(f, tool, arg);
 result = ai_agent_run_tool(op, tool, arg, firstnl);
 ai_agent_feed_result(op, result);
 return 0;                                      /* keep going */
}

/**
 * ai_agent_offer_follow_up - Leave the pane ready for the next instruction.
 * @op: the agent task that has just ended.
 *
 * A finished run arms the chat input on the pane (the transcript stays in
 * view) so the user types the follow-up right there instead of re-opening
 * Alt-G; the workspace session carries the context forward and Esc leaves.
 * A run that ended because the connection died offers none of this: its last
 * line stays "[AI] transport error", so the user reads a failure, never a
 * task that looks done and asks what to do next.
 */
static void ai_agent_offer_follow_up(ai_async_op *op)
{
 if (op->transport_failed) return;
 ai_pane_attn(op->f, "done - type a follow-up below (Enter sends, Esc leaves)");
 e_ai_chat_arm(op->f);
}

/* Agent wrap-up: persist the exchange and, for a run that took a checkpoint
 * (at launch, or before a write once the dial was raised), offer the changeset
 * review (navigable like compile errors). */
static void ai_agent_finish(ai_async_op *op)
{
 if (op->ml.n > 0 && !strcmp(op->ml.role[op->ml.n - 1], "assistant"))
  wpe_ai_session_append("assistant", op->ml.content[op->ml.n - 1]);
 wpe_ai_session_save(op->f);
 /* A run that took a checkpoint is reviewed whatever the dial reads now: the
    dial may have been lowered again after the writes it allowed. */
 if (op->checkpointed && wpe_ai_checkpoint_active())
  wpe_ai_changeset_review(op->f);
 ai_agent_offer_follow_up(op);
}

/* The agent's standing instructions: its tools, one action per turn, and how
   a run ends.  The tool names here are the ones ai_agent_run_tool answers to. */
static const char ai_agent_system_prompt[] =
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

/* Room for a preflight reason and for the pane's task line; how many turns
   of the workspace session a run carries forward. */
#define AI_AGENT_ERR_MAX       320
#define AI_AGENT_TASK_LINE_MAX 1100
#define AI_AGENT_PRIOR_MAX     12
/* The pane line when no model is set and the check gave no words. */
#define AI_AGENT_NO_MODEL_LINE "no model set"

/**
 * ai_agent_ready - Can a run start: is the backend reachable and a model set?
 * @f: the window the pane belongs to.
 *
 * The reason is shown in the pane before anything is sent, so the user
 * fixes the setting instead of watching a run that cannot begin.
 * Return: 1 when the run may start.
 */
static int ai_agent_ready(FENSTER *f)
{
 char err[AI_AGENT_ERR_MAX];
 err[0] = '\0';
 if (wpe_ai_preflight(e_ai_backend, err, sizeof err)) {
  ai_pane(f, err, AI_ROLE_ERROR, 1);
  return 0;
 }
 err[0] = '\0';
 if (!wpe_ai_ensure_model(err, sizeof err)) return 1;
 if (err[0]) ai_pane(f, err, AI_ROLE_ERROR, 1);
 else ai_pane(f, AI_AGENT_NO_MODEL_LINE, AI_ROLE_ERROR, 1);
 return 0;
}

/**
 * ai_agent_announce_task - Open the run in the pane: the header, then the task.
 * @f: the window the pane belongs to; @goal: the task as the user typed it.
 *
 * The header names the backend and model the run uses and the task line
 * repeats the goal, so the transcript reads on its own later; the trace
 * records the task and the dial the run started under.
 */
static void ai_agent_announce_task(FENSTER *f, const char *goal)
{
 char st[AI_HEADER_MAX], line[AI_AGENT_TASK_LINE_MAX];
 ai_header_text_shown(st, sizeof st);
 ai_pane(f, st, AI_ROLE_MACHINERY, 0);
 snprintf(line, sizeof line, "[agent] task: %s", goal);
 ai_pane(f, line, AI_ROLE_MACHINERY, 1);
 wpe_ai_trace("agent task=%s", goal);
 wpe_ai_trace("agent policy=%s", wpe_ai_policy_name(e_ai_policy));
}

/**
 * ai_agent_op_new - The task record for a run on window @f.
 *
 * Remembers the window's id (the desktop's entry for its slot), so the run
 * can find its window again after the user switches windows, and names the
 * loop's two callbacks and its turn limit.
 * Return: the calloc'd task, NULL out of memory.
 */
static ai_async_op *ai_agent_op_new(FENSTER *f)
{
 ai_async_op *op = calloc(1, sizeof *op);
 int wi;
 if (!op) return NULL;
 op->f = f;
 op->cn = f->ed;
 wi = ai_window_slot(f, f);
 if (wi) op->save_id = f->ed->edt[wi];
 else op->save_id = -1;
 op->kind = AI_KIND_AGENT;
 op->max_iter = AI_AGENT_MAX_ITERS;
 op->process = ai_agent_process;
 op->finish = ai_agent_finish;
 return op;
}

/**
 * ai_agent_add_prior_turns - Carry the workspace session's last turns into the run.
 * @m: the run's message list.
 *
 * A follow-up typed after a run ("now do the same for the tests") reads as
 * one conversation: the model sees what it was asked and answered before.
 */
static void ai_agent_add_prior_turns(struct ai_mlist *m)
{
 wpe_ai_msg prior[AI_AGENT_PRIOR_MAX];
 int np = wpe_ai_session_messages(prior, AI_AGENT_PRIOR_MAX), i;
 for (i = 0; i < np; i++) ai_ml_add(m, prior[i].role, prior[i].content);
}

/**
 * ai_agent_compose - Fill the run's message list and take its checkpoint.
 * @op: the task; @goal: the task's words; @extra: an optional system message
 * after the identity (the build-fix loop states its build command), NULL for none.
 *
 * The prompt's order: the tool list, who answers, the open files, then the
 * project instructions (which end by saying the tool list stands).  The pane
 * follows the same order: the task line, the project instructions, then the
 * checkpoint a raised dial calls for -- never unattended without one.
 */
static void ai_agent_compose(ai_async_op *op, const char *goal, const char *extra)
{
 ai_ml_add(&op->ml, "system", ai_agent_system_prompt);
 ai_ml_add_identity(&op->ml);
 ai_ml_add_open_files(&op->ml, op->f);
 ai_ml_add_project_memory(&op->ml, op->f);
 ai_agent_ensure_checkpoint(op);
 if (extra && extra[0]) ai_ml_add(&op->ml, "system", extra);
 ai_ml_add_diagnostics(&op->ml);
 ai_agent_add_prior_turns(&op->ml);
 ai_ml_add(&op->ml, "user", goal);
 wpe_ai_session_append("user", goal);
}

/**
 * ai_agent_launch - Start the built-in agent on an already-formed goal.
 * @f: the window the run belongs to; @goal: the task; @extra: an optional
 * system message after the identity, NULL for none.
 *
 * Shared by Alt-G g (prompted) and Build & fix, so both runs open, prompt
 * and checkpoint the same way.  The external CLI is held to text only: the
 * agent drives its own tool protocol and runs each tool under the dial, so
 * the CLI's own tools, which would bypass that review, are never enabled.
 * Return: 0 always (the run goes on in the background).
 */
static int ai_agent_launch(FENSTER *f, const char *goal, const char *extra)
{
 ai_async_op *op;
 if (!ai_agent_ready(f)) return 0;
 ai_agent_announce_task(f, goal);
 wpe_ai_session_load(f);
 e_ai_cli_mode = WPE_AI_CLI_TEXTONLY;
 op = ai_agent_op_new(f);
 if (!op) return 0;
 ai_agent_compose(op, goal, extra);
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
              "tool under xwpe's review.", AI_ROLE_MACHINERY, 0);
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
   ai_pane(f, line, AI_ROLE_MACHINERY, 1); }
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
 char action[AI_TOOL_LINE_MAX], *firstnl;
 int done;

 if (!ai_window_alive(op->cn, f)) return 1;
 done = ai_plan_parse(reply, pd->props, &pd->np);
 firstnl = strchr(reply, '\n');
 { size_t l = firstnl ? (size_t)(firstnl - reply) : strlen(reply);
   if (l >= sizeof action) l = sizeof action - 1; memcpy(action, reply, l); action[l] = '\0'; }
 if (!done && !strncmp(action, "TOOL ", 5)) {
  char *tool = action + 5, *arg = strchr(tool, ' '), *result = NULL, paneln[640];
  if (arg) { *arg = '\0'; arg++; } else arg = (char *)"";
  snprintf(paneln, sizeof paneln, AI_MULTIFILE_PREFIX " %s %s", tool, arg);
  ai_pane(f, paneln, AI_ROLE_MACHINERY, 0);
  wpe_ai_trace("plan tool=%s arg=%s", tool, arg);
  if (!strcmp(tool, "read_file"))      result = wpe_ai_read_scope_file(f, arg);
  else if (!strcmp(tool, AI_TOOL_LIST_DIR)) result = ai_tool_run_readonly(tool, arg);
  else if (!strcmp(tool, AI_TOOL_GREP))     result = ai_tool_run_readonly(tool, arg);
  else result = strdup("(only read-only tools are allowed in plan mode)");
  result = ai_tool_result_nonempty(result);   /* empty output -> explicit note */
  { size_t n2 = strlen(result) + 32; char *tr = malloc(n2);
    if (tr) { snprintf(tr, n2, "TOOL RESULT:\n%s", result); ai_ml_add(&op->ml, "user", tr); free(tr); } }
  free(result);
  return 0;
 }
 if (!done && pd->np == 0) {
  ai_pane(f, action, AI_ROLE_ASSISTANT, 0);   /* neither a tool nor a plan: final text */
  return 1;
 }
 return done ? 1 : 0;
}

/* The outside-the-workspace mark and word are defined with the agent's write
   box (ai_diff_confirm_write), which shows them for the same reason. */

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
 snprintf(line, sizeof line, AI_MULTIFILE_PREFIX " applied %s, %s", path, AI_PLAN_OUTSIDE_WORD);
 ai_pane(f, line, AI_ROLE_MACHINERY, 0);
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
  ai_pane(f, AI_MULTIFILE_PREFIX " the model proposed no file changes", AI_ROLE_MACHINERY, 0);
  wpe_ai_trace("plan proposals=0");
  return;
 }
 snprintf(line, sizeof line, AI_MULTIFILE_PREFIX " the AI proposes to change %d file%s (review in the dialog):",
          np, np == 1 ? "" : "s");
 ai_pane(f, line, AI_ROLE_MACHINERY, 1);
 wpe_ai_trace("plan proposals=%d", np);
 snprintf(title, sizeof title, AI_MULTIFILE_NAME " - %d file%s", np, np == 1 ? "" : "s");
 mode = ai_plan_choice(f, pd, title);
 if (mode == AI_PLAN_CANCEL) {
  ai_pane(f, AI_MULTIFILE_PREFIX " cancelled - nothing changed", AI_ROLE_MACHINERY, 0);
  wpe_ai_trace("plan cancelled");
  return;
 }
 for (i = 0; i < np; i++) {
  ECNT *cn = f->ed;
  FENSTER *w;
  const char *path = pd->props[i].path;
  int done;
  if (e_edit(cn, pd->props[i].path) != 0) {
   snprintf(line, sizeof line, AI_MULTIFILE_PREFIX " could not open %s", path);
   ai_pane(f, line, AI_ROLE_ERROR, 0);
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
 snprintf(line, sizeof line, AI_MULTIFILE_PREFIX " applied %d of %d file%s - each is open (Ctrl-U undoes per window)", applied, np, np == 1 ? "" : "s");
 ai_pane(f, line, AI_ROLE_MACHINERY, 0);
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
 if (wpe_ai_preflight(e_ai_backend, err, sizeof err)) { ai_pane(f, err, AI_ROLE_ERROR, 1); return; }
 err[0] = '\0';
 if (wpe_ai_ensure_model(err, sizeof err)) { ai_pane(f, err[0] ? err : "no model set", AI_ROLE_ERROR, 1); return; }

 e_ai_cli_mode = WPE_AI_CLI_TEXTONLY;               /* xwpe owns every edit */
 wpe_ai_session_load(f);
 pd = calloc(1, sizeof *pd);
 if (!pd) return;
 pd->nsc = wpe_ai_scope_files(f, e_project_is_open(), 1, &pd->scope);
 snprintf(line, sizeof line, AI_MULTIFILE_PREFIX " task: %s  (scope: %d files)", task, pd->nsc);
 ai_pane(f, line, AI_ROLE_MACHINERY, 1);
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
 ai_ml_add_project_memory(&op->ml, f);
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
 ai_conv_start(op, AI_MULTIFILE_PREFIX " working");
}

/**
 * e_ai_plan - Change several files at once (Alt-G f, "Multi-file edit").
 * @f: the window it was asked from.  Return: 0, the editor-action convention.
 *
 * Asks for the task first and only then looks at what is running, so the task
 * can be typed while another one finishes; it is then queued and starts by
 * itself, like every other assistant action.  A window that is not a file is
 * refused before the prompt opens (ai_refuse_non_file_window).
 */
static int e_ai_plan(FENSTER *f)
{
 static char task[AI_PROMPT_MAX];

 if (ai_refuse_non_file_window(f))
  return 0;
 task[0] = '\0';
 if (!e_ai_prompt1(task, AI_MULTIFILE_NAME ": task", f) || !task[0])
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
 * The session stays live across turns; Alt-G g feeds the next turn, Esc during
 * a turn asks the engine to stop that turn, and Alt-G g with an empty prompt
 * (or Esc there) ends the session. */

static wpe_host *g_host;                /* the live session, or NULL */
static ECNT     *g_host_cn;             /* container, for liveness checks */
static FENSTER  *g_host_win;            /* pane's anchor window */
static int       g_host_fd = -1;        /* child stdout on the fd-loop */
static int       g_host_turn;           /* a turn is streaming */
static int       g_host_in_prompt;      /* a permission y/n modal is up */
static int       g_host_interrupting;   /* Esc asked the engine to stop its turn */
static int       g_host_deadline_fd = -1; /* one-shot timer: the engine's time to comply */
static time_t    g_host_deadline_at;    /* the same bound, for a build without timerfd */

/* The CLI's tool names, as it sends them in can_use_tool requests. */
#define HOST_TOOL_BASH          "Bash"
#define HOST_TOOL_WEBFETCH      "WebFetch"
#define HOST_TOOL_WEBSEARCH     "WebSearch"
#define HOST_TOOL_WRITE         "Write"
#define HOST_TOOL_EDIT          "Edit"
#define HOST_TOOL_MULTIEDIT     "MultiEdit"
#define HOST_TOOL_NOTEBOOKEDIT  "NotebookEdit"

/* How the permission dial reads a CLI tool: the classes the built-in loop has. */
enum host_tool_class {
 HOST_TOOL_CLASS_COMMAND,               /* runs a command or reaches the web */
 HOST_TOOL_CLASS_WRITE,                 /* edits a file */
 HOST_TOOL_CLASS_OTHER                  /* anything else the CLI gates */
};

/* A short name for the hosted engine, for the pane messages: the configured
   command, else "Claude Code" (the built-in default). */
static const char *host_engine_name(void)
{
 const char *cmd = wpe_host_command();   /* what the spawn will actually run */
 return cmd ? cmd : "Claude Code";
}

/**
 * host_note_web_unenforceable - Say when the web switch cannot reach this CLI.
 * @f: the window the pane belongs to.
 *
 * The switch reaches a hosted CLI two ways: the deny list on the command line,
 * which the editor can only build for the `claude` it launches itself, and the
 * per-tool question, which only arrives if that CLI asks.  A CLI named in
 * Options gets neither flag, and the `auto` dial is where a CLI is least
 * likely to ask, so there the switch may not hold at all.  The user is told
 * once, when the session opens, rather than finding out from a search they
 * did not expect.
 */
static void host_note_web_unenforceable(FENSTER *f)
{
 if (!wpe_host_command()) return;                          /* our own claude: the flag holds */
 if (!ai_web_is_off()) return;
 if (e_ai_policy != WPE_AI_POLICY_AUTO) return;            /* it is asked per tool instead */
 ai_pane(f, "[agent] web access is off, but this engine is a command of your own at the"
            " auto dial: it is not asked per tool, so the switch cannot hold it back.",
         AI_ROLE_MACHINERY, 1);
}

/**
 * host_deadline_disarm - Drop the timer that bounds an interrupted turn.
 *
 * Called when the engine ended the turn in time, or when the session ends for
 * any reason, so a stale timer never fires into a session that no longer exists.
 */
static void host_deadline_disarm(void)
{
#ifdef __linux__
 if (g_host_deadline_fd >= 0) {
  wpe_fd_del(g_host_deadline_fd);
  close(g_host_deadline_fd);
  g_host_deadline_fd = -1;
 }
#endif
 g_host_deadline_at = 0;
}

/**
 * host_bring_pane_to_front - Put the pane on top so its last line is read.
 * @f: the window the session was anchored to (gives the desktop).
 *
 * When the session ends after the changeset review, the review's Messages
 * window sits on top of the pane, docked in the same place, and the "session
 * ended" line lands under it where the user cannot see it; uncovering it later
 * showed a stale image of the pane.  Switching to the pane is how the chat
 * surfaces it: a full repaint from the buffer, with the last line in view.
 */
static void host_bring_pane_to_front(FENSTER *f)
{
 ECNT *cn = f->ed;
 FENSTER *wf = ai_pane_existing(cn);
 int wi;
 if (!wf) return;
 if (cn->f[cn->mxedt] == wf) return;        /* already on top */
 for (wi = 1; wi <= cn->mxedt; wi++)
  if (cn->f[wi] == wf) { e_switch_window(cn->edt[wi], wf); return; }
}

static void e_ai_host_end(const char *why)
{
 host_deadline_disarm();
 g_host_interrupting = 0;
 if (g_host_fd >= 0) { wpe_fd_del(g_host_fd); g_host_fd = -1; }
 if (g_host) { wpe_host_free(g_host); g_host = NULL; }
 wpe_ai_trace("host end notice=%s", why ? why : "(none)");
 if (g_host_win && why) {
  ai_pane(g_host_win, why, AI_ROLE_MACHINERY, 1);
  host_bring_pane_to_front(g_host_win);
 }
 g_host_win = NULL; g_host_cn = NULL; g_host_turn = 0;
}

/**
 * host_end_session_notice - End the session and say so in the pane.
 *
 * The one line every way of ending a session shows, "[agent] <engine> session
 * ended", so the user reads the same words whether the prompt was left empty,
 * the CLI exited, or an interrupted turn ran past its bound.
 */
static void host_end_session_notice(void)
{
 char l[120];
 snprintf(l, sizeof l, "[agent] %.80s session ended", host_engine_name());
 e_ai_host_end(l);
}

/**
 * host_take_checkpoint - Checkpoint the workspace for a hosted session, and say so.
 * @f: the window the session is anchored to (its pane shows the line).
 *
 * The hosted engine edits files on disk with its own tools, so the checkpoint
 * is taken at session start, whatever the dial, and the whole session is
 * reviewable and revertible when it ends (Alt-G g, then Esc).  It used to be
 * taken silently; the pane now shows the same "[agent] checkpoint taken" line
 * the built-in loop shows, so the user knows the safety net is there before
 * the first turn goes out.
 */
static void host_take_checkpoint(FENSTER *f)
{
 char **scope;
 int nsc = wpe_ai_scope_files(f, e_project_is_open(), 1, &scope);
 wpe_ai_checkpoint_create(f, scope, nsc);
 wpe_ai_free_list(scope, nsc);
 ai_pane(f, AI_AGENT_CHECKPOINT_LINE, AI_ROLE_MACHINERY, 0);
}

/* Non-host code (e.g. Options > Disable) asks whether a host is up. */
int  wpe_ai_host_active(void) { return g_host != NULL; }
void wpe_ai_host_shutdown(void) { if (g_host) e_ai_host_end(NULL); }

static void host_ev_text(const char *text, void *ud)
{ ai_pane_multiline((FENSTER *)ud, text, AI_ROLE_ASSISTANT, 0); }

static void host_ev_tool(const char *tool, const char *arg, void *ud)
{
 char l[720];
 snprintf(l, sizeof l, "[agent] %s %.640s", tool, arg ? arg : "");
 ai_pane((FENSTER *)ud, l, AI_ROLE_MACHINERY, 0);
 wpe_ai_trace("host tool=%s arg=%s", tool, arg ? arg : "");
}

static void host_ev_file(const char *path, void *ud)
{
 FENSTER *f = ud;
 char l[1200];
 wpe_ai_reload_open_window(f, path);         /* refresh the open buffer, one undo */
 snprintf(l, sizeof l, "[agent] edited %.1100s", path ? path : "");
 ai_pane(f, l, AI_ROLE_MACHINERY, 0);
 wpe_ai_trace("host edit=%s", path ? path : "");
}

static void host_ev_notice(const char *text, void *ud)
{ ai_pane((FENSTER *)ud, text ? text : "", AI_ROLE_MACHINERY, 0); }

/**
 * host_turn_end_notice - Tell the user how the turn ended and what comes next.
 * @f: the pane's window; @summary: the CLI's result text; @is_error: its flag.
 *
 * The assistant text already streamed, so the result field is not repeated on
 * success.  An error is named; a turn the user interrupted with Esc says so,
 * so the shorter answer is not mistaken for the engine's own ending; a normal
 * end says how to continue.  All three name Alt-G g as the way on.
 */
static void host_turn_end_notice(FENSTER *f, const char *summary, int is_error)
{
 char l[720];
 if (is_error)
  snprintf(l, sizeof l, "[agent] error: %.640s", (summary && summary[0]) ? summary : "run failed");
 else if (g_host_interrupting)
  snprintf(l, sizeof l, "[agent] turn interrupted - Alt-G g continues, Esc there ends");
 else
  snprintf(l, sizeof l, "[agent] turn complete - Alt-G g continues, Esc there ends");
 ai_pane(f, l, AI_ROLE_MACHINERY, 0);
}

/**
 * host_turn_finish - Leave the turn: no turn streams, no interrupt is pending.
 *
 * Drops the bound armed by an interrupt, so an engine that complied in time is
 * never ended by a timer meant for one that did not.
 */
static void host_turn_finish(void)
{
 host_deadline_disarm();
 g_host_interrupting = 0;
 g_host_turn = 0;
}

static void host_ev_result(const char *summary, int is_error, void *ud)
{
 FENSTER *f = ud;
 host_turn_end_notice(f, summary, is_error);
 wpe_ai_trace("host result is_error=%d interrupted=%d", is_error, g_host_interrupting);
 host_turn_finish();
}

/**
 * ai_host_tool_class - Class a CLI tool the way the built-in loop classes its own.
 * @name: the tool name from the can_use_tool request.
 *
 * The dial reads "edits" as "file edits pass, anything that runs or reaches
 * out asks".  The CLI's Bash, WebFetch and WebSearch are the commands; its
 * Write, Edit, MultiEdit and NotebookEdit are the writes; a tool the editor
 * does not know asks rather than passing as if it were an edit, which is how a
 * web fetch used to be auto-approved under edits.
 * Return: HOST_TOOL_CLASS_COMMAND, HOST_TOOL_CLASS_WRITE or HOST_TOOL_CLASS_OTHER.
 */
static enum host_tool_class ai_host_tool_class(const char *name)
{
 if (!name) return HOST_TOOL_CLASS_OTHER;
 if (!strcmp(name, HOST_TOOL_BASH))      return HOST_TOOL_CLASS_COMMAND;
 if (!strcmp(name, HOST_TOOL_WEBFETCH))  return HOST_TOOL_CLASS_COMMAND;
 if (!strcmp(name, HOST_TOOL_WEBSEARCH)) return HOST_TOOL_CLASS_COMMAND;
 if (!strcmp(name, HOST_TOOL_WRITE))     return HOST_TOOL_CLASS_WRITE;
 if (!strcmp(name, HOST_TOOL_EDIT))      return HOST_TOOL_CLASS_WRITE;
 if (!strcmp(name, HOST_TOOL_MULTIEDIT)) return HOST_TOOL_CLASS_WRITE;
 if (!strcmp(name, HOST_TOOL_NOTEBOOKEDIT)) return HOST_TOOL_CLASS_WRITE;
 return HOST_TOOL_CLASS_OTHER;
}

/**
 * ai_host_tool_is_web - Is this one of the host engine's two network tools?
 * @name: the tool the CLI named.
 *
 * The web switch has to recognise them by name here, because the host engine
 * runs its own tools and only reports them: there is no dispatcher of ours to
 * gate.
 * Return: 1 for WebSearch and WebFetch.
 */
static int ai_host_tool_is_web(const char *name)
{
 if (!name) return 0;
 return !strcmp(name, HOST_TOOL_WEBFETCH) || !strcmp(name, HOST_TOOL_WEBSEARCH);
}

/**
 * ai_host_tool_asks_under_edits - Does this class need the box under "edits"?
 * @cls: the tool's class.
 *
 * Only a write passes on the dial's word; a command and anything unclassified
 * ask, so the user sees every request that is not a file edit.
 * Return: 1 when the dial position edits must still ask, 0 when it may pass.
 */
static int ai_host_tool_asks_under_edits(enum host_tool_class cls)
{
 if (cls == HOST_TOOL_CLASS_WRITE) return 0;
 return 1;
}

/**
 * host_ev_permission - The CLI asks to use a tool; the dial and the user decide.
 * @tool: the CLI's tool name; @arg: its short argument; @ud: the pane's window.
 *
 * Reuses the built-in loop's approval (auto passes everything with the
 * auto-approved line, edits passes writes and asks for the rest, ask asks
 * every time) and its denial record, so a refused WebFetch shows
 * "[agent] denied: WebFetch <url>" exactly as a refused web_fetch does.
 * g_host_in_prompt stops the fd pump from re-entering while the box reads a key.
 * Return: 1 allow, 0 deny (the CLI then reads "Denied by the user").
 */
static int host_ev_permission(const char *tool, const char *arg, void *ud)
{
 FENSTER *f = ud;
 char what[720];
 const char *name = tool ? tool : "tool";
 const char *shown = arg ? arg : "";
 enum host_tool_class cls = ai_host_tool_class(tool);
 int ok;
 /* The web switch decides before the dial does.  The dial asks at two of its
    three positions, so at `auto` a host task would have reached the network
    with the switch off and nothing on screen; refusing here makes the switch
    mean the same thing for the host engine as for the editor's own tools. */
 if (ai_host_tool_is_web(tool) && ai_web_is_off()) {
  free(ai_agent_refuse_web(f, name, shown));
  return 0;
 }
 snprintf(what, sizeof what, "%s %.640s", name, shown);
 g_host_in_prompt = 1;
 ok = ai_agent_approve(f, what, ai_host_tool_asks_under_edits(cls));
 g_host_in_prompt = 0;
 if (!ok) free(ai_agent_deny(f, name, shown));   /* the pane and trace lines; the model text is the CLI's own */
 wpe_ai_trace("host permission tool=%s allow=%d", name, ok);
 return ok;
}

static const wpe_host_events g_host_ev = {
 host_ev_text, host_ev_tool, host_ev_file, host_ev_notice, host_ev_result,
 host_ev_permission
};

/**
 * host_end_interrupt_overdue - End a session whose engine ignored the interrupt.
 *
 * The user pressed Esc, AIToolTimeout seconds passed and the turn is still
 * open: waiting longer would leave the editor tied to an engine that does not
 * listen.  The session ends as on a child exit, with the same pane line.  The
 * changeset review opens first when something was checkpointed and the editor
 * is in its own key loop; from inside a dialog's key read a modal review would
 * stack on the dialog, so there the session simply ends.
 */
static void host_end_interrupt_overdue(void)
{
 if (!g_host || !g_host_interrupting) return;
 wpe_ai_trace("host interrupt deadline: session ended after %d s", e_ai_tool_timeout);
 if (wpe_ai_checkpoint_active() && ai_editor_idle(g_host_cn))
  wpe_ai_changeset_review(g_host_win);
 host_end_session_notice();
}

/**
 * host_deadline_fire - The one-shot timer of an interrupted turn ran out.
 * @fd: the timer; @data: unused.
 *
 * Runs on the fd-loop like the heartbeat, so the editor never blocks waiting
 * for the engine: it keeps taking keys and the bound still holds.
 */
static void host_deadline_fire(int fd, void *data)
{
 uint64_t exp;
 (void)data;
 if (read(fd, &exp, sizeof exp) != (ssize_t)sizeof exp) return;
 host_end_interrupt_overdue();
}

/**
 * host_deadline_check - The bound for a build without timerfd.
 *
 * Checked whenever the engine's stdout wakes the fd callback; an engine that
 * keeps streaming past the bound is ended there.  On Linux the one-shot timer
 * fires first, also during the engine's silence.
 */
static void host_deadline_check(void)
{
 if (!g_host_interrupting || !g_host_deadline_at) return;
 if (time(NULL) < g_host_deadline_at) return;
 host_end_interrupt_overdue();
}

/**
 * host_deadline_arm - Give the engine AIToolTimeout seconds to end the turn.
 *
 * Mirrors the heartbeat's timerfd idiom (a one-shot here) so the wait is an
 * fd-loop event, not a loop; the wall-clock copy serves host_deadline_check
 * where timerfd does not exist.
 */
static void host_deadline_arm(void)
{
 host_deadline_disarm();
 g_host_deadline_at = time(NULL) + e_ai_tool_timeout;
#ifdef __linux__
 g_host_deadline_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
 if (g_host_deadline_fd >= 0) {
  struct itimerspec its;
  memset(&its, 0, sizeof its);
  its.it_value.tv_sec = e_ai_tool_timeout;
  timerfd_settime(g_host_deadline_fd, 0, &its, NULL);
  wpe_fd_add(g_host_deadline_fd, POLLIN, host_deadline_fire, NULL);
 }
#endif
}

/* fd-loop callback: drain and render the session's stream-json events. */
static void host_fd_cb(int fd, void *data)
{
 int turn_done = 0, hup = 0;
 (void)fd; (void)data;
 if (!g_host || g_host_in_prompt) return;   /* not while a permission modal is up */
 if (!ai_window_alive(g_host_cn, g_host_win)) { e_ai_host_end(NULL); return; }
 wpe_host_pump(g_host, &g_host_ev, g_host_win, &turn_done, &hup);
 if (hup) { host_end_session_notice(); return; }
 host_deadline_check();
}

/**
 * wpe_ai_host_turn_active - Is the hosted engine streaming a turn right now?
 *
 * The editor's key loop asks this before Esc, so the key that cancels a
 * built-in task also interrupts a hosted one instead of closing the pane.
 * Return: 1 while a turn is open, 0 otherwise.
 */
int wpe_ai_host_turn_active(void)
{
 if (!g_host) return 0;
 return g_host_turn;
}

/**
 * wpe_ai_host_interrupt - Esc during a hosted turn: ask the engine to stop it.
 *
 * The pane says "[agent] interrupting ..." at once, the request goes out on the
 * stream-json channel, and the engine gets AIToolTimeout seconds to end the
 * turn; when it does, the pane shows "turn interrupted" and the session stays
 * alive for the next Alt-G g.  A second Esc while the first is pending changes
 * nothing.  A write failure (the CLI's stdin gone) is left to the same bound,
 * so the user is never stuck either way.
 */
void wpe_ai_host_interrupt(void)
{
 int rc;
 if (!wpe_ai_host_turn_active()) return;
 if (g_host_interrupting) { wpe_ai_trace("host interrupt already pending"); return; }
 g_host_interrupting = 1;
 rc = wpe_host_interrupt(g_host);
 ai_pane(g_host_win, "[agent] interrupting ...", AI_ROLE_MACHINERY, 0);
 wpe_ai_trace("host interrupt sent rc=%d bound=%d s", rc, e_ai_tool_timeout);
 host_deadline_arm();
}

int e_ai_host(FENSTER *f)
{
 static char task[AI_PROMPT_MAX];
 char err[320];

 if (!wpe_ai_enabled()) {
  ai_pane(f, AI_OFF_LINE, AI_ROLE_MACHINERY, 1);
  return 0;
 }
 if (wpe_ai_busy()) {
  ai_pane(f, "[AI] a task is already running - press Esc to cancel it, or wait",
          AI_ROLE_MACHINERY, 1);
  return 0;
 }
 if (g_host && g_host_turn) {
  ai_pane(f, "[agent] still working on the previous turn", AI_ROLE_MACHINERY, 1);
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
     if (wpe_ai_checkpoint_active()) wpe_ai_changeset_review(f);
     host_end_session_notice();
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
   ai_pane(f, l, AI_ROLE_ERROR, 1);
   return 0;
  }
  g_host_win = f; g_host_cn = f->ed;
  g_host_fd = wpe_host_fd(g_host);
  wpe_fd_add(g_host_fd, POLLIN, host_fd_cb, NULL);
  host_take_checkpoint(f);
  { char l[160];
    snprintf(l, sizeof l, "[agent] engine: %.80s - session started (Esc at the prompt ends it)",
             host_engine_name());
    ai_pane(f, l, AI_ROLE_MACHINERY, 1); }
  host_note_web_unenforceable(f);
  wpe_ai_trace("host start policy=%s adapter=%d", wpe_ai_policy_name(e_ai_policy), e_ai_host_adapter);
 }
 { char l[1100]; snprintf(l, sizeof l, "[agent] you: %.1000s", task); ai_pane(f, l, AI_ROLE_USER, 0); }
 if (wpe_host_send(g_host, task) != 0) {
  e_ai_host_end("[agent] send failed - session ended");
  return 0;
 }
 g_host_turn = 1;
 ai_pane(f, "[agent] working... (Esc interrupts the turn)", AI_ROLE_MACHINERY, 0);
 wpe_ai_trace("host send");
 return 0;
}
#else  /* no agent host in this build: Esc has no hosted turn to interrupt */
int  wpe_ai_host_turn_active(void) { return 0; }
void wpe_ai_host_interrupt(void) { }
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
  ai_pane(f, AI_OFF_LINE, AI_ROLE_MACHINERY, 1);
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
 * The spinner line has already gone with the task (ai_spin_end), so the note
 * is appended where it stood rather than written over whatever line is last
 * now.  Writes into the pane that is already open and nowhere else: opening
 * the pane here would push a window onto the stack while e_close_window is
 * in the middle of popping one, and painting into the pane when the pane is
 * the window being closed would paint into freed memory.
 */
static void ai_op_note_window_gone(ECNT *cn, FENSTER *closing, const char *kind)
{
 FENSTER *wf = ai_pane_existing(cn);
 char line[120];
 if (!wf || wf == closing)
  return;
 snprintf(line, sizeof line, "[AI %s] dropped - its window was closed", kind);
 ai_tr_line(wf, line, AI_ROLE_MACHINERY);
 ai_pane_paint(wf);
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
/**
 * ai_op_targets_window - Does the running task go away with this window?
 * @op: the running task; @closing: the window e_close_window is about to free.
 *
 * An Edit, Complete, Agent or Plan belongs to the file window it was started
 * from.  A chat turn belongs to the pane it streams into: Ctrl-W on the pane
 * ends the stream, so no token can re-create an empty pane afterwards.
 * Return: 1 when the task must be dropped with the window.
 */
static int ai_op_targets_window(const ai_async_op *op, const FENSTER *closing)
{
 if (op->f == closing)
  return 1;
 if (!op->kind || strcmp(op->kind, AI_KIND_CHAT))
  return 0;
 return closing->datnam && !strcmp(closing->datnam, AI_PANE_NAME);
}

static void ai_op_drop_for_window(FENSTER *closing)
{
 ai_async_op *op = g_ai_op;
 ECNT *cn;
 const char *kind;
 if (!op || !ai_op_targets_window(op, closing))
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
 * stays in the pane as far as it got.  A turn that runs as a task is dropped
 * with the task (ai_op_drop_for_window); this catches a session that got no
 * task, so no stream ever outlives its window.
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
 ai_chat_forget_header();            /* the next pane shows the header and hints again */
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
 if (ai_is_pane(closing))
  ai_roles_forget();
#ifdef WPE_AI_AGENT_HOST
 ai_host_drop_for_window(closing);
#endif
 if (g_ai_bg_win == closing)
  g_ai_bg_win = NULL;
}

#endif /* WPE_AI */

typedef int wpe_ai_ui_translation_unit;
