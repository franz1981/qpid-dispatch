/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <qpid/dispatch/buffer.h>
#include <qpid/dispatch/alloc.h>

#include <stdint.h>
#include <string.h>


size_t BUFFER_SIZE     = 512;
static int size_locked = 0;

ALLOC_DECLARE(qd_buffer_t);
ALLOC_DEFINE_CONFIG(qd_buffer_t, sizeof(qd_buffer_t), &BUFFER_SIZE, 0);


void qd_buffer_set_size(size_t size)
{
    assert(!size_locked);
    BUFFER_SIZE = size;
}


qd_buffer_t *qd_buffer(void)
{
    size_locked = 1;
    qd_buffer_t *buf = new_qd_buffer_t();

    DEQ_ITEM_INIT(buf);
    buf->size   = 0;
    sys_atomic_init(&buf->bfanout, 0);
    return buf;
}


void qd_buffer_free(qd_buffer_t *buf)
{
    if (!buf) return;
    sys_atomic_destroy(&buf->bfanout);
    free_qd_buffer_t(buf);
}


unsigned char *qd_buffer_base(qd_buffer_t *buf)
{
    return (unsigned char*) &buf[1];
}


unsigned char *qd_buffer_cursor(qd_buffer_t *buf)
{
    return ((unsigned char*) &buf[1]) + buf->size;
}


size_t qd_buffer_capacity(qd_buffer_t *buf)
{
    return BUFFER_SIZE - buf->size;
}


size_t qd_buffer_size(qd_buffer_t *buf)
{
    return buf->size;
}


void qd_buffer_insert(qd_buffer_t *buf, size_t len)
{
    buf->size += len;
    assert(buf->size <= BUFFER_SIZE);
}


uint32_t qd_buffer_set_fanout(qd_buffer_t *buf, uint32_t value)
{
    return sys_atomic_set(&buf->bfanout, value);
}


uint32_t qd_buffer_inc_fanout(qd_buffer_t *buf)
{
    return sys_atomic_inc(&buf->bfanout);
}


uint32_t qd_buffer_dec_fanout(qd_buffer_t *buf)
{
    return sys_atomic_dec(&buf->bfanout);
}


unsigned char *qd_buffer_at(qd_buffer_t *buf, size_t len)
{
    // If the len is greater than the buffer size, we might point to some garbage.
    // We dont want that to happen, so do the assert.
    assert(len <= BUFFER_SIZE);
    return ((unsigned char*) &buf[1]) + len;
}

void qd_buffer_lists_clone(qd_buffer_list_t *restrict *restrict dst, qd_buffer_list_t *restrict *const restrict src,
                           int count)
{
    qd_buffer_t *restrict buf[count];
    for (int i = 0; i < count; i++) {
        DEQ_INIT(*dst[i]);
        buf[i] = DEQ_HEAD(*src[i]);
    }
    while (1) {
        size_t to_copy[count];
        qd_buffer_t *restrict next[count];
        unsigned char *restrict src_buf[count];
        int to_process = 0;
        for (int i = 0; i < count; i++) {
            if (buf[i] != NULL) {
                to_copy[i] = qd_buffer_size(buf[i]);
                src_buf[i] = qd_buffer_base(buf[i]);
                next[i] = DEQ_NEXT(buf[i]);
                to_process++;
            } else {
                to_copy[i] = 0;
                src_buf[i] = NULL;
                next[i] = NULL;
            }
        }
        if (to_process == 0) {
            return;
        }
        while (1) {
            qd_buffer_t *restrict new_buf[count];
            size_t capacity[count];
            for (int i = 0; i < count; i++) {
                if (to_copy[i] > 0) {
                    new_buf[i] = qd_buffer();
                } else {
                    new_buf[i] = NULL;
                }
            }
            long total_to_copy = 0;
            for (int i = 0; i < count; i++) {
                if (new_buf[i] != NULL) {
                    size_t cap = qd_buffer_capacity(new_buf[i]);
                    if (cap > to_copy[i]) {
                        cap = to_copy[i];
                    }
                    capacity[i] = cap;
                    memcpy(qd_buffer_cursor(new_buf[i]), src_buf[i], capacity[i]);
                }
            }
            for (int i = 0; i < count; i++) {
                if (new_buf[i] != NULL) {
                    qd_buffer_insert(new_buf[i], capacity[i]);
                    DEQ_INSERT_TAIL(*dst[i], new_buf[i]);
                    src_buf[i] += capacity[i];
                    to_copy[i] -= capacity[i];
                    total_to_copy += to_copy[i];
                }
            }
            if (total_to_copy == 0) {
                break;
            }
        }
        to_process = 0;
        for (int i = 0; i < count; i++) {
            buf[i] = next[i];
            if (buf[i] != NULL) {
                to_process++;
            }
        }
        if (to_process == 0) {
            return;
        }
    }
}

unsigned int qd_buffer_list_clone(qd_buffer_list_t *dst, const qd_buffer_list_t *src)
{
    uint32_t len = 0;
    DEQ_INIT(*dst);
    qd_buffer_t *buf = DEQ_HEAD(*src);
    while (buf) {
        size_t to_copy = qd_buffer_size(buf);
        unsigned char *src = qd_buffer_base(buf);
        len += to_copy;
        while (to_copy) {
            qd_buffer_t *newbuf = qd_buffer();
            size_t count = qd_buffer_capacity(newbuf);
            // default buffer capacity may have changed,
            // so don't assume it will fit:
            if (count > to_copy) count = to_copy;
            memcpy(qd_buffer_cursor(newbuf), src, count);
            qd_buffer_insert(newbuf, count);
            DEQ_INSERT_TAIL(*dst, newbuf);
            src += count;
            to_copy -= count;
        }
        buf = DEQ_NEXT(buf);
    }
    return len;
}


void qd_buffer_list_free_buffers(qd_buffer_list_t *list)
{
    qd_buffer_t *buf = DEQ_HEAD(*list);
    while (buf) {
        DEQ_REMOVE_HEAD(*list);
        qd_buffer_free(buf);
        buf = DEQ_HEAD(*list);
    }
}


unsigned int qd_buffer_list_length(const qd_buffer_list_t *list)
{
    unsigned int len = 0;
    qd_buffer_t *buf = DEQ_HEAD(*list);
    while (buf) {
        len += qd_buffer_size(buf);
        buf = DEQ_NEXT(buf);
    }
    return len;
}
