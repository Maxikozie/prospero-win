/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* A DualShock 4 on a Linux host as the title's XInput controller 0, for a
 * game run under the host desktop Wine: preloaded (LD_PRELOAD) into Wine, it
 * exports the two functions xinput's Unix library looks up with dlsym
 * (Wine patches 0470/0471), pw_wine_pad_at and pw_wine_set_rumble_at, so
 * the game reads the pad through the same path as on the console, fed from
 * evdev instead of scePadReadState.
 *
 *   PW_SINK_PROCESS    only a process whose command line contains this
 *                      serves the pad (explorer and services load xinput too)
 *   PW_HOST_PAD        the pad's event node (default: the first Sony
 *                      controller found under /dev/input)
 *   PW_HOST_PAD_TOUCH  its touchpad's event node (default: found the same
 *                      way; "none" for no touchpad)
 *   PW_HOST_PAD_EVENTS a text file replayed instead of devices, one event a
 *                      line, "p|t <type> <code> <value>" (p the pad, t the
 *                      touchpad); each call to pw_wine_pad_at consumes up to
 *                      and including the next pad SYN_REPORT
 *
 * The mapping is the console's (native/pw_wine_display.c pw_wine_game_pad,
 * whose table and stick scaling are repeated here): Cross A, Circle B,
 * Square X, Triangle Y, L1/R1, L3/R3, the d-pad, Options Start; L2/R2 are
 * the analogue triggers only; sticks 0..255 to -32768..32767 with y up.
 * Share and PS are the system's on the console and reach the game there
 * never, so they are dropped here too. The touchpad is split as the console
 * title splits it: a click on its left half is Back while held; a click on
 * its right half is the title's mark and never reaches the game: each one
 * logs "host_pad mark N t=S" on release.
 *
 * Only controller 0 is served. The pad is opened on the first call, which
 * is the game's first XInput call: a game that looks for a controller only
 * at startup sees it then. On a read error (unplugged) the controller reads
 * disconnected and the nodes are looked for again once a second.
 * Rumble goes out as one FF_RUMBLE effect (left motor strong, right weak).
 *
 * stderr gets "host_pad open ...", the marks, "host_pad progress ..." every
 * 30 s and "host_pad summary ..." at exit, each with reads= (calls for
 * controller 0), connected_reads=, first_read_ms= (after load), packets=,
 * rumble_events=, marks=.
 *
 * Built with -DHOST_PAD_MONITOR it is a program instead, which prints the
 * controller state whenever it changes (no Wine needed); --list lists the
 * candidate nodes. */
#define _GNU_SOURCE
#include "../wine/ps5/pw_wine_sink.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define HP_EXPORT __attribute__((visibility("default")))

/* DualSense button bits, as PwPadPs5's previous_buttons holds them. */
enum {
    HP_L3=0x2u, HP_R3=0x4u, HP_OPTIONS=0x8u, HP_UP=0x10u, HP_RIGHT=0x20u, HP_DOWN=0x40u, HP_LEFT=0x80u,
    HP_L1=0x400u, HP_R1=0x800u, HP_TRIANGLE=0x1000u, HP_CIRCLE=0x2000u, HP_CROSS=0x4000u, HP_SQUARE=0x8000u,
};

/* native/pw_wine_display.c pad_buttons, Create (0x1, Back) left out. */
static const struct { uint32_t dualsense; uint16_t xinput; } hp_buttons[] = {
    { 0x4000u, 0x1000u }, { 0x2000u, 0x2000u }, { 0x8000u, 0x4000u }, { 0x1000u, 0x8000u },
    { 0x400u, 0x0100u }, { 0x800u, 0x0200u }, { 0x2u, 0x0040u }, { 0x4u, 0x0080u },
    { 0x10u, 0x0001u }, { 0x40u, 0x0002u }, { 0x80u, 0x0004u }, { 0x20u, 0x0008u },
    { 0x8u, 0x0010u },
};

/* native/pw_wine_display.c pad_axis: 0..255 (0x80 centred) to XInput's
 * -32768..32767; up is 0 on the pad and positive in XInput. */
