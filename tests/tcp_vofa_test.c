/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise VOFA through the shared TCP loop without a legacy data listener. */
#define _GNU_SOURCE
#include <assert.h>
#include "../source/tcp/tcp_server.c"
#include "../source/telemetry/vofa.c"
#include "telemetry/vofa_test.h"

void _log_info(const char *file, int line, const char *fmt, ...)
{ (void)file; (void)line; (void)fmt; }
void _log_error(const char *file, int line, const char *fmt, ...)
{ (void)file; (void)line; (void)fmt; }
int watchdog_feed_thread(pthread_t tid, const char *name)
{ (void)tid; (void)name; return 0; }

/* The only controller access available to the telemetry module is read-only. */
int follow_telemetry_snapshot(follow_telemetry_t *out)
{
    *out = (follow_telemetry_t){.speed_target = 0.42, .steering_target = -0.1,
        .speed_actual = NAN, .steering_actual = NAN};
    return 0;
}

static int response_code;
static char response_body[4096];

void http_send_response(int fd, int code, const char *status, const char *mime,
                        const void *body, size_t length)
{
    (void)fd; (void)status; (void)mime;
    assert(length < sizeof(response_body));
    memcpy(response_body, body, length);
    response_body[length] = 0;
    response_code = code;
}

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
        if (status.clients == count) return;
        usleep(10000);
    }
    assert(!"TCP client acceptance timed out");
}

