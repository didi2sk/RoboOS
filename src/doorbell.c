/*
 * doorbell.c - a video doorbell (0.105): a button on a GPIO pin -> photo + sound + Home Assistant.
 *
 * On a press (30 ms debounce, then 5 s hold-off): the camera takes a photo (saved in /CAM/<date>/ and sent
 * to MQTT <base>/camera = the Home Assistant camera), the gong / a WAV plays meanwhile core 1 sends the
 * photo, then MQTT <base>/doorbell {"event_type":"ring"} = the Home Assistant doorbell event. The event comes
 * AFTER the photo so a notification made from it shows the new picture. Without network it still rings and
 * the photo stays on the card.
 *
 * /DOORBELL.TXT: state on|off, pin <gpio> (default 6 = the button of the THT shield), active low|high
 * (low = button to GND with the internal pull-up), sound gong|off|/SOUNDS/X.WAV, photo on|off (card),
 * mqtt on|off. Polled from con_trygetc on core 0 (FAT, camera and sound all live there).
 *
 * Radar (optional): the digital output of a presence radar (RCWL-0516, LD2410 OUT, HC-SR501 PIR: high = someone
 * there) on another GPIO. When someone comes (low -> high, 200 ms debounce, then 30 s hold-off) the camera takes
 * a photo: on the card (/CAM/<date>/, radar_photo) and / or to MQTT <base>/camera (radar_mqtt); MQTT
 * <base>/radar = ON / OFF (Home Assistant motion sensor). /DOORBELL.TXT: radar <pin>|off, radar_active high|low,
 * radar_photo on|off, radar_mqtt on|off.
 *
 * Distance sensor HC-SR04 (0.107, optional): TRIG + ECHO pins (ECHO is 5 V - through a divider, e.g. 1k / 2k).
 * Measured every 200 ms (10 us TRIG pulse, the ECHO pulse width / 58 = cm; the wait ends at 2x the limit, so
 * at most ~15 ms for 1 m). Near = 2 readings in a row below dist_cm, away = 3 above. "Someone at the door" =
 * near (and, with a radar too, the radar active as well) - so passers-by further away are not photographed.
 * /DOORBELL.TXT: dist_trig, dist_echo, dist_cm.
 */
#include "kernel.h"
#include "sd.h"

static int on, pin = 6, active_low = 1, photo = 1, use_mqtt = 1, loaded;
static char sound[48] = "gong";
static u64 next_poll, hold_until;
static int cnt, was, busy;
static u32 rings;
static char last_when[32], last_photo[64], last_note[64];
static int radar_pin, radar_high = 1, radar_photo = 1, radar_mqtt, rcnt, rstate;
static u64 rhold_until;
static u32 visits;
static char rlast_when[32], rlast_photo[64];
static int dist_trig, dist_echo, dist_cm = 100, near, ncnt, fcnt, last_cm = -1;
static u32 echo_missing;
static u64 next_dist;

static void scopy(char *d, const char *s, int max) { int i = 0; for (; s[i] && i < max - 1; i++) d[i] = s[i]; d[i] = 0; }

static void setup_pin(void)
{
    if (radar_pin) { gpio_func(radar_pin, 0); gpio_pull(radar_pin, radar_high ? 1 : 2); }
    if (dist_trig) { gpio_func(dist_trig, 1); gpio_set(dist_trig, 0); gpio_func(dist_echo, 0); gpio_pull(dist_echo, 1); near = 0; ncnt = fcnt = 0; }
    rcnt = 0; rstate = -1; rhold_until = timer_us() + 2000000;
    if (!on) return;
    gpio_func(pin, 0);
    gpio_pull(pin, active_low ? 2 : 1);
    cnt = 0; was = 1;                                     /* a press counts only after a release was seen */
    hold_until = timer_us() + 500000;                     /* let the pull settle */
}

