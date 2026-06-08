/* we_trace.c -- implementation of the WPE_TRACE facility (see we_trace.h).
 *
 * The whole translation unit compiles to nothing unless WPE_TRACE is defined,
 * so it is safe to keep in the build at all times. */
#include "we_trace.h"

#ifdef WPE_TRACE

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* wpe_trace_file -- the lazily-opened append handle for the trace file.
 * Opened once, on the first trace; returns NULL (tracing silently off) if the
 * file cannot be opened, and does not retry. */
static FILE *wpe_trace_file(void)
{
    static FILE *fp = NULL;
    static int tried = 0;

    if (!tried) {
        const char *path = getenv("WPE_TRACE_FILE");
        if (!path || !*path)
            path = "/tmp/xwpe-trace.txt";
        fp = fopen(path, "a");
        tried = 1;
    }
    return fp;
}

void wpe_trace_write(const char *category, const char *fmt, ...)
{
    FILE *fp = wpe_trace_file();
    va_list ap;

    if (!fp)
        return;
    fprintf(fp, "[%s] ", category ? category : "?");
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fflush(fp);
}

#endif /* WPE_TRACE */
