#ifndef SLAB_ARENA_H
#define SLAB_ARENA_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdalign.h>

#define SLAB_DEFAULT_PAGE_SIZE 65536

typedef struct SlabPage {
    struct SlabPage* next;
    size_t capacity;
    size_t used;
    alignas(64) uint8_t data[]; // 64-byte aligned for AVX2/AVX-512 SIMD
} SlabPage;

typedef struct {
    SlabPage* page;
    size_t used;
} SlabCheckpoint;

typedef struct {
    SlabPage* head;
    SlabPage* current;
} SlabWorkspace;

typedef struct {
    const char* ptr;
    size_t len;
} SlabStr;

static inline SlabPage* slab_page_create(size_t min_capacity) {
    size_t cap = SLAB_DEFAULT_PAGE_SIZE;
    if (min_capacity > cap) {
        cap = min_capacity * 2;
    }
    cap = (cap + 63) & ~((size_t)63);
    size_t total = sizeof(SlabPage) + cap;
    total = (total + 63) & ~((size_t)63);

    void* mem = NULL;
#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L || defined(__APPLE__)
    if (posix_memalign(&mem, 64, total) != 0) {
        return NULL;
    }
#else
    mem = aligned_alloc(64, total);
    if (!mem) return NULL;
#endif

    SlabPage* page = (SlabPage*)mem;
    page->next = NULL;
    page->capacity = cap;
    page->used = 0;
    return page;
}

static inline void* slab_alloc(SlabWorkspace* ws, size_t size, size_t align) {
    if (!ws) return NULL;
    if (align == 0) align = 8;
    // Align up requested size
    size = (size + (align - 1)) & ~(align - 1);

    if (!ws->head) {
        ws->head = slab_page_create(size);
        ws->current = ws->head;
        if (!ws->head) return NULL;
    }

    uintptr_t current_addr = (uintptr_t)(ws->current->data + ws->current->used);
    uintptr_t aligned_addr = (current_addr + (align - 1)) & ~(align - 1);
    size_t needed = (aligned_addr - (uintptr_t)ws->current->data) + size;

    if (needed <= ws->current->capacity) {
        ws->current->used = needed;
        return (void*)aligned_addr;
    }

    // Need a new page
    SlabPage* new_page = slab_page_create(size);
    if (!new_page) return NULL;
    ws->current->next = new_page;
    ws->current = new_page;

    uintptr_t base_addr = (uintptr_t)ws->current->data;
    uintptr_t new_aligned = (base_addr + (align - 1)) & ~(align - 1);
    ws->current->used = (new_aligned - base_addr) + size;
    return (void*)new_aligned;
}

static inline SlabCheckpoint slab_save(SlabWorkspace* ws) {
    SlabCheckpoint cp;
    cp.page = ws ? ws->current : NULL;
    cp.used = (ws && ws->current) ? ws->current->used : 0;
    return cp;
}

static inline void slab_restore(SlabWorkspace* ws, SlabCheckpoint cp) {
    if (!ws) return;
    if (!cp.page) {
        // Reset to initial head
        if (ws->head) {
            SlabPage* p = ws->head->next;
            while (p) {
                SlabPage* next = p->next;
                free(p);
                p = next;
            }
            ws->head->next = NULL;
            ws->head->used = 0;
            ws->current = ws->head;
        }
        return;
    }

    // Free any pages allocated after cp.page
    SlabPage* p = cp.page->next;
    while (p) {
        SlabPage* next = p->next;
        free(p);
        p = next;
    }
    cp.page->next = NULL;
    ws->current = cp.page;
    ws->current->used = cp.used;
}

static inline void slab_workspace_destroy(SlabWorkspace* ws) {
    if (!ws) return;
    SlabPage* p = ws->head;
    while (p) {
        SlabPage* next = p->next;
        free(p);
        p = next;
    }
    ws->head = NULL;
    ws->current = NULL;
}

static inline SlabStr slab_str_from_cstr(SlabWorkspace* ws, const char* s) {
    SlabStr str;
    if (!s) { str.ptr = ""; str.len = 0; return str; }
    str.len = strlen(s);
    char* buf = (char*)slab_alloc(ws, str.len + 1, 1);
    if (buf) {
        memcpy(buf, s, str.len);
        buf[str.len] = '\0';
        str.ptr = buf;
    } else {
        str.ptr = "";
        str.len = 0;
    }
    return str;
}

#endif // SLAB_ARENA_H
