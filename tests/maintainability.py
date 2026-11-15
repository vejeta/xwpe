#!/usr/bin/env python3
"""Maintainability measurements of the editor's own C sources.

Run by tests/check-maintainability.sh.  What it keeps true:

  row 3  no `f()` declaration (a prototype with no parameter list) in product
         code: a call is then checked against nothing.
  row 4  no `extern` in a .c file of a symbol a header already declares: the
         header is the one place a declaration lives.
  row 5  no getenv("XWPE_...") outside the files that own a variable: the
         trace owner (we_paint.c), the test dump (we_testdump.c), the
         assistant's configuration reader (we_ai.c) and we_main.c.
  row 6  the terminal's mouse symbols stay in we_term.c (mousemask, KEY_MOUSE,
         getmouse, MEVENT, fk_t_mouse): the console build loses its mouse
         silently when an edit removes them.
  rows 1-2 (REPORT ONLY in this pass) per-file ceilings of the longest
         function, the deepest nesting, the functions over six parameters, the
         duplicated blocks and the statics shared by two functions, and the
         tree ceilings of 120 lines and nesting 5.  The table is printed and
         never fails.

Rows 3 to 5 are counted per file in tests/maintainability.allow: a count may
only fall.  A count above its line is an error in every build.  A line above
the measurement is stale, an error only in the reference configuration (the
allow file's `reference` line against the binary's "built with:" words); any
other build reports it, and reports a line for a file of a feature it lacks
as not built here.  --strict ignores the allow lines.

The measurement is made on `$(CC) -E -C -fdirectives-only` with the build's own
flags: the `# N "file"` markers say which original lines are live, so an #ifdef
that opens a brace in both branches cannot desync the brace matcher and code
behind a feature the build lacks is not counted.

Environment (set by `make check`): MAINT_CC, MAINT_FLAGS, MAINT_SOURCES,
WPE_FEATURE_SOURCES; MAINT_BUILT_WITH is set by check-maintainability.sh from
tests/build-config.sh.  A hand run may leave them: CC (or cc), config.h if
present, every top-level .c, and an unknown configuration (not the reference).

  maintainability.py [--srcdir DIR] [--allow FILE] [--strict] [--list]
                     [--selftest]

Exit: 0 clean, 1 offenders or a blind scanner, 77 no compiler.
"""
import bisect
import multiprocessing
import os
import re
import shlex
import subprocess
import sys
import tempfile
from collections import defaultdict

SKIP = 77
BAD = 1

# Tree ceilings of row 2 (reported while the sweep is running).
TREE_LONGEST = 120
TREE_NEST = 5
PARAMS_OVER = 6
DUP_WINDOW = 6

# Files that may read an XWPE_ variable themselves (row 5).
GETENV_OWNERS = ("we_paint.c", "we_testdump.c", "we_ai.c", "we_main.c")

# Row 6: what the terminal build needs in we_term.c (the dual-mode guard).
MOUSE_FILE = "we_term.c"
MOUSE_SYMBOLS = ("mousemask", "KEY_MOUSE", "getmouse", "MEVENT", "fk_t_mouse")

REPORT_ROWS = 12

MARKER = re.compile(r'^# (\d+) "([^"]*)"')
TOKEN = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'',
                   re.S)
NOT_NEWLINE = re.compile(r"[^\n]")
STRUCT = re.compile(r"[{};]")
IDENT = re.compile(r"[A-Za-z_]\w*")

CTRL = {"if", "else", "while", "for", "switch", "do", "return", "case",
        "sizeof", "defined", "goto", "break", "continue", "default"}
KEYWORDS = CTRL | {"int", "char", "void", "long", "short", "unsigned",
                   "signed", "float", "double", "struct", "union", "enum",
                   "static", "extern", "const", "volatile", "register",
                   "typedef", "size_t", "ssize_t", "time_t", "pid_t",
                   "off_t", "mode_t", "FILE", "Bool", "u_int", "u_char"}
TYPES = {"int", "char", "void", "long", "short", "unsigned", "signed",
         "float", "double", "size_t", "ssize_t", "struct", "union", "enum",
         "FILE", "Bool", "register", "const", "static", "extern", "volatile",
         "time_t", "pid_t", "mode_t", "off_t", "u_int", "u_char"}
HEAD_BAD = re.compile(r"\b(typedef|extern\s+struct|if|else|while|for|switch|do|"
                      r"return|case|default|goto|asm|__asm)\b")

# A declaration starts a statement: storage words, then a type (any name, so
# a typedef such as FARBE counts as well as int), stars, the declared name and
# an empty list.  The type word is checked against CTRL by find_noproto, so
# `return f();` or `else g();` is a call, not a declaration.
NOPROTO_DECL = re.compile(
    r"(?:^|[;{}])\s*(?:(?:extern|static|const|unsigned|signed|register|"
    r"volatile|inline)\s+)*(?:(?:struct|union|enum)\s+)?([A-Za-z_]\w*)"
    r"[\s\*]+([A-Za-z_]\w*)\s*\(\s*\)\s*(?=[;,])")
