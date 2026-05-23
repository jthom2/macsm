#include "masmrun.h"

typedef struct {
    const char *path;
    char **lines;
    size_t line_count;
    size_t line_cap;
} SourceContext;

static SourceContext g_source;

void source_set_path(const char *path) {
    g_source.path = path;
}

void source_add_line(const char *line, size_t len) {
    if (g_source.line_count == g_source.line_cap) {
        g_source.line_cap = g_source.line_cap ? g_source.line_cap * 2 : 128;
        g_source.lines = xrealloc(g_source.lines, g_source.line_cap * sizeof(g_source.lines[0]));
    }
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        len--;
    }
    g_source.lines[g_source.line_count++] = xstrndup(line, len);
}

void source_clear_lines(void) {
    for (size_t i = 0; i < g_source.line_count; i++) {
        free(g_source.lines[i]);
    }
    free(g_source.lines);
    g_source.lines = NULL;
    g_source.line_count = 0;
    g_source.line_cap = 0;
}

const char *source_get_line(int line) {
    if (line <= 0 || (size_t)line > g_source.line_count) {
        return NULL;
    }
    return g_source.lines[(size_t)line - 1];
}

const char *source_get_path(void) {
    return g_source.path;
}

int source_default_column(int line) {
    if (line <= 0 || (size_t)line > g_source.line_count) {
        return 1;
    }
    const char *text = g_source.lines[(size_t)line - 1];
    int col = 1;
    while (*text && isspace((unsigned char)*text)) {
        text++;
        col++;
    }
    return col;
}

static const char *ci_strstr_local(const char *haystack, const char *needle) {
    if (!needle || !*needle) {
        return haystack;
    }
    size_t needle_len = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < needle_len && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == needle_len) {
            return p;
        }
    }
    return NULL;
}

int source_find_column(int line, const char *needle) {
    if (line <= 0 || (size_t)line > g_source.line_count || !needle || !*needle) {
        return source_default_column(line);
    }
    const char *source = g_source.lines[(size_t)line - 1];
    const char *found = strstr(source, needle);
    if (!found) {
        found = ci_strstr_local(source, needle);
    }
    if (!found) {
        return source_default_column(line);
    }
    return (int)(found - source) + 1;
}

static const char *severity_name(DiagnosticSeverity severity) {
    switch (severity) {
        case DIAG_WARNING:
            return "warning";
        case DIAG_NOTE:
            return "note";
        case DIAG_ERROR:
        default:
            return "error";
    }
}

void diag_push_at(DiagnosticList *list, DiagnosticSeverity severity, int line, int col,
                  const char *category, const char *message, const char *detail,
                  const char *hint) {
    if (list->count == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 16;
        list->items = xrealloc(list->items, list->cap * sizeof(list->items[0]));
    }
    Diagnostic *diag = &list->items[list->count++];
    diag->severity = severity;
    diag->line = line;
    diag->col = col > 0 ? col : source_default_column(line);
    diag->category = xstrdup(category && *category ? category : "general");
    diag->message = xstrdup(message && *message ? message : "<unknown>");
    diag->detail = detail && *detail ? xstrdup(detail) : NULL;
    diag->hint = hint && *hint ? xstrdup(hint) : NULL;
}

void diag_push(DiagnosticList *list, DiagnosticSeverity severity, int line,
               const char *category, const char *message, const char *detail,
               const char *hint) {
    diag_push_at(list, severity, line, source_find_column(line, message),
                 category, message, detail, hint);
}

void parse_error_continue(DiagnosticList *list, int line, int col, const char *category,
                          const char *message, const char *detail, const char *hint) {
    diag_push_at(list, DIAG_ERROR, line, col, category, message, detail, hint);
}

size_t diag_count_errors(const DiagnosticList *list) {
    size_t count = 0;
    for (size_t i = 0; i < list->count; i++) {
        if (list->items[i].severity == DIAG_ERROR) {
            count++;
        }
    }
    return count;
}

static void print_source_context(FILE *stream, int line, int col) {
    if (line <= 0 || (size_t)line > g_source.line_count) {
        return;
    }
    const char *source = g_source.lines[(size_t)line - 1];
    if (!*source) {
        return;
    }
    size_t len = strlen(source);
    if (col < 1) {
        col = 1;
    }
    if ((size_t)col > len + 1) {
        col = (int)len + 1;
    }
    fprintf(stream, "  source: %s\n", source);
    fputs("          ", stream);
    for (int i = 1; i < col; i++) {
        fputc(source[(size_t)i - 1] == '\t' ? '\t' : ' ', stream);
    }
    fputs("^\n", stream);
}

