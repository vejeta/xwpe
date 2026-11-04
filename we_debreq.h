#ifndef WPE_DEBREQ_H
#define WPE_DEBREQ_H

/* A command to the debugger, and the answer it gives.

   The answer is one type for everyone who handles it: the request engine
   that collects it from the debugger's output, the session state
   (we_debsess.h) that reads what it means, and the editor while it still
   reads answers itself.  One shape means a reader written for the editor's
   line readers today keeps working unchanged when the answer comes from the
   engine instead.

   Plain C: no curses, no X11, no Wayland, no editor header. */

#include <stddef.h>

/* The debugger's answer to one command.  @text holds its lines, joined,
   '\0'-terminated, or is NULL when the debugger gave nothing before its
   prompt.  An owner that grows the text sets @size to the room it has; a
   reader handed text it does not own sees @size 0 and never grows or frees
   it.  @error is set when an error record or a line of the debugger's error
   stream was part of the answer. */
typedef struct
{
 char   *text;
 size_t  len, size;
 int     error;
} E_D_ANSWER;

#endif /* WPE_DEBREQ_H */
