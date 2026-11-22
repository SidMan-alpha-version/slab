#include "ast.h"
#include <stdio.h>
#include <string.h>

TypeSpec* ast_create_type(SlabWorkspace* ws, TypeKind kind) {
    TypeSpec* t = (TypeSpec*)slab_alloc(ws, sizeof(TypeSpec), alignof(TypeSpec));
    if (!t) return NULL;
    t->kind = kind;
    t->custom_name = NULL;
    t->elem_type = NULL;
    t->capacity_expr = NULL;
    t->has_bounds = false;
    t->int_min = 0;
    t->int_max = 0;
    return t;
}

Expr* ast_create_expr(SlabWorkspace* ws, ExprKind kind, int line, int col) {
    Expr* e = (Expr*)slab_alloc(ws, sizeof(Expr), alignof(Expr));
    if (!e) return NULL;
    memset(e, 0, sizeof(Expr));
    e->kind = kind;
    e->line = line;
    e->col = col;
    e->inferred_type = NULL;
    return e;
}

Stmt* ast_create_stmt(SlabWorkspace* ws, StmtKind kind, int line, int col) {
    Stmt* s = (Stmt*)slab_alloc(ws, sizeof(Stmt), alignof(Stmt));
    if (!s) return NULL;
    memset(s, 0, sizeof(Stmt));
    s->kind = kind;
    s->line = line;
    s->col = col;
    return s;
}

Block* ast_create_block(SlabWorkspace* ws) {
    Block* b = (Block*)slab_alloc(ws, sizeof(Block), alignof(Block));
    if (!b) return NULL;
    b->statements = NULL;
    b->count = 0;
    b->capacity = 0;
    return b;
}

void ast_block_append(SlabWorkspace* ws, Block* block, Stmt* stmt) {
    if (!block || !stmt) return;
    if (block->count >= block->capacity) {
        size_t new_cap = block->capacity == 0 ? 8 : block->capacity * 2;
        Stmt** new_stmts = (Stmt**)slab_alloc(ws, new_cap * sizeof(Stmt*), alignof(Stmt*));
        if (block->statements && block->count > 0) {
            memcpy(new_stmts, block->statements, block->count * sizeof(Stmt*));
        }
        block->statements = new_stmts;
        block->capacity = new_cap;
    }
    block->statements[block->count++] = stmt;
}

Program* ast_create_program(SlabWorkspace* ws) {
    Program* p = (Program*)slab_alloc(ws, sizeof(Program), alignof(Program));
    if (!p) return NULL;
    memset(p, 0, sizeof(Program));
    return p;
}

bool ast_type_is_reference(const TypeSpec* type) {
    if (!type) return false;
    return (type->kind == TYPE_KIND_ARRAY ||
            type->kind == TYPE_KIND_TABLE ||
            type->kind == TYPE_KIND_LIST ||
            type->kind == TYPE_KIND_STR);
}

const char* ast_type_to_string(const TypeSpec* type) {
    if (!type) return "unknown";
    switch (type->kind) {
        case TYPE_KIND_INT: return "int";
        case TYPE_KIND_I32: return "i32";
        case TYPE_KIND_U16: return "u16";
        case TYPE_KIND_U32: return "u32";
        case TYPE_KIND_F32: return "f32";
        case TYPE_KIND_F64: return "f64";
        case TYPE_KIND_BOOL: return "bool";
        case TYPE_KIND_STR: return "str";
        case TYPE_KIND_VOID: return "void";
        case TYPE_KIND_ARRAY: return "array";
        case TYPE_KIND_TABLE: return "table";
        case TYPE_KIND_LIST: return "list";
        case TYPE_KIND_CUSTOM: return type->custom_name ? type->custom_name : "record";
        default: return "unknown";
    }
}
