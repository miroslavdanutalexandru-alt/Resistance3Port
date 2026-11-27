# Third-party notices: Resistance 3 PC port (0.3.1-beta)

**How this port is distributed.** The release is source code plus Setup: this repository at the release tag (or the GitHub source archive), and `Resistance3Port-Setup-<version>.exe`, an installer that contains the same files plus the port's own `Launcher.exe`. **No built game program, no game data, and no NVIDIA, Intel or FFmpeg binaries are distributed by this project.** Setup runs on your PC: it reads your own copy of the game, downloads the optional SDKs below from their publishers (after you accept their licences) and builds `build\Resistance3.exe` there.

*Resistance 3* is © Sony Interactive Entertainment, developed by Insomniac Games. The game files, the decrypted program and the code generated from it come only from your own copy and stay on your PC. This is an unofficial fan project, not affiliated with or endorsed by Sony, Insomniac, NVIDIA, AMD or Intel.

The port's own code is MIT (see `LICENSE`).

## Shipped in the repository and the installer

| Component | Licence | Licence file | Where |
|---|---|---|---|
| The port's code, scripts and `Launcher.exe` | MIT | `LICENSE` | `project\`, `tools\`, root |
| ps3recomp (runtime, PS3 libraries, lifters) by Ned Heller / sp00nznet | MIT | `ps3recomp\LICENSE` | `ps3recomp\` |
| Techniques adapted from the inFAMOUS 2 PC port (https://github.com/SamiEnazi/infamous2-port), Copyright (c) 2026 Sami Enazi (its `engine\` is ps3recomp, MIT, Ned Heller / sp00nznet): the adaptive SPU wait spin (`PORT_SPU_SPIN_ADAPT`), the physical-core scarcity check (`PORT_CORE_SCARCE`) and, in Fall of Man, the frame-clock kick wait (`RSX_SPINWAIT=4`). Re-implemented in `ps3recomp\runtime\spu\spu_channels.c`, `ps3recomp\runtime\syscalls\sys_ppu_thread.c` and `project\main.cpp`. | MIT | the MIT notice below | `ps3recomp\runtime\`, `project\` |
| Elastic audio consumption (`PORT_AUDIO_ELASTIC`, `ps3recomp\libs\audio\audio_elastic.h`) adapted from the GoW Ascension recomp (https://github.com/bc160201983/GoW-Ascension-Recomp, commit `1be0537`), Copyright (c) 2026 BilalReloaded (its `ps3kit\` is ps3recomp, MIT, Ned Heller / sp00nznet) | MIT | below, and in the header | `ps3recomp\libs\audio\` |
| AMD FidelityFX SDK 1.1.4 (FSR 3.1 upscaling and FSR 3 frame interpolation / frame-generation swapchain), Vulkan: `amd_fidelityfx_vk.dll` and the unmodified `ffx_api` headers | MIT | `tools\ffx_vk\LICENSE.txt` | `tools\ffx_vk\` |
| AMD FidelityFX SDK (FSR 3.1 upscaler 4.1.1, frame generation 4.0.1, loader 2.3.0), Direct3D 12: unmodified `amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll`, `amd_fidelityfx_framegeneration_dx12.dll`, used only by the experimental Direct3D 12 renderer | MIT | `tools\ffx_dx12\LICENSE.txt` | `tools\ffx_dx12\` |
| AMD FidelityFX CACAO (https://github.com/GPUOpen-Effects/FidelityFX-CACAO; ambient occlusion, `R3_AO_MODE=cacao`), ported by the port to GLSL in `VKR2Cacao.h` (part of the RPCS3 patch, `ps3recomp\patches\`) | MIT (Copyright (c) 2020-2021 Advanced Micro Devices, Inc.; Copyright (c) 2016 Intel Corporation) | the MIT notice at the top of the file | GLSL in `VKR2Cacao.h` (part of the RPCS3 patch, `ps3recomp\patches\`) |
| RPCS3 patch (the changes to RPCS3's RSX core and Vulkan renderer) | **GPL-2.0** | `ps3recomp\third_party\rpcs3\LICENSE` (after Setup fetches RPCS3) | `ps3recomp\patches\` |
| SDL3 (controllers; linked statically) | Zlib | `ps3recomp\third_party\SDL3\LICENSE.txt` | `ps3recomp\third_party\SDL3\` |
| SoundTouch 2.4.1 (audio time stretching; linked statically) | LGPL-2.1 | `ps3recomp\third_party\soundtouch\COPYING.TXT` | `ps3recomp\third_party\soundtouch\` |
| at3_standalone (FFmpeg's ATRAC3 / ATRAC3plus decoders, from PPSSPP commit `ebc151f`; linked statically) | LGPL-2.1-or-later | `ps3recomp\third_party\at3_standalone\COPYING.LGPLv2.1` | `ps3recomp\third_party\at3_standalone\` |
| Valve Fossilize (pipeline recording), with RapidJSON, miniz and dirent | MIT (RapidJSON: MIT with BSD / JSON notes) | `ps3recomp\third_party\fossilize\LICENSE` and the files in its subfolders | `ps3recomp\third_party\fossilize\` |
| FFmpeg headers (`libavcodec`, `libavutil`, ...) | LGPL-2.1-or-later | the header of each file | `ffmpeg\include\` |
| HD UI pack upscaler, `tools\hdui_upscaler\` (run by `HDUIPack.exe` as a separate program): `realesrgan-ncnn-vulkan.exe` (the graphics card, the default) and `realesrgan-ncnn-cpu.exe` (CPU only, the fallback), built for the port from Real-ESRGAN-ncnn-vulkan (https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan, commit `37026f4`, patched; source in `tools\hdui_upscaler\src\`, with file helpers from https://github.com/nihui/realsr-ncnn-vulkan) against Tencent ncnn 20261020 (https://github.com/Tencent/ncnn, commit `7e36d25`; the Vulkan build also contains glslang); `vcomp140.dll`; the `realesr-animevideov3-x4` model from Real-ESRGAN (https://github.com/xinntao/Real-ESRGAN, unchanged from its `realesrgan-ncnn-vulkan-20220424-windows.zip`) | Real-ESRGAN-ncnn-vulkan: MIT, © 2021 Xintao Wang; realsr-ncnn-vulkan: MIT, © 2019 nihui; ncnn: BSD-3-Clause, © 2017 Tencent (its third-party parts as listed in its licence); glslang: BSD-3-Clause and the other licences in its file; the model: **BSD-3-Clause**, © 2021 Xintao Wang; `vcomp140.dll`: Microsoft Visual C++ redistributable (Visual Studio licence) | `tools\hdui_upscaler\LICENSE.*.txt`, `tools\hdui_upscaler\models\LICENSE.Real-ESRGAN.txt`, `tools\hdui_upscaler\README.txt` | `tools\hdui_upscaler\` |

## Fetched or downloaded by Setup on your PC (not distributed by us)

| Component | Licence | Licence file after Setup | Notes |
|---|---|---|---|
| RPCS3, upstream commit `eee3f0f7c285f2b8aa18fec278a2146c5cd874eb`, and its submodules (glslang, yaml-cpp, AsmJit, Vulkan Memory Allocator, stb, pugixml, Vulkan-Headers) | GPL-2.0 (RPCS3); BSD / MIT / Zlib / Apache-2.0 (submodules; glslang has one GPL-3.0-with-Bison-exception generated file) | `ps3recomp\third_party\rpcs3\LICENSE` and each submodule's licence | The port's patch from `ps3recomp\patches\` is applied on top. |
| FFmpeg 7.1 (BtbN `win64-lgpl-shared-7.1` build, unmodified `avcodec-61`, `avutil-59`, `swresample-5`) | **LGPL-3.0-or-later** (configured with `--enable-version3`, without `--enable-gpl`) | `tools\ffmpeg_bin\LICENSE.txt`; `tools\ffmpeg_bin\SOURCE.txt` says where the build and its source come from | Pinned by SHA-256. Loaded at run time for the H.264, MPEG-2 video and AC-3 decoders. |
| shaderscan v1.0.2 (`tools\shaderscan.exe`, the launcher's optional offline shader-cache seeding; from https://github.com/miroslavdanutalexandru-alt/shaderscan) | **GPL-2.0-only** (it hashes and analyses shaders the way RPCS3 does); contains miniz (MIT) and the Fall of Man port's RSX translators (MIT) | `LICENSE` and `THIRD_PARTY_NOTICES.md` in the shaderscan repository, which also holds its source | Release exe, pinned by version and SHA-256 in `tools\shaderscan.version` and downloaded by `tools\get_shaderscan.ps1` (Setup / `build.bat`). A separate program that the launcher runs; not linked into the port or the game. Optional: skipped when offline. |
| NVIDIA DLSS SDK v310.9.16 (headers, `nvsdk_ngx_s.lib`, `nvngx_dlss.dll`) | NVIDIA RTX SDK licence | `tools\dlss\LICENSE.txt` | Optional; only after you accept the licence. Used only by `r2_dlss.dll`, a separate bridge DLL built from the port's MIT `project\r2_dlss.cpp` that the game loads at run time through a small C interface. |
| Intel XeSS SDK 2.1.1 (headers, `libxess.dll`) | Intel Simplified Software License | `tools\xess\LICENSE.txt`, `tools\xess\third-party-programs.txt` | Optional; only after you accept the licence. Unmodified; no reverse engineering. |
| NVIDIA Streamline SDK 2.14.16 (headers; `sl.interposer/common/dlss_g/reflex/pcl.dll`, `nvngx_dlssg.dll`, `NvLowLatencyVk.dll`) | Headers MIT; DLLs NVIDIA RTX SDK licence / NVIDIA Reflex licence | `tools\streamline\license.txt`, `tools\streamline\nvngx_dlss.license.txt`, `tools\streamline\reflex.license.txt`, `tools\streamline\3rd-party-licenses.md` | Optional (DLSS Frame Generation); only after you accept the licence. Unmodified, signed NVIDIA production DLLs. |
| RPCS3's `key_vault.cpp` (public application keys) | GPL-2.0 | as RPCS3 | Used only by Setup to decrypt your own copy; not part of the game program. |
| Tracy Profiler v0.15 (https://github.com/wolfpld/tracy; developer builds only: `build_cpu.bat tracy` with `PORT_TRACY_DIR` set) | BSD-3-Clause | `LICENSE` in your Tracy checkout | Optional and off by default; never downloaded by Setup and not linked into the normal build. |

## Downloaded on your PC by the HD UI pack tool, not shipped

The official `realesrgan-x4plus` model the first time *Create HD UI pack* runs on the graphics card (or with `--model x4plus`); the other models only when you pick them (`HDUIPack.exe --model remacri` / `ultrasharp`, or `tools\hd_ui_pack.py`). Each file is pinned by SHA-256 and downloaded into `tools\upscaler\models\`; nothing from it is shipped with the port or linked into the game. The textures it makes come from your own game and stay on your PC. `HDUIPack.exe` itself is built on your PC from the port's MIT source (`project\hd_ui_pack.cpp`) with stb (MIT or public domain, from the RPCS3 copy Setup fetches) and miniz (MIT, Fossilize's copy, above).

| Component | Licence | Notes |
|---|---|---|
| Upscaling model **realesrgan-x4plus** by Xintao Wang (`realesrgan-x4plus.param` / `.bin`, from Real-ESRGAN's official release zip https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip, SHA-256 `abc02804e17982a3be33675e4d471e91ea374e65b70167abc09e31acb412802d`; only these two files are extracted, each SHA-256 checked, and the zip is deleted) | **BSD-3-Clause**, © 2021 Xintao Wang (https://github.com/xinntao/Real-ESRGAN/blob/master/LICENSE) | The default on the graphics card (the model the Bloodborne PC port's Lantern Engine uses). |
| Upscaling model **4x Remacri** by FoolhardyVEVO (`remacri-4x.param` / `.bin`, ncnn conversion from the Upscayl repository, https://github.com/upscayl/upscayl `resources/models/`, pinned commit `a00d55f`) | **CC BY-NC-SA 4.0** (non-commercial) | Only with `--model remacri`. Only the model files are downloaded, not Upscayl itself. |
| Upscaling model **4x UltraSharp** by Kim2091 (`ultrasharp-4x.param` / `.bin`, same source and commit) | **CC BY-NC-SA 4.0** (non-commercial) | Only with `--model ultrasharp`. |

Because of the remacri / UltraSharp models' non-commercial licence, an HD UI pack made with them must not be sold or used commercially (and, being made from the game, must not be shared at all).

## The game program you build

`build\Resistance3.exe`, built on your PC, links the RPCS3 renderer and is therefore covered by the **GPL-2.0**. Its complete corresponding source is this repository at the release tag plus RPCS3 at the commit above. SoundTouch and at3_standalone (LGPL) are linked statically; this source lets you relink them with modified versions. FFmpeg (LGPL) is loaded as separate, replaceable DLLs.

**If you redistribute a build you made yourself**, you take on the obligations of every licence above, including the GPL-2.0 source requirement. Do not redistribute it with `r2_dlss.dll` or `nvngx_dlss.dll`: the NVIDIA RTX SDK licence forbids making the SDK subject to an open-source licence (§4e) and has its own redistribution conditions. Never redistribute anything made from the game itself.

## Not used

- **SDL3.dll:** not needed, because SDL3 is linked statically.
- **Microsoft Visual C++ runtime:** not bundled. Install it from Microsoft (VC++ 2015–2022 x64, 14.40 or newer).
- **Vulkan loader (`vulkan-1.dll`):** comes with the GPU driver.

## Attributions

- **NVIDIA DLSS:** this port uses NVIDIA DLSS. NVIDIA, GeForce RTX and NVIDIA DLSS are trademarks of NVIDIA Corporation. The DLSS SDK licence (Supplement §7.1(b)) asks for this attribution on the splash screen, about box or credits; it appears in the README credits and the launcher's About page.
- AMD FidelityFX, Intel XeSS, NVIDIA Streamline, RPCS3, SDL, SoundTouch, at3_standalone, FFmpeg, Fossilize and Tracy: credited in the README and covered by the licence files above.
- **Resistance: Fall of Man PC port** (https://github.com/Psyykosis/Resistance-Fall-of-Man-Recomp, MIT): this port shares its ps3recomp-based runtime, tools and fixes (SPURS / SPU job handling, frame pacing, input, audio and video).
- **Killzone PC ports** by drakolordx7 (https://github.com/drakolordx7/KillzoneRecomp): ideas for the launcher and quality-of-life features, used with the maintainer's permission; no code was copied.

### inFAMOUS 2 PC port (MIT)

```
MIT License

Copyright (c) 2026 Sami Enazi

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### GoW Ascension recomp (MIT)

```
MIT License

Copyright (c) 2026 BilalReloaded

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
