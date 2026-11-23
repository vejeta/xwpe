"""The debugger's key switches reach one table of actions.

Debug > Quit, Ctrl-G Q, Ctrl-F2 and Ctrl-Break used to be four ways into the
debugger, two of which skipped the check for a missing session, so one
command said two different sentences.  The prefix switch (e_deb_inp) and the
function-key switch (e_debug_switch) now hand every shared action to
e_d_run_action, which looks it up in one table (we_debug.c, e_d_action_entries).

This rule fails the build when the table stops being the one owner:

  * a key switch (any function that asks the binding table for a debugger
    action) calls an entry itself, reaches a raw command through a name of
    its own (a #define alias, a function pointer or a static helper), or
    never calls the table;
  * a cell of the table (entry or key_entry), read through #define aliases,
    function pointers and static helpers, reaches the raw Pause or Quit
    without going through one of the owner's two answers for a state of the
    session (e_d_say_no_session, e_d_say_no_program);
  * the watch command is a key entry (it answers for itself as the menu's
    entry, and no key may skip the table's wrapper for it).
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(os.path.dirname(HERE), "we_debug.c")
# The entries of the table: a key switch calls none of them directly.
ENTRIES = ("e_d_pause", "e_d_pause_command", "e_d_q_quit", "e_d_quit_entry",
           "e_make_watches", "e_deb_trace", "e_deb_next", "e_deb_run",
           "e_breakpoint", "e_deb_stack", "e_d_finish_func", "e_d_goto_cursor")
# The commands that act on a session and answer for none: a cell of the table
# reaches them only through a function that starts with the session check.
RAW_COMMANDS = ("e_d_pause", "e_d_q_quit")
# Raw for a key too: the menu's own entry answers for itself, a key does not.
RAW_FOR_KEYS = RAW_COMMANDS + ("e_make_watches",)
SESSION_CHECKS = ("e_d_say_no_session", "e_d_say_no_program")
# What a finding names as the owner the cell went around.
SESSION_CHECK_WORDS = " or ".join(SESSION_CHECKS)
TABLE_NAME = "e_d_action_entries"
KEY_CONTEXTS = ("WPE_CTX_DEBUG", "WPE_CTX_PREFIX_DEBUG")
NOT_CALLS = {"if", "while", "for", "switch", "return", "sizeof", "case"}

FUNCTION = re.compile(
    r"^(?:static\s+)?(?:inline\s+)?[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*"
    r"\(([^;{}]*)\)\s*\n\{\n", re.M)


def functions_of(text):
    """{name: body} of every function of @text defined with its opening brace
    on a line of its own, `static` or not."""
    found = {}
    for match in FUNCTION.finditer(text):
        if match.group(1) in NOT_CALLS:
            continue
        end = text.find("\n}\n", match.end() - 2)
        if end < 0:
            continue
        found[match.group(1)] = text[match.start():end]
    return found


def calls_of(body):
    """The names a function body calls, in no order."""
    return {name for name in re.findall(r"\b([A-Za-z_]\w*)\s*\(", body)
            if name not in NOT_CALLS}


def resolve(name, aliases):
    """The function @name stands for: the #define aliases, function pointers
    and assignments of @aliases followed to the name they hold."""
    seen = set()
    while name not in seen and name in aliases:
        seen.add(name)
        name = aliases[name]
    return name


def starts_with_session_check(body):
    """True when the function answers a missing session or a missing program
    first, through one of the owner's two entries."""
    inside = body.split("{", 1)[1] if "{" in body else body
    return any(re.match(r"\s*if\s*\(\s*%s\s*\(" % re.escape(owner), inside)
               for owner in SESSION_CHECKS)


