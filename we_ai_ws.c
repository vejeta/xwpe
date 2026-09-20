/* we_ai_ws.c - the AI assistant's WORKSPACE layer: which files the agent may
 * study (scope), a checkpoint before an unattended run, the changeset review
 * afterwards (navigable with the Next/Prev-Error keys), and sessions tied to
 * the workspace.  Editor-side.  Compiled only under --enable-ai (WPE_AI). */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef WPE_AI

#include "messages.h"
#include "edit.h"
#include "WeExpArr.h"
#include "progr.h"
#include "we_ai.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <json-c/json.h>

extern struct dirfile **e_p_df;      /* project member list (we_prog.c)      */

/* ===================== small helpers ===================================== */

static char *ws_strdup(const char *s)
{
 size_t n = strlen(s ? s : "") + 1;
 char *p = malloc(n);
 if (p) memcpy(p, s ? s : "", n);
 return p;
}

/* Single-quote a string for /bin/sh. */
static void ws_shq(char *dst, size_t sz, const char *src)
{
 size_t o = 0;
 if (sz < 3) { if (sz) dst[0] = '\0'; return; }
 dst[o++] = '\'';
 for (; *src && o + 5 < sz; src++) {
  if (*src == '\'') { memcpy(dst + o, "'\\''", 4); o += 4; }
  else dst[o++] = *src;
 }
 dst[o++] = '\'';
 dst[o] = '\0';
}

static void ws_list_push(char ***l, int *n, int *cap, char *s)
{
 if (*n == *cap) { *cap = *cap ? *cap * 2 : 8; *l = realloc(*l, (size_t)(*cap) * sizeof **l); }
 (*l)[(*n)++] = s;
}

static int ws_list_has(char **l, int n, const char *s)
{
 int i;
 for (i = 0; i < n; i++) if (!strcmp(l[i], s)) return 1;
 return 0;
}

void wpe_ai_free_list(char **list, int n)
{
 int i;
 if (!list) return;
 for (i = 0; i < n; i++) free(list[i]);
 free(list);
}

static char *ws_read_file(const char *path, size_t maxlen)
{
 FILE *fp = fopen(path, "rb");
 char *buf;
 size_t cap = 4096, len = 0;
 int ch;
 if (!fp) return NULL;
 buf = malloc(cap);
 if (!buf) { fclose(fp); return NULL; }
 while ((ch = fgetc(fp)) != EOF && len < maxlen) {
  if (len + 2 > cap) { char *nb; cap *= 2; nb = realloc(buf, cap); if (!nb) break; buf = nb; }
  buf[len++] = (char)ch;
 }
 buf[len] = '\0';
 fclose(fp);
 return buf;
}

static int ws_write_file(const char *path, const char *text)
{
 FILE *fp = fopen(path, "wb");
 if (!fp) return -1;
 fwrite(text, 1, strlen(text), fp);
 fclose(fp);
 return 0;
}

/* The workspace root: the project dir when a .prj is open, else the file's. */
static char *ws_root(FENSTER *f)
{
 char *d = e_build_dir(f);
 if (d && *d) return d;
 free(d);
 return ws_strdup(f->dirct ? f->dirct : ".");
}

/* ===================== scope ============================================= */

static const char *ws_reserved[] = {
 "Messages", "AI", "Watches", "Stack", "Project", "Grep", "Find", "Windows",
 "Variables", "Install", "Help", "Function-Index", "Noname", NULL };

static int ws_is_real_file_window(FENSTER *w)
{
 int i;
 if (!w || !w->datnam || !*w->datnam || !w->dirct) return 0;
 if (!DTMD_ISTEXT(w->dtmd)) return 0;
 for (i = 0; ws_reserved[i]; i++) if (!strcmp(w->datnam, ws_reserved[i])) return 0;
 return 1;
}

static int ws_source_like(const char *name)
{
 const char *ext = strrchr(name, '.');
 static const char *ok[] = { ".c", ".h", ".cc", ".cpp", ".hpp", ".py", ".go",
   ".rs", ".java", ".scala", ".js", ".ts", ".sh", ".md", ".txt", ".mk",
   ".am", ".ac", ".pl", ".f", ".pas", ".cob", NULL };
 int i;
 if (!ext) return !strcmp(name, "Makefile");
 for (i = 0; ok[i]; i++) if (!strcasecmp(ext, ok[i])) return 1;
 return 0;
}

