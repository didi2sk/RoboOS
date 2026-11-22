#!/bin/sh
# Print the OV5647 camera sensor registers RoboOS sets, while the camera runs under Linux
# (e.g. 'rpicam-hello -t 0 -n &'), in the same format as RoboOS 'cam sregs' - to compare.
# Only READS (i2ctransfer -f, because the kernel camera driver owns the bus). Needs i2c-tools.
REGS="0100 3000 3001 3002 300d 300e 3016 3017 3018 301c 301d 3034 3035 3036 303c 3106 3212 3500 3501 3502 3503 350a 350b 3600 3612 3618 3620 3621 3630 3632 3633 3634 3636 3703 3704 3705 3708 3709 370b 370c 3715 3717 3731 3800 3801 3802 3803 3804 3805 3806 3807 3808 3809 380a 380b 380c 380d 380e 380f 3811 3813 3814 3815 3820 3821 3827 3a08 3a09 3a0a 3a0b 3a0d 3a0e 3a0f 3a10 3a11 3a18 3a19 3a1b 3a1e 3a1f 3b07 3c01 3f01 3f05 3f06 4000 4001 4004 4202 4800 4801 4802 4805 4810 4814 4818 4819 481f 4823 4826 4827 4836 4837 5000 5001 5002 5003 503d 5a00"
sudo modprobe i2c-dev 2>/dev/null
BUS=""
for d in /dev/i2c-*; do
  n=${d#/dev/i2c-}
  id=$(sudo i2ctransfer -f -y "$n" w2@0x36 0x30 0x0a r1 2>/dev/null)
  if [ "$id" = "0x56" ]; then BUS=$n; break; fi
done
[ -z "$BUS" ] && { echo "no OV5647 (0x36, id 0x56) found on any /dev/i2c-*; is the camera running?"; exit 1; }
echo "OV5647 on /dev/i2c-$BUS"
i=0; line=""
for r in $REGS; do
  v=$(sudo i2ctransfer -f -y "$BUS" w2@0x36 0x$(echo $r | cut -c1-2) 0x$(echo $r | cut -c3-4) r1 2>/dev/null | sed 's/0x//')
  [ -z "$v" ] && v="??"
  line="$line$r=$v "
  i=$((i+1))
  if [ $i -eq 8 ]; then echo "$line"; line=""; i=0; fi
done
[ -n "$line" ] && echo "$line"
