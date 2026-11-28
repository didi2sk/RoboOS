# RoboOS

**English** | [Slovensky](README.sk.md)

![RoboOS boot screen](docs/screenshot.png)

A mini operating system for the Raspberry Pi that runs directly on the hardware, no Linux.
You control it with a **USB keyboard** or over a serial line (UART). Output goes to
HDMI and UART at the same time, and both inputs work together.

Current version: **0.148**

## Supported boards

One `kernel8.img` runs on every board with the 64-bit BCM2837 chip; the board is
detected at boot (shown under the logo and in the prompt, e.g. `robo@zero2`).

| Board | Notes |
|-------|-------|
| Raspberry Pi 3 Model B / B+ / A+ | tested on 3B |
| Raspberry Pi Zero 2 W | mini-HDMI; keyboard through an **OTG adapter** on the port marked `USB` (power goes to `PWR IN`); hot-plug works on that port too, with or without a USB hub |
| Raspberry Pi 2 Model B **v1.2** | v1.2 has the BCM2837 (printed on the board: "Raspberry Pi 2 Model B V1.2") |
| Compute Module 3 / 3+ | |

Not supported: Pi 1, Zero / Zero W, Pi 2 v1.1 (32-bit chips), Pi 4 and Pi 5 (different
peripherals, USB behind PCIe).

The ACT LED is on the firmware GPIO expander (3B, CM3), GPIO29 (3B+, 3A+, Zero 2 W - active low)
or GPIO47 (Pi 2 v1.2).

## Putting it on an SD card

1. Format the SD card as **FAT32** with an **MBR** partition table (any size).
2. Copy the whole contents of the `sd/` folder onto it:
   `bootcode.bin`, `start.elf`, `fixup.dat`, `config.txt`, `kernel8.img`,
   plus the folders `APPS` (example programs), `RUN` (autostart) and `FIRMWARE` (WiFi chip firmware).
3. Put the card in the Raspberry Pi and power it on.

## GPIO header (pinout)

![Raspberry Pi 40-pin GPIO header](docs/pinout.webp)

*Picture: [Raspberry Pi documentation](https://www.raspberrypi.com/documentation/computers/raspberry-pi.html), © Raspberry Pi Ltd, [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/).*
Odd pins (1, 3, ... 39) = the inner row, even pins (2, 4, ... 40) = the row at the board's edge. The numbers in
RoboOS commands are always **GPIO numbers**, not header pins.

What RoboOS uses by default:

| GPIO (header pin) | used for |
|---|---|
| 2, 3 (3, 5) | I2C: sensors, displays, RTC, FM radio, I2C keypad |
| 14, 15 (8, 10) | UART console (USB-TTL adapter) |
| 8-11 (24, 21, 19, 23) | SPI: RC522 card reader, Waveshare OLED HAT |
| 4 (7) | 1-Wire on the IoT shield (no driver yet) |
| 17, 27, 22, 5 (11, 13, 15, 29) | IoT shield inputs IN1-IN4 = alarm zones (`alarm shield`) |
| 24, 25, 12, 16 (18, 22, 32, 36) | IoT shield outputs / relays (24 = alarm siren) |
| 23 (16) | IoT shield LED (alarm LED) |
| 6 (31) | doorbell button (THT shield) |
| 11, GND, 0, 5, 6, 13, 19, 26 (23-37 inner row) | 4x4 keypad pushed straight on (`alarm keypad plug`) |
| 18, 19 (12, 35) | sound on the Zero 2 W (the Pi 3 uses its jack) |

## Programs and autostart

The SD card is also RoboOS's disk (FAT32 or FAT16, read and write).
RoboOS has its own SD driver (the firmware only loads `kernel8.img` and then steps aside).
If the card is not found, `sd init` shows which step failed and the controller registers.
Since 0.28 the card runs on the SDHOST controller (as in Linux), so that the second controller
(Arasan) is free for WiFi later; if SDHOST does not work, RoboOS uses the Arasan as before and the
boot screen shows a yellow `SDHOST:` line with the reason. `sd` shows which one is used,
`sd init sdhost` / `sd init emmc` try one of them.

- Everything in **`/RUN`** (`*.bin` and `*.bas`) starts automatically after boot,
  in alphabetical order. Press any key within 2 s to skip the autostart.
- `run <name>` starts a program from the current directory, `/RUN` or `/APPS`; the extension
  is optional.
- Moving around the card: `cd APPS`, `cd ..`, `cd /`, `pwd`; the prompt shows where you are
  (`robo@rpi3:/APPS$`). All commands and BASIC (`LOAD`, `SAVE`, `DIR`) take paths relative
  to the current directory. **Tab** completes file and directory names.
- `mkdir <dir>` makes a directory (8.3 name), `rm <file>` deletes a file, `rm <dir>` an empty
  directory. The firmware and kernel files in `/` are protected: deleting them needs `rm -f`.
- **Ctrl+C** stops a running program and returns to the shell.

### BASIC - write programs directly on the RPi

```
robo@rpi3:~$ basic
] 10 PINMODE 17, OUT
] 20 GPIO 17, HIGH : WAIT 500
] 30 GPIO 17, LOW : WAIT 500
] 40 GOTO 20
] RUN                      (Ctrl+C stops it)
] SAVE "/RUN/blink"        (saved as /RUN/blink.bas -> starts after every boot)
] BYE
```

- Commands: `RUN`, `LIST [a-b]`, `NEW`, `SAVE "name"`, `LOAD "name"`, `DIR [path]`, `HELP`, `BYE`;
  `MKDIR "dir"`, `RMDIR "dir"`, `KILL "file"` work in programs too.
  Typing a line number alone deletes that line.
- Statements: `PRINT`/`?`, `INPUT`, `LET`, `IF..THEN..ELSE`, `GOTO`, `GOSUB`/`RETURN`,
  `FOR..TO..STEP`/`NEXT`, `DIM`, `END`, `REM`/`'`, `CLS`, `RANDOMIZE`; separate statements with `:`.
- GPIO: `PINMODE pin, OUT|IN|PULLUP|PULLDOWN`, `GPIO pin, HIGH|LOW`, `GPIO(pin)`, `LED ON|OFF`, `WAIT ms`.
- Pictures: `IMAGE "file", x, y [, w, h]` (see the pictures section).
- Video: `VIDEO "file.mpg"` (see the video section).
- Sound: `SOUND Hz, ms`, `PLAY "notes"`, `VOLUME 0-100`, `PLAYWAV "file.wav"` (see the sound section).
- Network: `NETSEND`, `NETOK`, `NETAVAIL`, `NETRECV`; TCP/UDP: `TCPCONNECT`, `TCPLISTEN`,
  `TCPACCEPT`, `UDPOPEN`, `SOCKPRINT`, `UDPSEND`, `SOCKAVAIL`, `SOCKGET`, `SOCKNUM`, `SOCKCLOSE`
  (see the network sections).
- Functions: `ABS()`, `RND(n)`, `MIN(a,b)`, `MAX(a,b)`, `MILLIS`, `MICROS`, `INKEY`, `TEMP`, `CHR$()` (in PRINT).
- Integers only (64-bit). Keywords must be separated by spaces. `.bas` files are plain text,
  so you can also write them on a PC.
- File names when saving: 8.3 (max 8 characters + extension).

Examples on the card: `APPS/blink.bas`, `APPS/button.bas`, `APPS/knight.bas`,
`APPS/ledsrv.bas` (LED controlled over TCP), `APPS/udptemp.bas` (temperature over UDP),
`APPS/sound.bas` (tones and a tune).

### Text editor

`edit <file>` opens a full-screen editor, so `.bas` and text files can be written and
changed right on the RPi (`edit /RUN/blink.bas`, then `run blink`).

| Key | Action |
|-----|--------|
| arrows, Home, End, Page Up, Page Down | move the cursor |
| Backspace, Delete, Enter, Tab | edit (Tab inserts spaces) |
| Ctrl+S | save |
| Ctrl+Q | quit (asks if there are unsaved changes) |
| Ctrl+K / Ctrl+U | cut the current line / paste it above the cursor line |
| Ctrl+L | redraw the screen |

- Files up to 1 MB. New files are saved with 8.3 names (max 8 characters + extension);
  a file with a long name can be viewed but not saved.
- Line ends are kept as they were (CR LF or LF); new files use CR LF.
- On a UART terminal the editor uses the HDMI text size (128x48), so make the terminal
  window at least that big; without HDMI it uses 80x24.

### C programs (SDK)

The `sdk/` folder builds programs in C with an Arduino-style API:

```c
#include "robos.h"

void setup(void) { pinMode(17, OUTPUT); }

void loop(void)
{
    digitalWrite(17, HIGH); delay(500);
    digitalWrite(17, LOW);  delay(500);
}
```

`cd sdk && make` turns every `examples/*.c` into `bin/*.bin`; copy those to the card
(`/RUN` for autostart). API: `pinMode`, `digitalWrite`, `digitalRead`, `delay`,
`delayMicroseconds`, `millis`, `micros`, `print`, `println`, `printf`, `getKey`,
`clearScreen`, `textColor`, `actLed`, `cpuTemperature`, `stopRequested`, `exit`,
`netSend`, `netReceive`, `netName`, and TCP/UDP: `tcpConnect`, `tcpListen`, `tcpAccept`,
`udpOpen`, `sockWrite`, `sockPrint`, `sockRead`, `sockAvailable`, `udpSendTo`,
`udpReceiveFrom`, `sockClose`, `sockError`, `ipAddress`, and sound: `tone(hz, ms)`, `noTone`,
`playNotes`, `soundVolume`, `playWav`, pictures: `drawImage(path, x, y, w, h)`, `imageError`, and
video: `playVideo(path)`, `videoError`, files: `readFile`, `writeFile`
(see `sdk/robos.h`). A program can use `setup()` + `loop()` or `main()`.
Examples: `hello`, `blink`, `button`, `knight`, `chat`, `tcpserv` (TCP server), `udpecho` (UDP),
`music` (a tune and a siren), `alarm` (see below).

### Demo: alarm (`alarm`)

Since 0.120 the alarm is also in the kernel (the `alarm` command, web page, Home Assistant - see
"Burglar alarm in the kernel"); this program stays as a programming example.
`run alarm` starts a small burglar alarm (`sdk/examples/alarm.c`); copy it to `/RUN` to start it
at boot. Its settings are in `/ALARM.TXT` (made with defaults on the first start):

```
code 1234                    # the PIN that arms and disarms
exit 20                      # seconds to leave after arming
entry 15                     # seconds to disarm after a "delay" zone opens
siren 180                    # seconds of siren (0 = until disarmed)
siren_pin 24                 # output: siren / relay (HIGH = on)
armed_pin 23                 # output: LED - on = armed, blinks = exit / entry time, fast = alarm
sound on                     # countdown beeps, siren tone, voice on the audio jack
volume 80
voice /ALARM                 # folder with voice WAV files ("none" = tones only)
port 8080                    # TCP port for JSON (0 = off)
zone 17 delay nc Front door  # zone <gpio> <instant|delay|24h> <nc|no> <name>
zone 27 instant nc Window
zone 22 instant no PIR hall
zone 5 24h nc Tamper
```

- **Inputs** use the internal pull-up: wire each contact between the GPIO and GND. `nc` = normally
  closed (door contact: open = alarm), `no` = normally open (PIR relay, panic button: closed = alarm).
  `instant` = alarm at once while armed, `delay` = starts the entry time (the way in, ignored
  during the exit time), `24h` = alarm at any time, also disarmed (tamper, panic). It refuses to
  arm while an instant / 24h zone is open.
- **Serial port / keyboard**: `arm 1234`, `disarm 1234`, `status` (the code is shown as `*`).
  Every event is printed as `[ALARM hh:mm:ss] ...` on the serial port and the screen.
- **JSON** on the TCP port: one line with `nc <board> 8080`, or HTTP
  (`curl -d "{\"cmd\":\"disarm\",\"code\":\"1234\"}" http://<board>:8080/`; GET = status):
  `{"cmd":"arm","code":"1234"}`, `{"cmd":"disarm","code":"1234"}`, `{"cmd":"status"}`.
  The answer has the state, the siren, the seconds left, the cause and every zone.
- **Sound**: a beep every second of the exit / entry time, the last 10 seconds spoken, the
  siren as a rising and falling tone, and spoken events. The card has Slovak voice files in
  `/ALARM` (`ARMING`, `ARMED`, `ENTRY`, `ALARM`, `DISARMED`, `WRONG`, `NOARM`, `1`-`10` `.WAV`),
  made by `tools/mkvoice.sh sk` with espeak-ng (`tools/mkvoice.sh en` for English) - replace
  them with your own recordings. A voice file plays to its end before the alarm goes on.
- Five wrong codes block the code for 60 s. It is a demo: Ctrl+C on the board ends it.

Programs can read and write files since API version 8: `readFile(path, buf, max)`,
`writeFile(path, data, len)`.

Programs are loaded at 0x01000000 (max 16 MB). A crash inside a program prints the
exception and returns to the shell.

**GPIO is 3.3 V** (not 5 V like an Arduino Uno), max about 16 mA per pin.
Use a 220-330 Ω resistor for LEDs and a transistor/module for relays and motors.

## Pictures

RoboOS shows JPEG (also progressive), PNG (with transparency), BMP and GIF (the first frame)
on the HDMI screen; a big picture is scaled down to fit, keeping its shape.

- `view <picture>` shows it full screen; **Left/Right** (or Space) go to the previous / next
  picture in the same directory, **Q** or Enter returns to the text screen as it was.
  `view <dir>` starts with the first picture there.
- **Boot picture**: a file `/SPLASH.PNG` (or `/SPLASH.JPG`, `/SPLASH.BMP`) is shown while
  RoboOS starts; the boot messages appear behind it and show up after at least 2.5 s
  (a key ends it sooner). The card comes with a RoboOS picture - put your own there
  (best 1024x768), or delete it to boot straight to the text.
- BASIC: `IMAGE "photo.jpg", x, y` draws a picture at x, y (pixels; 1024x768 unless `screen` changed it);
  `IMAGE "photo.jpg", x, y, w, h` fits it into a w x h box. C: `drawImage(path, x, y, w, h)`.
- Pictures are decoded in memory: a 12-megapixel phone photo takes a few seconds.

## Screen resolution

The text screen starts at 1024x768. `screen 1280x720` (any size from 320x200 to 2048x1280)
switches right away and is saved in `/SCREEN.TXT`, so the next boot uses it too;
`screen auto` uses the monitor's own resolution (what the firmware found on HDMI),
`screen` alone shows the current size and the monitor's. If the picture stays black,
delete `/SCREEN.TXT` on a PC (or put `1024x768` in it).

RoboOS asks the graphics chip for **two screen pages**: the text is on the first one, a
video is drawn on the hidden one and then shown in one step (page flip), so it does not tear.

## Video

`video <file.mpg>` plays a video full screen with sound (jack, GPIO18/19 or an I2S DAC -
`sound out` chooses). **Q** or Enter stops it, **Space** pauses, **Left/Right** jump 10 s
back / forward; the shell then prints how many frames were shown and the frame rate.

The format is **MPEG-1 video with MP2 sound** (`.mpg`, as on a Video CD): free to use, the
patents have expired long ago, and the decoder (pl_mpeg) is small enough to run directly on
the board. MP4 (H.264 / AAC) is licensed and needs the GPU's hardware decoder, whose
interface is closed, so RoboOS does not play it. Convert any video on a PC with ffmpeg:

```
ffmpeg -i film.mp4 -vf scale=640:-2 -c:v mpeg1video -q:v 4 -c:a mp2 -b:a 192k -f mpeg FILM.MPG
```

- 640x360 or 640x480 at 25/30 fps is a good size for a Pi 3; the picture is scaled to
  fit the screen (at most 2x bigger), keeping its shape. `-q:v` 2 (best) .. 10 (smallest).
- The work is split between the cores: core 0 reads the card and decodes, **cores 2 and 3**
  convert the colours (YCbCr to RGB) and scale the previous frame at the same time, core 1
  keeps USB and the network running. If the board cannot keep up, frames are skipped
  (the sound never is).
- BASIC: `VIDEO "film.mpg"`; C: `playVideo(path)` (API version 7, `videoError()`).

## Network: messages between RoboOS boards

The Pi 3B (LAN9514) and the Pi 3B+ (LAN7515) have an Ethernet chip that is a USB device
behind the onboard hub; RoboOS has a driver for both (both work: DHCP, NTP). The 3B links at 100 Mbit, the 3B+
at 1000 Mbit (`net speed 100` limits it; over USB 2.0 the chip moves at most ~300 Mbit). Plug the
boards into the same switch or router and they can send each other short text messages.
The protocol is RoboOS's own, on raw Ethernet frames (EtherType `88b5`), so a PC does
not understand it. For talking to PCs and the internet there is TCP/IP (next section).

```
robo@rpi3:~$ net who
  rpi3-1c2d        b8:27:eb:11:1c:2d  seen 0 s ago
robo@rpi3:~$ net send rpi3-1c2d hello
delivered
```

- Every board has a name: by default `rpi3-` + the end of its MAC address.
  `net name <new>` changes it and saves it to `/NAME.TXT` on the SD card.
- `net who` asks who is on the network. `net send <name> <text>` sends a message and
  waits for the confirmation (3 tries); `net send all <text>` goes to everybody, unconfirmed.
- A message that arrives while you are at the shell prompt is printed as `[NET] name: text`.
  While a program runs, messages wait in an inbox (the last 8) for the program.
- `net` shows the name, chip, MAC address, link state and counters. `net test [seconds]`
  lists all frames on the wire, `net loop on` makes sent messages come back to the same
  board (try things with one board, or in QEMU).
- BASIC: `NETSEND "name", 42` or `NETSEND "ALL", "text"`; `NETOK` tells if the last send
  went through; `NETAVAIL` is the number of waiting messages and `NETRECV` returns the
  oldest one as a number. Example: one board sends its button, the other lights an LED:

  ```
  10 PINMODE 27, PULLUP : REM board A        10 PINMODE 17, OUT : REM board B
  20 NETSEND "ALL", GPIO(27) : WAIT 100      20 IF NETAVAIL THEN GPIO 17, NETRECV = 0
  30 GOTO 20                                 30 GOTO 20
  ```
- C programs: `netSend(to, text)`, `netReceive(from, text, max)`, `netName()`;
  see `sdk/examples/chat.c` (`run chat` = a chat between the boards).

Not available on the Zero 2 W and 3A+ (no Ethernet). The 3B and 3B+ can talk to each other.

## Network: TCP/IP

RoboOS uses [lwIP](https://savannah.nongnu.org/projects/lwip/) 2.2.1 (in `lwip/`,
BSD license), a small TCP/IP stack written for systems without an operating system.
After boot the board asks the router for an address (DHCP) and sends its name with it,
so the router's list of devices shows it under its RoboOS name. When the address
arrives, `[NET] IP address ...` appears at the prompt.

```
robo@rpi3:~$ ip
host:    rpi3-9a4f
link:    up
address: 192.168.1.37 / 255.255.255.0  (DHCP)
gateway: 192.168.1.1
dns:     192.168.1.1
robo@rpi3:~$ ping 192.168.1.1
reply from 192.168.1.1: seq=1 ttl=64 time=0.9 ms
```

- `ip` shows the address; `ip set <address> [mask] [gateway]` sets a fixed address,
  `ip dhcp` goes back to DHCP. The choice is saved in `/IP.TXT` on the SD card.
- `ping <address or name> [count]` - names are looked up with DNS.
- `ip` also counts the frames: `in` (ARP, IPv4, other, VLAN-tagged) and `out` (errors) -
  if `in` stays near 0, nothing reaches the board (cable, switch, router). Frames with a
  VLAN tag (802.1Q, also the "priority only" tag some routers add) have the tag removed.
- `http on` starts a small web page with the board's name, version, uptime and CPU
  temperature: open `http://<address>/` in a browser. `http off` stops it. The choice is
  saved in `/HTTP.TXT`, so the page (and the API below) comes back after a reboot.
- IPv4 only.

### WiFi (being built)

The WiFi chip (CYW43438 on the Pi 3B, CYW43436 on the Zero 2 W, CYW43455 on the Pi 3B+) hangs on
SDIO. RoboOS starts it, looks for networks and joins one (WPA2-PSK, or open):

```
wifi scan                     load the firmware into the chip and list the networks around
wifi join MyNet password      join (saved in /WIFI.TXT: the board joins by itself after a reboot)
wifi join "My Net" password   a name with spaces
wifi                          chip, firmware, MAC address, network, signal
wifi band 2|5|auto            only 2.4 GHz / only 5 GHz / both (Pi 3B+ only - the 3B and Zero 2 W have 2.4 GHz; saved; a network with both bands:
                              the firmware picks one, 'wifi' shows the channel it got)
wifi leave                    disconnect (and stop re-joining);  wifi forget  also deletes /WIFI.TXT
```

The chip's own firmware does the WPA2 handshake. One IP interface carries everything (DHCP,
web, API, HTTPS, NTP, programs): **Ethernet when its cable is in, otherwise WiFi** - pulling the
cable switches to WiFi (`[NET] now on WiFi`, a new address by DHCP). `ip` shows which one is used.
The Zero 2 W has no Ethernet, so WiFi is its network.