int wpe_ai_scope_files(FENSTER *f, int with_project, int with_folder, char ***out)
{
 ECNT *cn = f->ed;
 char **l = NULL;
 int n = 0, cap = 0, i;

 for (i = cn->mxedt; i > 0; i--) {
  FENSTER *w = cn->f[i];
  char *p;
  if (!ws_is_real_file_window(w)) continue;
  p = e_mkfilename(w->dirct, w->datnam);
  if (p && !ws_list_has(l, n, p)) ws_list_push(&l, &n, &cap, p); else free(p);
 }
 if (with_project && e_project_is_open() && e_p_df && e_p_df[0]) {
  char *root = ws_root(f);
  int k;
  for (k = 0; k < e_p_df[0]->anz; k++) {
   const char *m = e_p_df[0]->name[k];
   char *p;
   if (!m || !*m || !strcmp(m, " ")) continue;
   if (m[0] == '/') p = ws_strdup(m);
   else { size_t L = strlen(root) + strlen(m) + 2; p = malloc(L); if (p) snprintf(p, L, "%s/%s", root, m); }
   if (p && !ws_list_has(l, n, p)) ws_list_push(&l, &n, &cap, p); else free(p);
  }
  free(root);
 }
 if (with_folder && f->dirct) {
  DIR *d = opendir(f->dirct);
  struct dirent *de;
  int added = 0;
  while (d && (de = readdir(d)) && added < 60) {
   char *p;
   size_t L;
   if (de->d_name[0] == '.' || !ws_source_like(de->d_name)) continue;
   L = strlen(f->dirct) + strlen(de->d_name) + 2;
   p = malloc(L);
   if (!p) continue;
   snprintf(p, L, "%s/%s", f->dirct, de->d_name);
   if (!ws_list_has(l, n, p)) { ws_list_push(&l, &n, &cap, p); added++; } else free(p);
  }
  if (d) closedir(d);
 }
 *out = l;
 return n;
}

/* A prompt block naming the files currently OPEN in the editor, with the one the
   user is focused on marked, so every AI mode is handed the working set directly
   instead of having to discover it (e.g. by inspecting running processes).
   Writes into buf (empty string when nothing qualifies) and returns it. */
char *wpe_ai_open_windows_block(FENSTER *f, char *buf, size_t n)
{
 ECNT *cn = f->ed;
 char **seen = NULL;
 size_t len = 0;
 /* When AI is invoked, f is often the AI pane rather than a file window, so mark
    the topmost real file window as focused; when f IS a file window, mark it. */
 int i, nseen = 0, cap = 0, count = 0, f_real = ws_is_real_file_window(f);

 buf[0] = '\0';
 for (i = cn->mxedt; i > 0 && len < n - 96; i--) {
  FENSTER *w = cn->f[i];
  char *p;
  int focused;
  if (!ws_is_real_file_window(w)) continue;
  p = e_mkfilename(w->dirct, w->datnam);
  if (!p) continue;
  if (ws_list_has(seen, nseen, p)) { free(p); continue; }   /* one entry per file */
  ws_list_push(&seen, &nseen, &cap, p);                     /* takes ownership of p */
  if (count == 0)
   len += (size_t)snprintf(buf + len, n - len,
     "FILES OPEN IN THE EDITOR (the user's working set right now; prefer these "
     "when the request does not name a file):\n");
  focused = f_real ? (w == f) : (count == 0);
  len += (size_t)snprintf(buf + len, n - len, "  %s%s\n", p,
     focused ? "   <- focused (the file the user is looking at)" : "");
  count++;
 }
 wpe_ai_free_list(seen, nseen);
 return buf;
}

static char *ws_window_text(FENSTER *w)
{
 BUFFER *b = w->b;
 size_t cap = 4096, len = 0;
 char *t = malloc(cap);
 int y;
 if (!t) return NULL;
 for (y = 0; y < b->mxlines; y++) {
  const char *ls = (const char *)b->bf[y].s;
  int ll = b->bf[y].len < 0 ? 0 : b->bf[y].len;
  if (len + (size_t)ll + 2 > cap) { char *nt; while (len + (size_t)ll + 2 > cap) cap *= 2; nt = realloc(t, cap); if (!nt) { free(t); return NULL; } t = nt; }
  if (ls && ll > 0) { memcpy(t + len, ls, (size_t)ll); len += (size_t)ll; }
  t[len++] = '\n';
 }
 t[len] = '\0';
 return t;
}

