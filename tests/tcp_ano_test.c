/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise ANO through the shared TCP loop without a legacy data listener. */
#define _GNU_SOURCE
#include <assert.h>
#include "../source/tcp/tcp_server.c"
#include "../source/telemetry/ano.c"
#include "telemetry/ano_test.h"
#include "telemetry/ano_protocol.h"

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
        tcp_ano_status_t status;
        tcp_ano_get_status(ctx, &status);
        if (status.clients == count) return;
        usleep(10000);
    }
    assert(!"TCP client acceptance timed out");
}

/* These checks deliberately decode the wire envelope independently of the
 * protocol implementation, including variable payload lengths and checksums. */
static size_t read_frame(int fd, unsigned char frame[TCP_ANO_FRAME_BYTES])
{
    assert(recv(fd, frame, 6, MSG_WAITALL) == 6);
    assert(frame[0] == 0xAB && frame[1] == 0x01 && frame[2] == 0xFE);
    size_t payload = (size_t)frame[4] | ((size_t)frame[5] << 8);
    assert(payload <= TCP_ANO_FRAME_BYTES - 8);
    assert(recv(fd, frame + 6, payload + 2, MSG_WAITALL) == (ssize_t)(payload + 2));
    unsigned char sum = 0, add = 0;
    for (size_t i = 0; i < payload + 6; i++) {
        sum = (unsigned char)(sum + frame[i]);
        add = (unsigned char)(add + sum);
    }
    assert(frame[payload + 6] == sum && frame[payload + 7] == add);
    return payload + 8;
}

static size_t read_id(int fd, unsigned char id, unsigned char frame[TCP_ANO_FRAME_BYTES])
{
    unsigned long long until = tcp_now_ms() + 2000;
    do {
        size_t length = read_frame(fd, frame);
        if (frame[3] == id) return length;
        assert(frame[3] == 0xF1); /* Periodic samples may precede a query reply. */
    } while (tcp_now_ms() < until);
    assert(!"ANO reply timed out");
    return 0;
}

static void samples_only(int fd, unsigned milliseconds)
{
    unsigned long long until = tcp_now_ms() + milliseconds;
    while (tcp_now_ms() < until) {
        int remaining = (int)(until - tcp_now_ms());
        if (remaining <= 0) break;
        struct pollfd event = {.fd = fd, .events = POLLIN};
        int ready = poll(&event, 1, remaining);
        assert(ready >= 0);
        if (!ready) break;
        unsigned char frame[TCP_ANO_FRAME_BYTES];
        assert(read_frame(fd, frame) == 24 && frame[3] == 0xF1);
    }
}

static size_t host_frame(unsigned char destination, unsigned char source, unsigned char id,
                         const unsigned char *payload, size_t size, unsigned char *out)
{
    out[0] = 0xAB; out[1] = source; out[2] = destination; out[3] = id;
    out[4] = (unsigned char)size; out[5] = (unsigned char)(size >> 8);
    memcpy(out + 6, payload, size);
    unsigned char sum = 0, add = 0;
    for (size_t i = 0; i < size + 6; i++) {
        sum = (unsigned char)(sum + out[i]);
        add = (unsigned char)(add + sum);
    }
    out[size + 6] = sum; out[size + 7] = add;
    return size + 8;
}

