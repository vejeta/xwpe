/* we_ai_path.c -- where a path really lands, for the assistant's write policy.
 *
 * The workspace check used to compare path text: "ws/vendor/x.c" counted as
 * inside however "ws/vendor" was made.  A symbolic link inside the workspace
 * that leads outside it (a vendored directory, a link the model itself made
 * with a shell command) therefore let an unattended run write beyond the tree
 * without the box that every outside write gets.  Here a path is followed the
 * way the kernel will follow it when the file is opened, so the question is
 * asked of the place the bytes will really go.
 *
 * Pure C, no display header: the C tests link this file alone.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "we_ai_path.h"

/* The walk's state: what is resolved so far, what is left to read, and how
   many links were followed.  `missing` is set from the first component that
   does not exist: nothing below it can be a link, so the rest is appended as
   written (a new file under a linked directory is judged by the directory). */
typedef struct {
 char out[WPE_PATH_RESOLVED_MAX];    /* the resolved prefix, "/" at the start */
 char rest[WPE_PATH_RESOLVED_MAX];   /* the text still to read                */
 size_t pos;                         /* where in `rest` the next component is */
 int hops;
 int missing;
} wpe_path_walk;

/**
 * path_next_component - Read the next component of the walk's remaining text.
 * @w: the walk; @comp/@sz: receive the component.
 *
 * Return: its length, 0 when nothing is left, -1 when it does not fit.
 */
static int path_next_component(wpe_path_walk *w, char *comp, size_t sz)
{
 size_t n = 0;
 while (w->rest[w->pos] == '/') w->pos++;
 while (w->rest[w->pos + n] && w->rest[w->pos + n] != '/') n++;
 if (n >= sz) return -1;
 memcpy(comp, w->rest + w->pos, n);
 comp[n] = '\0';
 w->pos += n;
 return (int)n;
}

/* Drop the last component of the resolved prefix; the root stays the root. */
static void path_pop(char *out)
{
 char *slash = strrchr(out, '/');
 if (!slash || slash == out) { out[0] = '/'; out[1] = '\0'; return; }
 *slash = '\0';
}

/* Put "<dir>/<name>" in @dst; -1 when it does not fit. */
static int path_join(char *dst, size_t sz, const char *dir, const char *name)
{
 int n;
 if (!strcmp(dir, "/")) n = snprintf(dst, sz, "/%s", name);
 else n = snprintf(dst, sz, "%s/%s", dir, name);
 if (n < 0 || (size_t)n >= sz) return -1;
 return 0;
}

/**
 * path_follow_link - Replace a link's name in the walk by what it points to.
 * @w: the walk; @link: the full path of a name lstat(2) just saw as a link.
 *
 * An absolute target restarts from the root, a relative one continues from
 * the directory that holds the link; either way the target's own components
 * are read next, ahead of whatever followed the link.  A link that cannot be
 * read (swapped between the look and the read) stops the walk: taking it as a
 * plain name would let the kernel follow it later where this walk did not,
 * and the answer would no longer be where the bytes go.
 * Return: 0 when the target was spliced in, -1 when the walk cannot go on (too
 * many links, a link that cannot be read, no room).
 */
static int path_follow_link(wpe_path_walk *w, const char *link)
{
 char target[WPE_PATH_RESOLVED_MAX], joined[WPE_PATH_RESOLVED_MAX];
 ssize_t n;
 int len;
 if (++w->hops > WPE_PATH_MAX_HOPS) return -1;
 n = readlink(link, target, sizeof target - 1);
 if (n <= 0) return -1;
 target[n] = '\0';
 if (target[0] == '/') { w->out[0] = '/'; w->out[1] = '\0'; }
 len = snprintf(joined, sizeof joined, "%s/%s", target, w->rest + w->pos);
 if (len < 0 || (size_t)len >= sizeof joined) return -1;
 memcpy(w->rest, joined, (size_t)len + 1);
 w->pos = 0;
 return 0;
}

