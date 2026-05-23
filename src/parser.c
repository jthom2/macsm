#include "masmrun.h"

static char *current_proc_name = NULL;
static int current_proc_index = -1;
static bool current_proc_prologue_done = false;
static bool current_proc_saw_instruction = false;
static char *current_proc_last_op = NULL;
static uint32_t hll_label_counter = 1;

typedef enum {
    CTRL_IF,
    CTRL_WHILE,
    CTRL_REPEAT
} ControlKind;

typedef struct {
    ControlKind kind;
    char *start_label;
    char *next_label;
    char *end_label;
    char *continue_label;
    bool saw_else;
} ControlFrame;

typedef struct {
    ControlFrame *items;
    size_t count;
    size_t cap;
} ControlStack;

static ControlStack control_stack = {0};

static void validate_local_name(const char *name, int line);
static ProcLocals *ensure_current_proc_locals(Program *program, int line);
static void locals_add_var(ProcLocals *locals, LocalVar local, int line);

static bool find_dup_form(const char *item, char **count_out, char **inner_out) {
    const char *p = item;
    bool in_string = false;
    char quote = '\0';
    while (*p) {
        if (in_string) {
            if (*p == quote) {
                in_string = false;
            }
            p++;
            continue;
        }
        if (*p == '"' || *p == '\'') {
            in_string = true;
            quote = *p++;
            continue;
        }
        if (tolower((unsigned char)p[0]) == 'd' && tolower((unsigned char)p[1]) == 'u' && tolower((unsigned char)p[2]) == 'p') {
            const char *before = p;
            const char *after = p + 3;
            while (before > item && isspace((unsigned char)before[-1])) {
                before--;
            }
            while (isspace((unsigned char)*after)) {
                after++;
            }
            if (*after == '(') {
                const char *end = item + strlen(item);
                while (end > after && isspace((unsigned char)end[-1])) {
                    end--;
                }
                if (end > after && end[-1] == ')') {
                    *count_out = xstrndup(item, (size_t)(before - item));
                    *inner_out = xstrndup(after + 1, (size_t)(end - after - 2));
                    return true;
                }
            }
        }
        p++;
    }
    return false;
}

static size_t parse_data_items(Program *program, const char *items_text, int elem_size, int line);

static void write_string_data(Program *program, const char *literal, int elem_size, int line, size_t *count) {
    size_t len = strlen(literal);
    if (len < 2) {
        parse_error(line, "invalid string literal");
    }
    char quote = literal[0];
    if (!((quote == '"' || quote == '\'') && literal[len - 1] == quote)) {
        parse_error(line, "invalid string literal");
    }
    for (size_t i = 1; i + 1 < len; i++) {
        unsigned char c = (unsigned char)literal[i];
        if (c == '\\' && i + 2 < len) {
            i++;
            switch (literal[i]) {
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case '0': c = '\0'; break;
                default: c = (unsigned char)literal[i]; break;
            }
        } else if (c == (unsigned char)quote && i + 2 < len && literal[i + 1] == quote) {
            i++;
        }
        write_data(program, elem_size, c, line);
        (*count)++;
    }
}

static size_t parse_one_data_item(Program *program, const char *item_text, int elem_size, int line) {
    char *copy = xstrdup(item_text);
    char *item = trim_in_place(copy);
    if (*item == '\0') {
        free(copy);
        return 0;
    }

    char *dup_count = NULL;
    char *dup_inner = NULL;
    if (find_dup_form(item, &dup_count, &dup_inner)) {
        int64_t count_value = eval_expr(NULL, program, dup_count, line);
        if (count_value < 0 || count_value > 1000000) {
            parse_error(line, "invalid DUP count");
        }
        size_t total = 0;
        for (int64_t i = 0; i < count_value; i++) {
            total += parse_data_items(program, dup_inner, elem_size, line);
        }
        free(dup_count);
        free(dup_inner);
        free(copy);
        return total;
    }

    if (ci_eq(item, "?")) {
        write_data(program, elem_size, 0, line);
        free(copy);
        return 1;
    }

    size_t item_len = strlen(item);
    if (item_len >= 2 && ((item[0] == '"' && item[item_len - 1] == '"') || (item[0] == '\'' && item[item_len - 1] == '\''))) {
        size_t count = 0;
        write_string_data(program, item, elem_size, line, &count);
        free(copy);
        return count;
    }

    double fval = 0.0;
    if (parse_float_literal(item, &fval)) {
        if (elem_size == 4) {
            float f32 = (float)fval;
            uint32_t bits;
            memcpy(&bits, &f32, 4);
            write_data(program, 4, (uint64_t)bits, line);
        } else if (elem_size == 8) {
            uint64_t bits;
            memcpy(&bits, &fval, 8);
            write_data(program, 8, bits, line);
        } else {
            parse_error(line, "float literal requires REAL4 or REAL8 data type");
        }
        free(copy);
        return 1;
    }

    int64_t value = eval_expr(NULL, program, item, line);
    write_data(program, elem_size, (uint64_t)value, line);
    free(copy);
    return 1;
}

static size_t parse_data_items(Program *program, const char *items_text, int elem_size, int line) {
    char *items[512];
    int count = 0;
    split_operands(items_text, items, &count, 512, line);
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        total += parse_one_data_item(program, items[i], elem_size, line);
        free(items[i]);
    }
    return total;
}

static StructDef *struct_add(Program *program, const char *name, int line) {
    if (structs_find(&program->structs, name)) {
        parse_error2(line, "duplicate STRUCT definition", name);
    }
    if (program->structs.count == program->structs.cap) {
        program->structs.cap = program->structs.cap ? program->structs.cap * 2 : 8;
        program->structs.items = xrealloc(program->structs.items,
                                          program->structs.cap * sizeof(program->structs.items[0]));
    }
    StructDef def;
    memset(&def, 0, sizeof(def));
    def.name = xstrdup(name);
    program->structs.items[program->structs.count] = def;
    return &program->structs.items[program->structs.count++];
}

