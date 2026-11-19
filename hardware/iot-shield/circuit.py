"""
RoboOS IoT shield - the circuit, written once and used for both the schematic
(gen_sch.py) and the board (gen_pcb.py).

Each part: ref, library symbol, value, footprint, LCSC number (JLCPCB assembly,
"" = choose one), {pin number: net}. Pins not listed are "not connected".
Schematic placement: block name + (x, y) inside the block, in 2.54 mm grid units.
"""

FP_R = "Resistor_SMD:R_0603_1608Metric"
FP_R1206 = "Resistor_SMD:R_1206_3216Metric"
FP_C = "Capacitor_SMD:C_0603_1608Metric"
FP_C0805 = "Capacitor_SMD:C_0805_2012Metric"
FP_C1206 = "Capacitor_SMD:C_1206_3216Metric"
FP_LED = "LED_SMD:LED_0603_1608Metric"
FP_TB = "TerminalBlock_Phoenix:TerminalBlock_Phoenix_PT-1,5-%d-3.5-H_1x%02d_P3.50mm_Horizontal"

parts = []


def part(ref, sym, value, fp, lcsc, pins, block, x, y, rot=0, note=""):
    parts.append(dict(ref=ref, sym=sym, value=value, fp=fp, lcsc=lcsc, pins=pins,
                      block=block, x=x, y=y, rot=rot, note=note))


def R(ref, value, a, b, block, x, y, lcsc="", fp=FP_R, rot=0):
    part(ref, "Device:R", value, fp, lcsc, {"1": a, "2": b}, block, x, y, rot)


def C(ref, value, a, b, block, x, y, lcsc="", fp=FP_C, rot=0):
    part(ref, "Device:C", value, fp, lcsc, {"1": a, "2": b}, block, x, y, rot)


R10K, R1K, R4K7, R100, R100K, R3K3 = "C25804", "C21190", "C23162", "C22775", "C25803", "C26032"
C100N, C10U50, C22U = "C14663", "C13585", "C45783"

# ---------------- Raspberry Pi header ----------------
gpio = {"11": "IN1", "13": "IN2", "15": "IN3", "29": "IN4",
        "18": "OUT1", "22": "OUT2", "32": "OUT3", "36": "OUT4",
        "16": "LED_ST", "7": "ONEWIRE", "3": "SDA", "5": "SCL"}
pins = {"1": "+3V3", "17": "+3V3", "2": "+5V", "4": "+5V"}
for p in ("6", "9", "14", "20", "25", "30", "34", "39"):
    pins[p] = "GND"
pins.update(gpio)
part("J1", "Connector:Raspberry_Pi_2_3", "RPi 40-pin socket",
     "Connector_PinSocket_2.54mm:PinSocket_2x20_P2.54mm_Vertical", "C2977589", pins, "pi", 14, 18,
     note="2x20 female, 8.5 mm (on the bottom side)")

# ---------------- power: 12-24 V in, 5 V 2 A buck ----------------
part("J2", "Connector_Generic:Conn_01x02", "12-24V IN", FP_TB % (2, 2), "", {"1": "VIN_RAW", "2": "GND"}, "pwr", 2, 4)
part("F1", "Device:Polyfuse", "2A hold 30V", "Fuse:Fuse_1812_4532Metric", "C6851470", {"1": "VIN_RAW", "2": "VIN_F"}, "pwr", 8, 2, rot=270)
part("D1", "Diode:SS34", "SS34", "Diode_SMD:D_SMA", "C8678", {"1": "VIN", "2": "VIN_F"}, "pwr", 14, 2, rot=180)
part("D2", "Device:D_TVS", "SMBJ26CA", "Diode_SMD:D_SMB", "C123932", {"1": "VIN", "2": "GND"}, "pwr", 18, 6, rot=90)
C("C1", "10u 50V", "VIN", "GND", "pwr", 22, 6, C10U50, FP_C1206)
C("C2", "100n", "VIN", "GND", "pwr", 25, 6, C100N)
part("U1", "Regulator_Switching:AP63205WU", "AP63205WU", "Package_TO_SOT_SMD:TSOT-23-6", "C2071056",
     {"1": "+5V", "2": "VIN", "3": "VIN", "4": "GND", "5": "SW", "6": "BST"}, "pwr", 33, 6)