void diag_print_all(FILE *stream, const char *path, const DiagnosticList *list) {
    const char *shown_path = path ? path : (g_source.path ? g_source.path : "<input>");
    for (size_t i = 0; i < list->count; i++) {
        const Diagnostic *diag = &list->items[i];
        int col = diag->col > 0 ? diag->col : source_default_column(diag->line);
        fprintf(stream, "%s:%d:%d: %s: %s: %s",
                shown_path, diag->line, col, severity_name(diag->severity),
                diag->category, diag->message);
        if (diag->detail) {
            fprintf(stream, " - %s", diag->detail);
        }
        fputc('\n', stream);
        print_source_context(stream, diag->line, col);
        if (diag->hint) {
            fprintf(stream, "  hint: %s\n", diag->hint);
        }
    }
}

void diag_free(DiagnosticList *list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].category);
        free(list->items[i].message);
        free(list->items[i].detail);
        free(list->items[i].hint);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int min3(int a, int b, int c) {
    int m = a < b ? a : b;
    return m < c ? m : c;
}

static int edit_distance_ci(const char *a, const char *b, int limit) {
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    if ((alen > blen ? alen - blen : blen - alen) > (size_t)limit) {
        return limit + 1;
    }
    int *prev = xmalloc((blen + 1) * sizeof(prev[0]));
    int *curr = xmalloc((blen + 1) * sizeof(curr[0]));
    for (size_t j = 0; j <= blen; j++) {
        prev[j] = (int)j;
    }
    for (size_t i = 1; i <= alen; i++) {
        curr[0] = (int)i;
        int row_best = curr[0];
        for (size_t j = 1; j <= blen; j++) {
            int cost = tolower((unsigned char)a[i - 1]) == tolower((unsigned char)b[j - 1]) ? 0 : 1;
            curr[j] = min3(prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost);
            if (curr[j] < row_best) {
                row_best = curr[j];
            }
        }
        int *tmp = prev;
        prev = curr;
        curr = tmp;
        if (row_best > limit) {
            free(prev);
            free(curr);
            return limit + 1;
        }
    }
    int result = prev[blen];
    free(prev);
    free(curr);
    return result;
}

char *diag_suggest_near(const char *value, const char *const *candidates, size_t count) {
    if (!value || !*value || !candidates || count == 0) {
        return NULL;
    }
    size_t len = strlen(value);
    int limit = len <= 4 ? 2 : 3;
    const char *best = NULL;
    int best_dist = limit + 1;
    for (size_t i = 0; i < count; i++) {
        if (!candidates[i] || !*candidates[i]) {
            continue;
        }
        int dist = edit_distance_ci(value, candidates[i], limit);
        if (dist < best_dist) {
            best_dist = dist;
            best = candidates[i];
        }
    }
    if (!best || best_dist > limit) {
        return NULL;
    }
    size_t needed = strlen(best) + 18;
    char *hint = xmalloc(needed);
    snprintf(hint, needed, "did you mean '%s'?", best);
    return hint;
}

void parse_error(int line, const char *message) {
    DiagnosticList list = {0};
    diag_push_at(&list, DIAG_ERROR, line, source_default_column(line), "parse", message, NULL, NULL);
    diag_print_all(stderr, NULL, &list);
    diag_free(&list);
    exit(1);
}

void parse_error2(int line, const char *message, const char *detail) {
    DiagnosticList list = {0};
    diag_push_at(&list, DIAG_ERROR, line, source_find_column(line, detail), "parse", message, detail, NULL);
    diag_print_all(stderr, NULL, &list);
    diag_free(&list);
    exit(1);
}

void runtime_error(int line, const char *message) {
    DiagnosticList list = {0};
    diag_push_at(&list, DIAG_ERROR, line, source_default_column(line), "runtime", message, NULL, NULL);
    diag_print_all(stderr, NULL, &list);
    diag_free(&list);
    exit(1);
}

void runtime_error2(int line, const char *message, const char *detail) {
    DiagnosticList list = {0};
    diag_push_at(&list, DIAG_ERROR, line, source_find_column(line, detail), "runtime", message, detail, NULL);
    diag_print_all(stderr, NULL, &list);
    diag_free(&list);
    exit(1);
}
