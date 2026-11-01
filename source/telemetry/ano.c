/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "telemetry/ano.h"
#include "control/car_sport_ctrl.h"
#include "http/http_internal.h"
#include "tcp/tcp_server.h"
#include "telemetry/ano_protocol.h"
#include "telemetry/ano_simulation.h"
#include <math.h>
#include <sys/stat.h>
#include <time.h>

#define ANO_FRAME_BYTES 24
#define ANO_REQUEST_INFO 1u
#define ANO_REQUEST_PARAMETERS 2u
#define ANO_REQUEST_COMMANDS 4u

typedef struct {
    uint64_t handle;
    unsigned requests;
    unsigned char input[ANO_V8_MAX_FRAME];
    size_t input_length;
} ano_client_t;
/* ANO 独占接收槽和半包状态；TCP 层只投递事件，不操作这些业务状态。 */
static struct {
    atomic_uint_fast64_t handle;
    tcp_slot_t rx;
    atomic_int refresh;
    ano_client_t clients[TCP_CLIENTS_PER_PORT];
} connection = {.handle = 0};

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

static ano_client_t *client_state(uint64_t handle)
{
    for (int i = 0; i < TCP_CLIENTS_PER_PORT; i++)
        if (connection.clients[i].handle == handle)
            return &connection.clients[i];
    return NULL;
}
/* 同一数据槽维持连接事件顺序；溢出复位时丢弃所有旧半包。
 * 每次最多处理一个槽容量，防止持续收包饿死遥测发送。 */
static void ano_process_slot(void)
{
    tcp_data_t item;
    for (int n = 0; n < TCP_SLOT_CAPACITY && data_slot_pop(&connection.rx.queue, &item); n++) {
        if (item.kind == TCP_RESET) {
            memset(connection.clients, 0, sizeof(connection.clients));
            continue;
        }
        ano_client_t *client = client_state(item.handle);
        if (item.kind == TCP_CONNECTED) {
            if (!client)
                for (int i = 0; i < TCP_CLIENTS_PER_PORT; i++)
                    if (!connection.clients[i].handle) {
                        client = &connection.clients[i];
                        break;
                    }
            if (client) {
                *client = (ano_client_t){.handle = item.handle, .requests = ANO_REQUEST_INFO};
                connection.refresh = 1;
            }
        } else if (client && item.kind == TCP_DISCONNECTED) {
            *client = (ano_client_t){0};
        } else if (client && item.kind == TCP_DATA) {
            ano_consume_input(client, item.bytes, item.length);
        }
    }
}
static int prepare_sample(uint64_t handle, const unsigned char *frame, size_t length,
                          unsigned char *reply, size_t capacity, void *user)
{
    (void)user;
    ano_client_t *client = client_state(handle);
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
    ano_process_slot();
    ano_tick(app);
    return ano_wait_ms(app);
}
static const tcp_component_t protocol = {.slot = &connection.rx, .poll = poll_connection};
static int ano_slot_init(void)
{
    atomic_store(&connection.handle, 0);
    atomic_store(&connection.refresh, 0);
    memset(connection.clients, 0, sizeof(connection.clients));
    return tcp_slot_init(&connection.rx);
}
/* 先在业务层组 ANO 帧，再只用目标句柄发送；发送失败时恢复未确认的查询状态。 */
static int ano_publish(const unsigned char *data, size_t length)
{
    int result = 0;
    for (int i = 0; i < TCP_CLIENTS_PER_PORT; i++) {
        ano_client_t *client = &connection.clients[i];
        if (!client->handle)
            continue;
        unsigned saved = client->requests;
        unsigned char frame[TCP_REPLY_CAPACITY];
        int n = prepare_sample(client->handle, data, length, frame, sizeof(frame), NULL);
        if (n < 0 || tcp_send(client->handle, frame, (size_t)n)) {
            client->requests = saved;
            connection.refresh = 1;
            result = -1;
        }
    }
    return result;
}

#define ANO_CONFIG "config/ano.conf"

typedef struct {
    int enabled, port, rate_hz, test_mode;
} ano_config_t;

static pthread_mutex_t ano_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t ano_save_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    atomic_int ready;
    app_ctx_t *app;
    ano_config_t config;
    unsigned epoch;
    unsigned long long samples, last_sample_ms, next_sample_ms;
    unsigned long long test_started_ms;
    follow_telemetry_t latest;
    const char *error;
} ano;

static unsigned long long ano_now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (unsigned long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int parse_decimal(const char *text, int low, int high, int *out)
{
    if (!text || !*text)
        return -1;
    unsigned long value = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p < '0' || *p > '9' || value > (unsigned)high / 10)
            return -1;
        value = value * 10 + *p - '0';
        if (value > (unsigned)high)
            return -1;
    }
    if (value < (unsigned)low)
        return -1;
    *out = (int)value;
    return 0;
}