static void test_transport(void)
{
    static tcp_ctx_t ctx;
    static struct app_config_t config;
    app_ctx_t app = {.tcp = &ctx, .cfg = &config};
    atomic_store(&app.running, 1);
    assert(!tcp_init(&app));
    assert(ctx.ano_listener == -1);
    int reserved = tcp_listen(0);
    assert(reserved >= 0);
    int first_port = port_of(reserved);
    tcp_ano_stage_t stage;
    /* A busy ANO port must leave the event loop usable. */
    assert(tcp_ano_stage(&ctx, 1, first_port, &stage) == -1);
    close(reserved);
    assert(!tcp_ano_stage(&ctx, 1, first_port, &stage));
    tcp_ano_commit(&ctx, 1, first_port, &stage);
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, tcp_task, &app));
    int telemetry = connect_client(first_port);
    wait_clients(&ctx, 1);
    int second = connect_client(first_port);
    wait_clients(&ctx, 2);
    int excess = connect_client(first_port);
    unsigned char received[TCP_ANO_FRAME_BYTES];
    assert(recv(excess, received, 1, 0) == 0);
    close(excess);
    wait_clients(&ctx, 2);
    /* A vacated slot admits a replacement without disturbing the first client. */
    close(second);
    wait_clients(&ctx, 1);
    second = connect_client(first_port);
    wait_clients(&ctx, 2);

    unsigned char frame[TCP_ANO_FRAME_BYTES];
    float values[4] = {1, -2, 0.5f, -0.0f};
    size_t length = ano_v8_sample(values, frame, sizeof(frame));
    assert(length == 24);
    assert(!tcp_ano_publish(&ctx, frame, length));
    /* The device identifies itself before sending its first sample. */
    read_frame(telemetry, received); assert(received[3] == 0xE3);
    assert(read_frame(telemetry, received) == length && !memcmp(received, frame, length));
    read_frame(second, received); assert(received[3] == 0xE3);
    assert(read_frame(second, received) == length && !memcmp(received, frame, length));
    assert(recv(telemetry, received, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
    unsigned char batch[TCP_ANO_FRAME_BYTES];
    memcpy(batch, frame, length); memcpy(batch + length, frame, length);
    assert(!tcp_ano_publish(&ctx, batch, length * 2));
    for (int i = 0; i < 2; i++) {
        assert(read_frame(telemetry, received) == length && !memcmp(received, frame, length));
        assert(read_frame(second, received) == length && !memcmp(received, frame, length));
    }
    unsigned char large_payload[ANO_V8_MAX_PAYLOAD];
    memset(large_payload, 0x5A, sizeof(large_payload));
    assert(ano_v8_frame(0xF2, large_payload, sizeof(large_payload), batch, sizeof(batch)) == sizeof(batch));
    assert(!tcp_ano_publish(&ctx, batch, sizeof(batch)));
    assert(read_frame(telemetry, received) == sizeof(batch) && !memcmp(received, batch, sizeof(batch)));
    assert(read_frame(second, received) == sizeof(batch) && !memcmp(received, batch, sizeof(batch)));
    assert(tcp_ano_publish(&ctx, batch, sizeof(batch) + 1) == -1);
    assert(tcp_ano_publish(&ctx, NULL, 1) == -1);
    assert(tcp_ano_publish(&ctx, batch, 0) == -1);
    close(second);
    wait_clients(&ctx, 1);

    reserved = tcp_listen(0);
    assert(reserved >= 0);
    int next_port = port_of(reserved);
    close(reserved);
    /* Failed persistence can discard a staged listener without losing clients. */
    assert(!tcp_ano_stage(&ctx, 1, next_port, &stage));
    tcp_ano_discard(&ctx, &stage);
    wait_clients(&ctx, 1);
    assert(!tcp_ano_stage(&ctx, 1, next_port, &stage));
    tcp_ano_commit(&ctx, 1, next_port, &stage);
    assert(recv(telemetry, received, 1, 0) == 0);
    close(telemetry);
    telemetry = connect_client(next_port);
    wait_clients(&ctx, 1);
    assert(!tcp_ano_publish(&ctx, frame, length));
    read_frame(telemetry, received); assert(received[3] == 0xE3);
    assert(read_frame(telemetry, received) == length && !memcmp(received, frame, length));
    tcp_ano_stop(&ctx);
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
        ano_test_sample((unsigned)quarter * 1000, channels);
        ano_test_sample((unsigned)quarter * 1000 + 4000, repeated);
        for (int i = 0; i < 4; i++) {
            assert(fabsf(channels[i] - expected[quarter][i]) < 1e-6f);
            assert(channels[i] == repeated[i]);
        }
    }
    /* Long uptime cannot make phase or output overflow. */
    float channels[4];
    ano_test_sample(UINT64_MAX, channels);
    for (int i = 0; i < 4; i++)
        assert(isfinite(channels[i]) && fabsf(channels[i]) <= 1.0f);
}

