#ifndef MASMRUN_H
#define MASMRUN_H

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MASMRUN_VERSION "0.1.0"

#define MASMRUN_EXIT_USAGE 64
#define MASMRUN_EXIT_UNSUPPORTED 1
#define MASMRUN_EXIT_INTERNAL 2

#define MEMORY_SIZE 0x01000000u
#define DATA_BASE   0x00010000u
#define STACK_TOP   0x00FFF000u
#define STEP_LIMIT  10000000u

typedef enum {
    REG_EAX,
    REG_ECX,
    REG_EDX,
    REG_EBX,
    REG_ESP,
    REG_EBP,
    REG_ESI,
    REG_EDI,
    REG_COUNT
} RegisterIndex;

typedef struct {
    RegisterIndex index;
    int width;
    int shift;
} RegisterRef;

typedef struct {
    uint32_t regs[REG_COUNT];
    bool zf;
    bool sf;
    bool cf;
    bool of;
    bool af;
    bool df;
    uint32_t eip;
    bool running;
    int exit_code;
    double fpu_st[8];
    int fpu_top;
    uint16_t fpu_status;
    uint16_t fpu_control;
} CPU;

typedef struct {
    bool trace;
    bool debug;
    bool dump_symbols;
    bool dump_data;
    const char *source_path;
} RunOptions;

typedef enum {
    SYM_DATA,
    SYM_CONST,
    SYM_CODE
} SymbolKind;

typedef struct {
    char *name;
    SymbolKind kind;
    uint32_t address;
    int64_t value;
    size_t instr_index;
    int elem_size;
    size_t elem_count;
    size_t byte_size;
    char *type_name;
} Symbol;

typedef struct {
    Symbol *items;
    size_t count;
    size_t cap;
} SymbolTable;

typedef struct {
    char *op;
    char *prefix;
    char *operands[4];
    int operand_count;
    int line;
    char *raw;
    int proc_index;
} Instruction;

typedef struct {
    Instruction *items;
    size_t count;
    size_t cap;
    char *entry;
} InstructionList;

typedef struct {
    char *name;
    int32_t offset;
    int width;
    size_t elem_count;
    bool is_param;
} LocalVar;

typedef struct {
    char *proc_name;
    LocalVar *items;
    size_t count;
    size_t cap;
    int32_t frame_bytes;
    int32_t param_bytes;
} ProcLocals;

typedef struct {
    ProcLocals *items;
    size_t count;
    size_t cap;
} ProcLocalTable;

typedef enum {
    CALLCONV_STDCALL,
    CALLCONV_IRVINE_REG
} CallingConvention;

typedef enum {
    PROTO_USER,
    PROTO_BUILTIN
} PrototypeTargetKind;

typedef struct {
    char *name;
    int width;
} PrototypeParam;

typedef struct {
    char *name;
    PrototypeParam *params;
    size_t param_count;
    size_t param_cap;
    CallingConvention calling_conv;
    PrototypeTargetKind target_kind;
} Prototype;

typedef struct {
    Prototype *items;
    size_t count;
    size_t cap;
} PrototypeTable;

typedef struct {
    char *name;
    uint32_t offset;
    int width;
    size_t count;
    char *type_ref;
    bool has_default;
    int64_t default_value;
} StructField;

typedef struct {
    char *name;
    StructField *fields;
    size_t field_count;
    size_t field_cap;
    size_t byte_size;
} StructDef;

typedef struct {
    StructDef *items;
    size_t count;
    size_t cap;
} StructTable;

typedef enum {
    TYPE_ALIAS_PTR
} TypeAliasKind;

typedef struct {
    char *name;
    TypeAliasKind kind;
    char *target;
    int width;
} TypeAlias;

typedef struct {
    TypeAlias *items;
    size_t count;
    size_t cap;
} TypeAliasTable;

typedef struct Program {
    SymbolTable symbols;
    InstructionList code;
    ProcLocalTable procs;
    PrototypeTable prototypes;
    StructTable structs;
    TypeAliasTable aliases;
    uint8_t *memory;
    uint32_t data_next;
    size_t last_data_index;
    bool has_last_data;
} Program;

typedef struct Runtime {
    CPU cpu;
    Program *program;
    const RunOptions *options;
    int current_proc_index;
} Runtime;

typedef enum {
    DIAG_ERROR,
    DIAG_WARNING,
    DIAG_NOTE
} DiagnosticSeverity;

typedef struct {
    DiagnosticSeverity severity;
    int line;
    int col;
    char *category;
    char *message;
    char *detail;
    char *hint;
} Diagnostic;