static FENSTER *ws_find_window(FENSTER *f, const char *path)
{
 ECNT *cn = f->ed;
 int i;
 for (i = cn->mxedt; i > 0; i--) {
  FENSTER *w = cn->f[i];
  char *p;
  int same;
  if (!ws_is_real_file_window(w)) continue;
  p = e_mkfilename(w->dirct, w->datnam);
  same = p && !strcmp(p, path);
  free(p);
  if (same) return w;
 }
 return NULL;
}

char *wpe_ai_read_scope_file(FENSTER *f, const char *path)
{
 FENSTER *w = ws_find_window(f, path);
 if (w) return ws_window_text(w);
 return ws_read_file(path, 200000);
}

/* ===================== checkpoint ======================================== */

static int    ck_mode;                 /* 0 none, 1 git ref, 2 snapshot copies */
static char   ck_ref[160];
static char   ck_dir[1024];
static char   ck_snapdir[1024];
static char **ck_files;
static int    ck_n;
static char **ck_before;  static int ck_before_n;  /* files present before   */
static char **ck_dirs;    static int ck_dirs_n;    /* dirs watched for new   */

int wpe_ai_checkpoint_active(void) { return ck_mode != 0; }

static void ck_clear(void)
{
 wpe_ai_free_list(ck_files, ck_n);
 wpe_ai_free_list(ck_before, ck_before_n);
 wpe_ai_free_list(ck_dirs, ck_dirs_n);
 ck_files = NULL; ck_n = 0;
 ck_before = NULL; ck_before_n = 0;
 ck_dirs = NULL; ck_dirs_n = 0;
 ck_mode = 0; ck_ref[0] = '\0';
}

/* Regular, non-hidden files directly inside dir (full paths), bounded. */
static void ws_list_dir_files(const char *dir, char ***l, int *n, int *cap)
{
 DIR *d = opendir(dir);
 struct dirent *de;
 int added = 0;
 while (d && (de = readdir(d)) && added < 2000) {
  char *p;
  size_t L;
  struct stat st;
  if (de->d_name[0] == '.') continue;
  L = strlen(dir) + strlen(de->d_name) + 2;
  p = malloc(L);
  if (!p) continue;
  snprintf(p, L, "%s/%s", dir, de->d_name);
  if (stat(p, &st) == 0 && S_ISREG(st.st_mode) && !ws_list_has(*l, *n, p)) {
   ws_list_push(l, n, cap, p);
   added++;
  } else free(p);
 }
 if (d) closedir(d);
}

/* Drop trailing slashes so the same directory always has ONE spelling (the
 * root may arrive as ".../dir/" while a file's dirname is ".../dir"). */
static void ws_strip_slash(char *d)
{
 size_t l = strlen(d);
 while (l > 1 && d[l - 1] == '/') d[--l] = '\0';
}

/* The directories to watch for NEW files: the root plus every scope file's dir. */
static void ws_watch_dirs(char **scope, int nscope)
{
 int i, cap = 0;
 wpe_ai_free_list(ck_dirs, ck_dirs_n);
 ck_dirs = NULL; ck_dirs_n = 0;
 ws_list_push(&ck_dirs, &ck_dirs_n, &cap, ws_strdup(ck_dir));
 for (i = 0; i < nscope; i++) {
  const char *sl = strrchr(scope[i], '/');
  char *d;
  if (!sl) continue;
  d = ws_strdup(scope[i]);
  d[sl - scope[i]] = '\0';
  if (!d[0]) { free(d); d = ws_strdup("/"); }
  ws_strip_slash(d);
  if (!ws_list_has(ck_dirs, ck_dirs_n, d)) ws_list_push(&ck_dirs, &ck_dirs_n, &cap, d);
  else free(d);
 }
}

