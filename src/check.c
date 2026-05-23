#include "masmrun.h"

typedef struct {
    char *name;
} NameItem;

typedef struct {
    NameItem *items;
    size_t count;
    size_t cap;
} NameSet;

typedef struct {
    char *name;
    size_t argc;
    bool register_driven;
} CheckPrototype;

typedef struct {
    CheckPrototype *items;
    size_t count;
    size_t cap;
} CheckPrototypeList;

typedef enum {
    CHECK_NONE,
    CHECK_DATA,
    CHECK_CODE
} CheckSection;

typedef enum {
    CHECK_CTRL_IF,
    CHECK_CTRL_WHILE,
    CHECK_CTRL_REPEAT
} CheckControlKind;

typedef struct {
    CheckControlKind kind;
    int line;
    bool saw_else;
} CheckControlFrame;

typedef struct {
    CheckControlFrame *items;
    size_t count;
    size_t cap;
} CheckControlStack;

static void findings_add(DiagnosticList *findings, int line, const char *category,
                         const char *feature, const char *reason);

static const char *const SUPPORTED_OPS[] = {
    "exit", "call", "invoke", "ret", "push", "pop", "mov", "movzx", "movsx",
    "lea", "xchg", "cmp", "test", "add", "adc", "sub", "and", "or", "xor",
    "inc", "dec", "neg", "not", "shl", "sal", "shr", "sar", "imul", "mul",
    "div", "idiv", "cdq", "cbw", "cwde", "cwd", "clc", "stc", "cld", "std",
    "daa", "loop", "mwrite", "pushad", "popad", "pushfd", "popfd",
    "sbb", "rol", "ror", "rcl", "rcr",
    "movsb", "stosb", "lodsb", "scasb", "cmpsb",
    "movsd", "stosd", "lodsd",
    "cmove", "cmovz", "cmovne", "cmovnz", "cmovl", "cmovnge", "cmovge", "cmovnl",
    "cmovg", "cmovnle", "cmovle", "cmovng", "cmovs", "cmovns",
    "cmovc", "cmovb", "cmovnc", "cmovae", "cmovnb",
    "cmova", "cmovnbe", "cmovbe", "cmovna", "cmovo", "cmovno",
    "jmp", "je", "jz", "jne", "jnz", "jc", "jnc", "jb", "jnae", "jae",
    "jnb", "ja", "jnbe", "jbe", "jna", "js", "jns", "jo", "jno", "jg",
    "jnle", "jge", "jnl", "jl", "jnge", "jle", "jng",
    "fld", "fst", "fstp", "fild", "fistp", "fist",
    "fadd", "fsub", "fsubr", "fmul", "fdiv", "fdivr",
    "faddp", "fsubp", "fmulp", "fdivp",
    "fcom", "fcomp", "fcompp",
    "fnstsw", "sahf",
    "fldz", "fld1", "fchs", "fabs"
};

static const char **builtin_names(size_t *count_out) {
    size_t count = 0;
    const BuiltinSignature *sigs = builtin_signatures(&count);
    static const char **cached = NULL;
    static size_t cached_count = 0;
    if (cached) {
        *count_out = cached_count;
        return cached;
    }
    const char **names = xmalloc(count * sizeof(names[0]));
    for (size_t i = 0; i < count; i++) {
        names[i] = sigs[i].name;
    }
    cached = names;
    cached_count = count;
    *count_out = count;
    return cached;
}

static const char *const SUPPORTED_DIRECTIVES[] = {
    ".386", ".model", ".stack", ".data", ".data?", ".const", ".code",
    "INCLUDE", "INCLUDELIB", "OPTION", "PROC", "ENDP", "END", "LOCAL", "PROTO",
    "MACRO", "ENDM",
    ".IF", ".ELSEIF", ".ELSE", ".ENDIF", ".WHILE", ".ENDW", ".REPEAT",
    ".UNTIL", ".BREAK", ".CONTINUE"
};

static const char *const RESERVED_WORDS[] = {
    "OFFSET", "TYPE", "LENGTHOF", "SIZEOF", "PTR", "DUP", "PROTO", "PROC", "ENDP",
    "LOCAL", "EQU", "INCLUDE", "INCLUDELIB", "BYTE", "WORD", "DWORD", "SDWORD", "SWORD",
    "QWORD", "DB", "DW", "DD", "DQ", "REAL4", "REAL8"
};

static void names_add(NameSet *set, const char *name) {
    if (!name || !*name) {
        return;
    }
    for (size_t i = 0; i < set->count; i++) {
        if (ci_eq(set->items[i].name, name)) {
            return;
        }
    }
    if (set->count == set->cap) {
        set->cap = set->cap ? set->cap * 2 : 32;
        set->items = xrealloc(set->items, set->cap * sizeof(set->items[0]));
    }
    set->items[set->count++].name = xstrdup(name);
}

static bool names_has(const NameSet *set, const char *name) {
    for (size_t i = 0; i < set->count; i++) {
        if (ci_eq(set->items[i].name, name)) {
            return true;
        }
    }
    return false;
}

static void names_free(NameSet *set) {
    for (size_t i = 0; i < set->count; i++) {
        free(set->items[i].name);
    }
    free(set->items);
}

