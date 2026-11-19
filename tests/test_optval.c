/* test_optval.c -- unit test for the owner of an option value's stored form
 * (we_optval.c).  Always runs; run it under ASan+UBSan too.
 *
 * What the user sees behind each group:
 *  - an error pattern with line breaks and backslashes in it is saved and
 *    read back exactly as typed, empty and all-escape patterns included;
 *  - a hand-written ^M (backslash r) in an option file is a carriage return;
 *  - an option file with a stray or trailing backslash is read without the
 *    editor looking past the value, and without leftover bytes in the text.
 *
 * Every input of the reading side lives in a malloc of exactly its length
 * with no terminator, and every output has guard bytes past its size, so a
 * read of one byte too many or a write of one byte too many is seen by the
 * sanitizer or by the guard check.
 */
#include <stdio.h>
#include "linebuf.h"
#include <stdlib.h>
#include <string.h>

#include "../we_optval.h"

#define GUARD 0x55
#define GUARD_BYTES 8
#define MAX_LEN 600
#define ROUND_TRIPS 4000
#define SEED 20261114u

static int fails;

static void chk_int(const char *what, long got, long want)
{
 if (got != want)
 {
  printf("FAIL: %s: got %ld want %ld\n", what, got, want);
  fails++;
 }
}

static void chk_bytes(const char *what, const char *got, size_t got_n,
                      const char *want, size_t want_n)
{
 if (got_n != want_n || memcmp(got, want, want_n) != 0)
 {
  printf("FAIL: %s: %zu bytes differ from the %zu expected\n", what, got_n, want_n);
  fails++;
 }
}

/* The bytes of a buffer past its size @n are never written. */
static void chk_guard(const char *what, const unsigned char *buf, size_t n)
{
 size_t i;

 for (i = n; i < n + GUARD_BYTES; i++)
  if (buf[i] != GUARD)
  {
   printf("FAIL: %s: byte %zu past the size was written\n", what, i - n);
   fails++;
   return;
  }
}

/* A copy of @in[@n] in a malloc of exactly @n bytes, no terminator. */
static char *exact_copy(const char *in, size_t n)
{
 char *p = malloc(n ? n : 1);

 if (n)
  memcpy(p, in, n);
 return p;
}

/* Encode @in[@n] into a buffer of exactly the computed size, guarded. */
static char *encode_checked(const char *what, const char *in, size_t n, size_t *got_n)
{
 size_t cap = wpe_optval_encoded_size(n);
 unsigned char *buf = malloc(cap + GUARD_BYTES);

 memset(buf, GUARD, cap + GUARD_BYTES);
 *got_n = wpe_optval_encode((char *)buf, cap, in, n);
 chk_guard(what, buf, cap);
 chk_int(what, buf[*got_n], 0);
 return (char *)buf;
}

/* Decode @in[@n] into a buffer of exactly n + 1 bytes, guarded.  @in is read
   from an exact-size copy, so a read past @in[@n - 1] is an ASan report. */
static char *decode_checked(const char *what, const char *in, size_t n, size_t *got_n)
{
 char *exact = exact_copy(in, n);
 unsigned char *buf = malloc(n + 1 + GUARD_BYTES);

 memset(buf, GUARD, n + 1 + GUARD_BYTES);
 *got_n = wpe_optval_decode((char *)buf, n + 1, exact, n);
 chk_guard(what, buf, n + 1);
 chk_int(what, buf[*got_n], 0);
 free(exact);
 return (char *)buf;
}

/* decode(encode(x)) == x, with every size checked on the way. */
static void round_trip(const char *what, const char *x, size_t n)
{
 size_t enc_n, dec_n;
 char *enc = encode_checked(what, x, n, &enc_n);
 char *dec;

 if (enc_n > wpe_optval_encoded_size(n) - 1)
 {
  printf("FAIL: %s: encoded form longer than the computed size\n", what);
  fails++;
 }
 dec = decode_checked(what, enc, enc_n, &dec_n);
 chk_bytes(what, dec, dec_n, x, n);
 free(enc);
 free(dec);
}

/* Decode the stored text @stored (a C string) and compare with @want. */
static void decodes_to(const char *what, const char *stored, const char *want, size_t want_n)
{
 size_t got_n;
 char *got = decode_checked(what, stored, strlen(stored), &got_n);

 chk_bytes(what, got, got_n, want, want_n);
 free(got);
}

/* Encode @x (a C string) and compare with the stored text @want. */
static void encodes_to(const char *what, const char *x, const char *want)
{
 size_t got_n;
 char *got = encode_checked(what, x, strlen(x), &got_n);

 chk_bytes(what, got, got_n, want, strlen(want));
 free(got);
}

static unsigned rng_state = SEED;

/* A deterministic xorshift, so a failure names its seed and repeats. */
static unsigned rng(void)
{
 rng_state ^= rng_state << 13;
 rng_state ^= rng_state >> 17;
 rng_state ^= rng_state << 5;
 return rng_state;
}

