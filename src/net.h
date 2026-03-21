#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <libwebsockets.h>

/* ── Wire packet ──────────────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint16_t buttons;   /* bitmask, see ButtonMask */
    uint8_t  lt;        /* left trigger  0-255 */
    uint8_t  rt;        /* right trigger 0-255 */
    int16_t  lx;        /* left stick X  -32768..32767 */
    int16_t  ly;        /* left stick Y */
    int16_t  rx;        /* right stick X */
    int16_t  ry;        /* right stick Y */
} GamepadPacket;        /* 12 bytes; little-endian (x86-64 assumed) */

typedef enum {
    BTN_MASK_A      = (1 << 0),
    BTN_MASK_B      = (1 << 1),
    BTN_MASK_X      = (1 << 2),
    BTN_MASK_Y      = (1 << 3),
    BTN_MASK_TL     = (1 << 4),   /* LB */
    BTN_MASK_TR     = (1 << 5),   /* RB */
    BTN_MASK_SELECT = (1 << 6),
    BTN_MASK_START  = (1 << 7),
    BTN_MASK_THUMBL = (1 << 8),   /* L3 */
    BTN_MASK_THUMBR = (1 << 9),   /* R3 */
    BTN_MASK_DPAD_U = (1 << 10),
    BTN_MASK_DPAD_D = (1 << 11),
    BTN_MASK_DPAD_L = (1 << 12),
    BTN_MASK_DPAD_R = (1 << 13),
} ButtonMask;

/* ── Forward declarations ─────────────────────────────────────────────────── */

typedef struct EvdevState EvdevState;
typedef struct UinputDev  UinputDev;

/* ── App context (shared between main loop and LWS callbacks) ─────────────── */

typedef enum { MODE_SERVER, MODE_CLIENT } AppMode;

typedef struct {
    EvdevState    *local;
    UinputDev     *remote_vpad;
    struct lws    *wsi;
    bool           want_write;
    bool           connected;
    GamepadPacket  recv_buf;
    bool           recv_new;
    time_t         last_sent;
    time_t         reconnect_after;
    /* client connection info (kept for reconnect) */
    char           host[256];
    int            port;
    AppMode        mode;
} AppCtx;

/* ── Network functions ────────────────────────────────────────────────────── */

struct lws_context *net_create_server(int port, AppCtx *ctx);
struct lws_context *net_create_client(const char *host, int port, AppCtx *ctx);
void net_do_connect(struct lws_context *lws_ctx, AppCtx *ctx);
