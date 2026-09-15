#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
    #define _POSIX_C_SOURCE 199309L
#endif

#include <stdio.h>
#include <stdlib.h>
#include "sk_scheduler.h"

// Scaled down to prevent flooding the terminal, 
// generating exactly 4 chunks per system.
#define ENTITY_COUNT 16000
#define CHUNK_SIZE 4000

typedef struct { 
    float* values; 
} sys_payload;

void physics_system_task(void* data, size_t start, size_t end) {
    printf("  [Physics]  Processing entities %5zu to %5zu\n", start, end);
    sys_payload* p = (sys_payload*)data;
    for(size_t i = start; i < end; i++) {
        p->values[i] += 9.81f; 
    }
}

void ai_system_task(void* data, size_t start, size_t end) {
    printf("  [AI]       Processing entities %5zu to %5zu\n", start, end);
    sys_payload* p = (sys_payload*)data;
    for(size_t i = start; i < end; i++) {
        p->values[i] *= 1.05f; 
    }
}

void particle_system_task(void* data, size_t start, size_t end) {
    printf("  [Particle] Processing entities %5zu to %5zu\n", start, end);
    sys_payload* p = (sys_payload*)data;
    for(size_t i = start; i < end; i++) {
        p->values[i] -= 0.1f; 
    }
}

int main(void) {
    printf("=== Sk-Scheduler: Concurrent Systems Tick ===\n\n");
    
    // Boot with a large enough queue and deque to handle multiple jobs being ingested at once
    scheduler_boot(2048,4096);

    sys_payload phys_data = { (float*)calloc(ENTITY_COUNT, sizeof(float)) };
    sys_payload ai_data   = { (float*)calloc(ENTITY_COUNT, sizeof(float)) };
    sys_payload part_data = { (float*)calloc(ENTITY_COUNT, sizeof(float)) };

    if (!phys_data.values || !ai_data.values || !part_data.values) {
        fprintf(stderr, "Fatal: Memory allocation failed.\n");
        return 1;
    }

    sc_job phys_job, ai_job, part_job;
    scheduler_init_job(&phys_job, &phys_data, ENTITY_COUNT, physics_system_task);
    scheduler_init_job(&ai_job, &ai_data, ENTITY_COUNT, ai_system_task);
    scheduler_init_job(&part_job, &part_data, ENTITY_COUNT, particle_system_task);

    printf("Submitting Physics, AI, and Particle systems simultaneously...\n\n");
    
    scheduler_submit_job(&phys_job, CHUNK_SIZE);
    scheduler_submit_job(&ai_job, CHUNK_SIZE);
    scheduler_submit_job(&part_job, CHUNK_SIZE);

    scheduler_wait_for_job(&phys_job);
    scheduler_wait_for_job(&ai_job);
    scheduler_wait_for_job(&part_job);

    printf("\nAll concurrent systems resolved. Frame ready for render!\n");

    free(phys_data.values);
    free(ai_data.values);
    free(part_data.values);
    
    scheduler_stop_workers();
    return 0;
}