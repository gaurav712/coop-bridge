#include "evdev.h"

#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/input.h>

/* ── Gamepad detection ───────────────────────────────────────────────────── */

#define BIT_SET(arr, bit) \
    ((arr)[(bit)/(8*sizeof(unsigned long))] & (1UL << ((bit)%(8*sizeof(unsigned long)))))

static bool fd_is_gamepad(int fd)
{
    unsigned long evbits[(EV_MAX / (8 * sizeof(unsigned long))) + 1];
    memset(evbits, 0, sizeof(evbits));
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0)
        return false;
    if (!BIT_SET(evbits, EV_KEY) || !BIT_SET(evbits, EV_ABS))
        return false;

    unsigned long keybits[(KEY_MAX / (8 * sizeof(unsigned long))) + 1];
    memset(keybits, 0, sizeof(keybits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0)
        return false;

    for (int b = BTN_JOYSTICK; b <= 0x14f; b++)
        if (BIT_SET(keybits, b))
            return true;
    return false;
}

#undef BIT_SET

const char *evdev_find_gamepad(char *buf, size_t bufsz)
{
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
            close(fd);
            closedir(d);
            return buf;
        }
        closedir(d);
    }

    /* Fallback: numerically sorted capability scan */
    d = opendir("/dev/input");
    if (!d) return NULL;

    int nums[256], count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < 256)
        if (strncmp(ent->d_name, "event", 5) == 0)
            nums[count++] = atoi(ent->d_name + 5);
    closedir(d);

    for (int i = 0; i < count - 1; i++)
        for (int j = i + 1; j < count; j++)
            if (nums[j] < nums[i]) { int t = nums[i]; nums[i] = nums[j]; nums[j] = t; }

    for (int i = 0; i < count; i++) {
        snprintf(buf, bufsz, "/dev/input/event%d", nums[i]);
        int fd = open(buf, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        bool is_gp = fd_is_gamepad(fd);
        close(fd);
        if (is_gp) return buf;
    }

    fprintf(stderr, "error: no gamepad found\n");
    return NULL;
}

/* ── Open + describe ─────────────────────────────────────────────────────── */

int evdev_open(const char *path, DeviceMsg *desc)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) { perror("evdev: open"); return -1; }

    memset(desc, 0, sizeof(*desc));
    desc->msg_type = MSG_DEVICE;

    ioctl(fd, EVIOCGNAME(sizeof(desc->name)), desc->name);

    struct input_id ids;
    if (ioctl(fd, EVIOCGID, &ids) == 0) {
        desc->bustype = ids.bustype;
        desc->vendor  = ids.vendor;
        desc->product = ids.product;
        desc->version = ids.version;
    }

    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(desc->key_bits)), desc->key_bits);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(desc->abs_bits)), desc->abs_bits);

    for (int a = 0; a < WIRE_ABS_CNT; a++) {
        if (!(desc->abs_bits[a / 8] & (1 << (a % 8))))
            continue;
        struct input_absinfo ai;
        if (ioctl(fd, EVIOCGABS(a), &ai) == 0) {
            desc->abs[a].minimum    = ai.minimum;
            desc->abs[a].maximum    = ai.maximum;
            desc->abs[a].fuzz       = ai.fuzz;
            desc->abs[a].flat       = ai.flat;
            desc->abs[a].resolution = ai.resolution;
        }
    }

    return fd;
}

/* ── Read events ─────────────────────────────────────────────────────────── */

int evdev_read_batch(int fd, WireEvent *out, int max)
{
    struct input_event ev;
    int n = 0;
    while (n < max && read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        out[n].type  = ev.type;
        out[n].code  = ev.code;
        out[n].value = ev.value;
        n++;
    }
    return n;
}

void evdev_close(int fd)
{
    if (fd >= 0)
        close(fd);
}
