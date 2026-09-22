/* we_ai_host.c                                          */
/* Copyright (C) 2026 Juan Manuel Mendez Rey             */
/* This is free software; you can redistribute it and/or */
/* modify it under the terms of the                      */
/* GNU General Public License, see the file COPYING.     */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef WPE_AI_AGENT_HOST

#include "we_ai.h"
#include "we_ai_http.h"
#include "we_ai_host.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <json-c/json.h>

#define HOST_MAX_EDITS 32
#define HOST_PATH_MAX  1024

struct wpe_host {
 pid_t           pid;
 int             in_fd;                 /* child stdin  (we write user turns) */
 int             out_fd;                /* child stdout (stream-json events)  */
 wpe_http_stream hs;                    /* incremental line framer            */
 char            session_id[128];
 /* Files an edit tool touched this turn; reloaded into open buffers at the
    turn boundary, once the writes are surely on disk. */
 char            edited[HOST_MAX_EDITS][HOST_PATH_MAX];
 int             nedited;
 int             text_seen;             /* assistant text streamed this turn */
};

/* ------------------------------------------------------------------ helpers */

static void host_set_nonblock(int fd)
{
 int fl = fcntl(fd, F_GETFL, 0);
 if (fl != -1) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Remember a path an edit tool wrote, de-duplicated, for reload at turn end. */
static void host_note_edit(wpe_host *h, const char *path)
{
 int i;
 if (!path || !path[0] || h->nedited >= HOST_MAX_EDITS) return;
 for (i = 0; i < h->nedited; i++)
  if (!strcmp(h->edited[i], path)) return;
 snprintf(h->edited[h->nedited], HOST_PATH_MAX, "%s", path);
 h->nedited++;
}

/* Map an edit tool's input object to the file path it targets. */
static const char *host_tool_path(struct json_object *input)
{
 struct json_object *p;
 if (!input) return NULL;
 if (json_object_object_get_ex(input, "file_path", &p)) return json_object_get_string(p);
 if (json_object_object_get_ex(input, "path", &p))      return json_object_get_string(p);
 if (json_object_object_get_ex(input, "notebook_path", &p)) return json_object_get_string(p);
 return NULL;
}

/* A short, human argument for the "[host] <tool> <arg>" status line. */
static const char *host_tool_arg(const char *tool, struct json_object *input)
{
 struct json_object *p;
 if (!input) return "";
 if (!strcmp(tool, "Bash")) {
  if (json_object_object_get_ex(input, "command", &p)) return json_object_get_string(p);
 }
 if (!strcmp(tool, "Grep") || !strcmp(tool, "Glob")) {
  if (json_object_object_get_ex(input, "pattern", &p)) return json_object_get_string(p);
 }
 { const char *fp = host_tool_path(input); if (fp) return fp; }
 return "";
}

static int host_is_edit_tool(const char *tool)
{
 return !strcmp(tool, "Write") || !strcmp(tool, "Edit") ||
        !strcmp(tool, "MultiEdit") || !strcmp(tool, "NotebookEdit");
}

/* ------------------------------------------------------------------ spawn */

wpe_host *wpe_host_start(const char *model, const char *resume, int policy,
                         char *err, size_t errsz)
{
 int in[2], out[2];
 pid_t pid;
 wpe_host *h;
 /* Which agent CLI to host: XWPE_AI_HOST_CMD (tests / one-off) wins, else the
    persisted AIHostCommand (Options), else NULL -> the built-in `claude` argv.
    A configured command runs via the shell, so `aider`/`opencode`/... need no
    code change -- just a different AIHostCommand. */
 const char *hostcmd = getenv("XWPE_AI_HOST_CMD");
 if (!hostcmd || !*hostcmd) hostcmd = e_ai_host_command;

 if (err && errsz) err[0] = '\0';
 if (pipe(in) != 0) { if (err) snprintf(err, errsz, "pipe: %s", strerror(errno)); return NULL; }
 if (pipe(out) != 0) { close(in[0]); close(in[1]);
   if (err) snprintf(err, errsz, "pipe: %s", strerror(errno)); return NULL; }

 pid = fork();
 if (pid < 0) {
  close(in[0]); close(in[1]); close(out[0]); close(out[1]);
  if (err) snprintf(err, errsz, "fork: %s", strerror(errno));
  return NULL;
 }
 if (pid == 0) {                        /* child */
  char *argv[24];
  int a = 0, dn;
  dup2(in[0], 0);
  dup2(out[1], 1);
  dn = open("/dev/null", O_WRONLY);
  if (dn >= 0) { dup2(dn, 2); close(dn); }
  close(in[0]); close(in[1]); close(out[0]); close(out[1]);
  if (hostcmd && hostcmd[0]) {          /* mock/other host: run via the shell */
   argv[a++] = "/bin/sh"; argv[a++] = "-c"; argv[a++] = (char *)hostcmd; argv[a] = NULL;
   execvp("/bin/sh", argv);
   _exit(127);
  }
  argv[a++] = "claude";
  argv[a++] = "--print";
  argv[a++] = "--output-format"; argv[a++] = "stream-json";
  argv[a++] = "--input-format";  argv[a++] = "stream-json";
  argv[a++] = "--verbose";
  if (model && *model && strcmp(model, "default")) { argv[a++] = "--model"; argv[a++] = (char *)model; }
  if (resume && *resume) { argv[a++] = "--resume"; argv[a++] = (char *)resume; }
  /* Permission dial -> the CLI's permission mode, with per-tool prompts routed
     back to the editor (--permission-prompts host --permission-prompt-tool stdio:
     the CLI emits a control_request and blocks until we answer on stdin).
       Auto  : run unattended, no prompts.
       Edits : auto-accept file edits, ask before commands/other gated tools.
       Ask   : ask before every gated tool. */
  if (policy == WPE_AI_POLICY_AUTO) {
   argv[a++] = "--dangerously-skip-permissions";
  } else {
   argv[a++] = "--permission-mode";
   argv[a++] = (policy == WPE_AI_POLICY_EDITS) ? "acceptEdits" : "default";
   argv[a++] = "--permission-prompts"; argv[a++] = "host";
   argv[a++] = "--permission-prompt-tool"; argv[a++] = "stdio";
  }
  argv[a] = NULL;
  execvp("claude", argv);
  _exit(127);
 }

 close(in[0]); close(out[1]);           /* parent keeps in[1] (write), out[0] (read) */
 h = calloc(1, sizeof *h);
 if (!h) {
  close(in[1]); close(out[0]);
  kill(pid, SIGKILL); waitpid(pid, NULL, 0);
  if (err) snprintf(err, errsz, "out of memory");
  return NULL;
 }
 h->pid = pid;
 h->in_fd = in[1];
 h->out_fd = out[0];
 host_set_nonblock(h->out_fd);
 wpe_http_stream_init(&h->hs);
 h->hs.header_done = 1;                 /* raw pipe: bytes are body, not HTTP */
 h->hs.status = 200;
 return h;
}

int wpe_host_fd(wpe_host *h) { return h ? h->out_fd : -1; }

const char *wpe_host_session_id(wpe_host *h) { return h ? h->session_id : ""; }

/* ------------------------------------------------------------------ send */

int wpe_host_send(wpe_host *h, const char *user_text)
{
 struct json_object *o, *m;
 const char *line;
 size_t len, off;
 int rc = 0;

 if (!h || h->in_fd < 0) return -1;
 h->text_seen = 0;                      /* a fresh turn begins */
 o = json_object_new_object();
 m = json_object_new_object();
 json_object_object_add(m, "role", json_object_new_string("user"));
 json_object_object_add(m, "content", json_object_new_string(user_text ? user_text : ""));
 json_object_object_add(o, "type", json_object_new_string("user"));
 json_object_object_add(o, "message", m);
 line = json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN);
 len = strlen(line);
 for (off = 0; off < len; ) {
  ssize_t w = write(h->in_fd, line + off, len - off);
  if (w < 0) { if (errno == EINTR) continue; rc = -1; break; }
  off += (size_t)w;
 }
 if (rc == 0) { char nl = '\n'; if (write(h->in_fd, &nl, 1) < 0) rc = -1; }
 json_object_put(o);
 return rc;
}

