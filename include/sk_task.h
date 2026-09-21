#pragma once

#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
//the function each worker will call to execute( written and passed by the developer)
typedef void (*sc_task_fn)(void* user_data, size_t start_idx, size_t end_idx);

typedef struct sc_task_i{

   alignas(64) void* task_payload;

    size_t start_inx;

    size_t end_inx;

    sc_task_fn task_fn;

    //count of children tasks still running(while this task is waiting)
    atomic_size_t unfinished_dependencies;

    //which parent task needs this task completed, null if no one
    struct sc_task_i* parent_task;

    //index for the free list in the memory pool
    _Atomic uint32_t next_free_idx;

}sc_task;

sc_task* memory_pool_acquire();
void memory_pool_release(sc_task* task) ;