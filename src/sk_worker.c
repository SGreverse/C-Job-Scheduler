#if defined(__linux__)
    #define _GNU_SOURCE 
#endif

#include <stdio.h>

#if defined(__linux__)
    #include <pthread.h>
    #include <sched.h>
#elif defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#elif defined(__APPLE__)
    #include <pthread.h>
    #include <mach/mach_init.h>
    #include <mach/thread_policy.h>
    #include <mach/thread_act.h>
#endif
#include "sk_scheduler.h"
#include "sk_worker.h"
#include "sk_task.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>


extern sc_worker* worker_pool;
extern size_t global_worker_count;

_Thread_local sc_worker* tl_current_worker = NULL;
_Thread_local sc_task* tl_current_task = NULL;

// psuedo RNG
static inline uint32_t xorshift32(uint32_t* state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}


//pops from the worker's spsc queue 
static inline sc_task* spsc_consumer_pop(sc_worker* worker) {
    size_t current_head = atomic_load_explicit(&worker->rb_head, memory_order_relaxed);
    
    if (current_head >= worker->rb_tail_cache) {
        worker->rb_tail_cache = atomic_load_explicit(&worker->rb_tail, memory_order_acquire);
        if (current_head >= worker->rb_tail_cache) {
            return NULL;
        }
    }
    
    sc_task* task = worker->rb_buffer[current_head & worker->rb_capacity_mask];
    atomic_store_explicit(&worker->rb_head, current_head + 1, memory_order_release);
    return task;
}

static inline void pin_thread_to_core(size_t core_id) {
#if defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    
    pthread_t current_thread = pthread_self();
    if (pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset) != 0) {
        fprintf(stderr, "Hardware Warning: Failed to pin thread to core %zu\n", core_id);
    }

#elif defined(_WIN32)
    HANDLE thread = GetCurrentThread();
    // Bitwise shift to generate the affinity mask for the target core
    DWORD_PTR mask = (DWORD_PTR)1 << core_id;
    if (SetThreadAffinityMask(thread, mask) == 0) {
        fprintf(stderr, "Hardware Warning: Failed to pin thread to core %zu\n", core_id);
    }

#elif defined(__APPLE__)
    // Darwin/XNU kernel does not permit strict pinning, but supports "Affinity Tags".
    // This hints the scheduler to keep threads sharing a tag on the same L2/L3 cache cluster.
    thread_port_t mach_thread = pthread_mach_thread_np(pthread_self());
    thread_affinity_policy_data_t policy = { (integer_t)core_id };
    thread_policy_set(mach_thread, THREAD_AFFINITY_POLICY, (thread_policy_t)&policy, 1);
#else
    // Fallback for unsupported architectures
    fprintf(stderr, "Hardware Warning: Thread pinning not supported on this OS.\n");
#endif
}

sc_worker* allocate_worker_pool(size_t logical_proccessors_amount,size_t spsc_queue_size,size_t cl_deque_size){
    sc_worker* pool = (sc_worker*)sys_aligned_alloc(64, logical_proccessors_amount * sizeof(sc_worker));
    
    if (!pool) {
        fprintf(stderr, ": Failed to allocate aligned worker pool.\n");
        exit(EXIT_FAILURE);
    }

    for (size_t i = 0; i < logical_proccessors_amount; i++) {
        pool[i].core_id = i;

        size_t actual_size = next_power_of_two(cl_deque_size);

        atomic_init(&pool[i].termination_flag, false);
        chase_lev_init(&pool[i].cl_deque, actual_size);

        actual_size = next_power_of_two(spsc_queue_size);

        pool[i].rb_capacity_mask = actual_size - 1;
        atomic_init(&pool[i].rb_head, 0);
        atomic_init(&pool[i].rb_tail, 0);
        pool[i].rb_head_cache = 0;
        pool[i].rb_tail_cache = 0;
        pool[i].rb_buffer = (sc_task**)sys_aligned_alloc(64, actual_size * sizeof(sc_task*));
        if (!pool[i].rb_buffer){
            fprintf(stderr, "Malloc failure: Couldn't allocate spsc buffer");
            exit(EXIT_FAILURE);
        }

    }

    return pool;
}