static int apply_field(ano_config_t *config, const char *key, const char *value, unsigned *seen)
{
    unsigned bit;
    int *destination, low, high;
    if (!strcmp(key, "enabled")) {
        bit = 1;
        destination = &config->enabled;
        low = 0;
        high = 1;
    } else if (!strcmp(key, "port")) {
        bit = 2;
        destination = &config->port;
        low = 1024;
        high = 65535;
    } else if (!strcmp(key, "rate_hz")) {
        bit = 4;
        destination = &config->rate_hz;
        low = 1;
        high = 100;
    } else if (!strcmp(key, "test_mode")) {
        bit = 8;
        destination = &config->test_mode;
        low = 0;
        high = 1;
    } else {
        return -1;
    }
    if ((*seen & bit) || parse_decimal(value, low, high, destination))
        return -1;
    *seen |= bit;
    return 0;
}

static int load_config(ano_config_t *config)
{
    int fd = open(ANO_CONFIG, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    struct stat info;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode)) {
        close(fd);
        return -1;
    }
    FILE *file = fdopen(fd, "r");
    if (!file) {
        close(fd);
        return -1;
    }
    char line[128];
    unsigned seen = 0;
    int result = 0;
    while (fgets(line, sizeof(line), file)) {
        if (!strchr(line, '\n') && !feof(file)) {
            result = -1;
            break;
        }
        char *comment = strchr(line, '#');
        if (comment)
            *comment = 0;
        char key[32], value[32], extra[2];
        int fields = sscanf(line, "%31s %31s %1s", key, value, extra);
        if (fields == EOF)
            continue;
        if (fields != 2 || apply_field(config, key, value, &seen)) {
            result = -1;
            break;
        }
    }
    if (ferror(file))
        result = -1;
    fclose(file);
    return result;
}