EXTERN = re.compile(r"^\s*extern\s+(.*?)([A-Za-z_]\w*)\s*(\(|\[|;)\s*", re.M)
STATIC_DECL = re.compile(r"^\s*static\s+(?!const\b)([^;{=]*?)\b(\w+)\s*"
                         r"(?:\[[^\]]*\])?\s*(=[^;]*)?;", re.M)
LINKAGE = re.compile(r'\bextern\s*"[^"]*"\s*$')
POINTER_DECL = re.compile(r"\(\s*\*\s*([A-Za-z_]\w*)\s*[\)\[]")
GETENV_CALL = re.compile(r"\bgetenv\s*\(")
GETENV_ARG = re.compile(r'\s*"XWPE_')
GETENV_DEFINE = re.compile(r'^\s*#\s*define\b.*\bgetenv\s*\(\s*"XWPE_', re.M)
PUNCT = re.compile(r"^(?:[\s{}()\[\];,]|\"\")+$")
LITERAL = re.compile(r'"[^"]*"')
NOPROTO_NOT = {"if", "while", "for", "switch", "return", "sizeof", "defined",
               "do", "else", "case"}

# The kinds of an allow line.
COUNTED = ("noproto", "extern", "getenv")
REFERENCE = ("reference", "-")
ROW_NAMES = {"noproto": "row 3, f() declaration",
             "extern": "row 4, extern of a header's symbol",
             "getenv": "row 5, getenv(\"XWPE_...\") outside its owners"}


# ---------------------------------------------------------------------------
# Reading C text


def _hollow(match):
    """One comment or literal with its characters blanked, newlines and
    length kept, so every column and every line number still lines up."""
    text = match.group(0)
    if text[0] in "\"'":
        return text[0] + NOT_NEWLINE.sub(" ", text[1:-1]) + text[-1]
    return NOT_NEWLINE.sub(" ", text)


def blank_preproc(code):
    """Blank the preprocessor lines (continuations too), keeping line numbers:
    a #define between two definitions would otherwise join the next header."""
    out = []
    cont = False
    for line in code.split("\n"):
        if cont or line.lstrip().startswith("#"):
            cont = line.rstrip().endswith("\\")
            out.append(NOT_NEWLINE.sub(" ", line))
        else:
            out.append(line)
    return "\n".join(out)


def strip_code(text):
    """text with comments and literals hollowed and directives blanked."""
    return blank_preproc(TOKEN.sub(_hollow, text))


def split_params(text):
    """Split a parameter list at its top-level commas."""
    parts, depth, cur = [], 0, []
    for ch in text:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    if "".join(cur).strip():
        parts.append("".join(cur))
    return [p.strip() for p in parts if p.strip()]


def count_params(header):
    """The number of parameters of a definition's header (K&R lists too)."""
    opens = [i for i, ch in enumerate(header) if ch == "("]
    if not opens:
        return -1
    match = re.search(r"\(([^()]*(?:\([^()]*\)[^()]*)*)\)\s*$", header)
    if match:
        inner = match.group(1).strip()
    else:
        start, depth, end = opens[-1], 0, opens[-1]
        for k in range(start, len(header)):
            if header[k] == "(":
                depth += 1
            elif header[k] == ")":
                depth -= 1
                if depth == 0:
                    end = k
                    break
        inner = header[start + 1:end].strip()
    if not inner or inner == "void":
        return 0
    return len(split_params(inner))


def is_func_header(header):
    """Whether the text before a top-level `{` is a function's header."""
    header = header.strip()
    if not header or "(" not in header or ")" not in header:
        return False
    if "=" in header.split("(")[0] or HEAD_BAD.search(header):
        return False
    return bool(re.match(r"^[A-Za-z_][A-Za-z_0-9 \t\r\n*&]*$",
                         header.split("(")[0]))


def func_name(header):
    """The defined name: the first identifier before a '(' that is no type."""
    for cand in re.findall(r"([A-Za-z_]\w*)\s*\(", header):
        if cand not in KEYWORDS:
            return cand
    return ""


def line_starts(code):
    """The offset of every line's first character, for offset -> line."""
    starts, off = [], 0
    for line in code.split("\n"):
        starts.append(off)
        off += len(line) + 1
    return starts


def line_at(starts, offset):
    """The 1-based line holding offset."""
    return bisect.bisect_right(starts, offset)


def scan_functions(code):
    """[(name, start, end, nesting, params)] of the definitions in code.

    Nesting counts the function's own braces: a flat function is 1."""
    starts = line_starts(code)
    funcs = []
    depth = 0
    seg_start = 0
    fn_start = fn_header = None
    fn_max = 0
    for match in STRUCT.finditer(code):
        ch = match.group(0)
        i = match.start()
        if ch == "{":
            if depth == 0 and fn_start is None:
                header = code[seg_start:i]
                if is_func_header(header):
                    fn_start, fn_header, fn_max = seg_start, header, 1
                else:
                    seg_start = i + 1
            elif fn_start is not None:
                fn_max = max(fn_max, depth + 1)
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0 and fn_start is not None:
                name = func_name(fn_header)
                at = fn_header.find(name) if name else -1
                first = fn_start + (at if at >= 0 else 0)
                funcs.append((name, line_at(starts, first), line_at(starts, i),
                              fn_max, count_params(fn_header.strip())))
                fn_start = None
                seg_start = i + 1
            elif depth == 0:
                seg_start = i + 1
            if depth < 0:
                depth = 0
        elif depth == 0 and fn_start is None:
            seg_start = i + 1
    return funcs


