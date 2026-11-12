# Four goldens to approve

Each golden is a short key script and what today's `wpe` did with it. Once
approved, the model must agree with it and the editor must keep doing it.
In the pictures `|` is the cursor, `$` ends a line, `~` marks a row that
wraps on into the next one. Every step passed the editor's self-checks.
Approve a golden by adding its line to `APPROVED`; say no and it is
dropped or re-recorded.

## typing_wraps (plain text, Max. Columns 120)

Manual: "After this number of columns a word wrap is performed."

- A line of 116 characters, cursor at its end. Typing `Z`, `Q`: they go
  in, no wrap yet (118 characters).
- Typing `R`: the line wraps at the last blank. The first row keeps
  `w000 ... w021 ` (with its blank) and wraps on; `w022xyZQR` becomes the
  next row, cursor after the `R`. The file still has one line there.
- Home, typing `A` at the start of that second row: `Aw022xyZQR`.
- On the next line `next`, typing `!` at its end: `next!`.

To decide: the blank where the line wrapped stays at the end of the first
row (it is still in the file, between `w021` and `w022`).

To decide: the wrap comes before the line reaches Max. Columns. At 120 a
row holds at most 118 characters: the `R` that would make it 119 wraps
it, where the manual's words suggest a wrap only past 120.

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
