/* we_ai_session.h -- what a saved assistant conversation may hold.

   The conversation the assistant keeps in the user's state folder is read
   back at the next start and sent to the model, and its resume word goes to
   the Claude command line tool.  This is the one place that says what such a
   file may contain: only the two turns the editor itself writes (the user's
   and the assistant's), a resume word that is an id, and the folder or
   project it was kept for.  A file that holds anything else was not written
   by the editor and is not used.

   Plain C and json-c: no editor header, so a unit test links it alone. */
#ifndef WE_AI_SESSION_H
#define WE_AI_SESSION_H

#include <stddef.h>

/* Who spoke a turn.  The editor appends only these two; a system turn is
   written by the editor in the request it builds, never into the store, so
   the store has no way to hold one. */
typedef enum {
 WPE_AI_TURN_USER,
 WPE_AI_TURN_ASSISTANT
} wpe_ai_turn_role;

/* "user" or "assistant": the words the backends put in a request. */
const char *wpe_ai_turn_role_name(wpe_ai_turn_role role);

/* The shape of a session id the command line tool gives: 8-4-4-4-12 hex
   digits, 36 bytes, nothing else.  It cannot start with '-', so the word
   after --resume can never be read as a flag.  1 when @s is one. */
#define WPE_AI_SESSION_ID_LEN 36
int wpe_ai_session_id_valid(const char *s);

/* What a file's conversation is, after it was judged. */
typedef struct {
 char              id[WPE_AI_SESSION_ID_LEN + 1];   /* the resume word, or "" */
 int               n;                               /* turns */
 wpe_ai_turn_role *role;
 char            **content;
} ws_session_in;

/* Why a file is not used; WS_SESSION_OK when it is. */
typedef enum {
 WS_SESSION_OK,
 WS_SESSION_NOT_JSON,        /* does not parse, or its top is not an object */
 WS_SESSION_BAD_ROLE,        /* a turn with a role the editor never writes, or none */
 WS_SESSION_BAD_CONTENT,     /* a turn that is not an object with a text */
 WS_SESSION_BAD_ID,          /* a resume word that is not an id */
 WS_SESSION_WRONG_PLACE,     /* kept for another project or folder */
 WS_SESSION_NO_MEMORY
} ws_session_verdict;

/* Judge a file's text.  @place is the project file or folder the caller is
   resuming; a file that names another place is refused (two names that hash
   alike must never resume each other's conversation), and one that names
   none (an older editor's) is read.  One bad turn refuses the whole file:
   the editor never writes one, so such a file is not the editor's.  On
   anything but WS_SESSION_OK @out holds nothing to free. */
ws_session_verdict ws_session_parse(const char *json, const char *place, ws_session_in *out);
void ws_session_in_free(ws_session_in *in);

/* The words of a verdict, for the trace ("not json", "foreign role", ...). */
const char *ws_session_verdict_words(ws_session_verdict v);

#endif
