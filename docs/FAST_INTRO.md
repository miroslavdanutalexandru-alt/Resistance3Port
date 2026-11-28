# Fast Intro (optional)

**Fast Intro** removes three redundant presentation cards from Resistance 3's startup:
the Insomniac logo, Sony logo and autosave notice. It **keeps the epilepsy
warning**. It does not bypass required engine initialization, the game's
installation confirmation, or saved-game loading.

This is a reversible **Mods-folder override**. No copyrighted game assets,
complete SWF files, saves or translated game code are included in this
repository. The script extracts the frontend from **your own installed game**,
changes one small ActionScript instruction, and writes the replacement
inside the game's Mods folder. The original archive is never edited.

## Requirements

- Your own supported Resistance 3 installation (US disc, BCUS98176, 1.00).
- A port build with **Display > PC > Mods folder** support.
- Python 3.10 or later.
- Close the game before installing, verifying or removing the mod.

This was tested with the frontend from the supported US disc and a
0.3.1-beta port build. The installer deliberately rejects unexpected
ActionScript layouts rather than guessing where to patch.

## Install

From a clone or download of this repository, open PowerShell and run:

    py -3 tools\install_fast_intro.py --game-dir "C:\Games\Resistance3Port" --dry-run
    py -3 tools\install_fast_intro.py --game-dir "C:\Games\Resistance3Port"

Replace the example path with **your port installation directory**, the folder
containing Launcher.exe and gamefiles. If `tools\install_fast_intro.py` is
already inside that installed directory, `--game-dir` can be omitted; by
default it uses the parent folder of `tools`. A dry-run checks compatibility
and reports what would be generated without writing anything.

Open **Launcher.exe**, go to **Display > PC > Mods folder**, select **On**,
and save the settings. The game reads the mod on the next launch.
Check logs\run.log for an entry containing [mods] R3-FastIntro.

The first modded launch may build a cached PSARC in mods\.cache, which can use
several hundred megabytes of disk space. Later launches reuse the cache.

## Verify or remove

    py -3 tools\install_fast_intro.py --game-dir "C:\Games\Resistance3Port" --verify
    py -3 tools\install_fast_intro.py --game-dir "C:\Games\Resistance3Port" --remove

The verify option compares the generated replacement against the installed mod.
The remove option deletes **only** the Fast Intro replacement if it still
exactly matches the expected output, then removes empty directories belonging
to it. On a successful removal, the script also clears generated
`mods\.cache\usrdir_packed_game_global_cached.psarc.*.psarc` cache files,
which can be about 430 MB. If another mod overrides the same archive, its
cache may need to be rebuilt on the next game start. Customized or unrecognized
Fast Intro replacements are not overwritten or deleted.

The installer does not change launcher preferences or other mods. Turn Mods
off in the launcher separately if desired.

## Expected behavior and limits

- With Fast Start and automatic Continue already configured in the test
  installation, the first saved-game level archive opened at **about 29.9 s**
  instead of **37.9 s**. The no-save Campaign selection appeared at about
  **32.7 s** instead of **40.7 s**. These are startup milestones, not a
  guarantee of fully rendered gameplay at that instant; performance varies.
- The warning and installation confirmation remain. Fast Intro alone does
  **not** automate menus or load the latest save. It can combine with other
  port features that do so.
- The game's PSARC is read-only to this installer; it does not read or edit
  game saves.
- The script targets only the normal `<game>\mods` folder (`R2_MODS=1`).
  Custom `R2_MODS=<folder>` locations are not supported by this installer;
  use the normal Mods folder to install, verify or remove Fast Intro.
- Installation writes a complete temporary SWF and atomically replaces the
  target; interrupted writes cannot leave a partial final mod file.
- If another mod replaces the same global_cached.psarc frontend member,
  mod load order determines which one takes effect.
- If the game data differs from a recognized layout, the installer fails
  closed. Do not force it onto another game region or updated disc without
  compatibility tests.

The automated tests in tests/test_fast_intro.py use **synthetic** SWF/PSARC
fixtures only. No game content is distributed with the tests or installer.