/**
 * path_step - Take one component: a dot, a ".." or a name, and follow it.
 * @w: the walk; @comp: the component.
 *
 * Return: 0 to go on, -1 when the walk cannot.
 */
static int path_step(wpe_path_walk *w, const char *comp)
{
 char cand[WPE_PATH_RESOLVED_MAX];
 struct stat st;
 if (!strcmp(comp, ".")) return 0;
 if (!strcmp(comp, "..")) {
  path_pop(w->out);
  w->missing = 0;                   /* back on a place that exists */
  return 0;
 }
 if (path_join(cand, sizeof cand, w->out, comp) < 0) return -1;
 if (!w->missing && lstat(cand, &st) != 0) w->missing = 1;
 if (!w->missing && S_ISLNK(st.st_mode)) return path_follow_link(w, cand);
 memcpy(w->out, cand, strlen(cand) + 1);
 return 0;
}

/**
 * wpe_path_resolve - Follow a path to where it really lands, file or not.
 * @path: an absolute path, as the model wrote it; @out/@sz: the result.
 *
 * Every symbolic link on the way is followed, the last component's included,
 * so a link to a file that does not exist yet is judged by where that file
 * would be made.  Dots and ".." are folded as the kernel folds them, after
 * the links before them.  The part of the path that does not exist (a new
 * file, a new directory) is kept as written below the deepest part that does.
 * Unlike realpath(3) it does not fail on a missing file, which is the common
 * case for a write.
 *
 * Return: WPE_PATH_RESOLVED, or WPE_PATH_UNRESOLVABLE for a link loop, a link
 * that cannot be read, a path that does not fit WPE_PATH_RESOLVED_MAX, or a
 * path that is not absolute.
 */
int wpe_path_resolve(const char *path, char *out, size_t sz)
{
 wpe_path_walk w;
 char comp[WPE_PATH_RESOLVED_MAX];
 int n;
 if (!path || path[0] != '/' || strlen(path) >= sizeof w.rest) return WPE_PATH_UNRESOLVABLE;
 memcpy(w.rest, path, strlen(path) + 1);
 w.out[0] = '/'; w.out[1] = '\0';
 w.pos = 0; w.hops = 0; w.missing = 0;
 while ((n = path_next_component(&w, comp, sizeof comp)) != 0) {
  if (n < 0 || path_step(&w, comp) < 0) return WPE_PATH_UNRESOLVABLE;
 }
 if (strlen(w.out) >= sz) return WPE_PATH_UNRESOLVABLE;
 memcpy(out, w.out, strlen(w.out) + 1);
 return WPE_PATH_RESOLVED;
}

/**
 * path_is_under - Is a resolved path the directory itself or something below it?
 * @rdir: a resolved directory; @rpath: a resolved path.
 *
 * The one test of "inside" every scope question goes through, so the
 * workspace, a grant and a walk's start cannot disagree about it.  The
 * directory's name must end at a component: "/ws2/x" is not under "/ws".
 * The root "/" holds everything.
 * Return: 1 when @rpath is @rdir or below it.
 */
static int path_is_under(const char *rdir, const char *rpath)
{
 size_t n = strlen(rdir);
 if (n <= 1) return 1;
 if (strncmp(rpath, rdir, n) != 0) return 0;
 return rpath[n] == '/' || rpath[n] == '\0';
}

/**
 * path_under_a_grant - Is a resolved path below any of the granted directories?
 * @grants/@ngrants: the directories as written; @rpath: a resolved path.
 *
 * A grant is followed like every other path, so a grant given through a link
 * covers the place the link leads and nothing else.  One that cannot be
 * followed grants nothing, and neither does the root "/": it holds every
 * file, so honouring it would switch the scope off instead of widening it.
 * Return: 1 when some grant holds the path.
 */
static int path_under_a_grant(const char *const *grants, int ngrants, const char *rpath)
{
 char rgrant[WPE_PATH_RESOLVED_MAX];
 int i;
 for (i = 0; i < ngrants; i++) {
  if (!grants[i]) continue;
  if (wpe_path_resolve(grants[i], rgrant, sizeof rgrant) != WPE_PATH_RESOLVED) continue;
  if (!strcmp(rgrant, "/")) continue;
  if (path_is_under(rgrant, rpath)) return 1;
 }
 return 0;
}

