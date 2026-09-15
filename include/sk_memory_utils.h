#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

//aligns the pointer so that it would fit the aligment requirement
//must use sys_aligned_free() to free it
static inline void* sys_aligned_alloc(size_t alignment, size_t size) {
    size_t offset = alignment - 1 + sizeof(void*);
    void* raw_ptr = malloc(size + offset);
    
    if (!raw_ptr) {
        return NULL;
    }

    uintptr_t unaligned_addr = (uintptr_t)raw_ptr + sizeof(void*);
    uintptr_t aligned_addr = (unaligned_addr + alignment - 1) & ~(alignment - 1);
    void* aligned_ptr = (void*)aligned_addr;

    ((void**)aligned_ptr)[-1] = raw_ptr;

    return aligned_ptr;
}

//frees the aligned memory
static inline void sys_aligned_free(void* aligned_ptr) {
    if (aligned_ptr) {
        free(((void**)aligned_ptr)[-1]);
    }
}

//round size_t to the next power of 2
static inline size_t next_power_of_two(size_t size) {
    // Clamp mathematically invalid capacities to a safe minimum
    if (size <= 2) {
        return 2;
    }

    size--;

    size |= size >> 1;
    size |= size >> 2;
    size |= size >> 4;
    size |= size >> 8;
    size |= size >> 16;

#if SIZE_MAX == 0xFFFFFFFFFFFFFFFFULL
    size |= size >> 32;
#endif

    return size + 1;
}