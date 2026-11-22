#include "codegen.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static void emit_indent(Codegen* cg) {
    for (int i = 0; i < cg->indent_level; i++) {
        fprintf(cg->out, "    ");
    }
}

static void emit_escaped_c_str(FILE* out, const char* str, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = str[i];
        switch (c) {
            case '"': fprintf(out, "\\\""); break;
            case '\\': fprintf(out, "\\\\"); break;
            case '\n': fprintf(out, "\\n"); break;
            case '\t': fprintf(out, "\\t"); break;
            case '\r': fprintf(out, "\\r"); break;
            default:
                if ((unsigned char)c < 32 || (unsigned char)c >= 127) {
                    fprintf(out, "\\x%02x", (unsigned char)c);
                } else {
                    fputc(c, out);
                }
                break;
        }
    }
}

static const char* c_type_str(Codegen* cg, const TypeSpec* type) {
    if (!type) return "double";
    switch (type->kind) {
        case TYPE_KIND_INT:
            if (cg->pack_enabled && type->has_bounds) {
                if (type->int_min >= 0 && type->int_max <= 255) return "uint8_t";
                if (type->int_min >= -128 && type->int_max <= 127) return "int8_t";
                if (type->int_min >= 0 && type->int_max <= 65535) return "uint16_t";
                if (type->int_min >= -32768 && type->int_max <= 32767) return "int16_t";
                if (type->int_min >= 0 && type->int_max <= 4294967295LL) return "uint32_t";
            }
            return "int64_t";
        case TYPE_KIND_I32: return "int32_t";
        case TYPE_KIND_U16: return "uint16_t";
        case TYPE_KIND_U32: return "uint32_t";
        case TYPE_KIND_F32: return "float";
        case TYPE_KIND_F64: return "double";
        case TYPE_KIND_BOOL: return "bool";
        case TYPE_KIND_STR: return "SlabStr";
        case TYPE_KIND_VOID: return "void";
        case TYPE_KIND_ARRAY: {
            char* buf = (char*)slab_alloc(cg->ws, 128, 1);
            snprintf(buf, 128, "%s_SoA", type->custom_name ? type->custom_name : "Item");
            return buf;
        }
        case TYPE_KIND_TABLE: {
            char* buf = (char*)slab_alloc(cg->ws, 128, 1);
            snprintf(buf, 128, "%s_AoS", type->custom_name ? type->custom_name : "Item");
            return buf;
        }
        case TYPE_KIND_LIST: {
            char* buf = (char*)slab_alloc(cg->ws, 128, 1);
            if (type->elem_type && type->elem_type->kind == TYPE_KIND_STR) {
                snprintf(buf, 128, "List_SlabStr");
            } else {
                snprintf(buf, 128, "List_%s", type->elem_type ? c_type_str(cg, type->elem_type) : "f64");
            }
            return buf;
        }
        case TYPE_KIND_CUSTOM:
            return type->custom_name ? type->custom_name : "void*";
        default:
            return "double";
    }
}

static bool is_packed_bool_field(Codegen* cg, Expr* expr) {
    if (!cg->pack_enabled || !expr) return false;
    if (expr->kind != EXPR_MEMBER) return false;
    Expr* tgt = expr->member.target;
    if (!tgt || !tgt->inferred_type || tgt->inferred_type->kind != TYPE_KIND_ARRAY) return false;
    const char* rec_name = tgt->inferred_type->custom_name;
    const char* fld_name = expr->member.member_name;
    if (!rec_name || !fld_name) return false;
    for (size_t r = 0; r < cg->program->record_count; r++) {
        if (strcmp(cg->program->records[r]->name, rec_name) == 0) {
            RecordDecl* rec = cg->program->records[r];
            for (size_t f = 0; f < rec->field_count; f++) {
                if (strcmp(rec->fields[f].name, fld_name) == 0) {
                    return rec->fields[f].type && rec->fields[f].type->kind == TYPE_KIND_BOOL;
                }
            }
        }
    }
    return false;
}

static void emit_expr(Codegen* cg, Expr* expr);
static void emit_stmt(Codegen* cg, Stmt* stmt);
static void emit_block(Codegen* cg, Block* block);

typedef struct {
    const char* name;
    TypeSpec* type;
} CapturedVar;

typedef struct {
    CapturedVar vars[32];
    size_t count;
    const char* loop_var;
    Program* prog;
} CaptureCollector;

static void collect_captured_stmt(CaptureCollector* col, Stmt* stmt, const char** locals, size_t* local_count);

static void collect_captured_expr(CaptureCollector* col, Expr* expr, const char** locals, size_t local_count) {
    if (!expr) return;
    switch (expr->kind) {
        case EXPR_IDENT: {
            if (strcmp(expr->ident_name, col->loop_var) == 0) return;
            for (size_t i = 0; i < local_count; i++) {
                if (strcmp(locals[i], expr->ident_name) == 0) return;
            }
            for (size_t i = 0; i < col->prog->const_count; i++) {
                if (strcmp(col->prog->consts[i]->name, expr->ident_name) == 0) return;
            }
            if (strcmp(expr->ident_name, "print") == 0 || strcmp(expr->ident_name, "println") == 0) return;
            for (size_t i = 0; i < col->count; i++) {
                if (strcmp(col->vars[i].name, expr->ident_name) == 0) return;
            }
            if (col->count < 32) {
                col->vars[col->count].name = expr->ident_name;
                col->vars[col->count].type = expr->inferred_type;
                col->count++;
            }
            break;
        }
        case EXPR_BINARY:
            collect_captured_expr(col, expr->binary.left, locals, local_count);
            collect_captured_expr(col, expr->binary.right, locals, local_count);
            break;
        case EXPR_UNARY:
            collect_captured_expr(col, expr->unary.operand, locals, local_count);
            break;
        case EXPR_CALL:
            collect_captured_expr(col, expr->call.callee, locals, local_count);
            for (size_t i = 0; i < expr->call.arg_count; i++) {
                collect_captured_expr(col, expr->call.args[i], locals, local_count);
            }
            break;
        case EXPR_MEMBER:
            collect_captured_expr(col, expr->member.target, locals, local_count);
            break;
        case EXPR_INDEX:
            collect_captured_expr(col, expr->index.target, locals, local_count);
            collect_captured_expr(col, expr->index.index, locals, local_count);
            break;
        case EXPR_SIGMA:
            if (expr->sigma.from_expr) collect_captured_expr(col, expr->sigma.from_expr, locals, local_count);
            collect_captured_expr(col, expr->sigma.limit_expr, locals, local_count);
            if (expr->sigma.step_expr) collect_captured_expr(col, expr->sigma.step_expr, locals, local_count);
            collect_captured_expr(col, expr->sigma.body, locals, local_count);
            break;
        case EXPR_INTEGRAL:
            collect_captured_expr(col, expr->integral.from_expr, locals, local_count);
            collect_captured_expr(col, expr->integral.limit_expr, locals, local_count);
            collect_captured_expr(col, expr->integral.step_expr, locals, local_count);
            collect_captured_expr(col, expr->integral.body, locals, local_count);
            break;
        case EXPR_ARC:
            collect_captured_expr(col, expr->arc.operand, locals, local_count);
            break;
        case EXPR_SEARCH:
            collect_captured_expr(col, expr->search.condition, locals, local_count);
            if (expr->search.or_default) collect_captured_expr(col, expr->search.or_default, locals, local_count);
            break;
        default:
            break;
    }
}

static void collect_captured_stmt(CaptureCollector* col, Stmt* stmt, const char** locals, size_t* local_count) {
    if (!stmt) return;
    switch (stmt->kind) {
        case STMT_ASSIGN:
            if (stmt->assign_stmt.is_declaration && stmt->assign_stmt.target->kind == EXPR_IDENT) {
                if (*local_count < 64) {
                    locals[(*local_count)++] = stmt->assign_stmt.target->ident_name;
                }
            } else {
                collect_captured_expr(col, stmt->assign_stmt.target, locals, *local_count);
            }
            collect_captured_expr(col, stmt->assign_stmt.value, locals, *local_count);
            break;
        case STMT_EXPR:
            collect_captured_expr(col, stmt->expr_stmt.expr, locals, *local_count);
            break;
        case STMT_IF:
            collect_captured_expr(col, stmt->if_stmt.condition, locals, *local_count);
            if (stmt->if_stmt.then_block) {
                for (size_t i = 0; i < stmt->if_stmt.then_block->count; i++) {
                    collect_captured_stmt(col, stmt->if_stmt.then_block->statements[i], locals, local_count);
                }
            }
            if (stmt->if_stmt.else_block) {
                for (size_t i = 0; i < stmt->if_stmt.else_block->count; i++) {
                    collect_captured_stmt(col, stmt->if_stmt.else_block->statements[i], locals, local_count);
                }
            }
            break;
        case STMT_WHILE:
            collect_captured_expr(col, stmt->while_stmt.condition, locals, *local_count);
            if (stmt->while_stmt.block) {
                for (size_t i = 0; i < stmt->while_stmt.block->count; i++) {
                    collect_captured_stmt(col, stmt->while_stmt.block->statements[i], locals, local_count);
                }
            }
            break;
        default:
            break;
    }
}

