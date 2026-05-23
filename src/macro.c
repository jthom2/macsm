#include "masmrun.h"

#define MACRO_MAX_DEPTH 32

typedef struct {
    char *name;
    char *default_value;
} MacroParam;

typedef struct {
    char *text;
    int line;
} MacroBodyLine;

typedef struct {
    char *name;
    MacroParam *params;
    size_t param_count;
    size_t param_cap;
    MacroBodyLine *body_lines;
    size_t body_count;
    size_t body_cap;
} Macro;

typedef struct {
    Macro *items;
    size_t count;
    size_t cap;
} MacroTable;

typedef struct {
    char *text;
    int line;
} SourceLine;

typedef struct {
    SourceLine *items;
    size_t count;
    size_t cap;
} SourceLineList;

typedef struct {
    char *name;
    char *value;
} NameValue;

typedef struct {
    NameValue *items;
    size_t count;
    size_t cap;
} NameValueMap;

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} StrBuf;

typedef struct {
    const char *path;
    DiagnosticList *findings;
    MacroTable macros;
    uint32_t call_id;
    const char *stack[MACRO_MAX_DEPTH + 1];
    int stack_depth;
} MacroExpandContext;

static void sb_reserve(StrBuf *sb, size_t extra) {
    size_t need = sb->len + extra + 1;
    if (need <= sb->cap) {
        return;
    }
    size_t next = sb->cap ? sb->cap : 128;
    while (next < need) {
        next *= 2;
    }
    sb->buf = xrealloc(sb->buf, next);
    sb->cap = next;
}

static void sb_append_n(StrBuf *sb, const char *text, size_t len) {
    sb_reserve(sb, len);
    memcpy(sb->buf + sb->len, text, len);
    sb->len += len;
    sb->buf[sb->len] = '\0';
}

static void sb_append(StrBuf *sb, const char *text) {
    sb_append_n(sb, text, strlen(text));
}

static void sb_append_char(StrBuf *sb, char c) {
    sb_reserve(sb, 1);
    sb->buf[sb->len++] = c;
    sb->buf[sb->len] = '\0';
}

static char *sb_take(StrBuf *sb) {
    if (!sb->buf) {
        return xstrdup("");
    }
    char *out = sb->buf;
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
    return out;
}

static void lines_push(SourceLineList *list, char *text, int line) {
    if (list->count == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 64;
        list->items = xrealloc(list->items, list->cap * sizeof(list->items[0]));
    }
    SourceLine item = {text, line};
    list->items[list->count++] = item;
}

static void lines_free(SourceLineList *list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].text);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static char *lines_join(const SourceLineList *list) {
    StrBuf sb = {0};
    for (size_t i = 0; i < list->count; i++) {
        if (i > 0) {
            sb_append_char(&sb, '\n');
        }
        sb_append(&sb, list->items[i].text);
    }
    return sb_take(&sb);
}

static void split_source_lines(const char *text, SourceLineList *out) {
    int line_no = 1;
    const char *line_start = text;
    for (const char *p = text; ; p++) {
        if (*p == '\n' || *p == '\0') {
            size_t len = (size_t)(p - line_start);
            if (len > 0 && line_start[len - 1] == '\r') {
                len--;
            }
            lines_push(out, xstrndup(line_start, len), line_no);
            if (*p == '\0') {
                break;
            }
            line_start = p + 1;
            line_no++;
        }
    }
}

static void map_put(NameValueMap *map, const char *name, const char *value) {
    for (size_t i = 0; i < map->count; i++) {
        if (ci_eq(map->items[i].name, name)) {
            free(map->items[i].value);
            map->items[i].value = xstrdup(value ? value : "");
            return;
        }
    }
    if (map->count == map->cap) {
        map->cap = map->cap ? map->cap * 2 : 8;
        map->items = xrealloc(map->items, map->cap * sizeof(map->items[0]));
    }
    map->items[map->count].name = xstrdup(name);
    map->items[map->count].value = xstrdup(value ? value : "");
    map->count++;
}

static const char *map_get(const NameValueMap *map, const char *name) {
    for (size_t i = 0; i < map->count; i++) {
        if (ci_eq(map->items[i].name, name)) {
            return map->items[i].value;
        }
    }
    return NULL;
}

