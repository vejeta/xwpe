# Four goldens to approve

Each golden is a short key script and what today's `wpe` did with it. Once
approved, the model must agree with it and the editor must keep doing it.
In the pictures `|` is the cursor, `$` ends a line, `~` marks a row that
wraps on into the next one. Every step passed the editor's self-checks.
Approve a golden by adding its line to `APPROVED`; say no and it is
dropped or re-recorded.

## typing_wraps (plain text, Max. Columns 120)

Manual: "After this number of columns a word wrap is performed."

Re-recorded on 2026-11-12, because typing now wraps where the manual
says. The old recording wrapped at the 119th character; this one types
two more characters so that it still reaches the margin.

- A line of 116 characters, cursor at its end. Typing `Z`, `Q`, `R`, `S`:
  they go in, no wrap yet. The line is now 120 characters, as many as
  Max. Columns, and it is still one row.
- Typing `T`, the 121st character: the line wraps at the last blank. The
  first row keeps `w000 ... w021 ` (with its blank) and wraps on;
  `w022xyZQRST` becomes the next row, cursor after the `T`. The file
  still has one line there.
- Home, typing `A` at the start of that second row: `Aw022xyZQRST`.
- On the next line `next`, typing `!` at its end: `next!`.

What changed from the old recording: a row now holds all 120 characters
of Max. Columns, and the wrap comes with the next character typed. Before,
a row took only 118 and the 119th wrapped it.

To decide: the blank where the line wrapped stays at the end of the first
row (it is still in the file, between `w021` and `w022`).

## enter_autoindent (C file, auto-indent on)

Manual: "The next (new) line is indented to the beginning of the previous
line." The compatibility chapter says one Ctrl-U takes back a Return and
its indent; this golden does not press Ctrl-U, so it does not show that.

- `    int a = 1;|$` then Return: a new line with four blanks, cursor after
  them: `    |$`. Typing `b` gives `    b|$`.
- Return again: another line `    |$`.
- Home, Return on that line of blanks: an empty line goes in above it and
  the cursor stays at column 0 of the blanks, with no indent added (the
  line above it is now empty).
- Up to the empty line, End (stays at 0), Left, Left: the cursor is in
  `    b` before the `b`. Return: `    $` stays above and `    b` goes down
  with its indent, cursor before the `b`.

To decide: a Return leaves the line of blanks it indented as it was
(`    $`), so pressing Return twice leaves trailing blanks in the file.

## backspace_joins (plain text)

Manual: "Backspace: delete a character backwards"; at the start of a line
it joins the line to the one above.

- `one$ two$ $ three four$ five$`. Down to `two`, Backspace: `onetwo$`,
  cursor between `one` and `two`.
- Down to the empty line, Backspace: the empty line goes, cursor at the
  end of `onetwo`. Two more Backspaces: `onet`.
- Down to `three four`, Home, Backspace: `onetthree four$`, cursor after
  `onet`.

## ctrl_y (plain text)

Manual: "Ctrl Y: delete a whole line". The compatibility chapter says the
only line can be taken back by Ctrl-U too; this golden does not press
Ctrl-U, so it does not show that.

- `one$ two two$ three$`, cursor in `two two` at column 2. Ctrl-Y: `two two`
  goes, the cursor stays at column 2 of `three`.
- Ctrl-Y: `three` goes (it was the last line), the cursor goes up to `one`.
- Ctrl-Y on the only line: the window is empty, cursor at the start.
- Typing `x`: `x`.

To decide: after the only line went, the row left has no line break, so
saving now writes `x` with no newline at the end of the file.

## tab_autoindent (the manual and the code disagree)

This is not a golden. The model has a rule from the manual and a rule from
the code for the same key, and they say different things.

Manual (Automatic Indentation, help.xwpe_eng lines 2045 to 2053): with
auto-indent on, Tab in front of the first visible character indents the
line like the line above. Behind that column it indents the line N
characters more than the line above, and N is the number set for
Tabstops (8 by default). Behind that, or between visible characters, it
puts in a tab.

Editor: the same in front of the first visible character, but the "N
more" is the number set for Auto Ind. Col. (3 by default), not Tabstops.
So with the defaults the manual gives 8 more columns and the editor 3.

To decide: which one is right. If the manual, Tab indents by Tabstops and
the code changes. If the editor, the manual's sentence is wrong and gets
corrected. Until you decide the model follows the editor, and its rule is
marked as coming from the code, so an agreement with the editor on this
key proves nothing.

# Goldens of the selection block mode and the trim of an auto-indent

