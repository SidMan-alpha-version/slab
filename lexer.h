#ifndef SLAB_LEXER_H
#define SLAB_LEXER_H

#include "token.h"

#define SLAB_MAX_INDENT_DEPTH 256

typedef struct {
    const char* source;
    size_t source_len;
    const char* current;
    const char* token_start;
    int line;
    int col;
    int token_line;
    int token_col;

    int indent_stack[SLAB_MAX_INDENT_DEPTH];
    int indent_depth;
    int pending_dedents;
    bool at_line_start;
    int paren_depth;
    bool eof_handled;
    TokenType last_emitted_type;
} Lexer;

void lexer_init(Lexer* lexer, const char* source, size_t length);
Token lexer_next_token(Lexer* lexer);

#endif // SLAB_LEXER_H