static void struct_add_field(StructDef *def, StructField field, int line) {
    for (size_t i = 0; i < def->field_count; i++) {
        if (ci_eq(def->fields[i].name, field.name)) {
            parse_error2(line, "duplicate STRUCT field", field.name);
        }
    }
    if (def->field_count == def->field_cap) {
        def->field_cap = def->field_cap ? def->field_cap * 2 : 8;
        def->fields = xrealloc(def->fields, def->field_cap * sizeof(def->fields[0]));
    }
    def->fields[def->field_count++] = field;
}

static int resolve_data_type_size(Program *program, const char *type, const StructDef **struct_out) {
    if (struct_out) {
        *struct_out = NULL;
    }
    int elem_size = data_type_size(type);
    if (elem_size > 0) {
        return elem_size;
    }
    TypeAlias *alias = type_aliases_find(&program->aliases, type);
    if (alias) {
        return alias->width;
    }
    StructDef *def = structs_find(&program->structs, type);
    if (def) {
        if (struct_out) {
            *struct_out = def;
        }
        return (int)def->byte_size;
    }
    return 0;
}

static void parse_struct_field_line(Program *program, StructDef *def, char *line, int line_no) {
    char *cursor = line;
    char *field_name = next_token(&cursor);
    char *field_type = next_token(&cursor);
    if (!field_name || !field_type) {
        parse_error(line_no, "STRUCT field requires name, type, and initializer");
    }
    const StructDef *nested = NULL;
    int width = resolve_data_type_size(program, field_type, &nested);
    if (width <= 0 || nested) {
        parse_error2(line_no, "STRUCT fields currently support scalar types only", field_type);
    }
    char *init_text = trim_in_place(cursor);
    if (*init_text == '\0') {
        parse_error2(line_no, "STRUCT field requires initializer", field_name);
    }

    char *items[8];
    int item_count = 0;
    split_operands(init_text, items, &item_count, 8, line_no);
    if (item_count != 1) {
        for (int i = 0; i < item_count; i++) {
            free(items[i]);
        }
        parse_error2(line_no, "STRUCT fields currently support scalar initializer only", init_text);
    }

    int64_t default_value = 0;
    if (!ci_eq(items[0], "?") && !ci_eq(items[0], "<>")) {
        default_value = eval_expr(NULL, program, items[0], line_no);
    }
    free(items[0]);

    if (def->byte_size > SIZE_MAX - (size_t)width) {
        parse_error(line_no, "STRUCT size overflow");
    }
    StructField field;
    memset(&field, 0, sizeof(field));
    field.name = xstrdup(field_name);
    field.offset = (uint32_t)def->byte_size;
    field.width = width;
    field.count = 1;
    field.type_ref = xstrdup(field_type);
    field.has_default = true;
    field.default_value = default_value;
    struct_add_field(def, field, line_no);
    def->byte_size += (size_t)width;
}

static bool parse_struct_directive(Program *program, char *line, int line_no, StructDef **active_struct) {
    char *copy = xstrdup(line);
    char *cursor = copy;
    char *first = next_token(&cursor);
    char *second = next_token(&cursor);
    if (*active_struct) {
        if (first && second && ci_eq(second, "ENDS")) {
            if (!ci_eq(first, (*active_struct)->name)) {
                parse_error2(line_no, "ENDS does not match current STRUCT", first);
            }
            *active_struct = NULL;
        } else {
            parse_struct_field_line(program, *active_struct, line, line_no);
        }
        free(copy);
        return true;
    }
    if (first && second && ci_eq(second, "STRUCT")) {
        *active_struct = struct_add(program, first, line_no);
        free(copy);
        return true;
    }
    free(copy);
    return false;
}

static bool parse_typedef_line(Program *program, char *line, int line_no) {
    char *copy = xstrdup(line);
    char *cursor = copy;
    char *alias_name = next_token(&cursor);
    char *typedef_kw = next_token(&cursor);
    if (!alias_name || !typedef_kw || !ci_eq(typedef_kw, "TYPEDEF")) {
        free(copy);
        return false;
    }
    char *ptr_kw = next_token(&cursor);
    char *target_name = next_token(&cursor);
    if (!ptr_kw || !ci_eq(ptr_kw, "PTR") || !target_name) {
        free(copy);
        parse_error2(line_no, "TYPEDEF currently supports only PTR target", line);
    }
    if (!structs_find(&program->structs, target_name)) {
        free(copy);
        parse_error2(line_no, "TYPEDEF target struct not found", target_name);
    }
    if (type_aliases_find(&program->aliases, alias_name)) {
        free(copy);
        parse_error2(line_no, "duplicate TYPEDEF alias", alias_name);
    }
    if (program->aliases.count == program->aliases.cap) {
        program->aliases.cap = program->aliases.cap ? program->aliases.cap * 2 : 8;
        program->aliases.items = xrealloc(program->aliases.items,
                                          program->aliases.cap * sizeof(program->aliases.items[0]));
    }
    TypeAlias alias;
    memset(&alias, 0, sizeof(alias));
    alias.name = xstrdup(alias_name);
    alias.kind = TYPE_ALIAS_PTR;
    alias.target = xstrdup(target_name);
    alias.width = 4;
    program->aliases.items[program->aliases.count++] = alias;
    free(copy);
    return true;
}

static void write_struct_instance(Program *program, const StructDef *def, const char *item_text, int line) {
    size_t field_count = def->field_count;
    int64_t *values = xmalloc((field_count ? field_count : 1) * sizeof(values[0]));
    for (size_t i = 0; i < field_count; i++) {
        values[i] = def->fields[i].has_default ? def->fields[i].default_value : 0;
    }

    char *copy = xstrdup(item_text);
    char *item = trim_in_place(copy);
    if (!(ci_eq(item, "?") || ci_eq(item, "<>"))) {
        size_t len = strlen(item);
        if (len < 2 || item[0] != '<' || item[len - 1] != '>') {
            free(values);
            free(copy);
            parse_error2(line, "struct initializer must use <...> or <>", item_text);
        }
        item[len - 1] = '\0';
        char *inner = trim_in_place(item + 1);
        if (*inner) {
            char *parts[256];
            int count = 0;
            split_operands(inner, parts, &count, 256, line);
            if ((size_t)count > field_count) {
                for (int i = 0; i < count; i++) {
                    free(parts[i]);
                }
                free(values);
                free(copy);
                parse_error2(line, "too many values in struct initializer", item_text);
            }
            for (int i = 0; i < count; i++) {
                values[i] = eval_expr(NULL, program, parts[i], line);
                free(parts[i]);
            }
        }
    }

    for (size_t i = 0; i < field_count; i++) {
        const StructField *field = &def->fields[i];
        write_data(program, field->width, (uint64_t)values[i], line);
    }
    free(values);
    free(copy);
}

