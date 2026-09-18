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

/* e_d_p_named is defined in we_prog.c; declared here in case progr.h predates it. */
int e_d_p_named(char *winname, char *str, FENSTER *f, int sw);

#define AI_PANE_NAME  "AI"
#define AI_CTX_MAX    16000     /* cap the current-file context we send        */

/* ----- one active chat at a time (MVP) ----------------------------------- */
typedef struct {
 wpe_ai_stream *st;
 FENSTER       *ref;           /* anchor window (gives us cn = ref->ed)        */
 char          *pending;       /* incomplete trailing line being streamed      */
 size_t         plen, pcap;
 int            fd;
 int            active;
} ai_chat_session;

static ai_chat_session *g_ai_chat = NULL;

static void ai_pane(FENSTER *f, const char *line, int surface)
{
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
 if (s->fd >= 0) wpe_fd_del(s->fd);
 wpe_ai_stream_free(s->st);
 free(s->pending);
 if (g_ai_chat == s) g_ai_chat = NULL;
 free(s);
}

/* Accumulate a streamed delta; emit each COMPLETE line to the pane, keep the
 * trailing partial line buffered (so tokens paint line-by-line, never split). */
static void ai_delta_cb(const char *delta, void *ud)
{
 ai_chat_session *s = ud;
 size_t dl = strlen(delta), start, i;

 if (s->plen + dl + 1 > s->pcap) {
  size_t nc = s->pcap ? s->pcap : 256;
  char *nb;
  while (s->plen + dl + 1 > nc) nc *= 2;
  nb = realloc(s->pending, nc);
  if (!nb) return;
  s->pending = nb;
  s->pcap = nc;
 }
 memcpy(s->pending + s->plen, delta, dl);
 s->plen += dl;
 s->pending[s->plen] = '\0';

 start = 0;
 for (i = 0; i < s->plen; i++) {
  if (s->pending[i] == '\n') {
   s->pending[i] = '\0';
   ai_pane(s->ref, s->pending + start, 0);
   start = i + 1;
  }
 }
 if (start) {
  memmove(s->pending, s->pending + start, s->plen - start);
  s->plen -= start;
  s->pending[s->plen] = '\0';
 }
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
  if (s->plen) { s->pending[s->plen] = '\0'; ai_pane(s->ref, s->pending, 0); s->plen = 0; }
  wpe_ai_trace("chat done");
  s->active = 0;
  ai_chat_finish(s);
 }
}

/* Build the system prompt (assistant role + current-file context). */
static char *ai_build_system(FENSTER *f)
{
 char *ctx = ai_current_file_text(f);
 const char *head =
   "You are an AI assistant embedded in the xwpe console editor. "
   "Answer concisely in plain text. When the user asks about code, use the "
   "current file shown below.\n\n--- current file ---\n";
 size_t n = strlen(head) + (ctx ? strlen(ctx) : 0) + 8;
 char *sys = malloc(n);
 if (!sys) { free(ctx); return NULL; }
 snprintf(sys, n, "%s%s", head, ctx ? ctx : "");
 free(ctx);
 return sys;
}

