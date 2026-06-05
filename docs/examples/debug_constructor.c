#include <stdio.h>

/* Start-symbol demo for the debugger.  setup() runs before main() thanks
   to the constructor attribute.  By default xwpe breaks at main(); set the
   start symbol to "setup" (Options -> Compiler -> start symbol) to stop
   here instead, before main() is reached. */

void setup(void) __attribute__((constructor));

void setup(void)
{
    printf("setup() called before main\n");
}

int main(void)
{
    printf("main() called\n");
    return 0;
}
