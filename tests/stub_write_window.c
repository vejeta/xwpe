/* stub_write_window.c -- the write owner's question about a window, played
 * by a test that links the window-kind owner (we_winkind.c) without the
 * write owner (we_write.c).
 *
 * we_winkind.c asks wpe_write_window_is_a_file before it answers a question
 * about a file's name.  A test that does not link the real owner links this
 * answer instead: a window is a file unless the test sets
 * stub_window_is_a_file to 0 to play a window whose text came from a pipe.
 */
#include "../edit.h"

int stub_window_is_a_file = 1;

int wpe_write_window_is_a_file(const FENSTER *f)
{
 (void)f;
 return(stub_window_is_a_file);
}
