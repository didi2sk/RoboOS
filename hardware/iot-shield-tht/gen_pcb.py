#!/usr/bin/env python3
"""Builds iot-shield-tht.kicad_pcb from circuit.py with the KiCad 7 pcbnew API:
HAT outline 65 x 56.5 mm, footprints placed by hand (table below), nets from
the circuit, GND planes on both layers. Routing: route.sh (Freerouting)."""
import os, sys, json
import pcbnew
from pcbnew import VECTOR2I, FromMM, ToMM

D = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, D)
import circuit

FPLIB = "/usr/share/kicad/footprints/"
OX, OY = 100.0, 100.0               # Pi HAT top-left corner in KiCad coordinates (header and holes as on a HAT)
XL, W, H = -20.0, 65.0, 56.5        # the board reaches 20 mm past the HAT outline over the Pi's SD card end

exec(open(os.path.join(D, "place.py")).read())     # place = {ref: ...}, kept apart for easy tuning


def mm(x, y):
    return VECTOR2I(FromMM(OX + x), FromMM(OY + y))


def pad_pos(fp, num):
    for p in fp.Pads():
        if p.GetNumber() == num:
            return ToMM(p.GetPosition().x) - OX, ToMM(p.GetPosition().y) - OY


def try_orient(fp, want):
    """turn / flip fp until want(fp) is true"""
    for flip in (False, True):
        if flip and fp.GetReference() != "J1":
            break                                       # only the header socket goes to the bottom side
        for r in (0, 90, 180, 270):
            fp.SetOrientationDegrees(r)
            if want(fp):
                return True
        fp.Flip(fp.GetPosition(), True)
    return False


board = pcbnew.NewBoard(os.path.join(D, "iot-shield-tht.kicad_pcb"))
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
            if side == "right":  # pins upwards (pin 1 at the bottom), the wire opening to the right
                return abs(a[0] - b[0]) < .01 and b[1] < a[1] and ToMM(bb.GetRight()) - OX > a[0] + 4
            return abs(a[1] - b[1]) < .01 and b[0] > a[0] and ToMM(bb.GetBottom()) - OY > a[1] + 4

        assert try_orient(fp, ok), p["ref"]
        a = pad_pos(fp, "1")
        if side == "left":
            fp.Move(VECTOR2I(FromMM(XL + 4.6 - a[0]), FromMM(start - a[1])))
        elif side == "right":
            fp.Move(VECTOR2I(FromMM(W - 4.6 - a[0]), FromMM(start - a[1])))
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


L = XL
seg(L + R, 0, W - R, 0); seg(W, R, W, H - R); seg(W - R, H, L + R, H); seg(L, H - R, L, R)
arc(W - R, R, W - R, 0, W, R); arc(W - R, H - R, W, H - R, W - R, H)
arc(L + R, H - R, L + R, H, L, H - R); arc(L + R, R, L, R, L + R, 0)

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
    for x, y in ((XL + 0.5, 0.5), (W - 0.5, 0.5), (W - 0.5, H - 0.5), (XL + 0.5, H - 0.5)):
        ol.Append(FromMM(OX + x), FromMM(OY + y))
    board.Add(z)

# ---- text ----
t = pcbnew.PCB_TEXT(board)
t.SetText("RoboOS IoT shield THT v0.1")
t.SetPosition(mm(32.5, 28.0))
t.SetLayer(pcbnew.B_SilkS)
t.SetMirrored(True)
t.SetTextSize(VECTOR2I(FromMM(1.0), FromMM(1.0)))
t.SetTextThickness(FromMM(0.15))
board.Add(t)

labels = LABELS
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

# ---- overlap check (same side): courtyard boxes ----
def box(f):
    la = pcbnew.B_CrtYd if f.IsFlipped() else pcbnew.F_CrtYd
    c = f.GetCourtyard(la)
    bb = c.BBox() if c.OutlineCount() and c.BBox().GetWidth() > 0 else f.GetBoundingBox(False, False)
    return ToMM(bb.GetX()) - OX, ToMM(bb.GetY()) - OY, ToMM(bb.GetRight()) - OX, ToMM(bb.GetBottom()) - OY


bad = 0
items = list(fps.items())
for i in range(len(items)):
    for j in range(i + 1, len(items)):
        (ra, a), (rb, b) = items[i], items[j]
        if a.IsFlipped() != b.IsFlipped():
            continue
        A, B = box(a), box(b)
        if A[0] < B[2] - 0.05 and B[0] < A[2] - 0.05 and A[1] < B[3] - 0.05 and B[1] < A[3] - 0.05:
            print("OVERLAP", ra, rb, "%.1f %.1f" % (min(A[2], B[2]) - max(A[0], B[0]), min(A[3], B[3]) - max(A[1], B[1])))
            bad += 1
for r, f in fps.items():
    x0, y0, x1, y1 = box(f)
    if (x0 < XL - 0.01 or y0 < -0.01 or x1 > W + 0.01 or y1 > H + 0.01) and not r.startswith("J"):
        print("OUTSIDE", r)
    if not f.IsFlipped() and y0 < 6.4 and 5.5 < x1 and x0 < 59.5 and r != "J1" and not r.startswith("H"):
        print("UNDER HEADER", r)

pcbnew.SaveBoard(os.path.join(D, "iot-shield-tht.kicad_pcb"), board)
print("board saved,", len(fps), "footprints,", bad, "overlaps")
