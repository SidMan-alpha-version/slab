# The Slab Programming Language Specification (v1.0)

Slab is a high-performance, mathematically focused, systems programming language engineered for extreme RAM efficiency and predictable low-latency execution. Slab eliminates garbage collection entirely in favor of scoped workspace arenas and checkpoint unwinding, while providing native duality between Struct-of-Arrays (SoA) and Array-of-Structures (AoS) memory layouts, mathematical notation, and multi-core data parallelism.

---

## 1. Core Philosophy & Guarantees

1. **Deterministic Arena Memory (Zero GC):**
   - All dynamic memory is allocated from explicit `workspace` arenas.
   - Inner scratch computations can be unwound in $O(1)$ time with `reset:`.
   - The semantic analyzer enforces compile-time escape safety: pointers or slices allocated in an inner workspace cannot escape to an outer scope.

2. **Dual SoA / AoS Layouts:**
   - `array<T>[N]` allocates a **Struct-of-Arrays (SoA)**: each field of `T` resides in a contiguous, 64-byte aligned vector buffer suitable for automatic AVX-512 / NEON SIMD vectorization.
   - `table<T>[N]` allocates an **Array-of-Structures (AoS)**: items are stored contiguously in standard C layout for cache-friendly element processing and seamless C interoperability via `.raw_ptr()`.

3. **Multi-Core Data Parallelism:**
   - Multi-core loops are declared using `parallel for i in start..end -> step:`.
   - Lowered to POSIX `<pthread.h>` worker callbacks with zero external dependencies.
   - The compiler enforces data-race freedom at compile time by prohibiting collection mutations (`.push()`, `.clear()`) and workspace creations inside the parallel loop body.

4. **Pure Mathematical Notation:**
   - Discrete summations: `sigma expr from start limit end -> step`
   - Numerical integration: `integral(f(x) from a limit b -> dx)`
   - Dual-purpose `arc()` operator:
     - Inverse trigonometric functions: `arc(sin(x))` $\rightarrow \arcsin(x)$, `arc(cos(x))` $\rightarrow \arccos(x)$, `arc(tan(x))` $\rightarrow \arctan(x)$
     - Zero-copy reverse iteration: `for item in arc(arr):`
     - Zero-RAM in-place reversal: `arc(arr)` / `arc(str_val)`
   - Built-in constants: `math.pi`, `math.e`, `math.tau`.
   - Rejection of mutable operator sugars (`+=`, `-=`, `*=`, `/=`) to preserve pure mathematical identity: $x = x + \Delta$.

5. **Zero-Copy Strings:**
   - Strings are immutable `{ const char* ptr; size_t len; }` views.
   - Slicing (`s.slice(a, b)`), trimming (`s.trim()`), prefix/suffix checking (`s.starts_with()`, `s.ends_with()`), and searching (`s.contains()`) allocate 0 bytes of RAM.

---

## 2. Syntax & Grammar Reference

### 2.1 Declarations
```slab
# Global Constants
const G = 9.80665

# Record Types
record Particle:
    x: f64
    y: f64
    active: bool

# Functions
fn calculate_kinetic_energy(mass: f64, velocity: f64) -> f64:
    return 0.5 * mass * velocity * velocity

# External C Interop
import "my_c_header.h"
extern fn c_external_routine(ptr: int, count: int) -> int
```

### 2.2 Memory Management: Workspace & Reset
```slab
fn process():
    workspace local_ws:
        pts = array<Particle>[1000]
        pts.push(0.0, 0.0, true)

        # Reusable scratchpad with O(1) rewind
        for step in 0..100:
            reset:
                # Any memory allocated inside reset: is rewound at block exit
                temp_calc = "temporary calculation string"
                state temp_calc
```

### 2.3 Loops & Control Flow
```slab
# Sequential Range Loop
for i in 0..10 -> 2:
    state i

# Multi-Core Data-Parallel Range Loop
parallel for i in 0..100000:
    pts.x[i] = pts.x[i] + pts.vx[i] * dt

# Zero-Copy Reverse Iterator
for p in arc(pts):
    state p.x

# While Loop
while cond:
    do_something()

# Do ... Until Loop
do:
    step = step + 1
until step >= max_steps
```

### 2.4 Mathematical Expressions
```slab
# Discrete Summation
sum = sigma (1.0 / (i * i)) from 1.0 limit 1000.0 -> 1.0

# Numerical Integration
area = integral(sin(x) from 0.0 limit math.pi -> 0.0001)

# Inverse Trigonometry
angle = arc(sin(0.5))

# Permutation Sorting (in-place, cycle reordering, low RAM)
sort pts by y desc

# Linear Search with fallback
found_idx = search pts : pts.x[i] > 100.0 or -1
```

---

## 3. CLI Options
- `slab run <file.slb>`: Fast JIT compilation and immediate execution.
- `slab build <file.slb> [-o binary]`: Compile standalone native binary with `-O3 -march=native`.
- `slab build <file.slb> --pack [-o binary]`: Enable 1-bit boolean packing and dead-field stripping.
- `slab emit-c <file.slb> [-o out.c]`: Output generated C code.
