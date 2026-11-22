#include "analyzer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct Symbol {
    const char* name;
    TypeSpec* type;
    int scope_depth;
    bool is_const;
    struct Symbol* next;
} Symbol;

typedef struct Scope {
    Symbol* symbols;
    struct Scope* parent;
    int depth;
    bool is_workspace_or_reset;
} Scope;

typedef struct {
    Analyzer* analyzer;
    Scope* current_scope;
    RecordDecl** records;
    size_t record_count;
    FnDecl** functions;
    size_t fn_count;
    bool in_parallel_for;
} AnalyzerContext;

static void sem_error(Analyzer* analyzer, int line, int col, const char* message) {
    analyzer->had_error = true;
    fprintf(stderr, "Error [line %d:col %d]: %s\n", line, col, message);
}

static FnDecl* find_function(AnalyzerContext* ctx, const char* name) {
    if (!name) return NULL;
    for (size_t i = 0; i < ctx->fn_count; i++) {
        if (strcmp(ctx->functions[i]->name, name) == 0) {
            return ctx->functions[i];
        }
    }
    return NULL;
}

static bool is_builtin_func(const char* name) {
    if (!name) return false;
    return strcmp(name, "print") == 0 || strcmp(name, "println") == 0 ||
           strcmp(name, "sqrt") == 0 || strcmp(name, "sin") == 0 ||
           strcmp(name, "cos") == 0 || strcmp(name, "tan") == 0 ||
           strcmp(name, "arcsin") == 0 || strcmp(name, "arccos") == 0 ||
           strcmp(name, "arctan") == 0 || strcmp(name, "cot") == 0 ||
           strcmp(name, "sec") == 0 || strcmp(name, "csc") == 0 ||
           strcmp(name, "exp") == 0 || strcmp(name, "log") == 0 ||
           strcmp(name, "abs") == 0 || strcmp(name, "len") == 0 ||
           strcmp(name, "trim") == 0 || strcmp(name, "slice") == 0 ||
           strcmp(name, "split") == 0 || strcmp(name, "contains") == 0 ||
           strcmp(name, "starts_with") == 0 || strcmp(name, "ends_with") == 0 ||
           strcmp(name, "raw_ptr") == 0;
}

static Scope* scope_push(AnalyzerContext* ctx, bool is_workspace_or_reset) {
    Scope* s = (Scope*)slab_alloc(ctx->analyzer->ws, sizeof(Scope), alignof(Scope));
    s->symbols = NULL;
    s->parent = ctx->current_scope;
    s->is_workspace_or_reset = is_workspace_or_reset;
    s->depth = ctx->current_scope ? ctx->current_scope->depth + (is_workspace_or_reset ? 1 : 0) : 0;
    ctx->current_scope = s;
    return s;
}

static void scope_pop(AnalyzerContext* ctx) {
    if (ctx->current_scope) {
        ctx->current_scope = ctx->current_scope->parent;
    }
}

static void symbol_add(AnalyzerContext* ctx, const char* name, TypeSpec* type, bool is_const) {
    Symbol* sym = (Symbol*)slab_alloc(ctx->analyzer->ws, sizeof(Symbol), alignof(Symbol));
    sym->name = name;
    sym->type = type;
    sym->scope_depth = ctx->current_scope ? ctx->current_scope->depth : 0;
    sym->is_const = is_const;
    sym->next = ctx->current_scope->symbols;
    ctx->current_scope->symbols = sym;
}

static Symbol* symbol_lookup(AnalyzerContext* ctx, const char* name) {
    for (Scope* s = ctx->current_scope; s != NULL; s = s->parent) {
        for (Symbol* sym = s->symbols; sym != NULL; sym = sym->next) {
            if (strcmp(sym->name, name) == 0) {
                return sym;
            }
        }
    }
    return NULL;
}

static RecordDecl* find_record(AnalyzerContext* ctx, const char* name) {
    if (!name) return NULL;
    for (size_t i = 0; i < ctx->record_count; i++) {
        if (strcmp(ctx->records[i]->name, name) == 0) {
            return ctx->records[i];
        }
    }
    return NULL;
}