static int16_t hp_axis(uint8_t raw, int up)
{
    int32_t d = up ? 0x80 - (int32_t)raw : (int32_t)raw - 0x80;
    int32_t reach = d < 0 ? (up ? 127 : 128) : (up ? 128 : 127);
    return (int16_t)(d * (d < 0 ? 32768 : 32767) / reach);
}

/* What the two nodes have reported so far. */
struct hp_state {
    uint32_t buttons;             /* HP_* without the d-pad */
    int hat_x, hat_y;             /* -1, 0, 1 */
    uint8_t axis[6];              /* lx ly rx ry l2 r2, 0..255 */
    int32_t touch_x, touch_max;   /* the touchpad's x and its range's top */
    int click;                    /* touchpad click held */
    int click_left;               /* the click began on the left half */
    unsigned marks;               /* right-half clicks released */
};

/* The ranges of the pad's six axes, for scaling to 0..255. */
struct hp_ranges { int32_t min[6], max[6]; };

static const uint16_t hp_axis_codes[6] = { ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ };

static void hp_state_init(struct hp_state *s, struct hp_ranges *r)
{
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < 6; i++) { s->axis[i] = i < 4 ? 0x80 : 0; r->min[i] = 0; r->max[i] = 255; }
    s->touch_max = 1919;
}

static uint8_t hp_scale(int32_t v, int32_t lo, int32_t hi)
{
    if (hi <= lo) return 0x80;
    if (v <= lo) return 0;
    if (v >= hi) return 255;
    return (uint8_t)(((int64_t)(v - lo) * 255 + (hi - lo) / 2) / (hi - lo));
}

static uint32_t hp_key_bit(uint16_t code)
{
    switch (code) {
    case BTN_SOUTH: return HP_CROSS;
    case BTN_EAST: return HP_CIRCLE;
    case BTN_NORTH: return HP_TRIANGLE;
    case BTN_WEST: return HP_SQUARE;
    case BTN_TL: return HP_L1;
    case BTN_TR: return HP_R1;
    case BTN_THUMBL: return HP_L3;
    case BTN_THUMBR: return HP_R3;
    case BTN_START: return HP_OPTIONS;
    default: return 0;   /* BTN_TL2/TR2 (the triggers are analogue), BTN_SELECT, BTN_MODE */
    }
}

/* One event from the pad's node. */
static void hp_pad_event(struct hp_state *s, const struct hp_ranges *r, uint16_t type, uint16_t code, int32_t value)
{
    if (type == EV_KEY) {
        uint32_t bit = hp_key_bit(code);
        if (value) s->buttons |= bit; else s->buttons &= ~bit;
    } else if (type == EV_ABS) {
        if (code == ABS_HAT0X) s->hat_x = value < 0 ? -1 : value > 0;
        else if (code == ABS_HAT0Y) s->hat_y = value < 0 ? -1 : value > 0;
        else for (int i = 0; i < 6; i++)
            if (code == hp_axis_codes[i]) s->axis[i] = hp_scale(value, r->min[i], r->max[i]);
    }
}

/* One event from the touchpad's node; 1 when it completed a mark. */
static int hp_touch_event(struct hp_state *s, uint16_t type, uint16_t code, int32_t value)
{
    if (type == EV_ABS && (code == ABS_X || code == ABS_MT_POSITION_X)) s->touch_x = value;
    else if (type == EV_KEY && code == BTN_LEFT) {
        if (value && !s->click) { s->click = 1; s->click_left = s->touch_x <= s->touch_max / 2; }
        else if (!value && s->click) {
            s->click = 0;
            if (!s->click_left) { s->marks++; return 1; }
        }
    }
    return 0;
}

