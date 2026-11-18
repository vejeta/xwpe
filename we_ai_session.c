/* we_ai_session.c -- what a saved assistant conversation may hold
   (see we_ai_session.h).  Plain C and json-c. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "we_ai_session.h"

/** The words a turn's role may have in the file, and how a role is spelled. */
#define SESSION_ROLE_USER      "user"
#define SESSION_ROLE_ASSISTANT "assistant"

/** The groups of an id and where the dashes stand: 8-4-4-4-12. */
#define SESSION_ID_DASH_1 8
#define SESSION_ID_DASH_2 13
#define SESSION_ID_DASH_3 18
#define SESSION_ID_DASH_4 23

/** wpe_ai_turn_role_name - The word a request carries for a turn's role. */
const char *wpe_ai_turn_role_name(wpe_ai_turn_role role)
{
 return role == WPE_AI_TURN_ASSISTANT ? SESSION_ROLE_ASSISTANT : SESSION_ROLE_USER;
}

/**
 * session_id_dash_at - Is a dash where an id has one?
 * @i: the position in the id.
 */
static int session_id_dash_at(size_t i)
{
 return i == SESSION_ID_DASH_1 || i == SESSION_ID_DASH_2 ||
        i == SESSION_ID_DASH_3 || i == SESSION_ID_DASH_4;
}

/**
 * wpe_ai_session_id_valid - Is this word a session id?
 * @s: the word, or NULL.
 * Return: 1 for 36 bytes of hex digits with dashes in the places of a UUID.
 *
 * What the user gets: a conversation file, or the command line tool's own
 * answer, cannot put a word of its choice after --resume on the tool's
 * command line.  A word that cannot start with a dash can never be read as
 * one of the tool's flags.
 */
int wpe_ai_session_id_valid(const char *s)
{
 size_t i;

 if (s == NULL || strlen(s) != WPE_AI_SESSION_ID_LEN)
  return 0;
 for (i = 0; i < WPE_AI_SESSION_ID_LEN; i++)
 {
  if (session_id_dash_at(i))
  {
   if (s[i] != '-')
    return 0;
  }
  else if (!isxdigit((unsigned char)s[i]))
   return 0;
 }
 return 1;
}

/**
 * session_role_of - The role a file's turn names, if the editor writes it.
 * @word: the role's text.
 * @role: receives it.
 * Return: 1 for "user" and "assistant", 0 for anything else.
 */
static int session_role_of(const char *word, wpe_ai_turn_role *role)
{
 if (strcmp(word, SESSION_ROLE_USER) == 0)
  *role = WPE_AI_TURN_USER;
 else if (strcmp(word, SESSION_ROLE_ASSISTANT) == 0)
  *role = WPE_AI_TURN_ASSISTANT;
 else
  return 0;
 return 1;
}

/**
 * session_take_id - Judge the resume word of a file.
 * @top: the file's object; @out: receives the word.
 * Return: WS_SESSION_OK, or WS_SESSION_BAD_ID for a word that is not an id.
 *
 * No word, or an empty one, is a conversation with nothing to resume.
 */
static ws_session_verdict session_take_id(struct json_object *top, ws_session_in *out)
{
 struct json_object *v;
 const char *s;

 if (!json_object_object_get_ex(top, "cli_session", &v))
  return WS_SESSION_OK;
 if (!json_object_is_type(v, json_type_string))
  return WS_SESSION_BAD_ID;
 s = json_object_get_string(v);
 if (*s == '\0')
  return WS_SESSION_OK;
 if (!wpe_ai_session_id_valid(s))
  return WS_SESSION_BAD_ID;
 memcpy(out->id, s, WPE_AI_SESSION_ID_LEN + 1);
 return WS_SESSION_OK;
}

/**
 * session_check_place - Is the file kept for the place the caller is resuming?
 * @top: the file's object; @place: the caller's project file or folder.
 * Return: WS_SESSION_OK, or WS_SESSION_WRONG_PLACE.
 *
 * A file of an older editor names no place and is read.
 */
static ws_session_verdict session_check_place(struct json_object *top, const char *place)
{
 struct json_object *v;

 if (!json_object_object_get_ex(top, "place", &v))
  return WS_SESSION_OK;
 if (!json_object_is_type(v, json_type_string))
  return WS_SESSION_WRONG_PLACE;
 return strcmp(json_object_get_string(v), place) == 0 ? WS_SESSION_OK : WS_SESSION_WRONG_PLACE;
}

