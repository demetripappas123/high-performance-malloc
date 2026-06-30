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

typedef struct block_header {
    size_t size;
    int magic;
    int free;
    /* Doubly linked heap list (all blocks in address order). */
    struct block_header *prev_heap;
    struct block_header *next_heap;
    /* Segregated free-list links (when free and size <= THRESHOLD). */
    struct block_header *prev;
    struct block_header *next;
    /* Red–black tree (when free and size > THRESHOLD, or links unused). */
    node_color color;
    struct block_header *left;
    struct block_header *right;
    struct block_header *parent;
} block_header;

/* ---------- NIL sentinel (all “null” RB children point here) ---------- */

static block_header rb_nil_storage;

static block_header *nil(void) {
    return &rb_nil_storage;
}

static int is_nil(const block_header *x) {
    return x == &rb_nil_storage;
}

static void rb_nil_init(void) {
    rb_nil_storage.left = rb_nil_storage.right = nil();
    rb_nil_storage.parent = NULL;
    rb_nil_storage.color = BLACK;
    rb_nil_storage.size = 0;
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
    unsigned i = bucket_index_for_size(b->size);
    b->prev = NULL;
    b->next = buckets[i];
    if (buckets[i])
        buckets[i]->prev = b;
    buckets[i] = b;
}

static void bucket_remove(block_header *b, unsigned idx) {
    if (b->prev)
        b->prev->next = b->next;
    else
        buckets[idx] = b->next;
    if (b->next)
        b->next->prev = b->prev;
    b->prev = b->next = NULL;
}

/* ---------- Red–black tree (intrusive on block_header, key = size) ---------- */

static void left_rotate(block_header **root, block_header *x) {
    block_header *y = x->right;
    x->right = y->left;
    if (!is_nil(y->left))
        y->left->parent = x;
    y->parent = x->parent;
    if (x->parent == NULL)
        *root = y;
    else if (x == x->parent->left)
        x->parent->left = y;
    else
        x->parent->right = y;
    y->left = x;
    x->parent = y;
}

static void right_rotate(block_header **root, block_header *x) {
    block_header *y = x->left;
    x->left = y->right;
    if (!is_nil(y->right))
        y->right->parent = x;
    y->parent = x->parent;
    if (x->parent == NULL)
        *root = y;
    else if (x == x->parent->left)
        x->parent->left = y;
    else
        x->parent->right = y;
    y->right = x;
    x->parent = y;
}

static void rb_bst_insert(block_header **root, block_header *z) {
    block_header *y = NULL;
    block_header *x = *root;
    while (!is_nil(x) && x != NULL) {
        y = x;
        if (z->size < x->size)
            x = x->left;
        else
            x = x->right;
    }
    z->parent = y;
    if (y == NULL) {
        *root = z;
    } else if (z->size < y->size) {
        y->left = z;
    } else {
        y->right = z;
    }
    z->left = z->right = nil();
    z->color = RED;
}

static void rb_insert_fixup(block_header **root, block_header *z) {
    while (z->parent != NULL && z->parent->color == RED) {
        if (z->parent == z->parent->parent->left) {
            block_header *y = z->parent->parent->right;
            if (y != NULL && !is_nil(y) && y->color == RED) {
                z->parent->color = BLACK;
                y->color = BLACK;
                z->parent->parent->color = RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->right) {
                    z = z->parent;
                    left_rotate(root, z);
                }
                z->parent->color = BLACK;
                z->parent->parent->color = RED;
                right_rotate(root, z->parent->parent);
            }
        } else {
            block_header *y = z->parent->parent->left;
            if (y != NULL && !is_nil(y) && y->color == RED) {
                z->parent->color = BLACK;
                y->color = BLACK;
                z->parent->parent->color = RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->left) {
                    z = z->parent;
                    right_rotate(root, z);
                }
                z->parent->color = BLACK;
                z->parent->parent->color = RED;
                left_rotate(root, z->parent->parent);
            }
        }
    }
    if (*root != NULL)
        (*root)->color = BLACK;
}

static void rb_insert(block_header **root, block_header *z) {
    if (*root == NULL) {
        z->parent = NULL;
        z->left = z->right = nil();
        z->color = BLACK;
        *root = z;
        return;
    }
    rb_bst_insert(root, z);
    rb_insert_fixup(root, z);
}

static block_header *subtree_min(block_header *x) {
    while (!is_nil(x->left))
        x = x->left;
    return x;
}

static void rb_transplant(block_header **root, block_header *u, block_header *v) {
    if (u->parent == NULL) {
        if (is_nil(v))
            *root = NULL;
        else
            *root = v;
    } else if (u == u->parent->left) {
        u->parent->left = v;
    } else {
        u->parent->right = v;
    }
    if (is_nil(v))
        nil()->parent = u->parent;
    else
        v->parent = u->parent;
}

#define IS_BLACK(n) (is_nil(n) || (n)->color == BLACK)

