#include "evdev.h"

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/ioctl.h>

/* ── Auto-detection ───────────────────────────────────────────────────────── */

/* Returns true if the device at fd looks like a gamepad:
 * must have EV_KEY + EV_ABS and at least one button in the BTN_GAMEPAD range
 * (0x130–0x13f) or BTN_JOYSTICK range (0x120–0x12f). */
static bool fd_is_gamepad(int fd)
{
    /* Check event type capabilities */
    unsigned long evbits[(EV_MAX / (8 * sizeof(unsigned long))) + 1];
    memset(evbits, 0, sizeof(evbits));
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0)
        return false;

#define BIT_SET(arr, bit) \
    ((arr)[(bit) / (8*sizeof(unsigned long))] & (1UL << ((bit) % (8*sizeof(unsigned long)))))

    if (!BIT_SET(evbits, EV_KEY) || !BIT_SET(evbits, EV_ABS))
        return false;

    /* Check for gamepad / joystick buttons */
    unsigned long keybits[(KEY_MAX / (8 * sizeof(unsigned long))) + 1];
    memset(keybits, 0, sizeof(keybits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0)
        return false;

    /* BTN_JOYSTICK = 0x120, BTN_GAMEPAD = 0x130; scan 0x120-0x14f */
    for (int b = BTN_JOYSTICK; b <= 0x14f; b++) {
        if (BIT_SET(keybits, b))
            return true;
    }
#undef BIT_SET
    return false;
}

/* Try by-id first (udev symlinks ending in -event-joystick are authoritative),
 * then fall back to a numerically sorted capability scan of /dev/input/event*. */
const char *evdev_find_gamepad(char *buf, size_t bufsz)
{
    char name[256];

    /* Primary: by-id entries ending in -event-joystick */
    DIR *d = opendir("/dev/input/by-id");
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            size_t len = strlen(ent->d_name);
            if (len < 16 || strcmp(ent->d_name + len - 15, "-event-joystick") != 0)
                continue;
            snprintf(buf, bufsz, "/dev/input/by-id/%s", ent->d_name);
            int fd = open(buf, O_RDONLY | O_NONBLOCK);
            if (fd < 0) continue;
            name[0] = '\0';
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            close(fd);
            closedir(d);
            fprintf(stderr, "[evdev] found gamepad: %s (%s)\n", buf, name);
            return buf;
        }
        closedir(d);
    }

    /* Fallback: scan /dev/input/event* sorted numerically */
    d = opendir("/dev/input");
    if (!d) { perror("opendir /dev/input"); return NULL; }

    /* Collect event numbers */
    int nums[256];
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < 256) {
        if (strncmp(ent->d_name, "event", 5) == 0)
            nums[count++] = atoi(ent->d_name + 5);
    }
    closedir(d);

    /* Sort numerically */
    for (int i = 0; i < count - 1; i++)
        for (int j = i + 1; j < count; j++)
            if (nums[j] < nums[i]) { int t = nums[i]; nums[i] = nums[j]; nums[j] = t; }

    for (int i = 0; i < count; i++) {
        snprintf(buf, bufsz, "/dev/input/event%d", nums[i]);
        int fd = open(buf, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        bool is_gp = fd_is_gamepad(fd);
        if (is_gp) {
            name[0] = '\0';
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            fprintf(stderr, "[evdev] found gamepad: %s (%s)\n", buf, name);
        }
        close(fd);
        if (is_gp) return buf;
    }

    fprintf(stderr, "[evdev] no gamepad found\n");
    return NULL;
}

/* Map linux BTN_* codes to our button bitmask positions */
static const struct { uint16_t code; uint16_t mask; } BTN_MAP[] = {
    { BTN_SOUTH,  BTN_MASK_A      },
    { BTN_EAST,   BTN_MASK_B      },
    { BTN_NORTH,  BTN_MASK_X      },
    { BTN_WEST,   BTN_MASK_Y      },
    { BTN_TL,     BTN_MASK_TL     },
    { BTN_TR,     BTN_MASK_TR     },
    { BTN_SELECT, BTN_MASK_SELECT },
    { BTN_START,  BTN_MASK_START  },
    { BTN_THUMBL, BTN_MASK_THUMBL },
    { BTN_THUMBR, BTN_MASK_THUMBR },
    /* Some controllers expose d-pad as keys */
    { BTN_DPAD_UP,    BTN_MASK_DPAD_U },
    { BTN_DPAD_DOWN,  BTN_MASK_DPAD_D },
    { BTN_DPAD_LEFT,  BTN_MASK_DPAD_L },
    { BTN_DPAD_RIGHT, BTN_MASK_DPAD_R },
};
#define BTN_MAP_LEN ((int)(sizeof(BTN_MAP) / sizeof(BTN_MAP[0])))

