//
// Created by forked_franz on 10/02/17.
//

#ifndef FRANZ_FLOW_FIXED_SIZE_RING_BUFFER_H
#define FRANZ_FLOW_FIXED_SIZE_RING_BUFFER_H

#include <stdbool.h>
#include "index.h"
#include "bytes_utils.h"

struct fs_rb_t {
    int8_t padding[(2 * CACHE_LINE_LENGTH)];
    struct {
        _Atomic uint64_t producer_position;
        _Atomic uint64_t consumer_cache_position;
        int8_t padding[(2 * CACHE_LINE_LENGTH)];
    } producer;
    struct {
        _Atomic uint64_t consumer_position;
        int8_t padding[(2 * CACHE_LINE_LENGTH)];
    } consumer;
    index_t mask;
    index_t capacity;
    uint32_t aligned_message_size;
    uint8_t *buffer;
};

index_t fs_rb_capacity(const index_t requested_capacity, const uint32_t message_size);

uint64_t fs_rb_load_consumer_position(const struct fs_rb_t *const header);

uint64_t fs_rb_load_producer_position(const struct fs_rb_t *const header);

bool new_fs_rb(
        struct fs_rb_t *const header,
        const index_t requested_capacity,
        const uint32_t message_size);

bool try_fs_rb_sp_claim(
        const struct fs_rb_t *const header,
        const uint32_t max_look_ahead_step,
        uint8_t **const claimed_message);

bool try_fs_rb_mp_claim(
        const struct fs_rb_t *const header,
        uint8_t **const claimed_message);

void fs_rb_commit_claim(const uint8_t *const claimed_message_address);

typedef bool(*const fs_rb_message_consumer)(uint8_t *const, void *const);

uint32_t fs_rb_read(
        const struct fs_rb_t *const header,
        const fs_rb_message_consumer consumer,
        const uint32_t count, void *const context);

bool try_fs_rb_claim_read(const struct fs_rb_t *const header,
                                        uint8_t **const read_message_address);

void fs_rb_commit_read(const struct fs_rb_t *const header);

bool fs_rb_is_empty(const struct fs_rb_t *const header);


index_t fs_rb_size(const struct fs_rb_t *const header);

#endif //FRANZ_FLOW_FIXED_SIZE_RING_BUFFER_H
