#include "masmrun.h"
#include <math.h>

bool parse_register(const char *name, RegisterRef *ref) {
    struct {
        const char *name;
        RegisterIndex index;
        int width;
        int shift;
    } regs[] = {
        {"eax", REG_EAX, 4, 0}, {"ecx", REG_ECX, 4, 0}, {"edx", REG_EDX, 4, 0}, {"ebx", REG_EBX, 4, 0},
        {"esp", REG_ESP, 4, 0}, {"ebp", REG_EBP, 4, 0}, {"esi", REG_ESI, 4, 0}, {"edi", REG_EDI, 4, 0},
        {"ax", REG_EAX, 2, 0}, {"cx", REG_ECX, 2, 0}, {"dx", REG_EDX, 2, 0}, {"bx", REG_EBX, 2, 0},
        {"sp", REG_ESP, 2, 0}, {"bp", REG_EBP, 2, 0}, {"si", REG_ESI, 2, 0}, {"di", REG_EDI, 2, 0},
        {"al", REG_EAX, 1, 0}, {"cl", REG_ECX, 1, 0}, {"dl", REG_EDX, 1, 0}, {"bl", REG_EBX, 1, 0},
        {"ah", REG_EAX, 1, 8}, {"ch", REG_ECX, 1, 8}, {"dh", REG_EDX, 1, 8}, {"bh", REG_EBX, 1, 8},
    };
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        if (ci_eq(name, regs[i].name)) {
            ref->index = regs[i].index;
            ref->width = regs[i].width;
            ref->shift = regs[i].shift;
            return true;
        }
    }
    return false;
}

static uint32_t width_mask(int width) {
    switch (width) {
        case 1: return 0xffu;
        case 2: return 0xffffu;
        case 4: return 0xffffffffu;
        default: return 0xffffffffu;
    }
}

static uint32_t sign_bit_for_width(int width) {
    return width == 1 ? 0x80u : width == 2 ? 0x8000u : 0x80000000u;
}

static int32_t sign_extend_value(uint32_t value, int width) {
    if (width == 1) {
        return (int32_t)(int8_t)(value & 0xffu);
    }
    if (width == 2) {
        return (int32_t)(int16_t)(value & 0xffffu);
    }
    return (int32_t)value;
}

uint32_t reg_get(const Runtime *runtime, RegisterRef ref) {
    return (runtime->cpu.regs[ref.index] >> ref.shift) & width_mask(ref.width);
}

void reg_set(Runtime *runtime, RegisterRef ref, uint32_t value) {
    uint32_t mask = width_mask(ref.width) << ref.shift;
    runtime->cpu.regs[ref.index] = (runtime->cpu.regs[ref.index] & ~mask) | ((value << ref.shift) & mask);
}

static char *remove_outer_brackets(char *s) {
    s = trim_in_place(s);
    size_t len = strlen(s);
    if (len >= 2 && s[0] == '[' && s[len - 1] == ']') {
        s[len - 1] = '\0';
        return trim_in_place(s + 1);
    }
    return s;
}

typedef enum {
    OPERAND_IMM,
    OPERAND_REG,
    OPERAND_MEM
} OperandKind;

typedef struct {
    OperandKind kind;
    int width;
    uint32_t value;
    uint32_t address;
    RegisterRef reg;
} Operand;

static char *strip_size_prefix(char *s, int *width, bool *explicit_width) {
    s = trim_in_place(s);
    char *first_start = s;
    while (*s && !isspace((unsigned char)*s)) {
        s++;
    }
    if (s == first_start) {
        return s;
    }
    char saved = *s;
    *s = '\0';
    int parsed_width = 0;
    bool matched = is_size_prefix(first_start, &parsed_width);
    *s = saved;
    if (!matched) {
        return first_start;
    }

    *width = parsed_width;
    *explicit_width = true;
    char *rest = trim_in_place(s);
    char *ptr_start = rest;
    while (*rest && !isspace((unsigned char)*rest)) {
        rest++;
    }
    if (rest > ptr_start) {
        char ptr_saved = *rest;
        *rest = '\0';
        bool is_ptr = ci_eq(ptr_start, "PTR");
        *rest = ptr_saved;
        if (is_ptr) {
            return trim_in_place(rest);
        }
    }
    return ptr_start;
}

static LocalVar *runtime_local_find(Runtime *runtime, const char *name) {
    if (!runtime || runtime->current_proc_index < 0) {
        return NULL;
    }
    Program *program = runtime->program;
    if ((size_t)runtime->current_proc_index >= program->procs.count) {
        return NULL;
    }
    return locals_find(&program->procs.items[runtime->current_proc_index], name);
}

typedef enum {
    SIZE_QUERY_TYPE,
    SIZE_QUERY_LENGTHOF,
    SIZE_QUERY_SIZEOF
} SizeQueryKind;

static bool split_field_reference(const char *text, char **base_out, char **field_out) {
    if (!text) {
        return false;
    }
    int bracket_depth = 0;
    char quote = '\0';
    const char *dot = NULL;
    for (const char *p = text; *p; p++) {
        if (quote) {
            if (*p == '\\' && p[1]) {
                p++;
            } else if (*p == quote) {
                quote = '\0';
            }
            continue;
        }
        if (*p == '\'' || *p == '"') {
            quote = *p;
        } else if (*p == '[') {
            bracket_depth++;
        } else if (*p == ']') {
            if (bracket_depth > 0) {
                bracket_depth--;
            }
        } else if (*p == '.' && bracket_depth == 0) {
            dot = p;
        }
    }
    if (!dot) {
        return false;
    }
    char *base_raw = xstrndup(text, (size_t)(dot - text));
    char *field_raw = xstrdup(dot + 1);
    char *base = trim_in_place(base_raw);
    char *field = trim_in_place(field_raw);
    if (*base == '\0' || *field == '\0') {
        free(base_raw);
        free(field_raw);
        return false;
    }
    *base_out = xstrdup(base);
    *field_out = xstrdup(field);
    free(base_raw);
    free(field_raw);
    return true;
}

static StructDef *symbol_struct_def(Program *program, const Symbol *symbol) {
    if (!program || !symbol || symbol->kind != SYM_DATA || !symbol->type_name) {
        return NULL;
    }
    return structs_find(&program->structs, symbol->type_name);
}

static const StructField *struct_find_field(const StructDef *def, const char *field_name) {
    if (!def || !field_name) {
        return NULL;
    }
    for (size_t i = 0; i < def->field_count; i++) {
        if (ci_eq(def->fields[i].name, field_name)) {
            return &def->fields[i];
        }
    }
    return NULL;
}

static const StructField *find_unique_field(Program *program, const char *field_name, size_t *matches_out) {
    const StructField *match = NULL;
    size_t matches = 0;
    for (size_t i = 0; i < program->structs.count; i++) {
        const StructField *candidate = struct_find_field(&program->structs.items[i], field_name);
        if (candidate) {
            matches++;
            if (!match) {
                match = candidate;
            }
        }
    }
    if (matches_out) {
        *matches_out = matches;
    }
    return match;
}

static StructDef *infer_struct_from_field_base(Runtime *runtime, const char *base_text) {
    Program *program = runtime->program;
    char *copy = xstrdup(base_text);
    char *trimmed = trim_in_place(copy);
    if (*trimmed == '[') {
        free(copy);
        return NULL;
    }

    char *name_end = trimmed;
    while (is_ident_char(*name_end)) {
        name_end++;
    }
    char saved = *name_end;
    *name_end = '\0';

    StructDef *def = NULL;
    if (*trimmed) {
        Symbol *symbol = symbols_find(&program->symbols, trimmed);
        if (symbol && symbol->kind == SYM_DATA) {
            def = symbol_struct_def(program, symbol);
        }
    }

    *name_end = saved;
    free(copy);
    return def;
}

static const StructField *resolve_field_for_access(Runtime *runtime, const StructDef *preferred,
                                                   const char *field_name, int line) {
    Program *program = runtime->program;
    if (preferred) {
        const StructField *field = struct_find_field(preferred, field_name);
        if (!field) {
            runtime_error2(line, "unknown struct field", field_name);
        }
        return field;
    }

    size_t matches = 0;
    const StructField *field = find_unique_field(program, field_name, &matches);
    if (matches == 0) {
        runtime_error2(line, "unknown struct field", field_name);
    }
    if (matches > 1) {
        runtime_error2(line, "ambiguous struct field for indirect access", field_name);
    }
    return field;
}

