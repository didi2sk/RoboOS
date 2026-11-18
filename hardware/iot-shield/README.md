# RoboOS IoT shield (v0.1 - first draft)

**English** | [Slovensky](README.sk.md)

A board for the 40-pin header of a Raspberry Pi 3 (HAT size, 65 x 56.5 mm) that turns
it into a small IoT / automation controller for RoboOS.

**Status: drawn, not manufactured, not tested. The board is placed but NOT routed yet**
(see "Routing" below). Check everything before ordering.

![top side](board-top.png)

Schematic: [iot-shield-schematic.pdf](iot-shield-schematic.pdf) (KiCad 7: `iot-shield.kicad_sch`, `iot-shield.kicad_pcb`).

## What is on it

| Block | Parts | RoboOS |
|---|---|---|
| Power 12-24 V | terminal J2, polyfuse 2 A, SS34 reverse diode, TVS SMBJ26CA, buck AP63205 (5 V 2 A) to header pins 2+4 | - |
| 4 isolated inputs | J3 IN1-IN4 + COM, PC817, 3.3 k (5-24 V, about 7 mA at 24 V) | GPIO17, 27, 22, 5 - **low** = voltage on the input |
| 4 outputs | J4 OUT1-OUT4 + V+, AO3400A low side, B5819W flyback diodes | GPIO24, 25, 12 (PWM), 16 - high = on |
| Analog | J5: 3V3, A0-A3, GND; ADS1115 at 0x48, 1 k + 100 n filter per input | `sensors`, MQTT `a0`-`a3`, BASIC `SENSOR(4..7)` |
| Climate | BME280 at 0x76 (on the board edge, away from the regulator) | `sensors`, MQTT `temp`/`hum`/`press` |
| Clock | DS3231MZ at 0x68 + CR1220 | the time at boot without a network |
| 1-Wire | J6 3V3 / DQ / GND, 4.7 k pull-up | GPIO4 (DS18B20 - no RoboOS driver yet) |
| Qwiic / STEMMA QT | J7, 3.3 V I2C | `i2c scan` |
| LEDs | green = 5 V, red = GPIO23 (status) | `gpio 23 hi` |

Connect a load (relay coil, LED strip, valve) between **V+** and **OUTx**. Inputs: put the
signal between **INx** and **COM** (COM is isolated from the Pi ground).

## Things to know

- 5 V reaches the Pi through header pins 2 and 4 (back-powering). **Do not plug the Pi's USB
  power in at the same time.** The official HAT spec wants an ideal diode here - not in v0.1.
- Input range 12-24 V, absolute max about 28 V (TVS standoff 26 V, regulator 32 V).
- Output current: the polyfuse (2 A) covers the Pi and the loads together.
- Analog inputs: 0-3.3 V only. The ADC range is +-4.096 V but its inputs must not go above VDD.
- The header socket J1 (2x20, female, 8.5 mm) goes on the **bottom** side; everything else on top.
- LCSC part numbers in `bom.csv` were typed from memory: **check each one** (value, package, stock)
  in the JLCPCB parts list before ordering. Terminal blocks: any 3.5 mm pitch, 2/5/5/6 pins.

## Routing

`gen_pcb.py` places the parts and puts GND planes on both layers; the tracks are not drawn
yet. Two ways:
1. by hand in KiCad 7 (open `iot-shield.kicad_pcb`, the ratsnest shows every connection), or
2. Freerouting: `./make.sh /path/to/freerouting.jar` exports `build/board.dsn`, runs
   Freerouting and reads the result back (`route.py`), then fills the planes and writes `build/drc.txt`.

Power nets (VIN, +5V, GND, SW, OUTx) are in the net class "Power" (0.8 mm tracks).

## Rebuilding

Everything is generated from `circuit.py` (one list of parts + nets):

```
sudo apt install kicad kicad-symbols kicad-footprints    # KiCad 7
./make.sh             # schematic + netlist check + board + PDF + BOM/CPL + pictures
```

`check_net.py` compares the schematic's netlist with `circuit.py` (it must say "48 of 48").
Change `circuit.py`, run `./make.sh`, then in KiCad use "Update PCB from Schematic" if you
already routed by hand (the generator would place everything again).
