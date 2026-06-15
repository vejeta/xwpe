/* we_clip.h -- OS clipboard integration for xwpe.
 *
 * Two layers:
 *  - pure helpers (base64, OSC 52 framing) with no editor/X11 dependency, so
 *    they unit-test standalone;
 *  - a backend seam (function pointers) that the X11 and terminal UIs fill in,
 *    defaulting to no-op so a headless / --without-x build links clean.
 *
 * Copyright (C) 2026 Juan Manuel Mendez Rey
 * Released under the GNU General Public License, version 2.
 */
#ifndef WE_CLIP_H
#define WE_CLIP_H

/* e_b64_encode - standard RFC 4648 base64.
 *
 * Encodes in[0..inlen) into out as a NUL-terminated base64 string.  Used to
 * frame clipboard text for OSC 52 (terminal) where the payload must be ASCII.
 *
 * @in:     bytes to encode (may contain NULs / UTF-8).
 * @inlen:  number of input bytes (>= 0).
 * @out:    destination buffer.
 * @outcap: capacity of out; must be >= 4*((inlen+2)/3) + 1.
 * Return: encoded length excluding the NUL, or -1 if outcap is too small.
 */
int e_b64_encode(const unsigned char *in, int inlen, char *out, int outcap);

/* e_clip_osc52_write - put text on the OS clipboard from a terminal.
 *
 * Frames text as the OSC 52 "set clipboard" sequence
 *   ESC ] 52 ; c ; base64(text) BEL
 * and writes it to fd (the controlling tty).  The terminal emulator performs
 * the real OS-clipboard write, so this works locally AND over SSH, with no X11
 * and no helper process.  Supported by kitty, foot, wezterm, iTerm2, xterm
 * (allowWindowOps) and tmux (set-clipboard on).
 *
 * @fd:   tty file descriptor to write the escape to.
 * @utf8: clipboard text (UTF-8), need not be NUL-terminated.
 * @len:  number of bytes of utf8.
 * Return: 0 on success; -1 if len exceeds the emulator-safe cap
 *         (E_CLIP_OSC52_MAX) or the write fails.  Callers must surface the cap
 *         to the user (status line) -- never silently truncate.
 */
int e_clip_osc52_write(int fd, const char *utf8, int len);

/* Emulator-safe cap on the pre-base64 payload.  Most terminals bound OSC 52
 * around 74-100 KB of base64; we cap the raw text well under that. */
#define E_CLIP_OSC52_MAX 74000

/* Backend seam: the active UI front-end installs its OS-clipboard writer here.
 * e_clip_os_set(text, len) puts `len` bytes of UTF-8 on the system clipboard --
 * the terminal front-end frames it as OSC 52 (we_term.c), the X11 front-end
 * owns the X selections (we_xterm.c).  Defaults to a no-op, so a headless /
 * --without-x build links and "copy" simply does not reach the OS.  The
 * editor's Copy/Cut call it right after filling the internal clipboard, which
 * is what makes a plain ^C / ^Ins also land on the OS clipboard. */
extern void (*e_clip_os_set)(const char *utf8, int len);

/* e_clip_os_get - fetch the OS clipboard for Paste.  Returns a malloc'd,
 * NUL-terminated UTF-8 string (caller frees) and sets *len to its byte length,
 * or NULL to mean "use xwpe's own internal clipboard" -- which is what the
 * caller does when no external app owns the selection, when WE are the owner
 * (so the internal copy is authoritative), or in a terminal (OSC 52 read is
 * unreliable, so console Paste stays internal + the emulator's own paste).
 * The X11 front-end installs the real reader; the default returns NULL. */
extern char *(*e_clip_os_get)(int *len);

#endif /* WE_CLIP_H */
