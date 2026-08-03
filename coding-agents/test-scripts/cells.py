#!/usr/bin/env python3
"""Summarise an interleaved run's per-cell frame rates.

    cells.py <logfile> [--keep-first-cycle]

Reads the `[diag] cycle=N cell=X fps=Y` lines the engine writes when
SPRING_DIAG_CELLS is set, and compares the cells as a paired sample: one cycle
gives one reading of every cell, all under the same scene, focus state and driver
state, so cycles are the pairs.

The first cycle is dropped by default. It contains the loading screen and the
first seconds of play, which draw nothing like the frozen scene.

The test is a two-sided sign test on the paired cycles. It asks only how often
one cell beat the other, which needs no assumption about the distribution and is
honest about small samples: five cycles cannot produce a p below 0.0625 whatever
the effect size.
"""

import itertools
import re
import statistics
import sys
from math import comb

LINE = re.compile(r"\[diag\] cycle=(\d+) cell=(\S+) fps=([\d.]+) frames=(\d+) over ([\d.]+)s")


def read(path):
    """{cell: {cycle: fps}}"""
    out = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LINE.search(line)
            if m:
                cycle, cell, fps = int(m.group(1)), m.group(2), float(m.group(3))
                out.setdefault(cell, {})[cycle] = fps
    return out


def sign_test(pairs):
    """Two-sided sign test. pairs is a list of (a, b). Returns (wins, ties, p)."""
    wins = sum(1 for a, b in pairs if a > b)
    losses = sum(1 for a, b in pairs if a < b)
    ties = len(pairs) - wins - losses
    n = wins + losses
    if n == 0:
        return wins, ties, 1.0
    k = min(wins, losses)
    tail = sum(comb(n, i) for i in range(k + 1)) / (2 ** n)
    return wins, ties, min(1.0, 2 * tail)


def main(argv):
    path = argv[1]
    keep_first = "--keep-first-cycle" in argv

    data = read(path)
    if not data:
        print(f"no [diag] cell lines in {path}. Was SPRING_DIAG_CELLS set?")
        return 1

    cycles = sorted(set(itertools.chain.from_iterable(d.keys() for d in data.values())))
    if not keep_first:
        cycles = [c for c in cycles if c != min(cycles)]
        if not cycles:
            print("only one cycle in the log, so nothing survives dropping the first.")
            print("Run longer, or pass --keep-first-cycle and distrust the result.")
            return 1

    cells = sorted(data)
    width = max(len(c) for c in cells)

    print(f"cycles used: {cycles}")
    print()
    print("cycle  " + "  ".join(f"{c:>{max(width, 7)}}" for c in cells))
    for cy in cycles:
        row = [f"{data[c].get(cy, float('nan')):>{max(width, 7)}.2f}" for c in cells]
        print(f"{cy:>5}  " + "  ".join(row))

    print()
    for c in cells:
        vals = [data[c][cy] for cy in cycles if cy in data[c]]
        if vals:
            print(
                f"{c:>{width}}: n={len(vals)} median {statistics.median(vals):6.2f}"
                f"  min {min(vals):6.2f}  max {max(vals):6.2f}"
            )

    print()
    for a, b in itertools.combinations(cells, 2):
        pairs = [(data[a][cy], data[b][cy]) for cy in cycles if cy in data[a] and cy in data[b]]
        if not pairs:
            continue
        wins, ties, p = sign_test(pairs)
        deltas = [x - y for x, y in pairs]
        print(
            f"{a} vs {b}: {a} faster in {wins}/{len(pairs)} cycles"
            f"{f' ({ties} tied)' if ties else ''},"
            f" median delta {statistics.median(deltas):+.2f} fps, sign test p={p:.4f}"
        )

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
