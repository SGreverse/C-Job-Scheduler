#define _POSIX_C_SOURCE 199309L
#define SK_SCHEDULER_IMPLEMENTATION
#include "sk_scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>

#define ARRAY_SIZE 10000000
#define CHUNK_SIZE 65536
#define NUM_ITERATIONS 100

typedef struct {
    float* data_array;
    float multiplier;
} math_payload;

static int compare_ll(const void* a, const void* b) {
    long long arg1 = *(const long long*)a;
    long long arg2 = *(const long long*)b;
    return (arg1 > arg2) - (arg1 < arg2);
}

void process_array_chunk(void* user_data, size_t start_idx, size_t end_idx) {
    math_payload* payload = (math_payload*)user_data;
    float* restrict arr = payload->data_array;
    float mult = payload->multiplier;
    
    for (size_t i = start_idx; i < end_idx; i++) {
        arr[i] = (float)i * mult;
    }
}

int main(void) {
    printf("=== Isolated Array Processing Benchmark (%d elements, %d iterations) ===\n\n", ARRAY_SIZE, NUM_ITERATIONS);

    math_payload seq_payload, conc_payload;
    seq_payload.data_array = (float*)malloc(ARRAY_SIZE * sizeof(float));
    seq_payload.multiplier = 1.05f;
    
    conc_payload.data_array = (float*)malloc(ARRAY_SIZE * sizeof(float));
    conc_payload.multiplier = 1.05f;

    long long* seq_times = (long long*)malloc(NUM_ITERATIONS * sizeof(long long));
    long long* conc_times = (long long*)malloc(NUM_ITERATIONS * sizeof(long long));

    if (!seq_payload.data_array || !conc_payload.data_array || !seq_times || !conc_times) {
        fprintf(stderr, "Fatal: Memory allocation failed.\n");
        return 1;
    }

    long long seq_total_us = 0, conc_total_us = 0;
    struct timespec start, end;

    // --- TEST 1: Sequential Execution ---
    printf("[1/2] Benchmarking Sequential Baseline...\n");
    // Warmup to fault in OS virtual memory pages before timing
    for (int w = 0; w < 5; w++) {
        process_array_chunk(&seq_payload, 0, ARRAY_SIZE);
    }

    for (int i = 0; i < NUM_ITERATIONS; i++) {
        clock_gettime(CLOCK_MONOTONIC, &start);
        process_array_chunk(&seq_payload, 0, ARRAY_SIZE);
        clock_gettime(CLOCK_MONOTONIC, &end);

        long long elapsed_us = (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_nsec - start.tv_nsec) / 1000LL;
        seq_times[i] = elapsed_us;
        seq_total_us += elapsed_us;
    }

    // --- TEST 2: Lock-Free Concurrent Execution ---
    printf("[2/2] Benchmarking Concurrent (sk-scheduler)...\n");
    scheduler_boot(1024, 4096);

    // Warmup to fault in pages and warm worker caches
    for (int w = 0; w < 5; w++) {
        sc_job warmup_job;
        scheduler_init_job(&warmup_job, &conc_payload, ARRAY_SIZE, process_array_chunk);
        scheduler_submit_job(&warmup_job, CHUNK_SIZE);
        scheduler_wait_for_job(&warmup_job);
    }

    for (int i = 0; i < NUM_ITERATIONS; i++) {
        sc_job math_job;
        scheduler_init_job(&math_job, &conc_payload, ARRAY_SIZE, process_array_chunk);

        clock_gettime(CLOCK_MONOTONIC, &start);
        if (!scheduler_submit_job(&math_job, CHUNK_SIZE)) {
            fprintf(stderr, "Hardware backpressure: SPSC ingestion rejected the job at iteration %d.\n", i);
            scheduler_stop_workers();
            return 1;
        }
        scheduler_wait_for_job(&math_job);
        clock_gettime(CLOCK_MONOTONIC, &end);

        long long elapsed_us = (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_nsec - start.tv_nsec) / 1000LL;
        conc_times[i] = elapsed_us;
        conc_total_us += elapsed_us;
    }
    
    scheduler_stop_workers();

    // --- Validation ---
    bool is_valid = true;
    for (size_t i = 0; i < ARRAY_SIZE; i++) {
        float expected = (float)i * 1.05f;
        if (seq_payload.data_array[i] != expected || conc_payload.data_array[i] != expected) {
            is_valid = false;
            printf("\nValidation Failed at index %zu! Expected: %f\n", i, expected);
            break;
        }
    }

    if (is_valid) {
        qsort(seq_times, NUM_ITERATIONS, sizeof(long long), compare_ll);
        qsort(conc_times, NUM_ITERATIONS, sizeof(long long), compare_ll);

        long long seq_p50 = seq_times[NUM_ITERATIONS / 2];
        long long conc_p50 = conc_times[NUM_ITERATIONS / 2];

        printf("\n=== Execution Statistics (%d Iterations) ===\n", NUM_ITERATIONS);
        printf("Validation       : SUCCESS\n\n");
        printf("--- Sequential ---\n");
        printf("Median (P50)     : %lld us (P90: %lld us, P99: %lld us)\n", seq_p50, seq_times[(int)(NUM_ITERATIONS * 0.90)], seq_times[(int)(NUM_ITERATIONS * 0.99)]);
        printf("Min / Max / Avg  : %lld us / %lld us / %lld us\n\n", seq_times[0], seq_times[NUM_ITERATIONS - 1], seq_total_us / NUM_ITERATIONS);

        printf("--- Concurrent (sk-scheduler) ---\n");
        printf("Median (P50)     : %lld us (P90: %lld us, P99: %lld us)\n", conc_p50, conc_times[(int)(NUM_ITERATIONS * 0.90)], conc_times[(int)(NUM_ITERATIONS * 0.99)]);
        printf("Min / Max / Avg  : %lld us / %lld us / %lld us\n\n", conc_times[0], conc_times[NUM_ITERATIONS - 1], conc_total_us / NUM_ITERATIONS);

        printf("--- Net Result ---\n");
        printf("Median Speedup   : %.2fx Faster\n", (float)seq_p50 / (float)conc_p50);
    }

    free(seq_times); free(conc_times);
    free(seq_payload.data_array);
    free(conc_payload.data_array);
    return 0;
}