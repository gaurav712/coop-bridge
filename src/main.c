#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <linux/limits.h>

#include "evdev.h"
#include "uinput.h"
#include "net.h"

#define DEFAULT_PORT 7777

static volatile sig_atomic_t running = 1;

static void handle_signal(int sig) { (void)sig; running = 0; }

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s                    server, port %d\n"
        "  %s <HOST>             client, connect to HOST:%d\n"
        "  %s <HOST>:<PORT>      client, custom port\n"
        "  %s --server [PORT]    server, custom port\n"
        "  %s --device PATH      override auto-detected gamepad\n"
        "  %s --test-evdev       print gamepad events, no networking\n"
        "\nGamepad is auto-detected from /dev/input/event*.\n",
        prog, DEFAULT_PORT, prog, DEFAULT_PORT,
        prog, prog, prog, prog);
}

/* ── --test-evdev mode ────────────────────────────────────────────────────── */

static void run_test_evdev(EvdevState *s)
{
    fprintf(stderr, "[test-evdev] fd=%d — Ctrl-C to stop\n", s->fd);
    while (running) {
        evdev_read_all(s);
        if (s->dirty) {
            GamepadPacket pkt;
            evdev_pack(s, &pkt);
            s->dirty = false;
            printf("buttons=0x%04x LT=%3u RT=%3u LX=%6d LY=%6d RX=%6d RY=%6d\n",
                   pkt.buttons, pkt.lt, pkt.rt,
                   pkt.lx, pkt.ly, pkt.rx, pkt.ry);
            fflush(stdout);
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 5 * 1000000 };
        nanosleep(&ts, NULL);
    }
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    const char *device_path = NULL;
    char        auto_path[PATH_MAX];
    char        host[256] = {0};
    int         port      = DEFAULT_PORT;
    AppMode     mode      = MODE_SERVER;
    int         test_evdev = 0;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "--test-evdev")) {
            test_evdev = 1;
        } else if (!strcmp(argv[i], "--device") && i + 1 < argc) {
            device_path = argv[++i];
        } else if (!strcmp(argv[i], "--server")) {
            mode = MODE_SERVER;
            if (i + 1 < argc && argv[i+1][0] != '-')
                port = atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            /* Bare positional argument: treat as HOST or HOST:PORT */
            mode = MODE_CLIENT;
            const char *arg = argv[i];
            const char *colon = strrchr(arg, ':');
            if (colon) {
                size_t hlen = (size_t)(colon - arg);
                if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
                memcpy(host, arg, hlen);
                host[hlen] = '\0';
                port = atoi(colon + 1);
            } else {
                strncpy(host, arg, sizeof(host) - 1);
            }
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (mode == MODE_CLIENT && host[0] == '\0') {
        fprintf(stderr, "Error: client mode requires a host address\n\n");
        usage(argv[0]);
        return 1;
    }

    /* Auto-detect gamepad if not overridden */
    if (!device_path) {
        if (!evdev_find_gamepad(auto_path, sizeof(auto_path)))
            return 1;
        device_path = auto_path;
    }

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    EvdevState local;
    if (evdev_open(&local, device_path) < 0)
        return 1;
    fprintf(stderr, "[main] gamepad: %s\n", device_path);

    if (test_evdev) {
        run_test_evdev(&local);
        evdev_close(&local);
        return 0;
    }

    UinputDev remote_vpad;
    if (uinput_create(&remote_vpad) < 0) {
        evdev_close(&local);
        return 1;
    }
    fprintf(stderr, "[main] virtual gamepad created\n");

    AppCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.local       = &local;
    ctx.remote_vpad = &remote_vpad;
    ctx.mode        = mode;

    struct lws_context *lws_ctx = NULL;
    if (mode == MODE_SERVER) {
        lws_ctx = net_create_server(port, &ctx);
    } else {
        lws_ctx = net_create_client(host, port, &ctx);
    }

    if (!lws_ctx) {
        uinput_destroy(&remote_vpad);
        evdev_close(&local);
        return 1;
    }

    fprintf(stderr, "[main] running — Ctrl-C to stop\n");

    while (running) {
        evdev_read_all(&local);

        if (local.dirty && ctx.connected) {
            local.dirty    = false;
            ctx.want_write = true;
            lws_callback_on_writable(ctx.wsi);
        }

        /* 1Hz keepalive */
        if (ctx.connected && (time(NULL) - ctx.last_sent) > 1) {
            ctx.want_write = true;
            lws_callback_on_writable(ctx.wsi);
        }

        if (ctx.recv_new) {
            if (memcmp(&ctx.recv_buf, &ctx.last_injected, sizeof(GamepadPacket)) != 0) {
                uinput_write_packet(&remote_vpad, &ctx.recv_buf);
                ctx.last_injected = ctx.recv_buf;
            }
            ctx.recv_new = false;
        }

        /* Client reconnect */
        if (!ctx.connected && mode == MODE_CLIENT &&
            ctx.reconnect_after != 0 && time(NULL) >= ctx.reconnect_after) {
            ctx.reconnect_after = 0;
            net_do_connect(lws_ctx, &ctx);
        }

        lws_service(lws_ctx, 1);
    }

    lws_context_destroy(lws_ctx);
    uinput_destroy(&remote_vpad);
    evdev_close(&local);
    fprintf(stderr, "[main] bye\n");
    return 0;
}
