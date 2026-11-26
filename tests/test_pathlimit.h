/* test_pathlimit.h -- how long a path the system a test runs on takes.
 *
 * A row that builds "a folder near the limit" has to build one the system it
 * runs on really takes.  The limit is the system's, not the row's: Linux says
 * 4096 bytes, macOS and the BSDs 1024, and a folder deep enough for the first
 * is refused by the second with ENAMETOOLONG.  pathconf answers for the folder
 * the test works in; where the system has no answer the editor's own number
 * (WPE_PLATFORM_PATH_MAX, we_path.h) stands in, and the caller's buffer caps
 * it, so a row never writes past what it holds.
 *
 * Static inline, libc and we_path.h only, like test_watchdog.h.
 */
#ifndef TEST_PATHLIMIT_H
#define TEST_PATHLIMIT_H

#include <string.h>
#include <unistd.h>

#include "../we_path.h"

/** test_path_limit - The longest path this system takes under @under, the
    terminator included.  @under is an existing folder to ask, the one the
    test works in; @room is the bytes the caller's own path buffers hold, so
    the answer never runs past them. */
static inline size_t test_path_limit(const char *under, size_t room)
{
 long limit = pathconf(under, _PC_PATH_MAX);

 if (limit < 0)
  limit = WPE_PLATFORM_PATH_MAX;
 if ((size_t)limit > room)
  limit = (long)room;
 return (size_t)limit;
}

/** test_levels_that_fit - How many levels of names of @name_bytes a folder
    can have under @under and keep @headroom bytes to spare.  @limit is the
    system's, from test_path_limit; @headroom is what the row still puts
    below the last level, a file name and its own text.  A folder that leaves
    no room at all takes no level, which reads as 0 and never as a count that
    wrapped round. */
static inline int test_levels_that_fit(size_t limit, const char *under,
                                       size_t name_bytes, size_t headroom)
{
 size_t taken = headroom + strlen(under);

 if (taken >= limit)
  return 0;
 return (int)((limit - taken) / (name_bytes + 1));
}

#endif /* TEST_PATHLIMIT_H */
