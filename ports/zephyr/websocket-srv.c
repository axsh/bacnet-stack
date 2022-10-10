/**
 * @file
 * @brief Implementation of server websocket interface MAC OS / BSD.
 * @author Kirill Neznamov
 * @date June 2022
 * @section LICENSE
 *
 * Copyright (C) 2022 Legrand North America, LLC
 * as an unpublished work.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later WITH GCC-exception-2.0
 */

#include <zephyr.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <init.h>
#include <posix/pthread.h>
#include <net/socket.h>
#include <net/tls_credentials.h>
#include "bacnet/datalink/bsc/bvlc-sc.h"
#include "bacnet/datalink/bsc/websocket.h"
#include "civetweb.h"

#include <logging/log.h>
#include <logging/log_ctrl.h>

LOG_MODULE_DECLARE(bacnet, LOG_LEVEL_DBG);

#if 0
#define BOARD_REPLY_PREFIX      CONFIG_BOARD" says: "
#define BOARD_REPLY_PREFIX_LEN      sizeof(BOARD_REPLY_PREFIX)

#define BOARD_REPLY_SUFFIX      " too!"
#define BOARD_REPLY_SUFFIX_LEN      sizeof(BOARD_REPLY_SUFFIX)

#define BOARD_REPLY_TOAL_LEN        (BOARD_REPLY_PREFIX_LEN +\
                     BOARD_REPLY_SUFFIX_LEN)
#endif

#define FIN_SHIFT       7u
#define RSV1_SHIFT      6u
#define RSV2_SHIFT      5u
#define RSV3_SHIFT      4u
#define OPCODE_SHIFT        0u


#define BOOL_MASK       0x1  /* boolean value mask */
#define HALF_BYTE_MASK      0xF  /* half byte value mask */
#define WS_URL          "/"  /* WebSocket server main url */

/* Use smallest possible value of 1024 (see the line 18619 of civetweb.c) */
#define MAX_REQUEST_SIZE_BYTES          1024

typedef enum {
    BSC_WEBSOCKET_STATE_IDLE = 0,
    BSC_WEBSOCKET_STATE_CONNECTING = 1,
    BSC_WEBSOCKET_STATE_CONNECTED = 2,
    BSC_WEBSOCKET_STATE_DISCONNECTING = 3,
    BSC_WEBSOCKET_STATE_DISCONNECTED = 4
} BSC_WEBSOCKET_STATE;

typedef enum {
    WORKER_ID_START = 0,
    WORKER_ID_DISCONNECT = 1,
    WORKER_ID_SEND = 2,
    WORKER_ID_STOP = 3,
} WORKER_ID_EVENT;

typedef struct {
    BSC_WEBSOCKET_STATE state;
    struct mg_connection *ctx;
    bool want_send_data;
    //bool can_send_data;
    size_t length;
    uint8_t buf[BVLC_SC_NPDU_SIZE];
} BSC_WEBSOCKET_CONNECTION;

static BSC_WEBSOCKET_CONNECTION 
    bws_hub_conn[BSC_SERVER_HUB_WEBSOCKETS_MAX_NUM] = {0};
static BSC_WEBSOCKET_CONNECTION
    bws_direct_conn[BSC_SERVER_DIRECT_WEBSOCKETS_MAX_NUM] = {0};

typedef struct {
    BSC_WEBSOCKET_PROTOCOL proto;
    struct mg_websocket_subprotocols protocols;
    size_t timeout;
    uint16_t port;
    BSC_WEBSOCKET_SRV_DISPATCH dispatch;
    void *user_param;
    struct mg_context *context;
    size_t connection_max;
    BSC_WEBSOCKET_CONNECTION *connections;
} BSC_WEBSOCKET_SERVER_CONTEXT;

static char *bws_hub_protocol = BSC_WEBSOCKET_HUB_PROTOCOL_STR;
static char *bws_direct_protocol = BSC_WEBSOCKET_DIRECT_PROTOCOL_STR;

