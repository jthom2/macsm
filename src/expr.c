#include "masmrun.h"

int64_t eval_expr(Runtime *runtime, Program *program, const char *expr, int line);

typedef struct {
    Runtime *runtime;
    Program *program;
    const char *cursor;
    int line;
} ExprParser;

static void expr_skip_ws(ExprParser *parser) {
    while (isspace((unsigned char)*parser->cursor)) {
        parser->cursor++;
    }
}

bool color_constant_value(const char *name, int64_t *value_out) {
    struct {
        const char *name;
        int value;
    } colors[] = {
        {"black", 0}, {"blue", 1}, {"green", 2}, {"cyan", 3},
        {"red", 4}, {"magenta", 5}, {"brown", 6}, {"lightGray", 7},
        {"lightGrey", 7}, {"gray", 8}, {"grey", 8}, {"darkGray", 8},
        {"darkGrey", 8}, {"lightBlue", 9}, {"lightGreen", 10},
        {"lightCyan", 11}, {"lightRed", 12}, {"lightMagenta", 13},
        {"yellow", 14}, {"white", 15}
    };
    for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
        if (ci_eq(name, colors[i].name)) {
            *value_out = colors[i].value;
            return true;
        }
    }
    return false;
}

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
    char *base = xstrndup(text, (size_t)(dot - text));
    char *field = xstrdup(dot + 1);
    base = trim_in_place(base);
    field = trim_in_place(field);
    if (*base == '\0' || *field == '\0') {
        free(base);
        free(field);
        return false;
    }
    *base_out = xstrdup(base);
    *field_out = xstrdup(field);
    free(base);
    free(field);
    return true;
}

static StructDef *symbol_struct_def(Program *program, const Symbol *symbol) {
    if (!program || !symbol || symbol->kind != SYM_DATA || !symbol->type_name) {
        return NULL;
    }
    return structs_find(&program->structs, symbol->type_name);
}

