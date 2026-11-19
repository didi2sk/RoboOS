#!/usr/bin/env python3
"""bom.csv (JLCPCB style: Comment, Designator, Footprint, LCSC) and cpl.csv (positions) from circuit.py / the board."""
import os, sys, csv
D = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, D)
import circuit

groups = {}
for p in circuit.parts:
    if p["ref"].startswith("H"):
        continue
    k = (p["value"], p["fp"].split(":")[1], p["lcsc"])
    groups.setdefault(k, []).append(p["ref"])
key = lambda r: (r.rstrip("0123456789"), int(r[len(r.rstrip("0123456789")):]))
with open(os.path.join(D, "bom.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["Comment", "Designator", "Footprint", "LCSC", "Qty"])
    for (v, fp, l), refs in sorted(groups.items(), key=lambda g: key(sorted(g[1], key=key)[0])):
        w.writerow([v, ",".join(sorted(refs, key=key)), fp, l, len(refs)])
print("bom.csv:", sum(len(r) for r in groups.values()), "parts in", len(groups), "lines")

try:
    import pcbnew
except ImportError:
    sys.exit(0)
b = pcbnew.LoadBoard(os.path.join(D, "iot-shield.kicad_pcb"))
with open(os.path.join(D, "cpl.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
    for fp in sorted(b.GetFootprints(), key=lambda f: key(f.GetReference())):
        if fp.GetReference().startswith("H"):
            continue
        pos = fp.GetPosition()
        w.writerow([fp.GetReference(), "%.3fmm" % (pcbnew.ToMM(pos.x) - 100), "%.3fmm" % (100 - pcbnew.ToMM(pos.y)),
                    "Bottom" if fp.IsFlipped() else "Top", "%.0f" % fp.GetOrientationDegrees()])
print("cpl.csv written (JLCPCB may need rotation fixes per part - check their preview)")
