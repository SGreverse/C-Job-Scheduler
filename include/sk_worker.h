#pragma once

#include <bits/pthreadtypes.h>
#include <pthread.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#include "sk_cl_deque.h"
#include "sk_task.h"


typedef struct {
    // external thread exclusive cache line 
    alignas(64) atomic_size_t tail;
    size_t head_cache; // local cache to prevent true sharing memory reads
    
    // wroker exclusive cache line
    alignas(64) atomic_size_t head;
    size_t tail_cache;
    
    size_t capacity_mask; // capacity - 1 for bitwise wrapping

    sc_task** buffer;     

} sc_spsc_queue;

typedef struct{
    
    alignas(64) size_t core_id;

    pthread_t thread_handler;

    atomic_bool termination_flag;


    sc_cl_deque cl_deque;

    alignas(64) atomic_size_t rb_head;
    size_t rb_tail_cache; 

    alignas(64) atomic_size_t rb_tail;
    size_t rb_head_cache; 
    
    size_t rb_capacity_mask;
    sc_task** rb_buffer;

}sc_worker;

//allocates all sc_worker for each cpu core and creates the chase-lev deque and spsc ring buffer for each worker
sc_worker* allocate_worker_pool(size_t logical_proccessors_amount,size_t spsc_queue_size,size_t cl_deque_size);

void* worker_main_loop(void* worker);
