#!/usr/bin/env python3
"""Writes iot-shield.kicad_sch from circuit.py: the library symbols are copied
from the KiCad 7 libraries, every pin gets a short wire and a net label."""
import uuid, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sexp import parse, dump, find, first, Sym
import circuit

LIB = "/usr/share/kicad/symbols/"
G = 2.54
NS = uuid.UUID("6f1d0c52-9a43-4c1e-8d7e-2b9f3a51c0de")


def uid(*k):
    return str(uuid.uuid5(NS, "/".join(map(str, k))))


ROOT = uid("root")
_libs = {}


def lib_symbol(lib_id):
    """the symbol from its library, flattened if it 'extends' another one"""
    lib, name = lib_id.split(":")
    if lib not in _libs:
        _libs[lib] = {s[1]: s for s in find(parse(open(LIB + lib + ".kicad_sym").read()), "symbol")}
    s = _libs[lib][name]
    ext = first(s, "extends")
    if not ext:
        out = [Sym("symbol"), lib_id] + [x for x in s[2:]]
        return out
    parent = lib_symbol(lib + ":" + ext[1])
    props = {p[1]: p for p in find(s, "property")}
    out = [Sym("symbol"), lib_id]
    for x in parent[2:]:
        if isinstance(x, list) and x[0] == "property" and x[1] in props:
            out.append(props.pop(x[1]))
        elif isinstance(x, list) and x[0] == "symbol":
            sub = list(x)
            sub[1] = name + sub[1][len(ext[1]):]
            out.append(sub)
        else:
            out.append(x)
    for p in props.values():
        out.insert(len([y for y in out if not (isinstance(y, list) and y[0] == "symbol")]), p)
    return out


def pins_of(symdef):
    res = []

    def walk(e):
        for x in e:
            if isinstance(x, list):
                if x and x[0] == "pin":
                    at = first(x, "at")
                    res.append((first(x, "number")[1], float(at[1]), float(at[2]), int(float(at[3]))))
                else:
                    walk(x)
    walk(symdef)
    return res


