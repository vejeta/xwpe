#include "calc.h"

/* Count the separators; used only for a quick sanity check of the text. */
int scan_tokens(const char *text)
{
    int n = 0;

    for (; *text; text++)
        if (*text == ';')
            n++;
    return n;
}

/* The front end: reject empty input, then build the block. */
int front_end(const char *text, struct block *b)
{
    if (scan_tokens(text) < 0)
        return -1;
    if (*text == '\0')
        return -1;
    return parse_body(text, b);
}
