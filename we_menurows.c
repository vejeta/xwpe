/* we_menurows.c -- where the highlight of a dropdown menu goes next.
   See we_menurows.h; no editor headers, so tests/test_menurows.c links it alone. */

#include "we_menurows.h"

int wpe_menu_step_row(const void *rows, int count, int from, int step,
                      wpe_menu_row_pick pick)
{
  int i, n = from;

  for(i = 0; i < count; i++)
  {
    n += step;
    if(n < 0)
      n = count - 1;
    else if(n >= count)
      n = 0;
    if(pick(rows, n))
      return n;
  }
  return from;
}

int wpe_menu_first_row(const void *rows, int count, wpe_menu_row_pick pick)
{
  if(count <= 0)
    return 0;
  return wpe_menu_step_row(rows, count, count - 1, 1, pick);
}

int wpe_menu_last_row(const void *rows, int count, wpe_menu_row_pick pick)
{
  if(count <= 0)
    return 0;
  return wpe_menu_step_row(rows, count, 0, -1, pick);
}
