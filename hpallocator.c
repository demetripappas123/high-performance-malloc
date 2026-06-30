#include "allocator.h"
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#define MAGIC 0x12345678

#define ALIGNMENT ((size_t)8)
#define SIZE_THRESHOLD ((size_t)512)
/* Smallest power-of-two class for buckets (2^MIN_BUCKET_SHIFT bytes). */
#define MIN_BUCKET_SHIFT 3
/*
 * One bucket per class: 2^MIN, 2^(MIN+1), … up through SIZE_THRESHOLD.
 * Keep HPA_LOG2_THRESHOLD in sync: SIZE_THRESHOLD must equal 2^HPA_LOG2_THRESHOLD.
 */
#define HPA_LOG2_THRESHOLD 9 /* 512 */
#define NUM_BUCKETS ((unsigned)(HPA_LOG2_THRESHOLD - MIN_BUCKET_SHIFT + 1))
#define MIN_SPLIT_PAYLOAD ((size_t)16)

_Static_assert(SIZE_THRESHOLD == (1ul << HPA_LOG2_THRESHOLD),
               "SIZE_THRESHOLD and HPA_LOG2_THRESHOLD must match");

typedef enum { RED, BLACK } node_color;

#define HP_SIZE_MASK ((size_t)~7u)
#define HP_FREE_BIT  ((size_t)1)
#define HP_RED_BIT   ((size_t)2)

typedef struct block_header {
    size_t size_and_flags;
#ifndef NDEBUG
    uint32_t magic;
#endif
    struct block_header *prev_heap;
    struct block_header *next_heap;
    union {
        struct {
            struct block_header *prev;
            struct block_header *next;
        } bucket;
        struct {
            struct block_header *left;
            struct block_header *right;
            struct block_header *parent;
        } tree;
    } pool;
} block_header;

#define BUCKET_PREV(b) ((b)->pool.bucket.prev)
#define BUCKET_NEXT(b) ((b)->pool.bucket.next)
#define TREE_LEFT(b)   ((b)->pool.tree.left)
#define TREE_RIGHT(b)  ((b)->pool.tree.right)
#define TREE_PARENT(b) ((b)->pool.tree.parent)

static inline size_t block_size(const block_header *b) {
    return b->size_and_flags & HP_SIZE_MASK;
}

static inline int block_is_free(const block_header *b) {
    return (int)(b->size_and_flags & HP_FREE_BIT);
}

static inline node_color block_color(const block_header *b) {
    return (b->size_and_flags & HP_RED_BIT) ? RED : BLACK;
}

static inline void block_set_size(block_header *b, size_t payload) {
    b->size_and_flags = (b->size_and_flags & ~HP_SIZE_MASK) | (payload & HP_SIZE_MASK);
}

static inline void block_set_free(block_header *b, int free) {
    if (free)
        b->size_and_flags |= HP_FREE_BIT;
    else
        b->size_and_flags &= ~HP_FREE_BIT;
}

static inline void block_set_color(block_header *b, node_color c) {
    if (c == RED)
        b->size_and_flags |= HP_RED_BIT;
    else
        b->size_and_flags &= ~HP_RED_BIT;
}

static inline void block_init_header(block_header *b, size_t payload, int free) {
    b->size_and_flags = (payload & HP_SIZE_MASK) | (free ? HP_FREE_BIT : 0);
#ifndef NDEBUG
    b->magic = MAGIC;
#endif
}

static inline int block_magic_ok(const block_header *b) {
#ifndef NDEBUG
    return b->magic == MAGIC;
#else
    (void)b;
    return 1;
#endif
}

/* ---------- NIL sentinel (all “null” RB children point here) ---------- */

static block_header rb_nil_storage;

static block_header *nil(void) {
    return &rb_nil_storage;
}

static inline void block_clear_pool_links(block_header *b) {
    BUCKET_PREV(b) = BUCKET_NEXT(b) = NULL;
    TREE_LEFT(b) = TREE_RIGHT(b) = nil();
    TREE_PARENT(b) = NULL;
    block_set_color(b, BLACK);
}

static int is_nil(const block_header *x) {
    return x == &rb_nil_storage;
}

static void rb_nil_init(void) {
    block_init_header(&rb_nil_storage, 0, 0);
    TREE_LEFT(&rb_nil_storage) = TREE_RIGHT(&rb_nil_storage) = nil();
    TREE_PARENT(&rb_nil_storage) = NULL;
    block_set_color(&rb_nil_storage, BLACK);
}

static block_header *rb_root = NULL;
static block_header *buckets[NUM_BUCKETS];
static block_header *heap_head = NULL;
static block_header *heap_tail = NULL;
static int hp_initialized;

