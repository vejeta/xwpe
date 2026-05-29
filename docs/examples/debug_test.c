#include <stdio.h>

int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

int main() {
    int x = 5;
    int result = factorial(x);
    printf("factorial(%d) = %d\n", x, result);

    /* deliberate segfault for debugging tutorial */
    int *ptr = NULL;
    /* uncomment next line to trigger crash: */
    /* *ptr = 42; */

    return 0;
}
