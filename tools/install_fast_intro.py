#!/usr/bin/env python3
"""Build a reversible Fast Intro mod from a user's own Resistance 3 files.

No game assets are distributed by this script. The original PSARC and SWF
are read but never edited; the output is an optional per-member mod override.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys
import tempfile
import zlib
from pathlib import Path

ARCHIVE_REL = Path("gamefiles/PS3_GAME/USRDIR/packed/game/global_cached.psarc")
MEMBER = "/ui/screens/frontend/frontend.swf"
OVERRIDE_REL = Path(
    "mods/R3-FastIntro/USRDIR/packed/game/global_cached.psarc/"
    "ui/screens/frontend/frontend.swf"
)
TESTED_SOURCE_SHA256 = "d6edfba8c5de4bdf0481689543ae2b405f9c484661ab33668ed161b7f22314d5"
TESTED_OUTPUT_SHA256 = "828a05129e4fc08aac7430a39e7df85f931f0199bbe1327b073c371cdd08dedf"


class UnsupportedGameFile(ValueError):
    """The user's own files do not match a safely recognized layout."""


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def extract_member(archive: Path, member: str) -> bytes:
    """Read one PSARC member, without repacking or altering its archive."""
    with archive.open("rb") as f:
        header = f.read(32)
        if len(header) != 32:
            raise UnsupportedGameFile("Truncated PSARC header")
        magic, _version, compression, toc, entry_size, count, block_size, _flags = (
            struct.unpack(">4s7I", header)
        )
        if (magic != b"PSAR" or compression != 0x7A6C6962
                or entry_size != 30 or block_size != 65536
                or not 1 < count < 200_000 or not 32 + count * 30 <= toc < 100_000_000):
            raise UnsupportedGameFile("Unsupported PSARC archive format")
        file_size = archive.stat().st_size
        if toc >= file_size or (toc - 32 - count * 30) % 2:
            raise UnsupportedGameFile("Invalid PSARC table size")

        entries = []
        for _ in range(count):
            record = f.read(30)
            if len(record) != 30:
                raise UnsupportedGameFile("Truncated PSARC file table")
            entries.append((
                int.from_bytes(record[16:20], "big"),
                int.from_bytes(record[20:25], "big"),
                int.from_bytes(record[25:30], "big"),
            ))
        block_lengths = [
            size[0] for size in struct.iter_unpack(">H", f.read(toc - 32 - count * 30))
        ]

        def read_entry(index: int, limit: int) -> bytes:
            start_block, unpacked_size, offset = entries[index]
            if unpacked_size > limit or offset < toc or offset >= file_size:
                raise UnsupportedGameFile("Unexpected PSARC entry size/offset")
            num_blocks = (unpacked_size + block_size - 1) // block_size
            if start_block + num_blocks > len(block_lengths):
                raise UnsupportedGameFile("PSARC block table is too short")
            f.seek(offset)
            output = bytearray()
            for block in range(num_blocks):
                needed = min(block_size, unpacked_size - len(output))
                encoded = block_lengths[start_block + block] or needed
                if encoded <= 0 or encoded > block_size:
                    raise UnsupportedGameFile("Unexpected PSARC block size")
                payload = f.read(encoded)
                if len(payload) != encoded:
                    raise UnsupportedGameFile("Truncated PSARC member")
                if encoded < needed:
                    try:
                        payload = zlib.decompress(payload)
                    except zlib.error as error:
                        raise UnsupportedGameFile("Invalid PSARC compression") from error
                if len(payload) != needed:
                    raise UnsupportedGameFile("Unexpected decompressed block length")
                output.extend(payload)
            return bytes(output)

        try:
            names = read_entry(0, 5_000_000).decode("utf-8").splitlines()
        except UnicodeError as error:
            raise UnsupportedGameFile("Invalid PSARC manifest encoding") from error
        if len(names) != count - 1 or names.count(member) != 1:
            raise UnsupportedGameFile("Frontend file not found uniquely in PSARC")
        return read_entry(names.index(member) + 1, 3_000_000)


