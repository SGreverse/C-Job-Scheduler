/* To use this library, do this in EXACTLY ONE C file: */
/* #define SK_SCHEDULER_IMPLEMENTATION */
/* #include "sk_scheduler.h" */

#if defined(__linux__) && !defined(_GNU_SOURCE)
    #define _GNU_SOURCE
#endif

#pragma once
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
    #include <immintrin.h>
    #define cpu_relax() _mm_pause()
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define cpu_relax() __asm__ volatile("yield" ::: "memory")
#else
    // Fallback for unsupported architectures
    #define cpu_relax() continue;
#endif

#include <stdatomic.h>
#include <stddef.h>
#include <stdalign.h>
#include <stdbool.h>
#if !defined (_WIN32)
    #include <unistd.h>
#endif

//the function each worker will call to execute( written and passed by the developer)
typedef void (*sc_job_fn)(void* user_data, size_t start_idx, size_t end_idx);

//the main struct the developer creates/gets to be able to check on how his job is doing
typedef struct {
    
    //aligning prevents false sharing when jobs are in array
    alignas(64) atomic_bool is_done;

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
    return atomic_load(&job->is_done);
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

#ifdef SK_SCHEDULER_IMPLEMENTATION

/* --- sk_memory_utils.h --- */
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

//aligns the pointer so that it would fit the aligment requirement
//must use sys_aligned_free() to free it
static inline void* sys_aligned_alloc(size_t alignment, size_t size) {
    size_t offset = alignment - 1 + sizeof(void*);
    void* raw_ptr = malloc(size + offset);
    
    if (!raw_ptr) {
        return NULL;
    }

    uintptr_t unaligned_addr = (uintptr_t)raw_ptr + sizeof(void*);
    uintptr_t aligned_addr = (unaligned_addr + alignment - 1) & ~(alignment - 1);
    void* aligned_ptr = (void*)aligned_addr;

    ((void**)aligned_ptr)[-1] = raw_ptr;

    return aligned_ptr;
}

//frees the aligned memory
static inline void sys_aligned_free(void* aligned_ptr) {
    if (aligned_ptr) {
        free(((void**)aligned_ptr)[-1]);
    }
}

//round size_t to the next power of 2
static inline size_t next_power_of_two(size_t size) {
    // Clamp mathematically invalid capacities to a safe minimum
    if (size <= 2) {
        return 2;
    }

    size--;

    size |= size >> 1;
    size |= size >> 2;
    size |= size >> 4;
    size |= size >> 8;
    size |= size >> 16;

#if SIZE_MAX == 0xFFFFFFFFFFFFFFFFULL
    size |= size >> 32;
#endif

    return size + 1;
}

/* --- sk_task.h --- */
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

typedef struct {
    alignas(64) atomic_uint_least64_t head;

    sc_task* backing_array;

    size_t total_capacity;
} sc_memory_pool;

sc_task* memory_pool_acquire();
void memory_pool_release(sc_task* task) ;

/* --- sk_cl_deque.h --- */
#pragma once

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

/* --- sk_worker.h --- */
#pragma once

#if defined(_WIN32)
    #include <windows.h>
    typedef HANDLE sk_thread_t;
#else
    #include <pthread.h>
    typedef pthread_t sk_thread_t;
#endif

#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>



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

    sk_thread_t thread_handler;

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


/* --- sk_cl_deque.c --- */


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
    // attempt CAS race against any concurrent thieves
    bool won_race = atomic_compare_exchange_strong_explicit(
        &deque->top, 
        &expected_t, 
        t + 1, 
        memory_order_seq_cst, 
        memory_order_relaxed
    );
    
    //if CAS faield, theif got the item first
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

/* --- sk_worker.c --- */
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
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>



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

extern sc_worker* worker_pool;
extern size_t get_cpu_logical_proccessors(void);
extern void memory_pool_release(sc_task* task);




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
void spsc_destroy(sc_spsc_queue* spsc_queue) {
    if (spsc_queue->buffer != NULL) {
        free(spsc_queue->buffer);
        spsc_queue->buffer = NULL;
    }
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
        fprintf(stderr, "Fatal Hardware Error: Failed to allocate aligned worker pool.\n");
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
    size_t total_cores = get_cpu_logical_proccessors();
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
            // sleect random worker id
            uint64_t random_32 = (uint64_t)xorshift32(&rng_state);
            size_t victim_id = (size_t)((random_32 * (uint64_t)total_cores) >> 32);
            
            if (victim_id != local_worker->core_id) {
                sc_cl_deque* victim_deque = &worker_pool[victim_id].cl_deque;
                

                tl_current_task = chase_lev_steal(victim_deque);
            }
        }

        if (!tl_current_task) {
            cpu_relax();
            continue;
        }

        //executes the function
        tl_current_task->task_fn(tl_current_task->task_payload, tl_current_task->start_inx, tl_current_task->end_inx);

        //drop the virtual lock
        size_t remaining_deps = atomic_fetch_sub_explicit(
            &tl_current_task->unfinished_dependencies, 
            1, 
            memory_order_acq_rel
        );

        if (remaining_deps == 1) {
            sc_task* parent = tl_current_task->parent_task;
            memory_pool_release(tl_current_task); // Safely recycle the child immediately

            // Traverse the DAG to resolve cascading unparks without dropping pointers
            while (parent != NULL) {
                size_t prev_parent = atomic_fetch_sub_explicit(
                    &parent->unfinished_dependencies, 1, memory_order_release
                );

                if (prev_parent == 1) {
                    atomic_thread_fence(memory_order_acquire);
                    atomic_store_explicit(&parent->unfinished_dependencies, 1, memory_order_relaxed);

                    if (!chase_lev_push(&local_worker->cl_deque, parent)) {
                        // execute inline when deque is full
                        tl_current_task = parent;
                        parent->task_fn(
                            parent->task_payload, 
                            parent->start_inx, 
                            parent->end_inx
                        );
                        
                        sc_task* next_parent = parent->parent_task;
                        memory_pool_release(parent);
                        parent = next_parent;
                        continue;
                    }
                }
                break;
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

//splits the job into tasks into 2 halfs and pushes right task to deque, until the payload size is smaller then chunk size
void internal_macro_job_splitter(void* payload, size_t start_idx, size_t end_idx) {
    sc_job* job = (sc_job*)payload;

    // root task handles the completion
    scheduler_set_continuation(no_op_continuation);

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

/* --- sk_scheduler.c --- */


#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
    #define _POSIX_C_SOURCE 199309L
    #include <sched.h>
    #include <pthread.h>
#elif defined(_WIN32)
    #include <windows.h>
#endif

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>


#define DEFAULT_CHUNK_SIZE 128
#define BATCH_SIZE 256

#define POOL_EMPTY_INDEX 0xFFFFFFFF

#define TASK_CACHE_CAPACITY 64



// global memory pool,allocated at boot
alignas(64) atomic_uint_least64_t global_pool_head;
sc_task* global_task_array = NULL;
size_t global_task_capacity = 0;

//every thread has his own cache of tasks 
_Thread_local sc_task* task_cache[TASK_CACHE_CAPACITY];
_Thread_local size_t task_cache_count = 0;

sc_worker* worker_pool=NULL;

//robin round counter for worker
static atomic_size_t rr_worker_index = ATOMIC_VAR_INIT(0);

extern _Thread_local sc_worker* tl_current_worker;
extern _Thread_local sc_task* tl_current_task;



static inline uint64_t pack_aba(uint32_t index, uint32_t tag) {
    return ((uint64_t)tag << 32) | index;
}

static inline uint32_t unpack_index(uint64_t packed_val) {
    return (uint32_t)(packed_val & 0xFFFFFFFF);
}

static inline uint32_t unpack_tag(uint64_t packed_val) {
    return (uint32_t)(packed_val >> 32);
}

sc_task* memory_pool_acquire() {

    //if thread cached memory pool contains any available tasks, pop it from there
    if(task_cache_count>0){
        return task_cache[--task_cache_count];
    }

    //if cache is empty, grab a task from global memory pool
    uint64_t current_head = atomic_load_explicit(&global_pool_head, memory_order_acquire);
    uint32_t head_idx;
    
    while (true) {
        head_idx = unpack_index(current_head);
        
        if (head_idx == POOL_EMPTY_INDEX) {
            return NULL;
        }

        uint32_t next_idx = atomic_load_explicit(&global_task_array[head_idx].next_free_idx, memory_order_relaxed);
        uint32_t current_tag = unpack_tag(current_head);
        uint64_t new_head = pack_aba(next_idx, current_tag + 1);

        if (atomic_compare_exchange_weak_explicit(
                &global_pool_head, 
                &current_head, 
                new_head,
                memory_order_acquire, 
                memory_order_acquire)) {
            break;
        }
        
        // force the pipeline to pause and back off the memory bus
        cpu_relax(); 
    }
    
    return &global_task_array[head_idx];
}

void memory_pool_release(sc_task* task) {
    if(task_cache_count < TASK_CACHE_CAPACITY){
        task_cache[task_cache_count++] = task;
        return;
    }
    
    // cache full, evict(is it evict or avict) the task cache
    for (size_t i = 1; i < TASK_CACHE_CAPACITY; i++) {
        uint32_t prev_idx = (uint32_t)(task_cache[i - 1] - global_task_array);
        task_cache[i]->next_free_idx = prev_idx;
    }

    uint32_t current_task_idx = (uint32_t)(task - global_task_array);
    task->next_free_idx = (uint32_t)(task_cache[TASK_CACHE_CAPACITY - 1] - global_task_array);
    uint32_t tail_idx = (uint32_t)(task_cache[0] - global_task_array);

    uint64_t current_head = atomic_load_explicit(&global_pool_head, memory_order_relaxed);

    while (true) {
        uint32_t old_head_idx = unpack_index(current_head);
        global_task_array[tail_idx].next_free_idx = old_head_idx;
        
        uint32_t current_tag = unpack_tag(current_head);
        uint64_t new_head = pack_aba(current_task_idx, current_tag + 1);

        if (atomic_compare_exchange_weak_explicit(&global_pool_head, &current_head, new_head,memory_order_release, memory_order_relaxed)) {
            break;
        }
        cpu_relax();
    }
    
    task_cache_count = 0;
}
// gets the amount of logical hardware threads
size_t get_cpu_logical_proccessors(void) {
#if defined(_WIN32)
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return sysinfo.dwNumberOfProcessors;
#elif defined(_SC_NPROCESSORS_ONLN)
    return sysconf(_SC_NPROCESSORS_ONLN);
#elif defined(_SC_NPROCESSORS_CONF)
    return sysconf(_SC_NPROCESSORS_CONF);
#else
    return 1; // Fallback default if unknown
#endif
}

static inline bool spsc_producer_push(sc_worker* worker, sc_task* task) {
    size_t current_tail = atomic_load_explicit(&worker->rb_tail, memory_order_relaxed);
    size_t capacity = worker->rb_capacity_mask + 1;

    if (current_tail - worker->rb_head_cache >= capacity) {
        worker->rb_head_cache = atomic_load_explicit(&worker->rb_head, memory_order_acquire);
        if (current_tail - worker->rb_head_cache >= capacity) {
            return false;
        }
    }
    
    worker->rb_buffer[current_tail & worker->rb_capacity_mask] = task;
    atomic_store_explicit(&worker->rb_tail, current_tail + 1, memory_order_release);
    return true;
}

//fw decleraitons
void internal_macro_job_splitter(void* user_data, size_t start_idx, size_t end_idx);
void hooked_job_finish_task(void* payload,size_t start_inx,size_t end_inx);

void scheduler_boot(size_t spsc_queue_size,size_t cl_deque_size){
    size_t lp_count=get_cpu_logical_proccessors();
    if(lp_count<1){
        lp_count=1;
        printf("Bitch ass cpu core sys call \n");
    }

    //initialize the task pool
    global_task_capacity = 65536;
    global_task_array = (sc_task*)sys_aligned_alloc(64, global_task_capacity * sizeof(sc_task));

    if (!global_task_array) {
        fprintf(stderr, "Fatal Hardware Error: Failed to allocate neccesary data for the global memory pool.\n");
        exit(EXIT_FAILURE);
    }

    for (size_t i = 0; i < global_task_capacity; i++) {
        
        //link the free list
        if (i == global_task_capacity - 1) {
            global_task_array[i].next_free_idx = POOL_EMPTY_INDEX;
        } else {
            global_task_array[i].next_free_idx = (uint32_t)(i + 1);
        }
        
        //initialize the tasks
        atomic_init(&global_task_array[i].unfinished_dependencies, 0);
        global_task_array[i].parent_task = NULL;
    }

    
    uint64_t initial_head = pack_aba(0, 0);
    atomic_init(&global_pool_head, initial_head);

    
    //allocate all worker threads and run them
    worker_pool=allocate_worker_pool(lp_count,spsc_queue_size,cl_deque_size);
    if(!worker_pool){
        fprintf(stderr, "Fatal Hardware Error: Failed to allocate worker pool.\n");
        return;
    }
    for(size_t i=0;i<lp_count;i++){
        #if defined(_WIN32)
            worker_pool[i].thread_handler = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)worker_main_loop, &worker_pool[i], 0, NULL);
            if(!worker_pool[i].thread_handler){
                fprintf(stderr,"Thread Creating Error:Thread #%lu couldn't be created",i+1);
            }
        #else
            int result=pthread_create(&worker_pool[i].thread_handler, NULL, worker_main_loop, &worker_pool[i]);
            if(result!=0){
                fprintf(stderr,"Thread Creating Error:Thread #%lu couldn't be created",i+1);
            }
        #endif
    }

}


void scheduler_init_job(sc_job* job,void* payload,size_t payload_size,sc_job_fn job_fn){
    job->is_done=false;
    job->job_payload=payload;
    job->payload_size=payload_size;
    job->job_function=job_fn;
    job->chunk_size=0;
}


bool scheduler_submit_job(sc_job* job,size_t chunk_size){
    if(chunk_size>job->payload_size || chunk_size==0){
        chunk_size=DEFAULT_CHUNK_SIZE;
    }
    job->chunk_size = chunk_size;

    if(!worker_pool){
        return false;
    }

    sc_task* root_task=memory_pool_acquire();
    if(!root_task){
        //if no more tasks available in the pool, reutrn false
        return false;
    }
    root_task->task_payload=job;
    root_task->start_inx=0;
    root_task->end_inx=job->payload_size;
    root_task->task_fn=hooked_job_finish_task;
    root_task->parent_task=NULL;
    atomic_store(&root_task->unfinished_dependencies,1);

    sc_task* child_task = memory_pool_acquire();
    if(!child_task) {
        memory_pool_release(root_task);
        return false;
    }
    
    child_task->task_payload = job;
    child_task->start_inx = 0;
    child_task->end_inx = job->payload_size;
    child_task->task_fn = internal_macro_job_splitter;
    child_task->parent_task = root_task; 
    atomic_store(&child_task->unfinished_dependencies, 1);

    size_t worker_count = get_cpu_logical_proccessors();
    if (worker_count < 1) worker_count = 1;
    size_t target_idx = atomic_fetch_add_explicit(&rr_worker_index, 1, memory_order_relaxed) % worker_count;

    bool success =spsc_producer_push(&worker_pool[target_idx], child_task);

    if (!success) {
        //if spsc if full, return the task to the pool
        memory_pool_release(root_task);
        return false;
    }



    return true;

}
void scheduler_spawn_subtasks(void* payload, size_t start_idx, size_t end_idx, size_t chunk_size, sc_job_fn task_fn) {
    if (start_idx >= end_idx) return;
    if (chunk_size == 0) chunk_size = DEFAULT_CHUNK_SIZE; 

    sc_task* parent = tl_current_task;
    size_t total_elements = end_idx - start_idx;
    size_t total_required_chunks = (total_elements + chunk_size - 1) / chunk_size;

    size_t chunks_processed = 0;

    // no cap on how many tasks the developer can push
    //but push batches of 256 tasks at a time
    while (chunks_processed < total_required_chunks) {
        
        size_t chunks_this_pass = total_required_chunks - chunks_processed;
        if (chunks_this_pass > BATCH_SIZE) {
            chunks_this_pass = BATCH_SIZE;
        }

        sc_task* batch_array[BATCH_SIZE];
        size_t successfully_allocated = 0;

        for (size_t i = 0; i < chunks_this_pass; i++) {
            sc_task* child = memory_pool_acquire();
            if (!child) break; 
            
            size_t start = start_idx + ((chunks_processed + i) * chunk_size);
            size_t end = start + chunk_size;
            if (end > end_idx) end = end_idx;

            child->task_payload = payload;
            child->start_inx = start;
            child->end_inx = end;
            child->task_fn = task_fn;
            child->parent_task = parent;
            
            atomic_store_explicit(&child->unfinished_dependencies, 1, memory_order_relaxed);
            
            batch_array[successfully_allocated++] = child;
        }

        if (successfully_allocated == 0) {
            size_t remaining_start = start_idx + (chunks_processed * chunk_size);
            task_fn(payload, remaining_start, end_idx);
            return;
        }

        atomic_fetch_add_explicit(&parent->unfinished_dependencies, successfully_allocated, memory_order_relaxed);


        size_t pushed_count = chase_lev_push_batch(&tl_current_worker->cl_deque, batch_array, successfully_allocated);

        if (pushed_count < successfully_allocated) {
            size_t rejected_count = successfully_allocated - pushed_count;
            
            // rollback rejected tasks
            atomic_fetch_sub_explicit(&parent->unfinished_dependencies, rejected_count, memory_order_relaxed);

            // execute the rejected tasks inline
            for (size_t i = pushed_count; i < successfully_allocated; i++) {
                sc_task* rejected_task = batch_array[i];
                task_fn(payload, rejected_task->start_inx, rejected_task->end_inx);
                memory_pool_release(rejected_task);
            }
            
            size_t remaining_start = start_idx + ((chunks_processed + successfully_allocated) * chunk_size);
            if (remaining_start < end_idx) {
                task_fn(payload, remaining_start, end_idx);
            }
            return;
        }

        chunks_processed += chunks_this_pass;
    }
}

void scheduler_set_continuation(sc_job_fn next_task){
    tl_current_task->task_fn=next_task;
}

void scheduler_stop_workers(){
    if(!worker_pool) return;

    size_t lp_count = get_cpu_logical_proccessors();
    while(lp_count<1){
        lp_count = get_cpu_logical_proccessors();
        printf("Bitch ass cpu core sys call \n");
    }

    for (size_t i = 0; i < lp_count; i++) {
        atomic_store_explicit(&worker_pool[i].termination_flag, true, memory_order_release);
    }

    for (size_t i = 0; i < lp_count; i++) {
        #if defined(_WIN32)
            WaitForSingleObject(worker_pool[i].thread_handler, INFINITE);
            CloseHandle(worker_pool[i].thread_handler);
        #else
            pthread_join(worker_pool[i].thread_handler, NULL);
        #endif
    }

    for (size_t i = 0; i < lp_count; i++) {
        if (worker_pool[i].cl_deque.buffer) {
            sys_aligned_free(worker_pool[i].cl_deque.buffer);
        }
        
        if (worker_pool[i].rb_buffer) {
            sys_aligned_free(worker_pool[i].rb_buffer); 
        }
    }

    if (global_task_array) {
        sys_aligned_free(global_task_array);
        global_task_array = NULL;
    }

    sys_aligned_free(worker_pool);
    worker_pool = NULL;
    
}

void scheduler_wait_for_job(sc_job* job) {
    uint32_t spin_count = 0;
    uint32_t yield_count = 0;
    long sleep_ns = 1000; 

    while (!scheduler_is_job_complete(job)) {
        if (spin_count < 1000) {
            cpu_relax();
            spin_count++;
        } 
        else if (yield_count < 50) {
            #if defined(_WIN32)
                Sleep(0);
            #else
                sched_yield(); 
            #endif
            yield_count++;
        } 
        else {
            #if defined(_WIN32)
                Sleep((DWORD)(sleep_ns / 1000000));
            #else
                struct timespec ts = {0, sleep_ns};
                nanosleep(&ts, NULL);
            #endif
            
            if (sleep_ns < 1000000) {
                sleep_ns *= 2; 
            }
        }
    }
}

#endif /* SK_SCHEDULER_IMPLEMENTATION */
