/* we_features.h -- what this build contains, said once, from config.h.

   configure defines one WPE_BUILT_* fact per option or library it decides
   (always 0 or 1); this table names each with the configure option's own
   word, so a reader that sees "with-wayland=no" can say "not built here:
   --with-wayland" with no table of its own. */
#ifndef WE_FEATURES_H
#define WE_FEATURES_H

#include <stddef.h>

typedef struct
{
 const char *word;    /* the configure option without its dashes, or the library's name */
 int built;           /* 1 when this build has it */
} WpeFeature;

/**
 * wpe_features - The table of what configure was asked for and found.
 * @n: receives the number of entries.
 *
 * One entry per WPE_BUILT_* fact, in the order configure prints them.  For
 * --version, and for anything that must say which build this is.
 *
 * Return: the table.
 */
const WpeFeature *wpe_features(size_t *n);

/**
 * wpe_features_line - "built with: word=yes word=no ...", on one line.
 *
 * What `wpe --version` prints after the version.  A test suite or a bug
 * report reads it instead of guessing from strings in the binary, so it
 * reads the same on an in-tree, an installed and a sanitizer build.
 *
 * Return: a string owned by this file, valid for the life of the process.
 */
const char *wpe_features_line(void);

#endif /* WE_FEATURES_H */
