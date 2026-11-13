/* we_fieldtake.h -- the one rule for taking the text of a dialog field out
   into a buffer of the caller's.

   A field holds as many bytes as its own capacity says, and that is not the
   size of the buffer a caller keeps the result in.  Copying with strcpy ran
   past the buffer; copying with strncpy cut the text.  This refuses: the
   text goes in whole or not at all.

   Plain C, one static inline, so a unit test uses it without the editor. */
#ifndef WE_FIELDTAKE_H
#define WE_FIELDTAKE_H

#include <stddef.h>
#include <string.h>

#ifdef __GNUC__
#define WPE_FIELD_TAKE_RESULT __attribute__((warn_unused_result))
#else
#define WPE_FIELD_TAKE_RESULT
#endif

/**
 * wpe_field_take - Copy a field's text whole into a buffer, or not at all.
 * @txt: the field's text; @dst/@size: the caller's buffer and its size.
 *
 * Return: 0, or -1 with @dst empty when @txt and its terminator do not fit.
 */
static inline int wpe_field_take(const char *txt, char *dst, size_t size) WPE_FIELD_TAKE_RESULT;
static inline int wpe_field_take(const char *txt, char *dst, size_t size)
{
 size_t len;

 if (!dst || !size)
  return -1;
 dst[0] = '\0';
 if (!txt)
  return -1;
 len = strlen(txt);
 if (len >= size)
  return -1;
 memcpy(dst, txt, len + 1);
 return 0;
}

#endif
