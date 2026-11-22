#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void error_at(Parser* parser, Token* token, const char* message) {
    if (parser->panic_mode) return;
    parser->panic_mode = true;
    parser->had_error = true;
    fprintf(stderr, "Error [%s line %d:col %d]: %s",
            parser->filename ? parser->filename : "<input>",
            token->line, token->col, message);
    if (token->type == TOKEN_ERROR) {
        fprintf(stderr, " (%.*s)\n", (int)token->length, token->start);
    } else if (token->type == TOKEN_EOF) {
        fprintf(stderr, " at end of file\n");
    } else {
        fprintf(stderr, " at '%.*s'\n", (int)token->length, token->start);
    }
}

static void advance(Parser* parser) {
    parser->previous = parser->current;
    for (;;) {
        parser->current = lexer_next_token(parser->lexer);
        if (parser->current.type != TOKEN_ERROR) break;
        error_at(parser, &parser->current, parser->current.start);
    }
}

static bool check(Parser* parser, TokenType type) {
    return parser->current.type == type;
}

static bool match(Parser* parser, TokenType type) {
    if (!check(parser, type)) return false;
    advance(parser);
    return true;
}

static void consume(Parser* parser, TokenType type, const char* message) {
    if (parser->current.type == type) {
        advance(parser);
        return;
    }
    error_at(parser, &parser->current, message);
}

static void skip_newlines(Parser* parser) {
    while (parser->current.type == TOKEN_NEWLINE) {
        advance(parser);
    }
}

static const char* copy_token_string(Parser* parser, Token* token) {
    char* str = (char*)slab_alloc(parser->ws, token->length + 1, 1);
    if (str && token->length > 0) {
        memcpy(str, token->start, token->length);
        str[token->length] = '\0';
    } else if (str) {
        str[0] = '\0';
    }
    return str;
}

static const char* copy_str_literal(Parser* parser, Token* token) {
    char* str = (char*)slab_alloc(parser->ws, token->str_val.len + 1, 1);
    if (str && token->str_val.len > 0) {
        // Handle escapes
        size_t j = 0;
        for (size_t i = 0; i < token->str_val.len; i++) {
            if (token->str_val.chars[i] == '\\' && i + 1 < token->str_val.len) {
                i++;
                switch (token->str_val.chars[i]) {
                    case 'n': str[j++] = '\n'; break;
                    case 't': str[j++] = '\t'; break;
                    case 'r': str[j++] = '\r'; break;
                    case '\\': str[j++] = '\\'; break;
                    case '"': str[j++] = '"'; break;
                    case '\'': str[j++] = '\''; break;
                    default: str[j++] = token->str_val.chars[i]; break;
                }
            } else {
                str[j++] = token->str_val.chars[i];
            }
        }
        str[j] = '\0';
    } else if (str) {
        str[0] = '\0';
    }
    return str;
}

void parser_init(Parser* parser, Lexer* lexer, SlabWorkspace* ws, const char* filename) {
    parser->lexer = lexer;
    parser->ws = ws;
    parser->filename = filename;
    parser->had_error = false;
    parser->panic_mode = false;
    advance(parser);
}

// Forward declarations
static Expr* parse_expr(Parser* parser);
static Block* parse_block(Parser* parser);
static Stmt* parse_stmt(Parser* parser);
static TypeSpec* parse_type_spec(Parser* parser);