int wpe_ai_checkpoint_create(FENSTER *f, char **scope, int nscope)
{
 char *root = ws_root(f);
 char q[1100], cmd[1400], *out;
 int i;

 ck_clear();
 strncpy(ck_dir, root, sizeof ck_dir - 1);
 ck_dir[sizeof ck_dir - 1] = '\0';
 ws_strip_slash(ck_dir);
 free(root);

 ws_shq(q, sizeof q, ck_dir);
 snprintf(cmd, sizeof cmd, "cd %s && git rev-parse --is-inside-work-tree 2>/dev/null", q);
 out = wpe_ai_run_capture(cmd);
 if (out && !strncmp(out, "true", 4)) {
  free(out);
  snprintf(cmd, sizeof cmd, "cd %s && git stash create 2>/dev/null", q);
  out = wpe_ai_run_capture(cmd);
  if (out) { size_t l = strlen(out); while (l && (out[l-1] == '\n' || out[l-1] == ' ')) out[--l] = '\0'; }
  if (out && *out && strlen(out) < sizeof ck_ref) strcpy(ck_ref, out);
  else strcpy(ck_ref, "HEAD");
  free(out);
  ck_mode = 1;
  wpe_ai_trace("checkpoint git ref=%s dir=%s", ck_ref, ck_dir);
  return 0;
 }
 free(out);

 /* snapshot copies */
 {
  const char *home = getenv("HOME");
  time_t now = time(NULL);
  if (!home) home = ".";
  snprintf(ck_snapdir, sizeof ck_snapdir, "%s/.xwpe/checkpoints/%ld", home, (long)now);
  ws_shq(q, sizeof q, ck_snapdir);
  snprintf(cmd, sizeof cmd, "mkdir -p %s", q);
  out = wpe_ai_run_capture(cmd); free(out);
  for (i = 0; i < nscope; i++) {
   char *text = ws_read_file(scope[i], 4000000), dst[1200];
   if (!text) continue;
   snprintf(dst, sizeof dst, "%s/%d", ck_snapdir, ck_n);
   if (ws_write_file(dst, text) == 0) {
    int cap = ck_n;
    ws_list_push(&ck_files, &ck_n, &cap, ws_strdup(scope[i]));
   }
   free(text);
  }
  /* remember what exists now, so files the run CREATES are detected too */
  ws_watch_dirs(scope, nscope);
  {
   int cap = 0, k;
   wpe_ai_free_list(ck_before, ck_before_n);
   ck_before = NULL; ck_before_n = 0;
   for (k = 0; k < ck_dirs_n; k++) ws_list_dir_files(ck_dirs[k], &ck_before, &ck_before_n, &cap);
  }
 }
 ck_mode = 2;
 wpe_ai_trace("checkpoint snapshot n=%d dir=%s", ck_n, ck_snapdir);
 return 0;
}

/* ===================== changeset ========================================= */

typedef struct { char *path; int line; char *text; int is_new; } ws_change;

/* Collapse runs of '/' to a single '/' in place, so paths built from a scope
   list (which may carry a "dir//name" from a trailing-slash join) compare equal
   to the plainly-spelled ones -- otherwise the same file shows up twice in the
   changeset and reverts match only one spelling. */
static void ws_path_norm(char *p)
{
 char *r = p, *w = p;
 int prev_slash = 0;
 for (; *r; r++) {
  if (*r == '/' && prev_slash) continue;
  *w++ = *r;
  prev_slash = (*r == '/');
 }
 *w = '\0';
}

static void ws_add_change(ws_change **c, int *n, int *cap, const char *path,
                          int line, const char *text, int is_new)
{
 char *np = ws_strdup(path);
 int i;
 if (np) ws_path_norm(np);
 for (i = 0; i < *n; i++)                       /* drop a duplicate for the same spot */
  if ((*c)[i].line == line && np && (*c)[i].path && !strcmp((*c)[i].path, np))
   { free(np); return; }
 if (*n == *cap) { *cap = *cap ? *cap * 2 : 16; *c = realloc(*c, (size_t)(*cap) * sizeof **c); }
 (*c)[*n].path = np ? np : ws_strdup(path);
 (*c)[*n].line = line;
 (*c)[*n].text = ws_strdup(text);
 (*c)[*n].is_new = is_new;
 (*n)++;
}