static uint32_t evaluate_size_query(Runtime *runtime, SizeQueryKind kind,
                                    const char *name_text, int line) {
    Program *program = runtime->program;
    char *base = NULL;
    char *field_name = NULL;
    bool field_ref = split_field_reference(name_text, &base, &field_name);
    char *name_buf = field_ref ? base : xstrdup(name_text);
    char *name = trim_in_place(name_buf);

    LocalVar *local = runtime_local_find(runtime, name);
    Symbol *symbol = symbols_find(&program->symbols, name);
    TypeAlias *alias = type_aliases_find(&program->aliases, name);
    StructDef *struct_type = structs_find(&program->structs, name);

    uint32_t value = 0;
    if (!field_ref) {
        if (local) {
            if (kind == SIZE_QUERY_TYPE) {
                value = (uint32_t)local->width;
            } else if (kind == SIZE_QUERY_LENGTHOF) {
                value = (uint32_t)local->elem_count;
            } else {
                value = (uint32_t)((size_t)local->width * local->elem_count);
            }
        } else if (symbol && symbol->kind == SYM_DATA) {
            if (kind == SIZE_QUERY_TYPE) {
                value = (uint32_t)symbol->elem_size;
            } else if (kind == SIZE_QUERY_LENGTHOF) {
                value = (uint32_t)symbol->elem_count;
            } else {
                value = (uint32_t)symbol->byte_size;
            }
        } else if (alias) {
            if (kind == SIZE_QUERY_LENGTHOF) {
                value = 1;
            } else {
                value = (uint32_t)alias->width;
            }
        } else if (struct_type) {
            if (kind == SIZE_QUERY_LENGTHOF) {
                value = 1;
            } else {
                value = (uint32_t)struct_type->byte_size;
            }
        } else {
            if (kind == SIZE_QUERY_TYPE) {
                runtime_error2(line, "TYPE requires a data symbol, alias, or struct type", name);
            } else if (kind == SIZE_QUERY_LENGTHOF) {
                runtime_error2(line, "LENGTHOF requires a data symbol, alias, or struct type", name);
            } else {
                runtime_error2(line, "SIZEOF requires a data symbol, alias, or struct type", name);
            }
        }
    } else {
        const StructDef *owner = NULL;
        if (symbol && symbol->kind == SYM_DATA) {
            owner = symbol_struct_def(program, symbol);
        } else if (struct_type) {
            owner = struct_type;
        }
        const StructField *field = resolve_field_for_access(runtime, owner, field_name, line);
        if (kind == SIZE_QUERY_TYPE) {
            value = (uint32_t)field->width;
        } else if (kind == SIZE_QUERY_LENGTHOF) {
            value = (uint32_t)field->count;
        } else {
            value = (uint32_t)((size_t)field->width * field->count);
        }
    }

    free(base);
    free(field_name);
    if (!field_ref) {
        free(name_buf);
    }
    return value;
}

static Operand parse_operand(Runtime *runtime, const char *text, int default_width, int line) {
    Program *program = runtime->program;
    char *copy = xstrdup(text);
    char *s = trim_in_place(copy);
    int width = default_width;
    bool explicit_width = false;

    s = strip_size_prefix(s, &width, &explicit_width);
    if (width == 0) {
        width = 4;
    }

    if (ci_starts_with(s, "OFFSET ")) {
        Operand op;
        memset(&op, 0, sizeof(op));
        op.kind = OPERAND_IMM;
        op.width = 4;
        op.value = (uint32_t)eval_expr(runtime, program, s + 7, line);
        free(copy);
        return op;
    }
    if (ci_starts_with(s, "TYPE ")) {
        char *name = trim_in_place(s + 5);
        Operand op = {OPERAND_IMM, 4, evaluate_size_query(runtime, SIZE_QUERY_TYPE, name, line), 0, {0, 0, 0}};
        free(copy);
        return op;
    }
    if (ci_starts_with(s, "LENGTHOF ")) {
        char *name = trim_in_place(s + 9);
        Operand op = {OPERAND_IMM, 4, evaluate_size_query(runtime, SIZE_QUERY_LENGTHOF, name, line), 0, {0, 0, 0}};
        free(copy);
        return op;
    }
    if (ci_starts_with(s, "SIZEOF ")) {
        char *name = trim_in_place(s + 7);
        Operand op = {OPERAND_IMM, 4, evaluate_size_query(runtime, SIZE_QUERY_SIZEOF, name, line), 0, {0, 0, 0}};
        free(copy);
        return op;
    }

    char *field_base = NULL;
    char *field_name = NULL;
    if (split_field_reference(s, &field_base, &field_name)) {
        const StructDef *owner = infer_struct_from_field_base(runtime, field_base);
        const StructField *field = resolve_field_for_access(runtime, owner, field_name, line);
        Operand base = parse_operand(runtime, field_base, 4, line);
        if (base.kind != OPERAND_MEM) {
            runtime_error2(line, "dot field access requires a memory base", text);
        }
        Operand op;
        memset(&op, 0, sizeof(op));
        op.kind = OPERAND_MEM;
        op.address = base.address + field->offset;
        op.width = (default_width || explicit_width) ? width : field->width;
        free(field_base);
        free(field_name);
        free(copy);
        return op;
    }

    LocalVar *local = runtime_local_find(runtime, s);
    if (local) {
        Operand op;
        memset(&op, 0, sizeof(op));
        op.kind = OPERAND_MEM;
        op.width = (default_width || explicit_width) ? width : local->width;
        op.address = runtime->cpu.regs[REG_EBP] + (uint32_t)local->offset;
        free(copy);
        return op;
    }

    RegisterRef reg;
    if (parse_register(s, &reg)) {
        Operand op;
        memset(&op, 0, sizeof(op));
        op.kind = OPERAND_REG;
        op.width = reg.width;
        op.reg = reg;
        free(copy);
        return op;
    }

    size_t len = strlen(s);
    if (len >= 2 && s[0] == '[' && s[len - 1] == ']') {
        char *inner = remove_outer_brackets(s);
        Operand op;
        memset(&op, 0, sizeof(op));
        op.kind = OPERAND_MEM;
        op.width = width;
        op.address = (uint32_t)eval_expr(runtime, program, inner, line);
        free(copy);
        return op;
    }

    char *bracket = strchr(s, '[');
    if (bracket && bracket != s) {
        char *end = strrchr(bracket, ']');
        if (!end || end[1] != '\0') {
            runtime_error2(line, "invalid indexed memory operand", text);
        }
        *bracket = '\0';
        *end = '\0';
        char *label = trim_in_place(s);
        char *inside = trim_in_place(bracket + 1);
        LocalVar *base_local = runtime_local_find(runtime, label);
        if (base_local) {
            int indexed_width = (default_width || explicit_width) ? width : base_local->width;
            Operand op;
            memset(&op, 0, sizeof(op));
            op.kind = OPERAND_MEM;
            op.width = indexed_width;
            op.address = runtime->cpu.regs[REG_EBP] + (uint32_t)base_local->offset +
                         (uint32_t)eval_expr(runtime, program, inside, line);
            free(copy);
            return op;
        }
        Symbol *base_symbol = symbols_find(&program->symbols, label);
        int indexed_width = width;
        if (base_symbol && base_symbol->kind == SYM_DATA && !default_width && !explicit_width) {
            indexed_width = base_symbol->elem_size;
        }
        size_t expr_len = strlen(label) + strlen(inside) + 4;
        char *indexed_expr = xmalloc(expr_len);
        snprintf(indexed_expr, expr_len, "%s+%s", label, inside);
        Operand op;
        memset(&op, 0, sizeof(op));
        op.kind = OPERAND_MEM;
        op.width = indexed_width;
        op.address = (uint32_t)eval_expr(runtime, program, indexed_expr, line);
        free(indexed_expr);
        free(copy);
        return op;
    }

    Symbol *symbol = symbols_find(&program->symbols, s);
    if (symbol && symbol->kind == SYM_DATA) {
        Operand op;
        memset(&op, 0, sizeof(op));
        op.kind = OPERAND_MEM;
        op.width = (default_width || explicit_width) ? width : symbol->elem_size;
        op.address = symbol->address;
        free(copy);
        return op;
    }

    Operand op;
    memset(&op, 0, sizeof(op));
    op.kind = OPERAND_IMM;
    op.width = width;
    op.value = (uint32_t)eval_expr(runtime, program, s, line);
    free(copy);
    return op;
}

