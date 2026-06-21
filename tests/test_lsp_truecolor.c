/* test_lsp_truecolor.c -- unit test for the semantic-token truecolor palette
 * (we_lsp.c): the single source of truth that both the X11/Xft and the
 * direct-colour console renderers use to give a few semantic-token categories a
 * real 24-bit colour the 16-colour palette cannot express.  The headline case
 * is methods/functions, which fell back to bright red because the 16 colours
 * have no orange; here they map to a dedicated orange slot (#FF8C00).  A pure
 * lookup, so it is checked without a server or a terminal. */
#include <stdio.h>
#include <string.h>

#include "../we_lsp.h"

static int fails;

static void chk(const char *name, int got, int want)
{
 if (got == want)
  printf("ok   - %s\n", name);
 else
 {  printf("FAIL - %s (want %d, got %d)\n", name, want, got);  fails++;  }
}

int main(void)
{
 int r, g, b;

 /* method / function / macro share the one orange slot (slot 0). */
 chk("method -> slot 0",   e_lsp_sem_truecolor("method"),   0);
 chk("function -> slot 0", e_lsp_sem_truecolor("function"), 0);
 chk("macro -> slot 0",    e_lsp_sem_truecolor("macro"),    0);

 /* type-like categories share the one teal slot (slot 1). */
 chk("type -> slot 1",      e_lsp_sem_truecolor("type"),      1);
 chk("class -> slot 1",     e_lsp_sem_truecolor("class"),     1);
 chk("interface -> slot 1", e_lsp_sem_truecolor("interface"), 1);
 chk("enum -> slot 1",      e_lsp_sem_truecolor("enum"),      1);

 /* categories the 16 colours handle well keep the 16-colour mapping. */
 chk("keyword -> none",  e_lsp_sem_truecolor("keyword"),  LSP_SEM_TC_NONE);
 chk("variable -> none", e_lsp_sem_truecolor("variable"), LSP_SEM_TC_NONE);
 chk("NULL -> none",     e_lsp_sem_truecolor(NULL),       LSP_SEM_TC_NONE);
 chk("unknown -> none",  e_lsp_sem_truecolor("nonsense"), LSP_SEM_TC_NONE);

 /* slot 0 is orange #FF8C00. */
 r = g = b = -1;
 chk("slot 0 resolves", e_lsp_sem_slot_rgb(0, &r, &g, &b), 1);
 chk("slot 0 R = 0xFF", r, 0xFF);
 chk("slot 0 G = 0x8C", g, 0x8C);
 chk("slot 0 B = 0x00", b, 0x00);

 /* slot 1 is teal #4EC9B0. */
 r = g = b = -1;
 chk("slot 1 resolves", e_lsp_sem_slot_rgb(1, &r, &g, &b), 1);
 chk("slot 1 R = 0x4E", r, 0x4E);
 chk("slot 1 G = 0xC9", g, 0xC9);
 chk("slot 1 B = 0xB0", b, 0xB0);

 /* out-of-range slots are rejected, not read past the table. */
 chk("negative slot rejected", e_lsp_sem_slot_rgb(-1, &r, &g, &b), 0);
 chk("huge slot rejected",     e_lsp_sem_slot_rgb(99, &r, &g, &b), 0);

 /* NULL out-params are tolerated (caller may want just the validity check). */
 chk("NULL out-params ok", e_lsp_sem_slot_rgb(0, NULL, NULL, NULL), 1);

 if (fails)
 {  printf("%d FAILED\n", fails);  return 1;  }
 printf("all passed\n");
 return 0;
}
