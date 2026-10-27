#define _POSIX_C_SOURCE 199309L
#define SK_SCHEDULER_IMPLEMENTATION
#include "sk_scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>

#if defined(_WIN32)
    #include <windows.h>
    typedef HANDLE os_thread_t;
#else
    #include <pthread.h>
    typedef pthread_t os_thread_t;
#endif

#define IMAGE_WIDTH 1920
#define IMAGE_HEIGHT 1080
#define TOTAL_PIXELS (IMAGE_WIDTH * IMAGE_HEIGHT)
#define MAX_ITERATIONS 1000
#define NUM_RUNS 50

//in this benchmark since i dont want to mess with the scheduler's interface i will hardcode the amount of logical thread my cores can run
//if you checked and its different for your computer(i have 6 physical cores each can run 2 threads simultenousely) you can change this
#define MY_CORE_COUNT 12

// Chunk size of 4 scanlines (4 * 1920 = 7680 pixels) for fine-grained work stealing
#define SCANLINE_CHUNK_PIXELS (IMAGE_WIDTH * 4)

typedef struct {
    uint16_t* output_buffer;
    uint32_t width;
    uint32_t height;
    uint32_t max_iter;
    double x_min, x_max;
    double y_min, y_max;
} mandelbrot_payload;

static int compare_ll(const void* a, const void* b) {
    long long arg1 = *(const long long*)a;
    long long arg2 = *(const long long*)b;
    return (arg1 > arg2) - (arg1 < arg2);
}

// --- 1. Heavy Compute Kernel (Escape-Time Complex Fractal) ---
void render_mandelbrot_chunk(void* user_data, size_t start_idx, size_t end_idx) {
    mandelbrot_payload* payload = (mandelbrot_payload*)user_data;
    uint16_t* restrict out = payload->output_buffer;
    const uint32_t width = payload->width;
    const uint32_t max_iter = payload->max_iter;

    const double dx = (payload->x_max - payload->x_min) / (double)width;
    const double dy = (payload->y_max - payload->y_min) / (double)payload->height;
    const double x_min = payload->x_min;
    const double y_min = payload->y_min;

    for (size_t idx = start_idx; idx < end_idx; idx++) {
        uint32_t px = (uint32_t)(idx % width);
        uint32_t py = (uint32_t)(idx / width);

        double cr = x_min + (double)px * dx;
        double ci = y_min + (double)py * dy;

        double zr = 0.0, zi = 0.0;
        double zr2 = 0.0, zi2 = 0.0;
        uint32_t iter = 0;

        while ((zr2 + zi2 <= 4.0) && (iter < max_iter)) {
            zi = 2.0 * zr * zi + ci;
            zr = zr2 - zi2 + cr;
            zr2 = zr * zr;
            zi2 = zi * zi;
            iter++;
        }

        out[idx] = (uint16_t)iter;
    }
}

// --- 2. Persistent Static Thread Pool (No Work-Stealing) ---
// Each thread is statically assigned 1/N of the image. We use a lock-free
// generation barrier so we don't pay pthread_create overhead during timed runs.
typedef struct {
    size_t start_idx;
    size_t end_idx;
    mandelbrot_payload* payload;
} static_worker_slice;

static atomic_size_t static_generation = 0;
static atomic_size_t static_completed_count = 0;
static atomic_bool static_stop_flag = false;

void* static_thread_loop(void* arg) {
    static_worker_slice* slice = (static_worker_slice*)arg;
    size_t local_gen = 0;

    while (!atomic_load_explicit(&static_stop_flag, memory_order_relaxed)) {
        size_t current_gen = atomic_load_explicit(&static_generation, memory_order_acquire);
        if (current_gen > local_gen) {
            local_gen = current_gen;
            render_mandelbrot_chunk(slice->payload, slice->start_idx, slice->end_idx);
            atomic_fetch_add_explicit(&static_completed_count, 1, memory_order_release);
        } else {
            cpu_relax();
        }
    }
    return NULL;
}