static void map_free(NameValueMap *map) {
    for (size_t i = 0; i < map->count; i++) {
        free(map->items[i].name);
        free(map->items[i].value);
    }
    free(map->items);
    memset(map, 0, sizeof(*map));
}

static Macro *macro_find(MacroTable *table, const char *name) {
    for (size_t i = 0; i < table->count; i++) {
        if (ci_eq(table->items[i].name, name)) {
            return &table->items[i];
        }
    }
    return NULL;
}

static Macro *macro_add(MacroTable *table, const char *name) {
    if (table->count == table->cap) {
        table->cap = table->cap ? table->cap * 2 : 16;
        table->items = xrealloc(table->items, table->cap * sizeof(table->items[0]));
    }
    Macro macro;
    memset(&macro, 0, sizeof(macro));
    macro.name = xstrdup(name);
    table->items[table->count] = macro;
    return &table->items[table->count++];
}

static void macro_add_param(Macro *macro, const char *name, const char *default_value) {
    if (macro->param_count == macro->param_cap) {
        macro->param_cap = macro->param_cap ? macro->param_cap * 2 : 4;
        macro->params = xrealloc(macro->params, macro->param_cap * sizeof(macro->params[0]));
    }
    MacroParam param;
    param.name = xstrdup(name);
    param.default_value = default_value && *default_value ? xstrdup(default_value) : NULL;
    macro->params[macro->param_count++] = param;
}

static void macro_add_body_line(Macro *macro, const char *text, int line) {
    if (macro->body_count == macro->body_cap) {
        macro->body_cap = macro->body_cap ? macro->body_cap * 2 : 16;
        macro->body_lines = xrealloc(macro->body_lines, macro->body_cap * sizeof(macro->body_lines[0]));
    }
    MacroBodyLine body = {xstrdup(text), line};
    macro->body_lines[macro->body_count++] = body;
}

static void macro_table_free(MacroTable *table) {
    for (size_t i = 0; i < table->count; i++) {
        Macro *macro = &table->items[i];
        free(macro->name);
        for (size_t j = 0; j < macro->param_count; j++) {
            free(macro->params[j].name);
            free(macro->params[j].default_value);
        }
        free(macro->params);
        for (size_t j = 0; j < macro->body_count; j++) {
            free(macro->body_lines[j].text);
        }
        free(macro->body_lines);
    }
    free(table->items);
    memset(table, 0, sizeof(*table));
}

static char *macro_stack_hint(const MacroExpandContext *ctx) {
    if (ctx->stack_depth <= 0) {
        return NULL;
    }
    StrBuf sb = {0};
    sb_append(&sb, "expansion stack: ");
    for (int i = 0; i < ctx->stack_depth; i++) {
        if (i > 0) {
            sb_append(&sb, " -> ");
        }
        sb_append(&sb, ctx->stack[i]);
    }
    return sb_take(&sb);
}

static void macro_diag(MacroExpandContext *ctx, int line, const char *message, const char *detail) {
    if (!ctx->findings) {
        if (detail && *detail) {
            parse_error2(line, message, detail);
        } else {
            parse_error(line, message);
        }
    }
    char *hint = macro_stack_hint(ctx);
    parse_error_continue(ctx->findings, line, 1, "macro", message, detail, hint);
    free(hint);
}

static bool valid_macro_identifier(const char *name) {
    if (!name || !*name || !is_ident_start(*name)) {
        return false;
    }
    for (const char *p = name + 1; *p; p++) {
        if (!is_ident_char(*p)) {
            return false;
        }
    }
    return true;
}

static char *make_comment_placeholder(const char *line) {
    StrBuf sb = {0};
    sb_append_char(&sb, ';');
    if (line && *line) {
        sb_append_char(&sb, ' ');
        sb_append(&sb, line);
    }
    return sb_take(&sb);
}

static char *trim_dup(const char *text) {
    char *copy = xstrdup(text ? text : "");
    char *trimmed = trim_in_place(copy);
    char *out = xstrdup(trimmed);
    free(copy);
    return out;
}