static void emit_expr(Codegen* cg, Expr* expr) {
    if (!expr) return;

    switch (expr->kind) {
        case EXPR_INT_LIT:
            fprintf(cg->out, "%lldLL", (long long)expr->int_val);
            break;
        case EXPR_FLOAT_LIT:
            fprintf(cg->out, "%f", expr->float_val);
            break;
        case EXPR_STRING_LIT:
            fprintf(cg->out, "(SlabStr){ .ptr = \"");
            emit_escaped_c_str(cg->out, expr->str_val.chars ? expr->str_val.chars : "", expr->str_val.len);
            fprintf(cg->out, "\", .len = %zu }", expr->str_val.len);
            break;
        case EXPR_BOOL_LIT:
            fprintf(cg->out, "%s", expr->bool_val ? "true" : "false");
            break;
        case EXPR_MATH_CONST:
            if (strcmp(expr->ident_name, "pi") == 0) {
                fprintf(cg->out, "M_PI");
            } else if (strcmp(expr->ident_name, "e") == 0) {
                fprintf(cg->out, "M_E");
            } else if (strcmp(expr->ident_name, "tau") == 0) {
                fprintf(cg->out, "M_TAU");
            } else {
                fprintf(cg->out, "0.0");
            }
            break;
        case EXPR_IDENT:
            fprintf(cg->out, "%s", expr->ident_name);
            break;
        case EXPR_BINARY:
            if (expr->binary.op == TOKEN_PLUS &&
                ((expr->binary.left && expr->binary.left->inferred_type && expr->binary.left->inferred_type->kind == TYPE_KIND_STR) ||
                 (expr->binary.right && expr->binary.right->inferred_type && expr->binary.right->inferred_type->kind == TYPE_KIND_STR))) {
                fprintf(cg->out, "slab_str_concat(%s, ", cg->current_ws_var);
                emit_expr(cg, expr->binary.left);
                fprintf(cg->out, ", ");
                emit_expr(cg, expr->binary.right);
                fprintf(cg->out, ")");
            } else if (expr->binary.op == TOKEN_CARET) {
                fprintf(cg->out, "pow(");
                emit_expr(cg, expr->binary.left);
                fprintf(cg->out, ", ");
                emit_expr(cg, expr->binary.right);
                fprintf(cg->out, ")");
            } else if (expr->binary.op == TOKEN_AT) {
                fprintf(cg->out, "slab_dot_product(");
                emit_expr(cg, expr->binary.left);
                fprintf(cg->out, ", ");
                emit_expr(cg, expr->binary.right);
                fprintf(cg->out, ")");
            } else {
                fprintf(cg->out, "(");
                emit_expr(cg, expr->binary.left);
                switch (expr->binary.op) {
                    case TOKEN_PLUS: fprintf(cg->out, " + "); break;
                    case TOKEN_MINUS: fprintf(cg->out, " - "); break;
                    case TOKEN_STAR: fprintf(cg->out, " * "); break;
                    case TOKEN_SLASH: fprintf(cg->out, " / "); break;
                    case TOKEN_PERCENT: fprintf(cg->out, " %% "); break;
                    case TOKEN_EQ_EQ: fprintf(cg->out, " == "); break;
                    case TOKEN_BANG_EQ: fprintf(cg->out, " != "); break;
                    case TOKEN_LT: fprintf(cg->out, " < "); break;
                    case TOKEN_LT_EQ: fprintf(cg->out, " <= "); break;
                    case TOKEN_GT: fprintf(cg->out, " > "); break;
                    case TOKEN_GT_EQ: fprintf(cg->out, " >= "); break;
                    case TOKEN_AND_AND: fprintf(cg->out, " && "); break;
                    case TOKEN_OR_OR: fprintf(cg->out, " || "); break;
                    default: fprintf(cg->out, " ? "); break;
                }
                emit_expr(cg, expr->binary.right);
                fprintf(cg->out, ")");
            }
            break;
        case EXPR_UNARY:
            if (expr->unary.op == TOKEN_MINUS) {
                fprintf(cg->out, "(-");
                emit_expr(cg, expr->unary.operand);
                fprintf(cg->out, ")");
            } else if (expr->unary.op == TOKEN_BANG) {
                fprintf(cg->out, "(!");
                emit_expr(cg, expr->unary.operand);
                fprintf(cg->out, ")");
            }
            break;
        case EXPR_CALL: {
            if (expr->call.callee && expr->call.callee->kind == EXPR_MEMBER) {
                Expr* target = expr->call.callee->member.target;
                const char* method = expr->call.callee->member.member_name;

                // String method calls
                if (target->inferred_type && target->inferred_type->kind == TYPE_KIND_STR) {
                    if (strcmp(method, "contains") == 0 && expr->call.arg_count > 0) {
                        fprintf(cg->out, "slab_str_contains(");
                        emit_expr(cg, target);
                        fprintf(cg->out, ", ");
                        emit_expr(cg, expr->call.args[0]);
                        fprintf(cg->out, ")");
                        break;
                    }
                    if (strcmp(method, "starts_with") == 0 && expr->call.arg_count > 0) {
                        fprintf(cg->out, "slab_str_starts_with(");
                        emit_expr(cg, target);
                        fprintf(cg->out, ", ");
                        emit_expr(cg, expr->call.args[0]);
                        fprintf(cg->out, ")");
                        break;
                    }
                    if (strcmp(method, "ends_with") == 0 && expr->call.arg_count > 0) {
                        fprintf(cg->out, "slab_str_ends_with(");
                        emit_expr(cg, target);
                        fprintf(cg->out, ", ");
                        emit_expr(cg, expr->call.args[0]);
                        fprintf(cg->out, ")");
                        break;
                    }
                    if (strcmp(method, "trim") == 0) {
                        fprintf(cg->out, "slab_str_trim(");
                        emit_expr(cg, target);
                        fprintf(cg->out, ")");
                        break;
                    }
                    if (strcmp(method, "slice") == 0 && expr->call.arg_count >= 2) {
                        fprintf(cg->out, "slab_str_slice(");
                        emit_expr(cg, target);
                        fprintf(cg->out, ", ");
                        emit_expr(cg, expr->call.args[0]);
                        fprintf(cg->out, ", ");
                        emit_expr(cg, expr->call.args[1]);
                        fprintf(cg->out, ")");
                        break;
                    }
                    if (strcmp(method, "split") == 0 && expr->call.arg_count > 0) {
                        fprintf(cg->out, "slab_str_split(%s, ", cg->current_ws_var);
                        emit_expr(cg, target);
                        fprintf(cg->out, ", ");
                        emit_expr(cg, expr->call.args[0]);
                        fprintf(cg->out, ")");
                        break;
                    }
                }

                if (strcmp(method, "push") == 0) {
                    const char* rec_name = target->inferred_type ? target->inferred_type->custom_name : "Item";
                    bool is_aos = (target->inferred_type && target->inferred_type->kind == TYPE_KIND_TABLE);
                    if (is_aos) {
                        fprintf(cg->out, "%s_AoS_push(&", rec_name);
                    } else {
                        fprintf(cg->out, "%s_SoA_push(&", rec_name);
                    }
                    emit_expr(cg, target);

                    RecordDecl* rec = NULL;
                    if (!is_aos && cg->pack_enabled) {
                        for (size_t r = 0; r < cg->program->record_count; r++) {
                            if (strcmp(cg->program->records[r]->name, rec_name) == 0) {
                                rec = cg->program->records[r];
                                break;
                            }
                        }
                    }

                    if (is_aos) {
                        fprintf(cg->out, ", (%s){ ", rec_name);
                        for (size_t i = 0; i < expr->call.arg_count; i++) {
                            if (i > 0) fprintf(cg->out, ", ");
                            emit_expr(cg, expr->call.args[i]);
                        }
                        fprintf(cg->out, " }");
                    } else {
                        for (size_t i = 0; i < expr->call.arg_count; i++) {
                            if (rec && i < rec->field_count && !rec->fields[i].is_read) {
                                continue;
                            }
                            fprintf(cg->out, ", ");
                            emit_expr(cg, expr->call.args[i]);
                        }
                    }
                    fprintf(cg->out, ")");
                    break;
                }
                if (strcmp(method, "clear") == 0) {
                    emit_expr(cg, target);
                    fprintf(cg->out, ".len = 0");
                    break;
                }
                if (strcmp(method, "raw_ptr") == 0) {
                    fprintf(cg->out, "(void*)(");
                    emit_expr(cg, target);
                    fprintf(cg->out, ".data)");
                    break;
                }
            }

            // Standalone built-ins (string & math)
            if (expr->call.callee && expr->call.callee->kind == EXPR_IDENT) {
                const char* fn = expr->call.callee->ident_name;

                if (strcmp(fn, "contains") == 0 && expr->call.arg_count >= 2) {
                    fprintf(cg->out, "slab_str_contains(");
                    emit_expr(cg, expr->call.args[0]);
                    fprintf(cg->out, ", ");
                    emit_expr(cg, expr->call.args[1]);
                    fprintf(cg->out, ")");
                    break;
                }
                if (strcmp(fn, "starts_with") == 0 && expr->call.arg_count >= 2) {
                    fprintf(cg->out, "slab_str_starts_with(");
                    emit_expr(cg, expr->call.args[0]);
                    fprintf(cg->out, ", ");
                    emit_expr(cg, expr->call.args[1]);
                    fprintf(cg->out, ")");
                    break;
                }
                if (strcmp(fn, "ends_with") == 0 && expr->call.arg_count >= 2) {
                    fprintf(cg->out, "slab_str_ends_with(");
                    emit_expr(cg, expr->call.args[0]);
                    fprintf(cg->out, ", ");
                    emit_expr(cg, expr->call.args[1]);
                    fprintf(cg->out, ")");
                    break;
                }
                if (strcmp(fn, "trim") == 0 && expr->call.arg_count >= 1) {
                    fprintf(cg->out, "slab_str_trim(");
                    emit_expr(cg, expr->call.args[0]);
                    fprintf(cg->out, ")");
                    break;
                }
                if (strcmp(fn, "slice") == 0 && expr->call.arg_count >= 3) {
                    fprintf(cg->out, "slab_str_slice(");
                    emit_expr(cg, expr->call.args[0]);
                    fprintf(cg->out, ", ");
                    emit_expr(cg, expr->call.args[1]);
                    fprintf(cg->out, ", ");
                    emit_expr(cg, expr->call.args[2]);
                    fprintf(cg->out, ")");
                    break;
                }
                if (strcmp(fn, "split") == 0 && expr->call.arg_count >= 2) {
                    fprintf(cg->out, "slab_str_split(%s, ", cg->current_ws_var);
                    emit_expr(cg, expr->call.args[0]);
                    fprintf(cg->out, ", ");
                    emit_expr(cg, expr->call.args[1]);
                    fprintf(cg->out, ")");
                    break;
                }
                if (strcmp(fn, "len") == 0 && expr->call.arg_count >= 1) {
                    fprintf(cg->out, "((int64_t)(");
                    emit_expr(cg, expr->call.args[0]);
                    fprintf(cg->out, ").len)");
                    break;
                }

                // Math inverse trig & functions
                if (strcmp(fn, "arcsin") == 0 && expr->call.arg_count > 0) {
                    fprintf(cg->out, "asin("); emit_expr(cg, expr->call.args[0]); fprintf(cg->out, ")");
                    break;
                }
                if (strcmp(fn, "arccos") == 0 && expr->call.arg_count > 0) {
                    fprintf(cg->out, "acos("); emit_expr(cg, expr->call.args[0]); fprintf(cg->out, ")");
                    break;
                }
                if (strcmp(fn, "arctan") == 0 && expr->call.arg_count > 0) {
                    fprintf(cg->out, "atan("); emit_expr(cg, expr->call.args[0]); fprintf(cg->out, ")");
                    break;
                }
                if (strcmp(fn, "cot") == 0 && expr->call.arg_count > 0) {
                    fprintf(cg->out, "slab_cot("); emit_expr(cg, expr->call.args[0]); fprintf(cg->out, ")");
                    break;
                }
                if (strcmp(fn, "sec") == 0 && expr->call.arg_count > 0) {
                    fprintf(cg->out, "slab_sec("); emit_expr(cg, expr->call.args[0]); fprintf(cg->out, ")");
                    break;
                }
                if (strcmp(fn, "csc") == 0 && expr->call.arg_count > 0) {
                    fprintf(cg->out, "slab_csc("); emit_expr(cg, expr->call.args[0]); fprintf(cg->out, ")");
                    break;
                }

                // Println / print built-ins
                if (strcmp(fn, "println") == 0 || strcmp(fn, "print") == 0) {
                    bool nl = (strcmp(fn, "println") == 0);
                    if (expr->call.arg_count == 0) {
                        if (nl) fprintf(cg->out, "printf(\"\\n\")");
                        break;
                    }
                    for (size_t i = 0; i < expr->call.arg_count; i++) {
                        Expr* a = expr->call.args[i];
                        if (a->inferred_type && a->inferred_type->kind == TYPE_KIND_STR) {
                            fprintf(cg->out, "slab_print_str(");
                            emit_expr(cg, a);
                            fprintf(cg->out, ")");
                        } else if (a->inferred_type && (a->inferred_type->kind == TYPE_KIND_F32 || a->inferred_type->kind == TYPE_KIND_F64)) {
                            fprintf(cg->out, "printf(\"%%g\", (double)(");
                            emit_expr(cg, a);
                            fprintf(cg->out, "))");
                        } else if (a->inferred_type && a->inferred_type->kind == TYPE_KIND_BOOL) {
                            fprintf(cg->out, "printf(\"%%s\", (");
                            emit_expr(cg, a);
                            fprintf(cg->out, ") ? \"true\" : \"false\")");
                        } else {
                            fprintf(cg->out, "printf(\"%%lld\", (long long)(");
                            emit_expr(cg, a);
                            fprintf(cg->out, "))");
                        }
                        if (i + 1 < expr->call.arg_count) {
                            fprintf(cg->out, ", printf(\" \"), ");
                        }
                    }
                    if (nl) {
                        fprintf(cg->out, ", printf(\"\\n\")");
                    }
                    break;
                }
            }

            emit_expr(cg, expr->call.callee);
            fprintf(cg->out, "(");
            for (size_t i = 0; i < expr->call.arg_count; i++) {
                if (i > 0) fprintf(cg->out, ", ");
                emit_expr(cg, expr->call.args[i]);
            }
            fprintf(cg->out, ")");
            break;
        }
        case EXPR_MEMBER:
            emit_expr(cg, expr->member.target);
            fprintf(cg->out, ".%s", expr->member.member_name);
            break;
        case EXPR_INDEX:
            if (is_packed_bool_field(cg, expr->index.target)) {
                fprintf(cg->out, "((((");
                emit_expr(cg, expr->index.target->member.target);
                fprintf(cg->out, ").%s[(", expr->index.target->member.member_name);
                emit_expr(cg, expr->index.index);
                fprintf(cg->out, ") >> 6] >> ((");
                emit_expr(cg, expr->index.index);
                fprintf(cg->out, ") & 63)) & 1ULL) != 0)");
            } else {
                emit_expr(cg, expr->index.target);
                fprintf(cg->out, "[");
                emit_expr(cg, expr->index.index);
                fprintf(cg->out, "]");
            }
            break;
        case EXPR_SIGMA:
            fprintf(cg->out, "({ double _sum = 0.0; double _step = ");
            if (expr->sigma.step_expr) {
                emit_expr(cg, expr->sigma.step_expr);
            } else {
                fprintf(cg->out, "1.0");
            }
            fprintf(cg->out, "; for (double i = ");
            if (expr->sigma.from_expr) {
                emit_expr(cg, expr->sigma.from_expr);
            } else {
                fprintf(cg->out, "0.0");
            }
            fprintf(cg->out, "; i <= (");
            emit_expr(cg, expr->sigma.limit_expr);
            fprintf(cg->out, "); i += _step) { _sum += (");
            emit_expr(cg, expr->sigma.body);
            fprintf(cg->out, "); } _sum; })");
            break;
        case EXPR_INTEGRAL:
            fprintf(cg->out, "({ double _int_sum = 0.0; double _a = (");
            emit_expr(cg, expr->integral.from_expr);
            fprintf(cg->out, "); double _b = (");
            emit_expr(cg, expr->integral.limit_expr);
            fprintf(cg->out, "); double _h = (");
            emit_expr(cg, expr->integral.step_expr);
            fprintf(cg->out, "); for (double x = _a; x < _b; x += _h) { _int_sum += (");
            emit_expr(cg, expr->integral.body);
            fprintf(cg->out, ") * _h; } _int_sum; })");
            break;
        case EXPR_ARC: {
            Expr* op = expr->arc.operand;
            if (op && op->kind == EXPR_CALL && op->call.callee && op->call.callee->kind == EXPR_IDENT) {
                const char* fn = op->call.callee->ident_name;
                if (strcmp(fn, "sin") == 0 && op->call.arg_count > 0) {
                    fprintf(cg->out, "asin("); emit_expr(cg, op->call.args[0]); fprintf(cg->out, ")");
                    break;
                }
                if (strcmp(fn, "cos") == 0 && op->call.arg_count > 0) {
                    fprintf(cg->out, "acos("); emit_expr(cg, op->call.args[0]); fprintf(cg->out, ")");
                    break;
                }
                if (strcmp(fn, "tan") == 0 && op->call.arg_count > 0) {
                    fprintf(cg->out, "atan("); emit_expr(cg, op->call.args[0]); fprintf(cg->out, ")");
                    break;
                }
            }
            if (op && op->inferred_type && op->inferred_type->kind == TYPE_KIND_STR) {
                fprintf(cg->out, "({ SlabStr _s = ");
                emit_expr(cg, op);
                fprintf(cg->out, "; slab_str_reverse_in_place(_s); _s; })");
                break;
            }
            if (op && op->inferred_type && op->inferred_type->kind == TYPE_KIND_ARRAY) {
                const char* rec_name = op->inferred_type->custom_name ? op->inferred_type->custom_name : "Item";
                fprintf(cg->out, "({ %s_SoA_reverse_in_place(&", rec_name);
                emit_expr(cg, op);
                fprintf(cg->out, "); ");
                emit_expr(cg, op);
                fprintf(cg->out, "; })");
                break;
            }
            fprintf(cg->out, "asin(");
            emit_expr(cg, op);
            fprintf(cg->out, ")");
            break;
        }
        case EXPR_SEARCH:
            fprintf(cg->out, "({ int64_t _res = -1; ");
            fprintf(cg->out, "for (size_t _idx = 0; _idx < (%s).len; _idx++) { ", expr->search.ident);
            fprintf(cg->out, "size_t i = _idx; (void)i; if ");
            emit_expr(cg, expr->search.condition);
            fprintf(cg->out, " { _res = (int64_t)_idx; break; } } ");
            fprintf(cg->out, "if (_res == -1) _res = ");
            if (expr->search.or_default) {
                emit_expr(cg, expr->search.or_default);
            } else {
                fprintf(cg->out, "-1");
            }
            fprintf(cg->out, "; _res; })");
            break;
        case EXPR_LOAD:
            fprintf(cg->out, "%s_SoA_load(%s, \"%s\")",
                    expr->load.record_name ? expr->load.record_name : "Item",
                    cg->current_ws_var,
                    expr->load.path ? expr->load.path : "");
            break;
        case EXPR_READ_TEXT:
            fprintf(cg->out, "slab_read_file(%s, \"%s\")",
                    cg->current_ws_var,
                    expr->read_text.path ? expr->read_text.path : "");
            break;
        case EXPR_ETCH:
            fprintf(cg->out, "({ static const char _etch_str[] = \"");
            emit_escaped_c_str(cg->out, expr->etch.etch_str ? expr->etch.etch_str : "", strlen(expr->etch.etch_str ? expr->etch.etch_str : ""));
            fprintf(cg->out, "\"; (SlabStr){ .ptr = _etch_str, .len = sizeof(_etch_str) - 1 }; })");
            break;
        case EXPR_ARRAY_NEW:
            fprintf(cg->out, "%s_SoA_create(%s, ",
                    expr->collection_new.record_name ? expr->collection_new.record_name : "Item",
                    cg->current_ws_var);
            if (expr->collection_new.capacity) {
                emit_expr(cg, expr->collection_new.capacity);
            } else {
                fprintf(cg->out, "1024");
            }
            fprintf(cg->out, ")");
            break;
        case EXPR_TABLE_NEW:
            fprintf(cg->out, "%s_AoS_create(%s, ",
                    expr->collection_new.record_name ? expr->collection_new.record_name : "Item",
                    cg->current_ws_var);
            if (expr->collection_new.capacity) {
                emit_expr(cg, expr->collection_new.capacity);
            } else {
                fprintf(cg->out, "1024");
            }
            fprintf(cg->out, ")");
            break;
        case EXPR_LIST_NEW:
            fprintf(cg->out, "List_SlabStr_create(%s, 8)", cg->current_ws_var);
            break;
    }
}

