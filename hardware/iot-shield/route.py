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
    classes.append({"name": name, "clearance": cl, "track_width": tw, "via_diameter": 0.8 if name != "Default" else 0.6,
                    "via_drill": 0.4 if name != "Default" else 0.3, "microvia_diameter": 0.3, "microvia_drill": 0.1,
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
from sexp import parse, find, first


def import_ses(board, path):
    """tracks and vias from a Freerouting session file (pcbnew.ImportSpecctraSES needs the editor in KiCad 7)"""
    t = parse(open(path).read())
    routes = first(t, "routes")
    res = first(routes, "resolution")
    div = {"um": 1000.0, "mil": 1 / 0.0254, "mm": 1.0}[str(res[1])] * float(res[2])   # file units per mm
    pt = lambda x, y: pcbnew.VECTOR2I(pcbnew.FromMM(float(x) / div), pcbnew.FromMM(-float(y) / div))
    vias = {}
    lib = first(routes, "library_out")
    for ps in find(lib or [], "padstack"):
        import re
        m = re.search(r"_(\d+):(\d+)_um", ps[1])
        if m:
            vias[ps[1]] = (int(m.group(1)) / 1000.0, int(m.group(2)) / 1000.0)
    layers = {"F.Cu": pcbnew.F_Cu, "B.Cu": pcbnew.B_Cu}
    nets = board.GetNetsByName()
    old = [x for x in board.GetTracks()]
    for x in old:
        board.Remove(x)
    nt = nv = 0
    for net in find(first(routes, "network_out"), "net"):
        ni = nets[net[1]]
        for w in find(net, "wire"):
            path = first(w, "path")
            layer, width, xy = layers[str(path[1])], float(path[2]) / div, path[3:]
            for i in range(0, len(xy) - 2, 2):
                tr = pcbnew.PCB_TRACK(board)
                tr.SetStart(pt(xy[i], xy[i + 1])); tr.SetEnd(pt(xy[i + 2], xy[i + 3]))
                tr.SetWidth(pcbnew.FromMM(width)); tr.SetLayer(layer); tr.SetNet(ni)
                board.Add(tr); nt += 1
        for v in find(net, "via"):
            d, dr = vias.get(v[1], (0.6, 0.3))
            via = pcbnew.PCB_VIA(board)
            via.SetPosition(pt(v[2], v[3])); via.SetWidth(pcbnew.FromMM(d)); via.SetDrill(pcbnew.FromMM(dr))
            via.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu); via.SetNet(ni)
            board.Add(via); nv += 1
    print("SES imported:", nt, "track segments,", nv, "vias")


pcb = os.path.join(D, "iot-shield.kicad_pcb")
board = pcbnew.LoadBoard(pcb)
dsn = os.path.join(D, "build", "board.dsn")
ses = os.path.join(D, "build", "board.ses")
zones = list(board.Zones()) if jar else []
for z in zones:                             # GND is routed as tracks; the planes come back afterwards
    board.Remove(z)
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
import shutil
cmd = ["java", "-jar", jar, "-de", dsn, "-do", ses, "-mp", passes, "-mt", "1", "-da"]   # multi-threaded optimizing makes clearance errors
if not os.environ.get("DISPLAY") and shutil.which("xvfb-run"):   # Freerouting 1.9 wants a screen even in batch mode
    cmd = ["xvfb-run", "-a"] + cmd
if os.path.exists(ses):
    os.remove(ses)
r = subprocess.run(cmd,
                   capture_output=True, text=True, timeout=1800)
print(r.stdout[-1500:], r.stderr[-1500:])
assert os.path.exists(ses), "no SES"
import_ses(board, ses)
for z in zones:
    board.Add(z)
pcbnew.ZONE_FILLER(board).Fill(board.Zones())
pcbnew.SaveBoard(pcb, board)
rep = os.path.join(D, "build", "drc.txt")
pcbnew.WriteDRCReport(board, rep, pcbnew.EDA_UNITS_MILLIMETRES, True)
print(open(rep).read()[-3000:])