def find_statics(code, funcs):
    """[(name, line, [users])] of the file-scope mutable statics that two or
    more functions of the file name."""
    starts = line_starts(code)
    idents = {}
    lines = code.split("\n")
    for name, first, last, _nest, _par in funcs:
        idents[(name, first)] = set(IDENT.findall("\n".join(lines[first - 1:last])))
    found = []
    for match in STATIC_DECL.finditer(code):
        line = line_at(starts, match.start())
        if any(first <= line <= last for _n, first, last, _a, _b in funcs):
            continue
        name = match.group(2)
        if name in TYPES:
            continue
        users = sorted({fn for (fn, _first), ids in idents.items() if name in ids})
        if len(users) >= 2:
            found.append((name, line, users))
    return found


# ---------------------------------------------------------------------------
# One text, rows 3 to 6


def find_noproto(code):
    """Lines of `type name();` declarations."""
    lines = []
    for number, text in enumerate(code.split("\n"), 1):
        for match in NOPROTO_DECL.finditer(text):
            kind, name = match.group(1), match.group(2)
            if kind in CTRL or name in TYPES or name in NOPROTO_NOT:
                continue
            lines.append(number)
            break
    return lines


def find_extern(code, header_symbols):
    """[(line, name, header)] of `extern` lines naming a header's symbol."""
    found = []
    for number, text in enumerate(code.split("\n"), 1):
        if not text.lstrip().startswith("extern"):
            continue
        match = EXTERN.match(text)
        if not match:
            continue
        name = match.group(2)
        if name not in TYPES and name in header_symbols:
            found.append((number, name, header_symbols[name]))
    return found


def find_getenv(text, code):
    """Lines of getenv("XWPE_...") calls, and of #define lines that make one.

    text is the live text with its literals, code the same hollowed: a call
    in a comment or in a literal is not one."""
    starts = line_starts(text)
    lines = []
    for match in GETENV_CALL.finditer(code):
        if GETENV_ARG.match(text, match.end()):
            lines.append(line_at(starts, match.start()))
    for match in GETENV_DEFINE.finditer(text):
        lines.append(line_at(starts, match.start()))
    return sorted(set(lines))


def normalise(code):
    """[(line, text)] of the lines that carry structure, for duplicate blocks."""
    out = []
    for number, line in enumerate(code.split("\n"), 1):
        text = LITERAL.sub('""', " ".join(line.split()))
        if text and not PUNCT.match(text):
            out.append((number, text))
    return out


def scan_unit_text(text, header_symbols, is_source):
    """Everything a live text says: rows 3 to 6 and, for a source, the
    function table and the shared statics.  A header gets rows 3 to 5."""
    code = strip_code(text)
    unit = {"noproto": find_noproto(code),
            "getenv": find_getenv(text, code),
            "norm": normalise(code)}
    unit["extern"] = find_extern(code, header_symbols) if is_source else []
    if is_source:
        funcs = scan_functions(code)
        unit["funcs"] = funcs
        unit["statics"] = find_statics(code, funcs)
        unit["mouse"] = sorted(set(re.findall(
            r"\b(%s)\b" % "|".join(MOUSE_SYMBOLS), code)))
    return unit


# ---------------------------------------------------------------------------
# The preprocessor


class Config:
    """What the build says about itself."""

    def __init__(self, srcdir, cc, flags, sources, listed, features=None,
                 feature_of=None):
        self.srcdir = srcdir
        self.real = os.path.realpath(srcdir)
        self.cc = cc
        self.flags = flags
        self.sources = sources
        self.listed = listed      # the build named its sources (a failure is an error)
        # {word: "yes"|"no"} from the binary's "built with:" line, or None
        # when the run was not told (a hand run of this file).
        self.features = features
        # {source: word} from make's feature source lists.
        self.feature_of = feature_of or {}

    def not_built_word(self, name):
        """The feature word that keeps name out of this build, or None."""
        word = self.feature_of.get(name)
        if word and self.features is not None and self.features.get(word) == "no":
            return word
        return None


def parse_words(text):
    """{word: value} of a "word=yes word=no ..." line."""
    words = {}
    for item in text.split():
        if "=" in item:
            word, value = item.split("=", 1)
            words[word] = value
    return words


def parse_feature_sources(text):
    """{source: word} of make's "word:src.c src.c;word:..." list."""
    owner = {}
    for group in text.split(";"):
        if ":" not in group:
            continue
        word, files = group.split(":", 1)
        for name in files.split():
            owner.setdefault(name, word.strip())
    return owner


def option_of(word):
    """What the user typed for a word: --with-x; a library's name as it is."""
    if word.startswith(("enable-", "with-")):
        return "--" + word
    return word


_CONFIG = None
_HEADER_SYMBOLS = {}


def is_generated(path):
    """Whether the file says a tool wrote it (wayland-scanner's glue): it is
    in the build but not the editor's own code."""
    with open(path, "rb") as fh:
        return b"Generated" in fh.read(200)


