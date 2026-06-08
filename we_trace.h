/* we_trace.h -- compile-time-guarded trace facility for xwpe.
 *
 * One place for the throwaway debugging that used to be hand-written as
 * fopen("/tmp/xwpe-SOMETHING.txt") scattered through the code (the jdb
 * tracer, the mouse/render dumps, ...).  Build with
 *
 *     ./configure --enable-trace          (or add -DWPE_TRACE to CFLAGS)
 *
 * to turn it on.  When it is off -- the default -- every WPE_TRACE() call
 * expands to nothing and costs nothing at run time.
 *
 * Output is appended to $WPE_TRACE_FILE when that variable is set, otherwise
 * to /tmp/xwpe-trace.txt.  Each call writes "[category] " followed by your
 * formatted text (include your own '\n'), so a subsystem can tag its trace
 * and you can grep one out:
 *
 *     WPE_TRACE("jdb",   "recv %d bytes: %s\n", n, buf);
 *     WPE_TRACE("mouse", "drag gb=%d y=%d x=%d\n", g_mouse_buttons, y, x);
 *
 *     $ WPE_TRACE_FILE=/tmp/t.log xwpe foo.c ; grep '^\[mouse\]' /tmp/t.log
 */
#ifndef WE_TRACE_H
#define WE_TRACE_H

#ifdef WPE_TRACE

/* Append one "[category] ..." record to the trace file.  Defined in
   we_trace.c.  Call it through the WPE_TRACE() macro, not directly. */
void wpe_trace_write(const char *category, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#define WPE_TRACE(category, ...) wpe_trace_write((category), __VA_ARGS__)

#else  /* !WPE_TRACE -- a true no-op, zero run-time cost */

#define WPE_TRACE(category, ...) ((void)0)

#endif /* WPE_TRACE */

#endif /* WE_TRACE_H */