/* Parse `git diff` output: one change per hunk, at the new-file start line. */
static void ws_changes_from_gitdiff(const char *diff, const char *path,
                                    ws_change **c, int *n, int *cap)
{
 const char *p = diff;
 int start = -1, plus = 0, minus = 0;
 while (p && *p) {
  const char *nl = strchr(p, '\n');
  size_t l = nl ? (size_t)(nl - p) : strlen(p);
  if (l >= 2 && p[0] == '@' && p[1] == '@') {
   const char *pl;
   char text[64];
   if (start >= 0) {
    snprintf(text, sizeof text, "[AI] +%d -%d", plus, minus);
    ws_add_change(c, n, cap, path, start, text, 0);
   }
   pl = strchr(p, '+');
   start = pl ? atoi(pl + 1) : 1;
   if (start < 1) start = 1;
   plus = minus = 0;
  } else if (l >= 1 && start >= 0) {
   if (p[0] == '+' && !(l >= 3 && p[1] == '+' && p[2] == '+')) plus++;
   else if (p[0] == '-' && !(l >= 3 && p[1] == '-' && p[2] == '-')) minus++;
  }
  if (!nl) break;
  p = nl + 1;
 }
 if (start >= 0) {
  char text[64];
  snprintf(text, sizeof text, "[AI] +%d -%d", plus, minus);
  ws_add_change(c, n, cap, path, start, text, 0);
 }
}

/* xwpe's own state (.xwpe/, the session sidecar) and any other dot-path are
 * not part of a reviewable changeset. */
static int ws_hidden_path(const char *rel)
{
 const char *p = rel;
 while (p && *p) {
  if (*p == '.' && (p == rel || p[-1] == '/')) return 1;
  p = strchr(p, '/');
  if (p) p++;
 }
 return 0;
}

static int ws_collect_changes(ws_change **c, int *n)
{
 int cap = 0;
 *c = NULL; *n = 0;
 if (ck_mode == 1) {
  char q[1100], cmd[1400], *out, *line;
  ws_shq(q, sizeof q, ck_dir);
  snprintf(cmd, sizeof cmd, "cd %s && git diff --name-only %s 2>/dev/null", q, ck_ref);
  out = wpe_ai_run_capture(cmd);
  line = out;
  while (line && *line) {
   char *nl = strchr(line, '\n'), qf[1100], full[1200], *d;
   if (nl) *nl = '\0';
   if (*line && !ws_hidden_path(line)) {
    snprintf(full, sizeof full, "%s/%s", ck_dir, line);
    ws_shq(qf, sizeof qf, line);
    snprintf(cmd, sizeof cmd, "cd %s && git diff %s -- %s 2>/dev/null", q, ck_ref, qf);
    d = wpe_ai_run_capture(cmd);
    ws_changes_from_gitdiff(d ? d : "", full, c, n, &cap);
    free(d);
   }
   if (!nl) break;
   line = nl + 1;
  }
  free(out);
  snprintf(cmd, sizeof cmd, "cd %s && git status --porcelain --untracked-files=all 2>/dev/null", q);
  out = wpe_ai_run_capture(cmd);
  line = out;
  while (line && *line) {
   char *nl = strchr(line, '\n');
   if (nl) *nl = '\0';
   if (!strncmp(line, "?? ", 3) && !ws_hidden_path(line + 3)) {
    char full[1200];
    snprintf(full, sizeof full, "%s/%s", ck_dir, line + 3);
    ws_add_change(c, n, &cap, full, 1, "[AI] new file", 1);
   }
   if (!nl) break;
   line = nl + 1;
  }
  free(out);
  return *n;
 }
 if (ck_mode == 2) {
  int i;
  for (i = 0; i < ck_n; i++) {
   char snap[1200];
   char *old, *now;
   wpe_ai_seg *segs;
   int ns, k, newline = 1;
   snprintf(snap, sizeof snap, "%s/%d", ck_snapdir, i);
   old = ws_read_file(snap, 4000000);
   now = ws_read_file(ck_files[i], 4000000);
   if (!old || !now) { free(old); free(now); continue; }
   ns = wpe_ai_diff_segments(old, now, &segs);
   for (k = 0; k < ns; k++) {
    if (segs[k].is_change) {
     char text[64];
     snprintf(text, sizeof text, "[AI] +%d -%d", segs[k].bn, segs[k].an);
     ws_add_change(c, n, &cap, ck_files[i], newline, text, 0);
     newline += segs[k].bn;
    } else newline += segs[k].an;
   }
   wpe_ai_segs_free(segs, ns);
   free(old); free(now);
  }
  /* files that did not exist at checkpoint time = new files */
  {
   char **nowl = NULL;
   int nn = 0, ncap = 0, k;
   for (k = 0; k < ck_dirs_n; k++) ws_list_dir_files(ck_dirs[k], &nowl, &nn, &ncap);
   for (k = 0; k < nn; k++)
    if (!ws_list_has(ck_before, ck_before_n, nowl[k]))
     ws_add_change(c, n, &cap, nowl[k], 1, "[AI] new file", 1);
   wpe_ai_free_list(nowl, nn);
  }
  return *n;
 }
 return 0;
}

