#!/usr/bin/env python3
"""Builds iot-shield.kicad_pcb from circuit.py with the KiCad 7 pcbnew API:
HAT outline 65 x 56.5 mm, footprints placed by hand (table below), nets from
the circuit, GND planes on both layers. Routing: route.sh (Freerouting)."""
import os, sys, json
import pcbnew
from pcbnew import VECTOR2I, FromMM, ToMM

D = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, D)
import circuit

FPLIB = "/usr/share/kicad/footprints/"
OX, OY = 100.0, 100.0               # board top-left corner in KiCad coordinates
W, H = 65.0, 56.5

# ref: (x, y, rotation, side) in board mm (0,0 = top-left, header along the top edge)
# rotation for terminal blocks is worked out below from the edge they sit on
place = {
    # header: pin 1 at (8.37, 4.77), like the Pi (mounting holes 3.5 mm in)
    "J1": ("header",),
    "J2": ("edge", "left", 11.0),        # pin 1 y
    "J3": ("edge", "left", 19.5),
    "J4": ("edge", "bottom", 9.5),       # pin 1 x
    "J5": ("edge", "bottom", 32.5),
    "H1": (3.5, 3.5, 0), "H2": (61.5, 3.5, 0), "H3": (3.5, 53.0, 0), "H4": (61.5, 53.0, 0),
    # power (top left, under the header row)
    "F1": (10.8, 10.6, 90), "D1": (15.6, 10.6, 90), "D2": (20.2, 11.0, 90),
    "C1": (24.0, 10.6, 90), "C2": (26.0, 10.6, 90),
    "U1": (29.0, 11.4, 0), "C3": (29.0, 8.6, 0), "L1": (34.0, 11.4, 0),
    "C4": (38.6, 9.4, 0), "C5": (38.6, 12.4, 0),
    "R1": (42.3, 9.0, 90), "D3": (42.3, 12.0, 90), "R2": (44.6, 9.0, 90), "D4": (44.6, 12.0, 90),
    "J7": (51.0, 9.4, 180),
    # ADC above J5
    "U2": (40.0, 41.0, 0), "C34": (44.4, 41.0, 90),
    # right side: battery, clock, BME280, 1-Wire
    "BT1": (51.0, 24.0, 90), "U4": (51.6, 38.4, 0), "C41": (56.2, 38.4, 90),
    "U3": (59.4, 45.0, 0), "C40": (57.0, 45.0, 90),
    "J6": (62.0, 32.0, 0), "R40": (60.6, 28.0, 0),
}
for i in range(4):
    x = 14.5 + 6.4 * i                                       # inputs: one column per channel
    place["R%d" % (11 + i)] = (x - 1.4, 19.2, 90)            # 1206 3.3k
    place["D%d" % (11 + i)] = (x + 1.6, 19.2, 90)
    place["U%d" % (11 + i)] = (x, 27.6, 90)
    place["R%d" % (15 + i)] = (x, 34.4, 0)
    x = 9.0 + 5.3 * i                                        # outputs: one column per channel above J4
    place["D%d" % (21 + i)] = (x, 40.0, 90)
    place["Q%d" % (1 + i)] = (x - 0.2, 45.6, 90)
    place["R%d" % (21 + i)] = (x + 2.6, 45.6, 90)
    place["R%d" % (25 + i)] = (x + 2.6, 40.0, 90)
    place["R%d" % (30 + i)] = (32.0 + 3.5 * i, 46.3, 90)
    place["C%d" % (30 + i)] = (33.7 + 3.5 * i, 46.3, 90)


def mm(x, y):
    return VECTOR2I(FromMM(OX + x), FromMM(OY + y))


def pad_pos(fp, num):
    for p in fp.Pads():
        if p.GetNumber() == num:
            return ToMM(p.GetPosition().x) - OX, ToMM(p.GetPosition().y) - OY


def try_orient(fp, want):
    """turn / flip fp until want(fp) is true"""
    for flip in (False, True):
        for r in (0, 90, 180, 270):
            fp.SetOrientationDegrees(r)
            if want(fp):
                return True
        fp.Flip(fp.GetPosition(), True)
    return False


