#ifndef SLAB_TOKEN_H
#define SLAB_TOKEN_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    // End of file & Layout
    TOKEN_EOF = 0,
    TOKEN_NEWLINE,
    TOKEN_INDENT,
    TOKEN_DEDENT,

    // Identifiers & Literals
    TOKEN_IDENT,
    TOKEN_INT_LIT,
    TOKEN_FLOAT_LIT,
    TOKEN_STRING_LIT,

    // Keywords - Declarations & Structure
    TOKEN_IMPORT,
    TOKEN_EXTERN,
    TOKEN_CONST,
    TOKEN_RECORD,
    TOKEN_FN,
    TOKEN_WORKSPACE,
    TOKEN_RESET,

    // Keywords - Control Flow
    TOKEN_FOR,
    TOKEN_PARALLEL,
    TOKEN_IN,
    TOKEN_WHILE,
    TOKEN_DO,
    TOKEN_UNTIL,
    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_SORT,
    TOKEN_BY,
    TOKEN_ASC,
    TOKEN_DESC,
    TOKEN_SAVE,
    TOKEN_TO,
    TOKEN_STATE,
    TOKEN_RETURN,

    // Keywords - Math & Built-ins
    TOKEN_SIGMA,
    TOKEN_FROM,
    TOKEN_LIMIT,
    TOKEN_INTEGRAL,
    TOKEN_ARC,
    TOKEN_SEARCH,
    TOKEN_OR,
    TOKEN_LOAD,
    TOKEN_READ_TEXT,
    TOKEN_ETCH,
    TOKEN_MATH,
    TOKEN_TRUE,
    TOKEN_FALSE,

    // Alias for backwards compatibility
#define TOKEN_INTERN TOKEN_ETCH

    // Type Keywords
    TOKEN_TYPE_INT,
    TOKEN_TYPE_I32,
    TOKEN_TYPE_U16,
    TOKEN_TYPE_U32,
    TOKEN_TYPE_F32,
    TOKEN_TYPE_F64,
    TOKEN_TYPE_BOOL,
    TOKEN_TYPE_STR,
    TOKEN_TYPE_ARRAY,
    TOKEN_TYPE_TABLE,
    TOKEN_TYPE_LIST,

    // Operators
    TOKEN_PLUS,       // +
    TOKEN_MINUS,      // -
    TOKEN_STAR,       // *
    TOKEN_SLASH,      // /
    TOKEN_PERCENT,    // %
    TOKEN_AT,         // @ (dot / matrix multiply)
    TOKEN_CARET,      // ^ (exponentiation)
    TOKEN_OR_OR,      // ||
    TOKEN_AND_AND,    // &&
    TOKEN_EQ_EQ,      // ==
    TOKEN_BANG_EQ,    // !=
    TOKEN_LT,         // <
    TOKEN_LT_EQ,      // <=
    TOKEN_GT,         // >
    TOKEN_GT_EQ,      // >=
    TOKEN_BANG,       // !
    TOKEN_EQ,         // =
    TOKEN_ARROW,      // ->
    TOKEN_DOT_DOT,    // ..

    // Delimiters
    TOKEN_COLON,      // :
    TOKEN_COMMA,      // ,
    TOKEN_DOT,        // .
    TOKEN_LPAREN,     // (
    TOKEN_RPAREN,     // )
    TOKEN_LBRACKET,   // [
    TOKEN_RBRACKET,   // ]

    // Invalid / Error
    TOKEN_ERROR
} TokenType;

typedef struct {
    TokenType type;
    const char* start;
    size_t length;
    int line;
    int col;

    // Parsed literal values for convenience
    union {
        int64_t int_val;
        double float_val;
        struct {
            const char* chars;
            size_t len;
        } str_val;
    };
} Token;

const char* token_type_name(TokenType type);

#endif // SLAB_TOKEN_H