// Type parsing
static TypeSpec* parse_type_spec(Parser* parser) {
    TypeSpec* type = NULL;

    if (match(parser, TOKEN_TYPE_INT)) {
        type = ast_create_type(parser->ws, TYPE_KIND_INT);
        if (match(parser, TOKEN_LBRACKET)) {
            consume(parser, TOKEN_INT_LIT, "Expected integer lower bound");
            type->int_min = parser->previous.int_val;
            consume(parser, TOKEN_DOT_DOT, "Expected '..' in integer range");
            consume(parser, TOKEN_INT_LIT, "Expected integer upper bound");
            type->int_max = parser->previous.int_val;
            consume(parser, TOKEN_RBRACKET, "Expected ']' after integer range");
            type->has_bounds = true;
        }
    } else if (match(parser, TOKEN_TYPE_I32)) {
        type = ast_create_type(parser->ws, TYPE_KIND_I32);
    } else if (match(parser, TOKEN_TYPE_U16)) {
        type = ast_create_type(parser->ws, TYPE_KIND_U16);
    } else if (match(parser, TOKEN_TYPE_U32)) {
        type = ast_create_type(parser->ws, TYPE_KIND_U32);
    } else if (match(parser, TOKEN_TYPE_F32)) {
        type = ast_create_type(parser->ws, TYPE_KIND_F32);
    } else if (match(parser, TOKEN_TYPE_F64)) {
        type = ast_create_type(parser->ws, TYPE_KIND_F64);
    } else if (match(parser, TOKEN_TYPE_BOOL)) {
        type = ast_create_type(parser->ws, TYPE_KIND_BOOL);
    } else if (match(parser, TOKEN_TYPE_STR)) {
        type = ast_create_type(parser->ws, TYPE_KIND_STR);
    } else if (match(parser, TOKEN_TYPE_ARRAY)) {
        type = ast_create_type(parser->ws, TYPE_KIND_ARRAY);
        consume(parser, TOKEN_LT, "Expected '<' after 'array'");
        consume(parser, TOKEN_IDENT, "Expected record type identifier inside array<...>");
        type->custom_name = copy_token_string(parser, &parser->previous);
        consume(parser, TOKEN_GT, "Expected '>' after record type");
        if (match(parser, TOKEN_LBRACKET)) {
            type->capacity_expr = parse_expr(parser);
            consume(parser, TOKEN_RBRACKET, "Expected ']' after array capacity");
        }
    } else if (match(parser, TOKEN_TYPE_TABLE)) {
        type = ast_create_type(parser->ws, TYPE_KIND_TABLE);
        consume(parser, TOKEN_LT, "Expected '<' after 'table'");
        consume(parser, TOKEN_IDENT, "Expected record type identifier inside table<...>");
        type->custom_name = copy_token_string(parser, &parser->previous);
        consume(parser, TOKEN_GT, "Expected '>' after record type");
        if (match(parser, TOKEN_LBRACKET)) {
            type->capacity_expr = parse_expr(parser);
            consume(parser, TOKEN_RBRACKET, "Expected ']' after table capacity");
        }
    } else if (match(parser, TOKEN_TYPE_LIST)) {
        type = ast_create_type(parser->ws, TYPE_KIND_LIST);
        consume(parser, TOKEN_LT, "Expected '<' after 'list'");
        type->elem_type = parse_type_spec(parser);
        consume(parser, TOKEN_GT, "Expected '>' after element type");
        if (match(parser, TOKEN_LBRACKET)) {
            type->capacity_expr = parse_expr(parser);
            consume(parser, TOKEN_RBRACKET, "Expected ']' after list capacity");
        }
    } else if (match(parser, TOKEN_IDENT)) {
        type = ast_create_type(parser->ws, TYPE_KIND_CUSTOM);
        type->custom_name = copy_token_string(parser, &parser->previous);
    } else {
        error_at(parser, &parser->current, "Expected type specification");
        type = ast_create_type(parser->ws, TYPE_KIND_VOID);
    }
    return type;
}

