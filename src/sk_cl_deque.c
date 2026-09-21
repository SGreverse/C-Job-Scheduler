
#include "sk_cl_deque.h"
#include "sk_memory_utils.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>


void chase_lev_init(sc_cl_deque* deque, int64_t deque_size) {
    int64_t actual_size = (int64_t)next_power_of_two((size_t)deque_size);
    
    deque->capacity_mask = actual_size - 1;
    
    atomic_init(&deque->top, 0);
    atomic_init(&deque->bottom, 0);
    
    
    deque->buffer = (sc_task**)sys_aligned_alloc(64, actual_size * sizeof(sc_task*));
    
    if (!deque->buffer) {
        fprintf(stderr, "Fatal Hardware Error: Failed to allocate aligned Chase-Lev buffer.\n");
        exit(EXIT_FAILURE);
    }
}

bool chase_lev_push(sc_cl_deque *deque, sc_task *task){
    int64_t b=atomic_load_explicit(&deque->bottom, memory_order_relaxed);

    int64_t t = atomic_load_explicit(&deque->top, memory_order_acquire);

    int64_t capacity = deque->capacity_mask+1;

    if (b - t >= capacity) return false;


    deque->buffer[b & deque->capacity_mask]=task;

    atomic_store_explicit(&deque->bottom, b + 1, memory_order_release);

    return true;
}

size_t chase_lev_push_batch(sc_cl_deque* deque, sc_task** tasks, size_t count) {
    if (count == 0) return 0;

    int64_t b = atomic_load_explicit(&deque->bottom, memory_order_relaxed);
    
    int64_t t = atomic_load_explicit(&deque->top, memory_order_acquire);
    int64_t capacity = deque->capacity_mask + 1;

    int64_t available = capacity - (b - t);
    if (available <= 0) return 0;

    size_t push_count = (size_t)(available < (int64_t)count ? available : (int64_t)count);

    
    for (size_t i = 0; i < push_count; i++) {
        deque->buffer[(b + i) & deque->capacity_mask] = tasks[i];
    }

    atomic_store_explicit(&deque->bottom, b + push_count, memory_order_release);

    return push_count;
}

sc_task* chase_lev_pop(sc_cl_deque* deque) {
    //decrememnt bottom
    int64_t b = atomic_load_explicit(&deque->bottom, memory_order_relaxed) - 1;
    
    atomic_store_explicit(&deque->bottom, b, memory_order_relaxed);
    
    atomic_thread_fence(memory_order_seq_cst);

    int64_t t = atomic_load_explicit(&deque->top, memory_order_relaxed);
    
    int64_t size = b - t;
    
    if (size < 0) {
        atomic_store_explicit(&deque->bottom, t, memory_order_relaxed);
        return NULL;
    }
    sc_task* task = deque->buffer[b & deque->capacity_mask];

    if (size > 0) return task;

    int64_t expected_t = t;
    // attempt CAS race against any thieves
    bool won_race = atomic_compare_exchange_strong_explicit(
        &deque->top, 
        &expected_t, 
        t + 1, 
        memory_order_seq_cst, 
        memory_order_relaxed
    );
    
    //if CAS failed, thief got the item first
    if (!won_race) task=NULL;
    
    
    atomic_store_explicit(&deque->bottom, t + 1, memory_order_relaxed);
    
    return task;
}

sc_task* chase_lev_steal(sc_cl_deque *victim_deque){
    int64_t t = atomic_load_explicit(&victim_deque->top, memory_order_seq_cst);
    
    int64_t b = atomic_load_explicit(&victim_deque->bottom, memory_order_acquire);

    int64_t size=b-t;

    if(size<=0) {
        return NULL;
    }

    sc_task* task= victim_deque->buffer[t & victim_deque->capacity_mask];

    bool won_race = atomic_compare_exchange_strong_explicit(
        &victim_deque->top, 
        &t, 
        t + 1, 
        memory_order_seq_cst, 
        memory_order_relaxed
    );
    
    //if CAS faield, thief got the item first
    if (!won_race) return NULL;

    return task;

}