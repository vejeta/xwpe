"""The keys that make each action of the model, per key style.

Two key styles today (Options > Editor > Keys): the old Turbo C style and
CUA.  For the key loop's own actions they differ only in Save: F2 in the
old style, Alt-F2 under CUA (we_edit.c e_tst_fkt, where CUA's F2 opens the
file manager).  The manual's primary key is used where the terminal
decodes it; where it does not (the file ends on Ctrl-PgUp/PgDn, the window
ends on Ctrl-Home/End), the manual's alternative is used and the primary
key is a separate action, so a key the terminal drops shows as a finding.
No assistant key and no programming-layer key is here.
"""
from .oracle import (BACKSPACE, CTRL_END, CTRL_HOME, CTRL_LEFT, CTRL_PGDN,
                     CTRL_PGUP, CTRL_RIGHT, DELETE, DOWN, END, ENTER, ESC, F2,
                     HOME, INSERT, LEFT, PGDN, PGUP, RIGHT, SHIFT_DOWN, SHIFT_END,
                     SHIFT_HOME, SHIFT_LEFT, SHIFT_RIGHT, SHIFT_UP, TAB, UP, alt,
                     ctrl)

STYLE_OLD = "old"
STYLE_CUA = "cua"
STYLES = (STYLE_OLD, STYLE_CUA)

ALT_F2 = "\033[1;3Q"

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

# The manual's primary keys for the file and window ends, which this
# terminal does not decode: actions of their own, same effect.
PRIMARY_ALIASES = {
    "file_start_primary": ("file_start", [CTRL_PGUP]),
    "file_end_primary": ("file_end", [CTRL_PGDN]),
    "window_top_primary": ("window_top", [CTRL_HOME]),
    "window_bottom_primary": ("window_bottom", [CTRL_END]),
}


def keys_for(style, action_name, arg=None):
    """The keys that make @action_name in @style (@arg: the bytes typed)."""
    if action_name == "type":
        return [arg.decode("utf-8")] if arg[0] > 0x1F else [chr(arg[0])]
    if action_name in PRIMARY_ALIASES:
        return list(PRIMARY_ALIASES[action_name][1])
    if style == STYLE_CUA and action_name == "save":
        return [ALT_F2]
    return list(_OLD[action_name])


def model_action(action_name):
    """The model's action for a keymap action (a primary-key alias maps to
    the action it stands for)."""
    if action_name in PRIMARY_ALIASES:
        return PRIMARY_ALIASES[action_name][0]
    return action_name


def xwperc_for(style):
    """The options file a session of @style starts with."""
    if style == STYLE_CUA:
        return "[General]\nOptions2 : 153\n"
    return None
