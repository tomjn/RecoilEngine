#!/usr/bin/env python3
"""Score the stray-line artefact in the minimap, per diagnostic cell.

    minimap_score.py <logfile> [screenshot dir]

game_metal_spot_minimap_drawer.lua issues one gl.BeginEnd(GL_LINE_LOOP) per metal
spot, so the minimap draws dozens of consecutive identical batches every frame.
When two of them merge, the loop closes across two circles instead of separately
and a line appears between them. It is intermittent, most frames are clean, and
it is never the same pair, so a screenshot proves nothing on its own and the only
honest way to compare two configurations is as a distribution.

The map never changes and the minimap sits at a fixed screen position, so the
green pixel count is constant apart from the artefact. The score is the excess
over the run's own minimum, which is the count for a clean frame.

Shots are matched to cells through the log rather than the filename, because the
engine names screenshots by wall-clock time and knows nothing about the schedule.
Each "[shots] taking" line carries a timestamp, and each "[diag] cycle= cell="
line marks the end of the cell it names, so the cell active at a shot is the
first one to end at or after it.
"""

import math
import re
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from crop import read_png  # noqa: E402

# The minimap in a 3024x1832 window, in that window's pixels.
MINIMAP = (12, 0, 695, 521)


def is_minimap_green(r, g, b):
    return g > 150 and r < 120 and b < 120


# Where widget_loop_amp.lua draws, converted from its bottom-up DrawScreen
# coordinates to the top-down image, and inset so the count is taken strictly
# inside the widget's own opaque backdrop.
AMP = (1600, 480, 2860, 1310)


def is_amp_cyan(r, g, b):
    return g > 150 and b > 150 and r < 100


REGIONS = {"minimap": (MINIMAP, is_minimap_green), "amp": (AMP, is_amp_cyan)}

# The amplifier's grid in image coordinates, from widget_loop_amp.lua's constants
# with its bottom-up y flipped. A ring pixel sits at radius from the nearest node.
AMP_GRID = (1620, 1331, 22, 7)


def amp_stray_pixels(path):
    """Cyan pixels that are not on a circle, counted absolutely.

    Subtracting a per-run baseline does not work here. Once the artefact appears
    in every frame there is no clean frame to subtract, and the run scores zero
    everywhere while being visibly full of stray lines. That happened.

    Instead this uses the fact that the grid is regular and known. Every legitimate
    pixel lies at the circle radius from a grid node, and a line drawn between two
    circles crosses the empty interiors on its way. Counting pixels away from the
    ring therefore needs no reference frame at all. A histogram over one frame put
    79,711 pixels at radius 6 to 8 and 32 anywhere else, so the separation is not
    marginal.
    """
    ox, oy, sp, r = AMP_GRID
    (x0, y0, x1, y1), match = REGIONS["amp"]
    w, h, px = read_png(str(path))
    x1, y1 = min(x1, w), min(y1, h)

    stray = 0
    for y in range(y0, y1):
        row = y * w * 3
        for x in range(x0, x1):
            i = row + x * 3
            if not match(px[i], px[i + 1], px[i + 2]):
                continue
            dx = (x - ox) % sp
            dx = dx if dx <= sp // 2 else dx - sp
            dy = (oy - y) % sp
            dy = dy if dy <= sp // 2 else dy - sp
            if abs(math.hypot(dx, dy) - r) > 2:
                stray += 1
    return stray

TIME = re.compile(r"^\[t=(\d\d):(\d\d):(\d\d)\.(\d+)\]")
SHOT = re.compile(r"\[shots\] taking (\d+) of")
CELL = re.compile(r"\[diag\] cycle=(\d+) cell=(\S+)")


def stamp(line):
    m = TIME.match(line)
    if m is None:
        return None
    h, mi, s, frac = m.groups()
    return int(h) * 3600 + int(mi) * 60 + int(s) + int(frac) / 10 ** len(frac)


