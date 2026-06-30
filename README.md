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

## Benchmark results

**Environment:** WSL2 (Ubuntu), `gcc -O2`  
**Date:** June 28, 2026  
**Config:** `STRESS_OPS=50000`, `LONG_OPS=200000`, `LEAK_BLOCKS=100`, seed `42`

### Summary: hpmalloc vs system malloc

| Scenario | Metric | System | hpmalloc | Relative |
|---|---|---:|---:|---|
| Random sizes / random free | malloc avg | 216 ns | 269 ns | hp 0.80× (slower) |
| | free avg | 85 ns | 103 ns | hp 0.82× (slower) |
| | realloc avg | 605 ns | 916 ns | hp 0.66× (slower) |
| | throughput | 3.68M ops/s | 2.61M ops/s | hp 0.71× |
| Alternating small/large | malloc avg | 961 ns | 1171 ns | hp 0.82× (slower) |
| | free avg | 182 ns | 199 ns | hp 0.91× (slower) |
| | realloc avg | 5175 ns | 8661 ns | hp 0.60× (slower) |
| | throughput | 1.14M ops/s | 0.80M ops/s | hp 0.70× |
| Long-running cycles | malloc avg | 75 ns | 61 ns | hp 1.23× (faster) |
| | free avg | 52 ns | 47 ns | hp 1.11× (faster) |
| | throughput | 8.12M ops/s | 9.51M ops/s | hp 1.17× (faster) |

**Takeaway:** Under mixed random and large-block workloads, system `malloc` is faster. On steady small-block alloc/free cycles (with warm free lists), `hpmalloc` wins on latency and throughput.

---

### Scenario 1: Random sizes / random free

#### System malloc

| Metric | avg | median | p95 | p99 | samples |
|---|---:|---:|---:|---:|---:|
| malloc | 216 ns | 40 ns | 340 ns | 3466 ns | 24,556 |
| free | 85 ns | 50 ns | 201 ns | 351 ns | 22,503 |
| realloc | 605 ns | 181 ns | 2775 ns | 8385 ns | 2,931 |

- **Throughput:** 3,677,908 ops/sec (0.014 s wall)
- **Total allocs / frees / reallocs:** 24,556 / 24,556 / 2,931
- **Peak live allocations:** 2,132
- **Avg request size:** 5,660 bytes
- **Largest request:** 65,536 bytes
- **Total bytes requested:** 138,992,682
- **Internal fragmentation:** 1.001 (via `malloc_usable_size`)

#### hpmalloc

| Metric | avg | median | p95 | p99 | samples |
|---|---:|---:|---:|---:|---:|
| malloc | 269 ns | 80 ns | 511 ns | 4098 ns | 24,580 |
| free | 103 ns | 80 ns | 190 ns | 330 ns | 22,479 |
| realloc | 916 ns | 190 ns | 2925 ns | 14,297 ns | 2,931 |

- **Throughput:** 2,612,949 ops/sec (0.020 s wall)
- **Total allocs / frees / reallocs:** 24,580 / 24,580 / 2,931
- **Peak live allocations:** 2,121
- **Avg request size:** 5,771 bytes
- **Largest request:** 65,536 bytes
- **Total bytes requested:** 141,860,630
- **Peak heap:** 12,494,576 bytes
- **Heap growth:** 12,494,576 bytes
- **sbrk calls:** 608
- **Metadata overhead:** 0.2000 (header bytes / user bytes)
- **Internal fragmentation:** 1.000
- **External fragmentation:** 0.912 (largest free / total free)
- **Reuse rate:** 0.978 (pool allocs / hpmalloc calls)

---

### Scenario 2: Alternating small/large

#### System malloc

| Metric | avg | median | p95 | p99 | samples |
|---|---:|---:|---:|---:|---:|
| malloc | 961 ns | 121 ns | 2996 ns | 9388 ns | 24,955 |
| free | 182 ns | 131 ns | 431 ns | 681 ns | 22,872 |
| realloc | 5175 ns | 431 ns | 25,808 ns | 94,717 ns | 2,167 |

- **Throughput:** 1,143,561 ops/sec (0.046 s wall)
- **Peak live allocations:** 2,151
- **Avg request size:** 20,282 bytes
- **Largest request:** 65,533 bytes
- **Total bytes requested:** 506,135,385

#### hpmalloc

| Metric | avg | median | p95 | p99 | samples |
|---|---:|---:|---:|---:|---:|
| malloc | 1171 ns | 241 ns | 3677 ns | 11,832 ns | 24,919 |
| free | 199 ns | 160 ns | 451 ns | 751 ns | 22,908 |
| realloc | 8661 ns | 501 ns | 65,994 ns | 107,271 ns | 2,166 |

- **Throughput:** 800,464 ops/sec (0.065 s wall)
- **Peak live allocations:** 2,118
- **Avg request size:** 20,285 bytes
- **Largest request:** 65,527 bytes
- **Total bytes requested:** 505,473,278
- **Peak heap:** 31,665,912 bytes
- **sbrk calls:** 622
- **Metadata overhead:** 0.0052
- **External fragmentation:** 1.000
- **Reuse rate:** 0.977

---

### Scenario 3: Long-running cycles

512 allocations per batch, repeated `LONG_OPS / 512` times. Small blocks only (16–2063 bytes).

#### System malloc

| Metric | avg | median | p95 | p99 | samples |
|---|---:|---:|---:|---:|---:|
| malloc | 75 ns | 51 ns | 131 ns | 241 ns | 199,680 |
| free | 52 ns | 40 ns | 80 ns | 150 ns | 199,680 |

- **Throughput:** 8,121,679 ops/sec (0.049 s wall)
- **Peak live allocations:** 512
- **Avg request size:** 1,040 bytes
- **Total bytes requested:** 207,750,406

#### hpmalloc

| Metric | avg | median | p95 | p99 | samples |
|---|---:|---:|---:|---:|---:|
| malloc | 61 ns | 50 ns | 130 ns | 220 ns | 199,680 |
| free | 47 ns | 40 ns | 60 ns | 110 ns | 199,680 |

- **Throughput:** 9,506,377 ops/sec (0.042 s wall)
- **Peak live allocations:** 512
- **Avg request size:** 1,040 bytes
- **Total bytes requested:** 207,652,012
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
