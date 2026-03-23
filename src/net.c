#include "net.h"
#include "uinput.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

/* SO_BUSY_POLL may not be visible under strict POSIX headers; value is stable
 * since Linux 3.11 */
#ifndef SO_BUSY_POLL
#define SO_BUSY_POLL 46
#endif

/* ── LWS callback ────────────────────────────────────────────────────────── */

static int callback_gamepad(struct lws *wsi, enum lws_callback_reasons reason,
                            void *user, void *in, size_t len) {
  (void)user;
  AppCtx *ctx = lws_context_user(lws_get_context(wsi));

  switch (reason) {

  case LWS_CALLBACK_ESTABLISHED:
  case LWS_CALLBACK_CLIENT_ESTABLISHED: {
    ctx->wsi = wsi;
    ctx->connected = true;
    ctx->want_send_desc = true;
    ctx->frag_len = 0;
    if (ctx->remote_vpad) {
      uinput_destroy(ctx->remote_vpad);
      free(ctx->remote_vpad);
      ctx->remote_vpad = NULL;
    }
    /* SO_BUSY_POLL: spin-poll the NIC for up to 50μs before sleeping,
     * eliminating interrupt/context-switch latency for small packets */
    int busy = 50;
    int fd = lws_get_socket_fd(wsi);
    if (fd >= 0)
      setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &busy, sizeof(busy));
    lws_callback_on_writable(wsi);
    break;
  }

  case LWS_CALLBACK_SERVER_WRITEABLE:
  case LWS_CALLBACK_CLIENT_WRITEABLE:
    if (ctx->want_send_desc) {
      unsigned char buf[LWS_PRE + sizeof(DeviceMsg)];
      memcpy(buf + LWS_PRE, &ctx->local_desc, sizeof(DeviceMsg));
      lws_write(wsi, buf + LWS_PRE, sizeof(DeviceMsg), LWS_WRITE_BINARY);
      ctx->want_send_desc = false;
      pthread_mutex_lock(&ctx->pending_mutex);
      int requeue = ctx->pending_n > 0;
      pthread_mutex_unlock(&ctx->pending_mutex);
      if (requeue)
        lws_callback_on_writable(wsi);
    } else {
      pthread_mutex_lock(&ctx->pending_mutex);
      int n = ctx->pending_n;
      WireEvent events[MAX_BATCH];
      if (n > 0)
        memcpy(events, ctx->pending, (size_t)n * sizeof(WireEvent));
      ctx->pending_n = 0;
      pthread_mutex_unlock(&ctx->pending_mutex);
      if (n > 0) {
        size_t sz = 2 + (size_t)n * sizeof(WireEvent);
        unsigned char buf[LWS_PRE + 2 + MAX_BATCH * sizeof(WireEvent)];
        buf[LWS_PRE] = MSG_EVENTS;
        buf[LWS_PRE + 1] = (unsigned char)n;
        memcpy(buf + LWS_PRE + 2, events, (size_t)n * sizeof(WireEvent));
        lws_write(wsi, buf + LWS_PRE, sz, LWS_WRITE_BINARY);
      }
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

    if (ctx->frag_len < 1)
      break;

    if (ctx->frag_buf[0] == MSG_DEVICE) {
      if (ctx->frag_len < sizeof(DeviceMsg))
        break;
      ctx->frag_len = 0;
      DeviceMsg *desc = (DeviceMsg *)ctx->frag_buf;
      desc->name[sizeof(desc->name) - 1] = '\0';
      UinputDev *vpad = malloc(sizeof(UinputDev));
      if (!vpad)
        break;
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
      if (ctx->frag_len < 2)
        break;
      int count = ctx->frag_buf[1];
      if (count < 1 || count > MAX_BATCH) {
        ctx->frag_len = 0;
        break;
      }
      size_t needed = 2 + (size_t)count * sizeof(WireEvent);
      if (ctx->frag_len < needed)
        break;
      ctx->frag_len = 0;
      if (!ctx->remote_vpad)
        break;
      uinput_write_events(ctx->remote_vpad,
                          (const WireEvent *)(ctx->frag_buf + 2), count);
    } else {
      ctx->frag_len = 0;
    }
    break;

  case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
  case LWS_CALLBACK_CLOSED:
  case LWS_CALLBACK_CLIENT_CLOSED:
    if (ctx->wsi == wsi) {
      ctx->connected = false;
      ctx->wsi = NULL;
      ctx->pending_n = 0;
      ctx->frag_len = 0;
      ctx->reconnect_after = time(NULL) + 3;
    }
    break;

  default:
    break;
  }
  return 0;
}

static struct lws_protocols protocols[] = {
    {
        .name = "coop-bridge",
        .callback = callback_gamepad,
        .per_session_data_size = 0,
        .rx_buffer_size = sizeof(DeviceMsg),
    },
    LWS_PROTOCOL_LIST_TERM};

/* ── Public API ─────────────────────────────────────────────────────────────
 */

struct lws_context *net_create_server(int port, AppCtx *ctx) {
  struct lws_context_creation_info info;
  memset(&info, 0, sizeof(info));
  info.port = port;
  info.protocols = protocols;
  info.user = ctx;
  lws_set_log_level(0, NULL);

  struct lws_context *lws_ctx = lws_create_context(&info);
  if (!lws_ctx)
    fprintf(stderr, "error: failed to create server on port %d\n", port);
  return lws_ctx;
}

struct lws_context *net_create_client(const char *host, int port, AppCtx *ctx) {
  struct lws_context_creation_info info;
  memset(&info, 0, sizeof(info));
  info.port = CONTEXT_PORT_NO_LISTEN;
  info.protocols = protocols;
  info.user = ctx;
  lws_set_log_level(0, NULL);

  struct lws_context *lws_ctx = lws_create_context(&info);
  if (!lws_ctx) {
    fprintf(stderr, "error: failed to create client context\n");
    return NULL;
  }

  strncpy(ctx->host, host, sizeof(ctx->host) - 1);
  ctx->port = port;

  net_do_connect(lws_ctx, ctx);
  return lws_ctx;
}

void net_do_connect(struct lws_context *lws_ctx, AppCtx *ctx) {
  char addr[256];
  memcpy(addr, ctx->host, sizeof(addr));

  struct lws_client_connect_info i;
  memset(&i, 0, sizeof(i));
  i.context = lws_ctx;
  i.address = addr;
  i.port = ctx->port;
  i.path = "/";
  i.host = addr;
  i.origin = addr;
  i.protocol = "coop-bridge";

  lws_client_connect_via_info(&i);
}