// Expression parsing
static Expr* parse_primary(Parser* parser) {
    int line = parser->current.line;
    int col = parser->current.col;

    if (match(parser, TOKEN_INT_LIT)) {
        Expr* e = ast_create_expr(parser->ws, EXPR_INT_LIT, line, col);
        e->int_val = parser->previous.int_val;
        return e;
    }
    if (match(parser, TOKEN_FLOAT_LIT)) {
        Expr* e = ast_create_expr(parser->ws, EXPR_FLOAT_LIT, line, col);
        e->float_val = parser->previous.float_val;
        return e;
    }
    if (match(parser, TOKEN_STRING_LIT)) {
        Expr* e = ast_create_expr(parser->ws, EXPR_STRING_LIT, line, col);
        e->str_val.chars = copy_str_literal(parser, &parser->previous);
        e->str_val.len = strlen(e->str_val.chars);
        return e;
    }
    if (match(parser, TOKEN_TRUE)) {
        Expr* e = ast_create_expr(parser->ws, EXPR_BOOL_LIT, line, col);
        e->bool_val = true;
        return e;
    }
    if (match(parser, TOKEN_FALSE)) {
        Expr* e = ast_create_expr(parser->ws, EXPR_BOOL_LIT, line, col);
        e->bool_val = false;
        return e;
    }
    if (match(parser, TOKEN_MATH)) {
        consume(parser, TOKEN_DOT, "Expected '.' after 'math'");
        if (match(parser, TOKEN_IDENT)) {
            Expr* e = ast_create_expr(parser->ws, EXPR_MATH_CONST, line, col);
            e->ident_name = copy_token_string(parser, &parser->previous);
            return e;
        }
        error_at(parser, &parser->current, "Expected math constant ('pi', 'e', 'tau')");
        return ast_create_expr(parser->ws, EXPR_FLOAT_LIT, line, col);
    }
    if (match(parser, TOKEN_SIGMA)) {
        Expr* e = ast_create_expr(parser->ws, EXPR_SIGMA, line, col);
        e->sigma.body = parse_expr(parser);
        if (match(parser, TOKEN_FROM)) {
            e->sigma.from_expr = parse_expr(parser);
        } else {
            e->sigma.from_expr = NULL;
        }
        consume(parser, TOKEN_LIMIT, "Expected 'limit' in sigma expression");
        e->sigma.limit_expr = parse_expr(parser);
        if (match(parser, TOKEN_ARROW)) {
            e->sigma.step_expr = parse_expr(parser);
        } else {
            e->sigma.step_expr = NULL;
        }
        return e;
    }
    if (match(parser, TOKEN_INTEGRAL)) {
        Expr* e = ast_create_expr(parser->ws, EXPR_INTEGRAL, line, col);
        consume(parser, TOKEN_LPAREN, "Expected '(' after 'integral'");
        e->integral.body = parse_expr(parser);
        consume(parser, TOKEN_FROM, "Expected 'from' in integral expression");
        e->integral.from_expr = parse_expr(parser);
        consume(parser, TOKEN_LIMIT, "Expected 'limit' in integral expression");
        e->integral.limit_expr = parse_expr(parser);
        consume(parser, TOKEN_ARROW, "Expected '->' step in integral expression");
        e->integral.step_expr = parse_expr(parser);
        consume(parser, TOKEN_RPAREN, "Expected ')' after integral arguments");
        return e;
    }
    if (match(parser, TOKEN_ARC)) {
        Expr* e = ast_create_expr(parser->ws, EXPR_ARC, line, col);
        consume(parser, TOKEN_LPAREN, "Expected '(' after 'arc'");
        e->arc.operand = parse_expr(parser);
        consume(parser, TOKEN_RPAREN, "Expected ')' after arc expression");
        return e;
    }
    if (match(parser, TOKEN_SEARCH)) {
        Expr* e = ast_create_expr(parser->ws, EXPR_SEARCH, line, col);
        consume(parser, TOKEN_IDENT, "Expected identifier after 'search'");
        e->search.ident = copy_token_string(parser, &parser->previous);
        consume(parser, TOKEN_COLON, "Expected ':' in search expression");
        e->search.condition = parse_expr(parser);
        if (match(parser, TOKEN_OR)) {
            e->search.or_default = parse_expr(parser);
        } else {
            e->search.or_default = NULL;
        }
        return e;
    }
    if (match(parser, TOKEN_LOAD)) {
        Expr* e = ast_create_expr(parser->ws, EXPR_LOAD, line, col);
        consume(parser, TOKEN_LT, "Expected '<' after 'load'");
        consume(parser, TOKEN_IDENT, "Expected record identifier in load<...>");
        e->load.record_name = copy_token_string(parser, &parser->previous);
        consume(parser, TOKEN_GT, "Expected '>' after record identifier");
        consume(parser, TOKEN_LPAREN, "Expected '(' in load call");
        consume(parser, TOKEN_STRING_LIT, "Expected string literal path in load call");
        e->load.path = copy_str_literal(parser, &parser->previous);
        consume(parser, TOKEN_RPAREN, "Expected ')' in load call");
        return e;
    }
    if (match(parser, TOKEN_READ_TEXT)) {
        Expr* e = ast_create_expr(parser->ws, EXPR_READ_TEXT, line, col);
        consume(parser, TOKEN_LPAREN, "Expected '(' after 'read_text'");
        consume(parser, TOKEN_STRING_LIT, "Expected string literal path in read_text call");
        e->read_text.path = copy_str_literal(parser, &parser->previous);
        consume(parser, TOKEN_RPAREN, "Expected ')' after read_text argument");
        return e;
    }
    if (match(parser, TOKEN_ETCH)) {
        Expr* e = ast_create_expr(parser->ws, EXPR_ETCH, line, col);
        consume(parser, TOKEN_LPAREN, "Expected '(' after 'etch'");
        consume(parser, TOKEN_STRING_LIT, "Expected string literal in etch call");
        e->etch.etch_str = copy_str_literal(parser, &parser->previous);
        consume(parser, TOKEN_RPAREN, "Expected ')' after etch string");
        return e;
    }
    if (match(parser, TOKEN_TYPE_ARRAY)) {
        Expr* e = ast_create_expr(parser->ws, EXPR_ARRAY_NEW, line, col);
        consume(parser, TOKEN_LT, "Expected '<' after 'array'");
        consume(parser, TOKEN_IDENT, "Expected record type identifier inside array<...>");
        e->collection_new.record_name = copy_token_string(parser, &parser->previous);
        consume(parser, TOKEN_GT, "Expected '>' after record type");
        if (match(parser, TOKEN_LBRACKET)) {
            e->collection_new.capacity = parse_expr(parser);
            consume(parser, TOKEN_RBRACKET, "Expected ']' after array capacity");
        } else {
            e->collection_new.capacity = NULL;
        }
        return e;
    }
    if (match(parser, TOKEN_TYPE_TABLE)) {
        Expr* e = ast_create_expr(parser->ws, EXPR_TABLE_NEW, line, col);
        consume(parser, TOKEN_LT, "Expected '<' after 'table'");
        consume(parser, TOKEN_IDENT, "Expected record type identifier inside table<...>");
        e->collection_new.record_name = copy_token_string(parser, &parser->previous);
        consume(parser, TOKEN_GT, "Expected '>' after record type");
        if (match(parser, TOKEN_LBRACKET)) {
            e->collection_new.capacity = parse_expr(parser);
            consume(parser, TOKEN_RBRACKET, "Expected ']' after table capacity");
        } else {
            e->collection_new.capacity = NULL;
        }
        return e;
    }
    if (match(parser, TOKEN_TYPE_LIST)) {
        Expr* e = ast_create_expr(parser->ws, EXPR_LIST_NEW, line, col);
        consume(parser, TOKEN_LT, "Expected '<' after 'list'");
        e->collection_new.elem_type = parse_type_spec(parser);
        consume(parser, TOKEN_GT, "Expected '>' after element type");
        if (match(parser, TOKEN_LBRACKET)) {
            e->collection_new.capacity = parse_expr(parser);
            consume(parser, TOKEN_RBRACKET, "Expected ']' after list capacity");
        } else {
            e->collection_new.capacity = NULL;
        }
        return e;
    }
    if (match(parser, TOKEN_IDENT)) {
        Expr* e = ast_create_expr(parser->ws, EXPR_IDENT, line, col);
        e->ident_name = copy_token_string(parser, &parser->previous);
        return e;
    }
    if (match(parser, TOKEN_LPAREN)) {
        Expr* e = parse_expr(parser);
        consume(parser, TOKEN_RPAREN, "Expected ')' after grouped expression");
        return e;
    }

    error_at(parser, &parser->current, "Expected expression");
    return ast_create_expr(parser->ws, EXPR_INT_LIT, line, col);
}

