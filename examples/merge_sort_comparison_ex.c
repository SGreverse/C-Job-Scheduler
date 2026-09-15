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

// NOTE TO VIEWER OF THIS EXAMPLE:change these defines as you please to see the different results 
#define ARRAY_SIZE 10000000
#define BASE_CASE_THRESHOLD 1024

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
    // 1. Allocate arrays for both tests
    sort_payload seq_payload, conc_payload;
    seq_payload.array = (int*)malloc(ARRAY_SIZE * sizeof(int));
    seq_payload.temp_array = (int*)malloc(ARRAY_SIZE * sizeof(int));
    conc_payload.array = (int*)malloc(ARRAY_SIZE * sizeof(int));
    conc_payload.temp_array = (int*)malloc(ARRAY_SIZE * sizeof(int));

    if (!seq_payload.array || !conc_payload.array) {
        fprintf(stderr, "Fatal: Memory allocation failed.\n");
        return 1;
    }

    // 2. Seed the arrays identically
    srand(time(NULL));
    for (size_t i = 0; i < ARRAY_SIZE; i++) {
        int val = rand() % 1000000;
        seq_payload.array[i] = val;
        conc_payload.array[i] = val;
    }

    printf("=== Starting Sort Benchmark (%d elements) ===\n\n", ARRAY_SIZE);
    struct timespec start, end;

    // ---------------------------------------------------------
    // TEST A: Standard Sequential Sort
    // ---------------------------------------------------------
    printf("[1/2] Running standard sequential merge sort...\n");
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    sequential_merge_sort(seq_payload.array, seq_payload.temp_array, 0, ARRAY_SIZE);
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    long long seq_elapsed_us = (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_nsec - start.tv_nsec) / 1000LL;

    // ---------------------------------------------------------
    // TEST B: Lock-Free Concurrent Sort
    // ---------------------------------------------------------
    printf("[2/2] Running lock-free concurrent merge sort...\n");
    scheduler_boot(1024,4096);
    
    sc_job sort_job;
    scheduler_init_job(&sort_job, &conc_payload, ARRAY_SIZE, concurrent_merge_sort);

    clock_gettime(CLOCK_MONOTONIC, &start);
    
    if (!scheduler_submit_job(&sort_job, ARRAY_SIZE)) {
        fprintf(stderr, "Hardware backpressure: Job rejected.\n");
        return 1;
    }

    scheduler_wait_for_job(&sort_job);

    clock_gettime(CLOCK_MONOTONIC, &end);
    long long conc_elapsed_us = (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_nsec - start.tv_nsec) / 1000LL;
    scheduler_stop_workers();

    // ---------------------------------------------------------
    // Validation & Reporting
    // ---------------------------------------------------------
    bool is_sorted = true;
    for (size_t i = 0; i < ARRAY_SIZE - 1; i++) {
        if (conc_payload.array[i] > conc_payload.array[i + 1] || seq_payload.array[i] > seq_payload.array[i + 1]) {
            is_sorted = false;
            break;
        }
    }

    if (is_sorted) {
        float speedup = (float)seq_elapsed_us / (float)conc_elapsed_us;
        printf("\n=== Execution Statistics ===\n");
        printf("Validation       : SUCCESS\n");
        printf("Sequential Time  : %.2f ms\n", seq_elapsed_us / 1000.0f);
        printf("Concurrent Time  : %.2f ms\n", conc_elapsed_us / 1000.0f);
        printf("Speedup Multiplier: %.2fx Faster\n", speedup);
    } else {
        printf("\nVALIDATION FAILED!\n");
    }

    free(seq_payload.array); free(seq_payload.temp_array);
    free(conc_payload.array); free(conc_payload.temp_array);
    return 0;
}