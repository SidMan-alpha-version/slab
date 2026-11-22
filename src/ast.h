#ifndef SLAB_AST_H
#define SLAB_AST_H

#include "slab_arena.h"
#include "token.h"
#include <stdbool.h>

typedef enum {
    TYPE_KIND_INT,
    TYPE_KIND_I32,
    TYPE_KIND_U16,
    TYPE_KIND_U32,
    TYPE_KIND_F32,
    TYPE_KIND_F64,
    TYPE_KIND_BOOL,
    TYPE_KIND_STR,
    TYPE_KIND_VOID,
    TYPE_KIND_ARRAY,
    TYPE_KIND_TABLE,
    TYPE_KIND_LIST,
    TYPE_KIND_CUSTOM
} TypeKind;

typedef struct TypeSpec TypeSpec;
typedef struct Expr Expr;
typedef struct Stmt Stmt;
typedef struct Block Block;

struct TypeSpec {
    TypeKind kind;
    const char* custom_name; // Record name for array<T>, table<T>, or custom record
    TypeSpec* elem_type;     // For list<T>
    Expr* capacity_expr;     // Optional capacity: array<T>[N]
    bool has_bounds;
    int64_t int_min;
    int64_t int_max;
};

typedef enum {
    EXPR_INT_LIT,
    EXPR_FLOAT_LIT,
    EXPR_STRING_LIT,
    EXPR_BOOL_LIT,
    EXPR_IDENT,
    EXPR_MATH_CONST,
    EXPR_BINARY,
    EXPR_UNARY,
    EXPR_CALL,
    EXPR_MEMBER,
    EXPR_INDEX,
    EXPR_SIGMA,
    EXPR_INTEGRAL,
    EXPR_ARC,
    EXPR_SEARCH,
    EXPR_LOAD,
    EXPR_READ_TEXT,
    EXPR_ETCH,
    EXPR_ARRAY_NEW,
    EXPR_TABLE_NEW,
    EXPR_LIST_NEW
} ExprKind;

#define EXPR_INTERN EXPR_ETCH

struct Expr {
    ExprKind kind;
    int line;
    int col;
    TypeSpec* inferred_type;

    union {
        int64_t int_val;
        double float_val;
        struct {
            const char* chars;
            size_t len;
        } str_val;
        bool bool_val;
        const char* ident_name;

        struct {
            TokenType op;
            Expr* left;
            Expr* right;
        } binary;

        struct {
            TokenType op;
            Expr* operand;
        } unary;

        struct {
            Expr* callee;
            Expr** args;
            size_t arg_count;
        } call;

        struct {
            Expr* target;
            const char* member_name;
        } member;

        struct {
            Expr* target;
            Expr* index;
        } index;

        struct {
            Expr* body;
            Expr* from_expr;
            Expr* limit_expr;
            Expr* step_expr;
        } sigma;

        struct {
            Expr* body;
            Expr* from_expr;
            Expr* limit_expr;
            Expr* step_expr;
        } integral;

        struct {
            Expr* operand;
        } arc;

        struct {
            const char* ident;
            Expr* condition;
            Expr* or_default;
        } search;

        struct {
            const char* record_name;
            const char* path;
        } load;

        struct {
            const char* path;
        } read_text;

        struct {
            const char* etch_str;
        } etch;
#define intern_str etch_str
#define intern etch

        struct {
            const char* record_name;
            TypeSpec* elem_type;
            Expr* capacity;
        } collection_new;
    };
};

typedef enum {
    STMT_WORKSPACE,
    STMT_RESET_BLOCK,
    STMT_RESET_CALL,
    STMT_FOR_RANGE,
    STMT_FOR_IN,
    STMT_WHILE,
    STMT_DO_UNTIL,
    STMT_IF,
    STMT_SORT,
    STMT_SAVE,
    STMT_STATE,
    STMT_RETURN,
    STMT_ASSIGN,
    STMT_EXPR
} StmtKind;

struct Stmt {
    StmtKind kind;
    int line;
    int col;

    union {
        struct {
            const char* name;
            Block* block;
        } workspace;

        struct {
            Block* block;
        } reset_block;

        struct {
            const char* var_name;
            Expr* start_expr;
            Expr* end_expr;
            Expr* step_expr;
            Block* block;
            bool is_parallel;
            size_t parallel_id;
        } for_range;

        struct {
            const char* var_name;
            Expr* iter_expr;
            Block* block;
        } for_in;

        struct {
            Expr* condition;
            Block* block;
        } while_stmt;

        struct {
            Block* block;
            Expr* condition;
        } do_until;

        struct {
            Expr* condition;
            Block* then_block;
            Block* else_block;
        } if_stmt;

        struct {
            const char* target_ident;
            const char* field_ident;
            bool is_asc;
        } sort_stmt;

        struct {
            Expr* expr;
            const char* path;
        } save_stmt;

        struct {
            Expr** exprs;
            size_t count;
        } state_stmt;

        struct {
            Expr* expr;
        } return_stmt;

        struct {
            Expr* target;
            Expr* value;
            bool is_declaration;
        } assign_stmt;

        struct {
            Expr* expr;
        } expr_stmt;
    };
};

struct Block {
    Stmt** statements;
    size_t count;
    size_t capacity;
};

typedef struct {
    const char* name;
    TypeSpec* type;
    bool is_read;
    int line;
    int col;
} RecordField;

typedef struct {
    const char* name;
    RecordField* fields;
    size_t field_count;
    int line;
    int col;
} RecordDecl;

typedef struct {
    const char* name;
    TypeSpec* type;
} FnParam;

typedef struct {
    const char* name;
    FnParam* params;
    size_t param_count;
    TypeSpec* return_type;
    Block* body;
    int line;
    int col;
    bool is_extern;
} FnDecl;

typedef struct {
    const char* name;
    Expr* value;
    int line;
    int col;
} ConstDecl;

typedef struct {
    const char* path;
    int line;
    int col;
} ImportStmt;

typedef struct {
    ImportStmt** imports;
    size_t import_count;
    ConstDecl** consts;
    size_t const_count;
    RecordDecl** records;
    size_t record_count;
    FnDecl** functions;
    size_t fn_count;
} Program;

// AST constructors
TypeSpec* ast_create_type(SlabWorkspace* ws, TypeKind kind);
Expr* ast_create_expr(SlabWorkspace* ws, ExprKind kind, int line, int col);
Stmt* ast_create_stmt(SlabWorkspace* ws, StmtKind kind, int line, int col);
Block* ast_create_block(SlabWorkspace* ws);
void ast_block_append(SlabWorkspace* ws, Block* block, Stmt* stmt);
Program* ast_create_program(SlabWorkspace* ws);

bool ast_type_is_reference(const TypeSpec* type);
const char* ast_type_to_string(const TypeSpec* type);

#endif // SLAB_AST_H
