# Placement for gen_pcb.py (exec'd there): ref -> (x, y, rotation) of the footprint origin (pad 1 for THT parts)
# in board mm, (0, 0) = the HAT corner at the header's pin-1 end, header along the top edge.
# The board runs from x = XL (-20) to 65: the extra 20 mm hang over the Pi's SD card end.
# ("edge", side, first pin) puts a terminal block on the board edge.
# Tall parts (relays, TO-220, the 5 V module, electrolytics) stay out of the module bays:
#   ADS1115 module over the inputs (x 6..34, y 18..37), DS3231 module bottom right (x 31..45, y 44..57).
place = {
    "J1": ("header",),
    "H1": (3.5, 3.5, 0), "H2": (61.5, 3.5, 0), "H3": (3.5, 52.5, 0), "H4": (61.5, 52.5, 0),
    "H5": (3.5, 26.5, 0), "H6": (61.5, 26.5, 0),
    "J2": ("edge", "left", 9.5), "J3": ("edge", "left", 17.5),
    "J4": ("edge", "bottom", -15.0), "J5": ("edge", "bottom", 9.0),
    "J8": ("edge", "right", 18.5), "J9": ("edge", "right", 39.5),
    # power, top left
    "F1": (-17.5, 3.5, 0), "C1": (-6.5, 5.0, 0), "D1": (-9.5, 12.5, 0), "D2": (-9.0, 18.0, 0),
    "C3": (1.5, 12.0, 0), "U1": (9.5, 9.0, 0),
    # relays and drivers
    "K1": (36.5, 15.0, 0), "K2": (36.5, 36.0, 0),
    "D31": (20.0, 8.3, 0), "R41": (26.0, 8.3, 0), "Q1": (22.0, 13.5, 0),
    "D32": (28.5, 22.5, 270), "R42": (32.0, 22.5, 270), "Q2": (31.0, 40.0, 90),
    # MOSFET outputs, left
    "R23": (-10.0, 22.5, 270), "R27": (-7.0, 22.5, 270), "R24": (-4.0, 22.5, 270), "R28": (-1.0, 22.5, 270),
    "D23": (-10.0, 29.0, 0), "D24": (-3.5, 32.3, 0),
    "Q3": (-10.5, 37.5, 0), "Q4": (-10.5, 43.5, 0),
    # LEDs
    "R1": (-1.0, 35.4, 270), "D3": (3.0, 35.4, 270), "R2": (-1.0, 41.0, 270), "D4": (3.0, 41.0, 270),
    # ADC socket + analog series resistors, RTC socket, button
    "J11": (8.0, 17.7, 90),
    "R30": (8.0, 44.0, 0), "R31": (13.2, 44.0, 0), "R32": (18.4, 44.0, 0), "R33": (23.6, 44.0, 0),
    "J10": (31.0, 44.5, 0),
    "SW1": (47.0, 47.5, 0), "R50": (43.5, 46.5, 270), "C50": (56.5, 47.0, 270),
}
for i in range(4):
    y = 21.2 + 5.7 * i
    place["R%d" % (11 + i)] = (8.0, y, 270)
    place["D%d" % (11 + i)] = (11.0, y, 270)
    place["U%d" % (11 + i)] = (14.2, y, 0)
    place["R%d" % (15 + i)] = (25.0, y, 270)

LABELS = [("+ 12-24V -", XL + 4.6, 11.25, 90, 1), ("IN1 IN2 IN3 IN4 COM", XL + 4.6, 24.5, 90, 1),
          ("OUT3 OUT4 V+", -11.5, 52.5, 0, 1), ("3V3 A0 A1 A2 A3 GND", 17.75, 52.5, 0, 1),
          ("K1: NC COM NO", 60.4, 15.0, 90, 1), ("K2: NC COM NO", 60.4, 36.0, 90, 1)]