static CheckPrototype *check_prototype_find(CheckPrototypeList *list, const char *name) {
    for (size_t i = 0; i < list->count; i++) {
        if (ci_eq(list->items[i].name, name)) {
            return &list->items[i];
        }
    }
    return NULL;
}

static void check_prototype_add(CheckPrototypeList *list, const char *name,
                                size_t argc, bool register_driven) {
    CheckPrototype *existing = check_prototype_find(list, name);
    if (existing) {
        existing->argc = argc;
        existing->register_driven = register_driven;
        return;
    }
    if (list->count == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 16;
        list->items = xrealloc(list->items, list->cap * sizeof(list->items[0]));
    }
    CheckPrototype prototype;
    prototype.name = xstrdup(name);
    prototype.argc = argc;
    prototype.register_driven = register_driven;
    list->items[list->count++] = prototype;
}

static void check_prototypes_add_builtins(CheckPrototypeList *list) {
    size_t count = 0;
    const BuiltinSignature *sigs = builtin_signatures(&count);
    for (size_t i = 0; i < count; i++) {
        check_prototype_add(list, sigs[i].name, sigs[i].argc,
                            sigs[i].conv == CALLCONV_IRVINE_REG);
    }
}

static void check_prototypes_free(CheckPrototypeList *list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].name);
    }
    free(list->items);
}

static size_t count_signature_params(const char *text, int line) {
    char *copy = xstrdup(text ? text : "");
    char *trimmed = trim_in_place(copy);
    if (*trimmed == ',') {
        trimmed = trim_in_place(trimmed + 1);
    }
    char *items[64];
    int count = 0;
    split_operands(trimmed, items, &count, 64, line);
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        char *item = trim_in_place(items[i]);
        if (i == 0 && !strchr(item, ':') &&
            (ci_eq(item, "STDCALL") || ci_eq(item, "C") || ci_eq(item, "SYSCALL"))) {
            free(items[i]);
            continue;
        }
        if (*item) {
            total++;
        }
        free(items[i]);
    }
    free(copy);
    return total;
}

static bool supported_param_type(const char *type) {
    char *copy = xstrdup(type);
    char *trimmed = trim_in_place(copy);
    bool ok = data_type_size(trimmed) > 0;
    if (!ok) {
        char *last = strrchr(trimmed, ' ');
        ok = last && ci_eq(trim_in_place(last + 1), "PTR");
    }
    free(copy);
    return ok;
}

static void check_signature_params(DiagnosticList *findings, int line, const char *text,
                                   bool require_names) {
    char *copy = xstrdup(text ? text : "");
    char *trimmed = trim_in_place(copy);
    if (*trimmed == ',') {
        trimmed = trim_in_place(trimmed + 1);
    }
    char *items[64];
    int count = 0;
    split_operands(trimmed, items, &count, 64, line);
    for (int i = 0; i < count; i++) {
        char *item = trim_in_place(items[i]);
        if (i == 0 && !strchr(item, ':') &&
            (ci_eq(item, "STDCALL") || ci_eq(item, "C") || ci_eq(item, "SYSCALL"))) {
            free(items[i]);
            continue;
        }
        if (*item == '\0') {
            free(items[i]);
            continue;
        }
        char *colon = strchr(item, ':');
        char *type = item;
        if (colon) {
            *colon = '\0';
            char *name = trim_in_place(item);
            if (require_names && *name == '\0') {
                findings_add(findings, line, "prototype", item, "PROC parameter requires a name");
            }
            type = trim_in_place(colon + 1);
        } else if (require_names) {
            findings_add(findings, line, "prototype", item, "PROC parameter requires name:TYPE");
        }
        if (*type == '\0') {
            findings_add(findings, line, "prototype", item, "parameter requires a type");
        } else if (!supported_param_type(type)) {
            findings_add(findings, line, "prototype", type, "unsupported parameter type");
        }
        free(items[i]);
    }
    free(copy);
}

static void findings_add_hint(DiagnosticList *findings, int line, const char *category,
                              const char *feature, const char *reason, const char *hint) {
    const char *shown_feature = feature && *feature ? feature : "<unknown>";
    diag_push_at(findings, DIAG_ERROR, line, source_find_column(line, shown_feature),
                 category, shown_feature, reason, hint);
}

static void findings_add(DiagnosticList *findings, int line, const char *category,
                         const char *feature, const char *reason) {
    findings_add_hint(findings, line, category, feature, reason, NULL);
}

static bool finding_seen(const DiagnosticList *findings, int line, const char *category, const char *feature) {
    for (size_t i = 0; i < findings->count; i++) {
        const Diagnostic *finding = &findings->items[i];
        if (finding->line == line &&
            ci_eq(finding->category, category) &&
            ci_eq(finding->message, feature)) {
            return true;
        }
    }
    return false;
}

static void findings_add_once_hint(DiagnosticList *findings, int line, const char *category,
                                   const char *feature, const char *reason, const char *hint) {
    if (!finding_seen(findings, line, category, feature)) {
        findings_add_hint(findings, line, category, feature, reason, hint);
    }
}

