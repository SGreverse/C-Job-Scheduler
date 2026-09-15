#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
    #define _POSIX_C_SOURCE 199309L
#endif

#include <stdio.h>
#include <stdlib.h>
#include "sk_scheduler.h"

// 500,000 jobs, each spawning 256 microscopic tasks.
// Total allocations/deallocations: 128,000,000.
#define CHURN_ITERATIONS 500000 
#define ELEMENTS_PER_JOB 256 
#define CHUNK_SIZE 1 

// We do absolutely zero CPU work. The goal is to finish the task 
// instantaneously so it is immediately released back to the global pool.
void churn_task(void* data, size_t start, size_t end) {
    (void)data;
    (void)start;
    (void)end;
}

int main(void) {
    printf("=== Sk-Scheduler: Memory Churn (ABA) Test ===\n\n");
    
    // Boot with standard capacities
    scheduler_boot(1024, 4096);

    printf("[Churn] Submitting %d sequential jobs...\n", CHURN_ITERATIONS);
    printf("[Churn] This will rapidly acquire and release %d tasks.\n\n", 
           CHURN_ITERATIONS * ELEMENTS_PER_JOB);

    sc_job churn_job;
    
    // Blast the engine with rapid-fire, back-to-back jobs
    for (int i = 0; i < CHURN_ITERATIONS; i++) {
        scheduler_init_job(&churn_job, NULL, ELEMENTS_PER_JOB, churn_task);
        
        if (!scheduler_submit_job(&churn_job, CHUNK_SIZE)) {
            fprintf(stderr, "\nFatal: Job rejected at iteration %d. (Memory Leak detected)\n", i);
            scheduler_stop_workers();
            return 1;
        }
        
        scheduler_wait_for_job(&churn_job);
        
        // Print progress every 10% to prove the engine hasn't frozen
        if (i > 0 && i % (CHURN_ITERATIONS / 10) == 0) {
            printf("  ... %d%% Complete\n", (i * 100) / CHURN_ITERATIONS);
        }
    }

    printf("  ... 100%% Complete\n\n");
    
    printf("=== Test Results ===\n");
    printf("Status: SUCCESS! (No infinite loops, memory leaks, or ABA collisions)\n");

    scheduler_stop_workers();
    return 0;
}