/**
 * session_take_turn - Judge one turn of a file and keep it.
 * @m: the turn's object; @out: the conversation so far, room for the turn.
 * Return: WS_SESSION_OK, WS_SESSION_BAD_ROLE, WS_SESSION_BAD_CONTENT or
 * WS_SESSION_NO_MEMORY.
 */
static ws_session_verdict session_take_turn(struct json_object *m, ws_session_in *out)
{
 struct json_object *r, *c;
 wpe_ai_turn_role role;
 char *text;

 if (!json_object_is_type(m, json_type_object))
  return WS_SESSION_BAD_CONTENT;
 if (!json_object_object_get_ex(m, "role", &r) || !json_object_is_type(r, json_type_string) ||
     !session_role_of(json_object_get_string(r), &role))
  return WS_SESSION_BAD_ROLE;
 if (!json_object_object_get_ex(m, "content", &c) || !json_object_is_type(c, json_type_string))
  return WS_SESSION_BAD_CONTENT;
 text = strdup(json_object_get_string(c));
 if (text == NULL)
  return WS_SESSION_NO_MEMORY;
 out->role[out->n] = role;
 out->content[out->n] = text;
 out->n++;
 return WS_SESSION_OK;
}

/**
 * session_take_turns - Judge every turn of a file and keep them.
 * @top: the file's object; @out: receives the turns.
 * Return: WS_SESSION_OK, or the first reason a turn is not the editor's.
 */
static ws_session_verdict session_take_turns(struct json_object *top, ws_session_in *out)
{
 struct json_object *arr;
 ws_session_verdict v;
 size_t count, i;

 if (!json_object_object_get_ex(top, "messages", &arr))
  return WS_SESSION_OK;
 if (!json_object_is_type(arr, json_type_array))
  return WS_SESSION_BAD_CONTENT;
 count = json_object_array_length(arr);
 if (count == 0)
  return WS_SESSION_OK;
 out->role = calloc(count, sizeof *out->role);
 out->content = calloc(count, sizeof *out->content);
 if (out->role == NULL || out->content == NULL)
  return WS_SESSION_NO_MEMORY;
 for (i = 0; i < count; i++)
 {
  v = session_take_turn(json_object_array_get_idx(arr, i), out);
  if (v != WS_SESSION_OK)
   return v;
 }
 return WS_SESSION_OK;
}

/** ws_session_in_free - Free what a judged conversation holds, and empty it. */
void ws_session_in_free(ws_session_in *in)
{
 int i;

 for (i = 0; i < in->n; i++)
  free(in->content[i]);
 free(in->role);
 free(in->content);
 memset(in, 0, sizeof *in);
}

/**
 * ws_session_parse - Judge a saved conversation and take what it holds.
 * @json: the file's text; @place: the project file or folder being resumed;
 * @out: receives the conversation.
 * Return: WS_SESSION_OK, or why the file is not used (then @out is empty).
 *
 * What the user gets: a conversation file that was not written by the editor
 * (a forged turn with a role of its own, a resume word that is not an id, a
 * file kept for another project) is never sent to the model and never steers
 * the command line tool, whoever put it in the state folder.
 */
ws_session_verdict ws_session_parse(const char *json, const char *place, ws_session_in *out)
{
 struct json_object *top;
 ws_session_verdict v;

 memset(out, 0, sizeof *out);
 top = json == NULL ? NULL : json_tokener_parse(json);
 if (top == NULL || !json_object_is_type(top, json_type_object))
 {
  if (top != NULL)
   json_object_put(top);
  return WS_SESSION_NOT_JSON;
 }
 v = session_check_place(top, place);
 if (v == WS_SESSION_OK)
  v = session_take_id(top, out);
 if (v == WS_SESSION_OK)
  v = session_take_turns(top, out);
 json_object_put(top);
 if (v != WS_SESSION_OK)
  ws_session_in_free(out);
 return v;
}

/** ws_session_verdict_words - Why a file was not used, in words for the trace. */
const char *ws_session_verdict_words(ws_session_verdict v)
{
 switch (v)
 {
 case WS_SESSION_OK:           return "ok";
 case WS_SESSION_NOT_JSON:     return "not a conversation";
 case WS_SESSION_BAD_ROLE:     return "a turn with a role the editor never writes";
 case WS_SESSION_BAD_CONTENT:  return "a turn that is not text";
 case WS_SESSION_BAD_ID:       return "a resume word that is not an id";
 case WS_SESSION_WRONG_PLACE:  return "kept for another place";
 case WS_SESSION_NO_MEMORY:    return "out of memory";
 }
 return "not used";
}
