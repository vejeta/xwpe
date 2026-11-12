#include <stdio.h>
#include "calc.h"

int main(int argc, char **argv)
{
    int n;

    if (argc < 2) {
        fprintf(stderr, "usage: calc 'a=1;b=2'\n");
        return 2;
    }
    n = build_unit(argv[1]);
    if (n < 0) {
        fprintf(stderr, "calc: cannot compile\n");
        return 1;
    }
    printf("%d statements\n", n);
    return 0;
}