Recorded on the tree with Options > Editor > SelEction blocK (Options2 bit
0x400) or Trim Auto-indent (0x800) on. They are not in `APPROVED`: a CUA
user decides first. `[` and `]` mark the selection in the pictures.

## sel_typing_replaces (plain text, selection mode)

- `[hello] world$ second line$`, cursor after `hello` (Shift-Right five
  times). Typing `J`: `J| world$`; the selection is gone, one Ctrl-U step.
- Ctrl-U: `hello world`, the selection `[hello]` marked again, cursor after
  it. Ctrl-R: `J| world` again.

## sel_move_collapses

- Shift-Right three times marks `[hel]lo`. Right (no Shift): the mark goes,
  the cursor is after `hell`. Shift-Down marks from there to the same column
  of the next line; Left (no Shift) collapses it again.

## sel_shift_extends_both_ways

- Right, Right, then Shift-Right twice marks `he[ll]o`. Shift-Left twice
  gets back to the start; a third Shift-Left turns the selection round
  (`h[e]llo`, cursor now before it) and a fourth makes it `[he]llo`.

## sel_block_all_then_type

- Ctrl-K X marks the whole text. Typing `x` leaves `x|$`. Ctrl-U brings all
  three lines back and the whole text marked.

To decide: after that Ctrl-U the cursor stands at the start of the second
line, not at the start of the text. It is where the owner left it after
putting the first line back, the same rule the open question about where
Ctrl-U leaves the cursor already names; the model follows the editor.

## sel_block_line_then_type

- Down, then Ctrl-K L marks the line `two two` with its line break. Typing
  `L` replaces it: `one$ L|three$`, so `L` joins the line below.

## sel_delete_backspace

- `[hel]lo world`: Delete leaves `lo world`, cursor at the start; Shift-Right
  twice and Backspace leave ` world`. Neither key takes a character beside
  the selection.

## sel_ctrl_k_b_in_selection_mode

- Ctrl-K B at `he|llo`, Right three times, Ctrl-K K: the block
  `he[llo]`. Typing `!` leaves `he!| world`.

Decided (D98): only a selection made with Shift or the mouse collapses on a
plain move. A block Ctrl-K B started stays open until Ctrl-K K, as in Turbo
Pascal.

## sel_undo_restores_selection

- Shift-Down, Shift-Right twice marks `[hello world$ se]cond line$` (two
  lines). Return replaces it by one line break: `$ cond line$ third$`, cursor
  at the start of the second line, one step. Ctrl-U brings back both lines
  and the selection; Ctrl-R breaks the line again.

## sel_tab_one_line_replaces

- `h[el]lo`: Tab replaces the two letters by a tab. Home, Shift-Down marks
  two lines (the second only at its column 0); Tab then indents the first
  line as Ctrl-K I does, in one step, and the selection stays. Nothing is
  typed over it.

## sel_overwrite_replaces

- Overwrite mode (Ins off), `[hel]lo`: typing `Z` gives `Z|lo world`. The
  `l` after the selection is not overwritten.

## enter_autoindent_trim (C file, auto-indent on, Trim Auto-indent on)

- `    int a = 1;|$`, Return: a line of four blanks. Typing `b` keeps them
  (`    b`). Return: a new line of four blanks. Return again: the line of
  four blanks is emptied and a new one of four blanks stands below. Up: the
  last line of blanks is emptied, in the same step as the key. Down.
  Result: `    int a = 1;$ $     b$ $ $` with the indent kept only on the line
  typed on; every empty line is empty.

To decide: Ctrl-U after Up brings the four blanks back with the cursor
move (one step). After it the blanks are no longer taken off when the
cursor leaves again.

## Save and the final line break (tests/test_selection_mode.py)

These are not traces: the file on disk is what they show.

- save_keeps_no_final_newline (Keep Final newline on): a file `a\nb` with no
  final line break is saved as `a\nb`. With it off (Borland), as `a\nb\n`.
- save_empty_stays_empty (on): an empty file is saved empty. Off: `\n`.
  To decide: the second is the one odd case the editor has always had; the
  user may want it changed for the Borland style too.
- save_new_file_as_typed (on): a new file in which `q` was typed is saved
  as `q`. Off: `q\n`.

## ctrl_y, and the only line (D94 task 1.0, not done in this pass)

Not re-recorded: it needs the rule "the last line of the text always has its
line break" in the line owner (Ctrl-Y on the only line must leave an empty
line with its break, not a row that goes on). That is a change to the
editing rule of every mode, not to the selection mode, and it is listed in
the report of this pass.
