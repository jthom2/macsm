# MASM macOS CX

Native C11 MASM/Irvine32-style runner for macOS on Apple Silicon.

`macsm` does not call MASM, NASM, Wine, QEMU, or a Windows VM. It parses a focused MASM-style `.asm` source file, lays out a flat 32-bit data/stack memory model, interprets a practical subset of x86 instructions, and provides built-in Irvine32-compatible console routines.

## Build

```sh
make
```

The only required compiler is the system C compiler available as `cc`/Apple clang.

## Run

```sh
./macsm examples/hello.asm
./macsm examples/sum_loop.asm
```

Programs are run directly from `.asm` source. No executable, PE file, Mach-O file, or generated script is required.

Debug output is available without changing program stdout:

```sh
./macsm --trace examples/hello.asm
./macsm --debug examples/sum_loop.asm
./macsm --dump-symbols --dump-data examples/sum_loop.asm
```

`--trace` writes one stderr line per instruction with EIP, source line, registers, and flags. `--dump-symbols` and `--dump-data` write parser/data-layout details to stderr after parsing.

`--debug` starts an interactive debugger before the entry instruction. Debugger prompts and responses are written to stderr with a `debug:` prefix, so target program stdout stays byte-for-byte unchanged. Commands are read from stdin: `b <line|label|file:line>`, `c`, `s`, `n`, `p <reg|expr>`, `x/<n><b|w|d> <addr|expr>`, `regs`, `bt`, and `q`. Backtraces are best-effort through the EBP frame chain and stop after 16 frames.

Static compatibility checks are available without executing the program:

```sh
./macsm --check examples/hello.asm
```

`--check` writes a human-readable report to stdout. Findings use `file:line:col`, source-line carets, category labels, and near-match hints where available. It exits `0` when a file is fully supported by the current runner and `1` when unsupported directives, instructions, data declarations, calls, unresolved labels/symbols, or expression forms are found. It cannot be combined with execution debug flags.

## Supported v1 surface

- MASM setup and layout: `INCLUDE Irvine32.inc`, `INCLUDELIB Irvine32.lib`, `.386`, `.model flat, stdcall`, `.stack`, `.data`, `.code`, `PROTO`, `PROC`/`ENDP`, labels, `END main`.
- Data declarations: `BYTE`/`DB`, `WORD`/`DW`, `DWORD`/`DD`, `SDWORD`, `QWORD`, `REAL4`, `REAL8`, strings, numeric and floating literals, `?`, `DUP`, constants with `EQU` or `=`.
- MASM operators: `OFFSET`, `TYPE`, `LENGTHOF`, `SIZEOF`, `$` in data expressions, parentheses, `*`, `/`, direct data labels, `[reg+offset]`, `label[index]`, and `BYTE/WORD/DWORD/REAL4/REAL8 PTR`.
- Procedures, macros, and high-level flow: `PROC a:DWORD` parameters, `LOCAL` stack variables, parser-lowered `INVOKE`, `MACRO`/`ENDM` with named parameters/defaults and macro `LOCAL` labels, `ret imm`, `.IF`/`.ELSEIF`/`.ELSE`/`.ENDIF`, `.WHILE`/`.ENDW`, `.REPEAT`/`.UNTIL`, `.BREAK`, and `.CONTINUE`.
- Structs and type aliases: `Name STRUCT`/`ENDS`, `TYPEDEF PTR`, `<...>` initializers, dot-field access (`label.field`, `[reg].field`, `[reg+offset].field`), and field-aware `TYPE`/`SIZEOF`/`LENGTHOF`.
- Core instructions: `mov`, `movsx`, `movzx`, `lea`, `push`, `pop`, `call`, `ret`, `add`, `adc`, `sub`, `inc`, `dec`, `imul`, `mul`, `div`, `idiv`, `cdq`, `cmp`, `test`, `clc`, `stc`, `daa`, `jmp`, common `jcc` forms, `loop`, `and`, `or`, `xor`, `not`, `neg`, shifts, and `xchg`.
- x87 FPU subset: `fld`, `fst`, `fstp`, `fild`, `fist`, `fistp`, `fldz`, `fld1`, `fchs`, `fabs`, `fadd`, `fsub`, `fsubr`, `fmul`, `fdiv`, `fdivr`, `faddp`, `fsubp`, `fmulp`, `fdivp`, `fcom`, `fcomp`, `fcompp`, `fnstsw ax`, and `sahf`.
- Irvine32 shims: `WriteString`, `WriteChar`, `WriteInt`, `WriteDec`, `WriteHex`, `WriteBin`, `WriteFloat`, `ShowFPUStack`, `Crlf`, `ReadInt`, `ReadChar`, `ReadKey`, `ReadString`, `GetCommandTail`, `Randomize`, `RandomRange`, `DumpRegs`, `WaitMsg`, `GetMseconds`, `Delay`, plus host-file helpers `OpenInputFile`/`CreateOutputFile`/`ReadFromFile`/`WriteToFile`/`CloseFile` and no-op console helpers `SetTextColor`, `Clrscr`, and `Gotoxy`. Deterministic test seams: `MASMRUN_FAKE_MSECONDS`, `MASMRUN_FAKE_DELAY=1`.
- Irvine32 `exit` macro behavior is supported as a pseudo-instruction.
- Irvine32 color constants such as `red`, `blue`, `white`, `black`, and the common light variants are built in for `SetTextColor`-style expressions.

Unsupported MASM syntax fails with a source line diagnostic instead of silently producing approximate behavior.

## Test

```sh
make test
```

The test suite uses only Python's standard `unittest` module. Add real VS/Irvine32 assignment files under `examples/` or extend `tests/test_masmrun.py` with fixture-specific stdin/stdout expectations as the compatibility corpus grows.

The checked-in test suite also pins exact stdout for the current files in `x86_Win32_MASM_Examples/`.

## Source layout

- `masmrun.c`: CLI and mode selection.
- `parser.c`: MASM sections, labels, data declarations, and instruction parsing.
- `macro.c`: `MACRO`/`ENDM` collection, expansion, parameter/default substitution, and macro-local label renaming.
- `hll.c`: condition lowering for MASM high-level control directives.
- `expr.c`: MASM expression evaluation.
- `runtime.c`: CPU state, operands, instruction execution, and trace.
- `debugger.c`: interactive `--debug` commands, breakpoints, inspection, and EBP-chain backtraces.
- `irvine32.c`: built-in Irvine32 routines.
- `program.c`: symbols, memory, debug dumps, and cleanup.
- `diagnostics.c`: shared diagnostic lists, source-line tracking, caret rendering, and fatal parse/runtime shims.
- `check.c`: best-effort static compatibility reports using shared diagnostics.
- `util.c`: shared string, token, literal, and file helpers.

## Current limits

This is not a full MASM assembler, Win32 emulator, or x87 hardware model. Real PE linking, GUI Win32 APIs, the full Irvine32 library, and cycle-accurate floating-point behavior are outside the current implementation.
