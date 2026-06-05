/* paint.c -- a program that PAINTS its screen (ANSI colour + cursor
   positioning), to exercise the Borland Alt-F5 "User Screen".

   Build it in wpe with F9, run it with Ctrl-F9, then press Alt-F5: the editor
   steps aside and the User Screen shows exactly what this program drew --
   colours and box, which the line-oriented Messages window cannot represent. */
#include <stdio.h>

int main(void)
{
    printf("\033[2J\033[H");                 /* clear screen, home */
    printf("\033[1;1H\033[44;97m  xwpe User Screen demo  \033[0m\n");
    printf("\033[3;5H\033[31mred\033[0m  "
           "\033[32mgreen\033[0m  "
           "\033[33myellow\033[0m  "
           "\033[34mblue\033[0m  "
           "\033[35mmagenta\033[0m  "
           "\033[36mcyan\033[0m\n");
    /* a little box, drawn with cursor positioning */
    printf("\033[5;5H┌───────┐");
    printf("\033[6;5H│ hello │");
    printf("\033[7;5H└───────┘");
    printf("\033[9;1HIf you can see the colours and the box, Alt-F5 works.\n");
    fflush(stdout);
    return 0;
}
