#include <string.h>
#include "calc.h"

/* Copy the source text into out; the text is already in memory here. */
int read_source(const char *src, char *out, int size)
{
    int len = (int)strlen(src);

    if (len >= size)
        return -1;
    memcpy(out, src, (size_t)len + 1);
    return len;
}

/* Compile one source string; the result is the number of statements. */
int build_unit(const char *src)
{
    char text[256];
    struct block b;

    if (read_source(src, text, (int)sizeof text) < 0)
        return -1;
    if (front_end(text, &b) < 0)
        return -1;
    return b.count;
}

/* Interactive helper: check a single statement typed by the user. */
int repl_line(const char *line)
{
    struct stmt s;

    return parse_stmt(line, &s);
}
