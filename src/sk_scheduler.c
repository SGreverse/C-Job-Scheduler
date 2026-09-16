

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
    #define _POSIX_C_SOURCE 199309L
    #include <sched.h>
    #include <pthread.h>
#elif defined(_WIN32)
    #include <windows.h>
#endif

#include "sk_memory_utils.h"
#include "sk_scheduler.h"
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