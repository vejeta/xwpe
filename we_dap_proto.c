/* we_dap_proto.c -- Debug Adapter Protocol wire format (transport core).
 * See we_dap_proto.h.  Depends only on libc and json-c; no xwpe globals. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include "we_dap_proto.h"

void e_dap_reader_init(e_dap_reader *r)
{
 r->buf = NULL;
 r->len = 0;
 r->cap = 0;
}

void e_dap_reader_free(e_dap_reader *r)
{
 free(r->buf);
 r->buf = NULL;
 r->len = r->cap = 0;
}

void e_dap_reader_push(e_dap_reader *r, const char *data, size_t n)
{
 if (n == 0)
  return;
 if (r->len + n > r->cap)
 {
  size_t cap = r->cap ? r->cap : 256;
  while (cap < r->len + n)
   cap *= 2;
  r->buf = realloc(r->buf, cap);
  r->cap = cap;
 }
 memcpy(r->buf + r->len, data, n);
 r->len += n;
}

/* Locate the end of the header block ("\r\n\r\n") within the first `len`
 * bytes.  Returns the offset of the first byte AFTER the separator, or 0 if
 * the separator is not yet present. */
static size_t hdr_end(const char *buf, size_t len)
{
 size_t i;
 for (i = 0; i + 3 < len; i++)
  if (buf[i] == '\r' && buf[i + 1] == '\n' &&
      buf[i + 2] == '\r' && buf[i + 3] == '\n')
   return i + 4;
 return 0;
}

/* Parse the Content-Length value from a header block of `hlen` bytes.
 * Returns the length, or -1 if the header is absent/malformed. */
static long content_length(const char *buf, size_t hlen)
{
 static const char key[] = "content-length:";
 size_t i;
 for (i = 0; i + sizeof(key) - 1 <= hlen; i++)
 {
  size_t k;
  for (k = 0; key[k]; k++)
   if (tolower((unsigned char)buf[i + k]) != key[k])
    break;
  if (key[k] == '\0')
  {
   const char *p = buf + i + sizeof(key) - 1;
   const char *end = buf + hlen;
   long n;
   char *stop;
   while (p < end && (*p == ' ' || *p == '\t'))
    p++;
   n = strtol(p, &stop, 10);
   if (stop == p || n < 0)
    return -1;
   return n;
  }
 }
 return -1;
}

/* Drop the first `n` bytes from the reader buffer. */
static void reader_consume(e_dap_reader *r, size_t n)
{
 if (n >= r->len)
  r->len = 0;
 else
 {
  memmove(r->buf, r->buf + n, r->len - n);
  r->len -= n;
 }
}

struct json_object *e_dap_reader_pop(e_dap_reader *r, int *err)
{
 size_t he, body_start;
 long clen;
 struct json_object *obj;

 if (err)
  *err = 0;
 if (r->len == 0)
  return NULL;

 he = hdr_end(r->buf, r->len);
 if (he == 0)
  return NULL;                 /* headers not complete yet */

 clen = content_length(r->buf, he);
 if (clen < 0)
 {
  /* Malformed header: drop it so the stream can resync. */
  reader_consume(r, he);
  if (err)
   *err = 1;
  return NULL;
 }

 body_start = he;
 if (r->len < body_start + (size_t)clen)
  return NULL;                 /* body not fully arrived yet */

 {
  struct json_tokener *tok = json_tokener_new();
  obj = json_tokener_parse_ex(tok, r->buf + body_start, (int)clen);
  if (obj == NULL || json_tokener_get_error(tok) != json_tokener_success)
  {
   if (obj)
   {
    json_object_put(obj);
    obj = NULL;
   }
   if (err)
    *err = 1;
  }
  json_tokener_free(tok);
 }

 reader_consume(r, body_start + (size_t)clen);
 return obj;
}

char *e_dap_build_request(int seq, const char *command,
                          struct json_object *arguments, size_t *out_len)
{
 struct json_object *msg;
 const char *body;
 size_t blen, hlen;
 char header[48];
 char *out;

 msg = json_object_new_object();
 if (!msg)
  return NULL;
 json_object_object_add(msg, "seq", json_object_new_int(seq));
 json_object_object_add(msg, "type", json_object_new_string("request"));
 json_object_object_add(msg, "command", json_object_new_string(command));
 if (arguments)
  /* json_object_get(): add a reference so json_object_put(msg) below does not
     free the caller's argument object. */
  json_object_object_add(msg, "arguments", json_object_get(arguments));

 body = json_object_to_json_string_ext(msg, JSON_C_TO_STRING_PLAIN);
 blen = strlen(body);
 hlen = (size_t)snprintf(header, sizeof(header),
                         "Content-Length: %zu\r\n\r\n", blen);
 out = malloc(hlen + blen + 1);
 if (out)
 {
  memcpy(out, header, hlen);
  memcpy(out + hlen, body, blen);
  out[hlen + blen] = '\0';
  if (out_len)
   *out_len = hlen + blen;
 }
 json_object_put(msg);
 return out;
}
