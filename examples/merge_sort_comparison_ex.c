#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
    #define _POSIX_C_SOURCE 199309L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "sk_scheduler.h"

// NOTE TO VIEWER OF THIS EXAMPLE: change these defines as you please
#define ARRAY_SIZE 100000
#define BASE_CASE_THRESHOLD 1024
#define NUM_RUNS 1000

typedef struct {
    int* array;
    int* temp_array;
} sort_payload;

// --- 1. Common Base Case ---
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

// --- 2. Sequential Merge Sort Implementation ---
void sequential_merge_step(int* arr, int* temp, size_t start, size_t mid, size_t end) {
    size_t i = start, j = mid, k = start;
    
    while (i < mid && j < end) {
        if (arr[i] <= arr[j]) temp[k++] = arr[i++];
        else temp[k++] = arr[j++];
    }
    
    while (i < mid) temp[k++] = arr[i++];
    while (j < end) temp[k++] = arr[j++];

    for (i = start; i < end; i++) {
        arr[i] = temp[i];
    }
}

void sequential_merge_sort(int* arr, int* temp, size_t start, size_t end) {
    size_t length = end - start;
    if (length <= BASE_CASE_THRESHOLD) {
        insertion_sort(arr, start, end);
        return;
    }

    size_t mid = start + (length - (length / 2));

    sequential_merge_sort(arr, temp, start, mid);
    sequential_merge_sort(arr, temp, mid, end);
    sequential_merge_step(arr, temp, start, mid, end);
}

// --- 3. Concurrent Merge Sort Implementation ---
void merge_step_continuation(void* user_data, size_t start_idx, size_t end_idx) {
    sort_payload* payload = (sort_payload*)user_data;
    sequential_merge_step(payload->array, payload->temp_array, start_idx, start_idx + ((end_idx - start_idx) - ((end_idx - start_idx) / 2)), end_idx);
}

void concurrent_merge_sort(void* user_data, size_t start_idx, size_t end_idx) {
    size_t length = end_idx - start_idx;
    sort_payload* payload = (sort_payload*)user_data;

    if (length <= BASE_CASE_THRESHOLD) {
        insertion_sort(payload->array, start_idx, end_idx);
        return;
    }

    scheduler_set_continuation(merge_step_continuation);

    size_t chunk = length - (length / 2);
    scheduler_spawn_subtasks(user_data, start_idx, end_idx, chunk, concurrent_merge_sort);
}

