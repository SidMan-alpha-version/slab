#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "slab_arena.h"
#include "lexer.h"
#include "ast.h"
#include "parser.h"
#include "analyzer.h"
#include "codegen.h"

static void print_usage(const char* prog) {
    fprintf(stderr, "Slab Programming Language Compiler\n");
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s run <file.slb>                 Fast dev build and run\n", prog);
    fprintf(stderr, "  %s build <file.slb> [--pack] -o <out>  Compile standalone native binary\n", prog);
    fprintf(stderr, "  %s emit-c <file.slb> [--pack] -o <out.c> Output generated C23 code\n", prog);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char* command = argv[1];
    if (strcmp(command, "-h") == 0 || strcmp(command, "--help") == 0 || strcmp(command, "help") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    bool cmd_run = (strcmp(command, "run") == 0);
    bool cmd_build = (strcmp(command, "build") == 0);
    bool cmd_emit_c = (strcmp(command, "emit-c") == 0);

    if (!cmd_run && !cmd_build && !cmd_emit_c) {
        fprintf(stderr, "Error: Unknown command '%s'\n", command);
        print_usage(argv[0]);
        return 1;
    }

    const char* input_file = NULL;
    const char* output_file = NULL;
    bool pack_enabled = false;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--pack") == 0) {
            pack_enabled = true;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 < argc) {
                output_file = argv[++i];
            } else {
                fprintf(stderr, "Error: -o requires an argument\n");
                return 1;
            }
        } else if (!input_file && argv[i][0] != '-') {
            input_file = argv[i];
        }
    }

    if (!input_file) {
        fprintf(stderr, "Error: No input file specified\n");
        return 1;
    }

    // Set default output file if needed
    if (!output_file) {
        if (cmd_build) output_file = "a.out";
        else if (cmd_emit_c) output_file = NULL; // stdout
    }

    // Initialize compiler workspace
    SlabWorkspace comp_ws = {0};

    // Read source file
    FILE* f = fopen(input_file, "rb");
    if (!f) {
        fprintf(stderr, "Error: Cannot open input file '%s'\n", input_file);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize < 0) {
        fclose(f);
        fprintf(stderr, "Error: Failed to read file '%s'\n", input_file);
        return 1;
    }

    char* source = (char*)slab_alloc(&comp_ws, (size_t)fsize + 1, 1);
    if (!source) {
        fclose(f);
        fprintf(stderr, "Error: Out of memory reading file\n");
        return 1;
    }
    size_t read_bytes = fread(source, 1, (size_t)fsize, f);
    source[read_bytes] = '\0';
    fclose(f);

    // 1. Lexer & Parser
    Lexer lexer;
    lexer_init(&lexer, source, read_bytes);

    Parser parser;
    parser_init(&parser, &lexer, &comp_ws, input_file);
    Program* program = parser_parse_program(&parser);

    if (parser.had_error) {
        slab_workspace_destroy(&comp_ws);
        return 1;
    }

    // 2. Semantic Analyzer
    Analyzer analyzer;
    analyzer_init(&analyzer, &comp_ws, program, pack_enabled, input_file);
    if (!analyzer_analyze(&analyzer)) {
        slab_workspace_destroy(&comp_ws);
        return 1;
    }

    // 3. Execution / Code Generation
    if (cmd_emit_c) {
        FILE* out = stdout;
        if (output_file) {
            out = fopen(output_file, "w");
            if (!out) {
                fprintf(stderr, "Error: Cannot open output file '%s'\n", output_file);
                slab_workspace_destroy(&comp_ws);
                return 1;
            }
        }
        Codegen cg;
        codegen_init(&cg, &comp_ws, program, pack_enabled, out);
        codegen_generate(&cg);
        if (output_file) fclose(out);
        slab_workspace_destroy(&comp_ws);
        return 0;
    }

    // Find directory containing the slab binary to reliably find include/slab_runtime.h
    char exe_dir[1024] = ".";
    char* last_slash = strrchr(argv[0], '/');
    if (last_slash) {
        size_t dirlen = (size_t)(last_slash - argv[0]);
        if (dirlen < sizeof(exe_dir)) {
            memcpy(exe_dir, argv[0], dirlen);
            exe_dir[dirlen] = '\0';
        }
    }

    if (cmd_build) {
        char temp_c_path[256];
        snprintf(temp_c_path, sizeof(temp_c_path), "_slab_%d.c", getpid());
        FILE* c_out = fopen(temp_c_path, "w");
        if (!c_out) {
            fprintf(stderr, "Error: Cannot create temporary C file\n");
            slab_workspace_destroy(&comp_ws);
            return 1;
        }

        Codegen cg;
        codegen_init(&cg, &comp_ws, program, pack_enabled, c_out);
        codegen_generate(&cg);
        fclose(c_out);

        // Compile with clang
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "clang -std=c11 -O3 -march=native -pthread -Wno-pass-failed -Iinclude -Isrc -I%s/include -I%s/src %s -o %s -lm",
                 exe_dir, exe_dir, temp_c_path, output_file);
        int res = system(cmd);
        remove(temp_c_path);
        slab_workspace_destroy(&comp_ws);
        return res == 0 ? 0 : 1;
    }

    if (cmd_run) {
        char temp_c_path[256];
        char temp_bin_path[256];
        snprintf(temp_c_path, sizeof(temp_c_path), "_slab_run_%d.c", getpid());
        snprintf(temp_bin_path, sizeof(temp_bin_path), "_slab_run_%d", getpid());

        FILE* c_out = fopen(temp_c_path, "w");
        if (!c_out) {
            fprintf(stderr, "Error: Cannot create temporary C file\n");
            slab_workspace_destroy(&comp_ws);
            return 1;
        }

        Codegen cg;
        codegen_init(&cg, &comp_ws, program, pack_enabled, c_out);
        codegen_generate(&cg);
        fclose(c_out);

        // Fast clang invocation
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "clang -std=c11 -O2 -pthread -Wno-pass-failed -Iinclude -Isrc -I%s/include -I%s/src %s -o %s -lm",
                 exe_dir, exe_dir, temp_c_path, temp_bin_path);
        int build_res = system(cmd);
        remove(temp_c_path);

        if (build_res != 0) {
            slab_workspace_destroy(&comp_ws);
            return 1;
        }

        // Run binary
        snprintf(cmd, sizeof(cmd), "./%s", temp_bin_path);
        int run_res = system(cmd);
        remove(temp_bin_path);

        slab_workspace_destroy(&comp_ws);
        return run_res;
    }

    slab_workspace_destroy(&comp_ws);
    return 0;
}