static void findings_add_once(DiagnosticList *findings, int line, const char *category,
                              const char *feature, const char *reason) {
    findings_add_once_hint(findings, line, category, feature, reason, NULL);
}

static bool check_read_file(const char *path, char **text_out) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "masmrun: cannot open %s: %s\n", path, strerror(errno));
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "masmrun: cannot seek %s\n", path);
        fclose(file);
        return false;
    }
    long size = ftell(file);
    if (size < 0) {
        fprintf(stderr, "masmrun: cannot size %s\n", path);
        fclose(file);
        return false;
    }
    rewind(file);
    char *buffer = xmalloc((size_t)size + 1);
    size_t got = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        fprintf(stderr, "masmrun: cannot read %s\n", path);
        free(buffer);
        return false;
    }
    buffer[got] = '\0';
    *text_out = buffer;
    return true;
}

static void load_source_lines(const char *path, const char *text) {
    source_clear_lines();
    source_set_path(path);
    const char *line_start = text;
    for (const char *p = text; ; p++) {
        if (*p == '\n' || *p == '\0') {
            source_add_line(line_start, (size_t)(p - line_start));
            if (*p == '\0') {
                break;
            }
            line_start = p + 1;
        }
    }
}

static bool supported_instruction(const char *op) {
    for (size_t i = 0; i < sizeof(SUPPORTED_OPS) / sizeof(SUPPORTED_OPS[0]); i++) {
        if (ci_eq(op, SUPPORTED_OPS[i])) {
            return true;
        }
    }
    return false;
}

static bool jump_instruction(const char *op) {
    return ci_eq(op, "loop") || ci_eq(op, "jmp") || ci_eq(op, "je") || ci_eq(op, "jz") ||
           ci_eq(op, "jne") || ci_eq(op, "jnz") || ci_eq(op, "jc") || ci_eq(op, "jnc") ||
           ci_eq(op, "jb") || ci_eq(op, "jnae") || ci_eq(op, "jae") || ci_eq(op, "jnb") ||
           ci_eq(op, "ja") || ci_eq(op, "jnbe") || ci_eq(op, "jbe") || ci_eq(op, "jna") ||
           ci_eq(op, "js") || ci_eq(op, "jns") || ci_eq(op, "jo") || ci_eq(op, "jno") ||
           ci_eq(op, "jg") || ci_eq(op, "jnle") || ci_eq(op, "jge") || ci_eq(op, "jnl") ||
           ci_eq(op, "jl") || ci_eq(op, "jnge") || ci_eq(op, "jle") || ci_eq(op, "jng");
}

static bool supported_builtin_call(const char *name) {
    size_t count = 0;
    const char **names = builtin_names(&count);
    for (size_t i = 0; i < count; i++) {
        if (ci_eq(name, names[i])) {
            return true;
        }
    }
    return false;
}

static bool reserved_identifier(const char *name) {
    int64_t color = 0;
    RegisterRef reg;
    int width = 0;
    if (ci_eq(name, "$") || ci_eq(name, "?")) {
        return true;
    }
    if (color_constant_value(name, &color) || parse_register(name, &reg) || is_size_prefix(name, &width)) {
        return true;
    }
    for (size_t i = 0; i < sizeof(RESERVED_WORDS) / sizeof(RESERVED_WORDS[0]); i++) {
        if (ci_eq(name, RESERVED_WORDS[i])) {
            return true;
        }
    }
    return false;
}

static char *suggest_from_names(const char *value, const NameSet *names) {
    if (!names || names->count == 0) {
        return NULL;
    }
    const char **candidates = xmalloc(names->count * sizeof(candidates[0]));
    for (size_t i = 0; i < names->count; i++) {
        candidates[i] = names->items[i].name;
    }
    char *hint = diag_suggest_near(value, candidates, names->count);
    free(candidates);
    return hint;
}

static char *suggest_from_known_identifiers(const char *value,
                                            const NameSet *data_names,
                                            const NameSet *const_names,
                                            const NameSet *code_names,
                                            const NameSet *local_names) {
    size_t builtin_count = 0;
    const char **builtin_list = builtin_names(&builtin_count);
    size_t static_count = builtin_count +
                          sizeof(RESERVED_WORDS) / sizeof(RESERVED_WORDS[0]) +
                          sizeof(SUPPORTED_OPS) / sizeof(SUPPORTED_OPS[0]);
    size_t dynamic_count = data_names->count + const_names->count + code_names->count + local_names->count;
    const char **candidates = xmalloc((static_count + dynamic_count) * sizeof(candidates[0]));
    size_t n = 0;
    for (size_t i = 0; i < data_names->count; i++) {
        candidates[n++] = data_names->items[i].name;
    }
    for (size_t i = 0; i < const_names->count; i++) {
        candidates[n++] = const_names->items[i].name;
    }
    for (size_t i = 0; i < code_names->count; i++) {
        candidates[n++] = code_names->items[i].name;
    }
    for (size_t i = 0; i < local_names->count; i++) {
        candidates[n++] = local_names->items[i].name;
    }
    for (size_t i = 0; i < builtin_count; i++) {
        candidates[n++] = builtin_list[i];
    }
    for (size_t i = 0; i < sizeof(RESERVED_WORDS) / sizeof(RESERVED_WORDS[0]); i++) {
        candidates[n++] = RESERVED_WORDS[i];
    }
    for (size_t i = 0; i < sizeof(SUPPORTED_OPS) / sizeof(SUPPORTED_OPS[0]); i++) {
        candidates[n++] = SUPPORTED_OPS[i];
    }
    char *hint = diag_suggest_near(value, candidates, n);
    free(candidates);
    return hint;
}