static Expr* parse_postfix(Parser* parser) {
    Expr* expr = parse_primary(parser);

    for (;;) {
        if (match(parser, TOKEN_DOT)) {
            consume(parser, TOKEN_IDENT, "Expected member name after '.'");
            const char* member_name = copy_token_string(parser, &parser->previous);

            // Check if it's a method call like .push(...)
            if (match(parser, TOKEN_LPAREN)) {
                Expr* call_expr = ast_create_expr(parser->ws, EXPR_CALL, expr->line, expr->col);
                Expr* mem = ast_create_expr(parser->ws, EXPR_MEMBER, expr->line, expr->col);
                mem->member.target = expr;
                mem->member.member_name = member_name;
                call_expr->call.callee = mem;

                Expr* args[64];
                size_t arg_count = 0;
                if (!check(parser, TOKEN_RPAREN)) {
                    do {
                        if (arg_count < 64) {
                            args[arg_count++] = parse_expr(parser);
                        } else {
                            parse_expr(parser);
                        }
                    } while (match(parser, TOKEN_COMMA));
                }
                consume(parser, TOKEN_RPAREN, "Expected ')' after method arguments");

                call_expr->call.arg_count = arg_count;
                call_expr->call.args = (Expr**)slab_alloc(parser->ws, arg_count * sizeof(Expr*), alignof(Expr*));
                if (arg_count > 0 && call_expr->call.args) {
                    memcpy(call_expr->call.args, args, arg_count * sizeof(Expr*));
                }
                expr = call_expr;
            } else {
                Expr* mem = ast_create_expr(parser->ws, EXPR_MEMBER, expr->line, expr->col);
                mem->member.target = expr;
                mem->member.member_name = member_name;
                expr = mem;
            }
        } else if (match(parser, TOKEN_LPAREN)) {
            // Function call
            Expr* call_expr = ast_create_expr(parser->ws, EXPR_CALL, expr->line, expr->col);
            call_expr->call.callee = expr;

            Expr* args[64];
            size_t arg_count = 0;
            if (!check(parser, TOKEN_RPAREN)) {
                do {
                    if (arg_count < 64) {
                        args[arg_count++] = parse_expr(parser);
                    } else {
                        parse_expr(parser);
                    }
                } while (match(parser, TOKEN_COMMA));
            }
            consume(parser, TOKEN_RPAREN, "Expected ')' after arguments");

            call_expr->call.arg_count = arg_count;
            call_expr->call.args = (Expr**)slab_alloc(parser->ws, arg_count * sizeof(Expr*), alignof(Expr*));
            if (arg_count > 0 && call_expr->call.args) {
                memcpy(call_expr->call.args, args, arg_count * sizeof(Expr*));
            }
            expr = call_expr;
        } else if (match(parser, TOKEN_LBRACKET)) {
            // Array/table indexing
            Expr* idx_expr = ast_create_expr(parser->ws, EXPR_INDEX, expr->line, expr->col);
            idx_expr->index.target = expr;
            idx_expr->index.index = parse_expr(parser);
            consume(parser, TOKEN_RBRACKET, "Expected ']' after array index");
            expr = idx_expr;
        } else {
            break;
        }
    }

    return expr;
}

static Expr* parse_unary(Parser* parser) {
    if (match(parser, TOKEN_MINUS) || match(parser, TOKEN_BANG)) {
        TokenType op = parser->previous.type;
        int line = parser->previous.line;
        int col = parser->previous.col;
        Expr* operand = parse_unary(parser);
        Expr* un = ast_create_expr(parser->ws, EXPR_UNARY, line, col);
        un->unary.op = op;
        un->unary.operand = operand;
        return un;
    }
    return parse_postfix(parser);
}

static Expr* parse_power(Parser* parser) {
    Expr* left = parse_unary(parser);
    if (match(parser, TOKEN_CARET)) {
        int line = parser->previous.line;
        int col = parser->previous.col;
        Expr* right = parse_power(parser); // right-associative recursion
        Expr* bin = ast_create_expr(parser->ws, EXPR_BINARY, line, col);
        bin->binary.op = TOKEN_CARET;
        bin->binary.left = left;
        bin->binary.right = right;
        return bin;
    }
    return left;
}

