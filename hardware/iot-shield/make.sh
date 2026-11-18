#!/bin/sh
# Rebuilds everything from circuit.py (KiCad 7: apt install kicad kicad-symbols kicad-footprints)
#   ./make.sh                      schematic, board (placed, GND planes), PDF, BOM, CPL
#   ./make.sh freerouting.jar      ... and autoroute with Freerouting (https://github.com/freerouting/freerouting)
set -e
cd "$(dirname "$0")"
mkdir -p build
python3 gen_sch.py
python3 check_net.py
python3 gen_pcb.py
python3 route.py "$@"
kicad-cli sch export pdf -o iot-shield-schematic.pdf iot-shield.kicad_sch >/dev/null
python3 bom.py
./render.sh
echo "gerbers for a routed board: kicad-cli pcb export gerbers -o build/gerber/ iot-shield.kicad_pcb; kicad-cli pcb export drill -o build/gerber/ iot-shield.kicad_pcb"
