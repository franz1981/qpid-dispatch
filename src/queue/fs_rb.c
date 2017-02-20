//
// Created by forked_franz on 10/02/17.
//

#include <stdatomic.h>
#include "fs_rb.h"
#include "bytes_utils.h"

#define MESSAGE_STATE_SIZE 4

static const index_t MESSAGE_STATE_FREE = 0;
static const index_t MESSAGE_STATE_BUSY = 1;

index_t fs_rb_capacity(const index_t requested_capacity, const uint32_t message_size) {
    const index_t next_pow_2_requested_capacity = next_pow_2(requested_capacity);
    const index_t aligned_message_size = align(message_size + MESSAGE_STATE_SIZE, MESSAGE_STATE_SIZE);
    return (next_pow_2_requested_capacity * aligned_message_size);
}

uint64_t fs_rb_load_consumer_position(const struct fs_rb_t *const header) {
    const uint64_t position = atomic_load_explicit(&header->consumer.consumer_position, memory_order_relaxed);
    return position;
}

uint64_t fs_rb_load_producer_position(const struct fs_rb_t *const header) {
    const uint64_t position = atomic_load_explicit(&header->producer.producer_position, memory_order_relaxed);
    return position;
}

bool new_fs_rb(struct fs_rb_t *const header,
               const index_t requested_capacity,
               const uint32_t message_size) {
    const index_t next_pow_2_requested_capacity = next_pow_2(requested_capacity);
    const index_t aligned_message_size = align(message_size + MESSAGE_STATE_SIZE, MESSAGE_STATE_SIZE);
    header->capacity = next_pow_2_requested_capacity;
    header->mask = next_pow_2_requested_capacity - 1;
    header->aligned_message_size = aligned_message_size;
    header->producer.producer_position = 0;
    header->producer.consumer_cache_position = 0;
    header->consumer.consumer_position = 0;
    return true;
}

static bool sp_claim_slow_path(uint8_t *const buffer, const index_t message_state_offset,
                               const _Atomic uint64_t *consumer_cache_position_address,
                               const uint64_t consumer_cache_position, const uint32_t max_look_ahead_step,
                               const index_t mask, const index_t aligned_message_size) {
    //try to look ahead if the consumer has freed MAX_LOOK_AHEAD_STEP messages
    const uint64_t next_consumer_cache_position = consumer_cache_position + max_look_ahead_step;
    //check the state of the message
    const index_t look_ahead_message_offset = (next_consumer_cache_position & mask) * aligned_message_size;
    const _Atomic uint32_t *const look_ahead_message_state_atomic_address = (_Atomic uint32_t *) (buffer +
                                                                                                  look_ahead_message_offset);
    const uint32_t message_state_value = atomic_load_explicit(look_ahead_message_state_atomic_address,
                                                              memory_order_relaxed);
    if (message_state_value == MESSAGE_STATE_FREE) {
        atomic_thread_fence(memory_order_acquire);
        //can consume
        atomic_store_explicit(consumer_cache_position_address, next_consumer_cache_position, memory_order_relaxed);
        return true;
    } else {
        //fallback case: try the current claimed message
        const _Atomic uint32_t *const claimed_message_state_atomic_address = (_Atomic uint32_t *) (buffer +
                                                                                                   message_state_offset);
        const uint32_t claimed_message_state_value = atomic_load_explicit(claimed_message_state_atomic_address,
                                                                          memory_order_relaxed);
        if (claimed_message_state_value != MESSAGE_STATE_FREE) {
            return false;
        }
        atomic_thread_fence(memory_order_acquire);
        return true;
    }
}

