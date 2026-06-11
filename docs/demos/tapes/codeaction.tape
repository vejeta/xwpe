# xwpe LSP demo -- code action + Undo/Redo (Alt-Q A, Ctrl-U, Ctrl-R).
# Cursor on the positional call greet("Ada", 42); Alt-Q A offers
# "Convert to named arguments".  Metals ships that refactor UNRESOLVED (only a
# `data` field), so xwpe runs a codeAction/resolve round-trip to fetch the edit,
# then rewrites the call to greet(name = "Ada", age = 42).  The headline: the
# server's rewrite is an ordinary edit -- Ctrl-U undoes it, Ctrl-R redoes it.
#
# Needs a running, indexed Metals.  The cold-start/indexing wait and the
# one-time "JDK too new -> pinned" notice are handled while Hidden.
#
# Record with:  vhs docs/demos/tapes/codeaction.tape   (from the repo root;
# WPE -> built ./wpe, DEMO -> an indexed Scala workspace -- see record.sh).

Output "docs/demos/gifs/codeaction.gif"
Set Shell bash
Set FontSize 16
Set Width 1100
Set Height 820
Set TypingSpeed 90ms
Set Padding 12

Hide
Type "cd ${DEMO} && TERM=xterm-256color ${WPE} actions.scala"
Enter
Sleep 4s
# start Metals (Alt-Q E = diagnostics), dismiss the one-time JDK-pin notice,
# and let it index -- all hidden.
Alt+q
Sleep 400ms
Type "e"
Sleep 2s
Enter
Sleep 95s
Show

Sleep 2s
# move the cursor onto the greet("Ada", 42) CALL on line 28 (pure arrows, so
# the recording is deterministic): down to the line, right into the arg list.
Down@60ms 27
Right@45ms 10
Sleep 2s
# Alt-Q A: open the code-action popup and let the viewer read the offer (four
# entries here -- the cursor is on the "Ada" literal, so Metals also offers the
# string refactors; "named arguments" is the last one).
Alt+q
Sleep 400ms
Type "a"
Sleep 3s
# select "Convert 'greet(...)' to named arguments" (4th entry) and apply; xwpe
# resolves it (codeAction/resolve) and rewrites the call in place.
Down@350ms 3
Sleep 1500ms
Enter
Sleep 4s
# Ctrl-U undoes the server's rewrite -- back to the positional call.
Ctrl+u
Sleep 3s
# Ctrl-R redoes it -- the named-argument form returns.
Ctrl+r
Sleep 4s
Ctrl+q
Sleep 1500ms
