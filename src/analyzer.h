#ifndef SLAB_ANALYZER_H
#define SLAB_ANALYZER_H

#include "ast.h"
#include <stdbool.h>

typedef struct {
    SlabWorkspace* ws;
    Program* program;
    bool pack_enabled;
    bool had_error;
    const char* filename;
} Analyzer;

void analyzer_init(Analyzer* analyzer, SlabWorkspace* ws, Program* program, bool pack_enabled, const char* filename);
bool analyzer_analyze(Analyzer* analyzer);

#endif // SLAB_ANALYZER_H
