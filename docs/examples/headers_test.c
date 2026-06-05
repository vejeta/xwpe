#include <stdio.h>
#include "headers_test.h"

/* Header-dependency demo for xwpe's automatic recompilation (F9 / Make).
   Editing headers_test.h recompiles this file; editing a header that is
   only named in a comment or inside a disabled block does NOT. */

// #include "fake.h"       /* commented out: must NOT trigger a recompile */

#if 0
#include "disabled.h"      /* disabled by #if 0: must NOT trigger a recompile */
#endif

int get_value(void) { return 42; }

int main(void)
{
    printf("value = %d\n", get_value());
    return 0;
}