//for testing metrics 
static size_t split_count = 0;
static size_t coalesce_count = 0;
static size_t sbrk_calls = 0;
static size_t bytes_requested_from_os = 0;
static size_t peak_bytes_from_os = 0;
static size_t malloc_calls = 0;
static size_t free_calls = 0;
static size_t pool_allocs = 0;
static size_t os_allocs = 0;







static void hp_init(void) {
    if (hp_initialized)
        return;
    rb_nil_init();
    for (unsigned i = 0; i < NUM_BUCKETS; i++)
        buckets[i] = NULL;
    hp_initialized = 1;
}

static size_t align_up(size_t s) {
    return (s + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1);
}

/* ---------- Power-of-two buckets: index for size s (s > 0, aligned) ---------- */

static unsigned bucket_index_for_size(size_t s) {
    unsigned idx = 0;
    size_t lim = (size_t)1u << MIN_BUCKET_SHIFT;
    while (s > lim && idx + 1u < NUM_BUCKETS) {
        lim <<= 1;
        idx++;
    }
    return idx;
}

static void bucket_insert(block_header *b) {
    unsigned i = bucket_index_for_size(block_size(b));
    BUCKET_PREV(b) = NULL;
    BUCKET_NEXT(b) = buckets[i];
    if (buckets[i])
        BUCKET_PREV(buckets[i]) = b;
    buckets[i] = b;
}

static void bucket_remove(block_header *b, unsigned idx) {
    if (BUCKET_PREV(b))
        BUCKET_NEXT(BUCKET_PREV(b)) = BUCKET_NEXT(b);
    else
        buckets[idx] = BUCKET_NEXT(b);
    if (BUCKET_NEXT(b))
        BUCKET_PREV(BUCKET_NEXT(b)) = BUCKET_PREV(b);
    BUCKET_PREV(b) = BUCKET_NEXT(b) = NULL;
}

/* ---------- Red–black tree (intrusive on block_header, key = size) ---------- */

static void left_rotate(block_header **root, block_header *x) {
    block_header *y = TREE_RIGHT(x);
    TREE_RIGHT(x) = TREE_LEFT(y);
    if (!is_nil(TREE_LEFT(y)))
        TREE_PARENT(TREE_LEFT(y)) = x;
    TREE_PARENT(y) = TREE_PARENT(x);
    if (TREE_PARENT(x) == NULL)
        *root = y;
    else if (x == TREE_LEFT(TREE_PARENT(x)))
        TREE_LEFT(TREE_PARENT(x)) = y;
    else
        TREE_RIGHT(TREE_PARENT(x)) = y;
    TREE_LEFT(y) = x;
    TREE_PARENT(x) = y;
}

static void right_rotate(block_header **root, block_header *x) {
    block_header *y = TREE_LEFT(x);
    TREE_LEFT(x) = TREE_RIGHT(y);
    if (!is_nil(TREE_RIGHT(y)))
        TREE_PARENT(TREE_RIGHT(y)) = x;
    TREE_PARENT(y) = TREE_PARENT(x);
    if (TREE_PARENT(x) == NULL)
        *root = y;
    else if (x == TREE_LEFT(TREE_PARENT(x)))
        TREE_LEFT(TREE_PARENT(x)) = y;
    else
        TREE_RIGHT(TREE_PARENT(x)) = y;
    TREE_RIGHT(y) = x;
    TREE_PARENT(x) = y;
}

static void rb_bst_insert(block_header **root, block_header *z) {
    block_header *y = NULL;
    block_header *x = *root;
    while (!is_nil(x) && x != NULL) {
        y = x;
        if (block_size(z) < block_size(x))
            x = TREE_LEFT(x);
        else
            x = TREE_RIGHT(x);
    }
    TREE_PARENT(z) = y;
    if (y == NULL) {
        *root = z;
    } else if (block_size(z) < block_size(y)) {
        TREE_LEFT(y) = z;
    } else {
        TREE_RIGHT(y) = z;
    }
    TREE_LEFT(z) = TREE_RIGHT(z) = nil();
    block_set_color(z, RED);
}

