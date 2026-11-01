/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <string.h>
/* 共用算法，各自实例：调用者提供自己的缓冲区，槽不持有全局存储。
 * 每个实例有独立的锁、游标和丢弃计数；push/pop 复制完整数据项。
 * 生命周期：init → 收发 → 停止生产/消费并注销绑定 → destroy。
 * pop 返回 1/0（有数据/空）；push 返回 0/-1（成功/满）。 */
typedef struct {
    pthread_mutex_t lock;
    void *items;
    size_t item_size, capacity, head, count;
    unsigned long long dropped;
} data_slot_t;
static inline int data_slot_init(data_slot_t *s, void *items, size_t size, size_t capacity)
{
    if (!s || !items || !size || !capacity)
        return EINVAL;
    memset(s, 0, sizeof(*s));
    s->items = items;
    s->item_size = size;
    s->capacity = capacity;
    int rc = pthread_mutex_init(&s->lock, NULL);
    if (rc) {
        s->items = NULL;
        s->capacity = 0;
    }
    return rc;
}
static inline void data_slot_clear(data_slot_t *s)
{
    pthread_mutex_lock(&s->lock);
    s->head = s->count = 0;
    pthread_mutex_unlock(&s->lock);
}
/* overwrite=0：满则拒绝，适用于 TCP（不能丢掉流中的任意字节）。
 * overwrite=1：淘汰最旧项，适用于需要新鲜测量值的 CAN 反馈。 */
static inline int data_slot_push(data_slot_t *s, const void *item, int overwrite)
{
    pthread_mutex_lock(&s->lock);
    if (s->count == s->capacity) {
        s->dropped++;
        if (!overwrite) {
            pthread_mutex_unlock(&s->lock);
            return -1;
        }
        s->head = (s->head + 1) % s->capacity;
        s->count--;
    }
    size_t index = (s->head + s->count) % s->capacity;
    memcpy((char *)s->items + index * s->item_size, item, s->item_size);
    s->count++;
    pthread_mutex_unlock(&s->lock);
    return 0;
}
static inline int data_slot_pop(data_slot_t *s, void *out)
{
    pthread_mutex_lock(&s->lock);
    if (!s->count) {
        pthread_mutex_unlock(&s->lock);
        return 0;
    }
    memcpy(out, (char *)s->items + s->head * s->item_size, s->item_size);
    s->head = (s->head + 1) % s->capacity;
    s->count--;
    pthread_mutex_unlock(&s->lock);
    return 1;
}
static inline size_t data_slot_count(data_slot_t *s)
{
    pthread_mutex_lock(&s->lock);
    size_t count = s->count;
    pthread_mutex_unlock(&s->lock);
    return count;
}
static inline void data_slot_destroy(data_slot_t *s) { pthread_mutex_destroy(&s->lock); }
