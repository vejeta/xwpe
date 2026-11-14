/* we_optval.h -- the one owner of an option value's stored form.

   A value the user can set, today a language's MessageString (the error and
   warning pattern), holds bytes a line-oriented option file cannot carry: a
   line break and a backslash.  It is stored escaped and read back through the
   functions below, so the pattern the user typed is the pattern that comes
   back, whatever bytes it holds, and a damaged or hand-written file can
   neither make the editor read past the value nor write past a buffer.

   The stored form:
     - a line break (LF) is written as the two bytes backslash, n;
     - a backslash is written as the two bytes backslash, backslash;
     - every other byte, a carriage return included, is written as it is.

   Reading is total.  Backslash then n is a line break, backslash then r is a
   carriage return (what the manual calls ^M), backslash then backslash is a
   backslash.  A backslash before any other byte is a plain backslash and the
   byte after it is read on its own.  A backslash that ends the value is a
   plain backslash.

   Plain C: libc only, so a unit test links it alone. */
#ifndef WE_OPTVAL_H
#define WE_OPTVAL_H

#include <stddef.h>

/** The buffer wpe_optval_encode needs for a value of @n bytes: every byte
    an escape, plus the terminator. */
size_t wpe_optval_encoded_size(size_t n);

/** Write the stored form of @in[@n] into @out[@cap], terminated.  The user
    gains a value that reads back byte for byte, a line break and a backslash
    included.  @cap must be at least wpe_optval_encoded_size(@n); no byte past
    @out[@cap - 1] is written.
    Return: the bytes written before the terminator. */
size_t wpe_optval_encode(char *out, size_t cap, const char *in, size_t n);

/** Read the stored form @in[@n] into @out[@cap], terminated.  Works on any
    bytes, so a hand-edited or damaged option file never makes the editor
    read past the value or write past @out.  @cap must be at least @n + 1;
    no byte past @in[@n - 1] is read.
    Return: the bytes written before the terminator. */
size_t wpe_optval_decode(char *out, size_t cap, const char *in, size_t n);

/** The stored form of the string @str, in a new buffer (free with free).
    Return: the buffer, or NULL when memory is short. */
char *WpeStringToValue(const char *str);

/** The string a stored form @value stands for, in a new buffer (free with
    free).  Return: the buffer, or NULL when memory is short. */
char *WpeValueToString(const char *value);

#endif