# The three ways the source makes one name stand for the function another
# holds, in the order the rule prefers them: a #define alias, a function
# pointer's initialiser, a plain assignment.
ALIAS_FORMS = (
    re.compile(r"^#\s*define\s+(\w+)\s+(\w+)\s*$", re.M),
    re.compile(r"\(\s*\*\s*(\w+)\s*\)\s*\([^)]*\)\s*=\s*(\w+)\s*;"),
    re.compile(r"\b(\w+)\s*=\s*(\w+)\s*;"),
)


def aliases_of(text):
    """{name: the name it stands for} of every alias form in @text.  A name
    two forms hold keeps the first of them."""
    held = {}
    for form in ALIAS_FORMS:
        for match in form.finditer(text):
            held.setdefault(match.group(1), match.group(2))
    return held


def table_rows(text):
    """[(action, entry, key_entry)] of the action table, names as written."""
    start = re.search(r"%s\s*\[\]\s*=\s*\{" % TABLE_NAME, text)
    if start is None:
        return None
    end = text.index("};", start.end())
    return re.findall(r"\{\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\}", text[start.end():end])


class Source:
    """we_debug.c as this rule reads it.

    A walk visits every function a key switch or a cell of the table can
    reach, which in this file is most of the debugger, so what each function
    calls, what each name stands for and which functions start with the
    owner's answer are read once here.  The walk then costs dictionary
    lookups instead of a scan of a megabyte of C for every name it meets."""

    def __init__(self, text):
        self.functions = functions_of(text)
        self.aliases = aliases_of(text)
        self.rows = table_rows(text)
        self.calls = {name: calls_of(body.split("{", 1)[1])
                      for name, body in self.functions.items()}
        self.guarded = frozenset(name for name, body in self.functions.items()
                                 if starts_with_session_check(body))

    def resolve(self, name):
        """The function @name stands for, aliases followed."""
        return resolve(name, self.aliases)

    def key_switches(self):
        """The functions that ask the binding table for a debugger action: the
        Ctrl-G prefix and the function keys."""
        return sorted(name for name, body in self.functions.items()
                      if any(context in body for context in KEY_CONTEXTS)
                      and "wpe_binding_action(" in body)

    def reaches(self, name, raw, seen=None):
        """The raw commands of @raw that @name reaches through its calls
        without a function that starts with the session check in between.

        Every name on the way is read as the source writes it: one that is not
        a function of its own is followed through the #define alias, the
        function pointer or the assignment it stands for, and one that is a
        function (a static helper included) through its body."""
        seen = seen if seen is not None else set()
        if name not in self.functions:
            name = self.resolve(name)
        if name in raw:
            return {name}
        if name in seen or name not in self.functions:
            return set()
        seen.add(name)
        if name in self.guarded:
            return set()
        reached = set()
        for callee in self.calls[name]:
            reached |= self.reaches(callee, raw, seen)
        return reached


def switch_offences(source):
    """['switch calls entry'] for each key switch that calls an entry itself,
    reaches a raw command through a name of its own, or never calls the
    table.

    A switch that writes `DO_PAUSE(f)` for a #define of e_d_pause, `ptr(f)`
    for a pointer to it, or `helper(f)` for a static helper around it goes
    around the table exactly as one that writes `e_d_pause(f)`, so the walk
    that reads a cell of the table reads the switch's body too."""
    found = []
    switches = source.key_switches()
    if not switches:
        return ["no key switch found: the rule reads nothing"]
    for switch in switches:
        body = source.functions[switch]
        for entry in ENTRIES:
            if re.search(r"\b%s\s*\(" % entry, body):
                found.append("%s calls %s" % (switch, entry))
        for raw in sorted(source.reaches(switch, RAW_FOR_KEYS)):
            found.append("%s reaches %s around the table" % (switch, raw))
        if "e_d_run_action(" not in body:
            found.append("%s never calls e_d_run_action" % switch)
    return found