static RecordField* find_record_field(RecordDecl* rec, const char* field_name) {
    if (!rec || !field_name) return NULL;
    for (size_t i = 0; i < rec->field_count; i++) {
        if (strcmp(rec->fields[i].name, field_name) == 0) {
            return &rec->fields[i];
        }
    }
    return NULL;
}

static void analyze_expr(AnalyzerContext* ctx, Expr* expr);
static void analyze_stmt(AnalyzerContext* ctx, Stmt* stmt);
static void analyze_block(AnalyzerContext* ctx, Block* block);

static void analyze_expr(AnalyzerContext* ctx, Expr* expr) {
    if (!expr) return;

    switch (expr->kind) {
        case EXPR_INT_LIT:
            expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_INT);
            break;
        case EXPR_FLOAT_LIT:
            expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_F64);
            break;
        case EXPR_STRING_LIT:
            expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_STR);
            break;
        case EXPR_BOOL_LIT:
            expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_BOOL);
            break;
        case EXPR_MATH_CONST:
            expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_F64);
            break;
        case EXPR_IDENT: {
            Symbol* sym = symbol_lookup(ctx, expr->ident_name);
            if (sym) {
                expr->inferred_type = sym->type;
            } else if (is_builtin_func(expr->ident_name) || find_function(ctx, expr->ident_name)) {
                expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_VOID);
            } else {
                char msg[256];
                snprintf(msg, sizeof(msg), "Undeclared identifier '%s'", expr->ident_name);
                sem_error(ctx->analyzer, expr->line, expr->col, msg);
                expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_F64);
            }
            break;
        }
        case EXPR_BINARY:
            analyze_expr(ctx, expr->binary.left);
            analyze_expr(ctx, expr->binary.right);

            switch (expr->binary.op) {
                case TOKEN_PLUS:
                case TOKEN_MINUS:
                case TOKEN_STAR:
                case TOKEN_SLASH:
                case TOKEN_PERCENT:
                case TOKEN_CARET:
                case TOKEN_AT: {
                    TypeSpec* lt = expr->binary.left ? expr->binary.left->inferred_type : NULL;
                    TypeSpec* rt = expr->binary.right ? expr->binary.right->inferred_type : NULL;
                    if (expr->binary.op == TOKEN_PLUS &&
                        ((lt && lt->kind == TYPE_KIND_STR) || (rt && rt->kind == TYPE_KIND_STR))) {
                        expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_STR);
                    } else if ((lt && (lt->kind == TYPE_KIND_F32 || lt->kind == TYPE_KIND_F64)) ||
                               (rt && (rt->kind == TYPE_KIND_F32 || rt->kind == TYPE_KIND_F64)) ||
                               expr->binary.op == TOKEN_CARET || expr->binary.op == TOKEN_SLASH) {
                        expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_F64);
                    } else {
                        expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_INT);
                    }
                    break;
                }
                case TOKEN_EQ_EQ:
                case TOKEN_BANG_EQ:
                case TOKEN_LT:
                case TOKEN_LT_EQ:
                case TOKEN_GT:
                case TOKEN_GT_EQ:
                case TOKEN_AND_AND:
                case TOKEN_OR_OR:
                    expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_BOOL);
                    break;
                default:
                    expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_INT);
                    break;
            }
            break;
        case EXPR_UNARY:
            analyze_expr(ctx, expr->unary.operand);
            if (expr->unary.op == TOKEN_BANG) {
                expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_BOOL);
            } else {
                expr->inferred_type = expr->unary.operand ? expr->unary.operand->inferred_type :
                                      ast_create_type(ctx->analyzer->ws, TYPE_KIND_INT);
            }
            break;
        case EXPR_MEMBER: {
            analyze_expr(ctx, expr->member.target);
            TypeSpec* tgt_type = expr->member.target ? expr->member.target->inferred_type : NULL;
            if (tgt_type) {
                if (tgt_type->kind == TYPE_KIND_ARRAY || tgt_type->kind == TYPE_KIND_TABLE) {
                    if (strcmp(expr->member.member_name, "len") == 0) {
                        expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_INT);
                        break;
                    }
                    RecordDecl* rec = find_record(ctx, tgt_type->custom_name);
                    if (rec) {
                        RecordField* fld = find_record_field(rec, expr->member.member_name);
                        if (fld) {
                            fld->is_read = true; // Field is accessed
                            expr->inferred_type = fld->type;
                            break;
                        } else if (strcmp(expr->member.member_name, "raw_ptr") != 0 &&
                                   strcmp(expr->member.member_name, "push") != 0 &&
                                   strcmp(expr->member.member_name, "clear") != 0) {
                            char msg[256];
                            snprintf(msg, sizeof(msg), "Record '%s' has no field '%s'", rec->name, expr->member.member_name);
                            sem_error(ctx->analyzer, expr->line, expr->col, msg);
                        }
                    }
                } else if (tgt_type->kind == TYPE_KIND_LIST) {
                    if (strcmp(expr->member.member_name, "len") == 0) {
                        expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_INT);
                        break;
                    }
                } else if (tgt_type->kind == TYPE_KIND_STR) {
                    if (strcmp(expr->member.member_name, "len") == 0) {
                        expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_INT);
                        break;
                    }
                } else if (tgt_type->kind == TYPE_KIND_CUSTOM) {
                    RecordDecl* rec = find_record(ctx, tgt_type->custom_name);
                    if (rec) {
                        RecordField* fld = find_record_field(rec, expr->member.member_name);
                        if (fld) {
                            fld->is_read = true;
                            expr->inferred_type = fld->type;
                            break;
                        } else {
                            char msg[256];
                            snprintf(msg, sizeof(msg), "Record '%s' has no field '%s'", rec->name, expr->member.member_name);
                            sem_error(ctx->analyzer, expr->line, expr->col, msg);
                        }
                    }
                }
            }
            expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_F64);
            break;
        }
        case EXPR_CALL: {
            analyze_expr(ctx, expr->call.callee);
            for (size_t i = 0; i < expr->call.arg_count; i++) {
                analyze_expr(ctx, expr->call.args[i]);
            }

            // Check if method call like arr.push(...) or str.contains(...)
            if (expr->call.callee && expr->call.callee->kind == EXPR_MEMBER) {
                const char* method = expr->call.callee->member.member_name;
                if (ctx->in_parallel_for) {
                    if (strcmp(method, "push") == 0 || strcmp(method, "clear") == 0) {
                        sem_error(ctx->analyzer, expr->line, expr->col,
                                  "Cannot mutate collections with .push() or .clear() inside parallel for");
                    }
                }
                TypeSpec* tgt_type = expr->call.callee->member.target ? expr->call.callee->member.target->inferred_type : NULL;
                if (tgt_type && tgt_type->kind == TYPE_KIND_STR) {
                    if (strcmp(method, "contains") == 0 ||
                        strcmp(method, "starts_with") == 0 ||
                        strcmp(method, "ends_with") == 0) {
                        expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_BOOL);
                        break;
                    }
                    if (strcmp(method, "trim") == 0 || strcmp(method, "slice") == 0) {
                        expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_STR);
                        break;
                    }
                    if (strcmp(method, "split") == 0) {
                        TypeSpec* lst = ast_create_type(ctx->analyzer->ws, TYPE_KIND_LIST);
                        lst->elem_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_STR);
                        expr->inferred_type = lst;
                        break;
                    }
                }
                if (strcmp(method, "push") == 0 || strcmp(method, "clear") == 0) {
                    expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_VOID);
                    break;
                }
                if (strcmp(method, "raw_ptr") == 0) {
                    expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_INT);
                    break;
                }
            }

            // Standard math and string functions, or user/extern function calls
            if (expr->call.callee && expr->call.callee->kind == EXPR_IDENT) {
                const char* fn = expr->call.callee->ident_name;
                if (strcmp(fn, "print") == 0 || strcmp(fn, "println") == 0) {
                    expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_VOID);
                    break;
                }
                if (strcmp(fn, "contains") == 0 ||
                    strcmp(fn, "starts_with") == 0 ||
                    strcmp(fn, "ends_with") == 0) {
                    expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_BOOL);
                    break;
                }
                if (strcmp(fn, "trim") == 0 || strcmp(fn, "slice") == 0) {
                    expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_STR);
                    break;
                }
                if (strcmp(fn, "split") == 0) {
                    TypeSpec* lst = ast_create_type(ctx->analyzer->ws, TYPE_KIND_LIST);
                    lst->elem_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_STR);
                    expr->inferred_type = lst;
                    break;
                }
                if (strcmp(fn, "len") == 0) {
                    expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_INT);
                    break;
                }
                if (strcmp(fn, "sqrt") == 0 || strcmp(fn, "sin") == 0 ||
                    strcmp(fn, "cos") == 0 || strcmp(fn, "tan") == 0 ||
                    strcmp(fn, "arcsin") == 0 || strcmp(fn, "arccos") == 0 ||
                    strcmp(fn, "arctan") == 0 || strcmp(fn, "cot") == 0 ||
                    strcmp(fn, "sec") == 0 || strcmp(fn, "csc") == 0 ||
                    strcmp(fn, "exp") == 0 || strcmp(fn, "log") == 0) {
                    expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_F64);
                    break;
                }
                if (strcmp(fn, "abs") == 0) {
                    if (expr->call.arg_count > 0 && expr->call.args[0]->inferred_type) {
                        expr->inferred_type = expr->call.args[0]->inferred_type;
                    } else {
                        expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_F64);
                    }
                    break;
                }

                FnDecl* fd = find_function(ctx, fn);
                if (fd) {
                    if (expr->call.arg_count != fd->param_count) {
                        char msg[256];
                        snprintf(msg, sizeof(msg), "Function '%s' expects %zu arguments, got %zu",
                                 fn, fd->param_count, expr->call.arg_count);
                        sem_error(ctx->analyzer, expr->line, expr->col, msg);
                    }
                    expr->inferred_type = fd->return_type;
                    break;
                } else {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Call to undeclared function '%s'", fn);
                    sem_error(ctx->analyzer, expr->line, expr->col, msg);
                }
            }

            expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_VOID);
            break;
        }
        case EXPR_INDEX: {
            analyze_expr(ctx, expr->index.target);
            analyze_expr(ctx, expr->index.index);
            TypeSpec* tgt = expr->index.target ? expr->index.target->inferred_type : NULL;
            if (tgt) {
                if (tgt->kind == TYPE_KIND_ARRAY || tgt->kind == TYPE_KIND_TABLE) {
                    TypeSpec* cust = ast_create_type(ctx->analyzer->ws, TYPE_KIND_CUSTOM);
                    cust->custom_name = tgt->custom_name;
                    expr->inferred_type = cust;
                    break;
                } else if (tgt->kind == TYPE_KIND_LIST) {
                    expr->inferred_type = tgt->elem_type;
                    break;
                }
            }
            expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_F64);
            break;
        }
        case EXPR_SIGMA:
            if (expr->sigma.from_expr) analyze_expr(ctx, expr->sigma.from_expr);
            analyze_expr(ctx, expr->sigma.limit_expr);
            if (expr->sigma.step_expr) analyze_expr(ctx, expr->sigma.step_expr);
            scope_push(ctx, false);
            symbol_add(ctx, "i", ast_create_type(ctx->analyzer->ws, TYPE_KIND_INT), false);
            analyze_expr(ctx, expr->sigma.body);
            scope_pop(ctx);
            expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_F64);
            break;
        case EXPR_INTEGRAL:
            analyze_expr(ctx, expr->integral.from_expr);
            analyze_expr(ctx, expr->integral.limit_expr);
            analyze_expr(ctx, expr->integral.step_expr);
            scope_push(ctx, false);
            symbol_add(ctx, "x", ast_create_type(ctx->analyzer->ws, TYPE_KIND_F64), false);
            analyze_expr(ctx, expr->integral.body);
            scope_pop(ctx);
            expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_F64);
            break;
        case EXPR_ARC:
            analyze_expr(ctx, expr->arc.operand);
            if (expr->arc.operand && expr->arc.operand->inferred_type) {
                TypeSpec* ot = expr->arc.operand->inferred_type;
                if (ot->kind == TYPE_KIND_ARRAY || ot->kind == TYPE_KIND_TABLE || ot->kind == TYPE_KIND_STR) {
                    expr->inferred_type = ot;
                    break;
                }
            }
            expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_F64);
            break;
        case EXPR_SEARCH:
            scope_push(ctx, false);
            symbol_add(ctx, "i", ast_create_type(ctx->analyzer->ws, TYPE_KIND_INT), false);
            analyze_expr(ctx, expr->search.condition);
            scope_pop(ctx);
            if (expr->search.or_default) analyze_expr(ctx, expr->search.or_default);
            expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_INT);
            break;
        case EXPR_LOAD: {
            TypeSpec* arr = ast_create_type(ctx->analyzer->ws, TYPE_KIND_ARRAY);
            arr->custom_name = expr->load.record_name;
            expr->inferred_type = arr;
            break;
        }
        case EXPR_READ_TEXT:
        case EXPR_ETCH:
            expr->inferred_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_STR);
            break;
        case EXPR_ARRAY_NEW: {
            if (ctx->in_parallel_for) {
                sem_error(ctx->analyzer, expr->line, expr->col, "Disallowed collection allocation inside parallel for");
            }
            if (expr->collection_new.capacity) analyze_expr(ctx, expr->collection_new.capacity);
            TypeSpec* arr = ast_create_type(ctx->analyzer->ws, TYPE_KIND_ARRAY);
            arr->custom_name = expr->collection_new.record_name;
            expr->inferred_type = arr;
            break;
        }
        case EXPR_TABLE_NEW: {
            if (ctx->in_parallel_for) {
                sem_error(ctx->analyzer, expr->line, expr->col, "Disallowed collection allocation inside parallel for");
            }
            if (expr->collection_new.capacity) analyze_expr(ctx, expr->collection_new.capacity);
            TypeSpec* tbl = ast_create_type(ctx->analyzer->ws, TYPE_KIND_TABLE);
            tbl->custom_name = expr->collection_new.record_name;
            expr->inferred_type = tbl;
            break;
        }
        case EXPR_LIST_NEW: {
            if (ctx->in_parallel_for) {
                sem_error(ctx->analyzer, expr->line, expr->col, "Disallowed collection allocation inside parallel for");
            }
            if (expr->collection_new.capacity) analyze_expr(ctx, expr->collection_new.capacity);
            TypeSpec* lst = ast_create_type(ctx->analyzer->ws, TYPE_KIND_LIST);
            lst->elem_type = expr->collection_new.elem_type;
            expr->inferred_type = lst;
            break;
        }
    }
}

