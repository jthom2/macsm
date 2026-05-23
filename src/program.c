#include "masmrun.h"

void symbols_add(SymbolTable *table, Symbol symbol, int line) {
    for (size_t i = 0; i < table->count; i++) {
        if (ci_eq(table->items[i].name, symbol.name)) {
            parse_error2(line, "duplicate symbol", symbol.name);
        }
    }
    if (table->count == table->cap) {
        table->cap = table->cap ? table->cap * 2 : 64;
        table->items = xrealloc(table->items, table->cap * sizeof(table->items[0]));
    }
    table->items[table->count++] = symbol;
}

Symbol *symbols_find(SymbolTable *table, const char *name) {
    for (size_t i = 0; i < table->count; i++) {
        if (ci_eq(table->items[i].name, name)) {
            return &table->items[i];
        }
    }
    return NULL;
}

LocalVar *locals_find(ProcLocals *locals, const char *name) {
    if (!locals) {
        return NULL;
    }
    for (size_t i = 0; i < locals->count; i++) {
        if (ci_eq(locals->items[i].name, name)) {
            return &locals->items[i];
        }
    }
    return NULL;
}

Prototype *prototypes_find(PrototypeTable *table, const char *name) {
    if (!table) {
        return NULL;
    }
    for (size_t i = 0; i < table->count; i++) {
        if (ci_eq(table->items[i].name, name)) {
            return &table->items[i];
        }
    }
    return NULL;
}

StructDef *structs_find(StructTable *table, const char *name) {
    if (!table) {
        return NULL;
    }
    for (size_t i = 0; i < table->count; i++) {
        if (ci_eq(table->items[i].name, name)) {
            return &table->items[i];
        }
    }
    return NULL;
}

TypeAlias *type_aliases_find(TypeAliasTable *table, const char *name) {
    if (!table) {
        return NULL;
    }
    for (size_t i = 0; i < table->count; i++) {
        if (ci_eq(table->items[i].name, name)) {
            return &table->items[i];
        }
    }
    return NULL;
}

void instructions_add(InstructionList *list, Instruction instruction) {
    if (list->count == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 128;
        list->items = xrealloc(list->items, list->cap * sizeof(list->items[0]));
    }
    list->items[list->count++] = instruction;
}

uint32_t read_le(const Program *program, uint32_t address, int width, int line) {
    if (width < 1 || width > 4 || address > MEMORY_SIZE || address + (uint32_t)width > MEMORY_SIZE) {
        runtime_error(line, "memory read out of bounds");
    }
    uint32_t value = 0;
    for (int i = 0; i < width; i++) {
        value |= ((uint32_t)program->memory[address + (uint32_t)i]) << (8 * i);
    }
    return value;
}

void write_le(Program *program, uint32_t address, int width, uint32_t value, int line) {
    if (width < 1 || width > 4 || address > MEMORY_SIZE || address + (uint32_t)width > MEMORY_SIZE) {
        runtime_error(line, "memory write out of bounds");
    }
    for (int i = 0; i < width; i++) {
        program->memory[address + (uint32_t)i] = (uint8_t)((value >> (8 * i)) & 0xffu);
    }
}

void write_data(Program *program, int width, uint64_t value, int line) {
    if (width < 1 || width > 8 || program->data_next + (uint32_t)width > STACK_TOP) {
        parse_error(line, "data segment exceeds runner memory");
    }
    for (int i = 0; i < width; i++) {
        program->memory[program->data_next + (uint32_t)i] = (uint8_t)((value >> (8 * i)) & 0xffu);
    }
    program->data_next += (uint32_t)width;
}

static const char *symbol_kind_name(SymbolKind kind) {
    switch (kind) {
        case SYM_DATA: return "data";
        case SYM_CONST: return "const";
        case SYM_CODE: return "code";
    }
    return "unknown";
}

