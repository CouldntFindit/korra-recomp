/* input_android.c - touch to a virtual Xbox 360 pad.
 *
 * The engine polls XInput (import ord 2) and remaps the XINPUT_GAMEPAD into its
 * own 0x24-byte pad state (docs/quirks.md section 5), so feeding the import is
 * the only correct place: the engine's edge detection at 0xe4e554/558/55c then
 * works unchanged.
 *
 * Layout (landscape, normalised 0..1):
 *   left third, lower two thirds   left stick (drag from where the finger lands)
 *   right third, lower two thirds  right stick
 *   top-right cluster              A B X Y as four tap zones
 *   top-left                       START (menu confirm / pause)
 *   left edge, upper third         BACK
 *
 * Taps are level signals: a finger down in a button zone holds the button until
 * it lifts, which is what the engine's edge detector expects.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "host_android.h"

#define PAD_A      0x1000
#define PAD_B      0x2000
#define PAD_X      0x4000
#define PAD_Y      0x8000
#define PAD_START  0x0010
#define PAD_BACK   0x0020

typedef struct {
    uint32_t buttons;
    uint8_t  lt, rt;
    int16_t  lx, ly, rx, ry;
    uint32_t packet;
} padstate;

static padstate g_pad;
static uint32_t g_packet;

/* per-finger anchors for the sticks */
typedef struct { int active; int which; float origin_x, origin_y; } finger;
static finger g_fingers[8];

uint32_t ka_hat_packet(void) { return g_packet; }

void ka_pad_read(ka_pad_state *out) {
    if (!out) return;
    out->buttons = g_pad.buttons;
    out->lt = g_pad.lt; out->rt = g_pad.rt;
    out->lx = g_pad.lx; out->ly = g_pad.ly;
    out->rx = g_pad.rx; out->ry = g_pad.ry;
}

static int16_t to_axis(float v) {
    if (v > 1.f) v = 1.f;
    if (v < -1.f) v = -1.f;
    return (int16_t)(v * 32767.f);
}

/* Called by the GameActivity glue for every motion event. action: 0 = down,
 * 1 = move, 2 = up (Android's AMOTION_EVENT_ACTION_* collapsed). */
void ka_input_touch(int action, int id, float x, float y, int w, int h) {
    float nx = w > 0 ? x / (float)w : 0.f;
    float ny = h > 0 ? y / (float)h : 0.f;
    uint32_t before = g_pad.buttons;

    if (id < 0 || id >= 8) return;

    if (action == 0) {
        if (nx < 0.33f && ny > 0.33f) {
            g_fingers[id].active = 1; g_fingers[id].which = 1;
            g_fingers[id].origin_x = nx; g_fingers[id].origin_y = ny;
        } else if (nx > 0.67f && ny > 0.33f) {
            g_fingers[id].active = 1; g_fingers[id].which = 2;
            g_fingers[id].origin_x = nx; g_fingers[id].origin_y = ny;
        } else if (ny < 0.33f && nx < 0.12f) {
            g_fingers[id].active = 1; g_fingers[id].which = 5;
            g_pad.buttons |= PAD_BACK;
        } else if (ny < 0.33f && nx < 0.30f) {
            g_fingers[id].active = 1; g_fingers[id].which = 6;
            g_pad.buttons |= PAD_START;
        } else if (ny < 0.45f && nx > 0.75f) {
            /* right-top: four quadrants of a 2x2 cluster */
            g_fingers[id].active = 1; g_fingers[id].which = 3;
            if (ny < 0.22f) g_pad.buttons |= (nx < 0.87f ? PAD_Y : PAD_B);
            else            g_pad.buttons |= (nx < 0.87f ? PAD_X : PAD_A);
        } else {
            /* Anywhere else is A: the menus and the "press to continue" prompts. */
            g_fingers[id].active = 1; g_fingers[id].which = 4;
            g_pad.buttons |= PAD_A;
        }
    } else if (action == 1) {
        if (!g_fingers[id].active) return;
        if (g_fingers[id].which == 1) {
            g_pad.lx = to_axis((nx - g_fingers[id].origin_x) * 2.5f);
            g_pad.ly = to_axis(-(ny - g_fingers[id].origin_y) * 2.5f);
        } else if (g_fingers[id].which == 2) {
            g_pad.rx = to_axis((nx - g_fingers[id].origin_x) * 2.5f);
            g_pad.ry = to_axis(-(ny - g_fingers[id].origin_y) * 2.5f);
        } else if (g_fingers[id].which == 3) {
            /* sliding inside the cluster changes the button */
            g_pad.buttons &= ~(PAD_A | PAD_B | PAD_X | PAD_Y);
            if (ny < 0.22f) g_pad.buttons |= (nx < 0.87f ? PAD_Y : PAD_B);
            else            g_pad.buttons |= (nx < 0.87f ? PAD_X : PAD_A);
        }
    } else {
        int which = g_fingers[id].active ? g_fingers[id].which : 0;
        g_fingers[id].active = 0;
        g_fingers[id].which = 0;
        switch (which) {
        case 1: g_pad.lx = g_pad.ly = 0; break;
        case 2: g_pad.rx = g_pad.ry = 0; break;
        case 3: g_pad.buttons &= ~(PAD_A | PAD_B | PAD_X | PAD_Y); break;
        case 4: g_pad.buttons &= ~PAD_A; break;
        case 5: g_pad.buttons &= ~PAD_BACK; break;
        case 6: g_pad.buttons &= ~PAD_START; break;
        default: break;
        }
    }

    if (g_pad.buttons != before) g_packet++;
}

uint32_t ka_mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull);
}
