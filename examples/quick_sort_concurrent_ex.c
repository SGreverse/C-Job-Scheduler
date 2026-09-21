#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
    #define _POSIX_C_SOURCE 199309L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// Uncomment when testing as the main entry point with the release header
// #define SK_SCHEDULER_IMPLEMENTATION
#include "sk_scheduler.h"

// Note: 100,000,000 across 100 runs might take a while to benchmark. 
// You can lower ARRAY_SIZE if the total runtime is too long.
#define ARRAY_SIZE 10000
#define BASE_CASE_THRESHOLD 1024
#define NUM_RUNS 100

// Limit raw OS threads to 16 (depth 4) to prevent virtual memory exhaustion
#define MAX_OS_THREAD_DEPTH 4
#define OS_SPAWN_THRESHOLD 1000

// --- Cross-Platform Timing Helper ---
#if defined(_WIN32)
    #include <windows.h>
    typedef HANDLE os_thread_t;
    static double get_time_seconds(void) {
        LARGE_INTEGER freq, time;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&time);
        return (double)time.QuadPart / freq.QuadPart;
    }
#else
    #include <pthread.h>
    typedef pthread_t os_thread_t;
    static double get_time_seconds(void) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
    }
#endif
// ------------------------------------

// --- 1. Common Base Case & Partitioning ---
void insertion_sort(int* arr, size_t start, size_t end) {
    for (size_t i = start + 1; i < end; i++) {
        int key = arr[i];
        size_t j = i;
        while (j > start && arr[j - 1] > key) {
            arr[j] = arr[j - 1];
            j--;
        }
        arr[j] = key;
    }
}

static inline void swap(int* a, int* b) {
    int t = *a; *a = *b; *b = t;
}

// Standard Lomuto Partition scheme
size_t partition(int* arr, size_t start, size_t end) {
    int pivot = arr[end - 1];
    size_t i = start;
    
    for (size_t j = start; j < end - 1; j++) {
        if (arr[j] <= pivot) {
            swap(&arr[i], &arr[j]);
            i++;
        }
    }
    swap(&arr[i], &arr[end - 1]);
    return i;
}

// --- 2. Raw OS Thread Implementation (The Baseline) ---


typedef struct {
    int* arr;
    size_t start;
    size_t end;
    int depth;
} os_qs_args;

void raw_thread_quicksort(int* arr, size_t start, size_t end, int depth);

void* raw_thread_worker(void* arg) {
    // Copy the arguments directly from the parent's stack 
    os_qs_args args = *(os_qs_args*)arg;
    raw_thread_quicksort(args.arr, args.start, args.end, args.depth);
    return NULL;
}

void raw_thread_quicksort(int* arr, size_t start, size_t end, int depth) {
    size_t length = end - start;
    
    if (length <= BASE_CASE_THRESHOLD) {
        insertion_sort(arr, start, end);
        return;
    }

    size_t pivot_idx = partition(arr, start, end);
    size_t left_len = pivot_idx - start;

    // Only spawn an OS thread if we haven't hit the depth limit AND the chunk is massively heavy
    if (depth < MAX_OS_THREAD_DEPTH && left_len > OS_SPAWN_THRESHOLD) {
        
        // Pass arguments on the stack
        os_qs_args left_args = {arr, start, pivot_idx, depth + 1};

        os_thread_t left_thread;
#if defined(_WIN32)
        left_thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)raw_thread_worker, &left_args, 0, NULL);
#else
        pthread_create(&left_thread, NULL, raw_thread_worker, &left_args);
#endif
        
        // Sort the right side locally on the current thread
        raw_thread_quicksort(arr, pivot_idx + 1, end, depth + 1);

        // Wait for the left thread to finish before the stack variable goes out of scope
#if defined(_WIN32)
        WaitForSingleObject(left_thread, INFINITE);
        CloseHandle(left_thread);
#else
        pthread_join(left_thread, NULL);
#endif
    } else {
        // Fallback to sequential execution on the current thread
        if (left_len > 0) {
            raw_thread_quicksort(arr, start, pivot_idx, depth + 1);
        }
        size_t right_len = end - (pivot_idx + 1);
        if (right_len > 0) {
            raw_thread_quicksort(arr, pivot_idx + 1, end, depth + 1);
        }
    }
}

// --- 3. sk-scheduler Lock-Free Implementation ---
void no_op_qs_continuation(void* user_data, size_t start_idx, size_t end_idx) {
    // Quick sort partitions in place, so the continuation does nothing
    (void)user_data; (void)start_idx; (void)end_idx;
}

void sk_quicksort_task(void* user_data, size_t start_idx, size_t end_idx) {
    int* arr = (int*)user_data;
    size_t length = end_idx - start_idx;

    if (length <= BASE_CASE_THRESHOLD) {
        insertion_sort(arr, start_idx, end_idx);
        return;
    }

    size_t pivot_idx = partition(arr, start_idx, end_idx);

    // Task must wait for children to resolve before dropping the virtual lock
    scheduler_set_continuation(no_op_qs_continuation);

    // Dynamically set chunk_size to exactly match the partition lengths
    // This perfectly isolates each partition into exactly 1 lock-free task
    size_t left_len = pivot_idx - start_idx;
    if (left_len > 0) {
        scheduler_spawn_subtasks(user_data, start_idx, pivot_idx, left_len, sk_quicksort_task);
    }

    size_t right_len = end_idx - (pivot_idx + 1);
    if (right_len > 0) {
        scheduler_spawn_subtasks(user_data, pivot_idx + 1, end_idx, right_len, sk_quicksort_task);
    }
}