static void analyze_stmt(AnalyzerContext* ctx, Stmt* stmt) {
    if (!stmt) return;

    switch (stmt->kind) {
        case STMT_WORKSPACE: {
            if (ctx->in_parallel_for) {
                sem_error(ctx->analyzer, stmt->line, stmt->col, "Disallowed workspace block inside parallel for");
            }
            TypeSpec* ws_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_INT);
            symbol_add(ctx, stmt->workspace.name, ws_type, false);

            scope_push(ctx, true);
            analyze_block(ctx, stmt->workspace.block);
            scope_pop(ctx);
            break;
        }
        case STMT_RESET_BLOCK: {
            if (ctx->in_parallel_for) {
                sem_error(ctx->analyzer, stmt->line, stmt->col, "Disallowed reset block inside parallel for");
            }
            scope_push(ctx, true);
            analyze_block(ctx, stmt->reset_block.block);
            scope_pop(ctx);
            break;
        }
        case STMT_RESET_CALL:
            break;
        case STMT_FOR_RANGE: {
            analyze_expr(ctx, stmt->for_range.start_expr);
            analyze_expr(ctx, stmt->for_range.end_expr);
            if (stmt->for_range.step_expr) analyze_expr(ctx, stmt->for_range.step_expr);

            bool prev_parallel = ctx->in_parallel_for;
            if (stmt->for_range.is_parallel) {
                ctx->in_parallel_for = true;
            }

            scope_push(ctx, false);
            TypeSpec* var_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_INT);
            symbol_add(ctx, stmt->for_range.var_name, var_type, false);
            analyze_block(ctx, stmt->for_range.block);
            scope_pop(ctx);

            ctx->in_parallel_for = prev_parallel;
            break;
        }
        case STMT_FOR_IN: {
            analyze_expr(ctx, stmt->for_in.iter_expr);

            scope_push(ctx, false);
            TypeSpec* elem_type = NULL;
            if (stmt->for_in.iter_expr && stmt->for_in.iter_expr->inferred_type) {
                TypeSpec* it = stmt->for_in.iter_expr->inferred_type;
                if (it->kind == TYPE_KIND_ARRAY || it->kind == TYPE_KIND_TABLE) {
                    elem_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_CUSTOM);
                    elem_type->custom_name = it->custom_name;
                } else if (it->kind == TYPE_KIND_LIST) {
                    elem_type = it->elem_type;
                }
            }
            if (!elem_type) {
                elem_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_F64);
            }
            symbol_add(ctx, stmt->for_in.var_name, elem_type, false);
            analyze_block(ctx, stmt->for_in.block);
            scope_pop(ctx);
            break;
        }
        case STMT_WHILE:
            analyze_expr(ctx, stmt->while_stmt.condition);
            scope_push(ctx, false);
            analyze_block(ctx, stmt->while_stmt.block);
            scope_pop(ctx);
            break;
        case STMT_DO_UNTIL:
            scope_push(ctx, false);
            analyze_block(ctx, stmt->do_until.block);
            scope_pop(ctx);
            analyze_expr(ctx, stmt->do_until.condition);
            break;
        case STMT_IF:
            analyze_expr(ctx, stmt->if_stmt.condition);
            scope_push(ctx, false);
            analyze_block(ctx, stmt->if_stmt.then_block);
            scope_pop(ctx);
            if (stmt->if_stmt.else_block) {
                scope_push(ctx, false);
                analyze_block(ctx, stmt->if_stmt.else_block);
                scope_pop(ctx);
            }
            break;
        case STMT_SORT:
            break;
        case STMT_SAVE:
            analyze_expr(ctx, stmt->save_stmt.expr);
            break;
        case STMT_STATE:
            for (size_t i = 0; i < stmt->state_stmt.count; i++) {
                analyze_expr(ctx, stmt->state_stmt.exprs[i]);
            }
            break;
        case STMT_RETURN:
            if (stmt->return_stmt.expr) {
                analyze_expr(ctx, stmt->return_stmt.expr);
            }
            break;
        case STMT_ASSIGN: {
            analyze_expr(ctx, stmt->assign_stmt.value);

            // Escape Safety Check:
            // If target is an identifier:
            if (stmt->assign_stmt.target->kind == EXPR_IDENT) {
                const char* target_name = stmt->assign_stmt.target->ident_name;
                Symbol* sym = symbol_lookup(ctx, target_name);

                if (!sym) {
                    // New variable declaration in current scope
                    TypeSpec* assigned_type = stmt->assign_stmt.value ? stmt->assign_stmt.value->inferred_type : NULL;
                    if (!assigned_type) {
                        assigned_type = ast_create_type(ctx->analyzer->ws, TYPE_KIND_F64);
                    }
                    symbol_add(ctx, target_name, assigned_type, false);
                    stmt->assign_stmt.target->inferred_type = assigned_type;
                    stmt->assign_stmt.is_declaration = true;
                } else {
                    stmt->assign_stmt.target->inferred_type = sym->type;
                    stmt->assign_stmt.is_declaration = false;
                    int lhs_depth = sym->scope_depth;
                    int rhs_depth = ctx->current_scope ? ctx->current_scope->depth : 0;

                    // If RHS is also a symbol, get its specific definition depth
                    if (stmt->assign_stmt.value && stmt->assign_stmt.value->kind == EXPR_IDENT) {
                        Symbol* rhs_sym = symbol_lookup(ctx, stmt->assign_stmt.value->ident_name);
                        if (rhs_sym) {
                            rhs_depth = rhs_sym->scope_depth;
                        }
                    }

                    // Check escape violation:
                    // lhs.scope_depth < rhs.scope_depth AND rhs is workspace-allocated reference/slice
                    if (lhs_depth < rhs_depth) {
                        TypeSpec* rhs_type = stmt->assign_stmt.value ? stmt->assign_stmt.value->inferred_type : NULL;
                        if (ast_type_is_reference(rhs_type)) {
                            sem_error(ctx->analyzer, stmt->line, stmt->col,
                                      "Memory escape violation — cannot assign inner workspace/reset data to outer scope.");
                        }
                    }
                }
            } else {
                analyze_expr(ctx, stmt->assign_stmt.target);
            }
            break;
        }
        case STMT_EXPR:
            analyze_expr(ctx, stmt->expr_stmt.expr);
            break;
    }
}