static StructField *struct_find_field(StructDef *def, const char *field_name) {
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

static int64_t struct_field_expr_value(Runtime *runtime, Program *program,
                                       const char *base, const char *field_name,
                                       int line) {
    char *base_copy = xstrdup(base);
    char *trimmed = trim_in_place(base_copy);
    size_t len = strlen(trimmed);

    if (len >= 2 && trimmed[0] == '[' && trimmed[len - 1] == ']') {
        trimmed[len - 1] = '\0';
        char *inner = trim_in_place(trimmed + 1);
        StructField *field = NULL;
        size_t matches = 0;
        for (size_t i = 0; i < program->structs.count; i++) {
            StructField *candidate = struct_find_field(&program->structs.items[i], field_name);
            if (candidate) {
                matches++;
                if (!field) {
                    field = candidate;
                }
            }
        }
        if (matches == 0) {
            free(base_copy);
            parse_error2(line, "unknown struct field", field_name);
        }
        if (matches > 1) {
            free(base_copy);
            parse_error2(line, "ambiguous struct field for indirect access", field_name);
        }
        int64_t addr = eval_expr(runtime, program, inner, line);
        int64_t value = addr + (int64_t)field->offset;
        free(base_copy);
        return value;
    }

    char *bracket = strchr(trimmed, '[');
    char *name = trimmed;
    int64_t index_offset = 0;
    if (bracket) {
        char *end = strrchr(bracket + 1, ']');
        if (!end || trim_in_place(end + 1)[0] != '\0') {
            free(base_copy);
            parse_error2(line, "invalid indexed struct field expression", base);
        }
        *bracket = '\0';
        *end = '\0';
        name = trim_in_place(name);
        char *index_expr = trim_in_place(bracket + 1);
        index_offset = eval_expr(runtime, program, index_expr, line);
    }

    Symbol *symbol = symbols_find(&program->symbols, name);
    if (symbol && symbol->kind == SYM_DATA) {
        StructDef *def = symbol_struct_def(program, symbol);
        if (!def) {
            free(base_copy);
            parse_error2(line, "dot field base is not a struct data symbol", name);
        }
        StructField *field = struct_find_field(def, field_name);
        if (!field) {
            free(base_copy);
            parse_error2(line, "unknown struct field", field_name);
        }
        int64_t value = (int64_t)symbol->address + index_offset + (int64_t)field->offset;
        free(base_copy);
        return value;
    }

    StructDef *def = structs_find(&program->structs, name);
    if (def) {
        StructField *field = struct_find_field(def, field_name);
        if (!field) {
            free(base_copy);
            parse_error2(line, "unknown struct field", field_name);
        }
        int64_t value = index_offset + (int64_t)field->offset;
        free(base_copy);
        return value;
    }

    parse_error2(line, "unknown struct field base", name);
    free(base_copy);
    return 0;
}

static int64_t symbol_expr_value(Runtime *runtime, Program *program, const char *name, int line) {
    int64_t color = 0;
    if (color_constant_value(name, &color)) {
        return color;
    }

    char *field_base = NULL;
    char *field_name = NULL;
    if (split_field_reference(name, &field_base, &field_name)) {
        int64_t value = struct_field_expr_value(runtime, program, field_base, field_name, line);
        free(field_base);
        free(field_name);
        return value;
    }

    if (runtime) {
        if (runtime->current_proc_index >= 0 &&
            (size_t)runtime->current_proc_index < program->procs.count) {
            LocalVar *local = locals_find(&program->procs.items[runtime->current_proc_index], name);
            if (local) {
                return (int64_t)(runtime->cpu.regs[REG_EBP] + (uint32_t)local->offset);
            }
        }
        RegisterRef reg;
        if (parse_register(name, &reg)) {
            return (int64_t)reg_get(runtime, reg);
        }
    }

    Symbol *symbol = symbols_find(&program->symbols, name);
    if (symbol) {
        if (symbol->kind == SYM_CONST) {
            return symbol->value;
        }
        if (symbol->kind == SYM_DATA) {
            return symbol->address;
        }
        return (int64_t)symbol->instr_index;
    }

    parse_error2(line, "unknown symbol or expression factor", name);
    return 0;
}

static int64_t parse_expr_add_sub(ExprParser *parser);

static int64_t parse_expr_primary(ExprParser *parser) {
    expr_skip_ws(parser);
    if (*parser->cursor == '\0') {
        parse_error(parser->line, "empty expression factor");
    }

    if (*parser->cursor == '(') {
        parser->cursor++;
        int64_t value = parse_expr_add_sub(parser);
        expr_skip_ws(parser);
        if (*parser->cursor != ')') {
            parse_error(parser->line, "missing closing parenthesis in expression");
        }
        parser->cursor++;
        return value;
    }

    if (*parser->cursor == '$') {
        parser->cursor++;
        return parser->program ? (int64_t)parser->program->data_next : 0;
    }

    if (*parser->cursor == '\'' || *parser->cursor == '"') {
        char quote = *parser->cursor;
        const char *start = parser->cursor++;
        while (*parser->cursor && *parser->cursor != quote) {
            if (*parser->cursor == '\\' && parser->cursor[1]) {
                parser->cursor += 2;
            } else {
                parser->cursor++;
            }
        }
        if (*parser->cursor == quote) {
            parser->cursor++;
        }
        char *literal = xstrndup(start, (size_t)(parser->cursor - start));
        int64_t value = 0;
        bool ok = parse_char_literal(literal, &value);
        free(literal);
        if (!ok) {
            parse_error(parser->line, "invalid character literal in expression");
        }
        return value;
    }

    if (is_ident_start(*parser->cursor) || isdigit((unsigned char)*parser->cursor)) {
        const char *start = parser->cursor;
        while (*parser->cursor &&
               !isspace((unsigned char)*parser->cursor) &&
               *parser->cursor != '+' &&
               *parser->cursor != '-' &&
               *parser->cursor != '*' &&
               *parser->cursor != '/' &&
               *parser->cursor != '(' &&
               *parser->cursor != ')') {
            parser->cursor++;
        }
        char *token = xstrndup(start, (size_t)(parser->cursor - start));
        int64_t value = 0;
        if (parse_integer_literal(token, &value)) {
            free(token);
            return value;
        }
        value = symbol_expr_value(parser->runtime, parser->program, token, parser->line);
        free(token);
        return value;
    }

    parse_error2(parser->line, "unexpected character in expression", parser->cursor);
    return 0;
}

static int64_t parse_expr_unary(ExprParser *parser) {
    expr_skip_ws(parser);
    if (*parser->cursor == '+') {
        parser->cursor++;
        return parse_expr_unary(parser);
    }
    if (*parser->cursor == '-') {
        parser->cursor++;
        return -parse_expr_unary(parser);
    }
    return parse_expr_primary(parser);
}

static int64_t parse_expr_mul_div(ExprParser *parser) {
    int64_t result = parse_expr_unary(parser);
    while (true) {
        expr_skip_ws(parser);
        char op = *parser->cursor;
        if (op != '*' && op != '/') {
            break;
        }
        parser->cursor++;
        int64_t rhs = parse_expr_unary(parser);
        if (op == '*') {
            result *= rhs;
        } else {
            if (rhs == 0) {
                parse_error(parser->line, "division by zero in expression");
            }
            result /= rhs;
        }
    }
    return result;
}

static int64_t parse_expr_add_sub(ExprParser *parser) {
    int64_t result = parse_expr_mul_div(parser);
    while (true) {
        expr_skip_ws(parser);
        char op = *parser->cursor;
        if (op != '+' && op != '-') {
            break;
        }
        parser->cursor++;
        int64_t rhs = parse_expr_mul_div(parser);
        if (op == '+') {
            result += rhs;
        } else {
            result -= rhs;
        }
    }
    return result;
}

int64_t eval_expr(Runtime *runtime, Program *program, const char *expr, int line) {
    ExprParser parser = {runtime, program, expr, line};
    int64_t value = parse_expr_add_sub(&parser);
    expr_skip_ws(&parser);
    if (*parser.cursor != '\0') {
        parse_error2(line, "trailing text in expression", parser.cursor);
    }
    return value;
}
