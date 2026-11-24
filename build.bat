@echo off
setlocal
set "NOPAUSE="
for %%A in (%*) do if /i "%%~A"=="nopause" set "NOPAUSE=1"
cd /d "%~dp0"
if not exist logs mkdir logs
if not exist recompiled\ppu_recomp.h (echo The game code has not been generated yet - run Setup.bat first. & goto fail)
rem A game window left open locks the .exe and the link fails -- close any that are still running.
rem Close only a copy of Resistance3.exe started from THIS folder (never one from another checkout).
powershell -NoProfile -Command "$d=[IO.Path]::GetFullPath('%~dp0'); $k=0; Get-Process Resistance3 -EA 0 | ?{ $_.Path -and $_.Path.StartsWith($d,'OrdinalIgnoreCase') } | ForEach-Object { Stop-Process -Id $_.Id -Force; $k=1 }; if($k){ 'Closed a Resistance3.exe that was still running.' }"
echo ============================================================
echo  Resistance 3 - native PC build
echo  Builds one program (EBOOT.BIN; game.self is the same file).
echo  The first build takes a long time. Leave it running.
echo ============================================================
rem Visual Studio (vcvars64) or the portable build tools in tools_cache\ - see tools\toolchain_env.bat
call "%~dp0tools\toolchain_env.bat"
if errorlevel 1 goto fail
rem An MSYS2/Cygwin cmake found first on PATH reads D:/... as a relative path (configure then fails with paths like
rem /d/<folder>/D:/...): fall back to the CMake that comes with Visual Studio, else stop with a clear message.
if not exist tools\cmake_native_probe.cmake goto cmake_ok
cmake -P tools/cmake_native_probe.cmake >nul 2>&1 && goto cmake_ok
set "VSCMK=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake"
if exist "%VSCMK%\CMake\bin\cmake.exe" set "PATH=%VSCMK%\CMake\bin;%VSCMK%\Ninja;%PATH%"
cmake -P tools/cmake_native_probe.cmake >nul 2>&1 || goto cmake_bad
echo Using the CMake that comes with Visual Studio - the cmake found first on PATH is not a Windows build.
goto cmake_ok
:cmake_bad
echo No Windows CMake was found. The cmake on PATH, if any, is an MSYS2/Cygwin build, which cannot build this port:
where cmake 2>nul
echo Install CMake for Windows - winget install Kitware.CMake, or https://cmake.org/download/ - or run Setup.bat, then try again.
goto fail
:cmake_ok
rem the header path goes on the cmake lines in quotes of its own: quotes nested in set "..." would leave %~dp0
rem unquoted, and a folder name with "&" or ")" would then break the line
set "FFOPT=-DPS3RECOMP_FFMPEG_RUNTIME=ON"
set "FFINC=%~dp0ffmpeg\include"
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
rem files dated in the future (zip/installer time zones) make Ninja regenerate forever
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\fix_timestamps.ps1"
if not defined PORT_JOBS set "PORT_JOBS=4"
rem Setup.exe sets these (and PORT_PPU_JOBS, read by project\CMakeLists.txt) from the free RAM
if not defined RPCS3_JOBS set "RPCS3_JOBS=8"
rem PGO (profile-guided optimisation): when pgo\r3.profdata exists (recorded with tools\pgo_train.bat, see README),
rem the renderer, the runtime, the hand-written and the lifted SPU code are optimised with it. A profile older than
rem the sources only loses part of the gain (changed functions are built without it). Opt out: set PORT_PGO=0 or
rem "build.bat nopgo".
set "PGOMODE=off"
set "PGOPROF="
if exist "%~dp0pgo\r3.profdata" set "PGOMODE=use"
if "%PORT_PGO%"=="0" set "PGOMODE=off"
if /i "%PORT_PGO%"=="off" set "PGOMODE=off"
for %%A in (%*) do if /i "%%~A"=="nopgo" set "PGOMODE=off"
if "%PGOMODE%"=="use" set "PGOPROF=%~dp0pgo\r3.profdata"
if "%PGOMODE%"=="use" echo PGO: optimising with pgo\r3.profdata - set PORT_PGO=0 or run "build.bat nopgo" to build without it
rem pgo\r3.profdata is an LLVM indexed profile, format version 13: clang 22 or newer reads it; an older clang-cl
rem stops every PGO compile with "unsupported version". The port needs LLVM 22+ (Setup installs / updates it).
set "CLANGVER="
set "CLANGMAJ="
for /f "tokens=3" %%V in ('clang-cl --version 2^>nul ^| findstr /b /c:"clang version"') do if not defined CLANGVER set "CLANGVER=%%V"
if defined CLANGVER for /f "delims=." %%M in ("%CLANGVER%") do set "CLANGMAJ=%%M"
if not defined CLANGMAJ goto clang_ok
if %CLANGMAJ% GEQ 22 goto clang_ok
if "%PGOMODE%"=="use" (
  echo ERROR: clang-cl %CLANGVER% is too old. This port needs LLVM ^(clang-cl^) 22 or newer: the included profile
  echo pgo\r3.profdata uses LLVM profile format 13, which older versions cannot read.
  echo Run Setup.bat again to update LLVM, or install it with: winget upgrade LLVM.LLVM
  echo ^(To build without the profile anyway: "build.bat nopgo" or set PORT_PGO=0.^)
  goto fail
)
echo WARNING: clang-cl %CLANGVER% is older than the supported LLVM 22 - building without PGO; run Setup.bat to update LLVM.
:clang_ok

