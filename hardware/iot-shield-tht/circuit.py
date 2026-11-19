"""
RoboOS IoT shield THT - the hand-solderable variant: only through-hole parts, so it can be built at home.
The circuit is written once here and used for both the schematic (gen_sch.py) and the board (gen_pcb.py).

Each part: ref, library symbol, value, footprint, LCSC number ("" = any shop), {pin number: net}.
Pins not listed are "not connected". Schematic placement: block name + (x, y) inside the block, in 2.54 mm
grid units.

Same GPIOs as the SMD shield (hardware/iot-shield), so the same software runs on both:
IN1-4 GPIO17/27/22/5, OUT1/OUT2 = relays K1/K2 (GPIO24/25), OUT3/OUT4 = MOSFETs (GPIO12/16),
status LED GPIO23, button GPIO6, I2C GPIO2/3 for the plug-in ADS1115 and DS3231 modules.
"""

title, rev = "RoboOS IoT shield THT", "0.1"
comment = "Raspberry Pi 3 HAT, through-hole only: 12-24 V power, 4 opto inputs, 2 relays, 2 MOSFET outputs, button, ADC + RTC modules"
notes = [
    "RoboOS IoT shield THT v0.1 - FIRST DRAFT, not built or tested. Through-hole parts only.",
    "GPIO: IN1-4 = GPIO17/27/22/5 (low = input on), relays K1/K2 = GPIO24/25, MOSFETs = GPIO12/16,",
    "status LED GPIO23, button GPIO6 (low = pressed), I2C GPIO2/3: ADS1115 module 0x48, DS3231 module 0x68.",
    "5 V goes to the Pi pins 2/4: do not power the Pi from its USB port at the same time. Max input 28 V.",
    "Relay contacts: low voltage only (max 24 V, 5 A) - not for 230 V.",
]

FP_R = "Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P2.54mm_Vertical"
FP_C = "Capacitor_THT:C_Disc_D3.0mm_W1.6mm_P2.50mm"
FP_LED = "LED_THT:LED_D3.0mm"
FP_D = "Diode_THT:D_DO-35_SOD27_P2.54mm_Vertical_KathodeUp"
FP_TB = "TerminalBlock_Phoenix:TerminalBlock_Phoenix_PT-1,5-%d-3.5-H_1x%02d_P3.50mm_Horizontal"

parts = []


def part(ref, sym, value, fp, lcsc, pins, block, x, y, rot=0, note=""):
    parts.append(dict(ref=ref, sym=sym, value=value, fp=fp, lcsc=lcsc, pins=pins,
                      block=block, x=x, y=y, rot=rot, note=note))


def R(ref, value, a, b, block, x, y, fp=FP_R, rot=0):
    part(ref, "Device:R", value, fp, "", {"1": a, "2": b}, block, x, y, rot)


def C(ref, value, a, b, block, x, y, fp=FP_C, rot=0):
    part(ref, "Device:C", value, fp, "", {"1": a, "2": b}, block, x, y, rot)


# ---------------- Raspberry Pi header ----------------
gpio = {"11": "IN1", "13": "IN2", "15": "IN3", "29": "IN4",
        "18": "OUT1", "22": "OUT2", "32": "OUT3", "36": "OUT4",
        "16": "LED_ST", "31": "BTN", "3": "SDA", "5": "SCL"}
pins = {"1": "+3V3", "17": "+3V3", "2": "+5V", "4": "+5V"}
for p in ("6", "9", "14", "20", "25", "30", "34", "39"):
    pins[p] = "GND"
pins.update(gpio)
part("J1", "Connector:Raspberry_Pi_2_3", "RPi 40-pin socket",
     "Connector_PinSocket_2.54mm:PinSocket_2x20_P2.54mm_Vertical", "", pins, "pi", 14, 18,
     note="2x20 female, 8.5 mm (on the bottom side)")

# ---------------- power: 12-24 V in, 5 V 2 A switching module ----------------
part("J2", "Connector_Generic:Conn_01x02", "12-24V IN", FP_TB % (2, 2), "", {"1": "VIN_RAW", "2": "GND"}, "pwr", 2, 4)
part("F1", "Device:Polyfuse", "PTC 2A 30V+ (RXEF200)", "Fuse:Fuse_Bourns_MF-RHT200", "", {"1": "VIN_RAW", "2": "VIN_F"},
     "pwr", 8, 2, rot=270, note="radial PTC, 5.1 mm lead spacing, hold 2 A, >= 30 V (Littelfuse RXEF200, Bourns MF-R200)")
part("D1", "Diode:1N5822", "1N5822", "Diode_THT:D_DO-201AD_P5.08mm_Vertical_KathodeUp", "", {"1": "VIN", "2": "VIN_F"},
     "pwr", 14, 2, rot=180, note="reverse polarity protection, Schottky 3 A 40 V, standing")