static void test_protocol(void)
{
    static const unsigned char golden[] = {
        0xAB, 0x01, 0xFE, 0xF1, 0x10, 0x00,
        0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0xC0,
        0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x80, 0xE9, 0x50
    };
    const float channels[] = {1.0f, -2.0f, 0.5f, -0.0f};
    unsigned char frame[ANO_V8_MAX_FRAME], copy[ANO_V8_MAX_FRAME];
    assert(ano_v8_sample(channels, frame, sizeof(frame)) == sizeof(golden));
    assert(!memcmp(frame, golden, sizeof(golden)));
    assert(ano_v8_valid(golden, sizeof(golden)));
    assert(!ano_v8_valid(golden, sizeof(golden) - 1));
    assert(!ano_v8_valid(NULL, 0));
    for (size_t byte = 0; byte < sizeof(golden); byte++) {
        memcpy(copy, golden, sizeof(golden)); copy[byte] ^= 1;
        assert(!ano_v8_valid(copy, sizeof(golden)));
    }
    assert(!ano_v8_sample(channels, frame, sizeof(golden) - 1));
    assert(!ano_v8_sample(NULL, frame, sizeof(frame)));
    unsigned char payload[ANO_V8_MAX_PAYLOAD];
    memset(payload, 0x5A, sizeof(payload));
    assert(ano_v8_frame(0xF2, payload, sizeof(payload), frame, sizeof(frame)) == sizeof(frame));
    assert(frame[4] == sizeof(payload) && frame[5] == 0 && ano_v8_valid(frame, sizeof(frame)));
    assert(!ano_v8_frame(0xF2, payload, sizeof(payload) + 1, frame, sizeof(frame)));
    assert(!ano_v8_frame(0xF2, NULL, 1, frame, sizeof(frame)));
    size_t info = ano_v8_device_info(frame, sizeof(frame));
    assert(info > 17 && ano_v8_valid(frame, info) && frame[3] == 0xE3);
    assert(frame[6] == 0x01 && frame[13] == 0x20 && frame[14] == 0x03);
    assert(frame[4] == info - 8 && frame[5] == 0);
}

