import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "macsm"


class DiagnosticsTests(unittest.TestCase):
    def run_source(self, source, args=()):
        with tempfile.NamedTemporaryFile("w", suffix=".asm", delete=False) as tmp:
            tmp.write(textwrap.dedent(source).lstrip())
            tmp_path = Path(tmp.name)
        try:
            return subprocess.run(
                [str(BIN), *args, str(tmp_path)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
        finally:
            tmp_path.unlink(missing_ok=True)

    def test_check_mode_reports_columns_carets_and_near_match_hints(self):
        proc = self.run_source(
            """
            INCLUDE Irvine32.inc
            .data
            mesage BYTE "x",0
            .code
            main PROC
                mvo edx, OFFSET message
                call WriteStrng
                jmp Dnoe
            Done:
                exit
            main ENDP
            END main
            """,
            args=("--check",),
        )
        out = proc.stdout.decode()
        self.assertEqual(proc.returncode, 1, out)
        self.assertIn("Compatibility check: unsupported", out)
        self.assertIn(":6:5: error: instruction: mvo", out)
        self.assertIn(":7:10: error: call: WriteStrng", out)
        self.assertIn(":8:9: error: label: Dnoe", out)
        self.assertIn("source:", out)
        self.assertIn("^", out)
        self.assertIn("did you mean 'mov'?", out)
        self.assertIn("did you mean 'WriteString'?", out)
        self.assertIn("did you mean 'Done'?", out)

    def test_fatal_parse_errors_use_shared_caret_format(self):
        proc = self.run_source(
            """
            INCLUDE Bad.inc
            .code
            main PROC
                exit
            main ENDP
            END main
            """
        )
        err = proc.stderr.decode()
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn(":1:9: error: parse: only the built-in Irvine32.inc include is supported - Bad.inc", err)
        self.assertIn("source: INCLUDE Bad.inc", err)
        self.assertIn("^", err)


if __name__ == "__main__":
    unittest.main()
