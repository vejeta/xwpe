# xwpe

Xwpe is a development environment designed for use on UNIX systems.
Fred Kruse wrote xwpe and released it as free software under the GNU
General Public License. The user interface was designed to mimic the
Borland C and Pascal family. Extensive support is provided for
programming: syntax highlighting, integrated compiler and debugger
interface, project management, and a function-key driven menu system.

## Project history

This release continues the work of:

- **Fred Kruse** -- original author of xwpe (last official release 1.4.2,
  ca. 1996-1997).
- **Dennis Payne** -- xwpe-alpha continuation project (1.5.x series, final
  release 1.5.30a in 2006). Project home:
  https://www.identicalsoftware.com/xwpe/
- **Guus Bonnema** and other contributors via the GitHub fork at
  https://github.com/amagnasco/xwpe -- selected bug fixes integrated
  upstream here.
- **Debian xwpe contributors** (Jari Aalto, Francesco P. Lovergine,
  Andreas Tille, Helmut Grohne, Robert Millan, and others) -- the patches
  carried for years in https://salsa.debian.org/debian/xwpe/ are now
  integrated upstream and can be dropped from the Debian package.

## Current maintainer

Juan Manuel Mendez Rey \<juan.mendezr@proton.me\>, with the explicit
blessing of Dennis Payne.

## Project home

https://codeberg.org/mendezr/xwpe

Issues and contributions on Codeberg please. The previous GitHub fork at
amagnasco/xwpe is no longer the active project.

## What changed in 1.6.0

xwpe's screen buffer was a flat `char` array since 1993: 1 byte per
character, 1 byte per color attribute. This worked when terminals were
80x24, character sets were ASCII or Latin-1, and every character was
exactly 1 byte wide.

Modern terminals use UTF-8, where a single visible character can be 1 to
4 bytes (e.g. `é` = 2 bytes, `中` = 3 bytes, `⭐` = 4 bytes). The old
buffer stored each byte as a separate screen cell, causing borders to
shift, gaps to appear, and stale content after scrolling.

Version 1.6.0 replaces the byte buffer with `SCREENCELL`: a struct that
holds one decoded character (as `wchar_t`) plus its color attribute per
cell. The display loop decodes UTF-8 via `mbrtowc()` and stores one
`SCREENCELL` per visible column. The terminal refresh uses ncursesw's
`add_wch()` for wide characters and `wcwidth()` for characters that
occupy two columns (emoji, CJK). Each cell in the buffer corresponds
exactly to one column on screen -- borders align correctly regardless
of content.

This also enabled terminal resize support (via `KEY_RESIZE`) and
keyboard remapping for modern PC keyboards.

### Technical changes

| Component | Before (1.5.x) | After (1.6.0) | Why |
|-----------|----------------|---------------|-----|
| Screen buffer | `char *schirm` (2 bytes/cell: char + attr) | `SCREENCELL *schirm` (int ch + int attr) | Holds decoded `wchar_t` instead of raw bytes; 1 cell = 1 visual column |
| Buffer macros | `e_pr_char(x,y,c,frb)` via byte arithmetic `*(schirm + 2*MAXSCOL*y + 2*x)` | Same name, struct field access `schirm[y*MAXSCOL+x].ch` | Cleaner, no byte offset math, supports values > 255 |
| Display loop | `e_pr_line()`: 1 byte per iteration, `i++, j++` | `mbrtowc()` decodes multi-byte, `i += nb` (bytes consumed), `j++` (1 visual column) | One character per column regardless of byte count |
| Wide chars | Not handled | `wcwidth()` after decode; if width=2, fill second cell with space, advance j by 2 | Emoji (❌,⭐) and CJK take 2 columns |
| Terminal refresh | `addch(c)` per byte | `add_wch(&cc)` for wide chars, `addch(sp_chr[c])` for ACS, `addch(c)` for ASCII | `add_wch` is ncursesw's native wide character output |
| Change detection | `schirm[x] != altschirm[x]` (byte compare) | `schirm[idx].ch != altschirm[idx].ch` (struct field compare) | Same logic, different data type |
| Buffer alloc | `MALLOC(2 * MAXSCOL * MAXSLNS)` | `MALLOC(sizeof(SCREENCELL) * MAXSCOL * MAXSLNS)` | 8 bytes/cell instead of 2; buffer ~320KB for 80x50 |
| Save/restore | `e_gt_byte`/`e_pt_byte` (byte-by-byte copy) | `memcpy` of `SCREENCELL` structs | Full cell preserved including wide char value |
| ncurses link | `-lncurses` | `-lncursesw` | Wide character API (`add_wch`, `setcchar`) only in ncursesw |
| Scrollbar chars | `MCI='+'`, `MCA='0'` (ASCII) | `MCI=7` (ACS_S9), `MCA=11` (ACS_DIAMOND) | Restores visual style from 1990s terminal aesthetics |
| Resize | Not handled | `KEY_RESIZE` → realloc buffers, resize windows, repaint | Modern terminals change size dynamically |
| Keyboard | VT100/Sun mapping | PC keyboard mapping (PageUp, Home, End, Delete) | Original mapping was for hardware terminals no longer in use |
| GPM mouse | `Gpm_Open()` unconditionally | Check `/dev/gpmctl` first | Avoids "gpm: not found" error when daemon not running |

## Building

This release ships with the original autoconf-based build (configure.in
+ handwritten Makefile.in) for continuity with Payne's 1.5.30a. A
modernisation to autoreconf-driven autotools is planned for the next
release.

```sh
LIBS="-lncursesw" \
CFLAGS="-DNCURSES -std=gnu17 -I/usr/include/ncursesw \
  -D_XOPEN_SOURCE_EXTENDED -DNCURSES_WIDECHAR=1" \
./configure
make
sudo make install
```

Common configure-time dependencies on Debian/Ubuntu:

```sh
sudo apt install build-essential libncurses-dev libx11-dev libgpm-dev
```

Run the editor in terminal mode with `wpe`, in X11 mode with `xwpe`.

## Testing

See [tests/README.md](tests/README.md) for the automated test suite.

## Licence

GPL-2. See `COPYING`.