static BSC_WEBSOCKET_SERVER_CONTEXT bws_ctx[BSC_WEBSOCKET_PROTOCOLS_AMOUNT] = 
{
    {BSC_WEBSOCKET_HUB_PROTOCOL, {1, &bws_hub_protocol}, 0, 0, NULL, NULL,
        NULL, BSC_SERVER_HUB_WEBSOCKETS_MAX_NUM, bws_hub_conn},
    {BSC_WEBSOCKET_DIRECT_PROTOCOL, {1, &bws_direct_protocol}, 0, 0, NULL, NULL,
        NULL, BSC_SERVER_DIRECT_WEBSOCKETS_MAX_NUM, bws_direct_conn},
};

static int event_fd = -1;
//static bool civetweb_initialized = false;

//////////////////////////////////////////////////////////////////////////////

static void worker_disconnect(BSC_WEBSOCKET_CONNECTION *webconn, uint16_t status)
{
    uint8_t code[2];
    if (webconn->state == BSC_WEBSOCKET_STATE_CONNECTED) {
        code[0] = status >> 8;
        code[1] = status & 0xff;
        mg_websocket_write(webconn->ctx, MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE,
            code, sizeof(code));
    }
//    if (ctx->websock >= 0)
//        websocket_disconnect(ctx->websock);
//    else
//        zsock_close(ctx->sock);
//    ctx->state = BSC_WEBSOCKET_STATE_DISCONNECTED;
}

/* Websocket server handlers: */
static int civetweb_connect_handler(const struct mg_connection *conn,
                     void *cbdata)
{    
    BSC_WEBSOCKET_SERVER_CONTEXT *ctx = (BSC_WEBSOCKET_SERVER_CONTEXT*)cbdata;
    BSC_WEBSOCKET_CONNECTION *webconn = NULL;
    int h;

    // todo select connection
    for (h = 0; h < ctx->connection_max; h++) {
        if (ctx->connections[h].state == BSC_WEBSOCKET_STATE_IDLE) {
            break;
        }
    }

    if (h == ctx->connection_max) {
        LOG_ERR("Has not free connection for protocol: %s",
            ctx->protocols.subprotocols[0]);
        return 1;
    }

    webconn = &ctx->connections[h];
    webconn->ctx = (struct mg_connection *)conn;
    webconn->state = BSC_WEBSOCKET_STATE_CONNECTING;
    mg_set_user_connection_data(webconn->ctx, (void*)h);

    return 0;
}

static void civetweb_ready_handler(struct mg_connection *conn, void *cbdata)
{
    BSC_WEBSOCKET_SERVER_CONTEXT *ctx = (BSC_WEBSOCKET_SERVER_CONTEXT*)cbdata;
    int h = (int)mg_get_user_connection_data(conn);
    BSC_WEBSOCKET_CONNECTION *webconn = &ctx->connections[h];

    webconn->state = BSC_WEBSOCKET_STATE_CONNECTED;
    ctx->dispatch(ctx->proto, h, BSC_WEBSOCKET_CONNECTED, NULL, 0,
        ctx->user_param);
//    ctx->dispatch(ctx->proto, h, BSC_WEBSOCKET_SENDABLE, NULL, 0,
//        ctx->user_param);
}