static void rb_insert_fixup(block_header **root, block_header *z) {
    while (TREE_PARENT(z) != NULL && block_color(TREE_PARENT(z)) == RED) {
        if (TREE_PARENT(z) == TREE_LEFT(TREE_PARENT(TREE_PARENT(z)))) {
            block_header *y = TREE_RIGHT(TREE_PARENT(TREE_PARENT(z)));
            if (y != NULL && !is_nil(y) && block_color(y) == RED) {
                block_set_color(TREE_PARENT(z), BLACK);
                block_set_color(y, BLACK);
                block_set_color(TREE_PARENT(TREE_PARENT(z)), RED);
                z = TREE_PARENT(TREE_PARENT(z));
            } else {
                if (z == TREE_RIGHT(TREE_PARENT(z))) {
                    z = TREE_PARENT(z);
                    left_rotate(root, z);
                }
                block_set_color(TREE_PARENT(z), BLACK);
                block_set_color(TREE_PARENT(TREE_PARENT(z)), RED);
                right_rotate(root, TREE_PARENT(TREE_PARENT(z)));
            }
        } else {
            block_header *y = TREE_LEFT(TREE_PARENT(TREE_PARENT(z)));
            if (y != NULL && !is_nil(y) && block_color(y) == RED) {
                block_set_color(TREE_PARENT(z), BLACK);
                block_set_color(y, BLACK);
                block_set_color(TREE_PARENT(TREE_PARENT(z)), RED);
                z = TREE_PARENT(TREE_PARENT(z));
            } else {
                if (z == TREE_LEFT(TREE_PARENT(z))) {
                    z = TREE_PARENT(z);
                    right_rotate(root, z);
                }
                block_set_color(TREE_PARENT(z), BLACK);
                block_set_color(TREE_PARENT(TREE_PARENT(z)), RED);
                left_rotate(root, TREE_PARENT(TREE_PARENT(z)));
            }
        }
    }
    if (*root != NULL)
        block_set_color(*root, BLACK);
}

static void rb_insert(block_header **root, block_header *z) {
    if (*root == NULL) {
        TREE_PARENT(z) = NULL;
        TREE_LEFT(z) = TREE_RIGHT(z) = nil();
        block_set_color(z, BLACK);
        *root = z;
        return;
    }
    rb_bst_insert(root, z);
    rb_insert_fixup(root, z);
}

static block_header *subtree_min(block_header *x) {
    while (!is_nil(TREE_LEFT(x)))
        x = TREE_LEFT(x);
    return x;
}

static void rb_transplant(block_header **root, block_header *u, block_header *v) {
    if (TREE_PARENT(u) == NULL) {
        if (is_nil(v))
            *root = NULL;
        else
            *root = v;
    } else if (u == TREE_LEFT(TREE_PARENT(u))) {
        TREE_LEFT(TREE_PARENT(u)) = v;
    } else {
        TREE_RIGHT(TREE_PARENT(u)) = v;
    }
    if (is_nil(v))
        TREE_PARENT(nil()) = TREE_PARENT(u);
    else
        TREE_PARENT(v) = TREE_PARENT(u);
}

#define IS_BLACK(n) (is_nil(n) || block_color(n) == BLACK)

static void rb_delete_fixup(block_header **root, block_header *x) {
    while (x != *root && IS_BLACK(x)) {
        if (x == TREE_LEFT(TREE_PARENT(x))) {
            block_header *w = TREE_RIGHT(TREE_PARENT(x));
            if (!IS_BLACK(w)) {
                block_set_color(w, BLACK);
                block_set_color(TREE_PARENT(x), RED);
                left_rotate(root, TREE_PARENT(x));
                w = TREE_RIGHT(TREE_PARENT(x));
            }
            if (IS_BLACK(TREE_LEFT(w)) && IS_BLACK(TREE_RIGHT(w))) {
                block_set_color(w, RED);
                x = TREE_PARENT(x);
            } else {
                if (IS_BLACK(TREE_RIGHT(w))) {
                    if (!is_nil(TREE_LEFT(w)))
                        block_set_color(TREE_LEFT(w), BLACK);
                    block_set_color(w, RED);
                    right_rotate(root, w);
                    w = TREE_RIGHT(TREE_PARENT(x));
                }
                block_set_color(w, block_color(TREE_PARENT(x)));
                block_set_color(TREE_PARENT(x), BLACK);
                if (!is_nil(TREE_RIGHT(w)))
                    block_set_color(TREE_RIGHT(w), BLACK);
                left_rotate(root, TREE_PARENT(x));
                x = *root;
            }
        } else {
            block_header *w = TREE_LEFT(TREE_PARENT(x));
            if (!IS_BLACK(w)) {
                block_set_color(w, BLACK);
                block_set_color(TREE_PARENT(x), RED);
                right_rotate(root, TREE_PARENT(x));
                w = TREE_LEFT(TREE_PARENT(x));
            }
            if (IS_BLACK(TREE_LEFT(w)) && IS_BLACK(TREE_RIGHT(w))) {
                block_set_color(w, RED);
                x = TREE_PARENT(x);
            } else {
                if (IS_BLACK(TREE_LEFT(w))) {
                    if (!is_nil(TREE_RIGHT(w)))
                        block_set_color(TREE_RIGHT(w), BLACK);
                    block_set_color(w, RED);
                    left_rotate(root, w);
                    w = TREE_LEFT(TREE_PARENT(x));
                }
                block_set_color(w, block_color(TREE_PARENT(x)));
                block_set_color(TREE_PARENT(x), BLACK);
                if (!is_nil(TREE_LEFT(w)))
                    block_set_color(TREE_LEFT(w), BLACK);
                right_rotate(root, TREE_PARENT(x));
                x = *root;
            }
        }
    }
    if (!is_nil(x) && x != NULL)
        block_set_color(x, BLACK);
}