static void ws_free_changes(ws_change *c, int n)
{
 int i;
 for (i = 0; i < n; i++) { free(c[i].path); free(c[i].text); }
 free(c);
}

/* Reload an open window from disk (after a revert) with one undo snapshot. */
static void ws_reload_window(FENSTER *f, const char *path)
{
 FENSTER *w = ws_find_window(f, path);
 char *text;
 if (!w) return;
 text = ws_read_file(path, 4000000);
 if (!text) return;
 e_add_undo('B', w->b, w->b->b.x, w->b->b.y, 0);
 e_buffer_set_text(w->b, text);
 if (w->b->b.y >= w->b->mxlines) w->b->b.y = w->b->mxlines ? w->b->mxlines - 1 : 0;
 w->save++;
 e_firstl(w, 1);
 e_schirm(w, 1);
 e_rep_win_tree(w->ed);
 e_refresh();
 free(text);
}

/* wpe_ai_reload_open_window - If `path` is open in a window, reload its buffer
   from disk (one undo step).  Called after the agent's write_file so an edit to
   the file the user is looking at shows immediately, instead of leaving a stale
   buffer on screen while disk has the new content.  A no-op if the file is not
   open. */
void wpe_ai_reload_open_window(FENSTER *f, const char *path)
{
 /* ws_find_window compares against the window's ABSOLUTE path, but a tool may
    have written a path relative to the editor's working directory -- resolve it
    so a relative write still finds and refreshes the open window. */
 if (path && path[0] != '/') {
  char cwd[1024], abs[1200];
  if (getcwd(cwd, sizeof cwd)) {
   snprintf(abs, sizeof abs, "%s/%s", cwd, path);
   ws_path_norm(abs);
   ws_reload_window(f, abs);
   return;
  }
 }
 ws_reload_window(f, path);
}

static void ws_revert_path(FENSTER *f, const char *path, int is_new)
{
 if (ck_mode == 1) {
  char q[1100], qf[1100], cmd[1400], *out;
  const char *rel = path;
  size_t dl = strlen(ck_dir);
  if (!strncmp(path, ck_dir, dl) && path[dl] == '/') rel = path + dl + 1;
  ws_shq(q, sizeof q, ck_dir);
  ws_shq(qf, sizeof qf, rel);
  if (is_new) snprintf(cmd, sizeof cmd, "cd %s && rm -f -- %s", q, qf);
  else        snprintf(cmd, sizeof cmd, "cd %s && git checkout %s -- %s 2>&1", q, ck_ref, qf);
  out = wpe_ai_run_capture(cmd); free(out);
 } else if (ck_mode == 2) {
  int i;
  if (is_new) { unlink(path); wpe_ai_trace("changeset revert %s", path); return; }
  for (i = 0; i < ck_n; i++) if (!strcmp(ck_files[i], path)) {
   char snap[1200], *text;
   snprintf(snap, sizeof snap, "%s/%d", ck_snapdir, i);
   text = ws_read_file(snap, 4000000);
   if (text) { ws_write_file(path, text); free(text); }
   break;
  }
 }
 if (!is_new) ws_reload_window(f, path);
 wpe_ai_trace("changeset revert %s", path);
}