/* Answer a can_use_tool permission request on stdin, in the CLI's control-response
   envelope (verified against claude 2.1.278):
   {"type":"control_response","response":{"subtype":"success","request_id":R,
     "response":{"behavior":"allow","updatedInput":<input>}}}  (or deny + message).
   The CLI blocks until this arrives. */
static void host_answer_permission(wpe_host *h, const char *request_id,
                                   int allow, struct json_object *input)
{
 struct json_object *o, *outer, *inner;
 const char *line;
 size_t len, off;

 if (!h || h->in_fd < 0 || !request_id) return;
 inner = json_object_new_object();
 json_object_object_add(inner, "behavior", json_object_new_string(allow ? "allow" : "deny"));
 if (allow) {
  if (input) json_object_object_add(inner, "updatedInput", json_object_get(input));
 } else {
  json_object_object_add(inner, "message", json_object_new_string("Denied by the user"));
 }
 outer = json_object_new_object();
 json_object_object_add(outer, "subtype", json_object_new_string("success"));
 json_object_object_add(outer, "request_id", json_object_new_string(request_id));
 json_object_object_add(outer, "response", inner);
 o = json_object_new_object();
 json_object_object_add(o, "type", json_object_new_string("control_response"));
 json_object_object_add(o, "response", outer);

 line = json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN);
 len = strlen(line);
 for (off = 0; off < len; ) {
  ssize_t w = write(h->in_fd, line + off, len - off);
  if (w < 0) { if (errno == EINTR) continue; break; }
  off += (size_t)w;
 }
 { char nl = '\n'; ssize_t w = write(h->in_fd, &nl, 1); (void)w; }
 json_object_put(o);
}

