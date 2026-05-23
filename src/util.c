#include "masmrun.h"

void *xmalloc(size_t size) {
    void *ptr = malloc(size == 0 ? 1 : size);
    if (!ptr) {
        fprintf(stderr, "masmrun: out of memory\n");
        exit(2);
    }
    return ptr;
}

void *xrealloc(void *ptr, size_t size) {
    void *next = realloc(ptr, size == 0 ? 1 : size);
    if (!next) {
        fprintf(stderr, "masmrun: out of memory\n");
        exit(2);
    }
    return next;
}

char *xstrdup(const char *src) {
    size_t len = strlen(src);
    char *copy = xmalloc(len + 1);
    memcpy(copy, src, len + 1);
    return copy;
}

char *xstrndup(const char *src, size_t len) {
    char *copy = xmalloc(len + 1);
    memcpy(copy, src, len);
    copy[len] = '\0';
    return copy;
}

int ci_cmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) {
            return ca - cb;
        }
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

bool ci_eq(const char *a, const char *b) {
    return ci_cmp(a, b) == 0;
}

bool ci_starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) {
            return false;
        }
        s++;
        prefix++;
    }
    return true;
}

char *trim_in_place(char *s) {
    while (isspace((unsigned char)*s)) {
        s++;
    }
    if (*s == '\0') {
        return s;
    }
    char *end = s + strlen(s) - 1;
    while (end >= s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return s;
}

bool is_ident_start(char c) {
    return isalpha((unsigned char)c) || c == '_' || c == '@' || c == '$' || c == '?';
}

bool is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '@' || c == '$' || c == '?';
}

char *strip_comment(const char *line) {
    bool in_string = false;
    char quote = '\0';
    size_t len = strlen(line);
    char *out = xmalloc(len + 1);
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        char c = line[i];
        if (in_string) {
            out[j++] = c;
            if (c == quote) {
                if (i + 1 < len && line[i + 1] == quote) {
                    out[j++] = line[++i];
                } else {
                    in_string = false;
                }
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            in_string = true;
            quote = c;
            out[j++] = c;
            continue;
        }
        if (c == ';') {
            break;
        }
        out[j++] = c;
    }
    out[j] = '\0';
    return out;
}

char *next_token(char **cursor) {
    char *s = *cursor;
    while (isspace((unsigned char)*s)) {
        s++;
    }
    if (*s == '\0') {
        *cursor = s;
        return NULL;
    }
    char *start = s;
    while (*s && !isspace((unsigned char)*s) && *s != ',') {
        s++;
    }
    if (*s) {
        *s++ = '\0';
    }
    *cursor = s;
    return start;
}

char *read_text_file(const char *path, size_t *size_out) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "masmrun: cannot open %s: %s\n", path, strerror(errno));
        exit(1);
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "masmrun: cannot seek %s\n", path);
        fclose(file);
        exit(1);
    }
    long size = ftell(file);
    if (size < 0) {
        fprintf(stderr, "masmrun: cannot size %s\n", path);
        fclose(file);
        exit(1);
    }
    rewind(file);
    char *buffer = xmalloc((size_t)size + 1);
    size_t got = fread(buffer, 1, (size_t)size, file);
    if (got != (size_t)size) {
        fprintf(stderr, "masmrun: cannot read %s\n", path);
        fclose(file);
        exit(1);
    }
    buffer[got] = '\0';
    fclose(file);
    if (size_out) {
        *size_out = got;
    }
    return buffer;
}

bool parse_integer_literal(const char *text, int64_t *value_out) {
    char *copy = xstrdup(text);
    char *s = trim_in_place(copy);
    if (*s == '\0') {
        free(copy);
        return false;
    }

    bool negative = false;
    if (*s == '-' || *s == '+') {
        negative = *s == '-';
        s++;
    }

    int base = 10;
    size_t len = strlen(s);
    if (len >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    } else if (len > 1) {
        char suffix = s[len - 1];
        if (suffix == 'h' || suffix == 'H') {
            base = 16;
            s[len - 1] = '\0';
        } else if (suffix == 'b' || suffix == 'B') {
            base = 2;
            s[len - 1] = '\0';
        } else if (suffix == 'd' || suffix == 'D') {
            base = 10;
            s[len - 1] = '\0';
        }
    }

    if (*s == '\0') {
        free(copy);
        return false;
    }
    errno = 0;
    char *end = NULL;
    long long parsed = strtoll(s, &end, base);
    if (errno != 0 || end == s || *trim_in_place(end) != '\0') {
        free(copy);
        return false;
    }
    *value_out = negative ? -parsed : parsed;
    free(copy);
    return true;
}