typedef struct {
    Diagnostic *items;
    size_t count;
    size_t cap;
} DiagnosticList;

typedef struct {
    void *ctx;
    void (*emit_instruction)(void *ctx, const char *op, const char *operands_csv, int line, const char *raw);
    void (*emit_label)(void *ctx, const char *label, int line);
    char *(*make_label)(void *ctx);
} HllEmitter;

void parse_program(Program *program, const char *path);
int run_program(Program *program, const RunOptions *options);
int check_file(const char *path);

void symbols_add(SymbolTable *table, Symbol symbol, int line);
Symbol *symbols_find(SymbolTable *table, const char *name);
LocalVar *locals_find(ProcLocals *locals, const char *name);
Prototype *prototypes_find(PrototypeTable *table, const char *name);
StructDef *structs_find(StructTable *table, const char *name);
TypeAlias *type_aliases_find(TypeAliasTable *table, const char *name);
void instructions_add(InstructionList *list, Instruction instruction);
uint32_t read_le(const Program *program, uint32_t address, int width, int line);
void write_le(Program *program, uint32_t address, int width, uint32_t value, int line);
void write_data(Program *program, int width, uint64_t value, int line);
void dump_symbols(const Program *program);
void dump_data(const Program *program);
void free_program(Program *program);

void source_set_path(const char *path);
void source_add_line(const char *line, size_t len);
void source_clear_lines(void);
const char *source_get_line(int line);
const char *source_get_path(void);
int source_default_column(int line);
int source_find_column(int line, const char *needle);

void diag_push_at(DiagnosticList *list, DiagnosticSeverity severity, int line, int col,
                  const char *category, const char *message, const char *detail,
                  const char *hint);
void diag_push(DiagnosticList *list, DiagnosticSeverity severity, int line,
               const char *category, const char *message, const char *detail,
               const char *hint);
void parse_error_continue(DiagnosticList *list, int line, int col, const char *category,
                          const char *message, const char *detail, const char *hint);
size_t diag_count_errors(const DiagnosticList *list);
void diag_print_all(FILE *stream, const char *path, const DiagnosticList *list);
void diag_free(DiagnosticList *list);
char *diag_suggest_near(const char *value, const char *const *candidates, size_t count);

void parse_error(int line, const char *message);
void parse_error2(int line, const char *message, const char *detail);
void runtime_error(int line, const char *message);
void runtime_error2(int line, const char *message, const char *detail);

void *xmalloc(size_t size);
void *xrealloc(void *ptr, size_t size);
char *xstrdup(const char *src);
char *xstrndup(const char *src, size_t len);
int ci_cmp(const char *a, const char *b);
bool ci_eq(const char *a, const char *b);
bool ci_starts_with(const char *s, const char *prefix);
char *trim_in_place(char *s);
bool is_ident_start(char c);
bool is_ident_char(char c);
char *strip_comment(const char *line);
char *next_token(char **cursor);
char *read_text_file(const char *path, size_t *size_out);
bool parse_integer_literal(const char *text, int64_t *value_out);
bool parse_char_literal(const char *text, int64_t *value_out);
bool parse_float_literal(const char *text, double *value_out);
void split_operands(const char *text, char **out, int *count_out, int max_count, int line);
int data_type_size(const char *type);
bool is_ignored_directive(const char *line);
bool is_size_prefix(const char *token, int *width_out);

typedef struct {
    const char *name;
    CallingConvention conv;
    size_t argc;
    const int *param_widths;
} BuiltinSignature;

const BuiltinSignature *builtin_signatures(size_t *count_out);
const BuiltinSignature *builtin_signature_find(const char *name);

bool parse_register(const char *name, RegisterRef *ref);
uint32_t reg_get(const Runtime *runtime, RegisterRef ref);
void reg_set(Runtime *runtime, RegisterRef ref, uint32_t value);
void set_sub_flags(Runtime *runtime, uint32_t a, uint32_t b, uint32_t result, int width);
bool run_builtin(Runtime *runtime, const char *name, int invoke_argc, int line);
int64_t eval_expr(Runtime *runtime, Program *program, const char *expr, int line);
bool color_constant_value(const char *name, int64_t *value_out);
bool debug_pre_exec(Runtime *runtime, uint32_t eip, Instruction *ins);

char *macro_expand_source(const char *path, const char *text, DiagnosticList *findings);

void hll_lower_jump_if_false(HllEmitter *emitter, const char *expr,
                             const char *target_label, int line);
void hll_lower_jump_if_true(HllEmitter *emitter, const char *expr,
                            const char *target_label, int line);

#endif
