# RoboOS - notes for Claude Code

Bare-metal hobby OS for the Raspberry Pi (64-bit BCM2837 boards), written from scratch in C +
AArch64 assembly. No Linux, no libc. Owner: Robo (talks in Slovak; UI, code comments and the
English README are in English, `README.sk.md` is the Slovak README - keep both in sync).

## Build & test

```
make                 # kernel8.img (clang --target=aarch64-none-elf + ld.lld)
make apps            # SDK examples -> sd/APPS/*.bin
make sd.img          # 64 MB FAT32 test image for QEMU (needs dosfstools, mtools, fdisk)
make run             # QEMU raspi3b, UART on stdio, USB keyboard
```

Automated QEMU testing (no display): `tools/qemu_test.py kernel8.img "cmd1" "cmd2" ...`
- sends each arg + CR over UART, prints UART output, saves HDMI screenshot to /tmp/rpi_screen.ppm
- an arg starting with `!` goes to the QEMU monitor; `;` separates several, e.g.
  `"!sendkey e; sendkey c; sendkey h; sendkey o; sendkey ret"` types on the emulated USB keyboard
- `$'\x03'` sends Ctrl+C
- env: `QEMU_EXTRA="-device usb-kbd"`, `QEMU_SD=sd.img`, `QEMU_MACHINE=raspi3ap`, `BOOT_WAIT=seconds`
- verify FAT writes: `dd if=sd.img of=p.img bs=512 skip=2048; fsck.fat -n p.img`
  ("Free cluster summary uninitialized" is expected - we invalidate FSInfo on purpose)

Network in QEMU: `tools/qemu_net_test.py kernel8.img sd.img "http on" "@get5" "ping 10.0.2.2"` - QEMU
`usb-net` (its 2nd configuration is CDC-ECM, which `eth.c` drives) on user networking: DHCP 10.0.2.15,
gateway 10.0.2.2, board port 80 forwarded to host 127.0.0.1:18080. This sandbox has no DNS: to test names,
run a tiny DNS responder on host 127.0.0.1:53 and use `ip set 10.0.2.20 255.255.255.0 10.0.2.2`.

QEMU limitations: no split transactions / HS hubs (QEMU hubs are FS), no Zero 2 W machine,
`device_add` of a USB device at runtime freezes QEMU 8.2 (not our bug) - hot-plug can't be tested.

## Hardware status (what the owner has verified on real boards)

- RPi 3B: boot, HDMI, UART, MMU, USB keyboards **work** (incl. Apple A1243 keyboard with built-in
  HS hub, and an RGB gaming keyboard). Hot-plug works.
- RPi 3B: SD init + FAT file writes **work** (owner tested after the CMD55 fix, v0.7).
- RPi 3B: Ethernet (LAN9514, v0.8) **works**: init, link up 100 Mbit full duplex, `net test` received
  235 frames with 0 errors and sent 10 (photo). The owner's LAN delivers VLAN-tagged frames (type 8100).
- Pi 3B+ (owner has one, rev 0xa020d3): Ethernet LAN7515/LAN7800 (0424:7800, v0.10, from the Linux lan78xx
  driver) **links up**: the router showed the port at 100 Mbit (v0.10-0.11 advertised 10/100 only; the owner
  expected gigabit). v0.12 advertises 1000BASE-T full duplex and sets MAC speed 1000 - **not yet verified**;
  `net speed 100` is the fallback. Traffic through the LAN7515 (frames, DHCP) not yet confirmed either.
- Not yet verified on hardware: RoboOS messages between two boards (v0.9, tested only with `net loop on`
  in QEMU), program loader, BASIC, line editor / full-screen editor,
  board detection, Zero 2 W (ACT LED GPIO29 active-low is an assumption).
- Owner's boards: more than one Pi 3 (so two-board network tests are possible), a Zero 2 W, and a
  Pi 2 that is the 32-bit v1.1 (cannot run RoboOS without an AArch32 port). No Pi 4 / Pi 5.
- Debugging on hardware happens via photos of the HDMI screen - keep diagnostics printable on
  screen (`usb debug on`, `usb test`, `usb` tree with hub port status).

## Architecture

