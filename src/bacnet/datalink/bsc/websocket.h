/**
 * @file
 * @brief Client/Server thread-safe websocket interface API.
 * @author Kirill Neznamov
 * @date May 2022
 * @section LICENSE
 *
 * Copyright (C) 2022 Legrand North America, LLC
 * as an unpublished work.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef __BSC__WEBSOCKET__INCLUDED__
#define __BSC__WEBSOCKET__INCLUDED__

#include "bacnet/bacdef.h"
#include "bacnet/bacenum.h"


/**
 * Maximum number of sockets that can be opened on client's side.
 * @{
 */
#ifndef BSC_CONF_CLIENT_CONNECTIONS_NUM
#define BSC_CLIENT_WEBSOCKETS_MAX_NUM 4
#else
#define BSC_CLIENT_WEBSOCKETS_MAX_NUM BSC_CONF_CLIENT_CONNECTIONS_NUM
#endif

/** @} */

/**
 * Maximum number of sockets supported for hub websocket server
 * @{
 */
#ifndef BSC_CONF_SERVER_HUB_CONNECTIONS_MAX_NUM
#define BSC_SERVER_HUB_WEBSOCKETS_MAX_NUM 4
#else
#define BSC_SERVER_HUB_WEBSOCKETS_MAX_NUM BSC_CONF_SERVER_HUB_CONNECTIONS_MAX_NUM
#endif

/** @} */


/**
 * Maximum number of sockets supported for direct websocket server
 * @{
 */
#ifndef BSC_CONF_SERVER_DIRECT_CONNECTIONS_MAX_NUM
#define BSC_SERVER_DIRECT_WEBSOCKETS_MAX_NUM 4
#else
#define BSC_SERVER_DIRECT_WEBSOCKETS_MAX_NUM BSC_CONF_SERVER_DIRECT_CONNECTIONS_MAX_NUM
#endif

#define BSC_WSURL_MAX_LEN 256

typedef int BSC_WEBSOCKET_HANDLE;
#define BSC_WEBSOCKET_INVALID_HANDLE (-1)

// Websockets protocol defined in BACnet/SC \S AB.7.1.
#define BSC_WEBSOCKET_HUB_PROTOCOL_STR "hub.bsc.bacnet.org"
#define BSC_WEBSOCKET_DIRECT_PROTOCOL_STR "dc.bsc.bacnet.org"

typedef enum {
    BSC_WEBSOCKET_HUB_PROTOCOL = 0,
    BSC_WEBSOCKET_DIRECT_PROTOCOL = 1,
    BSC_WEBSOCKET_PROTOCOLS_AMOUNT = 2   // must be always last
} BSC_WEBSOCKET_PROTOCOL;

typedef enum {
    BSC_WEBSOCKET_SUCCESS = 0,
    BSC_WEBSOCKET_CLOSED = 1,
    BSC_WEBSOCKET_NO_RESOURCES = 2,
    BSC_WEBSOCKET_BAD_PARAM = 3,
    BSC_WEBSOCKET_INVALID_OPERATION = 4
} BSC_WEBSOCKET_RET;

typedef enum {
    BSC_WEBSOCKET_CONNECTED = 0,
    BSC_WEBSOCKET_DISCONNECTED = 1,
    BSC_WEBSOCKET_RECEIVED = 2,
    BSC_WEBSOCKET_SENDABLE = 3,
    BSC_WEBSOCKET_SERVER_STARTED = 4,
    BSC_WEBSOCKET_SERVER_STOPPED = 5
} BSC_WEBSOCKET_EVENT;

/** @} */
 
typedef void (*BSC_WEBSOCKET_CLI_DISPATCH) (BSC_WEBSOCKET_HANDLE h,
                              BSC_WEBSOCKET_EVENT ev,
                              uint8_t* buf,
                              size_t bufsize);

typedef void (*BSC_WEBSOCKET_SRV_DISPATCH) (BSC_WEBSOCKET_PROTOCOL proto,
                              BSC_WEBSOCKET_HANDLE h,
                              BSC_WEBSOCKET_EVENT ev,
                              uint8_t* buf,
                              size_t bufsize);