int main(void) {
    size_t num_cores = MY_CORE_COUNT;
    if (num_cores < 1) num_cores = 1;

    printf("=== 3-Way Mandelbrot Work-Stealing Benchmark (%dx%d, %d max_iter, %d runs, %zu cores) ===\n\n",
           IMAGE_WIDTH, IMAGE_HEIGHT, MAX_ITERATIONS, NUM_RUNS, num_cores);

    mandelbrot_payload seq_payload = {
        .output_buffer = (uint16_t*)malloc(TOTAL_PIXELS * sizeof(uint16_t)),
        .width = IMAGE_WIDTH,
        .height = IMAGE_HEIGHT,
        .max_iter = MAX_ITERATIONS,
        .x_min = -2.0, .x_max = 1.0,
        .y_min = -1.12, .y_max = 1.12
    };

    mandelbrot_payload static_payload = seq_payload;
    static_payload.output_buffer = (uint16_t*)malloc(TOTAL_PIXELS * sizeof(uint16_t));

    mandelbrot_payload conc_payload = seq_payload;
    conc_payload.output_buffer = (uint16_t*)malloc(TOTAL_PIXELS * sizeof(uint16_t));

    long long* seq_times = (long long*)malloc(NUM_RUNS * sizeof(long long));
    long long* static_times = (long long*)malloc(NUM_RUNS * sizeof(long long));
    long long* conc_times = (long long*)malloc(NUM_RUNS * sizeof(long long));

    if (!seq_payload.output_buffer || !static_payload.output_buffer || !conc_payload.output_buffer ||
        !seq_times || !static_times || !conc_times) {
        fprintf(stderr, "Fatal: Memory allocation failed.\n");
        return 1;
    }

    long long seq_sum_us = 0, static_sum_us = 0, conc_sum_us = 0;
    struct timespec start, end;

    // ---------------------------------------------------------
    // PHASE 1: Sequential Execution (Single Core)
    // ---------------------------------------------------------
    printf("[1/3] Benchmarking Sequential Renderer (1 Core)...\n");
    render_mandelbrot_chunk(&seq_payload, 0, TOTAL_PIXELS); // Warmup

    for (int run = 0; run < NUM_RUNS; run++) {
        printf("\r  Sequential run %d of %d...", run + 1, NUM_RUNS);
        fflush(stdout);

        clock_gettime(CLOCK_MONOTONIC, &start);
        render_mandelbrot_chunk(&seq_payload, 0, TOTAL_PIXELS);
        clock_gettime(CLOCK_MONOTONIC, &end);

        long long elapsed = (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_nsec - start.tv_nsec) / 1000LL;
        seq_times[run] = elapsed;
        seq_sum_us += elapsed;
    }
    printf("\n");

    // ---------------------------------------------------------
    // PHASE 2: Static OS Thread Partitioning (No Work-Stealing)
    // ---------------------------------------------------------
    printf("[2/3] Benchmarking Static Thread Partitioning (%zu Cores, Equal 1/%zu Slices)...\n", num_cores, num_cores);
    os_thread_t* static_threads = (os_thread_t*)malloc(num_cores * sizeof(os_thread_t));
    static_worker_slice* slices = (static_worker_slice*)malloc(num_cores * sizeof(static_worker_slice));

    size_t pixels_per_core = TOTAL_PIXELS / num_cores;
    for (size_t i = 0; i < num_cores; i++) {
        slices[i].start_idx = i * pixels_per_core;
        slices[i].end_idx = (i == num_cores - 1) ? TOTAL_PIXELS : (i + 1) * pixels_per_core;
        slices[i].payload = &static_payload;
#if defined(_WIN32)
        static_threads[i] = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)static_thread_loop, &slices[i], 0, NULL);
#else
        pthread_create(&static_threads[i], NULL, static_thread_loop, &slices[i]);
