# Resistance 3 PC port: community beta release notes

**Version:** 0.3.1-beta (GitHub pre-release). **Release date:** 2026-11-25.

A native PC port of *Resistance 3* (PS3) built by static recompilation: the game's PowerPC and SPU code is translated to C/C++ and compiled for Windows. Graphics go through RPCS3's Vulkan renderer, built into the game as a library.

> **No game files and no prebuilt game are included.** This release is the source code plus Setup. You need your own legally dumped copy of the game (see *Install*); Setup builds the game on your PC from that copy.

This is a **beta**. I have played through the intro and the opening Haven level myself, on one PC (Ryzen 5 5600X, GeForce RTX 5070, Windows 11); community testers have played further into the game. Expect bugs, and please report them (see *Reporting problems*).

## 0.3.1-beta

Fixes for the black screen in the Haven town fight, the black / purple flashes when shooting, AMD GPUs and controllers, the European disc (experimental) and a lot of new options. Items marked *Not yet tested* are new and have not been played with yet; please report how they work.

**Updating from 0.3.0:** use *Update now* in the launcher, or run the new Setup over your install (same folder). Your settings, saves and game copy are kept. Setup only generates the SPU half of the game code again (about a minute) and rebuilds what changed, so the update takes minutes, not the better part of an hour. The Haven fix below needs that step: if you update a git checkout by hand, run `Setup.bat` afterwards.

### New
- **European disc (BCES01118, 1.00): experimental, not yet tested on a real EU disc.** Setup, the installer and `setup_linux.sh` now accept the EU disc as well as the US one. Setup identifies the program by its RPCS3 PPU hash (US `502feb23...`, EU `2701cf37...`), builds the game from your own copy's program, generates the code again when you switch discs, and stops with a clear message on an unknown program or a game update. The game logs the detected copy at start (`[boot] disc: ...` in `logs\run.log`) and asks before running an unknown program. Saves follow the disc (`BCES01118_SAVE_*` for EU); after a disc switch the launcher offers once to copy the other disc's saves (the originals stay). *If you have the EU disc, please report Setup's `EBOOT.elf` line and the `[boot] disc` and `[patch]` lines of `logs\run.log`.*
- **More game languages in the launcher and F9:** Danish, Finnish, Norwegian and Swedish were added to *Game language* (Italian, German, French, Spanish, Portuguese, Polish, Russian and the others were already there). You don't need the EU disc for another language: the US disc already has the text of all of them and speech in English, French, Italian, German, Spanish, Portuguese, Polish, Russian and Japanese, so with either disc just pick yours in the launcher's *Game language* (`PS3_LANG`).
- **Aim assist option.** Launcher > Controls > Aiming and sticks > **Aim assist**, or in game with F9 or Options > *PC Controls & HUD*, where a change takes effect at once and is saved for the launcher too: *Auto* (the default) turns the game's aim assist off while you play with keyboard & mouse and keeps it with a controller; *On* keeps it always, as on PS3; *Off* turns it off for everyone. Aim assist is the slowdown when your aim passes over an enemy and the pull toward it. In split-screen, *Auto* only turns it off for the keyboard & mouse player (also with *Player 2 uses keyboard and mouse*). Enemies aim as before. `PORT_AIM_ASSIST=on` / `off` (unset = Auto). *Not yet tested in play; please report how it feels.*
- **Single-player cheats: God mode, Infinite ammo, Infinite grenades** (as in the Resistance 1 and 2 ports). Launcher > Advanced > **Cheats (single player)**, or in game with F9, where they turn on and off at once. God mode keeps your health full and stops damage; Infinite ammo keeps the magazine and spare ammo of every weapon you carry from going down; Infinite grenades keeps your grenade counts. Optional hotkeys (row *Cheat hotkeys*): Ctrl+F1 God mode, Ctrl+F2 Infinite ammo and grenades. They never work online or in a benchmark run. Once you use one, trophies are not awarded until you restart the game. `logs\run.log` shows `[cheat]` lines. `PORT_CHEAT_GODMODE=1`, `PORT_CHEAT_AMMO=1`, `PORT_CHEAT_GRENADES=1`, `PORT_CHEAT_KEYS=1`. *Not yet tested in play; please report whether each one works (and any death you still get with God mode on).*
- **Options to hide the game's own crosshair and its hit markers (issue #40).** The *Crosshair* row in F9 and Options > PC Controls & HUD was the port's *extra* crosshair (a dot or cross drawn on top of the game), so turning it off left the game's own crosshair lines on screen. That row is now called **Extra crosshair**, and two new rows hide what the game itself draws: **Game crosshair** (`R3_RETICLE=0`) and **Hit markers** (`R3_HITMARKER=0`). Both are in the launcher (Controls > Aiming and sticks), in F9 and in Options > PC Controls & HUD, and take effect at once. Scope overlays (Marksman, Deadeye and the like) are not affected. The extra crosshair's *size* row moved out of Options > PC Controls & HUD to make room (still in F9 and the launcher). `logs\run.log` shows `[hud] ... ->` lines when the game's crosshair or a hit marker is hidden. *Not yet tested in play; please report whether every weapon's crosshair goes away.*
- **Build and play on Linux through Proton (experimental).** `Setup.exe` / `Setup.bat` need Windows, so they can't run under Wine or Proton (Resistance 1 port issue #30). `setup_linux.sh` now does the same steps with bash and Python (game check and copy, UDF images through 7-Zip included; decryption; code generation; FFmpeg), and `build_linux.sh` builds the game, the launcher and HDUIPack with clang-cl against the Windows SDK and runtime fetched by xwin. The result runs under Proton. Under Wine the launcher doesn't offer *Update now*; update with `git pull` and `setup_linux.sh`. `PORT_XWIN_DIR` uses your own SDK folder (it is never deleted). See the README's Linux section.
- **Pick the GPU by name:** `RSX_ADAPTER=<name>` (as listed in `logs\rpcs3_rsx.log`), for example to run a hybrid laptop on its integrated GPU.

