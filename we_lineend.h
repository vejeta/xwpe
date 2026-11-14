/* we_lineend.h -- the one owner of a file's line endings.
   Copyright (C) 2026 Juan Manuel Mendez Rey
   This file is part of xwpe and is distributed under the GNU General Public
   License, version 2 or later.

   Saving a file changes only the lines the user changed.  A buffer has one
   line-ending style, decided when its file is read and kept with it; a
   carriage return that is not part of a CR LF break is a byte of the text.
   Nothing but this owner turns file bytes into rows or rows into file bytes. */

#ifndef WE_LINEEND_H
#define WE_LINEEND_H

#include "we_buffer.h"

/* BUFFER.eol.  Zero is LF, so a buffer nobody set (Messages, help, the
   assistant's panes, the clipboard window) is LF. */
#define E_EOL_LF              0
#define E_EOL_CRLF            1

/* BUFFER.final_break.  Zero is NONE: a buffer nobody set records no break. */
#define E_FINAL_BREAK_NONE    0
#define E_FINAL_BREAK_HAD     1

/* How bytes become rows. */
typedef enum {
 E_EOL_KEEP,   /* a file to be written back as it was: rule 2, every CR kept as
                  text unless it is part of a CR LF break of an all-CRLF file */
 E_EOL_ROWS,   /* text the buffer's own rows made (e_eol_text with E_EOL_LF):
                  split on LF only, every CR kept; the exact inverse */
 E_EOL_NORMALIZE /* text that never came from the buffer (the system
                  clipboard, a program's output): a CR before an LF is part
                  of the break, a CR anywhere else is text */
} WpeEolPolicy;

/* What the bytes of a file said about themselves. */
typedef struct WpeEolInfo {
 int eol;          /* E_EOL_CRLF when every break is CR LF (and there is one), else E_EOL_LF */
 int final_break;  /* E_FINAL_BREAK_HAD: the last byte is a line break */
 int breaks;       /* how many line breaks: 0 means the file says nothing of a style */
 int mixed;        /* 1: CR LF and bare LF both occur */
 int lone_cr;      /* how many CR bytes are not before an LF */
 int nul;          /* how many NUL bytes were dropped (a row is a C string) */
} WpeEolInfo;

STRING *e_eol_lines(const unsigned char *bytes, size_t n, int mx_x, WpeEolPolicy policy,
                    WpeEolInfo *info, int *count);
size_t  e_eol_normalize(char *out, const char *in, size_t len);
int     e_eol_replace_all(BUFFER *b, const char *text, WpeBufferStep step, WpeEolPolicy policy);
int     e_eol_append_text(BUFFER *b, const char *text, int skip_blank, WpeEolPolicy policy);
int     e_eol_append_rows(BUFFER *b, const char *text, WpeEolPolicy policy);
void    e_eol_nul_sentence(int count, char *buf, size_t size);
int     e_eol_read_file_into(FILE *fp, BUFFER *b, int x, int y, int manpage,
                             WpeEolInfo *info, POINT *end);
size_t  e_eol_text(BUFFER *b, int ya, int xa, int ye, int xe, int eol, int keep_final,
                   char **out);
int     e_eol_write(FILE *fp, BUFFER *b, int ya, int xa, int ye, int xe, int keep_final);
int     e_eol_default(const ECNT *cn);
void    e_eol_set_default(ECNT *cn, int data_key);
int     e_eol_data_key(const ECNT *cn);
void    e_eol_buffer_fresh(BUFFER *b, const ECNT *cn);
void    e_eol_buffer_adopt(BUFFER *b, const ECNT *cn, const WpeEolInfo *info);
const char *e_eol_name(const BUFFER *b);

#endif
