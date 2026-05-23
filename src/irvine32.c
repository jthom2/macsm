#include "masmrun.h"

#include <sys/time.h>

#define IRVINE_MAX_FILES 16
static FILE *g_files[IRVINE_MAX_FILES];
static bool g_clock_started;
static struct timeval g_clock_start;

static uint32_t alloc_handle(FILE *f) {
    for (int i = 0; i < IRVINE_MAX_FILES; i++) {
        if (!g_files[i]) {
            g_files[i] = f;
            return (uint32_t)(i + 1);
        }
    }
    return 0;
}

static FILE *handle_get(uint32_t h) {
    if (h == 0 || h > IRVINE_MAX_FILES) {
        return NULL;
    }
    return g_files[h - 1];
}

static void handle_free(uint32_t h) {
    if (h == 0 || h > IRVINE_MAX_FILES) {
        return;
    }
    g_files[h - 1] = NULL;
}

static char *read_cstring_at(Runtime *runtime, uint32_t address, int line, const char *who) {
    size_t cap = 256;
    size_t len = 0;
    char *buf = xmalloc(cap);
    while (true) {
        if (address >= MEMORY_SIZE) {
            free(buf);
            runtime_error(line, who);
        }
        char c = (char)runtime->program->memory[address++];
        if (len + 1 >= cap) {
            cap *= 2;
            buf = xrealloc(buf, cap);
        }
        buf[len++] = c;
        if (c == '\0') {
            break;
        }
    }
    return buf;
}

static void builtin_write_string(Runtime *runtime, int line) {
    uint32_t address = runtime->cpu.regs[REG_EDX];
    while (true) {
        if (address >= MEMORY_SIZE) {
            runtime_error(line, "WriteString address out of bounds");
        }
        char c = (char)runtime->program->memory[address++];
        if (c == '\0') {
            break;
        }
        fputc(c, stdout);
    }
}

static void builtin_read_int(Runtime *runtime) {
    char buffer[256];
    if (!fgets(buffer, sizeof(buffer), stdin)) {
        runtime->cpu.regs[REG_EAX] = 0;
        return;
    }
    char *end = NULL;
    long value = strtol(buffer, &end, 10);
    (void)end;
    runtime->cpu.regs[REG_EAX] = (uint32_t)(int32_t)value;
}

static void builtin_read_string(Runtime *runtime, int line) {
    uint32_t address = runtime->cpu.regs[REG_EDX];
    uint32_t max_count = runtime->cpu.regs[REG_ECX];
    if (max_count == 0) {
        runtime->cpu.regs[REG_EAX] = 0;
        return;
    }
    if (address >= MEMORY_SIZE || address + max_count > MEMORY_SIZE) {
        runtime_error(line, "ReadString buffer out of bounds");
    }
    char *buffer = xmalloc((size_t)max_count + 2);
    if (!fgets(buffer, (int)max_count + 1, stdin)) {
        runtime->program->memory[address] = '\0';
        runtime->cpu.regs[REG_EAX] = 0;
        free(buffer);
        return;
    }
    size_t len = strcspn(buffer, "\r\n");
    memcpy(&runtime->program->memory[address], buffer, len);
    runtime->program->memory[address + (uint32_t)len] = '\0';
    runtime->cpu.regs[REG_EAX] = (uint32_t)len;
    free(buffer);
}

static uint32_t builtin_strlen_at(const Program *program, uint32_t address, int line) {
    uint32_t start = address;
    while (true) {
        if (address >= MEMORY_SIZE) {
            runtime_error(line, "Str_* address out of bounds");
        }
        if (program->memory[address] == 0) {
            break;
        }
        address++;
    }
    return address - start;
}

static void builtin_str_length(Runtime *runtime, int line) {
    runtime->cpu.regs[REG_EAX] =
        builtin_strlen_at(runtime->program, runtime->cpu.regs[REG_EDX], line);
}

static void builtin_str_copy(Runtime *runtime, int line) {
    Program *program = runtime->program;
    uint32_t src = runtime->cpu.regs[REG_ESI];
    uint32_t dst = runtime->cpu.regs[REG_EDI];
    while (true) {
        if (src >= MEMORY_SIZE || dst >= MEMORY_SIZE) {
            runtime_error(line, "Str_copy address out of bounds");
        }
        uint8_t byte = program->memory[src++];
        program->memory[dst++] = byte;
        if (byte == 0) {
            break;
        }
    }
}