static void save(void)
{
    char b[320]; int n = 0;
    #define PUT(s) do { const char *s_ = (s); while (*s_ && n < (int)sizeof(b) - 1) b[n++] = *s_++; } while (0)
    PUT("state "); PUT(on ? "on" : "off");
    PUT("\npin "); { char t[4]; int k = 0; int v = pin; if (v >= 10) t[k++] = (char)('0' + v / 10); t[k++] = (char)('0' + v % 10); t[k] = 0; PUT(t); }
    PUT("\nactive "); PUT(active_low ? "low" : "high");
    PUT("\nsound "); PUT(sound);
    PUT("\nphoto "); PUT(photo ? "on" : "off");
    PUT("\nmqtt "); PUT(use_mqtt ? "on" : "off");
    PUT("\nradar "); if (radar_pin) { char t[4]; int k = 0; if (radar_pin >= 10) t[k++] = (char)('0' + radar_pin / 10); t[k++] = (char)('0' + radar_pin % 10); t[k] = 0; PUT(t); } else PUT("off");
    PUT("\nradar_active "); PUT(radar_high ? "high" : "low");
    PUT("\nradar_photo "); PUT(radar_photo ? "on" : "off");
    PUT("\nradar_mqtt "); PUT(radar_mqtt ? "on" : "off");
    #define NUM(v) do { char t_[8]; int k_ = 0, v_ = (v); do t_[k_++] = (char)('0' + v_ % 10); while ((v_ /= 10)); while (k_) b[n++] = t_[--k_]; } while (0)
    if (dist_trig) { PUT("\ndist_trig "); NUM(dist_trig); PUT("\ndist_echo "); NUM(dist_echo); }
    PUT("\ndist_cm "); NUM(dist_cm);
    #undef NUM
    PUT("\n");
    #undef PUT
    if (!fat_mounted() || fat_write_file("/DOORBELL.TXT", b, (u32)n)) kprintf("(not saved: no SD card or write error)\n");
}

static void load(void)
{
    loaded = 1;
    struct fat_file f; char b[256];
    if (!fat_mounted() || fat_open("/DOORBELL.TXT", &f) || f.is_dir) return;
    int n = fat_read(&f, b, sizeof(b) - 1);
    if (n <= 0) return;
    b[n] = 0;
    for (char *l = b; *l; ) {
        char *e = l; while (*e && *e != '\n' && *e != '\r') e++;
        char sv = *e; *e = 0;
        char *v = l; while (*v && *v != ' ' && *v != '=') v++;
        if (*v) { *v++ = 0; while (*v == ' ' || *v == '=') v++; }
        if (!strcmp(l, "state")) on = !strcmp(v, "on");
        else if (!strcmp(l, "pin")) { int p = 0; while (*v >= '0' && *v <= '9') p = p * 10 + (*v++ - '0'); if (p >= 2 && p <= 27) pin = p; }
        else if (!strcmp(l, "active")) active_low = strcmp(v, "high") != 0;
        else if (!strcmp(l, "sound") && *v) scopy(sound, v, sizeof(sound));
        else if (!strcmp(l, "photo")) photo = strcmp(v, "off") != 0;
        else if (!strcmp(l, "mqtt")) use_mqtt = strcmp(v, "off") != 0;
        else if (!strcmp(l, "radar")) { int p = 0; while (*v >= '0' && *v <= '9') p = p * 10 + (*v++ - '0'); radar_pin = (p >= 2 && p <= 27) ? p : 0; }
        else if (!strcmp(l, "radar_active")) radar_high = strcmp(v, "low") != 0;
        else if (!strcmp(l, "radar_photo")) radar_photo = strcmp(v, "off") != 0;
        else if (!strcmp(l, "radar_mqtt")) radar_mqtt = !strcmp(v, "on");
        else if (!strcmp(l, "dist_trig") || !strcmp(l, "dist_echo") || !strcmp(l, "dist_cm")) {
            int p = 0; while (*v >= '0' && *v <= '9') p = p * 10 + (*v++ - '0');
            if (l[5] == 't') dist_trig = (p >= 2 && p <= 27) ? p : 0;
            else if (l[5] == 'e') dist_echo = (p >= 2 && p <= 27) ? p : 0;
            else if (p >= 10 && p <= 400) dist_cm = p;
        }
        *e = sv; l = *e ? e + 1 : e; while (*l == '\n' || *l == '\r') l++;
    }
}

void doorbell_boot(void) { load(); if (!dist_echo || dist_echo == dist_trig) dist_trig = 0; setup_pin(); }
int doorbell_enabled(void) { return on; }
int doorbell_pin(void) { if (!loaded) load(); return on ? pin : -1; }   /* 0.131: for the alarm's pin check */
int doorbell_radar(void) { return radar_pin || dist_trig; }

