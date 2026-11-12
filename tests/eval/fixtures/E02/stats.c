/* stats.c - small integer helpers used by the report tool.
 *
 * Everything here works on plain ints and int arrays. No function allocates
 * and none keeps state between calls.
 */
#include <limits.h>
#include "stats.h"

/* ---- scalar helpers --------------------------------------------------- */

/* Keep v inside [lo, hi]. */
int clamp_int(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

/* Absolute value (INT_MIN is not handled). */
int abs_int(int v)
{
    if (v < 0)
        return -v;
    return v;
}

/* -1, 0 or 1 according to the sign of v. */
int sign_int(int v)
{
    if (v < 0)
        return -1;
    if (v > 0)
        return 1;
    return 0;
}

/* The smaller of two values. */
int min_int(int a, int b)
{
    if (a < b)
        return a;
    return b;
}

/* The larger of two values. */
int max_int(int a, int b)
{
    if (a > b)
        return a;
    return b;
}

/* The smallest of three values. */
int min3(int a, int b, int c)
{
    int m = a;

    if (b < m)
        m = b;
    if (c < m)
        m = c;
    return m;
}

/* The largest of three values. */
int max3(int a, int b, int c)
{
    int m = a;

    if (b > m)
        m = b;
    if (c > m)
        m = c;
    return m;
}

/* The middle one of three values. */
int median3(int a, int b, int c)
{
    int lo = min3(a, b, c);
    int hi = max3(a, b, c);

    return a + b + c - lo - hi;
}

/* part as a whole-number percentage of whole; 0 when whole is 0. */
int percent_of(int part, int whole)
{
    if (whole == 0)
        return 0;
    return (int)((long)part * 100 / whole);
}

/* num / den rounded to the nearest integer, halves away from zero. */
int round_div(int num, int den)
{
    if (den == 0)
        return 0;
    if (den < 0) {
        num = -num;
        den = -den;
    }
    if (num >= 0)
        return (num + den / 2) / den;
    return -((-num + den / 2) / den);
}

/* Linear interpolation from a (num/den = 0) to b (num/den = 1). */
int lerp_int(int a, int b, int num, int den)
{
    if (den == 0)
        return a;
    return a + (int)((long)(b - a) * num / den);
}

/* Map any index onto 0..n-1 the way a ring buffer does. */
int wrap_index(int i, int n)
{
    int r;

    if (n <= 0)
        return 0;
    r = i % n;
    if (r < 0)
        r += n;
    return r;
}

/* a + b, stopping at INT_MAX and INT_MIN instead of overflowing. */
int saturating_add(int a, int b)
{
    long s = (long)a + (long)b;

    if (s > INT_MAX)
        return INT_MAX;
    if (s < INT_MIN)
        return INT_MIN;
    return (int)s;
}

/* a - b, stopping at INT_MAX and INT_MIN instead of overflowing. */
int saturating_sub(int a, int b)
{
    long s = (long)a - (long)b;

    if (s > INT_MAX)
        return INT_MAX;
    if (s < INT_MIN)
        return INT_MIN;
    return (int)s;
}

/* ---- number theory and digits ----------------------------------------- */

/* Greatest common divisor; the result is never negative. */
int gcd_int(int a, int b)
{
    int t;

    a = abs_int(a);
    b = abs_int(b);
    while (b != 0) {
        t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* Least common multiple; 0 when either argument is 0. */
int lcm_int(int a, int b)
{
    int g;

    if (a == 0 || b == 0)
        return 0;
    g = gcd_int(a, b);
    return abs_int(a / g * b);
}

/* True when v has exactly one bit set. */
int is_power_of_two(unsigned v)
{
    if (v == 0)
        return 0;
    return (v & (v - 1)) == 0;
}

/* Number of bits set in v. */
int count_bits(unsigned v)
{
    int n = 0;

    while (v != 0) {
        n += (int)(v & 1u);
        v >>= 1;
    }
    return n;
}

/* Sum of the decimal digits of v, ignoring its sign. */
int digit_sum(int v)
{
    int s = 0;

    v = abs_int(v);
    while (v > 0) {
        s += v % 10;
        v /= 10;
    }
    return s;
}

/* The decimal digits of v in reverse order, keeping the sign. */
int reverse_digits(int v)
{
    int neg = v < 0;
    int r = 0;

    v = abs_int(v);
    while (v > 0) {
        r = r * 10 + v % 10;
        v /= 10;
    }
    return neg ? -r : r;
}

/* True when v reads the same in both directions; negatives never do. */
int is_palindrome_num(int v)
{
    if (v < 0)
        return 0;
    return reverse_digits(v) == v;
}

/* The n-th Fibonacci number, fib(0) = 0 and fib(1) = 1. */
int fib_iter(int n)
{
    int a = 0;
    int b = 1;
    int t;

    if (n <= 0)
        return 0;
    while (n-- > 1) {
        t = a + b;
        a = b;
        b = t;
    }
    return b;
}

/* n! for n up to 12; larger values would overflow an int. */
int factorial_small(int n)
{
    int r = 1;

    while (n > 1)
        r *= n--;
    return r;
}

/* Integer square root, rounded down. */
int isqrt(int v)
{
    long r = 0;

    if (v <= 0)
        return 0;
    while ((r + 1) * (r + 1) <= v)
        r++;
    return (int)r;
}

/* ---- arrays ------------------------------------------------------------ */

/* Smallest element; 0 for an empty array. */
int array_min(const int *v, int n)
{
    int m;
    int i;

    if (n <= 0)
        return 0;
    m = v[0];
    for (i = 1; i < n; i++)
        if (v[i] < m)
            m = v[i];
    return m;
}

/* Largest element; 0 for an empty array. */
int array_max(const int *v, int n)
{
    int m;
    int i;

    if (n <= 0)
        return 0;
    m = v[0];
    for (i = 1; i < n; i++)
        if (v[i] > m)
            m = v[i];
    return m;
}

/* Sum of all elements. */
int array_sum(const int *v, int n)
{
    int s = 0;
    int i;

    for (i = 0; i < n; i++)
        s += v[i];
    return s;
}

/* Mean of the elements times ten, so one decimal survives; 0 when empty. */
int array_mean_x10(const int *v, int n)
{
    if (n <= 0)
        return 0;
    return (int)((long)array_sum(v, n) * 10 / n);
}

/* Largest minus smallest element. */
int array_range(const int *v, int n)
{
    return array_max(v, n) - array_min(v, n);
}

/* How many elements are greater than zero. */
int array_count_positive(const int *v, int n)
{
    int c = 0;
    int i;

    for (i = 0; i < n; i++)
        if (v[i] > 0)
            c++;
    return c;
}

/* How many elements equal x. */
int array_count_equal(const int *v, int n, int x)
{
    int c = 0;
    int i;

    for (i = 0; i < n; i++)
        if (v[i] == x)
            c++;
    return c;
}

/* Index of the first element equal to x, or -1. */
int array_index_of(const int *v, int n, int x)
{
    int i;

    for (i = 0; i < n; i++)
        if (v[i] == x)
            return i;
    return -1;
}

/* True when every element is greater than zero (an empty array counts). */
int array_all_positive(const int *v, int n)
{
    int i;

    for (i = 0; i < n; i++)
        if (v[i] <= 0)
            return 0;
    return 1;
}

/* True when at least one element is below zero. */
int array_any_negative(const int *v, int n)
{
    int i;

    for (i = 0; i < n; i++)
        if (v[i] < 0)
            return 1;
    return 0;
}

/* Reverse the array in place. */
void array_reverse(int *v, int n)
{
    int i;
    int t;

    for (i = 0; i < n / 2; i++) {
        t = v[i];
        v[i] = v[n - 1 - i];
        v[n - 1 - i] = t;
    }
}

/* Copy n elements; the arrays must not overlap. */
void array_copy(int *dst, const int *src, int n)
{
    int i;

    for (i = 0; i < n; i++)
        dst[i] = src[i];
}

/* Set every element to x. */
void array_fill(int *v, int n, int x)
{
    int i;

    for (i = 0; i < n; i++)
        v[i] = x;
}

/* True when the elements never decrease. */
int array_is_sorted(const int *v, int n)
{
    int i;

    for (i = 1; i < n; i++)
        if (v[i] < v[i - 1])
            return 0;
    return 1;
}

/* Dot product of two arrays of the same length. */
int array_dot(const int *a, const int *b, int n)
{
    int s = 0;
    int i;

    for (i = 0; i < n; i++)
        s += a[i] * b[i];
    return s;
}

/* out[i] = v[0] + ... + v[i]. */
void array_prefix_sum(int *out, const int *v, int n)
{
    int s = 0;
    int i;

    for (i = 0; i < n; i++) {
        s += v[i];
        out[i] = s;
    }
}

/* ---- windows ----------------------------------------------------------- */

/* Sum of `count` elements starting at index `from`; the window is cut at
 * the ends of the array, so a window that runs past n adds what is there. */
int window_sum(const int *v, int n, int from, int count)
{
    int s = 0;
    int i;

    if (from < 0)
        from = 0;
    for (i = from; i <= from + count && i < n; i++)
        s += v[i];
    return s;
}

/* Largest of `count` elements starting at index `from`, cut like
 * window_sum; 0 when the window holds nothing. */
int window_max(const int *v, int n, int from, int count)
{
    int m = 0;
    int i;
    int seen = 0;

    if (from < 0)
        from = 0;
    for (i = from; i < from + count && i < n; i++) {
        if (!seen || v[i] > m)
            m = v[i];
        seen = 1;
    }
    return m;
}

/* ---- histograms -------------------------------------------------------- */

/* Which bucket a value falls into: width-sized steps starting at 0. */
int bucket_of(int value, int width)
{
    if (width <= 0 || value < 0)
        return 0;
    return value / width;
}

/* Count one value; values past the last bucket go into the last one. */
void histogram_add(int *h, int buckets, int value, int width)
{
    int b;

    if (buckets <= 0)
        return;
    b = bucket_of(value, width);
    if (b >= buckets)
        b = buckets - 1;
    h[b]++;
}

/* Number of values counted in all buckets. */
int histogram_total(const int *h, int buckets)
{
    return array_sum(h, buckets);
}

/* Index of the fullest bucket (the first one on a tie), or -1 if none. */
int histogram_peak(const int *h, int buckets)
{
    int best = -1;
    int i;

    for (i = 0; i < buckets; i++)
        if (best < 0 || h[i] > h[best])
            best = i;
    return best;
}