### Controls
- **Keyboard keys now follow the game's own controller setup.** Choosing another preset in the game's Options > Controls (for example fire on R2 and zoom on L2) or changing buttons on its *Customize* screen used to leave the keyboard unchanged: keys sent the same PS3 button as before, so they did the action of the new layout, not the one their launcher row names. Now each key keeps doing what its launcher row says (Fire fires, Crouch crouches, Sprint sprints) whatever the in-game layout; controllers are unchanged. The keys pick the layout up once you are back in play (not while you are still in a menu), and `logs\run.log` shows a `[controls] keys follow ...` line when they do. `PORT_KBM_LAYOUT=off` turns this off (keys always send the default PS3 buttons). An older keyboard-swap routine copied from Resistance 2 never ran in Resistance 3; it is only kept for `PORT_KBM_LAYOUT=old`.
- **Launcher Controls page:** the last key of a game action can no longer be cleared by accident (right-click shows a message instead), and right-clicking an empty row puts its default key back.
- **Weapon wheel on keyboard & mouse: every slot can be picked** (reported in a video review: "can't select Marksman"). The wheel only reads the left stick, pushed far out. The mouse drove only the right stick, and about 0.2 s after the wheel opened the port took it for a menu: WASD then pressed the d-pad and the stick dropped to centre, so diagonal slots like the Marksman (lower right) could not be reached. Now, while Q is held, moving the mouse toward a slot selects it and the selection stays when the mouse stops; WASD (diagonals included) work too; the view holds still; and the choice is still there in the frame you let go of Q, which is when the game takes it. A quick tap of Q still switches to the previous weapon. `PORT_KBM_WHEEL=0` restores the old behaviour. *Not yet tested in play; please report how it works.*
- **Mouse look can no longer reverse itself in fights.** The mouse's turn direction was learnt from how the view moved, so recoil, hits, aim assist and scripted turns could flip it mid-fight (`[mouselook] yaw direction now ...` in `logs\run.log`; seen in Resistance 2). It now follows the game's own *Invert* options directly, every frame; `mouselook_r3.cfg` keeps only which turn rate is horizontal. `R3_ML_LEARN=1` brings learning back (with the stricter rule contributed by @pclapida in PR #32: only a clear stick deflection held for a quarter of a second counts, so fast reversals don't flip it; `R3_ML_LEARN=legacy` = the older rule), `R3_ML_LEARN=0` fixes the direction (from `mouselook_r3.cfg`, or normal).
- **No more split-screen from a second controller or a virtual copy of your controller (issues #36, #37).** With DSX (or DS4Windows / Steam Input) running next to the default SDL3 controller support, the game saw both the real DualSense and the tool's virtual controller, treated them as two players, and pressing START could start split-screen. Now:
  - **Local co-op is off by default.** The game sees one player however many controllers are connected, and player 1 is **the controller you used last**: pick up any connected controller and press a button. To play split-screen, set launcher > Controls > **Local co-op (split-screen)** to **On** (`PAD_COOP=1`). The *Player 2 controller* and *Player 2 uses keyboard and mouse* options apply when it is on. Keyboard and mouse work as before.
  - **Better duplicate detection:** a controller that connects while another one is already playing (for example DSX's virtual DualShock 4 or Xbox pad, or a Steam Input pad) only joins once it is used on its own; if it copies several different inputs of the other controller, it is ignored (two real controllers simply held the same way are not mistaken for copies, and an ignored controller comes back when you press several different buttons on it while the other controller is untouched; it is then still watched, so a copy that comes back is ignored again). `PAD_DEDUP=0` turns this off. *Not yet tested with DSX; please report how it works.*
- **DualSense / DualShock 4 not detected: clearer `logs\run.log`.** You don't need DSX or DS4Windows: with *Controller support* on **SDL3** (the default), the game reads them directly over USB or Bluetooth. If one isn't seen, the log now lists controllers SDL3 found but can't use, notes when HidHide (used by DSX and DS4Windows to hide the real controller) is installed or a Steam setting may be hiding controllers, and says `controllers: XInput only` when the game was built without SDL3. If your controller only works with DSX, close DSX completely (also from the tray) and check that HidHide isn't still hiding the controller.

### Fixes
- **Black screen during the town defence in Haven**, found and fixed by @pclapida (PR #31). One entry point of an SPU job (job `E59180`, entry `0x360`) is only reached through the job's function table, so the code generator missed it and it ran in the slow SPU interpreter, which got stuck: the job never signalled that it was done and the screen stayed black. It is now translated like the rest of the job. In `logs\run.log` the freeze showed as `[wws-jobentry] pc=0x00360` followed by `[semfix] label 71`. This may also be the black screen in issue #27; if you still get it there, please report it. The fix is in the generated SPU code, so the update regenerates it (Setup or *Update now* does this).
- **Black / purple frames when shooting from the hip or aiming down sights**, fixed by @pclapida (PRs #34, #35, #38). After the fast depth-of-field path gave up, later frames skipped the safety checks of the PS3 path and could read the lighting before it was written; hip-fire frames (which have the blur turned off) made the fast path give up early and produced garbled colours; and without an upscaler the fast path never started at all. Frames with the blur off now give the sharp image as on the PS3, the checks stay on after the fast path gives up, and the fast path now also works without DLSS / FSR / XeSS (`R3_DOF_COPY_PERJOB=0` = the old check). The PIC plugin jobs are now built without direct jumps between code blocks, which could freeze the game in that case (`PORT_PIC_DIRECT_HOP=ON` at build time keeps them).
- **Freeze on the ladder out of the tunnels (Death Squad checkpoint).** An SPU job called through a table slot that held data instead of code, and the SPU lane stopped for good. The port now treats such a call like the empty slot the game would skip, logs it, and carries on (`SPU_JOB_BADCALL_SKIP=0` = old).
- **Freeze in chapter 2, Haven - Evacuation (issue #42).** The same SPU job (an object's state pointed past the end of its callback table) called an address with no code at all (`[spu] img=2 call to LS 0x0008C (empty function)` then `job ended: bad call ... halting this PM run` in `logs\run.log`). The port stopped the job there, so it never reported back and the game waited for it forever. At this one call site (job 403's state callback) such a call is now skipped like the empty slot the game itself skips, the job finishes, and `logs\run.log` shows one `[spu] WARNING: ... job 403 called a bad callback ... skipped like an empty slot` line. Bad calls anywhere else are handled as before. `SPU_JOB_BADCALL_SKIP=0` = old. *Not yet tested in play; please report whether the evacuation still freezes.*
- **Cutscenes without dialogue (subtitles only) with Windows or the launcher's *Game language* set to Dutch, Chinese, Finnish, Swedish, Danish or Norwegian.** For these languages the game's movie player plays the English voice-over itself, but the port's language fallback (new in 0.3.0) still renamed the English voice tracks to the missing language, so the player found no voice track and the cutscene (for example Malikov and Capelli at the start of Haven) played music and effects only. Those languages now keep the movies as they are (`PS3_LANG_TITLE_ENGLISH`, default `6,9-15`; `none` = the 0.3.0 behaviour). English (UK), Portuguese (Brazil) and Turkish, for which the game really asks for a voice track the disc lacks, still get the English one. `logs\run.log` now has a `[dialogue]` line for every movie (language, the disc's audio tracks, any renamed track) and audio port (how it is mixed to stereo) with `PORT_DIALOGUE_DIAG=1`.
- **Judder and doubled frames on screens where G-Sync / FreeSync isn't really working** (for example a 165 Hz laptop panel). The game's own pacing was even, but 60 or 120 frames a second on a 165 Hz screen that refreshes at a fixed rate are shown for an uneven number of refreshes. New launcher option Display > Frame rate > **Display sync** (`R3_DISPLAY_SYNC`): *Auto* (the default) measures whether VRR follows the game's frames (`[display] VRR effective: yes / no` in `logs\run.log`) and, when it doesn't, switches to VSync and a frame rate that divides the refresh rate (165 Hz: 55 fps, or 41.25 rendered -> 82.5 shown with Frame Generation; 144 Hz: 72), so every frame stays on screen equally long. *VRR* keeps everything as before; *Fixed refresh* always paces that way. *Not yet tested on a fixed-refresh screen; please report how it looks and include `logs\run.log`.*
- **Audio > Speaker layout now takes effect.** The game picks its mix layout from the audio output's channel count, which was always reported as 5.1, so choosing **Stereo** (or 7.1) in the launcher changed nothing and the game always mixed in 5.1, folded down to stereo by the port (the same problem as Resistance2Port #12). The setting now sets that channel count. `logs\run.log` shows the layout the game configured (`AudioOutConfigure(audioOut=0, 2 ch, ...)`). The port also zeroes any invalid (NaN / out-of-range) samples before output and reports them in `logs\run.log`.
- **Mods folder: a mod could be overwritten by the game's own file copies.** Mod files (and the patched archives in `mods\.cache\`) were also used when the game opened a file for writing, so a game copying or saving a file the mod replaces wrote over the mod instead. In Resistance 2 this cut the PC settings menu archive short on a fresh install and the game closed right after the menu. Mod files are now only ever used for reading; the game's own writes go to the game's own files. A damaged copy in `mods\.cache\` is detected and rebuilt automatically on the next start.
- **Frame Generation: fewer stutters and doubled-looking frames in play.** When the upscaler had to skip a single frame (`[r2aa] not applied: scene surfaces were replaced before the upscaler ran` in `logs\run.log`), Frame Generation switched off for three frames and came back with a reset, which showed as a hitch with repeated frames (seen several times a minute in Resistance 2, which shares this code). A gap of one or two frames in the middle of play now resumes on the next frame (`frame generation: N-frame scene gap bridged` in `logs\run.log`); loading screens, movies and menus still pause it as before. `R3_DLSS_FG_GAP=0` restores the old behaviour.
- **AMD Radeon: stretched "ghost" copy of the 3D scene, fixed as in the Resistance 2 port (its issue #16).** AMD GPUs have no 24-bit depth buffer, so the 32-bit float one RPCS3 uses in its place kept depth values beyond the far end of the PS3 range, which the 24-bit buffer caps; the sky then failed the depth test and the previous frame's content showed through. Window depth from geometry is now capped the same way, while geometry outside the game's depth range is still cut off as on the PS3 (`RSX_D32_UNORM_Z=0` turns this off; `logs\run.log` shows a `[r2rsx] RSX_D32_UNORM_Z:` line when it applies). Depth written by shaders is now capped to [0, 1] too on surfaces that are 16- or 24-bit on the PS3: in Resistance 2 a capture showed the anti-aliasing resolve pass writing 1.0000001 for the sky, which the float buffer kept, so the sky was not drawn and a stretched copy of the scene showed there; with this cap the ghost is gone on a Radeon 780M (`RSX_D32_FRAGDEPTH_CLAMP=0` turns it off; `logs\run.log` shows a `[r2rsx] RSX_D32_FRAGDEPTH_CLAMP:` line). `logs\run.log` also has a `[r2rsx] gpu:` line with the GPU properties the AMD and NVIDIA render paths differ on; `RSX_BARY_MANUAL=1` (shader input interpolation as on NVIDIA) and `RSX_D32_EMULATE_CMP=0` (no shader depth-compare emulation) are there for A/B tests. Please report whether you saw the ghost and whether it is gone.
- **Less frame stalling on occlusion checks, and fewer false camera cuts (both ported from the Resistance 2 port).** A late occlusion result no longer holds the frame: after 2 ms the object is simply drawn (`R3_OCC_NOWAIT=0` = old). Frame Generation / DLSS / FSR no longer reset their history when the weapon was drawn last with its own depth range (`R3_FG_CAM_SANITY=0` = old). SPU job code the port has not translated now runs only the missing function in the interpreter and goes back to translated code when it returns (`SPU_UNLIFTED_SCOPED=0` = old).
- **Ultrawide and aspect ratio changes apply at once (issue #26, PR #29 by @pclapida).** Picking 21:9 / 32:9 or *Fit window* in the F10 menu or in Options used to leave the picture at 16:9 until the game was restarted; now the next frames use the new shape (`[r2rsx] R3_ULTRAWIDE: picture ...` in `logs\run.log`).
- **AMD Radeon: no more doubled background or flicker after the shader preload (issue #16).** A pipeline whose build failed in the AMD driver during the preload was skipped for the whole session, so a sky / background pass went missing and an earlier image showed through; it is now built again when the game first draws with it (`PORT_PRELOAD_FAULT_RELINK=0` = old).
- **AMD Radeon: no more long stalls when new pipelines are needed in play.** On AMD the port builds one pipeline at a time, and the ones the game needed right now waited behind the slow background builds (about 25 s on a Radeon 780M). Play-time pipelines now go first (`PORT_PIPE_SERIAL_PRIO=0` = old).
- **AMD Radeon: shader preload crashes (issue #23).** On AMD and on CPUs with few threads the start-up preload loads with 2 threads (`PORT_PRELOAD_BOOT_THREADS=N`, `PORT_PRELOAD_BOOT_AMD=0` = old), the upscaler, Frame Generation and other runtime pipelines are built one at a time with the shader pipelines (`PORT_PIPE_SERIAL_SDK=0` = old), and a crash while loading or building them is named in the crash report and skipped on the next start. The screen now switches to *Compiling 0 / N* as soon as loading ends, so a crash at that point is no longer shown as *Loading*.
- **Shader compilation is now *Synchronous* by default** (launcher Performance > Shader cache > *Shader compilation*, `R2_SHADER_MODE`). The old default, *Asynchronous*, skipped a draw while its pipeline was being built, which showed as purple / green patches or missing objects on a first visit; *Synchronous* waits for it instead (a short hitch the first time). An install that still has the old default is switched once; you can pick *Asynchronous* again.
- **Fewer first-visit hitches:** the port ships the list of pipelines the game builds only in play (`tools\pipeline_seed.bin`, render states only, no game data) and builds them during the preload (`PORT_PIPELINE_SEED=0` = off). On GPUs without 24-bit depth (AMD) those entries are converted to the depth format actually used, so they match the real draws.
- **CPUs with fewer than 8 threads (issue #33):** the RSX thread now runs at a raised priority there, as on a 4-core i5-7300HQ it only got 55-77 % of the time it needed (`PORT_RSX_PRIO=0` = off, `1` = on any CPU).
- **HDR: fewer blue / purple artifacts with HDR on (issue #13).** Invalid (NaN) colours are now zeroed before the HDR output (`PORT_HDR_SANITIZE=0` = old), and `logs\rpcs3_rsx.log` lists the HDR surface formats. Please report whether the artifacts are gone.
- **Frame Generation / DLSS: fewer smeared objects.** Trees, doors and other repeated objects could get the motion of a neighbouring copy after the draw order changed (wrong motion of hundreds of pixels); they are now paired with the nearest matching copy (`RSX_DLSS_OBJMV_PAIR_NEAREST`, default 16, `0` = off). Objects drawn late in a frame keep their own motion instead of camera-only motion (`RSX_AA_OBJMV_DEFER_SPEC=0` = off).
- **Build:** `build.bat` only closes a running game started from the same folder (it used to close any `Resistance3.exe`), and a failed link (for example because the game is still running) is reported as a failure instead of *BUILD SUCCEEDED*.

### Diagnostics
- **No diagnostic logging is on by default any more.** The release build only writes errors, warnings, one-time start-up lines and crash / hang reports to `logs\run.log`; the timing and statistics logs that earlier betas wrote all the time (and that cost a little CPU every frame) are now opt-in. If I ask you for one of them in an issue, set it in `settings.bat`: hitch logger and RSX thread CPU split `PORT_HITCH_LOG=1` (`[hitch]`), frame-time report `PORT_FRAMETIME=1` (`logs\frametimes_*.txt`), `[fps]` 5 s pacing lines `PORT_FPS_LOG=1`, RSX stall report `RSX_STALL_DIAG_MS=3000`, `[audio]` lines `PORT_AUDIO_LOG=5000` (underruns are still logged when they happen), `[dialogue]` `PORT_DIALOGUE_DIAG=1`, `[pipe]` `R2_PIPE_LOG=1`, `[occ]` `R3_OCC_LATE_LOG=1`, `[hot]` `R3_HOT_LOG=1`, `[texcache]` `RSX_TEXDIAG=1`, `[rb-attr]` `RSX_RB_ATTRIB=1`, `[ads-rb]` `RSX_ADS_RB_LOG=1`, `[spu-jobs]` `SPU_JOB_TIMING=1`, `[spuwait]` / `[spu-sched]` `PORT_SPU_SCHED_STATS=1`, `[usleep-watch]` `PS3_USLEEP_WATCH_LOG=1`, `[present]` `RSX_PRESENT_STATS=1`, `[av-who]` `R3_AV_WHO=1`, Frame Generation `R3_FG_DIAG=1`. The game also no longer reads and hashes the whole `EBOOT.elf` at every start for the `[boot] disc:` check: the result is kept in `build\r3_eboot_hash.cache` until the file changes.
- **RSX stall report, as in Resistance 2** (`RSX_STALL_DIAG_MS`, opt-in, for example `3000`): when the GPU command stream stops moving, `logs\run.log` says what it waits on (`[rsx-stall]` lines, at most 8).
- **No-flip watchdog, as in Resistance 2** (a black screen for ~20 s after dying there, which ended the moment the window lost focus). When the game shows no frame for 5 s without being paused, `logs\run.log` gets a `[noflip]` line saying what the game waits on (thread stacks in `logs\noflip_<tick>.txt`), and the port then releases all input for half a second, as an alt-tab does (only if the game is reading the pad and not loading files), and logs whether that ended the wait. If the screen stays black for a while in Resistance 3, please send `logs\run.log` and the `noflip` file. `PORT_NOFLIP_WATCHDOG=0` turns the check off, `PORT_NOFLIP_NUDGE=0` only the input release. The thread-stack reports no longer make loader calls while a thread is suspended, and only one report runs at a time, so the hang and freeze reports cannot stall the game either.
- **Black screen at the end of Haven Outskirts (issue #27): more in `logs\run.log`.** When one of the runtime's fixed tables is full (event queues, event ports, event flags, semaphores, rwlocks, timers) or guest stack space runs out, the create call now logs a `[SYS] ... FAILED: ...` line (first 4 times each) instead of failing silently. A game that ignores such a failure can wait forever, which is how the R1 movie black screen fixed in 0.3.0 happened. This changes logging only, not behaviour. If the freeze happens, wait 2 minutes so the hang watchdog writes `logs\crash\R3_*_hang.txt`, then send the launcher's *Report a problem* zip.
- **Frame Generation diagnostics** (`R3_FG_DIAG`, default off): `1` writes a per-second `[fg-diag]` line to `logs\run.log` (real and generated frames, FG resets and why, frame-time spikes) and the Pause key saves the last and next frames to `logs\fgdiag.txt`; `2` also saves them by itself on a suspect event; `3` records every frame to `logs\fgdiag.txt`. With `1` or higher there is also an `[fg-vk]` line every 10 s when FG was reset or a generated frame was lost.
- **Hang report on request (`logs\hang.now`), as in Resistance 2.** If the game is stuck but still running (a black screen with sound, or a scene that never ends), creating an empty `logs\hang.now` makes the game write a hang report (`logs\crash\R3_<time>_hang.txt`) within a second. The report lists what every game thread is waiting on, and the game keeps running. The automatic report only comes when the game loop stops for 90 s. `PORT_CRASH_EXTRA=0` or `PORT_HANG_DUMP=0` turns it off. See `docs\REPORTING.md`.

### Setup
- **Progress for every step, with time left.** Generating the game code now reports how far it is (stage by stage, function by function), and building the game shows *game code file X of N*, the file compiling right now and how long it has been at it, and a time-left estimate from how fast this PC really is. The big game code files take very different times (the first four take about 10 minutes each), so the estimate now uses each file's measured compile time instead of counting build steps; "about a minute left" with 20 minutes to go no longer happens. A *still working, last activity N s ago* note tells a slow step apart from a stuck one. One overall bar covers the whole install. When an update regenerates only part of the game code (for example only the SPU code, which takes under a minute), the time left is estimated for that part alone.
- **Setup remembers how long each step took** (`logs\setup_timings.json`, and a summary in `logs\setup_script.log`) and uses it for the next estimates. `logs\setup.log` and the build logs can now be opened while Setup is running.
- **Much faster updates.** Setup now tracks the PowerPC (PPU) and SPU halves of the generated game code separately. An update that only changes SPU code (like the job module fix in PR #31) regenerates just the SPU half (about a minute instead of 10-25), and files that come out the same as before are left untouched, so the build only recompiles what really changed. Installs from 0.3.0-beta.1 are recognised: their PPU code is kept, only the SPU half is generated again. Updating such an install should take minutes instead of the better part of an hour.
- **Add the optional upscaler / Frame Generation SDKs after installing**: run `Setup.bat -GetSdks` (or run `Setup.exe` again with the SDK box ticked). It downloads the XeSS / Streamline / DLSS files Setup can fetch and rebuilds only what uses them. The RSX core build now also notices SDK folders that were copied in by hand.
- **RPCS3 source download gets through more networks** (Resistance 2 port issue #13, which also affected this port). When the secure connection to GitHub fails (antivirus HTTPS scanning, company proxies, unreachable certificate revocation servers), Git is tried again with Windows' own certificate store and without the revocation check, and the source archives are downloaded with `curl` as a second try. A proxy set in Windows is passed on to Git and to the archive downloads. If nothing gets through, Setup prints the exact links to download in a browser and the folder to put the `.zip` files in (`tools_cache\rpcs3_download\`); the next run uses them.
- **Building with an older Windows SDK works.** 0.3.0 only built with the Windows 11 24H2 SDK (10.0.26100); with 10.0.22621 the Direct3D 12 renderer failed to compile (`D3D12_DEPTH_STENCIL_DESC2`, `D3D12_FEATURE_DATA_D3D12_OPTIONS14`, `ID3D12GraphicsCommandList8` undeclared). It now builds with any Windows SDK from 10.0.20348 on and the game works the same: the driver, not the SDK, decides whether the newer D3D12 features are used. An SDK older than that now stops the build at once with a message saying which one to install, and the build log names the SDK used (`Using Windows SDK ...`).

- **Installing or updating LLVM works again (issue #6)**, fixed by @pclapida (PR #30): the portable LLVM could not be unpacked (`could not unpack C:\WINDOWS\System32\tar.exe`), and Setup checked a different `clang-cl` than the build uses, so it kept asking to update an LLVM that was already new enough. A Python check that could return a missing `python.exe` was fixed too. The same unpack bug in the portable xwin download (Linux build) was fixed, and the build no longer needs the debug runtime (Resistance 1 port issue #16).
- **UDF-only `.iso` images are accepted** (images without the older ISO 9660 part were rejected as unreadable, although Windows can mount them). Setup now mounts and checks them like a game folder.

### Project
- **CI and release tooling**, contributed by @undecuple: a release build workflow that builds Setup, the zips and checksums with `scripts\package_beta.ps1` on a Windows runner and only ever creates a *draft* release from a version tag (PR #2); a check that no game files and no unexpected large files are committed, using the same patterns as `package_beta.ps1` (PR #19); a gitleaks secrets scan (PR #20); Dependabot updates for the pinned GitHub Actions (PR #21); and `ps3recomp/UPSTREAM_BASE`, which records the upstream ps3recomp commit range the original import came from (PR #22).
- **5120 x 2160 window size, chunk filter fix and SPU worker limit**, contributed by @Thrashmetaldinosaur (PR #39): launcher Display > Window size now offers **5120 x 2160**; `build_cpu.bat` picks game code chunks by number, so it keeps working once there are 100 or more; `R3_SPU_WORKERS=N` limits how many workers generate SPU code (an invalid value prints a warning and uses the default). The PR's chunk discovery changes to the code generator will come in a later release that regenerates the game code anyway, so this update doesn't force a full regeneration.
- **SPU code reached only through code pointers is now translated too.** The code generator also looks for SPU functions that are named only by an address in data (constructor lists, function tables) or built in registers and called indirectly, so they no longer have to run in the interpreter (`SPU_LIFT_CODE_PTRS=0` when generating turns it off; the list is in `jobmods_code_ptrs.txt`). It finds nothing new in the current game image beyond the Haven fix above; it is there so the next missed entry point doesn't need a hand-made fix.

## 0.3.0-beta.1 (hotfix)

Setup fix only; the game itself is unchanged from 0.3.0.

- **Updating over a 0.2.x install no longer fails** with `'Emu/RSX/port_texdiag.h' file not found` while building the RSX core (Resistance2Port #17, Resistance3Port #24). Setup kept the RPCS3 source folder from 0.2.x (`ps3recomp\third_party\rpcs3`) and never applied 0.3.0's patch to it. It now detects a source patched by an older version and patches it again automatically (the old changes are kept in git's stash there; a copy that is not a git checkout is downloaded again). `build.bat` also checks the patch before building.

If you installed 0.3.0 fresh and it built, you don't need this update.

## 0.3.0-beta

Performance and stability update. Items marked *untested* or *not yet tested in game* are new and have not been played with yet; please report how they work.

### Fixes
- **Hangs while aiming fixed** (a lost wake-up between game threads could freeze the game, most often when aiming down sights).
- **First aim down sights no longer stutters**: the depth-of-field setup is saved to `r3_dof_fast.cache` next to the game and reused, so it is smooth from the second run (`R3_DOF_FAST_CACHE=1`, `R3_DOF_FAST_RAMP_KEEP=1`, both default on). A rebuilt or updated game ignores an old cache file.
- **Garbage textures on bullet holes and rubble fixed** (`RSX_AV_NEIGHBOUR_SMALL_FLUSH`, default on).
- **Less microstutter** from texture uploads (`RSX_NV0039_PREINVAL`, default on).
- **Frame Generation no longer crashes at boot** on some systems; if it cannot start, the game falls back to Frame Generation off.
- With Frame Generation, the Reflex frame cap is no longer multiplied by the generated frames (`R3_DLSS_FG_CAP=1`, default; `0` restores the old behaviour).
- **Ambient occlusion now works with DLSS**, FSR and XeSS (drawn before upscaling; `RSX_SSAO_DLSS=0` turns it off under DLSS only).
- FSR 3.1 gets the real camera near/far planes and field of view, for steadier upscaling.
- **Purple flashes and popping while aiming down sights fixed**: the game's graphics memory now sits at the address the game expects (`PS3_MAPPER_BASE`, set by `run.bat` and now also the exe's own default).
- **Dark blotches on rugs and floors under DLSS fixed** (motion vectors were de-jittered with the wrong sign; `RSX_AA_DEJIT_SIGNED=1`, default; `0` = old). The DLSS effect mask that also caused blotches is now off by default (`PORT_DLSS_BIAS_MASK=1` turns it back on).
- **Frame Generation camera motion improved**: the camera is picked from all matching vertex programs (`RSX_AA_EXACT_ALL=1`, default) and a confirmed exact camera is trusted (`RSX_AA_CONF_TRUST_EXACT=1`, default), so less smearing and wobble when turning; Frame Generation also resets after scene cuts. Set either to `0` for the old behaviour.
- **HD UI pack** is made on the graphics card in one upscaler run (Real-ESRGAN x4plus, bundled), with a CPU fallback if the GPU fails; damaged textures are rejected.
- **Quieter logs**: diagnostic logging (shadow, depth, upscaler statistics and others) is off by default or summarised once a minute, so `logs\run.log` stays small. Diagnostic dumps and debug keys are off unless enabled for troubleshooting.
- **Magenta / green flashes in dark tunnels fixed**: a depth buffer with few depth writes can no longer be taken as the scene depth for DLSS / FSR (`RSX_AA_CLASSIFY_DWRITE`, default on).
- **Purple / coloured block corruption fixed** (walls, the Death Squad checkpoint; `RSX_AA_DS_LATCH`, default on), as well as the flickering blanket (issue #16), shadow flicker, the Haven carpet drawn only in part (native-scale shadow maps, `RSX_SHADOW_NATIVE`, default on) and DLSS boiling / shimmer while standing still (`RSX_AA_POST_STABLE`, default on).
- **Texture popping and blurry textures up close fixed**: the game's GPU-time governor dropped the resolution and LOD distances on frames it thought were slow; launcher *Load shedding* turns it off (`R3_NO_LOADSHED`, default on).
- **Movement fixes**: movement key presses are no longer dropped, so you don't have to press twice to start walking (`PAD_KEY_LATCH`, default on); the walk start and movement timing follow the frame rate up to 75 fps (`R3_MOVE_DT_MIN`).
- **Controls no longer invert at the shooting range**: the tutorial's look check ignored the mouse and offered *Invert Y* to every mouse player (`R3_ML_LOOK_TRACK=0` = old).
- **60 fps by default** (issue #14): *Auto-detect* no longer picks 120 fps, since movement breaks above about 75 fps; the log warns above 75 (`R3_AUTOCONFIG_120=1` = old recommendation).
- **Black screen with an incomplete game copy** (issue #4): the game checks for `game.psarc` / `PARAM.SFO` at start and says what is missing (`R3_SKIP_FILECHECK=1` skips it); Setup checks the full file list.
- **DualSense / DualShock 4 without Steam Input or DS4Windows** (issue #6): the SDL3 controller library was missing from the release.
- **Hang and crash fixes**: an occlusion result the game waits for is sent when the GPU goes idle (`RSX_ZCULL_IDLE_FLUSH`), the GPU command position is no longer reported past a label before it is written (`R3_FIFO_GET_AT_LABEL`), a file-table race crash on the movie thread is fixed (issue #11), and a black screen when the game ran out of thread slots is fixed (`PS3_THREAD_SLOT_FIX`); all on by default.
- **Closing the window ends the game** at once, and quitting no longer runs the full PS3 shutdown path that could hang (`PS3_EXIT_CRT=1` = old).
- **AMD**: a driver crash while shaders were preloaded in the background is avoided (MSAA pipelines skipped, one thread on AMD, a crashing pipeline skipped next start: `PORT_PRELOAD_ALL_MSAA`, `PORT_PRELOAD_THREADS`, `PORT_PRELOAD_QUARANTINE`, `PORT_PRELOAD_SEH`); shadows and decals get a proper depth bias on 32-bit depth (`RSX_ZBIAS_D32`, auto).
- **Garbled linear textures** fixed: linear textures are byteswapped on the CPU (`RSX_TEXDECODE_LINEAR_CPU`, default on).
- **Mouse aim learning is less eager** (`R2_ML_LEARN=0` off, `legacy` = old), and **triggers** no longer flicker near the threshold (`PAD_TRIGGER_HYST=0` = old).

### New features
- **In-launcher updates**: the *Updates* option (Advanced page) picks **Notify** (default: *Update now* / *Release page* / *Skip version*), **Auto-install** (asks once, then updates) or **Off** (`PORT_NO_UPDATE_CHECK=1`). *Update now* runs `tools\update.ps1`: it downloads the release zip (size and SHA-256 checked), replaces the port's files, runs Setup in update mode and starts the launcher again. Settings, saves, the game copy, generated code, builds and shader caches are kept; a failed update is rolled back (`logs\update.log`). `PORT_UPDATE_NOTIFY_ONLY=1` brings back the old notice. Git checkouts and RecompLauncher installs only get the notice.
- **Save backups**: each save keeps its last 5 copies in `saves_backup\` (`PORT_SAVE_BACKUP`, default on).
- **Report a problem** in the launcher packs logs and system info into a `.zip` on your Desktop.
- **PC settings in the game's Options menu** as extra tabs (`R3_MENU_ROWS`, default on; `0` off, `2` rows at the end of CONTROLS).
- **PS3-style trophy pop-ups** with the trophy's own icon and name (`PORT_TROPHY_STYLE=old` for the classic look).
- The **cutscene skip hint follows Skip unwatched movies**: *Show cutscene skip hint* **Auto** (default) shows it when *Skip unwatched movies* is on, since every story movie can then be skipped (`PORT_SKIP_HINT=1` always, `0` never). *Not yet tested in game.*
- **Skip unwatched movies**: holding Space / Enter / A / Menu for 1 s now also skips pre-rendered movies you have not watched before (the game normally allows it only for seen ones). The port tells the game's own movie code that the movie was seen while the hold is active, so the game ends it exactly like a normal skip. On by default; launcher / F10 *Skip unwatched movies* (`R3_CUTSCENE_SKIP_ALL=0` turns it off). Needs *Hold to skip movies*. Start-up logo and menu movies are not affected. Skipped movies count as watched (as after a normal skip). If the game has not taken the held button after 3 s, the port sets the game's skip flag itself (`PORT_SKIP_FORCE=1`, default; `0` off). `logs\run.log` says how each movie ended. *Not yet tested in game.*
- **Photo mode** hides the HUD (`R3_PHOTO_HIDE_HUD=1`, default).
- **Language fallback**: English (UK) has no voice-over on the disc, so cutscenes and speech play the English voices and keep the UK text (`PS3_LANG_VOICE_FALLBACK`, default `1`; `2` = any file missing its language, `0` = off).
- **Online (experimental, off)**: `PORT_ONLINE`, see `online.ini.example`.
- If the game freezes, it writes a freeze report and restarts itself (`FREEZE_RELAUNCH`, set in `run.bat`; `0` turns it off).
- **Hold Esc for 1 s** to open the PC menu (`R3_ESC_HOLD_MENU=0` off); the F10 hint box is gone.
- **Save location** (launcher Advanced › Saves): game folder (default), *Saved Games\Resistance 3* or a folder you pick (`PS3_GAMEDATA_ROOT`).
- **Frame-time benchmark**: F11 starts / stops a capture (`R3_BENCH_KEY`, default F11; `F6`, `F5`, `off`) written to `logs\bench\`; a frame-time report (`logs\frametimes_*.txt`) is written at exit (`PORT_FRAMETIME=0` off).

### Controls
- New controller options: outer, anti- and trigger deadzones and turn boost (`PAD_OUTER_DEADZONE`, `PAD_ANTI_DEADZONE`, `PAD_TRIGGER_DEADZONE`, `PAD_TURN_BOOST`, all off by default), stick squircle (`PAD_SQUIRCLE`, default 4000), rumble strength (`PAD_RUMBLE`, default 100).
- Crouch and sprint as toggle or hold (`PAD_CROUCH_MODE`, `PAD_SPRINT_MODE`, default as the game; experimental).
- Menu confirm button (`PS3_ENTER_BUTTON=circle`; default Cross) and background input (`PAD_BACKGROUND_INPUT`, default auto).
- All controllers drive player 1 (`PAD_MERGE=1`, off by default because of co-op).

### Graphics
- **Custom crosshair** (`R3_CROSSHAIR=dot|cross`, off by default; `R3_CROSSHAIR_COLOR`, `R3_CROSSHAIR_SIZE`).
- **Sharper HUD and menus** at high resolutions (`RSX_HD_UI`, off by default).
- **Ultrawide** (`R3_ULTRAWIDE=auto|21:9|32:9`, off), **field of view** camera fix (`R3_FOV_SAFE`) and **level of detail distance** (`R3_LOD_SCALE`, default Original). *Untested.*
- Overlay and menu scale (`PORT_UI_SCALE`, default Auto) and a frame-time graph in the F3 overlay (`PORT_OVERLAY_GRAPH`, on).
- **Aim-down-sights blur** option (`R3_FX_ADS_BLUR=0` turns the zoom blur off, so aiming costs about the same as normal play).
- **Ultrawide HUD** (`RSX_HUD_FIX=1`, experimental: HUD in a centred 16:9 area), **Brightness** (`PS3_DISPLAY_GAMMA`) and **texture detail / LOD bias** (`R2_LOD_BIAS`) options.
- **DLSS preset** option (`RSX_DLSS_PRESET`); Auto picks the CNN preset E on RTX 20 / 30, where the transformer model costs much more (`PORT_DLSS_AUTO_ARCH=0` = NVIDIA's default everywhere).
- **Frame generation backend** option: Auto (DLSS-G, else FSR 3 2x), DLSS only or FSR 3 (`R3_FG_BACKEND`).
- **Latency reduction**: `PORT_LATENCY=off|reflex|antilag|auto`, including **AMD Anti-Lag 2** on Radeon; unset = the Reflex setting alone decides.
- **Direct3D 12 renderer** (`R2_RSX=d3d12`) for testing only; Vulkan stays the default.

### Audio
- Adaptive audio buffer that grows only when needed (`PORT_AUDIO_ADAPTIVE=1`, default; up to `PORT_AUDIO_BUFFER_MAX_MS=300`).
- Elastic audio timing (`PORT_AUDIO_ELASTIC`, default off, experimental).
- Master volume (`PORT_VOLUME`, default 100) and mono audio (`PORT_AUDIO_MONO=1`).
- Steadier sound: clock-drift correction (`PORT_AUDIO_DRIFT`), silence instead of looping sound while paused or stalled (`PORT_AUDIO_PAUSE_FREEZE`, `PORT_AUDIO_STALL_FREEZE_MS`), a fresh restart after alt-tab or the PC menu (`PORT_AUDIO_FOCUS_REPRIME`) and sound coming back after the audio device changes (`PORT_AUDIO_REOPEN`); all on by default.

### Performance
- **Optimised build**: `build.bat` uses the included profile (`pgo\r3.profdata`) for a faster game (`PORT_PGO=0` or `build.bat nopgo` opts out). It needs **LLVM (clang-cl) 22 or newer**; Setup updates an older LLVM, and `build.bat` stops with a clear message if it finds one.
- Native PC versions of the depth-of-field blur and other game jobs (`R3_NATIVE_DOF=1`, `R3_NATIVE_JOBS=1`, both default on), including the Bink movie decoder's IDCT (part of `R3_NATIVE_JOBS`).
- **Native audio**: the game's audio mixing jobs run as native PC code (`R3_NATIVE_AUDIO_JOB=1`, default; `0` = the recompiled SPU code). The aim blur uses a fast PC approximation (`R3_DOF_GPU_APPROX=1`, default; `0` is the exact PS3 result).
- Known shaders are preloaded at start (`PORT_SHADER_PRELOAD=1`, default).
- Renderer recording on its own thread (`RSX_RECORD_THREAD`, default off, experimental).
- **Faster renderer**: scaled depth copies are one GPU blit (`RSX_DEPTH_BLIT_DIRECT`, default on), the DLSS feature is reused when the render size changes (`RSX_AA_DYNRES`), occlusion counts use RPCS3's recommended *Approximate* mode (`RSX_ZCULL_COUNT=precise` = old) and many renderer fast paths.
- **Thread placement**: on Intel hybrid CPUs the busy game threads prefer P-cores (`PORT_CPU_PCORES`, on); optional raised priority (`PORT_THREAD_PRIORITY`, off); game threads opt out of Windows power throttling (`PORT_ECOQOS=1` keeps the Windows default).
- Lower CPU use while waiting (`PORT_NOSPIN`, default on).

### Settings and compatibility
- **Portable build tools** (experimental): Setup can put the build tools only into `tools_cache\` in the port folder (no installers, no administrator rights; `Setup.bat -PortableTools`, or **P** / **No** at Setup's question). `build.bat` uses them automatically. See [INSTALL.md](INSTALL.md#portable-build-tools-experimental).
- **Settings carry over**: `settings.ini` keys renamed to the names shared by the three Resistance ports are migrated automatically (the old line is kept for one release).
- **CPU check**: on a CPU without AVX2 the launcher explains the problem and uses the slower compatibility build (SSE4.1) that Setup builds for it, instead of crashing.
- Successful file opens in `logs\run.log` are now capped (failures are always logged; `PS3_FSLOG=1` logs all).
- **Setup and build fixes**: the build no longer fails on the FFmpeg headers when the folder path contains spaces; the RPCS3 source download retries and handles exFAT drives and Git's "dubious ownership" error (issue #5); after running out of memory Setup retries with fewer files at a time; it suggests a Microsoft Defender exclusion and warns about slow USB / exFAT / network drives; `tools\build_doctor.ps1` repairs a broken build configuration.

### Known issues
- **Frame Generation is still experimental.** Some artifacts remain: jitter on grass / vegetation and around doors and entrances, and some ghosting. These are being fixed in an upcoming hot patch.

### Support
This port is free and always will be. If you enjoy it and feel like buying me a coffee, it's genuinely appreciated: [buymeacoffee.com/damiro98](https://buymeacoffee.com/damiro98). A tip is not a payment for the game or this port, and nothing is unlocked by it.

## 0.2.3-beta (last hotfix)

This is the last hotfix. The next updates focus on performance and stability, based on your reports and playthroughs.

- **Setup works without winget.** If winget is missing or fails on your PC, Setup downloads the official installers of the build tools (Visual Studio Build Tools, CMake, Ninja, LLVM, Python, Git) directly, checks them (SHA-256 or Microsoft's signature) and installs them.
- **RecompLauncher support.** The port can be installed, set up and played from [RecompLauncher](https://recomplauncher.com): it checks your copy, builds the game, shows the main video settings on the game's page and starts the game. Your own Launcher.exe and Setup.exe work as before.
- The release zip is now named `...-Windows-x64.zip`.
## 0.2.2-beta (setup hotfix)

- **Fixed: the build could fail right away with a Ninja/CMake error** ("build.ninja still dirty", or CMake re-running in a loop) for players in time zones behind ours. Files from the download could be dated a few hours in the future. The installer and zips now carry correct times, and the build resets any future-dated files before it starts.

If your setup failed this way: install 0.2.2 (or wait a few hours) and run Setup again; it continues where it stopped.

## 0.2.1-beta (setup hotfix)

Setup fixes only; the game itself is unchanged from 0.2.0.

- **Progress bar during the build.** The bar no longer stops partway while the game compiles; it follows the build to the end, so Setup no longer looks stuck.
- **Setup shows that it is working:** a running clock for each step, the game copy in GB with the time left, the time left while the game compiles, and a "still working" note during steps that print nothing for several minutes.
- **The installer now opens the Setup window** (with the progress bar and your game already filled in) instead of a console window. Click **Install** there.
- **Much smaller build logs.** The compiler was set to report every possible warning, which made `logs\build.log` grow past 150 MB and slowed the build. It now reports normal warnings only.
- **Low-RAM PCs:** if the build runs out of memory, Setup again detects it and retries with fewer parallel jobs (this check could not read the build log in 0.2.0).

If 0.2.0 is already built and runs, you don't need this update.

## What's in this beta

### Launcher
- `Launcher.exe` holds all settings and key bindings. **Auto-detect** checks your CPU, GPU and screen and picks recommended settings.
- Pages: Display, Graphics, Upscaling & Frame Gen, Performance, Controls, Audio, Interface & Notifications, Advanced, **Trophies**, **Saves & tools** (save backup and restore) and About (open the logs, saves and screenshots folders, report a problem). `Launcher.exe --page N` opens one (0–10).
- Includes a log button for bug reports, and an update check at start that can update the port in place (*Update now*, see 0.3.0-beta). The *Updates* option on the Advanced page picks Notify (default), Auto-install or Off (`PORT_NO_UPDATE_CHECK=1`).

### Graphics
- **Upscaling and anti-aliasing:**
  - NVIDIA DLSS / DLAA on GeForce RTX cards. This is experimental, see *Known issues*.
  - AMD FSR 3.1 on any GPU.
  - Off.
  - If DLSS can't start, for example on a non-NVIDIA GPU, the port falls back to FSR 3.1.
- **Intel XeSS 2 (experimental):** pick *Intel XeSS* in the launcher (Compatible renderer; `RSX_FSR=4`). It runs on any GPU. If it can't start, or on the native renderer, the port uses FSR 3.1. Setup downloads the XeSS SDK after you accept Intel's licence.
- **Render resolution:** from 720p (PS3 native) up to 3840x2160 (`RSX_RES_SCALE`). Upscaling ratios are Native, 67, 58, 50 and 33 % (`RSX_UPSCALE`).
  - The default is the *1080p rendered → DLSS 4K* preset. On a 1080p screen the result is supersampled.
- **Frame rate targets:** 30, **60** (default), 120, 144, *Match display*, *Just under refresh* (for G-Sync / FreeSync) and Uncapped (`R2_FPS`).
  - The game clock follows real time, so 60 fps and above don't speed the game up.
  - An optional lower cap applies in menus and cutscenes (`PORT_MENU_FPS`).
- **VRR / G-Sync fix:** VSync *Auto* and *Adaptive* now use FIFO or FIFO-relaxed presentation instead of MAILBOX. G-Sync / FreeSync doesn't follow MAILBOX.
  - The *Presentation mode* option (`R2_PRESENT=immediate|mailbox|fifo`) overrides this.
  - The *Just under refresh* frame rate with *Immediate* gives tear-free VRR without VSync latency.
- **Refresh rate switch:** in exclusive fullscreen, the display switches to the refresh rate that best fits the frame rate cap. Experimental and off by default (`PORT_REFRESH_SWITCH=1`): every switch blanks the screen and can glitch audio.
- **Shader precompile:** with *Compile shaders before playing* (default on), every known pipeline is built before the first frame and a *Compiling shaders N / M* panel shows progress. Before Play, the launcher also runs the offline shader scan (shaderscan) with its own progress panel.
  - Shader caches are built on your PC and never shipped.
  - The cache is backed up to `%LOCALAPPDATA%\Resistance3Port\shadercache`.
- **Frame Generation** (experimental, off by default; Upscaling & Frame Gen page, `R3_DLSS_FG`, `R3_FG_BACKEND=auto|dlss|fsr3`): NVIDIA DLSS Frame Generation 2x on an RTX 40, up to 4x on an RTX 50 (needs NVIDIA DLSS); other GPUs get AMD FSR 3 2x.
- **Optional extras:**
  - HDR output (`R3_HDR`; experimental, off by default, needs Windows HDR)
  - NVIDIA Reflex (`R3_REFLEX`; experimental, off by default)
  - depth-of-field, film grain and motion-blur toggles (`R3_FX_*`; on by default, which is the game's own look)
  - colour-blind filters (`R3_COLORBLIND`)

### Controls
- **Mouse and keyboard:** raw mouse look (it doesn't emulate the stick) and rebindable keys (`controls.txt`, `mouselook.cfg`).
- **Controllers through SDL3:** Xbox, PlayStation (DualShock 4 / DualSense), Switch Pro and others (`PAD_BACKEND=sdl3`, the default; `xinput` is still available).
  - Optional gyro aiming (`PAD_GYRO`).
  - Light bar and Nintendo button layout options.
- **Button prompts follow your device** (`R2_PAD_GLYPHS=auto`): keyboard keys, or Xbox / PlayStation buttons. You can force one in the launcher.
- **Local split-screen co-op** for two players: two controllers, or one controller plus keyboard and mouse.

### PC features
- **PC menu:** F10, or hold View + Menu on a controller. It has Resume, PC Options and **Quit to Desktop** (with confirmation, and it waits for a save in progress to finish).
- **PC wording:** text that refers to the console is changed to PC terms. For example, "Press and hold the PS button…" becomes "Press F10 to quit the game". Only the text in memory changes. `R3_PC_TEXT=0` turns this off.
- **Screenshots** with F12 or PrtScn go to `screenshots\`. **F3** toggles the performance overlay.
- **Menus, overlay and notifications are drawn into the game image** (`PORT_OSD`), so they also show in exclusive fullscreen. The movie-skip hint can be turned off (*Show cutscene skip hint*, `PORT_SKIP_HINT=0`).
- Alt+Enter toggles windowed mode. You can choose the monitor, and pause or mute when the game is in the background.
- **Trophies** are recorded locally, with on-screen notifications. A **mods folder** loads replacement game files.
- **Crash and hang reports** are written to `logs\crash\`.

### Audio
- **Time stretching** (SoundTouch, `PORT_AUDIO_STRETCH=1`, default on): when the frame rate drops, audio slows slightly instead of crackling or gapping.

### Diagnostics
- **Hitch logger** (`PORT_HITCH_LOG=1`, off by default since 0.3.1; threshold `PORT_HITCH_MS`, default 25 ms): each slow frame writes one `[hitch]` line to `logs\run.log` showing where the time went. Please include it when you report stutter.

## What you download

- **`Resistance3Port-Setup-<version>.exe`** from the GitHub release: an installer with the port's source, scripts and `Launcher.exe`. It installs the files and starts Setup.
- or the **source code** (the release's source archive or `git clone` at the tag), then `Setup.bat`.

Either way, the game itself is built on your PC; no built game program is published. Setup downloads the build tools, RPCS3's source, FFmpeg (an LGPL build), shaderscan and, if you accept their licences, the NVIDIA DLSS, Streamline (DLSS Frame Generation) and Intel XeSS SDKs.

## Requirements

- Windows 10 or 11, 64-bit.
- A CPU with **AVX2** (Intel 4th generation or newer, or any AMD Ryzen) for the normal build. On older CPUs with SSE4.1 only, Setup also builds a slower compatibility build and the launcher uses it automatically. The game is mostly CPU-bound, so a faster CPU helps the most.
- A **Vulkan 1.3** GPU with a current driver. DLSS needs a GeForce RTX card. FSR 3.1 works on AMD, NVIDIA and Intel. I test on NVIDIA; AMD and Intel GPUs have only been tested by players, and their reports drive the AMD fixes.
- The Microsoft Visual C++ 2015–2022 x64 runtime, version 14.40 or newer.
- Your own copy of **Resistance 3, US disc (BCUS98176), version 1.00**. The European disc (BCES01118, 1.00) is accepted from 0.3.1 but is experimental (not yet tested on a real EU disc). Other regions, digital (PSN) copies and patched versions are not supported.
- About **50 GB** of free disk space: about 37 GB for the game files plus the build and the shader cache.
- For the source / installer setup: an internet connection and the free build tools. Setup installs the tools itself.

## Install

1. **Dump your own game.** Use a decrypted ISO, or a game folder containing `PS3_GAME` (the same kind of folder RPCS3 uses), from your own disc. Encrypted ISOs need decrypting first, for example with PS3Dec and the disc key. Don't use compressed archives, PKG files or update folders.
2. **Install the port.** Either:
   - run the installer, `Resistance3Port-Setup-<version>.exe`, which installs to `C:\Games\Resistance3Port` by default and then asks for your game; or
   - download the source code (the release's source archive, or `git clone` at the release tag) into a folder **without spaces** (for example `C:\Games\Resistance3Port`) and double-click `Setup.bat`.
3. **Setup** looks for your game and lists the copies it finds (common folders and RPCS3's game list). You can also enter a path, browse for it, or run `Setup.bat -Game "<ISO or folder>"`. It then:
   - checks the copy (title ID, version, size and the known file hash) and explains what is wrong if it can't be used;
   - copies or moves it to `gamefiles\PS3_GAME` inside the port folder;
   - decrypts the game's program, `EBOOT.BIN`, to `game\EBOOT.elf`, using the public application keys from RPCS3's source, and checks it is the supported version;
   - generates and builds the game-specific code from your copy. This takes 20–60 minutes the first time.
4. **Play** with `Launcher.exe`.

**How the port finds your game:**
- `run.bat`, which the launcher starts, runs the game with `game\EBOOT.elf` and points the game's virtual disc at `gamefiles` (`PS3_VFS_ROOT`).
- Saves go in `gamedata\dev_hdd0\home\00000001\savedata` (`PS3_GAMEDATA_ROOT` moves them).
- Settings and controls are also copied to `%USERPROFILE%\Saved Games\Resistance 3\config`. Put an empty `portable.txt` next to `Launcher.exe` to turn that off.
- Logs are in `logs\`.

**First launch:** before the game starts, the launcher prepares the shader cache from your game files (a progress panel; **Skip for now** or Esc skips it). The game then compiles its known shaders behind a **Compiling shaders N / M** panel, which can take a minute or two. Later starts are fast. Updating your GPU driver rebuilds the cache once.

## Known issues

- **DLSS is experimental.** Some textures can still shuffle or shimmer, and dark patches can appear on some surfaces. Fixes are in progress. If they bother you, use AMD FSR 3.1 in the launcher.
- **Some heavy fights can slow down.** The first aim down sights after a fresh install or update can still stutter once; from the second run it is smooth (see 0.3.0-beta). Please send `logs\run.log` if you see slowdowns.
- **Frame Generation is experimental and barely tested** (DLSS on RTX 40/50, FSR 3 elsewhere; off by default). Expect ghosting, jitter on grass / vegetation and around doors and entrances, and HUD or menu artifacts; I'm working on fixes, and reports are welcome (`R3_FG_DIAG`, see 0.3.1-beta, helps).
- **Changing Frame Generation in-game** can double the sound and then leave constant static. Change it in the launcher, or restart the game after changing it.
- **Occasional microstutter**, even when the shader cache is complete. If you see it, please send the `[hitch]` lines from `logs\run.log`.
- **The first launch prepares the shader cache**: a shader scan panel, then a **Compiling shaders N / M** panel, so the first start takes noticeably longer (see *Install*).
- 120 fps and above are less tested than 60. If physics or a scripted sequence looks wrong, try 60 or 30 fps.
- Exclusive fullscreen can give black F12 screenshots, and the screen goes black briefly at start and when switching windows. Borderless avoids both.
- Occasional freezes or crashes are possible, especially past the opening level, which I haven't played through myself yet (testers have).
- Ultrawide (`R3_ULTRAWIDE`, 21:9 / 32:9) is off by default, so those screens show the 16:9 picture with side bars. It has been tried in game by one tester (21:9); please report how it looks on yours.
- Everything else: **TBD after testing**.
- What's planned next: [ROADMAP.md](ROADMAP.md).

## Reporting problems

Please read **[REPORTING.md](REPORTING.md)** before reporting. In short: say what you did and how to reproduce it, and attach the session's log from `logs\` (the launcher has a button for it) plus any crash or hang report in `logs\crash\`.

## Licences

The port is MIT-licensed (see `LICENSE`). Its RPCS3 patch is GPL-2.0, and the game you build links RPCS3, so that build is GPL-2.0; don't redistribute it with the DLSS files. See `THIRD_PARTY_NOTICES.md` for the full list. This port uses NVIDIA DLSS. *Resistance 3* is a trademark of its owners. This project is not affiliated with or endorsed by them.
