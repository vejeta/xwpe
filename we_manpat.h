/* we_manpat.h -- the file pattern of one manual section in one man folder.

   Help > Function Index lists the manual pages of a section by looking for
   "<man folder>/man<section>/*" in every folder of MANPATH.  The folder comes
   from the environment and the section from the caller, so the pattern is
   built here, through the path owner (we_path.h): it is whole or it is
   nothing, and never longer than its buffer.

   Plain C, one static inline over we_path.c, so a unit test builds it
   without the editor. */
#ifndef WE_MANPAT_H
#define WE_MANPAT_H

#include <stddef.h>

#include "we_path.h"

/**
 * wpe_man_pattern - Write "<folder>/man<section>/*" into a buffer, or nothing.
 * @out/@n: the buffer and its size; @folder: one folder of MANPATH, "" for
 * the root; @section: the section's name as typed or picked ("1", "3p").
 *
 * Lets Function Index search a MANPATH of any length for a section of any
 * length without running past its buffer; a pattern that does not fit names
 * no folder, so that folder is skipped and the listing carries on with the
 * next.
 * Return: 0, or -1 with @out empty when the pattern does not fit.
 */
static inline int wpe_man_pattern(char *out, size_t n, const char *folder, const char *section)
{
 if (wpe_path_copy(out, n, folder ? folder : "") ||
     wpe_path_cat(out, n, "/man") ||
     wpe_path_cat(out, n, section ? section : "") ||
     wpe_path_cat(out, n, "/*"))
 {
  if (n)
   out[0] = '\0';
  return -1;
 }
 return 0;
}

#endif
