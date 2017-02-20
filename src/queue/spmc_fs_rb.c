//
// Created by forked_franz on 10/02/17.
//

#include <stdatomic.h>
#include "spmc_fs_rb.h"
#include "bytes_utils.h"

#define MESSAGE_STATE_SIZE 4

static const index_t MESSAGE_STATE_FREE = 0;
static const index_t MESSAGE_STATE_BUSY = 1;

index_t spmc_fs_rb_capacity(const index_t requested_capacity, const uint32_t message_size) {
    const index_t next_pow_2_requested_capacity = next_pow_2(requested_capacity);
    const index_t aligned_message_size = align(message_size + MESSAGE_STATE_SIZE, MESSAGE_STATE_SIZE);
    return (next_pow_2_requested_capacity * aligned_message_size);
}

inline uint64_t spmc_fs_rb_load_consumer_position(const struct spmc_fs_rb_t *const header) {
    const uint64_t position = atomic_load_explicit(&header->consumer.consumer_position, memory_order_relaxed);
    return position;
}

inline uint64_t spmc_fs_rb_load_producer_position(const struct spmc_fs_rb_t *const header) {
    const uint64_t position = atomic_load_explicit(&header->producer.producer_position, memory_order_relaxed);
    return position;
}

bool new_spmc_fs_rb(
        struct spmc_fs_rb_t *const header,
        const index_t requested_capacity,
        const uint32_t message_size) {
    const index_t next_pow_2_requested_capacity = next_pow_2(requested_capacity);
    const index_t aligned_message_size = align(message_size + MESSAGE_STATE_SIZE, MESSAGE_STATE_SIZE);
    header->capacity = next_pow_2_requested_capacity;
    header->mask = next_pow_2_requested_capacity - 1;
    header->aligned_message_size = aligned_message_size;
    header->producer.producer_position = 0;
    header->producer_cache.producer_cache_position = 0;
    header->consumer.consumer_position = 0;
    return true;
}

inline bool try_spmc_fs_rb_claim(
        const struct spmc_fs_rb_t *const header,
        uint64_t *const claimed_position,
        uint8_t **const claimed_message) {
    uint8_t *const buffer = header->buffer;
    const index_t mask = header->mask;
    const index_t aligned_message_size = header->aligned_message_size;
    const uint64_t producer_position = atomic_load_explicit(&header->producer.producer_position, memory_order_relaxed);
    const index_t message_state_offset = (producer_position & mask) * aligned_message_size;
    const _Atomic uint32_t *const message_state = (_Atomic uint32_t *) (buffer + message_state_offset);
    if (atomic_load_explicit(message_state, memory_order_acquire) == MESSAGE_STATE_BUSY) {
        const uint64_t consumer_position = atomic_load_explicit(&header->consumer.consumer_position,
                                                                memory_order_acquire);
        const uint64_t size = producer_position - consumer_position;
        if (size > mask) {
            return false;
        } else {
            //spin wait the slot to be cleared
            while (atomic_load_explicit(message_state, memory_order_acquire) == MESSAGE_STATE_BUSY) {

            }
        }
    }
    atomic_store_explicit(message_state, MESSAGE_STATE_BUSY, memory_order_relaxed);
    //can perform a relaxed write of the messatge state:
    //only changing the producer sequence will make the consumers to proceed
    *claimed_message = buffer + message_state_offset + MESSAGE_STATE_SIZE;
    *claimed_position = producer_position;
    return true;
}

inline void spmc_fs_rb_commit_claim(const struct spmc_fs_rb_t *const header, const uint64_t claimed_producer_position) {
    atomic_store_explicit(&header->producer.producer_position, claimed_producer_position + 1, memory_order_release);
}

