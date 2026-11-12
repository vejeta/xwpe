#ifndef CALC_H
#define CALC_H

#define MAX_STMTS 16
#define STMT_TEXT 32

struct stmt {
    char text[STMT_TEXT];
};

struct block {
    struct stmt items[MAX_STMTS];
    int count;
};

/* driver.c */
int build_unit(const char *src);
int read_source(const char *src, char *out, int size);
int repl_line(const char *line);

/* front.c */
int front_end(const char *text, struct block *b);
int scan_tokens(const char *text);

/* parser.c */
int parse_body(const char *text, struct block *b);
int parse_stmt(const char *text, struct stmt *s);

#endif
