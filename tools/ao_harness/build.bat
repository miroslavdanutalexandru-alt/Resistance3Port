@echo off
rem Builds the offline CACAO harness against the R3 rpcs3 build tree (glslang + Vulkan headers). Run from any dir.
setlocal
set HERE=%~dp0
set ROOT=%HERE%..\..
set RP=%ROOT%\ps3recomp\third_party\rpcs3
if "%RPCS3_BUILD%"=="" set RPCS3_BUILD=D:\Games\Resistance3Port\build_rpcs3
call "%ROOT%\tools\toolchain_env.bat" >nul || exit /b 1
clang-cl /nologo /MD /O2 /std:c++20 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS -Wno-unused-function ^
  /I"%RP%\3rdparty\Vulkan-Headers\include" /I"%RP%\3rdparty\glslang\glslang" /I"%RP%\rpcs3\Emu\RSX\VK" ^
  "%HERE%ao_harness.cpp" /Fo"%HERE%ao_harness.obj" /Fe"%HERE%ao_harness.exe" /link ^
  "%RPCS3_BUILD%\3rdparty\glslang\glslang\glslang\glslang.lib" ^
  "%RPCS3_BUILD%\3rdparty\glslang\glslang\glslang\glslang-default-resource-limits.lib" ^
  "%RPCS3_BUILD%\3rdparty\glslang\glslang\SPIRV\SPIRV.lib" || exit /b 1
echo built %HERE%ao_harness.exe