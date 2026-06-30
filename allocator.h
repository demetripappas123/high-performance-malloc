#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <stddef.h>

void* my_malloc(size_t size);
void my_free(void* ptr);

void* hpmalloc(size_t size);
void hpfree(void* ptr);

typedef struct hp_stats {
    size_t malloc_calls;
    size_t free_calls;
    size_t split_count;
    size_t coalesce_count;
    size_t sbrk_calls;
    size_t bytes_from_os;
    size_t peak_bytes_from_os;
    size_t pool_allocs;
    size_t os_allocs;
    size_t metadata_bytes;
    size_t live_user_bytes;
    size_t free_user_bytes;
    size_t largest_free_block;
    size_t live_blocks;
} hp_stats_t;

void hp_get_stats(hp_stats_t *out);
void hp_reset_stats(void);

#endif