# high-performance-malloc

Custom allocator (`hpmalloc` / `hpfree`) with segregated free lists for small blocks and a red-black tree for large blocks. Includes correctness tests, a stress harness, and a performance benchmark that compares against the system `malloc`.

## Build & run (WSL / Linux)

```bash
# Correctness
gcc -O2 -o test_correctness test_correctness.c hpallocator.c
./test_correctness

# Stress test (pattern verification)
gcc -O2 -o test_stress test_stress.c hpallocator.c
./test_stress

# Performance benchmark
gcc -O2 -o bench_perf bench_perf.c hpallocator.c
./bench_perf
```

Optional benchmark tuning:

```bash
STRESS_OPS=50000 LONG_OPS=200000 ./bench_perf
```

---

## Test results

| Test | Result |
|---|---|
| `test_correctness` | All tests passed |
| `test_stress` (10,000 ops) | Evil test passed |

---

## Benchmark results

**Environment:** WSL2 (Ubuntu), `gcc -O2`  
**Date:** June 28, 2026  
**Config:** `STRESS_OPS=50000`, `LONG_OPS=200000`, `LEAK_BLOCKS=100`, seed `42`  
**Allocator:** compact header (union pool links, bit-packed flags)

### Summary: hpmalloc vs system malloc

| Scenario | Metric | System | hpmalloc | Relative |
|---|---|---:|---:|---|
| Random sizes / random free | malloc avg | 610 ns | 535 ns | hp 1.14× (faster) |
| | free avg | 222 ns | 214 ns | hp 1.04× (faster) |
| | realloc avg | 1409 ns | 1657 ns | hp 0.85× (slower) |
| | throughput | 1.42M ops/s | 1.18M ops/s | hp 0.83× |
| Alternating small/large | malloc avg | 1003 ns | 1861 ns | hp 0.54× (slower) |
| | free avg | 318 ns | 398 ns | hp 0.80× (slower) |
| | realloc avg | 5453 ns | 14741 ns | hp 0.37× (slower) |
| | throughput | 0.98M ops/s | 0.46M ops/s | hp 0.47× |
| Long-running cycles | malloc avg | 160 ns | 94 ns | hp 1.71× (faster) |
| | free avg | 108 ns | 65 ns | hp 1.67× (faster) |
| | throughput | 3.99M ops/s | 6.61M ops/s | hp 1.66× (faster) |

**Takeaway:** `hpmalloc` wins on random mixed sizes and steady small-block cycles. System `malloc` is significantly faster on alternating small/large workloads with heavy `realloc` churn.

---

### Scenario 1: Random sizes / random free

#### System malloc

| Metric | avg | median | p95 | p99 | samples |
|---|---:|---:|---:|---:|---:|
| malloc | 610 ns | 80 ns | 752 ns | 6933 ns | 24,556 |
| free | 222 ns | 121 ns | 501 ns | 802 ns | 22,503 |
| realloc | 1409 ns | 431 ns | 6362 ns | 18,605 ns | 2,931 |

- **Throughput:** 1,420,665 ops/sec (0.037 s wall)
- **Total allocs / frees / reallocs:** 24,556 / 24,556 / 2,931
- **Peak live allocations:** 2,132
- **Avg request size:** 5,660 bytes
- **Largest request:** 65,536 bytes
- **Total bytes requested:** 138,992,682
- **Internal fragmentation:** 1.001 (via `malloc_usable_size`)

#### hpmalloc

| Metric | avg | median | p95 | p99 | samples |
|---|---:|---:|---:|---:|---:|
| malloc | 535 ns | 161 ns | 1092 ns | 7804 ns | 24,580 |
| free | 214 ns | 161 ns | 391 ns | 581 ns | 22,479 |
| realloc | 1657 ns | 391 ns | 5560 ns | 27,993 ns | 2,931 |

- **Throughput:** 1,184,781 ops/sec (0.044 s wall)
- **Total allocs / frees / reallocs:** 24,580 / 24,580 / 2,931
- **Peak live allocations:** 2,121
- **Avg request size:** 5,771 bytes
- **Largest request:** 65,536 bytes
- **Total bytes requested:** 141,860,630
- **Peak heap:** 12,316,880 bytes
- **Heap growth:** 12,316,880 bytes
- **sbrk calls:** 597
- **Metadata overhead:** 0.1400 (header bytes / user bytes)
- **Internal fragmentation:** 1.000
- **External fragmentation:** 0.789 (largest free / total free)
- **Reuse rate:** 0.978 (pool allocs / hpmalloc calls)

