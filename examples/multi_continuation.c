#define SK_SCHEDULER_IMPLEMENTATION
#include "sk_scheduler.h"

#define TOTAL_ELEMENTS 16

//the continuation of the continuation of the child task
void child_continuation_2(void* user_data, size_t start_idx, size_t end_idx){
    (void)user_data;
    printf("            [Child continuation 2] executed from index %2zu to %2zu\n", start_idx, end_idx);
}

//the continuation of the child task
void child_continuation(void* user_data, size_t start_idx, size_t end_idx){
    (void)user_data;
    printf("        [Child continuation] executed from index %2zu to %2zu\n", start_idx, end_idx);
    scheduler_set_continuation(child_continuation_2);
}

//the sub-task
void child_worker_task(void* user_data, size_t start_idx, size_t end_idx) {
    (void)user_data;
    printf("  [Child] Processing chunk from index %2zu to %2zu\n", start_idx, end_idx);
    scheduler_set_continuation(child_continuation);
}

//the continuation task
void final_merge_task(void* user_data, size_t start_idx, size_t end_idx) {
    (void)user_data;
    printf("[Parent continuation] All child tasks completed. Finalizing range %zu to %zu.\n", start_idx, end_idx);
}


void parent_spawner_task(void* user_data, size_t start_idx, size_t end_idx) {
    printf("[Parent] Spawning manual sub-tasks...\n");

    // 1: Tell the engine what this task should turn into after its children finish
    scheduler_set_continuation(final_merge_task);

    // 2: Spawn the children. 
    // by giving a chunk size of 1/4 of the total elements, 4 tasks will be spawned
    scheduler_spawn_subtasks(user_data, start_idx, end_idx, TOTAL_ELEMENTS/4, child_worker_task);
}


int main(){
    scheduler_boot(1024,1024);

    sc_job manual_job;

    scheduler_init_job(&manual_job, NULL, TOTAL_ELEMENTS, parent_spawner_task);

    // CRITICAL API USAGE:
    // By passing TOTAL_ELEMENTS as the chunk size, we force the internal `internal_macro_job_splitter` 
    // to instantly hand control over to our `parent_spawner_task` without splitting it automatically.
    if (!scheduler_submit_job(&manual_job, TOTAL_ELEMENTS)) {
        fprintf(stderr, "Failed to submit job.\n");
        scheduler_stop_workers();
        return 1;
    }

    scheduler_wait_for_job(&manual_job);

    printf("\nDAG Successfully Resolved and Job Completed!\n");

    scheduler_stop_workers();
    return 0;
}