static int civetweb_data_handler(struct mg_connection *conn, int bits,
                  char *data, size_t data_len, void *cbdata)
{
    int ret_state = 1;

    BSC_WEBSOCKET_SERVER_CONTEXT *ctx = (BSC_WEBSOCKET_SERVER_CONTEXT*)cbdata;
    int h = (int)mg_get_user_connection_data(conn);
    BSC_WEBSOCKET_CONNECTION *webconn = &ctx->connections[h];

    /* Encode bits as by https://tools.ietf.org/html/rfc6455#section-5.2: */
    const bool FIN = (bits >> FIN_SHIFT) & BOOL_MASK;
    const bool RSV1 = (bits >> RSV1_SHIFT) & BOOL_MASK;
    const bool RSV2 = (bits >> RSV2_SHIFT) & BOOL_MASK;
    const bool RSV3 = (bits >> RSV2_SHIFT) & BOOL_MASK;

    uint8_t OPCODE = (bits >> OPCODE_SHIFT) & HALF_BYTE_MASK;
//    ssize_t len;

    (void)FIN;
    (void)RSV1;
    (void)RSV2;
    (void)RSV3;

    LOG_DBG("got bits: %d", bits);
    LOG_DBG("\t\twith OPCODE: %d", OPCODE);


#if 0
    if (data_len > CONFIG_MAIN_STACK_SIZE) {
        /* Close connection due to no memory */
        OPCODE = MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE;
    }
#endif

    /* Process depending of opcode: */
    switch (OPCODE) {
    case MG_WEBSOCKET_OPCODE_CONTINUATION:
        break;
    case MG_WEBSOCKET_OPCODE_TEXT:
    case MG_WEBSOCKET_OPCODE_BINARY:
        if (data_len > sizeof(webconn->buf) - webconn->length) {
            ctx->dispatch(ctx->proto, h, BSC_WEBSOCKET_DISCONNECTED, NULL, 0,
                ctx->user_param);
            worker_disconnect(webconn, WEBSOCKET_CLOSE_STATUS_MESSAGE_TOO_LARGE);
            ret_state = 0;
        } else {
            memcpy(webconn->buf + webconn->length, data, data_len);
            webconn->length += data_len;
            if (FIN) {
                ctx->dispatch(ctx->proto, h, BSC_WEBSOCKET_RECEIVED,
                    webconn->buf, webconn->length, ctx->user_param);
                webconn->length = 0;
            }
        }
        break;
    case MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE:
        ret_state = 0;
        worker_disconnect(webconn, WEBSOCKET_CLOSE_STATUS_NORMAL);
        break;
    default:
        ret_state = 0;
        worker_disconnect(webconn, WEBSOCKET_CLOSE_STATUS_PROTOCOL_ERR);
        LOG_ERR("Unknown OPCODE: close connection");
        break;
    }

    if (ret_state < 0) {
        /* TODO: Maybe need we close WS connection here?! */
        LOG_ERR("Unknown ERROR: ret_state = %d", ret_state);
    } else if (ret_state == 0) {
        LOG_DBG("Close WS sonnection: ret_state = %d", ret_state);
    }

    return ret_state;
}

static void civetweb_close_handler(const struct mg_connection *conn,
                    void *cbdata)
{
    BSC_WEBSOCKET_SERVER_CONTEXT *ctx = (BSC_WEBSOCKET_SERVER_CONTEXT*)cbdata;
    int h = (int)mg_get_user_connection_data(conn);
    BSC_WEBSOCKET_CONNECTION *webconn = &ctx->connections[h];

    webconn->state = BSC_WEBSOCKET_STATE_DISCONNECTED;
    ctx->dispatch(ctx->proto, h, BSC_WEBSOCKET_DISCONNECTED, NULL, 0,
        ctx->user_param);
    webconn->state = BSC_WEBSOCKET_STATE_IDLE;
}

static void init_websocket_server_handlers(BSC_WEBSOCKET_SERVER_CONTEXT *ctx)
{
    mg_set_websocket_handler_with_subprotocols(ctx->context, WS_URL,
        &ctx->protocols,
        civetweb_connect_handler,
        civetweb_ready_handler,
        civetweb_data_handler,
        civetweb_close_handler,
        ctx);
}

