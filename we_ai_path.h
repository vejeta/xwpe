/* we_ai_path.h -- where a path really lands, for the assistant's write policy.
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

int wpe_path_resolve(const char *path, char *out, size_t sz);
int wpe_path_lands_outside(const char *root, const char *path, char *real, size_t sz);

#endif