| File | Role |
|------|------|
| `src/boot.S` | park cores 1-3, EL3/EL2 -> EL1, stack below 0x80000, vectors, BSS; exceptions use a separate 16 KB stack |
| `src/mmu.c` | identity map, 2 MB blocks: RAM cached, GPU RAM + 2 MB DMA block non-cached, peripherals device |
| `src/board.c` | revision -> board name, prompt host, ACT LED pin, `board_usb_direct()` |
| `src/usb.c` | DWC2 host driver: polling, one channel (0), Buffer DMA, one packet per channel run |
| `src/eth.c` | Ethernet over USB: LAN9514 (0424:ec00, 3B), LAN7515/LAN7800 (0424:7800, 3B+) and any CDC-ECM adapter (QEMU usb-net); vendor control requests 0xA0/0xA1 for registers, bulk IN/OUT for frames (`usb_bulk`), polled; `link_tick()` follows the link once a second (LAN78xx: MAC speed/duplex only while TX/RX are off, started on link-up) |
| `src/net.c` | RoboOS messages on raw frames (EtherType 0x88B5): names (`/NAME.TXT`), WHO/HERE, MSG with ACK + 3 tries, inbox of 8; polled every 5 ms from `con_trygetc()`; `net loop on` = loopback for QEMU tests |
| `src/ip.c` | sockets for programs (handles 1..8, 4 KB rx ring each, TCP flow control by refusing data, UDP records len/addr/port; blocking calls take an `idle()` that polls the network: app.c `poll_input`, BASIC `check_break`; closed when a program / BASIC run ends); TCP/IP glue for lwIP 2.2.1 (`lwip/`, unmodified, NO_SYS raw API; port in `src/lwip_port/`: lwipopts.h, arch/cc.h, tiny string/stdlib/stdio shims): netif -> `eth_send`, non-0x88B5 frames from `net.c` -> lwIP, `sys_check_timeouts()` from `net_poll`; DHCP (hostname = board name), `/IP.TXT` static config, `ip`, `ping` (raw ICMP, DNS), `http on` (one page) |
| `src/kbd.c` | HID boot keyboard, US layout, keypad, LEDs, autorepeat, runtime method lock |
| `src/sd.c` / `src/fat.c` | Arasan SDHCI (GPIO48-53 ALT3), PIO single-block read/write; FAT16/32 with LFN read, 8.3 write, `fat_mkdir` (8.3, mixed case stored in capitals, `.`/`..` with `..` = 0 for the root), `fat_remove` (file or empty dir, also marks its LFN entries 0xE5 so fsck stays clean); shell `mkdir`, `rm` (refuses the cwd and its parents, boot files in / need `-f`), BASIC MKDIR/RMDIR/KILL |
| `src/app.c` | loads `.bin` at 0x01000000, API table (`sdk/robos.h`), Ctrl+C via `ctx_save/ctx_restore` (`src/ctx.S`), autostart `/RUN` |
| `src/basic.c` | line-numbered integer BASIC, text `.BAS` files, SAVE/LOAD, GPIO statements |
| `src/lineedit.c` | shared line editor (shell, BASIC prompt, INPUT): cursor keys, insert/delete, history, Tab hook; redraws with `\b` + chars only |
| `src/edit.c` | full-screen editor (`edit <file>`): flat buffer at 0x02000000 (1 MB) + save scratch above it; draws via `con_goto`/`con_clreol`, only changed rows (UART is slow) |
| `src/sound.c` | tones: PWM in mark-space mode, range = 9.6 MHz / Hz (CM_PWMCTL src osc 19.2 MHz, DIVI 2), volume = duty (100 % -> 50 %); pins: Pi 3 jack GPIO40/41 ALT0, Pi 2 40/45, Zero 2 W / CM3 GPIO18/19 ALT5; `sound_poll()` from `con_trygetc()` ends timed tones; music strings like QBasic PLAY (checked before playing); stopped when a program / BASIC run / BASIC session ends. QEMU keeps the PWM register values (check with `mem 0x3f20c000`), but makes no sound - **not heard on hardware yet** |
| `src/i2s.c` | audio by DMA with two sinks. PWM sink (v0.16, WAV on jack / GPIO18-19 via `pwm_wav()`): PWM clock PLLD/2 = 250 MHz, range = 250 MHz / rate, balanced mode, USEF1+USEF2 (FIFO alternates L/R words), DMAC ENAB + DREQ/PANIC 7, DMA PERMAP 5 to 0x7E20C018, samples (s+32768)*range>>16; afterwards PWM off and `clock_on = 0` in sound.c so tones re-init the osc clock. I2S sink (`sound out i2s`, saved in /SOUND.TXT): PCM at 0x3F203000 master, FLEN 64 / FSLEN 32, FSI + CLKI, ch1 pos 1, ch2 pos 33, 16-bit unpacked words (as Linux bcm2835-i2s for I2S NB_NF); clock PLLD 500 MHz MASH1 to rate*64; GPIO18-21 ALT0; DMA channel 5 (PERMAP 2, DREQ) through a chain of 32 x 2 KB segments at DMA block +0x180000 that ENDS at the last filled one (underrun = silence, poll restarts it; position from CONBLK_AD / SOURCE_AD). Tones: sine generator, length counted in samples; WAV streamed in the own loop of `i2s_wav()` (no FAT reads from `sound_poll`). QEMU DMA runs the chain instantly and has no PCM, so only the data path is testable - **not heard on hardware yet** |
| `src/path.c` | current directory (`cwd_get`/`cwd_set`, spelling taken from the card) and `path_abs()` (relative, `.`, `..` -> absolute); used by shell commands, `app_resolve` (cwd first, then /RUN, /APPS for bare names), BASIC LOAD/SAVE/DIR, edit |
| `src/shell.c` | commands; Tab completes command names, then file/dir names (quotes names with spaces, rewrites the typed prefix in the card's spelling using `\b`) |
| `sdk/` | program SDK (API version 5: messages, TCP/UDP, sound, WAV): `start.S` header (`ROBOAPP1` magic at +8, image end at +16), `crt0.c`, `app.ld`, examples |

Memory: kernel at 0x80000, BSS ends ~0x120000, DMA block at 0x200000 (2 MB, uncached),
programs at 0x01000000-0x02000000, USB DMA: channel 0 buffer at the DMA block start, channel 1 (async bulk IN) +1 MB, I2S DMA control blocks + audio segments +0x180000 (~65 KB), editor text at 0x02000000 (1 MB) + save scratch up to 0x02300000.
Keys: `con_getkey()` decodes escape sequences into `K_UP`... (kbd.c sends the same sequences as a terminal).

## Hard-won lessons (do not regress)

- **-mstrict-align / -mgeneral-regs-only**: code runs with MMU off early on (device memory faults on unaligned access).
- **Mailbox & DMA buffers**: clean/invalidate D-cache around mailbox calls; USB DMA buffer lives in the uncached block; DMA bus address = phys | 0xC0000000.
- **DWC2 in DMA mode auto-retries NAK** on control/bulk, so HCINT can show NAK and XFERCOMPL together - check XFERCOMPL first.
- **Periodic split transactions** need microframe scheduling: SSPLIT in uframe N (avoid 6), CSPLIT in N+2.. (2-3 tries on NYET). Without it interrupt IN through the LAN9514 TT never delivered keys.
- **Keyboard read method** is decided at runtime: alternate interrupt IN and GET_REPORT until the first non-zero report, then lock (`k_lock`).
- **Hub ports must be powered one at a time** (with pgood delay); powering all ports of the Apple keyboard hub at once made it drop off the bus.
- Gaming keyboards attach late (>500 ms after port power) -> hot-plug polling of hub ports every 0.7 s, plus the root port on boards without an onboard hub.
- HID init like a PC: GET_DESCRIPTOR(report), SET_PROTOCOL(boot), SET_IDLE(0), SET_REPORT(LEDs) - some keyboards stay dark/silent otherwise.
- **SD on WiFi boards**: firmware leaves GPIO34-39 on ALT3 (Arasan -> WiFi chip); set them to input before
  routing GPIO48-53 to ALT3 (Circle does the same; CMD8 answers on the 3B with this in place).
- **SD CMD55 must wait for its R1 answer** even with RCA 0; otherwise ACMD41 collides with it on a real card
  (INT=0x30000 at "ACMD41 power-up" on the 3B; QEMU answers instantly and hides it). Reset CMD/DAT lines after a failed command.
- **Never halt a bulk IN channel that may be receiving** (timeout -> CHDIS) on a device that NAKs when empty:
  frames arriving at that moment are lost (QEMU usb-net lost ~half the TCP segments). LAN95xx/LAN78xx answer
  an empty IN with a ZLP (BIR=0) so a short blocking transfer is fine; ECM uses `usb_bulk_in_async()` on channel 1,
  left running between polls.
- lwIP: `ip_input` is an lwIP macro - do not name RoboOS functions like lwIP ones.
- In BASIC, compute an array index (which may allocate) before reading `v->arr` (evaluation-order bug fixed once).
- The font covers ASCII 32-126 only - keep all on-screen strings ASCII.
- Keep exceptions inside programs recoverable: exception stack is separate so `app_crashed()` can longjmp back.

## Conventions

- Version string in `src/main.c` (`VERSION`), mention it in both READMEs.
- After changes: build with no warnings, run the QEMU tests relevant to the change, update README.md + README.sk.md.
- Put the fresh `kernel8.img` into `sd/` (the CI also builds it).

## Open ideas / next steps

- verify two-board messaging/BASIC/loader/TCP-IP on real hardware; Zero 2 W bring-up
- `make sd.img` also copies `sd/APPS/*.bas`; examples: tcpserv.c, udpecho.c, music.c, ledsrv.bas, udptemp.bas, sound.bas
- network: RoboOS messages over raw frames (v0.9) and TCP/IP via lwIP (v0.11: DHCP, ping, DNS, one web page)
  are in, TCP/UDP for BASIC and C since v0.12; all TCP/IP is tested only in QEMU. Next: a remote shell
- `tools/qemu_net_test.py`: `@tcp:23:line|line` / `@udp:5000:text` talk to the board from the host; the host
  also serves TCP 10.0.2.2:18000 (answers "host got: ...") and listens on UDP 10.0.2.2:15001
- BASIC gotcha when testing: `run x.bas` leaves that program in BASIC memory; typed lines merge with it
- license (MIT suggested, not chosen yet)
- Slovak keyboard layout + font with diacritics
- sound: tones (v0.14), I2S DAC + WAV (v0.15), WAV on the PWM jack / GPIO by DMA (v0.16) are in; none heard
  on hardware yet
- I2C / SPI / PWM in API and BASIC, interrupts + timer, multitasking, editor search / go to line / save as
- optional AArch32 port for Pi 2 v1.1 / Zero