bool parse_char_literal(const char *text, int64_t *value_out) {
    char *copy = xstrdup(text);
    char *s = trim_in_place(copy);
    size_t len = strlen(s);
    if (len >= 2 && ((s[0] == '\'' && s[len - 1] == '\'') || (s[0] == '"' && s[len - 1] == '"'))) {
        if (len == 2) {
            *value_out = 0;
        } else if (s[1] == '\\' && len >= 4) {
            switch (s[2]) {
                case 'n': *value_out = '\n'; break;
                case 'r': *value_out = '\r'; break;
                case 't': *value_out = '\t'; break;
                case '0': *value_out = '\0'; break;
                default: *value_out = (unsigned char)s[2]; break;
            }
        } else {
            *value_out = (unsigned char)s[1];
        }
        free(copy);
        return true;
    }
    free(copy);
    return false;
}

void split_operands(const char *text, char **out, int *count_out, int max_count, int line) {
    *count_out = 0;
    const char *start = text;
    bool in_string = false;
    char quote = '\0';
    int paren_depth = 0;
    int bracket_depth = 0;
    int angle_depth = 0;
    for (const char *p = text; ; p++) {
        char c = *p;
        if (in_string) {
            if (c == quote) {
                if (p[1] == quote) {
                    p++;
                } else {
                    in_string = false;
                }
            }
        } else {
            if (c == '"' || c == '\'') {
                in_string = true;
                quote = c;
            } else if (c == '(') {
                paren_depth++;
            } else if (c == ')') {
                if (paren_depth > 0) {
                    paren_depth--;
                }
            } else if (c == '[') {
                bracket_depth++;
            } else if (c == ']') {
                if (bracket_depth > 0) {
                    bracket_depth--;
                }
            } else if (c == '<') {
                angle_depth++;
            } else if (c == '>') {
                if (angle_depth > 0) {
                    angle_depth--;
                }
            }
        }

        if ((c == ',' && !in_string && paren_depth == 0 && bracket_depth == 0 && angle_depth == 0) || c == '\0') {
            if (*count_out >= max_count) {
                parse_error(line, "too many operands");
            }
            char *piece = xstrndup(start, (size_t)(p - start));
            char *trimmed = trim_in_place(piece);
            out[*count_out] = xstrdup(trimmed);
            free(piece);
            (*count_out)++;
            if (c == '\0') {
                break;
            }
            start = p + 1;
        }
    }
    if (*count_out == 1 && out[0][0] == '\0') {
        free(out[0]);
        *count_out = 0;
    }
}

int data_type_size(const char *type) {
    if (ci_eq(type, "BYTE") || ci_eq(type, "DB") || ci_eq(type, "SBYTE")) {
        return 1;
    }
    if (ci_eq(type, "WORD") || ci_eq(type, "DW") || ci_eq(type, "SWORD")) {
        return 2;
    }
    if (ci_eq(type, "DWORD") || ci_eq(type, "DD") || ci_eq(type, "SDWORD")) {
        return 4;
    }
    if (ci_eq(type, "QWORD") || ci_eq(type, "DQ")) {
        return 8;
    }
    if (ci_eq(type, "REAL4")) {
        return 4;
    }
    if (ci_eq(type, "REAL8")) {
        return 8;
    }
    return 0;
}

bool is_ignored_directive(const char *line) {
    const char *directives[] = {
        ".386", ".486", ".586", ".686", ".8086", ".model", ".stack", ".option", ".radix"
    };
    for (size_t i = 0; i < sizeof(directives) / sizeof(directives[0]); i++) {
        if (ci_eq(line, directives[i]) || ci_starts_with(line, directives[i])) {
            return true;
        }
    }
    return false;
}

bool is_size_prefix(const char *token, int *width_out) {
    if (ci_eq(token, "BYTE") || ci_eq(token, "SBYTE")) {
        *width_out = 1;
        return true;
    }
    if (ci_eq(token, "WORD") || ci_eq(token, "SWORD")) {
        *width_out = 2;
        return true;
    }
    if (ci_eq(token, "DWORD") || ci_eq(token, "SDWORD")) {
        *width_out = 4;
        return true;
    }
    if (ci_eq(token, "REAL4")) {
        *width_out = 4;
        return true;
    }
    if (ci_eq(token, "REAL8")) {
        *width_out = 8;
        return true;
    }
    return false;
}

bool parse_float_literal(const char *text, double *value_out) {
    if (!text || !*text) {
        return false;
    }
    const char *p = text;
    if (*p == '-' || *p == '+') {
        p++;
    }
    bool has_dot = false;
    bool has_exp = false;
    for (const char *q = p; *q; q++) {
        if (*q == '.') {
            has_dot = true;
        } else if (*q == 'e' || *q == 'E') {
            has_exp = true;
            break;
        } else if (!isdigit((unsigned char)*q)) {
            return false;
        }
    }
    if (!has_dot && !has_exp) {
        return false;
    }
    char *end = NULL;
    double v = strtod(text, &end);
    if (!end || end == text || *end != '\0') {
        return false;
    }
    *value_out = v;
    return true;
}