// --- 4. Main Execution ---
int main(void) {
    // 1. Allocate arrays + baseline array to reset data every loop
    int* baseline_array = (int*)malloc(ARRAY_SIZE * sizeof(int));
    int* raw_array = (int*)malloc(ARRAY_SIZE * sizeof(int));
    int* sk_array = (int*)malloc(ARRAY_SIZE * sizeof(int));

    if (!baseline_array || !raw_array || !sk_array) {
        fprintf(stderr, "Fatal: Memory allocation failed.\n");
        return 1;
    }

    // 2. Seed the baseline array once
    srand((unsigned int)time(NULL));
    for (size_t i = 0; i < ARRAY_SIZE; i++) {
        baseline_array[i] = rand() % 1000000;
    }

    // Metrics Tracking
    long long raw_min_us = INT64_MAX, raw_max_us = 0, raw_sum_us = 0;
    long long sk_min_us = INT64_MAX, sk_max_us = 0, sk_sum_us = 0;
    bool all_valid = true;

    printf("=== Quick Sort Benchmark (%d elements, %d runs) ===\n\n", ARRAY_SIZE, NUM_RUNS);
    
    // Boot the scheduler once before the benchmark loop
    scheduler_boot(1024, 4096);

    for (int run = 0; run < NUM_RUNS; run++) {
        // CLI Progress indicator
        printf("\rExecuting run %d of %d...", run + 1, NUM_RUNS);
        fflush(stdout);

        // Reset arrays to identical unsorted states
        memcpy(raw_array, baseline_array, ARRAY_SIZE * sizeof(int));
        memcpy(sk_array, baseline_array, ARRAY_SIZE * sizeof(int));

        double start_time, end_time;

        // ---------------------------------------------------------
        // TEST A: Raw OS Thread Pool Limit
        // ---------------------------------------------------------
        start_time = get_time_seconds();
        raw_thread_quicksort(raw_array, 0, ARRAY_SIZE, 0);
        end_time = get_time_seconds();
        
        long long raw_elapsed = (long long)((end_time - start_time) * 1000000.0);
        if (raw_elapsed < raw_min_us) raw_min_us = raw_elapsed;
        if (raw_elapsed > raw_max_us) raw_max_us = raw_elapsed;
        raw_sum_us += raw_elapsed;

        // ---------------------------------------------------------
        // TEST B: sk-scheduler Lock-Free Tasks
        // ---------------------------------------------------------
        sc_job sort_job;
        scheduler_init_job(&sort_job, sk_array, ARRAY_SIZE, sk_quicksort_task);

        start_time = get_time_seconds();
        if (!scheduler_submit_job(&sort_job, ARRAY_SIZE)) {
            fprintf(stderr, "\nHardware backpressure: Job rejected on run %d.\n", run + 1);
            all_valid = false;
            break;
        }
        scheduler_wait_for_job(&sort_job);
        end_time = get_time_seconds();

        long long sk_elapsed = (long long)((end_time - start_time) * 1000000.0);
        if (sk_elapsed < sk_min_us) sk_min_us = sk_elapsed;
        if (sk_elapsed > sk_max_us) sk_max_us = sk_elapsed;
        sk_sum_us += sk_elapsed;

        // ---------------------------------------------------------
        // Validation per run
        // ---------------------------------------------------------
        for (size_t i = 0; i < ARRAY_SIZE - 1; i++) {
            if (sk_array[i] > sk_array[i + 1] || raw_array[i] > raw_array[i + 1]) {
                all_valid = false;
                break;
            }
        }

        if (!all_valid) {
            printf("\n\nVALIDATION FAILED on run %d!\n", run + 1);
            break;
        }
    }

    // Stop workers after all runs complete
    scheduler_stop_workers();
    printf("\n\n");

    // ---------------------------------------------------------
    // Reporting
    // ---------------------------------------------------------
    if (all_valid) {
        float raw_avg_ms = (raw_sum_us / (float)NUM_RUNS) / 1000.0f;
        float sk_avg_ms = (sk_sum_us / (float)NUM_RUNS) / 1000.0f;
        float avg_speedup = raw_avg_ms / sk_avg_ms;

        printf("=== Final Execution Statistics (%d Runs) ===\n", NUM_RUNS);
        printf("Validation         : SUCCESS\n\n");
        
        printf("--- OS Threads (Baseline) ---\n");
        printf("Average Time       : %.2f ms\n", raw_avg_ms);
        printf("Fastest Run        : %.2f ms\n", raw_min_us / 1000.0f);
        printf("Slowest Run        : %.2f ms\n\n", raw_max_us / 1000.0f);

        printf("--- sk-scheduler Lock-Free ---\n");
        printf("Average Time       : %.2f ms\n", sk_avg_ms);
        printf("Fastest Run        : %.2f ms\n", sk_min_us / 1000.0f);
        printf("Slowest Run        : %.2f ms\n\n", sk_max_us / 1000.0f);
        
        printf("--- Net Result ---\n");
        printf("Average Speedup    : %.2fx Faster\n", avg_speedup);
    }

    free(baseline_array);
    free(raw_array);
    free(sk_array);
    return 0;
}