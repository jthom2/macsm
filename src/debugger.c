#include "masmrun.h"

typedef struct {
    uint32_t eip;
    int line;
    bool one_shot;
} DebugBreakpoint;

typedef struct {
    DebugBreakpoint *items;
    size_t count;
    size_t cap;
    bool initialized;
    bool step_next;
    bool input_closed;
} DebuggerState;

static DebuggerState g_debugger;

static const char *base_name(const char *path) {
    if (!path) {
        return "";
    }
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool path_matches_source(const RunOptions *options, const char *file) {
    const char *path = options && options->source_path ? options->source_path : source_get_path();
    if (!path || !file || !*file) {
        return false;
    }
    return strcmp(file, path) == 0 || strcmp(file, base_name(path)) == 0;
}

static bool parse_decimal_u32(const char *text, uint32_t *value_out) {
    if (!text || !*text) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || !end || *trim_in_place(end) != '\0' || value > UINT32_MAX) {
        return false;
    }
    *value_out = (uint32_t)value;
    return true;
}

static bool find_instruction_by_line(const Program *program, int line, uint32_t *eip_out) {
    if (line <= 0) {
        return false;
    }
    for (size_t i = 0; i < program->code.count; i++) {
        if (program->code.items[i].line == line) {
            *eip_out = (uint32_t)i;
            return true;
        }
    }
    return false;
}

static const Symbol *symbol_for_eip(const Program *program, uint32_t eip) {
    const Symbol *best = NULL;
    for (size_t i = 0; i < program->symbols.count; i++) {
        const Symbol *symbol = &program->symbols.items[i];
        if (symbol->kind != SYM_CODE || symbol->instr_index > eip) {
            continue;
        }
        if (!best || symbol->instr_index >= best->instr_index) {
            best = symbol;
        }
    }
    return best;
}

static DebugBreakpoint *debug_breakpoint_at(uint32_t eip, bool include_one_shot) {
    for (size_t i = 0; i < g_debugger.count; i++) {
        DebugBreakpoint *bp = &g_debugger.items[i];
        if (bp->eip == eip && (include_one_shot || !bp->one_shot)) {
            return bp;
        }
    }
    return NULL;
}

static DebugBreakpoint *debug_add_breakpoint(uint32_t eip, int line, bool one_shot) {
    if (g_debugger.count == g_debugger.cap) {
        g_debugger.cap = g_debugger.cap ? g_debugger.cap * 2 : 8;
        g_debugger.items = xrealloc(g_debugger.items, g_debugger.cap * sizeof(g_debugger.items[0]));
    }
    DebugBreakpoint bp;
    bp.eip = eip;
    bp.line = line;
    bp.one_shot = one_shot;
    g_debugger.items[g_debugger.count] = bp;
    return &g_debugger.items[g_debugger.count++];
}

static void debug_remove_breakpoint(DebugBreakpoint *bp) {
    if (!bp) {
        return;
    }
    size_t index = (size_t)(bp - g_debugger.items);
    if (index >= g_debugger.count) {
        return;
    }
    for (size_t i = index + 1; i < g_debugger.count; i++) {
        g_debugger.items[i - 1] = g_debugger.items[i];
    }
    g_debugger.count--;
}

static void debug_print_location(const Runtime *runtime, uint32_t eip, const Instruction *ins,
                                 const char *reason) {
    const Program *program = runtime->program;
    const Symbol *symbol = symbol_for_eip(program, eip);
    const char *source = source_get_line(ins->line);
    fprintf(stderr, "debug: stopped%s%s at eip=%u line=%d",
            reason && *reason ? " " : "", reason && *reason ? reason : "",
            eip, ins->line);
    if (symbol && symbol->name) {
        fprintf(stderr, " %s", symbol->name);
    }
    fputc('\n', stderr);
    if (source && *source) {
        fprintf(stderr, "debug: source: %s\n", source);
    } else if (ins->raw && *ins->raw) {
        fprintf(stderr, "debug: source: %s\n", ins->raw);
    }
}