echo Building the RPCS3 RSX core...
rem An update keeps ps3recomp\third_party\rpcs3: when it lacks a file this version's patch adds (it was patched by an
rem older version, R3 issue #24), re-apply the patch first. Setup.bat does this before every build anyway.
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\fetch_rpcs3.ps1" -CheckFiles
if errorlevel 1 (
  echo Re-applying the port's RPCS3 patch...
  "%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\fetch_rpcs3.ps1"
  if errorlevel 1 (echo Could not update the RPCS3 source - see logs\fetch_rpcs3.log, then run Setup.bat again. & goto fail)
)
rem RSX code generation (output-identical: /fp:precise, no FMA contraction): AVX2 + ThinLTO + /GS-.
rem The exe already needs AVX2 (lifted code); set PORT_RSX_ARCH=generic / PORT_RSX_LTO=0 for the old build.
if not defined PORT_RSX_ARCH set "PORT_RSX_ARCH=AVX2"
set "RSXLTO=ON"
if "%PORT_RSX_LTO%"=="0" set "RSXLTO=OFF"
cmake -S ps3recomp\third_party\rpcs3 -B build_rpcs3 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DRPCS3_RSX_BUILD_LINKTEST=OFF -DENABLE_HLSL=ON -DRPCS3_RSX_ARCH=%PORT_RSX_ARCH% -DRPCS3_RSX_LTO=%RSXLTO% -DRPCS3_RSX_GS_OFF=ON -DRPCS3_RSX_PGO=%PGOMODE% "-DRPCS3_RSX_PGO_PROFILE=%PGOPROF%" "-DCMAKE_PROJECT_INCLUDE=%~dp0tools\rpcs3_sdk_deps.cmake" > logs\configure_rpcs3.log 2>&1
if errorlevel 1 (echo RPCS3 RSX configure FAILED - see logs\configure_rpcs3.log & goto fail)
cmake --build build_rpcs3 -j %RPCS3_JOBS% --target rpcs3_rsx > logs\build_rpcs3.log 2>&1
if errorlevel 1 (echo RPCS3 RSX build FAILED - see logs\build_rpcs3.log & goto fail)

(
  echo Configuring...
  cmake -S project -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DPORT_NAME=Resistance3 -DPS3RECOMP_DIR="%~dp0ps3recomp" -DRECOMP_DIR="%~dp0recompiled" -DPORT_SPU_AVX2=ON -DPORT_PGO=%PGOMODE% "-DPORT_PGO_PROFILE=%PGOPROF%" %FFOPT% "-DPS3RECOMP_FFMPEG_INCLUDE_DIR=%FFINC%" > logs\configure.log 2>&1
  if errorlevel 1 (echo Configure FAILED - see logs\configure.log & type logs\configure.log & goto fail)
)
echo Building the game...
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build_tee.ps1" -BuildDir build -Jobs "%PORT_JOBS%" -Log logs\build.log
if errorlevel 1 (echo. & echo BUILD FAILED - see logs\build.log & goto fail)


echo Building the launcher...
set "LRES="
rc /nologo /i project /fo build\launcher.res project\launcher.rc > logs\build_launcher_rc.log 2>&1 && set "LRES=build\launcher.res"
clang-cl /nologo /O2 /EHsc /W0 project\launcher.cpp /Fo:build\launcher.obj /Fe:Launcher.exe /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED %LRES% user32.lib gdi32.lib shell32.lib gdiplus.lib dwmapi.lib > logs\build_launcher.log 2>&1
if errorlevel 1 (echo Launcher build failed - see logs\build_launcher.log) else (echo Launcher.exe built)
rem HDUIPack.exe: the launcher's Create HD UI pack button (project\hd_ui_pack.cpp; reads the game's archives,
rem upscales with tools\hdui_upscaler on the GPU (CPU if that fails), writes mods\hd_ui). Failure only disables that button.
if not exist build\hdui mkdir build\hdui
clang-cl /nologo /O2 /EHsc /W0 /std:c++17 /D_CRT_SECURE_NO_WARNINGS /I ps3recomp\third_party\rpcs3\rpcs3\Emu\RSX\Common /I ps3recomp\third_party\fossilize\miniz /I ps3recomp\third_party\rpcs3\3rdparty\stblib\stb /I project project\hd_ui_pack.cpp project\mods_psarc.cpp ps3recomp\third_party\fossilize\miniz\miniz.c /Fo:build\hdui\ /Fe:HDUIPack.exe /link urlmon.lib bcrypt.lib > logs\build_hdui.log 2>&1
if errorlevel 1 (echo HDUIPack build failed - see logs\build_hdui.log) else (echo HDUIPack.exe built)

