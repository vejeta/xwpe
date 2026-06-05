#include <stdio.h>
#include <string.h>

/* Interactive-program demo.  Under Ctrl-F9 (Run) or while debugging, the
   prompt appears in the Messages window and you type the answer there;
   the program blocks on fgets() until you press Enter.  The UTF-8 output
   also exercises wide-character rendering. */

int main(void)
{
    char name[100];

    printf("What is your name? ");
    fflush(stdout);
    fgets(name, sizeof(name), stdin);
    name[strcspn(name, "\n")] = '\0';

    printf("Hello, %s! Welcome to xwpe 🎉\n", name);
    return 0;
}