def parse(logfile):
    """Returns (shot times in order, [(cell end time, cell name)])."""
    shots, cells = [], []
    for line in open(logfile, errors="replace"):
        t = stamp(line)
        if t is None:
            continue
        if SHOT.search(line):
            shots.append(t)
        m = CELL.search(line)
        if m is not None:
            cells.append((t, m.group(2)))
    return shots, cells


def cell_at(t, cells):
    for end, name in cells:
        if end >= t:
            return name
    return None


def marked_pixels(path, region):
    """Green circle pixels inside the minimap.

    The circles are a saturated green and nothing else in that rectangle is, so a
    plain threshold separates them from terrain, the white viewport trapezoid and
    the unit markers without needing a baseline per hue.
    """
    (x0, y0, x1, y1), match = REGIONS[region]
    w, h, px = read_png(str(path))
    x1, y1 = min(x1, w), min(y1, h)

    count = 0
    for y in range(y0, y1):
        row = y * w * 3
        for x in range(x0, x1):
            i = row + x * 3
            r, g, b = px[i], px[i + 1], px[i + 2]
            if match(r, g, b):
                count += 1
    return count


def main(logfile, shotdir, region):
    shots, cells = parse(logfile)

    if not shots:
        print("no [shots] lines in the log, was --shots used?")
        return 1

    # The run's own screenshots are the last len(shots) by modification time.
    files = sorted(Path(shotdir).glob("*.png"), key=lambda p: p.stat().st_mtime)
    files = files[-len(shots):]

    if len(files) != len(shots):
        print(f"log has {len(shots)} shots but only {len(files)} files, refusing to guess")
        return 1

    scored = []
    for t, f in zip(shots, files):
        score = amp_stray_pixels(f) if region == "amp" else marked_pixels(f, region)
        scored.append((cell_at(t, cells), score, f.name))

    # The baseline is the smallest count among frames that actually drew, not the
    # smallest overall. A shot taken before the widget started drawing reads near
    # zero, and using that as the reference makes every later frame score the
    # whole drawing as excess. That voided one run: every cell read about 10,000.
    if region == "amp":
        # Absolute, see amp_stray_pixels. No frame is needed as a reference.
        clean = 0
        print(f"{region}: stray pixels counted against the known grid\n")
    else:
        counts = sorted(s for _, s, _ in scored)
        typical = counts[len(counts) // 2]
        drew = [s for s in counts if s > typical * 0.5]

        if not drew:
            print("no frame drew anything, check the region and the colour test")
            return 1

        clean = min(drew)
        skipped = len(counts) - len(drew)
        print(f"{region}: clean-frame baseline {clean} pixels"
              f"{f', {skipped} frames ignored as not-yet-drawing' if skipped else ''}\n")

    by_cell = {}
    for cell, score, name in scored:
        by_cell.setdefault(cell, []).append(score - clean)
        flag = "  <-- stray" if score - clean > (10 if region == "amp" else 50) else ""
        print(f"  {str(cell):>10}  excess {score - clean:>6}  {name}{flag}")

    print()
    # A shot taken after the last cell ended has no cell, which is not sortable
    # against a string. Those are dropped from the comparison rather than pooled.
    for cell, excess in sorted(by_cell.items(), key=lambda kv: str(kv[0])):
        bad = sum(1 for e in excess if e > (10 if region == "amp" else 50))
        print(f"{str(cell):>10}: n={len(excess)}  median {int(statistics.median(excess))}"
              f"  max {max(excess)}  stray in {bad}/{len(excess)}")

    return 0


if __name__ == "__main__":
    log = sys.argv[1]
    region = "amp" if "--amp" in sys.argv else "minimap"
    rest = [a for a in sys.argv[2:] if not a.startswith("--")]
    shots = rest[0] if rest else str(Path.home() / "dev/spring-testdata/screenshots")
    sys.exit(main(log, shots, region))
