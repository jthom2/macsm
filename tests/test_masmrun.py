import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "macsm"


class MasmRunTests(unittest.TestCase):
    def run_asm(self, name, stdin=b"", args=()):
        proc = subprocess.run(
            [str(BIN), *args, str(ROOT / "examples" / name)],
            input=stdin,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        return proc

    def run_source(self, source, stdin=b"", args=()):
        with tempfile.NamedTemporaryFile("w", suffix=".asm", delete=False) as tmp:
            tmp.write(source)
            tmp_path = Path(tmp.name)
        try:
            proc = subprocess.run(
                [str(BIN), *args, str(tmp_path)],
                input=stdin,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            return proc
        finally:
            tmp_path.unlink(missing_ok=True)

    def run_check_source(self, source):
        with tempfile.NamedTemporaryFile("w", suffix=".asm", delete=False) as tmp:
            tmp.write(source)
            tmp_path = Path(tmp.name)
        try:
            proc = subprocess.run(
                [str(BIN), "--check", str(tmp_path)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            return proc
        finally:
            tmp_path.unlink(missing_ok=True)

    def test_hello_irvine32_program(self):
        proc = self.run_asm("hello.asm")
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"Hello from MASM\r\n")

    def test_sum_loop_arrays_and_lengthof_type(self):
        proc = self.run_asm("sum_loop.asm")
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"20\r\n")

    def test_readint_and_writeint(self):
        proc = self.run_asm("readint.asm", stdin=b"32\n")
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"Number: 42\r\n")

    def test_dup_widths_movsx_and_movzx(self):
        proc = self.run_source(
            """
            INCLUDE Irvine32.inc
            .data
            banner BYTE 3 DUP('A'),0
            bytes BYTE 0, 255, 7
            .code
            main PROC
                mov edx, OFFSET banner
                call WriteString
                call Crlf
                movzx eax, BYTE PTR bytes[2]
                call WriteDec
                call Crlf
                movsx eax, BYTE PTR bytes[1]
                call WriteInt
                call Crlf
                exit
            main ENDP
            END main
            """
        )
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"AAA\r\n7\r\n-1\r\n")

    def test_cmp_and_signed_branch(self):
        proc = self.run_source(
            """
            INCLUDE Irvine32.inc
            .data
            ok BYTE "branch-ok",0
            .code
            main PROC
                mov eax, 5
                cmp eax, 3
                jle Bad
                mov edx, OFFSET ok
                call WriteString
                call Crlf
                exit
            Bad:
                mov eax, 9
                INVOKE ExitProcess, eax
            main ENDP
            END main
            """
        )
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"branch-ok\r\n")

    def test_high_level_if_directive(self):
        proc = self.run_asm("hll_if.asm")
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"two\r\n")

    def test_includelib_is_builtin_ignored(self):
        proc = self.run_source(
            """
            INCLUDE Irvine32.inc
            INCLUDELIB Irvine32.lib
            .data
            msg BYTE "library-ok",0
            .code
            main PROC
                mov edx, OFFSET msg
                call WriteString
                exit
            main ENDP
            END main
            """
        )
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"library-ok")

    def test_data_continuation_location_counter_and_division(self):
        proc = self.run_source(
            """
            INCLUDE Irvine32.inc
            .data
            points DWORD 10, 20, 30
                   DWORD 40, 50, 60
                   DWORD 70, 80, 90
            NumPoints = ($ - points) / 12
            label BYTE "count=",0
            .code
            main PROC
                mov edx, OFFSET label
                call WriteString
                mov eax, NumPoints
                call WriteDec
                exit
            main ENDP
            END main
            """
        )
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"count=3")

    def test_struct_typedef_and_struct_array_initializers(self):
        proc = self.run_source(
            """
            INCLUDE Irvine32.inc
            .data
            Point STRUCT
                x DWORD ?
                y DWORD 5
            Point ENDS
            PointPtr TYPEDEF PTR Point
            p1 Point <1, 2>
            p2 Point <>
            points Point 2 DUP(<3, 4>)
            head PointPtr ?
            .code
            main PROC
                mov eax, DWORD PTR p1
                add eax, DWORD PTR p1[4]
                add eax, DWORD PTR p2
                add eax, DWORD PTR p2[4]
                add eax, DWORD PTR points
                add eax, DWORD PTR points[4]
                add eax, DWORD PTR points[8]
                add eax, DWORD PTR points[12]
                call WriteDec
                call Crlf
                mov eax, head
                call WriteDec
                exit
            main ENDP
            END main
            """
        )
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"22\r\n0")

    def test_struct_dot_fields_and_field_size_operators(self):
        proc = self.run_source(
            """
            INCLUDE Irvine32.inc
            .data
            Point STRUCT
                x DWORD ?
                y DWORD ?
            Point ENDS
            p1 Point <10, 20>
            points Point <1, 2>, <3, 4>
            .code
            main PROC
                mov esi, OFFSET p1
                mov edi, OFFSET points

                mov eax, p1.x
                add eax, p1.y
                add eax, [esi].x
                add eax, [edi].y
                add eax, [edi+8].x
                add eax, points[8].y

                add eax, TYPE p1.y
                add eax, SIZEOF p1.y
                add eax, LENGTHOF p1.y
                add eax, TYPE Point.y
                add eax, SIZEOF Point
                add eax, LENGTHOF Point

                mov ecx, OFFSET points[8].y
                sub ecx, OFFSET points
                add eax, ecx

                call WriteDec
                exit
            main ENDP
            END main
            """
        )
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"83")

    def test_irvine32_color_constants(self):
        proc = self.run_source(
            """
            INCLUDE Irvine32.inc
            .code
            main PROC
                mov eax, red + (white * 16)
                call WriteDec
                exit
            main ENDP
            END main
            """
        )
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"244")

    def test_clc_adc_and_daa_bcd_adjustment(self):
        proc = self.run_source(
            """
            INCLUDE Irvine32.inc
            .code
            main PROC
                mov al, 99h
                clc
                adc al, 1
                daa
                movzx eax, al
                call WriteHex
                call Crlf
                mov eax, 0
                adc eax, 0
                call WriteDec
                exit
            main ENDP
            END main
            """
        )
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"00000000\r\n1")

    def test_sign_extend_instructions(self):
        proc = self.run_asm("sign_extend.asm")
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"-2\r\n-128\r\n1234FFFF\r\n")

    def test_pushad_popad_pushfd_popfd(self):
        proc = self.run_asm("save_restore.asm")
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"regs-ok\r\nflags-ok\r\n")

    def test_string_instructions_and_rep_prefix(self):
        proc = self.run_asm("string_ops.asm")
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"MASM\r\n4\r\nXXX\r\nM\r\nequal\r\n")

    def test_check_accepts_rep_string_ops(self):
        proc = self.run_asm("string_ops.asm", args=("--check",))
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertIn(b"findings=0", proc.stdout)

    def test_str_routines(self):
        proc = self.run_asm("string_routines.asm")
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"5\r\nAbC!!\r\neq\r\nneq\r\ntrim\r\nABC!!\r\nabc!!\r\n")

    def test_local_directive_basic(self):
        proc = self.run_asm("locals.asm")
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"15\r\n")

    def test_local_directive_array(self):
        proc = self.run_asm("locals_array.asm")
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"local\r\n")

    def test_macro_print_and_defaults(self):
        proc = self.run_asm("macro_print.asm")
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"macro-ok\r\n")

    def test_macro_local_labels_do_not_collide(self):
        proc = self.run_asm("macro_local_label.asm")
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"4\r\n")

    def test_macro_recursion_depth_errors(self):
        proc = self.run_source(
            """
            INCLUDE Irvine32.inc
            .code
            Looping MACRO
                Looping
            ENDM
            main PROC
                Looping
                exit
            main ENDP
            END main
            """
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn(b"macro expansion depth exceeded", proc.stderr)

    def test_check_accepts_macros(self):
        proc = self.run_asm("macro_print.asm", args=("--check",))
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertIn(b"findings=0", proc.stdout)

    def test_local_missing_ret_errors(self):
        proc = self.run_source(
            """
            INCLUDE Irvine32.inc
            .code
            BadProc PROC
                LOCAL value:DWORD
                mov value, 1
                INVOKE ExitProcess, 0
            BadProc ENDP
            END BadProc
            """
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn(b"PROC with LOCALs must end with explicit RET", proc.stderr)

    def test_check_accepts_local_directive(self):
        proc = self.run_asm("locals.asm", args=("--check",))
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertIn(b"findings=0", proc.stdout)

    def test_struct_point_example(self):
        proc = self.run_asm("struct_point.asm")
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"3\r\n30\r\n")

    def test_struct_array_example(self):
        proc = self.run_asm("struct_array.asm")
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"600\r\n")

    def test_check_accepts_struct_examples(self):
        for name in ("struct_point.asm", "struct_array.asm"):
            proc = self.run_asm(name, args=("--check",))
            self.assertEqual(proc.returncode, 0, proc.stderr.decode())
            self.assertIn(b"findings=0", proc.stdout)

    def test_check_flags_unknown_struct_field(self):
        proc = self.run_source(
            """
            INCLUDE Irvine32.inc
            .data
            Point STRUCT
                x DWORD ?
                y DWORD ?
            Point ENDS
            p1 Point <1, 2>
            .code
            main PROC
                mov eax, p1.zzz_unknown_field
                exit
            main ENDP
            END main
            """,
            args=("--check",),
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn(b"zzz_unknown_field", proc.stdout)

    def test_dump_symbols_lists_structs_and_aliases(self):
        proc = self.run_asm("struct_point.asm", args=("--dump-symbols",))
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertIn(b"struct Point", proc.stderr)
        self.assertIn(b"field x", proc.stderr)
        self.assertIn(b"field y", proc.stderr)

    def test_proc_params_and_invoke(self):
        proc = self.run_asm("proc_params.asm")
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"15\r\n")

    def test_invoke_exit_process_code(self):
        proc = self.run_asm("invoke_exit.asm")
        self.assertEqual(proc.returncode, 7, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"")

    def test_high_level_loop_directives(self):
        for name, expected in {
            "hll_while.asm": b"15\r\n",
            "hll_repeat.asm": b"8\r\n",
        }.items():
            with self.subTest(name=name):
                proc = self.run_asm(name)
                self.assertEqual(proc.returncode, 0, proc.stderr.decode())
                self.assertEqual(proc.stdout, expected)

    def test_check_validates_invoke_signatures(self):
        proc = self.run_check_source(
            """
            INCLUDE Irvine32.inc
            .data
            msg BYTE "x",0
            .code
            Sum PROTO a:DWORD, b:DWORD
            main PROC
                INVOKE Sum, 1
                INVOKE WriteString, OFFSET msg
                exit
            main ENDP
            Sum PROC a:DWORD, b:DWORD
                mov eax, a
                add eax, b
                ret
            Sum ENDP
            END main
            """
        )
        self.assertEqual(proc.returncode, 1)
        self.assertIn(b"wrong INVOKE argument count", proc.stdout)
        self.assertIn(b"Irvine32 routine uses registers", proc.stdout)

    def test_check_reports_malformed_high_level_blocks(self):
        proc = self.run_check_source(
            """
            INCLUDE Irvine32.inc
            .code
            main PROC
                .WHILE eax
                    .BREAK
                exit
            main ENDP
            END main
            """
        )
        self.assertEqual(proc.returncode, 1)
        self.assertIn(b"missing matching high-level block terminator", proc.stdout)

    def test_all_features_composite(self):
        proc = self.run_asm("all_features.asm")
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"all-ok\r\n")

    def test_debug_flags_write_to_stderr_only(self):
        proc = self.run_asm("hello.asm", args=("--trace", "--dump-symbols", "--dump-data"))
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"Hello from MASM\r\n")
        self.assertIn(b"trace step=1", proc.stderr)
        self.assertIn(b"symbols count=", proc.stderr)
        self.assertIn(b"data message", proc.stderr)

    def test_make_clean_rebuilds_multifile_project(self):
        clean = subprocess.run(["make", "clean"], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        self.assertEqual(clean.returncode, 0, clean.stderr.decode())
        build = subprocess.run(["make"], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        self.assertEqual(build.returncode, 0, build.stderr.decode())
        self.assertTrue(BIN.exists())

    def test_check_mode_supported_examples(self):
        paths = sorted((ROOT / "examples").glob("*.asm"))
        paths += sorted((ROOT / "x86_Win32_MASM_Examples").glob("*.asm"))
        for path in paths:
            with self.subTest(path=path.name):
                proc = subprocess.run(
                    [str(BIN), "--check", str(path)],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    check=False,
                )
                self.assertEqual(proc.returncode, 0, proc.stderr.decode())
                self.assertIn(b"Compatibility check: supported", proc.stdout)
                self.assertIn(b"findings=0", proc.stdout)

    def test_check_mode_reports_multiple_unsupported_findings(self):
        proc = self.run_check_source(
            """
            INCLUDE Bad.inc
            INCLUDELIB Bad.lib
            .data
            Thing TBYTE 0
            MyStruct STRUCT
            MyStruct ENDS
            BadMacro MACRO a, b
                mov eax, a
            ENDM
            .code
            main PROC
                BadMacro 1
                .IF 1
                .WHILE eax
                BogusOp eax
                call Nope
                jmp Missing
                exit
            main ENDP
            END main
            """
        )
        self.assertEqual(proc.returncode, 1)
        out = proc.stdout
        for expected in [
            b"Compatibility check: unsupported",
            b"include:",
            b"directive:",
            b"data:",
            b"macro:",
            b"instruction:",
            b"call:",
            b"label:",
            b"expression:",
            b"Bad.inc",
            b"Bad.lib",
            b"TBYTE",
            b".IF 1",
            b".WHILE eax",
            b"BogusOp",
            b"Nope",
            b"Missing",
        ]:
            self.assertIn(expected, out)

    def test_check_mode_cli_errors(self):
        combo = subprocess.run(
            [str(BIN), "--check", "--trace", str(ROOT / "examples" / "hello.asm")],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(combo.returncode, 64)
        self.assertIn(b"--check cannot be combined", combo.stderr)

        missing = subprocess.run(
            [str(BIN), "--check", str(ROOT / "examples" / "missing.asm")],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(missing.returncode, 2)
        self.assertIn(b"cannot open", missing.stderr)

    def test_real_masm_example_corpus_exact_outputs(self):
        cases = {
            "JacksonThomas_HW10_3.asm": (b"", b"Number of characters in string: 16"),
            "JacksonThomas_HW10_4.asm": (b"", b"Z: 30\r\nZ: 60\r\nZ: 90\r\n"),
            "JacksonThomas_HW6_1.asm": (b"1\n", b"Please enter a value:Jackson Thomas"),
            "JacksonThomas_HW6_2.asm": (
                b"1\n2\n3\n4\n",
                b"Please enter a value:Please enter a value:Please enter a value:Please enter a value:\r\n4\r\n3\r\n2\r\n1\r\n",
            ),
            "JacksonThomas_HW7_1.asm": (
                b"A\n",
                b"Enter a character to search for: A\r\n\r\nFound character 'A' at index 23\r\n\r\n",
            ),
            "JacksonThomas_HW7_2.asm": (
                b"",
                b"Before Encryption: Thomas\r\nEncrypted: \x1a&!#/=\r\nDecrypted: Thomas\r\n",
            ),
            "JacksonThomas_HW8_2a.asm": (b"", b"99"),
            "JacksonThomas_HW8_2b-1.asm": (
                b"",
                b"EAX=0B56CA20 EBX=00000000 ECX=00000000 EDX=00005629 ESI=00000000 EDI=00000000 EBP=00FFF000 ESP=00FFF000\r\n",
            ),
            "JacksonThomas_HW8_2b.asm": (
                b"",
                b"EAX=0B56CA20 EBX=00000000 ECX=00000000 EDX=00005629 ESI=00000000 EDI=00000000 EBP=00FFF000 ESP=00FFF000\r\n",
            ),
            "JacksonThomas_HW9_1.asm": (b"", b"-1"),
            "JacksonThomas_HW9_2.asm": (
                b"",
                b" First BCD:  90000000\r\nSecond BCD:  00119999\r\n    Result:  90119999",
            ),
        }
        for name, (stdin, expected_stdout) in cases.items():
            with self.subTest(name=name):
                proc = subprocess.run(
                    [str(BIN), str(ROOT / "x86_Win32_MASM_Examples" / name)],
                    input=stdin,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    check=False,
                )
                self.assertEqual(proc.returncode, 0, proc.stderr.decode())
                self.assertEqual(proc.stdout, expected_stdout)


    def test_file_io_roundtrip(self):
        tmp_file = ROOT / "masmrun_file_io_test.tmp"
        if tmp_file.exists():
            tmp_file.unlink()
        try:
            proc = subprocess.run(
                [str(BIN), str(ROOT / "examples" / "file_io.asm")],
                cwd=str(ROOT),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertEqual(proc.returncode, 0, proc.stderr.decode())
            self.assertEqual(proc.stdout, b"hello-file\r\n")
            self.assertTrue(tmp_file.exists())
            self.assertEqual(tmp_file.read_bytes(), b"hello-file\x00")
        finally:
            tmp_file.unlink(missing_ok=True)

    def test_time_key_routines(self):
        env = {"MASMRUN_FAKE_MSECONDS": "1234", "MASMRUN_FAKE_DELAY": "1"}
        proc = subprocess.run(
            [str(BIN), str(ROOT / "examples" / "time_key.asm")],
            input=b"Z",
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env={**__import__("os").environ, **env},
            check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"ms=1234\r\nkey=Z\r\n")

    def test_check_accepts_irvine32_pack_examples(self):
        for name in ("file_io.asm", "time_key.asm"):
            with self.subTest(name=name):
                proc = subprocess.run(
                    [str(BIN), "--check", str(ROOT / "examples" / name)],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    check=False,
                )
                self.assertEqual(proc.returncode, 0, proc.stdout.decode() + proc.stderr.decode())

    def test_check_rejects_unknown_irvine_call(self):
        source = """INCLUDE Irvine32.inc
.code
main PROC
    call BogusRoutine
    INVOKE ExitProcess, 0
main ENDP
END main
"""
        proc = self.run_check_source(source)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn(b"BogusRoutine", proc.stdout)

    def test_fpu_average(self):
        proc = self.run_asm("fpu_average.asm")
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"avg=20\r\n")

    def test_fpu_compare(self):
        proc = self.run_asm("fpu_compare.asm")
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertEqual(proc.stdout, b"less\r\n")

    def test_check_accepts_fpu_examples(self):
        for name in ("fpu_average.asm", "fpu_compare.asm"):
            with self.subTest(name=name):
                proc = subprocess.run(
                    [str(BIN), "--check", str(ROOT / "examples" / name)],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    check=False,
                )
                self.assertEqual(proc.returncode, 0,
                                 proc.stdout.decode() + proc.stderr.decode())

    def test_check_rejects_unsupported_fpu_opcode(self):
        source = """INCLUDE Irvine32.inc
.data
    x REAL8 1.0
.code
main PROC
    fsin
    INVOKE ExitProcess, 0
main ENDP
END main
"""
        proc = self.run_check_source(source)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn(b"fsin", proc.stdout)


if __name__ == "__main__":
    unittest.main()