/* one HC-SR04 reading: cm, -2 = further than 2x the limit (or nothing), -1 = no echo at all (wiring / no 5 V) */
static int measure_cm(void)
{
    if (gpio_get(dist_echo)) return -1;                    /* still high from before / stuck */
    gpio_set(dist_trig, 1); delay_us(12); gpio_set(dist_trig, 0);
    u64 t0 = timer_us();
    while (!gpio_get(dist_echo)) if (timer_us() - t0 > 5000) return -1;
    u64 s0 = timer_us(), lim = (u64)(dist_cm * 2 + 20) * 58;
    while (gpio_get(dist_echo)) if (timer_us() - s0 > lim) return -2;
    return (int)((timer_us() - s0) / 58);
}

static void radar_mqtt_state(int present)
{
    if (mqtt_connected()) mqtt_publish("radar", present ? "ON" : "OFF", present ? 2 : 3, 1);
}

static void visit(void)
{
    busy = 1;
    visits++;
    rlast_photo[0] = 0;
    u64 now = clock_now();
    if (now) clock_format(rlast_when, now, 0); else scopy(rlast_when, "(no clock)", sizeof(rlast_when));
    con_color(COL_CYAN);
    if (dist_trig && last_cm >= 0) kprintf("\n[DOOR] someone at the door, %d cm, %s\n", last_cm, rlast_when);
    else kprintf("\n[DOOR] someone at the door %s\n", rlast_when);
    con_color(COL_DEFAULT);
    radar_mqtt_state(1);
    int send = radar_mqtt && mqtt_connected();
    if (radar_photo || send) {
        int r = camera_take(radar_photo, send, rlast_photo);
        if (r) kprintf("[RADAR] photo: %s\n", camera_take_error(r));
    }
    rhold_until = timer_us() + 30000000ULL;
    busy = 0;
}

static int no_idle(void) { return 0; }

static void play(void)
{
    if (!strcmp(sound, "off")) return;
    if (strcmp(sound, "gong")) {
        int r = sound_wav(sound, no_idle);
        if (r >= 0) return;
        scopy(last_note, "sound file not played - gong instead", sizeof(last_note));
    }
    int err = 0;
    sound_play("T90 O5 L4 E O4 C2", no_idle, &err);       /* ding - dong */
}

static void ring(void)
{
    busy = 1;
    rings++;
    last_note[0] = 0; last_photo[0] = 0;
    u64 now = clock_now();
    if (now) clock_format(last_when, now, 0); else scopy(last_when, "(no clock)", sizeof(last_when));
    con_color(COL_CYAN); kprintf("\n[DOORBELL] ring %s\n", last_when); con_color(COL_DEFAULT);
    int send = use_mqtt && mqtt_connected();
    int r = camera_take(photo, send, last_photo);         /* first, the visitor faces the button now */
    if (r && r != -1) scopy(last_note, camera_take_error(r), sizeof(last_note));
    else if (r == -1) scopy(last_note, "no camera - rang without a photo", sizeof(last_note));
    play();                                               /* core 1 sends the photo meanwhile */
    if (use_mqtt && mqtt_connected()) {
        u64 end = timer_us() + 5000000;
        while (mqtt_photo_busy() && timer_us() < end) delay_us(10000);
        const char *ev = "{\"event_type\":\"ring\"}";
        mqtt_publish("doorbell", ev, (int)strlen(ev), 0);
    }
    if (last_note[0]) kprintf("[DOORBELL] %s\n", last_note);
    hold_until = timer_us() + 5000000;
    busy = 0;
}