static size_t parse_struct_items(Program *program, const StructDef *def, const char *items_text, int line);

static size_t parse_one_struct_item(Program *program, const StructDef *def, const char *item_text, int line) {
    char *copy = xstrdup(item_text);
    char *item = trim_in_place(copy);
    if (*item == '\0') {
        free(copy);
        return 0;
    }
    char *dup_count = NULL;
    char *dup_inner = NULL;
    if (find_dup_form(item, &dup_count, &dup_inner)) {
        int64_t count_value = eval_expr(NULL, program, dup_count, line);
        if (count_value < 0 || count_value > 1000000) {
            free(dup_count);
            free(dup_inner);
            free(copy);
            parse_error(line, "invalid DUP count");
        }
        size_t total = 0;
        for (int64_t i = 0; i < count_value; i++) {
            total += parse_struct_items(program, def, dup_inner, line);
        }
        free(dup_count);
        free(dup_inner);
        free(copy);
        return total;
    }
    write_struct_instance(program, def, item, line);
    free(copy);
    return 1;
}

static size_t parse_struct_items(Program *program, const StructDef *def, const char *items_text, int line) {
    char *items[512];
    int count = 0;
    split_operands(items_text, items, &count, 512, line);
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        total += parse_one_struct_item(program, def, items[i], line);
        free(items[i]);
    }
    return total;
}

static void parse_data_line(Program *program, char *line, int line_no) {
    char *cursor = line;
    char *name = next_token(&cursor);
    if (!name) {
        return;
    }

    const StructDef *continuation_struct = NULL;
    int continuation_size = resolve_data_type_size(program, name, &continuation_struct);
    if (continuation_size) {
        if (!program->has_last_data || program->last_data_index >= program->symbols.count) {
            parse_error2(line_no, "unlabeled data declaration has no preceding symbol", name);
        }
        Symbol *last = &program->symbols.items[program->last_data_index];
        if (last->kind != SYM_DATA || last->elem_size != continuation_size) {
            parse_error2(line_no, "unlabeled data continuation must match the preceding data type", name);
        }
        char *items_text = trim_in_place(cursor);
        if (*items_text == '\0') {
            parse_error2(line_no, "data continuation requires an initializer", name);
        }
        size_t elem_count = continuation_struct
            ? parse_struct_items(program, continuation_struct, items_text, line_no)
            : parse_data_items(program, items_text, continuation_size, line_no);
        last->elem_count += elem_count;
        last->byte_size += elem_count * (size_t)continuation_size;
        return;
    }

    char *rest = trim_in_place(cursor);
    if (*rest == '=') {
        int64_t value = eval_expr(NULL, program, rest + 1, line_no);
        Symbol symbol = {xstrdup(name), SYM_CONST, 0, value, 0, 0, 0, 0, NULL};
        symbols_add(&program->symbols, symbol, line_no);
        return;
    }

    char *second_cursor = rest;
    char *type = next_token(&second_cursor);
    if (!type) {
        parse_error2(line_no, "expected data type after symbol", name);
    }

    if (ci_eq(type, "EQU")) {
        int64_t value = eval_expr(NULL, program, second_cursor, line_no);
        Symbol symbol = {xstrdup(name), SYM_CONST, 0, value, 0, 0, 0, 0, NULL};
        symbols_add(&program->symbols, symbol, line_no);
        return;
    }

    const StructDef *struct_type = NULL;
    int elem_size = resolve_data_type_size(program, type, &struct_type);
    if (!elem_size) {
        parse_error2(line_no, "unsupported data declaration type", type);
    }

    uint32_t address = program->data_next;
    char *items_text = trim_in_place(second_cursor);
    if (*items_text == '\0') {
        parse_error2(line_no, "data declaration requires an initializer", name);
    }
    size_t elem_count = struct_type
        ? parse_struct_items(program, struct_type, items_text, line_no)
        : parse_data_items(program, items_text, elem_size, line_no);
    Symbol symbol = {
        xstrdup(name),
        SYM_DATA,
        address,
        0,
        0,
        elem_size,
        elem_count,
        elem_count * (size_t)elem_size,
        xstrdup(type)
    };
    symbols_add(&program->symbols, symbol, line_no);
    program->last_data_index = program->symbols.count - 1;
    program->has_last_data = true;
}

static void add_code_label(Program *program, const char *name, int line) {
    Symbol symbol = {xstrdup(name), SYM_CODE, 0, 0, program->code.count, 0, 0, 0, NULL};
    symbols_add(&program->symbols, symbol, line);
}

static void control_frame_free(ControlFrame *frame) {
    free(frame->start_label);
    free(frame->next_label);
    free(frame->end_label);
    free(frame->continue_label);
}

static void clear_control_stack(void) {
    for (size_t i = 0; i < control_stack.count; i++) {
        control_frame_free(&control_stack.items[i]);
    }
    free(control_stack.items);
    memset(&control_stack, 0, sizeof(control_stack));
}

static void control_push(ControlFrame frame) {
    if (control_stack.count == control_stack.cap) {
        control_stack.cap = control_stack.cap ? control_stack.cap * 2 : 8;
        control_stack.items = xrealloc(control_stack.items,
                                       control_stack.cap * sizeof(control_stack.items[0]));
    }
    control_stack.items[control_stack.count++] = frame;
}

static ControlFrame *control_top(int line, const char *directive) {
    if (control_stack.count == 0) {
        parse_error2(line, "high-level directive without matching opener", directive);
    }
    return &control_stack.items[control_stack.count - 1];
}

