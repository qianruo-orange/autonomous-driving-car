/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "telemetry/ano_connection.h"
#include "telemetry/ano.h"
#include "telemetry/ano_protocol.h"
#define ANO_REQUEST_INFO 1u
#define ANO_REQUEST_PARAMETERS 2u
#define ANO_REQUEST_COMMANDS 4u

typedef struct {
    int fd;
    unsigned requests;
    unsigned char input[ANO_V8_MAX_FRAME];
    size_t input_length;
} ano_client_t;
static struct {
    int service;
    atomic_int refresh;
    ano_client_t clients[TCP_CLIENTS_PER_PORT];
} connection = {.service = -1};

static void ano_consume_input(ano_client_t *client, const unsigned char *bytes, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        if (client->input_length == sizeof(client->input)) {
            memmove(client->input, client->input + 1, --client->input_length);
        }
        client->input[client->input_length++] = bytes[i];
        while (client->input_length) {
            size_t consumed = 1;
            if (client->input[0] == ANO_V8_HEAD) {
                if (client->input_length < 6)
                    break;
                size_t payload = (size_t)client->input[4] | ((size_t)client->input[5] << 8);
                size_t frame_length = payload + ANO_V8_OVERHEAD;
                /* This read-only endpoint accepts the one-byte query command
                 * and the standard command + two reserved bytes only. Early
                 * filtering prevents a corrupt length from swallowing a later
                 * complete query while waiting for a nonexistent payload. */
                int descriptive = client->input[1] == ANO_V8_HOST_ID &&
                                  (client->input[2] == ANO_V8_DEVICE_ID ||
                                   client->input[2] == ANO_V8_BROADCAST_ID) &&
                                  (client->input[3] == ANO_V8_FID_PARAMETER ||
                                   client->input[3] == ANO_V8_FID_COMMAND_QUERY) &&
                                  (payload == 1 || payload == 3);
                if (descriptive && frame_length <= sizeof(client->input)) {
                    if (client->input_length < frame_length)
                        break;
                    if (ano_v8_valid(client->input, frame_length)) {
                        consumed = frame_length;
                        if (payload && client->input[1] == ANO_V8_HOST_ID &&
                            (client->input[2] == ANO_V8_DEVICE_ID ||
                             client->input[2] == ANO_V8_BROADCAST_ID)) {
                            unsigned request = 0;
                            if (client->input[3] == ANO_V8_FID_PARAMETER) {
                                if (client->input[6] == 0)
                                    request = ANO_REQUEST_INFO;
                                if (client->input[6] == 1)
                                    request = ANO_REQUEST_PARAMETERS;
                            } else if (client->input[3] == ANO_V8_FID_COMMAND_QUERY &&
                                       client->input[6] == 0) {
                                request = ANO_REQUEST_COMMANDS;
                            }
                            client->requests |= request;
                            if (request)
                                connection.refresh = 1;
                        }
                    }
                }
            }
            client->input_length -= consumed;
            memmove(client->input, client->input + consumed, client->input_length);
        }
    }
}