static char *suggest_call_target(const char *value, const NameSet *code_names) {
    size_t builtin_count = 0;
    const char **builtin_list = builtin_names(&builtin_count);
    size_t total = builtin_count + code_names->count;
    const char **candidates = xmalloc(total * sizeof(candidates[0]));
    size_t n = 0;
    for (size_t i = 0; i < builtin_count; i++) {
        candidates[n++] = builtin_list[i];
    }
    for (size_t i = 0; i < code_names->count; i++) {
        candidates[n++] = code_names->items[i].name;
    }
    char *hint = diag_suggest_near(value, candidates, n);
    free(candidates);
    return hint;
}

static char *without_strings(const char *text) {
    char *out = xstrdup(text);
    bool in_string = false;
    char quote = '\0';
    for (size_t i = 0; out[i]; i++) {
        if (in_string) {
            if (out[i] == quote) {
                in_string = false;
            }
            out[i] = ' ';
            continue;
        }
        if (out[i] == '"' || out[i] == '\'') {
            in_string = true;
            quote = out[i];
            out[i] = ' ';
        }
    }
    return out;
}

static NameSet g_check_type_names;
static NameSet g_check_field_names;

static void scan_identifiers(DiagnosticList *findings, int line, const char *category, const char *text,
                             const NameSet *data_names, const NameSet *const_names,
                             const NameSet *code_names, const NameSet *local_names) {
    char *clean = without_strings(text);
    for (char *p = clean; *p; p++) {
        if (!is_ident_start(*p)) {
            continue;
        }
        if (p > clean && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) {
            continue;
        }
        char *start = p;
        while (is_ident_char(*p)) {
            p++;
        }
        char saved = *p;
        *p = '\0';
        bool known = reserved_identifier(start) ||
                     names_has(data_names, start) ||
                     names_has(const_names, start) ||
                     names_has(code_names, start) ||
                     names_has(local_names, start) ||
                     names_has(&g_check_type_names, start) ||
                     names_has(&g_check_field_names, start) ||
                     supported_instruction(start) ||
                     supported_builtin_call(start);
        if (!known) {
            char *hint = suggest_from_known_identifiers(start, data_names, const_names, code_names, local_names);
            findings_add_once_hint(findings, line, category, start,
                                   "unresolved symbol or unsupported expression identifier", hint);
            free(hint);
        }
        *p = saved;
        if (!saved) {
            break;
        }
    }
    free(clean);
}

static void collect_local_names(NameSet *local_names, const char *text, int line) {
    char *items[128];
    int count = 0;
    split_operands(text, items, &count, 128, line);
    for (int i = 0; i < count; i++) {
        char *name = trim_in_place(items[i]);
        char *colon = strchr(name, ':');
        if (colon) {
            *colon = '\0';
        }
        char *bracket = strchr(name, '[');
        if (bracket) {
            *bracket = '\0';
        }
        name = trim_in_place(name);
        if (*name) {
            names_add(local_names, name);
        }
        free(items[i]);
    }
}

static char *check_code_label_colon(char *line) {
    char *p = line;
    if (!is_ident_start(*p)) {
        return NULL;
    }
    while (is_ident_char(*p)) {
        p++;
    }
    return *p == ':' ? p : NULL;
}

static void check_control_push(CheckControlStack *stack, CheckControlKind kind, int line) {
    if (stack->count == stack->cap) {
        stack->cap = stack->cap ? stack->cap * 2 : 8;
        stack->items = xrealloc(stack->items, stack->cap * sizeof(stack->items[0]));
    }
    CheckControlFrame frame = {kind, line, false};
    stack->items[stack->count++] = frame;
}

static CheckControlFrame *check_control_top(CheckControlStack *stack) {
    if (stack->count == 0) {
        return NULL;
    }
    return &stack->items[stack->count - 1];
}

static bool check_control_has_loop(const CheckControlStack *stack) {
    for (size_t i = stack->count; i > 0; i--) {
        CheckControlKind kind = stack->items[i - 1].kind;
        if (kind == CHECK_CTRL_WHILE || kind == CHECK_CTRL_REPEAT) {
            return true;
        }
    }
    return false;
}

static void check_control_free(CheckControlStack *stack) {
    free(stack->items);
}

static bool hll_directive_name(const char *op) {
    return ci_eq(op, ".IF") || ci_eq(op, ".ELSEIF") || ci_eq(op, ".ELSE") ||
           ci_eq(op, ".ENDIF") || ci_eq(op, ".WHILE") || ci_eq(op, ".ENDW") ||
           ci_eq(op, ".REPEAT") || ci_eq(op, ".UNTIL") ||
           ci_eq(op, ".BREAK") || ci_eq(op, ".CONTINUE");
}