static void analyze_block(AnalyzerContext* ctx, Block* block) {
    if (!block) return;
    for (size_t i = 0; i < block->count; i++) {
        analyze_stmt(ctx, block->statements[i]);
    }
}

void analyzer_init(Analyzer* analyzer, SlabWorkspace* ws, Program* program, bool pack_enabled, const char* filename) {
    analyzer->ws = ws;
    analyzer->program = program;
    analyzer->pack_enabled = pack_enabled;
    analyzer->had_error = false;
    analyzer->filename = filename;
}

bool analyzer_analyze(Analyzer* analyzer) {
    AnalyzerContext ctx;
    ctx.analyzer = analyzer;
    ctx.current_scope = NULL;
    ctx.records = analyzer->program->records;
    ctx.record_count = analyzer->program->record_count;
    ctx.functions = analyzer->program->functions;
    ctx.fn_count = analyzer->program->fn_count;
    ctx.in_parallel_for = false;

    // Validate type definitions
    for (size_t r = 0; r < ctx.record_count; r++) {
        RecordDecl* rec = ctx.records[r];
        for (size_t f = 0; f < rec->field_count; f++) {
            TypeSpec* ft = rec->fields[f].type;
            if (ft && (ft->kind == TYPE_KIND_CUSTOM || ft->kind == TYPE_KIND_ARRAY || ft->kind == TYPE_KIND_TABLE)) {
                if (ft->custom_name && !find_record(&ctx, ft->custom_name)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Unknown type '%s' in field '%s'", ft->custom_name, rec->fields[f].name);
                    sem_error(analyzer, rec->fields[f].line, rec->fields[f].col, msg);
                }
            }
        }
    }

    // Phase 2 --pack: if pack_enabled, initialize all record fields as unread
    if (analyzer->pack_enabled) {
        for (size_t r = 0; r < ctx.record_count; r++) {
            RecordDecl* rec = ctx.records[r];
            for (size_t f = 0; f < rec->field_count; f++) {
                rec->fields[f].is_read = false;
            }
        }
    }

    // Push global scope
    scope_push(&ctx, false);

    // Register top-level constants
    for (size_t i = 0; i < analyzer->program->const_count; i++) {
        ConstDecl* cd = analyzer->program->consts[i];
        analyze_expr(&ctx, cd->value);
        symbol_add(&ctx, cd->name, cd->value ? cd->value->inferred_type : NULL, true);
    }

    // Analyze functions
    for (size_t i = 0; i < analyzer->program->fn_count; i++) {
        FnDecl* fn = analyzer->program->functions[i];
        if (fn->is_extern || !fn->body) continue;
        scope_push(&ctx, false);

        for (size_t p = 0; p < fn->param_count; p++) {
            symbol_add(&ctx, fn->params[p].name, fn->params[p].type, false);
        }

        analyze_block(&ctx, fn->body);
        scope_pop(&ctx);
    }

    scope_pop(&ctx); // pop global scope
    return !analyzer->had_error;
}
