# macsm

Native C11 MASM/Irvine32-style runner for macOS (supporting both Apple Silicon and Intel).

`macsm` parses a focused MASM-style `.asm` source file, lays out a flat 32-bit data/stack memory model, interprets a practical subset of x86 instructions, and provides built-in Irvine32-compatible console routines—all without needing MASM, NASM, Wine, QEMU, or a Windows virtual machine.

---

## 🚀 Getting Started on macOS

There are two ways to get `macsm` running on your Mac.

### Method 1: Easiest way (Via Homebrew)
If you use [Homebrew](https://brew.sh/), you can install `macsm` globally with one command:

```bash
# Add the tap and install macsm
brew tap jthom2/tap
brew install macsm
```

Once installed, you can run any MASM `.asm` file from anywhere in your terminal:
```bash
macsm examples/demos/hello.asm
```

---

### Method 2: For Developers (Compile from Source)
If you want to build `macsm` manually or contribute to the project, you can compile it from source.

1. **Clone the Repository**:
   ```bash
   git clone https://github.com/jthom2/macsm.git
   cd macsm
   ```

2. **Build the Binary**:
   The only required tool is the system C compiler (`clang` or `cc`, available by default via macOS Command Line Tools):
   ```bash
   make
   ```

3. **Run the Examples**:
   ```bash
   ./macsm examples/demos/hello.asm
   ./macsm examples/demos/sum_loop.asm
   ```

---

## 🛠️ Usage & Debugging

`macsm` runs programs directly from `.asm` source. No linker, PE executable, or Mach-O generation is required.

### Debug Output & Tracing
Get deep diagnostic output without changing your program's stdout:
```bash
# Trace every executed instruction, registers, and flags to stderr
macsm --trace examples/demos/hello.asm

# Dump symbol tables and flat memory layouts
macsm --dump-symbols --dump-data examples/demos/sum_loop.asm
```

### Interactive Debugger
Start an interactive console debugger before the entry instruction:
```bash
macsm --debug examples/demos/sum_loop.asm
```
Inside the debugger, you can use standard GDB-style command abbreviations:
- `b <line|label>` to set a breakpoint
- `s` to step into an instruction
- `n` to step over
- `c` to continue execution
- `regs` to show current registers
- `bt` to print best-effort call backtraces
- `q` to quit

### Static Compatibility Check
Verify if a file is supported without executing it:
```bash
macsm --check examples/demos/hello.asm
```
`--check` returns a diagnostic report and carets showing syntax errors or unsupported directives. It exits with code `0` if fully supported and `1` if any unsupported features are found.

---

## 📚 Supported MASM & Irvine32 Features

- **Directives & Layout**: `INCLUDE Irvine32.inc`, `.model flat, stdcall`, `.stack`, `.data`, `.code`, `PROC`/`ENDP`, labels, and `END main`.
- **Data Declarations**: `BYTE`, `WORD`, `DWORD`, `SDWORD`, `REAL4`, `REAL8`, arrays with `DUP`, `EQU`, and structures (`STRUCT`/`ENDS`).
- **Operators**: `OFFSET`, `TYPE`, `LENGTHOF`, `SIZEOF`, and type-casting `PTR` operators.
- **High-Level Flow**: HLL condition lowering including `.IF`, `.ELSEIF`, `.ELSE`, `.ENDIF`, `.WHILE`, `.REPEAT`, and `.UNTIL`.
- **Instructions**: Standard data movement (`mov`, `lea`, `push`, `pop`), arithmetic (`add`, `sub`, `imul`, `div`, etc.), control flow (`jmp`, conditional jumps, `loop`), and bitwise operators (`and`, `or`, `xor`, etc.).
- **x87 FPU Math**: Core floating-point subset including `fld`, `fstp`, `fadd`, `fsub`, `fmul`, `fdiv`, and FPU status checks.
- **Irvine32 Library Shims**: Console IO (`WriteString`, `WriteChar`, `WriteDec`, `ReadInt`, etc.), helper routines (`RandomRange`, `GetMseconds`, `Delay`), and host file access (`OpenInputFile`, `ReadFromFile`, etc.).

---

## 🧪 Testing

To run the local Python-based test suite, compile the binary and run:
```bash
make test
```
