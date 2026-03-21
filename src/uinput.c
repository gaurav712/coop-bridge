#include "uinput.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <linux/uinput.h>
#include <linux/input.h>

/* Button codes for the virtual device, in the same order as ButtonMask bits */
static const uint16_t BTN_CODES[] = {
    BTN_SOUTH,   /* A       bit 0  */
    BTN_EAST,    /* B       bit 1  */
    BTN_NORTH,   /* X       bit 2  */
    BTN_WEST,    /* Y       bit 3  */
    BTN_TL,      /* LB      bit 4  */
    BTN_TR,      /* RB      bit 5  */
    BTN_SELECT,  /* Select  bit 6  */
    BTN_START,   /* Start   bit 7  */
    BTN_THUMBL,  /* L3      bit 8  */
    BTN_THUMBR,  /* R3      bit 9  */
    /* D-pad bits 10-13 are handled via ABS_HAT0X/Y, not key events */
};
#define BTN_CODES_LEN ((int)(sizeof(BTN_CODES) / sizeof(BTN_CODES[0])))

typedef struct { uint16_t code; int32_t min; int32_t max; int32_t fuzz; int32_t flat; } AxisDef;

static const AxisDef AXES[] = {
    { ABS_X,      -32768, 32767, 16,  128 },
    { ABS_Y,      -32768, 32767, 16,  128 },
    { ABS_RX,     -32768, 32767, 16,  128 },
    { ABS_RY,     -32768, 32767, 16,  128 },
    { ABS_Z,      0,      255,   0,   0   },  /* LT */
    { ABS_RZ,     0,      255,   0,   0   },  /* RT */
    { ABS_HAT0X,  -1,     1,     0,   0   },  /* d-pad H */
    { ABS_HAT0Y,  -1,     1,     0,   0   },  /* d-pad V */
};
#define AXES_LEN ((int)(sizeof(AXES) / sizeof(AXES[0])))

static void emit(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type  = type;
    ev.code  = code;
    ev.value = value;
    if (write(fd, &ev, sizeof(ev)) < 0)
        perror("uinput: write event");
}

int uinput_create(UinputDev *dev)
{
    dev->fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (dev->fd < 0) {
        perror("uinput: open /dev/uinput");
        fprintf(stderr, "Hint: ensure you are in the 'input' group and the udev rule is installed.\n");
        return -1;
    }

    /* Event types */
    ioctl(dev->fd, UI_SET_EVBIT, EV_KEY);
    ioctl(dev->fd, UI_SET_EVBIT, EV_ABS);
    ioctl(dev->fd, UI_SET_EVBIT, EV_SYN);

    /* Buttons */
    for (int i = 0; i < BTN_CODES_LEN; i++)
        ioctl(dev->fd, UI_SET_KEYBIT, (int)BTN_CODES[i]);

    /* Axis enable */
    for (int i = 0; i < AXES_LEN; i++)
        ioctl(dev->fd, UI_SET_ABSBIT, (int)AXES[i].code);

    /* Device metadata */
    struct uinput_setup setup;
    memset(&setup, 0, sizeof(setup));
    setup.id.bustype = BUS_USB;
    setup.id.vendor  = 0x045e;   /* Microsoft (cosmetic) */
    setup.id.product = 0x028e;   /* Xbox 360 controller  */
    setup.id.version = 1;
    strncpy(setup.name, "coop-bridge Remote Gamepad", UINPUT_MAX_NAME_SIZE - 1);
    ioctl(dev->fd, UI_DEV_SETUP, &setup);

    /* Axis ranges — must be set before UI_DEV_CREATE */
    for (int i = 0; i < AXES_LEN; i++) {
        struct uinput_abs_setup abs;
        memset(&abs, 0, sizeof(abs));
        abs.code              = AXES[i].code;
        abs.absinfo.minimum   = AXES[i].min;
        abs.absinfo.maximum   = AXES[i].max;
        abs.absinfo.fuzz      = AXES[i].fuzz;
        abs.absinfo.flat      = AXES[i].flat;
        abs.absinfo.resolution = 0;
        ioctl(dev->fd, UI_ABS_SETUP, &abs);
    }

    if (ioctl(dev->fd, UI_DEV_CREATE) < 0) {
        perror("uinput: UI_DEV_CREATE");
        close(dev->fd);
        dev->fd = -1;
        return -1;
    }

    return 0;
}

/* Button mask bits 0-9 map directly to BTN_CODES[].
   Bits 10-13 are d-pad, handled separately via hat axes. */
void uinput_write_packet(UinputDev *dev, const GamepadPacket *pkt)
{
    /* Buttons */
    for (int i = 0; i < BTN_CODES_LEN; i++)
        emit(dev->fd, EV_KEY, BTN_CODES[i], (pkt->buttons >> i) & 1);

    /* Analog axes */
    emit(dev->fd, EV_ABS, ABS_X,  pkt->lx);
    emit(dev->fd, EV_ABS, ABS_Y,  pkt->ly);
    emit(dev->fd, EV_ABS, ABS_RX, pkt->rx);
    emit(dev->fd, EV_ABS, ABS_RY, pkt->ry);
    emit(dev->fd, EV_ABS, ABS_Z,  pkt->lt);
    emit(dev->fd, EV_ABS, ABS_RZ, pkt->rt);

    /* D-pad: bitmask → hat axes */
    int hx = (pkt->buttons & BTN_MASK_DPAD_R) ?  1 :
             (pkt->buttons & BTN_MASK_DPAD_L) ? -1 : 0;
    int hy = (pkt->buttons & BTN_MASK_DPAD_D) ?  1 :
             (pkt->buttons & BTN_MASK_DPAD_U) ? -1 : 0;
    emit(dev->fd, EV_ABS, ABS_HAT0X, hx);
    emit(dev->fd, EV_ABS, ABS_HAT0Y, hy);

    /* Sync — tells kernel the report is complete */
    emit(dev->fd, EV_SYN, SYN_REPORT, 0);
}

void uinput_destroy(UinputDev *dev)
{
    if (dev->fd >= 0) {
        ioctl(dev->fd, UI_DEV_DESTROY);
        close(dev->fd);
        dev->fd = -1;
    }
}