def table_offences(source):
    """Findings for each cell of the table that reaches a raw command."""
    if not source.rows:
        return ["the action table %s is not found" % TABLE_NAME]
    found = []
    for action, entry, key_entry in source.rows:
        if entry == "NULL":
            continue
        for raw in sorted(source.reaches(entry, RAW_COMMANDS)):
            found.append("%s: entry %s reaches %s without %s"
                         % (action, entry, raw, SESSION_CHECK_WORDS))
        if key_entry == "NULL":
            continue
        for raw in sorted(source.reaches(key_entry, RAW_FOR_KEYS)):
            found.append("%s: key_entry %s reaches %s without %s"
                         % (action, key_entry, raw, SESSION_CHECK_WORDS))
    return found


def offences(text):
    """Every finding of the rule over the source @text."""
    source = Source(text)
    return switch_offences(source) + table_offences(source)


def source_text():
    with open(SOURCE, encoding="utf-8") as fh:
        return fh.read()


def test_the_action_table_is_the_one_owner_of_the_debugger_keys():
    assert offences(source_text()) == []


def test_the_rule_reads_the_table_it_checks():
    """Not an empty rule: the table has its rows and the wrappers start with
    the session check."""
    text = source_text()
    functions = functions_of(text)
    rows = table_rows(text)
    assert len(rows) >= 10
    assert ("WPE_ACT_DEBUG_QUIT", "e_d_quit_entry", "e_d_quit_asking_entry") in rows
    assert ("WPE_ACT_DEBUG_PAUSE", "e_d_pause_command", "NULL") in rows
    for wrapper in ("e_d_pause_command", "e_d_quit_entry", "e_d_quit_asking_entry"):
        assert starts_with_session_check(functions[wrapper]), wrapper


def test_a_key_switch_that_goes_around_the_table_is_named():
    text = source_text()
    planted = text.replace("int e_debug_switch(FENSTER *f, int c)\n{",
                           "int e_debug_switch(FENSTER *f, int c)\n{\n e_d_pause(f);", 1)
    assert "e_debug_switch calls e_d_pause" in offences(planted)
    bare = planted.replace("e_d_run_action(f, action, 1)", "0", 1)
    assert "e_debug_switch never calls e_d_run_action" in offences(bare)


def test_a_static_key_switch_is_still_read():
    """`static int e_debug_switch` used to crash the rule."""
    text = source_text().replace(
        "int e_debug_switch(FENSTER *f, int c)\n{",
        "static int e_debug_switch(FENSTER *f, int c)\n{\n e_d_pause(f);", 1)
    assert "e_debug_switch calls e_d_pause" in offences(text)


def test_a_third_key_switch_is_found_by_what_it_asks_for():
    text = source_text() + (
        "\nint e_third_switch(FENSTER *f, int c)\n{\n"
        " switch (wpe_binding_action(c, 0, WPE_CTX_DEBUG))\n {\n"
        "  case 1:\n   e_d_pause(f);\n   break;\n }\n return(0);\n}\n")
    found = offences(text)
    assert "e_third_switch calls e_d_pause" in found
    assert "e_third_switch never calls e_d_run_action" in found


# The two switches the rule reads: the Ctrl-G prefix and the function keys.
PREFIX_SWITCH_HEAD = "int e_deb_inp(FENSTER *f)\n{"
KEY_SWITCH_HEAD = "int e_debug_switch(FENSTER *f, int c)\n{"


def planted_switch_call(call, extra="", head=KEY_SWITCH_HEAD):
    """The source with @call as the first statement of the key switch @head,
    and @extra (the alias, the pointer or the helper the call names) put
    before the action table."""
    text = source_text()
    assert head in text, head
    text = text.replace(head, head + "\n " + call, 1)
    return text.replace("static const E_D_ACTION_ENTRY %s" % TABLE_NAME,
                        extra + "static const E_D_ACTION_ENTRY %s" % TABLE_NAME, 1)


