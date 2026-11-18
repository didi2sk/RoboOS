#!/bin/sh
# board pictures: build/top.svg (+ png via the scratch shot.js when given)
cd "$(dirname "$0")"
kicad-cli pcb export svg --exclude-drawing-sheet --page-size-mode 2 -l F.Cu,F.SilkS,F.CrtYd,Edge.Cuts,F.Fab -o build/top.svg iot-shield.kicad_pcb >/dev/null
kicad-cli pcb export svg --exclude-drawing-sheet --page-size-mode 2 -l B.Cu,B.SilkS,Edge.Cuts -m -o build/bottom.svg iot-shield.kicad_pcb >/dev/null
