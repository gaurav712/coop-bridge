#pragma once

#include "net.h"

typedef struct UinputDev {
  int fd;
} UinputDev;

/* Create a virtual device that mirrors the remote's physical gamepad. */
int uinput_create(UinputDev *dev, const DeviceMsg *desc);

/* Write a batch of raw events to the virtual device. */
void uinput_write_events(UinputDev *dev, const WireEvent *events, int count);

void uinput_destroy(UinputDev *dev);
