/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "http/http_client_limit.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>

static void *release_connection(void *arg)
{
    http_client_release(*(int *)arg, 300000);
    return NULL;
}

int main(void)
{
    const uint32_t first = 0x0100007f, second = 0x0200007f, third = 0x0300007f;
    assert(HTTP_MAX_CLIENTS == 2);
    /* Browser asset/API/video connections from one source share a slot. */
    assert(!http_client_acquire(10, first, 1000));
    assert(!http_client_acquire(11, first, 1000));
    assert(!http_client_acquire(12, second, 1000));
    assert(http_client_acquire(13, third, 1000) < 0);
    assert(http_client_acquire(10, first, 1000) < 0);
    http_client_release(10, 2000);
    /* A detached stream still keeps its client admitted indefinitely. */
    assert(http_client_acquire(13, third, 90000) < 0);
    http_client_release(11, 90000);
    assert(http_client_acquire(13, third, 90000 + HTTP_CLIENT_GRACE_MS - 1) < 0);
    /* A polling page reconnects during the grace period. */
    assert(!http_client_acquire(10, first, 90000 + HTTP_CLIENT_GRACE_MS - 1));
    assert(http_client_acquire(13, third, 200000) < 0);
    http_client_release(10, 200000);
    assert(!http_client_acquire(13, third, 200000 + HTTP_CLIENT_GRACE_MS));
    http_client_release(13, 240000);
    http_client_release(12, 240000);
    /* Released fd values are safe to reuse for another address. */
    assert(!http_client_acquire(10, first, 280000));
    assert(!http_client_acquire(11, second, 280000));
    int descriptors[2] = {10, 11};
    pthread_t workers[2];
    assert(!pthread_create(&workers[0], NULL, release_connection, &descriptors[0]));
    assert(!pthread_create(&workers[1], NULL, release_connection, &descriptors[1]));
    assert(!pthread_join(workers[0], NULL));
    assert(!pthread_join(workers[1], NULL));
    assert(!http_client_acquire(10, third, 340000));
    /* Missing/double release is harmless for unregistered fds. */
    http_client_release(999, 340000);
    http_client_release(11, 340000);
    http_client_release(11, 340000);
    assert(!http_client_acquire(11, first, 340000));
    assert(http_client_acquire(12, second, 340000) < 0);
    http_client_release(10, 350000);
    http_client_release(11, 350000);
    /* Production clock is monotonic and remains in millisecond units. */
    unsigned long long start = http_client_now_ms();
    assert(http_client_now_ms() >= start);
    puts("http_client_limit_test: OK");
    return 0;
}