static void rb_delete(block_header **root, block_header *z) {
    if (z == NULL || is_nil(z))
        return;

    if (TREE_PARENT(z) == NULL && is_nil(TREE_LEFT(z)) && is_nil(TREE_RIGHT(z))) {
        *root = NULL;
        TREE_LEFT(z) = TREE_RIGHT(z) = nil();
        TREE_PARENT(z) = NULL;
        block_set_color(z, BLACK);
        return;
    }

    block_header *y = z;
    block_header *x;
    node_color y_orig_color = block_color(y);
    if (is_nil(TREE_LEFT(z))) {
        x = TREE_RIGHT(z);
        rb_transplant(root, z, TREE_RIGHT(z));
    } else if (is_nil(TREE_RIGHT(z))) {
        x = TREE_LEFT(z);
        rb_transplant(root, z, TREE_LEFT(z));
    } else {
        y = subtree_min(TREE_RIGHT(z));
        y_orig_color = block_color(y);
        x = TREE_RIGHT(y);
        if (TREE_PARENT(y) == z)
            TREE_PARENT(x) = y;
        else {
            rb_transplant(root, y, TREE_RIGHT(y));
            TREE_RIGHT(y) = TREE_RIGHT(z);
            TREE_PARENT(TREE_RIGHT(y)) = y;
        }
        rb_transplant(root, z, y);
        TREE_LEFT(y) = TREE_LEFT(z);
        TREE_PARENT(TREE_LEFT(y)) = y;
        block_set_color(y, block_color(z));
    }
    if (y_orig_color == BLACK)
        rb_delete_fixup(root, x);

    TREE_LEFT(z) = TREE_RIGHT(z) = nil();
    TREE_PARENT(z) = NULL;
    block_set_color(z, BLACK);
}

/* Smallest free block in tree with size >= need, or NULL. */
static block_header *rb_best_fit(block_header *root, size_t need) {
    block_header *best = NULL;
    block_header *x = root;
    while (x != NULL && !is_nil(x)) {
        if (block_is_free(x) && block_size(x) >= need) {
            best = x;
            x = TREE_LEFT(x);
        } else {
            x = TREE_RIGHT(x);
        }
    }
    return best;
}

static void tree_remove_block(block_header *b) {
    rb_delete(&rb_root, b);
}

static void tree_insert_block(block_header *b) {
    block_clear_pool_links(b);
    rb_insert(&rb_root, b);
}

/* ---------- Heap extend / split / route free blocks ---------- */

static block_header *request_space(block_header *last, size_t user_size) {
    block_header *block = sbrk((intptr_t)(sizeof(block_header) + (intptr_t)user_size));
    if (block == (void *)-1)
        return NULL;


    sbrk_calls++;
    bytes_requested_from_os += sizeof(block_header) + user_size;
    if (bytes_requested_from_os > peak_bytes_from_os)
        peak_bytes_from_os = bytes_requested_from_os;

    
    block_init_header(block, user_size, 0);
    block_clear_pool_links(block);
    block->prev_heap = last;
    block->next_heap = NULL;

    if (last) {
        last->next_heap = block;
    } else {
        heap_head = block;
    }
    heap_tail = block;
    return block;
}

static void free_to_pool(block_header *b) {
    block_set_free(b, 1);
    block_clear_pool_links(b);

    if (block_size(b) <= SIZE_THRESHOLD)
        bucket_insert(b);
    else
        tree_insert_block(b);
}

static void remove_from_pool(block_header *b) {
    if (block_size(b) <= SIZE_THRESHOLD)
        bucket_remove(b, bucket_index_for_size(block_size(b)));
    else
        tree_remove_block(b);
}