static void emit_stmt(Codegen* cg, Stmt* stmt) {
    if (!stmt) return;

    emit_indent(cg);
    switch (stmt->kind) {
        case STMT_WORKSPACE: {
            const char* old_ws = cg->current_ws_var;
            char ws_var[64];
            snprintf(ws_var, sizeof(ws_var), "&_ws_%s", stmt->workspace.name);
            fprintf(cg->out, "{\n");
            cg->indent_level++;
            emit_indent(cg);
            fprintf(cg->out, "SlabWorkspace _ws_%s = {0};\n", stmt->workspace.name);

            cg->current_ws_var = ws_var;
            emit_block(cg, stmt->workspace.block);

            emit_indent(cg);
            fprintf(cg->out, "slab_workspace_destroy(&_ws_%s);\n", stmt->workspace.name);
            cg->indent_level--;
            emit_indent(cg);
            fprintf(cg->out, "}\n");
            cg->current_ws_var = old_ws;
            break;
        }
        case STMT_RESET_BLOCK: {
            fprintf(cg->out, "{\n");
            cg->indent_level++;
            emit_indent(cg);
            fprintf(cg->out, "SlabCheckpoint _cp = slab_save(%s);\n", cg->current_ws_var);
            emit_block(cg, stmt->reset_block.block);
            emit_indent(cg);
            fprintf(cg->out, "slab_restore(%s, _cp);\n", cg->current_ws_var);
            cg->indent_level--;
            emit_indent(cg);
            fprintf(cg->out, "}\n");
            break;
        }
        case STMT_RESET_CALL:
            fprintf(cg->out, "slab_restore(%s, (SlabCheckpoint){ .page = %s ? %s->head : NULL, .used = 0 });\n",
                    cg->current_ws_var, cg->current_ws_var, cg->current_ws_var);
            break;
        case STMT_FOR_RANGE:
            if (stmt->for_range.is_parallel) {
                size_t pid = stmt->for_range.parallel_id;
                CaptureCollector col = {0};
                col.loop_var = stmt->for_range.var_name;
                col.prog = cg->program;
                const char* locals[64];
                size_t local_count = 0;
                for (size_t i = 0; i < stmt->for_range.block->count; i++) {
                    collect_captured_stmt(&col, stmt->for_range.block->statements[i], locals, &local_count);
                }

                fprintf(cg->out, "{\n");
                cg->indent_level++;
                emit_indent(cg);
                if (col.count == 0) {
                    fprintf(cg->out, "_SlabParallelCtx_%zu _ctx_%zu = {0};\n", pid, pid);
                } else {
                    fprintf(cg->out, "_SlabParallelCtx_%zu _ctx_%zu = { ", pid, pid);
                    for (size_t v = 0; v < col.count; v++) {
                        if (v > 0) fprintf(cg->out, ", ");
                        fprintf(cg->out, ".%s = &%s", col.vars[v].name, col.vars[v].name);
                    }
                    fprintf(cg->out, " };\n");
                }
                emit_indent(cg);
                fprintf(cg->out, "slab_parallel_for((int64_t)(");
                emit_expr(cg, stmt->for_range.start_expr);
                fprintf(cg->out, "), (int64_t)(");
                emit_expr(cg, stmt->for_range.end_expr);
                fprintf(cg->out, "), ");
                if (stmt->for_range.step_expr) {
                    fprintf(cg->out, "(int64_t)(");
                    emit_expr(cg, stmt->for_range.step_expr);
                    fprintf(cg->out, ")");
                } else {
                    fprintf(cg->out, "1LL");
                }
                fprintf(cg->out, ", _slab_parallel_worker_%zu, &_ctx_%zu);\n", pid, pid);
                cg->indent_level--;
                emit_indent(cg);
                fprintf(cg->out, "}\n");
                break;
            }
            fprintf(cg->out, "SLAB_VECTORIZE\n");
            emit_indent(cg);
            fprintf(cg->out, "for (int64_t %s = (", stmt->for_range.var_name);
            emit_expr(cg, stmt->for_range.start_expr);
            fprintf(cg->out, "); %s < (", stmt->for_range.var_name);
            emit_expr(cg, stmt->for_range.end_expr);
            fprintf(cg->out, "); %s += ", stmt->for_range.var_name);
            if (stmt->for_range.step_expr) {
                emit_expr(cg, stmt->for_range.step_expr);
            } else {
                fprintf(cg->out, "1");
            }
            fprintf(cg->out, ") {\n");
            cg->indent_level++;
            emit_block(cg, stmt->for_range.block);
            cg->indent_level--;
            emit_indent(cg);
            fprintf(cg->out, "}\n");
            break;
        case STMT_FOR_IN: {
            // Check for zero-copy reverse iteration: for x in arc(arr):
            bool is_reverse = false;
            Expr* collection = stmt->for_in.iter_expr;
            if (collection && collection->kind == EXPR_ARC) {
                is_reverse = true;
                collection = collection->arc.operand;
            }

            fprintf(cg->out, "SLAB_VECTORIZE\n");
            emit_indent(cg);
            if (is_reverse) {
                fprintf(cg->out, "for (size_t _idx_%s = (", stmt->for_in.var_name);
                emit_expr(cg, collection);
                fprintf(cg->out, ").len; _idx_%s-- > 0; ) {\n", stmt->for_in.var_name);
            } else {
                fprintf(cg->out, "for (size_t _idx_%s = 0; _idx_%s < (", stmt->for_in.var_name, stmt->for_in.var_name);
                emit_expr(cg, collection);
                fprintf(cg->out, ").len; _idx_%s++) {\n", stmt->for_in.var_name);
            }

            cg->indent_level++;
            // Bind loop item if collection is List_SlabStr
            if (collection && collection->inferred_type && collection->inferred_type->kind == TYPE_KIND_LIST) {
                emit_indent(cg);
                fprintf(cg->out, "SlabStr %s = (", stmt->for_in.var_name);
                emit_expr(cg, collection);
                fprintf(cg->out, ").data[_idx_%s];\n", stmt->for_in.var_name);
            }
            emit_block(cg, stmt->for_in.block);
            cg->indent_level--;
            emit_indent(cg);
            fprintf(cg->out, "}\n");
            break;
        }
        case STMT_WHILE:
            fprintf(cg->out, "while (");
            emit_expr(cg, stmt->while_stmt.condition);
            fprintf(cg->out, ") {\n");
            cg->indent_level++;
            emit_block(cg, stmt->while_stmt.block);
            cg->indent_level--;
            emit_indent(cg);
            fprintf(cg->out, "}\n");
            break;
        case STMT_DO_UNTIL:
            fprintf(cg->out, "do {\n");
            cg->indent_level++;
            emit_block(cg, stmt->do_until.block);
            cg->indent_level--;
            emit_indent(cg);
            fprintf(cg->out, "} while (!(");
            emit_expr(cg, stmt->do_until.condition);
            fprintf(cg->out, "));\n");
            break;
        case STMT_IF:
            fprintf(cg->out, "if (");
            emit_expr(cg, stmt->if_stmt.condition);
            fprintf(cg->out, ") {\n");
            cg->indent_level++;
            emit_block(cg, stmt->if_stmt.then_block);
            cg->indent_level--;
            if (stmt->if_stmt.else_block) {
                emit_indent(cg);
                fprintf(cg->out, "} else {\n");
                cg->indent_level++;
                emit_block(cg, stmt->if_stmt.else_block);
                cg->indent_level--;
            }
            emit_indent(cg);
            fprintf(cg->out, "}\n");
            break;
        case STMT_SORT: {
            const char* rec_name = "Item";
            // Find record name from target variable
            for (size_t r = 0; r < cg->program->record_count; r++) {
                RecordDecl* rd = cg->program->records[r];
                for (size_t f = 0; f < rd->field_count; f++) {
                    if (strcmp(rd->fields[f].name, stmt->sort_stmt.field_ident) == 0) {
                        rec_name = rd->name;
                        break;
                    }
                }
            }
            fprintf(cg->out, "%s_SoA_sort_by_%s(&%s, %s, %s);\n",
                    rec_name,
                    stmt->sort_stmt.field_ident,
                    stmt->sort_stmt.target_ident,
                    stmt->sort_stmt.is_asc ? "true" : "false",
                    cg->current_ws_var);
            break;
        }
        case STMT_SAVE:
            if (stmt->save_stmt.expr->inferred_type && stmt->save_stmt.expr->inferred_type->kind == TYPE_KIND_STR) {
                fprintf(cg->out, "{ FILE* _sf = fopen(\"%s\", \"wb\"); if (_sf) { SlabStr _sv = ", stmt->save_stmt.path);
                emit_expr(cg, stmt->save_stmt.expr);
                fprintf(cg->out, "; if (_sv.ptr && _sv.len > 0) fwrite(_sv.ptr, 1, _sv.len, _sf); fclose(_sf); } }\n");
            } else {
                const char* rec_name = stmt->save_stmt.expr->inferred_type ? stmt->save_stmt.expr->inferred_type->custom_name : "Item";
                fprintf(cg->out, "%s_SoA_save(&", rec_name);
                emit_expr(cg, stmt->save_stmt.expr);
                fprintf(cg->out, ", \"%s\");\n", stmt->save_stmt.path);
            }
            break;
        case STMT_STATE:
            for (size_t i = 0; i < stmt->state_stmt.count; i++) {
                Expr* e = stmt->state_stmt.exprs[i];
                const char* name = (e->kind == EXPR_IDENT ? e->ident_name : "val");
                if (e->inferred_type && e->inferred_type->kind == TYPE_KIND_STR) {
                    fprintf(cg->out, "{ SlabStr _st = ");
                    emit_expr(cg, e);
                    fprintf(cg->out, "; printf(\"state [%%s] = %%.*s\\n\", \"%s\", (int)_st.len, _st.ptr ? _st.ptr : \"\"); }\n", name);
                } else if (e->inferred_type && (e->inferred_type->kind == TYPE_KIND_F32 || e->inferred_type->kind == TYPE_KIND_F64)) {
                    fprintf(cg->out, "printf(\"state [%%s] = %%g\\n\", \"%s\", (double)(", name);
                    emit_expr(cg, e);
                    fprintf(cg->out, "));\n");
                } else if (e->inferred_type && e->inferred_type->kind == TYPE_KIND_BOOL) {
                    fprintf(cg->out, "printf(\"state [%%s] = %%s\\n\", \"%s\", (", name);
                    emit_expr(cg, e);
                    fprintf(cg->out, ") ? \"true\" : \"false\");\n");
                } else {
                    fprintf(cg->out, "printf(\"state [%%s] = %%lld\\n\", \"%s\", (long long)(", name);
                    emit_expr(cg, e);
                    fprintf(cg->out, "));\n");
                }
                if (i + 1 < stmt->state_stmt.count) emit_indent(cg);
            }
            break;
        case STMT_RETURN:
            fprintf(cg->out, "return");
            if (stmt->return_stmt.expr) {
                fprintf(cg->out, " ");
                emit_expr(cg, stmt->return_stmt.expr);
            }
            fprintf(cg->out, ";\n");
            break;
        case STMT_ASSIGN:
            if (stmt->assign_stmt.target->kind == EXPR_IDENT) {
                TypeSpec* t = stmt->assign_stmt.target->inferred_type;
                if (!t && stmt->assign_stmt.value) {
                    t = stmt->assign_stmt.value->inferred_type;
                }
                if (stmt->assign_stmt.is_declaration) {
                    fprintf(cg->out, "%s %s = ", c_type_str(cg, t), stmt->assign_stmt.target->ident_name);
                } else {
                    fprintf(cg->out, "%s = ", stmt->assign_stmt.target->ident_name);
                }
                emit_expr(cg, stmt->assign_stmt.value);
                fprintf(cg->out, ";\n");
            } else if (stmt->assign_stmt.target->kind == EXPR_INDEX &&
                       is_packed_bool_field(cg, stmt->assign_stmt.target->index.target)) {
                // Packed 1-bit boolean assignment
                Expr* tgt_col = stmt->assign_stmt.target->index.target;
                Expr* idx = stmt->assign_stmt.target->index.index;
                fprintf(cg->out, "{ size_t _b_idx = (size_t)(");
                emit_expr(cg, idx);
                fprintf(cg->out, "); if (");
                emit_expr(cg, stmt->assign_stmt.value);
                fprintf(cg->out, ") { (");
                emit_expr(cg, tgt_col);
                fprintf(cg->out, ")[_b_idx >> 6] |= (1ULL << (_b_idx & 63)); } else { (");
                emit_expr(cg, tgt_col);
                fprintf(cg->out, ")[_b_idx >> 6] &= ~(1ULL << (_b_idx & 63)); } }\n");
            } else {
                emit_expr(cg, stmt->assign_stmt.target);
                fprintf(cg->out, " = ");
                emit_expr(cg, stmt->assign_stmt.value);
                fprintf(cg->out, ";\n");
            }
            break;
        case STMT_EXPR:
            if (stmt->expr_stmt.expr && stmt->expr_stmt.expr->kind == EXPR_ARC) {
                Expr* op = stmt->expr_stmt.expr->arc.operand;
                if (op && op->inferred_type && op->inferred_type->kind == TYPE_KIND_ARRAY) {
                    const char* rec_name = op->inferred_type->custom_name ? op->inferred_type->custom_name : "Item";
                    fprintf(cg->out, "%s_SoA_reverse_in_place(&", rec_name);
                    emit_expr(cg, op);
                    fprintf(cg->out, ");\n");
                    break;
                }
                if (op && op->inferred_type && op->inferred_type->kind == TYPE_KIND_STR) {
                    fprintf(cg->out, "slab_str_reverse_in_place(");
                    emit_expr(cg, op);
                    fprintf(cg->out, ");\n");
                    break;
                }
            }
            emit_expr(cg, stmt->expr_stmt.expr);
            fprintf(cg->out, ";\n");
            break;
    }
}