def top_level(config, ext):
    """Sorted names of the editor's own top-level files ending ext."""
    names = []
    for entry in os.listdir(config.srcdir):
        path = os.path.join(config.srcdir, entry)
        if entry.endswith(ext) and os.path.isfile(path) and not is_generated(path):
            names.append(entry)
    return sorted(names)


def own_file(config, path, cache):
    """The base name of path when it is a top-level file of the source folder."""
    if path in cache:
        return cache[path]
    name = None
    if not path.startswith("<"):
        real = os.path.realpath(path)
        if os.path.dirname(real) == config.real and real.endswith((".c", ".h")):
            name = os.path.basename(real)
    cache[path] = name
    return name


def run_preprocessor(config, source):
    """The preprocessed text of source, or None when the compiler refuses."""
    base = list(config.cc) + list(config.flags) + ["-E", "-C"]
    for extra in (["-fdirectives-only"], []):
        proc = subprocess.run(base + extra + [source], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE)
        if proc.returncode == 0:
            return proc.stdout.decode("latin-1"), bool(extra)
    return None, False


def read_live_lines(config, pp_text, source):
    """(the source's live lines {n: text}, {header: set of live lines}).

    A line of the output that follows a `# N "file"` marker belongs to that
    file at that line; a line the preprocessor skipped comes out blank."""
    cache = {}
    me = os.path.basename(source)
    mine = {}
    headers = defaultdict(set)
    name = None
    number = 0
    for line in pp_text.split("\n"):
        match = MARKER.match(line)
        if match:
            number = int(match.group(1))
            name = own_file(config, match.group(2), cache)
            continue
        if name == me:
            mine[number] = line
        elif name is not None and line.strip():
            headers[name].add(number)
        number += 1
    return mine, headers


def unit_for_source(source):
    """Worker: preprocess one source and read rows 3 to 6 from its live text."""
    config = _CONFIG
    pp_text, directives_only = run_preprocessor(config, source)
    if pp_text is None:
        return {"source": source, "error": "the compiler refused it"}
    mine, headers = read_live_lines(config, pp_text, source)
    top = max(mine) if mine else 0
    text = "\n".join(mine.get(n, "") for n in range(1, top + 1))
    unit = scan_unit_text(text, _HEADER_SYMBOLS, True)
    unit.update(source=source, headers={k: sorted(v) for k, v in headers.items()},
                directives_only=directives_only)
    return unit


# ---------------------------------------------------------------------------
# The tree


def top_level_statements(code):
    """The statements of code outside every brace, each without its brace
    groups: `struct s { int member; } var;` gives `struct s  var`."""
    statements, cur, depth = [], [], 0
    for ch in code:
        if ch == "{" and depth == 0 and LINKAGE.search("".join(cur)):
            cur = []                  # extern "C" { ... }: still file scope
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth = max(depth - 1, 0)
            if depth == 0 and re.search(r"\)\s*$", "".join(cur)):
                statements.append("".join(cur))    # a definition ends here
                cur = []
        elif depth == 0 and ch == ";":
            statements.append("".join(cur))
            cur = []
        elif depth == 0:
            cur.append(ch)
    return statements


def declared_names(statement):
    """The names a file-scope declaration declares (functions, variables,
    function pointers); nothing for a typedef, whose name is a type."""
    if re.match(r"\s*typedef\b", statement):
        return []
    names = []
    for declarator in split_params(statement):
        head = declarator.split("(", 1)[0]
        idents = IDENT.findall(re.sub(r"(\[[^\]]*\]|=.*)", " ", head))
        pointer = POINTER_DECL.match(declarator[len(head):])
        if pointer:
            names.append(pointer.group(1))    # int (*fp)(...)
        elif "(" in declarator and re.search(r"[A-Za-z_]\w*\s*$", head) \
                and idents and idents[-1] not in KEYWORDS:
            names.append(idents[-1])          # a function: the name before (
        elif "(" not in declarator and idents and idents[-1] not in KEYWORDS:
            names.append(idents[-1])          # a variable
    return names


def header_symbol_table(config):
    """symbol -> header, for every name a header declares at file scope.

    A struct member or a parameter that shares a global's name is not a
    declaration of the global, so an `extern` of it is not row 4's."""
    table = {}
    for header in top_level(config, ".h"):
        with open(os.path.join(config.srcdir, header), "rb") as fh:
            code = strip_code(fh.read().decode("latin-1"))
        for statement in top_level_statements(code):
            for name in declared_names(statement):
                table.setdefault(name, header)
    return table


def live_header_text(config, header, live_lines):
    """The header's text with the lines no unit saw live blanked."""
    with open(os.path.join(config.srcdir, header), "rb") as fh:
        raw = fh.read().decode("latin-1").split("\n")
    return "\n".join(line if n in live_lines else ""
                     for n, line in enumerate(raw, 1))