// --- 4. Main Execution ---
int main(void) {
    // 1. Allocate arrays for both tests + a baseline array to reset data every loop
    int* baseline_array = (int*)malloc(ARRAY_SIZE * sizeof(int));
    sort_payload seq_payload, conc_payload;
    seq_payload.array = (int*)malloc(ARRAY_SIZE * sizeof(int));
    seq_payload.temp_array = (int*)malloc(ARRAY_SIZE * sizeof(int));
    conc_payload.array = (int*)malloc(ARRAY_SIZE * sizeof(int));
    conc_payload.temp_array = (int*)malloc(ARRAY_SIZE * sizeof(int));

    if (!seq_payload.array || !conc_payload.array || !baseline_array) {
        fprintf(stderr, "Fatal: Memory allocation failed.\n");
        return 1;
    }

    // 2. Seed the baseline array once
    srand((unsigned int)time(NULL));
    for (size_t i = 0; i < ARRAY_SIZE; i++) {
        baseline_array[i] = rand() % 1000000;
    }

    // Metrics Tracking
    long long seq_min_us = INT64_MAX, seq_max_us = 0, seq_sum_us = 0;
    long long conc_min_us = INT64_MAX, conc_max_us = 0, conc_sum_us = 0;
    bool all_valid = true;

    printf("=== Starting Sort Benchmark (%d elements, %d runs) ===\n\n", ARRAY_SIZE, NUM_RUNS);
    
    // Boot the scheduler once before the benchmark loop
    scheduler_boot(1024, 4096);

    for (int run = 0; run < NUM_RUNS; run++) {
        // CLI Progress indicator (overwrites the same line)
        printf("\rExecuting run %d of %d...", run + 1, NUM_RUNS);
        fflush(stdout);

        // Reset arrays to identical unsorted states
        memcpy(seq_payload.array, baseline_array, ARRAY_SIZE * sizeof(int));
        memcpy(conc_payload.array, baseline_array, ARRAY_SIZE * sizeof(int));

        struct timespec start, end;

        // ---------------------------------------------------------
        // TEST A: Standard Sequential Sort
        // ---------------------------------------------------------
        clock_gettime(CLOCK_MONOTONIC, &start);
        sequential_merge_sort(seq_payload.array, seq_payload.temp_array, 0, ARRAY_SIZE);
        clock_gettime(CLOCK_MONOTONIC, &end);
        
        long long seq_elapsed = (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_nsec - start.tv_nsec) / 1000LL;
        if (seq_elapsed < seq_min_us) seq_min_us = seq_elapsed;
        if (seq_elapsed > seq_max_us) seq_max_us = seq_elapsed;
        seq_sum_us += seq_elapsed;

        // ---------------------------------------------------------
        // TEST B: Lock-Free Concurrent Sort
        // ---------------------------------------------------------
        sc_job sort_job;
        scheduler_init_job(&sort_job, &conc_payload, ARRAY_SIZE, concurrent_merge_sort);

        clock_gettime(CLOCK_MONOTONIC, &start);
        if (!scheduler_submit_job(&sort_job, ARRAY_SIZE)) {
            fprintf(stderr, "\nHardware backpressure: Job rejected on run %d.\n", run + 1);
            return 1;
        }
        scheduler_wait_for_job(&sort_job);
        clock_gettime(CLOCK_MONOTONIC, &end);

        long long conc_elapsed = (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_nsec - start.tv_nsec) / 1000LL;
        if (conc_elapsed < conc_min_us) conc_min_us = conc_elapsed;
        if (conc_elapsed > conc_max_us) conc_max_us = conc_elapsed;
        conc_sum_us += conc_elapsed;

        // ---------------------------------------------------------
        // Validation per run
        // ---------------------------------------------------------
        for (size_t i = 0; i < ARRAY_SIZE - 1; i++) {
            if (conc_payload.array[i] > conc_payload.array[i + 1] || seq_payload.array[i] > seq_payload.array[i + 1]) {
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
        float seq_avg_ms = (seq_sum_us / (float)NUM_RUNS) / 1000.0f;
        float conc_avg_ms = (conc_sum_us / (float)NUM_RUNS) / 1000.0f;
        float avg_speedup = seq_avg_ms / conc_avg_ms;

        printf("=== Final Execution Statistics (%d Runs) ===\n", NUM_RUNS);
        printf("Validation       : SUCCESS\n\n");
        
        printf("--- Sequential ---\n");
        printf("Average Time     : %.2f ms\n", seq_avg_ms);
        printf("Fastest Run      : %.2f ms\n", seq_min_us / 1000.0f);
        printf("Slowest Run      : %.2f ms\n\n", seq_max_us / 1000.0f);

        printf("--- Concurrent (sk-scheduler) ---\n");
        printf("Average Time     : %.2f ms\n", conc_avg_ms);
        printf("Fastest Run      : %.2f ms\n", conc_min_us / 1000.0f);
        printf("Slowest Run      : %.2f ms\n\n", conc_max_us / 1000.0f);
        
        printf("--- Net Result ---\n");
        printf("Average Speedup  : %.2fx Faster\n", avg_speedup);
    }

    free(baseline_array);
    free(seq_payload.array); free(seq_payload.temp_array);
    free(conc_payload.array); free(conc_payload.temp_array);
    return 0;
}