static Expr* parse_factor(Parser* parser) {
    Expr* expr = parse_power(parser);
    while (match(parser, TOKEN_STAR) || match(parser, TOKEN_SLASH) ||
           match(parser, TOKEN_PERCENT) || match(parser, TOKEN_AT)) {
        TokenType op = parser->previous.type;
        int line = parser->previous.line;
        int col = parser->previous.col;
        Expr* right = parse_power(parser);
        Expr* bin = ast_create_expr(parser->ws, EXPR_BINARY, line, col);
        bin->binary.op = op;
        bin->binary.left = expr;
        bin->binary.right = right;
        expr = bin;
    }
    return expr;
}

static Expr* parse_term(Parser* parser) {
    Expr* expr = parse_factor(parser);
    while (match(parser, TOKEN_PLUS) || match(parser, TOKEN_MINUS)) {
        TokenType op = parser->previous.type;
        int line = parser->previous.line;
        int col = parser->previous.col;
        Expr* right = parse_factor(parser);
        Expr* bin = ast_create_expr(parser->ws, EXPR_BINARY, line, col);
        bin->binary.op = op;
        bin->binary.left = expr;
        bin->binary.right = right;
        expr = bin;
    }
    return expr;
}

static Expr* parse_compare(Parser* parser) {
    Expr* expr = parse_term(parser);
    while (match(parser, TOKEN_LT) || match(parser, TOKEN_LT_EQ) ||
           match(parser, TOKEN_GT) || match(parser, TOKEN_GT_EQ)) {
        TokenType op = parser->previous.type;
        int line = parser->previous.line;
        int col = parser->previous.col;
        Expr* right = parse_term(parser);
        Expr* bin = ast_create_expr(parser->ws, EXPR_BINARY, line, col);
        bin->binary.op = op;
        bin->binary.left = expr;
        bin->binary.right = right;
        expr = bin;
    }
    return expr;
}

static Expr* parse_equality(Parser* parser) {
    Expr* expr = parse_compare(parser);
    while (match(parser, TOKEN_EQ_EQ) || match(parser, TOKEN_BANG_EQ)) {
        TokenType op = parser->previous.type;
        int line = parser->previous.line;
        int col = parser->previous.col;
        Expr* right = parse_compare(parser);
        Expr* bin = ast_create_expr(parser->ws, EXPR_BINARY, line, col);
        bin->binary.op = op;
        bin->binary.left = expr;
        bin->binary.right = right;
        expr = bin;
    }
    return expr;
}

static Expr* parse_logic_and(Parser* parser) {
    Expr* expr = parse_equality(parser);
    while (match(parser, TOKEN_AND_AND)) {
        TokenType op = parser->previous.type;
        int line = parser->previous.line;
        int col = parser->previous.col;
        Expr* right = parse_equality(parser);
        Expr* bin = ast_create_expr(parser->ws, EXPR_BINARY, line, col);
        bin->binary.op = op;
        bin->binary.left = expr;
        bin->binary.right = right;
        expr = bin;
    }
    return expr;
}

static Expr* parse_logic_or(Parser* parser) {
    Expr* expr = parse_logic_and(parser);
    while (match(parser, TOKEN_OR_OR)) {
        TokenType op = parser->previous.type;
        int line = parser->previous.line;
        int col = parser->previous.col;
        Expr* right = parse_logic_and(parser);
        Expr* bin = ast_create_expr(parser->ws, EXPR_BINARY, line, col);
        bin->binary.op = op;
        bin->binary.left = expr;
        bin->binary.right = right;
        expr = bin;
    }
    return expr;
}

static Expr* parse_expr(Parser* parser) {
    return parse_logic_or(parser);
}

// Block and Statement parsing
static Block* parse_block(Parser* parser) {
    consume(parser, TOKEN_COLON, "Expected ':' before block");
    skip_newlines(parser);
    consume(parser, TOKEN_INDENT, "Expected indented block");

    Block* block = ast_create_block(parser->ws);

    while (!check(parser, TOKEN_DEDENT) && !check(parser, TOKEN_EOF)) {
        skip_newlines(parser);
        if (check(parser, TOKEN_DEDENT) || check(parser, TOKEN_EOF)) break;
        Stmt* stmt = parse_stmt(parser);
        if (stmt) {
            ast_block_append(parser->ws, block, stmt);
        }
    }

    consume(parser, TOKEN_DEDENT, "Expected dedent at end of block");
    return block;
}

