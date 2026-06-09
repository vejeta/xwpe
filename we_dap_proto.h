/* we_dap_proto.h -- Debug Adapter Protocol wire format (transport core).
 *
 * Pure, xwpe-free layer for the DAP debug client (task #49): it turns the raw
 * byte stream from a debug adapter (Delve, lldb-dap, Metals, ...) into parsed
 * JSON messages and builds framed requests to send back.  It has NO dependency
 * on the editor (no globals, no fd-loop, no UI) so it can be unit-tested in
 * isolation; the session/UI wiring lives in we_dap.c.
 *
 * DAP frames are LSP-style:
 *
 *     Content-Length: <N>\r\n
 *     \r\n
 *     <N bytes of UTF-8 JSON>
 */
#ifndef WE_DAP_PROTO_H
#define WE_DAP_PROTO_H

#include <stddef.h>
#include <json-c/json.h>

/* Streaming reader: append received bytes, pop complete messages.  The adapter
 * may deliver a frame split across several reads, or several frames in one
 * read, so the reader buffers until a whole frame is present. */
typedef struct {
 char  *buf;
 size_t len;   /* bytes currently buffered */
 size_t cap;   /* allocated capacity       */
} e_dap_reader;

void e_dap_reader_init(e_dap_reader *r);
void e_dap_reader_free(e_dap_reader *r);

/* Append raw bytes received from the adapter's output. */
void e_dap_reader_push(e_dap_reader *r, const char *data, size_t n);

/* Pop the next complete message as a parsed json_object, or NULL if no whole
 * frame is buffered yet.  The caller owns the result (json_object_put()).
 * If a frame is present but malformed, *err is set non-zero and the bad frame
 * is consumed so the stream can resync; otherwise *err is 0. */
struct json_object *e_dap_reader_pop(e_dap_reader *r, int *err);

/* Build a framed DAP request ("Content-Length: N\r\n\r\n{json}").  Returns a
 * malloc'd NUL-terminated string and, if out_len != NULL, its byte length.
 * `arguments` may be NULL and is NOT consumed (caller still owns it).
 * Returns NULL on allocation failure. */
char *e_dap_build_request(int seq, const char *command,
                          struct json_object *arguments, size_t *out_len);

#endif /* WE_DAP_PROTO_H */
