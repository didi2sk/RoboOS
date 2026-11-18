#!/usr/bin/env python3
"""Compares the schematic's netlist (kicad-cli export) with circuit.py."""
import sys, os, re, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sexp import parse, find, first
import circuit
d = os.path.dirname(os.path.abspath(__file__))
subprocess.run(["kicad-cli", "sch", "export", "netlist", "-o", d + "/build/net.net", d + "/iot-shield.kicad_sch"], check=True, capture_output=True)
t = parse(open(d + "/build/net.net").read())
got = {}
for n in find(first(t, "nets"), "net"):
    name = first(n, "name")[1].lstrip("/")
    nodes = frozenset((first(x, "ref")[1], first(x, "pin")[1]) for x in find(n, "node"))
    got[nodes] = name
want = {}
for p in circuit.parts:
    for pin, net in p["pins"].items():
        want.setdefault(net, set()).add((p["ref"], pin))
bad = 0
for net, nodes in want.items():
    nodes = frozenset(nodes)
    if nodes not in got:
        bad += 1
        hit = [(g, sorted(k)[:6]) for k, g in got.items() if k & nodes]
        print("WRONG", net, "->", hit)
    elif got[nodes] != net:
        print("name", net, "is", got[nodes])
extra = [g for k, g in got.items() if not g.startswith("unconnected") and g not in want]
print("nets ok:", len(want) - bad, "of", len(want), "| unexpected:", extra)
sys.exit(1 if bad else 0)