C("C3", "100n", "BST", "SW", "pwr", 41, 2, C100N)
part("L1", "Device:L", "4.7u 3A", "Inductor_SMD:L_Changjiang_FNR5040S", "C167963", {"1": "SW", "2": "+5V"}, "pwr", 46, 4, rot=90)
C("C4", "22u", "+5V", "GND", "pwr", 51, 6, C22U, FP_C0805)
C("C5", "22u", "+5V", "GND", "pwr", 54, 6, C22U, FP_C0805)
R("R1", "2.2k", "+5V", "PWR_LED", "pwr", 58, 4, "C4190")
part("D3", "Device:LED", "green", FP_LED, "C12624", {"1": "GND", "2": "PWR_LED"}, "pwr", 58, 9, rot=90)
R("R2", "1k", "LED_ST", "ST_LED", "pwr", 68, 4, R1K)
part("D4", "Device:LED", "red (GPIO23)", FP_LED, "C2286", {"1": "GND", "2": "ST_LED"}, "pwr", 68, 9, rot=90)

# ---------------- 4 isolated inputs (PC817), active = GPIO low ----------------
part("J3", "Connector_Generic:Conn_01x05", "INPUTS 1-4, COM", FP_TB % (5, 5), "",
     {"1": "IN1_T", "2": "IN2_T", "3": "IN3_T", "4": "IN4_T", "5": "COM_IN"}, "in", 2, 10)
for i in range(1, 5):
    y = 2 + (i - 1) * 7
    R("R%d" % (10 + i), "3.3k 1206", "IN%d_T" % i, "IN%d_A" % i, "in", 10, y, R3K3, FP_R1206)
    part("D%d" % (10 + i), "Device:D", "1N4148WS", "Diode_SMD:D_SOD-323", "C2128",
         {"1": "IN%d_A" % i, "2": "COM_IN"}, "in", 17, y + 2, rot=90)
    part("U%d" % (10 + i), "Isolator:PC817", "PC817", "Package_DIP:SMDIP-4_W7.62mm", "C66405",
         {"1": "IN%d_A" % i, "2": "COM_IN", "3": "GND", "4": "IN%d" % i}, "in", 31, y + 1)
    R("R%d" % (14 + i), "10k", "+3V3", "IN%d" % i, "in", 40, y, R10K)

# ---------------- 4 MOSFET outputs (low side, max 24 V 2 A) ----------------
part("J4", "Connector_Generic:Conn_01x05", "OUTPUTS 1-4, V+", FP_TB % (5, 5), "",
     {"1": "OUT1_D", "2": "OUT2_D", "3": "OUT3_D", "4": "OUT4_D", "5": "VIN"}, "out", -4, 10)
for i in range(1, 5):
    y = 2 + (i - 1) * 7
    R("R%d" % (20 + i), "100", "OUT%d" % i, "OUT%d_G" % i, "out", 10, y + 2, R100)
    R("R%d" % (24 + i), "100k", "OUT%d_G" % i, "GND", "out", 17, y + 3, R100K)
    part("Q%d" % i, "Transistor_FET:AO3400A", "AO3400A", "Package_TO_SOT_SMD:SOT-23", "C20917",
         {"1": "OUT%d_G" % i, "2": "GND", "3": "OUT%d_D" % i}, "out", 29, y + 2)
    part("D%d" % (20 + i), "Device:D_Schottky", "B5819W", "Diode_SMD:D_SOD-123", "C8598",
         {"1": "VIN", "2": "OUT%d_D" % i}, "out", 37, y + 1, rot=90)

# ---------------- ADS1115: 4 analog inputs 0-3.3 V ----------------
part("J5", "Connector_Generic:Conn_01x06", "ANALOG 3V3 A0-A3 GND", FP_TB % (6, 6), "",
     {"1": "+3V3", "2": "A0_T", "3": "A1_T", "4": "A2_T", "5": "A3_T", "6": "GND"}, "adc", 2, 6)
for i in range(4):
    R("R%d" % (30 + i), "1k", "A%d_T" % i, "AIN%d" % i, "adc", 9 + 6 * i, 2, R1K)
    C("C%d" % (30 + i), "100n", "AIN%d" % i, "GND", "adc", 11 + 6 * i, 8, C100N)