int wpe_ai_changeset_review(FENSTER *f)
{
 ws_change *c = NULL;
 int n = 0, i, kept = 0;
 char line[1400];
 char **files; int *lines; char **texts;

 if (!ck_mode) { e_d_p_message("[AI] no checkpoint - nothing to review", f, 1); return 0; }
 ws_collect_changes(&c, &n);
 if (n == 0) {
  e_d_p_message("=== AI changeset: no files changed ===", f, 1);
  ws_free_changes(c, n);
  wpe_ai_trace("changeset empty");
  return 0;
 }

 snprintf(line, sizeof line, "=== AI changeset: %d change%s (Alt-T / Alt-V to step) ===", n, n == 1 ? "" : "s");
 e_d_p_message(line, f, 1);
 files = malloc((size_t)n * sizeof *files);
 lines = malloc((size_t)n * sizeof *lines);
 texts = malloc((size_t)n * sizeof *texts);
 for (i = 0; i < n; i++) {
  snprintf(line, sizeof line, "%s:%d: %s", c[i].path, c[i].line, c[i].text);
  e_d_p_message(line, f, 0);
  files[i] = c[i].path; lines[i] = c[i].line; texts[i] = c[i].text;
 }
 e_set_error_list(n, files, lines, NULL, texts);   /* Alt-T / Alt-V now step these */
 free(files); free(lines); free(texts);
 wpe_ai_trace("changeset n=%d", n);

 e_d_p_message("   a = keep all   r = REVERT all   f = file by file   c = commit (git)", f, 0);
 for (;;) {
  int k = e_toupper(e_getch());
  if (k == 'A' || k == WPE_ESC || k == 13 || k == '\r' || k == '\n') {
   kept = n;
   e_d_p_message("[AI] changes kept", f, 0);
   wpe_ai_trace("changeset keep all");
   break;
  }
  if (k == 'R') {
   for (i = 0; i < n; i++) ws_revert_path(f, c[i].path, c[i].is_new);
   e_d_p_message("[AI] ALL changes reverted to the checkpoint", f, 0);
   wpe_ai_trace("changeset revert all");
   break;
  }
  if (k == 'F') {
   char **seen = NULL; int ns = 0, sc = 0;
   for (i = 0; i < n; i++) {
    if (ws_list_has(seen, ns, c[i].path)) continue;
    ws_list_push(&seen, &ns, &sc, ws_strdup(c[i].path));
    snprintf(line, sizeof line, "keep %s ?  y = keep  n = revert", c[i].path);
    e_d_p_message(line, f, 0);
    for (;;) {
     int kk = e_toupper(e_getch());
     if (kk == 'Y' || kk == 13 || kk == '\r' || kk == '\n') { kept++; break; }
     if (kk == 'N') { ws_revert_path(f, c[i].path, c[i].is_new); break; }
    }
   }
   wpe_ai_free_list(seen, ns);
   e_d_p_message("[AI] file-by-file review done", f, 0);
   break;
  }
  if (k == 'C' && ck_mode == 1) {
   static char msg[256];
   char q[1100], qm[600], cmd[1900], *out;
   strcpy(msg, "AI-assisted change");
   if (e_add_arguments(msg, "Commit message", f, 0, AltB, NULL) && msg[0]) {
    ws_shq(q, sizeof q, ck_dir);
    ws_shq(qm, sizeof qm, msg);
    snprintf(cmd, sizeof cmd, "cd %s && git add -A && git commit -q -m %s 2>&1", q, qm);
    out = wpe_ai_run_capture(cmd);
    e_d_p_message(out && *out ? out : "[AI] committed", f, 0);
    free(out);
    wpe_ai_trace("changeset commit");
   }
   kept = n;
   break;
  }
 }
 ws_free_changes(c, n);
 return kept;
}

/* ===================== sessions ========================================== */

static struct {
 char  *cli;                 /* claudecli session id                         */
 char **role, **content;
 int    n;
} sess;

#define WS_SESS_MAX 40

static void sess_free(void)
{
 int i;
 free(sess.cli); sess.cli = NULL;
 for (i = 0; i < sess.n; i++) { free(sess.role[i]); free(sess.content[i]); }
 free(sess.role); free(sess.content);
 sess.role = sess.content = NULL; sess.n = 0;
}

static unsigned long ws_hash(const char *s)
{
 unsigned long h = 5381;
 while (s && *s) h = h * 33 + (unsigned char)*s++;
 return h;
}