static uint32_t operand_read(Runtime *runtime, Operand op, int line) {
    switch (op.kind) {
        case OPERAND_IMM:
            return op.value & width_mask(op.width);
        case OPERAND_REG:
            return reg_get(runtime, op.reg);
        case OPERAND_MEM:
            return read_le(runtime->program, op.address, op.width, line);
    }
    return 0;
}

static void operand_write(Runtime *runtime, Operand op, uint32_t value, int line) {
    switch (op.kind) {
        case OPERAND_IMM:
            runtime_error(line, "cannot write to an immediate operand");
            break;
        case OPERAND_REG:
            reg_set(runtime, op.reg, value);
            break;
        case OPERAND_MEM:
            write_le(runtime->program, op.address, op.width, value, line);
            break;
    }
}

static void set_logic_flags(Runtime *runtime, uint32_t result, int width) {
    uint32_t mask = width_mask(width);
    uint32_t value = result & mask;
    runtime->cpu.zf = value == 0;
    runtime->cpu.sf = (value & sign_bit_for_width(width)) != 0;
    runtime->cpu.cf = false;
    runtime->cpu.of = false;
    runtime->cpu.af = false;
}

static void set_add_flags_with_carry(Runtime *runtime, uint32_t a, uint32_t b, uint32_t carry, uint32_t result, int width) {
    uint64_t mask = width_mask(width);
    uint64_t full = (uint64_t)(a & (uint32_t)mask) + (uint64_t)(b & (uint32_t)mask) + (uint64_t)carry;
    uint32_t value = result & (uint32_t)mask;
    uint32_t rhs = (uint32_t)(((uint64_t)(b & (uint32_t)mask) + (uint64_t)carry) & mask);
    uint32_t sign = sign_bit_for_width(width);
    runtime->cpu.zf = value == 0;
    runtime->cpu.sf = (value & sign) != 0;
    runtime->cpu.cf = full > mask;
    runtime->cpu.of = ((~(a ^ rhs) & (a ^ value) & sign) != 0);
    runtime->cpu.af = (((a ^ rhs ^ value) & 0x10u) != 0);
}

static void set_add_flags(Runtime *runtime, uint32_t a, uint32_t b, uint32_t result, int width) {
    set_add_flags_with_carry(runtime, a, b, 0, result, width);
}

void set_sub_flags(Runtime *runtime, uint32_t a, uint32_t b, uint32_t result, int width) {
    uint32_t mask = width_mask(width);
    uint32_t value = result & mask;
    uint32_t sign = sign_bit_for_width(width);
    runtime->cpu.zf = value == 0;
    runtime->cpu.sf = (value & sign) != 0;
    runtime->cpu.cf = (a & mask) < (b & mask);
    runtime->cpu.of = (((a ^ b) & (a ^ value) & sign) != 0);
    runtime->cpu.af = (((a ^ b ^ value) & 0x10u) != 0);
}

static void set_sub_flags_with_borrow(Runtime *rt, uint32_t a, uint32_t b,
                                       uint32_t borrow, uint32_t result, int width) {
    uint64_t mask = width_mask(width);
    uint64_t full_b = (uint64_t)(b & mask) + (uint64_t)borrow;
    uint32_t value = result & (uint32_t)mask;
    uint32_t sign = sign_bit_for_width(width);
    rt->cpu.zf = value == 0;
    rt->cpu.sf = (value & sign) != 0;
    rt->cpu.cf = (uint64_t)(a & mask) < full_b;
    rt->cpu.of = (((a ^ b) & (a ^ value) & sign) != 0);
    rt->cpu.af = (((a ^ b ^ value) & 0x10u) != 0);
}

static void push32(Runtime *runtime, uint32_t value, int line) {
    uint32_t esp = runtime->cpu.regs[REG_ESP];
    if (esp < 4) {
        runtime_error(line, "stack overflow");
    }
    esp -= 4;
    runtime->cpu.regs[REG_ESP] = esp;
    write_le(runtime->program, esp, 4, value, line);
}

static uint32_t pop32(Runtime *runtime, int line) {
    uint32_t esp = runtime->cpu.regs[REG_ESP];
    if (esp > STACK_TOP - 4) {
        runtime_error(line, "stack underflow");
    }
    uint32_t value = read_le(runtime->program, esp, 4, line);
    runtime->cpu.regs[REG_ESP] = esp + 4;
    return value;
}

static const RegisterIndex pushad_order[8] = {
    REG_EAX, REG_ECX, REG_EDX, REG_EBX, REG_ESP, REG_EBP, REG_ESI, REG_EDI
};

static uint32_t pack_eflags(const CPU *cpu) {
    uint32_t value = 0x00000002u;
    if (cpu->cf) value |= 1u << 0;
    if (cpu->af) value |= 1u << 4;
    if (cpu->zf) value |= 1u << 6;
    if (cpu->sf) value |= 1u << 7;
    if (cpu->df) value |= 1u << 10;
    if (cpu->of) value |= 1u << 11;
    return value;
}

static void unpack_eflags(CPU *cpu, uint32_t value) {
    cpu->cf = (value & (1u << 0)) != 0;
    cpu->af = (value & (1u << 4)) != 0;
    cpu->zf = (value & (1u << 6)) != 0;
    cpu->sf = (value & (1u << 7)) != 0;
    cpu->df = (value & (1u << 10)) != 0;
    cpu->of = (value & (1u << 11)) != 0;
}

static bool is_string_op(const char *op) {
    return ci_eq(op, "movsb") || ci_eq(op, "stosb") ||
           ci_eq(op, "lodsb") || ci_eq(op, "scasb") ||
           ci_eq(op, "cmpsb") || ci_eq(op, "movsd") ||
           ci_eq(op, "stosd") || ci_eq(op, "lodsd");
}

static void exec_string_step(Runtime *runtime, const char *op, int line) {
    uint32_t step = runtime->cpu.df ? UINT32_MAX : 1u;
    Program *program = runtime->program;
    if (ci_eq(op, "movsb")) {
        uint32_t b = read_le(program, runtime->cpu.regs[REG_ESI], 1, line);
        write_le(program, runtime->cpu.regs[REG_EDI], 1, b, line);
        runtime->cpu.regs[REG_ESI] += step;
        runtime->cpu.regs[REG_EDI] += step;
    } else if (ci_eq(op, "stosb")) {
        uint32_t al = runtime->cpu.regs[REG_EAX] & 0xffu;
        write_le(program, runtime->cpu.regs[REG_EDI], 1, al, line);
        runtime->cpu.regs[REG_EDI] += step;
    } else if (ci_eq(op, "lodsb")) {
        uint32_t b = read_le(program, runtime->cpu.regs[REG_ESI], 1, line);
        runtime->cpu.regs[REG_EAX] = (runtime->cpu.regs[REG_EAX] & 0xffffff00u) | (b & 0xffu);
        runtime->cpu.regs[REG_ESI] += step;
    } else if (ci_eq(op, "scasb")) {
        uint32_t al = runtime->cpu.regs[REG_EAX] & 0xffu;
        uint32_t mem = read_le(program, runtime->cpu.regs[REG_EDI], 1, line);
        set_sub_flags(runtime, al, mem, al - mem, 1);
        runtime->cpu.regs[REG_EDI] += step;
    } else if (ci_eq(op, "cmpsb")) {
        uint32_t src = read_le(program, runtime->cpu.regs[REG_ESI], 1, line);
        uint32_t dst = read_le(program, runtime->cpu.regs[REG_EDI], 1, line);
        set_sub_flags(runtime, src, dst, src - dst, 1);
        runtime->cpu.regs[REG_ESI] += step;
        runtime->cpu.regs[REG_EDI] += step;
    } else if (ci_eq(op, "movsd")) {
        uint32_t step4 = runtime->cpu.df ? 0xFFFFFFFCu : 4u;
        uint32_t v = read_le(program, runtime->cpu.regs[REG_ESI], 4, line);
        write_le(program, runtime->cpu.regs[REG_EDI], 4, v, line);
        runtime->cpu.regs[REG_ESI] += step4;
        runtime->cpu.regs[REG_EDI] += step4;
    } else if (ci_eq(op, "stosd")) {
        uint32_t step4 = runtime->cpu.df ? 0xFFFFFFFCu : 4u;
        write_le(program, runtime->cpu.regs[REG_EDI], 4, runtime->cpu.regs[REG_EAX], line);
        runtime->cpu.regs[REG_EDI] += step4;
    } else if (ci_eq(op, "lodsd")) {
        uint32_t step4 = runtime->cpu.df ? 0xFFFFFFFCu : 4u;
        runtime->cpu.regs[REG_EAX] = read_le(program, runtime->cpu.regs[REG_ESI], 4, line);
        runtime->cpu.regs[REG_ESI] += step4;
    }
}

