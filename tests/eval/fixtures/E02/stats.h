#ifndef STATS_H
#define STATS_H

/* Scalar helpers. */
int clamp_int(int v, int lo, int hi);
int abs_int(int v);
int sign_int(int v);
int min_int(int a, int b);
int max_int(int a, int b);
int min3(int a, int b, int c);
int max3(int a, int b, int c);
int median3(int a, int b, int c);
int percent_of(int part, int whole);
int round_div(int num, int den);
int lerp_int(int a, int b, int num, int den);
int wrap_index(int i, int n);
int saturating_add(int a, int b);
int saturating_sub(int a, int b);

/* Number theory and digits. */
int gcd_int(int a, int b);
int lcm_int(int a, int b);
int is_power_of_two(unsigned v);
int count_bits(unsigned v);
int digit_sum(int v);
int reverse_digits(int v);
int is_palindrome_num(int v);
int fib_iter(int n);
int factorial_small(int n);
int isqrt(int v);

/* Arrays. */
int array_min(const int *v, int n);
int array_max(const int *v, int n);
int array_sum(const int *v, int n);
int array_mean_x10(const int *v, int n);
int array_range(const int *v, int n);
int array_count_positive(const int *v, int n);
int array_count_equal(const int *v, int n, int x);
int array_index_of(const int *v, int n, int x);
int array_all_positive(const int *v, int n);
int array_any_negative(const int *v, int n);
void array_reverse(int *v, int n);
void array_copy(int *dst, const int *src, int n);
void array_fill(int *v, int n, int x);
int array_is_sorted(const int *v, int n);
int array_dot(const int *a, const int *b, int n);
void array_prefix_sum(int *out, const int *v, int n);

/* Windows over an array: `count` elements starting at index `from`. */
int window_sum(const int *v, int n, int from, int count);
int window_max(const int *v, int n, int from, int count);

/* Histograms with fixed-width buckets. */
int bucket_of(int value, int width);
void histogram_add(int *h, int buckets, int value, int width);
int histogram_total(const int *h, int buckets);
int histogram_peak(const int *h, int buckets);

#endif
