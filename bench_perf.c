#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

#ifndef NUM_ALLOCS
#define NUM_ALLOCS 100000
#endif

#ifndef MAX_SIZE
#define MAX_SIZE 4096
#endif

static double elapsed_seconds(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) +
           (end.tv_nsec - start.tv_nsec) / 1e9;
}

int main(void) {
    void *ptrs[NUM_ALLOCS];
    size_t sizes[NUM_ALLOCS];

    srand(42);

    struct timespec start, end;

    printf("Running allocator performance test...\n");
    printf("NUM_ALLOCS = %d\n", NUM_ALLOCS);
    printf("MAX_SIZE   = %d bytes\n\n", MAX_SIZE);

    // Generate random allocation sizes
    for (int i = 0; i < NUM_ALLOCS; i++) {
        sizes[i] = (rand() % MAX_SIZE) + 1;
        ptrs[i] = NULL;
    }

    // Test malloc
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < NUM_ALLOCS; i++) {
        ptrs[i] = malloc(sizes[i]);

        if (ptrs[i] == NULL) {
            fprintf(stderr, "malloc failed at index %d\n", i);
            return 1;
        }

        memset(ptrs[i], 0xA5, sizes[i]);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    double malloc_time = elapsed_seconds(start, end);

    // Free half of allocations
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < NUM_ALLOCS; i += 2) {
        free(ptrs[i]);
        ptrs[i] = NULL;
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    double partial_free_time = elapsed_seconds(start, end);

    // Reallocate into freed slots
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < NUM_ALLOCS; i += 2) {
        size_t new_size = (rand() % MAX_SIZE) + 1;
        ptrs[i] = malloc(new_size);

        if (ptrs[i] == NULL) {
            fprintf(stderr, "second malloc failed at index %d\n", i);
            return 1;
        }

        memset(ptrs[i], 0x5A, new_size);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    double reuse_malloc_time = elapsed_seconds(start, end);

    // Free everything
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < NUM_ALLOCS; i++) {
        free(ptrs[i]);
        ptrs[i] = NULL;
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    double final_free_time = elapsed_seconds(start, end);

    printf("Results:\n");
    printf("Initial malloc + memset:   %.6f sec\n", malloc_time);
    printf("Partial free:              %.6f sec\n", partial_free_time);
    printf("Reuse malloc + memset:     %.6f sec\n", reuse_malloc_time);
    printf("Final free:                %.6f sec\n", final_free_time);
    printf("Total time:                %.6f sec\n",
           malloc_time + partial_free_time + reuse_malloc_time + final_free_time);

    return 0;
}