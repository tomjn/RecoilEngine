#!/usr/bin/env python3
"""Aggregate the A/B run directories into two distributions.

    summarise.py <root> [threshold]

A = glCallList flush on, B = SPRING_NO_LIST_FLUSH=1.

The artefact takes the colour of whichever batch was corrupted, so counting one
hue undercounts. Three hues are counted, and each is scored against its own
per-run baseline: the selected units' range circles put a constant several
hundred red and purple pixels on screen every frame, and only the excess over
that is corruption.
"""

import statistics
import sys
from pathlib import Path

sys.path.insert(0, "/Users/tomjn/dev/RecoilEngine/coding-agents/test-scripts")
from measure import read_ppm  # noqa: E402


def mask(channel, lo=None, hi=None):
    """One 0x01 byte per pixel inside the range, 0x00 outside."""
    table = bytes(
        1 if (lo is None or v > lo) and (hi is None or v < hi) else 0
        for v in range(256)
    )
    return int.from_bytes(channel.translate(table), "big")


def hue_counts(path):
    _, _, px = read_ppm(path)
    r, g, b = px[0::3], px[1::3], px[2::3]
    n = min(len(r), len(g), len(b))
    r, g, b = r[:n], g[:n], b[:n]

    red = mask(r, lo=140) & mask(g, hi=70) & mask(b, hi=70)
    magenta = mask(r, lo=120) & mask(b, lo=120) & mask(g, hi=60)
    lavender = mask(b, lo=150) & mask(r, lo=110) & mask(g, lo=60, hi=150)

    return {
        "red": red.bit_count(),
        "magenta": magenta.bit_count(),
        "lavender": lavender.bit_count(),
    }


def run_scores(run_dir):
    """Per-frame score: the largest of the three hue counts.

    Absolute, not baseline-relative. A per-run baseline is unusable because a
    run whose frames are mostly corrupted moves its own baseline: A-4's median
    lavender was 752008, which scored a run of full-screen corruption as mild.
    The separation is wide enough not to need one. The UI's own range circles
    and accents give about 400 red, 30 magenta and 900 lavender, while a
    corrupted frame reads in the tens or hundreds of thousands.
    """
    frames = sorted(run_dir.glob("*.ppm"))
    if not frames:
        return [], {}
    per_frame = [hue_counts(f) for f in frames]
    baseline = {hue: min(c[hue] for c in per_frame) for hue in per_frame[0]}
    scores = [max(c.values()) for c in per_frame]
    return scores, baseline


def main(root, threshold):
    summary = {}
    for side in ("A", "B"):
        pooled, lines = [], []
        for d in sorted(Path(root).glob(f"{side}-*")):
            scores, baseline = run_scores(d)
            if not scores:
                continue
            bad = sum(1 for s in scores if s > threshold)
            lines.append(
                f"  {d.name}: peak {max(scores):>8}  {bad}/{len(scores)} frames over"
                f" {threshold}   baseline r/m/l"
                f" {baseline['red']:.0f}/{baseline['magenta']:.0f}/{baseline['lavender']:.0f}"
            )
            pooled.extend(scores)
        print(f"\n=== side {side}")
        print("\n".join(lines))
        if pooled:
            bad = sum(1 for s in pooled if s > threshold)
            print(
                f"  pooled: {len(pooled)} frames  peak {max(pooled)}"
                f"  median {int(statistics.median(pooled))}"
                f"  corrupted {bad} = {100.0 * bad / len(pooled):.1f}%"
            )
            summary[side] = (bad, len(pooled), max(pooled))
    if len(summary) == 2:
        (ab, an, ap), (bb, bn, bp) = summary["A"], summary["B"]
        print(
            f"\nA = glCallList flush on: {ab}/{an} corrupted, peak {ap}"
            f"\nB = flush off:           {bb}/{bn} corrupted, peak {bp}"
        )


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 3000)
