/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* tests/host_pad_evdev.c: the evdev to XInput conversion (every button, the
 * d-pad, stick extremes and the y flip, the triggers, the touchpad halves,
 * the packet rule), the slots it serves, and a replay through the exported
 * pw_wine_pad_at. */
#include "host_pad_evdev.c"
#include <assert.h>

static PwWinePad conv(const struct hp_state *s)
{
    PwWinePad p = {0};
    hp_to_xinput(s, &p);
    return p;
}

static void test_buttons(void)
{
    static const struct { uint16_t code; uint16_t xinput; } map[] = {
        { BTN_SOUTH, 0x1000 }, { BTN_EAST, 0x2000 }, { BTN_WEST, 0x4000 }, { BTN_NORTH, 0x8000 },
        { BTN_TL, 0x0100 }, { BTN_TR, 0x0200 }, { BTN_THUMBL, 0x0040 }, { BTN_THUMBR, 0x0080 },
        { BTN_START, 0x0010 },
    };
    struct hp_state s; struct hp_ranges r;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        hp_state_init(&s, &r);
        hp_pad_event(&s, &r, EV_KEY, map[i].code, 1);
        assert(conv(&s).buttons == map[i].xinput);
        hp_pad_event(&s, &r, EV_KEY, map[i].code, 0);
        assert(conv(&s).buttons == 0);
    }
    /* The system's buttons and the triggers' digital halves never reach the game. */
    hp_state_init(&s, &r);
    hp_pad_event(&s, &r, EV_KEY, BTN_SELECT, 1);
    hp_pad_event(&s, &r, EV_KEY, BTN_MODE, 1);
    hp_pad_event(&s, &r, EV_KEY, BTN_TL2, 1);
    hp_pad_event(&s, &r, EV_KEY, BTN_TR2, 1);
    assert(conv(&s).buttons == 0);
    /* All of them at once: the controller's 0xf3ff less Back and Guide. */
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) hp_pad_event(&s, &r, EV_KEY, map[i].code, 1);
    hp_pad_event(&s, &r, EV_ABS, ABS_HAT0X, -1);
    hp_pad_event(&s, &r, EV_ABS, ABS_HAT0Y, -1);
    assert(conv(&s).buttons == (0xf3ff & ~0x0020 & ~0x000a));
}

static void test_dpad(void)
{
    struct hp_state s; struct hp_ranges r;
    hp_state_init(&s, &r);
    hp_pad_event(&s, &r, EV_ABS, ABS_HAT0Y, -1); assert(conv(&s).buttons == 0x0001);
    hp_pad_event(&s, &r, EV_ABS, ABS_HAT0Y, 1);  assert(conv(&s).buttons == 0x0002);
    hp_pad_event(&s, &r, EV_ABS, ABS_HAT0Y, 0);
    hp_pad_event(&s, &r, EV_ABS, ABS_HAT0X, -1); assert(conv(&s).buttons == 0x0004);
    hp_pad_event(&s, &r, EV_ABS, ABS_HAT0X, 1);  assert(conv(&s).buttons == 0x0008);
    hp_pad_event(&s, &r, EV_ABS, ABS_HAT0Y, 1);  assert(conv(&s).buttons == 0x000a);
    hp_pad_event(&s, &r, EV_ABS, ABS_HAT0X, 0);
    hp_pad_event(&s, &r, EV_ABS, ABS_HAT0Y, 0);  assert(conv(&s).buttons == 0);
}

static void test_axes(void)
{
    struct hp_state s; struct hp_ranges r;
    PwWinePad p;
    hp_state_init(&s, &r);
    p = conv(&s);
    assert(p.connected == 1 && p.thumb_lx == 0 && p.thumb_ly == 0 && p.thumb_rx == 0 && p.thumb_ry == 0);
    assert(p.left_trigger == 0 && p.right_trigger == 0);
    hp_pad_event(&s, &r, EV_ABS, ABS_X, 0);   hp_pad_event(&s, &r, EV_ABS, ABS_Y, 0);
    hp_pad_event(&s, &r, EV_ABS, ABS_RX, 255); hp_pad_event(&s, &r, EV_ABS, ABS_RY, 255);
    p = conv(&s);
    assert(p.thumb_lx == -32768 && p.thumb_ly == 32767);    /* left, up: y up is positive */
    assert(p.thumb_rx == 32767 && p.thumb_ry == -32768);    /* right, down */
    hp_pad_event(&s, &r, EV_ABS, ABS_Z, 255); hp_pad_event(&s, &r, EV_ABS, ABS_RZ, 128);
    p = conv(&s);
    assert(p.left_trigger == 255 && p.right_trigger == 128);
    /* Another range is scaled to 0..255 first. */
    r.min[0] = -32768; r.max[0] = 32767;
    hp_pad_event(&s, &r, EV_ABS, ABS_X, -32768); assert(conv(&s).thumb_lx == -32768);
    hp_pad_event(&s, &r, EV_ABS, ABS_X, 32767);  assert(conv(&s).thumb_lx == 32767);
    assert(hp_scale(5, 0, 0) == 0x80 && hp_scale(-1, 0, 255) == 0 && hp_scale(300, 0, 255) == 255);
}