def test_a_define_alias_inside_a_key_switch_is_followed():
    """`DO_PAUSE(f)` is `e_d_pause(f)` to the compiler, so it is to the rule:
    the switch went around the table without writing an entry's name."""
    text = planted_switch_call("DO_PAUSE(f);", "#define DO_PAUSE e_d_pause\n")
    assert "e_debug_switch reaches e_d_pause around the table" in offences(text)


def test_a_function_pointer_inside_a_key_switch_is_followed():
    text = planted_switch_call("pause_ptr(f);",
                               "static int (*pause_ptr)(FENSTER *f) = e_d_pause;\n")
    assert "e_debug_switch reaches e_d_pause around the table" in offences(text)


def test_a_static_helper_inside_a_key_switch_is_followed():
    text = planted_switch_call(
        "pause_helper(f);",
        "static int pause_helper(FENSTER *f)\n{\n return(e_d_pause(f));\n}\n")
    assert "e_debug_switch reaches e_d_pause around the table" in offences(text)


def test_the_prefix_switch_is_walked_the_same_way():
    """Both switches are read: the Ctrl-G prefix answers a raw command through
    a helper of its own exactly as the function keys do."""
    text = planted_switch_call(
        "pause_helper(f);",
        "static int pause_helper(FENSTER *f)\n{\n return(e_d_pause(f));\n}\n",
        head=PREFIX_SWITCH_HEAD)
    assert "e_deb_inp reaches e_d_pause around the table" in offences(text)


def test_a_switch_helper_that_asks_for_the_session_first_is_fine():
    """The walk stops where the owner's answer starts, in a switch as in a
    cell of the table."""
    helper = ("static int pause_helper(FENSTER *f)\n{\n if (e_d_say_no_program(f))\n"
              "  return(0);\n return(e_d_pause(f));\n}\n")
    text = planted_switch_call("pause_helper(f);", helper)
    assert offences(text) == []


def planted_row(old_row, new_row, extra=""):
    """The source with the table row @old_row replaced by @new_row, and @extra
    put before the table."""
    text = source_text()
    assert old_row in text, old_row
    text = text.replace(old_row, new_row, 1)
    return text.replace("static const E_D_ACTION_ENTRY %s" % TABLE_NAME,
                        extra + "static const E_D_ACTION_ENTRY %s" % TABLE_NAME, 1)


QUIT_ROW = "{ WPE_ACT_DEBUG_QUIT, e_d_quit_entry, e_d_quit_asking_entry }"
PAUSE_ROW = "{ WPE_ACT_DEBUG_PAUSE, e_d_pause_command, NULL }"
WATCH_ROW = "{ WPE_ACT_DEBUG_WATCH, e_make_watches, NULL }"


def test_raw_quit_as_a_key_entry_is_named():
    text = planted_row(QUIT_ROW, "{ WPE_ACT_DEBUG_QUIT, e_d_quit_entry, e_d_q_quit }")
    assert any("key_entry e_d_q_quit reaches e_d_q_quit" in f for f in offences(text))


def test_raw_pause_as_a_key_entry_is_named():
    text = planted_row(PAUSE_ROW, "{ WPE_ACT_DEBUG_PAUSE, e_d_pause_command, e_d_pause }")
    assert any("key_entry e_d_pause reaches e_d_pause" in f for f in offences(text))


def test_raw_pause_as_an_entry_is_named():
    text = planted_row(PAUSE_ROW, "{ WPE_ACT_DEBUG_PAUSE, e_d_pause, NULL }")
    assert any("entry e_d_pause reaches e_d_pause" in f for f in offences(text))


def test_the_watch_command_as_a_key_entry_is_named():
    text = planted_row(WATCH_ROW, "{ WPE_ACT_DEBUG_WATCH, e_make_watches, e_make_watches }")
    assert any("key_entry e_make_watches reaches e_make_watches" in f for f in offences(text))


def test_the_watch_command_as_the_menus_entry_is_allowed():
    assert offences(source_text()) == []
    assert WATCH_ROW in source_text()


