"""The keys that make each action of the model, per key style.

Two key styles (Options > Editor > Keys): the old Turbo C style, and the
second style, which 1.6 called CUA and which is now the Modern style (the
level is still named "cua" here; the rename of the levels is the
regeneration of the array and is not done).  In the Modern style Save is
Ctrl-S and Redo is Ctrl-Y; Ctrl-T, Ctrl-Z and Ctrl-Y no longer delete a word,
to the end of the row and a line, and Ctrl-O is Open and no longer a prefix,
so the actions that used those keys have no key there (UNAVAILABLE) and a
walk of that style never draws them.  For the file ends (Ctrl-PgUp/PgDn) and
the window ends (Ctrl-Home/End) the manual's alternative is used and the
primary key is a separate action of the same effect, so a terminal that drops
the primary key shows as a finding.
No assistant key and no programming-layer key is here.
"""
from .oracle import (BACKSPACE, CTRL_DEL, CTRL_END, CTRL_HOME, CTRL_INS,
                     CTRL_LEFT, CTRL_PGDN, CTRL_PGUP, CTRL_RIGHT, DELETE, DOWN,
                     END, ENTER, ESC, F2, HOME, INSERT, LEFT, PGDN, PGUP, RIGHT,
                     SHIFT_DEL, SHIFT_DOWN, SHIFT_END, SHIFT_HOME, SHIFT_INS,
                     SHIFT_LEFT, SHIFT_RIGHT, SHIFT_UP, TAB, UP, alt, ctrl)

STYLE_OLD = "old"
STYLE_CUA = "cua"
STYLES = (STYLE_OLD, STYLE_CUA)

ALT_F2 = "\033[1;3Q"

# Actions with no key in the Modern style: the Borland control letters it
# gave up (the walk's alternates for the file and window ends are
# Ctrl-Alt-letter chords, bound to nothing there) and the Ctrl-O prefix.
UNAVAILABLE = {
    STYLE_CUA: frozenset((
        "delete_word", "kill_to_eol", "delete_line",
        "file_start", "file_end", "window_top", "window_bottom",
        "marker_goto", "help_button", "help_highlight", "help_header", "help_end",
    )),
}


def available(style, action_name):
    """Can a walk of @style draw @action_name (does the style have a key)?"""
    return action_name not in UNAVAILABLE.get(style, ())

# action name -> keys, the old style.
_OLD = {
    "enter": [ENTER],
    "tab": [TAB],
    "backspace": [BACKSPACE],
    "delete": [DELETE],
    "delete_word": [ctrl("t")],
    "kill_to_eol": [ctrl("z")],
    "delete_line": [ctrl("y")],
    "left": [LEFT], "right": [RIGHT], "up": [UP], "down": [DOWN],
    "home": [HOME], "end": [END],
    "file_start": [ESC + ctrl("a")], "file_end": [ESC + ctrl("e")],
    "word_left": [CTRL_LEFT], "word_right": [CTRL_RIGHT],
    "page_up": [PGUP], "page_down": [PGDN],
    "window_top": [ESC + ctrl("p")], "window_bottom": [ESC + ctrl("n")],
    "shift_left": [SHIFT_LEFT], "shift_right": [SHIFT_RIGHT],
    "shift_up": [SHIFT_UP], "shift_down": [SHIFT_DOWN],
    "shift_home": [SHIFT_HOME], "shift_end": [SHIFT_END],
    "shift_page_up": ["\033[5;2~"], "shift_page_down": ["\033[6;2~"],
    "shift_word_left": ["\033[1;6D"], "shift_word_right": ["\033[1;6C"],
    "shift_file_start": ["\033[5;6~"], "shift_file_end": ["\033[6;6~"],
    "shift_window_top": ["\033[1;6H"], "shift_window_bottom": ["\033[1;6F"],
    "insert_toggle": [INSERT],
    "literal_toggle": [alt("j")],
    "save": [F2],
    "undo": [ctrl("u")],
    "redo": [ctrl("r")],
}

# The manual's primary keys for the file and window ends: actions of their
# own, same effect as the alternatives above.
PRIMARY_ALIASES = {
    "file_start_primary": ("file_start", [CTRL_PGUP]),
    "file_end_primary": ("file_end", [CTRL_PGDN]),
    "window_top_primary": ("window_top", [CTRL_HOME]),
    "window_bottom_primary": ("window_bottom", [CTRL_END]),
}


