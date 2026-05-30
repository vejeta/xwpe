# Mouse Support: ncurses integration and GPM migration

## Background

xwpe has three mouse backends, all designed around Fred Kruse's 1993
abstraction:

- **`fk_mouse`**: function pointer (pluggable backend)
- **`e_mouse`**: global struct `{x, y, k}` (coordinates + button state)
- **`e_mshit()`**: polling wrapper that calls `fk_mouse` and divides
  coordinates by 8
- **`we_mouse.c`**: 1189 lines of UI logic (menus, scrollbars, resize,
  drag, text selection) -- completely backend-independent

Kruse implemented only the X11 backend (`we_xterm.c`). The terminal
backend (`fk_t_mouse` in `we_term.c`) was a 4-line stub returning 0.

### Suraci's GPM backend (1998)

Sebastiano Suraci wrote `we_gpm.c` in 1998 (xwpe 1.5.17a), adding
mouse support on the Linux console via GPM (General Purpose Mouse).
His implementation:

- `WpeGpmMouseInit()`: calls `Gpm_Open()` with `gpm_zerobased = 1`,
  `eventMask = ~0`, sets `gpm_handler = WpeGpmHandler`
- `WpeGpmHandler()`: callback invoked by ncurses when GPM events
  arrive. Sets `e_mouse.x/y/k` directly, calls `GPM_DRAWPOINTER`,
  returns negative value to signal a mouse event
- `WpeGpmMouse()`: polling function for `fk_mouse`, uses
  `Gpm_GetSnapshot`/`Gpm_GetEvent`, returns coordinates * 8

This worked correctly with ncurses 4.x (1998), which had NO internal
GPM support.

## The problem (2026)

Debian's ncurses 6.x (libncursesw6) has **built-in GPM integration**.
When `mousemask()` is called, ncurses internally:

1. Checks if `TERM` contains "linux"
2. Calls its own `Gpm_Open()` with `eventMask = GPM_DOWN | GPM_UP`
3. Reads events from `gpm_fd` internally
4. Converts GPM 1-based coordinates to 0-based
5. Delivers events as `KEY_MOUSE` via `getch()` + `getmouse()`

When xwpe ALSO calls `Gpm_Open()` directly (Suraci's code), there are
**two consumers competing for the same `gpm_fd`**:

### Conflicting event masks

- Suraci: `eventMask = ~0` (all events including motion)
- ncurses: `eventMask = GPM_DOWN | GPM_UP` (clicks only)
- The second `Gpm_Open()` overwrites the mask on the server

### Conflicting coordinate systems

- Suraci: `gpm_zerobased = 1` (0-based coordinates)
- ncurses: expects 1-based, subtracts 1 internally
- After ncurses reopens, coordinate system is ambiguous

### Race condition on events

- Events from the single `gpm_fd` can be consumed by either handler
- Sometimes Suraci's handler gets it, sometimes ncurses does
- This causes the "sometimes clicks work, sometimes they don't" symptom

### What other applications do

| Application | Direct Gpm_Open? | mousemask()? | GPM works? |
|-------------|:---:|:---:|:---:|
| nano        | no  | yes | yes |
| htop        | no  | yes | yes |
| mc          | yes | no (S-Lang) | yes |
| xwpe        | yes | yes | **broken** |

**Rule: use one mechanism or the other, never both.**

## What we tried and didn't work

### Attempt 1: Skip mousemask() when GPM active

Rationale: let Suraci's handler work alone without ncurses interference.

Result: **GPM cursor disappeared**. Debian's ncurses needs `mousemask()`
to process GPM events at all -- even the handler callback isn't invoked
without it.

### Attempt 2: flushinp() / tcflush to drain residual Tab

Rationale: Tab key was being duplicated on console, assumed to be a
buffer residue from GPM interaction.

Result: **No effect**. The duplicate Tab came from `fk_t_mouse()`
polling `getch()` with a 50ms timeout, which stole keyboard input and
re-injected it via `ungetch()`. Fixed separately by guarding the
polling with `wpe_ncurses_mouse_active`.

### Attempt 3: Retry e_file_window on phantom Tab

Rationale: if DirTree panel returned Tab immediately, retry once.

Result: **Made it worse** -- required extra Tabs in other panels too.
The phantom Tab was a symptom of the getch() polling, not a GPM issue.

## The solution: remove direct GPM, let ncurses handle it

Since ncurses 6.x handles GPM transparently via `mousemask()`, we:

1. **Remove `WpeGpmMouseInit()` call** from `WpeDllInit()` in we_term.c
2. **Always use `fk_t_mouse`** (ncurses mouse backend) for terminal mode
3. **`mousemask(ALL_MOUSE_EVENTS)`** enables GPM automatically on console
4. **`KEY_MOUSE` + `getmouse()`** in `e_t_getch()` handles all events
5. **Keep `we_gpm.c`** in the tree (for reference and `--without-gpm`
   builds) but don't call `WpeGpmMouseInit()`
6. **Remove `wpe_ncurses_mouse_active` flag** -- ncurses mouse is always
   active when `mousemask()` succeeds

The result:
- Terminal emulators: xterm mouse protocol (already working)
- Console with GPM: ncurses internal GPM integration (fixed)
- Console without GPM: no mouse, keyboard works correctly (fixed)

## Kruse's architecture (1993)

The fact that this migration was possible -- swapping Suraci's GPM
backend for ncurses mouse -- without touching a single line of
`we_mouse.c` (the 1189 lines of UI logic) is a testament to Kruse's
engineering. The `fk_mouse` function pointer abstraction he designed
in 1993 at the Technical University of Berlin is textbook Unix:
clean interfaces, no platform-specific code in the logic layer, and
placeholder stubs ready for future backends.