static block_header *coalesce(block_header *b) {
    // merge with previous neighbor
    if (b->prev_heap && block_is_free(b->prev_heap)) {
        coalesce_count++;
        block_header *p = b->prev_heap;

        remove_from_pool(p);

        block_set_size(p, block_size(p) + sizeof(block_header) + block_size(b));
        p->next_heap = b->next_heap;

        if (b->next_heap)
            b->next_heap->prev_heap = p;
        else
            heap_tail = p;

        b = p;
    }

    // merge with next neighbor
    if (b->next_heap && block_is_free(b->next_heap)) {
        coalesce_count++;
        block_header *n = b->next_heap;

        remove_from_pool(n);

        block_set_size(b, block_size(b) + sizeof(block_header) + block_size(n));
        b->next_heap = n->next_heap;

        if (n->next_heap)
            n->next_heap->prev_heap = b;
        else
            heap_tail = b;
    }

    return b;
}



/*
 * If b is larger than need, split so user gets exactly need; remainder returned to pool.
 */
static block_header *split_block(block_header *b, size_t need) {
    const size_t header_sz = sizeof(block_header);
    size_t total_after_split = need + header_sz + MIN_SPLIT_PAYLOAD;
    if (block_size(b) < total_after_split)
        return b;

    split_count++;
    size_t rem_payload = block_size(b) - need - header_sz;

    block_header *n = (block_header *)((unsigned char *)(b + 1) + need);
    block_init_header(n, rem_payload, 1);
    n->prev_heap = b;
    n->next_heap = b->next_heap;
    if (b->next_heap)
        b->next_heap->prev_heap = n;
    else
        heap_tail = n;
    b->next_heap = n;
    block_clear_pool_links(n);

    block_set_size(b, need);
    free_to_pool(n);
    return b;
}

void *hpmalloc(size_t size) {

    malloc_calls++;

    hp_init();

    if (size == 0)
        return NULL;

    size_t need = align_up(size);
    block_header *blk = NULL;

    /* 1) Segregated lists: powers of two up to threshold. */
    if (need <= SIZE_THRESHOLD) {
        unsigned start = bucket_index_for_size(need);
        for (unsigned i = start; i < NUM_BUCKETS; i++) {
            for (block_header *b = buckets[i]; b; b = BUCKET_NEXT(b)) {
                if (block_size(b) >= need) {
                    bucket_remove(b, i);
                    blk = b;
                    break;
                }
            }
            if (blk)
                break;
        }
    }

    /* 2) Red–black tree: larger requests and fallback / exact large fits. */
    if (!blk) {
        blk = rb_best_fit(rb_root, need);
        if (blk)
            tree_remove_block(blk);
    }

    /* 3) New memory from OS. */
    if (!blk) {
        blk = request_space(heap_tail, need);
        if (!blk)
            return NULL;
        os_allocs++;
        return (void *)(blk + 1);
    }

    pool_allocs++;

    blk = split_block(blk, need);
    block_set_free(blk, 0);
    block_clear_pool_links(blk);
    return (void *)(blk + 1);
}


void hpfree(void *ptr) {
    if (!ptr)
        return;

    free_calls++;

    hp_init();

    block_header *b = (block_header *)ptr - 1;

    if (!block_magic_ok(b))
        return;

    if (block_is_free(b))
        return;

    block_set_free(b, 1);

    b = coalesce(b);

    free_to_pool(b);
}

void hp_get_stats(hp_stats_t *out) {
    if (!out)
        return;

    out->malloc_calls = malloc_calls;
    out->free_calls = free_calls;
    out->split_count = split_count;
    out->coalesce_count = coalesce_count;
    out->sbrk_calls = sbrk_calls;
    out->bytes_from_os = bytes_requested_from_os;
    out->peak_bytes_from_os = peak_bytes_from_os;
    out->pool_allocs = pool_allocs;
    out->os_allocs = os_allocs;
    out->metadata_bytes = 0;
    out->live_user_bytes = 0;
    out->free_user_bytes = 0;
    out->largest_free_block = 0;
    out->live_blocks = 0;

    for (block_header *b = heap_head; b; b = b->next_heap) {
        out->metadata_bytes += sizeof(block_header);
        if (block_is_free(b)) {
            out->free_user_bytes += block_size(b);
            if (block_size(b) > out->largest_free_block)
                out->largest_free_block = block_size(b);
        } else {
            out->live_blocks++;
            out->live_user_bytes += block_size(b);
        }
    }
}

void hp_reset_stats(void) {
    split_count = 0;
    coalesce_count = 0;
    sbrk_calls = 0;
    bytes_requested_from_os = 0;
    peak_bytes_from_os = 0;
    malloc_calls = 0;
    free_calls = 0;
    pool_allocs = 0;
    os_allocs = 0;
}