static ControlFrame control_pop(int line, ControlKind expected, const char *directive) {
    ControlFrame *top = control_top(line, directive);
    if (top->kind != expected) {
        parse_error2(line, "high-level directive closes the wrong block", directive);
    }
    ControlFrame frame = *top;
    control_stack.count--;
    return frame;
}

static ControlFrame *nearest_loop_frame(void) {
    for (size_t i = control_stack.count; i > 0; i--) {
        ControlFrame *frame = &control_stack.items[i - 1];
        if (frame->kind == CTRL_WHILE || frame->kind == CTRL_REPEAT) {
            return frame;
        }
    }
    return NULL;
}

static void prototype_add_param(Prototype *prototype, const char *name, int width) {
    if (prototype->param_count == prototype->param_cap) {
        prototype->param_cap = prototype->param_cap ? prototype->param_cap * 2 : 4;
        prototype->params = xrealloc(prototype->params,
                                     prototype->param_cap * sizeof(prototype->params[0]));
    }
    PrototypeParam param;
    param.name = name && *name ? xstrdup(name) : NULL;
    param.width = width;
    prototype->params[prototype->param_count++] = param;
}

static Prototype *prototype_ensure(Program *program, const char *name, CallingConvention calling_conv,
                                   PrototypeTargetKind target_kind) {
    Prototype *prototype = prototypes_find(&program->prototypes, name);
    if (prototype) {
        return prototype;
    }
    if (program->prototypes.count == program->prototypes.cap) {
        program->prototypes.cap = program->prototypes.cap ? program->prototypes.cap * 2 : 16;
        program->prototypes.items = xrealloc(program->prototypes.items,
                                             program->prototypes.cap * sizeof(program->prototypes.items[0]));
    }
    Prototype fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.name = xstrdup(name);
    fresh.calling_conv = calling_conv;
    fresh.target_kind = target_kind;
    program->prototypes.items[program->prototypes.count] = fresh;
    return &program->prototypes.items[program->prototypes.count++];
}

static int param_type_width(const char *type, int line) {
    char *copy = xstrdup(type);
    char *text = trim_in_place(copy);
    int width = data_type_size(text);
    if (width == 0) {
        char *ptr = strrchr(text, ' ');
        if (ptr && ci_eq(trim_in_place(ptr + 1), "PTR")) {
            width = 4;
        }
    }
    if (width == 0) {
        parse_error2(line, "unsupported parameter type", type);
    }
    free(copy);
    return width;
}

static int param_slot_bytes(int width) {
    return width > 4 ? width : 4;
}

static void clear_prototype_params(Prototype *prototype) {
    for (size_t i = 0; i < prototype->param_count; i++) {
        free(prototype->params[i].name);
    }
    free(prototype->params);
    prototype->params = NULL;
    prototype->param_count = 0;
    prototype->param_cap = 0;
}

static void parse_signature_params(Prototype *prototype, const char *text, int line, bool require_names) {
    clear_prototype_params(prototype);
    char *work_copy = xstrdup(text ? text : "");
    char *work = trim_in_place(work_copy);
    if (*work == ',') {
        work = trim_in_place(work + 1);
    }
    char *items[64];
    int count = 0;
    split_operands(work, items, &count, 64, line);
    for (int i = 0; i < count; i++) {
        char *item = trim_in_place(items[i]);
        if (*item == '\0') {
            free(items[i]);
            continue;
        }
        if (i == 0 && !strchr(item, ':') &&
            (ci_eq(item, "STDCALL") || ci_eq(item, "C") || ci_eq(item, "SYSCALL"))) {
            prototype->calling_conv = CALLCONV_STDCALL;
            free(items[i]);
            continue;
        }
        char *colon = strchr(item, ':');
        char *name = NULL;
        char *type = item;
        if (colon) {
            *colon = '\0';
            name = trim_in_place(item);
            type = trim_in_place(colon + 1);
        } else if (require_names) {
            parse_error2(line, "PROC parameter requires a name and type", item);
        }
        if (!type || *type == '\0') {
            parse_error2(line, "parameter requires a type", item);
        }
        if (name && *name) {
            validate_local_name(name, line);
        } else if (require_names) {
            parse_error(line, "PROC parameter requires a name");
        }
        prototype_add_param(prototype, name, param_type_width(type, line));
        free(items[i]);
    }
    free(work_copy);
}

static void add_builtin_prototype(Program *program, const char *name, CallingConvention conv,
                                  const int *widths, size_t width_count) {
    Prototype *prototype = prototype_ensure(program, name, conv, PROTO_BUILTIN);
    if (prototype->param_count != 0) {
        return;
    }
    for (size_t i = 0; i < width_count; i++) {
        prototype_add_param(prototype, NULL, widths[i]);
    }
}

static void add_builtin_prototypes(Program *program) {
    size_t count = 0;
    const BuiltinSignature *sigs = builtin_signatures(&count);
    for (size_t i = 0; i < count; i++) {
        add_builtin_prototype(program, sigs[i].name, sigs[i].conv,
                              sigs[i].param_widths, sigs[i].argc);
    }
}

static void clear_current_proc(void) {
    free(current_proc_name);
    current_proc_name = NULL;
    current_proc_index = -1;
    current_proc_prologue_done = false;
    current_proc_saw_instruction = false;
    free(current_proc_last_op);
    current_proc_last_op = NULL;
}

static void begin_proc(Program *program, const char *name, const char *params, int line) {
    if (current_proc_name) {
        parse_error2(line, "nested PROC is not supported", name);
    }
    clear_current_proc();
    current_proc_name = xstrdup(name);
    Prototype *prototype = prototype_ensure(program, name, CALLCONV_STDCALL, PROTO_USER);
    char *params_copy = xstrdup(params ? params : "");
    char *params_trim = trim_in_place(params_copy);
    if (*params_trim) {
        parse_signature_params(prototype, params_trim, line, true);
    }
    free(params_copy);
    if (prototype->param_count > 0) {
        ProcLocals *locals = ensure_current_proc_locals(program, line);
        int32_t offset = 8;
        for (size_t i = 0; i < prototype->param_count; i++) {
            PrototypeParam *param = &prototype->params[i];
            if (!param->name || !*param->name) {
                parse_error2(line, "PROC parameter requires a name", prototype->name);
            }
            LocalVar local = {
                xstrdup(param->name),
                offset,
                param->width,
                1,
                true
            };
            locals_add_var(locals, local, line);
            int slot = param_slot_bytes(param->width);
            if (offset > INT32_MAX - slot) {
                parse_error(line, "PROC parameter frame too large");
            }
            offset += slot;
            locals->param_bytes += slot;
        }
    }
}

