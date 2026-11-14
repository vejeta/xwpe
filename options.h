#ifndef __OPTIONS_H
#define __OPTIONS_H
/*-------------------------------------------------------------------------*\
  <options.h> -- Header file for option variables defines, etc.
  
  Date      Programmer  Description
  3/27/98   Kenn F.     Created to handle f->ed-> stuff.
\*-------------------------------------------------------------------------*/

/* FENSTER f -> CNT ed -> flopt */
 
#define FM_SHOW_HIDDEN_FILES  0x0001
#define FM_SHOW_HIDDEN_DIRS   0x0002
#define FM_DELETE_AT_EXIT     0x0004
#define FM_PROMPT_DELETE      0x0008
/* FM_DONT_DELETE */

#define FM_MOVE_OVERWRITE     0x0010
#define FM_MOVE_PROMPT        0x0020
/* FM_DONT_OVERWRITE */
#define FM_REMOVE_INTO_WB     0x0040
#define FM_REMOVE_PROMPT      0x0080
/* FM_REMOVE_NO_PROMPT */

#define FM_TRY_HARDLINK       0x0100
/* FM_ALWAYS_SYMBOLIC_LINK */
#define FM_SORT_NAME          0x0200
#define FM_SORT_TIME          0x0400
/* FM_SORT_BYTES */
#define FM_REVERSE_ORDER      0x0800

#define FM_REKURSIVE_ACTIONS  0x1000
#define FM_CLOSE_WINDOW       0x2000


/* FENSTER f -> CNT ed -> edopt */

#define ED_CUA_STYLE          0x0001
/* ED_OLD_STYLE */

#define ED_OLD_TILE_METHOD    0x0040

#define ED_SOURCE_AUTO_INDENT 0x0080
#define ED_ALWAYS_AUTO_INDENT 0x0100
/* ED_NEVER_AUTO_INDENT */

#define ED_ERRORS_STOP_AT     0x0010
#define ED_MESSAGES_STOP_AT   0x0020
#define ED_SYNTAX_HIGHLIGHT   0x0008

#define ED_SHOW_ENDMARKS      0x0002

/* Block-marking model.  Clear (default) = MODERN: Begin Mark (^K B) starts a
 * fresh, empty selection at the cursor (anchor-style, like VS Code/Sublime),
 * so re-marking never leaves the previous block highlighted.  Set = WordStar:
 * ^K B and ^K K are two independent persistent markers, so you can adjust one
 * end without touching the other (classic Borland/WordStar behaviour). */
#define ED_BLOCK_WORDSTAR     0x0004

/* AI assistant enabled at runtime.  Deliberately NOT part of ED_EDITOR_OPTIONS
 * (the default-on mask): the assistant is opt-in, so a fresh profile has it OFF
 * and no AI menu/key/text appears until the user turns it on.  Only meaningful
 * in a build configured with --enable-ai (WPE_AI); harmless as a bare constant
 * otherwise. */
#define ED_AI_ENABLE          0x0200

/* Selection block mode.  Set = a block is a selection like in a modern
 * editor: Shift and a move extends it, a move without Shift collapses it,
 * and a typed character, Enter, Tab inside one line, Delete or Backspace
 * replaces or deletes it as one Undo step.  It is a third level of the
 * block option (anchor, WordStar, selection), independent of the key style
 * bit above; with it set the WordStar bit has no effect. */
#define ED_BLOCK_SELECTION    0x0400

/* The indent that auto-indent put on a line is taken off again when the
 * cursor leaves the line without anything having been typed on it. */
#define ED_TRIM_AUTO_INDENT   0x0800

/* Save keeps the final line break of the file as it was read: a file that
 * had none is written without one, and so is a new file or an empty one. */
#define ED_KEEP_FILE_ENDING   0x1000

#define ED_EDITOR_OPTIONS \
  (ED_CUA_STYLE | ED_OLD_TILE_METHOD | ED_SOURCE_AUTO_INDENT | \
  ED_ALWAYS_AUTO_INDENT | ED_SHOW_ENDMARKS | ED_BLOCK_WORDSTAR | \
  ED_BLOCK_SELECTION | ED_TRIM_AUTO_INDENT | ED_KEEP_FILE_ENDING)

#define ED_PROGRAMMING_OPTIONS \
  (ED_ERRORS_STOP_AT | ED_MESSAGES_STOP_AT | ED_SYNTAX_HIGHLIGHT)

#endif

