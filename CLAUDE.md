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
- `$'\x03'` sends Ctrl+C; `~N` just waits N seconds (e.g. during `video`), `"!screendump /path.ppm"` grabs the screen mid-test
- env: `QEMU_EXTRA="-device usb-kbd"`, `QEMU_SD=sd.img`, `QEMU_MACHINE=raspi3ap`, `BOOT_WAIT=seconds`
- verify FAT writes: `dd if=sd.img of=p.img bs=512 skip=2048; fsck.fat -n p.img`
  ("Free cluster summary uninitialized" is expected - we invalidate FSInfo on purpose)

Network in QEMU: `tools/qemu_net_test.py kernel8.img sd.img "http on" "@get5" "ping 10.0.2.2"` (`"@sh:curl -s http://127.0.0.1:18080/api/status -H 'Authorization: Bearer x'"` runs a host command) - QEMU
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
- Not yet verified on hardware (v0.20): `screen` resolutions + `/SCREEN.TXT`, the two framebuffer pages
  (virtual height 2h, offset tag 0x48009), worker cores 2-3, `video` (decode speed, sound stream).
- Not yet verified on hardware (v0.21): the JSON API (only QEMU + curl).
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
| `src/boot.S` | EL3/EL2 -> EL1, core 0 stack below 0x80000, vectors, BSS; exceptions use a separate 16 KB stack PER CORE; `secondary_entry` for cores 1-3 (same EL drop, stack = core_stacks + n*64 KB, then `secondary_main`) |
| `src/smp.c` | v0.19: core 1 = I/O core. Woken via the spin table (write secondary_entry to 0xE0, dc civac, sev; QEMU raspi3b emulates the same). Core 1 loop: io_lock -> kbd_poll + usb_hotplug + net_poll (lwIP, RoboOS messages) -> io_unlock, 200 us pause. Recursive spinlocks (`__atomic`, need MMU on; no-ops before mmu_init): io lock (USB controller, eth, lwIP, sockets, messages), console lock (whole kprintf/con_* calls), mbox lock (mailbox buffer). Lock order io -> console / mbox. Core-0 entry points (ip.c wrappers `*_l`, net.c `*_locked`, eth_link/eth_set_gigabit, shell usb / net test) take the io lock; every WAIT for the network uses `io_release()` / `io_reacquire()` so core 1 can run (and a longjmp on Ctrl+C never leaks the lock). FAT is core 0 only: /NAME.TXT and /IP.TXT are read in kernel_main before core 1 starts (`net_name()`, `ip_prepare()`). v0.20: cores 2-3 are WORKERS (`smp_run(core, fn, arg)` / `smp_wait(core, ms)`, they `wfe` between jobs; a timeout marks the core dead, `smp_worker_ok`); program code is synced with `ic ialluis` so every core sees it. Key queue in kbd.c is single-producer/single-consumer with atomics. A crash on core 1 (`exc_handler`) releases its locks and core 0 falls back to polling in `con_trygetc()` (also used when core 1 does not start). `info` shows the cores |
| `src/fb.c` | v0.20: `fb_init(w, h)` (0,0 = native from tag 0x40003), asks for virtual height 2h -> `fb_pages()` 2 (falls back to 1); console always on page 0; `fb_page_row(page, y)`, `fb_show_page(p)` (offset tag 0x48009). `screen WxH|auto` (shell) saves /SCREEN.TXT, read in kernel_main (`screen_choice`) after fat_mount, BEFORE fb_init (SD init now comes first) |
| `src/video.c` | v0.20, built WITH floating point (Makefile rule like stbimg): pl_mpeg 1.0 (`third_party/pl_mpeg.h`, MIT) MPEG-1 video + MP2 in .mpg; file via `plm_buffer_create_with_callbacks` (fat_read/pos), `plm_probe` only 256 KB (5 MB made startup read the whole file); frame planes copied to a staging copy, cores 2/3 convert YCbCr->RGB + nearest scale (fit, max 2x) into the hidden page while core 0 decodes the next frame, then flip; frames older than the target time are not drawn (dropped); sound via the i2s.c stream API (`sound_stream_open(rate)`, `audio_write` blocks when full, `audio_close`), lead time 0.2 s; keys q/Enter/Ctrl+C, space, arrows +-10 s (USB Esc is ignored by kbd.c); `video` shell, BASIC `VIDEO`, C `playVideo` (API 7) |
| `src/mmu.c` | identity map, 2 MB blocks: RAM cached, GPU RAM + 2 MB DMA block non-cached, peripherals device |
| `src/board.c` | revision -> board name, prompt host, ACT LED pin, `board_usb_direct()` |
| `src/usb.c` | DWC2 host driver: polling (on core 1 since v0.19), one channel (0), Buffer DMA, one packet per channel run |
| `src/eth.c` | Ethernet over USB: LAN9514 (0424:ec00, 3B), LAN7515/LAN7800 (0424:7800, 3B+) and any CDC-ECM adapter (QEMU usb-net); vendor control requests 0xA0/0xA1 for registers, bulk IN/OUT for frames (`usb_bulk`), polled; `link_tick()` follows the link once a second (LAN78xx: MAC speed/duplex only while TX/RX are off, started on link-up) |
| `src/net.c` | RoboOS messages on raw frames (EtherType 0x88B5): names (`/NAME.TXT`), WHO/HERE, MSG with ACK + 3 tries, inbox of 8; polled every 5 ms from `con_trygetc()`; `net loop on` = loopback for QEMU tests |
| `src/ip.c` | sockets for programs (handles 1..8, 4 KB rx ring each, TCP flow control by refusing data, UDP records len/addr/port; blocking calls take an `idle()` that polls the network: app.c `poll_input`, BASIC `check_break`; closed when a program / BASIC run ends); TCP/IP glue for lwIP 2.2.1 (`lwip/`, unmodified, NO_SYS raw API; port in `src/lwip_port/`: lwipopts.h, arch/cc.h, tiny string/stdlib/stdio shims): netif -> `eth_send`, non-0x88B5 frames from `net.c` -> lwIP, `sys_check_timeouts()` from `net_poll`; DHCP (hostname = board name), `/IP.TXT` static config, `ip`, `ping` (raw ICMP, DNS), `http on` (one page) |
| `src/webapi.c` | v0.21 JSON API behind `http on` (runs on core 1 from lwIP callbacks, no lwIP inside): `Authorization: Bearer <token>` (constant-time compare; no token = 403), GET /api/status, GET /api/gpio[/n], POST /api/gpio (object or list, all validated before any pin changes, value -> output), POST /api/reboot (`sys_reboot()` from `api_poll()` in ip_poll 0.5 s later). ip.c collects each request (4 connections x 2 KB, headers + Content-Length, 5 s idle timeout, CORS OPTIONS). `/HTTP.TXT` = "on|off [token]", read in ip_prepare on core 0, `http on` autostarts in start() |
| `src/kbd.c` | HID boot keyboard, US layout, keypad, LEDs, autorepeat, runtime method lock |
| `src/sd.c` / `src/fat.c` | Arasan SDHCI (GPIO48-53 ALT3), PIO single-block read/write; FAT16/32 with LFN read, 8.3 write, `fat_mkdir` (8.3, mixed case stored in capitals, `.`/`..` with `..` = 0 for the root), `fat_remove` (file or empty dir, also marks its LFN entries 0xE5 so fsck stays clean); shell `mkdir`, `rm` (refuses the cwd and its parents, boot files in / need `-f`), BASIC MKDIR/RMDIR/KILL |
| `src/app.c` | loads `.bin` at 0x01000000, API table (`sdk/robos.h`), Ctrl+C via `ctx_save/ctx_restore` (`src/ctx.S`), autostart `/RUN` |
| `src/basic.c` | line-numbered integer BASIC, text `.BAS` files, SAVE/LOAD, GPIO statements |
| `src/lineedit.c` | shared line editor (shell, BASIC prompt, INPUT): cursor keys, insert/delete, history, Tab hook; redraws with `\b` + chars only |
| `src/edit.c` | full-screen editor (`edit <file>`): flat buffer at 0x02000000 (1 MB) + save scratch above it; draws via `con_goto`/`con_clreol`, only changed rows (UART is slow) |
| `src/sound.c` | tones: PWM in mark-space mode, range = 9.6 MHz / Hz (CM_PWMCTL src osc 19.2 MHz, DIVI 2), volume = duty (100 % -> 50 %); pins: Pi 3 jack GPIO40/41 ALT0, Pi 2 40/45, Zero 2 W / CM3 GPIO18/19 ALT5; `sound_poll()` from `con_trygetc()` ends timed tones; music strings like QBasic PLAY (checked before playing); stopped when a program / BASIC run / BASIC session ends. QEMU keeps the PWM register values (check with `mem 0x3f20c000`), but makes no sound - **not heard on hardware yet** |
| `src/i2s.c` | audio by DMA with two sinks. PWM sink (v0.16, WAV on jack / GPIO18-19 via `pwm_wav()`): PWM clock PLLD/2 = 250 MHz, range = 250 MHz / rate, balanced mode, USEF1+USEF2 (FIFO alternates L/R words), DMAC ENAB + DREQ/PANIC 7, DMA PERMAP 5 to 0x7E20C018, samples (s+32768)*range>>16; afterwards PWM off and `clock_on = 0` in sound.c so tones re-init the osc clock. I2S sink (`sound out i2s`, saved in /SOUND.TXT): PCM at 0x3F203000 master, FLEN 64 / FSLEN 32, FSI + CLKI, ch1 pos 1, ch2 pos 33, 16-bit unpacked words (as Linux bcm2835-i2s for I2S NB_NF); clock PLLD 500 MHz MASH1 to rate*64; GPIO18-21 ALT0; DMA channel 5 (PERMAP 2, DREQ) through a chain of 32 x 2 KB segments at DMA block +0x180000 that ENDS at the last filled one (underrun = silence, poll restarts it; position from CONBLK_AD / SOURCE_AD). Tones: sine generator, length counted in samples; WAV streamed in the own loop of `i2s_wav()` (no FAT reads from `sound_poll`). QEMU DMA runs the chain instantly and has no PCM, so only the data path is testable. v0.20 stream API for video: `audio_open(hz, on_i2s)`, `audio_write(lr, frames)` (waits while 31 of 32 segments are queued), `audio_queued_ms`, `audio_close(now)` (pads + drains up to 1 s, closes the PWM sink); `pump()` skips WAV/tone filling while `stream_on` - **not heard on hardware yet** |
| `src/image.c` | pictures: stb_image 2.32 (`third_party/stb_image.h`, configured in `src/stbimg.c`, which the Makefile builds WITHOUT -mgeneral-regs-only and with -w) decodes JPEG/PNG/BMP/GIF to RGBA in the heap; box-filter downscale (max 4x4 samples), alpha blended over the screen; `view` (fb_hold + direct pixels, bar via `fb_text_px`), boot picture /SPLASH.* (kernel_main now inits the SD card BEFORE USB so the picture covers the boot; text waits behind `fb_hold`, released after >= 2.5 s or a key, before autostart); BASIC IMAGE, C drawImage (API v6) |
| `src/heap.c` | bump allocator 0x04000000-0x10000000 (192 MB) with size headers for realloc; `heap_reset()` before each decode |
| `src/path.c` | current directory (`cwd_get`/`cwd_set`, spelling taken from the card) and `path_abs()` (relative, `.`, `..` -> absolute); used by shell commands, `app_resolve` (cwd first, then /RUN, /APPS for bare names), BASIC LOAD/SAVE/DIR, edit |
| `src/shell.c` | commands; Tab completes command names, then file/dir names (quotes names with spaces, rewrites the typed prefix in the card's spelling using `\b`) |
| `sdk/` | program SDK (API version 6: messages, TCP/UDP, sound, WAV, pictures): `start.S` header (`ROBOAPP1` magic at +8, image end at +16), `crt0.c`, `app.ld`, examples |

Memory: kernel at 0x80000, BSS ends ~0x120000, DMA block at 0x200000 (2 MB, uncached),
programs at 0x01000000-0x02000000, heap (pictures) 0x04000000-0x10000000, USB DMA: channel 0 buffer at the DMA block start, channel 1 (async bulk IN) +1 MB, I2S DMA control blocks + audio segments +0x180000 (~65 KB), editor text at 0x02000000 (1 MB) + save scratch up to 0x02300000.
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
- **Autorepeat after a long pause**: if nobody polled the keyboard for >100 ms (decoding a picture), the
  release may have been missed - repeat only after a fresh report (kbd_poll); otherwise one key press jumped
  two pictures in `view`.
- **Never call idle() / con_trygetc-with-Ctrl+C while holding the io lock**: a program's Ctrl+C longjmps out and
  the lock would stay taken forever (core 1 stuck). Release it first (`io_release`), reacquire after.
- QEMU-only (pre-existing since v0.11): `usb reset` with `-device usb-net` aborts QEMU (`usb_ep_get: Assertion
  dev != NULL`) - the ECM channel-1 transfer survives the core reset in QEMU's dwc2. Test `usb reset` without usb-net.
- `fat_read` reads whole sectors straight into the caller's buffer only if it is 4-byte aligned (sd_read stores
  u32 words); pl_mpeg's ring buffer hands odd addresses.