/**
 * @brief Asynchronous bws_cli_сonnect() function starts establishing
 * of a new connection to a websocket server specified by url parameter.
 * Result of completition of operation is call of dispatch_func() with
 * BSC_WEBSOCKET_CONNECTED in a case if connection established successfully or
 * BSC_WEBSOCKET_DISCONNECTED if connection attempt failed.
 *
 * @param type - type of BACNet/SC connection, check
 *    BSC_WEBSOCKET_CONNECTION_TYPE enum. According BACNet standard
 *    different type of connections require different websocket protocols.
 * @param url - BACNet/SC server URL. For example: wss://legrand.com:8080.
 * @param ca_cert - pointer to certificate authority (CA) cert in PEM or DER
 *                  format.
 * @param ca_cert_size - size in bytes of CA cert.
 * @param cert - pointer to client certificate in PEM or DER format.
 * @param cert_size - size in bytes of client certificate.
 * @param key - pointer to client private key in PEM or DER format.
 * @param key_size - size of private key in bytes of of client certificate.
 * @param timeout - timeout for connect operation in seconds. Must not
 *                  be NULL.
 * @param dispatch_func - pointer to dispatch callback function to handle
 *                        events from websocket specified by *out_handle.
 * @param out_handle - pointer to a websocket handle.
 *
 * @return error code from BSC_WEBSOCKET_RET enum.
 *    The following error codes can be returned:
 *     BSC_WEBSOCKET_BAD_PARAM - In a case if some input parameter is
 *                                  incorrect.
 *     BSC_WEBSOCKET_NO_RESOURCES - if a user has already opened
 *         more sockets than the limit defined by BSC_CLIENT_WEBSOCKETS_MAX_NUM,
 *         or if some mem allocation has failed or some allocation of system
 *         resources like mutex, thread, etc .., failed.
 *     BSC_WEBSOCKET_SUCCESS - connect operation was successfully started.
 */

BSC_WEBSOCKET_RET bws_cli_connect
   (BSC_WEBSOCKET_PROTOCOL proto,
       char *url,
       uint8_t *ca_cert,
       size_t ca_cert_size,
       uint8_t *cert,
       size_t cert_size,
       uint8_t *key,
       size_t key_size,
       size_t timeout_s,
       BSC_WEBSOCKET_CLI_DISPATCH dispatch_func,
       BSC_WEBSOCKET_HANDLE *out_handle);

/**
 * @brief Asynchronous  bws_cli_disconnnect() function starts process of
 * disconnection for specified websocket handle. When the process completes,
 * dispatch_func() with event BSC_WEBSOCKET_DISCONNECTED is called.
 * connection to some websocket server.
 *
 * @param h - websocket handle.
 *
 */

void bws_cli_disconnect(BSC_WEBSOCKET_HANDLE h);

/**
 * @brief Non-blocked bws_cli_send() function signals to the websocket
 * specified by websocket handle h that some data is needed to be sent.
 * When websocket becomes sendable, dispatch_func() is called with
 * event BSC_WEBSOCKET_SENDABLE and data can be sent from dispatch_func()
 * call using bws_cli_dispatch_send() call.
 *
 * @param h - websocket handle.
 *
 */

void bws_cli_send(BSC_WEBSOCKET_HANDLE h);

/**
 * @brief bws_cli_dispatch_send() function sends data to a websocket server
 *        in a case if websocket handle is sendable (e.g. ready to send data).
 *        In as case if data was not sent for some reasons thic could result
 *        dispatch_func() cal withe event  BSC_WEBSOCKET_DISCONNECTED
 * @param h - websocket handle.
 * @param payload - pointer to a data to send.
 * @param payload_size - size in bytes of data to send.
 *
 * @return error code from BSC_WEBSOCKET_RET enum.
 *    The following error codes can be returned:
 *     BSC_WEBSOCKET_BAD_PARAM - In a case if some input parameter is
 *                               incorrect.
 *     BSC_WEBSOCKET_NO_RESOURCES - if some mem allocation has failed o
 *         some allocation of system resources like mutex, thread,
 *         etc .., has failed.
 *     BSC_WEBSOCKET_INVALID_OPERATION - if the function was called not from
 *         dispatch_func() callback context or websocket is not in connected
 *         state.
 *     BSC_WEBSOCKET_SUCCESS - data is sent successfuly.
 */

BSC_WEBSOCKET_RET bws_cli_dispatch_send(BSC_WEBSOCKET_HANDLE h,
                           uint8_t *payload, size_t payload_size);

