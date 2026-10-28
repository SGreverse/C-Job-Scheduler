#pragma once
#define _GNU_SOURCE
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
    #include <immintrin.h>
    #define cpu_relax() _mm_pause()
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define cpu_relax() __asm__ volatile("yield" ::: "memory")
#else
    // Fallback for unsupported architectures
    #define cpu_relax() continue;
#endif

#include "sk_worker.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdalign.h>
#include <stdbool.h>

#if defined(__linux__)
    #include <unistd.h>
    #include <limits.h>
    #include <sys/syscall.h>
    #include <linux/futex.h>

    static inline void sk_futex_wait(_Atomic uint32_t* addr, uint32_t expected) {
        syscall(SYS_futex, (uint32_t*)addr, FUTEX_WAIT_PRIVATE, expected, NULL, NULL, 0);
    }

    static inline void sk_futex_wake(_Atomic uint32_t* addr) {
        syscall(SYS_futex, (uint32_t*)addr, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
    }
#elif defined(_WIN32)
    #include <windows.h>
    #pragma comment(lib, "Synchronization.lib")

    static inline void sk_futex_wait(_Atomic uint32_t* addr, uint32_t expected) {
        WaitOnAddress((volatile VOID*)addr, &expected, sizeof(uint32_t), INFINITE);
    }

    static inline void sk_futex_wake(_Atomic uint32_t* addr) {
        WakeByAddressAll((PVOID)addr);
    }
#else
    #include <unistd.h>
    #include <sched.h>

    static inline void sk_futex_wait(_Atomic uint32_t* addr, uint32_t expected) {
        if (atomic_load_explicit(addr, memory_order_relaxed) == expected) {
            sched_yield();
        }
    }

    static inline void sk_futex_wake(_Atomic uint32_t* addr) {
        (void)addr;
    }
#endif

#define JOB_STATE_RUNNING 0U
#define JOB_STATE_WAITING 1U
#define JOB_STATE_DONE    2U

extern sc_worker* worker_pool;

//the function each worker will call to execute( written and passed by the developer)
typedef void (*sc_job_fn)(void* user_data, size_t start_idx, size_t end_idx);

//the main struct the developer creates/gets to be able to check on how his job is doing
typedef struct {
    
    //aligning prevents false sharing when jobs are in array
    alignas(64) _Atomic uint32_t state;

    void* job_payload;

    size_t payload_size;

    size_t chunk_size;

    sc_job_fn job_function;

} sc_job;

//sets up the workers, the spsc buffer for task transmissioning, and the chase-lev deque(rounds deque and spsc queue sizes up to nearest power of 2).
//required before any form of job submission.
//after calling boot, when you dont want the threads to keep running, call scheduler_stop_workers() to stop the threads and free all the memory.
void scheduler_boot(size_t spsc_queue_size,size_t cl_deque_size);

//pass a ptr to an pre allocated job struct,and initialize the struct
void scheduler_init_job(sc_job* empty_job,void* payload,size_t payload_size,sc_job_fn job_fn);

//starts the job, while deciding the chunk division of the payload(which willl be later split into tasks of that chunk size).
//any illegal size will result in the default 128 chunk size(above payload_size). if you want the default chunk size, pass 0
bool scheduler_submit_job(sc_job* job,size_t chunk_size);

static inline bool scheduler_is_job_complete(sc_job* job) {
    return atomic_load_explicit(&job->state, memory_order_acquire) == JOB_STATE_DONE;
}

//sets what the parent task will execute after the child tasks that he spawned finish and parent task will start executing again


// spawns child tasks that the current task executing will be dependant on.
//will ONLY work when called from within the parent task function.
// choose wisely what the chunk size will be since it will be pushed in 256 task batch and any task that doesnt fit/fails to be allocated-
//will be executed inline
void scheduler_spawn_subtasks(void* payload, size_t start_idx, size_t end_idx, size_t chunk_size, sc_job_fn task_fn);

//sets what the current task will do after all child sub tasks finish and return back to the parent
void scheduler_set_continuation(sc_job_fn next_task);

//cleans all the allocated memory, and frees all the worker threads
void scheduler_stop_workers();

// blocks the calling thread until the job is complete
void scheduler_wait_for_job(sc_job* job);