static void parse_macro_params(MacroExpandContext *ctx, Macro *macro, const char *text, int line) {
    char *copy = xstrdup(text ? text : "");
    char *trimmed = trim_in_place(copy);
    if (*trimmed == ',') {
        trimmed = trim_in_place(trimmed + 1);
    }
    if (*trimmed == '\0') {
        free(copy);
        return;
    }
    char *items[128];
    int count = 0;
    split_operands(trimmed, items, &count, 128, line);
    for (int i = 0; i < count; i++) {
        char *item = trim_in_place(items[i]);
        if (*item == '\0') {
            free(items[i]);
            continue;
        }
        char *name = item;
        char *default_value = NULL;
        char *sep = strstr(item, ":=");
        if (sep) {
            *sep = '\0';
            default_value = trim_in_place(sep + 2);
        } else {
            sep = strchr(item, '=');
            if (sep) {
                *sep = '\0';
                default_value = trim_in_place(sep + 1);
            }
        }
        name = trim_in_place(name);
        if (!valid_macro_identifier(name)) {
            macro_diag(ctx, line, "invalid macro parameter name", name);
            free(items[i]);
            continue;
        }
        bool duplicate = false;
        for (size_t j = 0; j < macro->param_count; j++) {
            if (ci_eq(macro->params[j].name, name)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            macro_diag(ctx, line, "duplicate macro parameter", name);
            free(items[i]);
            continue;
        }
        macro_add_param(macro, name, default_value);
        free(items[i]);
    }
    free(copy);
}

static bool is_macro_header(const char *line, char **name_out, char **params_out) {
    char *no_comment = strip_comment(line);
    char *work = trim_in_place(no_comment);
    char *cursor = work;
    char *first = next_token(&cursor);
    char *second = next_token(&cursor);
    bool ok = first && second && ci_eq(second, "MACRO");
    if (ok) {
        *name_out = xstrdup(first);
        *params_out = trim_dup(cursor);
    } else {
        *name_out = NULL;
        *params_out = NULL;
    }
    free(no_comment);
    return ok;
}

static bool is_endm_line(const char *line) {
    char *no_comment = strip_comment(line);
    char *work = trim_in_place(no_comment);
    char *cursor = work;
    char *first = next_token(&cursor);
    char *second = next_token(&cursor);
    bool endm = (first && ci_eq(first, "ENDM")) || (second && ci_eq(second, "ENDM"));
    free(no_comment);
    return endm;
}

static void collect_macros(MacroExpandContext *ctx, const SourceLineList *input, SourceLineList *without_defs) {
    Macro *current = NULL;
    int current_line = 0;
    for (size_t i = 0; i < input->count; i++) {
        const SourceLine *line = &input->items[i];
        char *macro_name = NULL;
        char *params = NULL;
        bool header = is_macro_header(line->text, &macro_name, &params);
        bool endm = is_endm_line(line->text);
        if (!current) {
            if (header) {
                if (!valid_macro_identifier(macro_name)) {
                    macro_diag(ctx, line->line, "invalid macro name", macro_name);
                    free(macro_name);
                    free(params);
                    lines_push(without_defs, make_comment_placeholder(line->text), line->line);
                    continue;
                }
                if (macro_find(&ctx->macros, macro_name)) {
                    macro_diag(ctx, line->line, "duplicate macro definition", macro_name);
                    free(macro_name);
                    free(params);
                    lines_push(without_defs, make_comment_placeholder(line->text), line->line);
                    continue;
                }
                current = macro_add(&ctx->macros, macro_name);
                current_line = line->line;
                parse_macro_params(ctx, current, params, line->line);
                free(macro_name);
                free(params);
                lines_push(without_defs, make_comment_placeholder(line->text), line->line);
                continue;
            }
            lines_push(without_defs, xstrdup(line->text), line->line);
            free(macro_name);
            free(params);
            continue;
        }

        if (header) {
            macro_diag(ctx, line->line, "nested MACRO definitions are not supported", current->name);
        }
        if (endm) {
            lines_push(without_defs, make_comment_placeholder(line->text), line->line);
            current = NULL;
            current_line = 0;
            free(macro_name);
            free(params);
            continue;
        }
        macro_add_body_line(current, line->text, line->line);
        lines_push(without_defs, make_comment_placeholder(line->text), line->line);
        free(macro_name);
        free(params);
    }
    if (current) {
        macro_diag(ctx, current_line, "missing ENDM for MACRO", current->name);
    }
}

static char *sanitize_ident(const char *name) {
    StrBuf sb = {0};
    if (!name || !*name || !is_ident_start(name[0])) {
        sb_append_char(&sb, '_');
    }
    for (const char *p = name; *p; p++) {
        sb_append_char(&sb, is_ident_char(*p) ? *p : '_');
    }
    return sb_take(&sb);
}

static bool is_macro_local_line(const char *line) {
    char *no_comment = strip_comment(line);
    char *work = trim_in_place(no_comment);
    char *cursor = work;
    char *first = next_token(&cursor);
    bool is_local = first && ci_eq(first, "LOCAL");
    free(no_comment);
    return is_local;
}

static void capture_macro_locals(MacroExpandContext *ctx, const Macro *macro, const char *line,
                                 int call_line, uint32_t call_id, NameValueMap *locals) {
    char *no_comment = strip_comment(line);
    char *work = trim_in_place(no_comment);
    char *cursor = work;
    char *first = next_token(&cursor);
    if (!first || !ci_eq(first, "LOCAL")) {
        free(no_comment);
        return;
    }
    char *items[128];
    int count = 0;
    split_operands(cursor, items, &count, 128, call_line);
    char *macro_name = sanitize_ident(macro->name);
    for (int i = 0; i < count; i++) {
        char *name = trim_in_place(items[i]);
        if (!valid_macro_identifier(name)) {
            macro_diag(ctx, call_line, "invalid macro LOCAL name", name);
            free(items[i]);
            continue;
        }
        if (map_get(locals, name)) {
            macro_diag(ctx, call_line, "duplicate macro LOCAL name", name);
            free(items[i]);
            continue;
        }
        char *local_name = sanitize_ident(name);
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "__mac_%s_%u_%s", macro_name, call_id, local_name);
        map_put(locals, name, buffer);
        free(local_name);
        free(items[i]);
    }
    free(macro_name);
    free(no_comment);
}

