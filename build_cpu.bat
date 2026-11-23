@echo off
rem CPU-optimised builds of the game, each in its own folder (build\ is not touched).
rem   build_cpu.bat            -> build_cpu\Resistance3.exe       AVX2 everywhere + ThinLTO
rem   build_cpu.bat baseline   -> build_baseline\Resistance3.exe  no AVX (older CPUs; run.bat picks it there)
rem   build_cpu.bat pgo-gen    -> build_pgo_gen\Resistance3.exe   instrumented, for tools\pgo_train.bat
rem   build_cpu.bat pgo-use    -> build_pgo\Resistance3.exe       AVX2 + ThinLTO + PGO from pgo\r3.profdata
rem   build_cpu.bat tracy      -> build_tracy\Resistance3.exe     AVX2 + Tracy zones (connect the Tracy viewer)
rem Add "nopause" as the last argument when calling from a script.
rem The lifted PPU code (1.3 GB of C++, ~75 CPU-minutes) is not recompiled: its objects are
rem copied from build\ (same lift, same flags), except for baseline, which needs them without AVX2.
rem PORT_JOBS (default 4) compile jobs, at below-normal priority. PORT_LTO=0 skips ThinLTO.
rem PORT_PGO_PPU=1 / PORT_LTO_PPU=1 also instrument / LTO the lifted PPU code (compiles it: hours).
setlocal
cd /d "%~dp0"
if not exist logs mkdir logs
set "MODE=%~1"
if "%MODE%"=="" set "MODE=avx2"
if /i "%MODE%"=="nopause" (set "MODE=avx2" & set "NOPAUSE=1")
if /i "%~2"=="nopause" set "NOPAUSE=1"
if not defined PORT_JOBS set "PORT_JOBS=4"
set "LTO=ON"
if "%PORT_LTO%"=="0" set "LTO=OFF"
set "PPUX="
if "%PORT_LTO_PPU%"=="1" set "PPUX=%PPUX% -DPORT_LTO_PPU=ON"
if "%PORT_PGO_PPU%"=="1" set "PPUX=%PPUX% -DPORT_PGO_PPU=ON"
rem explicit OFF: a cached ON from an earlier configure of the same folder would otherwise stick
if not "%PORT_LTO_PPU%"=="1" set "PPUX=%PPUX% -DPORT_LTO_PPU=OFF"
if not "%PORT_PGO_PPU%"=="1" set "PPUX=%PPUX% -DPORT_PGO_PPU=OFF"
set "PREBUILT=1"
if "%PORT_LTO_PPU%"=="1" set "PREBUILT=0"
if "%PORT_PGO_PPU%"=="1" set "PREBUILT=0"
if /i "%MODE%"=="avx2"     (set "BDIR=build_cpu"      & set "OPTS=-DPORT_ARCH=AVX2 -DPORT_LTO=%LTO% -DPORT_PGO=off")
if /i "%MODE%"=="baseline" (set "BDIR=build_baseline" & set "OPTS=-DPORT_ARCH=baseline -DPORT_LTO=%LTO% -DPORT_PGO=off" & set "PREBUILT=0")
if /i "%MODE%"=="tracy"    (set "BDIR=build_tracy"    & set "OPTS=-DPORT_ARCH=AVX2 -DPORT_LTO=OFF -DPORT_PGO=off -DPORT_TRACY=ON")
if /i "%MODE%"=="pgo-gen"  (set "BDIR=build_pgo_gen"  & set "OPTS=-DPORT_ARCH=AVX2 -DPORT_LTO=OFF -DPORT_PGO=gen")
if /i "%MODE%"=="pgo-use" (
  if not exist pgo\r3.profdata (echo pgo\r3.profdata not found - run tools\pgo_train.bat and tools\pgo_merge.bat first. & goto fail)
  set "BDIR=build_pgo" & set "OPTS=-DPORT_ARCH=AVX2 -DPORT_LTO=%LTO% -DPORT_PGO=use"
)
rem outside the block: a quoted path with a space or ")" breaks neither the block nor the argument
set "PGOPROF="
if /i "%MODE%"=="pgo-use" set "PGOPROF=%~dp0pgo\r3.profdata"
if not defined BDIR (echo Unknown mode "%MODE%": use avx2, baseline, tracy, pgo-gen or pgo-use. & goto fail)