/* The controller as XInput gets it (packet left alone). */
static void hp_to_xinput(const struct hp_state *s, PwWinePad *out)
{
    uint32_t b = s->buttons;
    uint32_t packet = out->packet;
    memset(out, 0, sizeof(*out));
    out->packet = packet;
    out->connected = 1;
    if (s->hat_x < 0) b |= HP_LEFT;
    if (s->hat_x > 0) b |= HP_RIGHT;
    if (s->hat_y < 0) b |= HP_UP;
    if (s->hat_y > 0) b |= HP_DOWN;
    for (size_t i = 0; i < sizeof(hp_buttons) / sizeof(hp_buttons[0]); i++)
        if (b & hp_buttons[i].dualsense) out->buttons |= hp_buttons[i].xinput;
    if (s->click && s->click_left) out->buttons |= 0x0020u;   /* Back */
    out->left_trigger = s->axis[4];
    out->right_trigger = s->axis[5];
    out->thumb_lx = hp_axis(s->axis[0], 0);
    out->thumb_ly = hp_axis(s->axis[1], 1);
    out->thumb_rx = hp_axis(s->axis[2], 0);
    out->thumb_ry = hp_axis(s->axis[3], 1);
}

/* Bumps *packet (never to 0) when the state differs from the last one. */
static int hp_update_packet(const PwWinePad *now, PwWinePad *last)
{
    PwWinePad a = *now, b = *last;
    a.packet = b.packet = 0;
    if (!memcmp(&a, &b, sizeof(a))) return 0;
    uint32_t packet = last->packet + 1;
    if (!packet) packet = 1;
    *last = *now;
    last->packet = packet;
    return 1;
}

/* ---- devices and the exported functions ---- */

static pthread_mutex_t hp_lock = PTHREAD_MUTEX_INITIALIZER;
static struct hp_state hp_s;
static struct hp_ranges hp_r;
static PwWinePad hp_last;
static int hp_started, hp_pad_fd = -1, hp_touch_fd = -1, hp_rumble_ok, hp_effect = -1;
static FILE *hp_replay;
static char hp_pad_path[288], hp_touch_path[288], hp_name[128];
static uint64_t hp_load_ms, hp_first_read_ms, hp_reads, hp_connected_reads, hp_packets, hp_rumble_events;
static uint64_t hp_last_scan_ms, hp_last_progress_ms;
static uint32_t hp_rumble_left, hp_rumble_right;

static uint64_t hp_now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

__attribute__((constructor)) static void hp_loaded(void) { hp_load_ms = hp_now_ms(); }

/* Whether this process is the one that serves the pad. */
static int hp_is_target(void)
{
    static int known = -1;
    if (known < 0) {
        const char *name = getenv("PW_SINK_PROCESS");
        char line[4096] = {0};
        FILE *f = fopen("/proc/self/cmdline", "rb");
        size_t n = f ? fread(line, 1, sizeof(line) - 1, f) : 0;
        if (f) fclose(f);
        for (size_t i = 0; i < n; i++) if (!line[i]) line[i] = ' ';
        known = !name || !*name || strstr(line, name) != NULL;
    }
    return known;
}

/* A Sony controller's pad (want_touch 0) or touchpad (1) node, or "". */
static void hp_find(int want_touch, char *path, size_t size)
{
    DIR *d = opendir("/dev/input");
    struct dirent *e;
    char best[288] = "";
    path[0] = 0;
    if (!d) return;
    while ((e = readdir(d))) {
        char p[288], name[128] = "";
        struct input_id id;
        int fd;
        if (strncmp(e->d_name, "event", 5)) continue;
        snprintf(p, sizeof(p), "/dev/input/%s", e->d_name);
        if ((fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC)) < 0) continue;
        if (!ioctl(fd, EVIOCGID, &id) && id.vendor == 0x054c && ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 &&
            !strstr(name, "Motion Sensors") && (strstr(name, "Touchpad") != NULL) == want_touch &&
            (!best[0] || strverscmp(p, best) < 0))
            snprintf(best, sizeof(best), "%s", p);
        close(fd);
    }
    closedir(d);
    snprintf(path, size, "%s", best);
}

