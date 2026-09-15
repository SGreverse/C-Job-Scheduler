#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

//the library to use
#include "sk_scheduler.h"

typedef struct {
    const char* message;
} message_payload;

// the execution function (what the workers will do)
void print_hello_chunk(void* user_data, size_t start_idx, size_t end_idx) {
    message_payload* payload = (message_payload*)user_data;
    
    
    printf("Worker executing chunk [%zu to %zu]: %s\n", start_idx, end_idx, payload->message);
}

int main(void) {

    scheduler_boot(1024,4096);

    message_payload my_payload;
    my_payload.message = "Lock-Free Task Executed!";

    sc_job message_job;
    scheduler_init_job(&message_job, &my_payload, 10, print_hello_chunk);


    // chunk size is rounded up to 4
    if (!scheduler_submit_job(&message_job, 3)) {
        fprintf(stderr, "Failed to submit job to the scheduler.\n");
        scheduler_stop_workers();
        return 1;
    }

    //option 1: built in method( implemented for optimization)
    scheduler_wait_for_job(&message_job);

    //option 2: use a while loop with the built in cpu_relax() 
    //this wont work in this example since option 1 will wait until it finishes
    while(!scheduler_is_job_complete(&message_job)){
        cpu_relax();
    }
    printf("\nJob successfully completed!\n");

    scheduler_stop_workers();

    return 0;
}