/* Alt-B: prompt for a question and start an asynchronous streaming reply. */
static int e_ai_chat(FENSTER *f)
{
 static char prompt[2048];
 char err[320], line[2200];
 wpe_ai_msg msgs[2];
 wpe_ai_req req;
 char *sys;
 ai_chat_session *s;

 prompt[0] = '\0';
 if (!e_add_arguments(prompt, "Ask AI", f, 0, AltB, NULL) || !prompt[0])
  return 0;
 wpe_ai_trace("chat prompt=%s", prompt);

 if (g_ai_chat) { g_ai_chat->active = 0; ai_chat_finish(g_ai_chat); }

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

 snprintf(line, sizeof line, "You: %s", prompt);
 ai_pane(f, line, 1);
 ai_pane(f, "AI:", 0);

 sys = ai_build_system(f);
 msgs[0].role = "system"; msgs[0].content = sys ? sys : "";
 msgs[1].role = "user";   msgs[1].content = prompt;
 req.model = NULL;
 req.msgs = msgs;
 req.nmsgs = 2;

 s = calloc(1, sizeof *s);
 if (!s) { free(sys); return 0; }
 err[0] = '\0';
 s->st = wpe_ai_stream_start(&req, err, sizeof err);
 free(sys);
 if (!s->st) {
  snprintf(line, sizeof line, "[AI error] %s", err[0] ? err : "could not start");
  ai_pane(f, line, 0);
  free(s);
  return 0;
 }
 s->ref = f;
 s->fd = wpe_ai_stream_fd(s->st);
 s->active = 1;
 g_ai_chat = s;
 wpe_fd_add(s->fd, POLLIN, ai_fd_cb, s);
 wpe_ai_trace("chat stream fd=%d", s->fd);
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

static int e_ai_edit(FENSTER *f)
{
 static char instr[1024];
 char err[320], line[360];
 char *cur, *user, *reply, *clean;
 const char *sys =
   "You are a precise code editor. Apply the user's instruction to the file "
   "below and return ONLY the complete modified file content - no markdown "
   "fences, no commentary, no explanation.";
 wpe_ai_msg msgs[2];
 wpe_ai_req req;
 ECNT *cn = f->ed;
 int save_id = -1, wi;

 instr[0] = '\0';
 if (!e_add_arguments(instr, "AI edit instruction", f, 0, AltB, NULL) || !instr[0])
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

 ai_pane(f, "[AI edit] generating...", 1);
 wpe_ai_trace("edit instr=%s", instr);
 err[0] = '\0';
 reply = wpe_ai_complete(&req, 120000, err, sizeof err);
 free(user);
 if (!reply) {
  snprintf(line, sizeof line, "[AI edit] %s", err[0] ? err : "no response");
  ai_pane(f, line, 0);
  free(cur);
  return 0;
 }
 clean = ai_strip_fences(reply);
 free(reply);
 free(cur);
 if (!clean) return 0;

 {
  wpe_ai_seg *segs;
  int nseg, has_change = 0, i;
  char *now = ai_current_file_text(f);
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
    if (save_id >= 0) e_switch_window(save_id, f);  /* focus back to the file */
   } else {
    ai_pane(f, "[AI edit] discarded", 0);
    wpe_ai_trace("edit discarded");
   }
  }
  wpe_ai_segs_free(segs, nseg);
 }
 free(clean);
 return 0;
}

/* ======================= model picker =================================== */

static int e_ai_pick_model(FENSTER *f)
{
 char *names[32];
 char err[256], line[220];
 static char sel[128];
 int n, i;

 err[0] = '\0';
 n = wpe_ai_list_models(e_ai_backend, names, 32, err, sizeof err);
 if (n <= 0) { ai_pane(f, err[0] ? err : "no models found", 1); return 0; }
 ai_pane(f, "--- available models (type the exact name) ---", 1);
 for (i = 0; i < n; i++) ai_pane(f, names[i], 0);
 sel[0] = '\0';
 if (e_add_arguments(sel, "Model name", f, 0, AltB, NULL) && sel[0]) {
  free(e_ai_model);
  e_ai_model = strdup(sel);
  snprintf(line, sizeof line, "[AI] model = %s (Save Options to persist)", sel);
  ai_pane(f, line, 0);
  wpe_ai_trace("model set %s", sel);
 }
 for (i = 0; i < n; i++) free(names[i]);
 return 0;
}

int e_ai_agent(FENSTER *f);   /* defined in the Agent section below */

/* ======================= Alt-B prefix dispatch ========================== */
int e_ai_ui_key(FENSTER *f)
{
 int c;
 if (!wpe_ai_enabled()) {
  ai_pane(f, "AI assistant is off - enable it in Options > Editor "
             "(the \"Ai assistant\" box), then press Alt-B again.", 1);
  return 0;
 }
 c = e_toupper(e_getch());
 wpe_ai_trace("ui_key c=%d", c);
 switch (c) {
  case 'A': return e_ai_chat(f);        /* Ask (chat)                        */
  case 'E': return e_ai_edit(f);        /* Edit the current file             */
  case 'G': return e_ai_agent(f);       /* aGent (tool harness)              */
  case 'M': return e_ai_pick_model(f);  /* pick Model                        */
  case WPE_ESC: return 0;
  default:
   ai_pane(f, "AI (Alt-B):  a = Ask   e = Edit   g = aGent   m = Model", 1);
   return 0;
 }
}

