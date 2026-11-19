/* linebuf.h -- a C test's output reaches the log line by line.
 *
 * A test run by `make check` prints to a file, so the C library buffers its
 * output in full blocks.  A crash then loses every row printed before it and
 * the log names nothing (a NetBSD run once ended on a stack overflow with a
 * log of two lines).  This header makes stdout line-buffered before main
 * starts, so the last line in the log is the last row that ran.
 *
 * Every tests/test_*.c includes it, directly or through test_watchdog.h,
 * testdir.h or interpose.h (tests/check-interpose.sh).
 */
#ifndef WPE_TEST_LINEBUF_H
#define WPE_TEST_LINEBUF_H

#include <stdio.h>

/** wpe_test_line_buffer_stdout - Runs before main: stdout gets one flush per
    line, so a crash leaves the rows that ran in the log. */
static void __attribute__((constructor)) wpe_test_line_buffer_stdout(void)
{
 setvbuf(stdout, NULL, _IOLBF, 0);
}

#endif /* WPE_TEST_LINEBUF_H */