static int save_config(const ano_config_t *config)
{
    struct stat previous;
    int exists = !lstat(ANO_CONFIG, &previous);
    if ((!exists && errno != ENOENT) || (exists && !S_ISREG(previous.st_mode)))
        return -1;
    char temporary[] = "config/.ano.conf.XXXXXX";
    int fd = mkstemp(temporary);
    if (fd < 0)
        return -1;
    if (fchmod(fd, exists ? previous.st_mode & 0777 : 0600) ||
        (exists && geteuid() == 0 && fchown(fd, previous.st_uid, previous.st_gid))) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    FILE *file = fdopen(fd, "w");
    if (!file) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    int failed = fprintf(file, "enabled %d\nport %d\nrate_hz %d\ntest_mode %d\n", config->enabled,
                         config->port, config->rate_hz, config->test_mode) < 0;
    failed |= fflush(file) != 0;
    failed |= fsync(fd) != 0;
    failed |= fclose(file) != 0;
    if (failed || rename(temporary, ANO_CONFIG)) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static size_t encode_sample(const follow_telemetry_t *sample, unsigned char *frame)
{
    float channels[4] = {(float)sample->speed_target, (float)sample->speed_actual,
                         (float)sample->steering_target, (float)sample->steering_actual};
    return ano_v8_sample(channels, frame, ANO_FRAME_BYTES);
}

static void expire_feedback(follow_telemetry_t *sample, unsigned long long now)
{
    if (!sample->feedback_valid_until_ms || now > sample->feedback_valid_until_ms) {
        sample->feedback_valid = 0;
        sample->speed_actual = sample->steering_actual = NAN;
    }
}

static void simulate_sample(follow_telemetry_t *sample, unsigned long long started,
                            unsigned long long now)
{
    float channels[4];
    ano_simulation_sample(now >= started ? now - started : 0, channels);
    sample->speed_target = channels[0];
    sample->speed_actual = channels[1];
    sample->steering_target = channels[2];
    sample->steering_actual = channels[3];
    /* Synthetic output must never be reported as measured wheel feedback. */
    sample->feedback_valid = 0;
    sample->feedback_ms = sample->feedback_valid_until_ms = 0;
}

/* Invoked by tcp_task only. Controller snapshots are obtained before either
 * ANO configuration or shared TCP transport locks are acquired. */
void ano_tick(app_ctx_t *app)
{
    if (!app || !atomic_load(&ano.ready))
        return;
    unsigned long long now = ano_now_ms();
    int refresh = atomic_exchange(&connection.refresh, 0);
    pthread_mutex_lock(&ano_lock);
    unsigned epoch = ano.epoch;
    int due = ano.app == app && now >= ano.next_sample_ms;
    pthread_mutex_unlock(&ano_lock);
    if (!due && !refresh)
        return;
    follow_telemetry_t sample;
    int sampled = !follow_telemetry_snapshot(&sample);
    pthread_mutex_lock(&ano_lock);
    if (!atomic_load(&ano.ready) || ano.epoch != epoch) {
        pthread_mutex_unlock(&ano_lock);
        return;
    }
    if (!sampled)
        sample = ano.latest;
    expire_feedback(&sample, now);
    if (sampled || sample.feedback_valid != ano.latest.feedback_valid)
        ano.latest = sample;
    /* ANO plots use a fixed sample period for their X axis. Repeat the current
     * state at every configured period, even when inference has not advanced;
     * sampling never executes a PID iteration or invents measured feedback. */
    if (ano.config.enabled) {
        if (ano.config.test_mode)
            simulate_sample(&sample, ano.test_started_ms, now);
        unsigned char frame[ANO_FRAME_BYTES];
        size_t length = encode_sample(&sample, frame);
        if (length && !ano_publish(frame, length)) {
            ano.samples++;
            ano.last_sample_ms = now;
        }
    }
    unsigned interval =
        ano.config.enabled ? (1000 + ano.config.rate_hz - 1) / ano.config.rate_hz : 100;
    /* A new connection receives a synchronization sample immediately without
     * moving the deadline of the regular periodic stream. */
    if (due)
        ano.next_sample_ms = now + interval;
    pthread_mutex_unlock(&ano_lock);
}

int ano_wait_ms(app_ctx_t *app)
{
    if (!app || !atomic_load(&ano.ready))
        return 500;
    unsigned long long now = ano_now_ms();
    pthread_mutex_lock(&ano_lock);
    unsigned long long next = ano.next_sample_ms;
    pthread_mutex_unlock(&ano_lock);
    return next > now ? (int)(next - now) : 0;
}

int ano_init(void *arg)
{
    app_ctx_t *app = arg;
    pthread_mutex_lock(&ano_save_lock);
    if (!app || !app->tcp || !tcp_ready(app->tcp) || atomic_load(&ano.ready)) {
        pthread_mutex_unlock(&ano_save_lock);
        return -1;
    }
    pthread_mutex_lock(&ano_lock);
    unsigned epoch = ano.epoch + 1;
    memset(&ano, 0, sizeof(ano));
    ano.epoch = epoch;
    ano.app = app;
    ano.config = (ano_config_t){.port = 8080, .rate_hz = 30};
    ano.latest = (follow_telemetry_t){
        .speed_target = NAN, .speed_actual = NAN, .steering_target = NAN, .steering_actual = NAN};
    ano.error = "";
    ano_config_t saved = ano.config;
    if (load_config(&saved))
        ano.error = "invalid_saved_settings";
    else
        ano.config = saved;
    ano.test_started_ms = ano_now_ms();
    if (ano_slot_init()) {
        pthread_mutex_unlock(&ano_lock);
        pthread_mutex_unlock(&ano_save_lock);
        return -1;
    }
    tcp_service_stage_t stage;
    if (tcp_prepare_register(app->tcp, atomic_load(&connection.handle), ano.config.enabled,
                             ano.config.port, &protocol, NULL, &stage))
        ano.error = "listen_failed";
    else {
        tcp_commit_register(app->tcp, &stage);
        atomic_store(&connection.handle, stage.handle);
        connection.refresh = 1;
    }
    atomic_store(&ano.ready, 1);
    pthread_mutex_unlock(&ano_lock);
    pthread_mutex_unlock(&ano_save_lock);
    tcp_wake(app->tcp);
    return 0;
}

void ano_destroy(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&ano_save_lock);
    if (!atomic_exchange(&ano.ready, 0)) {
        pthread_mutex_unlock(&ano_save_lock);
        return;
    }
    pthread_mutex_lock(&ano_lock);
    app_ctx_t *app = ano.app;
    ano.app = NULL;
    pthread_mutex_unlock(&ano_lock);
    if (app)
        tcp_component_unregister(atomic_load(&connection.handle));
    data_slot_destroy(&connection.rx.queue);
    pthread_mutex_unlock(&ano_save_lock);
}

static const char *json_channel(double value, char *out, size_t length)
{
    if (isfinite(value))
        snprintf(out, length, "%.6f", value);
    else
        snprintf(out, length, "null");
    return out;
}