The chip needs its firmware on the card in `/FIRMWARE` - it is in `sd/FIRMWARE`, so copying
`sd/` is enough (`tools/getwifi.sh` downloads a fresh copy): `brcmfmac43430-sdio.bin` + `.txt` +
`.clm_blob` (Pi 3B; the newer firmware does not switch the radio on without the `.clm_blob`), `brcmfmac43436-sdio.bin` + `.txt` + `.clm_blob` (Zero 2 W), `brcmfmac43455-sdio.bin` +
`.txt` + `.clm_blob` (Pi 3B+). These files are not RoboOS: unmodified copies from Raspberry Pi's
firmware-nonfree package under the Cypress / Synaptics licenses, which allow redistributing them
for use with their chips - see `sd/FIRMWARE/README.txt` and the `LICENCE.*` files next to them.
WiFi needs the SD card on SDHOST (see the SD card section). Scan and join work on the Pi 3B;
the scan works on the Pi 3B+ (2.4 and 5 GHz). The Pi 3B+ firmware has no built-in WPA supplicant,
so since 0.36 RoboOS does the WPA2-PSK handshake itself (`src/wpa.c`: PBKDF2, the 4-way and group
key handshakes, AES key unwrap; checked on the PC against the IEEE 802.11 test vectors and a Python
access point, `tools/test_wpa.py`) - `wifi join` then says "RoboOS does the WPA2 handshake" (just information).
If the join fails (at boot, a lost link, a rebooted router), the board tries again by itself: after 20 s,
then every minute (`[NET] WiFi: joining <net> again`), until `wifi leave`. This **works on the Pi 3B+** (joined, DHCP and ping over WiFi with the cable out). WPA3-only networks (SAE) are not
supported (WPA2/WPA3 mixed are fine).
**The Zero 2 W works too** (boot, USB keyboard through OTG, WiFi).

### Bluetooth (BLE: sensors, phone)

The WiFi chip is a Bluetooth chip too. It is switched on only when you ask:

```
bt on            switch on: listens to the devices around and can be found by a phone
bt scan          what is around (bt scan 20 = listen 20 s)
bt sensors       Bluetooth thermometers as JSON
bt pair          pair a Bluetooth keyboard (classic BT; bt pair aa:bb:cc:dd:ee:ff = that one)
bt forget        forget the paired keyboard
bt               status (connected phone, devices, thermometers)
bt off           switch off
bt auto on|off   switch on at boot (/BT.TXT)
```

**Sensors.** Xiaomi LYWSD03MMC thermometers with the free ATC / pvvx firmware (service data
0x181A) are read all the time while Bluetooth is on: `bt sensors`, `GET /api/ble`
and MQTT `roboos/<name>/ble/<mac>` = `{"temp":21.53,"hum":45.12,"batt":87,"rssi":-63,"name":"ATC_112233"}`
(at most every 30 s), Home Assistant finds them by itself (temperature, humidity, battery).
`bt scan` also shows names and makers of everything else (phones, TVs, headphones).
Since 0.43 it also searches for classic Bluetooth devices (marked `BR`) and marks keyboards and mice:
`[keyboard, BLE]` or `[keyboard, classic BT]` - so you can see which kind your keyboard is
(cheap ones are often classic Bluetooth 3.0, which needs a different driver than BLE ones).

**Phone.** The board shows up as its name (`rpi3-...`) with the Nordic UART Service. Connect with
a terminal app - Android: *Serial Bluetooth Terminal* (Devices -> Bluetooth LE), iPhone / Android:
*nRF Toolbox* (UART) or *Bluefruit Connect* (UART) - and type:

```
help                          the commands
status                        name, version, uptime, address, time
sensors                       CPU, BME280, ADS1115 and the Bluetooth thermometers
gpio 17                       read a pin;  gpio 17 1 / gpio 17 0 / gpio 17 toggle = set it
login robo MyPassword1        needed when users exist ('users add'); a "view" user only reads
```

There is no pairing yet: the link is not encrypted, use it nearby (a password typed there
crosses the air readable). While Bluetooth is on, the UART console runs on the mini UART - same
pins 8/10, same 115200; `bt off` gives it back. Scanning works on the Pi 3B (19 devices on the first
try) and the Zero 2 W (12 devices); the phone part is tested in QEMU (`tools/qemu_bt_test.py` plays the controller and a phone).