def count_duplicates(norms):
    """{file: blocks} of DUP_WINDOW or more normalised lines found twice.

    One block is one reported pair, counted for both files of the pair."""
    index = defaultdict(list)
    for name, seq in norms.items():
        for i in range(len(seq) - DUP_WINDOW + 1):
            blob = "\n".join(t for _n, t in seq[i:i + DUP_WINDOW])
            index[hash(blob)].append((name, i))
    reported = set()
    spans = set()
    counts = defaultdict(int)
    for occ in index.values():
        if len(occ) < 2:
            continue
        for a in range(len(occ)):
            for b in range(a + 1, len(occ)):
                (fa, ia), (fb, ib) = occ[a], occ[b]
                if fa == fb and abs(ia - ib) < DUP_WINDOW:
                    continue
                sa, sb = norms[fa], norms[fb]
                if "\n".join(t for _n, t in sa[ia:ia + DUP_WINDOW]) != \
                        "\n".join(t for _n, t in sb[ib:ib + DUP_WINDOW]):
                    continue
                key = (fa, sa[ia][0], fb, sb[ib][0])
                if key in reported or (fb, sb[ib][0], fa, sa[ia][0]) in reported:
                    continue
                reported.add(key)
                span = (fa, sa[ia][0] // 20, fb, sb[ib][0] // 20)
                if span in spans:
                    continue
                spans.add(span)
                counts[fa] += 1
                if fb != fa:
                    counts[fb] += 1
    return counts


class Measure:
    """What one run found."""

    def __init__(self):
        self.files = {}                       # source -> metrics
        self.found = {k: defaultdict(list) for k in COUNTED}
        self.mouse_missing = None             # None: we_term.c not built here
        self.failed = []
        self.functions = []                   # (file, name, length, nest, params)
        self.directives_only = True
        self.listed = False                   # the build named its sources

    def metrics(self, name):
        return self.files.setdefault(name, {"longest": 0, "nest": 0, "params": 0,
                                            "dup": 0, "statics": 0})


def build_config(srcdir):
    """The Config of this run, from make's environment or from the folder."""
    cc = shlex.split(os.environ.get("MAINT_CC") or os.environ.get("CC") or "cc")
    flags = shlex.split(os.environ.get("MAINT_FLAGS", ""))
    named = os.environ.get("MAINT_SOURCES")
    built_with = os.environ.get("MAINT_BUILT_WITH")
    features = parse_words(built_with) if built_with else None
    feature_of = parse_feature_sources(os.environ.get("WPE_FEATURE_SOURCES", ""))
    config = Config(srcdir, cc, flags, [], bool(named), features, feature_of)
    if not flags and os.path.exists("config.h"):
        config.flags = ["-DHAVE_CONFIG_H", "-I.", "-I" + srcdir, "-include", "config.h"]
    if named:
        config.sources = sorted({s for s in shlex.split(named) if s.endswith(".c")})
    else:
        config.sources = top_level(config, ".c")
    return config


def measure_tree(config, jobs=None):
    """Preprocess and read every source of the build; return a Measure."""
    global _CONFIG, _HEADER_SYMBOLS
    _CONFIG = config
    _HEADER_SYMBOLS = header_symbol_table(config)
    sources = [s for s in config.sources
               if os.path.isfile(os.path.join(config.srcdir, s))]
    paths = [os.path.join(config.srcdir, s) for s in sources]
    jobs = jobs or min(len(paths), os.cpu_count() or 1) or 1
    try:
        ctx = multiprocessing.get_context("fork")
        with ctx.Pool(jobs) as pool:
            units = pool.map(unit_for_source, paths, chunksize=1)
    except (ValueError, OSError):
        units = [unit_for_source(p) for p in paths]
    result = Measure()
    result.listed = config.listed
    live_headers = defaultdict(set)
    norms = {}
    for source, unit in zip(sources, units):
        if "error" in unit:
            result.failed.append(source)
            continue
        result.directives_only = result.directives_only and unit["directives_only"]
        for header, lines in unit["headers"].items():
            live_headers[header].update(lines)
        add_unit(result, source, unit)
        norms[source] = unit["norm"]
    for header in sorted(set(live_headers) & set(top_level(config, ".h"))):
        text = live_header_text(config, header, live_headers[header])
        unit = scan_unit_text(text, _HEADER_SYMBOLS, False)
        add_unit(result, header, unit)
        norms[header] = unit["norm"]
    for name, blocks in count_duplicates(norms).items():
        result.metrics(name)["dup"] = blocks
    if MOUSE_FILE in sources and MOUSE_FILE not in result.failed:
        seen = set(units[sources.index(MOUSE_FILE)].get("mouse", ()))
        result.mouse_missing = [s for s in MOUSE_SYMBOLS if s not in seen]
    return result


def add_unit(result, name, unit):
    """Fold one file's findings into the run."""
    for kind in COUNTED:
        if unit[kind]:
            result.found[kind][name] = [row if isinstance(row, int) else row[0]
                                        for row in unit[kind]]
    result.metrics(name)
    for fn, first, last, nest, params in unit.get("funcs", ()):
        length = last - first + 1
        row = result.metrics(name)
        row["longest"] = max(row["longest"], length)
        row["nest"] = max(row["nest"], nest)
        row["params"] += 1 if params > PARAMS_OVER else 0
        result.functions.append((name, fn, length, nest, params))
    result.metrics(name)["statics"] = len(unit.get("statics", ()))


# ---------------------------------------------------------------------------
# The allow file and the verdict


def read_allow(path):
    """{(kind, file): fields} from the allow file; a bad line is reported."""
    allow = {}
    problems = []
    if not path or not os.path.exists(path):
        return allow, problems
    with open(path, encoding="utf-8") as fh:
        for number, raw in enumerate(fh, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            fields = line.split()
            kind = fields[0]
            if kind in COUNTED and len(fields) >= 4 and fields[2].isdigit():
                allow[(kind, fields[1])] = {"count": int(fields[2]),
                                            "pass": fields[3], "line": number}
            elif kind == "ceiling" and len(fields) == 7 \
                    and all(f.isdigit() for f in fields[2:]):
                allow[(kind, fields[1])] = {"values": [int(f) for f in fields[2:]],
                                            "line": number}
            elif kind == "reference" and len(fields) >= 2 \
                    and all(re.match(r"^[A-Za-z0-9-]+=(yes|no)$", f)
                            for f in fields[1:]) \
                    and REFERENCE not in allow:
                allow[REFERENCE] = {"words": parse_words(" ".join(fields[1:])),
                                    "line": number}
            else:
                problems.append("%s:%d: not a line of the grammar: %s"
                                % (path, number, line))
    return allow, problems


class Notes:
    """What a build other than the reference reports instead of failing."""

    def __init__(self):
        self.below = []           # "allow:N: kind file measures M not C"
        self.not_built = set()    # feature words whose files this build lacks
        self.not_built_lines = 0


def reference_state(allow, features):
    """(is the reference configuration, what to say, problems).

    The reference is the allow file's `reference` line: every word the binary
    prints must be named there, so a word configure gains cannot make every
    build silently "not the reference"."""
    line = allow.get(REFERENCE)
    if line is None:
        return False, None, ["the allow file has no `reference word=yes ...` line"]
    if features is None:
        return False, ("configuration unknown (run through "
                       "check-maintainability.sh): not the reference"), []
    ref = line["words"]
    problems = ["allow:%d: the reference line does not say %s, which this "
                "build prints" % (line["line"], word)
                for word in sorted(features) if word not in ref]
    problems += ["allow:%d: the reference line names %s, which this build "
                 "does not print" % (line["line"], word)
                 for word in sorted(ref) if word not in features]
    if problems:
        return False, None, problems
    differ = ["%s=%s" % (w, features[w]) for w in sorted(ref) if features[w] != ref[w]]
    if differ:
        return False, "not the reference configuration (%s)" % " ".join(differ), []
    return True, "the reference configuration", []


def judge_counted(result, allow, strict, built, reference=True, config=None,
                  notes=None):
    """The errors of rows 3 to 5.

    A count above its line fails in every build.  A line above the
    measurement is stale only in the reference configuration; elsewhere it
    goes to notes, as "not built here" when the file is a feature's the
    build lacks."""
    errors = []
    notes = notes if notes is not None else Notes()
    for kind in COUNTED:
        for name, lines in sorted(result.found[kind].items()):
            if kind == "getenv" and name in GETENV_OWNERS:
                continue
            line = allow.get((kind, name))
            allowed = 0 if strict or line is None else line["count"]
            if len(lines) > allowed:
                errors.append("%s: %s: %d at lines %s, %d allowed"
                              % (name, ROW_NAMES[kind], len(lines),
                                 ",".join(str(n) for n in lines[:12]), allowed))
        if strict:
            continue
        for (k, name), line in sorted(allow.items()):
            if k != kind:
                continue
            measured = len(result.found[kind].get(name, ()))
            if measured >= line["count"]:
                continue
            word = config.not_built_word(name) if config else None
            if name not in built and word:
                notes.not_built.add(word)
                notes.not_built_lines += 1
                continue
            text = ("allow:%d: %s %s measures %d not %d"
                    % (line["line"], kind, name, measured, line["count"]))
            if reference:
                errors.append(text.replace(" measures", " is stale, it measures")
                              + ": lower the line")
            else:
                notes.below.append(text)
    return errors


def judge_mouse(result):
    """The error of row 6."""
    if result.mouse_missing:
        return ["%s: row 6, the console mouse is gone, missing %s"
                % (MOUSE_FILE, ", ".join(result.mouse_missing))]
    return []


def judge(result, allow, strict, built, reference=True, config=None, notes=None):
    """Every error of the strict rows."""
    errors = judge_counted(result, allow, strict, built, reference, config, notes)
    errors += judge_mouse(result)
    if result.listed:
        errors += ["%s: the compiler refused the source, the build named it" % s
                   for s in result.failed]
    return errors


CEILING_FIELDS = ("longest", "nest", "params", "dup", "statics")


def report(result, allow, out):
    """Rows 1 and 2, report only: the table, and what moved against the lines."""
    over_len = [f for f in result.functions if f[2] > TREE_LONGEST]
    over_nest = [f for f in result.functions if f[3] > TREE_NEST]
    files = {f[0] for f in over_len} | {f[0] for f in over_nest}
    out("rows 1-2 (report only): %d files, %d functions; %d over %d lines, "
        "%d nested deeper than %d, in %d files"
        % (len(result.files), len(result.functions), len(over_len), TREE_LONGEST,
           len(over_nest), TREE_NEST, len(files)))
    if not result.directives_only:
        out("rows 1-2: this compiler has no -fdirectives-only, macros are "
            "expanded and the numbers are not the reference's")
    out("  %-18s %7s %5s %6s %4s %7s" % ("file", "longest", "nest", ">6par",
                                         "dup", "statics"))
    ranked = sorted(result.files.items(), key=lambda kv: -kv[1]["longest"])
    for name, row in ranked[:REPORT_ROWS]:
        out("  %-18s %7d %5d %6d %4d %7d" % ((name,) + tuple(row[f] for f in CEILING_FIELDS)))
    ups, downs = [], []
    for name, row in sorted(result.files.items()):
        line = allow.get(("ceiling", name))
        if line is None:
            continue
        for field, limit in zip(CEILING_FIELDS, line["values"]):
            if row[field] > limit:
                ups.append("%s %s %d > %d" % (name, field, row[field], limit))
            elif row[field] < limit:
                downs.append("%s %s %d < %d" % (name, field, row[field], limit))
    out("rows 1-2: %d measures above their ceiling line, %d below (not failed)"
        % (len(ups), len(downs)))
    for text in ups[:REPORT_ROWS]:
        out("  above: " + text)


def say_notes(said, notes, out):
    """Which configuration this is, and what it reported instead of failing."""
    if not said:
        return
    text = "rows 3-5: " + said
    if notes.below:
        text += "; %d line(s) above the measurement, reported, not failed" \
            % len(notes.below)
    if notes.not_built:
        text += "; %d line(s) not built here: %s" % (
            notes.not_built_lines,
            ", ".join(option_of(w) for w in sorted(notes.not_built)))
    out(text)
    for line in notes.below:
        out("  below its line here: " + line)


def allow_lines(result):
    """The allow file this measurement would need, for --list."""
    lines = []
    for kind in COUNTED:
        for name, found in sorted(result.found[kind].items()):
            if kind == "getenv" and name in GETENV_OWNERS:
                continue
            owner = "C6" if kind == "getenv" else "T14"
            lines.append("%s %s %d %s" % (kind, name, len(found), owner))
    for name, row in sorted(result.files.items()):
        if not any(row[f] for f in CEILING_FIELDS):
            continue
        lines.append("ceiling %s %s" % (name, " ".join(str(row[f]) for f in CEILING_FIELDS)))
    return lines


# ---------------------------------------------------------------------------
# The self-test: one planted offence per row, and a clean tree that passes

PLANT_HEADER = ("typedef struct planted_colours { int planted_member; } PLANTED;\n"
                "static inline int planted_inline(int v)\n{\n  return v;\n}\n"
                "extern int shared_counter(int step);\n"
                "int planted_with_member(PLANTED *planted_param);\n")


def plant_source():
    """A source with an offence in every row and every metric.

    Two f() declarations (a plain type and a typedef'd one), and two externs
    of a header's functions (one declared after an inline definition) beside
    two that only share a name with a struct member or a parameter, which
    row 4 must not count."""
    body = ["int planted_old_style();",
            "PLANTED *planted_typed_old_style();",
            "extern int shared_counter(int step);",
            "extern int planted_inline(int v);",
            "extern int planted_member;",
            "extern int planted_param;",
            "static int shared_total;",
            "int planted_env(void)",
            "{",
            "  return getenv(\"XWPE_PLANTED\") != 0;",
            "}",
            "int planted_many(int a, int b, int c, int d, int e, int f, int g)",
            "{",
            "  return a + b + c + d + e + f + g;",
            "}",
            "int planted_deep(int v)",
            "{"]
    for level in range(1, 7):
        body.append("%sif (v > %d) {" % ("  " * level, level))
    body.append("            v++;")
    body += ["%s}" % ("  " * level) for level in range(6, 0, -1)]
    body += ["  return v;", "}"]
    block = ["  total = total + 1;", "  count = count + 2;",
             "  step = total * count;", "  scale = step / 3;",
             "  shared_total = shared_total + scale;",
             "  fprintf(stderr, \"block %d\\n\", scale);"]
    for name in ("planted_dup_a", "planted_dup_b"):
        body += ["int %s(int total, int count, int step, int scale)" % name,
                 "{"] + block + ["  return shared_total;", "}"]
    body += ["int planted_long(int v)", "{"]
    body += ["  v = v + %d;" % n for n in range(TREE_LONGEST + 10)]
    body += ["  return v;", "}"]
    return "#include <stdlib.h>\n#include <stdio.h>\n#include \"fixture.h\"\n" \
        + "\n".join(body) + "\n"


CLEAN_SOURCE = ("#include \"fixture.h\"\n"
                "int clean_one(int v)\n{\n  return v + 1;\n}\n")
TERM_WITH_MOUSE = ("int term_mouse(void)\n{\n"
                   "  mousemask(0, 0);\n  KEY_MOUSE; getmouse(0);\n"
                   "  MEVENT ev; fk_t_mouse = 0;\n  return 0;\n}\n")
TERM_WITHOUT_MOUSE = "int term_plain(void)\n{\n  return 0;\n}\n"


def write_fixture(folder, source, term):
    """The planted folder's three files."""
    for name, text in (("fixture.h", PLANT_HEADER), ("fixture.c", source),
                       (MOUSE_FILE, term)):
        with open(os.path.join(folder, name), "w", encoding="utf-8") as fh:
            fh.write(text)


def selftest(cc):
    """Return a list of failures of the scanner itself (empty when it sees)."""
    failures = []
    with tempfile.TemporaryDirectory(prefix="maintainability.") as folder:
        write_fixture(folder, plant_source(), TERM_WITHOUT_MOUSE)
        config = Config(folder, cc, ["-I" + folder], ["fixture.c", MOUSE_FILE], True)
        result = measure_tree(config, jobs=1)
        errors = judge(result, {}, False, {"fixture.c", MOUSE_FILE, "fixture.h"})
        wanted = (("row 3", "fixture.c: row 3"), ("row 4", "fixture.c: row 4"),
                  ("row 5", "fixture.c: row 5"), ("row 6", MOUSE_FILE + ": row 6"))
        for label, prefix in wanted:
            if not any(e.startswith(prefix) for e in errors):
                failures.append("the planted offence of %s was not named: %r"
                                % (label, errors))
        for kind, want in (("noproto", 2), ("extern", 2)):
            got = len(result.found[kind].get("fixture.c", ()))
            if got != want:
                failures.append("planted %s sites measured %d, expected %d"
                                % (kind, got, want))
        row = result.metrics("fixture.c")
        expect = {"longest": TREE_LONGEST + 10, "nest": 7, "params": 1,
                  "dup": 1, "statics": 1}
        for field, floor in expect.items():
            if row[field] < floor:
                failures.append("planted %s measured %d, expected at least %d"
                                % (field, row[field], floor))
        write_fixture(folder, CLEAN_SOURCE, TERM_WITH_MOUSE)
        result = measure_tree(config, jobs=1)
        built = {"fixture.c", MOUSE_FILE, "fixture.h"}
        errors = judge(result, {}, False, built)
        if errors:
            failures.append("the clean fixture failed: %r" % errors)
        failures += selftest_configurations(result, built)
    return failures


def selftest_configurations(result, built):
    """A stale line fails in the reference configuration only, and a file of
    a feature the build lacks is reported as not built here."""
    failures = []
    stale = {("getenv", "fixture.c"): {"count": 1, "pass": "C6", "line": 1}}
    if not any("is stale" in e for e in judge(result, stale, False, built, True)):
        failures.append("a stale line passed in the reference configuration")
    notes = Notes()
    if judge(result, stale, False, built, False, None, notes) or not notes.below:
        failures.append("a stale line outside the reference was not a note")
    absent = {("extern", "feature.c"): {"count": 2, "pass": "T14", "line": 1}}
    config = Config(".", [], [], [], True, {"with-x": "no"}, {"feature.c": "with-x"})
    notes = Notes()
    if judge(result, absent, False, built, False, config, notes) \
            or notes.not_built != {"with-x"}:
        failures.append("a feature's file this build lacks was not 'not built here'")
    ref = {REFERENCE: {"words": {"with-x": "yes"}, "line": 1}}
    if reference_state(ref, {"with-x": "yes"})[0] is not True \
            or reference_state(ref, {"with-x": "no"})[0] is not False \
            or not reference_state(ref, {"with-x": "yes", "new-word": "no"})[2]:
        failures.append("the reference line was not judged word by word")
    return failures


# ---------------------------------------------------------------------------


def main(argv):
    srcdir = os.environ.get("srcdir", ".")
    here = os.path.dirname(os.path.abspath(__file__))
    allow_path = os.path.join(here, "maintainability.allow")
    strict = listing = do_selftest = False
    args = list(argv)
    while args:
        arg = args.pop(0)
        if arg == "--srcdir":
            srcdir = args.pop(0)
        elif arg == "--allow":
            allow_path = args.pop(0)
        elif arg == "--strict":
            strict = True
        elif arg == "--list":
            listing = True
        elif arg == "--selftest":
            do_selftest = True
        else:
            sys.stderr.write("check-maintainability: unknown argument %s\n" % arg)
            return BAD
    config = build_config(srcdir)
    try:
        subprocess.run(config.cc + ["--version"], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=True)
    except (OSError, subprocess.CalledProcessError):
        print("check-maintainability: SKIP, no compiler (%s)" % " ".join(config.cc))
        return SKIP
    if do_selftest:
        failures = selftest(config.cc)
        for text in failures:
            print("check-maintainability: selftest: " + text)
        if not failures:
            print("check-maintainability: selftest ok, every row names its planted offence")
        return BAD if failures else 0
    result = measure_tree(config)
    if listing:
        if config.features is not None:
            print("reference " + " ".join("%s=%s" % kv for kv in sorted(config.features.items())))
        print("\n".join(allow_lines(result)))
        return 0
    allow, problems = read_allow(allow_path)
    is_reference, said, ref_problems = reference_state(allow, config.features)
    built = set(result.files)
    notes = Notes()
    errors = problems + ref_problems + judge(result, allow, strict, built,
                                             is_reference, config, notes)
    say = lambda text: print("check-maintainability: " + text)
    report(result, allow, say)
    say_notes(said, notes, say)
    for text in errors:
        say(text)
    counted = sum(len(v) for kind in COUNTED for v in result.found[kind].values())
    print("check-maintainability: rows 3-6: %d sites measured, %d errors"
          % (counted, len(errors)))
    return BAD if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