static void test_touchpad(void)
{
    struct hp_state s; struct hp_ranges r;
    hp_state_init(&s, &r);
    s.touch_max = 1919;
    /* Left half: Back while held, no mark. */
    assert(!hp_touch_event(&s, EV_ABS, ABS_X, 300));
    assert(!hp_touch_event(&s, EV_KEY, BTN_LEFT, 1));
    assert(conv(&s).buttons == 0x0020);
    assert(!hp_touch_event(&s, EV_ABS, ABS_X, 1800));          /* the side is the one it began on */
    assert(conv(&s).buttons == 0x0020);
    assert(!hp_touch_event(&s, EV_KEY, BTN_LEFT, 0));
    assert(conv(&s).buttons == 0 && s.marks == 0);
    /* Right half: never reaches the game, one mark on release. */
    assert(!hp_touch_event(&s, EV_ABS, ABS_MT_POSITION_X, 1500));
    assert(!hp_touch_event(&s, EV_KEY, BTN_LEFT, 1));
    assert(conv(&s).buttons == 0);
    assert(hp_touch_event(&s, EV_KEY, BTN_LEFT, 0) == 1);
    assert(s.marks == 1 && conv(&s).buttons == 0);
    /* The middle belongs to the left half; a repeated release is no second mark. */
    assert(!hp_touch_event(&s, EV_ABS, ABS_X, 959));
    assert(!hp_touch_event(&s, EV_KEY, BTN_LEFT, 1));
    assert(conv(&s).buttons == 0x0020);
    assert(!hp_touch_event(&s, EV_KEY, BTN_LEFT, 0));
    assert(!hp_touch_event(&s, EV_KEY, BTN_LEFT, 0));
    assert(s.marks == 1);
}

static void test_packet(void)
{
    PwWinePad last = {0}, now = {0};
    now.connected = 1;
    assert(hp_update_packet(&now, &last) == 1 && last.packet == 1);
    now.packet = 77;                                   /* the packet itself is no change */
    assert(hp_update_packet(&now, &last) == 0 && last.packet == 1);
    now.buttons = 0x1000;
    assert(hp_update_packet(&now, &last) == 1 && last.packet == 2 && last.buttons == 0x1000);
    last.packet = 0xffffffffu;
    now.buttons = 0;
    assert(hp_update_packet(&now, &last) == 1 && last.packet == 1);   /* never 0 */
}

static void test_replay(void)
{
    char path[] = "/tmp/test_host_pad_evdev_XXXXXX";
    int fd = mkstemp(path);
    FILE *f;
    PwWinePad p;
    assert(fd >= 0);
    f = fdopen(fd, "w");
    fprintf(f, "# cross, then the left stick up, then a right-half touchpad click\n"
               "p 1 %d 1\np 0 0 0\n"
               "p 1 %d 0\np 3 %d 0\np 0 0 0\n"
               "t 3 %d 1700\nt 1 %d 1\nt 1 %d 0\np 0 0 0\n",
            BTN_SOUTH, BTN_SOUTH, ABS_Y, ABS_X, BTN_LEFT, BTN_LEFT);
    fclose(f);
    setenv("PW_HOST_PAD_EVENTS", path, 1);
    unsetenv("PW_SINK_PROCESS");
    /* Controllers 1-3 are not served and leave the replay alone. */
    memset(&p, 0xab, sizeof(p));
    assert(pw_wine_pad_at(1, &p) == 0 && p.connected == 0 && p.buttons == 0);
    assert(pw_wine_pad_at(3, &p) == 0);
    assert(pw_wine_pad_at(0, NULL) == 0);
    assert(pw_wine_pad_at(0, &p) == 1 && p.connected == 1 && p.buttons == 0x1000 && p.packet == 1);
    assert(pw_wine_pad_at(0, &p) == 1 && p.buttons == 0 && p.thumb_ly == 32767 && p.packet == 2);
    assert(pw_wine_pad_at(0, &p) == 1 && p.buttons == 0 && p.packet == 2 && hp_s.marks == 1);
    assert(pw_wine_pad_at(0, &p) == 1 && p.packet == 2);         /* end of file: the last state stays */
    assert(hp_reads == 4 && hp_connected_reads == 4 && hp_packets == 2);
    /* Rumble without a device is counted on change only. */
    pw_wine_set_rumble_at(0, 0x10000, 100);
    pw_wine_set_rumble_at(0, 0xffff, 100);
    pw_wine_set_rumble_at(2, 1, 1);
    pw_wine_set_rumble_at(0, 0, 0);
    assert(hp_rumble_events == 2 && hp_rumble_left == 0);
    unlink(path);
}

int main(void)
{
    test_buttons();
    test_dpad();
    test_axes();
    test_touchpad();
    test_packet();
    test_replay();
    puts("test_host_pad_evdev: ok");
    return 0;
}
