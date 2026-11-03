/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* A 32-bit XInput program for the PS4 port's milestone l1: the title's
 * controller 0 as Wine's xinput1_3.dll reads it (patches 0470/0471: the
 * Unix library xinput1_3, loaded by name, reads the title's pad slot in
 * ntdll; on the PS4 that library is xinput1_3.prx).
 *
 * At the start: XInputGetState(0), XInputGetCapabilities(0) and
 * XInputGetState(1) (no second controller: 1167, ERROR_DEVICE_NOT_CONNECTED).
 * Then SECONDS (30, or argv[1]) of polling controller 0 every ~8 ms, in
 * which the user presses every button once, takes both sticks to their
 * extremes and pulls both triggers fully: the OR of the buttons seen, each
 * axis's minimum and maximum, each trigger's maximum and the number of
 * packet-number changes are kept. Then one second of rumble on the left
 * (large) motor only, one second on the right only, and stop. One line on
 * stdout:
 *
 *   XINPUT bits=32 get0=0 caps=0 get1=1167 vid=- pid=- buttons_seen=0xf3ff
 *     lx_min=-32768 lx_max=32767 ... lt_max=255 rt_max=255 packets=N
 *     set0=0 rumble_events=2 seconds=30
 *
 * (one line; wrapped here). bits is the program's pointer width; get0,
 * caps, get1 and set0 are the return codes of the first calls; vid and
 * pid come from XInputGetCapabilitiesEx where the DLL exports it (ordinal
 * 108; xinput1_3 does not: "-"); rumble_events
 * counts the XInputSetState calls that returned 0 with a motor on. Exit 0,
 * or 1 when xinput1_3.dll or its three functions cannot be had (the line
 * then has "-" for every value). Built with i686-w64-mingw32-gcc. */
#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>

#define BITS ((int)(sizeof(void *) * 8))


typedef DWORD (WINAPI *GetStateFn)(DWORD, XINPUT_STATE *);
typedef DWORD (WINAPI *GetCapabilitiesFn)(DWORD, DWORD, XINPUT_CAPABILITIES *);
typedef DWORD (WINAPI *SetStateFn)(DWORD, XINPUT_VIBRATION *);
typedef DWORD (WINAPI *GetCapabilitiesExFn)(DWORD, DWORD, DWORD, XINPUT_CAPABILITIES_EX *);

typedef struct Seen {
    WORD buttons;
    SHORT lx_min, lx_max, ly_min, ly_max, rx_min, rx_max, ry_min, ry_max;
    BYTE lt_max, rt_max;
    DWORD packet, packets;
} Seen;

static void see(Seen *seen, const XINPUT_STATE *state)
{
    const XINPUT_GAMEPAD *pad = &state->Gamepad;

    seen->buttons |= pad->wButtons;
    if (pad->sThumbLX < seen->lx_min) seen->lx_min = pad->sThumbLX;
    if (pad->sThumbLX > seen->lx_max) seen->lx_max = pad->sThumbLX;
    if (pad->sThumbLY < seen->ly_min) seen->ly_min = pad->sThumbLY;
    if (pad->sThumbLY > seen->ly_max) seen->ly_max = pad->sThumbLY;
    if (pad->sThumbRX < seen->rx_min) seen->rx_min = pad->sThumbRX;
    if (pad->sThumbRX > seen->rx_max) seen->rx_max = pad->sThumbRX;
    if (pad->sThumbRY < seen->ry_min) seen->ry_min = pad->sThumbRY;
    if (pad->sThumbRY > seen->ry_max) seen->ry_max = pad->sThumbRY;
    if (pad->bLeftTrigger > seen->lt_max) seen->lt_max = pad->bLeftTrigger;
    if (pad->bRightTrigger > seen->rt_max) seen->rt_max = pad->bRightTrigger;
    if (state->dwPacketNumber != seen->packet) {
        seen->packet = state->dwPacketNumber;
        seen->packets++;
    }
}

/* XInputSetState(0) with these speeds; counts a success with a motor on. */
static DWORD rumble(SetStateFn set_state, WORD left, WORD right, unsigned *events)
{
    XINPUT_VIBRATION vibration;
    DWORD rc;

    vibration.wLeftMotorSpeed = left;
    vibration.wRightMotorSpeed = right;
    rc = set_state(0, &vibration);
    if (rc == ERROR_SUCCESS && (left || right)) (*events)++;
    return rc;
}