board = pcbnew.NewBoard(os.path.join(D, "iot-shield.kicad_pcb"))
uuids = json.load(open(os.path.join(D, "build", "uuids.json")))

nets = {}
for n in circuit.nets():
    ni = pcbnew.NETINFO_ITEM(board, n)
    board.Add(ni)
    nets[n] = ni

fps = {}
for p in circuit.parts:
    lib, name = p["fp"].split(":")
    fp = pcbnew.FootprintLoad(FPLIB + lib + ".pretty", name)
    fp.SetReference(p["ref"])
    fp.SetValue(p["value"])
    fp.SetFPID(pcbnew.LIB_ID(lib, name))
    fp.SetPath(pcbnew.KIID_PATH("/" + uuids[p["ref"]]))
    if p["lcsc"]:
        fp.SetProperty("LCSC", p["lcsc"])
    board.Add(fp)
    fps[p["ref"]] = fp
    for pad in fp.Pads():
        net = p["pins"].get(pad.GetNumber())
        if net:
            pad.SetNet(nets[net])

    pl = place[p["ref"]]
    if pl[0] == "header":
        fp.SetPosition(mm(0, 0))

        def ok(f):
            a, b, c = pad_pos(f, "1"), pad_pos(f, "2"), pad_pos(f, "3")
            return abs(b[0] - a[0]) < .01 and abs(b[1] - a[1] + 2.54) < .01 and abs(c[0] - a[0] - 2.54) < .01

        if not ok(fp):
            fp.Flip(fp.GetPosition(), True)                # the socket sits under the board
            assert try_orient(fp, ok), "header orientation"
        if not fp.IsFlipped():
            fp.Flip(fp.GetPosition(), True)
            assert try_orient(fp, lambda f: ok(f) and f.IsFlipped()), "header on the bottom"
        a = pad_pos(fp, "1")
        fp.Move(VECTOR2I(FromMM(8.37 - a[0]), FromMM(4.77 - a[1])))
    elif pl[0] == "edge":
        side, start = pl[1], pl[2]
        fp.SetPosition(mm(0, 0))
        n = len(fp.Pads())

        def ok(f):
            a, b = pad_pos(f, "1"), pad_pos(f, str(n))
            bb = f.GetBoundingBox(False, False)
            if side == "left":   # pins downwards, the wire opening (body side +y in the library) to the left
                return abs(a[0] - b[0]) < .01 and b[1] > a[1] and ToMM(bb.GetX()) - OX < a[0] - 4
            return abs(a[1] - b[1]) < .01 and b[0] > a[0] and ToMM(bb.GetBottom()) - OY > a[1] + 4

        assert try_orient(fp, ok), p["ref"]
        a = pad_pos(fp, "1")
        if side == "left":
            fp.Move(VECTOR2I(FromMM(4.6 - a[0]), FromMM(start - a[1])))
        else:
            fp.Move(VECTOR2I(FromMM(start - a[0]), FromMM(H - 4.6 - a[1])))
    else:
        x, y, r = pl
        fp.SetPosition(mm(x, y))
        fp.SetOrientationDegrees(r)
    fp.Reference().SetTextSize(VECTOR2I(FromMM(0.8), FromMM(0.8)))
    fp.Reference().SetTextThickness(FromMM(0.12))

# ---- outline: 65 x 56.5 with 3 mm corners ----
R = 3.0


def seg(x1, y1, x2, y2):
    s = pcbnew.PCB_SHAPE(board)
    s.SetShape(pcbnew.SHAPE_T_SEGMENT)
    s.SetStart(mm(x1, y1)); s.SetEnd(mm(x2, y2))
    s.SetLayer(pcbnew.Edge_Cuts); s.SetWidth(FromMM(0.1))
    board.Add(s)