static ProcLocals *ensure_current_proc_locals(Program *program, int line) {
    if (!current_proc_name) {
        parse_error(line, "LOCAL outside PROC");
    }
    if (current_proc_index >= 0) {
        return &program->procs.items[current_proc_index];
    }
    if (program->procs.count == program->procs.cap) {
        program->procs.cap = program->procs.cap ? program->procs.cap * 2 : 8;
        program->procs.items = xrealloc(program->procs.items,
                                        program->procs.cap * sizeof(program->procs.items[0]));
    }
    ProcLocals locals;
    memset(&locals, 0, sizeof(locals));
    locals.proc_name = xstrdup(current_proc_name);
    program->procs.items[program->procs.count] = locals;
    current_proc_index = (int)program->procs.count;
    program->procs.count++;
    return &program->procs.items[current_proc_index];
}

static bool local_name_reserved(const char *name) {
    int64_t color = 0;
    RegisterRef reg;
    int width = 0;
    if (color_constant_value(name, &color) || parse_register(name, &reg) || is_size_prefix(name, &width)) {
        return true;
    }
    const char *reserved[] = {
        "$", "?", "OFFSET", "TYPE", "LENGTHOF", "SIZEOF", "PTR", "DUP", "PROTO",
        "PROC", "ENDP", "LOCAL", "EQU", "INCLUDE", "INCLUDELIB", "DB", "DW", "DD", "DQ"
    };
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        if (ci_eq(name, reserved[i])) {
            return true;
        }
    }
    return false;
}

static void validate_local_name(const char *name, int line) {
    if (!is_ident_start(*name) || local_name_reserved(name)) {
        parse_error2(line, "invalid LOCAL name", name);
    }
    for (const char *p = name + 1; *p; p++) {
        if (!is_ident_char(*p)) {
            parse_error2(line, "invalid LOCAL name", name);
        }
    }
}

static void locals_add_var(ProcLocals *locals, LocalVar local, int line) {
    if (locals_find(locals, local.name)) {
        parse_error2(line, "duplicate LOCAL", local.name);
    }
    if (locals->count == locals->cap) {
        locals->cap = locals->cap ? locals->cap * 2 : 8;
        locals->items = xrealloc(locals->items, locals->cap * sizeof(locals->items[0]));
    }
    locals->items[locals->count++] = local;
}

static void parse_one_local(Program *program, ProcLocals *locals, const char *text, int line) {
    char *copy = xstrdup(text);
    char *item = trim_in_place(copy);
    if (*item == '\0') {
        free(copy);
        parse_error(line, "empty LOCAL declarator");
    }

    char *type = NULL;
    char *colon = strchr(item, ':');
    if (colon) {
        *colon = '\0';
        type = trim_in_place(colon + 1);
    }

    int width = 4;
    if (type && *type) {
        if (!is_size_prefix(type, &width)) {
            parse_error2(line, "unsupported LOCAL type", type);
        }
    }

    size_t elem_count = 1;
    char *name = trim_in_place(item);
    char *bracket = strchr(name, '[');
    if (bracket) {
        char *end = strchr(bracket + 1, ']');
        if (!end || trim_in_place(end + 1)[0] != '\0') {
            parse_error2(line, "invalid LOCAL array declarator", text);
        }
        *bracket = '\0';
        *end = '\0';
        char *count_expr = trim_in_place(bracket + 1);
        int64_t count = eval_expr(NULL, program, count_expr, line);
        if (count <= 0 || count > INT_MAX) {
            parse_error2(line, "invalid LOCAL array count", count_expr);
        }
        elem_count = (size_t)count;
    }

    name = trim_in_place(name);
    validate_local_name(name, line);

    if (elem_count > (size_t)INT32_MAX / (size_t)width ||
        locals->frame_bytes > INT32_MAX - (int32_t)(elem_count * (size_t)width)) {
        parse_error(line, "LOCAL frame too large");
    }
    int32_t bytes = (int32_t)(elem_count * (size_t)width);
    LocalVar local = {
        xstrdup(name),
        -(locals->frame_bytes + bytes),
        width,
        elem_count,
        false
    };
    locals->frame_bytes += bytes;
    locals_add_var(locals, local, line);
    free(copy);
}

static void parse_local_directive(Program *program, const char *text, int line) {
    ProcLocals *locals = ensure_current_proc_locals(program, line);
    char *items[128];
    int count = 0;
    split_operands(text, items, &count, 128, line);
    if (count == 0) {
        parse_error(line, "LOCAL requires at least one declarator");
    }
    for (int i = 0; i < count; i++) {
        parse_one_local(program, locals, items[i], line);
        free(items[i]);
    }
}

static void emit_instruction(Program *program, const char *op, const char *operands_csv,
                             int line, int proc_index, const char *raw) {
    Instruction instruction;
    memset(&instruction, 0, sizeof(instruction));
    instruction.op = xstrdup(op);
    instruction.line = line;
    instruction.raw = xstrdup(raw);
    instruction.proc_index = proc_index;
    if (operands_csv && *operands_csv) {
        split_operands(operands_csv, instruction.operands, &instruction.operand_count, 4, line);
    }
    instructions_add(&program->code, instruction);
}

static void emit_proc_prologue_if_needed(Program *program, int line) {
    if (current_proc_index < 0 || current_proc_prologue_done) {
        return;
    }
    ProcLocals *locals = &program->procs.items[current_proc_index];
    emit_instruction(program, "push", "ebp", line, current_proc_index, "push ebp");
    emit_instruction(program, "mov", "ebp, esp", line, current_proc_index, "mov ebp, esp");
    if (locals->frame_bytes > 0) {
        char frame[32];
        snprintf(frame, sizeof(frame), "esp, %d", locals->frame_bytes);
        emit_instruction(program, "sub", frame, line, current_proc_index, "sub esp, LOCAL frame");
    }
    current_proc_prologue_done = true;
}

