#ifndef SLAB_RUNTIME_H
#define SLAB_RUNTIME_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <pthread.h>
#include <unistd.h>
#include "slab_arena.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef M_E
#define M_E 2.71828182845904523536
#endif

#define M_TAU (6.28318530717958647692)

#if defined(__clang__)
#define SLAB_VECTORIZE _Pragma("clang loop vectorize(enable)")
#elif defined(__GNUC__)
#define SLAB_VECTORIZE _Pragma("GCC ivdep")
#else
#define SLAB_VECTORIZE
#endif

static inline void* slab_realloc_buffer(SlabWorkspace* ws, void* old_ptr, size_t old_cap, size_t new_cap, size_t elem_size, size_t align) {
    void* new_ptr = slab_alloc(ws, new_cap * elem_size, align);
    if (old_ptr && old_cap > 0 && new_ptr) {
        size_t copy_bytes = (old_cap < new_cap ? old_cap : new_cap) * elem_size;
        memcpy(new_ptr, old_ptr, copy_bytes);
    }
    return new_ptr;
}

static inline SlabStr slab_read_file(SlabWorkspace* ws, const char* path) {
    SlabStr str = { .ptr = "", .len = 0 };
    FILE* f = fopen(path, "rb");
    if (!f) return str;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return str; }
    char* buf = (char*)slab_alloc(ws, (size_t)size + 1, 1);
    if (buf) {
        size_t read_bytes = fread(buf, 1, (size_t)size, f);
        buf[read_bytes] = '\0';
        str.ptr = buf;
        str.len = read_bytes;
    }
    fclose(f);
    return str;
}

static inline void slab_print_str(SlabStr s) {
    if (s.ptr && s.len > 0) {
        fwrite(s.ptr, 1, s.len, stdout);
    }
}

// static inline void slab_println_str(SlabStr s) { slab_print_str(s); putchar('\n'); }


// Dot product helper for arrays
static inline double slab_dot_product(const double* __restrict__ a, const double* __restrict__ b, size_t n) {
    double sum = 0.0;
    SLAB_VECTORIZE
    for (size_t i = 0; i < n; i++) {
        sum += a[i] * b[i];
    }
    return sum;
}

// Trigonometric helpers
static inline double slab_cot(double x) { return 1.0 / tan(x); }
static inline double slab_sec(double x) { return 1.0 / cos(x); }
static inline double slab_csc(double x) { return 1.0 / sin(x); }

// ==========================================
// Zero-Copy String Manipulation Suite
// ==========================================

typedef struct {
    size_t len;
    size_t cap;
    SlabWorkspace* ws;
    SlabStr* data;
} List_SlabStr;

static inline List_SlabStr List_SlabStr_create(SlabWorkspace* ws, size_t cap) {
    List_SlabStr list;
    list.len = 0;
    list.cap = cap > 0 ? cap : 8;
    list.ws = ws;
    list.data = (SlabStr*)slab_alloc(ws, list.cap * sizeof(SlabStr), alignof(SlabStr));
    return list;
}

static inline void List_SlabStr_push(List_SlabStr* list, SlabStr item) {
    if (list->len >= list->cap) {
        size_t new_cap = list->cap * 2;
        list->data = (SlabStr*)slab_realloc_buffer(list->ws, list->data, list->cap, new_cap, sizeof(SlabStr), alignof(SlabStr));
        list->cap = new_cap;
    }
    list->data[list->len++] = item;
}

static inline bool slab_str_contains(SlabStr s, SlabStr sub) {
    if (sub.len == 0) return true;
    if (sub.len > s.len) return false;
    for (size_t i = 0; i <= s.len - sub.len; i++) {
        if (memcmp(s.ptr + i, sub.ptr, sub.len) == 0) return true;
    }
    return false;
}

static inline bool slab_str_starts_with(SlabStr s, SlabStr prefix) {
    if (prefix.len > s.len) return false;
    return memcmp(s.ptr, prefix.ptr, prefix.len) == 0;
}

static inline bool slab_str_ends_with(SlabStr s, SlabStr suffix) {
    if (suffix.len > s.len) return false;
    return memcmp(s.ptr + (s.len - suffix.len), suffix.ptr, suffix.len) == 0;
}

static inline SlabStr slab_str_trim(SlabStr s) {
    size_t start = 0;
    while (start < s.len && ((unsigned char)s.ptr[start] == ' ' || s.ptr[start] == '\t' ||
                             s.ptr[start] == '\r' || s.ptr[start] == '\n')) {
        start++;
    }
    size_t end = s.len;
    while (end > start && ((unsigned char)s.ptr[end - 1] == ' ' || s.ptr[end - 1] == '\t' ||
                           s.ptr[end - 1] == '\r' || s.ptr[end - 1] == '\n')) {
        end--;
    }
    return (SlabStr){ .ptr = s.ptr + start, .len = end - start };
}