def test_a_define_alias_of_a_raw_command_is_followed():
    text = planted_row(PAUSE_ROW, "{ WPE_ACT_DEBUG_PAUSE, e_d_pause_command, PAUSE_KEY }",
                       "#define PAUSE_KEY e_d_pause\n")
    assert any("key_entry PAUSE_KEY reaches e_d_pause" in f for f in offences(text))


def test_a_function_pointer_to_a_raw_command_is_followed():
    text = planted_row(PAUSE_ROW, "{ WPE_ACT_DEBUG_PAUSE, e_d_pause_command, pause_ptr }",
                       "static int (*pause_ptr)(FENSTER *f) = e_d_pause;\n")
    assert any("key_entry pause_ptr reaches e_d_pause" in f for f in offences(text))


def test_a_static_helper_around_a_raw_command_is_followed():
    helper = "static int pause_helper(FENSTER *f)\n{\n return(e_d_pause(f));\n}\n"
    text = planted_row(PAUSE_ROW, "{ WPE_ACT_DEBUG_PAUSE, e_d_pause_command, pause_helper }", helper)
    assert any("key_entry pause_helper reaches e_d_pause" in f for f in offences(text))


def test_a_helper_that_asks_for_the_session_first_is_fine():
    helper = ("static int pause_helper(FENSTER *f)\n{\n if (e_d_say_no_session(f))\n  return(0);\n"
              " return(e_d_pause(f));\n}\n")
    text = planted_row(PAUSE_ROW, "{ WPE_ACT_DEBUG_PAUSE, e_d_pause_command, pause_helper }", helper)
    assert offences(text) == []


def test_a_helper_that_asks_for_the_program_first_is_fine():
    """The owner answers two states of the session: a command that needs the
    program itself asks the second one, and that is enough."""
    helper = ("static int pause_helper(FENSTER *f)\n{\n if (e_d_say_no_program(f))\n  return(0);\n"
              " return(e_d_pause(f));\n}\n")
    text = planted_row(PAUSE_ROW, "{ WPE_ACT_DEBUG_PAUSE, e_d_pause_command, pause_helper }", helper)
    assert offences(text) == []


PAUSE_COMMAND_BODY = ("int e_d_pause_command(FENSTER *f)\n{\n if (e_d_say_no_program(f))\n"
                      "  return(0);\n return(e_d_pause(f));\n}")


def test_a_wrapper_that_lost_its_session_check_is_named():
    text = source_text()
    assert PAUSE_COMMAND_BODY in text
    planted = text.replace(
        PAUSE_COMMAND_BODY,
        "int e_d_pause_command(FENSTER *f)\n{\n return(e_d_pause(f));\n}", 1)
    assert any("entry e_d_pause_command reaches e_d_pause" in f for f in offences(planted))


def test_a_wrapper_that_asks_a_look_alike_check_is_named():
    """The owner's two entries are the only checks the rule takes: a wrapper
    whose first call merely looks like one of them still reaches the raw
    command, so a session state would be answered twice, in two sentences."""
    helper = ("static int e_d_say_no_session_soon(FENSTER *f)\n"
              "{\n if (e_d_session_runs())\n  return(0);\n return(e_d_pause(f));\n}\n")
    text = planted_row(PAUSE_ROW, "{ WPE_ACT_DEBUG_PAUSE, e_d_say_no_session_soon, NULL }", helper)
    assert any("entry e_d_say_no_session_soon reaches e_d_pause" in f for f in offences(text))


def test_the_pause_entry_asks_the_owner_that_names_the_compiling_session():
    """Pause needs the program, so its entry asks the owner that says which
    state of the session left it with none: a session still being compiled
    gets its own line there, and never one of Pause's own."""
    text = source_text()
    assert PAUSE_COMMAND_BODY in text
    assert "e_d_say_no_program" in functions_of(text)
    assert "E_SAY_DEBUG_COMPILING" in functions_of(text)["e_d_say_no_program"]
