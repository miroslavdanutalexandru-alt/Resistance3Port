"""Synthetic tests for the prebuilt PPU object scan, the ultrawide window size
and the R3_SPU_WORKERS override.

Runs without PS3 game files or native game execution.
"""
import importlib.util
import io
import json
import os
import re
from contextlib import redirect_stdout
from pathlib import Path
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]


def load_tool(name):
    path = ROOT / "tools" / name
    spec = importlib.util.spec_from_file_location(name[:-3], path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class ChunkDiscoveryTests(unittest.TestCase):
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
        m = re.search(r"\$_\.Name -match '([^']+)'", text)
        self.assertIsNotNone(m)
        pattern = re.compile(m.group(1))
        names = ["ppu_recomp_000.cpp", "ppu_recomp_027.cpp", "ppu_recomp_100.cpp", "ppu_recomp_1000.cpp",
                 "ppu_recomp_table.cpp", "ppu_recomp_0extra.cpp", "ppu_recomp_223.cpp.backup"]
        self.assertEqual([n for n in names if pattern.search(n)], names[:4])

    def test_spu_worker_override(self):
        code = (ROOT / "tools" / "r3_spu.py").read_text(encoding="utf8")
        self.assertEqual(code.count("ThreadPoolExecutor(spu_worker_count())"), 2)
        mod = load_tool("r3_spu.py")
        with mock.patch.object(mod.os, "cpu_count", return_value=16):
            with mock.patch.dict(os.environ, {"R3_SPU_WORKERS": ""}):
                self.assertEqual(mod.spu_worker_count(), 16)
            with mock.patch.dict(os.environ, {"R3_SPU_WORKERS": "4"}):
                self.assertEqual(mod.spu_worker_count(), 4)
            with mock.patch.dict(os.environ, {"R3_SPU_WORKERS": "64"}):
                self.assertEqual(mod.spu_worker_count(), 16)
            for invalid in ("0", "-1", "abc"):
                with self.subTest(invalid=invalid):
                    out = io.StringIO()
                    with mock.patch.dict(os.environ, {"R3_SPU_WORKERS": invalid}), redirect_stdout(out):
                        self.assertEqual(mod.spu_worker_count(), 16)
                    self.assertIn("WARNING", out.getvalue())
        with mock.patch.object(mod.os, "cpu_count", return_value=None):
            with mock.patch.dict(os.environ, {"R3_SPU_WORKERS": ""}):
                self.assertEqual(mod.spu_worker_count(), 4)
            with mock.patch.dict(os.environ, {"R3_SPU_WORKERS": "8"}):
                self.assertEqual(mod.spu_worker_count(), 8)


if __name__ == "__main__":
    unittest.main()