#endif
    }

    // Warmup static threads
    for (int w = 0; w < 3; w++) {
        atomic_store_explicit(&static_completed_count, 0, memory_order_relaxed);
        atomic_fetch_add_explicit(&static_generation, 1, memory_order_release);
        while (atomic_load_explicit(&static_completed_count, memory_order_acquire) < num_cores) {
            cpu_relax();
        }
    }

    for (int run = 0; run < NUM_RUNS; run++) {
        printf("\r  Static Threads run %d of %d...", run + 1, NUM_RUNS);
        fflush(stdout);

        clock_gettime(CLOCK_MONOTONIC, &start);
        atomic_store_explicit(&static_completed_count, 0, memory_order_relaxed);
        atomic_fetch_add_explicit(&static_generation, 1, memory_order_release);

        while (atomic_load_explicit(&static_completed_count, memory_order_acquire) < num_cores) {
            cpu_relax();
        }
        clock_gettime(CLOCK_MONOTONIC, &end);

        long long elapsed = (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_nsec - start.tv_nsec) / 1000LL;
        static_times[run] = elapsed;
        static_sum_us += elapsed;
    }
    printf("\n");

    // Shut down static threads completely before booting sk-scheduler
    atomic_store_explicit(&static_stop_flag, true, memory_order_release);
    for (size_t i = 0; i < num_cores; i++) {
#if defined(_WIN32)
        WaitForSingleObject(static_threads[i], INFINITE);
        CloseHandle(static_threads[i]);
#else
        pthread_join(static_threads[i], NULL);
#endif
    }
    free(static_threads);
    free(slices);

    // ---------------------------------------------------------
    // PHASE 3: sk-scheduler Dynamic Work-Stealing Execution
    // ---------------------------------------------------------
    printf("[3/3] Benchmarking sk-scheduler Work-Stealing Renderer (%zu Cores)...\n", num_cores);
    scheduler_boot(1024, 4096);

    // Warmup
    for (int w = 0; w < 3; w++) {
        sc_job warmup_job;
        scheduler_init_job(&warmup_job, &conc_payload, TOTAL_PIXELS, render_mandelbrot_chunk);
        scheduler_submit_job(&warmup_job, SCANLINE_CHUNK_PIXELS);
        scheduler_wait_for_job(&warmup_job);
    }

    for (int run = 0; run < NUM_RUNS; run++) {
        printf("\r  sk-scheduler run %d of %d...", run + 1, NUM_RUNS);
        fflush(stdout);

        sc_job render_job;
        scheduler_init_job(&render_job, &conc_payload, TOTAL_PIXELS, render_mandelbrot_chunk);

        clock_gettime(CLOCK_MONOTONIC, &start);
        if (!scheduler_submit_job(&render_job, SCANLINE_CHUNK_PIXELS)) {
            fprintf(stderr, "\nHardware backpressure: Job rejected on run %d.\n", run + 1);
            scheduler_stop_workers();
            return 1;
        }
        scheduler_wait_for_job(&render_job);
        clock_gettime(CLOCK_MONOTONIC, &end);

        long long elapsed = (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_nsec - start.tv_nsec) / 1000LL;
        conc_times[run] = elapsed;
        conc_sum_us += elapsed;
    }
    printf("\n");

    scheduler_stop_workers();

    // ---------------------------------------------------------
    // Validation (Bit-for-Bit Pixel Verification Across All 3)
    // ---------------------------------------------------------
    bool valid_static = (memcmp(seq_payload.output_buffer, static_payload.output_buffer, TOTAL_PIXELS * sizeof(uint16_t)) == 0);
    bool valid_conc = (memcmp(seq_payload.output_buffer, conc_payload.output_buffer, TOTAL_PIXELS * sizeof(uint16_t)) == 0);

    if (!valid_static || !valid_conc) {
        printf("\nVALIDATION FAILED: Output buffers differ from sequential reference!\n");
    } else {
        qsort(seq_times, NUM_RUNS, sizeof(long long), compare_ll);
        qsort(static_times, NUM_RUNS, sizeof(long long), compare_ll);
        qsort(conc_times, NUM_RUNS, sizeof(long long), compare_ll);

        float seq_p50 = seq_times[NUM_RUNS / 2] / 1000.0f;
        float static_p50 = static_times[NUM_RUNS / 2] / 1000.0f;
        float conc_p50 = conc_times[NUM_RUNS / 2] / 1000.0f;

        printf("\n=== Final Execution Statistics (%d Runs) ===\n", NUM_RUNS);
        printf("Validation                         : SUCCESS (Bit-exact 1920x1080 match)\n\n");

        printf("--- 1. Sequential (1 Core) ---\n");
        printf("Median (P50)                       : %.2f ms\n", seq_p50);
        printf("Min / Max / Avg                    : %.2f ms / %.2f ms / %.2f ms\n\n",
               seq_times[0] / 1000.0f, seq_times[NUM_RUNS - 1] / 1000.0f, (seq_sum_us / (float)NUM_RUNS) / 1000.0f);

        printf("--- 2. Static Partitioning (%zu Cores, No Stealing) ---\n", num_cores);
        printf("Median (P50)                       : %.2f ms\n", static_p50);
        printf("Min / Max / Avg                    : %.2f ms / %.2f ms / %.2f ms\n\n",
               static_times[0] / 1000.0f, static_times[NUM_RUNS - 1] / 1000.0f, (static_sum_us / (float)NUM_RUNS) / 1000.0f);

        printf("--- 3. sk-scheduler Work-Stealing (%zu Cores) ---\n", num_cores);
        printf("Median (P50)                       : %.2f ms\n", conc_p50);
        printf("Min / Max / Avg                    : %.2f ms / %.2f ms / %.2f ms\n\n",
               conc_times[0] / 1000.0f, conc_times[NUM_RUNS - 1] / 1000.0f, (conc_sum_us / (float)NUM_RUNS) / 1000.0f);

        printf("--- Net Speedup Results ---\n");
        printf("Static Threads vs Sequential       : %.2fx Faster\n", seq_p50 / static_p50);
        printf("sk-scheduler vs Static Threads     : %.2fx Faster (Work-Stealing Gain!)\n", static_p50 / conc_p50);
        printf("sk-scheduler vs Sequential         : %.2fx Faster\n", seq_p50 / conc_p50);
    }

    free(seq_times); free(static_times); free(conc_times);
    free(seq_payload.output_buffer);
    free(static_payload.output_buffer);
    free(conc_payload.output_buffer);
    return 0;
}