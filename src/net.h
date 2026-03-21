#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <libwebsockets.h>
#include <linux/input.h>
#include <linux/uinput.h>

/* ── Wire event (8 bytes, no timestamp) ──────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint16_t type;
    uint16_t code;
    int32_t  value;
} WireEvent;

/* ── Message types ───────────────────────────────────────────────────────── */

#define MSG_DEVICE  0x01   /* sent once on connect: describes the local gamepad */
#define MSG_EVENTS  0x02   /* batch of raw input events */

#define MAX_BATCH   64     /* max WireEvents per message */

/* Device description — mirrors the physical device's capabilities exactly.
 * Sent by both sides on connect; receiver creates a matching uinput device. */
typedef struct __attribute__((packed)) {
    uint8_t  msg_type;                  /* MSG_DEVICE */
    char     name[UINPUT_MAX_NAME_SIZE];
    uint16_t bustype, vendor, product, version;
    uint8_t  key_bits[(KEY_MAX / 8) + 1];   /* 96 bytes */
    uint8_t  abs_bits[(ABS_MAX / 8) + 1];   /*  8 bytes */
    struct {
        int32_t minimum, maximum, fuzz, flat, resolution;
    } abs[ABS_CNT];                     /* 64 * 20 = 1280 bytes */
} DeviceMsg;

/* ── Forward declarations ─────────────────────────────────────────────────── */

typedef struct UinputDev UinputDev;

/* ── App context ─────────────────────────────────────────────────────────── */

typedef enum { MODE_SERVER, MODE_CLIENT } AppMode;

typedef struct {
    int           evdev_fd;
    DeviceMsg     local_desc;        /* our gamepad's capabilities */
    UinputDev    *remote_vpad;       /* created once we receive remote's DeviceMsg */
    struct lws   *wsi;
    bool          connected;
    bool          want_send_desc;    /* send DeviceMsg on next writable */
    WireEvent     pending[MAX_BATCH];
    int           pending_n;
    AppMode       mode;
    char          host[256];
    int           port;
    time_t        reconnect_after;
} AppCtx;

/* ── Network functions ───────────────────────────────────────────────────── */

struct lws_context *net_create_server(int port, AppCtx *ctx);
struct lws_context *net_create_client(const char *host, int port, AppCtx *ctx);
void net_do_connect(struct lws_context *lws_ctx, AppCtx *ctx);