rem Visual Studio (vcvars64) or the portable build tools in tools_cache\ - see tools\toolchain_env.bat
call "%~dp0tools\toolchain_env.bat"
if errorlevel 1 goto fail
rem the header path goes on the cmake lines in quotes of its own: quotes nested in set "..." would leave %~dp0
rem unquoted, and a folder name with "&" or ")" would then break the line
set "FFOPT=-DPS3RECOMP_FFMPEG_RUNTIME=ON"
set "FFINC=%~dp0ffmpeg\include"
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist build_rpcs3\rpcs3_rsx.lib (echo build_rpcs3\rpcs3_rsx.lib missing - run build.bat once first. & goto fail)
rem The RPCS3 RSX library (rsx::thread, Vulkan backend) per mode, ps3recomp\third_party\rpcs3 RPCS3_RSX_*:
rem baseline needs a generic (no AVX) one; pgo-gen an instrumented one and pgo-use a profile-optimised one, so
rem the renderer is in the profile too. The other modes link build.bat's build_rpcs3 (AVX2 + ThinLTO + /GS-).
set "RSXDIR=build_rpcs3"
if /i "%MODE%"=="baseline" (set "RSXDIR=build_rpcs3_generic" & set "RSXOPTS=-DRPCS3_RSX_ARCH=generic -DRPCS3_RSX_LTO=%LTO% -DRPCS3_RSX_PGO=off")
if /i "%MODE%"=="pgo-gen"  (set "RSXDIR=build_rpcs3_pgo_gen" & set "RSXOPTS=-DRPCS3_RSX_ARCH=AVX2 -DRPCS3_RSX_LTO=OFF -DRPCS3_RSX_PGO=gen")
if /i "%MODE%"=="pgo-use"  (set "RSXDIR=build_rpcs3_pgo_use" & set "RSXOPTS=-DRPCS3_RSX_ARCH=AVX2 -DRPCS3_RSX_LTO=%LTO% -DRPCS3_RSX_PGO=use")
if "%RSXDIR%"=="build_rpcs3" goto rsx_ok
if not defined RPCS3_JOBS set "RPCS3_JOBS=8"
echo Building the RPCS3 RSX core for "%MODE%" into %RSXDIR%\ ^(log logs\build_%RSXDIR%.log^)...
cmake -S ps3recomp\third_party\rpcs3 -B %RSXDIR% -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DRPCS3_RSX_BUILD_LINKTEST=OFF -DENABLE_HLSL=ON -DRPCS3_RSX_GS_OFF=ON %RSXOPTS% "-DRPCS3_RSX_PGO_PROFILE=%PGOPROF%" > logs\configure_%RSXDIR%.log 2>&1
if errorlevel 1 (echo RPCS3 RSX configure FAILED - see logs\configure_%RSXDIR%.log & goto fail)
"%PS%" -NoProfile -Command "(Get-Process -Id $PID).PriorityClass = 'BelowNormal'; cmake --build %RSXDIR% -j %RPCS3_JOBS% --target rpcs3_rsx > logs\build_%RSXDIR%.log 2>&1; exit $LASTEXITCODE"
if errorlevel 1 (echo RPCS3 RSX build FAILED - see logs\build_%RSXDIR%.log & goto fail)
:rsx_ok