static uint32_t target_label(Runtime *runtime, const char *name, int line) {
    Symbol *symbol = symbols_find(&runtime->program->symbols, name);
    if (!symbol || symbol->kind != SYM_CODE) {
        runtime_error2(line, "unknown code label", name);
    }
    return (uint32_t)symbol->instr_index;
}

static void execute_call(Runtime *runtime, const char *target, int line) {
    if (run_builtin(runtime, target, 0, line)) {
        return;
    }
    uint32_t dest = target_label(runtime, target, line);
    push32(runtime, runtime->cpu.eip, line);
    runtime->cpu.eip = dest;
}

static bool jump_condition(Runtime *runtime, const char *op) {
    if (ci_eq(op, "jmp")) return true;
    if (ci_eq(op, "je") || ci_eq(op, "jz")) return runtime->cpu.zf;
    if (ci_eq(op, "jne") || ci_eq(op, "jnz")) return !runtime->cpu.zf;
    if (ci_eq(op, "jc") || ci_eq(op, "jb") || ci_eq(op, "jnae")) return runtime->cpu.cf;
    if (ci_eq(op, "jnc") || ci_eq(op, "jae") || ci_eq(op, "jnb")) return !runtime->cpu.cf;
    if (ci_eq(op, "ja") || ci_eq(op, "jnbe")) return !runtime->cpu.cf && !runtime->cpu.zf;
    if (ci_eq(op, "jbe") || ci_eq(op, "jna")) return runtime->cpu.cf || runtime->cpu.zf;
    if (ci_eq(op, "js")) return runtime->cpu.sf;
    if (ci_eq(op, "jns")) return !runtime->cpu.sf;
    if (ci_eq(op, "jo")) return runtime->cpu.of;
    if (ci_eq(op, "jno")) return !runtime->cpu.of;
    if (ci_eq(op, "jg") || ci_eq(op, "jnle")) return !runtime->cpu.zf && (runtime->cpu.sf == runtime->cpu.of);
    if (ci_eq(op, "jge") || ci_eq(op, "jnl")) return runtime->cpu.sf == runtime->cpu.of;
    if (ci_eq(op, "jl") || ci_eq(op, "jnge")) return runtime->cpu.sf != runtime->cpu.of;
    if (ci_eq(op, "jle") || ci_eq(op, "jng")) return runtime->cpu.zf || (runtime->cpu.sf != runtime->cpu.of);
    return false;
}

static bool is_jump_op(const char *op) {
    const char *ops[] = {
        "jmp", "je", "jz", "jne", "jnz", "jc", "jnc", "jb", "jnae", "jae", "jnb",
        "ja", "jnbe", "jbe", "jna", "js", "jns", "jo", "jno", "jg", "jnle",
        "jge", "jnl", "jl", "jnge", "jle", "jng"
    };
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        if (ci_eq(op, ops[i])) {
            return true;
        }
    }
    return false;
}

static void require_operands(const Instruction *ins, int count) {
    if (ins->operand_count != count) {
        runtime_error(ins->line, "wrong number of operands");
    }
}

/* ---- x87 FPU helpers ---- */

static bool parse_st_reg(const char *text, int *n_out) {
    char *copy = xstrdup(text);
    char *s = trim_in_place(copy);
    bool found = false;
    if (ci_starts_with(s, "st")) {
        char *rest = trim_in_place(s + 2);
        if (*rest == '\0') {
            *n_out = 0;
            found = true;
        } else if (rest[0] == '(' && rest[2] == ')' && rest[3] == '\0' &&
                   rest[1] >= '0' && rest[1] <= '7') {
            *n_out = rest[1] - '0';
            found = true;
        }
    }
    free(copy);
    return found;
}

static double fpu_st_get(const Runtime *runtime, int n) {
    return runtime->cpu.fpu_st[(runtime->cpu.fpu_top + n) & 7];
}

static void fpu_push(Runtime *runtime, double val) {
    runtime->cpu.fpu_top = (runtime->cpu.fpu_top - 1) & 7;
    runtime->cpu.fpu_st[runtime->cpu.fpu_top] = val;
}

static void fpu_pop(Runtime *runtime) {
    runtime->cpu.fpu_top = (runtime->cpu.fpu_top + 1) & 7;
}

static uint64_t read_le64(const Program *program, uint32_t address, int line) {
    if (address > MEMORY_SIZE || address + 8u > MEMORY_SIZE) {
        runtime_error(line, "memory read out of bounds (64-bit)");
    }
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) {
        value |= ((uint64_t)program->memory[address + (uint32_t)i]) << (8 * i);
    }
    return value;
}

static void write_le64(Program *program, uint32_t address, uint64_t value, int line) {
    if (address > MEMORY_SIZE || address + 8u > MEMORY_SIZE) {
        runtime_error(line, "memory write out of bounds (64-bit)");
    }
    for (int i = 0; i < 8; i++) {
        program->memory[address + (uint32_t)i] = (uint8_t)((value >> (8 * i)) & 0xffu);
    }
}

static double fpu_load_float(Runtime *runtime, uint32_t address, int width, int line) {
    if (width == 4) {
        uint32_t bits = read_le(runtime->program, address, 4, line);
        float f;
        memcpy(&f, &bits, 4);
        return (double)f;
    }
    uint64_t bits = read_le64(runtime->program, address, line);
    double d;
    memcpy(&d, &bits, 8);
    return d;
}

static void fpu_store_float(Runtime *runtime, uint32_t address, int width, double val, int line) {
    if (width == 4) {
        float f = (float)val;
        uint32_t bits;
        memcpy(&bits, &f, 4);
        write_le(runtime->program, address, 4, bits, line);
        return;
    }
    uint64_t bits;
    memcpy(&bits, &val, 8);
    write_le64(runtime->program, address, bits, line);
}

static bool fpu_parse_operand(Runtime *runtime, const char *text,
                              int *st_n_out, uint32_t *addr_out, int *width_out, int line) {
    char *copy = xstrdup(text);
    char *s = trim_in_place(copy);
    if (parse_st_reg(s, st_n_out)) {
        free(copy);
        if (addr_out) *addr_out = 0;
        if (width_out) *width_out = 0;
        return true;
    }
    free(copy);
    Operand op = parse_operand(runtime, text, 0, line);
    if (op.kind == OPERAND_REG) {
        runtime_error(line, "FPU instruction: unexpected integer register operand");
    }
    *st_n_out = -1;
    if (addr_out) *addr_out = op.address;
    if (width_out) *width_out = op.width > 0 ? op.width : 4;
    return false;
}

static void fpu_set_compare_flags(Runtime *runtime, double a, double b) {
    bool c0, c2, c3;
    if (isnan(a) || isnan(b)) {
        c0 = true; c2 = true; c3 = true;
    } else if (a < b) {
        c0 = true; c2 = false; c3 = false;
    } else if (a == b) {
        c0 = false; c2 = false; c3 = true;
    } else {
        c0 = false; c2 = false; c3 = false;
    }
    /* Clear C0 (bit 8), C2 (bit 10), C3 (bit 14) then set */
    runtime->cpu.fpu_status &= ~(uint16_t)(0x4500u);
    if (c0) runtime->cpu.fpu_status |= (uint16_t)(1u << 8);
    if (c2) runtime->cpu.fpu_status |= (uint16_t)(1u << 10);
    if (c3) runtime->cpu.fpu_status |= (uint16_t)(1u << 14);
}

