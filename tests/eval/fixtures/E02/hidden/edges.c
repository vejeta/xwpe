/* Held-out edge cases for window_sum, run after the agent has finished.
 *
 * The array is heap allocated at exactly its length so that a read past the
 * end is reported by the address sanitizer, and every window below ends
 * before, at or past the end of the array.  The expected values are the sums
 * of the elements that exist inside the window.
 */
#include <stdio.h>
#include <stdlib.h>
#include "stats.h"

#define LEN 10

static int failures;

static void check(const int *v, int from, int count, int want)
{
    int got = window_sum(v, LEN, from, count);

    if (got != want) {
        printf("FAIL window_sum(from=%d, count=%d): expected %d got %d\n",
               from, count, want, got);
        failures++;
    }
}

int main(void)
{
    int *v = malloc(LEN * sizeof *v);
    int i;

    if (v == NULL)
        return 2;
    for (i = 0; i < LEN; i++)
        v[i] = i + 1;

    check(v, 0, 0, 0);
    check(v, 5, 0, 0);
    check(v, 0, 1, 1);
    check(v, 3, 1, 4);
    check(v, 5, 2, 13);
    check(v, 2, 3, 12);
    check(v, 7, 3, 27);
    check(v, 0, 10, 55);
    check(v, 8, 5, 19);
    check(v, 9, 1, 10);
    check(v, 0, 11, 55);
    check(v, 10, 3, 0);

    free(v);
    if (failures == 0)
        printf("all edge checks passed\n");
    return failures != 0;
}