void* worker_main_loop(void* arg) {
    sc_worker* local_worker = (sc_worker*)arg;
    tl_current_worker = local_worker; 
    
    uint32_t rng_state = (uint32_t)(local_worker->core_id + 1) * 2654435761U;
    size_t total_cores = global_worker_count;
    if (total_cores < 1){
        total_cores = 1;
        printf("Bitch ass cpu core sys call \n");
    }

    pin_thread_to_core(local_worker->core_id);

    while (!atomic_load_explicit(&local_worker->termination_flag, memory_order_relaxed)) {
        
        tl_current_task = NULL;

        tl_current_task = chase_lev_pop(&local_worker->cl_deque);

        // if chase lev queue is empty, try finding a task in the spsc queue
        if (!tl_current_task) {
            tl_current_task = spsc_consumer_pop(local_worker);
        }

        //steal work
        if (!tl_current_task) {
            if (!tl_current_task && total_cores > 1) {
                size_t victim_id = xorshift32(&rng_state) % (total_cores - 1);
                if (victim_id >= local_worker->core_id) {
                    victim_id++; // Uniformly maps [0 .. N-2] to all cores except local_worker->core_id!
                }
                tl_current_task = chase_lev_steal(&worker_pool[victim_id].cl_deque);
            }
        }

        if (!tl_current_task) {
            cpu_relax();
            continue;
        }
        
        sc_task_fn current_fn = tl_current_task->task_fn;
        tl_current_task->task_fn = NULL;

        // execute the function
        current_fn(tl_current_task->task_payload, tl_current_task->start_inx, tl_current_task->end_inx);
        
        //drop the virtual lock
        size_t remaining_deps = atomic_fetch_sub_explicit(
            &tl_current_task->unfinished_dependencies, 
            1, 
            memory_order_acq_rel
        );

        // the loop allows chaining of continuation and spawn sub task
        //meaning continuation function can also spawn sub tasks infinitly without any problems
        if (remaining_deps == 1) {
            // all children spawned by current_fn already finished while current_fn was running
            // If current_fn registered a continuation, run it right now inline.
            while (tl_current_task->task_fn != NULL) {
                sc_task_fn continuation_fn = tl_current_task->task_fn;
                tl_current_task->task_fn = NULL;
                atomic_store_explicit(&tl_current_task->unfinished_dependencies, 1, memory_order_relaxed);
                
                continuation_fn(tl_current_task->task_payload, tl_current_task->start_inx, tl_current_task->end_inx);
                
                remaining_deps = atomic_fetch_sub_explicit(
                    &tl_current_task->unfinished_dependencies, 
                    1, 
                    memory_order_acq_rel
                );
                if (remaining_deps != 1) {
                    break; // if continuation spawned new children that are still running , break and wait for them to finish
                }
            }

            if (remaining_deps == 1) {
                sc_task* parent = tl_current_task->parent_task;
                memory_pool_release(tl_current_task);

                // Traverse the DAG to resolve cascading unparks
                while (parent != NULL) {
                    size_t parent_dependencies = atomic_fetch_sub_explicit(
                        &parent->unfinished_dependencies, 1, memory_order_acq_rel
                    );

                    if (parent_dependencies != 1) {
                        // parent still has other unfinished children or hasnt dropped its virtual lock yet
                        break;
                    }

                    // this is the last dependency of the parent
                    // check if it has a continuation function to run(same process as before)
                    while (parent->task_fn != NULL) {
                        atomic_store_explicit(&parent->unfinished_dependencies, 1, memory_order_relaxed);

                        if (chase_lev_push(&local_worker->cl_deque, parent)) {
                            // Successfully scheduled parent's continuation on our deque!
                            parent = NULL;
                            break;
                        }

                        // Deque is full: execute parent's continuation inline
                        tl_current_task = parent;
                        sc_task_fn parent_fn = parent->task_fn;
                        parent->task_fn = NULL;

                        parent_fn(
                            parent->task_payload, 
                            parent->start_inx, 
                            parent->end_inx
                        );

                        size_t parent_rem = atomic_fetch_sub_explicit(
                            &parent->unfinished_dependencies, 1, memory_order_acq_rel
                        );
                        if (parent_rem != 1) {
                            // Parent's continuation spawned new async children, stop traversing the DAG
                            parent = NULL;
                            break;
                        }
                    }

                    if (parent == NULL) {
                        break;
                    }

                    // parent has no continuation left, so it is completely done 
                    // so recycle it and continue to the enxt parent
                    sc_task* next_parent = parent->parent_task;
                    memory_pool_release(parent);
                    parent = next_parent;
                }
            }
        }
    }
    
    return NULL;
}

void hooked_job_finish_task(void* payload,size_t start_inx,size_t end_inx){
    (void)start_inx;
    (void)end_inx;
    sc_job* job=(sc_job*)payload;
    
    atomic_store_explicit(&job->is_done,true,memory_order_release );
}

void no_op_continuation(void* payload, size_t start_idx, size_t end_idx) {
    (void)start_idx;
    (void)end_idx;
    (void)payload;
    return;
}

//splits the job into tasks into 2 halfs and keeps pushing right task to deque, until the payload size is smaller then chunk size
void internal_macro_job_splitter(void* payload, size_t start_idx, size_t end_idx) {
    sc_job* job = (sc_job*)payload;

    while ((end_idx - start_idx) > job->chunk_size) {
        size_t total_elements = end_idx - start_idx;
        size_t mid_point = start_idx + (total_elements / 2);

        size_t remainder = mid_point % job->chunk_size;
        if (remainder != 0) {
            mid_point += (job->chunk_size - remainder);
        }

        if (mid_point >= end_idx) break;

        sc_task* right_task = memory_pool_acquire();
        if (!right_task) {
            job->job_function(job->job_payload, mid_point, end_idx);
            end_idx = mid_point;
            continue;
        }

        right_task->task_payload = payload;
        right_task->start_inx = mid_point;
        right_task->end_inx = end_idx;
        right_task->task_fn = internal_macro_job_splitter;
        right_task->parent_task = tl_current_task;

        //set up virtual lock
        atomic_store_explicit(&right_task->unfinished_dependencies, 1, memory_order_relaxed);

        atomic_fetch_add_explicit(&tl_current_task->unfinished_dependencies, 1, memory_order_relaxed);

        if (!chase_lev_push(&tl_current_worker->cl_deque, right_task)) {
            //rollback
            atomic_fetch_sub_explicit(&tl_current_task->unfinished_dependencies, 1, memory_order_relaxed);
            memory_pool_release(right_task);
            job->job_function(job->job_payload, mid_point, end_idx);
        }

        end_idx = mid_point;
    }

        
    tl_current_task->task_payload = job->job_payload;
        
    job->job_function(job->job_payload, start_idx, end_idx);
}