static void execute_instruction(Runtime *runtime, Instruction *ins) {
    const char *op = ins->op;

    if (ins->prefix && !is_string_op(op)) {
        runtime_error2(ins->line, "rep prefix on non-string instruction", op);
    }

    if (ci_eq(op, "exit")) {
        runtime->cpu.exit_code = 0;
        runtime->cpu.running = false;
        return;
    }

    if (ci_eq(op, "cld") || ci_eq(op, "std")) {
        require_operands(ins, 0);
        runtime->cpu.df = ci_eq(op, "std");
        return;
    }

    if (ci_eq(op, "clc") || ci_eq(op, "stc")) {
        require_operands(ins, 0);
        runtime->cpu.cf = ci_eq(op, "stc");
        return;
    }

    if (ci_eq(op, "daa")) {
        require_operands(ins, 0);
        uint8_t old_al = (uint8_t)(runtime->cpu.regs[REG_EAX] & 0xffu);
        uint8_t al = old_al;
        bool old_cf = runtime->cpu.cf;
        if ((al & 0x0fu) > 9u || runtime->cpu.af) {
            al = (uint8_t)(al + 0x06u);
            runtime->cpu.af = true;
        } else {
            runtime->cpu.af = false;
        }
        if (old_al > 0x99u || old_cf) {
            al = (uint8_t)(al + 0x60u);
            runtime->cpu.cf = true;
        } else {
            runtime->cpu.cf = false;
        }
        runtime->cpu.regs[REG_EAX] = (runtime->cpu.regs[REG_EAX] & 0xffffff00u) | al;
        runtime->cpu.zf = al == 0;
        runtime->cpu.sf = (al & 0x80u) != 0;
        runtime->cpu.of = false;
        return;
    }

    if (is_jump_op(op)) {
        require_operands(ins, 1);
        if (jump_condition(runtime, op)) {
            runtime->cpu.eip = target_label(runtime, ins->operands[0], ins->line);
        }
        return;
    }

    if (strncasecmp(op, "cmov", 4) == 0 && strlen(op) > 4) {
        require_operands(ins, 2);
        Operand dst = parse_operand(runtime, ins->operands[0], 0, ins->line);
        Operand src = parse_operand(runtime, ins->operands[1], dst.width, ins->line);
        char jcc[16];
        snprintf(jcc, sizeof(jcc), "j%s", op + 4);
        if (jump_condition(runtime, jcc)) {
            uint32_t val = operand_read(runtime, src, ins->line);
            operand_write(runtime, dst, val, ins->line);
        }
        return;
    }

    if (ci_eq(op, "loop")) {
        require_operands(ins, 1);
        runtime->cpu.regs[REG_ECX]--;
        if (runtime->cpu.regs[REG_ECX] != 0) {
            runtime->cpu.eip = target_label(runtime, ins->operands[0], ins->line);
        }
        return;
    }

    if (ci_eq(op, "call")) {
        require_operands(ins, 1);
        execute_call(runtime, ins->operands[0], ins->line);
        return;
    }

    if (ci_eq(op, "invoke")) {
        if (ins->operand_count < 1) {
            runtime_error(ins->line, "INVOKE requires a target");
        }
        for (int i = ins->operand_count - 1; i >= 1; i--) {
            Operand arg = parse_operand(runtime, ins->operands[i], 4, ins->line);
            push32(runtime, operand_read(runtime, arg, ins->line), ins->line);
        }
        if (!run_builtin(runtime, ins->operands[0], ins->operand_count - 1, ins->line)) {
            uint32_t dest = target_label(runtime, ins->operands[0], ins->line);
            push32(runtime, runtime->cpu.eip, ins->line);
            runtime->cpu.eip = dest;
        }
        return;
    }

    if (ci_eq(op, "ret")) {
        uint32_t dest = pop32(runtime, ins->line);
        if (ins->operand_count == 1) {
            Operand bytes = parse_operand(runtime, ins->operands[0], 4, ins->line);
            runtime->cpu.regs[REG_ESP] += operand_read(runtime, bytes, ins->line);
        } else if (ins->operand_count != 0) {
            runtime_error(ins->line, "RET accepts zero or one operand");
        }
        runtime->cpu.eip = dest;
        return;
    }

    if (ci_eq(op, "push")) {
        require_operands(ins, 1);
        Operand src = parse_operand(runtime, ins->operands[0], 4, ins->line);
        push32(runtime, operand_read(runtime, src, ins->line), ins->line);
        return;
    }

    if (ci_eq(op, "pop")) {
        require_operands(ins, 1);
        Operand dst = parse_operand(runtime, ins->operands[0], 4, ins->line);
        operand_write(runtime, dst, pop32(runtime, ins->line), ins->line);
        return;
    }

    if (ci_eq(op, "pushad")) {
        require_operands(ins, 0);
        uint32_t saved_esp = runtime->cpu.regs[REG_ESP];
        for (int i = 0; i < 8; i++) {
            uint32_t value = pushad_order[i] == REG_ESP ? saved_esp : runtime->cpu.regs[pushad_order[i]];
            push32(runtime, value, ins->line);
        }
        return;
    }

    if (ci_eq(op, "popad")) {
        require_operands(ins, 0);
        for (int i = 7; i >= 0; i--) {
            uint32_t value = pop32(runtime, ins->line);
            if (pushad_order[i] != REG_ESP) {
                runtime->cpu.regs[pushad_order[i]] = value;
            }
        }
        return;
    }

    if (ci_eq(op, "pushfd")) {
        require_operands(ins, 0);
        push32(runtime, pack_eflags(&runtime->cpu), ins->line);
        return;
    }

    if (ci_eq(op, "popfd")) {
        require_operands(ins, 0);
        unpack_eflags(&runtime->cpu, pop32(runtime, ins->line));
        return;
    }

    if (ci_eq(op, "mov") || ci_eq(op, "movzx") || ci_eq(op, "movsx")) {
        require_operands(ins, 2);
        Operand dst = parse_operand(runtime, ins->operands[0], 0, ins->line);
        Operand src = parse_operand(runtime, ins->operands[1], (ci_eq(op, "mov") ? dst.width : 0), ins->line);
        uint32_t value = operand_read(runtime, src, ins->line);
        if (ci_eq(op, "movsx")) {
            value = (uint32_t)sign_extend_value(value, src.width);
        }
        operand_write(runtime, dst, value, ins->line);
        return;
    }

    if (ci_eq(op, "lea")) {
        require_operands(ins, 2);
        Operand dst = parse_operand(runtime, ins->operands[0], 4, ins->line);
        Operand src = parse_operand(runtime, ins->operands[1], 4, ins->line);
        if (src.kind != OPERAND_MEM) {
            runtime_error(ins->line, "LEA source must be memory-like");
        }
        operand_write(runtime, dst, src.address, ins->line);
        return;
    }

    if (ci_eq(op, "xchg")) {
        require_operands(ins, 2);
        Operand a = parse_operand(runtime, ins->operands[0], 0, ins->line);
        Operand b = parse_operand(runtime, ins->operands[1], a.width, ins->line);
        uint32_t av = operand_read(runtime, a, ins->line);
        uint32_t bv = operand_read(runtime, b, ins->line);
        operand_write(runtime, a, bv, ins->line);
        operand_write(runtime, b, av, ins->line);
        return;
    }

    if (ci_eq(op, "cmp") || ci_eq(op, "test")) {
        require_operands(ins, 2);
        Operand a = parse_operand(runtime, ins->operands[0], 0, ins->line);
        Operand b = parse_operand(runtime, ins->operands[1], a.width, ins->line);
        uint32_t av = operand_read(runtime, a, ins->line);
        uint32_t bv = operand_read(runtime, b, ins->line);
        if (ci_eq(op, "cmp")) {
            set_sub_flags(runtime, av, bv, av - bv, a.width);
        } else {
            set_logic_flags(runtime, av & bv, a.width);
        }
        return;
    }

    if (ci_eq(op, "add") || ci_eq(op, "adc") || ci_eq(op, "sub") || ci_eq(op, "and") || ci_eq(op, "or") || ci_eq(op, "xor")) {
        require_operands(ins, 2);
        Operand dst = parse_operand(runtime, ins->operands[0], 0, ins->line);
        Operand src = parse_operand(runtime, ins->operands[1], dst.width, ins->line);
        uint32_t a = operand_read(runtime, dst, ins->line);
        uint32_t b = operand_read(runtime, src, ins->line);
        uint32_t result = 0;
        if (ci_eq(op, "add")) {
            result = a + b;
            set_add_flags(runtime, a, b, result, dst.width);
        } else if (ci_eq(op, "adc")) {
            uint32_t carry = runtime->cpu.cf ? 1u : 0u;
            result = a + b + carry;
            set_add_flags_with_carry(runtime, a, b, carry, result, dst.width);
        } else if (ci_eq(op, "sub")) {
            result = a - b;
            set_sub_flags(runtime, a, b, result, dst.width);
        } else if (ci_eq(op, "and")) {
            result = a & b;
            set_logic_flags(runtime, result, dst.width);
        } else if (ci_eq(op, "or")) {
            result = a | b;
            set_logic_flags(runtime, result, dst.width);
        } else {
            result = a ^ b;
            set_logic_flags(runtime, result, dst.width);
        }
        operand_write(runtime, dst, result, ins->line);
        return;
    }

    if (ci_eq(op, "sbb")) {
        require_operands(ins, 2);
        Operand dst = parse_operand(runtime, ins->operands[0], 0, ins->line);
        Operand src = parse_operand(runtime, ins->operands[1], dst.width, ins->line);
        uint32_t a = operand_read(runtime, dst, ins->line);
        uint32_t b = operand_read(runtime, src, ins->line);
        uint32_t borrow = runtime->cpu.cf ? 1u : 0u;
        uint32_t result = a - b - borrow;
        operand_write(runtime, dst, result, ins->line);
        set_sub_flags_with_borrow(runtime, a, b, borrow, result, dst.width);
        return;
    }

    if (ci_eq(op, "inc") || ci_eq(op, "dec") || ci_eq(op, "neg") || ci_eq(op, "not")) {
        require_operands(ins, 1);
        Operand dst = parse_operand(runtime, ins->operands[0], 0, ins->line);
        uint32_t a = operand_read(runtime, dst, ins->line);
        uint32_t result = 0;
        if (ci_eq(op, "inc")) {
            result = a + 1;
            bool old_cf = runtime->cpu.cf;
            set_add_flags(runtime, a, 1, result, dst.width);
            runtime->cpu.cf = old_cf;
        } else if (ci_eq(op, "dec")) {
            result = a - 1;
            bool old_cf = runtime->cpu.cf;
            set_sub_flags(runtime, a, 1, result, dst.width);
            runtime->cpu.cf = old_cf;
        } else if (ci_eq(op, "neg")) {
            result = (uint32_t)(0 - a);
            set_sub_flags(runtime, 0, a, result, dst.width);
        } else {
            result = ~a;
        }
        operand_write(runtime, dst, result, ins->line);
        return;
    }

    if (ci_eq(op, "shl") || ci_eq(op, "sal") || ci_eq(op, "shr") || ci_eq(op, "sar")) {
        require_operands(ins, 2);
        Operand dst = parse_operand(runtime, ins->operands[0], 0, ins->line);
        Operand count_op = parse_operand(runtime, ins->operands[1], 1, ins->line);
        uint32_t a = operand_read(runtime, dst, ins->line);
        uint32_t count = operand_read(runtime, count_op, ins->line) & 31u;
        uint32_t result = a;
        if (count) {
            if (ci_eq(op, "shl") || ci_eq(op, "sal")) {
                runtime->cpu.cf = ((a << (count - 1)) & sign_bit_for_width(dst.width)) != 0;
                result = a << count;
            } else if (ci_eq(op, "shr")) {
                runtime->cpu.cf = ((a >> (count - 1)) & 1u) != 0;
                result = a >> count;
            } else {
                runtime->cpu.cf = ((a >> (count - 1)) & 1u) != 0;
                result = (uint32_t)(sign_extend_value(a, dst.width) >> count);
            }
            set_logic_flags(runtime, result, dst.width);
        }
        operand_write(runtime, dst, result, ins->line);
        return;
    }

    if (ci_eq(op, "rol") || ci_eq(op, "ror") || ci_eq(op, "rcl") || ci_eq(op, "rcr")) {
        require_operands(ins, 2);
        Operand dst = parse_operand(runtime, ins->operands[0], 0, ins->line);
        Operand cnt_op = parse_operand(runtime, ins->operands[1], 1, ins->line);
        uint32_t a = operand_read(runtime, dst, ins->line);
        uint32_t raw_count = operand_read(runtime, cnt_op, ins->line) & 31u;
        int bits = dst.width * 8;
        uint32_t mask = (uint32_t)width_mask(dst.width);
        uint32_t av = a & mask;
        uint32_t result = av;
        if (ci_eq(op, "rol")) {
            uint32_t count = raw_count % (uint32_t)bits;
            if (count) result = ((av << count) | (av >> (bits - count))) & mask;
            runtime->cpu.cf = (result & 1u) != 0;
            if (raw_count == 1)
                runtime->cpu.of = (bool)runtime->cpu.cf ^ (bool)((result >> (bits - 1)) & 1u);
        } else if (ci_eq(op, "ror")) {
            uint32_t count = raw_count % (uint32_t)bits;
            if (count) result = ((av >> count) | (av << (bits - count))) & mask;
            runtime->cpu.cf = ((result >> (bits - 1)) & 1u) != 0;
            if (raw_count == 1)
                runtime->cpu.of = (bool)((result >> (bits - 1)) & 1u) ^
                                  (bool)((result >> (bits - 2)) & 1u);
        } else if (ci_eq(op, "rcl")) {
            int eff = bits + 1;
            uint32_t count = raw_count % (uint32_t)eff;
            if (count) {
                uint64_t tmp = (uint64_t)av | ((uint64_t)runtime->cpu.cf << bits);
                uint64_t rot = ((tmp << count) | (tmp >> (eff - count))) &
                               ((1ULL << eff) - 1ULL);
                result = (uint32_t)(rot & mask);
                runtime->cpu.cf = (bool)((rot >> bits) & 1u);
            }
            if (raw_count == 1)
                runtime->cpu.of = (bool)runtime->cpu.cf ^ (bool)((result >> (bits - 1)) & 1u);
        } else {
            int eff = bits + 1;
            uint32_t count = raw_count % (uint32_t)eff;
            bool old_cf = runtime->cpu.cf;
            if (count) {
                uint64_t tmp = (uint64_t)av | ((uint64_t)runtime->cpu.cf << bits);
                uint64_t rot = ((tmp >> count) | (tmp << (eff - count))) &
                               ((1ULL << eff) - 1ULL);
                result = (uint32_t)(rot & mask);
                runtime->cpu.cf = (bool)((rot >> bits) & 1u);
            }
            if (raw_count == 1)
                runtime->cpu.of = old_cf ^ (bool)((result >> (bits - 1)) & 1u);
        }
        operand_write(runtime, dst, result, ins->line);
        return;
    }

    if (ci_eq(op, "imul")) {
        if (ins->operand_count == 1) {
            Operand src = parse_operand(runtime, ins->operands[0], 4, ins->line);
            int64_t result = (int64_t)(int32_t)runtime->cpu.regs[REG_EAX] * (int64_t)(int32_t)operand_read(runtime, src, ins->line);
            runtime->cpu.regs[REG_EAX] = (uint32_t)result;
            runtime->cpu.regs[REG_EDX] = (uint32_t)((uint64_t)result >> 32);
            runtime->cpu.cf = runtime->cpu.of = result < INT32_MIN || result > INT32_MAX;
        } else if (ins->operand_count == 2 || ins->operand_count == 3) {
            Operand dst = parse_operand(runtime, ins->operands[0], 0, ins->line);
            Operand src = parse_operand(runtime, ins->operands[1], dst.width, ins->line);
            int64_t a = sign_extend_value(operand_read(runtime, src, ins->line), src.width);
            int64_t b = 0;
            if (ins->operand_count == 3) {
                Operand imm = parse_operand(runtime, ins->operands[2], dst.width, ins->line);
                b = sign_extend_value(operand_read(runtime, imm, ins->line), imm.width);
            } else {
                b = sign_extend_value(operand_read(runtime, dst, ins->line), dst.width);
            }
            int64_t result = a * b;
            operand_write(runtime, dst, (uint32_t)result, ins->line);
            runtime->cpu.cf = runtime->cpu.of = result < INT32_MIN || result > INT32_MAX;
        } else {
            runtime_error(ins->line, "IMUL accepts one, two, or three operands");
        }
        return;
    }

    if (ci_eq(op, "mul")) {
        require_operands(ins, 1);
        Operand src = parse_operand(runtime, ins->operands[0], 4, ins->line);
        uint64_t result = (uint64_t)runtime->cpu.regs[REG_EAX] * (uint64_t)operand_read(runtime, src, ins->line);
        runtime->cpu.regs[REG_EAX] = (uint32_t)result;
        runtime->cpu.regs[REG_EDX] = (uint32_t)(result >> 32);
        runtime->cpu.cf = runtime->cpu.of = runtime->cpu.regs[REG_EDX] != 0;
        return;
    }

    if (ci_eq(op, "div") || ci_eq(op, "idiv")) {
        require_operands(ins, 1);
        Operand src = parse_operand(runtime, ins->operands[0], 4, ins->line);
        uint32_t divisor = operand_read(runtime, src, ins->line);
        if (divisor == 0) {
            runtime_error(ins->line, "division by zero");
        }
        if (ci_eq(op, "div")) {
            uint64_t dividend = ((uint64_t)runtime->cpu.regs[REG_EDX] << 32) | runtime->cpu.regs[REG_EAX];
            runtime->cpu.regs[REG_EAX] = (uint32_t)(dividend / divisor);
            runtime->cpu.regs[REG_EDX] = (uint32_t)(dividend % divisor);
        } else {
            int64_t dividend = ((int64_t)(int32_t)runtime->cpu.regs[REG_EDX] << 32) | runtime->cpu.regs[REG_EAX];
            int32_t signed_divisor = (int32_t)divisor;
            runtime->cpu.regs[REG_EAX] = (uint32_t)(dividend / signed_divisor);
            runtime->cpu.regs[REG_EDX] = (uint32_t)(dividend % signed_divisor);
        }
        return;
    }

    if (ci_eq(op, "cdq")) {
        require_operands(ins, 0);
        runtime->cpu.regs[REG_EDX] = (runtime->cpu.regs[REG_EAX] & 0x80000000u) ? 0xffffffffu : 0u;
        return;
    }

    if (ci_eq(op, "cbw")) {
        require_operands(ins, 0);
        uint32_t al = runtime->cpu.regs[REG_EAX] & 0xffu;
        uint32_t ah = (al & 0x80u) ? 0xffu : 0u;
        runtime->cpu.regs[REG_EAX] = (runtime->cpu.regs[REG_EAX] & 0xffff0000u) | (ah << 8) | al;
        return;
    }

    if (ci_eq(op, "cwde")) {
        require_operands(ins, 0);
        uint32_t ax = runtime->cpu.regs[REG_EAX] & 0xffffu;
        uint32_t hi = (ax & 0x8000u) ? 0xffff0000u : 0u;
        runtime->cpu.regs[REG_EAX] = hi | ax;
        return;
    }

    if (ci_eq(op, "cwd")) {
        require_operands(ins, 0);
        uint32_t ax = runtime->cpu.regs[REG_EAX] & 0xffffu;
        uint32_t dx = (ax & 0x8000u) ? 0xffffu : 0u;
        runtime->cpu.regs[REG_EDX] = (runtime->cpu.regs[REG_EDX] & 0xffff0000u) | dx;
        return;
    }

    if (is_string_op(op)) {
        require_operands(ins, 0);
        const char *prefix = ins->prefix;
        if (!prefix) {
            exec_string_step(runtime, op, ins->line);
            return;
        }
        bool zf_sensitive = ci_eq(op, "scasb") || ci_eq(op, "cmpsb");
        bool check_zf = zf_sensitive && (ci_eq(prefix, "repe") || ci_eq(prefix, "repne"));
        bool want_zf = ci_eq(prefix, "repe");
        while (runtime->cpu.regs[REG_ECX] != 0) {
            exec_string_step(runtime, op, ins->line);
            runtime->cpu.regs[REG_ECX]--;
            if (check_zf && runtime->cpu.zf != want_zf) {
                break;
            }
        }
        return;
    }

    if (ci_eq(op, "mwrite")) {
        require_operands(ins, 1);
        char scratch[4096];
        char *arg_copy = xstrdup(ins->operands[0]);
        char *arg = trim_in_place(arg_copy);
        char *text = arg;
        size_t len = strlen(text);
        if (len >= 2 && text[0] == '<' && text[len - 1] == '>') {
            text[len - 1] = '\0';
            text++;
        }
        if (len >= 2 && ((text[0] == '"' && text[strlen(text) - 1] == '"') || (text[0] == '\'' && text[strlen(text) - 1] == '\''))) {
            size_t out = 0;
            for (size_t i = 1; i + 1 < strlen(text) && out + 1 < sizeof(scratch); i++) {
                scratch[out++] = text[i];
            }
            scratch[out] = '\0';
            fputs(scratch, stdout);
        } else {
            free(arg_copy);
            runtime_error(ins->line, "mWrite expects a literal string");
        }
        free(arg_copy);
        return;
    }

    /* ---- x87 FPU instructions ---- */

    if (ci_eq(op, "fldz")) {
        require_operands(ins, 0);
        fpu_push(runtime, 0.0);
        return;
    }

    if (ci_eq(op, "fld1")) {
        require_operands(ins, 0);
        fpu_push(runtime, 1.0);
        return;
    }

    if (ci_eq(op, "fchs")) {
        require_operands(ins, 0);
        runtime->cpu.fpu_st[runtime->cpu.fpu_top] = -runtime->cpu.fpu_st[runtime->cpu.fpu_top];
        return;
    }

    if (ci_eq(op, "fabs")) {
        require_operands(ins, 0);
        double v = runtime->cpu.fpu_st[runtime->cpu.fpu_top];
        runtime->cpu.fpu_st[runtime->cpu.fpu_top] = v < 0.0 ? -v : v;
        return;
    }

    if (ci_eq(op, "fld")) {
        require_operands(ins, 1);
        int n = 0;
        uint32_t addr = 0;
        int width = 0;
        bool is_reg = fpu_parse_operand(runtime, ins->operands[0], &n, &addr, &width, ins->line);
        if (is_reg) {
            double val = fpu_st_get(runtime, n);
            fpu_push(runtime, val);
        } else {
            fpu_push(runtime, fpu_load_float(runtime, addr, width, ins->line));
        }
        return;
    }

    if (ci_eq(op, "fst") || ci_eq(op, "fstp")) {
        require_operands(ins, 1);
        int n = 0;
        uint32_t addr = 0;
        int width = 0;
        bool is_reg = fpu_parse_operand(runtime, ins->operands[0], &n, &addr, &width, ins->line);
        double val = fpu_st_get(runtime, 0);
        if (is_reg) {
            runtime->cpu.fpu_st[(runtime->cpu.fpu_top + (size_t)n) & 7] = val;
        } else {
            fpu_store_float(runtime, addr, width, val, ins->line);
        }
        if (ci_eq(op, "fstp")) {
            fpu_pop(runtime);
        }
        return;
    }

    if (ci_eq(op, "fild")) {
        require_operands(ins, 1);
        int n = 0;
        uint32_t addr = 0;
        int width = 0;
        fpu_parse_operand(runtime, ins->operands[0], &n, &addr, &width, ins->line);
        double val;
        if (width == 2) {
            val = (double)(int16_t)read_le(runtime->program, addr, 2, ins->line);
        } else if (width == 8) {
            uint64_t raw = read_le64(runtime->program, addr, ins->line);
            val = (double)(int64_t)raw;
        } else {
            val = (double)(int32_t)read_le(runtime->program, addr, 4, ins->line);
        }
        fpu_push(runtime, val);
        return;
    }

    if (ci_eq(op, "fistp") || ci_eq(op, "fist")) {
        require_operands(ins, 1);
        int n = 0;
        uint32_t addr = 0;
        int width = 0;
        fpu_parse_operand(runtime, ins->operands[0], &n, &addr, &width, ins->line);
        int64_t ival = (int64_t)fpu_st_get(runtime, 0);
        if (width == 2) {
            write_le(runtime->program, addr, 2, (uint32_t)(int16_t)ival, ins->line);
        } else if (width == 8) {
            write_le64(runtime->program, addr, (uint64_t)ival, ins->line);
        } else {
            write_le(runtime->program, addr, 4, (uint32_t)(int32_t)ival, ins->line);
        }
        if (ci_eq(op, "fistp")) {
            fpu_pop(runtime);
        }
        return;
    }

    if (ci_eq(op, "fadd") || ci_eq(op, "fsub") || ci_eq(op, "fsubr") ||
        ci_eq(op, "fmul") || ci_eq(op, "fdiv") || ci_eq(op, "fdivr") ||
        ci_eq(op, "faddp") || ci_eq(op, "fsubp") || ci_eq(op, "fmulp") || ci_eq(op, "fdivp")) {
        bool do_pop = ci_eq(op, "faddp") || ci_eq(op, "fsubp") ||
                      ci_eq(op, "fmulp") || ci_eq(op, "fdivp");
        double a = fpu_st_get(runtime, 0);
        double b = 0.0;
        int dst_n = 0;
        bool two_reg = false;

        if (ins->operand_count == 0) {
            b = fpu_st_get(runtime, 1);
        } else if (ins->operand_count == 1) {
            int n = 0;
            uint32_t addr = 0;
            int width = 0;
            bool is_reg = fpu_parse_operand(runtime, ins->operands[0], &n, &addr, &width, ins->line);
            b = is_reg ? fpu_st_get(runtime, n) : fpu_load_float(runtime, addr, width, ins->line);
        } else if (ins->operand_count == 2) {
            int src_n = 0;
            uint32_t dummy = 0;
            int dummy_w = 0;
            bool dst_is_reg = fpu_parse_operand(runtime, ins->operands[0], &dst_n, &dummy, &dummy_w, ins->line);
            bool src_is_reg = fpu_parse_operand(runtime, ins->operands[1], &src_n, &dummy, &dummy_w, ins->line);
            if (!dst_is_reg || !src_is_reg) {
                runtime_error(ins->line, "FPU arithmetic: two-register form expected");
            }
            a = fpu_st_get(runtime, dst_n);
            b = fpu_st_get(runtime, src_n);
            two_reg = true;
        } else {
            runtime_error(ins->line, "FPU arithmetic: too many operands");
        }

        double result;
        if (ci_eq(op, "fadd") || ci_eq(op, "faddp")) {
            result = a + b;
        } else if (ci_eq(op, "fsub") || ci_eq(op, "fsubp")) {
            result = a - b;
        } else if (ci_eq(op, "fsubr")) {
            result = b - a;
        } else if (ci_eq(op, "fmul") || ci_eq(op, "fmulp")) {
            result = a * b;
        } else if (ci_eq(op, "fdiv") || ci_eq(op, "fdivp")) {
            result = a / b;
        } else {
            result = b / a; /* fdivr */
        }

        if (two_reg) {
            runtime->cpu.fpu_st[(runtime->cpu.fpu_top + (size_t)dst_n) & 7] = result;
        } else {
            runtime->cpu.fpu_st[runtime->cpu.fpu_top] = result;
        }
        if (do_pop) {
            fpu_pop(runtime);
        }
        return;
    }

    if (ci_eq(op, "fcom") || ci_eq(op, "fcomp")) {
        double a = fpu_st_get(runtime, 0);
        double b;
        if (ins->operand_count == 0) {
            b = fpu_st_get(runtime, 1);
        } else {
            int n = 0;
            uint32_t addr = 0;
            int width = 0;
            bool is_reg = fpu_parse_operand(runtime, ins->operands[0], &n, &addr, &width, ins->line);
            b = is_reg ? fpu_st_get(runtime, n) : fpu_load_float(runtime, addr, width, ins->line);
        }
        fpu_set_compare_flags(runtime, a, b);
        if (ci_eq(op, "fcomp")) {
            fpu_pop(runtime);
        }
        return;
    }

    if (ci_eq(op, "fcompp")) {
        require_operands(ins, 0);
        double a = fpu_st_get(runtime, 0);
        double b = fpu_st_get(runtime, 1);
        fpu_set_compare_flags(runtime, a, b);
        fpu_pop(runtime);
        fpu_pop(runtime);
        return;
    }

    if (ci_eq(op, "fnstsw")) {
        require_operands(ins, 1);
        char *sw_copy = xstrdup(ins->operands[0]);
        char *sw_s = trim_in_place(sw_copy);
        if (!ci_eq(sw_s, "ax")) {
            free(sw_copy);
            runtime_error(ins->line, "fnstsw: only 'ax' operand is supported");
        }
        free(sw_copy);
        uint16_t sw = runtime->cpu.fpu_status;
        sw = (uint16_t)((sw & ~(uint16_t)0x3800u) |
                        (uint16_t)((runtime->cpu.fpu_top & 7u) << 11));
        runtime->cpu.regs[REG_EAX] = (runtime->cpu.regs[REG_EAX] & 0xffff0000u) | sw;
        return;
    }

    if (ci_eq(op, "sahf")) {
        require_operands(ins, 0);
        uint32_t ah = (runtime->cpu.regs[REG_EAX] >> 8) & 0xffu;
        runtime->cpu.cf = (ah & 0x01u) != 0;
        runtime->cpu.af = (ah & 0x10u) != 0;
        runtime->cpu.zf = (ah & 0x40u) != 0;
        runtime->cpu.sf = (ah & 0x80u) != 0;
        return;
    }

    runtime_error2(ins->line, "unsupported instruction", op);
}