static void test_queries(int fd)
{
    unsigned char request[64], reply[TCP_ANO_FRAME_BYTES], combined[128];
    static const unsigned char info[] = {0, 0, 0};
    static const unsigned char parameters[] = {1, 0, 0};
    size_t length = host_frame(0x01, 0xFE, 0xE0, info, sizeof(info), request);
    /* TCP segmentation must not turn incomplete requests into commands. */
    assert(send(fd, request, 1, MSG_NOSIGNAL) == 1);
    samples_only(fd, 80);
    assert(send(fd, request + 1, 4, MSG_NOSIGNAL) == 4);
    samples_only(fd, 80);
    assert(send(fd, request + 5, length - 5, MSG_NOSIGNAL) == (ssize_t)(length - 5));
    read_id(fd, 0xE3, reply);

    size_t first = host_frame(0x01, 0xFE, 0xE0, parameters, sizeof(parameters), combined);
    size_t second = host_frame(0xFF, 0xFE, 0xC1, info, sizeof(info), combined + first);
    assert(send(fd, combined, first + second, MSG_NOSIGNAL) == (ssize_t)(first + second));
    int parameter_seen = 0, command_seen = 0;
    for (int tries = 0; tries < 128 && (!parameter_seen || !command_seen); tries++) {
        size_t size = read_frame(fd, reply);
        if (reply[3] == 0xF1) continue;
        assert(size == 11);
        if (reply[3] == 0xE0) {
            assert(!parameter_seen && !memcmp(reply + 6, parameters, sizeof(parameters)));
            parameter_seen = 1;
        } else {
            assert(reply[3] == 0xC1 && !command_seen && !memcmp(reply + 6, info, sizeof(info)));
            command_seen = 1;
        }
    }
    assert(parameter_seen && command_seen);

    /* Bad checksums/addresses and unimplemented motion commands are ignored.
     * No actuator symbols are linked into this executable. */
    length = host_frame(0x01, 0xFE, 0xE0, info, sizeof(info), request);
    request[length - 1] ^= 1;
    assert(send(fd, request, length, MSG_NOSIGNAL) == (ssize_t)length);
    length = host_frame(0x02, 0xFE, 0xE0, info, sizeof(info), request);
    assert(send(fd, request, length, MSG_NOSIGNAL) == (ssize_t)length);
    length = host_frame(0x01, 0x01, 0xE0, info, sizeof(info), request);
    assert(send(fd, request, length, MSG_NOSIGNAL) == (ssize_t)length);
    length = host_frame(0x01, 0xFE, 0xC1, parameters, sizeof(parameters), request);
    assert(send(fd, request, length, MSG_NOSIGNAL) == (ssize_t)length);
    length = host_frame(0x01, 0xFE, 0x41, parameters, sizeof(parameters), request);
    assert(send(fd, request, length, MSG_NOSIGNAL) == (ssize_t)length);
    samples_only(fd, 250);

    /* Noise and an impossible declared length must not hide a following valid
     * broadcast query in the same TCP read. */
    const unsigned char noise[] = {0, 0x7F, 0xAB, 0xFE, 0x01, 0xE0, 0xFF, 0xFF};
    memcpy(combined, noise, sizeof(noise));
    length = host_frame(0xFF, 0xFE, 0xE0, info, sizeof(info), combined + sizeof(noise));
    assert(send(fd, combined, sizeof(noise) + length, MSG_NOSIGNAL) == (ssize_t)(sizeof(noise) + length));
    read_id(fd, 0xE3, reply);

    /* A fake but buffer-sized length must also be rejected early for these
     * fixed-size descriptive requests, rather than swallowing future frames. */
    const unsigned char stalled_header[] = {0xAB, 0xFE, 0x01, 0xE0, 0xC8, 0};
    memcpy(combined, stalled_header, sizeof(stalled_header));
    length = host_frame(0x01, 0xFE, 0xE0, info, sizeof(info), combined + sizeof(stalled_header));
    assert(send(fd, combined, sizeof(stalled_header) + length, MSG_NOSIGNAL) ==
           (ssize_t)(sizeof(stalled_header) + length));
    read_id(fd, 0xE3, reply);

    /* The PC's compact one-byte query form is accepted too. */
    length = host_frame(0x01, 0xFE, 0xE0, info, 1, request);
    assert(send(fd, request, length, MSG_NOSIGNAL) == (ssize_t)length);
    read_id(fd, 0xE3, reply);
}

static void test_form_validation(void)
{
    ano_config_t config = {.port = 6667, .rate_hz = 30};
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
    unsigned char frame[TCP_ANO_FRAME_BYTES];
    assert(read_id(fd, 0xF1, frame) == 24);
    assert(frame[4] == 16 && frame[5] == 0);
    for (int i = 0; i < 4; i++) {
        uint32_t bits = 0;
        for (int byte = 0; byte < 4; byte++)
            bits |= (uint32_t)frame[6 + i * 4 + byte] << (8 * byte);
        memcpy(&channels[i], &bits, sizeof(bits));
    }
}

