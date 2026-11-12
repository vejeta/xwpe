#include <string.h>
#include "calc.h"

/* Parse one statement: keep its text, refuse an empty or oversized one. */
int parse_stmt(const char *text, struct stmt *s)
{
    size_t len = strlen(text);

    if (len == 0 || len >= STMT_TEXT)
        return -1;
    memcpy(s->text, text, len + 1);
    return 0;
}

/* Parse a block: statements are separated by semicolons. */
int parse_body(const char *text, struct block *b)
{
    char piece[STMT_TEXT];
    int used = 0;

    b->count = 0;
    for (;; text++) {
        if (*text == ';' || *text == '\0') {
            piece[used] = '\0';
            if (used > 0) {
                if (b->count >= MAX_STMTS)
                    return -1;
                if (parse_stmt(piece, &b->items[b->count]) < 0)
                    return -1;
                b->count++;
            }
            used = 0;
            if (*text == '\0')
                break;
        } else if (used < STMT_TEXT - 1) {
            piece[used++] = *text;
        }
    }
    return b->count;
}