/**
 * @brief Asynchronous bws_srv_start() function triggers process of
 * starting of a websocket server on a specified port for specified
 * BACNet websocket protocol. At present time peer can have only 2
 * instances of server: onerelates to BSC_WEBSOCKET_HUB_PROTOCOL and 
 * the other to BSC_WEBSOCKET_HUB_PROTOCOL. When process completes,
 * dispatch_func() is called with BSC_WEBSOCKET_SERVER_STARTED
 * event.
 *
 * @param proto - type of BACNet websocket protocol defined in
 *                BSC_WEBSOCKET_PROTOCOL enum.
 * @param port- port number.
 * @param ca_cert - pointer to certificate authority (CA) cert in PEM or DER
 * format.
 * @param ca_cert_size - size in bytes of CA cert.
 * @param cert - pointer to server certificate in PEM or DER format.
 * @param cert_size - size in bytes of server certificate.
 * @param key - pointer to server private key in PEM or DER format.
 * @param key_size - size of private key in bytes of of client certificate.
 * @param dispatch_func - pointer to dispatch callback function to handle
 *                        events from a websocket which is corresponded to
 *                        server specified by proto param.
 *
 * @return error code from BSC_WEBSOCKET_RET enum.
 *  The following error codes can be returned:
 *    BSC_WEBSOCKET_BAD_PARAM - In a case if some input parameter is
 *            incorrect.
 *    BSC_WEBSOCKET_NO_RESOURCES - if a user has already opened
 *            more sockets than the limit defined to corresponded protocol
 *            (BSC_SERVER_HUB_WEBSOCKETS_MAX_NUM or
 *             BSC_CLIENT_WEBSOCKETS_MAX_NUM), or if some mem allocation
 *             has failed or some allocation of system resources like
 *             mutex, thread, condition variable etc .., failed.
 *    BSC_WEBSOCKET_SUCCESS - the operation is started or server was already
 *            started before.
 *    BSC_WEBSOCKET_INVALID_OPERATION - operation is not started because
 *            server in a process of shutdown.
 */

BSC_WEBSOCKET_RET bws_srv_start(
                        BSC_WEBSOCKET_PROTOCOL proto,
                        int port,
                        uint8_t *ca_cert,
                        size_t ca_cert_size,
                        uint8_t *cert,
                        size_t cert_size,
                        uint8_t *key,
                        size_t key_size,
                        BSC_WEBSOCKET_SRV_DISPATCH dispatch_func);

/**
 * @brief Asynchronous bws_srv_stop() function starts process of a shutdowns
 * of a websocket server specified by proto param. 
 * opened websocket connections are closed.
 *
 * @return error code from BSC_WEBSOCKET_RET enum.
 *    The following error codes can be returned:
 *         BSC_WEBSOCKET_SUCCESS - the operation is started or server was already
 *            stopped before.
 *         BSC_WEBSOCKET_INVALID_OPERATION - if server was not started or
 *                server shutdown is already in progress.
 */

BSC_WEBSOCKET_RET bws_srv_stop(BSC_WEBSOCKET_PROTOCOL proto);

/**
 * @brief Asynchronous bws_srv_disconnnect() function starts process of
 * disconnection for specified websocket handle h for specified server type
 * by proto parameter. When the process completes, dispatch_func() with event
 * BSC_WEBSOCKET_DISCONNECTED is called.
 *
 * @param proto - type of BACNet websocket protocol defined in
 *                BSC_WEBSOCKET_PROTOCOL enum.
 * @param h - websocket handle.
 *
 */

void bws_srv_disconnect(BSC_WEBSOCKET_PROTOCOL proto, BSC_WEBSOCKET_HANDLE h);

/**
 * @brief Asynchronous bws_srv_send() function signals to a websocket
 * specified by handle h for specified server type by proto param that
 * some data is needed to be sent.
 *
 * When websocket becomes sendable, dispatch_func() is called with
 * event BSC_WEBSOCKET_SENDABLE and data can be sent from dispatch_func()
 * call using bws_srv_dispatch_send() call.
 *
 * @param proto - type of BACNet websocket protocol defined in
 *                BSC_WEBSOCKET_PROTOCOL enum.
 * @param h - websocket handle.
 *
 */

void bws_srv_send(BSC_WEBSOCKET_PROTOCOL proto, BSC_WEBSOCKET_HANDLE h);

/**
 * @brief bws_srv_dispatch_send() function sends data to a websocket server
 *        in a case if websocket handle is sendable (e.g. ready to send data).
 *        In as case if data was not sent for some reasons thic could result
 *        dispatch_func() cal withe event  BSC_WEBSOCKET_DISCONNECTED
 *
 * @param proto - type of BACNet websocket protocol defined in
 *                BSC_WEBSOCKET_PROTOCOL enum.
 * @param h - websocket handle.
 * @param payload - pointer to a data to send.
 * @param payload_size - size in bytes of data to send.
 *
 * @return error code from BSC_WEBSOCKET_RET enum.
 *    The following error codes can be returned:
 *     BSC_WEBSOCKET_BAD_PARAM - In a case if some input parameter is
 *                               incorrect.
 *     BSC_WEBSOCKET_NO_RESOURCES - if some mem allocation has failed o
 *         some allocation of system resources like mutex, thread,
 *         etc .., has failed.
 *     BSC_WEBSOCKET_INVALID_OPERATION - if the function was called not from
 *         dispatch_func() callback context or websocket is not in connected
 *         state.
 *     BSC_WEBSOCKET_SUCCESS - data is sent successfuly.
 */

BSC_WEBSOCKET_RET bws_srv_dispatch_send(BSC_WEBSOCKET_PROTOCOL proto,
                                        BSC_WEBSOCKET_HANDLE h,
                                        uint8_t *payload, size_t payload_size);
#endif