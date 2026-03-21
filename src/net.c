#include "net.h"
#include "uinput.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ── LWS callback ────────────────────────────────────────────────────────── */

static int callback_gamepad(struct lws *wsi, enum lws_callback_reasons reason,
                            void *user, void *in, size_t len)
{
    (void)user;
    AppCtx *ctx = lws_context_user(lws_get_context(wsi));

    switch (reason) {

    case LWS_CALLBACK_ESTABLISHED:
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        ctx->wsi            = wsi;
        ctx->connected      = true;
        ctx->want_send_desc = true;
        ctx->frag_len       = 0;
        /* destroy any leftover virtual device from a previous session */
        if (ctx->remote_vpad) {
            uinput_destroy(ctx->remote_vpad);
            free(ctx->remote_vpad);
            ctx->remote_vpad = NULL;
        }
        fprintf(stderr, "[net] %s\n",
                reason == LWS_CALLBACK_ESTABLISHED ? "client connected" : "connected to server");
        lws_callback_on_writable(wsi);
        break;

    case LWS_CALLBACK_SERVER_WRITEABLE:
    case LWS_CALLBACK_CLIENT_WRITEABLE:
        if (ctx->want_send_desc) {
            /* First message: describe our physical gamepad */
            unsigned char buf[LWS_PRE + sizeof(DeviceMsg)];
            memcpy(buf + LWS_PRE, &ctx->local_desc, sizeof(DeviceMsg));
            int n = lws_write(wsi, buf + LWS_PRE, sizeof(DeviceMsg), LWS_WRITE_BINARY);
            if (n < 0)
                fprintf(stderr, "[net] lws_write (device msg) failed: %d\n", n);
            else
                fprintf(stderr, "[net] sent device description: %s\n", ctx->local_desc.name);
            ctx->want_send_desc = false;
        } else if (ctx->pending_n > 0) {
            /* Subsequent messages: event batch */
            size_t sz = 2 + (size_t)ctx->pending_n * sizeof(WireEvent);
            unsigned char buf[LWS_PRE + 2 + MAX_BATCH * sizeof(WireEvent)];
            buf[LWS_PRE]     = MSG_EVENTS;
            buf[LWS_PRE + 1] = (unsigned char)ctx->pending_n;
            memcpy(buf + LWS_PRE + 2, ctx->pending,
                   (size_t)ctx->pending_n * sizeof(WireEvent));
            fprintf(stderr, "[tx] %d events\n", ctx->pending_n);
            lws_write(wsi, buf + LWS_PRE, sz, LWS_WRITE_BINARY);
            ctx->pending_n = 0;
        }
        break;

    case LWS_CALLBACK_RECEIVE:
    case LWS_CALLBACK_CLIENT_RECEIVE:
        if (ctx->frag_len + len > sizeof(ctx->frag_buf)) {
            ctx->frag_len = 0;
            break;
        }
        memcpy(ctx->frag_buf + ctx->frag_len, in, len);
        ctx->frag_len += len;

        if (ctx->frag_len < 1) break;

        if (ctx->frag_buf[0] == MSG_DEVICE) {
            if (ctx->frag_len < sizeof(DeviceMsg)) break; /* incomplete */
            ctx->frag_len = 0;
            DeviceMsg *desc = (DeviceMsg *)ctx->frag_buf;
            fprintf(stderr, "[net] received device description: %s\n", desc->name);
            UinputDev *vpad = malloc(sizeof(UinputDev));
            if (uinput_create(vpad, desc) < 0) {
                free(vpad);
            } else {
                if (ctx->remote_vpad) {
                    uinput_destroy(ctx->remote_vpad);
                    free(ctx->remote_vpad);
                }
                ctx->remote_vpad = vpad;
            }
        } else if (ctx->frag_buf[0] == MSG_EVENTS) {
            if (ctx->frag_len < 2) break; /* incomplete */
            int count = ctx->frag_buf[1];
            if (count < 1 || count > MAX_BATCH) { ctx->frag_len = 0; break; }
            size_t needed = 2 + (size_t)count * sizeof(WireEvent);
            if (ctx->frag_len < needed) break; /* incomplete */
            ctx->frag_len = 0;
            if (!ctx->remote_vpad) break;
            const WireEvent *events = (const WireEvent *)(ctx->frag_buf + 2);
            fprintf(stderr, "[rx] %d events -> uinput\n", count);
            uinput_write_events(ctx->remote_vpad, events, count);
        } else {
            ctx->frag_len = 0; /* unknown type */
        }
        break;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        fprintf(stderr, "[net] connection error: %s\n",
                in ? (const char *)in : "(unknown)");
        /* fall through */
    case LWS_CALLBACK_CLOSED:
    case LWS_CALLBACK_CLIENT_CLOSED:
        if (ctx->wsi == wsi) {
            ctx->connected       = false;
            ctx->wsi             = NULL;
            ctx->reconnect_after = time(NULL) + 3;
            fprintf(stderr, "[net] disconnected, reconnecting in 3s\n");
        }
        break;

    default:
        break;
    }
    return 0;
}

static struct lws_protocols protocols[] = {
    {
        .name                  = "coop-bridge",
        .callback              = callback_gamepad,
        .per_session_data_size = 0,
        .rx_buffer_size        = sizeof(DeviceMsg) + 64,
    },
    LWS_PROTOCOL_LIST_TERM
};

/* ── Public API ───────────────────────────────────────────────────────────── */

struct lws_context *net_create_server(int port, AppCtx *ctx)
{
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port      = port;
    info.protocols = protocols;
    info.user      = ctx;
    lws_set_log_level(LLL_ERR | LLL_WARN, NULL);

    struct lws_context *lws_ctx = lws_create_context(&info);
    if (!lws_ctx)
        fprintf(stderr, "[net] failed to create server context\n");
    else
        fprintf(stderr, "[net] listening on port %d\n", port);
    return lws_ctx;
}

struct lws_context *net_create_client(const char *host, int port, AppCtx *ctx)
{
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port      = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.user      = ctx;
    lws_set_log_level(LLL_ERR | LLL_WARN, NULL);

    struct lws_context *lws_ctx = lws_create_context(&info);
    if (!lws_ctx) {
        fprintf(stderr, "[net] failed to create client context\n");
        return NULL;
    }

    strncpy(ctx->host, host, sizeof(ctx->host) - 1);
    ctx->port = port;

    net_do_connect(lws_ctx, ctx);
    return lws_ctx;
}

void net_do_connect(struct lws_context *lws_ctx, AppCtx *ctx)
{
    /* Use a local copy so LWS cannot corrupt ctx->host */
    char addr[256];
    memcpy(addr, ctx->host, sizeof(addr));

    struct lws_client_connect_info i;
    memset(&i, 0, sizeof(i));
    i.context  = lws_ctx;
    i.address  = addr;
    i.port     = ctx->port;
    i.path     = "/";
    i.host     = addr;
    i.origin   = addr;
    i.protocol = "coop-bridge";

    fprintf(stderr, "[net] connecting to %s:%d\n", ctx->host, ctx->port);
    if (!lws_client_connect_via_info(&i))
        fprintf(stderr, "[net] connect initiation failed\n");
}