---

### Scenario 2: Alternating small/large

#### System malloc

| Metric | avg | median | p95 | p99 | samples |
|---|---:|---:|---:|---:|---:|
| malloc | 1003 ns | 210 ns | 4819 ns | 8586 ns | 24,955 |
| free | 318 ns | 220 ns | 651 ns | 921 ns | 22,872 |
| realloc | 5453 ns | 621 ns | 30,858 ns | 78,488 ns | 2,167 |

- **Throughput:** 978,410 ops/sec (0.053 s wall)
- **Peak live allocations:** 2,151
- **Avg request size:** 20,282 bytes
- **Largest request:** 65,533 bytes
- **Total bytes requested:** 506,135,385

#### hpmalloc

| Metric | avg | median | p95 | p99 | samples |
|---|---:|---:|---:|---:|---:|
| malloc | 1861 ns | 431 ns | 6733 ns | 14,858 ns | 24,919 |
| free | 398 ns | 311 ns | 832 ns | 1433 ns | 22,908 |
| realloc | 14,741 ns | 872 ns | 103,635 ns | 173,305 ns | 2,166 |

- **Throughput:** 463,694 ops/sec (0.112 s wall)
- **Peak live allocations:** 2,118
- **Avg request size:** 20,285 bytes
- **Largest request:** 65,527 bytes
- **Total bytes requested:** 505,473,278
- **Peak heap:** 31,606,912 bytes
- **sbrk calls:** 620
- **Metadata overhead:** 0.0037
- **Internal fragmentation:** 1.000
- **External fragmentation:** 1.000
- **Reuse rate:** 0.977

---

### Scenario 3: Long-running cycles

512 allocations per batch, repeated `LONG_OPS / 512` times. Small blocks only (16–2063 bytes).

#### System malloc

| Metric | avg | median | p95 | p99 | samples |
|---|---:|---:|---:|---:|---:|
| malloc | 160 ns | 110 ns | 271 ns | 441 ns | 199,680 |
| free | 108 ns | 90 ns | 180 ns | 290 ns | 199,680 |

- **Throughput:** 3,988,609 ops/sec (0.100 s wall)
- **Peak live allocations:** 512
- **Avg request size:** 1,040 bytes
- **Total bytes requested:** 207,750,406
- **Internal fragmentation:** 1.008

#### hpmalloc

| Metric | avg | median | p95 | p99 | samples |
|---|---:|---:|---:|---:|---:|
| malloc | 94 ns | 70 ns | 170 ns | 291 ns | 199,680 |
| free | 65 ns | 60 ns | 80 ns | 121 ns | 199,680 |

- **Throughput:** 6,613,934 ops/sec (0.060 s wall)
- **Peak live allocations:** 512
- **Avg request size:** 1,040 bytes
- **Total bytes requested:** 207,652,012
- **Internal fragmentation:** 1.000
- **External fragmentation:** 1.000
- **Reuse rate:** 1.000 (all allocations served from pool after warmup)

---

### Leak check

Allocates 100 blocks, frees 50, reports unfreed remainder, then cleans up.

| Allocator | Leaked blocks (before cleanup) | Leaked bytes |
|---|---:|---:|
| system malloc | 50 | 102,014 |
| hpmalloc | 50 | 100,232 |

Both allocators correctly reported the intentional leaks; all memory was freed after measurement.

---

## Notes

- **Platform:** `hpmalloc` uses `sbrk()` and requires Linux/WSL. It does not build natively on Windows.
- **Realloc:** `hpmalloc` has no native `realloc`; the benchmark implements it as `malloc` + `memcpy` + `free`.
- **System allocator limits:** libc `malloc` does not expose peak heap, `sbrk` count, external fragmentation, or reuse rate; those metrics are `n/a` for system runs.
- **Tests:** `test_stress.c` runs 10,000 ops with byte-pattern integrity checks. `test_correctness.c` covers edge cases (zero-size alloc, double free, split/coalesce, etc.).
- **Compact header:** bucket and tree pointers share a union; `free` and color flags are packed into `size`. Metadata overhead on random workloads dropped from ~20% to ~14% with no change to split/coalesce policy.
