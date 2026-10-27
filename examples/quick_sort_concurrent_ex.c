#define _POSIX_C_SOURCE 199309L
#define SK_SCHEDULER_IMPLEMENTATION
#include "sk_scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define ARRAY_SIZE 1000000
#define BASE_CASE_THRESHOLD 1024
#define NUM_RUNS 100

// Limit raw OS threads to 16 (depth 4) to prevent virtual memory exhaustion
#define MAX_OS_THREAD_DEPTH 4
#define OS_SPAWN_THRESHOLD 1000

#if defined(_WIN32)
    #include <windows.h>
    typedef HANDLE os_thread_t;
#else
    #include <pthread.h>
    typedef pthread_t os_thread_t;
#endif

static inline long long get_time_us(void) {
#if defined(_WIN32)
    LARGE_INTEGER freq, time;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&time);
    return (long long)((time.QuadPart * 1000000LL) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + (long long)ts.tv_nsec / 1000LL;
#endif
}

static int compare_ll(const void* a, const void* b) {
    long long arg1 = *(const long long*)a;
    long long arg2 = *(const long long*)b;
    return (arg1 > arg2) - (arg1 < arg2);
}

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

    if (depth < MAX_OS_THREAD_DEPTH && left_len > OS_SPAWN_THRESHOLD) {
        os_qs_args left_args = {arr, start, pivot_idx, depth + 1};
        os_thread_t left_thread;

#if defined(_WIN32)
        left_thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)raw_thread_worker, &left_args, 0, NULL);
#else
        pthread_create(&left_thread, NULL, raw_thread_worker, &left_args);
#endif
        
        size_t right_len = end - (pivot_idx + 1);
        if (right_len > 0) {
            raw_thread_quicksort(arr, pivot_idx + 1, end, depth + 1);
        }

#if defined(_WIN32)
        WaitForSingleObject(left_thread, INFINITE);
        CloseHandle(left_thread);
#else
        pthread_join(left_thread, NULL);
