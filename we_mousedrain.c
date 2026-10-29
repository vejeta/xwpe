/* we_mousedrain.c -- bounded emptying of the curses mouse queue.
   See we_mousedrain.h; no curses headers, so tests/test_mousedrain.c links it alone. */

#include "we_mousedrain.h"

int wpe_mouse_drain(wpe_mouse_pull pull, void *arg)
{
  int thrown = 0;
  int n;

  for(n = 0; n < WPE_MOUSE_QUEUE_MAX; n++)
  {
    if (pull(arg) <= 0)
      break;
    thrown++;
  }
  return thrown;
}