static void builtin_str_compare(Runtime *runtime, int line) {
    Program *program = runtime->program;
    uint32_t a = runtime->cpu.regs[REG_EDX];
    uint32_t b = runtime->cpu.regs[REG_EDI];
    while (true) {
        if (a >= MEMORY_SIZE || b >= MEMORY_SIZE) {
            runtime_error(line, "Str_compare address out of bounds");
        }
        uint8_t ca = program->memory[a];
        uint8_t cb = program->memory[b];
        if (ca != cb || ca == 0) {
            set_sub_flags(runtime, ca, cb, (uint32_t)ca - (uint32_t)cb, 1);
            return;
        }
        a++;
        b++;
    }
}

static void builtin_str_trim(Runtime *runtime, int line) {
    Program *program = runtime->program;
    uint32_t address = runtime->cpu.regs[REG_EDX];
    uint8_t trim = (uint8_t)(runtime->cpu.regs[REG_EAX] & 0xffu);
    uint32_t len = builtin_strlen_at(program, address, line);
    while (len > 0 && program->memory[address + len - 1] == trim) {
        len--;
    }
    if (address + len >= MEMORY_SIZE) {
        runtime_error(line, "Str_trim address out of bounds");
    }
    program->memory[address + len] = 0;
}

static void builtin_str_case(Runtime *runtime, int line, bool upper) {
    Program *program = runtime->program;
    uint32_t address = runtime->cpu.regs[REG_EDX];
    while (true) {
        if (address >= MEMORY_SIZE) {
            runtime_error(line, "Str_*case address out of bounds");
        }
        uint8_t c = program->memory[address];
        if (c == 0) {
            break;
        }
        if (upper && c >= 'a' && c <= 'z') {
            program->memory[address] = (uint8_t)(c - 32);
        } else if (!upper && c >= 'A' && c <= 'Z') {
            program->memory[address] = (uint8_t)(c + 32);
        }
        address++;
    }
}

static uint32_t fake_mseconds_override(bool *has) {
    const char *env = getenv("MASMRUN_FAKE_MSECONDS");
    if (!env || !*env) {
        *has = false;
        return 0;
    }
    *has = true;
    return (uint32_t)strtoul(env, NULL, 10);
}

static void builtin_get_mseconds(Runtime *runtime) {
    bool has_override = false;
    uint32_t override_value = fake_mseconds_override(&has_override);
    if (has_override) {
        runtime->cpu.regs[REG_EAX] = override_value;
        return;
    }
    struct timeval now;
    gettimeofday(&now, NULL);
    if (!g_clock_started) {
        g_clock_start = now;
        g_clock_started = true;
    }
    long sec = now.tv_sec - g_clock_start.tv_sec;
    long usec = now.tv_usec - g_clock_start.tv_usec;
    long ms = sec * 1000 + usec / 1000;
    runtime->cpu.regs[REG_EAX] = (uint32_t)ms;
}