inline bool try_spmc_fs_rb_mc_claim_read(const struct spmc_fs_rb_t *const header,
                                         uint8_t **const read_message_address) {

    uint64_t producer_position_cache = atomic_load_explicit(&header->producer_cache.producer_cache_position,
                                                            memory_order_relaxed);
    uint64_t consumer_position = atomic_load_explicit(&header->consumer.consumer_position, memory_order_acquire);
    do {
        if (consumer_position >= producer_position_cache) {
            //it is really empty?
            const uint64_t producer_position = atomic_load_explicit(&header->producer.producer_position,
                                                                    memory_order_acquire);
            if (consumer_position >= producer_position) {
                return false;
            } else {
                producer_position_cache = producer_position;
                atomic_store_explicit(&header->producer_cache.producer_cache_position, producer_position,
                                      memory_order_relaxed);
            }
        }
    } while (!atomic_compare_exchange_strong_explicit(&header->consumer.consumer_position, &consumer_position,
                                                      consumer_position + 1, memory_order_seq_cst,
                                                      memory_order_acquire));
    const index_t mask = header->mask;
    const index_t aligned_message_size = header->aligned_message_size;
    const index_t message_state_offset = (consumer_position & mask) * aligned_message_size;
    uint8_t *const message_state_address = header->buffer + message_state_offset;
    *read_message_address = message_state_address + MESSAGE_STATE_SIZE;
    //from now on a producer will wait until the message state will change
    return true;
}

inline bool try_spmc_fs_rb_sc_claim_read(const struct spmc_fs_rb_t *const header,
                                         uint8_t **const read_message_address) {

    uint64_t producer_position_cache = atomic_load_explicit(&header->producer_cache.producer_cache_position,
                                                            memory_order_relaxed);
    uint64_t consumer_position = atomic_load_explicit(&header->consumer.consumer_position, memory_order_relaxed);
    if (consumer_position >= producer_position_cache) {
        //it is really empty?
        const uint64_t producer_position = atomic_load_explicit(&header->producer.producer_position,
                                                                memory_order_acquire);
        if (consumer_position >= producer_position) {
            return false;
        } else {
            producer_position_cache = producer_position;
            atomic_store_explicit(&header->producer_cache.producer_cache_position, producer_position,
                                  memory_order_relaxed);
        }
    }
    atomic_store_explicit(&header->consumer.consumer_position, consumer_position + 1, memory_order_release);
    const index_t mask = header->mask;
    const index_t aligned_message_size = header->aligned_message_size;
    const index_t message_state_offset = (consumer_position & mask) * aligned_message_size;
    uint8_t *const message_state_address = header->buffer + message_state_offset;
    *read_message_address = message_state_address + MESSAGE_STATE_SIZE;
    //from now on a producer will wait until the message state will change
    return true;
}

inline void spmc_fs_rb_commit_read(const struct spmc_fs_rb_t *const header, uint8_t *const read_message_address) {
    const _Atomic uint32_t *const message_state_address = (_Atomic uint32_t *) (read_message_address -
                                                                                MESSAGE_STATE_SIZE);
    atomic_store_explicit(message_state_address, MESSAGE_STATE_FREE, memory_order_release);
}

inline bool spmc_fs_rb_is_empty(const struct spmc_fs_rb_t *const header) {
    const uint64_t consumer_position = atomic_load_explicit(&header->consumer.consumer_position, memory_order_relaxed);
    const uint64_t producer_position = atomic_load_explicit(&header->producer.producer_position, memory_order_relaxed);
    return producer_position == consumer_position;
}


inline index_t spmc_fs_rb_size(const struct spmc_fs_rb_t *const header) {
    uint64_t consumer_position = atomic_load_explicit(&header->consumer.consumer_position, memory_order_relaxed);
    while (true) {
        const uint64_t before = consumer_position;
        const uint64_t producer_position = atomic_load_explicit(&header->producer.producer_position,
                                                                memory_order_relaxed);
        consumer_position = atomic_load_explicit(&header->consumer.consumer_position, memory_order_relaxed);
        if (before == consumer_position) {
            return (index_t) (producer_position - consumer_position);
        }
    }
}
