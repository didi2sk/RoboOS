#!/usr/bin/env python3
"""Net classes into iot-shield.kicad_pro, then Freerouting (DSN -> SES),
zone fill and a DRC report (build/drc.txt).
usage: route.py [/path/to/freerouting.jar [passes]]   (no jar: only the project file, DSN, zones, DRC)"""
import os, sys, json, subprocess
D = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, D)
import circuit

jar = sys.argv[1] if len(sys.argv) > 1 else ""
passes = sys.argv[2] if len(sys.argv) > 2 else "30"

classes, patterns = [], []
for name, (tw, cl, members) in circuit.net_classes.items():
    classes.append({"name": name, "clearance": cl, "track_width": tw, "via_diameter": 0.8 if name == "Power" else 0.6,
                    "via_drill": 0.4 if name == "Power" else 0.3, "microvia_diameter": 0.3, "microvia_drill": 0.1,
                    "diff_pair_width": 0.2, "diff_pair_gap": 0.25, "diff_pair_via_gap": 0.25,
                    "wire_width": 6, "bus_width": 12, "line_style": 0, "pcb_color": "rgba(0, 0, 0, 0.000)",
                    "schematic_color": "rgba(0, 0, 0, 0.000)"})
    for m in members or []:
        patterns.append({"netclass": name, "pattern": m})
pro = {
    "board": {"design_settings": {
        "defaults": {"board_outline_line_width": 0.1, "copper_line_width": 0.2},
        "rules": {"min_clearance": 0.2, "min_track_width": 0.2, "min_via_diameter": 0.6, "min_via_annular_width": 0.13,
                  "min_through_hole_diameter": 0.3, "min_hole_to_hole": 0.5, "min_copper_edge_clearance": 0.3,
                  "min_silk_clearance": 0.0, "min_hole_clearance": 0.25, "solder_mask_to_copper_clearance": 0.0},
        "track_widths": [0.0, 0.25, 0.5, 0.8], "via_dimensions": [{"diameter": 0.0, "drill": 0.0}, {"diameter": 0.8, "drill": 0.4}]}},
    "net_settings": {"classes": classes, "meta": {"version": 3}, "netclass_patterns": patterns},
    "meta": {"filename": "iot-shield.kicad_pro", "version": 1},
    "sheets": [], "text_variables": {},
}
json.dump(pro, open(os.path.join(D, "iot-shield.kicad_pro"), "w"), indent=2)

import pcbnew
pcb = os.path.join(D, "iot-shield.kicad_pcb")
board = pcbnew.LoadBoard(pcb)
dsn = os.path.join(D, "build", "board.dsn")
ses = os.path.join(D, "build", "board.ses")
assert pcbnew.ExportSpecctraDSN(board, dsn), "DSN export"
print("DSN written:", dsn)
if not jar:
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    pcbnew.SaveBoard(pcb, board)
    rep = os.path.join(D, "build", "drc.txt")
    pcbnew.WriteDRCReport(board, rep, pcbnew.EDA_UNITS_MILLIMETRES, True)
    print(open(rep).read()[:1500])
    sys.exit(0)
print("routing ...")
r = subprocess.run(["java", "-Djava.awt.headless=true", "-jar", jar, "-de", dsn, "-do", ses, "-mp", passes, "-mt", "4"],
                   capture_output=True, text=True, timeout=1800)
print(r.stdout[-1500:], r.stderr[-1500:])
assert os.path.exists(ses), "no SES"
assert pcbnew.ImportSpecctraSES(board, ses), "SES import"
pcbnew.ZONE_FILLER(board).Fill(board.Zones())
pcbnew.SaveBoard(pcb, board)
rep = os.path.join(D, "build", "drc.txt")
pcbnew.WriteDRCReport(board, rep, pcbnew.EDA_UNITS_MILLIMETRES, True)
print(open(rep).read()[-3000:])
