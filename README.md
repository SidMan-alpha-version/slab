# Slab Programming Language (v1.0)

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Build Status](https://img.shields.io/badge/build-passing-brightgreen.svg)](#)
[![Standards: C11](https://img.shields.io/badge/C%20Standard-C11-orange.svg)](#)

**Slab** is a high-performance, mathematically focused systems programming language designed for extreme RAM efficiency and predictable low-latency execution.

Slab eliminates Garbage Collection pauses entirely through scoped workspace arenas, provides native duality between Struct-of-Arrays (SoA) and Array-of-Structures (AoS) memory layouts, integrates mathematical operators directly into the language, and provides zero-dependency multi-core data parallelism.

---

## Key Features

- **Predictable Arena Memory (Zero GC):** Dynamic memory is allocated in explicit `workspace` blocks and can be rolled back in $O(1)$ time with `reset:` checkpoints. Compile-time escape analysis guarantees memory safety without garbage collection overhead.
- **Dual SoA / AoS Layouts:**
  - `array<T>` lowers to Struct-of-Arrays (SoA) for auto-vectorized SIMD column operations.
  - `table<T>` lowers to Array-of-Structures (AoS) for contiguous, cache-coherent records.
- **Multi-Core Data Parallelism:** Express data-parallel loops cleanly with `parallel for i in 0..N:`, powered natively by POSIX `<pthread.h>` with compile-time race prevention.
- **Expressive Mathematics:** First-class discrete summation (`sigma`), numerical integration (`integral`), inverse trigonometry and in-place reversal (`arc`), and constants (`math.pi`, `math.e`, `math.tau`).
- **Zero-Copy Strings:** String slices, trimming, prefix/suffix comparisons, and substring matching operate directly on string views with 0 bytes of RAM allocated.
- **Seamless C Interoperability:** Include external C headers with `import "header.h"`, declare external functions with `extern fn`, and pass table memory directly using `.raw_ptr()`.
- **`--pack` 1-Bit Bool & Dead-Field Stripping:** Compress `bool` columns in `array<T>` to 1-bit bitvectors (8x RAM reduction) and automatically strip unread fields from memory.

---

## Installation & Build

Slab has **zero external dependencies** and builds with any standard C11 compiler on macOS or Linux:

```bash
git clone https://github.com/SidMan-alpha-version/slab.git
cd slab
make
```

Run test suite:
```bash
make test
```

---

## Quick Start

### 1. Hello World
```slab
# examples/hello.slb
fn main():
    state("Hello, Slab v1.0!")
    msg = "Welcome to RAM-efficient systems programming."
    state(msg)
```

Run directly:
```bash
./slab run examples/hello.slb
```

### 2. Multi-Core Simulation
```slab
# examples/simulation.slb
record Particle:
    x: f64
    y: f64
    vx: f64
    vy: f64

fn main():
    workspace sim_ws:
        n = 100000
        pts = array<Particle>[n]

        for i in 0..n:
            pts.push(i * 0.01, i * 0.02, 1.0, 2.0)

        dt = 0.016
        parallel for i in 0..n:
            pts.x[i] = pts.x[i] + pts.vx[i] * dt
            pts.y[i] = pts.y[i] + pts.vy[i] * dt

        state pts.x[0], pts.y[0]
```

Compile standalone executable:
```bash
./slab build examples/simulation.slb -o sim
./sim
```

---

## Project Structure

```text
slab/
├── README.md              # Project overview and quick start
├── Makefile               # Standard C11 build and test targets
├── .gitignore             # Git ignore configuration
├── src/
│   ├── token.h            # Token enumerations
│   ├── slab_arena.h       # Bump-pointer arena memory allocator
│   ├── lexer.h / lexer.c  # Indentation-aware lexical analyzer
│   ├── ast.h / ast.c      # Abstract syntax tree definitions
│   ├── parser.h / parser.c# Recursive descent parser
│   ├── analyzer.h / .c    # Strict semantic analysis & escape checks
│   ├── codegen.h / .c     # C code emitter & parallel worker generator
│   └── main.c             # Compiler CLI entry point
├── include/
│   └── slab_runtime.h     # Zero-dependency runtime & pthread parallel for
├── docs/
│   └── LANGUAGE.md        # Full language specification
├── examples/
│   ├── hello.slb
│   ├── mathematics.slb
│   ├── arrays_and_tables.slb
│   └── simulation.slb
├── tests/
│   ├── valid/             # Full regression tests for all language constructs
│   ├── invalid/           # Negative semantic validation tests
│   └── run_tests.sh       # Automated test runner
└── benchmarks/
    ├── particle.slb       # 100,000 particle simulation
    └── particle.c         # Native C comparison baseline
```

---

## Documentation

For full details on syntax, grammar, memory model, and operators, see [docs/LANGUAGE.md](docs/LANGUAGE.md).

## License

MIT License.