**Keyboard (0.44, classic Bluetooth).** Put the keyboard into pairing mode (usually hold Connect or
Fn+C until its LED blinks) and type `bt pair`. RoboOS finds it, connects and shows a number:
`on the keyboard type 123456 and press Enter` - type it **on the Bluetooth keyboard**. Then it
types into the shell like a USB keyboard (US layout, autorepeat). The link key is saved in
`/BTKEYS.TXT`: after a reboot (with `bt auto on`) or when the keyboard slept, press a key and it
connects by itself. `bt` shows the state, `bt forget` deletes the key. BLE keyboards are not supported
yet. The 3B+ chip comes with a placeholder Bluetooth address (43:45:c0:00:1f:ac, the same on every
board); since 0.44 RoboOS gives it its own (b8:27:eb + serial number, like Raspberry Pi OS).
**Works on the Pi 3B+** with a cheap classic BT keyboard (pairing, typing, reconnect after a reboot);
0.45 lets the keyboard authenticate by itself when it calls (0.44 crossed it: error 0x05, one retry).
0.45 also loads the Bluetooth firmware at 921600 baud (8x faster: `bt auto on` no longer adds ~6 s to
the boot) and leaves the WiFi join at boot to the I/O core, so the router's WPA handshake is answered
at once (before, the 3B+ often joined only on the retry 20 s later).
0.46: Bluetooth listens in the background only 10 % of the time (30 ms every 300 ms, `bt scan` 50 %) - WiFi
and Bluetooth share one radio, and the 50 % scan made the WPA handshake fail until the 3rd try on the 3B+;
WiFi retries say why the last try failed. 0.49: a retry never cuts short a try that is still
associating or doing the WPA handshake (0.46 retried after 10 s and so joined only on the 4th try): after an
error 2 s, otherwise 20 s; the time a try took is printed (`connected to ... (N s)`). 0.50: on the 3B+ (RoboOS does the WPA handshake) the firmware's
own WPA status is ignored - 0.49 took it for a failure and restarted joins in the middle of the handshake (10 tries). 0.51: `wifi` lists the last 10 joins with the time of
each step (network found, link, WPA msg1/msg3, keys, lost + reason) - one photo shows where a join stops. 0.52: the owner's router answered
the quick retries with deauth reason 2 ("previous authentication no longer valid") 20 times: now every retry first
leaves cleanly (disassociate, 0.3 s) and a refused try is retried after 15 s, not 2 s. On the Pi 3B+ WiFi now joins on the first try within a second.
0.53: with `bt auto on` and WiFi set up, Bluetooth starts only after WiFi has joined (at most 15 s later) -
both share one radio, and the Bluetooth start made the boot-time WiFi join take 20 s.
0.54: `history` lists the last 30 shell commands (was 10), `!N` / `!!` run one again, `history clear`; the history is kept
in `/HISTORY.TXT` on the card, so it survives a reboot. WiFi: a refused FIRST try after a boot is retried after 3 s (not 20 s), and so is a first
try whose WPA handshake stalls (msg2 sent, no msg3 within 5 s) - the owner's photo: at boot the router never answered our msg2 and
dropped the board after 18 s, the second try then joined in 3 s.
0.55: I2C displays - SSD1306 / SH1106 OLED and 16x2 / 20x4 LCDs with a PCF8574 backpack, found at boot, status screen
(name, IP, time), `display`, BASIC `DISPLAY`, C API 11 (see "I2C display").
0.56: `ai` - questions to Claude with your own API key (see "AI"); shell lines up to 255 characters; more root CAs
for HTTPS (DigiCert G5, SSL.com, Certainly, Microsoft, IdenTrust).
0.57: OLED displays that never acknowledge on I2C (2.42" SSD1309 boards with the D2 jumper open - they worked on an ESP,
whose software I2C ignores ACK): `display type ssd1306` uses them anyway, written by bit-banged I2C on GPIO2/3.
Also OLED controllers that answer writes but not reads (SSD1309) are now found: `i2c scan` and the display search try
0x3C/0x3D with a write too.
0.58: display fonts (small 21 x 8 / normal / big), `display console on` (a copy of the terminal), text over the network
(`/api/display`, MQTT `set/display`). The 2.42" OLED works on the 3B+ (owner: IP and time shown).
0.59: `display font tiny` = 32 x 10 characters (X11 4x6, public domain); the OLED is drawn into a picture in memory and
only the changed 8-pixel stripes are sent. Rows 1-10 in BASIC, the API and MQTT.
0.60: `display time` = a big digital clock (with the date on 128x64), `display info` = the status screen;
`display console off` goes back to the status screen (in 0.59 it left the display empty). The mode is saved.
0.61: the clock in thin digits (DejaVu Sans, `tools/mkclock.py`) instead of the blocky 3x font, the seconds smaller
right beside HH:MM, the date in the 8x16 font below.
0.62: `font tiny` is now the clearer X11 5x8 (25 x 8); the cramped 4x6 (32 x 10) is `font micro`.
0.63: a plain API key typed into `/AI.TXT` on a PC (`key sk-ant-...`) is accepted and scrambled at the first `ai`.
0.64: also just the bare key in `/AI.TXT`, or `API_KEY="sk-ant-..."` - any line with `sk-ant-`.
0.65: `ai` prints every step (connecting, TLS handshake, sending, waiting, reading, with ms) - the owner saw it
stop after "asking ..." with no clue where.
0.66: more for the hang hunt: the answer line shows the HTTP status, length and chunked; while waiting, every 5 s
"still waiting: N bytes in so far, core 1: <what it does> (loops)".
0.67: FIX the `ai` freeze: after a chunked answer (the API answered HTTP 429) BearSSL's polite TLS close
(`br_sslio_close`) spun forever - reproduced in QEMU; the HTTPS client now just closes the TCP connection.
A 429 now hints at the rate / spend limit and the credit.
0.68: FIX `i2c scan` "finding" 0x3c / 0x3d with nothing connected (the write probe for OLEDs): a late no-ACK
is caught, and write probes are trusted only if the reserved address 0x7c does NOT answer. `i2c scan` also warns
when SDA / SCL are low while idle (short, swapped wires, an unpowered module).
0.69: still one phantom 0x3c on the owner's empty bus -> the write probe must answer 3 times in a row, `i2c scan`
marks such finds "[answers writes only]"; `i2c probe <addr>` shows the raw controller status + time of reads and
writes; `i2c speed <kHz>` (10-400, until reboot) - LCD backpacks (PCF8574) are rated for 100 kHz at most.
0.70: I2C bus clear - the owner's LCD backpack held SDA low ("SDA LOW" in `i2c scan`, nothing found) until a
failed transfer clocked it free; now at start-up, before `i2c scan` and after a timeout RoboOS sends up to 9 clock
pulses + a STOP whenever SDA is stuck low (`i2c scan` says so).
0.71: FM radio - `radio` for an RDA5807M module (tune, seek, scan, volume, RDS name + text, `display radio`,
/api/radio, MQTT set/radio); an MPU-6050 on 0x68 is no longer taken for the DS3231 clock (WHO_AM_I check).
0.72: Waveshare 1.3" OLED HAT (SH1106 on SPI0 + 3 keys + joystick) as a radio panel: `display hat`, `display keys`;
new SPI0 driver `src/spi.c`.
0.73: BMP180 (BMP085) pressure + temperature sensor at 0x77 in `sensors`, MQTT, the API and BASIC `SENSOR()`.
0.74: DCF77 radio clock receiver on a GPIO pin (`dcf`): the time without a network.
0.75: `dcf on` / `dcf off` (the pin is kept; with `dcf pon` the receiver is powered down too).
0.76: `dcf` shows how long each level lasted (a table + the last 12 periods) to tell a real signal from noise.
0.77: noise-tolerant DCF77 decoder: it locks onto the 1 s rhythm and judges each second as a whole (spikes no
longer break pulses); two good minutes that fit together set the clock even with bad minutes in between.
0.78: DCF77 with a weak / noisy signal: the minute start is found by itself and the bits are summed over
several minutes (hour and date stay, the minute goes +1), so the time comes even when every minute has errors.
0.79: AHT20 / AHT10 humidity + temperature sensor at 0x38 (e.g. the AHT20 + BMP280 board) in `sensors`, MQTT, API, BASIC.
0.80: offline voice commands with the DFRobot DF2301Q module (`voice`): a heard phrase runs a shell command.
0.81: I2C fix - after an address that did not answer, the next transfer failed (the controller was still
sending the STOP); `i2c scan` and the sensor search at boot missed devices that `i2c probe` saw.
0.82: sensors on the display (`display sensors`, also under KEY1 on the HAT), a CSV log (`sensors log 5` ->
/LOG/YYYYMMDD.CSV) and `GET /api/sensors` (JSON).
0.83: MPU-6050 / GY-521 (`motion`): tilt (spirit level), acceleration, rotation, shake detection; display, MQTT, API, BASIC.
0.84: MPU-6050 wake-up fixed (a GY-521 stayed asleep and gave all 0, which also counted fake shakes).
0.85: `motion` runs live on the whole screen until a key (spirit-level bars for pitch / roll); `motion once` prints once.
0.86: MPU-6050 on its internal oscillator (a GY-521 woke up but measured nothing with the gyro clock).
0.87: AMG8833 Grid-EYE thermal camera (`thermal`): live heat map on HDMI, person detection (MQTT), display, API, BASIC.
0.88: camera on the CSI ribbon, step 1 (`cam`): powers the camera and finds the sensor (OV5647 / IMX219).
0.89: camera step 2: `cam live` / `cam snap` / `cam bars` - the first picture from an OV5647 on HDMI (640x480).
0.90: `cam live` / `bars` print every start-up step (`cam 1` ... `cam 9`) - the board froze on 0.89, the last line shows where (`cam trace off` hides them).
0.91: while waiting for the first frame the camera lanes and the sensor exposure are printed every 0.5 s; `cam clock cont|ncont` tries the other MIPI clock mode.
0.92: `cam dump` prints every camera receiver register while streaming; `tools/camdump.py` prints the same on a Linux Pi where the camera works, to compare.
0.93: the camera now runs like the Pi firmware does: sensor mode 1296x972 (2x2 binned) and lane clock gate 0x155; `cam mode 640` / `cam gate 0x15` switch back for tests.
0.94: the sensor gets exactly the register list the Pi firmware writes (1296x976, logged by raspiraw) - no line sync packets; `cam mode fw|1296|640`.
0.95: the sensor is set up and streaming BEFORE the camera receiver starts (like rsta2/libcamera); `cam order rx` = the old order.
0.96: fix: in the firmware mode the sensor stayed in "no frames" (0x4202) from the start-up and sent only its clock.
0.97: Bluetooth keeps calling the paired keyboard until it answers (every 8 s, then 20 s, after ~3 minutes every 60 s), also after it disconnects.
0.98: `cam sregs` prints every camera sensor register RoboOS sets while it streams; `tools/ov5647regs.sh` prints the same list under Linux, to compare.
0.99: camera fix: while waiting for frames RoboOS kept writing the receiver status back (also its "synchronised" bits) thousands of times per second; now only error bits are cleared. Defaults like Linux on the same board: 1296x972 mode, gate 0x15, receiver first.
0.100: in the 1296 mode the sensor gets exactly the values Linux leaves in it (manual exposure 0x57e, gain 0x40, frame length 0x602, mirror 0x3821 = 3, no AWB).
0.101: camera BUG FOUND: RoboOS powered the wrong camera receiver (firmware power domain 13 = UNICAM0; the camera connector uses UNICAM1 = 14), so the receiver never saw data. Fixed - **the camera works on the Pi 3B+** (owner).
0.102: camera colours: the pixel colour order follows the sensor mirroring (GBRG in the default 1296 mode, like Linux); `cam bayer bggr|gbrg|grbg|rggb` if the colours look wrong.
0.103: camera photos as JPEG (`cam snap [file]`, s / space in `cam live`; own encoder `src/jpeg.c`) + picture settings saved in /CAM.TXT: exposure, gain, ev, white balance, brightness, contrast, saturation, hue, gamma, flip, JPEG quality (`cam set`).
0.104: camera web page `/camera`: live preview + sliders for every setting + save photo; API `GET|POST /api/camera`, `GET /api/camera.jpg`; `cam test on` = test picture without a camera.
0.105: video doorbell (`doorbell`: GPIO button -> photo on the card + to Home Assistant over MQTT, gong / WAV, ring event) and door radar (photo when someone comes); MQTT camera, Take photo button, doorbell event, radar sensor via discovery; `cam send`.
0.106: IR light on a GPIO for the camera (`cam ir <pin>`, `cam ir_mode off|on|photo|auto`; auto = only in the dark), also on the /camera page.
0.107: HC-SR04 distance sensor for the door (`doorbell distance <trig> <echo> [cm]`): a photo only when someone is nearer than the limit (with a radar: both).
0.108: `microscope` - full-screen live camera with zoom 1-5x (sensor pixels), arrows, photo (space) and MJPEG AVI video (v).
0.109: microscope in full resolution and full screen: sensor mode 1920x1080 (`cam mode 1080` too), every screen pixel drawn from the sensor pixels on 3 cores, `f` = fill / fit, photos up to 1920x1080.
0.110: camera sharpness (slider + `cam sharpness`), less flicker: a frame being drawn is never overwritten by the next one, exposure in whole mains periods (`cam flicker 50|60|off`), calmer auto exposure and white balance.
0.111: microscope without smears in motion: drawn on the hidden screen page and shown whole (double buffering); `k` / `cam cores 1-3` = how many cores draw.
0.112: microscope zoom in finer steps up to 2.5x: 1, 1.2, 1.5, 1.8, 2, 2.2, 2.5 (`+` / `-`, keys 1-7).
0.113: with `cam flicker 50|60` the auto exposure never goes below one mains period (10 / 8.3 ms) - a bright scene lowers the gain instead (lamp blinking gone).
0.114: microscope: Enter stops a video instead of quitting (only `q` quits), saving shows progress (% , MB, seconds left).
0.115: camera noise reduction (`cam denoise on|off`, checkbox on the page, `d` in the microscope); microscope keys `,` `.` = sharpness.
0.116: every slider of the `/camera` page takes effect on the next frame, also while the microscope / `cam live` run (brightness, contrast, saturation, hue, gamma and a fixed exposure / gain did not).
0.119: date + time stamp in photos, videos and Home Assistant photos (`cam stamp on|off`, checkbox on the `/camera` page).
0.120: burglar alarm in the kernel (`alarm`, web page `/alarm`, Home Assistant alarm panel + zone sensors + events over MQTT): zones instant / delay / interior / 24h, away / home, siren relay + tone + voice, photos + video on an alarm, photo at entry, event log /ALARM.LOG, armed again after a power cut; `alarm shield` for the IoT shield pins.
0.121: alarm keypads and card readers: 4x3 / 4x4 keypad on GPIO or on a PCF8574 (I2C), RC522 RFID reader (SPI), Wiegand readers / keypads; `alarm card learn|add|del`, code + # on the keypad, display mode `alarm` (state, stars, seconds left).
0.122: kernel update from the web: page `/update` with "Check" and "Install" buttons (the board downloads from GitHub itself, backup KERNEL8.BAK, reboots, the page waits for it), `GET|POST /api/kernel`.
0.123: the SD card is tried up to 4 times at boot (the owner's 3B often needed `sd init` after a reboot); when a retry was needed the boot line says how the first try failed.
0.124: SD init no longer asks the card for 1.8 V signalling (S18R - the Pi 3 cannot do it; large SDXC cards may misbehave), and a boot retry powers the card off and on first. Settings like the web token and `http on` are read from the card at boot - when the card was not mounted they were missing.
0.125: the card heals itself: a failed block read / write re-initialises the card once and tries again, and if the card was not mounted at boot every file access tries to mount it again (at most every 2 s). `sd` shows how often that happened.
0.126: re-initialising the card never touches the WiFi pins / the Arasan controller any more (in 0.125 an automatic card re-init could cut off the WiFi); the boot tries the SDHOST controller 3 times before falling back to the Arasan.
0.127: the WiFi firmware files are read from the card into RAM before the WiFi chip starts (on a 3B with a Samsung 64 GB card the card failed while its reads alternated with the firmware upload).
0.128: new home page (`http://<board>/`): status tiles (uptime, time, CPU, network, address, alarm, API, HTTPS), cards for Alarm / Camera / Photos & videos / Files / Update / JSON API, and a Reboot button (token or admin login; the page reloads when the board is back).
0.129: `alarm keypad gpio` with no pins = a 4x4 membrane keypad straight on the header (GPIO 5 6 13 19 / 12 16 20 21 = pins 29 31 33 35 / 32 36 38 40), wiring table in the README.
0.130: `alarm keypad plug [reversed]` = the keypad's 8-pin connector pushed straight onto header pins 23-37 (no wiring; the wire on the GND pin is read the other way round, all 16 keys work); keypad lines may be `gnd` in `alarm keypad gpio`.
0.131: `alarm keypad shield` (keypad with the IoT shield: GPIO 13 19 26 21 / 20 7 18 + GND); pin clashes between the keypad / card reader and alarm zones or the doorbell button are shown (`!!`, `PIN TAKEN`) and such a zone is not watched.
0.132: `alarm keypad test` - live key / line levels for checking the keypad wiring.
0.133: while the alarm is on, the display's normal status screen shows the alarm (state, code as stars, seconds left) by itself; the console shows the typed code as stars (`[KEYPAD] code: ****`). A zone with nothing connected shows as open (e.g. the window of `alarm shield`) - remove unused zones (`alarm zone del 27`).
0.144: GSM / LTE modem for the alarm (SIM800L, A7670E, SIM7600E on pins 8/10): SMS on alarm / arm / disarm, calls in an alarm, SMS commands ARM / HOME / DISARM / STATUS from listed numbers (`modem`).
0.145: OLED module with a rotary knob + CONFIRM / BACK buttons as the alarm keypad (`alarm knob`): turn = digit, push = add it, BACK = delete, CONFIRM = send.
0.146: alarm reports to a monitoring centre (PCO / ARC) with SIA DC-09 over TCP or UDP: Contact ID (ADM-CID) or SIA-DCS, AES encryption, backup receiver, supervision, daily test (`pco`).
0.147: PCO: supervision (NULL) with sequence 0000 and retries with the same bytes (time included), so receivers drop repeats - tested against `pco-server` (Robo PCO).
0.148: door intercom over the web (`intercom`, page `/door`): INMP441 microphone + MAX98357A speaker on I2S - listen to the door and hold to talk from the phone's browser.
0.143: FIX Pi 3B+: straight to a 100 Mbit board (Pi 3B) it said `link up, 1000 Mbit` and set its MAC to 1000 while the wire ran at 100 = nothing got through; 1000 is now taken only when the 1000BASE-T receivers report OK.
0.142: Pi 3B+: the LEDs on the network jack are switched on (RoboOS never enabled them); `net` shows what the PHY sees (link, auto-negotiation, what the other side offers, 1000 Mbit).
0.141: FIX: while WiFi carried the traffic, a cable plugged in was never noticed (the link was only checked while frames were read from the cable) - so cable + WiFi (0.140) did not use the cable.
0.140: FIX keypad terminal (0.139): every digit was cleared at once and `#` said NO CONNECTION at once (a time stamp taken later than the loop's own clock); `NO ANSWER` when the code arrived but no state came back. Messages between boards now go over EVERY link that is up - cable AND WiFi at once, the second copy is dropped - so one path may fail; `net ip wifi` keeps TCP/IP (web, MQTT, time) on WiFi even with a cable in (/LINK.TXT), e.g. master and keypad joined by a cable straight from board to board plus both on WiFi.
0.139: keypad terminal over WiFi: the code is kept on the terminal and sent WHOLE after # / A / B / D (one message instead of one per key), the display shows `Sending...` until the master answers, or `NO CONNECTION` + a low beep; messages go out from a queue in the background (repeated until acknowledged), so the keypad never stops reading keys; `alarm` shows delivered / not delivered messages and WiFi sends the chip refused.
0.138: the web pages (/update, /alarm, home page Reboot) show WHY the board refused (wrong token, API off, HTTPS is on, too many tries) instead of only asking for the token again.
0.137: the keypad terminal answers at once after `#`: the master sends its new state before a voice message plays (a WAV blocks for seconds) and keeps sending during it; a terminal that does not answer is really left alone for 10 s.
0.136: beeper for a keypad board: `sound out gpio` + a piezo / small speaker on GPIO18 (pin 12) and GND; when GPIO19 is a keypad or Waveshare HAT line, only GPIO18 is used.
0.135: the `remote` line can be set on the web page /alarm (it was dropped when saving there).
0.134: keypad terminal: `alarm remote <master>` makes another RoboOS board the alarm's keypad + display over the LAN (messages signed + encrypted with the same `http token`).
0.118: microscope key `w` = camera mode: 1920x1080 centre / 2592x1944 whole sensor (sharp, ~15 fps) / 1296x972 whole (fast); photos up to the full 2592x1944; `cam mode full`.
0.117: file manager on the web: `/files` and `/microscope` (view, download, delete photos and videos on the card).
QEMU test: `tools/qemu_btkbd_test.py` plays the controller and a keyboard.

### Date and time (NTP)

When the network is up, the board asks an NTP server for the time (at once, then every hour) and
prints `[NET] time 2026-11-18 21:05:07 (NTP pool.ntp.org)`. The settings are in `/TIME.TXT`:

```
time                     the date, time, zone and when it was synced
time zone +1 eu          UTC offset (+5:30 works too); eu = EU summer time, without it none
time server pool.ntp.org NTP server ('off' = none)
time sync                ask now
time set 2026-11-18 21:05   set by hand (local time), e.g. without a network
```

The time is in `GET /api/status` (`"time":"2026-11-18T21:05:07+01:00"`, `"unix"`), on the web page,
in BASIC (`PRINT DATE$; " "; TIME$`, `HOUR`, `MINUTE`, `SECOND`, `UNIXTIME`), in C (`unixTime()`,
`timeText(buf)`, API version 9) and in the alarm demo's messages. The Pi has no battery clock: after
a power cut the time comes back with the network (or `time set`).

### HTTPS (TLS)

`http tls on` switches on HTTPS on port 443 (saved in `/HTTP.TXT`, it comes back after a reboot).
Then the JSON API answers **only over HTTPS** - on port 80 it says to use `https://` - so the token
never goes over the network in clear text. `http tls off` switches it off, `http tls new` makes a
new certificate.

- The first `http tls on` makes an ECDSA P-256 key and a **self-signed certificate** for the board's
  name and address (valid 20 years): `/TLS/KEY.PEM` and `/TLS/CERT.PEM`. `http` shows its SHA-256
  fingerprint. A browser warns once ("not secure" - it does not know who made it); curl needs
  `-k`, or better, copy `CERT.PEM` to the PC and trust exactly this certificate:

  ```
  curl --cacert CERT.PEM -H "Authorization: Bearer secret123" https://192.168.1.50/api/status
  ```
- Your own certificate (e.g. from your own CA, or `openssl req -x509 -newkey ec -pkeyopt
  ec_paramgen_curve:P-256 -nodes -keyout KEY.PEM -out CERT.PEM -days 3650 -subj /CN=robo`):
  put `CERT.PEM` (PEM or DER, a chain is fine) and `KEY.PEM` (EC or RSA) into `/TLS`.
- TLS 1.2 with ECDHE and AES-GCM / ChaCha20 by [BearSSL](https://bearssl.org) 0.6 (MIT); a
  connection is about 40 ms in QEMU. At most 4 HTTPS connections at once; sessions are resumed.

### JSON API: pins and reboot over the network

With a token set (`http token <secret>`, 8-63 characters, saved in `/HTTP.TXT`), the web
server also answers JSON requests under `/api/`. Every request needs the header
`Authorization: Bearer <secret>`; without it the answer is 401, without any token set
the API is off (403). `http token` shows the token, `http token off` turns the API off.

| Request | What it does |
|---------|--------------|
| `GET /api/status` | name, board, version, address, uptime, CPU temperature (thousandths of a degree) |
| `GET /api/gpio` | all pins 0-27: `{"pin":17,"mode":"out","value":1}` ... |
| `GET /api/gpio/17` | one pin |
| `POST /api/gpio` | `{"pin":17,"value":1}` - sets a pin (it becomes an output); `value` 0, 1, true, false or `"toggle"`; `{"pin":4,"mode":"in","pull":"up"}` - input with a pull-up (`pull` up/down/off); a list `[{...},{...}]` sets several pins (all are checked first, then set) |
| `POST /api/sound` | plays a WAV from the `/SOUNDS` folder: `{"file":"dingdong.wav","out":"jack"}` - `out` is `jack`, `gpio` (GPIO18/19) or `i2s` (the DAC module, also `"dac"` / `"device"`), without it the current output (`sound out`); optional `"volume":0-100`. `{"tone":880,"ms":500}` plays a tone, `{"stop":true}` stops. Answers 202 at once |
| `GET /api/sound` | what plays, the result of the last sound, output, volume and the files in `/SOUNDS` |
| `POST /api/reboot` | answers `{"ok":true,"rebooting":true}` and reboots after 0.5 s |

```
curl -H "Authorization: Bearer secret123" http://192.168.1.50/api/status
curl -H "Authorization: Bearer secret123" -d '{"pin":17,"value":1}' http://192.168.1.50/api/gpio
curl -H "Authorization: Bearer secret123" -X POST http://192.168.1.50/api/reboot
```

Sounds: put WAV files (PCM 8/16 bit, 8-48 kHz) into `/SOUNDS` on the card (it comes with
`DINGDONG.WAV`, `BEEP.WAV`, `SIREN.WAV`). Core 0 plays them while it waits in the shell, BASIC or
a program (the SD card belongs to core 0), not during `playwav` or a video; the output switches
back afterwards and the choice in `/SOUND.TXT` stays.

```
curl -H "Authorization: Bearer secret123" -d "{\"file\":\"dingdong.wav\",\"out\":\"i2s\"}" http://192.168.1.50/api/sound
```

Errors come back as `{"ok":false,"error":"..."}` with 400 / 401 / 403 / 404 / 405. A web page
from another address may call the API too (CORS is allowed). The board prints
`[API] reboot asked by <address>` and `[API] wrong or missing token from <address>`.
**HTTP is not encrypted**: anybody who can watch your LAN can read the token, so use the
API only on your own network, not from the internet. Pins 14/15 are the UART.

### Updates over the network (kernel and files, 0.47)

With `http on` and a token (`http token <secret>`) or an admin login, a new kernel goes to the
board without taking the card out:

```
curl --data-binary @kernel8.img -H "Authorization: Bearer <secret>" http://<board>/api/update
curl --data-binary @ALARM.BIN -H "Authorization: Bearer <secret>" "http://<board>/api/file?path=/APPS/ALARM.BIN"
```

(`https://` and `curl -k` when HTTPS is on; `-u name:password` for a user login.) The kernel is
checked first: it must be a RoboOS kernel (it carries a `ROBOOS-KERNEL:<version>` tag), so a wrong
file never replaces the running one. The old kernel is copied to `KERNEL8.BAK`, the new one is
written into fresh clusters, the directory entry switches to it, it is read back and compared - then
the answer `{"ok":true,"old":"0.46","new":"0.47",...}` comes back and the board reboots
(`?reboot=0` = do not). 16 MB at most. `update` shows the kernel on the card and the backup,
`update restore` puts the backup back. If a new kernel does not start at all, take the card out
and rename `KERNEL8.BAK` to `kernel8.img`. Tested in QEMU (HTTP + HTTPS, wrong token, a file that
is no kernel, restore, reboot) - not yet on hardware.

**The board fetches it itself (0.48).** `update check` downloads the kernel and says which version it
is; `update get` installs it the same safe way and reboots. By default it comes from this repository
(`https://raw.githubusercontent.com/didi2sk/RoboOS/main/sd/kernel8.img`); another address goes into
`/UPDATE.TXT` with `update url https://...` (a PC in the home network: `http://192.168.1.10:8000/kernel8.img`,
e.g. `python -m http.server`). HTTPS certificates are checked against built-in roots (Let's Encrypt,
DigiCert, Sectigo, Google, GlobalSign, Amazon; `tools/mkta.py`) and `/TLS/CA.PEM` (your own CA) - the
board needs the time for that (NTP or `time set`). `update get -k` (or the word `insecure` in
/UPDATE.TXT) skips the check. The same version is not installed again, an older one only with
`update get force`. Redirects and chunked answers are followed. Works on the Pi 3B+: a kernel sent from a PC, and `update check` from GitHub with the certificate checked.

**From the web page (0.122):** `http(s)://<board>/update` (also the "update" link next to the version on the main page):
"Check for an update" shows the version on GitHub, "Install the update" makes the board download it itself, write it
(the old one as KERNEL8.BAK), reboot, and the page waits until the board answers again. "force" reinstalls the same or an
older version. Needs the token or an admin login like the other pages. API: `GET /api/kernel` (state, message, available),
`POST /api/kernel` `{"cmd":"check"}` / `{"cmd":"install","force":false}`.

### Users: a login for the web page and the API (optional)

As long as there are no users, nothing changes: the page is open and the API takes the token.
The first `users add` turns the login on for the page and the whole API:

```
users add robo MyPassword1          an admin: may do everything (the first user must be an admin)
users add guest Reader123 view      a "view" user: may only look (page, GET /api/...)
users                               the list
users passwd guest NewPass99        a new password
users del guest                     delete (when the last one goes, the page is open again)
```

The browser shows its own login window; curl: `curl -u robo:MyPassword1 http://<address>/api/status`.
A "view" user gets 403 for POST (pins, sound, reboot); the Bearer token still works and counts as
an admin. Five wrong passwords (or tokens) from one address lock that address out for a minute
(429), the board prints `[WEB] wrong login 'x' from <address>`. `/USERS.TXT` keeps only a salted
hash of each password (PBKDF2-HMAC-SHA256, 1000 rounds), never the password itself.
**Turn HTTPS on (`http tls on`)**: without it the browser sends the password readable over the
network; with HTTPS on, `http://` pages jump to `https://` and nothing asks for a password in clear text.
The shell (keyboard / UART on the board) can ask for a login too: `users console on` - after a
reboot or `logout` it shows `RoboOS login:` and only an admin gets in (the password shows as `*`,
3 wrong tries = 10 s wait). Programs in `/RUN` still start by themselves; Ctrl+C out of them leads to
the login. `users console off` turns it off. **Forgot the password?** Take the SD card to a PC and
delete `/USERS.TXT` - that opens the shell and the web page again (there is no hidden master password).

### TCP and UDP in programs

Connections are numbered 1-8 (a program gets a number from `TCPCONNECT`, `TCPLISTEN`,
`TCPACCEPT` or `UDPOPEN`); a negative result is an error: -1 no network, -2 unknown host
name, -3 no answer / refused, -4 too many connections, -5 bad number, -6 closed,
-7 out of memory, -8 port in use. Everything a program opened is closed when it ends.

BASIC (integers only, so data is read byte by byte or as numbers):

| | |
|---|---|
| `H = TCPCONNECT("host", port)` | connect (address or name), waits up to 10 s |
| `L = TCPLISTEN(port)`, `C = TCPACCEPT(L)` | server: `TCPACCEPT` gives 0 until somebody connects |
| `U = UDPOPEN(port)`, `U = UDPOPEN(port, "host", port)` | UDP; without a host, `SOCKPRINT` answers the last sender; port 0 = any |
| `SOCKPRINT H, "text"; X; CHR$(13)` | send, like PRINT (ends with CR LF unless it ends with `;`); one UDP datagram |
| `UDPSEND U, "host", port, items` | UDP to a given address (`"255.255.255.255"` = everybody) |
| `SOCKAVAIL(H)` | bytes waiting, -1 = the other side closed |
| `SOCKGET(H)` | next byte, -1 = nothing yet, -2 = closed |
| `SOCKNUM(H)` | next number in the data (waits up to 2 s), 0 if none |
| `SOCKCLOSE H` | close |

```
10 H = TCPCONNECT("192.168.1.10", 5000) : IF H < 0 THEN PRINT "error "; H : END
20 SOCKPRINT H, "temperature "; TEMP
30 K = SOCKGET(H) : IF K >= 0 THEN PRINT CHR$(K); : GOTO 30
40 IF K = -1 THEN 30
50 SOCKCLOSE H
```

C (`sdk/robos.h`): `tcpConnect`, `tcpListen`, `tcpAccept`, `udpOpen`, `sockWrite`, `sockPrint`,
`sockRead` (TCP: bytes, 0 = nothing yet, -1 = closed; UDP: one datagram), `sockAvailable`,
`udpSendTo`, `udpReceiveFrom`, `sockClose`, `sockError`, `ipAddress`. Examples:
`tcpserv` (a TCP server for `nc <address> 23`) and `udpecho` (`nc -u <address> 5000`).

The network (and the USB keyboard) run on their own processor core, so the board answers
pings, serves its web page and receives messages even while a program is busy computing.

A standard USB network adapter of the CDC-ECM class works too (this is how QEMU
tests the network; on real boards not tested yet).

## IoT: sensors (I2C) and MQTT

### I2C sensors

The I2C bus is on header pins 3 (GPIO2 = SDA) and 5 (GPIO3 = SCL), with 3.3 V on pin 1 and GND
on pin 6. At boot RoboOS looks for these modules by itself (`sensors scan` looks again):

| Module | Address | Values |
|---|---|---|
| BME280 (BMP280) | 0x76 / 0x77 | temperature, humidity (BME280 only), pressure |
| BMP180 (BMP085) | 0x77 | temperature, pressure (0.73) |
| AHT20 (AHT10) | 0x38 | humidity, temperature (0.79; the AHT20 + BMP280 board gives humidity + pressure) |

Using the values (0.82):

```
sensors                 all values now
display sensors         temperature / humidity / pressure on the display (2 rows: "21.4C 45.2%" + "1001.3 hPa")
sensors log 5           a line every 5 minutes to /LOG/YYYYMMDD.CSV (one file per day; saved in /SENSLOG.TXT)
sensors log now | off
```

The CSV (`time,temp_c,hum_pct,press_hpa,cpu_c`, local time) opens in Excel / LibreOffice for a graph; the log
needs the time (NTP, DCF77, battery clock or `time set`). `GET /api/sensors` (with the API token) answers e.g.
`{"ok":true,"cpu_c":48.31,"temp_c":21.43,"hum_pct":45.20,"press_hpa":1001.32,"log_min":5}`. BASIC `SENSOR(n)`,
C `sensorRead(n)` and MQTT (state JSON) have the same values.
| ADS1115 | 0x48-0x4B | 4 analog inputs A0-A3, 0-4.096 V in mV (do not exceed 3.3 V!) |
| DS3231 (DS1307) | 0x68 | battery clock: the time is known at boot even without a network |

```
sensors                  values of everything found (+ CPU temperature)
i2c scan                 every device on the bus, with a guess what it is
i2c read 0x76 0xd0       read a register (i2c read <addr> <reg> [count])
i2c write 0x76 0xf4 0x27 write a register
```

The clock module is set every time NTP answers (or by `time set`), and at boot the time is
taken from it - `time` then says "from the battery clock (DS3231)".

### I2C display (OLED / LCD, 0.55)

The usual cheap I2C displays go on the same four pins (3.3 V, GND, SDA, SCL) and are found at boot:

| Display | Address | Text |
|---|---|---|
| OLED 0.96" / 1.3" 128x64, SSD1306 or SH1106 | 0x3C / 0x3D | 16 x 4 (128x32: 16 x 2) |
| text LCD 16x2 / 20x4 (HD44780) with a PCF8574 "I2C backpack" | 0x27 / 0x3F | 16 x 2, 20 x 4 |

At boot it shows "RoboOS" + the version, after 2 s a **status screen**: board name, IP address, time,
WiFi / Ethernet (on 2 rows: IP + time) - handy for a board without a monitor.

```
display                  what was found (type, address, size)
display scan             look again (after plugging it in)
display text 2 Hello     text on row 2 (turns the status screen off until the next boot)
display clear            empty display
display status on|off    the status screen (saved)
display size 20x4        the LCD size can not be detected: 16x2 is assumed (also 128x32 for a small OLED)
display type ssd1306     also for an OLED that does not answer at all (no ACK): written blind
display type sh1106      if the OLED picture is shifted / the right edge is noise (auto, lcd, ssd1306, sh1106, none)
display addr 0x26        another LCD backpack address (A0-A2 jumpers: 0x20-0x27, PCF8574A 0x38-0x3F)
display font small       OLED font: micro 32 x 10, tiny 25 x 8, small 21 x 8, normal 16 x 4, big 8 x 2 (big digits) - saved
display time             big digital clock: thin HH:MM with smaller seconds beside, weekday + date below (128x64), LCD = time + date
display info             back to the status screen (name, IP, time, link)
display console on       a copy of the terminal on the display (best with the small font); off = back to info
display contrast 128     OLED brightness 0-255;  display light on|off = LCD backlight;  display on|off
```

Text over the network: JSON API `POST /api/display` with `{"text":"line 1\nline 2"}` (whole screen) or
`{"row":3,"text":"..."}`, `{"clear":true}`; `GET /api/display` shows what is on it. MQTT: `roboos/<name>/set/display`
(the payload = the screen, new line = next row) and `roboos/<name>/set/display/<row>`. Example:
`curl -X POST http://<board>/api/display -H "Authorization: Bearer <token>" -d '{"text":"Hello\nfrom the PC"}'`.

The settings go to `/DISPLAY.TXT`. BASIC: `DISPLAY 1, "T="; T` and `DISPLAY CLS` (example
`/APPS/display.bas`), C (API 11): `displayText(row, text)`, `displaySize()`. **Careful with 5 V LCDs:**
the backpack has its own pull-ups on SDA/SCL to its supply - powered from 5 V they put ~3.8 V on the
Pi's pins (over 3.3 V). Either remove those two resistors (usually 4.7k, next to the header) or use
an I2C level shifter; powered from 3.3 V the text is often too faint. OLED modules are 3.3 V parts - fine.
The contrast trimmer on the LCD backpack must be turned until the text shows. **Not yet tested on hardware** (only the PC test
`tools/test_display.c`, which plays the three chips).

### AI: questions to Claude (0.56)

With your own Anthropic API key (make one at console.anthropic.com -> API keys; it is paid per use,
set a monthly spend limit there) the board can ask Claude over HTTPS (certificate checked):

```
ai key sk-ant-...          store the key (then 'clear' the screen); 'ai key off' removes it
ai how long does a 2000 mAh battery last at 150 mA
ai @wifi why does it take so long      runs 'wifi' and sends its output with the question
ai @"sensors" is it warm in here
ai model claude-sonnet-5-5             another model; 'ai model' = back to the default
ai                                     model, whether a key is set, usage
```

The default model is the cheapest one, `claude-haiku-5-5` (a short question costs a fraction of a
cent). The answer is shown as plain ASCII (accents are dropped: the font has no diacritics). Claude
gets a short description of RoboOS and the list of shell commands, so it can answer "how do I ..."
questions with real commands. The key lives in `/AI.TXT`, **XOR-scrambled** - only so it is not
readable at a glance; anyone with the card can recover it, so keep the spend limit low. It never
goes to the shell history. **Not yet tested against the real API** (QEMU test with a fake server).

### FM radio (RDA5807M, 0.71)

The tiny RDA5807M module (RRD-102, ~11 x 11 mm) is an FM receiver on the I2C bus (addresses 0x10/0x11),
3.3 V. Wiring: VCC pin 1, GND pin 6, SDA pin 3, SCL pin 5, about 75 cm of wire on ANT; the sound comes
out of the module itself on L / R (32 ohm headphones directly, a speaker through a small amplifier).

```
radio 104.4          tune (also 104,4 or 1044), switches it on
radio up / down      seek the next station
radio scan           list every station of the band (87-108 MHz) with its signal
radio vol 8          volume 0-15 (0 = muted)
radio mono on|off    forced mono (weak stations hiss less); radio bass on|off
radio                frequency, stereo, signal, RDS station name and text
radio off / on
display radio        the radio on the OLED: frequency, station name, radio text
```

The station, volume and on/off are saved in `/RADIO.TXT` and restored at boot. Over the network:
`POST /api/radio` with `{"cmd":"104.4"}` (or `up`, `down`, `off`, `vol 8`), `GET /api/radio` (state +
RDS), MQTT `roboos/<name>/set/radio`. **Not yet tested on hardware** (PC test `tools/test_radio.c` with a fake chip).

**Radio panel: Waveshare 1.3" OLED HAT (0.72).** The HAT plugs straight onto the 40-pin header: a 128x64 SH1106
OLED on SPI0 (CE0, DC GPIO24, RST GPIO25) plus KEY1-3 (GPIO21/20/16) and a 5-way joystick (up 6, down 19,
left 5, right 26, press 13). The RDA5807M stays on I2C (pins 3/5 are free on the HAT).

```
display hat          use the HAT (saved as "bus spi" in /DISPLAY.TXT); 'display type auto' = back to I2C
display radio        radio screen
display keys         which keys are pressed now (test)
```

Keys: KEY1 = next screen (info, clock, radio, console); on the radio screen KEY2 / KEY3 = seek down / up,
joystick left / right = -/+ 0.1 MHz, up / down = volume, press = radio on / off. Note: GPIO24/25/16 are also
outputs of the IoT shield - do not use both. **Works on hardware** (owner, 0.72).

### DCF77 radio clock (0.74)

A DCF77 receiver module (77.5 kHz from Mainflingen near Frankfurt, ~2000 km range; a small board with a
ferrite rod antenna, e.g. "RC8000", ELV DCF-2, Pollin, Canaduino) gives the exact time without any network.
Wiring: VCC to 3.3 V (pin 1), GND (pin 6), the output (OUT / TCO / T) to a free GPIO, e.g. GPIO17 (pin 11);
a PON / P1 input (if there is one) to GND - or to a GPIO with `dcf pon <gpio>` (held low = receiver on).

```
dcf pin 17           the receiver's output is on GPIO17 (saved in /DCF.TXT, read at boot)
dcf                  signal: level changes, pulse lengths (100 ms = 0, 200 ms = 1), last minute, last time
dcf invert on|off    pulse polarity by hand (auto by default - modules differ)
dcf pull up|down|off pull resistor on the pin (up = for open-collector outputs)
dcf off / dcf on     stop / start reading it (saved; the pin is kept; PON goes high = receiver off)
```

The pin is read by core 1 every millisecond; the decoder locks onto the 1 s rhythm (averaged over ~16 s,
`dcf` shows the lock quality) and judges each second as a whole, so short spikes do not break it. Once two
minutes decode and fit together (all parity bits right, N minutes apart) the clock is set (`time` says "from the
DCF77 radio signal") and the battery clock too. If NTP set the time in the last 2 hours, DCF77 only checks
it. The first time comes after ~2-3 minutes of clean signal. Keep the antenna about 1 m away from the Pi,
monitors and chargers, turned broadside to Frankfurt; reception is best at night. **Not yet tested on
hardware** (PC test `tools/test_dcf.c`: CET/CEST, both polarities, glitches, a lost pulse, noise).

### Offline voice commands (DFRobot DF2301Q, 0.80)

The DFRobot Gravity offline voice recognition module (DF2301Q, SKU SEN0539) recognises speech by itself,
without a network: a wake word ("Hello robot" or one you teach it), 121 fixed English commands and up to
17 learned ones (taught on the module itself). Set its switch to **I2C** and wire it to the I2C pins
(VCC 3.3 V pin 1, GND pin 6, SDA pin 3, SCL pin 5); its address is 0x64. RoboOS reads the id of every
recognised command and runs the shell command you gave it:

```
voice                      status, the last heard id, the actions
voice add 5 gpio 17 1      learned command 5 switches GPIO17 on
voice add 6 gpio 17 0
voice add 7 radio off      any shell command
voice del 6
voice volume 1-7 | mute on|off | wake <seconds> | play <id>
```

Say the wake word, then the command: `[VOICE] 5 -> gpio 17 1` and the command runs. An id with no action is
printed, so you can find out the number of each phrase. The actions are saved in `/VOICE.TXT` (`<id> <command>`
per line). They run only at the shell prompt (not while a program or BASIC runs; a command heard then waits
up to 10 s). **Not yet tested on hardware** (PC test `tools/test_voice.c` with a fake module).

### Tilt and shakes (MPU-6050 / GY-521, 0.83)

The GY-521 board (MPU-6050: accelerometer + gyroscope + thermometer) goes on the I2C pins like the other
sensors (VCC 3.3 V pin 1, GND pin 6, SCL pin 5, SDA pin 3; XDA / XCL / INT unconnected). AD0 open = address
0x68, AD0 to 3.3 V = 0x69 (use that next to a DS3231 clock, which is also 0x68).

```
motion                  LIVE until a key: pitch / roll (deg) with spirit-level bars, acceleration (mg),
                        rotation (deg/s), chip temperature, shakes
motion once             the same values printed once
motion zero             this position = level (and the gyro at rest = 0); saved in /MOTION.TXT
motion threshold 400    a shake = |acceleration| more than 400 mg off 1 g (or turning > 150 deg/s)
motion alarm on|off     print every shake; MQTT <base>/motion gets the count anyway
display motion          pitch / roll / |a| / shakes on the display (also under KEY1 on the HAT)
```

BASIC `SENSOR(8)` pitch, `SENSOR(9)` roll (0.1 deg), `SENSOR(10)` shakes, `SENSOR(11..13)` acceleration x/y/z (mg);
the MQTT state and `GET /api/sensors` carry pitch, roll and shakes too. **Not yet tested on hardware** (PC test
`tools/test_motion.c`: integer atan2 within 0.1 deg of libm, tilt at known angles, gyro / temperature, shakes).

### Thermal camera (AMG8833 Grid-EYE, 0.87)

The AMG8833 (Panasonic Grid-EYE, e.g. the Adafruit board) measures 8x8 temperatures (0-80 C, about +-2.5 C,
up to ~7 m). Wire it to I2C (VIN 3.3 V pin 1, GND pin 6, SDA pin 3, SCL pin 5; 3Vo and INT open); address 0x69.

```
thermal                  a live heat map on the HDMI screen (smoothly scaled, colour scale, max / min / avg,
                         person yes/no), any key stops
thermal once             the 64 temperatures as numbers
thermal range 20 35      fixed colours (C); 'thermal range auto' = from the coldest to the hottest pixel
thermal person 2         a person / warm object = pixels 2 C above the background (default)
display thermal          max / min / avg / person on the I2C display (also under KEY1 on the HAT)
```

A person (two or more warm pixels) is sent to MQTT `<base>/presence` (1 / 0, retained) when it changes;
BASIC `SENSOR(14)` = the hottest pixel (0.01 C), `SENSOR(15)` = person; the MQTT state and `GET /api/sensors`
(with all 64 pixels) carry it too. **Works on the Pi 3B+** (the live map shows a hand; PC test `tools/test_thermal.c`: decoding,
presence, the heat map picture).

### Camera on the CSI ribbon (OV5647 / IMX219, 0.88)

The Raspberry Pi camera (v1 = OV5647, also most cheap clones; v2 = IMX219) is driven directly,
without the GPU firmware (the Linux `bcm2835-unicam` way). Step 1: `cam` switches the camera on
(CAM_GPIO0: firmware expander pin 5 on the Pi 3, GPIO40 on the Zero 2 W), talks to the sensor on
its own I2C bus (I2C0 on GPIO44/45 - not the pins on the header) and prints the chip id
(OV5647 = `5647`). `cam reg <addr> [value]` reads / writes a sensor register, `cam off` switches it off.
0.89 (OV5647 only): `cam live` shows the picture on HDMI until a key, `cam snap` one picture,
`cam bars` the sensor's own colour bars (tests the cable link without the lens). The sensor runs its
640x480 mode (60 fps) with its own auto exposure and white balance; the picture is 320x240 colour
pixels drawn 2x or 3x. After stopping it prints the frame counts and the receiver status - photograph
those lines if there is no picture. The sensor was found on a Pi 3B (0.88); **the picture works on the Pi 3B+ since 0.101** (0.89-0.100 powered the wrong receiver).

Since 0.103 the picture is 648x486 and photos are saved as JPEG (own small encoder in the kernel):

```
cam snap                one photo -> /CAM/20261122/213001.JPG (date folder + time; /CAM/IMG00001.JPG without the clock)
cam snap /DOOR.JPG      one photo to a chosen file (8.3 name, the folder must exist)
cam live                live picture; s or space saves a photo, other keys stop
cam view                one picture on HDMI only
cam set                 all picture settings
cam exposure auto|10    exposure: own auto exposure or fixed ms (0.1 - 35.6 ms)
cam gain auto|2.5       sensor gain 1 - 15.9x (auto: exposure first, then gain)
cam ev -1               auto exposure brighter / darker (-3 .. 3, each step = 2x)
cam wb auto|lock|120 90 white balance: auto (grey world), lock = keep the current, or red / blue gains in %
cam brightness 10       -100 .. 100
cam contrast 20         -100 .. 100
cam saturation 130      0 .. 300 % (0 = black and white)
cam hue -10             colour turn in degrees, -180 .. 180
cam gamma on|off        gamma 2 (off = linear, the 0.102 look)
cam flip h|v|hv|off     mirror the picture
cam quality 85          JPEG quality 10 .. 100
cam sharpness 30        sharpening 0 .. 200 % (0.110, default 30)
cam denoise on|off      noise reduction (0.115): small differences from the neighbours are smoothed, edges stay
cam stamp on|off        date + time in the corner of photos and videos (0.119, default on; needs the clock: NTP / time set)
cam flicker 50          mains frequency of the lamps 50|60|off: exposure in whole 10 ms steps, never shorter = no flicker (0.110/0.113)
cam defaults            all back to the defaults
```

The settings are saved in `/CAM.TXT`. A 648x486 photo is about 50-100 KB; the live view shows fps,
exposure and gain under the picture.

**Web page (0.104):** `http on` + `http token <secret>`, then open `http://<board address>/camera` (or https).
It shows a live preview (a few pictures per second) and sliders for all the settings above; a change applies
to the next picture and is saved in `/CAM.TXT` (2 s after the last change). "Save photo" writes a JPEG to
`/CAM` on the card, "download this picture" saves the preview to the PC. The page asks for the token once
(kept in the browser); with logins (`users`) the browser login is used. The camera switches off 10 s after
the page stops asking. API: `GET /api/camera` (settings + current exposure / gain), `POST /api/camera`
`{"saturation":"130","wb":"auto","snap":"1"}`, `GET /api/camera.jpg` (one picture):

```
curl -H "Authorization: Bearer <secret>" http://<board>/api/camera.jpg -o foto.jpg
```

`cam test on` gives a made-up colour-bar picture instead of the camera (to try the page without one).

**Microscope (0.108):** `microscope` (or `cam micro`) = the live camera picture over the whole HDMI screen with
the lowest delay (made for soldering under the camera), keyboard only:

| Key | |
|---|---|
| `+` / `-` | zoom 1 - 1.2 - 1.5 - 1.8 - 2 - 2.2 - 2.5x (0.112; keys `1` ... `7` jump to a step) |
| `f` | fill the whole screen (default) / fit the whole picture |
| `k` | cores that draw: 3 / 1 / 2 (also `cam cores 1-3` and on the `/camera` page) |
| `,` / `.` | sharpness - / + 10 % (0.115) |
| `d` | noise reduction on / off (0.115) |
| `w` | camera mode (0.118): **1080** = the centre of the sensor, sharp, 30 fps -> **full** = the whole sensor 2592x1944, wider and sharp, ~15 fps -> **1296** = the whole sensor binned, 30 fps (remembered as `micro_mode`) |
| arrows | move the zoomed area, `c` = back to the centre |
| space (or `p`) | photo (JPEG to `/CAM/<date>/`) |
| `v` | video start / stop (MJPEG AVI, `/CAM/<date>/<time>.AVI`; VLC, Windows and phones play it); Enter also stops it |
| `q` | quit (0.114: Enter no longer quits) |

Since 0.109 the microscope runs the sensor in its 1920x1080 mode (the centre of the sensor, no binning, 30 fps) and
draws every screen pixel straight from the sensor pixels (cores 0, 2 and 3 share the work), filling the whole screen;
on a 1080p screen 1x is one sensor pixel per screen pixel, the zoom shows real detail. Photos are saved at the
sensor's resolution of what is on the screen (up to 1920x1080), videos at 960x540. If the camera gives no picture
in that mode, it falls back to 1296x972 by itself. The video is kept in RAM while recording (up to ~190 MB, a few minutes) and written to the card
when stopped - during recording the picture is a little slower (every frame is JPEG-encoded). The settings of
`cam set` / the `/camera` page (exposure, white balance, flip...) apply here too. For a real microscope use a
macro / microscope lens on the camera (e.g. the Waveshare or Arducam microscope lenses for the Pi camera, or an
M12 / C-mount lens on a camera board with that mount) - the zoom here is digital.

**Files on the card over the web (0.117):** `http://<board>/files` (or `/microscope` = straight to `/CAM`
with the photos and videos) lists a folder, opens sub-folders, **view** / **download** a file, **delete** one or
all ticked ones. Same token / login as the `/camera` page. Files directly in the root (boot and settings files)
are not deleted from the web; a folder is deleted only when empty. API: `GET /api/files?path=/CAM`,
`GET /api/file?path=/CAM/X.JPG` (`&dl=1` = as a download), `POST /api/files {"delete":"/CAM/X.JPG"}`.
A big video is first read from the card into memory, so its download starts after a while.

**IR light (0.106):** IR LEDs or an IR lamp switched by a GPIO (through a transistor / MOSFET / relay - not
straight from the pin): `cam ir 22` (the pin, `cam ir off` = none), `cam ir_mode off|on|photo|auto`
(photo = on while the camera runs, auto = while the camera runs and it is dark: the exposure is at its limit
with gain 8x or more; off again when it is bright even with the light; at least 20 s between changes),
`cam ir_active low` if the switch turns on with a low level. Also on the `/camera` page (once a pin is set).
For a picture in the dark the camera must see IR: a NoIR camera, or one without the IR filter.

### Video doorbell + door radar (0.105)

A button on a GPIO pin (default GPIO6 = the button of the THT shield, to GND) rings: the camera takes a photo
(saved in `/CAM/<date>/`), it goes to Home Assistant over MQTT, a gong (or a WAV) plays, then the ring event
is sent - after the photo, so a notification shows the new picture. Without network it still rings and the
photo stays on the card. A presence radar or PIR (RCWL-0516, LD2410 OUT, HC-SR501: output high = someone
there) on another pin takes a photo when someone comes to the door (30 s apart), optionally.

```
doorbell on 6               watch the button on GPIO6 (button to GND, internal pull-up)
doorbell sound gong         or off, or /SOUNDS/DINGDONG.WAV
doorbell photo on|off       photo on the card
doorbell mqtt on|off        photo + ring to Home Assistant
doorbell test               ring now
doorbell radar 17           radar output on GPIO17 (doorbell radar off = none)
doorbell radar photo on     radar photo on the card
doorbell radar mqtt on      radar photo to Home Assistant too (default off)
doorbell distance 23 24 100 HC-SR04 (TRIG GPIO23, ECHO GPIO24): someone only nearer than 100 cm
doorbell                    status, last ring / visit
cam send                    one photo to Home Assistant now
```

A radar sees far (and through walls). To photograph only someone right at the door, add an HC-SR04
distance sensor (0.107): `doorbell distance <trig> <echo> [cm]` (`doorbell distance 80` changes only the limit,
`off` removes it). Its ECHO pin gives 5 V - connect it through a divider (e.g. 1 kOhm from ECHO, 2 kOhm to GND,
the middle to the GPIO), VCC to 5 V. With a radar as well, both must agree. `doorbell` shows the distance now.

Settings in `/DOORBELL.TXT`. With MQTT on (`mqtt broker ...`, discovery on), Home Assistant gets by itself:
a **camera** (`<base>/camera`, the last photo, retained), a **Take photo** button (`<base>/set/camera`), a
**Doorbell** event (`<base>/doorbell`, `{"event_type":"ring"}`) and **Someone at the door** (`<base>/radar` ON/OFF).
A notification with the photo on the phone (Home Assistant app), an automation like:

```yaml
alias: Doorbell
triggers:
  - trigger: state
    entity_id: event.rpi3_3456_doorbell     # your entity names (Settings - Devices - the RoboOS board)
actions:
  - action: notify.mobile_app_your_phone
    data:
      title: "Someone is at the door"
      message: "Doorbell"
      data:
        image: /api/camera_proxy/camera.rpi3_3456_camera
```

### MQTT

MQTT is the usual way IoT devices talk to a server (Home Assistant, Node-RED, Mosquitto...).
RoboOS connects to a broker and sends its values; the broker passes commands back.

```
mqtt broker 192.168.1.10      the broker (name or address, port 1883; saved to /MQTT.TXT)
mqtt user robo secret         if the broker wants a login
mqtt                          status
mqtt interval 30              send the values every 30 s (0 = never on its own)
mqtt pub hello world          send roboos/<name>/hello = world
mqtt off                      stop
```

Topics (`<name>` is the board name from `/NAME.TXT`, e.g. `roboos/rpi3-3456/...`):

| Topic | |
|---|---|
| `roboos/<name>/status` | `online` / `offline` (retained; `offline` is the "last will" when the board disappears) |
| `roboos/<name>/state` | JSON: `{"cpu":48.31,"temp":21.53,"hum":40.12,"press":1013.25,"a0":1234,...,"uptime":..,"ip":"..","time":".."}` |
| `roboos/<name>/set/gpio/17` | send `1` / `0` / `on` / `off` / `toggle`: the pin becomes an output |
| `roboos/<name>/gpio/17` | the board answers with the new level (retained) |
| `roboos/<name>/set/publish` | send the state now |
| `roboos/<name>/cmd/...` | free for your programs (BASIC / C read them) |

Home Assistant finds the sensors by itself (MQTT discovery, `mqtt discovery off` turns it
off). From a PC:

```
mosquitto_sub -h 192.168.1.10 -t 'roboos/#' -v
mosquitto_pub -h 192.168.1.10 -t roboos/rpi3-3456/set/gpio/17 -m toggle
```

`/MQTT.TXT` (you can also write it by hand):

```
broker 192.168.1.10
port 1883
user robo
pass secret
topic roboos/garden
interval 60
discovery on
```

With a login (Mosquitto `password_file`, Home Assistant's MQTT add-on user): `mqtt user <name> <password>`;
`mqtt` then shows "user <name>", a wrong login shows "last problem: bad user name or password" /
"not authorized" and is tried again only once a minute. The password is stored in plain text in
`/MQTT.TXT` and travels unencrypted, like everything on port 1883 (QoS 0) - use it on your own network.

In programs: BASIC `SENSOR(n)` (0 CPU, 1 temperature, 2 humidity in 0.01 C / %, 3 pressure in Pa,
4-7 ADC in mV, -999999 = no sensor), `MQTTPUB "topic", items` (like PRINT), `MQTTOK`;
example `/APPS/mqtt.bas`. C (API 10): `sensorRead(n)`, `mqttPublish(topic, text, retain)`,
`mqttReceive(topic, tmax, payload, pmax)` (messages to `cmd/...`), `mqttConnected()`;
example `iotnode` (sends sensor JSON, LED on GPIO23 by `cmd/led` on / off / blink).

### Burglar alarm in the kernel (0.120)

The `alarm` command (and the web page `/alarm`) is a complete burglar alarm that runs in the
background like the doorbell - no program has to run. It reads `/ALARM.TXT` (the demo's format, see
below, plus a few new keys) and is **off** until `alarm on`.

```
alarm shield                 zones + siren for the RoboOS IoT shield (both variants):
                             IN1 GPIO17 door (delay), IN2 GPIO27 window (instant), IN3 GPIO22 PIR (interior,
                             normally open), IN4 GPIO5 tamper (24h); siren = relay K1 / OUT1 GPIO24, LED GPIO23
alarm zone 17 delay nc Front door      add / change a zone: <gpio> <instant|delay|interior|24h> <nc|no> <name>
alarm zone del 22                      remove one
alarm code 1234 5678         change the code (default 1234 - change it!)
alarm on                     on (alarm off only while disarmed)
alarm arm 5678               arm (away): exit time, then armed
alarm home 5678              arm, but 'interior' zones are bypassed (you are at home)
alarm disarm 5678
alarm panic                  alarm now
alarm test                   siren relay + tone for 2 s
alarm set video 10           keys: exit entry siren (seconds), siren_pin armed_pin (GPIO|none), sound on|off,
                             volume, voice <folder>|none, photo on|off, photos 1-10, video <s>, entry_photo on|off, mqtt on|off
alarm                        state, zones (OPEN / ok), last photo + video
alarm log                    the last 48 events (also in /ALARM.LOG on the card, kept over a reboot)
```

Zone types: `instant` alarm at once (window), `delay` starts the entry time (the door you come in
by), `interior` a PIR inside: waits during the exit / entry time and is off when armed `home`,
`24h` always (tamper, panic button; it fires on opening). Contacts go between the GPIO and GND
(internal pull-up); on the IoT shield the optocoupler inputs behave the same way (input powered =
closed). `nc` = normally closed (door contact), `no` = normally open (PIR relay).

What happens on an alarm: siren relay + a rising / falling tone on the jack, voice files from
`/ALARM` (as the demo), **photos** (`photos`, 1 s apart, in `/CAM/<date>/`; the first one also to
Home Assistant), then a **video** of `video` seconds (MJPEG AVI in `/CAM/<date>/`), an MQTT event.
With `entry_photo on` a photo is also taken when the entry time starts (who came in). After `siren`
seconds the siren stops and the alarm re-arms by itself (zones left open are skipped until they
close); the cause stays as "alarm memory" until the next arming. The armed state is kept in
`/ALARM.ST`: after a power cut the board comes back **armed**. 5 wrong codes block the code for 60 s.
The code typed in the shell is not kept in the history; for real security turn on `users console on`
(the keyboard then asks for a login) - otherwise whoever has the keyboard can change the settings while
disarmed.

**Web page** `http(s)://<board>/alarm` (token or login like `/camera`): the state in big letters,
code + Arm (away) / Arm (home) / Disarm / Panic / Test siren, zones live, the last alarm photo and video,
the event log, and all settings with a zone editor (button "IoT shield" fills in the shield's inputs).
JSON API: `GET /api/alarm`, `POST /api/alarm` `{"cmd":"arm","mode":"home","code":"1234"}` (cmd arm,
disarm, panic, test) or `{"config":"<lines of /ALARM.TXT>"}` (the code is not changed this way).

**Home Assistant** (MQTT with discovery on): an **Alarm panel** (`alarm_control_panel`: arm home / away,
disarm, trigger - HA asks for the code and sends it to the board, the board checks it), one
**binary sensor per zone** (door / window / motion / tamper), an **Alarm event** (event types `alarm`,
`entry`, with the zone and the photo name), the photo in the existing **Camera** entity. Topics:
`<base>/alarm` (disarmed / arming / armed_away / armed_home / pending / triggered), `<base>/alarm/attr`
(JSON: cause, alarm memory, open zones, last photo), `<base>/alarm/zone/<gpio>` ON/OFF,
`<base>/alarm/event`, commands to `<base>/set/alarm` (`{"action":"DISARM","code":"1234"}` or `DISARM 1234`).

Writing a long video to the card takes a few seconds (the siren relay stays on meanwhile, the jack tone
pauses); disarming from the web / Home Assistant works at once even then. The demo program below still
exists (`run alarm`) - do not run both, they use the same pins.

**Keypad and card readers (0.121)** - a code / card at the door (one keypad and one reader at a time):

```
alarm keypad gpio                        4x4 membrane keypad straight on the header (see the table below)
alarm keypad gpio 13 19 26 21 20 7 18    other pins: 4 row pins, then 3 (or 4) column pins
alarm keypad i2c 0x20                    4x4 keypad on a PCF8574 board (P0-P3 rows, P4-P7 columns; 'swap' = reversed)
alarm rfid rc522                         RC522 13.56 MHz reader on SPI0: SDA/CS GPIO8, MISO 9, MOSI 10, SCK 11,
                                         3.3 V (NOT 5 V), RST to 3.3 V (or 'alarm rfid rc522 <gpio>')
alarm rfid wiegand 20 21                 Wiegand reader / outdoor keypad (D0, D1): 125 kHz EM cards, 26 / 34 bit;
                                         its D0/D1 are 5 V - use a divider (e.g. 1k / 2k) or a level shifter
alarm card learn                         then show a new card within 30 s (saved by itself)
alarm card add 1a2b3c4d Maria            / alarm card del Maria / alarm card (list)
alarm keypad off, alarm rfid off
```

**Simplest: push the connector straight onto the header (0.130, `alarm keypad plug`).** The keypad's 8-pin
connector goes onto pins **23 to 37 of the inner row** (odd numbers, the row nearer the board's middle), wire 1
(the left one, looking at the front with the cable down) on pin 23. Nothing to solder or wire. Pin 25 under wire 2
is GND - RoboOS reads that wire the other way round, so all 16 keys still work, and as the lines are only ever
pulled LOW the GND pin is safe. Pushed on the other way round (wire 1 on pin 37)? `alarm keypad plug reversed`.
Not with the IoT shield (it covers the header) and not with the RC522 (pin 23 = GPIO11 = SPI clock).

| header pin | 23 | 25 | 27 | 29 | 31 | 33 | 35 | 37 |
|---|---|---|---|---|---|---|---|---|
| keypad wire | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
| GPIO | 11 | GND | 0 | 5 | 6 | 13 | 19 | 26 |

**With the IoT shield (0.131, `alarm keypad shield`):** the shield covers GPIO 4 5 6 12 16 17 22-25 27, so the
keypad goes on the 7 free GPIOs with jumper wires, and wire 8 on any GND pin (the GND trick again: all 16 keys work,
SPI stays free for an RC522). The header pins must be reachable (a stacking header with long pins on the shield):

| keypad wire | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| GPIO | 13 | 19 | 26 | 21 | 20 | 7 | 18 | GND |
| header pin | 33 | 35 | 37 | 40 | 38 | 26 | 12 | 39 (or any GND) |

`alarm keypad test` (0.132) shows live which key is seen and which lines are low with nothing pressed - a line that is `L` without a key sits on a GND pin (the connector is shifted). For `alarm keypad plug` the connector must be on the inner row with exactly one free pin (39) between it and the USB end of the header.

When a keypad / card reader takes the pin of an alarm zone (e.g. `alarm keypad plug` takes GPIO 5 = IN4 of `alarm shield`)
or of the doorbell button (GPIO 6), RoboOS prints a yellow `!!` line, shows the zone as `PIN TAKEN` (also on the web
page) and does not watch it - move the zone or use the other keypad setting.

4x4 membrane keypad straight on the Pi header (0.129, `alarm keypad gpio` with no pins; no resistors needed -
internal pull-ups, one row driven at a time). The keypad's 8 wires, looking at the front with the cable down, from
the left: 1-4 = rows (1 2 3 A / 4 5 6 B / 7 8 9 C / * 0 # D), 5-8 = columns:

| keypad wire | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| GPIO | 5 | 6 | 13 | 19 | 12 | 16 | 20 | 21 |
| header pin | 29 | 31 | 33 | 35 | 32 | 36 | 38 | 40 |

Not together with the IoT shield (it uses GPIO 5, 6, 12, 16) - with the shield: `alarm keypad gpio 13 19 26 21 20 7 18 8`
(header pins 33 35 37 40 38 26 12 24; GPIO8 is SPI, so not with the RC522) or the I2C keypad. Keys mixed up (1 shows
as A)? The keypad has its wires the other way round: give the pins in reverse order. The alarm must be on for the
keypad to be read; `alarm` shows `keys N, last X`.

On the keypad: code + `#` = arm (when disarmed) or disarm, code + `A` = arm away, `B` = arm home,
`D` = disarm, `*` = clear (10 s without a key clears too); every key beeps on the jack. A known card arms
(disarmed) or disarms; an unknown card counts as a wrong code. The display (`display alarm`, also in the KEY1
cycle of the OLED HAT) shows the state, the seconds left, the code as stars and the last answer. The web page
has the same lines in "Keypad, card reader, cards" and a "Learn a new card" button (card numbers are shown only
to admins).

Free pins next to the IoT shield (it uses 17 27 22 5 in, 24 25 12 16 out, 23 LED, 6 button, 2/3 I2C, 4 1-Wire):
7, 13, 18, 19, 20, 21, 26 (+ 8-11 for SPI, 14/15 = UART). A 4x3 keypad on 13 19 26 21 / 20 7 18 takes all of them
(on the Zero 2 W GPIO18/19 is the sound) - with a Wiegand reader take the I2C keypad. The Waveshare OLED HAT also
uses SPI0 + GPIO24/25 - not together with the RC522 / the shield.

**Wiegand on a long cable (to ~100-150 m)**: Wiegand carries far (shielded cable, 22 AWG / 0.34 mm2), but a long
cable - outdoors especially - brings static and surges from storms; a GPIO pin takes 3.3 V at most and a divider
alone does not protect it. Separate the reader with an optocoupler on each line (like the inputs of the IoT shield):

```
 reader side (outside, own 12 V supply)               Pi side
 +12V --[ 2k2 ]--+--> LED of a PC817 --+          3V3 --[ 4k7 ]--+-- GPIO20 (D0)
                 |                      |                         |
             TVS 15 V                   D0 <-- from the reader  collector of the PC817
             (SMBJ15A)                                            emitter -- GND of the Pi
                 |
 GND of the reader
```

- The reader pulls D0 low -> the LED lights -> GPIO20 goes low: the same polarity as a direct connection, nothing
  to change in RoboOS. The same once more for D1 (GPIO21).
- Do **not** join the reader's GND with the Pi's GND: the reader has its own 12 V adapter, a surge on the cable
  stops at the optocoupler (~5 kV isolation). The TVS diode takes the spikes so they do not kill the LED.
- Wiegand pulses are 50-100 us; a PC817 switches on in a few us and RoboOS catches the falling edge, so it is fast
  enough. For a reader with pulses shorter than ~20 us use a fast optocoupler (6N137).
- Shielded cable with the shield grounded at ONE end only (the reader / its supply); power the reader from its
  own 12 V adapter, not from the Pi; outdoors also a PTC fuse + TVS on the 12 V line.
- For comparison: a GPIO matrix keypad ~2-3 m of cable, I2C ~1 m (tens of metres with a P82B715 extender), the
  RC522 on SPI only ~10-20 cm (keep it next to the Pi). With the Pi inside and the reader at the gate, an outdoor
  Wiegand keypad + 125 kHz card reader in one (12 V, IP65) sends keys AND cards over the same two wires.

**OLED module with a rotary knob (0.145, `alarm knob`)**: the small boards with a 0.96" / 1.3" I2C OLED, an EC11
knob and two buttons (pins CONFIRM, OLED_SDA, OLED_SCL, ENCODER_PUSH, ENCODER_TRA, ENCODER_TRB, BAK, GND, 3V3-5V)
work as the alarm keypad. Turn = choose a digit (the display shows `Code: **[5]`), push the knob = add that digit,
BAK = delete the last one, CONFIRM = send the code (like `#`: arm when disarmed, else disarm). Works on a terminal
too (`alarm remote ...`). Wiring (pull-ups are internal; the OLED is found like any I2C display, mode `alarm`):

| module pin | Pi header |
|---|---|
| 3V3-5V | pin 1 (3.3 V) |
| GND | pin 39 (or any GND) |
| OLED_SDA | pin 3 (GPIO2) |
| OLED_SCL | pin 5 (GPIO3) |
| ENCODER_TRA | pin 31 (GPIO6) |
| ENCODER_TRB | pin 33 (GPIO13) |
| ENCODER_PUSH | pin 35 (GPIO19) |
| CONFIRM | pin 37 (GPIO26) |
| BAK | pin 40 (GPIO21) |

Other pins: `alarm knob <tra> <trb> <push> <confirm> <back>` (GPIO numbers); the wrong direction: add `reverse`;
`alarm knob off`. These pins are not used by the IoT shield (only GPIO6 = the doorbell button, if you use it);
clashes with zones, the siren or the LED are shown with `!!`.

**Door intercom (0.148, `intercom`, page `/door`)**: voice at the door through the web page - you hear the door
and, while you hold **Hold to talk**, the door hears you (half duplex = no echo). Two cheap I2S modules share the
clock lines:

| module | pin | Pi header |
|---|---|---|
| INMP441 microphone | SCK / WS / SD | pin 12 (GPIO18) / pin 35 (GPIO19) / pin 38 (GPIO20) |
| | L/R, GND, VDD | GND (= left channel), GND, 3.3 V (pin 1) |
| MAX98357A amplifier | BCLK / LRC / DIN | pin 12 (GPIO18) / pin 35 (GPIO19) / pin 40 (GPIO21) |
| | GND, VIN | GND, 5 V (pin 2); speaker 4-8 ohm, 3 W on its + / - |

```
intercom on                  the page /door + the card on the home page (saved in /INTERCOM.TXT)
intercom beep                1 s of 1 kHz on the speaker (wiring test)
intercom mic                 live microphone level (wiring test; speak into it)
intercom volume 80           speaker volume 0-100 %
intercom gain 4              microphone gain x1-64
intercom                     state, wiring, levels, conflicts
```

On the phone open `https://<board>/door` (the browser lets a page use the microphone only over **https** -
`http tls on`, accept the certificate warning), give the API token once, press **Listen**; hold **Hold to talk**
(or the space bar) to speak. The camera picture is shown above when there is a camera. Sound: 16 kHz G.711 u-law,
~0.3-0.5 s delay. The I2S pins are used only while the page listens (8 s after the last request the board lets
them go), so `sound out i2s` and the intercom cannot play at the same time. GPIO19-21 are also used by the knob
(0.145), the keypad plug / shield layouts and the Waveshare HAT keys - `intercom` warns. Outside the home: through
a VPN (e.g. WireGuard on the router).

**Monitoring centre / PCO (0.146, `pco`)**: the alarm reports its events to an alarm receiving centre with the
standard IP protocol **SIA DC-09** (what PCO software and receivers like Sur-Gard, Osborne-Hoffman, Manitou, Patriot
or the Ajax / Jablotron receivers accept): Contact ID inside DC-09 (`ADM-CID`, default) or `SIA-DCS`, over TCP or
UDP, optionally AES-128/192/256 encrypted. Every message waits for the receiver's ACK; without one it is sent again
(3 tries, then the backup receiver, then every 30 s) - nothing is lost while the network is down (queue of 32). A NAK
(the receiver's clock differs) makes the board take the receiver's time and send again.

```
pco host pco.example.sk 5001 tcp     the receiver (address, port, tcp|udp) - from your PCO
pco backup 10.1.2.3 5001             a second receiver (or 'pco backup off')
pco account 1A2B                     your object / account number (3-16 hex digits)
pco line 1   /  pco receiver 12      line (L) and receiver (R) numbers if the PCO gives them
pco format cid|sia                   Contact ID (ADM-CID) or SIA-DCS
pco key <32/48/64 hex> | pco key text <16/24/32 chars> | pco key off     AES key
pco heartbeat 120                    supervision (NULL message) every 120 s, 0 = off
pco test 24                          a test report (E602) every 24 h; 'pco test' = one now
pco on | pco off                     settings in /PCO.TXT
pco send E130 3                      any Contact ID event by hand (for the test with the PCO)
pco                                  state, the last 8 reports with ACK / NAK / no answer
```

Events (Contact ID, partition 01; zone = the zone's place in the list, `alarm` shows them in order; user 001 = the
code, 011.. = the cards): E130 burglary (zone), E133 24h zone, E120 panic, R401 armed / R441 armed home (user),
E401 / E441 disarmed (user), E406 alarm cancelled, R130 / R120 restored when the siren time is over, E461 code
blocked (5 wrong), E602 periodic test, E305 power up. In SIA-DCS the same as BA / PA / CL / NL / OP / BC / BR / PR /
JA / RP / RR. The board needs the time (NTP, DCF77, RTC or `time set`) - DC-09 messages carry it, and encrypted
ones must. Test without a PCO: `tools/dc09_receiver.py <tcp port> <udp port> log.json [key]` (prints nothing, writes
every message it got + whether CRC / length / format / time are right).

**GSM / LTE modem for the alarm (0.144)**: SMS and calls over the mobile network - they still go out when the
internet is down. Modules: SIM800L (2G, being switched off in Europe) or SIMCom A7670E / SIM7600E (LTE), the same
standard AT commands. Wiring (the serial console moves away from these pins while the modem is on):

| Modem | Pi |
|---|---|
| RXD | pin 8 (GPIO14) - for the SIM800L (2.8 V logic) through a divider, e.g. 4.7 k in series + 10 k to GND |
| TXD | pin 10 (GPIO15) |
| GND | any GND pin |
| VCC | its OWN supply: SIM800L 3.7-4.2 V, peaks of 2 A (a Li-ion cell or a buck converter), never the Pi's 3.3 V / 5 V |

```
modem number add +421900123456     who gets the SMS / calls and may send commands (up to 5)
modem on                           probe + set up (9600 baud; an LTE module is switched from 115200 by itself)
modem                              status: network, signal in dBm, SIM, SMS counts
modem sms +421900123456 test       a test SMS        modem at AT+CSQ     any AT command
modem set sms_alarm|sms_arm|sms_disarm|call|commands on|off
```
On an alarm every number gets an SMS ("<board> 14:03: ALARM: door") and is called in turn (25 s each, a disarm
stops the calls); arming / disarming send an SMS too. SMS commands from the listed numbers only: `ARM 1234`,
`HOME 1234`, `DISARM 1234`, `STATUS` - the answer comes back by SMS. Old messages on the SIM are deleted when the
modem starts (a stale DISARM never runs). `alarm` shows the modem's state. Settings in /MODEM.TXT.

**Keypad terminal: a second Pi as the keypad + display (0.134).** The same kernel on another board (e.g. a Zero 2 W
by the door) becomes a terminal of the alarm board:

```
alarm remote <master name>     on the terminal (its own alarm stays off); 'alarm remote off' = normal board again
http token <secret>            the SAME token on the master and every terminal (it signs + encrypts the messages)
```

The terminal's keypad / card reader (`alarm keypad ...`, `alarm rfid ...` set up on the terminal) go to the master
as RoboOS messages (raw Ethernet / WiFi frames with an acknowledgement, same LAN, names from `net name`), the master
decides (codes, cards, lockout) and sends back what its display shows; the terminal shows it on its display, beeps
in the exit / entry time and sounds the siren tone in an alarm. Nothing to set on the master: a terminal appears in
`alarm` by itself (`terminal door, seen 3 s ago`). On the terminal `alarm arm|home|disarm <code>` and
`alarm key 1234#` also go to the master. With the token every message carries an HMAC-SHA256 and the text is
encrypted, a copied (replayed) frame is refused; without a token the code travels in clear on the LAN (`alarm` warns).
`no master` on the terminal's display = nothing from the master for 15 s.
**Cable + WiFi (0.140)**: messages go over every link that is up, so the boards can also be joined by a cable straight from one to the other (no router; auto-MDIX, any patch cable) while both stay on WiFi; run `net ip wifi` on both so the web, MQTT and time keep using WiFi. If one path fails, the other still carries the keypad. On the web page `/alarm` of the terminal
(0.135): write `remote <master name>` into the keypad / card lines box and Save (the board name itself is set with `net name`).

### IoT shield (hardware design)

`hardware/iot-shield/` has a KiCad design of a board that sits on the 40-pin header:
power from 12-24 V, 4 isolated inputs, 4 MOSFET outputs, ADS1115, BME280, DS3231 clock,
1-Wire. See its README - **routed, Gerbers included, never manufactured or tested**.

## Sound

No Raspberry Pi has a sound chip: the 3.5 mm jack is driven by the two PWM channels of the
processor through a filter. RoboOS uses them for square-wave tones; the hardware makes the
wave by itself, the program keeps running. Plug headphones or an active speaker into the
jack. Boards without a jack (Zero 2 W, CM3) use GPIO18 and GPIO19, where a piezo buzzer
(or a small amplifier with a speaker) can be connected; `sound out gpio` does the same on
any board.

- Shell: `beep [Hz] [ms]`, `play <notes>`, `sound` (output and volume),
  `sound volume 0-100`, `sound out jack|gpio`, `sound off`. Printing the BEL character
  (ASCII 7) beeps too.
- BASIC: `SOUND 440, 500` (Hz, ms; with ms 0 the tone keeps sounding until `SOUND 0, 0`),
  `PLAY "T120 L4 C D E F G2"`, `VOLUME 60`.
- C: `tone(hz, ms)` starts a tone and returns (ms 0 = until `noTone()`), `playNotes("...")`
  waits until the end, `soundVolume(percent)`.
- Notes like QBasic's PLAY: `C D E F G A B` with `#` or `+` (sharp) and `-` (flat), a number
  after a note = its length (`C8` = eighth), `.` = dotted, `P` or `R` = pause, `L4` = default
  length, `O4` = octave (0-7, `<` `>` down / up), `T120` = tempo, `MS` / `MN` / `ML` =
  staccato / normal / legato.

The volume changes the pulse width, so it also changes the sound a little.

**WAV files** play on the jack (or GPIO18/19) too: `playwav <file>` (shell), `PLAYWAV "file"`
(BASIC), `playWav(path)` (C) - PCM, 8 or 16 bit, mono or stereo, 8000-48000 Hz, e.g.
`playwav /APPS/chime.wav`; Ctrl+C stops. For this the PWM runs at 250 MHz and the samples
reach it by DMA, so the sound keeps playing while RoboOS does other things; the jack gives
about 12 bits of quality at 44.1 kHz (radio-like, with some hiss). Tones go back to the
simple square wave afterwards.

### I2S DAC module (better sound, WAV files)

For real sound connect a small I2S DAC board (PCM5102A, UDA1334A; or a MAX98357A, which is
a DAC with an amplifier for a speaker) and switch to it with `sound out i2s` (saved in
`/SOUND.TXT`, so it stays after a reboot; `sound out jack` switches back).

| DAC module | Raspberry Pi |
|------------|--------------|
| BCK (BCLK) | GPIO18 (pin 12) |
| LCK (LRCK, WS) | GPIO19 (pin 35) |
| DIN (DATA) | GPIO21 (pin 40) |
| VIN / GND | 3.3 V or 5 V (see the module) / GND |
| SCK, FMT (PCM5102A) | GND (the module makes its own clock; I2S format) |

- Tones (`beep`, `play`, `SOUND`, `tone()`) are then sine waves with a real volume.
- WAV files (same commands as above) play in full 16-bit quality.
- The samples go to the DAC by DMA, so the sound keeps playing while RoboOS does other
  things. `sound` (with I2S, or after a WAV) shows how many pieces have been played and whether the DMA ever ran
  empty ("restarts" - then the sound had a short gap).

## USB keyboard

You can plug the keyboard in before power-on or while the system is running (hot-plug:
hub ports are checked every 0.7 s). At boot you will see:

    [ OK ] USB keyboard: <name>

- **US layout**. Shift, Caps Lock, Num Lock, Ctrl+C and Ctrl+L work; the Caps/Num Lock LEDs follow.
- Numeric keypad: digits and `/ * - + .` with Num Lock on (default after boot);
  with Num Lock off, 8/2 = history up/down, 4/6 = cursor left/right, 7/1 = Home/End and `.` = Delete.
- Up/Down = history (last 30, saved in /HISTORY.TXT; `history`, `!N`), Left/Right/Home/End = cursor, Backspace, Delete, Tab, autorepeat when a key is held.
- Keyboards with the HID *boot protocol* are supported (practically all common ones,
  including keyboards with a built-in hub such as Apple's, and gaming keyboards).

If the keyboard is not found, run this on the UART console:

    usb debug on
    usb reset

The output shows where enumeration failed.

## UART console (optional, USB-TTL adapter, 3.3 V!)

| Adapter | RPi pin |
|---------|----------|
| GND     | pin 6 (GND) |
| RX      | pin 8 (GPIO14 / TXD) |
| TX      | pin 10 (GPIO15 / RXD) |

Do **not** connect VCC, the RPi has its own power supply. The adapter must be 3.3 V,
5 V logic will damage the pins.

Terminal settings: **115200 baud, 8N1, no flow control**:
- Windows: PuTTY (Serial, COMx), or `plink -serial COM5 -sercfg 115200,8,n,1,N`
- Linux/macOS: `screen /dev/ttyUSB0 115200` or `picocom -b 115200 /dev/ttyUSB0`

After power-on the logo and the `robo@rpi3:~$` prompt (host name depends on the board) appear on HDMI and in the terminal.
If you connect after boot, press Enter.

## Commands

| Command | Description |
|---------|-------------|
| `help` | list of commands |
| `info` | model, RAM, serial number, MAC, clocks, temperature, EL |
| `temp` | CPU temperature |
| `uptime` | time since boot |
| `echo <text>` | print text |
| `led on\|off\|blink [n]` | green ACT LED |
| `gpio` | overview of GPIO0–27 (function + level) |
| `gpio <pin> in\|out\|hi\|lo\|toggle\|read` | control a pin |
| `mem <addr> [length]` | memory hexdump (e.g. `mem 0x80000 64`) |
| `usb` | USB device tree (address, VID:PID, speed, name, hub port status) |
| `usb reset` | reinitialize USB and rescan devices |
| `usb test` | 8 s keyboard test (transfer statistics, raw reports) |
| `usb debug on\|off` | verbose USB and keyboard report logging |
| `net` | network status (name, chip, MAC, link, counters) |
| `net speed 100\|1000` | Pi 3B+: limit the link to 100 Mbit / allow 1000 |
| `net who` | list the RoboOS boards on the network |
| `net send <name\|all> <text>` | send a message |
| `net name [new]` | show / change this board's name |
| `net test [s]`, `net loop on\|off` | frame test, loopback |
| `ip` | IP address, `ip dhcp`, `ip set <address> [mask] [gateway]` |
| `ping <address\|name> [n]` | ping another computer |
| `http on\|off` | small web page with board info (saved) |
| `http token [<secret>\|off]` | JSON API token (Bearer), see the JSON API section |
| `http tls on\|off\|new` | HTTPS on port 443 (the API then only there), new certificate |
| `wifi [scan\|join <net> [pw]\|band 2\|5\|auto\|leave\|forget]` | WiFi: networks, join (saved), status |
| `update [check\|get [-k] [force]\|url <addr>\|restore]` | kernel updates: download one, or see what came from a PC; the backup back |
| `bt [on\|scan [s]\|sensors\|pair [mac]\|forget\|off\|auto on\|off]` | Bluetooth: devices, thermometers, phone, keyboard (only when asked) |
| `sensors [scan]`, `i2c scan\|read\|write` | I2C sensors (BME280, ADS1115, DS3231) |
| `ai <question>\|@cmd ..\|key\|model` | ask Claude (own Anthropic API key) |
| `radio [MHz\|up\|down\|scan\|vol n\|off]` | FM radio (RDA5807M module on I2C) |
| `motion [watch\|zero\|threshold mg\|alarm on\|off]` | tilt, acceleration, shakes (MPU-6050 / GY-521) |
| `modem [on\|off\|number\|pin\|set\|sms\|call\|at]` | GSM/LTE modem on pins 8/10: alarm SMS + calls, SMS commands |
| `alarm [on\|off\|arm\|home\|disarm <code>\|zone\|set\|shield\|keypad\|rfid\|card\|remote\|log\|test]` | burglar alarm in the kernel: zones, siren, keypad + RFID / Wiegand cards, photos + video, web page /alarm, Home Assistant |
| `cam [live\|snap\|bars\|reg a [v]\|off]` | camera on the CSI ribbon: find the sensor, live picture on HDMI |
| `thermal [once\|range lo hi\|person C]` | 8x8 thermal camera (AMG8833), live heat map |
| `voice [add id cmd\|del id\|volume\|mute\|wake]` | offline voice commands (DFRobot DF2301Q) |
| `dcf [on\|off\|pin n\|invert\|pull\|pon]` | DCF77 radio clock receiver on a GPIO |
| `display [scan\|text n ..\|clear\|status on\|off\|size\|type\|addr\|hat\|keys]` | I2C OLED / LCD display, Waveshare SPI OLED HAT |
| `mqtt [broker\|user\|pub\|interval\|off]` | MQTT client (IoT) |
| `users [add\|passwd\|del\|console on\|off]`, `logout` | logins for the web page + API (optional) |
| `time [sync\|zone\|server\|set]` | date and time from NTP, time zone, set by hand |
| `ls [path]` | list a directory on the SD card (default: the current one) |
| `cd [dir]` | change directory (`..` = up, `/` = top, no argument = top) |
| `pwd` | show the current directory |
| `view [picture\|dir]` | show pictures full screen (JPEG, PNG, BMP, GIF) |
| `video <file.mpg>` | play a video (MPEG-1 + MP2) with sound |
| `mkdir <dir>` | make a directory |
| `rm [-f] <file\|dir>` | delete a file or an empty directory (`-f` for boot files) |
| `cat <file>` | print a text file (`"quotes"` for names with spaces) |
| `edit <file>` | full-screen text editor |
| `run <program>` | run a `.bin` or `.bas` program |
| `basic [file]` | BASIC interpreter |
| `beep [Hz] [ms]` | a tone on the audio jack |
| `play <notes>` | play music, e.g. `play T120 L4 C D E F G2` |
| `sound [volume n\|out jack\|gpio\|i2s\|off]` | sound output and volume |
| `sound test` | no sound? three tests in a row (a PWM tone, a sine written by the CPU, a sine by DMA) with the registers on the screen - tell which one you heard |
| `playwav <file>` | play a WAV file (jack, GPIO18/19 or I2S DAC) |
| `sd [init [sdhost\|emmc]]` | SD card info (and the controller) / re-initialize after swapping the card |
| `screen [WxH\|auto]` | screen resolution (saved in `/SCREEN.TXT`) |
| `clear` | clear the screen (also Ctrl+L) |
| `reboot` | reboot via watchdog |
| `halt` | halt the system |

Line editor (UART and keyboard, in the shell and in BASIC): **Left/Right**, **Home/End**
(or Ctrl+A / Ctrl+E) move the cursor, so you can insert and delete anywhere in the line.
**Tab** completes a command, and after it a file or directory name (names with spaces get
"quotes"; with several matches it lists them), **Up/Down** browse
history, **Ctrl+C** cancels the line. An invalid memory access (e.g. `mem 0xdeadbeef00`)
raises an exception; it is printed and the system returns to the shell.

## How it works

The Raspberry Pi 3 has four processor cores. RoboOS uses all of them:

- **core 0**: the shell, BASIC and programs, the SD card, the screen and the sound;
- **core 1**: all of USB (keyboard, hot-plug, the Ethernet chip) and the network
  (TCP/IP, RoboOS messages) - all the time, whatever core 0 is doing;
- **cores 2 and 3**: workers - core 0 hands them a piece of work (now: converting video
  frames) and they sleep (`wfe`) when there is nothing to do.

`info` shows how the cores are used. If core 1 does not start, core 0 does everything
itself, as in older versions (the boot line says so); without workers the video is
converted on core 0.

| File | What it does |
|------|--------------|
| `src/boot.S` | EL2→EL1 switch, stacks, exception vectors, BSS; entry for cores 1-3 |
| `src/smp.c` | wakes core 1 (USB + network) and the workers 2-3 (`smp_run` / `smp_wait`), locks shared by the cores |
| `src/mmu.c` | identity mapping with 2 MB blocks, enables D/I caches |
| `src/mbox.c` | mailbox to the GPU firmware (board info, clocks, temperature, framebuffer) |
| `src/uart.c` | PL011 UART0 on GPIO14/15 |
| `src/fb.c` | HDMI framebuffer (1024×768 or `screen`), two pages for page flips, text console with an 8×16 font |
| `src/console.c` | output to UART + HDMI, `kprintf`, colors, input |
| `src/usb.c` | DWC2 USB host driver: init, root port, DMA transfers, split transactions, enumeration, hubs, hot-plug |
| `src/eth.c` | Ethernet: LAN9514 (3B) and LAN7515 (3B+) over USB bulk endpoints, raw frames |
| `src/net.c` | messages between RoboOS boards (names, who, send with confirmation, inbox) |
| `src/ip.c` | TCP/IP: lwIP port, network interface, DHCP, `ip`, `ping`, `http`, sockets for programs |
| `src/webapi.c` | JSON API behind `http`: Bearer token, GPIO, status, reboot |
| `lwip/` | lwIP 2.2.1 TCP/IP stack (unmodified, BSD license) |
| `src/kbd.c` | HID keyboard: boot protocol, scan code translation, autorepeat |
| `src/sd.c` | SD card: chooses the controller; the Arasan SDHCI driver (GPIO48-53 ALT3), read + write |
| `src/netdev.c` | Ethernet or WiFi under the IP stack (Ethernet when its cable is in) |
| `src/wifi.c` | WiFi chip over SDIO (Arasan, GPIO34-39): firmware, commands, scan (port of 9front ether4330.c, MIT) |
| `src/sdhost.c` | SD card on the SDHOST controller (GPIO48-53 ALT0), the default since 0.28 |
| `src/fat.c` | FAT16/FAT32: long names, read, write (8.3 names), mkdir, delete, both FAT copies |
| `src/app.c` | program loader, Arduino-style API, Ctrl+C, autostart from `/RUN` |
| `src/basic.c` | BASIC interpreter |
| `src/ctx.S` | setjmp/longjmp to abort programs |
| `src/board.c` | board detection: name, ACT LED, USB topology |
| `src/lineedit.c` | line editor: cursor keys, history, Tab hook (shell + BASIC) |
| `src/edit.c` | full-screen text editor |
| `src/shell.c` | commands, Tab completion |
| `src/path.c` | current directory, relative paths, `.` and `..` |
| `src/sound.c` | tones from the PWM (audio jack or GPIO18/19), music strings |
| `src/i2s.c` | sound by DMA: I2S DAC (PCM peripheral) or the PWM, sine tones, WAV files |
| `src/image.c` | pictures: decoding (stb_image in `src/stbimg.c`), scaling, `view`, boot picture |
| `src/video.c` | video player: pl_mpeg (`third_party/pl_mpeg.h`) reading from the card, colour conversion on cores 2-3, sound stream |
| `src/heap.c` | memory for big temporary work (decoding pictures) |
| `src/font.c` | font generated from DejaVu Sans Mono (`tools/mkfont.py`) |

### How USB works

On the RPi3 the keyboard is not connected directly; the path looks like this:

    DWC2 controller (in the SoC) -> LAN9514 hub (HS 480 Mbit) -> keyboard (LS 1.5 / FS 12 Mbit)

1. **Controller init**: power on via mailbox, reset, host mode, DMA.
2. **Root port**: port reset, speed detection.
3. **Enumeration**: descriptor, `SET_ADDRESS`, configuration; recursively through hubs.
4. **Hub**: port power, reset, speed of the device on the port.
5. **Split transactions**: a slow device behind a HS hub is addressed with
   *start-split* + *complete-split* via the hub's Transaction Translator.
6. **Keyboard**: HID report descriptor, `SET_PROTOCOL(boot)`, `SET_IDLE`, then polling
   every 10 ms. Until the keyboard sends its first key press, interrupt IN and
   `GET_REPORT` alternate; the method that works gets locked in (`usb` shows the mode).
7. **Periodic split transactions**: start-split in microframe N, complete-split
   in N+2…N+4 (USB 2.0, ch. 11.18), otherwise the TT drops the data.
8. **Powering hub ports one at a time**: the hub inside an Apple keyboard dropped off
   the bus when all ports were powered at once (inrush current).

The driver runs without interrupts (polling, on core 1) and uses one DMA channel (and a
second one for network adapters that keep a transfer waiting). DMA buffers live
in a 2 MB block mapped non-cacheable.

Boards without an onboard hub (Zero 2 W, 3A+) have the keyboard right on the root port;
then no split transactions are needed, and the root port is polled for hot-plug as well.

## Building from source

Linux / WSL with clang + lld:
```
make            # builds kernel8.img
make apps       # builds the SDK examples into sd/APPS
make run        # runs in QEMU (qemu-system-aarch64 -M raspi3b)
```
Or with the ARM GNU toolchain (also on Windows):
```
make TOOLCHAIN=gcc CROSS=aarch64-none-elf-
```

Testing in QEMU with an HDMI window, a USB keyboard and an SD card image:
```
make sd.img
qemu-system-aarch64 -M raspi3b -kernel kernel8.img -serial stdio -device usb-kbd \
    -drive file=sd.img,if=sd,format=raw
```
With a network (QEMU emulates a USB network adapter, DHCP gives 10.0.2.15):
```
qemu-system-aarch64 -M raspi3b -kernel kernel8.img -serial stdio -device usb-kbd \
    -drive file=sd.img,if=sd,format=raw \
    -device usb-net,netdev=n0 -netdev user,id=n0,hostfwd=tcp:127.0.0.1:8080-:80
```
(after `http on` the board's page is at http://127.0.0.1:8080/ on the PC).

## Ideas for later
- other keyboard layouts (a font with accented characters too)
- interrupts + timer → multitasking
- I2C / SPI / PWM (servos, LED dimming) in the API and in BASIC
- sound: MP3, mixing tones with WAV
- editor: search, go to line, save as
- a telnet-like remote shell, IPv6
- cores 2 and 3 for programs (run a function on another core)
- video: a playlist / folder mode, a time bar

## Credits

- The firmware (`bootcode.bin`, `start.elf`, `fixup.dat`) is by Raspberry Pi Ltd,
  license in `sd/LICENCE.broadcom`.
- The console font is rendered from DejaVu Sans Mono (Bitstream Vera license).
- The SD card init sequence follows the well-known bare-metal tutorials
  (bztsrc/raspi3-tutorial); the periodic split scheduling follows Circle/USPi.
- TCP/IP: lwIP by Adam Dunkels and the lwIP developers (BSD license, `lwip/COPYING`).
- Pictures: stb_image by Sean Barrett (public domain / MIT, `third_party/stb_image.h`).
- TLS: BearSSL by Thomas Pornin (MIT, `third_party/bearssl`).
- WiFi: based on the Plan 9 / 9front `ether4330.c` driver by Richard Miller (MIT).
- Video: pl_mpeg by Dominic Szablewski (MIT, `third_party/pl_mpeg.h`).
- The LAN7515 (Pi 3B+) register layout follows the Linux `lan78xx` driver.