/* A byte drawn so the escapes, their letters and a NUL come up often. */
static char random_byte(void)
{
 static const char favourite[] = { '\n', '\\', '\r', 'n', 'r', 0, (char)0xE2, (char)0xFF };
 unsigned pick = rng() % 4;

 if (pick == 0)
  return favourite[rng() % sizeof favourite];
 if (pick == 1)
  return (char)(rng() & 0xFF);
 return (char)(' ' + rng() % 95);
}

/* @scenario SC-option-codec-01 */
/* A seeded property: every byte string reads back as it was written. */
static void test_property_round_trip(void)
{
 char value[MAX_LEN];
 size_t n, i, k;

 printf("seed %u\n", SEED);
 for (k = 0; k < ROUND_TRIPS; k++)
 {
  n = rng() % (MAX_LEN + 1);
  for (i = 0; i < n; i++)
   value[i] = random_byte();
  round_trip("random value", value, n);
 }
}

/* @scenario SC-option-codec-01 */
/* The inputs the evidence measured, and the edges of the grammar. */
static void test_fixed_values(void)
{
 char line[300];

 round_trip("empty", "", 0);
 round_trip("one line break", "\n", 1);
 round_trip("one backslash", "\\", 1);
 round_trip("all escapes", "\n\\\n\\\n\\", 6);
 round_trip("a NUL inside", "a\0b\n", 4);
 round_trip("backslash then r", "\\r", 2);
 round_trip("a carriage return", "a\rb", 3);
 round_trip("ends in a backslash", "${FILE}:${LINE}:\n\n\\", 19);

 /* The evidence line: 254 bytes, three line breaks, the rest blanks. */
 memset(line, ' ', sizeof line);
 line[10] = line[100] = line[200] = '\n';
 round_trip("254 bytes, three line breaks", line, 254);
}

/* @scenario SC-option-codec-01 */
/* The stored form is exactly the two escapes and every other byte as it is. */
static void test_stored_form(void)
{
 encodes_to("line break", "a\nb", "a\\nb");
 encodes_to("backslash", "a\\b", "a\\\\b");
 encodes_to("carriage return stays raw", "a\rb", "a\rb");
 encodes_to("a backslash before n", "\\n", "\\\\n");
 encodes_to("empty", "", "");
 chk_int("size of the empty value", (long)wpe_optval_encoded_size(0), 1);
 chk_int("size of three bytes", (long)wpe_optval_encoded_size(3), 7);
}

/* @scenario SC-option-codec-01 */
/* Reading is total: what a hand-written or damaged file can hold. */
static void test_decode_is_total(void)
{
 decodes_to("n is a line break", "a\\nb", "a\nb", 3);
 decodes_to("r is a carriage return", "a\\rb", "a\rb", 3);
 decodes_to("two backslashes are one", "a\\\\b", "a\\b", 3);
 decodes_to("other letter keeps the backslash", "\\qb", "\\qb", 3);
 decodes_to("trailing lone backslash", "ab\\", "ab\\", 3);
 decodes_to("only a backslash", "\\", "\\", 1);
 decodes_to("empty", "", "", 0);
 decodes_to("backslash before the end then more", "\\\\\\", "\\\\", 2);
}

/* @scenario SC-option-codec-01 */
/* A buffer that is too small is filled to its size and terminated. */
static void test_small_buffers_are_respected(void)
{
 unsigned char buf[3 + GUARD_BYTES];

 memset(buf, GUARD, sizeof buf);
 chk_int("encode cut to the buffer", (long)wpe_optval_encode((char *)buf, 3, "\n\n\n", 3), 2);
 chk_int("encode terminator", buf[2], 0);
 chk_guard("encode cut", buf, 3);

 memset(buf, GUARD, sizeof buf);
 chk_int("decode cut to the buffer", (long)wpe_optval_decode((char *)buf, 3, "abcdef", 6), 2);
 chk_int("decode terminator", buf[2], 0);
 chk_guard("decode cut", buf, 3);
}

/* @scenario SC-option-codec-01 */
/* The two public names the editor calls. */
static void test_public_names(void)
{
 char *enc = WpeStringToValue("");
 char *dec = WpeValueToString("x\\ny\\");

 chk_int("empty string is empty", enc && enc[0] == 0, 1);
 chk_bytes("decode wrapper", dec, strlen(dec), "x\ny\\", 4);
 free(enc);
 free(dec);
 enc = WpeStringToValue("\n\n");
 chk_bytes("encode wrapper", enc, strlen(enc), "\\n\\n", 4);
 free(enc);
}

int main(void)
{
 test_fixed_values();
 test_stored_form();
 test_decode_is_total();
 test_small_buffers_are_respected();
 test_public_names();
 test_property_round_trip();
 if (fails)
 {
  printf("%d failure(s)\n", fails);
  return 1;
 }
 printf("test_optval: all passed\n");
 return 0;
}
