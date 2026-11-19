#!/usr/bin/env python3
"""parts.csv: the shopping list for the hand-soldered board (grouped by value + footprint)."""
import csv, os, sys
D = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, D)
import circuit

groups = {}
for p in circuit.parts:
    if p["ref"].startswith("H"):
        continue
    k = (p["value"], p["fp"].split(":")[1])
    groups.setdefault(k, []).append(p)


def key(ref):
    i = 0
    while i < len(ref) and not ref[i].isdigit():
        i += 1
    return ref[:i], int(ref[i:] or 0)


rows = sorted(groups.items(), key=lambda kv: key(kv[1][0]["ref"]))
with open(os.path.join(D, "parts.csv"), "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["Qty", "Value", "Refs", "Footprint", "Note"])
    for (value, fp), ps in rows:
        refs = sorted((p["ref"] for p in ps), key=key)
        w.writerow([len(ps), value, " ".join(refs), fp, ps[0]["note"]])
print("parts.csv:", sum(len(v) for v in groups.values()), "parts in", len(groups), "lines")
