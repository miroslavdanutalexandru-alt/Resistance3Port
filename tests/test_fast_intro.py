"""Synthetic-only tests. No game files or game-derived data are bundled."""
import struct
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.install_fast_intro import (
    ARCHIVE_REL, MEMBER, OVERRIDE_REL, UnsupportedGameFile,
    extract_member, patch_frontend, run, main,
)


def synthetic_swf():
    pool = ["intro_cards", "insomLogo", "sonyLogo", "autosaveCard", "seizureCard"]
    fields = struct.pack("<H", len(pool)) + b"".join(
        name.encode("ascii") + b"\0" for name in pool
    )
    consts = b"\x88" + struct.pack("<H", len(fields)) + fields
    intro = (
        b"\x96" + struct.pack("<H", 15)
        + b"".join(b"\x08" + bytes([i]) for i in range(len(pool)))
        + b"\x07" + struct.pack("<i", 4)
    )
    actions = consts + intro + b"\x42\x3c\x00"
    tag = (12 * 64 + 63).to_bytes(2, "little") + len(actions).to_bytes(4, "little") + actions
    body = b"\x08\x00\x00\x18\x01\x00" + tag + b"\x00\x00"
    return b"FWS\x09" + (8 + len(body)).to_bytes(4, "little") + body


def synthetic_psarc(frontend, compressed):
    import zlib
    manifest = (MEMBER + "\n").encode("utf-8")
    chunks = [manifest, frontend]
    packed = [(zlib.compress(chunk) if compressed and len(zlib.compress(chunk)) < len(chunk) else chunk) for chunk in chunks]
    sizes = [len(packed[i]) if len(packed[i]) < len(chunks[i]) else 0 for i in range(len(chunks))]
    toc = 32 + 2 * 30 + 2 * 2
    offsets = [toc, toc + len(packed[0])]
    records = [
        b"\0" * 16
        + i.to_bytes(4, "big")
        + len(chunk).to_bytes(5, "big")
        + offsets[i].to_bytes(5, "big")
        for i, chunk in enumerate(chunks)
    ]
    header = struct.pack(">4s7I", b"PSAR", 0x10004, 0x7A6C6962, toc, 30, 2, 65536, 0)
    return header + b"".join(records) + struct.pack(">2H", *sizes) + b"".join(packed)


class FastIntroTests(unittest.TestCase):
    def test_shipped_tools_script_defaults_to_install_root(self):
        from tools import install_fast_intro
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / ARCHIVE_REL
            source.parent.mkdir(parents=True)
            source.write_bytes(synthetic_psarc(synthetic_swf(), False))
            fake_script = root / "tools" / "install_fast_intro.py"
            with mock.patch.object(install_fast_intro, "__file__", str(fake_script)):
                self.assertEqual(main(["--dry-run"]), 0)
            self.assertFalse((root / OVERRIDE_REL).exists())

    def test_swfs_are_the_same_size_and_second_patch_fails_closed(self):
        original = synthetic_swf()
        modified = patch_frontend(original)
        self.assertEqual(len(original), len(modified))
        self.assertEqual(original[:8], modified[:8])
        self.assertNotEqual(original, modified)
        with self.assertRaises(UnsupportedGameFile):
            patch_frontend(modified)

    def test_unknown_card_layout_is_rejected(self):
        original = synthetic_swf().replace(b"autosaveCard", b"otherScreenX")
        with self.assertRaises(UnsupportedGameFile):
            patch_frontend(original)

    def test_psarc_reader_handles_compressed_and_plain_blocks(self):
        for compressed in (False, True):
            with self.subTest(compressed=compressed), tempfile.TemporaryDirectory() as temp:
                archive = Path(temp) / "test.psarc"
                asset = synthetic_swf()
                archive.write_bytes(synthetic_psarc(asset, compressed))
                self.assertEqual(extract_member(archive, MEMBER), asset)

    def test_install_verify_remove_and_original_archive_is_never_edited(self):
        for compressed in (False, True):
            with self.subTest(compressed=compressed), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                source = root / ARCHIVE_REL
                source.parent.mkdir(parents=True)
                archive_data = synthetic_psarc(synthetic_swf(), compressed)
                source.write_bytes(archive_data)
                mod = root / OVERRIDE_REL
                self.assertEqual(run(root, "dry-run"), 0)
                self.assertFalse(mod.exists())
                self.assertEqual(run(root, "install"), 0)
                self.assertEqual(mod.read_bytes(), patch_frontend(synthetic_swf()))
                self.assertEqual(run(root, "install"), 0)
                self.assertEqual(run(root, "verify"), 0)
                self.assertEqual(source.read_bytes(), archive_data)
                self.assertEqual(run(root, "remove"), 0)
                self.assertFalse(mod.exists())
                self.assertEqual(source.read_bytes(), archive_data)

    def test_generated_cache_removed_without_deleting_other_cache_files(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / ARCHIVE_REL
            source.parent.mkdir(parents=True)
            archive_data = synthetic_psarc(synthetic_swf(), False)
            source.write_bytes(archive_data)
            self.assertEqual(run(root, "install"), 0)
            cache = root / "mods" / ".cache"
            cache.mkdir(parents=True)
            obsolete = cache / "usrdir_packed_game_global_cached.psarc.123456789abcdef0.psarc"
            unrelated = cache / "other_game.psarc.12345678.psarc"
            obsolete.write_bytes(b"synthetic cache")
            unrelated.write_bytes(b"unrelated cache")
            self.assertEqual(run(root, "verify"), 0)
            self.assertTrue(obsolete.exists())
            self.assertEqual(run(root, "remove"), 0)
            self.assertFalse(obsolete.exists())
            self.assertEqual(unrelated.read_bytes(), b"unrelated cache")
            self.assertEqual(source.read_bytes(), archive_data)

    def test_atomic_replace_failure_leaves_no_partial_mod(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / ARCHIVE_REL
            source.parent.mkdir(parents=True)
            source.write_bytes(synthetic_psarc(synthetic_swf(), True))
            target = root / OVERRIDE_REL
            with mock.patch("tools.install_fast_intro.os.replace", side_effect=OSError("simulated replace failure")):
                with self.assertRaises(OSError):
                    run(root, "install")
            self.assertFalse(target.exists())
            self.assertFalse(list(target.parent.glob("*.tmp")))
            self.assertEqual(run(root, "install"), 0)
            self.assertEqual(run(root, "verify"), 0)

    def test_existing_customization_is_never_overwritten_or_deleted(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / ARCHIVE_REL
            source.parent.mkdir(parents=True)
            source.write_bytes(synthetic_psarc(synthetic_swf(), False))
            mod = root / OVERRIDE_REL
            mod.parent.mkdir(parents=True)
            mod.write_bytes(b"custom mod")
            cache = root / "mods" / ".cache"
            cache.mkdir(parents=True)
            stale = cache / "usrdir_packed_game_global_cached.psarc.cached.psarc"
            stale.write_bytes(b"cache retained on failed removal")
            with self.assertRaises(FileExistsError):
                run(root, "install")
            with self.assertRaises(FileExistsError):
                run(root, "remove")
            self.assertEqual(mod.read_bytes(), b"custom mod")
            self.assertTrue(stale.exists())


if __name__ == "__main__":
    unittest.main()