static void emit_proc_epilogue(Program *program, int line) {
    if (current_proc_index < 0) {
        return;
    }
    emit_instruction(program, "mov", "esp, ebp", line, current_proc_index, "mov esp, ebp");
    emit_instruction(program, "pop", "ebp", line, current_proc_index, "pop ebp");
}

static void remember_current_proc_op(const char *op) {
    if (!current_proc_name) {
        return;
    }
    free(current_proc_last_op);
    current_proc_last_op = xstrdup(op);
    current_proc_saw_instruction = true;
}

typedef struct {
    Program *program;
} ParserEmitContext;

static char *make_hll_label(void *ctx) {
    (void)ctx;
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "__masmrun_hll_%u", hll_label_counter++);
    return xstrdup(buffer);
}

static void hll_emit_instruction(void *ctx, const char *op, const char *operands_csv,
                                 int line, const char *raw) {
    ParserEmitContext *emit_ctx = ctx;
    emit_instruction(emit_ctx->program, op, operands_csv, line, current_proc_index, raw);
}

static void hll_emit_label(void *ctx, const char *label, int line) {
    ParserEmitContext *emit_ctx = ctx;
    add_code_label(emit_ctx->program, label, line);
}

static HllEmitter make_hll_emitter(Program *program) {
    ParserEmitContext *ctx = xmalloc(sizeof(*ctx));
    ctx->program = program;
    HllEmitter emitter = {
        ctx,
        hll_emit_instruction,
        hll_emit_label,
        make_hll_label
    };
    return emitter;
}

static void free_hll_emitter(HllEmitter *emitter) {
    free(emitter->ctx);
    emitter->ctx = NULL;
}

static void lower_hll_jump_if_false(Program *program, const char *expr,
                                    const char *target, int line) {
    HllEmitter emitter = make_hll_emitter(program);
    hll_lower_jump_if_false(&emitter, expr, target, line);
    free_hll_emitter(&emitter);
}

static bool parse_prototype_line(Program *program, char *line, int line_no) {
    char *copy = xstrdup(line);
    char *cursor = copy;
    char *name = next_token(&cursor);
    char *second = next_token(&cursor);
    bool is_proto = name && second && ci_eq(second, "PROTO");
    if (!is_proto) {
        free(copy);
        return false;
    }
    Prototype *prototype = prototype_ensure(program, name, CALLCONV_STDCALL, PROTO_USER);
    char *params = trim_in_place(cursor);
    if (*params) {
        parse_signature_params(prototype, params, line_no, false);
    }
    free(copy);
    return true;
}

static void lower_invoke(Program *program, const char *operands_text, int line, const char *raw) {
    char *operands[64];
    int operand_count = 0;
    split_operands(operands_text, operands, &operand_count, 64, line);
    if (operand_count < 1) {
        parse_error(line, "INVOKE requires a target");
    }
    const char *target = operands[0];
    int argc = operand_count - 1;
    Prototype *prototype = prototypes_find(&program->prototypes, target);
    Symbol *symbol = symbols_find(&program->symbols, target);
    if (!prototype && (!symbol || symbol->kind != SYM_CODE)) {
        parse_error2(line, "INVOKE target requires a prototype or known code label", target);
    }
    if (prototype) {
        if (prototype->calling_conv == CALLCONV_IRVINE_REG && argc > 0) {
            parse_error2(line, "Irvine32 routine uses registers, not INVOKE stack arguments", target);
        }
        if ((size_t)argc != prototype->param_count) {
            parse_error2(line, "wrong number of INVOKE arguments", target);
        }
    }
    for (int i = operand_count - 1; i >= 1; i--) {
        emit_instruction(program, "push", operands[i], line, current_proc_index, raw);
    }
    emit_instruction(program, "call", target, line, current_proc_index, raw);
    remember_current_proc_op("call");
    for (int i = 0; i < operand_count; i++) {
        free(operands[i]);
    }
}

