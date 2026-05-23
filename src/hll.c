#include "masmrun.h"

static char *trimmed_copy_range(const char *start, size_t len) {
    char *copy = xstrndup(start, len);
    char *trimmed = trim_in_place(copy);
    char *out = xstrdup(trimmed);
    free(copy);
    return out;
}

static char *trimmed_copy(const char *text) {
    return trimmed_copy_range(text, strlen(text));
}

static bool outer_parens_wrap(const char *s) {
    size_t len = strlen(s);
    if (len < 2 || s[0] != '(' || s[len - 1] != ')') {
        return false;
    }
    int depth = 0;
    bool in_string = false;
    char quote = '\0';
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (in_string) {
            if (c == quote) {
                in_string = false;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            in_string = true;
            quote = c;
            continue;
        }
        if (c == '(') {
            depth++;
        } else if (c == ')') {
            depth--;
            if (depth == 0 && i + 1 < len) {
                return false;
            }
        }
    }
    return depth == 0;
}

static char *normalized_condition(const char *expr, int line) {
    char *copy = trimmed_copy(expr);
    while (outer_parens_wrap(copy)) {
        size_t len = strlen(copy);
        char *inner = trimmed_copy_range(copy + 1, len - 2);
        free(copy);
        copy = inner;
    }
    if (*copy == '\0') {
        free(copy);
        parse_error(line, "empty high-level condition");
    }
    return copy;
}

static const char *find_top_level_token(const char *s, const char *token) {
    int paren_depth = 0;
    int bracket_depth = 0;
    bool in_string = false;
    char quote = '\0';
    size_t token_len = strlen(token);
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (in_string) {
            if (c == quote) {
                in_string = false;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            in_string = true;
            quote = c;
            continue;
        }
        if (c == '(') {
            paren_depth++;
        } else if (c == ')' && paren_depth > 0) {
            paren_depth--;
        } else if (c == '[') {
            bracket_depth++;
        } else if (c == ']' && bracket_depth > 0) {
            bracket_depth--;
        }
        if (paren_depth == 0 && bracket_depth == 0 &&
            strncmp(p, token, token_len) == 0) {
            return p;
        }
    }
    return NULL;
}

static const char *find_top_level_relop(const char *s, const char **rel_out) {
    int paren_depth = 0;
    int bracket_depth = 0;
    bool in_string = false;
    char quote = '\0';
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (in_string) {
            if (c == quote) {
                in_string = false;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            in_string = true;
            quote = c;
            continue;
        }
        if (c == '(') {
            paren_depth++;
            continue;
        }
        if (c == ')' && paren_depth > 0) {
            paren_depth--;
            continue;
        }
        if (c == '[') {
            bracket_depth++;
            continue;
        }
        if (c == ']' && bracket_depth > 0) {
            bracket_depth--;
            continue;
        }
        if (paren_depth != 0 || bracket_depth != 0) {
            continue;
        }
        if ((p[0] == '=' && p[1] == '=') ||
            (p[0] == '!' && p[1] == '=') ||
            (p[0] == '<' && p[1] == '=') ||
            (p[0] == '>' && p[1] == '=')) {
            *rel_out = p[0] == '=' ? "==" :
                       p[0] == '!' ? "!=" :
                       p[0] == '<' ? "<=" : ">=";
            return p;
        }
        if (p[0] == '<' || p[0] == '>') {
            *rel_out = p[0] == '<' ? "<" : ">";
            return p;
        }
    }
    return NULL;
}

static const char *jump_for_rel(const char *rel, bool jump_if_true) {
    if (ci_eq(rel, "==")) {
        return jump_if_true ? "je" : "jne";
    }
    if (ci_eq(rel, "!=")) {
        return jump_if_true ? "jne" : "je";
    }
    if (ci_eq(rel, "<")) {
        return jump_if_true ? "jl" : "jge";
    }
    if (ci_eq(rel, "<=")) {
        return jump_if_true ? "jle" : "jg";
    }
    if (ci_eq(rel, ">")) {
        return jump_if_true ? "jg" : "jle";
    }
    if (ci_eq(rel, ">=")) {
        return jump_if_true ? "jge" : "jl";
    }
    return jump_if_true ? "jne" : "je";
}

