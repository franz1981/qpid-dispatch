//
// Created by forked_franz on 10/02/17.
//

#include <stdatomic.h>
#include "mpmc_fs_rb.h"
#include <limits.h>

#define MESSAGE_STATE_SIZE 8

inline index_t mpmc_fs_rb_capacity(const index_t requested_capacity, const uint32_t message_size) {
    const index_t next_pow_2_requested_capacity = next_pow_2(requested_capacity);
    const index_t aligned_message_size = align(message_size + MESSAGE_STATE_SIZE, MESSAGE_STATE_SIZE);
    return (next_pow_2_requested_capacity * aligned_message_size);
}

inline uint64_t mpmc_fs_rb_load_consumer_position(const struct mpmc_fs_rb_t *const header) {
    const uint64_t position = atomic_load_explicit(&header->consumer.consumer_position, memory_order_relaxed);
    return position;
}

inline uint64_t mpmc_fs_rb_load_producer_position(const struct mpmc_fs_rb_t *const header) {
    const uint64_t position = atomic_load_explicit(&header->producer.producer_position, memory_order_relaxed);
    return position;
}

bool new_mpmc_fs_rb(
        struct mpmc_fs_rb_t *const header,
        const index_t requested_capacity,
        const uint32_t message_size) {
    const index_t next_pow_2_requested_capacity = next_pow_2(requested_capacity);
    const index_t aligned_message_size = align(message_size + MESSAGE_STATE_SIZE, MESSAGE_STATE_SIZE);
    header->capacity = next_pow_2_requested_capacity;
    header->mask = next_pow_2_requested_capacity - 1;
    header->aligned_message_size = aligned_message_size;
    header->producer.producer_position = 0;
    header->consumer.consumer_position = 0;
    for (int i = 0; i < next_pow_2_requested_capacity; i++) {
        _Atomic uint64_t *message_offset = (_Atomic uint64_t *) (header->buffer + (i * aligned_message_size));
        atomic_store_explicit(message_offset, i, memory_order_relaxed);
    }
    atomic_thread_fence(memory_order_release);
    return true;
}

inline bool try_mpmc_fs_rb_claim(
        const struct mpmc_fs_rb_t *const header,
        uint64_t *const claimed_position,
        uint8_t **const claimed_message) {
    uint8_t *const buffer = header->buffer;
    const index_t mask = header->mask;
    const int64_t capacity = header->capacity;
    const index_t aligned_message_size = header->aligned_message_size;

    int64_t producer_position = atomic_load_explicit(&header->producer.producer_position, memory_order_acquire);
    uint64_t message_state_offset;
    uint64_t message_state;
    int64_t consumer_position = LONG_MIN;
    bool retry;
    do {
        message_state_offset = (producer_position & mask) * aligned_message_size;
        const _Atomic uint64_t *const message_state_address = (_Atomic uint64_t *) (buffer + message_state_offset);
        message_state = atomic_load_explicit(message_state_address, memory_order_acquire);
        if (message_state < producer_position) {
            if (producer_position - capacity >= consumer_position &&
                producer_position - capacity >=
                (consumer_position = atomic_load_explicit(&header->consumer.consumer_position, memory_order_acquire))) {
                return false;
            } else {
                message_state = producer_position + 1;
            }
        }
        retry = (message_state > producer_position);
        if (retry) {
            producer_position = atomic_load_explicit(&header->producer.producer_position, memory_order_acquire);
        } else {
            retry = !atomic_compare_exchange_strong_explicit(&header->producer.producer_position, &producer_position,
                                                             producer_position + 1, memory_order_seq_cst,
                                                             memory_order_acquire);
        }
    } while (retry);
    *claimed_position = producer_position;
    //can perform a relaxed write of the messatge state:
    //only changing the producer sequence will make the consumers to proceed
    *claimed_message = buffer + message_state_offset + MESSAGE_STATE_SIZE;
    return true;
}

inline void mpmc_fs_rb_commit_claim(const uint64_t claimed_producer_position, uint8_t *const claimed_message) {
    const _Atomic uint64_t *const message_state_address = (_Atomic uint64_t *) (claimed_message -
                                                                                MESSAGE_STATE_SIZE);
    atomic_store_explicit(message_state_address, claimed_producer_position + 1, memory_order_release);
}

inline bool try_mpmc_fs_rb_claim_read(const struct mpmc_fs_rb_t *const header,
                                      uint64_t *const read_position,
                                      uint8_t **const read_message_address) {
    uint8_t *const buffer = header->buffer;
    const index_t mask = header->mask;
    const index_t aligned_message_size = header->aligned_message_size;

    int64_t consumer_position = atomic_load_explicit(&header->consumer.consumer_position, memory_order_acquire);
    uint64_t message_state_offset;
    uint64_t message_state;
    uint64_t expected_message_state;
    int64_t producer_position = -1;
    bool retry;
    do {
        message_state_offset = (consumer_position & mask) * aligned_message_size;
        const _Atomic uint64_t *const message_state_address = (_Atomic uint64_t *) (buffer + message_state_offset);
        message_state = atomic_load_explicit(message_state_address, memory_order_acquire);
        expected_message_state = consumer_position + 1;
        if (message_state < expected_message_state) {
            if (consumer_position >= producer_position &&
                consumer_position ==
                (producer_position = atomic_load_explicit(&header->producer.producer_position, memory_order_acquire))) {
                return false;
            } else {
                message_state = expected_message_state + 1;
            }
        }
        retry = (message_state > expected_message_state);
        if (retry) {
            consumer_position = atomic_load_explicit(&header->consumer.consumer_position, memory_order_acquire);
        } else {
            retry = !atomic_compare_exchange_strong_explicit(&header->consumer.consumer_position, &consumer_position,
                                                             consumer_position + 1, memory_order_seq_cst,
                                                             memory_order_acquire);
        }
    } while (retry);
    *read_position = consumer_position + mask + 1;
    //can perform a relaxed write of the messatge state:
    //only changing the producer sequence will make the consumers to proceed
    *read_message_address = buffer + message_state_offset + MESSAGE_STATE_SIZE;
    return true;
}

inline void mpmc_fs_rb_commit_read(const uint64_t position, uint8_t *const read_message_address) {
    const _Atomic uint64_t *const message_state_address = (_Atomic uint64_t *) (read_message_address -
                                                                                MESSAGE_STATE_SIZE);
    atomic_store_explicit(message_state_address, position, memory_order_release);
}

inline bool mpmc_fs_rb_is_empty(const struct mpmc_fs_rb_t *const header) {
    const uint64_t consumer_position = atomic_load_explicit(&header->consumer.consumer_position, memory_order_relaxed);
    const uint64_t producer_position = atomic_load_explicit(&header->producer.producer_position, memory_order_relaxed);
    return producer_position == consumer_position;
}


inline index_t mpmc_fs_rb_size(const struct mpmc_fs_rb_t *const header) {
    uint64_t consumer_position = atomic_load_explicit(&header->consumer.consumer_position, memory_order_relaxed);
    while (true) {
        const uint64_t before = consumer_position;
        const uint64_t producer_position = atomic_load_explicit(&header->producer.producer_position,
                                                                memory_order_relaxed);
        consumer_position = atomic_load_explicit(&header->consumer.consumer_position, memory_order_relaxed);
        if (before == consumer_position) {
            return (producer_position - consumer_position);
        }
    }
}
