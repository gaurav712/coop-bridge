#pragma once

#include "net.h"  /* GamepadPacket */

typedef struct UinputDev {
    int fd;
} UinputDev;

/* Create the virtual gamepad. Returns 0 on success, -1 on error. */
int  uinput_create(UinputDev *dev);

/* Inject a full gamepad state update. */
void uinput_write_packet(UinputDev *dev, const GamepadPacket *pkt);

void uinput_destroy(UinputDev *dev);