static void debug_print_regs(const Runtime *runtime) {
    fprintf(stderr,
            "debug: eax=%08X ebx=%08X ecx=%08X edx=%08X\n",
            runtime->cpu.regs[REG_EAX], runtime->cpu.regs[REG_EBX],
            runtime->cpu.regs[REG_ECX], runtime->cpu.regs[REG_EDX]);
    fprintf(stderr,
            "debug: esi=%08X edi=%08X ebp=%08X esp=%08X eip=%08X\n",
            runtime->cpu.regs[REG_ESI], runtime->cpu.regs[REG_EDI],
            runtime->cpu.regs[REG_EBP], runtime->cpu.regs[REG_ESP],
            runtime->cpu.eip);
    fprintf(stderr,
            "debug: flags zf=%d sf=%d cf=%d of=%d af=%d df=%d\n",
            runtime->cpu.zf ? 1 : 0, runtime->cpu.sf ? 1 : 0,
            runtime->cpu.cf ? 1 : 0, runtime->cpu.of ? 1 : 0,
            runtime->cpu.af ? 1 : 0, runtime->cpu.df ? 1 : 0);
}

static void debug_print_expr(Runtime *runtime, const char *expr, int line) {
    int64_t value = eval_expr(runtime, runtime->program, expr, line);
    uint32_t u32 = (uint32_t)value;
    fprintf(stderr, "debug: %s = 0x%08X (%" PRId32 ")\n", expr, u32, (int32_t)u32);
}

static void debug_examine(Runtime *runtime, const char *spec, int line) {
    if (!ci_starts_with(spec, "x/")) {
        fprintf(stderr, "debug: error: expected x/<n><b|w|d> <expr>\n");
        return;
    }
    const char *cursor = spec + 2;
    if (!isdigit((unsigned char)*cursor)) {
        fprintf(stderr, "debug: error: memory count is required\n");
        return;
    }
    uint32_t count = 0;
    while (isdigit((unsigned char)*cursor)) {
        count = count * 10u + (uint32_t)(*cursor - '0');
        cursor++;
    }
    int width = 0;
    if (*cursor == 'b') {
        width = 1;
    } else if (*cursor == 'w') {
        width = 2;
    } else if (*cursor == 'd') {
        width = 4;
    } else {
        fprintf(stderr, "debug: error: memory width must be b, w, or d\n");
        return;
    }
    cursor++;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor == '\0') {
        fprintf(stderr, "debug: error: memory expression is required\n");
        return;
    }
    uint32_t address = (uint32_t)eval_expr(runtime, runtime->program, cursor, line);
    fprintf(stderr, "debug: %08X:", address);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t item_address = address + i * (uint32_t)width;
        uint32_t value = read_le(runtime->program, item_address, width, line);
        if (width == 1) {
            fprintf(stderr, " %02X", value & 0xffu);
        } else if (width == 2) {
            fprintf(stderr, " %04X", value & 0xffffu);
        } else {
            fprintf(stderr, " %08X", value);
        }
    }
    fputc('\n', stderr);
}

static void debug_backtrace(Runtime *runtime, uint32_t current_eip, const Instruction *ins) {
    Program *program = runtime->program;
    const Symbol *symbol = symbol_for_eip(program, current_eip);
    fprintf(stderr, "debug: #0 %s eip=%u line=%d\n",
            symbol && symbol->name ? symbol->name : "<unknown>", current_eip, ins->line);

    uint32_t ebp = runtime->cpu.regs[REG_EBP];
    for (int frame = 1; frame < 16; frame++) {
        if (ebp < DATA_BASE || ebp + 8u > MEMORY_SIZE || ebp >= STACK_TOP) {
            break;
        }
        uint32_t next_ebp = read_le(program, ebp, 4, ins->line);
        uint32_t return_eip = read_le(program, ebp + 4u, 4, ins->line);
        if (return_eip >= program->code.count) {
            break;
        }
        const Instruction *return_ins = &program->code.items[return_eip];
        const Symbol *return_symbol = symbol_for_eip(program, return_eip);
        fprintf(stderr, "debug: #%d %s eip=%u line=%d ebp=%08X\n",
                frame,
                return_symbol && return_symbol->name ? return_symbol->name : "<unknown>",
                return_eip, return_ins->line, ebp);
        if (next_ebp <= ebp || next_ebp > STACK_TOP) {
            break;
        }
        ebp = next_ebp;
    }
}