static void test_configuration_and_stream(void)
{
    char temporary[] = "/tmp/car-ano-test-XXXXXX";
    int original_dir = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(original_dir >= 0 && mkdtemp(temporary));
    assert(!chdir(temporary));
    assert(!mkdir("config", 0700));

    int reserved = tcp_listen(0);
    assert(reserved >= 0);
    int port = port_of(reserved);
    close(reserved);
    FILE *file = fopen(ANO_CONFIG, "w");
    assert(file);
    /* An existing configuration predating test_mode must remain compatible. */
    assert(fprintf(file, "enabled 0\nport %d\nrate_hz 30\n", port) > 0);
    assert(!fclose(file));

    static tcp_ctx_t ctx;
    static struct app_config_t config;
    app_ctx_t app = {.tcp = &ctx, .cfg = &config};
    atomic_store(&app.running, 1);
    assert(!tcp_init(&app));
    assert(!ano_init(&app));
    http_ano_get(&app, -1, "GET", "/api/ano", "");
    assert(response_code == 200 && strstr(response_body, "\"test_mode\":0"));
    assert(strstr(response_body, "\"test_active\":false"));
    assert(strstr(response_body, "\"data_source\":\"controller\""));

    http_ano_post(&app, -1, "POST", "/api/ano", "test_mode=2");
    assert(response_code == 400);
    ano_config_t saved = {.port = 6667, .rate_hz = 30};
    assert(!load_config(&saved) && !saved.test_mode && !saved.enabled);
    http_ano_post(&app, -1, "POST", "/api/ano", "enabled=1&test_mode=1");
    assert(response_code == 200 && strstr(response_body, "\"test_mode\":1"));
    assert(!load_config(&saved) && saved.test_mode == 1 && saved.enabled == 1);

    /* Reload the saved mode through the actual module lifecycle. */
    ano_destroy(&app);
    assert(!ano_init(&app));
    http_ano_get(&app, -1, "GET", "/api/ano", "");
    assert(response_code == 200 && strstr(response_body, "\"test_mode\":1"));
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, tcp_task, &app));
    int telemetry = connect_client(port);
    wait_clients(&ctx, 1);

    unsigned char greeting[TCP_ANO_FRAME_BYTES];
    read_frame(telemetry, greeting); assert(greeting[3] == 0xE3);
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
    http_ano_get(&app, -1, "GET", "/api/ano", "");
    assert(response_code == 200 && strstr(response_body, "\"test_active\":true"));
    assert(strstr(response_body, "\"data_source\":\"simulation\""));
    assert(strstr(response_body, "\"feedback_valid\":false"));
    test_queries(telemetry);

    /* A failed port replacement must keep both simulation and the connection. */
    reserved = tcp_listen(0);
    assert(reserved >= 0);
    char form[80];
    snprintf(form, sizeof(form), "port=%d&test_mode=0", port_of(reserved));
    http_ano_post(&app, -1, "POST", "/api/ano", form);
    assert(response_code == 409);
    assert(!load_config(&saved) && saved.test_mode == 1 && saved.port == port);
    read_channels(telemetry, channels);
    for (int i = 0; i < 4; i++) assert(isfinite(channels[i]));
    close(reserved);

    /* Persistence failure discards a staged listener without changing the
     * current mode, saved bytes or established connection, even as root. */
    reserved = tcp_listen(0);
    assert(reserved >= 0);
    int staged_port = port_of(reserved);
    close(reserved);
    assert(!rename(ANO_CONFIG, "config/ano.saved"));
    assert(!mkdir(ANO_CONFIG, 0700));
    snprintf(form, sizeof(form), "port=%d&test_mode=0", staged_port);
    http_ano_post(&app, -1, "POST", "/api/ano", form);
    assert(response_code == 500);
    assert(!rmdir(ANO_CONFIG));
    assert(!rename("config/ano.saved", ANO_CONFIG));
    assert(!load_config(&saved) && saved.test_mode == 1 && saved.port == port);
    read_channels(telemetry, channels);
    for (int i = 0; i < 4; i++) assert(isfinite(channels[i]));
    tcp_ano_status_t transport;
    tcp_ano_get_status(&ctx, &transport);
    assert(transport.listening && transport.clients == 1);

    http_ano_post(&app, -1, "POST", "/api/ano", "test_mode=0");
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
    http_ano_get(&app, -1, "GET", "/api/ano", "");
    assert(strstr(response_body, "\"data_source\":\"controller\""));
    assert(strstr(response_body, "\"speed_actual\":null"));
    assert(strstr(response_body, "\"steering_actual\":null"));

    http_ano_post(&app, -1, "POST", "/api/ano", "enabled=0&test_mode=1");
    assert(response_code == 200 && strstr(response_body, "\"test_active\":false"));
    wait_clients(&ctx, 0);
    close(telemetry);
    atomic_store(&app.running, 0);
    eventfd_signal(ctx.wake_fd);
    assert(!pthread_join(worker, NULL));
    ano_destroy(&app);
    tcp_cleanup(&app);
    assert(!unlink(ANO_CONFIG));
    assert(!rmdir("config"));
    assert(!fchdir(original_dir));
    close(original_dir);
    assert(!rmdir(temporary));
}

int main(void)
{
    test_protocol();
    test_transport();
    test_waveforms();
    test_form_validation();
    test_configuration_and_stream();
    puts("tcp_ano_test: OK");
    return 0;
}