def constant_pool(action_code: bytes) -> list[str]:
    """Read the ActionScript 1/2 constant pool from the frontend DoAction tag."""
    if len(action_code) < 5 or action_code[0] != 0x88:
        raise UnsupportedGameFile("Unexpected frontend ActionScript layout")
    length = struct.unpack_from("<H", action_code, 1)[0]
    end = 3 + length
    if end > len(action_code):
        raise UnsupportedGameFile("Truncated ActionScript constant pool")
    count = struct.unpack_from("<H", action_code, 3)[0]
    index = 5
    strings = []
    for _ in range(count):
        stop = action_code.find(b"\0", index, end)
        if stop == -1:
            raise UnsupportedGameFile("Truncated ActionScript string")
        try:
            strings.append(action_code[index:stop].decode("utf-8"))
        except UnicodeError as error:
            raise UnsupportedGameFile("Invalid ActionScript string") from error
        index = stop + 1
    if index != end:
        raise UnsupportedGameFile("Unexpected ActionScript constant-pool bytes")
    return strings


def patch_frontend(swf: bytes) -> bytes:
    """Keep the epilepsy warning; omit three redundant branding/notice cards.

    Only the validated 18-byte AVM1 ActionPush instruction is changed.
    The replacement is assembled from ActionScript opcodes and the local
    file's constant-pool indexes, not copied game-derived binary content.
    """
    if len(swf) < 16 or swf[:3] != b"FWS":
        raise UnsupportedGameFile("Expected an uncompressed Flash frontend")
    if struct.unpack_from("<I", swf, 4)[0] != len(swf):
        raise UnsupportedGameFile("Unexpected SWF declared length")
    bits = swf[8] >> 3
    position = 8 + (5 + 4 * bits + 7) // 8 + 4  # RECT + frame rate/count
    replacements = []

    while position + 2 <= len(swf):
        tag_header = struct.unpack_from("<H", swf, position)[0]
        tag, size = tag_header >> 6, tag_header & 63
        header_size = 2
        if size == 63:
            if position + 6 > len(swf):
                raise UnsupportedGameFile("Truncated SWF tag")
            size = struct.unpack_from("<I", swf, position + 2)[0]
            header_size = 6
        begin = position + header_size
        end = begin + size
        if end > len(swf):
            raise UnsupportedGameFile("SWF tag exceeds file boundary")
        if tag == 12:  # DoAction
            actions = swf[begin:end]
            if actions.startswith(b"\x88") and b"intro_cards\0" in actions:
                pool = constant_pool(actions)
                names = (
                    "intro_cards", "insomLogo", "sonyLogo",
                    "autosaveCard", "seizureCard",
                )
                if any(pool.count(name) != 1 for name in names):
                    raise UnsupportedGameFile("Unexpected frontend card names")
                indexes = [pool.index(name) for name in names]
                if any(i >= 256 for i in indexes):
                    raise UnsupportedGameFile("Unsupported card index encoding")
                original_push = (
                    b"\x96" + struct.pack("<H", 15)
                    + b"".join(b"\x08" + bytes([i]) for i in indexes)
                    + b"\x07" + struct.pack("<i", 4)
                )
                if actions.count(original_push) != 1:
                    raise UnsupportedGameFile("Frontend intro array has changed")
                at = actions.index(original_push)
                if actions[at + len(original_push):at + len(original_push) + 2] != b"\x42\x3c":
                    raise UnsupportedGameFile("Frontend array initializer has changed")
                warning_only = (
                    b"\x96" + struct.pack("<H", 9)
                    + b"\x08" + bytes([indexes[0]])
                    + b"\x08" + bytes([indexes[4]])
                    + b"\x07" + struct.pack("<i", 1)
                )
                # Maintain the exact byte length. Jump over one inert Pop.
                padding = b"\x99" + struct.pack("<Hh", 2, 1) + b"\x17"
                replacement = warning_only + padding
                if len(replacement) != len(original_push):
                    raise AssertionError("Replacement length must be unchanged")
                replacements.append((begin + at, original_push, replacement))
        position = end
        if tag == 0:
            break

    if len(replacements) != 1:
        raise UnsupportedGameFile("Could not uniquely identify the frontend intro array")
    position, original, replacement = replacements[0]
    if swf[position:position + len(original)] != original:
        raise UnsupportedGameFile("Original intro instruction mismatch")
    result = swf[:position] + replacement + swf[position + len(original):]
    if len(result) != len(swf):
        raise AssertionError("Patch must not resize any SWF tags")
    if sha256(swf) == TESTED_SOURCE_SHA256 and sha256(result) != TESTED_OUTPUT_SHA256:
        raise AssertionError("Known game build did not produce verified mod")
    return result


