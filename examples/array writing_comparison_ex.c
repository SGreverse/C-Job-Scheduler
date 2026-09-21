#define SK_SCHEDULER_IMPLEMENTATION
#include "sk_scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>

#define ARRAY_SIZE 10000000
#define NUM_ITERATIONS 100

typedef struct {
    float* data_array;
    float multiplier;
} math_payload;

// The Lock-Free Execution Function
void process_array_chunk(void* user_data, size_t start_idx, size_t end_idx) {
    math_payload* payload = (math_payload*)user_data;
    
    // Process the contiguous memory chunk utilizing L1 spatial locality
    for(size_t i = start_idx; i < end_idx; i++){
        payload->data_array[i] = (float)i * payload->multiplier;
    }
}

int main(void) {
    printf("=== Array Processing Benchmark (%d elements, %d iterations) ===\n\n", ARRAY_SIZE, NUM_ITERATIONS);

    // 1. Allocate payloads for both sequential and concurrent tests
    math_payload seq_payload, conc_payload;
    seq_payload.data_array = (float*)malloc(ARRAY_SIZE * sizeof(float));
    seq_payload.multiplier = 1.05f;
    
    conc_payload.data_array = (float*)malloc(ARRAY_SIZE * sizeof(float));
    conc_payload.multiplier = 1.05f;

    if (!seq_payload.data_array || !conc_payload.data_array) {
        fprintf(stderr, "Fatal: Memory allocation failed.\n");
        return 1;
    }

    long long seq_total_us = 0, seq_min_us = INT64_MAX, seq_max_us = 0;
    long long conc_total_us = 0, conc_min_us = INT64_MAX, conc_max_us = 0;
    struct timespec start, end;

    // --- TEST 1: Sequential Execution ---
    printf("[1/2] Running standard sequential loop...\n");
    for (int i = 0; i < NUM_ITERATIONS; i++) {
        clock_gettime(CLOCK_MONOTONIC, &start);
        
        process_array_chunk(&seq_payload, 0, ARRAY_SIZE);
        
        clock_gettime(CLOCK_MONOTONIC, &end);
        long long elapsed_us = (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_nsec - start.tv_nsec) / 1000LL;
        
        seq_total_us += elapsed_us;
        if (elapsed_us < seq_min_us) seq_min_us = elapsed_us;
        if (elapsed_us > seq_max_us) seq_max_us = elapsed_us;
    }

    // --- TEST 2: Lock-Free Concurrent Execution ---
    printf("[2/2] Running lock-free scheduled job...\n");
    
    scheduler_boot(1024,4096);

    for (int i = 0; i < NUM_ITERATIONS; i++) {
        sc_job math_job;
        scheduler_init_job(&math_job, &conc_payload, ARRAY_SIZE, process_array_chunk);

        clock_gettime(CLOCK_MONOTONIC, &start);

        // Push the Job across the SPSC boundary, requesting L1-friendly chunks of 1024
        if (!scheduler_submit_job(&math_job, 1000000)) {
            fprintf(stderr, "Hardware backpressure: SPSC ingestion rejected the job at iteration %d.\n", i);
            scheduler_stop_workers();
            return 1;
        }

        scheduler_wait_for_job(&math_job);

        clock_gettime(CLOCK_MONOTONIC, &end);
        long long elapsed_us = (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_nsec - start.tv_nsec) / 1000LL;
        
        conc_total_us += elapsed_us;
        if (elapsed_us < conc_min_us) conc_min_us = elapsed_us;
        if (elapsed_us > conc_max_us) conc_max_us = elapsed_us;
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
        long long seq_avg_us = seq_total_us / NUM_ITERATIONS;
        long long conc_avg_us = conc_total_us / NUM_ITERATIONS;
        float speedup = (float)seq_avg_us / (float)conc_avg_us;

        printf("\n=== Execution Statistics (%d Iterations) ===\n", NUM_ITERATIONS);
        printf("Validation       : SUCCESS\n");
        printf("Sequential Avg   : %lld us (Min: %lld, Max: %lld)\n", seq_avg_us, seq_min_us, seq_max_us);
        printf("Concurrent Avg   : %lld us (Min: %lld, Max: %lld)\n", conc_avg_us, conc_min_us, conc_max_us);
        printf("Speedup          : %.2fx Faster\n", speedup);
    }

    free(seq_payload.data_array);
    free(conc_payload.data_array);

    return 0;
}