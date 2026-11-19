#!/usr/bin/env python3
"""bom.csv (JLCPCB style: Comment, Designator, Footprint, LCSC) and cpl.csv (positions) from circuit.py / the board."""
import os, sys, csv
D = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, D)
import circuit

groups = {}
for p in circuit.parts:
    if p["ref"].startswith("H") or p["ref"] in circuit.dnp:
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
def rot(fp):
    name = fp.GetFPIDAsString().split(":")[-1]
    add = next((v for k, v in fix.items() if name.startswith(k)), 0)
    return (fp.GetOrientationDegrees() + add) % 360

b = pcbnew.LoadBoard(os.path.join(D, "iot-shield.kicad_pcb"))
with open(os.path.join(D, "cpl.csv"), "w", newline="") as f:
    w = csv.writer(f)
    # same coordinates as the Gerbers (KiCad Y down -> Gerber Y up), so JLCPCB needs no alignment
    # JLCPCB's part models are turned against KiCad's footprints for these packages
    # (checked in their placement preview: pin 1 dot vs the KiCad pin-1 triangle)
    fix = {"SOIC-": 270, "TSSOP-": 270, "TSOT-23": 270, "SOT-23": 180}
    w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
    for fp in sorted(b.GetFootprints(), key=lambda f: key(f.GetReference())):
        if fp.GetReference().startswith("H") or fp.GetReference() in circuit.dnp:
            continue
        pos = fp.GetPosition()
        w.writerow([fp.GetReference(), "%.3fmm" % pcbnew.ToMM(pos.x), "%.3fmm" % -pcbnew.ToMM(pos.y),
                    "Bottom" if fp.IsFlipped() else "Top", "%.0f" % rot(fp)])
print("cpl.csv written (rotations corrected for JLCPCB, see fix)")