/* Linear normalise value from [src_min, src_max] to [dst_min, dst_max] */
static int16_t normalise(int32_t v, int32_t src_min, int32_t src_max,
                         int16_t dst_min, int16_t dst_max)
{
    if (src_max == src_min)
        return 0;
    int32_t range_src = src_max - src_min;
    int32_t range_dst = dst_max - dst_min;
    return (int16_t)(dst_min + (int32_t)(v - src_min) * range_dst / range_src);
}

static void query_abs(int fd, int code, struct input_absinfo *ai)
{
    if (ioctl(fd, EVIOCGABS(code), ai) < 0) {
        /* Provide safe defaults if the axis isn't present */
        memset(ai, 0, sizeof(*ai));
        ai->minimum = -32768;
        ai->maximum =  32767;
    }
}

int evdev_open(EvdevState *s, const char *path)
{
    memset(s, 0, sizeof(*s));
    s->fd = open(path, O_RDONLY | O_NONBLOCK);
    if (s->fd < 0) {
        perror("evdev: open");
        return -1;
    }

    query_abs(s->fd, ABS_X,   &s->ai_lx);
    query_abs(s->fd, ABS_Y,   &s->ai_ly);
    query_abs(s->fd, ABS_RX,  &s->ai_rx);
    query_abs(s->fd, ABS_RY,  &s->ai_ry);
    query_abs(s->fd, ABS_Z,   &s->ai_lt);
    query_abs(s->fd, ABS_RZ,  &s->ai_rt);

    return 0;
}

void evdev_read_all(EvdevState *s)
{
    struct input_event ev;

    while (read(s->fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_KEY) {
            for (int i = 0; i < BTN_MAP_LEN; i++) {
                if (ev.code == BTN_MAP[i].code) {
                    if (ev.value)
                        s->buttons |=  BTN_MAP[i].mask;
                    else
                        s->buttons &= ~BTN_MAP[i].mask;
                    s->dirty = true;
                    break;
                }
            }
        } else if (ev.type == EV_ABS) {
            s->dirty = true;
            switch (ev.code) {
            case ABS_X:
                s->lx = normalise(ev.value,
                    s->ai_lx.minimum, s->ai_lx.maximum, -32768, 32767);
                break;
            case ABS_Y:
                s->ly = normalise(ev.value,
                    s->ai_ly.minimum, s->ai_ly.maximum, -32768, 32767);
                break;
            case ABS_RX:
                s->rx = normalise(ev.value,
                    s->ai_rx.minimum, s->ai_rx.maximum, -32768, 32767);
                break;
            case ABS_RY:
                s->ry = normalise(ev.value,
                    s->ai_ry.minimum, s->ai_ry.maximum, -32768, 32767);
                break;
            case ABS_Z:
                s->lt = (uint8_t)normalise(ev.value,
                    s->ai_lt.minimum, s->ai_lt.maximum, 0, 255);
                break;
            case ABS_RZ:
                s->rt = (uint8_t)normalise(ev.value,
                    s->ai_rt.minimum, s->ai_rt.maximum, 0, 255);
                break;
            case ABS_HAT0X:
                s->buttons &= ~(BTN_MASK_DPAD_L | BTN_MASK_DPAD_R);
                if (ev.value < 0) s->buttons |= BTN_MASK_DPAD_L;
                if (ev.value > 0) s->buttons |= BTN_MASK_DPAD_R;
                break;
            case ABS_HAT0Y:
                s->buttons &= ~(BTN_MASK_DPAD_U | BTN_MASK_DPAD_D);
                if (ev.value < 0) s->buttons |= BTN_MASK_DPAD_U;
                if (ev.value > 0) s->buttons |= BTN_MASK_DPAD_D;
                break;
            default:
                break;
            }
        }
        /* EV_SYN: nothing to do */
    }

    if (errno != EAGAIN && errno != EWOULDBLOCK)
        perror("evdev: read");
}

void evdev_pack(const EvdevState *s, GamepadPacket *pkt)
{
    pkt->buttons = s->buttons;
    pkt->lt      = s->lt;
    pkt->rt      = s->rt;
    pkt->lx      = s->lx;
    pkt->ly      = s->ly;
    pkt->rx      = s->rx;
    pkt->ry      = s->ry;
}

void evdev_close(EvdevState *s)
{
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
}