rem shaderscan: the launcher's optional offline shader-cache seeding. It lives in its own repository
rem (https://github.com/miroslavdanutalexandru-alt/shaderscan, GPL-2.0); tools\get_shaderscan.ps1 downloads the release
rem pinned in tools\shaderscan.version into tools\shaderscan.exe and checks its SHA-256 (set SHADERSCAN_EXE=<exe> to
rem use your own build). Offline or failed: the launcher works without it.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File tools\get_shaderscan.ps1 || ver >nul
rem FFmpeg (video decode) is loaded at run time: Setup.bat puts the DLLs in tools\ffmpeg_bin
for %%D in (build) do for %%F in (avcodec-61.dll avutil-59.dll swresample-5.dll) do if exist "tools\ffmpeg_bin\%%F" copy /Y "tools\ffmpeg_bin\%%F" %%D\ >nul
rem NVIDIA DLSS (the "NVIDIA DLSS" option): r2_dlss.dll is the only binary linking the NGX SDK, which setup.ps1
rem downloads into tools\dlss (NVIDIA RTX SDK licence in tools\dlss\LICENSE.txt); nvngx_dlss.dll is NVIDIA's redistributable.
if exist "tools\dlss\lib\nvsdk_ngx_s.lib" (
  clang-cl /nologo /O2 /EHsc /MT /LD /W0 /Itools\dlss\include /Ips3recomp\third_party\rpcs3\3rdparty\Vulkan-Headers\include project\r2_dlss.cpp /Fo:build\r2_dlss.obj /Fe:build\r2_dlss.dll /link tools\dlss\lib\nvsdk_ngx_s.lib advapi32.lib user32.lib > logs\build_r2_dlss.log 2>&1
  if errorlevel 1 (echo Vulkan DLSS bridge build failed - see logs\build_r2_dlss.log) else (
    echo r2_dlss.dll built
    for %%D in (build) do copy /Y "tools\dlss\nvngx_dlss.dll" %%D\ >nul
  )
)
rem FSR 3.1 for the Vulkan renderer (AMD FidelityFX SDK 1.1.x, MIT licence in tools\ffx_vk\LICENSE.txt)
for %%D in (build) do if exist "tools\ffx_vk\amd_fidelityfx_vk.dll" copy /Y "tools\ffx_vk\amd_fidelityfx_vk.dll" %%D\ >nul
rem FSR 3.1 for Direct3D 12 (experimental, rsx_d3d12_engine_aa.inc): AMD FidelityFX SDK loader + upscaler, MIT (tools\ffx_dx12\LICENSE.txt)
for %%F in (amd_fidelityfx_loader_dx12.dll amd_fidelityfx_upscaler_dx12.dll amd_fidelityfx_framegeneration_dx12.dll) do if exist "tools\ffx_dx12\%%F" copy /Y "tools\ffx_dx12\%%F" build\ >nul
rem Intel XeSS for the Vulkan renderer (RSX_FSR=4, VKR2XeSS.cpp): Intel's redistributable libxess.dll, unmodified,
rem Intel Simplified Software License (tools\xess\LICENSE.txt, third-party-programs.txt). Missing = FSR 3.1.
for %%D in (build) do if exist "tools\xess\bin\libxess.dll" copy /Y "tools\xess\bin\libxess.dll" %%D\ >nul
rem NVIDIA DLSS Frame Generation (VKR2FrameGen.cpp): NVIDIA's redistributable Streamline 2.14.16 DLLs, unmodified
rem (NVIDIA RTX SDK licence: tools\streamline\license.txt; NvLowLatencyVk.dll: reflex.license.txt). Missing = no FG.
for %%F in (sl.interposer.dll sl.common.dll sl.dlss_g.dll sl.reflex.dll sl.pcl.dll nvngx_dlssg.dll NvLowLatencyVk.dll) do if exist "tools\streamline\bin\%%F" copy /Y "tools\streamline\bin\%%F" build\ >nul
echo.
echo BUILD SUCCEEDED: build\Resistance3.exe
echo Start the game with Launcher.exe
if not defined NOPAUSE pause
exit /b 0
:fail
if not defined NOPAUSE pause
exit /b 1