static Stmt* parse_stmt(Parser* parser) {
    int line = parser->current.line;
    int col = parser->current.col;

    // workspace_stmt
    if (match(parser, TOKEN_WORKSPACE)) {
        consume(parser, TOKEN_IDENT, "Expected workspace name");
        const char* name = copy_token_string(parser, &parser->previous);
        Block* block = parse_block(parser);
        Stmt* s = ast_create_stmt(parser->ws, STMT_WORKSPACE, line, col);
        s->workspace.name = name;
        s->workspace.block = block;
        return s;
    }

    // reset_stmt
    if (match(parser, TOKEN_RESET)) {
        if (match(parser, TOKEN_LPAREN)) {
            consume(parser, TOKEN_RPAREN, "Expected ')' after reset");
            skip_newlines(parser);
            return ast_create_stmt(parser->ws, STMT_RESET_CALL, line, col);
        } else {
            Block* block = parse_block(parser);
            Stmt* s = ast_create_stmt(parser->ws, STMT_RESET_BLOCK, line, col);
            s->reset_block.block = block;
            return s;
        }
    }

    // for_stmt or parallel for_stmt
    if (match(parser, TOKEN_PARALLEL) || match(parser, TOKEN_FOR)) {
        bool is_parallel = (parser->previous.type == TOKEN_PARALLEL);
        if (is_parallel) {
            consume(parser, TOKEN_FOR, "Expected 'for' after 'parallel'");
        }
        consume(parser, TOKEN_IDENT, "Expected loop variable name");
        const char* var_name = copy_token_string(parser, &parser->previous);
        consume(parser, TOKEN_IN, "Expected 'in' in for loop");
        Expr* start_or_iter = parse_expr(parser);

        if (match(parser, TOKEN_DOT_DOT)) {
            // Range loop: for i in start..end [-> step]:
            Expr* end_expr = parse_expr(parser);
            Expr* step_expr = NULL;
            if (match(parser, TOKEN_ARROW)) {
                step_expr = parse_expr(parser);
            }
            Block* block = parse_block(parser);
            Stmt* s = ast_create_stmt(parser->ws, STMT_FOR_RANGE, line, col);
            s->for_range.var_name = var_name;
            s->for_range.start_expr = start_or_iter;
            s->for_range.end_expr = end_expr;
            s->for_range.step_expr = step_expr;
            s->for_range.block = block;
            s->for_range.is_parallel = is_parallel;
            s->for_range.parallel_id = 0;
            return s;
        } else {
            if (is_parallel) {
                error_at(parser, &parser->previous, "parallel for requires numeric range 'start..end'");
            }
            // Iterator loop: for x in collection:
            Block* block = parse_block(parser);
            Stmt* s = ast_create_stmt(parser->ws, STMT_FOR_IN, line, col);
            s->for_in.var_name = var_name;
            s->for_in.iter_expr = start_or_iter;
            s->for_in.block = block;
            return s;
        }
    }

    // while_stmt
    if (match(parser, TOKEN_WHILE)) {
        Expr* cond = parse_expr(parser);
        Block* block = parse_block(parser);
        Stmt* s = ast_create_stmt(parser->ws, STMT_WHILE, line, col);
        s->while_stmt.condition = cond;
        s->while_stmt.block = block;
        return s;
    }

    // do_until_stmt: do: block until expr NEWLINE
    if (match(parser, TOKEN_DO)) {
        Block* block = parse_block(parser);
        consume(parser, TOKEN_UNTIL, "Expected 'until' after do block");
        Expr* cond = parse_expr(parser);
        skip_newlines(parser);
        Stmt* s = ast_create_stmt(parser->ws, STMT_DO_UNTIL, line, col);
        s->do_until.block = block;
        s->do_until.condition = cond;
        return s;
    }

    // if_stmt
    if (match(parser, TOKEN_IF)) {
        Expr* cond = parse_expr(parser);
        Block* then_b = parse_block(parser);
        Block* else_b = NULL;
        if (match(parser, TOKEN_ELSE)) {
            else_b = parse_block(parser);
        }
        Stmt* s = ast_create_stmt(parser->ws, STMT_IF, line, col);
        s->if_stmt.condition = cond;
        s->if_stmt.then_block = then_b;
        s->if_stmt.else_block = else_b;
        return s;
    }

    // sort_stmt: sort IDENT by IDENT (asc | desc) NEWLINE
    if (match(parser, TOKEN_SORT)) {
        consume(parser, TOKEN_IDENT, "Expected collection identifier to sort");
        const char* target = copy_token_string(parser, &parser->previous);
        consume(parser, TOKEN_BY, "Expected 'by' in sort statement");
        consume(parser, TOKEN_IDENT, "Expected field identifier to sort by");
        const char* field = copy_token_string(parser, &parser->previous);
        bool is_asc = true;
        if (match(parser, TOKEN_DESC)) {
            is_asc = false;
        } else {
            match(parser, TOKEN_ASC);
        }
        skip_newlines(parser);
        Stmt* s = ast_create_stmt(parser->ws, STMT_SORT, line, col);
        s->sort_stmt.target_ident = target;
        s->sort_stmt.field_ident = field;
        s->sort_stmt.is_asc = is_asc;
        return s;
    }

    // save_stmt: save expr to STRING_LIT NEWLINE
    if (match(parser, TOKEN_SAVE)) {
        Expr* e = parse_expr(parser);
        consume(parser, TOKEN_TO, "Expected 'to' in save statement");
        consume(parser, TOKEN_STRING_LIT, "Expected string literal path in save statement");
        const char* path = copy_str_literal(parser, &parser->previous);
        skip_newlines(parser);
        Stmt* s = ast_create_stmt(parser->ws, STMT_SAVE, line, col);
        s->save_stmt.expr = e;
        s->save_stmt.path = path;
        return s;
    }

    // state_stmt: state expr {, expr} NEWLINE
    if (match(parser, TOKEN_STATE)) {
        Expr* exprs[64];
        size_t count = 0;
        do {
            if (count < 64) {
                exprs[count++] = parse_expr(parser);
            } else {
                parse_expr(parser);
            }
        } while (match(parser, TOKEN_COMMA));
        skip_newlines(parser);
        Stmt* s = ast_create_stmt(parser->ws, STMT_STATE, line, col);
        s->state_stmt.count = count;
        s->state_stmt.exprs = (Expr**)slab_alloc(parser->ws, count * sizeof(Expr*), alignof(Expr*));
        if (count > 0 && s->state_stmt.exprs) {
            memcpy(s->state_stmt.exprs, exprs, count * sizeof(Expr*));
        }
        return s;
    }

    // return_stmt: return [expr] NEWLINE
    if (match(parser, TOKEN_RETURN)) {
        Expr* e = NULL;
        if (!check(parser, TOKEN_NEWLINE) && !check(parser, TOKEN_DEDENT) && !check(parser, TOKEN_EOF)) {
            e = parse_expr(parser);
        }
        skip_newlines(parser);
        Stmt* s = ast_create_stmt(parser->ws, STMT_RETURN, line, col);
        s->return_stmt.expr = e;
        return s;
    }

    // assign_or_expr_stmt
    Expr* left = parse_expr(parser);
    if (match(parser, TOKEN_EQ)) {
        Expr* right = parse_expr(parser);
        skip_newlines(parser);
        Stmt* s = ast_create_stmt(parser->ws, STMT_ASSIGN, line, col);
        s->assign_stmt.target = left;
        s->assign_stmt.value = right;
        return s;
    }

    skip_newlines(parser);
    Stmt* s = ast_create_stmt(parser->ws, STMT_EXPR, line, col);
    s->expr_stmt.expr = left;
    return s;
}