void dump_symbols(const Program *program) {
    fprintf(stderr, "structs count=%zu\n", program->structs.count);
    for (size_t i = 0; i < program->structs.count; i++) {
        const StructDef *def = &program->structs.items[i];
        fprintf(stderr, "struct %-18s size=%zu fields=%zu\n",
                def->name, def->byte_size, def->field_count);
        for (size_t j = 0; j < def->field_count; j++) {
            const StructField *f = &def->fields[j];
            fprintf(stderr, "  field %-16s offset=%u width=%d count=%zu\n",
                    f->name, f->offset, f->width, f->count);
        }
    }
    fprintf(stderr, "aliases count=%zu\n", program->aliases.count);
    for (size_t i = 0; i < program->aliases.count; i++) {
        const TypeAlias *a = &program->aliases.items[i];
        fprintf(stderr, "alias %-18s target=%s width=%d\n",
                a->name, a->target ? a->target : "(null)", a->width);
    }
    fprintf(stderr, "symbols count=%zu\n", program->symbols.count);
    for (size_t i = 0; i < program->symbols.count; i++) {
        const Symbol *symbol = &program->symbols.items[i];
        if (symbol->kind == SYM_DATA) {
            fprintf(stderr,
                    "symbol %-18s kind=%s addr=0x%08X elem=%d count=%zu size=%zu\n",
                    symbol->name, symbol_kind_name(symbol->kind), symbol->address,
                    symbol->elem_size, symbol->elem_count, symbol->byte_size);
        } else if (symbol->kind == SYM_CONST) {
            fprintf(stderr,
                    "symbol %-18s kind=%s value=%lld\n",
                    symbol->name, symbol_kind_name(symbol->kind), (long long)symbol->value);
        } else {
            fprintf(stderr,
                    "symbol %-18s kind=%s instr=%zu\n",
                    symbol->name, symbol_kind_name(symbol->kind), symbol->instr_index);
        }
    }
}

void dump_data(const Program *program) {
    fprintf(stderr, "data range=0x%08X..0x%08X bytes=%u\n", DATA_BASE, program->data_next, program->data_next - DATA_BASE);
    for (size_t i = 0; i < program->symbols.count; i++) {
        const Symbol *symbol = &program->symbols.items[i];
        if (symbol->kind != SYM_DATA) {
            continue;
        }
        fprintf(stderr, "data %-18s addr=0x%08X size=%zu bytes=", symbol->name, symbol->address, symbol->byte_size);
        for (size_t j = 0; j < symbol->byte_size; j++) {
            if (j > 0) {
                fputc(' ', stderr);
            }
            fprintf(stderr, "%02X", program->memory[symbol->address + (uint32_t)j]);
        }
        fputc('\n', stderr);
    }
}

void free_program(Program *program) {
    for (size_t i = 0; i < program->symbols.count; i++) {
        free(program->symbols.items[i].name);
        free(program->symbols.items[i].type_name);
    }
    free(program->symbols.items);
    for (size_t i = 0; i < program->code.count; i++) {
        free(program->code.items[i].op);
        free(program->code.items[i].prefix);
        for (int j = 0; j < program->code.items[i].operand_count; j++) {
            free(program->code.items[i].operands[j]);
        }
        free(program->code.items[i].raw);
    }
    free(program->code.items);
    free(program->code.entry);
    for (size_t i = 0; i < program->procs.count; i++) {
        ProcLocals *locals = &program->procs.items[i];
        free(locals->proc_name);
        for (size_t j = 0; j < locals->count; j++) {
            free(locals->items[j].name);
        }
        free(locals->items);
    }
    free(program->procs.items);
    for (size_t i = 0; i < program->prototypes.count; i++) {
        Prototype *prototype = &program->prototypes.items[i];
        free(prototype->name);
        for (size_t j = 0; j < prototype->param_count; j++) {
            free(prototype->params[j].name);
        }
        free(prototype->params);
    }
    free(program->prototypes.items);
    for (size_t i = 0; i < program->structs.count; i++) {
        StructDef *def = &program->structs.items[i];
        free(def->name);
        for (size_t j = 0; j < def->field_count; j++) {
            free(def->fields[j].name);
            free(def->fields[j].type_ref);
        }
        free(def->fields);
    }
    free(program->structs.items);
    for (size_t i = 0; i < program->aliases.count; i++) {
        free(program->aliases.items[i].name);
        free(program->aliases.items[i].target);
    }
    free(program->aliases.items);
    free(program->memory);
    source_clear_lines();
}