static void rb_delete_fixup(block_header **root, block_header *x) {
    while (x != *root && IS_BLACK(x)) {
        if (x == x->parent->left) {
            block_header *w = x->parent->right;
            if (!IS_BLACK(w)) {
                w->color = BLACK;
                x->parent->color = RED;
                left_rotate(root, x->parent);
                w = x->parent->right;
            }
            if (IS_BLACK(w->left) && IS_BLACK(w->right)) {
                w->color = RED;
                x = x->parent;
            } else {
                if (IS_BLACK(w->right)) {
                    if (!is_nil(w->left))
                        w->left->color = BLACK;
                    w->color = RED;
                    right_rotate(root, w);
                    w = x->parent->right;
                }
                w->color = x->parent->color;
                x->parent->color = BLACK;
                if (!is_nil(w->right))
                    w->right->color = BLACK;
                left_rotate(root, x->parent);
                x = *root;
            }
        } else {
            block_header *w = x->parent->left;
            if (!IS_BLACK(w)) {
                w->color = BLACK;
                x->parent->color = RED;
                right_rotate(root, x->parent);
                w = x->parent->left;
            }
            if (IS_BLACK(w->left) && IS_BLACK(w->right)) {
                w->color = RED;
                x = x->parent;
            } else {
                if (IS_BLACK(w->left)) {
                    if (!is_nil(w->right))
                        w->right->color = BLACK;
                    w->color = RED;
                    left_rotate(root, w);
                    w = x->parent->left;
                }
                w->color = x->parent->color;
                x->parent->color = BLACK;
                if (!is_nil(w->left))
                    w->left->color = BLACK;
                right_rotate(root, x->parent);
                x = *root;
            }
        }
    }
    if (!is_nil(x) && x != NULL)
        x->color = BLACK;
}

static void rb_delete(block_header **root, block_header *z) {
    if (z == NULL || is_nil(z))
        return;

    // deleting the only node in the tree
    if (z->parent == NULL && is_nil(z->left) && is_nil(z->right)) {
        *root = NULL;
        z->left = z->right = nil();
        z->parent = NULL;
        z->color = BLACK;
        return;
    }

    block_header *y = z;
    block_header *x;
    node_color y_orig_color = y->color;
    if (is_nil(z->left)) {
        x = z->right;
        rb_transplant(root, z, z->right);
    } else if (is_nil(z->right)) {
        x = z->left;
        rb_transplant(root, z, z->left);
    } else {
        y = subtree_min(z->right);
        y_orig_color = y->color;
        x = y->right;
        if (y->parent == z)
            x->parent = y;
        else {
            rb_transplant(root, y, y->right);
            y->right = z->right;
            y->right->parent = y;
        }
        rb_transplant(root, z, y);
        y->left = z->left;
        y->left->parent = y;
        y->color = z->color;
    }
    if (y_orig_color == BLACK)
        rb_delete_fixup(root, x);

    z->left = z->right = nil();
    z->parent = NULL;
    z->color = BLACK;
}

/* Smallest free block in tree with size >= need, or NULL. */
static block_header *rb_best_fit(block_header *root, size_t need) {
    block_header *best = NULL;
    block_header *x = root;
    while (x != NULL && !is_nil(x)) {
        if (x->free && x->size >= need) {
            best = x;
            x = x->left;
        } else {
            x = x->right;
        }
    }
    return best;
}

static void tree_remove_block(block_header *b) {
    rb_delete(&rb_root, b);
}

static void tree_insert_block(block_header *b) {
    b->prev = b->next = NULL;
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

    
    block->size = user_size;
    block->magic = MAGIC;
    block->free = 0;
    block->prev = block->next = NULL;
    block->prev_heap = last;
    block->next_heap = NULL;
    block->left = block->right = nil();
    block->parent = NULL;
    block->color = BLACK;

    if (last) {
        last->next_heap = block;
    } else {
        heap_head = block;
    }
    heap_tail = block;
    return block;
}

static void free_to_pool(block_header *b) {
    b->free = 1;
    b->prev = b->next = NULL;
    b->left = b->right = nil();
    b->parent = NULL;
    b->color = BLACK;

    if (b->size <= SIZE_THRESHOLD)
        bucket_insert(b);
    else
        tree_insert_block(b);
}

static void remove_from_pool(block_header *b) {
    if (b->size <= SIZE_THRESHOLD)
        bucket_remove(b, bucket_index_for_size(b->size));
    else
        tree_remove_block(b);
}



static block_header *coalesce(block_header *b) {
    // merge with previous neighbor
    if (b->prev_heap && b->prev_heap->free) {
        coalesce_count++;
        block_header *p = b->prev_heap;

        remove_from_pool(p);

        p->size += sizeof(block_header) + b->size;
        p->next_heap = b->next_heap;

        if (b->next_heap)
            b->next_heap->prev_heap = p;
        else
            heap_tail = p;

        b = p;
    }

    // merge with next neighbor
    if (b->next_heap && b->next_heap->free) {
        coalesce_count++;
        block_header *n = b->next_heap;

        remove_from_pool(n);

        b->size += sizeof(block_header) + n->size;
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
    if (b->size < total_after_split)
        return b;

    split_count++;
    size_t rem_payload = b->size - need - header_sz;

    block_header *n = (block_header *)((unsigned char *)(b + 1) + need);
    n->size = rem_payload;
    n->magic = MAGIC;
    n->free = 1;
    n->prev_heap = b;
    n->next_heap = b->next_heap;
    if (b->next_heap)
        b->next_heap->prev_heap = n;
    else
        heap_tail = n;
    b->next_heap = n;
    n->prev = n->next = NULL;
    n->left = n->right = nil();
    n->parent = NULL;
    n->color = BLACK;

    b->size = need;
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
            for (block_header *b = buckets[i]; b; b = b->next) {
                if (b->size >= need) {
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
    blk->free = 0;
    blk->prev = blk->next = NULL;
    blk->left = blk->right = nil();
    blk->parent = NULL;
    blk->color = BLACK;
    return (void *)(blk + 1);
}


void hpfree(void *ptr) {
    if (!ptr)
        return;

    free_calls++;

    hp_init();

    block_header *b = (block_header *)ptr - 1;

    if (b->magic != MAGIC)
        return;

    // prevent double free corruption
    if (b->free)
        return;

    b->free = 1;

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
        if (b->free) {
            out->free_user_bytes += b->size;
            if (b->size > out->largest_free_block)
                out->largest_free_block = b->size;
        } else {
            out->live_blocks++;
            out->live_user_bytes += b->size;
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
