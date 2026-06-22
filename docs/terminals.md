# Terminal & console notes

**Any terminal emulator** (xterm, kitty, gnome-terminal, iTerm2, tmux): the
mouse -- pointer, click, window drag/resize -- works natively over the xterm
protocol, no extra setup. (On macOS, enable Option-as-Meta so the `Alt-` keys
reach xwpe; see [docs/install/macos.md](install/macos.md).)

## OS clipboard

Cut / Copy / Paste are one clipboard, shared with the system: `^C` / `^Ins`
(Copy) and `Shift-Del` (Cut) put the selection on the **real OS clipboard**, and
`^V` / `Shift-Ins` (Paste) takes whatever was last copied in **any** app. In a
terminal (`wpe`) Copy uses the OSC 52 escape, so it works **even over SSH** --
provided the emulator allows it (kitty, foot, wezterm, iTerm2 do; `xterm` needs
`allowWindowOps`; in tmux `set -g set-clipboard on`); pasting from another app
there is the emulator's own paste (`Shift-Insert`). In X11 (`xwpe`) Copy owns
both the PRIMARY (middle-click) and CLIPBOARD (`Ctrl-V`) selections as UTF-8, and
`^V` pulls from them. Works on macOS too (OSC 52 in iTerm2/kitty; XQuartz
pasteboard sync for the X11 build).

## Multiplexers -- prefer `tmux` over GNU `screen`

xwpe uses the modern SGR mouse protocol (`ESC[<...M`). `tmux` forwards it, so the
mouse works inside a tmux session. GNU `screen` (depending on version/`TERM`)
does **not** pass SGR mouse through, so clicks and drags arrive as raw bytes that
get **typed into the editor** instead of moving the cursor. This is a `screen`
limitation, not an xwpe bug -- the same xwpe works fine in the same terminal
*outside* screen. Use `tmux`, or run xwpe directly without a multiplexer, if you
need the mouse. (Keyboard-only use is unaffected in either.)

## Linux console (no X, Ctrl+Alt+F2)

Bitmap fonts look tiny on HiDPI, and the mouse needs the GPM daemon:

```sh
sudo apt install console-terminus
setfont Lat15-Terminus32x16            # readable on HiDPI
sudo apt install gpm                   # then: pointer, click, window drag/resize on a bare VT
```

With `gpm` running you get the full mouse on a bare VT, no X needed.