#endif
    } else {
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
void sk_quicksort_task(void* user_data, size_t start_idx, size_t end_idx) {
    int* arr = (int*)user_data;
    size_t length = end_idx - start_idx;

    if (length <= BASE_CASE_THRESHOLD) {
        insertion_sort(arr, start_idx, end_idx);
        return;
    }

    size_t pivot_idx = partition(arr, start_idx, end_idx);

    // Note: No continuation needed! In-place partition completes before children run,
    // and the DAG automatically cascades completion upward once both children finish.
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
    int* baseline_array = (int*)malloc(ARRAY_SIZE * sizeof(int));
    int* raw_array = (int*)malloc(ARRAY_SIZE * sizeof(int));
    int* sk_array = (int*)malloc(ARRAY_SIZE * sizeof(int));
    long long* raw_times = (long long*)malloc(NUM_RUNS * sizeof(long long));
    long long* sk_times = (long long*)malloc(NUM_RUNS * sizeof(long long));

    if (!baseline_array || !raw_array || !sk_array || !raw_times || !sk_times) {
        fprintf(stderr, "Fatal: Memory allocation failed.\n");
        return 1;
    }

    srand((unsigned int)time(NULL));
    for (size_t i = 0; i < ARRAY_SIZE; i++) {
        baseline_array[i] = rand() % 1000000;
    }

    long long raw_sum_us = 0, sk_sum_us = 0;
    bool all_valid = true;

    printf("=== Isolated Quick Sort Benchmark (%d elements, %d runs) ===\n\n", ARRAY_SIZE, NUM_RUNS);

    // ---------------------------------------------------------
    // PHASE 1: Raw OS Threads Baseline (Before booting workers)
    // ---------------------------------------------------------
    printf("[1/2] Benchmarking Raw OS Threads Baseline...\n");
    for (int w = 0; w < 3; w++) {
        memcpy(raw_array, baseline_array, ARRAY_SIZE * sizeof(int));
        raw_thread_quicksort(raw_array, 0, ARRAY_SIZE, 0);
    }

    for (int run = 0; run < NUM_RUNS; run++) {
        memcpy(raw_array, baseline_array, ARRAY_SIZE * sizeof(int));

        long long start = get_time_us();
        raw_thread_quicksort(raw_array, 0, ARRAY_SIZE, 0);
        long long elapsed = get_time_us() - start;

        raw_times[run] = elapsed;
        raw_sum_us += elapsed;
    }

    // ---------------------------------------------------------
    // PHASE 2: sk-scheduler Lock-Free Tasks
    // ---------------------------------------------------------
    printf("[2/2] Benchmarking Concurrent (sk-scheduler)...\n");
    scheduler_boot(1024, 4096);

    for (int w = 0; w < 5; w++) {
        memcpy(sk_array, baseline_array, ARRAY_SIZE * sizeof(int));
        sc_job warmup_job;
        scheduler_init_job(&warmup_job, sk_array, ARRAY_SIZE, sk_quicksort_task);
        scheduler_submit_job(&warmup_job, ARRAY_SIZE);
        scheduler_wait_for_job(&warmup_job);
    }

    for (int run = 0; run < NUM_RUNS; run++) {
        memcpy(sk_array, baseline_array, ARRAY_SIZE * sizeof(int));

        sc_job sort_job;
        scheduler_init_job(&sort_job, sk_array, ARRAY_SIZE, sk_quicksort_task);

        long long start = get_time_us();
        if (!scheduler_submit_job(&sort_job, ARRAY_SIZE)) {
            fprintf(stderr, "\nHardware backpressure: Job rejected on run %d.\n", run + 1);
            all_valid = false;
            break;
        }
        scheduler_wait_for_job(&sort_job);
        long long elapsed = get_time_us() - start;

        sk_times[run] = elapsed;
        sk_sum_us += elapsed;

        for (size_t i = 0; i < ARRAY_SIZE - 1; i++) {
            if (sk_array[i] > sk_array[i + 1]) {
                all_valid = false;
                break;
            }
        }
        if (!all_valid) {
            printf("\nVALIDATION FAILED on run %d!\n", run + 1);
            break;
        }
    }

    scheduler_stop_workers();

    if (all_valid) {
        qsort(raw_times, NUM_RUNS, sizeof(long long), compare_ll);
        qsort(sk_times, NUM_RUNS, sizeof(long long), compare_ll);

        float raw_p50 = raw_times[NUM_RUNS / 2] / 1000.0f;
        float sk_p50 = sk_times[NUM_RUNS / 2] / 1000.0f;

        printf("\n=== Final Execution Statistics (%d Runs) ===\n", NUM_RUNS);
        printf("Validation         : SUCCESS\n\n");
        
        printf("--- OS Threads (Baseline) ---\n");
        printf("Median (P50)       : %.2f ms\n", raw_p50);
        printf("P90 / P99          : %.2f ms / %.2f ms\n", raw_times[(int)(NUM_RUNS * 0.90)] / 1000.0f, raw_times[(int)(NUM_RUNS * 0.99)] / 1000.0f);
        printf("Min / Max          : %.2f ms / %.2f ms\n", raw_times[0] / 1000.0f, raw_times[NUM_RUNS - 1] / 1000.0f);
        printf("Mean (Average)     : %.2f ms\n\n", (raw_sum_us / (float)NUM_RUNS) / 1000.0f);

        printf("--- sk-scheduler Lock-Free ---\n");
        printf("Median (P50)       : %.2f ms\n", sk_p50);
        printf("P90 / P99          : %.2f ms / %.2f ms\n", sk_times[(int)(NUM_RUNS * 0.90)] / 1000.0f, sk_times[(int)(NUM_RUNS * 0.99)] / 1000.0f);
        printf("Min / Max          : %.2f ms / %.2f ms\n", sk_times[0] / 1000.0f, sk_times[NUM_RUNS - 1] / 1000.0f);
        printf("Mean (Average)     : %.2f ms\n\n", (sk_sum_us / (float)NUM_RUNS) / 1000.0f);
        
        printf("--- Net Result ---\n");
        printf("Median Speedup     : %.2fx Faster\n", raw_p50 / sk_p50);
    }

    free(raw_times); free(sk_times);
    free(baseline_array); free(raw_array); free(sk_array);
    return 0;
}