#include <stdio.h>

void foo() {
    int x = "hello";
}

void bar() {
    int y = 3.14 + "world";
}

int main() {
    printf("Hello from C\n");
    foo();
    bar();
    return 0;
}