CK = ctrl("k")
CO = ctrl("o")

# Block, clipboard and help-marker keys; the Ctrl-K and Ctrl-O prefixes read
# the same in both key styles.
_BLOCKS = {
    "block_begin": [CK, "b"], "block_end": [CK, "k"], "block_hide": [CK, "h"],
    "block_goto_begin": [CK, "a"], "block_goto_end": [CK, "z"],
    "block_line": [CK, "l"], "block_all": [CK, "x"],
    "block_copy": [CK, "c"], "block_move": [CK, "v"], "block_delete": [CK, "y"],
    "block_indent": [CK, "i"], "block_unindent": [CK, "u"],
    "help_button": [CO, "u"], "help_highlight": [CO, "m"], "help_header": [CO, "h"],
    "help_end": [CO, "e"],
    "edit_copy": [ctrl("c")], "edit_cut": [ctrl("x")], "edit_paste": [ctrl("v")],
}
_OLD.update(_BLOCKS)

# The clipboard keys the manual names beside the Ctrl forms (Ctrl-Ins,
# Shift-Del, Shift-Ins, Alt-Ins, Ctrl-Del): actions of their own with the same
# effect, so a terminal that drops one shows as a finding.
CLIPBOARD_ALIASES = {
    "edit_copy_ctrl_ins": ("edit_copy", [CTRL_INS]),
    "edit_cut_shift_del": ("edit_cut", [SHIFT_DEL]),
    "edit_paste_shift_ins": ("edit_paste", [SHIFT_INS]),
    "edit_paste_alt_ins": ("edit_paste", [ESC + INSERT]),
    "block_delete_ctrl_del": ("block_delete", [CTRL_DEL]),
}
PRIMARY_ALIASES.update(CLIPBOARD_ALIASES)

# In a number field of the option dialog Backspace does nothing and typed
# digits are added to the old ones; Ctrl-U empties the field first.
FIELD_CLEAR = ctrl("u")

# The option dialog's number fields: the accelerator and how many digits.
OPTION_FIELD_KEYS = {
    "maxcol": ("m", 3), "tabn": ("t", 2), "maxchg": ("x", 3),
    "numundo": ("n", 2), "autoindent_col": ("i", 2),
}
# The Replace dialog: Alt-S R opens it; Alt-N moves to New Text; Alt-P turns
# off the prompt; Alt-G picks Global; Alt-A is Change All.
REPLACE_PRELUDE = [alt("s"), "r"]


def _text_keys(data):
    return [chr(c) for c in data]


def keys_for(style, action_name, arg=None):
    """The keys that make @action_name in @style (@arg: the bytes typed, the
    (search, replacement) of a replace, (field, value) of an option, or the
    marker's number)."""
    if action_name == "type":
        return [arg.decode("utf-8")] if arg[0] > 0x1F else [chr(arg[0])]
    if action_name in PRIMARY_ALIASES:
        return list(PRIMARY_ALIASES[action_name][1])
    if style == STYLE_CUA and action_name == "save":
        return [ctrl("s")]
    if style == STYLE_CUA and action_name == "redo":
        return [ctrl("y")]
    if action_name == "replace_all":
        search, repl = arg
        return (REPLACE_PRELUDE + _text_keys(search) + [alt("n")] + _text_keys(repl)
                + [alt("p"), alt("g"), alt("a")])
    if action_name == "option_set":
        field, value = arg
        letter, digits = OPTION_FIELD_KEYS[field]
        return ([alt("o"), "e", alt(letter), FIELD_CLEAR]
                + [c for c in str(value)] + [ENTER])
    if action_name == "marker_set":
        return [CK, str(arg)]
    if action_name == "marker_goto":
        return [CO, str(arg)]
    return list(_OLD[action_name])


def sample_arg(action_name):
    """An argument that makes @action_name's keys, for a table check."""
    return {"type": b"x", "replace_all": (b"a", b"b"), "option_set": ("tabn", 4),
            "marker_set": 1, "marker_goto": 1}.get(action_name)


def model_action(action_name):
    """The model's action for a keymap action (a primary-key alias maps to
    the action it stands for)."""
    if action_name in PRIMARY_ALIASES:
        return PRIMARY_ALIASES[action_name][0]
    return action_name


def xwperc_for(style):
    """The options file a session of @style starts with."""
    if style == STYLE_CUA:
        return "[General]\nOptions2 : 153\nKeysNotice : 1\n"
    return None