static char *substitute_tokens(const char *line, const NameValueMap *params, const NameValueMap *locals) {
    StrBuf sb = {0};
    bool in_string = false;
    char quote = '\0';
    size_t i = 0;
    while (line[i]) {
        char c = line[i];
        if (!in_string && c == ';') {
            sb_append(&sb, line + i);
            break;
        }
        if (in_string) {
            sb_append_char(&sb, c);
            if (c == quote) {
                if (line[i + 1] == quote) {
                    sb_append_char(&sb, line[i + 1]);
                    i++;
                } else {
                    in_string = false;
                }
            }
            i++;
            continue;
        }
        if (c == '"' || c == '\'') {
            in_string = true;
            quote = c;
            sb_append_char(&sb, c);
            i++;
            continue;
        }
        if (c == '&' && is_ident_start(line[i + 1])) {
            size_t start = i + 1;
            size_t end = start;
            while (is_ident_char(line[end])) {
                end++;
            }
            char *name = xstrndup(line + start, end - start);
            const char *value = map_get(locals, name);
            if (!value) {
                value = map_get(params, name);
            }
            if (value) {
                sb_append(&sb, value);
            } else {
                sb_append_char(&sb, '&');
                sb_append_n(&sb, name, strlen(name));
            }
            free(name);
            i = end;
            continue;
        }
        if (is_ident_start(c)) {
            size_t start = i;
            size_t end = i + 1;
            while (is_ident_char(line[end])) {
                end++;
            }
            char *name = xstrndup(line + start, end - start);
            const char *value = map_get(locals, name);
            if (!value) {
                value = map_get(params, name);
            }
            if (value) {
                sb_append(&sb, value);
            } else {
                sb_append_n(&sb, line + start, end - start);
            }
            free(name);
            i = end;
            continue;
        }
        sb_append_char(&sb, c);
        i++;
    }
    return sb_take(&sb);
}

static char *append_provenance(const char *line, const char *macro_name,
                               const char *path, int call_line) {
    char line_no_buf[32];
    snprintf(line_no_buf, sizeof(line_no_buf), "%d", call_line);
    StrBuf sb = {0};
    sb_append(&sb, line);
    sb_append(&sb, " ; expanded from macro ");
    sb_append(&sb, macro_name);
    sb_append(&sb, " at ");
    sb_append(&sb, path ? path : "<input>");
    sb_append_char(&sb, ':');
    sb_append(&sb, line_no_buf);
    return sb_take(&sb);
}