//////////////////////////////////////////////////////////////////////////////
static void *bws_srv_worker(void *arg)
{
    int ret = 0;
    //uint64_t remaining = ULLONG_MAX;
    //uint32_t message_type;
    BSC_WEBSOCKET_SERVER_CONTEXT *ctx;
    BSC_WEBSOCKET_HANDLE h;
    uint8_t event[2];   // event and proto
    uint16_t event_status;
    uint8_t proto;
    int spair[2];
    //int timeout;
    struct zsock_pollfd fds = { 0 }; // event

    char port_str[10];
    char *options[] = {
        "listening_ports", port_str,
        "num_threads", "1",
        "max_request_size", STRINGIFY(MAX_REQUEST_SIZE_BYTES),
        NULL
    };
    struct mg_callbacks callbacks = {0};

    ret = zsock_socketpair(AF_UNIX, SOCK_STREAM, 0, spair);
    if (ret == 0) {
        event_fd = spair[0];
        fds.fd = spair[1];
        fds.events = ZSOCK_POLLIN;
    } else {
        LOG_ERR("socketpair() failed: %d", errno);
        return NULL;
    }

    mg_init_library(MG_FEATURES_SSL |
                    //MG_FEATURES_IPV6 |
                    MG_FEATURES_WEBSOCKET);

    while (1) {
        fds.revents = 0;
        ret = zsock_poll(&fds, 1, -1);
        LOG_INF("zsock_polled: %d", ret);

        zsock_recv(fds.fd, &event, sizeof(event), ZSOCK_MSG_DONTWAIT);
        proto = event[1];
        LOG_INF("Worker event happend, id %d proto %d", event[0], event[1]);
        ctx = &bws_ctx[proto];

        switch (event[0]) {
            case WORKER_ID_START:                
                snprintf(port_str, sizeof(port_str), "%d", ctx->port);
                ctx->context = mg_start(&callbacks, NULL, (const char **)options);
                if (ctx->context == NULL) {
                    LOG_ERR("Unable to start the server\n");
                    ctx->dispatch(ctx->proto, -1, BSC_WEBSOCKET_DISCONNECTED,
                        NULL, 0,ctx->user_param);
                } else {
                    init_websocket_server_handlers(ctx);
                }
                break;
            case WORKER_ID_STOP:
                mg_stop(ctx->context);
                ctx->context = NULL;
                ctx->dispatch(ctx->proto, -1, BSC_WEBSOCKET_DISCONNECTED, NULL, 0,
                    ctx->user_param);
                break;
            case WORKER_ID_DISCONNECT:
                zsock_recv(fds.fd, &h, sizeof(h), ZSOCK_MSG_DONTWAIT);
                zsock_recv(fds.fd, &event_status, sizeof(event_status),
                    ZSOCK_MSG_DONTWAIT);
                worker_disconnect(&ctx->connections[h], event_status);
                mg_close_connection(ctx->connections[h].ctx);
                break;
            case WORKER_ID_SEND:
                zsock_recv(fds.fd, &h, sizeof(h), ZSOCK_MSG_DONTWAIT);
                ctx->dispatch(ctx->proto, h, BSC_WEBSOCKET_SENDABLE, NULL, 0,
                    ctx->user_param);
                break;
        }
    }

    LOG_INF("Close worker");
    return NULL;
}

//////////////////////////////////////////////////////////////////////////////

BSC_WEBSOCKET_RET bws_srv_start(
                        BSC_WEBSOCKET_PROTOCOL proto,
                        int port,
                        uint8_t *ca_cert,
                        size_t ca_cert_size,
                        uint8_t *cert,
                        size_t cert_size,
                        uint8_t *key,
                        size_t key_size,
                        size_t timeout_s,
                        BSC_WEBSOCKET_SRV_DISPATCH dispatch_func,
                        void* dispatch_func_user_param)
{
    LOG_INF("bws_srv_start() >>> proto = %d port = %d", proto, port);
    if (proto >= BSC_WEBSOCKET_PROTOCOLS_AMOUNT) {
        LOG_ERR("bws_srv_start() << Error proto %d", proto);
        return BSC_WEBSOCKET_BAD_PARAM;
    }

    bws_ctx[proto].port = port;
    bws_ctx[proto].timeout = timeout_s;
    bws_ctx[proto].dispatch = dispatch_func;
    bws_ctx[proto].user_param = dispatch_func_user_param;

    // todo use certs, etc

    uint8_t msg[2] = {WORKER_ID_START, proto};
    zsock_send(event_fd, msg, sizeof(msg), 0);

    return BSC_WEBSOCKET_SUCCESS;
}

