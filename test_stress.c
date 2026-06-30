#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "allocator.h"

#define SLOTS 1000
#define OPS 10000

typedef struct {
    void *ptr;
    size_t size;
    unsigned char pattern;
} Slot;

static size_t random_size(void) {
    int r = rand() % 100;

    if (r < 55) {
        size_t sizes[] = {8, 16, 24, 32, 40, 64, 96, 128, 256, 400, 504, 512};
        return sizes[rand() % (sizeof(sizes) / sizeof(sizes[0]))];
    } else if (r < 85) {
        size_t sizes[] = {513, 520, 600, 768, 1024, 2048, 4096};
        return sizes[rand() % (sizeof(sizes) / sizeof(sizes[0]))];
    } else {
        size_t sizes[] = {8192, 16384, 32768, 65536};
        return sizes[rand() % (sizeof(sizes) / sizeof(sizes[0]))];
    }
}

static void fill_pattern(void *ptr, size_t size, unsigned char pattern) {
    memset(ptr, pattern, size);
}

static void check_pattern(void *ptr, size_t size, unsigned char pattern) {
    unsigned char *p = (unsigned char *)ptr;

    for (size_t i = 0; i < size; i++) {
        if (p[i] != pattern) {
            fprintf(stderr,
                    "Memory corruption: expected %u, got %u at byte %zu of block size %zu\n",
                    pattern, p[i], i, size);
            abort();
        }
    }
}

int main(void) {
    Slot slots[SLOTS] = {0};

    srand(12345);

    printf("Running stress test...\n");

    for (int op = 0; op < OPS; op++) {
        int idx = rand() % SLOTS;

        if (slots[idx].ptr == NULL) {
            size_t size = random_size();
            void *p = hpmalloc(size);

            assert(p != NULL);

            unsigned char pattern = (unsigned char)(idx ^ op ^ size);

            slots[idx].ptr = p;
            slots[idx].size = size;
            slots[idx].pattern = pattern;

            fill_pattern(p, size, pattern);
        } else {
            check_pattern(slots[idx].ptr, slots[idx].size, slots[idx].pattern);

            hpfree(slots[idx].ptr);

            slots[idx].ptr = NULL;
            slots[idx].size = 0;
            slots[idx].pattern = 0;
        }

        // Occasionally verify a random live block.
        if (op % 1000 == 0) {
            int check_idx = rand() % SLOTS;
            if (slots[check_idx].ptr != NULL) {
                check_pattern(slots[check_idx].ptr,
                              slots[check_idx].size,
                              slots[check_idx].pattern);
            }
        }
    }

    // Verify and free everything still live.
    for (int i = 0; i < SLOTS; i++) {
        if (slots[i].ptr != NULL) {
            check_pattern(slots[i].ptr, slots[i].size, slots[i].pattern);
            hpfree(slots[i].ptr);
        }
    }

    printf("Evil test passed.\n");

    return 0;
}