import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "macsm"


class DebuggerTests(unittest.TestCase):
    def run_debug(self, program="sum_loop.asm", commands=b""):
        return subprocess.run(
            [str(BIN), "--debug", str(ROOT / "examples" / program)],
            input=commands,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

    def test_sum_loop_debugger_script(self):
        proc = self.run_debug(
            commands=(
                b"b 16\n"
                b"c\n"
                b"regs\n"
                b"p eax\n"
                b"x/4d numbers\n"
                b"s\n"
                b"n\n"
                b"bt\n"
                b"c\n"
            )
        )
        stderr = proc.stderr.decode()
        self.assertEqual(proc.returncode, 0, stderr)
        self.assertEqual(proc.stdout, b"20\r\n")
        self.assertIn("debug: breakpoint 1 at", stderr)
        self.assertIn("line=16", stderr)
        self.assertIn("debug: eax=00000000", stderr)
        self.assertIn("debug: eax = 0x00000000 (0)", stderr)
        self.assertIn("debug: 00010000: 00000002 00000004 00000006 00000008", stderr)
        self.assertIn("line=17", stderr)
        self.assertIn("line=18", stderr)
        self.assertIn("debug: #0", stderr)

    def test_breakpoint_by_label(self):
        proc = self.run_debug(commands=b"b L1\nc\nq\n")
        stderr = proc.stderr.decode()
        self.assertEqual(proc.returncode, 0, stderr)
        self.assertEqual(proc.stdout, b"")
        self.assertIn("debug: breakpoint 1 at", stderr)
        self.assertIn("line=16", stderr)

    def test_breakpoint_by_file_line(self):
        proc = self.run_debug(commands=b"b sum_loop.asm:16\nc\nq\n")
        stderr = proc.stderr.decode()
        self.assertEqual(proc.returncode, 0, stderr)
        self.assertEqual(proc.stdout, b"")
        self.assertIn("debug: breakpoint 1 at", stderr)
        self.assertIn("line=16", stderr)

    def test_debug_trace_is_rejected(self):
        proc = subprocess.run(
            [str(BIN), "--debug", "--trace", str(ROOT / "examples" / "hello.asm")],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(proc.returncode, 64)
        self.assertIn(b"--debug cannot be combined with --trace", proc.stderr)

    def test_debug_check_is_rejected(self):
        proc = subprocess.run(
            [str(BIN), "--debug", "--check", str(ROOT / "examples" / "hello.asm")],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(proc.returncode, 64)
        self.assertIn(b"--check cannot be combined", proc.stderr)

    def test_debug_output_stays_on_stderr(self):
        proc = self.run_debug(commands=b"c\n")
        stderr = proc.stderr.decode()
        self.assertEqual(proc.returncode, 0, stderr)
        self.assertEqual(proc.stdout, b"20\r\n")
        self.assertIn("debug:", stderr)
        self.assertNotIn(b"debug:", proc.stdout)


if __name__ == "__main__":
    unittest.main()
