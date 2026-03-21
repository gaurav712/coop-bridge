#include "uinput.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <linux/uinput.h>
#include <linux/input.h>

int uinput_create(UinputDev *dev, const DeviceMsg *desc)
{
    dev->fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (dev->fd < 0) {
        perror("uinput: open /dev/uinput");
        return -1;
    }

    /* Enable event types that the remote device has */
    for (int k = 0; k < WIRE_KEY_BYTES * 8; k++)
        if (desc->key_bits[k / 8] & (1 << (k % 8))) {
            ioctl(dev->fd, UI_SET_EVBIT, EV_KEY);
            break;
        }
    for (int a = 0; a < WIRE_ABS_CNT; a++)
        if (desc->abs_bits[a / 8] & (1 << (a % 8))) {
            ioctl(dev->fd, UI_SET_EVBIT, EV_ABS);
            break;
        }
    ioctl(dev->fd, UI_SET_EVBIT, EV_SYN);

    /* Register each button */
    for (int k = 0; k < WIRE_KEY_BYTES * 8; k++)
        if (desc->key_bits[k / 8] & (1 << (k % 8)))
            ioctl(dev->fd, UI_SET_KEYBIT, k);

    /* Register each abs axis */
    for (int a = 0; a < WIRE_ABS_CNT; a++)
        if (desc->abs_bits[a / 8] & (1 << (a % 8)))
            ioctl(dev->fd, UI_SET_ABSBIT, a);

    /* Device metadata — same name and IDs as the remote's physical device */
    struct uinput_setup setup;
    memset(&setup, 0, sizeof(setup));
    setup.id.bustype = desc->bustype;
    setup.id.vendor  = desc->vendor;
    setup.id.product = desc->product;
    setup.id.version = desc->version;
    memcpy(setup.name, desc->name, sizeof(setup.name) - 1);
    setup.name[sizeof(setup.name) - 1] = '\0';
    ioctl(dev->fd, UI_DEV_SETUP, &setup);

    /* Axis ranges */
    for (int a = 0; a < WIRE_ABS_CNT; a++) {
        if (!(desc->abs_bits[a / 8] & (1 << (a % 8))))
            continue;
        struct uinput_abs_setup abs;
        memset(&abs, 0, sizeof(abs));
        abs.code                = (uint16_t)a;
        abs.absinfo.minimum     = desc->abs[a].minimum;
        abs.absinfo.maximum     = desc->abs[a].maximum;
        abs.absinfo.fuzz        = desc->abs[a].fuzz;
        abs.absinfo.flat        = desc->abs[a].flat;
        abs.absinfo.resolution  = desc->abs[a].resolution;
        ioctl(dev->fd, UI_ABS_SETUP, &abs);
    }

    if (ioctl(dev->fd, UI_DEV_CREATE) < 0) {
        perror("uinput: UI_DEV_CREATE");
        close(dev->fd);
        dev->fd = -1;
        return -1;
    }

    fprintf(stderr, "[uinput] created virtual device: %s\n", desc->name);
    return 0;
}

void uinput_write_events(UinputDev *dev, const WireEvent *events, int count)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    for (int i = 0; i < count; i++) {
        ev.type  = events[i].type;
        ev.code  = events[i].code;
        ev.value = events[i].value;
        write(dev->fd, &ev, sizeof(ev));
    }
}

void uinput_destroy(UinputDev *dev)
{
    if (dev->fd >= 0) {
        ioctl(dev->fd, UI_DEV_DESTROY);
        close(dev->fd);
        dev->fd = -1;
    }
}