set "PPUOPT=-DPORT_PPU_PREBUILT_OBJ_DIR="
if "%PREBUILT%"=="1" (
  rem snapshot of build\'s lifted PPU objects; refuses ones older than their source
  "%PS%" -NoProfile -Command "$src=(Get-ChildItem build\CMakeFiles\Resistance3.dir -Recurse -Filter ppu_recomp_000.cpp.obj | Select-Object -First 1).DirectoryName; if (-not $src) { exit 2 }; $dst='%BDIR%\ppu_obj'; New-Item -ItemType Directory -Force $dst | Out-Null; $n=0; foreach ($c in Get-ChildItem recompiled -Filter 'ppu_recomp_*.cpp' | Where-Object { $_.Name -match '^ppu_recomp_[0-9]+\.cpp$' }) { $o=Join-Path $src ($c.Name+'.obj'); if ((Test-Path $o) -and (Get-Item $o).LastWriteTime -gt $c.LastWriteTime) { $d=Join-Path $dst ($c.Name+'.obj'); if (-not (Test-Path $d) -or (Get-Item $d).LastWriteTime -ne (Get-Item $o).LastWriteTime) { Copy-Item $o $d -Force }; $n++ } else { Remove-Item (Join-Path $dst ($c.Name+'.obj')) -ErrorAction SilentlyContinue; Write-Host ('  '+$c.Name+': no up-to-date object in build\ - compiled here') } }; Write-Host ('  '+$n+' lifted PPU object(s) taken from build\')"
  if errorlevel 2 (echo   no lifted PPU objects in build\ - compiling them here ^(long^)) else set "PPUOPT=-DPORT_PPU_PREBUILT_OBJ_DIR=%~dp0%BDIR%\ppu_obj"
)

echo ============================================================
echo  Resistance 3 - CPU build "%MODE%" into %BDIR%\
echo ============================================================
cmake -S project -B %BDIR% -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DPORT_NAME=Resistance3 -DPS3RECOMP_DIR="%~dp0ps3recomp" -DRECOMP_DIR="%~dp0recompiled" -DR2_RPCS3_RSX_BUILD="%~dp0%RSXDIR%" -DPORT_SPU_AVX2=ON %OPTS% %PPUX% "%PPUOPT%" "-DPORT_PGO_PROFILE=%PGOPROF%" %FFOPT% "-DPS3RECOMP_FFMPEG_INCLUDE_DIR=%FFINC%" > logs\configure_%BDIR%.log 2>&1
if errorlevel 1 (echo Configure FAILED - see logs\configure_%BDIR%.log & goto fail)
findstr /c:"CPU target" /c:"LTO:" /c:"PGO:" /c:"Lifted PPU" /c:"Tracy:" logs\configure_%BDIR%.log
echo Building (below-normal priority, %PORT_JOBS% jobs; log logs\build_%BDIR%.log)...
"%PS%" -NoProfile -Command "(Get-Process -Id $PID).PriorityClass = 'BelowNormal'; cmake --build %BDIR% -j %PORT_JOBS% 2>&1 | Tee-Object -FilePath logs\build_%BDIR%.log | Select-String -CaseSensitive -Pattern '^FAILED|error:|^\[[0-9]+/[0-9]+\] Linking' | ForEach-Object { $_.Line }; $c = $LASTEXITCODE; if (Select-String -Path logs\build_%BDIR%.log -Pattern '^FAILED: |^ninja: build stopped' -Quiet) { $c = 1 }; exit $c"
if errorlevel 1 (echo. & echo BUILD FAILED - see logs\build_%BDIR%.log & goto fail)

for %%F in (avcodec-61.dll avutil-59.dll swresample-5.dll) do if exist "tools\ffmpeg_bin\%%F" copy /Y "tools\ffmpeg_bin\%%F" %BDIR%\ >nul
for %%F in (r2_dlss.dll nvngx_dlss.dll amd_fidelityfx_vk.dll libxess.dll sl.interposer.dll sl.common.dll sl.dlss_g.dll sl.reflex.dll sl.pcl.dll nvngx_dlssg.dll NvLowLatencyVk.dll) do if exist "build\%%F" copy /Y "build\%%F" %BDIR%\ >nul
echo.
echo BUILD SUCCEEDED: %BDIR%\Resistance3.exe
if /i "%MODE%"=="pgo-gen" echo Next: tools\pgo_train.bat (plays the instrumented build), then scripts\pgo_merge_and_build.ps1 (merge + pgo-use build)
if /i "%MODE%"=="tracy" echo Run it with: set PORT_GAME_EXE=build_tracy\Resistance3.exe ^& run.bat  - then connect tracy-profiler.exe
if /i "%MODE%"=="avx2" echo Run it with: set PORT_GAME_EXE=build_cpu\Resistance3.exe ^& run.bat
if not defined NOPAUSE pause
exit /b 0
:fail
if not defined NOPAUSE pause
exit /b 1