static void emit_block(Codegen* cg, Block* block) {
    if (!block) return;
    for (size_t i = 0; i < block->count; i++) {
        emit_stmt(cg, block->statements[i]);
    }
}

static void emit_parallel_worker_for_stmt(Codegen* cg, Stmt* stmt) {
    if (!stmt) return;
    if (stmt->kind == STMT_FOR_RANGE && stmt->for_range.is_parallel) {
        stmt->for_range.parallel_id = ++cg->parallel_counter;
        size_t pid = stmt->for_range.parallel_id;

        CaptureCollector col = {0};
        col.loop_var = stmt->for_range.var_name;
        col.prog = cg->program;
        const char* locals[64];
        size_t local_count = 0;
        for (size_t i = 0; i < stmt->for_range.block->count; i++) {
            collect_captured_stmt(&col, stmt->for_range.block->statements[i], locals, &local_count);
        }

        fprintf(cg->out, "typedef struct {\n");
        if (col.count == 0) {
            fprintf(cg->out, "    int _dummy;\n");
        } else {
            for (size_t v = 0; v < col.count; v++) {
                fprintf(cg->out, "    %s* %s;\n", c_type_str(cg, col.vars[v].type), col.vars[v].name);
            }
        }
        fprintf(cg->out, "} _SlabParallelCtx_%zu;\n\n", pid);

        fprintf(cg->out, "static void _slab_parallel_worker_%zu(int64_t %s, void* _arg) {\n", pid, stmt->for_range.var_name);
        fprintf(cg->out, "    _SlabParallelCtx_%zu* _ctx = (_SlabParallelCtx_%zu*)_arg;\n", pid, pid);
        if (col.count == 0) {
            fprintf(cg->out, "    (void)_ctx;\n");
        }
        for (size_t v = 0; v < col.count; v++) {
            fprintf(cg->out, "#define %s (*(_ctx->%s))\n", col.vars[v].name, col.vars[v].name);
        }
        cg->indent_level = 1;
        emit_block(cg, stmt->for_range.block);
        cg->indent_level = 0;
        for (size_t v = 0; v < col.count; v++) {
            fprintf(cg->out, "#undef %s\n", col.vars[v].name);
        }
        fprintf(cg->out, "}\n\n");
    }

    switch (stmt->kind) {
        case STMT_FOR_RANGE:
            if (stmt->for_range.block) {
                for (size_t i = 0; i < stmt->for_range.block->count; i++) {
                    emit_parallel_worker_for_stmt(cg, stmt->for_range.block->statements[i]);
                }
            }
            break;
        case STMT_FOR_IN:
            if (stmt->for_in.block) {
                for (size_t i = 0; i < stmt->for_in.block->count; i++) {
                    emit_parallel_worker_for_stmt(cg, stmt->for_in.block->statements[i]);
                }
            }
            break;
        case STMT_WHILE:
            if (stmt->while_stmt.block) {
                for (size_t i = 0; i < stmt->while_stmt.block->count; i++) {
                    emit_parallel_worker_for_stmt(cg, stmt->while_stmt.block->statements[i]);
                }
            }
            break;
        case STMT_DO_UNTIL:
            if (stmt->do_until.block) {
                for (size_t i = 0; i < stmt->do_until.block->count; i++) {
                    emit_parallel_worker_for_stmt(cg, stmt->do_until.block->statements[i]);
                }
            }
            break;
        case STMT_IF:
            if (stmt->if_stmt.then_block) {
                for (size_t i = 0; i < stmt->if_stmt.then_block->count; i++) {
                    emit_parallel_worker_for_stmt(cg, stmt->if_stmt.then_block->statements[i]);
                }
            }
            if (stmt->if_stmt.else_block) {
                for (size_t i = 0; i < stmt->if_stmt.else_block->count; i++) {
                    emit_parallel_worker_for_stmt(cg, stmt->if_stmt.else_block->statements[i]);
                }
            }
            break;
        case STMT_WORKSPACE:
            if (stmt->workspace.block) {
                for (size_t i = 0; i < stmt->workspace.block->count; i++) {
                    emit_parallel_worker_for_stmt(cg, stmt->workspace.block->statements[i]);
                }
            }
            break;
        case STMT_RESET_BLOCK:
            if (stmt->reset_block.block) {
                for (size_t i = 0; i < stmt->reset_block.block->count; i++) {
                    emit_parallel_worker_for_stmt(cg, stmt->reset_block.block->statements[i]);
                }
            }
            break;
        default:
            break;
    }
}