static ano_client_t *client_state(int fd)
{
    for (int i = 0; i < TCP_CLIENTS_PER_PORT; i++)
        if (connection.clients[i].fd == fd)
            return &connection.clients[i];
    return NULL;
}
static void connected(app_ctx_t *app, int port, int fd, void *user)
{
    (void)app;
    (void)port;
    (void)user;
    for (int i = 0; i < TCP_CLIENTS_PER_PORT; i++)
        if (connection.clients[i].fd < 0) {
            connection.clients[i] = (ano_client_t){.fd = fd, .requests = ANO_REQUEST_INFO};
            connection.refresh = 1;
            return;
        }
}
static void disconnected(app_ctx_t *app, int port, int fd, void *user)
{
    (void)app;
    (void)port;
    (void)user;
    ano_client_t *client = client_state(fd);
    if (client)
        *client = (ano_client_t){.fd = -1};
}
static int received(app_ctx_t *app, int port, int fd, const unsigned char *data, size_t length,
                    unsigned char *reply, size_t capacity, void *user)
{
    (void)app;
    (void)port;
    (void)reply;
    (void)capacity;
    (void)user;
    ano_client_t *client = client_state(fd);
    if (!client)
        return -1;
    ano_consume_input(client, data, length);
    return 0;
}
static int prepare_sample(int fd, const unsigned char *frame, size_t length, unsigned char *reply,
                          size_t capacity, void *user)
{
    (void)user;
    ano_client_t *client = client_state(fd);
    if (!client || length > capacity)
        return -1;
    size_t used = 0, bytes;
    /* Only descriptive queries are supported. Parameters and movement
     * commands remain controlled by the authenticated Web/CAN paths. */
    if (client->requests & ANO_REQUEST_INFO) {
        bytes = ano_v8_device_info(reply, capacity - length);
        if (bytes) {
            used += bytes;
            client->requests &= ~ANO_REQUEST_INFO;
        }
    }
    if (client->requests & ANO_REQUEST_PARAMETERS) {
        const unsigned char empty[] = {1, 0, 0};
        bytes = ano_v8_frame(ANO_V8_FID_PARAMETER, empty, sizeof(empty), reply + used,
                             capacity - length - used);
        if (bytes) {
            used += bytes;
            client->requests &= ~ANO_REQUEST_PARAMETERS;
        }
    }
    if (client->requests & ANO_REQUEST_COMMANDS) {
        const unsigned char empty[] = {0, 0, 0};
        bytes = ano_v8_frame(ANO_V8_FID_COMMAND_QUERY, empty, sizeof(empty), reply + used,
                             capacity - length - used);
        if (bytes) {
            used += bytes;
            client->requests &= ~ANO_REQUEST_COMMANDS;
        }
    }

    memcpy(reply + used, frame, length);
    if (client->requests)
        connection.refresh = 1;
    return (int)(used + length);
}
static int poll_connection(app_ctx_t *app, void *user)
{
    (void)user;
    ano_tick(app);
    return ano_wait_ms(app);
}
static const tcp_service_ops_t protocol = {
    .connected = connected,
    .disconnected = disconnected,
    .receive = received,
    .prepare = prepare_sample,
    .poll = poll_connection,
};
void ano_connection_init(void)
{
    connection.service = -1;
    connection.refresh = 0;
    for (int i = 0; i < TCP_CLIENTS_PER_PORT; i++)
        connection.clients[i].fd = -1;
}
int ano_connection_stage(tcp_ctx_t *ctx, int enabled, int port, ano_connection_stage_t *stage)
{
    if (port < 1024 || port > 65535) {
        errno = EINVAL;
        return -1;
    }
    return tcp_service_stage(ctx, connection.service, enabled, port, &protocol, NULL, stage);
}
void ano_connection_commit(tcp_ctx_t *ctx, int enabled, int port, ano_connection_stage_t *stage)
{
    (void)enabled;
    (void)port;
    tcp_service_commit(ctx, stage);
    connection.service = stage->service;
    connection.refresh = 1;
}
void ano_connection_discard(tcp_ctx_t *ctx, ano_connection_stage_t *stage)
{
    tcp_service_discard(ctx, stage);
}
void ano_connection_stop(tcp_ctx_t *ctx) { tcp_service_disable(ctx, connection.service); }
int ano_connection_take_refresh(tcp_ctx_t *ctx)
{
    (void)ctx;
    return atomic_exchange(&connection.refresh, 0);
}
int ano_connection_publish(tcp_ctx_t *ctx, const unsigned char *data, size_t length)
{
    return tcp_service_publish(ctx, connection.service, data, length);
}
void ano_connection_get_status(tcp_ctx_t *ctx, ano_connection_status_t *out)
{
    tcp_service_get_status(ctx, connection.service, out);
}
