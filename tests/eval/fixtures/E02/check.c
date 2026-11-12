#include <stdio.h>
#include "stats.h"

static int failures;

#define CHECK(name, got, want)                                           \
    do {                                                                 \
        int g_ = (got);                                                  \
        int w_ = (want);                                                 \
        if (g_ != w_) {                                                  \
            printf("FAIL %s: expected %d got %d\n", name, w_, g_);       \
            failures++;                                                  \
        }                                                                \
    } while (0)

int main(void)
{
    int v[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    int h[4] = { 0, 0, 0, 0 };
    int pre[5];
    int rev[4] = { 1, 2, 3, 4 };

    CHECK("clamp_int", clamp_int(15, 0, 10), 10);
    CHECK("abs_int", abs_int(-7), 7);
    CHECK("sign_int", sign_int(-3), -1);
    CHECK("median3", median3(9, 2, 5), 5);
    CHECK("percent_of", percent_of(1, 8), 12);
    CHECK("round_div", round_div(7, 2), 4);
    CHECK("wrap_index", wrap_index(-1, 5), 4);
    CHECK("saturating_add", saturating_add(2147483647, 5), 2147483647);
    CHECK("gcd_int", gcd_int(12, 18), 6);
    CHECK("lcm_int", lcm_int(4, 6), 12);
    CHECK("count_bits", count_bits(255u), 8);
    CHECK("digit_sum", digit_sum(-1234), 10);
    CHECK("reverse_digits", reverse_digits(1200), 21);
    CHECK("fib_iter", fib_iter(10), 55);
    CHECK("factorial_small", factorial_small(5), 120);
    CHECK("isqrt", isqrt(99), 9);
    CHECK("array_min", array_min(v, 10), 1);
    CHECK("array_max", array_max(v, 10), 10);
    CHECK("array_sum", array_sum(v, 10), 55);
    CHECK("array_mean_x10", array_mean_x10(v, 10), 55);
    CHECK("array_range", array_range(v, 10), 9);
    CHECK("array_index_of", array_index_of(v, 10, 7), 6);
    CHECK("array_is_sorted", array_is_sorted(v, 10), 1);
    CHECK("array_dot", array_dot(v, v, 3), 14);
    array_reverse(rev, 4);
    CHECK("array_reverse", rev[0] * 1000 + rev[3], 4001);
    array_prefix_sum(pre, v, 5);
    CHECK("array_prefix_sum", pre[4], 15);
    CHECK("window_sum", window_sum(v, 10, 2, 3), 12);
    CHECK("window_max", window_max(v, 10, 2, 3), 5);
    histogram_add(h, 4, 5, 10);
    histogram_add(h, 4, 45, 10);
    CHECK("histogram_total", histogram_total(h, 4), 2);
    CHECK("histogram_peak", histogram_peak(h, 4), 0);

    if (failures == 0)
        printf("all checks passed\n");
    return failures != 0;
}