static void emit_parallel_workers(Codegen* cg, Program* prog) {
    cg->parallel_counter = 0;
    for (size_t f = 0; f < prog->fn_count; f++) {
        FnDecl* fn = prog->functions[f];
        if (fn->body) {
            for (size_t i = 0; i < fn->body->count; i++) {
                emit_parallel_worker_for_stmt(cg, fn->body->statements[i]);
            }
        }
    }
}

void codegen_init(Codegen* cg, SlabWorkspace* ws, Program* program, bool pack_enabled, FILE* out) {
    cg->ws = ws;
    cg->program = program;
    cg->pack_enabled = pack_enabled;
    cg->out = out;
    cg->indent_level = 0;
    cg->temp_var_counter = 0;
    cg->parallel_counter = 0;
    cg->current_ws_var = "&_root_ws";
}

bool codegen_generate(Codegen* cg) {
    // 1. Preamble
    fprintf(cg->out, "/* Generated by Slab Compiler */\n");
    fprintf(cg->out, "#include \"slab_runtime.h\"\n\n");

    // Imports
    for (size_t i = 0; i < cg->program->import_count; i++) {
        fprintf(cg->out, "#include \"%s\"\n", cg->program->imports[i]->path);
    }
    if (cg->program->import_count > 0) fprintf(cg->out, "\n");

    // 2. Record Definitions (SoA and AoS)
    for (size_t r = 0; r < cg->program->record_count; r++) {
        RecordDecl* rec = cg->program->records[r];

        // AoS Item Struct
        fprintf(cg->out, "typedef struct {\n");
        for (size_t f = 0; f < rec->field_count; f++) {
            fprintf(cg->out, "    %s %s;\n", c_type_str(cg, rec->fields[f].type), rec->fields[f].name);
        }
        fprintf(cg->out, "} %s;\n\n", rec->name);

        // AoS Table Struct & methods
        fprintf(cg->out, "typedef struct {\n");
        fprintf(cg->out, "    size_t len;\n");
        fprintf(cg->out, "    size_t cap;\n");
        fprintf(cg->out, "    SlabWorkspace* ws;\n");
        fprintf(cg->out, "    %s* data;\n", rec->name);
        fprintf(cg->out, "} %s_AoS;\n\n", rec->name);

        fprintf(cg->out, "static inline %s_AoS %s_AoS_create(SlabWorkspace* ws, size_t cap) {\n", rec->name, rec->name);
        fprintf(cg->out, "    %s_AoS tbl;\n", rec->name);
        fprintf(cg->out, "    tbl.len = 0;\n    tbl.cap = cap;\n    tbl.ws = ws;\n");
        fprintf(cg->out, "    tbl.data = (%s*)slab_alloc(ws, cap * sizeof(%s), 64);\n", rec->name, rec->name);
        fprintf(cg->out, "    return tbl;\n}\n\n");

        fprintf(cg->out, "static inline void %s_AoS_push(%s_AoS* tbl, %s item) {\n", rec->name, rec->name, rec->name);
        fprintf(cg->out, "    if (tbl->len >= tbl->cap) {\n");
        fprintf(cg->out, "        size_t new_cap = tbl->cap * 2;\n");
        fprintf(cg->out, "        tbl->data = (%s*)slab_realloc_buffer(tbl->ws, tbl->data, tbl->cap, new_cap, sizeof(%s), 64);\n", rec->name, rec->name);
        fprintf(cg->out, "        tbl->cap = new_cap;\n");
        fprintf(cg->out, "    }\n");
        fprintf(cg->out, "    tbl->data[tbl->len++] = item;\n}\n\n");

        // SoA Array Struct (with --pack dead field stripping & 1-bit boolean packing)
        fprintf(cg->out, "typedef struct {\n");
        fprintf(cg->out, "    size_t len;\n");
        fprintf(cg->out, "    size_t cap;\n");
        fprintf(cg->out, "    SlabWorkspace* ws;\n");
        for (size_t f = 0; f < rec->field_count; f++) {
            if (cg->pack_enabled && !rec->fields[f].is_read) {
                continue; // Dead-field stripped! 0 bytes in RAM!
            }
            if (cg->pack_enabled && rec->fields[f].type && rec->fields[f].type->kind == TYPE_KIND_BOOL) {
                fprintf(cg->out, "    uint64_t* __restrict__ %s;\n", rec->fields[f].name);
            } else {
                fprintf(cg->out, "    %s* __restrict__ %s;\n", c_type_str(cg, rec->fields[f].type), rec->fields[f].name);
            }
        }
        fprintf(cg->out, "} %s_SoA;\n\n", rec->name);

        fprintf(cg->out, "static inline %s_SoA %s_SoA_create(SlabWorkspace* ws, size_t cap) {\n", rec->name, rec->name);
        fprintf(cg->out, "    %s_SoA arr;\n", rec->name);
        fprintf(cg->out, "    arr.len = 0;\n    arr.cap = cap;\n    arr.ws = ws;\n");
        for (size_t f = 0; f < rec->field_count; f++) {
            if (cg->pack_enabled && !rec->fields[f].is_read) continue;
            if (cg->pack_enabled && rec->fields[f].type && rec->fields[f].type->kind == TYPE_KIND_BOOL) {
                fprintf(cg->out, "    arr.%s = (uint64_t*)slab_alloc(ws, ((cap + 63) / 64) * sizeof(uint64_t), 64);\n", rec->fields[f].name);
                fprintf(cg->out, "    memset(arr.%s, 0, ((cap + 63) / 64) * sizeof(uint64_t));\n", rec->fields[f].name);
            } else {
                fprintf(cg->out, "    arr.%s = (%s*)slab_alloc(ws, cap * sizeof(%s), 64);\n",
                        rec->fields[f].name, c_type_str(cg, rec->fields[f].type), c_type_str(cg, rec->fields[f].type));
            }
        }
        fprintf(cg->out, "    return arr;\n}\n\n");

        fprintf(cg->out, "static inline void %s_SoA_push(%s_SoA* arr", rec->name, rec->name);
        for (size_t f = 0; f < rec->field_count; f++) {
            if (cg->pack_enabled && !rec->fields[f].is_read) continue;
            fprintf(cg->out, ", %s %s", c_type_str(cg, rec->fields[f].type), rec->fields[f].name);
        }
        fprintf(cg->out, ") {\n");
        fprintf(cg->out, "    if (arr->len >= arr->cap) {\n");
        fprintf(cg->out, "        size_t new_cap = arr->cap * 2;\n");
        for (size_t f = 0; f < rec->field_count; f++) {
            if (cg->pack_enabled && !rec->fields[f].is_read) continue;
            if (cg->pack_enabled && rec->fields[f].type && rec->fields[f].type->kind == TYPE_KIND_BOOL) {
                fprintf(cg->out, "        arr->%s = (uint64_t*)slab_realloc_buffer(arr->ws, arr->%s, (arr->cap + 63) / 64, (new_cap + 63) / 64, sizeof(uint64_t), 64);\n",
                        rec->fields[f].name, rec->fields[f].name);
                fprintf(cg->out, "        memset(arr->%s + ((arr->cap + 63) / 64), 0, (((new_cap + 63) / 64) - ((arr->cap + 63) / 64)) * sizeof(uint64_t));\n", rec->fields[f].name);
            } else {
                fprintf(cg->out, "        arr->%s = (%s*)slab_realloc_buffer(arr->ws, arr->%s, arr->cap, new_cap, sizeof(%s), 64);\n",
                        rec->fields[f].name, c_type_str(cg, rec->fields[f].type), rec->fields[f].name, c_type_str(cg, rec->fields[f].type));
            }
        }
        fprintf(cg->out, "        arr->cap = new_cap;\n");
        fprintf(cg->out, "    }\n");
        for (size_t f = 0; f < rec->field_count; f++) {
            if (cg->pack_enabled && !rec->fields[f].is_read) continue;
            if (cg->pack_enabled && rec->fields[f].type && rec->fields[f].type->kind == TYPE_KIND_BOOL) {
                fprintf(cg->out, "    if (%s) arr->%s[arr->len >> 6] |= (1ULL << (arr->len & 63));\n", rec->fields[f].name, rec->fields[f].name);
                fprintf(cg->out, "    else arr->%s[arr->len >> 6] &= ~(1ULL << (arr->len & 63));\n", rec->fields[f].name);
            } else {
                fprintf(cg->out, "    arr->%s[arr->len] = %s;\n", rec->fields[f].name, rec->fields[f].name);
            }
        }
        fprintf(cg->out, "    arr->len++;\n}\n\n");

        // In-place reverse across all SoA columns (arc(arr))
        fprintf(cg->out, "static inline void %s_SoA_reverse_in_place(%s_SoA* arr) {\n", rec->name, rec->name);
        fprintf(cg->out, "    if (arr->len <= 1) return;\n");
        fprintf(cg->out, "    for (size_t _i = 0, _j = arr->len - 1; _i < _j; _i++, _j--) {\n");
        for (size_t f = 0; f < rec->field_count; f++) {
            if (cg->pack_enabled && !rec->fields[f].is_read) continue;
            if (cg->pack_enabled && rec->fields[f].type && rec->fields[f].type->kind == TYPE_KIND_BOOL) {
                fprintf(cg->out, "        bool _b_i = (arr->%s[_i >> 6] >> (_i & 63)) & 1ULL;\n", rec->fields[f].name);
                fprintf(cg->out, "        bool _b_j = (arr->%s[_j >> 6] >> (_j & 63)) & 1ULL;\n", rec->fields[f].name);
                fprintf(cg->out, "        if (_b_j) arr->%s[_i >> 6] |= (1ULL << (_i & 63)); else arr->%s[_i >> 6] &= ~(1ULL << (_i & 63));\n", rec->fields[f].name, rec->fields[f].name);
                fprintf(cg->out, "        if (_b_i) arr->%s[_j >> 6] |= (1ULL << (_j & 63)); else arr->%s[_j >> 6] &= ~(1ULL << (_j & 63));\n", rec->fields[f].name, rec->fields[f].name);
            } else {
                const char* tstr = c_type_str(cg, rec->fields[f].type);
                fprintf(cg->out, "        %s _tmp_%s = arr->%s[_i]; arr->%s[_i] = arr->%s[_j]; arr->%s[_j] = _tmp_%s;\n",
                        tstr, rec->fields[f].name, rec->fields[f].name, rec->fields[f].name, rec->fields[f].name, rec->fields[f].name, rec->fields[f].name);
            }
        }
        fprintf(cg->out, "    }\n}\n\n");

        // Permutation Index Sorting by each field
        for (size_t f = 0; f < rec->field_count; f++) {
            if (cg->pack_enabled && !rec->fields[f].is_read) continue;
            fprintf(cg->out, "static inline void %s_SoA_sort_by_%s(%s_SoA* arr, bool asc, SlabWorkspace* ws) {\n",
                    rec->name, rec->fields[f].name, rec->name);
            fprintf(cg->out, "    size_t n = arr->len;\n    if (n <= 1) return;\n");
            fprintf(cg->out, "    uint32_t* idx = (uint32_t*)slab_alloc(ws, n * sizeof(uint32_t), 4);\n");
            fprintf(cg->out, "    for (size_t i = 0; i < n; i++) idx[i] = (uint32_t)i;\n");
            fprintf(cg->out, "    for (size_t i = 1; i < n; i++) {\n");
            fprintf(cg->out, "        uint32_t key = idx[i];\n");
            fprintf(cg->out, "        int64_t j = (int64_t)i - 1;\n");
            fprintf(cg->out, "        while (j >= 0) {\n");
            fprintf(cg->out, "            bool cond = asc ? (arr->%s[idx[j]] > arr->%s[key]) : (arr->%s[idx[j]] < arr->%s[key]);\n",
                    rec->fields[f].name, rec->fields[f].name, rec->fields[f].name, rec->fields[f].name);
            fprintf(cg->out, "            if (!cond) break;\n");
            fprintf(cg->out, "            idx[j + 1] = idx[j];\n            j--;\n        }\n");
            fprintf(cg->out, "        idx[j + 1] = key;\n    }\n");
            // In-place cycle reordering of all active columns
            fprintf(cg->out, "    uint32_t* dest = (uint32_t*)slab_alloc(ws, n * sizeof(uint32_t), 4);\n");
            fprintf(cg->out, "    for (size_t i = 0; i < n; i++) dest[idx[i]] = (uint32_t)i;\n");
            fprintf(cg->out, "    for (size_t i = 0; i < n; i++) {\n");
            fprintf(cg->out, "        while (dest[i] != (uint32_t)i) {\n");
            fprintf(cg->out, "            uint32_t target = dest[i];\n");
            fprintf(cg->out, "            dest[i] = dest[target];\n");
            fprintf(cg->out, "            dest[target] = target;\n");
            for (size_t c = 0; c < rec->field_count; c++) {
                if (cg->pack_enabled && !rec->fields[c].is_read) continue;
                if (cg->pack_enabled && rec->fields[c].type && rec->fields[c].type->kind == TYPE_KIND_BOOL) {
                    fprintf(cg->out, "            bool _b_i = (arr->%s[i >> 6] >> (i & 63)) & 1ULL;\n", rec->fields[c].name);
                    fprintf(cg->out, "            bool _b_t = (arr->%s[target >> 6] >> (target & 63)) & 1ULL;\n", rec->fields[c].name);
                    fprintf(cg->out, "            if (_b_t) arr->%s[i >> 6] |= (1ULL << (i & 63)); else arr->%s[i >> 6] &= ~(1ULL << (i & 63));\n", rec->fields[c].name, rec->fields[c].name);
                    fprintf(cg->out, "            if (_b_i) arr->%s[target >> 6] |= (1ULL << (target & 63)); else arr->%s[target >> 6] &= ~(1ULL << (target & 63));\n", rec->fields[c].name, rec->fields[c].name);
                } else {
                    const char* ct = c_type_str(cg, rec->fields[c].type);
                    fprintf(cg->out, "            %s _tmp_%s = arr->%s[i]; arr->%s[i] = arr->%s[target]; arr->%s[target] = _tmp_%s;\n",
                            ct, rec->fields[c].name, rec->fields[c].name, rec->fields[c].name, rec->fields[c].name, rec->fields[c].name, rec->fields[c].name);
                }
            }
            fprintf(cg->out, "        }\n    }\n}\n\n");
        }

        // Structured File I/O for Record: Save & Load (CSV / BIN)
        fprintf(cg->out, "static inline void %s_SoA_save(const %s_SoA* arr, const char* path) {\n", rec->name, rec->name);
        fprintf(cg->out, "    bool is_bin = strstr(path, \".bin\") != NULL;\n");
        fprintf(cg->out, "    FILE* f = fopen(path, is_bin ? \"wb\" : \"w\");\n    if (!f) return;\n");
        fprintf(cg->out, "    if (is_bin) {\n");
        fprintf(cg->out, "        fwrite(&arr->len, sizeof(size_t), 1, f);\n");
        for (size_t f = 0; f < rec->field_count; f++) {
            if (cg->pack_enabled && !rec->fields[f].is_read) continue;
            if (cg->pack_enabled && rec->fields[f].type && rec->fields[f].type->kind == TYPE_KIND_BOOL) {
                fprintf(cg->out, "        fwrite(arr->%s, sizeof(uint64_t), (arr->len + 63) / 64, f);\n", rec->fields[f].name);
            } else {
                fprintf(cg->out, "        fwrite(arr->%s, sizeof(%s), arr->len, f);\n", rec->fields[f].name, c_type_str(cg, rec->fields[f].type));
            }
        }
        fprintf(cg->out, "    } else {\n");
        fprintf(cg->out, "        // CSV Header\n        fprintf(f, \"");
        bool first = true;
        for (size_t f = 0; f < rec->field_count; f++) {
            if (cg->pack_enabled && !rec->fields[f].is_read) continue;
            if (!first) fprintf(cg->out, ",");
            fprintf(cg->out, "%s", rec->fields[f].name);
            first = false;
        }
        fprintf(cg->out, "\\n\");\n");
        fprintf(cg->out, "        for (size_t i = 0; i < arr->len; i++) {\n");
        first = true;
        for (size_t f = 0; f < rec->field_count; f++) {
            if (cg->pack_enabled && !rec->fields[f].is_read) continue;
            if (!first) fprintf(cg->out, "            fprintf(f, \",\");\n");
            if (rec->fields[f].type && (rec->fields[f].type->kind == TYPE_KIND_F32 || rec->fields[f].type->kind == TYPE_KIND_F64)) {
                fprintf(cg->out, "            fprintf(f, \"%%g\", (double)arr->%s[i]);\n", rec->fields[f].name);
            } else if (rec->fields[f].type && rec->fields[f].type->kind == TYPE_KIND_BOOL) {
                if (cg->pack_enabled) {
                    fprintf(cg->out, "            fprintf(f, \"%%s\", ((arr->%s[i >> 6] >> (i & 63)) & 1ULL) ? \"true\" : \"false\");\n", rec->fields[f].name);
                } else {
                    fprintf(cg->out, "            fprintf(f, \"%%s\", arr->%s[i] ? \"true\" : \"false\");\n", rec->fields[f].name);
                }
            } else {
                fprintf(cg->out, "            fprintf(f, \"%%lld\", (long long)arr->%s[i]);\n", rec->fields[f].name);
            }
            first = false;
        }
        fprintf(cg->out, "            fprintf(f, \"\\n\");\n        }\n    }\n    fclose(f);\n}\n\n");

        fprintf(cg->out, "static inline %s_SoA %s_SoA_load(SlabWorkspace* ws, const char* path) {\n", rec->name, rec->name);
        fprintf(cg->out, "    bool is_bin = strstr(path, \".bin\") != NULL;\n");
        fprintf(cg->out, "    FILE* f = fopen(path, is_bin ? \"rb\" : \"r\");\n");
        fprintf(cg->out, "    if (!f) return %s_SoA_create(ws, 16);\n", rec->name);
        fprintf(cg->out, "    if (is_bin) {\n");
        fprintf(cg->out, "        size_t n = 0;\n        if (fread(&n, sizeof(size_t), 1, f) != 1) { fclose(f); return %s_SoA_create(ws, 16); }\n", rec->name);
        fprintf(cg->out, "        %s_SoA arr = %s_SoA_create(ws, n > 0 ? n : 16);\n        arr.len = n;\n", rec->name, rec->name);
        for (size_t f = 0; f < rec->field_count; f++) {
            if (cg->pack_enabled && !rec->fields[f].is_read) continue;
            if (cg->pack_enabled && rec->fields[f].type && rec->fields[f].type->kind == TYPE_KIND_BOOL) {
                fprintf(cg->out, "        fread(arr.%s, sizeof(uint64_t), (n + 63) / 64, f);\n", rec->fields[f].name);
            } else {
                fprintf(cg->out, "        fread(arr.%s, sizeof(%s), n, f);\n", rec->fields[f].name, c_type_str(cg, rec->fields[f].type));
            }
        }
        fprintf(cg->out, "        fclose(f);\n        return arr;\n    } else {\n");
        fprintf(cg->out, "        %s_SoA arr = %s_SoA_create(ws, 64);\n", rec->name, rec->name);
        fprintf(cg->out, "        char line_buf[1024];\n        if (!fgets(line_buf, sizeof(line_buf), f)) { fclose(f); return arr; }\n");
        fprintf(cg->out, "        while (fgets(line_buf, sizeof(line_buf), f)) {\n");
        fprintf(cg->out, "            char* cursor = line_buf;\n");
        for (size_t f = 0; f < rec->field_count; f++) {
            if (cg->pack_enabled && !rec->fields[f].is_read) continue;
            fprintf(cg->out, "            char* tok_%s = cursor; while (*cursor && *cursor != ',' && *cursor != '\\r' && *cursor != '\\n') cursor++; if (*cursor == ',') *cursor++ = '\\0';\n", rec->fields[f].name);
        }
        fprintf(cg->out, "            %s_SoA_push(&arr", rec->name);
        for (size_t f = 0; f < rec->field_count; f++) {
            if (cg->pack_enabled && !rec->fields[f].is_read) continue;
            if (rec->fields[f].type && (rec->fields[f].type->kind == TYPE_KIND_F32 || rec->fields[f].type->kind == TYPE_KIND_F64)) {
                fprintf(cg->out, ", atof(tok_%s)", rec->fields[f].name);
            } else if (rec->fields[f].type && rec->fields[f].type->kind == TYPE_KIND_BOOL) {
                fprintf(cg->out, ", (strcmp(tok_%s, \"true\") == 0 || atoi(tok_%s) != 0)", rec->fields[f].name, rec->fields[f].name);
            } else {
                fprintf(cg->out, ", atoll(tok_%s)", rec->fields[f].name);
            }
        }
        fprintf(cg->out, ");\n        }\n        fclose(f);\n        return arr;\n    }\n}\n\n");
    }

    // 3. Global Constants
    for (size_t i = 0; i < cg->program->const_count; i++) {
        ConstDecl* cd = cg->program->consts[i];
        fprintf(cg->out, "static const %s %s = ", c_type_str(cg, cd->value ? cd->value->inferred_type : NULL), cd->name);
        emit_expr(cg, cd->value);
        fprintf(cg->out, ";\n");
    }
    if (cg->program->const_count > 0) fprintf(cg->out, "\n");

    // 4. Function Prototypes
    for (size_t i = 0; i < cg->program->fn_count; i++) {
        FnDecl* fn = cg->program->functions[i];
        if (strcmp(fn->name, "main") == 0) {
            fprintf(cg->out, "int main(int argc, char** argv);\n");
        } else if (fn->is_extern) {
            fprintf(cg->out, "extern %s %s(", c_type_str(cg, fn->return_type), fn->name);
            for (size_t p = 0; p < fn->param_count; p++) {
                if (p > 0) fprintf(cg->out, ", ");
                fprintf(cg->out, "%s %s", c_type_str(cg, fn->params[p].type), fn->params[p].name);
            }
            fprintf(cg->out, ");\n");
        } else {
            fprintf(cg->out, "%s %s(", c_type_str(cg, fn->return_type), fn->name);
            for (size_t p = 0; p < fn->param_count; p++) {
                if (p > 0) fprintf(cg->out, ", ");
                fprintf(cg->out, "%s %s", c_type_str(cg, fn->params[p].type), fn->params[p].name);
            }
            fprintf(cg->out, ");\n");
        }
    }
    fprintf(cg->out, "\n");

    // 5. Multi-Core Parallel Workers
    emit_parallel_workers(cg, cg->program);

    // 6. Function Definitions
    for (size_t i = 0; i < cg->program->fn_count; i++) {
        FnDecl* fn = cg->program->functions[i];
        if (fn->is_extern || !fn->body) continue;
        bool is_main = (strcmp(fn->name, "main") == 0);

        if (is_main) {
            fprintf(cg->out, "int main(int argc, char** argv) {\n");
            cg->indent_level = 1;
            emit_indent(cg);
            fprintf(cg->out, "(void)argc; (void)argv;\n");
            emit_indent(cg);
            fprintf(cg->out, "SlabWorkspace _root_ws = {0};\n");
            emit_block(cg, fn->body);
            emit_indent(cg);
            fprintf(cg->out, "slab_workspace_destroy(&_root_ws);\n");
            emit_indent(cg);
            fprintf(cg->out, "return 0;\n");
            cg->indent_level = 0;
            fprintf(cg->out, "}\n\n");
        } else {
            fprintf(cg->out, "%s %s(", c_type_str(cg, fn->return_type), fn->name);
            for (size_t p = 0; p < fn->param_count; p++) {
                if (p > 0) fprintf(cg->out, ", ");
                fprintf(cg->out, "%s %s", c_type_str(cg, fn->params[p].type), fn->params[p].name);
            }
            fprintf(cg->out, ") {\n");
            cg->indent_level = 1;
            emit_block(cg, fn->body);
            cg->indent_level = 0;
            fprintf(cg->out, "}\n\n");
        }
    }

    return true;
}
