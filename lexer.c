#include "lexer.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

const char* token_type_name(TokenType type) {
    switch (type) {
        case TOKEN_EOF: return "EOF";
        case TOKEN_NEWLINE: return "NEWLINE";
        case TOKEN_INDENT: return "INDENT";
        case TOKEN_DEDENT: return "DEDENT";
        case TOKEN_IDENT: return "IDENT";
        case TOKEN_INT_LIT: return "INT_LIT";
        case TOKEN_FLOAT_LIT: return "FLOAT_LIT";
        case TOKEN_STRING_LIT: return "STRING_LIT";
        case TOKEN_IMPORT: return "import";
        case TOKEN_EXTERN: return "extern";
        case TOKEN_CONST: return "const";
        case TOKEN_RECORD: return "record";
        case TOKEN_FN: return "fn";
        case TOKEN_WORKSPACE: return "workspace";
        case TOKEN_RESET: return "reset";
        case TOKEN_FOR: return "for";
        case TOKEN_PARALLEL: return "parallel";
        case TOKEN_IN: return "in";
        case TOKEN_WHILE: return "while";
        case TOKEN_DO: return "do";
        case TOKEN_UNTIL: return "until";
        case TOKEN_IF: return "if";
        case TOKEN_ELSE: return "else";
        case TOKEN_SORT: return "sort";
        case TOKEN_BY: return "by";
        case TOKEN_ASC: return "asc";
        case TOKEN_DESC: return "desc";
        case TOKEN_SAVE: return "save";
        case TOKEN_TO: return "to";
        case TOKEN_STATE: return "state";
        case TOKEN_RETURN: return "return";
        case TOKEN_SIGMA: return "sigma";
        case TOKEN_FROM: return "from";
        case TOKEN_LIMIT: return "limit";
        case TOKEN_INTEGRAL: return "integral";
        case TOKEN_ARC: return "arc";
        case TOKEN_SEARCH: return "search";
        case TOKEN_OR: return "or";
        case TOKEN_LOAD: return "load";
        case TOKEN_READ_TEXT: return "read_text";
        case TOKEN_ETCH: return "etch";
        case TOKEN_MATH: return "math";
        case TOKEN_TRUE: return "true";
        case TOKEN_FALSE: return "false";
        case TOKEN_TYPE_INT: return "int";
        case TOKEN_TYPE_I32: return "i32";
        case TOKEN_TYPE_U16: return "u16";
        case TOKEN_TYPE_U32: return "u32";
        case TOKEN_TYPE_F32: return "f32";
        case TOKEN_TYPE_F64: return "f64";
        case TOKEN_TYPE_BOOL: return "bool";
        case TOKEN_TYPE_STR: return "str";
        case TOKEN_TYPE_ARRAY: return "array";
        case TOKEN_TYPE_TABLE: return "table";
        case TOKEN_TYPE_LIST: return "list";
        case TOKEN_PLUS: return "+";
        case TOKEN_MINUS: return "-";
        case TOKEN_STAR: return "*";
        case TOKEN_SLASH: return "/";
        case TOKEN_PERCENT: return "%";
        case TOKEN_AT: return "@";
        case TOKEN_CARET: return "^";
        case TOKEN_OR_OR: return "||";
        case TOKEN_AND_AND: return "&&";
        case TOKEN_EQ_EQ: return "==";
        case TOKEN_BANG_EQ: return "!=";
        case TOKEN_LT: return "<";
        case TOKEN_LT_EQ: return "<=";
        case TOKEN_GT: return ">";
        case TOKEN_GT_EQ: return ">=";
        case TOKEN_BANG: return "!";
        case TOKEN_EQ: return "=";
        case TOKEN_ARROW: return "->";
        case TOKEN_DOT_DOT: return "..";
        case TOKEN_COLON: return ":";
        case TOKEN_COMMA: return ",";
        case TOKEN_DOT: return ".";
        case TOKEN_LPAREN: return "(";
        case TOKEN_RPAREN: return ")";
        case TOKEN_LBRACKET: return "[";
        case TOKEN_RBRACKET: return "]";
        case TOKEN_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

void lexer_init(Lexer* lexer, const char* source, size_t length) {
    lexer->source = source;
    lexer->source_len = length;
    lexer->current = source;
    lexer->token_start = source;
    lexer->line = 1;
    lexer->col = 1;
    lexer->token_line = 1;
    lexer->token_col = 1;

    lexer->indent_stack[0] = 0;
    lexer->indent_depth = 0;
    lexer->pending_dedents = 0;
    lexer->at_line_start = true;
    lexer->paren_depth = 0;
    lexer->eof_handled = false;
    lexer->last_emitted_type = TOKEN_NEWLINE;
}

static inline bool is_at_end(Lexer* lexer) {
    return (size_t)(lexer->current - lexer->source) >= lexer->source_len || *lexer->current == '\0';
}

static inline char peek(Lexer* lexer) {
    if (is_at_end(lexer)) return '\0';
    return *lexer->current;
}

static inline char peek_next(Lexer* lexer) {
    if ((size_t)(lexer->current - lexer->source) + 1 >= lexer->source_len) return '\0';
    return lexer->current[1];
}

static inline char advance(Lexer* lexer) {
    char c = *lexer->current++;
    lexer->col++;
    return c;
}

static inline bool match(Lexer* lexer, char expected) {
    if (is_at_end(lexer) || *lexer->current != expected) return false;
    lexer->current++;
    lexer->col++;
    return true;
}

static Token make_token(Lexer* lexer, TokenType type) {
    Token token;
    token.type = type;
    token.start = lexer->token_start;
    token.length = (size_t)(lexer->current - lexer->token_start);
    token.line = lexer->token_line;
    token.col = lexer->token_col;
    lexer->last_emitted_type = type;
    return token;
}

static Token make_error(Lexer* lexer, const char* message) {
    Token token;
    token.type = TOKEN_ERROR;
    token.start = message;
    token.length = strlen(message);
    token.line = lexer->token_line;
    token.col = lexer->token_col;
    lexer->last_emitted_type = TOKEN_ERROR;
    return token;
}

static TokenType check_keyword(const char* str, size_t len) {
    switch (len) {
        case 2:
            if (memcmp(str, "fn", 2) == 0) return TOKEN_FN;
            if (memcmp(str, "if", 2) == 0) return TOKEN_IF;
            if (memcmp(str, "in", 2) == 0) return TOKEN_IN;
            if (memcmp(str, "do", 2) == 0) return TOKEN_DO;
            if (memcmp(str, "by", 2) == 0) return TOKEN_BY;
            if (memcmp(str, "to", 2) == 0) return TOKEN_TO;
            if (memcmp(str, "or", 2) == 0) return TOKEN_OR;
            break;
        case 3:
            if (memcmp(str, "for", 3) == 0) return TOKEN_FOR;
            if (memcmp(str, "asc", 3) == 0) return TOKEN_ASC;
            if (memcmp(str, "arc", 3) == 0) return TOKEN_ARC;
            if (memcmp(str, "int", 3) == 0) return TOKEN_TYPE_INT;
            if (memcmp(str, "i32", 3) == 0) return TOKEN_TYPE_I32;
            if (memcmp(str, "u16", 3) == 0) return TOKEN_TYPE_U16;
            if (memcmp(str, "u32", 3) == 0) return TOKEN_TYPE_U32;
            if (memcmp(str, "f32", 3) == 0) return TOKEN_TYPE_F32;
            if (memcmp(str, "f64", 3) == 0) return TOKEN_TYPE_F64;
            if (memcmp(str, "str", 3) == 0) return TOKEN_TYPE_STR;
            break;
        case 4:
            if (memcmp(str, "else", 4) == 0) return TOKEN_ELSE;
            if (memcmp(str, "sort", 4) == 0) return TOKEN_SORT;
            if (memcmp(str, "desc", 4) == 0) return TOKEN_DESC;
            if (memcmp(str, "save", 4) == 0) return TOKEN_SAVE;
            if (memcmp(str, "load", 4) == 0) return TOKEN_LOAD;
            if (memcmp(str, "from", 4) == 0) return TOKEN_FROM;
            if (memcmp(str, "math", 4) == 0) return TOKEN_MATH;
            if (memcmp(str, "true", 4) == 0) return TOKEN_TRUE;
            if (memcmp(str, "bool", 4) == 0) return TOKEN_TYPE_BOOL;
            if (memcmp(str, "list", 4) == 0) return TOKEN_TYPE_LIST;
            if (memcmp(str, "etch", 4) == 0) return TOKEN_ETCH;
            break;
        case 5:
            if (memcmp(str, "const", 5) == 0) return TOKEN_CONST;
            if (memcmp(str, "reset", 5) == 0) return TOKEN_RESET;
            if (memcmp(str, "while", 5) == 0) return TOKEN_WHILE;
            if (memcmp(str, "until", 5) == 0) return TOKEN_UNTIL;
            if (memcmp(str, "state", 5) == 0) return TOKEN_STATE;
            if (memcmp(str, "sigma", 5) == 0) return TOKEN_SIGMA;
            if (memcmp(str, "limit", 5) == 0) return TOKEN_LIMIT;
            if (memcmp(str, "false", 5) == 0) return TOKEN_FALSE;
            if (memcmp(str, "array", 5) == 0) return TOKEN_TYPE_ARRAY;
            if (memcmp(str, "table", 5) == 0) return TOKEN_TYPE_TABLE;
            break;
        case 6:
            if (memcmp(str, "import", 6) == 0) return TOKEN_IMPORT;
            if (memcmp(str, "extern", 6) == 0) return TOKEN_EXTERN;
            if (memcmp(str, "record", 6) == 0) return TOKEN_RECORD;
            if (memcmp(str, "return", 6) == 0) return TOKEN_RETURN;
            if (memcmp(str, "search", 6) == 0) return TOKEN_SEARCH;
            if (memcmp(str, "intern", 6) == 0) return TOKEN_ETCH;
            break;
        case 8:
            if (memcmp(str, "integral", 8) == 0) return TOKEN_INTEGRAL;
            if (memcmp(str, "parallel", 8) == 0) return TOKEN_PARALLEL;
            break;
        case 9:
            if (memcmp(str, "workspace", 9) == 0) return TOKEN_WORKSPACE;
            if (memcmp(str, "read_text", 9) == 0) return TOKEN_READ_TEXT;
            break;
        default:
            break;
    }
    return TOKEN_IDENT;
}

Token lexer_next_token(Lexer* lexer) {
    if (lexer->pending_dedents > 0) {
        lexer->pending_dedents--;
        Token t;
        t.type = TOKEN_DEDENT;
        t.start = lexer->current;
        t.length = 0;
        t.line = lexer->line;
        t.col = lexer->col;
        lexer->last_emitted_type = TOKEN_DEDENT;
        return t;
    }

    while (!is_at_end(lexer)) {
        if (lexer->at_line_start && lexer->paren_depth == 0) {
            int spaces = 0;
            const char* probe = lexer->current;
            int probe_col = lexer->col;
            while (*probe == ' ' || *probe == '\t') {
                if (*probe == ' ') {
                    spaces += 1;
                    probe_col += 1;
                } else {
                    int next_tab = (spaces + 4) & ~3;
                    spaces = next_tab;
                    probe_col = next_tab + 1;
                }
                probe++;
            }

            // If empty line or comment-only line, skip without changing indent
            if (*probe == '\n' || *probe == '\r' || *probe == '#' || *probe == '\0') {
                lexer->current = probe;
                lexer->col = probe_col;
                if (*lexer->current == '#') {
                    while (!is_at_end(lexer) && *lexer->current != '\n') {
                        lexer->current++;
                        lexer->col++;
                    }
                }
                if (!is_at_end(lexer) && (*lexer->current == '\n' || *lexer->current == '\r')) {
                    if (*lexer->current == '\r' && peek_next(lexer) == '\n') lexer->current++;
                    lexer->current++;
                    lexer->line++;
                    lexer->col = 1;
                }
                continue;
            }

            lexer->current = probe;
            lexer->col = probe_col;
            lexer->at_line_start = false;

            int current_indent = lexer->indent_stack[lexer->indent_depth];
            if (spaces > current_indent) {
                if (lexer->indent_depth + 1 >= SLAB_MAX_INDENT_DEPTH) {
                    return make_error(lexer, "Indentation stack overflow");
                }
                lexer->indent_depth++;
                lexer->indent_stack[lexer->indent_depth] = spaces;
                Token t;
                t.type = TOKEN_INDENT;
                t.start = lexer->current;
                t.length = 0;
                t.line = lexer->line;
                t.col = lexer->col;
                lexer->last_emitted_type = TOKEN_INDENT;
                return t;
            } else if (spaces < current_indent) {
                int target_depth = -1;
                for (int d = lexer->indent_depth - 1; d >= 0; d--) {
                    if (lexer->indent_stack[d] == spaces) {
                        target_depth = d;
                        break;
                    }
                }
                if (target_depth == -1) {
                    return make_error(lexer, "IndentationError: unindent does not match any outer indentation level");
                }
                int dedents = lexer->indent_depth - target_depth;
                lexer->indent_depth = target_depth;
                lexer->pending_dedents = dedents - 1;

                Token t;
                t.type = TOKEN_DEDENT;
                t.start = lexer->current;
                t.length = 0;
                t.line = lexer->line;
                t.col = lexer->col;
                lexer->last_emitted_type = TOKEN_DEDENT;
                return t;
            }
        }

        // Skip horizontal whitespace
        while (*lexer->current == ' ' || *lexer->current == '\t') {
            advance(lexer);
        }

        // Comments
        if (*lexer->current == '#') {
            while (!is_at_end(lexer) && *lexer->current != '\n') {
                advance(lexer);
            }
            continue;
        }

        // Newlines
        if (*lexer->current == '\n' || *lexer->current == '\r') {
            lexer->token_start = lexer->current;
            lexer->token_line = lexer->line;
            lexer->token_col = lexer->col;

            if (*lexer->current == '\r' && peek_next(lexer) == '\n') advance(lexer);
            advance(lexer);
            lexer->line++;
            lexer->col = 1;

            if (lexer->paren_depth > 0) {
                // Newline inside parens/brackets is ignored
                continue;
            }

            lexer->at_line_start = true;
            if (lexer->last_emitted_type != TOKEN_NEWLINE &&
                lexer->last_emitted_type != TOKEN_INDENT &&
                lexer->last_emitted_type != TOKEN_DEDENT) {
                return make_token(lexer, TOKEN_NEWLINE);
            }
            continue;
        }

        // Check for end of file
        if (is_at_end(lexer)) break;

        lexer->token_start = lexer->current;
        lexer->token_line = lexer->line;
        lexer->token_col = lexer->col;

        char c = advance(lexer);

        // Identifiers and keywords
        if (isalpha((unsigned char)c) || c == '_') {
            while (isalnum((unsigned char)peek(lexer)) || peek(lexer) == '_') {
                advance(lexer);
            }
            size_t len = (size_t)(lexer->current - lexer->token_start);
            TokenType type = check_keyword(lexer->token_start, len);
            Token t = make_token(lexer, type);
            if (type == TOKEN_TRUE) t.int_val = 1;
            else if (type == TOKEN_FALSE) t.int_val = 0;
            return t;
        }

        // Numbers
        if (isdigit((unsigned char)c)) {
            bool is_float = false;
            while (isdigit((unsigned char)peek(lexer))) {
                advance(lexer);
            }
            // Check for decimal dot (ensure it's not `..` range)
            if (peek(lexer) == '.' && peek_next(lexer) != '.') {
                is_float = true;
                advance(lexer); // consume '.'
                while (isdigit((unsigned char)peek(lexer))) {
                    advance(lexer);
                }
            }
            // Exponent part: e.g. 1e-4, 2E+3
            if (peek(lexer) == 'e' || peek(lexer) == 'E') {
                is_float = true;
                advance(lexer);
                if (peek(lexer) == '+' || peek(lexer) == '-') {
                    advance(lexer);
                }
                while (isdigit((unsigned char)peek(lexer))) {
                    advance(lexer);
                }
            }

            Token t = make_token(lexer, is_float ? TOKEN_FLOAT_LIT : TOKEN_INT_LIT);
            char buf[64];
            size_t len = t.length < 63 ? t.length : 63;
            memcpy(buf, t.start, len);
            buf[len] = '\0';
            if (is_float) {
                t.float_val = strtod(buf, NULL);
            } else {
                t.int_val = strtoll(buf, NULL, 10);
            }
            return t;
        }

        // Strings
        if (c == '"' || c == '\'') {
            char quote = c;
            const char* str_content = lexer->current;
            while (!is_at_end(lexer) && peek(lexer) != quote) {
                if (peek(lexer) == '\\' && peek_next(lexer) != '\0') {
                    advance(lexer);
                }
                advance(lexer);
            }
            if (is_at_end(lexer)) {
                return make_error(lexer, "Unterminated string literal");
            }
            size_t content_len = (size_t)(lexer->current - str_content);
            advance(lexer); // consume closing quote

            Token t = make_token(lexer, TOKEN_STRING_LIT);
            t.str_val.chars = str_content;
            t.str_val.len = content_len;
            return t;
        }

        // Operators & delimiters
        switch (c) {
            case '+':
                if (peek(lexer) == '=') {
                    advance(lexer);
                    return make_error(lexer, "Slab rejects '+=': pure math syntax requires 'x = x + ...'");
                }
                return make_token(lexer, TOKEN_PLUS);
            case '-':
                if (peek(lexer) == '=') {
                    advance(lexer);
                    return make_error(lexer, "Slab rejects '-=': pure math syntax requires 'x = x - ...'");
                }
                if (match(lexer, '>')) return make_token(lexer, TOKEN_ARROW);
                return make_token(lexer, TOKEN_MINUS);
            case '*':
                if (peek(lexer) == '=') {
                    advance(lexer);
                    return make_error(lexer, "Slab rejects '*=': pure math syntax requires 'x = x * ...'");
                }
                return make_token(lexer, TOKEN_STAR);
            case '/':
                if (peek(lexer) == '=') {
                    advance(lexer);
                    return make_error(lexer, "Slab rejects '/=': pure math syntax requires 'x = x / ...'");
                }
                return make_token(lexer, TOKEN_SLASH);
            case '%':
                if (peek(lexer) == '=') {
                    advance(lexer);
                    return make_error(lexer, "Slab rejects '%=': pure math syntax requires 'x = x % ...'");
                }
                return make_token(lexer, TOKEN_PERCENT);
            case '@': return make_token(lexer, TOKEN_AT);
            case '^': return make_token(lexer, TOKEN_CARET);

            case '=':
                if (match(lexer, '=')) return make_token(lexer, TOKEN_EQ_EQ);
                return make_token(lexer, TOKEN_EQ);
            case '!':
                if (match(lexer, '=')) return make_token(lexer, TOKEN_BANG_EQ);
                return make_token(lexer, TOKEN_BANG);
            case '<':
                if (match(lexer, '=')) return make_token(lexer, TOKEN_LT_EQ);
                return make_token(lexer, TOKEN_LT);
            case '>':
                if (match(lexer, '=')) return make_token(lexer, TOKEN_GT_EQ);
                return make_token(lexer, TOKEN_GT);
            case '|':
                if (match(lexer, '|')) return make_token(lexer, TOKEN_OR_OR);
                return make_error(lexer, "Unexpected character '|', expected '||'");
            case '&':
                if (match(lexer, '&')) return make_token(lexer, TOKEN_AND_AND);
                return make_error(lexer, "Unexpected character '&', expected '&&'");

            case '.':
                if (match(lexer, '.')) return make_token(lexer, TOKEN_DOT_DOT);
                return make_token(lexer, TOKEN_DOT);
            case ':': return make_token(lexer, TOKEN_COLON);
            case ',': return make_token(lexer, TOKEN_COMMA);
            case '(':
                lexer->paren_depth++;
                return make_token(lexer, TOKEN_LPAREN);
            case ')':
                if (lexer->paren_depth > 0) lexer->paren_depth--;
                return make_token(lexer, TOKEN_RPAREN);
            case '[':
                lexer->paren_depth++;
                return make_token(lexer, TOKEN_LBRACKET);
            case ']':
                if (lexer->paren_depth > 0) lexer->paren_depth--;
                return make_token(lexer, TOKEN_RBRACKET);
            default:
                return make_error(lexer, "Unexpected character");
        }
    }

    // Handle end-of-file dedents
    if (lexer->last_emitted_type != TOKEN_NEWLINE &&
        lexer->last_emitted_type != TOKEN_DEDENT &&
        lexer->last_emitted_type != TOKEN_EOF) {
        lexer->token_start = lexer->current;
        lexer->token_line = lexer->line;
        lexer->token_col = lexer->col;
        return make_token(lexer, TOKEN_NEWLINE);
    }

    if (lexer->indent_depth > 0) {
        lexer->indent_depth--;
        Token t;
        t.type = TOKEN_DEDENT;
        t.start = lexer->current;
        t.length = 0;
        t.line = lexer->line;
        t.col = lexer->col;
        lexer->last_emitted_type = TOKEN_DEDENT;
        return t;
    }

    Token eof;
    eof.type = TOKEN_EOF;
    eof.start = lexer->current;
    eof.length = 0;
    eof.line = lexer->line;
    eof.col = lexer->col;
    lexer->last_emitted_type = TOKEN_EOF;
    return eof;
}
