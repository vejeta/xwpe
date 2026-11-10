/* we_ai_path.h -- where a path really lands, for the assistant's scope.
 *
 * Pure C, no display header: the C tests link we_ai_path.c alone.
 */
#ifndef WE_AI_PATH_H
#define WE_AI_PATH_H

#include <stddef.h>

/* Room for a resolved path; a path that does not fit is "outside". */
#define WPE_PATH_RESOLVED_MAX 4096

/* How many symbolic links one path may pass through before it counts as a
   loop; the same bound the kernel uses. */
#define WPE_PATH_MAX_HOPS 40

/* wpe_path_resolve results. */
#define WPE_PATH_RESOLVED     0
#define WPE_PATH_UNRESOLVABLE (-1)   /* a loop, too long, or not absolute */

/* What a tool does with a path: the scope is the same for both, but only a
   read can be widened by a grant. */
#define WPE_SCOPE_READ  0
#define WPE_SCOPE_WRITE 1

/* Where wpe_scope_classify finds a path. */
#define WPE_SCOPE_INSIDE  0   /* under the workspace root                       */
#define WPE_SCOPE_GRANTED 1   /* outside it, under a directory granted for reads */
#define WPE_SCOPE_OUTSIDE 2   /* anywhere else, or a path that cannot be told   */

/* The most entries one look for links that lead out may visit. */
#define WPE_SCAN_ENTRY_MAX 20000

/* How deep a look for links that lead out goes: a folder's own entries, or
   every level below it. */
#define WPE_SCAN_ONE_LEVEL  1
#define WPE_SCAN_ALL_LEVELS (-1)

int wpe_path_resolve(const char *path, char *out, size_t sz);
int wpe_scope_classify(const char *root, const char *const *grants, int ngrants,
                       const char *path, int access, char *real, size_t sz);
int wpe_path_lands_outside(const char *root, const char *path, char *real, size_t sz);
int wpe_path_open_write(const char *path);
int wpe_path_count_outward_links(const char *start, int max_depth, int *truncated);

#endif
