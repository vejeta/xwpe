#include <stdio.h>
#include "items.h"

int main(void)
{
    struct line basket[3] = { {2, 150}, {1, 999}, {4, 25} };
    struct line empty[1] = { {0, 0} };

    printf("basket=%ld\n", compute_total(basket, 3));
    printf("empty=%ld\n", compute_total(empty, 0));
    return 0;
}
