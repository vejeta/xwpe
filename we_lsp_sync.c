/* we_lsp_sync.c -- the language server engine's waiting calls, for the C
 * tests only.
 *
 * The editor asks a language server on its loop (we_lsp.c: e_lsp_*_async,
 * answered by continuation) and never waits.  The engine tests drive a
 * real server without an editor, and for them a call that returns the
 * answer is the natural shape: these wrappers send the loop request and run
 * the loop (we_fdloop.c) until its continuation has been told, or a
 * deadline passes.  This file is not in the editor's link set, so no wait
 * of the editor's can come from here.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <poll.h>

#include "we_lsp.h"
#include "we_fdloop.h"

#define LSP_SYNC_TMO_INIT  120000   /* server start + initialize                */
#define LSP_SYNC_TMO_REQ    30000   /* an interactive request once warm          */

/* What a waiting call collects from its continuation. */
typedef struct {
 int   done;          /* the continuation has been told                     */
 char *text;          /* hover, format: the text, the caller's to free       */
 int   found;         /* a location: one came                                */
 char *path;          /* a location's file (out_path of the caller)          */
 size_t path_sz;
 int   line;
 e_lsp_units character;
 e_lsp_completion_item *items;   /* completion: the caller's array            */
 int   max, n;
 e_lsp_location   *locs;         /* references, highlight: the caller's array */
 e_lsp_symbol     *syms;         /* symbols, hierarchies                      */
 e_lsp_range      *ranges;       /* selection range                           */
 e_lsp_code_lens  *lenses;       /* code lenses                               */
 e_lsp_inlay_hint *inlays;       /* inlay hints                               */
 e_lsp_sem_token  *tokens;       /* semantic tokens                           */
 e_lsp_code_action *acts;        /* code actions                              */
 int   others;                   /* rename, code action: other files touched  */
} lsp_sync_wait;

/**
 * lsp_sync_now_ms - A monotonic instant, for the waits' deadlines.
 */
