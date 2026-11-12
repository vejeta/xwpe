"""Every kind of operation the editing model covers, as the keys that make
it under the default (old) key style, and the places keys are read.

The list is the one the state channel must answer for: a kind whose keys
get no dump back is a key path the channel misses.  No assistant key and
no programming-layer key (Alt-G, Ctrl-G, Alt-Q) is here: the session runs
with the assistant off and those keys belong to other layers.
"""
from .oracle import (ALT_BACKSPACE, BACKSPACE, CTRL_DEL, CTRL_END, CTRL_HOME,
                     CTRL_INS, CTRL_LEFT, CTRL_PGDN, CTRL_PGUP, CTRL_RIGHT,
                     DELETE, DOWN, END, ENTER, ESC, F2, F3, F4, HOME, INSERT,
                     LEFT, PGDN, PGUP, RIGHT, SHIFT_DEL, SHIFT_DOWN, SHIFT_END,
                     SHIFT_HOME, SHIFT_INS, SHIFT_LEFT, SHIFT_RIGHT, SHIFT_UP,
                     TAB, UP, alt, ctrl)

F6 = "\033[17~"

# (name, keys)
OP_KINDS = [
    ("type_ascii", ["x"]),
    ("type_space", [" "]),
    ("type_utf8", ["é"]),
    ("enter", [ENTER]),
    ("tab", [TAB]),
    ("backspace", [BACKSPACE]),
    ("delete", [DELETE]),
    ("ctrl_t_delete_word", [ctrl("t")]),
    ("ctrl_z_delete_to_eol", [ctrl("z")]),
    ("ctrl_y_delete_line", [ctrl("y")]),
    ("left", [LEFT]), ("right", [RIGHT]), ("up", [UP]), ("down", [DOWN]),
    ("home", [HOME]), ("end", [END]),
    ("ctrl_home", [CTRL_HOME]), ("ctrl_end", [CTRL_END]),
    ("pgup", [PGUP]), ("pgdn", [PGDN]),
    ("ctrl_pgup", [CTRL_PGUP]), ("ctrl_pgdn", [CTRL_PGDN]),
    ("ctrl_left", [CTRL_LEFT]), ("ctrl_right", [CTRL_RIGHT]),
    ("shift_left", [SHIFT_LEFT]), ("shift_right", [SHIFT_RIGHT]),
    ("shift_up", [SHIFT_UP]), ("shift_down", [SHIFT_DOWN]),
    ("shift_home", [SHIFT_HOME]), ("shift_end", [SHIFT_END]),
    ("insert_toggle", [INSERT]),
    ("alt_j", [alt("j")]),
    ("alias_ctrl_p", [ctrl("p")]), ("alias_ctrl_n", [ctrl("n")]),
    ("alias_ctrl_b", [ctrl("b")]), ("alias_ctrl_f", [ctrl("f")]),
    ("alias_ctrl_a", [ctrl("a")]), ("alias_ctrl_e", [ctrl("e")]),
    ("alias_ctrl_d", [ctrl("d")]), ("alias_alt_i", [alt("i")]),
] + [("ctrl_k_" + c.lower(), [ctrl("k"), c.lower()])
     for c in "ABKLXZHCVYDIURWF09"] + [
    ("ctrl_o_" + c.lower(), [ctrl("o"), c.lower()]) for c in "YTELBUMH09FAK"] + [
    ("cut", [ctrl("x")]), ("copy", [ctrl("c")]), ("paste", [ctrl("v")]),
    ("shift_del_cut", [SHIFT_DEL]), ("ctrl_ins_copy", [CTRL_INS]),
    ("shift_ins_paste", [SHIFT_INS]), ("ctrl_del_delete", [CTRL_DEL]),
    ("undo", [ctrl("u")]), ("redo", [ctrl("r")]),
    ("alt_backspace_undo", [ALT_BACKSPACE]),
    ("show_buffer", [alt("y")]),
    ("find", [F4]), ("search_again", [ctrl("l")]),
    ("replace", [alt("s"), "r"]), ("goto_line", [alt("s"), "g"]),
    ("save", [F2]),
    ("next_window", [F6]), ("alt_n_window", [alt("1")]),
    ("close_window", [ctrl("w")]),
] + [("edit_menu_" + c.lower(), [alt("e"), c.lower()]) for c in "TCPSDUR"] + [
    ("block_menu_" + c.lower(), [alt("b"), c.lower()])
    for c in "BEOLGNCMDHRWITA"]

OP_NAMES = [name for name, _ in OP_KINDS]

# (name, keys that enter it, the ctx the dump must then name, or a prefix)
CONTEXTS = [
    ("edit", [], "edit"),
    ("prefix_ctrl_k", [ctrl("k")], "prefix"),
    ("prefix_ctrl_o", [ctrl("o")], "prefix"),
    ("menu", [alt("o")], "menu"),
    ("dialog_editor_options", [alt("o"), "e"], "dialog:"),
    ("dialog_replace", [alt("s"), "r"], "dialog:"),
    ("message", [alt("#"), "a"], "message"),
    ("filemanager", [F3], "filemanager"),
]

CONTEXT_NAMES = [name for name, _, _ in CONTEXTS]

# How many Esc presses bring any of the places above back to the text.
LEAVE_ESC_MAX = 4
LEAVE_KEY = ESC