- In BASIC, compute an array index (which may allocate) before reading `v->arr` (evaluation-order bug fixed once).
- The font covers ASCII 32-126 only - keep all on-screen strings ASCII.
- Keep exceptions inside programs recoverable: exception stack is separate so `app_crashed()` can longjmp back.

## Conventions

- Version string in `src/main.c` (`VERSION`), mention it in both READMEs.
- After changes: build with no warnings, run the QEMU tests relevant to the change, update README.md + README.sk.md.
- Put the fresh `kernel8.img` into `sd/` (the CI also builds it).

## Open ideas / next steps

- verify two-board messaging/BASIC/loader/TCP-IP and the I/O core (v0.19) on real hardware; Zero 2 W bring-up
- cores 2-3: workers exist since v0.20 (video); next a "run this function on core N" API for programs; later preemptive multitasking
- video: playlist / folder, time bar; test files: `ffmpeg -f lavfi -i testsrc=size=640x360:rate=30 -f lavfi -i sine -t 5 -c:v mpeg1video -q:v 4 -c:a mp2 -f mpeg TEST.MPG` then `mcopy -i sd.img@@1M TEST.MPG ::/`
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
- I2C / SPI / PWM in API and BASIC, interrupts + timer, editor search / go to line / save as
- optional AArch32 port for Pi 2 v1.1 / Zero