static long lsp_sync_now_ms(void)
{
 struct timespec ts;

 clock_gettime(CLOCK_MONOTONIC, &ts);
 return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/**
 * lsp_sync_on_readable - The loop found the server's output readable.
 */
static void lsp_sync_on_readable(int fd, void *data)
{
 (void)fd;
 e_lsp_poll(data);
}

/**
 * lsp_sync_until - Run the loop until @done says so, the server is gone, or
 * @timeout_ms passes.
 * @s: the session; @done: the predicate; @arg: its argument.
 * Return: 1 when @done said so.
 */
static int lsp_sync_until(e_lsp_session *s, int (*done)(e_lsp_session *, void *),
                          void *arg, int timeout_ms)
{
 long deadline = lsp_sync_now_ms() + timeout_ms;
 int fd = e_lsp_fd(s), ok;

 if (fd < 0 || wpe_fd_add(fd, POLLIN, lsp_sync_on_readable, s) != 0)
  return done(s, arg);
 while (!done(s, arg) && !e_lsp_dead(s))
 {
  long left = deadline - lsp_sync_now_ms();

  if (left <= 0)
   break;
  wpe_fd_poll((int)left);
 }
 ok = done(s, arg);
 wpe_fd_del(fd);
 return ok;
}

/** lsp_sync_told - Has the continuation been told? */
static int lsp_sync_told(e_lsp_session *s, void *arg)
{
 (void)s;
 return ((lsp_sync_wait *)arg)->done;
}

/** lsp_sync_started - Has the server answered `initialize`? */
static int lsp_sync_started(e_lsp_session *s, void *arg)
{
 (void)arg;
 return e_lsp_started(s);
}

/** lsp_sync_diagnostics_came - Has the watched file's batch come? */
static int lsp_sync_diagnostics_came(e_lsp_session *s, void *arg)
{
 (void)arg;
 return e_lsp_diagnostics_seen(s) > 0;
}

/**
 * lsp_sync_answer - Wait for the request whose continuation is @w's; a
 * request not answered in time is given up, so its continuation is told.
 * Return: 1 when it was answered.
 */
static int lsp_sync_answer(e_lsp_session *s, lsp_sync_wait *w, int sent)
{
 if (sent != E_D_SEND_OK)
  return 0;
 if (!lsp_sync_until(s, lsp_sync_told, w, LSP_SYNC_TMO_REQ))
  e_lsp_requests_cancel(s, E_D_REQ_TIMED_OUT);
 return w->done;
}

e_lsp_session *e_lsp_open(char *const argv[], const char *root_dir,
                          const char *lang, const e_lsp_host *host)
{
 e_lsp_session *s = e_lsp_open_async(argv, root_dir, lang, host);

 if (!s)
  return NULL;
 if (!lsp_sync_until(s, lsp_sync_started, NULL, LSP_SYNC_TMO_INIT))
 {
  e_lsp_close(s);
  return NULL;
 }
 return s;
}

int e_lsp_wait_diagnostics(e_lsp_session *s, const char *path, int timeout_ms)
{
 if (!s)
  return 0;
 e_lsp_diagnostics_watch(s, path);
 return lsp_sync_until(s, lsp_sync_diagnostics_came, NULL, timeout_ms);
}

/** lsp_sync_text_told - A hover or a format answered: keep its text. */
static void lsp_sync_text_told(E_D_REQ_END how, char *text, void *ctx)
{
 lsp_sync_wait *w = ctx;

 (void)how;
 w->text = text;
 w->done = 1;
}

char *e_lsp_hover(e_lsp_session *s, const char *path, int line, e_lsp_units character)
{
 lsp_sync_wait w;

 memset(&w, 0, sizeof w);
 if (!s)
  return NULL;
 lsp_sync_answer(s, &w, e_lsp_hover_async(s, path, line, character,
                                          lsp_sync_text_told, &w));
 return w.text;
}

/** lsp_sync_location_told - A jump answered: keep its first location. */
static void lsp_sync_location_told(E_D_REQ_END how, int found, const char *path,
                                   int line, e_lsp_units character, void *ctx)
{
 lsp_sync_wait *w = ctx;

 (void)how;
 w->done = 1;
 w->found = found;
 if (!found)
  return;
 snprintf(w->path, w->path_sz, "%s", path);
 w->line = line;
 w->character = character;
}

/**
 * lsp_sync_locate - One of the three jumps, waited for.
 * Return: 0 with out_* set, or -1.
 */
static int lsp_sync_locate(e_lsp_session *s, int which, const char *path,
                           int line, e_lsp_units character, char *out_path,
                           size_t out_sz, int *out_line, e_lsp_units *out_char)
{
 lsp_sync_wait w;

 memset(&w, 0, sizeof w);
 if (!s)
  return -1;
 w.path = out_path;
 w.path_sz = out_sz;
 lsp_sync_answer(s, &w, e_lsp_locate_async(s, which, path, line, character,
                                           lsp_sync_location_told, &w));
 if (!w.found)
  return -1;
 if (out_line)
  *out_line = w.line;
 if (out_char)
  *out_char = w.character;
 return 0;
}

int e_lsp_definition(e_lsp_session *s, const char *path, int line, e_lsp_units character,
                     char *out_path, size_t out_sz, int *out_line, e_lsp_units *out_char)
{
 return lsp_sync_locate(s, E_LSP_LOCATE_DEFINITION, path, line, character,
                        out_path, out_sz, out_line, out_char);
}

int e_lsp_implementation(e_lsp_session *s, const char *path, int line, e_lsp_units character,
                         char *out_path, size_t out_sz, int *out_line, e_lsp_units *out_char)
{
 return lsp_sync_locate(s, E_LSP_LOCATE_IMPLEMENTATION, path, line, character,
                        out_path, out_sz, out_line, out_char);
}

int e_lsp_type_definition(e_lsp_session *s, const char *path, int line, e_lsp_units character,
                          char *out_path, size_t out_sz, int *out_line, e_lsp_units *out_char)
{
 return lsp_sync_locate(s, E_LSP_LOCATE_TYPE_DEFINITION, path, line, character,
                        out_path, out_sz, out_line, out_char);
}

/** lsp_sync_items_told - A completion answered: copy its candidates. */
static void lsp_sync_items_told(E_D_REQ_END how,
                                const e_lsp_completion_item *items, int n,
                                void *ctx)
{
 lsp_sync_wait *w = ctx;
 int i;

 (void)how;
 w->done = 1;
 w->n = n;
 for (i = 0; i < n && i < w->max; i++)
  w->items[i] = items[i];
 if (w->n > w->max)
  w->n = w->max;
}

int e_lsp_completion(e_lsp_session *s, const char *path, int line, e_lsp_units character,
                     e_lsp_completion_item *items, int max)
{
 lsp_sync_wait w;

 memset(&w, 0, sizeof w);
 if (!s)
  return -1;
 w.items = items;
 w.max = max;
 w.n = -1;
 if (!lsp_sync_answer(s, &w, e_lsp_completion_async(s, path, line, character,
                                                     max, lsp_sync_items_told,
                                                     &w)))
  return -1;
 return w.n;
}

/**
 * lsp_sync_wait_for - A wait collecting at most @max entries.
 */
static void lsp_sync_wait_for(lsp_sync_wait *w, int max)
{
 memset(w, 0, sizeof *w);
 w->max = max;
 w->n = -1;
}

/**
 * lsp_sync_count - What a list request returns: the count, or -1 when it
 * was not answered.
 */
static int lsp_sync_count(e_lsp_session *s, lsp_sync_wait *w, int sent)
{
 if (!lsp_sync_answer(s, w, sent))
  return -1;
 return w->n;
}

/**
 * lsp_sync_took - Note a list answer's count, at most the caller's room.
 * Return: how many entries to copy.
 */
static int lsp_sync_took(lsp_sync_wait *w, int n)
{
 w->done = 1;
 w->n = n;
 if (w->n > w->max)
  w->n = w->max;
 return w->n;
}

/** lsp_sync_locs_told - References or highlight answered. */
static void lsp_sync_locs_told(E_D_REQ_END how, const e_lsp_location *locs,
                               int n, void *ctx)
{
 lsp_sync_wait *w = ctx;
 int i, k = lsp_sync_took(w, n);

 (void)how;
 for (i = 0; i < k; i++)
  w->locs[i] = locs[i];
}

/** lsp_sync_syms_told - Symbols or a hierarchy answered. */
static void lsp_sync_syms_told(E_D_REQ_END how, const e_lsp_symbol *syms,
                               int n, void *ctx)
{
 lsp_sync_wait *w = ctx;
 int i, k = lsp_sync_took(w, n);

 (void)how;
 for (i = 0; i < k; i++)
  w->syms[i] = syms[i];
}

/** lsp_sync_ranges_told - Selection range answered. */
static void lsp_sync_ranges_told(E_D_REQ_END how, const e_lsp_range *ranges,
                                 int n, void *ctx)
{
 lsp_sync_wait *w = ctx;
 int i, k = lsp_sync_took(w, n);

 (void)how;
 for (i = 0; i < k; i++)
  w->ranges[i] = ranges[i];
}

/** lsp_sync_lenses_told - Code lenses answered. */
static void lsp_sync_lenses_told(E_D_REQ_END how, const e_lsp_code_lens *lenses,
                                 int n, void *ctx)
{
 lsp_sync_wait *w = ctx;
 int i, k = lsp_sync_took(w, n);

 (void)how;
 for (i = 0; i < k; i++)
  w->lenses[i] = lenses[i];
}

/** lsp_sync_inlays_told - Inlay hints answered. */
static void lsp_sync_inlays_told(E_D_REQ_END how, const e_lsp_inlay_hint *hints,
                                 int n, void *ctx)
{
 lsp_sync_wait *w = ctx;
 int i, k = lsp_sync_took(w, n);

 (void)how;
 for (i = 0; i < k; i++)
  w->inlays[i] = hints[i];
}

/** lsp_sync_tokens_told - Semantic tokens answered. */
static void lsp_sync_tokens_told(E_D_REQ_END how, const e_lsp_sem_token *tokens,
                                 int n, void *ctx)
{
 lsp_sync_wait *w = ctx;
 int i, k = lsp_sync_took(w, n);

 (void)how;
 for (i = 0; i < k; i++)
  w->tokens[i] = tokens[i];
}

/** lsp_sync_acts_told - Code actions answered. */
static void lsp_sync_acts_told(E_D_REQ_END how, const e_lsp_code_action *acts,
                               int n, void *ctx)
{
 lsp_sync_wait *w = ctx;
 int i, k = lsp_sync_took(w, n);

 (void)how;
 for (i = 0; i < k; i++)
  w->acts[i] = acts[i];
}

/** lsp_sync_applied_told - A rename or a code action answered. */
static void lsp_sync_applied_told(E_D_REQ_END how, char *newtext, int others,
                                  void *ctx)
{
 lsp_sync_wait *w = ctx;

 (void)how;
 w->done = 1;
 w->text = newtext;
 w->others = others;
}

char *e_lsp_signature_help(e_lsp_session *s, const char *path,
                           int line, e_lsp_units character)
{
 lsp_sync_wait w;

 memset(&w, 0, sizeof w);
 if (!s)
  return NULL;
 lsp_sync_answer(s, &w, e_lsp_signature_help_async(s, path, line, character,
                                                   lsp_sync_text_told, &w));
 return w.text;
}

int e_lsp_references(e_lsp_session *s, const char *path, int line, e_lsp_units character,
                     e_lsp_location *locs, int max)
{
 lsp_sync_wait w;

 if (!s)
  return -1;
 lsp_sync_wait_for(&w, max);
 w.locs = locs;
 return lsp_sync_count(s, &w, e_lsp_references_async(s, path, line, character,
                                                     max, lsp_sync_locs_told,
                                                     &w));
}

int e_lsp_document_highlight(e_lsp_session *s, const char *path, int line,
                             e_lsp_units character, e_lsp_location *locs, int max)
{
 lsp_sync_wait w;

 if (!s)
  return -1;
 lsp_sync_wait_for(&w, max);
 w.locs = locs;
 return lsp_sync_count(s, &w,
                       e_lsp_document_highlight_async(s, path, line, character,
                                                      max, lsp_sync_locs_told,
                                                      &w));
}

int e_lsp_call_hierarchy(e_lsp_session *s, const char *path, int line,
                         e_lsp_units character, int outgoing,
                         e_lsp_symbol *out, int max)
{
 lsp_sync_wait w;

 if (!s)
  return -1;
 lsp_sync_wait_for(&w, max);
 w.syms = out;
 return lsp_sync_count(s, &w,
                       e_lsp_call_hierarchy_async(s, path, line, character,
                                                  outgoing, max,
                                                  lsp_sync_syms_told, &w));
}

int e_lsp_type_hierarchy(e_lsp_session *s, const char *path, int line,
                         e_lsp_units character, int subtypes,
                         e_lsp_symbol *out, int max)
{
 lsp_sync_wait w;

 if (!s)
  return -1;
 lsp_sync_wait_for(&w, max);
 w.syms = out;
 return lsp_sync_count(s, &w,
                       e_lsp_type_hierarchy_async(s, path, line, character,
                                                  subtypes, max,
                                                  lsp_sync_syms_told, &w));
}

int e_lsp_selection_range(e_lsp_session *s, const char *path, int line,
                          e_lsp_units character, e_lsp_range *out, int max)
{
 lsp_sync_wait w;

 if (!s)
  return -1;
 lsp_sync_wait_for(&w, max);
 w.ranges = out;
 return lsp_sync_count(s, &w,
                       e_lsp_selection_range_async(s, path, line, character,
                                                   max, lsp_sync_ranges_told,
                                                   &w));
}

int e_lsp_code_lenses(e_lsp_session *s, const char *path,
                      e_lsp_code_lens *lenses, int max)
{
 lsp_sync_wait w;

 if (!s)
  return -1;
 lsp_sync_wait_for(&w, max);
 w.lenses = lenses;
 return lsp_sync_count(s, &w, e_lsp_code_lenses_async(s, path, max,
                                                      lsp_sync_lenses_told,
                                                      &w));
}

int e_lsp_inlay_hints(e_lsp_session *s, const char *path,
                      int start_line, int end_line,
                      e_lsp_inlay_hint *out, int max)
{
 lsp_sync_wait w;

 if (!s)
  return -1;
 lsp_sync_wait_for(&w, max);
 w.inlays = out;
 return lsp_sync_count(s, &w, e_lsp_inlay_hints_async(s, path, start_line,
                                                      end_line, max,
                                                      lsp_sync_inlays_told,
                                                      &w));
}

int e_lsp_semantic_tokens(e_lsp_session *s, const char *path,
                          e_lsp_sem_token *out, int max)
{
 lsp_sync_wait w;

 if (!s)
  return -1;
 lsp_sync_wait_for(&w, max);
 w.tokens = out;
 return lsp_sync_count(s, &w, e_lsp_semantic_tokens_async(s, path, max,
                                                          lsp_sync_tokens_told,
                                                          &w));
}

int e_lsp_document_symbols(e_lsp_session *s, const char *path,
                           e_lsp_symbol *syms, int max)
{
 lsp_sync_wait w;

 if (!s)
  return -1;
 lsp_sync_wait_for(&w, max);
 w.syms = syms;
 return lsp_sync_count(s, &w, e_lsp_document_symbols_async(s, path, max,
                                                           lsp_sync_syms_told,
                                                           &w));
}

int e_lsp_workspace_symbols(e_lsp_session *s, const char *query,
                            e_lsp_symbol *syms, int max)
{
 lsp_sync_wait w;

 if (!s)
  return -1;
 lsp_sync_wait_for(&w, max);
 w.syms = syms;
 return lsp_sync_count(s, &w, e_lsp_workspace_symbols_async(s, query, max,
                                                            lsp_sync_syms_told,
                                                            &w));
}

int e_lsp_code_actions(e_lsp_session *s, const char *path, int line, e_lsp_units character,
                       e_lsp_code_action *acts, int max)
{
 lsp_sync_wait w;

 if (!s)
  return -1;
 lsp_sync_wait_for(&w, max);
 w.acts = acts;
 return lsp_sync_count(s, &w, e_lsp_code_actions_async(s, path, line, character,
                                                       max, lsp_sync_acts_told,
                                                       &w));
}

char *e_lsp_apply_code_action(e_lsp_session *s, int index, const char *path,
                              const char *current_text, int *other_files)
{
 lsp_sync_wait w;

 memset(&w, 0, sizeof w);
 if (other_files)
  *other_files = 0;
 if (!s)
  return NULL;
 lsp_sync_answer(s, &w, e_lsp_code_action_run_async(s, index, path, current_text,
                                                    lsp_sync_applied_told, &w));
 if (other_files)
  *other_files = w.others;
 return w.text;
}

char *e_lsp_rename(e_lsp_session *s, const char *path, int line, e_lsp_units character,
                   const char *new_name, const char *current_text,
                   int *other_files)
{
 lsp_sync_wait w;

 memset(&w, 0, sizeof w);
 if (other_files)
  *other_files = 0;
 if (!s)
  return NULL;
 lsp_sync_answer(s, &w, e_lsp_rename_async(s, path, line, character, new_name,
                                           current_text, lsp_sync_applied_told,
                                           &w));
 if (other_files)
  *other_files = w.others;
 return w.text;
}

char *e_lsp_format(e_lsp_session *s, const char *path, const char *current_text)
{
 lsp_sync_wait w;

 memset(&w, 0, sizeof w);
 if (!s)
  return NULL;
 lsp_sync_answer(s, &w, e_lsp_format_async(s, path, current_text, NULL,
                                           lsp_sync_text_told, &w));
 return w.text;
}

char *e_lsp_format_range(e_lsp_session *s, const char *path,
                         const char *current_text,
                         int start_line, e_lsp_units start_char,
                         int end_line, e_lsp_units end_char)
{
 lsp_sync_wait w;
 e_lsp_range r;

 memset(&w, 0, sizeof w);
 if (!s)
  return NULL;
 r.start_line = start_line;
 r.start_char = start_char;
 r.end_line = end_line;
 r.end_char = end_char;
 lsp_sync_answer(s, &w, e_lsp_format_async(s, path, current_text, &r,
                                           lsp_sync_text_told, &w));
 return w.text;
}
