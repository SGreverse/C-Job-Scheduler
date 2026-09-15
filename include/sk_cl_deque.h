#pragma once

#include "sk_task.h"
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct{
    alignas(64) atomic_int_least64_t top;
    
    alignas(64) atomic_int_least64_t bottom;

    sc_task** buffer;

    int64_t capacity_mask;

}sc_cl_deque;

void chase_lev_init(sc_cl_deque* deque, int64_t deque_size);

//owner operations
bool chase_lev_push(sc_cl_deque* deque, sc_task* task);
sc_task* chase_lev_pop(sc_cl_deque* deque);
size_t chase_lev_push_batch(sc_cl_deque* deque, sc_task** tasks, size_t count);
// thief operation 
sc_task* chase_lev_steal(sc_cl_deque* victim_deque);