static void emit_cmp_jump(HllEmitter *emitter, const char *left, const char *right,
                          const char *jump_op, const char *target, int line,
                          const char *raw) {
    size_t operands_len = strlen(left) + strlen(right) + 3;
    char *operands = xmalloc(operands_len);
    snprintf(operands, operands_len, "%s, %s", left, right);
    emitter->emit_instruction(emitter->ctx, "cmp", operands, line, raw);
    emitter->emit_instruction(emitter->ctx, jump_op, target, line, raw);
    free(operands);
}

static void lower_condition(HllEmitter *emitter, const char *expr,
                            const char *target_label, int line,
                            bool jump_if_true) {
    char *condition = normalized_condition(expr, line);

    const char *split = find_top_level_token(condition, "||");
    if (split) {
        char *left = trimmed_copy_range(condition, (size_t)(split - condition));
        char *right = trimmed_copy(split + 2);
        if (jump_if_true) {
            lower_condition(emitter, left, target_label, line, true);
            lower_condition(emitter, right, target_label, line, true);
        } else {
            char *eval_right = emitter->make_label(emitter->ctx);
            char *done = emitter->make_label(emitter->ctx);
            lower_condition(emitter, left, eval_right, line, false);
            emitter->emit_instruction(emitter->ctx, "jmp", done, line, condition);
            emitter->emit_label(emitter->ctx, eval_right, line);
            lower_condition(emitter, right, target_label, line, false);
            emitter->emit_label(emitter->ctx, done, line);
            free(eval_right);
            free(done);
        }
        free(left);
        free(right);
        free(condition);
        return;
    }

    split = find_top_level_token(condition, "&&");
    if (split) {
        char *left = trimmed_copy_range(condition, (size_t)(split - condition));
        char *right = trimmed_copy(split + 2);
        if (jump_if_true) {
            char *eval_right = emitter->make_label(emitter->ctx);
            char *done = emitter->make_label(emitter->ctx);
            lower_condition(emitter, left, eval_right, line, true);
            emitter->emit_instruction(emitter->ctx, "jmp", done, line, condition);
            emitter->emit_label(emitter->ctx, eval_right, line);
            lower_condition(emitter, right, target_label, line, true);
            emitter->emit_label(emitter->ctx, done, line);
            free(eval_right);
            free(done);
        } else {
            lower_condition(emitter, left, target_label, line, false);
            lower_condition(emitter, right, target_label, line, false);
        }
        free(left);
        free(right);
        free(condition);
        return;
    }

    if (condition[0] == '!') {
        char *inner = trimmed_copy(condition + 1);
        lower_condition(emitter, inner, target_label, line, !jump_if_true);
        free(inner);
        free(condition);
        return;
    }

    const char *rel = NULL;
    const char *rel_pos = find_top_level_relop(condition, &rel);
    if (rel_pos) {
        size_t rel_len = strlen(rel);
        char *left = trimmed_copy_range(condition, (size_t)(rel_pos - condition));
        char *right = trimmed_copy(rel_pos + rel_len);
        if (*left == '\0' || *right == '\0') {
            parse_error2(line, "malformed high-level condition", condition);
        }
        emit_cmp_jump(emitter, left, right, jump_for_rel(rel, jump_if_true),
                      target_label, line, condition);
        free(left);
        free(right);
        free(condition);
        return;
    }

    emit_cmp_jump(emitter, condition, "0", jump_if_true ? "jne" : "je",
                  target_label, line, condition);
    free(condition);
}

void hll_lower_jump_if_false(HllEmitter *emitter, const char *expr,
                             const char *target_label, int line) {
    lower_condition(emitter, expr, target_label, line, false);
}

void hll_lower_jump_if_true(HllEmitter *emitter, const char *expr,
                            const char *target_label, int line) {
    lower_condition(emitter, expr, target_label, line, true);
}