/* ------------------------------------------------------------------ parse */

/* Dispatch one assistant message's content blocks. */
static void host_handle_assistant(wpe_host *h, struct json_object *msg,
                                  const wpe_host_events *ev, void *ud)
{
 struct json_object *content;
 int i, n;
 if (!msg || !json_object_object_get_ex(msg, "content", &content)) return;
 if (!json_object_is_type(content, json_type_array)) {
  /* content may be a plain string */
  if (json_object_is_type(content, json_type_string) && ev->on_text)
   ev->on_text(json_object_get_string(content), ud);
  return;
 }
 n = json_object_array_length(content);
 for (i = 0; i < n; i++) {
  struct json_object *blk = json_object_array_get_idx(content, i), *t, *x;
  const char *bt;
  if (!blk || !json_object_object_get_ex(blk, "type", &t)) continue;
  bt = json_object_get_string(t);
  if (!strcmp(bt, "text")) {
   if (json_object_object_get_ex(blk, "text", &x)) {
    h->text_seen = 1;
    if (ev->on_text) ev->on_text(json_object_get_string(x), ud);
   }
  } else if (!strcmp(bt, "tool_use")) {
   struct json_object *nm, *in = NULL;
   const char *tool = "tool";
   if (json_object_object_get_ex(blk, "name", &nm)) tool = json_object_get_string(nm);
   json_object_object_get_ex(blk, "input", &in);
   if (ev->on_tool) ev->on_tool(tool, host_tool_arg(tool, in), ud);
   if (host_is_edit_tool(tool)) host_note_edit(h, host_tool_path(in));
  }
 }
}