/* ======================= Agent mode ==================================== */

#define AI_AGENT_MAX_ITERS 8
#define AI_TOOL_OUT_MAX    6000

/* Run a shell command; capture bounded stdout+stderr (malloc'd). */
static char *ai_run_capture(const char *cmd)
{
 FILE *fp;
 char *out, full[2100];
 size_t cap = 4096, len = 0;
 int ch;
 snprintf(full, sizeof full, "%s 2>&1", cmd);
 fp = popen(full, "r");
 if (!fp) return strdup("(could not run command)");
 out = malloc(cap);
 if (!out) { pclose(fp); return NULL; }
 while ((ch = fgetc(fp)) != EOF && len < AI_TOOL_OUT_MAX) {
  if (len + 2 > cap) { char *nb; cap *= 2; nb = realloc(out, cap); if (!nb) break; out = nb; }
  out[len++] = (char)ch;
 }
 out[len] = '\0';
 pclose(fp);
 return out;
}

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

/* Approve a mutating tool call (write_file / run_command).  1 = approved. */
static int ai_agent_approve(FENSTER *f, const char *what)
{
 char line[640];
 snprintf(line, sizeof line, "[agent] APPROVE?  %s", what);
 ai_pane(f, line, 1);
 ai_pane(f, "   y = allow    n / Esc = deny", 0);
 for (;;) {
  int c = e_getch();
  if (c == WPE_ESC || e_toupper(c) == 'N') return 0;
  if (e_toupper(c) == 'Y' || c == 13 || c == '\r' || c == '\n') return 1;
 }
}

/* growable conversation */
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
}

int e_ai_agent(FENSTER *f)
{
 static char goal[1024];
 char err[320];
 struct ai_mlist ml;
 int iter;
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

 goal[0] = '\0';
 if (!e_add_arguments(goal, "AI agent task", f, 0, AltB, NULL) || !goal[0])
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

 memset(&ml, 0, sizeof ml);
 ai_ml_add(&ml, "system", sys);
 ai_ml_add(&ml, "user", goal);

 for (iter = 0; iter < AI_AGENT_MAX_ITERS; iter++) {
  wpe_ai_msg *msgs;
  wpe_ai_req req;
  char *reply, *firstnl, action[1100];
  int i;

  msgs = malloc(ml.n * sizeof *msgs);
  if (!msgs) break;
  for (i = 0; i < ml.n; i++) { msgs[i].role = ml.role[i]; msgs[i].content = ml.content[i]; }
  req.model = NULL; req.msgs = msgs; req.nmsgs = ml.n;
  err[0] = '\0';
  reply = wpe_ai_complete(&req, 120000, err, sizeof err);
  free(msgs);
  if (!reply) { ai_pane(f, err[0] ? err : "[agent] no response", 0); break; }
  ai_ml_add(&ml, "assistant", reply);

  firstnl = strchr(reply, '\n');
  { size_t l = firstnl ? (size_t)(firstnl - reply) : strlen(reply);
    if (l >= sizeof action) l = sizeof action - 1;
    memcpy(action, reply, l); action[l] = '\0'; }

  if (!strncmp(action, "DONE", 4)) {
   ai_pane(f, action[0] ? action : "[agent] done", 0);
   wpe_ai_trace("agent done");
   free(reply);
   break;
  }
  if (strncmp(action, "TOOL ", 5)) {          /* not a tool call = final answer */
   ai_pane(f, action, 0);
   free(reply);
   break;
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
    if (ai_agent_approve(f, arg)) result = ai_run_capture(arg);
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
      if (content && ai_agent_approve(f, what)) {
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
     if (tr) { snprintf(tr, n, "TOOL RESULT:\n%s", result); ai_ml_add(&ml, "user", tr); free(tr); } }
   free(result);
  }
  free(reply);
 }
 if (iter >= AI_AGENT_MAX_ITERS) ai_pane(f, "[agent] stopped (max steps)", 0);
 ai_ml_free(&ml);
 return 0;
}

#endif /* WPE_AI */

typedef int wpe_ai_ui_translation_unit;