static void handle_hll_directive(Program *program, const char *mnemonic,
                                 const char *text, int line, const char *raw) {
    char *expr = xstrdup(text ? text : "");
    char *trimmed = trim_in_place(expr);

    if (ci_eq(mnemonic, ".IF")) {
        emit_proc_prologue_if_needed(program, line);
        char *false_label = make_hll_label(NULL);
        char *end_label = make_hll_label(NULL);
        lower_hll_jump_if_false(program, trimmed, false_label, line);
        ControlFrame frame;
        memset(&frame, 0, sizeof(frame));
        frame.kind = CTRL_IF;
        frame.next_label = false_label;
        frame.end_label = end_label;
        control_push(frame);
        remember_current_proc_op(".if");
    } else if (ci_eq(mnemonic, ".ELSEIF")) {
        ControlFrame *frame = control_top(line, mnemonic);
        if (frame->kind != CTRL_IF || frame->saw_else) {
            parse_error2(line, ".ELSEIF without open IF block", mnemonic);
        }
        emit_instruction(program, "jmp", frame->end_label, line, current_proc_index, raw);
        add_code_label(program, frame->next_label, line);
        free(frame->next_label);
        frame->next_label = make_hll_label(NULL);
        lower_hll_jump_if_false(program, trimmed, frame->next_label, line);
        remember_current_proc_op(".elseif");
    } else if (ci_eq(mnemonic, ".ELSE")) {
        ControlFrame *frame = control_top(line, mnemonic);
        if (frame->kind != CTRL_IF || frame->saw_else) {
            parse_error2(line, ".ELSE without open IF block", mnemonic);
        }
        emit_instruction(program, "jmp", frame->end_label, line, current_proc_index, raw);
        add_code_label(program, frame->next_label, line);
        free(frame->next_label);
        frame->next_label = NULL;
        frame->saw_else = true;
        remember_current_proc_op(".else");
    } else if (ci_eq(mnemonic, ".ENDIF")) {
        ControlFrame frame = control_pop(line, CTRL_IF, mnemonic);
        if (frame.next_label) {
            add_code_label(program, frame.next_label, line);
        }
        add_code_label(program, frame.end_label, line);
        control_frame_free(&frame);
    } else if (ci_eq(mnemonic, ".WHILE")) {
        emit_proc_prologue_if_needed(program, line);
        char *start_label = make_hll_label(NULL);
        char *end_label = make_hll_label(NULL);
        add_code_label(program, start_label, line);
        lower_hll_jump_if_false(program, trimmed, end_label, line);
        ControlFrame frame;
        memset(&frame, 0, sizeof(frame));
        frame.kind = CTRL_WHILE;
        frame.start_label = start_label;
        frame.end_label = end_label;
        control_push(frame);
        remember_current_proc_op(".while");
    } else if (ci_eq(mnemonic, ".ENDW")) {
        ControlFrame frame = control_pop(line, CTRL_WHILE, mnemonic);
        emit_instruction(program, "jmp", frame.start_label, line, current_proc_index, raw);
        add_code_label(program, frame.end_label, line);
        control_frame_free(&frame);
        remember_current_proc_op(".endw");
    } else if (ci_eq(mnemonic, ".REPEAT")) {
        emit_proc_prologue_if_needed(program, line);
        char *start_label = make_hll_label(NULL);
        char *end_label = make_hll_label(NULL);
        char *continue_label = make_hll_label(NULL);
        add_code_label(program, start_label, line);
        ControlFrame frame;
        memset(&frame, 0, sizeof(frame));
        frame.kind = CTRL_REPEAT;
        frame.start_label = start_label;
        frame.end_label = end_label;
        frame.continue_label = continue_label;
        control_push(frame);
        remember_current_proc_op(".repeat");
    } else if (ci_eq(mnemonic, ".UNTIL")) {
        ControlFrame frame = control_pop(line, CTRL_REPEAT, mnemonic);
        add_code_label(program, frame.continue_label, line);
        lower_hll_jump_if_false(program, trimmed, frame.start_label, line);
        add_code_label(program, frame.end_label, line);
        control_frame_free(&frame);
        remember_current_proc_op(".until");
    } else if (ci_eq(mnemonic, ".BREAK")) {
        emit_proc_prologue_if_needed(program, line);
        ControlFrame *loop = nearest_loop_frame();
        if (!loop) {
            parse_error(line, ".BREAK outside loop");
        }
        emit_instruction(program, "jmp", loop->end_label, line, current_proc_index, raw);
        remember_current_proc_op(".break");
    } else if (ci_eq(mnemonic, ".CONTINUE")) {
        emit_proc_prologue_if_needed(program, line);
        ControlFrame *loop = nearest_loop_frame();
        if (!loop) {
            parse_error(line, ".CONTINUE outside loop");
        }
        const char *target = loop->kind == CTRL_REPEAT ? loop->continue_label : loop->start_label;
        emit_instruction(program, "jmp", target, line, current_proc_index, raw);
        remember_current_proc_op(".continue");
    } else {
        free(expr);
        parse_error2(line, "unsupported MASM directive", mnemonic);
    }

    free(expr);
}

static void parse_instruction_line(Program *program, char *line, int line_no) {
    char *original = xstrdup(line);
    char *cursor = line;
    char *mnemonic = next_token(&cursor);
    if (!mnemonic) {
        free(original);
        return;
    }

    if (ci_eq(mnemonic, "END")) {
        char *entry = next_token(&cursor);
        if (entry && *entry) {
            free(program->code.entry);
            program->code.entry = xstrdup(entry);
        }
        free(original);
        return;
    }

    char *maybe_proc_cursor = trim_in_place(cursor);
    char *proc_copy = xstrdup(maybe_proc_cursor);
    char *proc_cursor = proc_copy;
    char *second = next_token(&proc_cursor);
    if (second && ci_eq(second, "PROC")) {
        add_code_label(program, mnemonic, line_no);
        begin_proc(program, mnemonic, trim_in_place(proc_cursor), line_no);
        free(proc_copy);
        free(original);
        return;
    }
    if (second && ci_eq(second, "ENDP")) {
        if (current_proc_name && !ci_eq(current_proc_name, mnemonic)) {
            parse_error2(line_no, "ENDP does not match current PROC", mnemonic);
        }
        if (current_proc_index >= 0 &&
            (!current_proc_last_op || !ci_eq(current_proc_last_op, "ret"))) {
            parse_error(line_no, "PROC with LOCALs must end with explicit RET");
        }
        clear_current_proc();
        free(proc_copy);
        free(original);
        return;
    }
    free(proc_copy);

    if (ci_eq(mnemonic, "PROTO")) {
        free(original);
        return;
    }

    if (mnemonic[0] == '.') {
        handle_hll_directive(program, mnemonic, trim_in_place(cursor), line_no, original);
        free(original);
        return;
    }

    if (ci_eq(mnemonic, "LOCAL")) {
        if (!current_proc_name) {
            parse_error(line_no, "LOCAL outside PROC");
        }
        if (current_proc_saw_instruction || current_proc_prologue_done) {
            parse_error(line_no, "LOCAL must appear before other instructions");
        }
        parse_local_directive(program, trim_in_place(cursor), line_no);
        free(original);
        return;
    }

    char *prefix = NULL;
    if (ci_eq(mnemonic, "rep") || ci_eq(mnemonic, "repe") ||
        ci_eq(mnemonic, "repz") || ci_eq(mnemonic, "repne") ||
        ci_eq(mnemonic, "repnz")) {
        char *real = next_token(&cursor);
        if (!real) {
            parse_error(line_no, "rep prefix requires a string instruction");
        }
        const char *norm = ci_eq(mnemonic, "repz") ? "repe" :
                           ci_eq(mnemonic, "repnz") ? "repne" : mnemonic;
        prefix = xstrdup(norm);
        mnemonic = real;
    }

    emit_proc_prologue_if_needed(program, line_no);
    if (ci_eq(mnemonic, "invoke")) {
        lower_invoke(program, trim_in_place(cursor), line_no, original);
        free(original);
        return;
    }
    if (ci_eq(mnemonic, "ret")) {
        emit_proc_epilogue(program, line_no);
    }

    Instruction instruction;
    memset(&instruction, 0, sizeof(instruction));
    instruction.op = xstrdup(mnemonic);
    instruction.prefix = prefix;
    instruction.line = line_no;
    instruction.raw = original;
    instruction.proc_index = current_proc_index;
    char ret_cleanup[32];
    char *operands_text = trim_in_place(cursor);
    if (ci_eq(mnemonic, "ret") && *operands_text == '\0' &&
        current_proc_index >= 0 &&
        program->procs.items[current_proc_index].param_bytes > 0) {
        snprintf(ret_cleanup, sizeof(ret_cleanup), "%d",
                 program->procs.items[current_proc_index].param_bytes);
        operands_text = ret_cleanup;
    }
    if (*operands_text) {
        split_operands(operands_text, instruction.operands, &instruction.operand_count, 4, line_no);
    }
    instructions_add(&program->code, instruction);
    remember_current_proc_op(mnemonic);
}