BSC_WEBSOCKET_RET bws_srv_stop(BSC_WEBSOCKET_PROTOCOL proto)
{
    LOG_INF("bws_srv_stop() >>> proto = %d", proto);
    if (proto >= BSC_WEBSOCKET_PROTOCOLS_AMOUNT) {
        LOG_ERR("bws_srv_start() << Error proto %d", proto);
        return BSC_WEBSOCKET_BAD_PARAM;
    }

    uint8_t msg[2] = {WORKER_ID_STOP, proto};
    zsock_send(event_fd, msg, sizeof(msg), 0);

    return BSC_WEBSOCKET_SUCCESS;
}

void bws_srv_disconnect(BSC_WEBSOCKET_PROTOCOL proto, BSC_WEBSOCKET_HANDLE h)
{
    LOG_INF("bws_srv_disconnect() >>> proto = %d h = %d", proto, h);
    if ((proto >= BSC_WEBSOCKET_PROTOCOLS_AMOUNT) ||
        (h <= bws_ctx[proto].connection_max)) {
        LOG_ERR("bws_srv_disconnect(): BSC_WEBSOCKET_BAD_PARAM");
        return;
    }

    if ((bws_ctx[proto].connections[h].state == BSC_WEBSOCKET_STATE_IDLE) ||
        (bws_ctx[proto].connections[h].ctx == NULL)) {
        LOG_ERR("bws_srv_disconnect(): BSC_WEBSOCKET_INVALID_OPERATION");
        return;
    }

    uint8_t msg[5] = {WORKER_ID_DISCONNECT, proto, h, 0, 0};
    *(uint16_t*)(msg+3) = (uint16_t)WEBSOCKET_CLOSE_STATUS_NORMAL;
    zsock_send(event_fd, msg, sizeof(msg), 0);
}

void bws_srv_send(BSC_WEBSOCKET_PROTOCOL proto, BSC_WEBSOCKET_HANDLE h)
{
    LOG_INF("bws_srv_send() >>> proto = %d h = %d", proto, h);
    uint8_t msg[3] = {WORKER_ID_SEND, proto, h};
    zsock_send(event_fd, msg, sizeof(msg), 0);
}

BSC_WEBSOCKET_RET bws_srv_dispatch_send(BSC_WEBSOCKET_PROTOCOL proto,
                                        BSC_WEBSOCKET_HANDLE h,
                                        uint8_t *payload, size_t payload_size)
{
    BSC_WEBSOCKET_CONNECTION *webconn;
    int ret;

    LOG_INF(
        "bws_srv_dispatch_send() >>> proto %d, h = %d, payload = %p, size = %d",
        proto, h, payload, payload_size);

    if ((proto >= BSC_WEBSOCKET_PROTOCOLS_AMOUNT) ||
        (h <= bws_ctx[proto].connection_max)) {
        return BSC_WEBSOCKET_BAD_PARAM;
    }

    webconn = &bws_ctx[proto].connections[h];
    if ((webconn->state == BSC_WEBSOCKET_STATE_IDLE) ||
        (webconn->ctx == NULL)) {
        return BSC_WEBSOCKET_INVALID_OPERATION;
    }

    ret = mg_websocket_write(webconn->ctx, MG_WEBSOCKET_OPCODE_BINARY, payload,
        payload_size);
    return ret == 0 ? BSC_WEBSOCKET_NO_RESOURCES :
           ret < 0 ? BSC_WEBSOCKET_INVALID_OPERATION :
           BSC_WEBSOCKET_SUCCESS;
}

//////////////////////////////////////////////////////////////////////////

//#define STACKSIZE     CONFIG_MAIN_STACK_SIZE
#define STACKSIZE (4096 + CONFIG_TEST_EXTRA_STACKSIZE)

K_THREAD_STACK_DEFINE(websocket_srv_stack, STACKSIZE);

static int init_websocket_server(const struct device *dev)
{
    pthread_attr_t attr;
    pthread_t thread;

    ARG_UNUSED(dev);

    (void)pthread_attr_init(&attr);
    (void)pthread_attr_setstack(&attr, &websocket_srv_stack, STACKSIZE);

    (void)pthread_create(&thread, &attr, &bws_srv_worker, 0);

    LOG_INF("WebSocket Server was started!");
    return 0;
}

SYS_INIT(init_websocket_server, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