bool try_fs_rb_sp_claim(const struct fs_rb_t *const header,
                        const uint32_t max_look_ahead_step,
                        uint8_t **const claimed_message) {
    uint8_t *const buffer = header->buffer;
    const index_t mask = header->mask;
    const index_t aligned_message_size = header->aligned_message_size;
    const uint64_t consumer_cache_position = atomic_load_explicit(&header->producer.consumer_cache_position,
                                                                  memory_order_relaxed);
    const uint64_t producer_position = atomic_load_explicit(&header->producer.producer_position, memory_order_relaxed);
    const index_t message_state_offset = (producer_position & mask) * aligned_message_size;
    //the consumer_cache_position is no longer valid?
    if (producer_position >= consumer_cache_position &&
        !sp_claim_slow_path(buffer, message_state_offset, &header->producer.consumer_cache_position,
                            consumer_cache_position,
                            max_look_ahead_step, mask, aligned_message_size)) {
        return false;
    }
    atomic_store_explicit(&header->producer.producer_position, producer_position + 1, memory_order_relaxed);
    *claimed_message = buffer + message_state_offset + MESSAGE_STATE_SIZE;
    return true;
}

static bool mp_claim_slow_path(const _Atomic uint64_t *const consumer_position_address,
                               const _Atomic uint64_t *const consumer_cache_position_address,
                               const int64_t wrap_point, int64_t *consumer_cache_position) {
    //the queue is really full??
    const uint64_t consumer_position = atomic_load_explicit(consumer_position_address, memory_order_relaxed);
    if (consumer_position <= wrap_point) {
        return false;
    } else {
        atomic_thread_fence(memory_order_acquire);
        *consumer_cache_position = consumer_position;
        atomic_store_explicit(consumer_cache_position_address, consumer_position, memory_order_relaxed);
        return true;
    }
}

bool try_fs_rb_mp_claim(
        const struct fs_rb_t *const header,
        uint8_t **const claimed_message) {
    uint8_t *const buffer = header->buffer;
    const index_t mask = header->mask;
    const index_t capacity = header->capacity;
    const index_t aligned_message_size = header->aligned_message_size;
    int64_t producer_position = atomic_load_explicit(&header->producer.producer_position, memory_order_acquire);
    int64_t consumer_cache_position = atomic_load_explicit(&header->producer.consumer_cache_position, memory_order_relaxed);
    do {
        const int64_t wrap_point = producer_position - capacity;
        if (consumer_cache_position <= wrap_point) {
            //is *REALLY* full?
            if (!mp_claim_slow_path(&header->consumer.consumer_position, &header->producer.consumer_cache_position, wrap_point,
                                    &consumer_cache_position)) {
                return false;
            }
        }
        //i'm using memory_order_seq_cst instead of release for an additional guarantee
        //for weaker memory model archs: a full barrier means that the offering thread could rely
        //on seq_cst offer
    } while (!atomic_compare_exchange_weak_explicit(&header->producer.producer_position, &producer_position,
                                                    producer_position + 1, memory_order_seq_cst, memory_order_relaxed));
    const index_t message_state_offset = (producer_position & mask) * aligned_message_size;
    *claimed_message = buffer + message_state_offset + MESSAGE_STATE_SIZE;
    return true;
}

void fs_rb_commit_claim(const uint8_t *const claimed_message_address) {
    const _Atomic uint32_t *const message_state = (_Atomic uint32_t *) (claimed_message_address - MESSAGE_STATE_SIZE);
    atomic_store_explicit(message_state, MESSAGE_STATE_BUSY, memory_order_release);
}


