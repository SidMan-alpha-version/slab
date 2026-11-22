#ifndef SLAB_CODEGEN_H
#define SLAB_CODEGEN_H

#include "ast.h"
#include <stdio.h>
#include <stdbool.h>

typedef struct {
    SlabWorkspace* ws;
    Program* program;
    bool pack_enabled;
    FILE* out;
    int indent_level;
    int temp_var_counter;
    size_t parallel_counter;
    const char* current_ws_var;
} Codegen;

void codegen_init(Codegen* cg, SlabWorkspace* ws, Program* program, bool pack_enabled, FILE* out);
bool codegen_generate(Codegen* cg);

#endif // SLAB_CODEGEN_H