static void hp_say(const char *what)
{
    fprintf(stderr, "host_pad %s reads=%llu connected_reads=%llu first_read_ms=%llu packets=%llu rumble_events=%llu "
            "marks=%u device=%s touch=%s rumble=%s\n", what, (unsigned long long)hp_reads,
            (unsigned long long)hp_connected_reads, (unsigned long long)hp_first_read_ms,
            (unsigned long long)hp_packets, (unsigned long long)hp_rumble_events, hp_s.marks,
            hp_replay ? "replay" : hp_pad_path[0] ? hp_pad_path : "-", hp_touch_path[0] ? hp_touch_path : "-",
            hp_rumble_ok ? "ok" : "unavailable");
    fflush(stderr);
}

/* The device's current keys and axes, after opening or SYN_DROPPED. */
static void hp_resync(void)
{
    unsigned long keys[KEY_MAX / (8 * sizeof(unsigned long)) + 1] = {0};
    static const uint16_t codes[] = { BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR, BTN_THUMBL,
                                      BTN_THUMBR, BTN_START };
    struct input_absinfo a;
    if (hp_pad_fd < 0) return;
    if (ioctl(hp_pad_fd, EVIOCGKEY(sizeof(keys)), keys) >= 0)
        for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++)
            hp_pad_event(&hp_s, &hp_r, EV_KEY, codes[i], !!(keys[codes[i] / (8 * sizeof(long))] &
                                                          (1ul << (codes[i] % (8 * sizeof(long))))));
    for (int i = 0; i < 6; i++)
        if (!ioctl(hp_pad_fd, EVIOCGABS(hp_axis_codes[i]), &a)) {
            hp_r.min[i] = a.minimum; hp_r.max[i] = a.maximum;
            hp_pad_event(&hp_s, &hp_r, EV_ABS, hp_axis_codes[i], a.value);
        }
    if (!ioctl(hp_pad_fd, EVIOCGABS(ABS_HAT0X), &a)) hp_pad_event(&hp_s, &hp_r, EV_ABS, ABS_HAT0X, a.value);
    if (!ioctl(hp_pad_fd, EVIOCGABS(ABS_HAT0Y), &a)) hp_pad_event(&hp_s, &hp_r, EV_ABS, ABS_HAT0Y, a.value);
}

static void hp_close(void)
{
    if (hp_pad_fd >= 0) close(hp_pad_fd);
    if (hp_touch_fd >= 0) close(hp_touch_fd);
    hp_pad_fd = hp_touch_fd = -1;
    hp_effect = -1;
    hp_rumble_ok = 0;
}

