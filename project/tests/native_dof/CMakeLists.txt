# Offline bit-exactness test for the native depth-of-field kernels (R3 SPU job 419 ->
# igpic images 618..623, project/r3_native_dof_gen_*.c). Replays captures written in-game by the
# lifted kernels (R3_DOF_CAPTURE=N -> scratch/dof_capture/*.bin) through the native code and
# compares the whole 256 KB LS + 128 registers byte for byte. Skipped when no captures exist.
#   cmake -S project/tests/native_dof -B build_doftest -G Ninja -DCMAKE_C_COMPILER=clang-cl
#   cmake --build build_doftest && ctest --test-dir build_doftest --output-on-failure
cmake_minimum_required(VERSION 3.20)
project(native_dof_test C)
enable_testing()

get_filename_component(PORT_PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(PORT_ROOT "${PORT_PROJECT_DIR}/.." ABSOLUTE)
set(DOF_CAPTURE_DIR "${PORT_ROOT}/scratch/dof_capture" CACHE PATH "R3_DOF_CAPTURE output directory")

set(_gen)
foreach(img 618 619 620 621 622 623)
    list(APPEND _gen "${PORT_PROJECT_DIR}/r3_native_dof_gen_${img}.c")
endforeach()
add_executable(verify_dof verify_dof.c ${_gen})
target_include_directories(verify_dof PRIVATE "${PORT_PROJECT_DIR}"
    "${PORT_ROOT}/ps3recomp/runtime/spu" "${PORT_ROOT}/ps3recomp/include")
# same code generation as the game's lifted SPU sources (project/CMakeLists.txt, PORT_SPU_AVX2=ON)
target_compile_definitions(verify_dof PRIVATE _CRT_SECURE_NO_WARNINGS
    SPU_INLINE_CH SPU_LS_NO_DBG SPU_USE_DISPATCH_MT SPU_XF_NO_EXACT)
target_compile_options(verify_dof PRIVATE -O2 /arch:AVX2 /GS- -fno-strict-aliasing -fwrapv -w)
target_link_options(verify_dof PRIVATE /STACK:8388608)

file(TO_NATIVE_PATH "${DOF_CAPTURE_DIR}" _capdir)
add_test(NAME native_dof_exact COMMAND verify_dof "${_capdir}")
set_tests_properties(native_dof_exact PROPERTIES SKIP_RETURN_CODE 77)
