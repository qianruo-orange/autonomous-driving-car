/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise both protocols through the real TCP event loop on loopback. */
#define _GNU_SOURCE
#include <assert.h>
#include "../source/tcp/tcp_server.c"

void _log_info(const char *file, int line, const char *fmt, ...)
{ (void)file; (void)line; (void)fmt; }
void _log_error(const char *file, int line, const char *fmt, ...)
{ (void)file; (void)line; (void)fmt; }
int watchdog_feed_thread(pthread_t tid, const char *name)
{ (void)tid; (void)name; return 0; }
void vofa_tick(app_ctx_t *app) { (void)app; }
int vofa_wait_ms(app_ctx_t *app) { (void)app; return 10; }

static int port_of(int fd)
{
    struct sockaddr_in address;
    socklen_t size = sizeof(address);
    assert(!getsockname(fd, (struct sockaddr *)&address, &size));
    return ntohs(address.sin_port);
}

static int connect_client(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(fd >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = htons(port)};
    assert(!connect(fd, (struct sockaddr *)&address, sizeof(address)));
    struct timeval timeout = {.tv_sec = 2};
    assert(!setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    return fd;
}

static void wait_clients(tcp_ctx_t *ctx, int count)
{
    for (int i = 0; i < 200; i++) {
        tcp_vofa_status_t status;
        tcp_vofa_get_status(ctx, &status);
        pthread_mutex_lock(&ctx->client_mutex);
        int ordinary = ctx->client_count > 0 && ctx->clients[0].fd >= 0;
        pthread_mutex_unlock(&ctx->client_mutex);
        if (status.clients == count && ordinary) return;
        usleep(10000);
    }
    assert(!"TCP client acceptance timed out");
}

int main(void)
{
    static tcp_ctx_t ctx;
    static struct app_config_t config;
    app_ctx_t app = {.tcp = &ctx, .cfg = &config};
    config.args.max_clients = TCP_MAX_CLIENTS;
    atomic_store(&app.running, 1);
    assert(!tcp_init(&app)); /* Port zero asks the OS for an unused data port. */
    int data_port = port_of(ctx.listen_fd);
    int reserved = tcp_listen(0, NULL);
    assert(reserved >= 0);
    int first_port = port_of(reserved);
    tcp_vofa_stage_t stage;
    /* A busy port must leave the ordinary endpoint untouched. */
    assert(tcp_vofa_stage(&ctx, 1, data_port, &stage) == -1);
    assert(tcp_vofa_stage(&ctx, 1, first_port, &stage) == -1);
    close(reserved);
    assert(!tcp_vofa_stage(&ctx, 1, first_port, &stage));
    tcp_vofa_commit(&ctx, 1, first_port, &stage);
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, tcp_task, &app));
    int ordinary = connect_client(data_port);
    int telemetry = connect_client(first_port);
    wait_clients(&ctx, 1);

    unsigned char frame[TCP_VOFA_FRAME_BYTES] = {0};
    frame[18] = 0x80; frame[19] = 0x7f;
    assert(!tcp_vofa_publish(&ctx, frame));
    assert(!tcp_tx_packet(&ctx, -1, "CAN\n", 4));
    unsigned char received[20];
    assert(recv(telemetry, received, sizeof(received), MSG_WAITALL) == 20);
    assert(!memcmp(received, frame, sizeof(frame)));
    assert(recv(ordinary, received, 4, MSG_WAITALL) == 4);
    assert(!memcmp(received, "CAN\n", 4));
    assert(recv(ordinary, received, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
    assert(recv(telemetry, received, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);

    reserved = tcp_listen(0, NULL);
    assert(reserved >= 0);
    int next_port = port_of(reserved);
    close(reserved);
    /* Failed persistence can discard a staged listener without losing clients. */
    assert(!tcp_vofa_stage(&ctx, 1, next_port, &stage));
    tcp_vofa_discard(&ctx, &stage);
    wait_clients(&ctx, 1);
    assert(!tcp_vofa_stage(&ctx, 1, next_port, &stage));
    tcp_vofa_commit(&ctx, 1, next_port, &stage);
    assert(recv(telemetry, received, 1, 0) == 0);
    close(telemetry);
    telemetry = connect_client(next_port);
    wait_clients(&ctx, 1);
    assert(!tcp_vofa_publish(&ctx, frame));
    assert(recv(telemetry, received, sizeof(received), MSG_WAITALL) == 20);
    assert(!memcmp(received, frame, sizeof(frame)));
    tcp_vofa_stop(&ctx);
    assert(recv(telemetry, received, 1, 0) == 0);
    assert(!tcp_tx_packet(&ctx, -1, "OK", 2));
    assert(recv(ordinary, received, 2, MSG_WAITALL) == 2);
    assert(!memcmp(received, "OK", 2));
    atomic_store(&app.running, 0);
    eventfd_signal(ctx.tx_efd);
    assert(!pthread_join(worker, NULL));
    close(ordinary); close(telemetry);
    tcp_cleanup(&app);
    puts("tcp_vofa_test: OK");
    return 0;
}