// Top level parsing
Program* parser_parse_program(Parser* parser) {
    Program* prog = ast_create_program(parser->ws);

    ImportStmt* imports[128];
    size_t import_count = 0;
    ConstDecl* consts[128];
    size_t const_count = 0;
    RecordDecl* records[128];
    size_t record_count = 0;
    FnDecl* functions[256];
    size_t fn_count = 0;

    // Buffer for any top-level statements that should go into main()
    Stmt* top_level_stmts[512];
    size_t top_stmt_count = 0;

    skip_newlines(parser);

    while (!check(parser, TOKEN_EOF)) {
        skip_newlines(parser);
        if (check(parser, TOKEN_EOF)) break;

        // import_stmt
        if (match(parser, TOKEN_IMPORT)) {
            consume(parser, TOKEN_STRING_LIT, "Expected string literal after 'import'");
            const char* path = copy_str_literal(parser, &parser->previous);
            skip_newlines(parser);
            if (import_count < 128) {
                ImportStmt* imp = (ImportStmt*)slab_alloc(parser->ws, sizeof(ImportStmt), alignof(ImportStmt));
                imp->path = path;
                imp->line = parser->previous.line;
                imp->col = parser->previous.col;
                imports[import_count++] = imp;
            }
            continue;
        }

        // const_decl
        if (match(parser, TOKEN_CONST)) {
            consume(parser, TOKEN_IDENT, "Expected constant identifier");
            const char* name = copy_token_string(parser, &parser->previous);
            consume(parser, TOKEN_EQ, "Expected '=' in constant declaration");
            Expr* val = parse_expr(parser);
            skip_newlines(parser);
            if (const_count < 128) {
                ConstDecl* cd = (ConstDecl*)slab_alloc(parser->ws, sizeof(ConstDecl), alignof(ConstDecl));
                cd->name = name;
                cd->value = val;
                cd->line = parser->previous.line;
                cd->col = parser->previous.col;
                consts[const_count++] = cd;
            }
            continue;
        }

        // record_decl
        if (match(parser, TOKEN_RECORD)) {
            int line = parser->previous.line;
            int col = parser->previous.col;
            consume(parser, TOKEN_IDENT, "Expected record identifier");
            const char* record_name = copy_token_string(parser, &parser->previous);
            consume(parser, TOKEN_COLON, "Expected ':' after record name");
            skip_newlines(parser);
            consume(parser, TOKEN_INDENT, "Expected indented field declarations");

            RecordField fields[64];
            size_t field_count = 0;

            while (!check(parser, TOKEN_DEDENT) && !check(parser, TOKEN_EOF)) {
                skip_newlines(parser);
                if (check(parser, TOKEN_DEDENT) || check(parser, TOKEN_EOF)) break;

                consume(parser, TOKEN_IDENT, "Expected field name");
                const char* fname = copy_token_string(parser, &parser->previous);
                int fline = parser->previous.line;
                int fcol = parser->previous.col;
                consume(parser, TOKEN_COLON, "Expected ':' after field name");
                TypeSpec* ftype = parse_type_spec(parser);
                skip_newlines(parser);

                if (field_count < 64) {
                    fields[field_count].name = fname;
                    fields[field_count].type = ftype;
                    fields[field_count].is_read = true;
                    fields[field_count].line = fline;
                    fields[field_count].col = fcol;
                    field_count++;
                }
            }
            consume(parser, TOKEN_DEDENT, "Expected dedent at end of record declaration");

            if (record_count < 128) {
                RecordDecl* rd = (RecordDecl*)slab_alloc(parser->ws, sizeof(RecordDecl), alignof(RecordDecl));
                rd->name = record_name;
                rd->line = line;
                rd->col = col;
                rd->field_count = field_count;
                rd->fields = (RecordField*)slab_alloc(parser->ws, field_count * sizeof(RecordField), alignof(RecordField));
                if (field_count > 0 && rd->fields) {
                    memcpy(rd->fields, fields, field_count * sizeof(RecordField));
                }
                records[record_count++] = rd;
            }
            continue;
        }

        // fn_decl or extern fn
        if (match(parser, TOKEN_EXTERN) || match(parser, TOKEN_FN)) {
            bool is_extern = (parser->previous.type == TOKEN_EXTERN);
            if (is_extern) {
                consume(parser, TOKEN_FN, "Expected 'fn' after 'extern'");
            }
            int line = parser->previous.line;
            int col = parser->previous.col;
            consume(parser, TOKEN_IDENT, "Expected function name");
            const char* fn_name = copy_token_string(parser, &parser->previous);
            consume(parser, TOKEN_LPAREN, "Expected '(' after function name");

            FnParam params[32];
            size_t param_count = 0;
            if (!check(parser, TOKEN_RPAREN)) {
                do {
                    consume(parser, TOKEN_IDENT, "Expected parameter name");
                    const char* pname = copy_token_string(parser, &parser->previous);
                    consume(parser, TOKEN_COLON, "Expected ':' after parameter name");
                    TypeSpec* ptype = parse_type_spec(parser);
                    if (param_count < 32) {
                        params[param_count].name = pname;
                        params[param_count].type = ptype;
                        param_count++;
                    }
                } while (match(parser, TOKEN_COMMA));
            }
            consume(parser, TOKEN_RPAREN, "Expected ')' after parameters");

            TypeSpec* ret_type = NULL;
            if (match(parser, TOKEN_ARROW)) {
                ret_type = parse_type_spec(parser);
            } else {
                ret_type = ast_create_type(parser->ws, TYPE_KIND_VOID);
            }

            Block* body = NULL;
            if (is_extern) {
                skip_newlines(parser);
            } else {
                body = parse_block(parser);
            }

            if (fn_count < 256) {
                FnDecl* fd = (FnDecl*)slab_alloc(parser->ws, sizeof(FnDecl), alignof(FnDecl));
                fd->name = fn_name;
                fd->line = line;
                fd->col = col;
                fd->is_extern = is_extern;
                fd->param_count = param_count;
                fd->params = (FnParam*)slab_alloc(parser->ws, param_count * sizeof(FnParam), alignof(FnParam));
                if (param_count > 0 && fd->params) {
                    memcpy(fd->params, params, param_count * sizeof(FnParam));
                }
                fd->return_type = ret_type;
                fd->body = body;
                functions[fn_count++] = fd;
            }
            continue;
        }

        // Top-level statements (e.g. for scripts or benchmarks without explicit fn main())
        Stmt* stmt = parse_stmt(parser);
        if (stmt && top_stmt_count < 512) {
            top_level_stmts[top_stmt_count++] = stmt;
        }
    }

    // If top-level statements exist, wrap them in a synthetic `main` function
    if (top_stmt_count > 0) {
        Block* main_body = ast_create_block(parser->ws);
        for (size_t i = 0; i < top_stmt_count; i++) {
            ast_block_append(parser->ws, main_body, top_level_stmts[i]);
        }
        FnDecl* main_fn = (FnDecl*)slab_alloc(parser->ws, sizeof(FnDecl), alignof(FnDecl));
        main_fn->name = "main";
        main_fn->line = 1;
        main_fn->col = 1;
        main_fn->param_count = 0;
        main_fn->params = NULL;
        main_fn->return_type = ast_create_type(parser->ws, TYPE_KIND_INT);
        main_fn->body = main_body;
        functions[fn_count++] = main_fn;
    }

    // Populate Program struct
    prog->import_count = import_count;
    prog->imports = (ImportStmt**)slab_alloc(parser->ws, import_count * sizeof(ImportStmt*), alignof(ImportStmt*));
    if (import_count > 0 && prog->imports) {
        memcpy(prog->imports, imports, import_count * sizeof(ImportStmt*));
    }

    prog->const_count = const_count;
    prog->consts = (ConstDecl**)slab_alloc(parser->ws, const_count * sizeof(ConstDecl*), alignof(ConstDecl*));
    if (const_count > 0 && prog->consts) {
        memcpy(prog->consts, consts, const_count * sizeof(ConstDecl*));
    }

    prog->record_count = record_count;
    prog->records = (RecordDecl**)slab_alloc(parser->ws, record_count * sizeof(RecordDecl*), alignof(RecordDecl*));
    if (record_count > 0 && prog->records) {
        memcpy(prog->records, records, record_count * sizeof(RecordDecl*));
    }

    prog->fn_count = fn_count;
    prog->functions = (FnDecl**)slab_alloc(parser->ws, fn_count * sizeof(FnDecl*), alignof(FnDecl*));
    if (fn_count > 0 && prog->functions) {
        memcpy(prog->functions, functions, fn_count * sizeof(FnDecl*));
    }

    return prog;
}
