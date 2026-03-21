#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <linux/input.h>

#include "net.h"  /* GamepadPacket, ButtonMask */

typedef struct EvdevState {
    int      fd;
    uint16_t buttons;
    uint8_t  lt, rt;
    int16_t  lx, ly, rx, ry;
    bool     dirty;
    /* physical axis ranges (from EVIOCGABS at open time) */
    struct input_absinfo ai_lx, ai_ly, ai_rx, ai_ry;
    struct input_absinfo ai_lt, ai_rt;
} EvdevState;

/*
 * Scan /dev/input/event* and return the path of the first gamepad found.
 * Writes into buf (size >= PATH_MAX). Returns buf on success, NULL if none found.
 */
const char *evdev_find_gamepad(char *buf, size_t bufsz);

/* Open the evdev device at path. Returns 0 on success, -1 on error. */
int  evdev_open(EvdevState *s, const char *path);

/* Drain all pending events into s. Sets s->dirty if anything changed. */
void evdev_read_all(EvdevState *s);

/* Pack current state into a wire packet. Clears s->dirty. */
void evdev_pack(const EvdevState *s, GamepadPacket *pkt);

void evdev_close(EvdevState *s);
