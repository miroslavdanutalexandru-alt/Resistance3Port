"""Synthetic tests for scalable, table-safe PPU source discovery.

Runs without PS3 game files or native game execution.
"""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import os

ROOT = Path(__file__).resolve().parents[1]


def load_tool(name):
    path = ROOT / "tools" / name
    spec = importlib.util.spec_from_file_location(name[:-3], path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class ChunkDiscoveryTests(unittest.TestCase):
    def test_variable_chunk_count_and_table_exclusion(self):
        filenames = (
            "ppu_recomp_000.cpp", "ppu_recomp_009.cpp",
            "ppu_recomp_100.cpp", "ppu_recomp_223.cpp",
            "ppu_recomp_999.cpp", "ppu_recomp_1000.cpp",
            "ppu_recomp_table.cpp", "ppu_recomp_0extra.cpp",
            "ppu_recomp_223.cpp.backup", "other.cpp"
        )
        with tempfile.TemporaryDirectory() as tmp:
            for filename in filenames:
                (Path(tmp) / filename).write_text("// synthetic\n", encoding="utf8")
            expected = sorted(str(Path(tmp) / n) for n in filenames[:6])
            for script in ("regen_common.py", "ppu_hooks.py"):
                with self.subTest(script=script):
                    found = load_tool(script).ppu_chunks(tmp)
                    self.assertEqual(found, expected)

    def test_hook_application_in_high_numbered_chunk(self):
        # Synthetic C++ only: ensure a hook in chunk 223 is applied, not lost
        # because older tools only discovered files beginning with "0".
        import json
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / "ppu_recomp_000.cpp").write_text(
                '#include "ppu_recomp.h"\nvoid func_00000001(ppu_context* ctx) {\n  untouched;\n}\n',
                encoding="utf8")
            target = directory / "ppu_recomp_223.cpp"
            target.write_text(
                '#include "ppu_recomp.h"\nvoid func_1234ABCD(ppu_context* ctx) {\n  old_value;\n}\n',
                encoding="utf8")
            (directory / "ppu_recomp_table.cpp").write_text(
                "// function table should not be modified\n", encoding="utf8")
            hooks = directory / "hooks.json"
            hooks.write_text(json.dumps({
                "funcs": {"func_1234ABCD": [{
                    "at": 1, "ctx": "void func_1234ABCD(ppu_context* ctx) {",
                    "rm": ["  old_value;"], "add": ["  fixed_value;"]
                }]},
                "preamble": [], "header": None
            }), encoding="utf8")
            load_tool("ppu_hooks.py").apply(str(directory), str(hooks))
            self.assertIn("fixed_value;", target.read_text(encoding="utf8"))
            self.assertNotIn("old_value;", target.read_text(encoding="utf8"))
            self.assertIn("function table", (directory / "ppu_recomp_table.cpp").read_text())
    def test_release_windowed_resolution(self):
        # This choice is for Windowed mode, not 5120x2160 internal rendering.
        data = json.loads((ROOT / "recomplauncher.json").read_text(encoding="utf8"))
        options = next(x for x in data["settings"] if x["id"] == "window")["choices"]
        matches = [x for x in options if x["set"]["window"] == "5120x2160"]
        self.assertEqual([x["label"] for x in matches], ["5120 x 2160 (ultrawide)"])
        cpp = (ROOT / "project" / "launcher.cpp").read_text(encoding="utf8")
        self.assertIn('{"5120 x 2160 (ultrawide)", "5120x2160"}', cpp)

    def test_prebuilt_ppu_object_discovery_uses_numeric_chunk_filter(self):
        text = (ROOT / "build_cpu.bat").read_text(encoding="utf8")
        self.assertIn("Get-ChildItem recompiled -Filter 'ppu_recomp_*.cpp'", text)
        self.assertIn(r"Name -match '^ppu_recomp_[0-9]+\.cpp$'", text)

    def test_spu_worker_override(self):
        # When upstream's SPU extraction lands, this test follows its new module.
        spu_source = "r3_spu.py" if (ROOT / "tools" / "r3_spu.py").exists() else "r3_regen.py"
        code = (ROOT / "tools" / spu_source).read_text(encoding="utf8")
        self.assertEqual(code.count("ThreadPoolExecutor(spu_worker_count())"), 2)
        mod = load_tool(spu_source)
        with mock.patch.object(mod.os, "cpu_count", return_value=16):
            with mock.patch.dict(os.environ, {"R3_SPU_WORKERS": ""}):
                self.assertEqual(mod.spu_worker_count(), 16)
            with mock.patch.dict(os.environ, {"R3_SPU_WORKERS": "4"}):
                self.assertEqual(mod.spu_worker_count(), 4)
            with mock.patch.dict(os.environ, {"R3_SPU_WORKERS": "64"}):
                self.assertEqual(mod.spu_worker_count(), 16)
            for invalid in ("0", "-1", "abc"):
                with self.subTest(invalid=invalid):
                    with mock.patch.dict(os.environ, {"R3_SPU_WORKERS": invalid}):
                        with self.assertRaises(ValueError):
                            mod.spu_worker_count()
        with mock.patch.object(mod.os, "cpu_count", return_value=None):
            with mock.patch.dict(os.environ, {"R3_SPU_WORKERS": ""}):
                self.assertEqual(mod.spu_worker_count(), 4)


if __name__ == "__main__":
    unittest.main()
