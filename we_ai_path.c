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
#include <errno.h>
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
 * wpe_path_lands_outside - Will writing this path put bytes outside the root?
 * @root: the workspace's directory, absolute; @path: the file, absolute, as
 * the model named it; @real/@sz: receive where the file really lands, empty
 * when it could not be told; may be NULL.
 *
 * What the user gains: a link inside the project that leads elsewhere no
 * longer hides a write.  Both sides are followed (a project reached through
 * a linked home directory still compares like with like), and a path that
 * cannot be followed, a loop, counts as outside: the safe answer is to ask.
 *
 * Return: 1 when the file lands outside, 0 when inside or when it cannot be
 * told (no root, a root that cannot be followed, or the root "/", below which
 * nothing is outside).
 */
int wpe_path_lands_outside(const char *root, const char *path, char *real, size_t sz)
{
 char rroot[WPE_PATH_RESOLVED_MAX], rpath[WPE_PATH_RESOLVED_MAX];
 size_t rl;
 if (real && sz) real[0] = '\0';
 if (!root || !path) return 0;
 if (wpe_path_resolve(root, rroot, sizeof rroot) != WPE_PATH_RESOLVED) return 0;
 rl = strlen(rroot);
 if (rl <= 1) return 0;
 if (wpe_path_resolve(path, rpath, sizeof rpath) != WPE_PATH_RESOLVED) return 1;
 if (real && strlen(rpath) < sz) memcpy(real, rpath, strlen(rpath) + 1);
 if (strncmp(rpath, rroot, rl) != 0) return 1;
 if (rpath[rl] != '/' && rpath[rl] != '\0') return 1;
 return 0;
}