static void trace_instruction(const Runtime *runtime, uint32_t step, uint32_t eip, const Instruction *ins) {
    fprintf(stderr,
            "trace step=%u eip=%u line=%d prefix=%s op=%s "
            "eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X ebp=%08X esp=%08X "
            "zf=%d sf=%d cf=%d of=%d af=%d df=%d | %s\n",
            step, eip, ins->line, ins->prefix ? ins->prefix : "-", ins->op,
            runtime->cpu.regs[REG_EAX], runtime->cpu.regs[REG_EBX],
            runtime->cpu.regs[REG_ECX], runtime->cpu.regs[REG_EDX],
            runtime->cpu.regs[REG_ESI], runtime->cpu.regs[REG_EDI],
            runtime->cpu.regs[REG_EBP], runtime->cpu.regs[REG_ESP],
            runtime->cpu.zf ? 1 : 0, runtime->cpu.sf ? 1 : 0,
            runtime->cpu.cf ? 1 : 0, runtime->cpu.of ? 1 : 0,
            runtime->cpu.af ? 1 : 0, runtime->cpu.df ? 1 : 0,
            ins->raw ? ins->raw : "");
}

int run_program(Program *program, const RunOptions *options) {
    Runtime runtime;
    memset(&runtime, 0, sizeof(runtime));
    runtime.program = program;
    runtime.options = options;
    runtime.current_proc_index = -1;
    runtime.cpu.regs[REG_ESP] = STACK_TOP;
    runtime.cpu.regs[REG_EBP] = STACK_TOP;
    runtime.cpu.running = true;

    Symbol *entry = symbols_find(&program->symbols, program->code.entry);
    if (!entry || entry->kind != SYM_CODE) {
        fprintf(stderr, "masmrun: entry point not found: %s\n", program->code.entry);
        return 1;
    }
    runtime.cpu.eip = (uint32_t)entry->instr_index;

    uint32_t steps = 0;
    while (runtime.cpu.running) {
        if (runtime.cpu.eip >= program->code.count) {
            runtime.cpu.running = false;
            break;
        }
        uint32_t current_eip = runtime.cpu.eip;
        Instruction *ins = &program->code.items[current_eip];
        runtime.current_proc_index = ins->proc_index;
        if (runtime.options && runtime.options->debug &&
            !debug_pre_exec(&runtime, current_eip, ins)) {
            break;
        }
        if (!runtime.cpu.running) {
            break;
        }
        if (++steps > STEP_LIMIT) {
            runtime_error(0, "step limit exceeded");
        }
        runtime.cpu.eip = current_eip + 1;
        if (runtime.options && runtime.options->trace) {
            trace_instruction(&runtime, steps, current_eip, ins);
        }
        execute_instruction(&runtime, ins);
    }
    fflush(stdout);
    return runtime.cpu.exit_code;
}
