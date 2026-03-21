#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <linux/limits.h>

#include "evdev.h"
#include "uinput.h"
#include "net.h"

#define DEFAULT_PORT 7777

static volatile sig_atomic_t running = 1;
static void handle_signal(int sig) { (void)sig; running = 0; }

/* Watchdog: wakes up lws_service() every 1ms so evdev is read promptly */
static void *lws_watchdog(void *arg)
{
    struct lws_context *lws_ctx = arg;
    struct timespec ts = { .tv_nsec = 1000000 }; /* 1ms */
    while (running) {
        nanosleep(&ts, NULL);
        lws_cancel_service(lws_ctx);
    }
    return NULL;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s                    server, port %d\n"
        "  %s <HOST>             client, connect to HOST:%d\n"
        "  %s <HOST>:<PORT>      client, custom port\n"
        "  %s --server [PORT]    server, custom port\n"
        "  %s --device PATH      override auto-detected gamepad\n"
        "  %s --test-evdev       print raw events, no networking\n"
        "\nGamepad is auto-detected from /dev/input/.\n",
        prog, DEFAULT_PORT, prog, DEFAULT_PORT,
        prog, prog, prog, prog);
}

static void run_test_evdev(int fd)
{
    fprintf(stderr, "[test-evdev] Ctrl-C to stop\n");
    WireEvent events[MAX_BATCH];
    while (running) {
        int n = evdev_read_batch(fd, events, MAX_BATCH);
        for (int i = 0; i < n; i++)
            fprintf(stderr, "  type=%u code=%u value=%d\n",
                    events[i].type, events[i].code, events[i].value);
        struct timespec ts = { .tv_nsec = 1000000 };
        nanosleep(&ts, NULL);
    }
}

/* Parse "HOST" or "HOST:PORT" into host/port. host buf must be 256 bytes. */
static void parse_hostport(const char *arg, char *host, int *port)
{
    const char *colon = strrchr(arg, ':');
    if (colon) {
        size_t hlen = (size_t)(colon - arg);
        if (hlen >= 256) hlen = 255;
        memcpy(host, arg, hlen);
        host[hlen] = '\0';
        *port = atoi(colon + 1);
    } else {
        strncpy(host, arg, 255);
        host[255] = '\0';
    }
}

int main(int argc, char **argv)
{
    const char *device_path = NULL;
    char        auto_path[PATH_MAX];
    char        host[256] = {0};
    int         port      = DEFAULT_PORT;
    AppMode     mode      = MODE_SERVER;
    int         test_evdev = 0;

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
            mode = MODE_CLIENT;
            parse_hostport(argv[i], host, &port);
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

    if (!device_path) {
        if (!evdev_find_gamepad(auto_path, sizeof(auto_path)))
            return 1;
        device_path = auto_path;
    }

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    AppCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mode = mode;

    ctx.evdev_fd = evdev_open(device_path, &ctx.local_desc);
    if (ctx.evdev_fd < 0)
        return 1;
    fprintf(stderr, "[main] gamepad: %s (%s)\n", device_path, ctx.local_desc.name);

    if (test_evdev) {
        run_test_evdev(ctx.evdev_fd);
        evdev_close(ctx.evdev_fd);
        return 0;
    }

    struct lws_context *lws_ctx = NULL;
    if (mode == MODE_SERVER) {
        lws_ctx = net_create_server(port, &ctx);
    } else {
        memcpy(ctx.host, host, sizeof(ctx.host));
        ctx.port = port;
        lws_ctx = net_create_client(host, port, &ctx);
    }
    if (!lws_ctx) {
        evdev_close(ctx.evdev_fd);
        return 1;
    }

    fprintf(stderr, "[main] running — Ctrl-C to stop\n");

    pthread_t watchdog;
    pthread_create(&watchdog, NULL, lws_watchdog, lws_ctx);

    while (running) {
        /* lws_service blocks until I/O or timeout; watchdog calls
         * lws_cancel_service() every 1ms so we return promptly */
        lws_service(lws_ctx, 5);

        /* Read all pending gamepad events */
        int n = evdev_read_batch(ctx.evdev_fd, ctx.pending, MAX_BATCH);
        if (n > 0) {
            ctx.pending_n = n;
            fprintf(stderr, "[evdev] %d events, connected=%d\n", n, ctx.connected);
            if (ctx.connected)
                lws_callback_on_writable(ctx.wsi);
        }

        /* Client reconnect */
        if (!ctx.connected && mode == MODE_CLIENT &&
            ctx.reconnect_after != 0 && time(NULL) >= ctx.reconnect_after) {
            ctx.reconnect_after = 0;
            net_do_connect(lws_ctx, &ctx);
        }
    }

    pthread_join(watchdog, NULL);

    lws_context_destroy(lws_ctx);
    if (ctx.remote_vpad) {
        uinput_destroy(ctx.remote_vpad);
        free(ctx.remote_vpad);
    }
    evdev_close(ctx.evdev_fd);
    fprintf(stderr, "[main] bye\n");
    return 0;
}
