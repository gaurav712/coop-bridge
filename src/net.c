#include "net.h"
#include "evdev.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* ── LWS protocol callback ────────────────────────────────────────────────── */

static int callback_gamepad(struct lws *wsi, enum lws_callback_reasons reason,
                            void *user, void *in, size_t len)
{
    (void)user;
    struct lws_context *lws_ctx = lws_get_context(wsi);
    AppCtx *ctx = lws_context_user(lws_ctx);

    switch (reason) {
    /* ── Server: new peer connected ──────────────────────────────────────── */
    case LWS_CALLBACK_ESTABLISHED:
        ctx->wsi       = wsi;
        ctx->connected = true;
        fprintf(stderr, "[net] client connected\n");
        lws_callback_on_writable(wsi);
        break;

    /* ── Client: connected to server ─────────────────────────────────────── */
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        ctx->wsi       = wsi;
        ctx->connected = true;
        fprintf(stderr, "[net] connected to server\n");
        lws_callback_on_writable(wsi);
        break;

    /* ── Send path ───────────────────────────────────────────────────────── */
    case LWS_CALLBACK_SERVER_WRITEABLE:
    case LWS_CALLBACK_CLIENT_WRITEABLE:
        if (ctx->want_write && ctx->connected) {
            unsigned char buf[LWS_PRE + sizeof(GamepadPacket)];
            GamepadPacket pkt;
            evdev_pack(ctx->local, &pkt);
            memcpy(buf + LWS_PRE, &pkt, sizeof(pkt));
            int n = lws_write(wsi, buf + LWS_PRE, sizeof(pkt), LWS_WRITE_BINARY);
            if (n < 0)
                fprintf(stderr, "[net] lws_write failed: %d\n", n);
            else
                fprintf(stderr, "[tx] btns=0x%04x lt=%u rt=%u lx=%d ly=%d rx=%d ry=%d\n",
                        pkt.buttons, pkt.lt, pkt.rt, pkt.lx, pkt.ly, pkt.rx, pkt.ry);
            ctx->want_write = false;
            ctx->last_sent  = time(NULL);
        }
        break;

    /* ── Receive path ────────────────────────────────────────────────────── */
    case LWS_CALLBACK_RECEIVE:
    case LWS_CALLBACK_CLIENT_RECEIVE:
        if (len == sizeof(GamepadPacket)) {
            memcpy(&ctx->recv_buf, in, sizeof(GamepadPacket));
            ctx->recv_new = true;
            const GamepadPacket *p = (const GamepadPacket *)in;
            fprintf(stderr, "[rx] btns=0x%04x lt=%u rt=%u lx=%d ly=%d rx=%d ry=%d\n",
                    p->buttons, p->lt, p->rt, p->lx, p->ly, p->rx, p->ry);
        } else {
            fprintf(stderr, "[rx] unexpected length %zu (expected %zu)\n",
                    len, sizeof(GamepadPacket));
        }
        break;

    /* ── Connection lost ─────────────────────────────────────────────────── */
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
        .rx_buffer_size        = sizeof(GamepadPacket) * 4,
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
    /* Suppress verbose LWS logs; set to LLL_USER|LLL_ERR|LLL_WARN for more */
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

    /* Store for reconnect */
    strncpy(ctx->host, host, sizeof(ctx->host) - 1);
    ctx->port = port;

    net_do_connect(lws_ctx, ctx);
    return lws_ctx;
}

void net_do_connect(struct lws_context *lws_ctx, AppCtx *ctx)
{
    struct lws_client_connect_info i;
    memset(&i, 0, sizeof(i));
    i.context   = lws_ctx;
    i.address   = ctx->host;
    i.port      = ctx->port;
    i.path      = "/";
    i.host      = ctx->host;
    i.origin    = ctx->host;
    i.protocol  = "coop-bridge";

    fprintf(stderr, "[net] connecting to %s:%d\n", ctx->host, ctx->port);
    if (!lws_client_connect_via_info(&i))
        fprintf(stderr, "[net] connect initiation failed\n");
}