def rot_pt(x, y, r):
    """library point (y up) -> schematic offset (y down) for symbol rotation r"""
    for _ in range(r // 90):
        x, y = -y, x
    return x, -y


def font(size=1.27, *extra):
    return [Sym("effects"), [Sym("font"), [Sym("size"), size, size]]] + list(extra)


items, used = [], {}
labels_at = set()


points = {}


def mark(x, y, net, who):
    k = (round(x, 2), round(y, 2))
    if k in points and points[k][0] != net:
        print("COLLISION at", k, points[k], "and", (net, who))
    points.setdefault(k, (net, who))


def wire(x1, y1, x2, y2):
    items.append([Sym("wire"), [Sym("pts"), [Sym("xy"), x1, y1], [Sym("xy"), x2, y2]],
                  [Sym("stroke"), [Sym("width"), 0], [Sym("type"), Sym("default")]], [Sym("uuid"), uid("w", x1, y1, x2, y2)]])


def label(net, x, y, ang):
    just = {0: "left", 180: "right", 90: "left", 270: "right"}[ang]
    items.append([Sym("label"), net, [Sym("at"), x, y, ang], [Sym("fields_autoplaced")],
                  font(1.27, [Sym("justify"), Sym(just), Sym("bottom")]), [Sym("uuid"), uid("l", net, x, y)]])


def r2(v):
    return round(v / 1.27) * 1.27


power_n = 0


def place(lib_id, ref, value, fp, x, y, rot, pinmap, lcsc="", hide_ref=False, note=""):
    global power_n
    if lib_id not in used:
        used[lib_id] = lib_symbol(lib_id)
    sd = used[lib_id]
    su = uid("sym", ref)
    props = []
    ry = 6.35 if not hide_ref else 0
    fa = 90 if rot in (90, 270) else 0                 # KiCad turns fields with the symbol: this keeps them level
    tx = x + (3.81 if rot in (90, 270) else 2.54)
    props.append([Sym("property"), "Reference", ref, [Sym("at"), tx, y - (1.27 if fa else 3.81), fa],
                  font(1.27, [Sym("justify"), Sym("left")]) + ([Sym("hide")] if hide_ref else [])])
    props.append([Sym("property"), "Value", value, [Sym("at"), tx, y + (1.27 if fa else 3.81), fa],
                  font(1.27, [Sym("justify"), Sym("left")])])
    props.append([Sym("property"), "Footprint", fp, [Sym("at"), x, y, 0], font(1.27, Sym("hide"))])
    props.append([Sym("property"), "Datasheet", "~", [Sym("at"), x, y, 0], font(1.27, Sym("hide"))])
    if lcsc:
        props.append([Sym("property"), "LCSC", lcsc, [Sym("at"), x, y, 0], font(1.27, Sym("hide"))])
    if note:
        props.append([Sym("property"), "Note", note, [Sym("at"), x, y, 0], font(1.27, Sym("hide"))])
    pins = pins_of(sd)
    is_power = ref.startswith("#")
    sym = [Sym("symbol"), [Sym("lib_id"), lib_id], [Sym("at"), x, y, rot], [Sym("unit"), 1],
           [Sym("in_bom"), Sym("no" if is_power else "yes")], [Sym("on_board"), Sym("no" if is_power else "yes")],
           [Sym("dnp"), Sym("no")], [Sym("uuid"), su]] + props
    for num, *_ in pins:
        sym.append([Sym("pin"), num, [Sym("uuid"), uid("pin", ref, num)]])
    sym.append([Sym("instances"), [Sym("project"), "iot-shield",
                [Sym("path"), "/" + ROOT, [Sym("reference"), ref], [Sym("unit"), 1]]]])
    items.append(sym)
    done = set()
    for num, px, py, pa in pins:
        dx, dy = rot_pt(px, py, rot)
        ex, ey = round(x + dx, 2), round(y + dy, 2)
        if (ex, ey) in done:
            continue
        done.add((ex, ey))
        net = pinmap.get(num)
        a = (pa + rot) % 360
        ox, oy = {0: (-1, 0), 180: (1, 0), 90: (0, 1), 270: (0, -1)}[a]
        if net is None:
            if not is_power:
                items.append([Sym("no_connect"), [Sym("at"), ex, ey], [Sym("uuid"), uid("nc", ref, num)]])
            continue
        lx, ly = round(ex + ox * G, 2), round(ey + oy * G, 2)
        mark(ex, ey, net, ref + "." + num)
        mark(lx, ly, net, ref + "." + num)
        if ox:
            for k in range(1, 4):
                mark(ex + ox * k * 0.635, ey, net, ref)
        wire(ex, ey, lx, ly)
        if len(pins) <= 2:                             # small parts: level labels, they do not run into the next part
            ang = {(-1, 0): 180, (1, 0): 0, (0, -1): 0, (0, 1): 0}[(ox, oy)]
        else:
            ang = {(-1, 0): 180, (1, 0): 0, (0, -1): 90, (0, 1): 270}[(ox, oy)]
        label(net, lx, ly, ang)
    return su


def text(t, x, y, size=2.0):
    items.append([Sym("text"), t, [Sym("at"), x, y, 0],
                  font(size, [Sym("justify"), Sym("left"), Sym("bottom")]), [Sym("uuid"), uid("t", t)]])


uuids = {}
for b, ((bx, by), title) in circuit.blocks.items():
    text(title, bx - 5, by - 6, 1.8)
for p in circuit.parts:
    (bx, by), _ = circuit.blocks[p["block"]]
    x, y = r2(bx + p["x"] * G), r2(by + p["y"] * G)
    uuids[p["ref"]] = place(p["sym"], p["ref"], p["value"], p["fp"], x, y, p["rot"], p["pins"], p["lcsc"], note=p["note"])

for i, net in enumerate(circuit.power_flags):
    place("power:PWR_FLAG", "#FLG%02d" % (i + 1), "PWR_FLAG", "", r2(375), r2(95 + i * 12.7), 0, {"1": net}, hide_ref=True)

notes = [
    "RoboOS IoT shield v0.1 - FIRST DRAFT, not built or tested.",
    "GPIO: IN1-4 = GPIO17/27/22/5 (low = input on), OUT1-4 = GPIO24/25/12/16, status LED GPIO23,",
    "I2C GPIO2/3 (BME280 0x76, ADS1115 0x48, DS3231 0x68), 1-Wire GPIO4. 5 V goes to the Pi pins 2/4:",
    "do not power the Pi from its USB port at the same time. Max input 28 V (TVS 26 V, AP63205 32 V).",
]
for i, n in enumerate(notes):
    text(n, 280, 245 + i * 5, 1.5)

doc = [Sym("kicad_sch"), [Sym("version"), Sym("20230121")], [Sym("generator"), Sym("eeschema")],
       [Sym("uuid"), ROOT], [Sym("paper"), "A3"],
       [Sym("title_block"), [Sym("title"), "RoboOS IoT shield"], [Sym("date"), "2026-11-18"], [Sym("rev"), "0.1"],
        [Sym("company"), "RoboOS"], [Sym("comment"), 1, "Raspberry Pi 3 HAT: 12-24 V power, 4 opto inputs, 4 MOSFET outputs, ADC, BME280, RTC"]],
       [Sym("lib_symbols")] + list(used.values())] + items + \
      [[Sym("sheet_instances"), [Sym("path"), "/", [Sym("page"), "1"]]]]

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "iot-shield.kicad_sch")
open(out, "w").write(dump(doc) + "\n")
import json
json.dump(uuids, open(os.path.join(os.path.dirname(out), "build", "uuids.json"), "w"), indent=1)
print("written", out, len(circuit.parts), "parts,", len(circuit.nets()), "nets")
