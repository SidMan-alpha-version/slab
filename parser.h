#ifndef SLAB_PARSER_H
#define SLAB_PARSER_H

#include "ast.h"
#include "lexer.h"

typedef struct {
    Lexer* lexer;
    Token current;
    Token previous;
    SlabWorkspace* ws;
    bool had_error;
    bool panic_mode;
    const char* filename;
} Parser;

void parser_init(Parser* parser, Lexer* lexer, SlabWorkspace* ws, const char* filename);
Program* parser_parse_program(Parser* parser);

#endif // SLAB_PARSER_H
