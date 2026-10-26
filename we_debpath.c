/* The file names a debugger is started on: copied whole or refused, never
 * cut.  A debugger started on a shortened name fails at once, and the user
 * would see the debugger end with no reason given; refusing lets the editor
 * say the path is too long instead.
 */
#include <stdio.h>
#include <string.h>
#include "we_debpath.h"

int e_d_copy_arg(char *out, size_t n, const char *src)
{
	size_t len = strlen(src);

	if (len >= n) {
		out[0] = '\0';
		return (-1);
	}
	memcpy(out, src, len + 1);
	return (0);
}

int e_d_debuggee_path(char *out, size_t n, const char *dir, const char *name)
{
	int len = snprintf(out, n, "%s/%s", dir, name);

	if (len < 0 || (size_t)len >= n) {
		out[0] = '\0';
		return (-1);
	}
	return (0);
}