static void test_transport(void)
{
    static tcp_ctx_t ctx;
    static struct app_config_t config;
    app_ctx_t app = {.tcp = &ctx, .cfg = &config};
    atomic_store(&app.running, 1);
    assert(!tcp_init(&app));
    assert(ctx.vofa_listener == -1);
    int reserved = tcp_listen(0);
    assert(reserved >= 0);
    int first_port = port_of(reserved);
    tcp_vofa_stage_t stage;
    /* A busy VOFA port must leave the event loop usable. */
    assert(tcp_vofa_stage(&ctx, 1, first_port, &stage) == -1);
    close(reserved);
    assert(!tcp_vofa_stage(&ctx, 1, first_port, &stage));
    tcp_vofa_commit(&ctx, 1, first_port, &stage);
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, tcp_task, &app));
    int telemetry = connect_client(first_port);
    wait_clients(&ctx, 1);
    int second = connect_client(first_port);
    wait_clients(&ctx, 2);
    int excess = connect_client(first_port);
    unsigned char received[20];
    assert(recv(excess, received, 1, 0) == 0);
    close(excess);
    wait_clients(&ctx, 2);
    /* A vacated slot admits a replacement without disturbing the first client. */
    close(second);
    wait_clients(&ctx, 1);
    second = connect_client(first_port);
    wait_clients(&ctx, 2);

    unsigned char frame[TCP_VOFA_FRAME_BYTES] = {0};
    frame[18] = 0x80; frame[19] = 0x7f;
    assert(!tcp_vofa_publish(&ctx, frame));
    assert(recv(telemetry, received, sizeof(received), MSG_WAITALL) == 20);
    assert(!memcmp(received, frame, sizeof(frame)));
    assert(recv(second, received, sizeof(received), MSG_WAITALL) == 20);
    assert(!memcmp(received, frame, sizeof(frame)));
    assert(recv(telemetry, received, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
    close(second);
    wait_clients(&ctx, 1);

    reserved = tcp_listen(0);
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
    atomic_store(&app.running, 0);
    eventfd_signal(ctx.wake_fd);
    assert(!pthread_join(worker, NULL));
    close(telemetry);
    tcp_cleanup(&app);
}

static void test_waveforms(void)
{
    const float expected[4][4] = {
        {0, 1, -1, 1}, {1, 0, 0, 1}, {0, -1, 1, -1}, {-1, 0, 0, -1}
    };
    for (int quarter = 0; quarter < 4; quarter++) {
        float channels[4], repeated[4];
        vofa_test_sample((unsigned)quarter * 1000, channels);
        vofa_test_sample((unsigned)quarter * 1000 + 4000, repeated);
        for (int i = 0; i < 4; i++) {
            assert(fabsf(channels[i] - expected[quarter][i]) < 1e-6f);
            assert(channels[i] == repeated[i]);
        }
    }
    /* Long uptime cannot make phase or output overflow. */
    float channels[4];
    vofa_test_sample(UINT64_MAX, channels);
    for (int i = 0; i < 4; i++)
        assert(isfinite(channels[i]) && fabsf(channels[i]) <= 1.0f);
}

static void test_form_validation(void)
{
    vofa_config_t config = {.port = 6667, .rate_hz = 30};
    assert(!parse_form("enabled=1&port=7777&rate_hz=50&test_mode=1", &config));
    assert(config.enabled == 1 && config.port == 7777 && config.rate_hz == 50 &&
           config.test_mode == 1);
    const char *invalid[] = {
        "test_mode=2", "test_mode=-1", "test_mode=true", "test_mode=1.0",
        "test_mode=", "test_mode=0&test_mode=1", "test_mode=1&", "unknown=1"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        assert(parse_form(invalid[i], &config) == -1);
}

static void read_channels(int fd, float channels[4])
{
    unsigned char frame[TCP_VOFA_FRAME_BYTES];
    assert(recv(fd, frame, sizeof(frame), MSG_WAITALL) == sizeof(frame));
    assert(!memcmp(frame + 16, "\x00\x00\x80\x7f", 4));
    for (int i = 0; i < 4; i++) {
        uint32_t bits = 0;
        for (int byte = 0; byte < 4; byte++)
            bits |= (uint32_t)frame[i * 4 + byte] << (8 * byte);
        memcpy(&channels[i], &bits, sizeof(bits));
    }
}

static void test_configuration_and_stream(void)
{
    char temporary[] = "/tmp/car-vofa-test-XXXXXX";
    int original_dir = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(original_dir >= 0 && mkdtemp(temporary));
    assert(!chdir(temporary));
    assert(!mkdir("config", 0700));

    int reserved = tcp_listen(0);
    assert(reserved >= 0);
    int port = port_of(reserved);
    close(reserved);
    FILE *file = fopen(VOFA_CONFIG, "w");
    assert(file);
    /* An existing configuration predating test_mode must remain compatible. */
    assert(fprintf(file, "enabled 0\nport %d\nrate_hz 30\n", port) > 0);
    assert(!fclose(file));

    static tcp_ctx_t ctx;
    static struct app_config_t config;
    app_ctx_t app = {.tcp = &ctx, .cfg = &config};
    atomic_store(&app.running, 1);
    assert(!tcp_init(&app));
    assert(!vofa_init(&app));
    http_vofa_get(&app, -1, "GET", "/api/vofa", "");
    assert(response_code == 200 && strstr(response_body, "\"test_mode\":0"));
    assert(strstr(response_body, "\"test_active\":false"));
    assert(strstr(response_body, "\"data_source\":\"controller\""));

    http_vofa_post(&app, -1, "POST", "/api/vofa", "test_mode=2");
    assert(response_code == 400);
    vofa_config_t saved = {.port = 6667, .rate_hz = 30};
    assert(!load_config(&saved) && !saved.test_mode && !saved.enabled);
    http_vofa_post(&app, -1, "POST", "/api/vofa", "enabled=1&test_mode=1");
    assert(response_code == 200 && strstr(response_body, "\"test_mode\":1"));
    assert(!load_config(&saved) && saved.test_mode == 1 && saved.enabled == 1);

    /* Reload the saved mode through the actual module lifecycle. */
    vofa_destroy(&app);
    assert(!vofa_init(&app));
    http_vofa_get(&app, -1, "GET", "/api/vofa", "");
    assert(response_code == 200 && strstr(response_body, "\"test_mode\":1"));
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, tcp_task, &app));
    int telemetry = connect_client(port);
    wait_clients(&ctx, 1);

    float first[4], channels[4];
    read_channels(telemetry, first);
    for (int sample = 0; sample < 8; sample++) {
        read_channels(telemetry, channels);
        for (int i = 0; i < 4; i++)
            assert(isfinite(channels[i]) && fabsf(channels[i]) <= 1.0f);
        assert(fabsf(channels[0] * channels[0] + channels[1] * channels[1] - 1) < 1e-5f);
        assert(channels[3] == 1 || channels[3] == -1);
    }
    assert(fabsf(channels[0] - first[0]) + fabsf(channels[1] - first[1]) > 0.01f);
    http_vofa_get(&app, -1, "GET", "/api/vofa", "");
    assert(response_code == 200 && strstr(response_body, "\"test_active\":true"));
    assert(strstr(response_body, "\"data_source\":\"simulation\""));
    assert(strstr(response_body, "\"feedback_valid\":false"));

    /* A failed port replacement must keep both simulation and the connection. */
    reserved = tcp_listen(0);
    assert(reserved >= 0);
    char form[80];
    snprintf(form, sizeof(form), "port=%d&test_mode=0", port_of(reserved));
    http_vofa_post(&app, -1, "POST", "/api/vofa", form);
    assert(response_code == 409);
    assert(!load_config(&saved) && saved.test_mode == 1 && saved.port == port);
    read_channels(telemetry, channels);
    for (int i = 0; i < 4; i++) assert(isfinite(channels[i]));
    close(reserved);

    http_vofa_post(&app, -1, "POST", "/api/vofa", "test_mode=0");
    assert(response_code == 200 && strstr(response_body, "\"test_mode\":0"));
    /* Already transmitted test frames may precede the first controller frame. */
    int controller_seen = 0;
    for (int i = 0; i < 32; i++) {
        read_channels(telemetry, channels);
        if (isnan(channels[1]) && isnan(channels[3])) {
            assert(fabsf(channels[0] - 0.42f) < 1e-6f);
            assert(fabsf(channels[2] + 0.1f) < 1e-6f);
            controller_seen = 1;
            break;
        }
    }
    assert(controller_seen);
    http_vofa_get(&app, -1, "GET", "/api/vofa", "");
    assert(strstr(response_body, "\"data_source\":\"controller\""));
    assert(strstr(response_body, "\"speed_actual\":null"));
    assert(strstr(response_body, "\"steering_actual\":null"));

    http_vofa_post(&app, -1, "POST", "/api/vofa", "enabled=0&test_mode=1");
    assert(response_code == 200 && strstr(response_body, "\"test_active\":false"));
    wait_clients(&ctx, 0);
    close(telemetry);
    atomic_store(&app.running, 0);
    eventfd_signal(ctx.wake_fd);
    assert(!pthread_join(worker, NULL));
    vofa_destroy(&app);
    tcp_cleanup(&app);
    assert(!unlink(VOFA_CONFIG));
    assert(!rmdir("config"));
    assert(!fchdir(original_dir));
    close(original_dir);
    assert(!rmdir(temporary));
}

int main(void)
{
    test_transport();
    test_waveforms();
    test_form_validation();
    test_configuration_and_stream();
    puts("tcp_vofa_test: OK");
    return 0;
}
