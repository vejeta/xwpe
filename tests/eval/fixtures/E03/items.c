#include "items.h"

/* compute_total adds qty * unit_cents over every line. */
long compute_total(const struct line *lines, int n)
{
    long sum = 0;
    int i;

    for (i = 0; i < n; i++)
        sum += (long)lines[i].qty * lines[i].unit_cents;
    return sum;
}
