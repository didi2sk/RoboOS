#!/bin/sh
# Rebuilds everything from circuit.py (KiCad 7: apt install kicad kicad-symbols kicad-footprints)
#   ./make.sh                      schematic, board (placed only, NOT routed), PDF, parts list
#   ./make.sh freerouting.jar [N]  ... routed with Freerouting 1.9 (https://github.com/freerouting/freerouting),
#                                  N passes (250 used for the committed board), needs java (+ xvfb-run headless)
set -e
cd "$(dirname "$0")"
mkdir -p build
python3 gen_sch.py
python3 check_net.py
python3 gen_pcb.py
python3 route.py "$@"
kicad-cli sch export pdf -o iot-shield-tht-schematic.pdf iot-shield-tht.kicad_sch >/dev/null
python3 bom.py
./render.sh
if [ -n "$1" ]; then
  rm -rf build/gerber && mkdir -p build/gerber
  kicad-cli pcb export gerbers -l F.Cu,B.Cu,F.Mask,B.Mask,F.SilkS,B.SilkS,F.Paste,B.Paste,Edge.Cuts -o build/gerber/ iot-shield-tht.kicad_pcb >/dev/null
  kicad-cli pcb export drill -o build/gerber/ iot-shield-tht.kicad_pcb >/dev/null
  rm -f iot-shield-tht-gerbers.zip && (cd build/gerber && zip -q ../../iot-shield-tht-gerbers.zip *)
  echo "iot-shield-tht-gerbers.zip written"
fi