static void collect_symbols(char *text, NameSet *data_names, NameSet *const_names,
                            NameSet *code_names, NameSet *local_names,
                            CheckPrototypeList *prototypes) {
    CheckSection section = CHECK_NONE;
    int line_no = 1;
    char *line_start = text;
    bool in_struct = false;
    for (char *p = text; ; p++) {
        if (*p == '\n' || *p == '\0') {
            char saved = *p;
            *p = '\0';
            if (p > line_start && p[-1] == '\r') {
                p[-1] = '\0';
            }
            char *no_comment = strip_comment(line_start);
            char *line = trim_in_place(no_comment);
            if (*line) {
                char *proto_copy = xstrdup(line);
                char *proto_cursor = proto_copy;
                char *proto_name = next_token(&proto_cursor);
                char *proto_second = next_token(&proto_cursor);
                if (proto_name && proto_second && ci_eq(proto_second, "PROTO")) {
                    size_t argc = count_signature_params(proto_cursor, line_no);
                    if (argc > 0 || !check_prototype_find(prototypes, proto_name)) {
                        check_prototype_add(prototypes, proto_name, argc, false);
                    }
                    free(proto_copy);
                    free(no_comment);
                    *p = saved;
                    if (saved == '\0') {
                        break;
                    }
                    line_start = p + 1;
                    line_no++;
                    continue;
                }
                free(proto_copy);

                char *type_copy = xstrdup(line);
                char *type_cursor = type_copy;
                char *type_first = next_token(&type_cursor);
                char *type_second = next_token(&type_cursor);
                bool consumed_type_line = false;
                if (in_struct) {
                    if (type_first && type_second && ci_eq(type_second, "ENDS")) {
                        in_struct = false;
                    } else if (type_first) {
                        names_add(&g_check_field_names, type_first);
                    }
                    consumed_type_line = true;
                } else if (type_first && type_second && ci_eq(type_second, "STRUCT")) {
                    names_add(&g_check_type_names, type_first);
                    in_struct = true;
                    consumed_type_line = true;
                } else if (type_first && type_second && ci_eq(type_second, "TYPEDEF")) {
                    names_add(&g_check_type_names, type_first);
                    consumed_type_line = true;
                }
                free(type_copy);
                if (consumed_type_line) {
                    free(no_comment);
                    *p = saved;
                    if (saved == '\0') {
                        break;
                    }
                    line_start = p + 1;
                    line_no++;
                    continue;
                }

                if (ci_eq(line, ".data") || ci_eq(line, ".data?") || ci_eq(line, ".const")) {
                    section = CHECK_DATA;
                } else if (ci_eq(line, ".code")) {
                    section = CHECK_CODE;
                } else if (section == CHECK_DATA) {
                    char *copy = xstrdup(line);
                    char *cursor = copy;
                    char *first = next_token(&cursor);
                    char *rest = trim_in_place(cursor);
                    if (first && data_type_size(first) == 0) {
                        if (*rest == '=') {
                            names_add(const_names, first);
                        } else {
                            char *rest_cursor = rest;
                            char *second = next_token(&rest_cursor);
                            if (second && ci_eq(second, "EQU")) {
                                names_add(const_names, first);
                            } else if (second && (data_type_size(second) > 0 ||
                                                   names_has(&g_check_type_names, second))) {
                                names_add(data_names, first);
                            }
                        }
                    }
                    free(copy);
                } else if (section == CHECK_CODE) {
                    char *copy = xstrdup(line);
                    char *work = trim_in_place(copy);
                    while (true) {
                        char *colon = check_code_label_colon(work);
                        if (!colon) {
                            break;
                        }
                        *colon = '\0';
                        char *label = trim_in_place(work);
                        if (*label) {
                            names_add(code_names, label);
                        }
                        work = trim_in_place(colon + 1);
                        if (!*work) {
                            break;
                        }
                    }
                    char *cursor = work;
                    char *first = next_token(&cursor);
                    if (first && ci_eq(first, "LOCAL")) {
                        collect_local_names(local_names, cursor, line_no);
                    } else {
                        char *second = next_token(&cursor);
                        if (first && second && ci_eq(second, "PROC")) {
                            names_add(code_names, first);
                            check_prototype_add(prototypes, first,
                                                count_signature_params(cursor, line_no), false);
                            collect_local_names(local_names, cursor, line_no);
                        }
                    }
                    free(copy);
                }
            }
            free(no_comment);
            *p = saved;
            if (saved == '\0') {
                break;
            }
            line_start = p + 1;
            line_no++;
        }
    }
    (void)line_no;
}

static void check_data_line(DiagnosticList *findings, int line_no, char *line,
                            const NameSet *data_names, const NameSet *const_names,
                            const NameSet *code_names, const NameSet *local_names) {
    char *cursor = line;
    char *first = next_token(&cursor);
    if (!first) {
        return;
    }
    if (data_type_size(first) > 0) {
        scan_identifiers(findings, line_no, "expression", cursor, data_names, const_names, code_names, local_names);
        return;
    }
    char *rest = trim_in_place(cursor);
    if (*rest == '=') {
        scan_identifiers(findings, line_no, "expression", rest + 1, data_names, const_names, code_names, local_names);
        return;
    }
    char *rest_cursor = rest;
    char *second = next_token(&rest_cursor);
    if (!second) {
        findings_add(findings, line_no, "data", first, "data statement is missing a type or initializer");
        return;
    }
    if (ci_eq(second, "EQU")) {
        scan_identifiers(findings, line_no, "expression", rest_cursor, data_names, const_names, code_names, local_names);
        return;
    }
    if (data_type_size(second) == 0 && !names_has(&g_check_type_names, second)) {
        findings_add(findings, line_no, "data", second, "unsupported data declaration type");
        return;
    }
    scan_identifiers(findings, line_no, "expression", rest_cursor, data_names, const_names, code_names, local_names);
}