bool try_fs_rb_claim_read(const struct fs_rb_t *const header,
                          uint8_t **const read_message_address) {
    uint8_t *const buffer = header->buffer;
    const index_t mask = header->mask;
    const index_t aligned_message_size = header->aligned_message_size;
    const uint64_t consumer_position = atomic_load_explicit(&header->consumer.consumer_position, memory_order_relaxed);
    const index_t message_state_offset = (consumer_position & mask) * aligned_message_size;
    uint8_t *const message_state_address = buffer + message_state_offset;
    const _Atomic uint32_t *const message_state_atomic_address = (_Atomic uint32_t *) message_state_address;
    //check if it could be empty
    if (atomic_load_explicit(message_state_atomic_address, memory_order_acquire) == MESSAGE_STATE_FREE) {
        const uint64_t producer_position = atomic_load_explicit(&header->producer.producer_position,
                                                                memory_order_relaxed);
        if (producer_position == consumer_position) {
            //is empty for REAL!
            return false;
        } else {
            atomic_thread_fence(memory_order_acquire);
            //spin until the element will became visible: not elegant but with n threads <= n cores it shouldn't happen
            //so easily
            while (atomic_load_explicit(message_state_atomic_address, memory_order_relaxed) == MESSAGE_STATE_FREE) {

            }
        }
    }
    *read_message_address = message_state_address + MESSAGE_STATE_SIZE;
    //can already update the message_state here because the consumer slot won't be available
    //until the consumer_position will move forward
    atomic_store_explicit(message_state_atomic_address, MESSAGE_STATE_FREE, memory_order_relaxed);
    return true;
}


void fs_rb_commit_read(const struct fs_rb_t *const header) {
    const uint64_t consumer_position = atomic_load_explicit(&header->consumer.consumer_position, memory_order_relaxed);
    //it releases previous writes:
    //- message state
    //- message content
    atomic_store_explicit(&header->consumer.consumer_position, consumer_position + 1, memory_order_release);
}

uint32_t fs_rb_read(
        const struct fs_rb_t *const header,
        const fs_rb_message_consumer consumer,
        const uint32_t count, void *const context) {
    uint8_t *const buffer = header->buffer;
    uint32_t msg_read = 0;
    const index_t mask = header->mask;
    const index_t aligned_message_size = header->aligned_message_size;
    const uint64_t consumer_position = atomic_load_explicit(&header->consumer.consumer_position, memory_order_relaxed);
    while (msg_read < count) {
        const uint64_t message_position = consumer_position + msg_read;
        const index_t message_state_offset = (message_position & mask) * aligned_message_size;
        uint8_t *const message_state_address = buffer + message_state_offset;
        const _Atomic uint32_t *const message_state_atomic_address = (_Atomic uint32_t *) message_state_address;
        const uint32_t message_state_value = atomic_load_explicit(message_state_atomic_address, memory_order_relaxed);
        if (message_state_value == MESSAGE_STATE_FREE) {
            return msg_read;
        } else {
            atomic_thread_fence(memory_order_acquire);
            uint8_t *message_content_address = message_state_address + MESSAGE_STATE_SIZE;
            const bool stop = !consumer(message_content_address, context);
            //this first release is necessary in the single producer case to be sure that ay operation on the message content performed in consumer
            //will be visible to it when it will acquire the message state indicator
            atomic_store_explicit(message_state_atomic_address, MESSAGE_STATE_FREE, memory_order_release);
            //the second release is necessary to be sure that in the multi producer case, the last store of the indicator will happen before the consume read
            atomic_store_explicit(&header->consumer.consumer_position, message_position + 1, memory_order_release);
            msg_read++;
            if (stop) {
                return msg_read;
            }
        }
    }
    return count;
}

bool fs_rb_is_empty(const struct fs_rb_t *const header) {
    const uint64_t consumer_position = atomic_load_explicit(&header->consumer.consumer_position, memory_order_relaxed);
    const uint64_t producer_position = atomic_load_explicit(&header->producer.producer_position, memory_order_relaxed);
    return producer_position == consumer_position;
}

index_t fs_rb_size(const struct fs_rb_t *const header) {
    uint64_t consumer_position = atomic_load_explicit(&header->consumer.consumer_position, memory_order_relaxed);
    while (true) {
        const uint64_t before = consumer_position;
        const uint64_t producer_position = atomic_load_explicit(&header->producer.producer_position, memory_order_relaxed);
        consumer_position = atomic_load_explicit(&header->consumer.consumer_position, memory_order_relaxed);
        if (before == consumer_position) {
            return (index_t) (producer_position - consumer_position);
        }
    }
}