part("D2", "Device:D_TVS", "P6KE30CA", "Diode_THT:D_DO-15_P3.81mm_Vertical_KathodeUp", "", {"1": "VIN", "2": "GND"},
     "pwr", 18, 6, rot=90, note="bidirectional TVS 600 W, standoff 25.6 V")
part("C1", "Device:C_Polarized", "100u 50V", "Capacitor_THT:CP_Radial_D8.0mm_P3.50mm", "", {"1": "VIN", "2": "GND"}, "pwr", 22, 6)
part("U1", "Regulator_Switching:R-78B5.0-2.0", "R-78B5.0-2.0", "Converter_DCDC:Converter_DCDC_RECOM_R-78B-2.0_THT", "",
     {"1": "VIN", "2": "GND", "3": "+5V"}, "pwr", 33, 6,
     note="5 V 2 A switching 78xx replacement (RECOM R-78B5.0-2.0, TRACO TSR 2-2450, Mornsun K7805-2000R3)")
part("C3", "Device:C_Polarized", "47u 16V", "Capacitor_THT:CP_Radial_D5.0mm_P2.00mm", "", {"1": "+5V", "2": "GND"}, "pwr", 42, 6)
R("R1", "1k", "+5V", "PWR_LED", "pwr", 50, 4)
part("D3", "Device:LED", "red 3mm (power)", FP_LED, "", {"1": "GND", "2": "PWR_LED"}, "pwr", 50, 9, rot=90)
R("R2", "330", "LED_ST", "ST_LED", "pwr", 56, 4)
part("D4", "Device:LED", "green 3mm (GPIO23)", FP_LED, "", {"1": "GND", "2": "ST_LED"}, "pwr", 56, 9, rot=90)

# ---------------- 4 isolated inputs (PC817), active = GPIO low ----------------
part("J3", "Connector_Generic:Conn_01x05", "INPUTS 1-4, COM", FP_TB % (5, 5), "",
     {"1": "IN1_T", "2": "IN2_T", "3": "IN3_T", "4": "IN4_T", "5": "COM_IN"}, "in", 2, 10)
for i in range(1, 5):
    y = 2 + (i - 1) * 7
    R("R%d" % (10 + i), "3.3k", "IN%d_T" % i, "IN%d_A" % i, "in", 10, y)
    part("D%d" % (10 + i), "Diode:1N4148", "1N4148", FP_D, "", {"1": "IN%d_A" % i, "2": "COM_IN"}, "in", 17, y + 2, rot=90)
    part("U%d" % (10 + i), "Isolator:PC817", "PC817", "Package_DIP:DIP-4_W7.62mm", "",
         {"1": "IN%d_A" % i, "2": "COM_IN", "3": "GND", "4": "IN%d" % i}, "in", 31, y + 1)
    R("R%d" % (14 + i), "10k", "+3V3", "IN%d" % i, "in", 40, y)

# ---------------- 2 relays (5 V coil, NPN driver) ----------------
for i in (1, 2):
    y = 2 + (i - 1) * 10
    part("J%d" % (7 + i), "Connector_Generic:Conn_01x03", "RELAY %d: NO COM NC (pin 1 at the bottom)" % i, FP_TB % (3, 3), "",
         {"1": "K%d_NO" % i, "2": "K%d_COM" % i, "3": "K%d_NC" % i}, "rel", 34, y + 2)
    part("K%d" % i, "Relay:SANYOU_SRD_Form_C", "SRD-05VDC-SL-C", "Relay_THT:Relay_SPDT_SANYOU_SRD_Series_Form_C", "",
         {"1": "K%d_COM" % i, "2": "+5V", "5": "K%d_C" % i, "3": "K%d_NO" % i, "4": "K%d_NC" % i}, "rel", 24, y + 3,
         note="5 V coil ~70 mA, contacts 10 A - here max 24 V / 5 A (terminal block, tracks)")
    part("D%d" % (30 + i), "Diode:1N4148", "1N4148", FP_D, "", {"1": "+5V", "2": "K%d_C" % i}, "rel", 15, y + 3, rot=90)
    R("R%d" % (40 + i), "1k", "OUT%d" % i, "K%d_B" % i, "rel", 2, y + 4)
    part("Q%d" % i, "Transistor_BJT:BC337", "BC337", "Package_TO_SOT_THT:TO-92_Inline_Wide", "",
         {"1": "K%d_C" % i, "2": "K%d_B" % i, "3": "GND"}, "rel", 10, y + 4)

# ---------------- 2 MOSFET outputs (low side, max 24 V 2 A) ----------------
part("J4", "Connector_Generic:Conn_01x03", "OUT3 OUT4 V+", FP_TB % (3, 3), "",
     {"1": "OUT3_D", "2": "OUT4_D", "3": "VIN"}, "out", -4, 8)