def arc(cx, cy, sx, sy, ex, ey):
    s = pcbnew.PCB_SHAPE(board)
    s.SetShape(pcbnew.SHAPE_T_ARC)
    import math
    a0, a1 = math.atan2(sy - cy, sx - cx), math.atan2(ey - cy, ex - cx)
    if a1 < a0: a1 += 2 * math.pi
    am = (a0 + a1) / 2
    s.SetArcGeometry(mm(sx, sy), mm(cx + R * math.cos(am), cy + R * math.sin(am)), mm(ex, ey))
    s.SetLayer(pcbnew.Edge_Cuts); s.SetWidth(FromMM(0.1))
    board.Add(s)


seg(R, 0, W - R, 0); seg(W, R, W, H - R); seg(W - R, H, R, H); seg(0, H - R, 0, R)
arc(W - R, R, W - R, 0, W, R); arc(W - R, H - R, W, H - R, W - R, H)
arc(R, H - R, R, H, 0, H - R); arc(R, R, 0, R, R, 0)

# ---- GND planes on both sides ----
for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
    z = pcbnew.ZONE(board)
    z.SetLayer(layer)
    z.SetNet(nets["GND"])
    z.SetLocalClearance(FromMM(0.3))
    z.SetMinThickness(FromMM(0.25))
    z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
    z.SetThermalReliefGap(FromMM(0.3))
    z.SetThermalReliefSpokeWidth(FromMM(0.4))
    ol = z.Outline()
    ol.NewOutline()
    for x, y in ((0.5, 0.5), (W - 0.5, 0.5), (W - 0.5, H - 0.5), (0.5, H - 0.5)):
        ol.Append(FromMM(OX + x), FromMM(OY + y))
    board.Add(z)

# ---- text ----
t = pcbnew.PCB_TEXT(board)
t.SetText("RoboOS IoT shield v0.1")
t.SetPosition(mm(32.5, 28.0))
t.SetLayer(pcbnew.B_SilkS)
t.SetMirrored(True)
t.SetTextSize(VECTOR2I(FromMM(1.0), FromMM(1.0)))
t.SetTextThickness(FromMM(0.15))
board.Add(t)

labels = [("+  12-24V  -", 4.6, 12.75, 90, 1), ("IN1 IN2 IN3 IN4 COM", 4.6, 26.5, 90, 1),
          ("OUT1 OUT2 OUT3 OUT4 V+", 16.5, 52.5, 0, 1), ("3V3 A0 A1 A2 A3 GND", 41.25, 52.5, 0, 1),
          ("3V3 DQ GND", 59.6, 34.5, 90, 0)]
for s, x, y, r, bottom in labels:
    t = pcbnew.PCB_TEXT(board)
    t.SetText(s)
    t.SetPosition(mm(x, y))
    t.SetTextAngleDegrees(r)
    t.SetLayer(pcbnew.B_SilkS if bottom else pcbnew.F_SilkS)
    t.SetMirrored(bool(bottom))
    t.SetTextSize(VECTOR2I(FromMM(0.9), FromMM(0.9)))
    t.SetTextThickness(FromMM(0.15))
    board.Add(t)

# ---- courtyard check (same side) ----
bad = 0
items = list(fps.items())
for i in range(len(items)):
    for j in range(i + 1, len(items)):
        (ra, a), (rb, b) = items[i], items[j]
        if a.IsFlipped() != b.IsFlipped():
            continue
        la = pcbnew.B_CrtYd if a.IsFlipped() else pcbnew.F_CrtYd
        ca, cb = a.GetCourtyard(la), b.GetCourtyard(la)
        if ca.OutlineCount() and cb.OutlineCount() and ca.Collide(cb):
            print("OVERLAP", ra, rb)
            bad += 1
for r, f in fps.items():
    bb = f.GetBoundingBox(False, False)
    if ToMM(bb.GetX()) < OX - 0.01 or ToMM(bb.GetY()) < OY - 0.01 or ToMM(bb.GetRight()) > OX + W + 0.01 or ToMM(bb.GetBottom()) > OY + H + 0.01:
        if not r.startswith("J"):
            print("OUTSIDE", r)

pcbnew.SaveBoard(os.path.join(D, "iot-shield.kicad_pcb"), board)
print("board saved,", len(fps), "footprints,", bad, "overlaps")