/* core 0, from con_trygetc: every 10 ms */
void doorbell_poll(void)
{
    if ((!on && !radar_pin && !dist_trig) || busy || io_held()) return;
    u64 t = timer_us();
    if (t < next_poll) return;
    next_poll = t + 10000;
    if (dist_trig && t >= next_dist) {                     /* HC-SR04 every 200 ms */
        next_dist = t + 200000;
        int c = measure_cm();
        if (c == -1) echo_missing++;
        last_cm = c;
        if (c >= 0 && c <= dist_cm) { fcnt = 0; if (++ncnt >= 2) near = 1; }
        else { ncnt = 0; if (++fcnt >= 3) near = 0; }
    }
    if (radar_pin || dist_trig) {                          /* 200 ms the same level = a change */
        int lv = (!radar_pin || (gpio_get(radar_pin) ? radar_high : !radar_high)) && (!dist_trig || near);
        if (lv != rstate) { if (++rcnt >= 20) { int old = rstate; rstate = lv; rcnt = 0;
            if (lv && old == 0 && t >= rhold_until) { visit(); return; }
            if (!lv && old == 1) radar_mqtt_state(0); } }
        else rcnt = 0;
    }
    if (!on) return;
    int act = gpio_get(pin) ? !active_low : active_low;
    if (act) { if (cnt < 3) cnt++; } else cnt = 0;
    int pressed = cnt >= 3;
    if (pressed && !was && t >= hold_until) { was = 1; ring(); return; }
    if (!act) was = 0;
    else if (pressed) was = 1;
}