/**
 * wpe_scope_classify - Where does this path land: in the workspace, in a grant, or beyond?
 * @root: the workspace's directory, absolute; @grants/@ngrants: directories the
 * user allowed the assistant to read, absolute; @path: the file, absolute, as
 * the model named it; @access: WPE_SCOPE_READ or WPE_SCOPE_WRITE;
 * @real/@sz: receive where the file really lands, empty when it could not be
 * told; may be NULL.
 *
 * What the user gains: one answer for every tool, native or hosted.  A link
 * inside the project that leads elsewhere no longer hides a write or a read.
 * Both sides are followed (a project reached through a linked home directory
 * still compares like with like), and a path that cannot be followed, a loop,
 * counts as outside: the safe answer is to ask.  A grant widens reads only: a
 * write beyond the workspace stays outside whatever was granted.
 *
 * Return: WPE_SCOPE_INSIDE, WPE_SCOPE_GRANTED or WPE_SCOPE_OUTSIDE.  A path
 * is inside when it cannot be told (no root, a root that cannot be followed,
 * or the root "/", below which nothing is outside).
 */
int wpe_scope_classify(const char *root, const char *const *grants, int ngrants,
                       const char *path, int access, char *real, size_t sz)
{
 char rroot[WPE_PATH_RESOLVED_MAX], rpath[WPE_PATH_RESOLVED_MAX];
 if (real && sz) real[0] = '\0';
 if (!root || !path) return WPE_SCOPE_INSIDE;
 if (wpe_path_resolve(root, rroot, sizeof rroot) != WPE_PATH_RESOLVED) return WPE_SCOPE_INSIDE;
 if (strlen(rroot) <= 1) return WPE_SCOPE_INSIDE;
 if (wpe_path_resolve(path, rpath, sizeof rpath) != WPE_PATH_RESOLVED) return WPE_SCOPE_OUTSIDE;
 if (real && strlen(rpath) < sz) memcpy(real, rpath, strlen(rpath) + 1);
 if (path_is_under(rroot, rpath)) return WPE_SCOPE_INSIDE;
 if (access == WPE_SCOPE_READ && path_under_a_grant(grants, ngrants, rpath))
  return WPE_SCOPE_GRANTED;
 return WPE_SCOPE_OUTSIDE;
}

/**
 * wpe_path_lands_outside - Will writing this path put bytes outside the root?
 * @root: the workspace's directory, absolute; @path: the file, absolute, as
 * the model named it; @real/@sz: as for wpe_scope_classify.
 *
 * The write side of wpe_scope_classify, for callers that have no grants.
 * Return: 1 when the file lands outside, 0 when inside or when it cannot be told.
 */
int wpe_path_lands_outside(const char *root, const char *path, char *real, size_t sz)
{
 int where = wpe_scope_classify(root, NULL, 0, path, WPE_SCOPE_WRITE, real, sz);
 return where == WPE_SCOPE_OUTSIDE;
}

/**
 * wpe_path_open_write - Open a file for writing at the place the check judged.
 * @path: the file, as the model named it (relative to the working directory
 * or absolute).
 *
 * What the user gains: the write goes where the scope check looked.  The path
 * is followed first, exactly as the check followed it, and the place it leads
 * to is opened without following one more link.  A link put in the last place
 * between the check and the open makes the open fail instead of sending the
 * bytes somewhere the user was never shown.  The file is created when absent
 * and emptied when present.
 *
 * Return: the descriptor, or -1 with errno set (ELOOP for a path that cannot
 * be followed or a link made after the check).
 */