part("U2", "Analog_ADC:ADS1115IDGS", "ADS1115", "Package_SO:TSSOP-10_3x3mm_P0.5mm", "C37593",
     {"1": "GND", "3": "GND", "4": "AIN0", "5": "AIN1", "6": "AIN2", "7": "AIN3", "8": "+3V3", "9": "SDA", "10": "SCL"},
     "adc", 40, 6)
C("C34", "100n", "+3V3", "GND", "adc", 46, 2, C100N)

# ---------------- BME280, DS3231 clock, 1-Wire, Qwiic ----------------
part("U3", "Sensor:BME280", "BME280", "Package_LGA:Bosch_LGA-8_2.5x2.5mm_P0.65mm_ClockwisePinNumbering", "C92489",
     {"1": "GND", "2": "+3V3", "3": "SDA", "4": "SCL", "5": "GND", "6": "+3V3", "7": "GND", "8": "+3V3"}, "i2c", 6, 8)
C("C40", "100n", "+3V3", "GND", "i2c", 14, 3, C100N)
part("U4", "Timer_RTC:DS3231MZ", "DS3231MZ", "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", "C107410",
     {"2": "+3V3", "5": "GND", "6": "VBAT", "7": "SDA", "8": "SCL"}, "i2c", 28, 8)
C("C41", "100n", "+3V3", "GND", "i2c", 34, 3, C100N)
part("BT1", "Device:Battery_Cell", "CR1220", "Battery:BatteryHolder_Keystone_3000_1x12mm", "C70376",
     {"1": "VBAT", "2": "GND"}, "i2c", 38, 9)
part("J6", "Connector_Generic:Conn_01x03", "1-WIRE (DS18B20)", "Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical", "",
     {"1": "+3V3", "2": "ONEWIRE", "3": "GND"}, "i2c", 54, 10)
R("R40", "4.7k", "+3V3", "ONEWIRE", "i2c", 46, 3, R4K7)
part("J7", "Connector_Generic:Conn_01x04", "QWIIC (I2C)", "Connector_JST:JST_SH_SM04B-SRSS-TB_1x04-1MP_P1.00mm_Horizontal", "C160404",
     {"1": "GND", "2": "+3V3", "3": "SDA", "4": "SCL"}, "i2c", 68, 8)

for i, (x, y) in enumerate(((2, 2), (8, 2), (2, 6), (8, 6))):
    part("H%d" % (i + 1), "Mechanical:MountingHole", "M2.5", "MountingHole:MountingHole_2.7mm_M2.5", "", {}, "mech", x, y)

# not assembled by JLCPCB (DNP): the BME280 is expensive and warmed by the board - use a Qwiic module on J7
dnp = {"U3", "C40", "BT1"}

# PWR_FLAGs: nets fed only through connectors
power_flags = ["VIN_RAW", "GND", "+3V3", "+5V", "VBAT", "COM_IN"]

# schematic blocks: origin (mm on an A3 sheet) and title
blocks = {
    "pi":   ((30, 40), "Raspberry Pi header (HAT socket, bottom side)"),
    "pwr":  ((150, 30), "Power: 12-24 V in, 5 V / 2 A buck to the Pi (pins 2+4)"),
    "in":   ((150, 85), "4 isolated inputs (PC817): 5-24 V between INx and COM -> GPIO low"),
    "out":  ((265, 85), "4 outputs: low-side MOSFET, load between V+ and OUTx, max 2 A total"),
    "adc":  ((30, 175), "ADS1115 (0x48): 4 analog inputs 0-3.3 V"),
    "i2c":  ((170, 205), "I2C: BME280 (0x76), DS3231 clock (0x68) + CR1220, 1-Wire on GPIO4, Qwiic"),
    "mech": ((355, 30), "Mounting holes"),
}

# net classes for the board: track width, clearance
net_classes = {
    "Power": (0.8, 0.25, ["VIN_RAW", "VIN_F", "VIN", "+5V", "SW", "OUT1_D", "OUT2_D", "OUT3_D", "OUT4_D"]),
    "Ground": (0.4, 0.2, ["GND"]),            # routed as tracks too, so it does not depend on the planes
    "Default": (0.25, 0.2, None),
}


def nets():
    s = set()
    for p in parts:
        s.update(p["pins"].values())
    return sorted(s)