/* Opens the nodes (named or found); 1 when the pad is open. */
static int hp_open(void)
{
    const char *pad = getenv("PW_HOST_PAD"), *touch = getenv("PW_HOST_PAD_TOUCH");
    struct input_absinfo a;
    hp_last_scan_ms = hp_now_ms();
    if (pad && *pad) snprintf(hp_pad_path, sizeof(hp_pad_path), "%s", pad);
    else hp_find(0, hp_pad_path, sizeof(hp_pad_path));
    if (touch && !strcmp(touch, "none")) hp_touch_path[0] = 0;
    else if (touch && *touch) snprintf(hp_touch_path, sizeof(hp_touch_path), "%s", touch);
    else hp_find(1, hp_touch_path, sizeof(hp_touch_path));
    if (!hp_pad_path[0]) return 0;
    hp_pad_fd = open(hp_pad_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    hp_rumble_ok = hp_pad_fd >= 0;
    if (hp_pad_fd < 0) hp_pad_fd = open(hp_pad_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (hp_pad_fd < 0) {
        fprintf(stderr, "host_pad cannot open %s: %s\n", hp_pad_path, strerror(errno));
        return 0;
    }
    hp_name[0] = 0;
    ioctl(hp_pad_fd, EVIOCGNAME(sizeof(hp_name)), hp_name);
    if (hp_touch_path[0] && (hp_touch_fd = open(hp_touch_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC)) >= 0 &&
        !ioctl(hp_touch_fd, EVIOCGABS(ABS_X), &a) && a.maximum > 0)
        hp_s.touch_max = a.maximum;
    hp_resync();
    fprintf(stderr, "host_pad open pad=%s name=\"%s\" touch=%s touch_max=%d rumble=%s\n", hp_pad_path, hp_name,
            hp_touch_fd >= 0 ? hp_touch_path : "-", hp_s.touch_max, hp_rumble_ok ? "ok" : "unavailable");
    return 1;
}

static void hp_mark(void)
{
    fprintf(stderr, "host_pad mark %u t=%.1f\n", hp_s.marks, (hp_now_ms() - hp_load_ms) / 1000.0);
}

/* Drains one node; -1 when it failed (unplugged). */
static int hp_drain(int fd, int touch)
{
    struct input_event ev[64];
    static int dropped;
    for (;;) {
        ssize_t n = read(fd, ev, sizeof(ev));
        if (n < 0) return errno == EAGAIN || errno == EINTR ? 0 : -1;
        if (n == 0) return -1;
        for (size_t i = 0; i < (size_t)n / sizeof(ev[0]); i++) {
            if (touch) { if (hp_touch_event(&hp_s, ev[i].type, ev[i].code, ev[i].value)) hp_mark(); continue; }
            if (ev[i].type == EV_SYN && ev[i].code == SYN_DROPPED) { dropped = 1; continue; }
            if (dropped) { if (ev[i].type == EV_SYN && ev[i].code == SYN_REPORT) { dropped = 0; hp_resync(); } continue; }
            hp_pad_event(&hp_s, &hp_r, ev[i].type, ev[i].code, ev[i].value);
        }
    }
}

/* Replays up to and including the next pad SYN_REPORT; 0 at the end. */
static int hp_replay_step(void)
{
    char line[256], node;
    unsigned type, code;
    int value;
    while (fgets(line, sizeof(line), hp_replay)) {
        if (sscanf(line, " %c %u %u %d", &node, &type, &code, &value) != 4) continue;
        if (node == 't') { if (hp_touch_event(&hp_s, type, code, value)) hp_mark(); }
        else if (node == 'p') {
            if (type == EV_SYN && code == SYN_REPORT) return 1;
            hp_pad_event(&hp_s, &hp_r, type, code, value);
        }
    }
    return 0;
}

static void hp_start(void)
{
    const char *replay = getenv("PW_HOST_PAD_EVENTS");
    hp_started = 1;
    hp_state_init(&hp_s, &hp_r);
    if (replay && *replay) {
        if (!(hp_replay = fopen(replay, "r"))) fprintf(stderr, "host_pad cannot open %s: %s\n", replay, strerror(errno));
        else fprintf(stderr, "host_pad open replay=%s\n", replay);
    } else if (!hp_open()) fprintf(stderr, "host_pad no pad found: controller 0 reads disconnected\n");
}

HP_EXPORT int pw_wine_pad_at(uint32_t index, PwWinePad *pad)
{
    int connected;
    uint64_t now;
    if (!pad) return 0;
    memset(pad, 0, sizeof(*pad));
    if (index != 0 || !hp_is_target()) return 0;
    pthread_mutex_lock(&hp_lock);
    now = hp_now_ms();
    if (!hp_started) { hp_start(); hp_first_read_ms = now - hp_load_ms; hp_last_progress_ms = now; }
    hp_reads++;
    if (hp_replay) { hp_replay_step(); connected = 1; }
    else {
        if (hp_pad_fd < 0 && now - hp_last_scan_ms >= 1000) hp_open();
        if (hp_pad_fd >= 0 && hp_drain(hp_pad_fd, 0) < 0) {
            fprintf(stderr, "host_pad lost %s\n", hp_pad_path);
            hp_close();
            hp_state_init(&hp_s, &hp_r);
        }
        if (hp_touch_fd >= 0 && hp_drain(hp_touch_fd, 1) < 0) { close(hp_touch_fd); hp_touch_fd = -1; }
        connected = hp_pad_fd >= 0;
    }
    if (connected) {
        PwWinePad now_pad = {0};
        hp_to_xinput(&hp_s, &now_pad);
        hp_packets += hp_update_packet(&now_pad, &hp_last);
        *pad = hp_last;
        hp_connected_reads++;
    }
    if (now - hp_last_progress_ms >= 30000) { hp_last_progress_ms = now; hp_say("progress"); }
    pthread_mutex_unlock(&hp_lock);
    return connected;
}

HP_EXPORT void pw_wine_set_rumble_at(uint32_t index, uint32_t left, uint32_t right)
{
    struct ff_effect fx;
    struct input_event play;
    if (index != 0 || !hp_is_target()) return;
    if (left > 0xffffu) left = 0xffffu;
    if (right > 0xffffu) right = 0xffffu;
    pthread_mutex_lock(&hp_lock);
    if (left != hp_rumble_left || right != hp_rumble_right) {
        hp_rumble_left = left; hp_rumble_right = right;
        hp_rumble_events++;
        if (hp_pad_fd >= 0 && hp_rumble_ok) {
            memset(&fx, 0, sizeof(fx));
            fx.type = FF_RUMBLE;
            fx.id = (int16_t)hp_effect;
            fx.u.rumble.strong_magnitude = (uint16_t)left;
            fx.u.rumble.weak_magnitude = (uint16_t)right;
            if (ioctl(hp_pad_fd, EVIOCSFF, &fx) < 0) {
                fprintf(stderr, "host_pad rumble refused: %s\n", strerror(errno));
                hp_rumble_ok = 0;
            } else {
                hp_effect = fx.id;
                memset(&play, 0, sizeof(play));
                play.type = EV_FF; play.code = (uint16_t)hp_effect; play.value = (left || right) ? 1 : 0;
                if (write(hp_pad_fd, &play, sizeof(play)) != (ssize_t)sizeof(play)) {
                    fprintf(stderr, "host_pad rumble write failed: %s\n", strerror(errno));
                    hp_rumble_ok = 0;
                }
            }
        }
    }
    pthread_mutex_unlock(&hp_lock);
}

__attribute__((destructor)) static void hp_unloaded(void)
{
    if (!hp_started) return;
    if (hp_pad_fd >= 0 && hp_rumble_ok && (hp_rumble_left || hp_rumble_right)) {
        struct input_event stop = { .type = EV_FF, .code = (uint16_t)hp_effect, .value = 0 };
        if (write(hp_pad_fd, &stop, sizeof(stop)) < 0) {}
    }
    hp_say("summary");
}

#ifdef HOST_PAD_MONITOR
#include <signal.h>

static volatile sig_atomic_t hp_stop;
static void hp_on_signal(int sig) { (void)sig; hp_stop = 1; }

int main(int argc, char **argv)
{
    PwWinePad pad;
    uint32_t packet = 0;
    double seconds = 0;
    uint64_t start;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) {
            char p[288], t[288];
            hp_find(0, p, sizeof(p)); hp_find(1, t, sizeof(t));
            printf("pad=%s touch=%s\n", p[0] ? p : "-", t[0] ? t : "-");
            return p[0] ? 0 : 1;
        } else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else {
            fprintf(stderr, "usage: host_pad_monitor [--list] [--seconds N]\n"
                    "Prints controller 0 as XInput gets it whenever it changes (PW_HOST_PAD, PW_HOST_PAD_TOUCH,\n"
                    "PW_HOST_PAD_EVENTS as for the preload); --list prints the nodes it would open.\n");
            return !strcmp(argv[i], "--help") || !strcmp(argv[i], "-h") ? 0 : 2;
        }
    }
    signal(SIGINT, hp_on_signal);
    signal(SIGTERM, hp_on_signal);
    start = hp_now_ms();
    while (!hp_stop && (seconds <= 0 || (hp_now_ms() - start) < seconds * 1000)) {
        int connected = pw_wine_pad_at(0, &pad);
        if (connected && pad.packet != packet) {
            packet = pad.packet;
            printf("packet=%u buttons=0x%04x lt=%u rt=%u lx=%d ly=%d rx=%d ry=%d\n", pad.packet, pad.buttons,
                   pad.left_trigger, pad.right_trigger, pad.thumb_lx, pad.thumb_ly, pad.thumb_rx, pad.thumb_ry);
            fflush(stdout);
        }
        if (!connected && hp_replay) break;
        if (hp_replay && feof(hp_replay)) break;
        usleep(8000);
    }
    return 0;
}
#endif