for i in (3, 4):
    y = 2 + (i - 3) * 7
    R("R%d" % (20 + i), "100", "OUT%d" % i, "OUT%d_G" % i, "out", 10, y + 2)
    R("R%d" % (24 + i), "100k", "OUT%d_G" % i, "GND", "out", 17, y + 3, rot=90)
    part("Q%d" % i, "Transistor_FET:IRLZ44N", "IRLZ44N", "Package_TO_SOT_THT:TO-220-3_Vertical", "",
         {"1": "OUT%d_G" % i, "2": "OUT%d_D" % i, "3": "GND"}, "out", 27, y + 2,
         note="logic-level N-MOSFET (IRLZ44N, IRLB8721, IRL540N)")
    part("D%d" % (20 + i), "Diode:1N5819", "1N5819", "Diode_THT:D_DO-41_SOD81_P3.81mm_Vertical_KathodeUp", "",
         {"1": "VIN", "2": "OUT%d_D" % i}, "out", 35, y + 1, rot=90)

# ---------------- plug-in modules: ADS1115 (4 analog inputs) + DS3231 clock ----------------
part("J5", "Connector_Generic:Conn_01x06", "ANALOG 3V3 A0-A3 GND", FP_TB % (6, 6), "",
     {"1": "+3V3", "2": "A0_T", "3": "A1_T", "4": "A2_T", "5": "A3_T", "6": "GND"}, "adc", 2, 6)
for i in range(4):
    R("R%d" % (30 + i), "1k", "A%d_T" % i, "AIN%d" % i, "adc", 9 + 6 * i, 2)
part("J11", "Connector_Generic:Conn_01x10", "ADS1115 module", "Connector_PinSocket_2.54mm:PinSocket_1x10_P2.54mm_Vertical", "",
     {"1": "+3V3", "2": "GND", "3": "SCL", "4": "SDA", "5": "GND", "7": "AIN0", "8": "AIN1", "9": "AIN2", "10": "AIN3"},
     "adc", 40, 6, note="the common blue ADS1115 board: VDD GND SCL SDA ADDR ALRT A0 A1 A2 A3 (ADDR = GND -> 0x48)")
part("J10", "Connector_Generic:Conn_01x05", "DS3231 module", "Connector_PinSocket_2.54mm:PinSocket_1x05_P2.54mm_Vertical", "",
     {"1": "+3V3", "2": "SDA", "3": "SCL", "5": "GND"}, "adc", 52, 6,
     note="the small DS3231 'RTC for Raspberry Pi' board: + D C NC - (address 0x68, CR1220 on the module)")

# ---------------- button ----------------
part("SW1", "Switch:SW_Push", "button 6x6", "Button_Switch_THT:SW_PUSH_6mm", "", {"1": "BTN", "2": "GND"}, "adc", 60, 6)
R("R50", "10k", "+3V3", "BTN", "adc", 60, 1, rot=90)
C("C50", "100n", "BTN", "GND", "adc", 64, 6)

# H1-H4 = Pi 3 holes (58 x 49 mm), H5/H6 = the lower holes of a Pi Zero (58 x 23 mm)
for i, (x, y) in enumerate(((2, 2), (8, 2), (2, 6), (8, 6), (2, 10), (8, 10))):
    part("H%d" % (i + 1), "Mechanical:MountingHole", "M2.5", "MountingHole:MountingHole_2.7mm_M2.5", "", {}, "mech", x, y)

dnp = set()

# PWR_FLAGs: nets fed only through connectors
power_flags = ["VIN_RAW", "GND", "+3V3", "+5V", "COM_IN"]

# schematic blocks: origin (mm on an A3 sheet) and title
blocks = {
    "pi":   ((30, 40), "Raspberry Pi header (HAT socket, bottom side)"),
    "pwr":  ((150, 30), "Power: 12-24 V in, 5 V / 2 A module to the Pi (pins 2+4)"),
    "in":   ((150, 85), "4 isolated inputs (PC817): 5-24 V between INx and COM -> GPIO low"),
    "rel":  ((265, 85), "2 relays K1/K2 (GPIO24/25), 5 V coils, contacts max 24 V / 5 A"),
    "out":  ((265, 150), "2 outputs (GPIO12/16): low-side MOSFET, load between V+ and OUTx"),
    "adc":  ((30, 190), "Plug-in modules: ADS1115 (0x48, analog 0-3.3 V), DS3231 clock (0x68); button GPIO6"),
    "mech": ((355, 30), "Mounting holes"),
}

# net classes for the board: track width, clearance
net_classes = {
    "Contacts": (1.5, 0.5, ["K1_NO", "K1_COM", "K1_NC", "K2_NO", "K2_COM", "K2_NC"]),
    "Power": (1.0, 0.3, ["VIN_RAW", "VIN_F", "VIN", "+5V", "OUT3_D", "OUT4_D"]),
    "Ground": (0.6, 0.25, ["GND"]),
    "Default": (0.4, 0.25, None),
}


def nets():
    s = set()
    for p in parts:
        s.update(p["pins"].values())
    return sorted(s)
