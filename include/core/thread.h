/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CORE_THREAD_H
#define CORE_THREAD_H
#include <pthread.h>
/* Main owns thread admission. Callers retain their existing join/detach policy.
 * Returns a pthread error number; refuses new work during application shutdown. */
int app_thread_create(pthread_t *thread, const pthread_attr_t *attributes,
                      void *(*task)(void *), void *argument);
int app_thread_start_detached(void *(*task)(void *), void *argument);
#endif