int wpe_path_open_write(const char *path)
{
 char abs[WPE_PATH_RESOLVED_MAX], real[WPE_PATH_RESOLVED_MAX];
 int n;
 if (!path || !path[0]) { errno = ENOENT; return -1; }
 if (path[0] == '/') n = snprintf(abs, sizeof abs, "%s", path);
 else {
  char cwd[WPE_PATH_RESOLVED_MAX];
  if (!getcwd(cwd, sizeof cwd)) return -1;
  n = snprintf(abs, sizeof abs, "%s/%s", cwd, path);
 }
 if (n < 0 || (size_t)n >= sizeof abs) { errno = ENAMETOOLONG; return -1; }
 if (wpe_path_resolve(abs, real, sizeof real) != WPE_PATH_RESOLVED) { errno = ELOOP; return -1; }
 return open(real, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0666);
}

/* The state of one look for links that lead out of a directory. */
typedef struct {
 char rstart[WPE_PATH_RESOLVED_MAX];  /* the resolved directory the walk began in */
 int  max_depth;                      /* levels to enter; negative for no bound   */
 int  visited;                        /* entries seen so far                      */
 int  outward;                        /* links found that lead out                */
 int  truncated;                      /* the entry bound was reached              */
} wpe_path_scan;

static void path_scan_dir(wpe_path_scan *sc, const char *dir, int depth);

/**
 * path_scan_entry - Judge one entry of a directory: a link out, a directory to enter, or nothing.
 * @sc: the scan; @path: the entry's full path; @depth: the level of its directory.
 *
 * A link is never entered: it is counted when it leads outside the start and
 * left alone either way, which is what keeps a walk from reading beyond the
 * tree through it.
 */
static void path_scan_entry(wpe_path_scan *sc, const char *path, int depth)
{
 struct stat st;
 char real[WPE_PATH_RESOLVED_MAX];
 if (lstat(path, &st) != 0) return;
 if (S_ISLNK(st.st_mode)) {
  if (wpe_path_resolve(path, real, sizeof real) != WPE_PATH_RESOLVED ||
      !path_is_under(sc->rstart, real))
   sc->outward++;
  return;
 }
 if (S_ISDIR(st.st_mode) && (sc->max_depth < 0 || depth + 1 < sc->max_depth))
  path_scan_dir(sc, path, depth + 1);
}

/**
 * path_scan_dir - Visit the entries of one directory, within the scan's bound.
 * @sc: the scan; @dir: the directory; @depth: its level below the start.
 */
static void path_scan_dir(wpe_path_scan *sc, const char *dir, int depth)
{
 DIR *d = opendir(dir);
 struct dirent *de;
 char path[WPE_PATH_RESOLVED_MAX];
 if (!d) return;
 while ((de = readdir(d)) != NULL) {
  if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
  if (sc->visited >= WPE_SCAN_ENTRY_MAX) { sc->truncated = 1; break; }
  sc->visited++;
  if (snprintf(path, sizeof path, "%s/%s", dir, de->d_name) >= (int)sizeof path) continue;
  path_scan_entry(sc, path, depth);
 }
 closedir(d);
}

/**
 * wpe_path_count_outward_links - How many links below a directory lead out of it?
 * @start: the directory a search starts in, absolute; @max_depth: how many
 * levels to enter, 1 for the directory's own entries, negative for all;
 * @truncated: set to 1 when the scan stopped at WPE_SCAN_ENTRY_MAX entries,
 * else 0; may be NULL.
 *
 * What the user gains: a search that did not follow such a link says so, so
 * an empty result is not read as "nothing there".  Links are looked at, never
 * entered, so the count itself reads nothing beyond the tree.
 *
 * Return: the number of links whose target is outside @start or cannot be
 * followed.
 */
int wpe_path_count_outward_links(const char *start, int max_depth, int *truncated)
{
 wpe_path_scan sc;
 memset(&sc, 0, sizeof sc);
 if (truncated) *truncated = 0;
 if (!start || wpe_path_resolve(start, sc.rstart, sizeof sc.rstart) != WPE_PATH_RESOLVED)
  return 0;
 sc.max_depth = max_depth;
 path_scan_dir(&sc, sc.rstart, 0);
 if (truncated) *truncated = sc.truncated;
 return sc.outward;
}