static void ws_session_path(FENSTER *f, char *buf, size_t sz)
{
 if (e_project_is_open()) {
  char *root = ws_root(f);
  snprintf(buf, sz, "%s/.xwpe-ai-session", root);
  free(root);
 } else {
  const char *home = getenv("HOME");
  char q[1100], cmd[1300], *out;
  if (!home) home = ".";
  snprintf(buf, sz, "%s/.xwpe/ai", home);
  ws_shq(q, sizeof q, buf);
  snprintf(cmd, sizeof cmd, "mkdir -p %s", q);
  out = wpe_ai_run_capture(cmd); free(out);
  snprintf(buf, sz, "%s/.xwpe/ai/%lu.session", home, ws_hash(f->dirct ? f->dirct : "."));
 }
}

void wpe_ai_session_append(const char *role, const char *content)
{
 if (sess.n == WS_SESS_MAX) {                     /* drop the oldest turn */
  free(sess.role[0]); free(sess.content[0]);
  memmove(sess.role, sess.role + 1, (size_t)(sess.n - 1) * sizeof *sess.role);
  memmove(sess.content, sess.content + 1, (size_t)(sess.n - 1) * sizeof *sess.content);
  sess.n--;
 }
 sess.role = realloc(sess.role, (size_t)(sess.n + 1) * sizeof *sess.role);
 sess.content = realloc(sess.content, (size_t)(sess.n + 1) * sizeof *sess.content);
 sess.role[sess.n] = ws_strdup(role);
 sess.content[sess.n] = ws_strdup(content);
 sess.n++;
}

int wpe_ai_session_messages(wpe_ai_msg *buf, int max)
{
 int start = sess.n > max ? sess.n - max : 0, i, k = 0;
 for (i = start; i < sess.n; i++) { buf[k].role = sess.role[i]; buf[k].content = sess.content[i]; k++; }
 return k;
}

void wpe_ai_session_load(FENSTER *f)
{
 char path[1300], *text;
 struct json_object *o, *v, *arr;
 sess_free();
 free(e_ai_resume_session); e_ai_resume_session = NULL;
 ws_session_path(f, path, sizeof path);
 text = ws_read_file(path, 2000000);
 if (!text) return;
 o = json_tokener_parse(text);
 free(text);
 if (!o) return;
 if (json_object_object_get_ex(o, "cli_session", &v)) {
  const char *s = json_object_get_string(v);
  if (s && *s) { sess.cli = ws_strdup(s); e_ai_resume_session = ws_strdup(s); }
 }
 if (json_object_object_get_ex(o, "messages", &arr)) {
  int L = json_object_array_length(arr), i;
  for (i = 0; i < L; i++) {
   struct json_object *m = json_object_array_get_idx(arr, i), *r, *c;
   if (json_object_object_get_ex(m, "role", &r) && json_object_object_get_ex(m, "content", &c))
    wpe_ai_session_append(json_object_get_string(r), json_object_get_string(c));
  }
 }
 json_object_put(o);
 wpe_ai_trace("session load %s cli=%s n=%d", path, sess.cli ? sess.cli : "-", sess.n);
}

void wpe_ai_session_save(FENSTER *f)
{
 char path[1300];
 struct json_object *o = json_object_new_object(), *arr = json_object_new_array();
 int i;
 if (e_ai_last_session_id && *e_ai_last_session_id) {
  free(sess.cli); sess.cli = ws_strdup(e_ai_last_session_id);
 }
 json_object_object_add(o, "cli_session", json_object_new_string(sess.cli ? sess.cli : ""));
 for (i = 0; i < sess.n; i++) {
  struct json_object *m = json_object_new_object();
  json_object_object_add(m, "role", json_object_new_string(sess.role[i]));
  json_object_object_add(m, "content", json_object_new_string(sess.content[i]));
  json_object_array_add(arr, m);
 }
 json_object_object_add(o, "messages", arr);
 ws_session_path(f, path, sizeof path);
 ws_write_file(path, json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN));
 json_object_put(o);
 wpe_ai_trace("session save %s cli=%s n=%d", path, sess.cli ? sess.cli : "-", sess.n);
}

void wpe_ai_session_reset(FENSTER *f)
{
 char path[1300];
 sess_free();
 free(e_ai_resume_session); e_ai_resume_session = NULL;
 free(e_ai_last_session_id); e_ai_last_session_id = NULL;
 ws_session_path(f, path, sizeof path);
 unlink(path);
 wpe_ai_trace("session reset");
}

#endif /* WPE_AI */

typedef int wpe_ai_ws_translation_unit;
