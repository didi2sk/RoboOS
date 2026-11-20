# RoboOS

**English** | [Slovensky](README.sk.md)

![RoboOS boot screen](docs/screenshot.png)

A mini operating system for the Raspberry Pi that runs directly on the hardware, no Linux.
You control it with a **USB keyboard** or over a serial line (UART). Output goes to
HDMI and UART at the same time, and both inputs work together.

Current version: **0.45**

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
- Up/Down = history, Left/Right/Home/End = cursor, Backspace, Delete, Tab, autorepeat when a key is held.
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
| `bt [on\|scan [s]\|sensors\|pair [mac]\|forget\|off\|auto on\|off]` | Bluetooth: devices, thermometers, phone, keyboard (only when asked) |
| `sensors [scan]`, `i2c scan\|read\|write` | I2C sensors (BME280, ADS1115, DS3231) |
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
