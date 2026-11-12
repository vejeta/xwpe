#ifndef ITEMS_H
#define ITEMS_H

struct line {
    int qty;
    int unit_cents;
};

/* Total price of all lines, in cents. */
long compute_total(const struct line *lines, int n);

#endif
