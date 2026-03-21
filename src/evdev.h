#pragma once

#include <stdbool.h>
#include <linux/limits.h>
#include "net.h"

/* Scan /dev/input/ and return path of first gamepad. buf must be PATH_MAX. */
const char *evdev_find_gamepad(char *buf, size_t bufsz);

/* Open device, fill desc with its capabilities. Returns fd >= 0 or -1. */
int evdev_open(const char *path, DeviceMsg *desc);

/* Read pending events into out[]. Returns count (0 if none). */
int evdev_read_batch(int fd, WireEvent *out, int max);

void evdev_close(int fd);