/* Parse and dispatch one complete stream-json line. */
static void host_handle_line(wpe_host *h, const char *line,
                             const wpe_host_events *ev, void *ud, int *turn_done)
{
 struct json_tokener *tok;
 struct json_object *o, *t, *r;
 const char *type;

 if (!line || !line[0]) return;
 tok = json_tokener_new();
 o = json_tokener_parse_ex(tok, line, (int)strlen(line));
 if (!o || json_tokener_get_error(tok) != json_tokener_success) {
  if (o) json_object_put(o);
  json_tokener_free(tok);
  return;                               /* ignore a non-JSON / partial line */
 }
 json_tokener_free(tok);

 if (json_object_object_get_ex(o, "session_id", &r))
  snprintf(h->session_id, sizeof h->session_id, "%s", json_object_get_string(r));

 type = json_object_object_get_ex(o, "type", &t) ? json_object_get_string(t) : "";

 if (!strcmp(type, "assistant")) {
  struct json_object *m;
  if (json_object_object_get_ex(o, "message", &m)) host_handle_assistant(h, m, ev, ud);
 } else if (!strcmp(type, "system")) {
  /* system events are status/init/thinking-token counters -- Opus 5 emits many
     during a thinking phase.  Do NOT print one pane line each (it spammed the
     window); surface only an error subtype. */
  struct json_object *sub;
  const char *s = json_object_object_get_ex(o, "subtype", &sub)
                    ? json_object_get_string(sub) : "";
  if (ev->on_notice && s[0] && strstr(s, "error")) {
   char l[160]; snprintf(l, sizeof l, "[agent] %s", s); ev->on_notice(l, ud);
  }
 } else if (!strcmp(type, "result")) {
  const char *summary = "";
  int is_err = 0;
  struct json_object *x;
  if (json_object_object_get_ex(o, "is_error", &x)) is_err = json_object_get_boolean(x);
  if (json_object_object_get_ex(o, "result", &x)) summary = json_object_get_string(x);
  else if (json_object_object_get_ex(o, "subtype", &x)) summary = json_object_get_string(x);
  { int i; for (i = 0; i < h->nedited; i++)                 /* writes are on disk now */
     if (ev->on_file_edit) ev->on_file_edit(h->edited[i], ud); }
  h->nedited = 0;
  /* Normally the answer streamed via on_text; only if the turn produced NO text
     (e.g. a pure result) do we surface the result string as the answer, so the
     answer neither doubles nor goes missing. */
  if (!is_err && !h->text_seen && summary[0] && ev->on_text) ev->on_text(summary, ud);
  if (ev->on_result) ev->on_result(summary, is_err, ud);
  if (turn_done) *turn_done = 1;
 } else if (!strcmp(type, "control_request")) {
  /* The agent asks to use a gated tool; ask the user (on_permission) and answer
     on stdin, which unblocks the CLI. */
  struct json_object *req, *sub, *tn, *inp = NULL, *rq;
  const char *reqid = json_object_object_get_ex(o, "request_id", &rq)
                        ? json_object_get_string(rq) : NULL;
  const char *subt = "", *tool = "tool";
  if (json_object_object_get_ex(o, "request", &req)) {
   if (json_object_object_get_ex(req, "subtype", &sub)) subt = json_object_get_string(sub);
   if (json_object_object_get_ex(req, "tool_name", &tn)) tool = json_object_get_string(tn);
   json_object_object_get_ex(req, "input", &inp);
  }
  if (!strcmp(subt, "can_use_tool")) {
   int allow = ev->on_permission ? ev->on_permission(tool, host_tool_arg(tool, inp), ud) : 0;
   host_answer_permission(h, reqid, allow, inp);
  }
 }
 /* "user" (tool_result echoes) and "stream_event" (partials) are ignored in
    this phase; full assistant messages carry the text and tool calls. */
 json_object_put(o);
}

void wpe_host_pump(wpe_host *h, const wpe_host_events *ev, void *ud,
                   int *turn_done, int *hup)
{
 char buf[8192];
 char *line;
 size_t llen;

 if (turn_done) *turn_done = 0;
 if (hup) *hup = 0;
 if (!h || h->out_fd < 0) { if (hup) *hup = 1; return; }

 for (;;) {
  ssize_t rd = read(h->out_fd, buf, sizeof buf);
  if (rd > 0) { wpe_http_stream_push(&h->hs, buf, (size_t)rd); }
  else if (rd == 0) { wpe_http_stream_eof(&h->hs); if (hup) *hup = 1; break; }
  else {
   if (errno == EAGAIN || errno == EWOULDBLOCK) break;
   if (errno == EINTR) continue;
   wpe_http_stream_eof(&h->hs); if (hup) *hup = 1; break;
  }
 }
 while (wpe_http_stream_next_line(&h->hs, &line, &llen))
  host_handle_line(h, line, ev, ud, turn_done);
}

/* ------------------------------------------------------------------ teardown */

void wpe_host_free(wpe_host *h)
{
 if (!h) return;
 if (h->in_fd >= 0)  close(h->in_fd);   /* EOF on stdin asks the child to exit */
 if (h->out_fd >= 0) close(h->out_fd);
 if (h->pid > 0) {
  int status;
  pid_t r;
  kill(h->pid, SIGTERM);                /* ask it to quit (stdin EOF + SIGTERM) */
  r = waitpid(h->pid, &status, WNOHANG);
  if (r == 0) {                         /* still running: force it and reap */
   kill(h->pid, SIGKILL);
   waitpid(h->pid, &status, 0);
  }
 }
 wpe_http_stream_free(&h->hs);
 free(h);
}

#endif /* WPE_AI_AGENT_HOST */

typedef int wpe_ai_host_translation_unit;