static inline SlabStr slab_str_slice(SlabStr s, int64_t start, int64_t end) {
    if (start < 0) start = 0;
    if (start > (int64_t)s.len) start = (int64_t)s.len;
    if (end < start) end = start;
    if (end > (int64_t)s.len) end = (int64_t)s.len;
    return (SlabStr){ .ptr = s.ptr + start, .len = (size_t)(end - start) };
}

static inline List_SlabStr slab_str_split(SlabWorkspace* ws, SlabStr s, SlabStr delim) {
    List_SlabStr res = List_SlabStr_create(ws, 8);
    if (s.len == 0) return res;
    if (delim.len == 0) {
        List_SlabStr_push(&res, s);
        return res;
    }
    size_t pos = 0;
    while (pos <= s.len) {
        size_t next = pos;
        bool found = false;
        while (next + delim.len <= s.len) {
            if (memcmp(s.ptr + next, delim.ptr, delim.len) == 0) {
                found = true;
                break;
            }
            next++;
        }
        if (found) {
            SlabStr slice = { .ptr = s.ptr + pos, .len = next - pos };
            List_SlabStr_push(&res, slice);
            pos = next + delim.len;
            if (pos == s.len) {
                SlabStr empty = { .ptr = s.ptr + pos, .len = 0 };
                List_SlabStr_push(&res, empty);
                break;
            }
        } else {
            SlabStr slice = { .ptr = s.ptr + pos, .len = s.len - pos };
            List_SlabStr_push(&res, slice);
            break;
        }
    }
    return res;
}

static inline SlabStr slab_str_concat(SlabWorkspace* ws, SlabStr a, SlabStr b) {
    size_t total_len = a.len + b.len;
    char* buf = (char*)slab_alloc(ws, total_len + 1, 1);
    if (a.len > 0 && a.ptr) memcpy(buf, a.ptr, a.len);
    if (b.len > 0 && b.ptr) memcpy(buf + a.len, b.ptr, b.len);
    buf[total_len] = '\0';
    return (SlabStr){ .ptr = buf, .len = total_len };
}

static inline void slab_str_reverse_in_place(SlabStr s) {
    if (s.len <= 1 || !s.ptr) return;
    char* p = (char*)s.ptr;
    for (size_t i = 0, j = s.len - 1; i < j; i++, j--) {
        char tmp = p[i];
        p[i] = p[j];
        p[j] = tmp;
    }
}

// Multi-Core Parallel For Loop (Zero External Dependencies)
typedef struct {
    int64_t start;
    int64_t end;
    int64_t step;
    void (*fn)(int64_t, void*);
    void* ctx;
} SlabParallelWorkerArg;

static inline void* slab_parallel_worker(void* arg) {
    SlabParallelWorkerArg* w = (SlabParallelWorkerArg*)arg;
    for (int64_t i = w->start; i < w->end; i += w->step) {
        w->fn(i, w->ctx);
    }
    return NULL;
}

static inline void slab_parallel_for(int64_t start, int64_t end, int64_t step, void (*fn)(int64_t, void*), void* ctx) {
    if (step <= 0) step = 1;
    int64_t total_iterations = (end > start) ? ((end - start + step - 1) / step) : 0;
    if (total_iterations <= 0) return;

    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    if (nprocs <= 1 || total_iterations < 100) {
        for (int64_t i = start; i < end; i += step) {
            fn(i, ctx);
        }
        return;
    }

    size_t num_threads = (size_t)nprocs;
    if ((int64_t)num_threads > total_iterations) {
        num_threads = (size_t)total_iterations;
    }

    pthread_t* threads = (pthread_t*)malloc(num_threads * sizeof(pthread_t));
    SlabParallelWorkerArg* args = (SlabParallelWorkerArg*)malloc(num_threads * sizeof(SlabParallelWorkerArg));
    if (!threads || !args) {
        for (int64_t i = start; i < end; i += step) {
            fn(i, ctx);
        }
        if (threads) free(threads);
        if (args) free(args);
        return;
    }

    int64_t iters_per_thread = total_iterations / num_threads;
    int64_t remainder = total_iterations % num_threads;

    int64_t current_iter = 0;
    for (size_t t = 0; t < num_threads; t++) {
        int64_t count = iters_per_thread + (t < (size_t)remainder ? 1 : 0);
        args[t].start = start + current_iter * step;
        args[t].end = args[t].start + count * step;
        args[t].step = step;
        args[t].fn = fn;
        args[t].ctx = ctx;
        current_iter += count;

        pthread_create(&threads[t], NULL, slab_parallel_worker, &args[t]);
    }

    for (size_t t = 0; t < num_threads; t++) {
        pthread_join(threads[t], NULL);
    }

    free(threads);
    free(args);
}

#endif // SLAB_RUNTIME_H
