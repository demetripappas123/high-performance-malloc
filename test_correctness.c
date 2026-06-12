// test_correctness.c

#include <assert.h>
#include <stdio.h>
#include "allocator.h"

int main(void) {

    printf("Running correctness tests...\n");

    // Test 1: malloc(0)
    assert(hpmalloc(0) == NULL);

    // Test 2: free(NULL)
    hpfree(NULL);

    // Test 3: basic allocation
    char *p = (char *)hpmalloc(64);
    assert(p != NULL);

    p[0] = 'A';
    p[63] = 'Z';

    assert(p[0] == 'A');
    assert(p[63] == 'Z');

    hpfree(p);

    // Test 4: double free should not crash
    hpfree(p);

    // Test 5: small block reuse
    void *a = hpmalloc(64);
    assert(a != NULL);

    hpfree(a);

    void *b = hpmalloc(64);
    assert(b != NULL);

    hpfree(b);

    // Test 6: large block reuse
    void *large1 = hpmalloc(2048);
    assert(large1 != NULL);

    hpfree(large1);

    void *large2 = hpmalloc(1024);
    assert(large2 != NULL);

    hpfree(large2);

    // Test 7: splitting
    void *split_src = hpmalloc(2048);
    assert(split_src != NULL);

    hpfree(split_src);

    void *small1 = hpmalloc(256);
    assert(small1 != NULL);

    void *small2 = hpmalloc(256);
    assert(small2 != NULL);

    hpfree(small1);
    hpfree(small2);

    // Test 8: coalescing
    void *c1 = hpmalloc(400);
    void *c2 = hpmalloc(400);

    assert(c1 != NULL);
    assert(c2 != NULL);

    hpfree(c1);
    hpfree(c2);

    void *big = hpmalloc(700);
    assert(big != NULL);

    hpfree(big);

    // Test 9: bucket/tree threshold crossing
    void *t1 = hpmalloc(400);
    void *t2 = hpmalloc(400);

    hpfree(t1);
    hpfree(t2);

    void *t3 = hpmalloc(750);
    assert(t3 != NULL);

    hpfree(t3);

    // Test 10: lots of small alloc/free
    for (int i = 0; i < 1000; i++) {
        void *x = hpmalloc(32);
        assert(x != NULL);
        hpfree(x);
    }

    printf("All correctness tests passed.\n");

    return 0;
}