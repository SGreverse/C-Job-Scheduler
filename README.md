# sk Job Scheduler

A high-performance, lock-free task scheduling engine for C, designed for maximum concurrency and hardware utilization. 

`sk-scheduler` is built on a Work-Stealing Chase-Lev deque architecture. It allows you to break massive workloads into small tasks and distribute them across all available CPU cores with virtually zero thread-contention. 

(for anyone wondering,the sk part is based on my initials)
## Features

* **Drop-in Integration:** Distributed as a single-header STB-style library. No build systems to configure.
* **True Lock-Free:** Built entirely on C11 atomics, without any use of mutexes or locks.
* **Work-Stealing:** Idle CPU cores steal work from busy cores with extra work to guarantee maximum hardware saturation.
* **DAG Dependencies:** Safely spawn child sub-tasks from within running tasks to build complex execution graphs.
* **POSIX Native:** integrates with Linux and macOS threading affinities and yields.

## Installation

`sk-scheduler` is an STB-style single-header library. 

1. Download `sk_scheduler.h` from the `release/` folder.
2. Drop it into your project folder.
3. In **exactly one** `.c` file, define the implementation macro before including the header:

    ```c
    #define SK_SCHEDULER_IMPLEMENTATION
    #include "sk_scheduler.h"
    ```

In all other files where you need the API, use `#include "sk_scheduler.h"` as normal.

## Quick Start

Here is a minimal example of booting the engine, dispatching a job, and waiting for it to finish.

```c
#include <stdio.h>

#define SK_SCHEDULER_IMPLEMENTATION
#include "sk_scheduler.h"

// 1. Define the function your tasks will execute
void simple_task(void* user_data, size_t start_idx, size_t end_idx) {
    printf("Task executing from index %zu to %zu\n", start_idx, end_idx);
}

int main(void) {
    // 2. Boot the scheduler (SPSC queue size: 1024, Chase-Lev size: 4096)
    scheduler_boot(1024, 4096);
    
    // 3. Initialize a job targeting 10,000 abstract elements
    sc_job my_job;
    scheduler_init_job(&my_job, NULL, 10000, simple_task);
    
    // 4. Submit the job, chunking it into batches of 250 elements
    if (scheduler_submit_job(&my_job, 250)) {
      // 5. Block the main thread until all tasks complete
      scheduler_wait_for_job(&my_job);
      printf("Job successfully completed!\n");
    }
    else {
        printf("Failed to submit job.\n");
    }
    
    // 6. Safely shut down all worker threads and free memory
    scheduler_stop_workers();
    
    return 0;
}
```
for more types of examples, you can check out the examples folder and try running some of the examples yourself

## Current Progress

right now, the scheduler is only in v1. it has been stress tested on my ubuntu machine only but i will try to conduct more tests on different machines/OS and try to find more bugs.

## Documentation
For a deep dive into the DAG architecture, memory pool mechanics, and advanced sub-task spawning, please refer to the <a href='no_docs_yet_be_patient_please'>Full Documentation</a>.

## License

MIT License

Copyright (c) 2026 Segev Kam

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

