#!/usr/bin/env python3
"""A covering array of the option factors, with mixed strength.

Every pair of factors is covered in some row; the factors of each
``STRENGTH_3`` group are covered three at a time.  The generator is the
greedy one of AETG: each new row is the best of a few random candidates, each
built factor by factor to cover as many uncovered tuples as it can, with a
constraint that rules out combinations no session can have.  It is seeded,
so the same seed gives the same array, which is checked in as
``option_rows.tsv`` together with the command that regenerates it.
``missing`` verifies the coverage of any array, so a different tool's output
(PICT's) can replace the file.

Regenerate (the output goes to the file by the editor, not by a redirect)::

    tests/.venv/bin/python tests/edit_model/covering.py --seed 20261112
"""
import argparse
import itertools
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if os.path.dirname(HERE) not in sys.path:
    sys.path.insert(0, os.path.dirname(HERE))

from edit_model import option_row  # noqa: E402

TSV = os.path.join(HERE, "option_rows.tsv")
DEFAULT_SEED = 20261112
CANDIDATES = 12
ROW_PREFIX = "r"
HEADER_MARK = "#"
STRENGTH_PAIR = 2
STRENGTH_GROUP = 3


def _tuples_of(names, factors, strength):
    for combo in itertools.combinations(names, strength):
        for levels in itertools.product(*(factors[n] for n in combo)):
            yield tuple(zip(combo, levels))


def required(factors=None, groups=None):
    """The tuples an array must cover: every pair, and every triple inside
    a strength-3 group; tuples a constraint rules out are left out."""
    factors = factors or option_row.FACTORS
    groups = option_row.STRENGTH_3 if groups is None else groups
    names = list(factors)
    out = set(_tuples_of(names, factors, STRENGTH_PAIR))
    for g in groups:
        out.update(_tuples_of(list(g), factors, STRENGTH_GROUP))
    return set(t for t in out if not option_row.violates(dict(t)))


def covered_by(row):
    """The tuples of required() that one row (a dict) covers."""
    items = sorted(row.items())
    out = set()
    for strength in (STRENGTH_PAIR, STRENGTH_GROUP):
        for combo in itertools.combinations(items, strength):
            out.add(tuple(sorted(combo)))
    return out


def _key(t):
    return tuple(sorted(t))


def missing(rows, factors=None, groups=None):
    """The required tuples no row covers."""
    need = set(_key(t) for t in required(factors, groups))
    for row in rows:
        need -= covered_by(row)
    return need


def _score(row, f, level, index):
    """How many uncovered tuples would be completed by setting @f to @level
    in @row (the other members already set)."""
    n = 0
    for tup in index.get((f, level), ()):
        if all(row.get(g) == lv for g, lv in tup if g != f):
            n += 1
    return n


def _candidate(rng, factors, uncovered, index):
    row = {}
    seed_tuple = rng.choice(sorted(uncovered))
    for f, lv in seed_tuple:
        row[f] = lv
    order = [f for f in factors if f not in row]
    rng.shuffle(order)
    for f in order:
        levels = list(factors[f])
        rng.shuffle(levels)
        best, best_score = None, -1
        for lv in levels:
            trial = dict(row)
            trial[f] = lv
            if option_row.violates(trial):
                continue
            s = _score(row, f, lv, index)
            if s > best_score:
                best, best_score = lv, s
        row[f] = best
    return row


def generate(seed=DEFAULT_SEED, factors=None, groups=None):
    """The array as a list of dicts, for the seed."""
    factors = factors or option_row.FACTORS
    rng = random.Random(seed)
    uncovered = set(_key(t) for t in required(factors, groups))
    rows = []
    while uncovered:
        index = {}
        for tup in uncovered:
            for f, lv in tup:
                index.setdefault((f, lv), []).append(tup)
        best, best_gain = None, -1
        for _ in range(CANDIDATES):
            cand = _candidate(rng, factors, uncovered, index)
            gain = len(covered_by(cand) & uncovered)
            if gain > best_gain:
                best, best_gain = cand, gain
        rows.append(best)
        uncovered -= covered_by(best)
    return rows


def to_tsv(rows, seed, factors=None):
    factors = factors or option_row.FACTORS
    names = list(factors)
    out = ["%s regenerate: tests/.venv/bin/python tests/edit_model/covering.py --seed %d"
           % (HEADER_MARK, seed),
           "%s every pair of factors, and every triple inside the groups %s"
           % (HEADER_MARK, "; ".join("+".join(g) for g in option_row.STRENGTH_3)),
           "\t".join(["row"] + names)]
    for i, row in enumerate(rows):
        out.append("\t".join(["%s%03d" % (ROW_PREFIX, i)] + [row[n] for n in names]))
    return "\n".join(out) + "\n"


def load(path=TSV):
    """The checked-in array: [option_row.Row]."""
    rows = []
    names = None
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line or line.startswith(HEADER_MARK):
                continue
            cells = line.split("\t")
            if names is None:
                names = cells[1:]
                continue
            rows.append(option_row.Row(dict(zip(names, cells[1:])), cells[0]))
    return rows


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--seed", type=int, default=DEFAULT_SEED)
    ap.add_argument("--check", action="store_true",
                    help="verify the checked-in file's coverage and exit")
    args = ap.parse_args(argv)
    if args.check:
        left = missing([r.levels() for r in load()])
        print("uncovered tuples: %d" % len(left))
        return 1 if left else 0
    rows = generate(args.seed)
    sys.stdout.write(to_tsv(rows, args.seed))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