static void check_code_line(DiagnosticList *findings, int line_no, char *line,
                            const NameSet *data_names, const NameSet *const_names,
                            const NameSet *code_names, const NameSet *local_names,
                            CheckPrototypeList *prototypes) {
    char *work = line;
    while (true) {
        char *colon = check_code_label_colon(work);
        if (!colon) {
            break;
        }
        *colon = '\0';
        work = trim_in_place(colon + 1);
        if (!*work) {
            return;
        }
    }

    char *cursor = work;
    char *op = next_token(&cursor);
    if (!op) {
        return;
    }
    char *maybe_proc = trim_in_place(cursor);
    char *proc_copy = xstrdup(maybe_proc);
    char *proc_cursor = proc_copy;
    char *second = next_token(&proc_cursor);
    if (second && (ci_eq(second, "PROC") || ci_eq(second, "ENDP"))) {
        if (ci_eq(second, "PROC")) {
            check_signature_params(findings, line_no, proc_cursor, true);
        }
        free(proc_copy);
        return;
    }
    free(proc_copy);
    if (ci_eq(op, "END") || ci_eq(op, "PROTO")) {
        return;
    }
    if (ci_eq(op, "LOCAL")) {
        return;
    }
    if (ci_eq(op, "rep") || ci_eq(op, "repe") || ci_eq(op, "repz") ||
        ci_eq(op, "repne") || ci_eq(op, "repnz")) {
        char *real = next_token(&cursor);
        if (!real) {
            findings_add(findings, line_no, "instruction", op, "rep prefix requires a string instruction");
            return;
        }
        const char *string_ops[] = {"movsb", "stosb", "lodsb", "scasb", "cmpsb",
                                    "movsd", "stosd", "lodsd"};
        bool ok = false;
        for (size_t i = 0; i < sizeof(string_ops) / sizeof(string_ops[0]); i++) {
            if (ci_eq(real, string_ops[i])) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            findings_add_once(findings, line_no, "instruction", real,
                              "rep prefix requires a supported string instruction");
            return;
        }
        op = real;
    }
    if (!supported_instruction(op)) {
        char *hint = diag_suggest_near(op, SUPPORTED_OPS, sizeof(SUPPORTED_OPS) / sizeof(SUPPORTED_OPS[0]));
        findings_add_hint(findings, line_no, "instruction", op, "unsupported instruction or macro", hint);
        free(hint);
        scan_identifiers(findings, line_no, "expression", cursor, data_names, const_names, code_names, local_names);
        return;
    }

    char *operands[16];
    int operand_count = 0;
    split_operands(cursor, operands, &operand_count, 16, line_no);
    if ((ci_eq(op, "call") || ci_eq(op, "invoke")) && operand_count >= 1) {
        CheckPrototype *prototype = check_prototype_find(prototypes, operands[0]);
        if (!supported_builtin_call(operands[0]) && !names_has(code_names, operands[0]) && !prototype) {
            char *hint = suggest_call_target(operands[0], code_names);
            findings_add_once_hint(findings, line_no, "call", operands[0],
                                   "unknown Irvine32 call or local procedure", hint);
            free(hint);
        } else if (ci_eq(op, "invoke")) {
            if (!prototype && !names_has(code_names, operands[0])) {
                findings_add_once(findings, line_no, "call", operands[0],
                                  "INVOKE target requires a prototype or known procedure");
            } else if (prototype) {
                size_t argc = (size_t)(operand_count - 1);
                if (prototype->register_driven && argc > 0) {
                    findings_add_once(findings, line_no, "call", operands[0],
                                      "Irvine32 routine uses registers, not INVOKE stack arguments");
                }
                if (argc != prototype->argc) {
                    findings_add_once(findings, line_no, "call", operands[0],
                                      "wrong INVOKE argument count");
                }
            }
        }
    } else if (jump_instruction(op) && operand_count >= 1) {
        if (!names_has(code_names, operands[0])) {
            char *hint = suggest_from_names(operands[0], code_names);
            findings_add_once_hint(findings, line_no, "label", operands[0], "unresolved jump target", hint);
            free(hint);
        }
    }
    for (int i = 0; i < operand_count; i++) {
        scan_identifiers(findings, line_no, "expression", operands[i], data_names, const_names, code_names, local_names);
        free(operands[i]);
    }
}

static const char *check_control_name(CheckControlKind kind) {
    switch (kind) {
        case CHECK_CTRL_IF: return ".IF";
        case CHECK_CTRL_WHILE: return ".WHILE";
        case CHECK_CTRL_REPEAT: return ".REPEAT";
    }
    return "<block>";
}

