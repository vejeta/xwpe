#ifndef WPE_DEBPATH_H
#define WPE_DEBPATH_H

/* The file names a debugger is started on.  Kept in its own file, with no
   editor headers, so it can be unit tested directly: we_debug.c is far too
   entangled with the editor to link on its own, and a path long enough to be
   refused cannot be made through the file system to test it there. */

#include <stddef.h>

/**
 * e_d_copy_arg - Copy a path into a debugger start buffer without cutting it.
 * @out: destination; @n: its size; @src: the path to copy.
 *
 * A debugger given a shortened file name stops at once ("a68g ended
 * unexpectedly"), so a path that does not fit must be refused, not trimmed.
 * Return: 0 on success, -1 when @src does not fit (@out is then empty).
 */
int e_d_copy_arg(char *out, size_t n, const char *src);

/**
 * e_d_debuggee_path - Build "dir/name", the full path a debugger is started on.
 * @out: destination; @n: its size; @dir: the file's directory; @name: its name.
 *
 * pdb and a68g are given the source's full path so they find it from any
 * working directory, however deeply the file is nested.
 * Return: 0 on success, -1 when the result does not fit in @n (@out is empty).
 */
int e_d_debuggee_path(char *out, size_t n, const char *dir, const char *name);

#endif /* WPE_DEBPATH_H */
