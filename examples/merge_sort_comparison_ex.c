#define _POSIX_C_SOURCE 199309L

#define SK_SCHEDULER_IMPLEMENTATION
#include "sk_scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>


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

static int compare_ll(const void* a, const void* b) {
    long long arg1 = *(const long long*)a;
    long long arg2 = *(const long long*)b;
    return (arg1 > arg2) - (arg1 < arg2);
}

int main(void) {
    int* baseline_array = (int*)malloc(ARRAY_SIZE * sizeof(int));
    sort_payload seq_payload, conc_payload;
    seq_payload.array = (int*)malloc(ARRAY_SIZE * sizeof(int));
    seq_payload.temp_array = (int*)malloc(ARRAY_SIZE * sizeof(int));
    conc_payload.array = (int*)malloc(ARRAY_SIZE * sizeof(int));
    conc_payload.temp_array = (int*)malloc(ARRAY_SIZE * sizeof(int));

    long long* seq_times = (long long*)malloc(NUM_RUNS * sizeof(long long));
    long long* conc_times = (long long*)malloc(NUM_RUNS * sizeof(long long));

    if (!seq_payload.array || !conc_payload.array || !baseline_array || !seq_times || !conc_times) {
        fprintf(stderr, "Fatal: Memory allocation failed.\n");
        return 1;
    }

    srand((unsigned int)time(NULL));
    for (size_t i = 0; i < ARRAY_SIZE; i++) {
        baseline_array[i] = rand() % 1000000;
    }

    long long seq_sum_us = 0, conc_sum_us = 0;
    bool all_valid = true;
    struct timespec start, end;

    printf("=== Starting Isolated Sort Benchmark (%d elements, %d runs) ===\n\n", ARRAY_SIZE, NUM_RUNS);

    // ---------------------------------------------------------
    // PHASE 1: Sequential Baseline (Before booting spinning workers!)
    // ---------------------------------------------------------
    printf("[1/2] Benchmarking Sequential Baseline...\n");
    // 5 Warmup runs to fault in pages and stabilize CPU turbo clock
    for (int w = 0; w < 5; w++) {
        memcpy(seq_payload.array, baseline_array, ARRAY_SIZE * sizeof(int));
        sequential_merge_sort(seq_payload.array, seq_payload.temp_array, 0, ARRAY_SIZE);
    }

    for (int run = 0; run < NUM_RUNS; run++) {
        memcpy(seq_payload.array, baseline_array, ARRAY_SIZE * sizeof(int));

        clock_gettime(CLOCK_MONOTONIC, &start);
        sequential_merge_sort(seq_payload.array, seq_payload.temp_array, 0, ARRAY_SIZE);
        clock_gettime(CLOCK_MONOTONIC, &end);

        long long elapsed = (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_nsec - start.tv_nsec) / 1000LL;
        seq_times[run] = elapsed;
        seq_sum_us += elapsed;
    }

    // ---------------------------------------------------------
    // PHASE 2: Concurrent sk-scheduler
    // ---------------------------------------------------------
    printf("[2/2] Benchmarking Concurrent (sk-scheduler)...\n");
    scheduler_boot(1024, 4096);

    // 5 Warmup runs to warm up worker thread caches and task pools
    for (int w = 0; w < 5; w++) {
        memcpy(conc_payload.array, baseline_array, ARRAY_SIZE * sizeof(int));
        sc_job warmup_job;
        scheduler_init_job(&warmup_job, &conc_payload, ARRAY_SIZE, concurrent_merge_sort);
        scheduler_submit_job(&warmup_job, ARRAY_SIZE);
        scheduler_wait_for_job(&warmup_job);
    }

    for (int run = 0; run < NUM_RUNS; run++) {
        memcpy(conc_payload.array, baseline_array, ARRAY_SIZE * sizeof(int));

        sc_job sort_job;
        scheduler_init_job(&sort_job, &conc_payload, ARRAY_SIZE, concurrent_merge_sort);

        clock_gettime(CLOCK_MONOTONIC, &start);
        if (!scheduler_submit_job(&sort_job, ARRAY_SIZE)) {
            fprintf(stderr, "\nHardware backpressure: Job rejected on run %d.\n", run + 1);
            scheduler_stop_workers();
            return 1;
        }
        scheduler_wait_for_job(&sort_job);
        clock_gettime(CLOCK_MONOTONIC, &end);

        long long elapsed = (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_nsec - start.tv_nsec) / 1000LL;
        conc_times[run] = elapsed;
        conc_sum_us += elapsed;

        for (size_t i = 0; i < ARRAY_SIZE - 1; i++) {
            if (conc_payload.array[i] > conc_payload.array[i + 1]) {
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
        qsort(seq_times, NUM_RUNS, sizeof(long long), compare_ll);
        qsort(conc_times, NUM_RUNS, sizeof(long long), compare_ll);

        float seq_p50 = seq_times[NUM_RUNS / 2] / 1000.0f;
        float conc_p50 = conc_times[NUM_RUNS / 2] / 1000.0f;

        printf("\n=== Final Execution Statistics (%d Runs) ===\n", NUM_RUNS);
        printf("Validation       : SUCCESS\n\n");

        printf("--- Sequential ---\n");
        printf("Median (P50)     : %.2f ms\n", seq_p50);
        printf("P90 / P99        : %.2f ms / %.2f ms\n", seq_times[(int)(NUM_RUNS * 0.90)] / 1000.0f, seq_times[(int)(NUM_RUNS * 0.99)] / 1000.0f);
        printf("Min / Max        : %.2f ms / %.2f ms\n", seq_times[0] / 1000.0f, seq_times[NUM_RUNS - 1] / 1000.0f);
        printf("Mean (Average)   : %.2f ms\n\n", (seq_sum_us / (float)NUM_RUNS) / 1000.0f);

        printf("--- Concurrent (sk-scheduler) ---\n");
        printf("Median (P50)     : %.2f ms\n", conc_p50);
        printf("P90 / P99        : %.2f ms / %.2f ms\n", conc_times[(int)(NUM_RUNS * 0.90)] / 1000.0f, conc_times[(int)(NUM_RUNS * 0.99)] / 1000.0f);
        printf("Min / Max        : %.2f ms / %.2f ms\n", conc_times[0] / 1000.0f, conc_times[NUM_RUNS - 1] / 1000.0f);
        printf("Mean (Average)   : %.2f ms\n\n", (conc_sum_us / (float)NUM_RUNS) / 1000.0f);

        printf("--- Net Result ---\n");
        printf("Median Speedup   : %.2fx Faster\n", seq_p50 / conc_p50);
    }

    free(seq_times); free(conc_times);
    free(baseline_array);
    free(seq_payload.array); free(seq_payload.temp_array);
    free(conc_payload.array); free(conc_payload.temp_array);
    return 0;
}