int main(int argc, char **argv)
{
    HMODULE xinput = LoadLibraryA("xinput1_3.dll");
    GetStateFn get_state = NULL;
    GetCapabilitiesFn get_capabilities = NULL;
    SetStateFn set_state = NULL;
    GetCapabilitiesExFn get_capabilities_ex = NULL;
    XINPUT_STATE state;
    XINPUT_CAPABILITIES caps;
    XINPUT_CAPABILITIES_EX caps_ex;
    Seen seen;
    DWORD get0, caps_rc, get1, set0, start;
    unsigned seconds = 30, rumble_events = 0;
    char vid[8] = "-", pid[8] = "-";

    if (argc > 1) seconds = (unsigned)strtoul(argv[1], NULL, 10);
    if (xinput) {
        get_state = (GetStateFn)(void *)GetProcAddress(xinput, "XInputGetState");
        get_capabilities = (GetCapabilitiesFn)(void *)GetProcAddress(xinput, "XInputGetCapabilities");
        set_state = (SetStateFn)(void *)GetProcAddress(xinput, "XInputSetState");
        get_capabilities_ex = (GetCapabilitiesExFn)(void *)GetProcAddress(xinput, MAKEINTRESOURCEA(108));
    }
    if (!get_state || !get_capabilities || !set_state) {
        printf("XINPUT bits=%d get0=- caps=- get1=- vid=- pid=- buttons_seen=- lx_min=- lx_max=- ly_min=- ly_max=- "
               "rx_min=- rx_max=- ry_min=- ry_max=- lt_max=- rt_max=- packets=- set0=- rumble_events=- "
               "seconds=- load_error=%lu\n", BITS, (unsigned long)GetLastError());
        fflush(stdout);
        return 1;
    }

    ZeroMemory(&seen, sizeof(seen));
    ZeroMemory(&state, sizeof(state));
    get0 = get_state(0, &state);
    if (get0 == ERROR_SUCCESS) {
        seen.packet = state.dwPacketNumber;
        see(&seen, &state);
    }
    ZeroMemory(&caps, sizeof(caps));
    caps_rc = get_capabilities(0, XINPUT_FLAG_GAMEPAD, &caps);
    ZeroMemory(&caps_ex, sizeof(caps_ex));
    if (get_capabilities_ex && get_capabilities_ex(1, 0, 0, &caps_ex) == ERROR_SUCCESS) {
        snprintf(vid, sizeof(vid), "%04x", caps_ex.VendorId);
        snprintf(pid, sizeof(pid), "%04x", caps_ex.ProductId);
    }
    ZeroMemory(&state, sizeof(state));
    get1 = get_state(1, &state);

    start = GetTickCount();
    while (GetTickCount() - start < seconds * 1000u) {
        ZeroMemory(&state, sizeof(state));
        if (get_state(0, &state) == ERROR_SUCCESS) see(&seen, &state);
        Sleep(8);
    }

    set0 = rumble(set_state, 0xffff, 0, &rumble_events);
    Sleep(1000);
    rumble(set_state, 0, 0xffff, &rumble_events);
    Sleep(1000);
    rumble(set_state, 0, 0, &rumble_events);

    printf("XINPUT bits=%d get0=%lu caps=%lu get1=%lu vid=%s pid=%s buttons_seen=0x%04x lx_min=%d lx_max=%d "
           "ly_min=%d ly_max=%d rx_min=%d rx_max=%d ry_min=%d ry_max=%d lt_max=%u rt_max=%u packets=%lu "
           "set0=%lu rumble_events=%u seconds=%u\n",
           BITS, (unsigned long)get0, (unsigned long)caps_rc, (unsigned long)get1, vid, pid, seen.buttons,
           seen.lx_min, seen.lx_max, seen.ly_min, seen.ly_max, seen.rx_min, seen.rx_max, seen.ry_min,
           seen.ry_max, seen.lt_max, seen.rt_max, (unsigned long)seen.packets, (unsigned long)set0,
           rumble_events, seconds);
    fflush(stdout);
    return 0;
}
