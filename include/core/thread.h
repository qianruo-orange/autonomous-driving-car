/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef APP_THREAD_H
#define APP_THREAD_H

#include <pthread.h>

/* main's module table owns policy, priority and CPU affinity. Modules retain
 * task arguments, lifetime and joins; detached must be 0 (joinable) or 1.
 * Returns a pthread error code. */
int app_thread_create(pthread_t *thread, const char *name,
                      void *(*task)(void *), void *argument, int detached);

#endif