static void builtin_delay(Runtime *runtime) {
    uint32_t ms = runtime->cpu.regs[REG_EAX];
    const char *fake = getenv("MASMRUN_FAKE_DELAY");
    if (fake && *fake && *fake != '0') {
        return;
    }
    if (ms == 0) {
        return;
    }
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void builtin_read_key(Runtime *runtime) {
    int c = fgetc(stdin);
    if (c == EOF) {
        runtime->cpu.zf = true;
        runtime->cpu.regs[REG_EAX] &= 0xffffff00u;
        return;
    }
    runtime->cpu.zf = false;
    runtime->cpu.regs[REG_EAX] = (runtime->cpu.regs[REG_EAX] & 0xffffff00u) | (uint32_t)(uint8_t)c;
}

static void builtin_get_command_tail(Runtime *runtime, int line) {
    uint32_t address = runtime->cpu.regs[REG_EDX];
    if (address >= MEMORY_SIZE) {
        runtime_error(line, "GetCommandTail address out of bounds");
    }
    runtime->program->memory[address] = '\0';
    runtime->cpu.cf = true;
}

static void builtin_write_bin(Runtime *runtime) {
    uint32_t value = runtime->cpu.regs[REG_EAX];
    char buf[33];
    for (int i = 0; i < 32; i++) {
        buf[i] = (value & (1u << (31 - i))) ? '1' : '0';
    }
    buf[32] = '\0';
    fputs(buf, stdout);
}

static void builtin_open_input_file(Runtime *runtime, int line) {
    char *name = read_cstring_at(runtime, runtime->cpu.regs[REG_EDX], line,
                                 "OpenInputFile address out of bounds");
    FILE *f = fopen(name, "rb");
    free(name);
    if (!f) {
        runtime->cpu.regs[REG_EAX] = 0xFFFFFFFFu;
        runtime->cpu.cf = true;
        return;
    }
    uint32_t handle = alloc_handle(f);
    if (handle == 0) {
        fclose(f);
        runtime->cpu.regs[REG_EAX] = 0xFFFFFFFFu;
        runtime->cpu.cf = true;
        return;
    }
    runtime->cpu.regs[REG_EAX] = handle;
    runtime->cpu.cf = false;
}

static void builtin_create_output_file(Runtime *runtime, int line) {
    char *name = read_cstring_at(runtime, runtime->cpu.regs[REG_EDX], line,
                                 "CreateOutputFile address out of bounds");
    FILE *f = fopen(name, "wb");
    free(name);
    if (!f) {
        runtime->cpu.regs[REG_EAX] = 0xFFFFFFFFu;
        runtime->cpu.cf = true;
        return;
    }
    uint32_t handle = alloc_handle(f);
    if (handle == 0) {
        fclose(f);
        runtime->cpu.regs[REG_EAX] = 0xFFFFFFFFu;
        runtime->cpu.cf = true;
        return;
    }
    runtime->cpu.regs[REG_EAX] = handle;
    runtime->cpu.cf = false;
}

static void builtin_read_from_file(Runtime *runtime, int line) {
    uint32_t handle = runtime->cpu.regs[REG_EAX];
    uint32_t address = runtime->cpu.regs[REG_EDX];
    uint32_t count = runtime->cpu.regs[REG_ECX];
    FILE *f = handle_get(handle);
    if (!f) {
        runtime->cpu.regs[REG_EAX] = 0xFFFFFFFFu;
        runtime->cpu.cf = true;
        return;
    }
    if (count == 0) {
        runtime->cpu.regs[REG_EAX] = 0;
        runtime->cpu.cf = false;
        return;
    }
    if (address >= MEMORY_SIZE || address + count > MEMORY_SIZE) {
        runtime_error(line, "ReadFromFile buffer out of bounds");
    }
    size_t got = fread(&runtime->program->memory[address], 1, count, f);
    runtime->cpu.regs[REG_EAX] = (uint32_t)got;
    runtime->cpu.cf = ferror(f) != 0;
}

static void builtin_write_to_file(Runtime *runtime, int line) {
    uint32_t handle = runtime->cpu.regs[REG_EAX];
    uint32_t address = runtime->cpu.regs[REG_EDX];
    uint32_t count = runtime->cpu.regs[REG_ECX];
    FILE *f = handle_get(handle);
    if (!f) {
        runtime->cpu.regs[REG_EAX] = 0;
        runtime->cpu.cf = true;
        return;
    }
    if (count == 0) {
        runtime->cpu.regs[REG_EAX] = 0;
        runtime->cpu.cf = false;
        return;
    }
    if (address >= MEMORY_SIZE || address + count > MEMORY_SIZE) {
        runtime_error(line, "WriteToFile buffer out of bounds");
    }
    size_t put = fwrite(&runtime->program->memory[address], 1, count, f);
    runtime->cpu.regs[REG_EAX] = (uint32_t)put;
    runtime->cpu.cf = put != count;
}

static void builtin_close_file(Runtime *runtime) {
    uint32_t handle = runtime->cpu.regs[REG_EAX];
    FILE *f = handle_get(handle);
    if (!f) {
        runtime->cpu.regs[REG_EAX] = 0;
        return;
    }
    fclose(f);
    handle_free(handle);
    runtime->cpu.regs[REG_EAX] = 1;
}

static void builtin_write_float(Runtime *runtime) {
    double val = runtime->cpu.fpu_st[runtime->cpu.fpu_top];
    printf("%g", val);
}

static void builtin_show_fpu_stack(Runtime *runtime) {
    for (int i = 0; i < 8; i++) {
        double val = runtime->cpu.fpu_st[(runtime->cpu.fpu_top + i) & 7];
        printf("ST(%d)=%.6g\r\n", i, val);
    }
}

static const int g_exit_widths[] = {4};

static const BuiltinSignature g_builtins[] = {
    {"WriteString",       CALLCONV_IRVINE_REG, 0, NULL},
    {"WriteChar",         CALLCONV_IRVINE_REG, 0, NULL},
    {"WriteInt",          CALLCONV_IRVINE_REG, 0, NULL},
    {"WriteDec",          CALLCONV_IRVINE_REG, 0, NULL},
    {"WriteHex",          CALLCONV_IRVINE_REG, 0, NULL},
    {"WriteBin",          CALLCONV_IRVINE_REG, 0, NULL},
    {"Crlf",              CALLCONV_IRVINE_REG, 0, NULL},
    {"ReadInt",           CALLCONV_IRVINE_REG, 0, NULL},
    {"ReadChar",          CALLCONV_IRVINE_REG, 0, NULL},
    {"ReadKey",           CALLCONV_IRVINE_REG, 0, NULL},
    {"ReadString",        CALLCONV_IRVINE_REG, 0, NULL},
    {"Randomize",         CALLCONV_IRVINE_REG, 0, NULL},
    {"RandomRange",       CALLCONV_IRVINE_REG, 0, NULL},
    {"WaitMsg",           CALLCONV_IRVINE_REG, 0, NULL},
    {"DumpRegs",          CALLCONV_IRVINE_REG, 0, NULL},
    {"SetTextColor",      CALLCONV_IRVINE_REG, 0, NULL},
    {"Clrscr",            CALLCONV_IRVINE_REG, 0, NULL},
    {"Gotoxy",            CALLCONV_IRVINE_REG, 0, NULL},
    {"GetMseconds",       CALLCONV_IRVINE_REG, 0, NULL},
    {"Delay",             CALLCONV_IRVINE_REG, 0, NULL},
    {"GetCommandTail",    CALLCONV_IRVINE_REG, 0, NULL},
    {"OpenInputFile",     CALLCONV_IRVINE_REG, 0, NULL},
    {"CreateOutputFile",  CALLCONV_IRVINE_REG, 0, NULL},
    {"ReadFromFile",      CALLCONV_IRVINE_REG, 0, NULL},
    {"WriteToFile",       CALLCONV_IRVINE_REG, 0, NULL},
    {"CloseFile",         CALLCONV_IRVINE_REG, 0, NULL},
    {"Str_length",        CALLCONV_IRVINE_REG, 0, NULL},
    {"Str_copy",          CALLCONV_IRVINE_REG, 0, NULL},
    {"Str_compare",       CALLCONV_IRVINE_REG, 0, NULL},
    {"Str_trim",          CALLCONV_IRVINE_REG, 0, NULL},
    {"Str_ucase",         CALLCONV_IRVINE_REG, 0, NULL},
    {"Str_lcase",         CALLCONV_IRVINE_REG, 0, NULL},
    {"WriteFloat",        CALLCONV_IRVINE_REG, 0, NULL},
    {"ShowFPUStack",      CALLCONV_IRVINE_REG, 0, NULL},
    {"ExitProcess",       CALLCONV_STDCALL,    1, g_exit_widths},
};

const BuiltinSignature *builtin_signatures(size_t *count_out) {
    if (count_out) {
        *count_out = sizeof(g_builtins) / sizeof(g_builtins[0]);
    }
    return g_builtins;
}

const BuiltinSignature *builtin_signature_find(const char *name) {
    size_t count = sizeof(g_builtins) / sizeof(g_builtins[0]);
    for (size_t i = 0; i < count; i++) {
        if (ci_eq(name, g_builtins[i].name)) {
            return &g_builtins[i];
        }
    }
    return NULL;
}

bool run_builtin(Runtime *runtime, const char *name, int invoke_argc, int line) {
    (void)invoke_argc;
    if (ci_eq(name, "WriteString")) {
        builtin_write_string(runtime, line);
        return true;
    }
    if (ci_eq(name, "WriteChar")) {
        fputc((int)(runtime->cpu.regs[REG_EAX] & 0xffu), stdout);
        return true;
    }
    if (ci_eq(name, "WriteInt")) {
        fprintf(stdout, "%d", (int32_t)runtime->cpu.regs[REG_EAX]);
        return true;
    }
    if (ci_eq(name, "WriteDec")) {
        fprintf(stdout, "%u", runtime->cpu.regs[REG_EAX]);
        return true;
    }
    if (ci_eq(name, "WriteHex")) {
        fprintf(stdout, "%08X", runtime->cpu.regs[REG_EAX]);
        return true;
    }
    if (ci_eq(name, "Crlf")) {
        fputs("\r\n", stdout);
        return true;
    }
    if (ci_eq(name, "ReadInt")) {
        builtin_read_int(runtime);
        return true;
    }
    if (ci_eq(name, "ReadChar")) {
        int c = fgetc(stdin);
        if (c == EOF) {
            c = 0;
        }
        runtime->cpu.regs[REG_EAX] = (runtime->cpu.regs[REG_EAX] & 0xffffff00u) | (uint32_t)(uint8_t)c;
        return true;
    }
    if (ci_eq(name, "ReadString")) {
        builtin_read_string(runtime, line);
        return true;
    }
    if (ci_eq(name, "Randomize")) {
        srand((unsigned)time(NULL));
        return true;
    }
    if (ci_eq(name, "RandomRange")) {
        uint32_t range = runtime->cpu.regs[REG_EAX];
        runtime->cpu.regs[REG_EAX] = range == 0 ? 0 : (uint32_t)(rand() % (int)range);
        return true;
    }
    if (ci_eq(name, "WaitMsg")) {
        fputs("Press any key to continue...", stdout);
        fflush(stdout);
        (void)fgetc(stdin);
        return true;
    }
    if (ci_eq(name, "DumpRegs")) {
        fprintf(stdout,
                "EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X EBP=%08X ESP=%08X\r\n",
                runtime->cpu.regs[REG_EAX], runtime->cpu.regs[REG_EBX],
                runtime->cpu.regs[REG_ECX], runtime->cpu.regs[REG_EDX],
                runtime->cpu.regs[REG_ESI], runtime->cpu.regs[REG_EDI],
                runtime->cpu.regs[REG_EBP], runtime->cpu.regs[REG_ESP]);
        return true;
    }
    if (ci_eq(name, "ExitProcess")) {
        uint32_t esp = runtime->cpu.regs[REG_ESP];
        uint32_t code = 0;
        if (esp < STACK_TOP) {
            code = read_le(runtime->program, esp, 4, line);
        }
        runtime->cpu.exit_code = (int)(code & 0xffu);
        runtime->cpu.running = false;
        return true;
    }
    if (ci_eq(name, "SetTextColor") || ci_eq(name, "Clrscr") || ci_eq(name, "Gotoxy")) {
        return true;
    }
    if (ci_eq(name, "WriteBin")) {
        builtin_write_bin(runtime);
        return true;
    }
    if (ci_eq(name, "ReadKey")) {
        builtin_read_key(runtime);
        return true;
    }
    if (ci_eq(name, "GetMseconds")) {
        builtin_get_mseconds(runtime);
        return true;
    }
    if (ci_eq(name, "Delay")) {
        builtin_delay(runtime);
        return true;
    }
    if (ci_eq(name, "GetCommandTail")) {
        builtin_get_command_tail(runtime, line);
        return true;
    }
    if (ci_eq(name, "OpenInputFile")) {
        builtin_open_input_file(runtime, line);
        return true;
    }
    if (ci_eq(name, "CreateOutputFile")) {
        builtin_create_output_file(runtime, line);
        return true;
    }
    if (ci_eq(name, "ReadFromFile")) {
        builtin_read_from_file(runtime, line);
        return true;
    }
    if (ci_eq(name, "WriteToFile")) {
        builtin_write_to_file(runtime, line);
        return true;
    }
    if (ci_eq(name, "CloseFile")) {
        builtin_close_file(runtime);
        return true;
    }
    if (ci_eq(name, "Str_length")) {
        builtin_str_length(runtime, line);
        return true;
    }
    if (ci_eq(name, "Str_copy")) {
        builtin_str_copy(runtime, line);
        return true;
    }
    if (ci_eq(name, "Str_compare")) {
        builtin_str_compare(runtime, line);
        return true;
    }
    if (ci_eq(name, "Str_trim")) {
        builtin_str_trim(runtime, line);
        return true;
    }
    if (ci_eq(name, "Str_ucase")) {
        builtin_str_case(runtime, line, true);
        return true;
    }
    if (ci_eq(name, "Str_lcase")) {
        builtin_str_case(runtime, line, false);
        return true;
    }
    if (ci_eq(name, "WriteFloat")) {
        builtin_write_float(runtime);
        return true;
    }
    if (ci_eq(name, "ShowFPUStack")) {
        builtin_show_fpu_stack(runtime);
        return true;
    }
    return false;
}
