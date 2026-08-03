#!/usr/bin/env python3
"""Count strongly red pixels in the phase dumps of a run.

The immediate-mode batching artefact draws as a large red and purple band, so
the peak count across a run's frames separates a clean run from a corrupted one
by three orders of magnitude. A clean frame reads in the hundreds, the band in
the hundreds of thousands.

    measure.py <dump-dir> [<dump-dir> ...]

Prints the per-frame counts and the peak for each directory.
"""

import sys
from pathlib import Path

# strongly red, chosen to ignore the game's own red UI accents
R_MIN, G_MAX, B_MAX = 140, 70, 70


def read_ppm(path):
    data = path.read_bytes()
    fields, offset = [], 0
    while len(fields) < 4:
        end = offset
        while data[end : end + 1] not in (b" ", b"\n", b"\t", b"\r"):
            end += 1
        token = data[offset:end]
        if token.startswith(b"#"):
            offset = data.index(b"\n", offset) + 1
            continue
        fields.append(token)
        offset = end + 1
    if fields[0] != b"P6":
        raise ValueError(f"{path}: not a P6 PPM")
    return int(fields[1]), int(fields[2]), data[offset:]


def count_red(path):
    _, _, pixels = read_ppm(path)
    return sum(
        1
        for i in range(0, len(pixels) - 2, 3)
        if pixels[i] > R_MIN and pixels[i + 1] < G_MAX and pixels[i + 2] < B_MAX
    )


def main(dirs):
    for d in dirs:
        frames = sorted(Path(d).glob("*.ppm"))
        if not frames:
            print(f"{d}: no dumps")
            continue
        counts = [(f.name, count_red(f)) for f in frames]
        peak = max(c for _, c in counts)
        print(f"{d}: peak {peak} across {len(counts)} frames")
        for name, c in counts:
            print(f"    {c:>8}  {name}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    main(sys.argv[1:])