void doorbell_cmd(int argc, char **argv)
{
    if (!loaded) load();
    const char *a = argc > 1 ? argv[1] : "";
    int changed = 0;
    if (!strcmp(a, "on") || !strcmp(a, "off")) {
        on = !strcmp(a, "on");
        if (on && argc > 2) { int p = 0; const char *v = argv[2]; while (*v >= '0' && *v <= '9') p = p * 10 + (*v++ - '0'); if (p >= 2 && p <= 27) pin = p; }
        changed = 1;
    } else if (!strcmp(a, "pin") && argc > 2) {
        int p = 0; const char *v = argv[2]; while (*v >= '0' && *v <= '9') p = p * 10 + (*v++ - '0');
        if (*v || p < 2 || p > 27) { kprintf("pin 2-27\n"); return; }
        pin = p; changed = 1;
    } else if (!strcmp(a, "active") && argc > 2) { active_low = strcmp(argv[2], "high") != 0; changed = 1; }
    else if (!strcmp(a, "sound") && argc > 2) {
        if (!strcmp(argv[2], "gong") || !strcmp(argv[2], "off")) scopy(sound, argv[2], sizeof(sound));
        else {
            char p[64];
            if (path_abs(argv[2], p, sizeof(p))) { kprintf("bad file name\n"); return; }
            struct fat_file f;
            if (fat_open(p, &f)) { kprintf("not found: %s\n", p); return; }
            scopy(sound, p, sizeof(sound));
        }
        changed = 1;
    } else if (!strcmp(a, "photo") && argc > 2) { photo = !strcmp(argv[2], "on"); changed = 1; }
    else if (!strcmp(a, "mqtt") && argc > 2) { use_mqtt = !strcmp(argv[2], "on"); changed = 1; }
    else if (!strcmp(a, "test") || !strcmp(a, "ring")) { ring(); return; }
    else if (!strcmp(a, "distance") && argc > 2) {
        int x[3] = { 0, 0, 0 }, nx = 0;
        if (!strcmp(argv[2], "off")) dist_trig = dist_echo = 0;
        else {
            for (int i = 2; i < argc && nx < 3; i++) { int p = 0; const char *v = argv[i]; while (*v >= '0' && *v <= '9') p = p * 10 + (*v++ - '0'); if (*v || v == argv[i]) nx = 9; else x[nx++] = p; }
            if (nx == 1 && x[0] >= 10 && x[0] <= 400) dist_cm = x[0];
            else if ((nx == 2 || nx == 3) && x[0] >= 2 && x[0] <= 27 && x[1] >= 2 && x[1] <= 27 && x[0] != x[1] && (nx == 2 || (x[2] >= 10 && x[2] <= 400))) {
                dist_trig = x[0]; dist_echo = x[1]; if (nx == 3) dist_cm = x[2];
            } else { kprintf("doorbell distance <trig> <echo> [cm] | <cm 10-400> | off   (HC-SR04; ECHO through a 5 V -> 3.3 V divider)\n"); return; }
        }
        changed = 1;
    }
    else if (!strcmp(a, "radar") && argc > 2) {
        const char *v = argv[2];
        if (!strcmp(v, "off")) radar_pin = 0;
        else if (!strcmp(v, "photo") && argc > 3) radar_photo = !strcmp(argv[3], "on");
        else if (!strcmp(v, "mqtt") && argc > 3) radar_mqtt = !strcmp(argv[3], "on");
        else if (!strcmp(v, "active") && argc > 3) radar_high = strcmp(argv[3], "low") != 0;
        else if (!strcmp(v, "test")) { visit(); return; }
        else { int p = 0; while (*v >= '0' && *v <= '9') p = p * 10 + (*v++ - '0');
               if (*v || p < 2 || p > 27) { kprintf("doorbell radar <pin 2-27>|off|photo on|off|mqtt on|off|active high|low|test\n"); return; }
               radar_pin = p; }
        changed = 1;
    }
    else if (*a) {
        kprintf("usage: doorbell                 status\n"
                "       doorbell on [pin]        watch the button (default GPIO6, to GND)\n"
                "       doorbell off\n"
                "       doorbell pin <n>         the button's GPIO\n"
                "       doorbell active low|high low = button to GND (internal pull-up)\n"
                "       doorbell sound gong|off|<file.wav>\n"
                "       doorbell photo on|off    save the photo on the card (/CAM/<date>/)\n"
                "       doorbell mqtt on|off     photo + ring event to Home Assistant (MQTT)\n"
                "       doorbell test            ring now, as if the button was pressed\n"
                "       doorbell radar <pin>|off radar / PIR output (high = someone there): photo when someone comes\n"
                "       doorbell radar photo on|off   that photo on the card (/CAM/<date>/)\n"
                "       doorbell radar mqtt on|off    that photo to Home Assistant too\n"
                "       doorbell radar active high|low, doorbell radar test\n"
                "       doorbell distance <trig> <echo> [cm]  HC-SR04: someone only when nearer than cm (default 100)\n"
                "       doorbell distance <cm> | off          (with a radar too, both must agree; ECHO via a 5 V divider!)\n");
        return;
    }
    if (changed) { save(); setup_pin(); mqtt_rediscover(); }
    kprintf("doorbell %s: button GPIO%d (%s), sound %s, photo on card %s, MQTT %s%s\n",
            on ? "ON" : "off", pin, active_low ? "to GND, pull-up" : "to 3.3 V, pull-down", sound,
            photo ? "on" : "off", use_mqtt ? "on" : "off", use_mqtt && !mqtt_connected() ? " (not connected now)" : "");
    if (on) kprintf("  button now: %s, rings %u\n", (gpio_get(pin) ? !active_low : active_low) ? "PRESSED" : "released", rings);
    if (rings) kprintf("  last ring %s%s%s%s%s\n", last_when, last_photo[0] ? ", photo " : "", last_photo, last_note[0] ? " - " : "", last_note);
    if (radar_pin) {
        kprintf("radar GPIO%d (active %s): now %s, photo on card %s, to MQTT %s, visits %u\n", radar_pin, radar_high ? "high" : "low",
                (gpio_get(radar_pin) ? radar_high : !radar_high) ? "SOMEONE THERE" : "nobody", radar_photo ? "on" : "off", radar_mqtt ? "on" : "off", visits);
        if (visits) kprintf("  last visit %s%s%s\n", rlast_when, rlast_photo[0] ? ", photo " : "", rlast_photo);
    } else kprintf("radar off ('doorbell radar <pin>')\n");
    if (dist_trig) {
        int c = measure_cm();
        kprintf("distance HC-SR04 TRIG GPIO%d ECHO GPIO%d: now ", dist_trig, dist_echo);
        if (c >= 0) kprintf("%d cm", c); else kprintf(c == -2 ? "further than %d cm" : "NO ECHO (wiring? 5 V? divider?)", dist_cm * 2);
        kprintf(", someone = nearer than %d cm%s%s\n", dist_cm, radar_pin ? " AND the radar" : "", near ? " - SOMEONE THERE" : "");
        if (echo_missing) kprintf("  readings without an echo: %u\n", echo_missing);
        if (!radar_pin) kprintf("  photo on card %s, to MQTT %s ('doorbell radar photo|mqtt on|off'), visits %u\n", radar_photo ? "on" : "off", radar_mqtt ? "on" : "off", visits);
        if (!radar_pin && visits) kprintf("  last visit %s%s%s\n", rlast_when, rlast_photo[0] ? ", photo " : "", rlast_photo);
    } else kprintf("distance sensor off ('doorbell distance <trig> <echo> [cm]')\n");
    if (changed) kprintf("(saved in /DOORBELL.TXT)\n");
}