def atomic_install(target: Path, contents: bytes) -> None:
    """Commit a complete replacement only after the file has been fully written.

    The temporary file shares the target directory, so os.replace is atomic
    on the destination filesystem. Interrupted writes never leave a partial SWF
    at the final target path.
    """
    target.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(prefix=target.name + ".", suffix=".tmp", dir=target.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(contents)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, target)
    finally:
        temporary.unlink(missing_ok=True)


def delete_generated_psarc_cache(game_dir: Path) -> int:
    """Remove generated cache for this archive; never touch original game data."""
    cache_dir = game_dir / "mods" / ".cache"
    deleted = 0
    for cached in cache_dir.glob("usrdir_packed_game_global_cached.psarc.*.psarc"):
        if cached.is_file():
            cached.unlink()
            deleted += 1
    return deleted


def run(game_dir: Path, mode: str) -> int:
    game_dir = game_dir.resolve()
    archive = game_dir / ARCHIVE_REL
    mod = game_dir / OVERRIDE_REL
    if not archive.is_file():
        raise FileNotFoundError(
            f"Missing original game archive: {archive}\n"
            "Pass --game-dir pointing to your installed Resistance3Port folder."
        )
    original = extract_member(archive, MEMBER)
    generated = patch_frontend(original)
    digest = sha256(generated)
    existing = mod.read_bytes() if mod.is_file() else None

    print(f"Game directory: {game_dir}")
    print(f"Generated mod SHA-256: {digest}")
    if mode == "dry-run":
        print(f"Dry run only; no files written. Target: {mod}")
    elif mode == "verify":
        if existing is None or existing != generated:
            raise UnsupportedGameFile("Fast Intro mod missing or does not match source game")
        print("Verified: the installed Fast Intro mod matches the original game.")
    elif mode == "install":
        if existing is not None and existing != generated:
            raise FileExistsError(
                f"Mod already exists and differs: {mod}\n"
                "Refusing to overwrite an unknown or customized asset."
            )
        if existing == generated:
            print("Already installed; nothing changed.")
        else:
            atomic_install(mod, generated)
            print(f"Installed: {mod}")
        print("Enable 'Mods folder' under Display > PC in Launcher.exe, then start the game.")
        print("First modded launch may build a cached PSARC; later launches reuse it.")
    elif mode == "remove":
        if existing is None:
            print("No Fast Intro mod file found; nothing to remove.")
        elif existing != generated:
            raise FileExistsError("Refusing to delete a mod file with unrecognized changes")
        else:
            mod.unlink()
            stop_at = game_dir / "mods"
            folder = mod.parent
            while folder != stop_at and folder.is_relative_to(stop_at):
                if any(folder.iterdir()):
                    break
                folder.rmdir()
                folder = folder.parent
            print("Removed Fast Intro mod. Original game archive was never modified.")
            count = delete_generated_psarc_cache(game_dir)
            print(f"Removed {count} generated global_cached.psarc cache file(s).")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--game-dir", type=Path, default=Path(__file__).resolve().parents[1],
        help="Installed Resistance3Port directory (default: parent of tools/)",
    )
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--dry-run", action="store_true", help="Generate and validate without writing")
    group.add_argument("--verify", action="store_true", help="Check an existing mod against the game")
    group.add_argument("--remove", action="store_true", help="Remove only an exact matching Fast Intro mod")
    args = parser.parse_args(argv)
    mode = "remove" if args.remove else "verify" if args.verify else "dry-run" if args.dry_run else "install"
    try:
        return run(args.game_dir, mode)
    except (OSError, UnsupportedGameFile, AssertionError, struct.error) as error:
        print(f"Fast Intro: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