void http_ano_get(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req)
{
    (void)method;
    (void)uri;
    (void)req;
    char json[2048], channels[4][48];
    pthread_mutex_lock(&ano_lock);
    ano_config_t config = ano.config;
    follow_telemetry_t latest = ano.latest;
    unsigned long long samples = ano.samples, last_sample = ano.last_sample_ms;
    unsigned long long test_started = ano.test_started_ms;
    const char *module_error = ano.error ? ano.error : "";
    int ready = atomic_load(&ano.ready);
    pthread_mutex_unlock(&ano_lock);
    unsigned long long now = ano_now_ms();
    expire_feedback(&latest, now);
    int test_active = ready && config.enabled && config.test_mode;
    if (test_active)
        simulate_sample(&latest, test_started, now);
    tcp_service_status_t transport;
    tcp_service_get_status(app ? app->tcp : NULL, atomic_load(&connection.handle), &transport);
    int length = snprintf(
        json, sizeof(json),
        "{\"config\":{\"enabled\":%s,\"port\":%d,\"rate_hz\":%d,\"test_mode\":%d},"
        "\"status\":{\"running\":%s,\"listening\":%s,\"bind\":\"0.0.0.0\",\"clients\":%d,"
        "\"samples\":%llu,\"sent_frames\":%llu,\"dropped_frames\":%llu,\"errors\":%llu,"
        "\"feedback_valid\":%s,\"test_active\":%s,\"data_source\":\"%s\","
        "\"last_sample_ms\":%llu,\"error\":\"%s\"},"
        "\"channels\":{\"speed_target\":%s,\"speed_actual\":%s,\"steering_target\":%s,"
        "\"steering_actual\":%s},\"protocol\":\"ANOv8\",\"frame_bytes\":24,\"max_clients\":%d}",
        config.enabled ? "true" : "false", config.port, config.rate_hz, config.test_mode,
        ready && transport.running ? "true" : "false", transport.listening ? "true" : "false",
        transport.clients, samples, transport.sent_frames, transport.dropped_frames,
        transport.errors, latest.feedback_valid ? "true" : "false", test_active ? "true" : "false",
        test_active ? "simulation" : "controller", last_sample,
        *module_error ? module_error : transport.error,
        json_channel(latest.speed_target, channels[0], 48),
        json_channel(latest.speed_actual, channels[1], 48),
        json_channel(latest.steering_target, channels[2], 48),
        json_channel(latest.steering_actual, channels[3], 48), TCP_CLIENTS_PER_PORT);
    http_ok_json(fd, json, (size_t)length);
}

static int parse_form(const char *request, ano_config_t *config)
{
    const char *body = http_body_start(request);
    if (!body || !*body || strlen(body) > 128)
        return -1;
    unsigned seen = 0;
    for (const char *start = body; *start;) {
        const char *end = strchr(start, '&');
        size_t length = end ? (size_t)(end - start) : strlen(start);
        const char *equals = memchr(start, '=', length);
        if (!equals || equals == start || equals == start + length - 1)
            return -1;
        size_t key_length = (size_t)(equals - start);
        size_t value_length = length - key_length - 1;
        if (key_length >= 32 || value_length >= 12)
            return -1;
        char key[32], value[12];
        memcpy(key, start, key_length);
        key[key_length] = 0;
        memcpy(value, equals + 1, value_length);
        value[value_length] = 0;
        if (apply_field(config, key, value, &seen))
            return -1;
        if (!end)
            return 0;
        start = end + 1;
        if (!*start)
            return -1;
    }
    return -1;
}

void http_ano_post(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req)
{
    pthread_mutex_lock(&ano_save_lock);
    if (!app || !app->tcp || !atomic_load(&ano.ready)) {
        pthread_mutex_unlock(&ano_save_lock);
        http_err(fd, 503, "Service Unavailable", "ANO module is not running");
        return;
    }
    pthread_mutex_lock(&ano_lock);
    ano_config_t next = ano.config;
    pthread_mutex_unlock(&ano_lock);
    if (parse_form(req, &next)) {
        pthread_mutex_unlock(&ano_save_lock);
        http_err(fd, 400, "Bad Request", "Invalid or repeated ANO field");
        return;
    }
    tcp_service_stage_t stage;
    if (tcp_prepare_register(app->tcp, atomic_load(&connection.handle), next.enabled, next.port,
                             &protocol, NULL, &stage)) {
        pthread_mutex_unlock(&ano_save_lock);
        http_err(fd, 409, "Conflict", "ANO TCP port is unavailable; settings unchanged");
        return;
    }
    if (save_config(&next)) {
        tcp_cancel_register(app->tcp, &stage);
        pthread_mutex_unlock(&ano_save_lock);
        http_err(fd, 500, "Error", "Cannot save ANO settings; settings unchanged");
        return;
    }
    pthread_mutex_lock(&ano_lock);
    tcp_commit_register(app->tcp, &stage);
    atomic_store(&connection.handle, stage.handle);
    connection.refresh = 1;
    if (next.enabled && next.test_mode && (!ano.config.enabled || !ano.config.test_mode))
        ano.test_started_ms = ano_now_ms();
    ano.config = next;
    ano.epoch++;
    ano.next_sample_ms = 0;
    ano.error = "";
    pthread_mutex_unlock(&ano_lock);
    pthread_mutex_unlock(&ano_save_lock);
    tcp_wake(app->tcp);
    http_ano_get(app, fd, method, uri, req);
}