static void debug_set_breakpoint(Runtime *runtime, const char *arg) {
    char *copy = xstrdup(arg ? arg : "");
    char *target = trim_in_place(copy);
    if (*target == '\0') {
        fprintf(stderr, "debug: error: breakpoint target is required\n");
        free(copy);
        return;
    }

    uint32_t eip = 0;
    char *colon = strrchr(target, ':');
    if (colon) {
        *colon = '\0';
        char *file = trim_in_place(target);
        char *line_text = trim_in_place(colon + 1);
        uint32_t parsed_line = 0;
        if (!path_matches_source(runtime->options, file)) {
            fprintf(stderr, "debug: error: file does not match input: %s\n", file);
            free(copy);
            return;
        }
        if (!parse_decimal_u32(line_text, &parsed_line) ||
            !find_instruction_by_line(runtime->program, (int)parsed_line, &eip)) {
            fprintf(stderr, "debug: error: no instruction at %s:%s\n", file, line_text);
            free(copy);
            return;
        }
    } else {
        uint32_t parsed_line = 0;
        if (parse_decimal_u32(target, &parsed_line)) {
            if (!find_instruction_by_line(runtime->program, (int)parsed_line, &eip)) {
                fprintf(stderr, "debug: error: no instruction at line %u\n", parsed_line);
                free(copy);
                return;
            }
        } else {
            Symbol *symbol = symbols_find(&runtime->program->symbols, target);
            if (!symbol || symbol->kind != SYM_CODE) {
                fprintf(stderr, "debug: error: unknown code label: %s\n", target);
                free(copy);
                return;
            }
            eip = (uint32_t)symbol->instr_index;
        }
    }

    Instruction *ins = &runtime->program->code.items[eip];
    DebugBreakpoint *bp = debug_add_breakpoint(eip, ins->line, false);
    fprintf(stderr, "debug: breakpoint %zu at eip=%u line=%d\n",
            (size_t)(bp - g_debugger.items) + 1u, bp->eip, bp->line);
    free(copy);
}

static bool debug_prompt(Runtime *runtime, uint32_t eip, Instruction *ins) {
    char buffer[1024];
    while (runtime->cpu.running) {
        fputs("debug:> ", stderr);
        fflush(stderr);
        if (!fgets(buffer, sizeof(buffer), stdin)) {
            g_debugger.input_closed = true;
            fputs("\ndebug: input closed; continuing\n", stderr);
            return true;
        }

        char *line = trim_in_place(buffer);
        if (*line == '\0') {
            continue;
        }

        if (ci_eq(line, "c") || ci_eq(line, "continue")) {
            return true;
        }
        if (ci_eq(line, "s") || ci_eq(line, "step")) {
            g_debugger.step_next = true;
            return true;
        }
        if (ci_eq(line, "n") || ci_eq(line, "next")) {
            if (ci_eq(ins->op, "call") && eip + 1u < runtime->program->code.count) {
                Instruction *next = &runtime->program->code.items[eip + 1u];
                debug_add_breakpoint(eip + 1u, next->line, true);
            } else {
                g_debugger.step_next = true;
            }
            return true;
        }
        if (ci_eq(line, "q") || ci_eq(line, "quit")) {
            runtime->cpu.exit_code = 0;
            runtime->cpu.running = false;
            return false;
        }
        if (ci_eq(line, "regs")) {
            debug_print_regs(runtime);
            continue;
        }
        if (ci_eq(line, "bt")) {
            debug_backtrace(runtime, eip, ins);
            continue;
        }
        if (ci_starts_with(line, "b ")) {
            debug_set_breakpoint(runtime, trim_in_place(line + 2));
            continue;
        }
        if (ci_starts_with(line, "p ")) {
            char *expr = trim_in_place(line + 2);
            if (*expr == '\0') {
                fprintf(stderr, "debug: error: expression is required\n");
            } else {
                debug_print_expr(runtime, expr, ins->line);
            }
            continue;
        }
        if (ci_starts_with(line, "x/")) {
            debug_examine(runtime, line, ins->line);
            continue;
        }

        fprintf(stderr, "debug: error: unknown command: %s\n", line);
    }
    return false;
}

bool debug_pre_exec(Runtime *runtime, uint32_t eip, Instruction *ins) {
    if (g_debugger.input_closed) {
        return true;
    }

    bool should_stop = false;
    char reason[64] = "";
    DebugBreakpoint *bp = NULL;

    if (!g_debugger.initialized) {
        g_debugger.initialized = true;
        should_stop = true;
        snprintf(reason, sizeof(reason), "at entry");
    } else if (g_debugger.step_next) {
        g_debugger.step_next = false;
        should_stop = true;
        snprintf(reason, sizeof(reason), "after step");
    } else {
        bp = debug_breakpoint_at(eip, true);
        if (bp) {
            should_stop = true;
            snprintf(reason, sizeof(reason), "at breakpoint");
        }
    }

    if (!should_stop) {
        return true;
    }

    if (bp && bp->one_shot) {
        debug_remove_breakpoint(bp);
    }
    debug_print_location(runtime, eip, ins, reason);
    return debug_prompt(runtime, eip, ins);
}