static void expand_line(MacroExpandContext *ctx, SourceLineList *output, const char *line,
                        int line_no, int depth, const char *provenance_macro,
                        int provenance_line);

static void expand_macro_call(MacroExpandContext *ctx, SourceLineList *output, const Macro *macro,
                              const char *args_text, int call_line, int depth) {
    if (depth > MACRO_MAX_DEPTH) {
        macro_diag(ctx, call_line, "macro expansion depth exceeded", macro->name);
        return;
    }

    NameValueMap params = {0};
    NameValueMap locals = {0};
    char *args[128];
    int argc = 0;
    split_operands(args_text ? args_text : "", args, &argc, 128, call_line);
    if ((size_t)argc > macro->param_count) {
        macro_diag(ctx, call_line, "too many macro arguments", macro->name);
        for (int i = 0; i < argc; i++) {
            free(args[i]);
        }
        map_free(&params);
        map_free(&locals);
        return;
    }
    for (size_t i = 0; i < macro->param_count; i++) {
        const char *value = NULL;
        if ((int)i < argc) {
            value = args[i];
        } else if (macro->params[i].default_value) {
            value = macro->params[i].default_value;
        } else {
            macro_diag(ctx, call_line, "missing macro argument", macro->params[i].name);
            for (int j = 0; j < argc; j++) {
                free(args[j]);
            }
            map_free(&params);
            map_free(&locals);
            return;
        }
        map_put(&params, macro->params[i].name, value);
    }
    for (int i = 0; i < argc; i++) {
        free(args[i]);
    }

    uint32_t call_id = ++ctx->call_id;
    ctx->stack[ctx->stack_depth++] = macro->name;

    for (size_t i = 0; i < macro->body_count; i++) {
        const MacroBodyLine *body = &macro->body_lines[i];
        if (is_macro_local_line(body->text)) {
            capture_macro_locals(ctx, macro, body->text, call_line, call_id, &locals);
            continue;
        }
        char *substituted = substitute_tokens(body->text, &params, &locals);
        expand_line(ctx, output, substituted, body->line, depth, macro->name, call_line);
        free(substituted);
    }

    ctx->stack_depth--;
    map_free(&params);
    map_free(&locals);
}

static void expand_line(MacroExpandContext *ctx, SourceLineList *output, const char *line,
                        int line_no, int depth, const char *provenance_macro,
                        int provenance_line) {
    char *no_comment = strip_comment(line);
    char *work = trim_in_place(no_comment);
    char *cursor = work;
    char *first = next_token(&cursor);
    const Macro *macro = first ? macro_find(&ctx->macros, first) : NULL;
    if (!macro) {
        if (provenance_macro) {
            lines_push(output,
                       append_provenance(line, provenance_macro, ctx->path, provenance_line),
                       line_no);
        } else {
            lines_push(output, xstrdup(line), line_no);
        }
        free(no_comment);
        return;
    }

    lines_push(output, make_comment_placeholder(line), line_no);
    if (ctx->stack_depth >= MACRO_MAX_DEPTH) {
        macro_diag(ctx, line_no, "macro expansion depth exceeded", macro->name);
        free(no_comment);
        return;
    }
    char *args = trim_dup(cursor);
    expand_macro_call(ctx, output, macro, args, line_no, depth + 1);
    free(args);
    free(no_comment);
}

char *macro_expand_source(const char *path, const char *text, DiagnosticList *findings) {
    MacroExpandContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.path = path;
    ctx.findings = findings;

    SourceLineList input = {0};
    SourceLineList without_defs = {0};
    SourceLineList expanded = {0};
    split_source_lines(text, &input);
    collect_macros(&ctx, &input, &without_defs);
    for (size_t i = 0; i < without_defs.count; i++) {
        SourceLine *line = &without_defs.items[i];
        expand_line(&ctx, &expanded, line->text, line->line, 0, NULL, line->line);
    }
    char *out = lines_join(&expanded);
    lines_free(&expanded);
    lines_free(&without_defs);
    lines_free(&input);
    macro_table_free(&ctx.macros);
    return out;
}
