//
// Created by forked_franz on 10/02/17.
//

#ifndef FRANZ_FLOW_MPMC_FIXED_SIZE_RING_BUFFER_H
#define FRANZ_FLOW_MPMC_FIXED_SIZE_RING_BUFFER_H

#include <stdbool.h>
#include "index.h"
#include "bytes_utils.h"

struct mpmc_fs_rb_t {
    int8_t padding[(2 * CACHE_LINE_LENGTH)];
    struct {
        _Atomic uint64_t producer_position;
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

index_t mpmc_fs_rb_capacity(const index_t requested_capacity, const uint32_t message_size);

uint64_t mpmc_fs_rb_load_consumer_position(const struct mpmc_fs_rb_t *const header);

uint64_t mpmc_fs_rb_load_producer_position(const struct mpmc_fs_rb_t *const header);

bool new_mpmc_fs_rb(
        struct mpmc_fs_rb_t *const header,
        const index_t requested_capacity,
        const uint32_t message_size);

bool try_mpmc_fs_rb_claim(
        const struct mpmc_fs_rb_t *const header,
        uint64_t *const claimed_position,
        uint8_t **const claimed_message);

void mpmc_fs_rb_commit_claim(const uint64_t claimed_producer_position,
                             uint8_t *const claimed_message);

bool try_mpmc_fs_rb_claim_read(const struct mpmc_fs_rb_t *const header,
                               uint64_t *const read_position,
                               uint8_t **const read_message_address);


void mpmc_fs_rb_commit_read(const uint64_t read_position, uint8_t *const read_message_address);

bool mpmc_fs_rb_is_empty(const struct mpmc_fs_rb_t *const header);


index_t mpmc_fs_rb_size(const struct mpmc_fs_rb_t *const header);

#endif //FRANZ_FLOW_MPMC_FIXED_SIZE_RING_BUFFER_H