static void check_hll_line(DiagnosticList *findings, int line_no, char *line,
                           const NameSet *data_names, const NameSet *const_names,
                           const NameSet *code_names, const NameSet *local_names,
                           CheckControlStack *controls) {
    char *cursor = line;
    char *op = next_token(&cursor);
    char *expr = trim_in_place(cursor);
    if (!op || !hll_directive_name(op)) {
        return;
    }

    if (ci_eq(op, ".IF")) {
        scan_identifiers(findings, line_no, "expression", expr,
                         data_names, const_names, code_names, local_names);
        check_control_push(controls, CHECK_CTRL_IF, line_no);
    } else if (ci_eq(op, ".ELSEIF")) {
        CheckControlFrame *top = check_control_top(controls);
        if (!top || top->kind != CHECK_CTRL_IF || top->saw_else) {
            findings_add(findings, line_no, "directive", op, ".ELSEIF without matching .IF");
            return;
        }
        scan_identifiers(findings, line_no, "expression", expr,
                         data_names, const_names, code_names, local_names);
    } else if (ci_eq(op, ".ELSE")) {
        CheckControlFrame *top = check_control_top(controls);
        if (!top || top->kind != CHECK_CTRL_IF || top->saw_else) {
            findings_add(findings, line_no, "directive", op, ".ELSE without matching .IF");
            return;
        }
        top->saw_else = true;
    } else if (ci_eq(op, ".ENDIF")) {
        CheckControlFrame *top = check_control_top(controls);
        if (!top || top->kind != CHECK_CTRL_IF) {
            findings_add(findings, line_no, "directive", op, ".ENDIF without matching .IF");
            return;
        }
        controls->count--;
    } else if (ci_eq(op, ".WHILE")) {
        scan_identifiers(findings, line_no, "expression", expr,
                         data_names, const_names, code_names, local_names);
        check_control_push(controls, CHECK_CTRL_WHILE, line_no);
    } else if (ci_eq(op, ".ENDW")) {
        CheckControlFrame *top = check_control_top(controls);
        if (!top || top->kind != CHECK_CTRL_WHILE) {
            findings_add(findings, line_no, "directive", op, ".ENDW without matching .WHILE");
            return;
        }
        controls->count--;
    } else if (ci_eq(op, ".REPEAT")) {
        check_control_push(controls, CHECK_CTRL_REPEAT, line_no);
    } else if (ci_eq(op, ".UNTIL")) {
        CheckControlFrame *top = check_control_top(controls);
        if (!top || top->kind != CHECK_CTRL_REPEAT) {
            findings_add(findings, line_no, "directive", op, ".UNTIL without matching .REPEAT");
            return;
        }
        scan_identifiers(findings, line_no, "expression", expr,
                         data_names, const_names, code_names, local_names);
        controls->count--;
    } else if (ci_eq(op, ".BREAK") || ci_eq(op, ".CONTINUE")) {
        if (!check_control_has_loop(controls)) {
            findings_add(findings, line_no, "directive", op, "loop-control directive outside loop");
        }
    }
}

static void print_report(const char *path, const DiagnosticList *findings) {
    printf("Compatibility check: %s file=%s findings=%zu\n",
           findings->count == 0 ? "supported" : "unsupported",
           path,
           findings->count);
    if (findings->count == 0) {
        return;
    }
    diag_print_all(stdout, path, findings);
}