static char *find_code_label_colon(char *line) {
    char *p = line;
    if (!is_ident_start(*p)) {
        return NULL;
    }
    while (is_ident_char(*p)) {
        p++;
    }
    if (*p == ':') {
        return p;
    }
    return NULL;
}

void parse_program(Program *program, const char *path) {
    clear_current_proc();
    clear_control_stack();
    hll_label_counter = 1;
    memset(program, 0, sizeof(*program));
    program->memory = xmalloc(MEMORY_SIZE);
    memset(program->memory, 0, MEMORY_SIZE);
    program->data_next = DATA_BASE;
    add_builtin_prototypes(program);

    source_clear_lines();
    source_set_path(path);
    size_t file_size = 0;
    char *text = read_text_file(path, &file_size);
    (void)file_size;
    const char *raw_line_start = text;
    for (const char *p = text; ; p++) {
        if (*p == '\n' || *p == '\0') {
            source_add_line(raw_line_start, (size_t)(p - raw_line_start));
            if (*p == '\0') {
                break;
            }
            raw_line_start = p + 1;
        }
    }
    char *expanded = macro_expand_source(path, text, NULL);
    free(text);
    text = expanded;
    source_clear_lines();
    source_set_path(path);

    enum {
        SECTION_NONE,
        SECTION_DATA,
        SECTION_CODE
    } section = SECTION_NONE;
    StructDef *active_struct = NULL;

    int line_no = 1;
    char *line_start = text;
    for (char *p = text; ; p++) {
        if (*p == '\n' || *p == '\0') {
            char saved = *p;
            source_add_line(line_start, (size_t)(p - line_start));
            *p = '\0';
            if (p > line_start && p[-1] == '\r') {
                p[-1] = '\0';
            }
            char *no_comment = strip_comment(line_start);
            char *line = trim_in_place(no_comment);

            if (*line != '\0') {
                if (active_struct) {
                    (void)parse_struct_directive(program, line, line_no, &active_struct);
                } else if (parse_prototype_line(program, line, line_no)) {
                    /* Prototype signatures are accepted before or within sections. */
                } else if (parse_typedef_line(program, line, line_no)) {
                    /* TYPEDEF aliases are accepted before or within data declarations. */
                } else if (parse_struct_directive(program, line, line_no, &active_struct)) {
                    if (section == SECTION_CODE) {
                        parse_error(line_no, "STRUCT blocks are not allowed in .code section");
                    }
                } else if (ci_eq(line, ".data") || ci_eq(line, ".data?") || ci_eq(line, ".const")) {
                    section = SECTION_DATA;
                } else if (ci_eq(line, ".code")) {
                    section = SECTION_CODE;
                } else if (is_ignored_directive(line) || ci_starts_with(line, "OPTION ")) {
                    /* Known top-level MASM setup directives are accepted and ignored. */
                } else if (ci_starts_with(line, "INCLUDE ")) {
                    char *include_name = trim_in_place(line + 8);
                    if (!ci_eq(include_name, "Irvine32.inc")) {
                        parse_error2(line_no, "only the built-in Irvine32.inc include is supported", include_name);
                    }
                } else if (ci_starts_with(line, "INCLUDELIB ")) {
                    char *lib_name = trim_in_place(line + 11);
                    if (!ci_eq(lib_name, "Irvine32.lib")) {
                        parse_error2(line_no, "only the built-in Irvine32.lib library is supported", lib_name);
                    }
                } else if (line[0] == '.' && section != SECTION_CODE) {
                    parse_error2(line_no, "unsupported MASM directive", line);
                } else if (section == SECTION_DATA) {
                    parse_data_line(program, line, line_no);
                } else if (section == SECTION_CODE) {
                    while (true) {
                        char *colon = find_code_label_colon(line);
                        if (!colon) {
                            break;
                        }
                        *colon = '\0';
                        char *label = trim_in_place(line);
                        add_code_label(program, label, line_no);
                        line = trim_in_place(colon + 1);
                        if (*line == '\0') {
                            break;
                        }
                    }
                    if (*line != '\0') {
                        parse_instruction_line(program, line, line_no);
                    }
                } else {
                    parse_error2(line_no, "statement appears before .data or .code", line);
                }
            }

            free(no_comment);
            if (saved == '\0') {
                break;
            }
            line_start = p + 1;
            line_no++;
        }
    }
    free(text);

    if (active_struct) {
        parse_error2(line_no, "missing ENDS for STRUCT", active_struct->name);
    }
    if (current_proc_name) {
        parse_error2(line_no, "missing ENDP for PROC", current_proc_name);
    }
    clear_current_proc();
    if (control_stack.count > 0) {
        parse_error(line_no, "missing closing high-level directive");
    }
    clear_control_stack();

    if (!program->code.entry) {
        program->code.entry = xstrdup("main");
    }
    Symbol *entry = symbols_find(&program->symbols, program->code.entry);
if (!entry || entry->kind != SYM_CODE) {
    parse_error2(line_no, "entry point not found", program->code.entry);
}
}
