/* we_clip.c -- OS clipboard integration for xwpe (base64 + OSC 52 framing).
 *
 * Copyright (C) 2026 Juan Manuel Mendez Rey
 * Released under the GNU General Public License, version 2.
 */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "we_clip.h"

/* Default OS-clipboard writer: do nothing.  A UI front-end overrides this at
 * start-up (terminal -> OSC 52, X11 -> selections); until then "copy" stays
 * internal, which is also the right behaviour for a headless build. */
static void e_clip_os_noop(const char *utf8, int len)
{
 (void)utf8;
 (void)len;
}

void (*e_clip_os_set)(const char *utf8, int len) = e_clip_os_noop;

static const char e_b64_tab[] =
 "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int e_b64_encode(const unsigned char *in, int inlen, char *out, int outcap)
{
 int need, i, o;

 if (inlen < 0)
  return -1;
 need = 4 * ((inlen + 2) / 3) + 1;       /* +1 for the trailing NUL */
 if (outcap < need)
  return -1;

 for (i = 0, o = 0; i + 2 < inlen; i += 3)
 {
  unsigned int v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
  out[o++] = e_b64_tab[(v >> 18) & 0x3f];
  out[o++] = e_b64_tab[(v >> 12) & 0x3f];
  out[o++] = e_b64_tab[(v >> 6) & 0x3f];
  out[o++] = e_b64_tab[v & 0x3f];
 }
 if (inlen - i == 1)                      /* one byte left -> 2 chars + "==" */
 {
  unsigned int v = in[i] << 16;
  out[o++] = e_b64_tab[(v >> 18) & 0x3f];
  out[o++] = e_b64_tab[(v >> 12) & 0x3f];
  out[o++] = '=';
  out[o++] = '=';
 }
 else if (inlen - i == 2)                 /* two bytes left -> 3 chars + "=" */
 {
  unsigned int v = (in[i] << 16) | (in[i + 1] << 8);
  out[o++] = e_b64_tab[(v >> 18) & 0x3f];
  out[o++] = e_b64_tab[(v >> 12) & 0x3f];
  out[o++] = e_b64_tab[(v >> 6) & 0x3f];
  out[o++] = '=';
 }
 out[o] = '\0';
 return o;
}

/* e_clip_write_all - write the whole buffer to fd, retrying short writes.
 * Returns 0 on success, -1 if any write() fails. */
static int e_clip_write_all(int fd, const char *buf, int len)
{
 int off = 0;

 while (off < len)
 {
  ssize_t w = write(fd, buf + off, (size_t)(len - off));
  if (w < 0)
   return -1;
  off += (int)w;
 }
 return 0;
}

int e_clip_osc52_write(int fd, const char *utf8, int len)
{
 static const char head[] = "\033]52;c;";   /* ESC ] 52 ; c ;  (7 bytes) */
 char *b64, *seq;
 int b64cap, b64len, seqlen, rc;

 if (len < 0 || len > E_CLIP_OSC52_MAX)
  return -1;

 b64cap = 4 * ((len + 2) / 3) + 1;
 if (!(b64 = malloc((size_t)b64cap)))
  return -1;
 b64len = e_b64_encode((const unsigned char *)utf8, len, b64, b64cap);
 if (b64len < 0)
 {
  free(b64);
  return -1;
 }

 seqlen = 7 + b64len + 1;                     /* head + base64 + BEL */
 if (!(seq = malloc((size_t)seqlen)))
 {
  free(b64);
  return -1;
 }
 memcpy(seq, head, 7);
 memcpy(seq + 7, b64, (size_t)b64len);
 seq[7 + b64len] = '\a';                      /* BEL terminator */

 rc = e_clip_write_all(fd, seq, seqlen);

 free(seq);
 free(b64);
 return rc;
}