int check_file(const char *path) {
    char *text = NULL;
    if (!check_read_file(path, &text)) {
        return MASMRUN_EXIT_INTERNAL;
    }
    DiagnosticList findings = {0};
    char *expanded = macro_expand_source(path, text, &findings);
    free(text);
    text = expanded;
    load_source_lines(path, text);

    char *symbol_text = xstrdup(text);
    NameSet data_names = {0};
    NameSet const_names = {0};
    NameSet code_names = {0};
    NameSet local_names = {0};
    CheckPrototypeList prototypes = {0};
    check_prototypes_add_builtins(&prototypes);
    collect_symbols(symbol_text, &data_names, &const_names, &code_names, &local_names, &prototypes);
    free(symbol_text);

    CheckSection section = CHECK_NONE;
    CheckControlStack controls = {0};
    bool in_struct_block = false;
    int line_no = 1;
    char *line_start = text;
    for (char *p = text; ; p++) {
        if (*p == '\n' || *p == '\0') {
            char saved = *p;
            *p = '\0';
            if (p > line_start && p[-1] == '\r') {
                p[-1] = '\0';
            }
            char *no_comment = strip_comment(line_start);
            char *line = trim_in_place(no_comment);
            if (*line) {
                char *proto_copy = xstrdup(line);
                char *proto_cursor = proto_copy;
                char *proto_name = next_token(&proto_cursor);
                char *proto_second = next_token(&proto_cursor);
                bool is_proto_line = proto_name && proto_second && ci_eq(proto_second, "PROTO");

                if (is_proto_line) {
                    check_signature_params(&findings, line_no, proto_cursor, false);
                    free(proto_copy);
                } else if (ci_eq(line, ".data") || ci_eq(line, ".data?") || ci_eq(line, ".const")) {
                    free(proto_copy);
                    section = CHECK_DATA;
                } else if (ci_eq(line, ".code")) {
                    free(proto_copy);
                    section = CHECK_CODE;
                } else if (is_ignored_directive(line) || ci_starts_with(line, "OPTION ")) {
                    free(proto_copy);
                    /* supported setup directive */
                } else if (ci_starts_with(line, "INCLUDE ")) {
                    free(proto_copy);
                    char *include_name = trim_in_place(line + 8);
                    if (!ci_eq(include_name, "Irvine32.inc")) {
                        const char *inc_candidates[] = {"Irvine32.inc"};
                        char *hint = diag_suggest_near(include_name, inc_candidates,
                                                        sizeof(inc_candidates) / sizeof(inc_candidates[0]));
                        findings_add_hint(&findings, line_no, "include", include_name,
                                          "only built-in Irvine32.inc is supported", hint);
                        free(hint);
                    }
                } else if (ci_starts_with(line, "INCLUDELIB ")) {
                    free(proto_copy);
                    char *lib_name = trim_in_place(line + 11);
                    if (!ci_eq(lib_name, "Irvine32.lib")) {
                        const char *lib_candidates[] = {"Irvine32.lib"};
                        char *hint = diag_suggest_near(lib_name, lib_candidates,
                                                        sizeof(lib_candidates) / sizeof(lib_candidates[0]));
                        findings_add_hint(&findings, line_no, "include", lib_name,
                                          "only built-in Irvine32.lib is supported", hint);
                        free(hint);
                    }
                } else if (section == CHECK_CODE && line[0] == '.') {
                    free(proto_copy);
                    if (hll_directive_name(line) || ci_starts_with(line, ".IF ") ||
                        ci_starts_with(line, ".ELSEIF ") || ci_starts_with(line, ".WHILE ") ||
                        ci_starts_with(line, ".UNTIL ")) {
                        check_hll_line(&findings, line_no, line, &data_names, &const_names,
                                       &code_names, &local_names, &controls);
                    } else {
                        char *hint = diag_suggest_near(line, SUPPORTED_DIRECTIVES,
                                                       sizeof(SUPPORTED_DIRECTIVES) / sizeof(SUPPORTED_DIRECTIVES[0]));
                        findings_add_hint(&findings, line_no, "directive", line, "unsupported MASM directive", hint);
                        free(hint);
                    }
                } else if (line[0] == '.') {
                    free(proto_copy);
                    char *hint = diag_suggest_near(line, SUPPORTED_DIRECTIVES,
                                                   sizeof(SUPPORTED_DIRECTIVES) / sizeof(SUPPORTED_DIRECTIVES[0]));
                    findings_add_hint(&findings, line_no, "directive", line, "unsupported MASM directive", hint);
                    free(hint);
                } else if (in_struct_block) {
                    free(proto_copy);
                    char *sc = xstrdup(line);
                    char *scur = sc;
                    char *t1 = next_token(&scur);
                    char *t2 = next_token(&scur);
                    if (t1 && t2 && ci_eq(t2, "ENDS")) {
                        in_struct_block = false;
                    }
                    free(sc);
                } else if (proto_name && proto_second && ci_eq(proto_second, "STRUCT")) {
                    free(proto_copy);
                    in_struct_block = true;
                } else if (proto_name && proto_second && ci_eq(proto_second, "TYPEDEF")) {
                    free(proto_copy);
                    /* TYPEDEF accepted; collect_symbols already registered the alias */
                } else if (ci_starts_with(line, "MACRO") || strstr(line, " MACRO") ||
                           ci_starts_with(line, "ENDM") || strstr(line, " ENDM")) {
                    free(proto_copy);
                    findings_add(&findings, line_no, "macro", line, "unsupported macro syntax");
                } else if (section == CHECK_DATA) {
                    free(proto_copy);
                    check_data_line(&findings, line_no, line, &data_names, &const_names, &code_names, &local_names);
                } else if (section == CHECK_CODE) {
                    free(proto_copy);
                    check_code_line(&findings, line_no, line, &data_names, &const_names,
                                    &code_names, &local_names, &prototypes);
                } else {
                    free(proto_copy);
                    findings_add(&findings, line_no, "layout", line, "statement appears before .data or .code");
                }
            }
            free(no_comment);
            *p = saved;
            if (saved == '\0') {
                break;
            }
            line_start = p + 1;
            line_no++;
        }
    }

    for (size_t i = 0; i < controls.count; i++) {
        const char *name = check_control_name(controls.items[i].kind);
        findings_add(&findings, controls.items[i].line, "directive", name,
                     "missing matching high-level block terminator");
    }

    print_report(path, &findings);
    int rc = findings.count == 0 ? 0 : MASMRUN_EXIT_UNSUPPORTED;
    diag_free(&findings);
    names_free(&data_names);
    names_free(&const_names);
    names_free(&code_names);
    names_free(&local_names);
    names_free(&g_check_type_names);
    names_free(&g_check_field_names);
    memset(&g_check_type_names, 0, sizeof(g_check_type_names));
    memset(&g_check_field_names, 0, sizeof(g_check_field_names));
    check_prototypes_free(&prototypes);
    check_control_free(&controls);
    free(text);
    return rc;
}
