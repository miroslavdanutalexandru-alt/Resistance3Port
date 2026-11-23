cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED PORT_NAME)
    set(PORT_NAME Resistance3)
endif()
project(${PORT_NAME}
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "Recompiled PS3 game project (ps3recomp)"
)
# CMake 4.x routes .rc through clang-cl preprocessing (meant for llvm-rc); rc.exe rejects that output.
if(WIN32 AND CMAKE_RC_COMPILER MATCHES "[/\\\\]rc\\.exe$")
    set(CMAKE_DEPFILE_FLAGS_RC "")
    set(CMAKE_RC_COMPILE_OBJECT "<CMAKE_RC_COMPILER> <DEFINES> <INCLUDES> <FLAGS> /fo <OBJECT> <SOURCE>")
endif()

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer Ninja if available
if(NOT CMAKE_GENERATOR MATCHES "Ninja" AND NOT DEFINED ENV{CMAKE_GENERATOR})
    message(STATUS "Tip: use -G Ninja for faster builds")
endif()

# ---------------------------------------------------------------------------
# The two directories this project is built from
# ---------------------------------------------------------------------------
# PS3RECOMP_DIR is the toolkit checkout: include/, runtime/, libs/, tools/.
# RECOMP_DIR is where the lifter's output for THIS game lives. They are
# separate on purpose -- a port keeps its generated code in its own tree and
# points at whatever toolkit checkout it is building against:
#
#   cmake -B build -G Ninja \
#         -DPS3RECOMP_DIR=/path/to/ps3recomp \
#         -DRECOMP_DIR=/path/to/my_game/recompiled
#
if(NOT DEFINED PS3RECOMP_DIR)
    # Default: this project is still sitting inside ps3recomp/templates/project/
    get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../" ABSOLUTE)
endif()
get_filename_component(PS3RECOMP_DIR "${PS3RECOMP_DIR}" ABSOLUTE)

set(RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/recompiled" CACHE PATH
    "Directory holding the lifter's output: ppu_recomp.h and the lifted sources")
get_filename_component(RECOMP_DIR "${RECOMP_DIR}" ABSOLUTE)

message(STATUS "ps3recomp SDK:   ${PS3RECOMP_DIR}")
message(STATUS "Lifted output:   ${RECOMP_DIR}")

if(NOT EXISTS "${RECOMP_DIR}/ppu_recomp.h")
    message(FATAL_ERROR
        "No ppu_recomp.h in ${RECOMP_DIR}.\n"
        "That header and the lifted sources beside it are what "
        "tools/ppu_lifter.py writes. Run the lifter, drop its output there, "
        "or point -DRECOMP_DIR= at wherever it already is.")
endif()

# The lifted translation units. A game's lift is hundreds of generated files,
# so they are globbed rather than listed; re-run CMake after a fresh lift.
file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS
    "${RECOMP_DIR}/*.c"
    "${RECOMP_DIR}/*.cpp"
)
if(NOT RECOMP_SOURCES)
    message(FATAL_ERROR
        "${RECOMP_DIR} has a ppu_recomp.h but no .c or .cpp beside it. The "
        "lifted functions and the function table live in those; without them "
        "there is nothing to run.")
endif()
list(LENGTH RECOMP_SOURCES RECOMP_SOURCE_COUNT)
message(STATUS "Lifted sources:  ${RECOMP_SOURCE_COUNT} file(s)")

# ---------------------------------------------------------------------------
# Resistance port: CPU build options (build_cpu.bat; README "CPU build options")
# ---------------------------------------------------------------------------
#   PORT_ARCH=AVX2      (build_cpu.bat) x86-64-v3 everywhere: the runtime and the
#                       hand-written code with -march=x86-64-v3 as well (the lifted
#                       PPU/SPU code already is: PORT_PPU_V3/PORT_SPU_AVX2). FMA
#                       contraction stays off (-ffp-contract=off) in all of it.
#   PORT_ARCH=mixed     (default, also "") AVX2 lifted code, x86-64 + SSE4.1 runtime and
#                       hand-written code (build.bat)
#   PORT_ARCH=baseline  no AVX anywhere (x86-64 + SSE4.1, lifted code too) for older CPUs;
#                       forces PORT_PPU_V3=OFF and PORT_SPU_AVX2=OFF
#                       (build_cpu.bat baseline -> build_baseline\, run.bat picks it
#                       when the launcher finds no AVX2)
#   PORT_LTO=ON         ThinLTO (clang-cl -flto=thin + lld-link) for the runtime,
#                       the hand-written and the lifted SPU code. PORT_LTO_PPU=ON
#                       adds the 1.3 GB of lifted PPU code (hours of link).
#   PORT_PGO=gen|use    clang instrumentation PGO, same scope (PORT_PGO_PPU=ON
#                       adds the lifted PPU code). gen: instrumented exe writes
#                       LLVM_PROFILE_FILE; use: PORT_PGO_PROFILE=<merged .profdata>.
#   PORT_PPU_PREBUILT_OBJ_DIR=<dir>  link ppu_recomp_NNN.cpp.obj from <dir>
#                       instead of compiling the lifted PPU code (they must be
#                       built from the same lift with the same PPU flags).
set(PORT_ARCH "" CACHE STRING "CPU target: mixed (default when empty) | AVX2 (x86-64-v3) | baseline")
if(PORT_ARCH STREQUAL "")
    set(PORT_ARCH mixed)
endif()
if(NOT PORT_ARCH MATCHES "^(AVX2|mixed|baseline)$")
    message(FATAL_ERROR "PORT_ARCH must be AVX2, mixed or baseline (got '${PORT_ARCH}')")
endif()
option(PORT_LTO "ThinLTO for runtime, hand-written and lifted SPU code" OFF)
option(PORT_LTO_PPU "ThinLTO for the lifted PPU code too" OFF)
set(PORT_PGO "off" CACHE STRING "Profile-guided optimisation: off | gen | use")
set(PORT_PGO_PROFILE "" CACHE FILEPATH "Merged .profdata for PORT_PGO=use")
option(PORT_PGO_PPU "PGO for the lifted PPU code too" OFF)
# Short spellings (scratch/lto_pgo.md), all default OFF/empty = the default build:
#   PORT_THINLTO=ON          = PORT_LTO=ON + PORT_LTO_PPU=ON (runtime and all lifted code),
#                              plus /Gw. PORT_THINLTO_SCOPE=nopu leaves the lifted PPU code
#                              out; =hot puts only the PPU files named in PORT_LTO_PPU_ONLY in.
#   PORT_LTO_JOBS=N          lld-link ThinLTO backend threads (default: all without the PPU
#                              code, else the RAM-based PPU job pool size: each backend of a
#                              lifted PPU module needs as much RAM as its compile).
#   PORT_LTO_CACHE=OFF       no /lldltocache (default ON: <build>/lto_cache).
#   PORT_LINK_ICF=ON         /OPT:SAFEICF as well as /OPT:REF.
#   PORT_PGO_GEN=ON          = PORT_PGO=gen;  PORT_PGO_USE=<.profdata> = PORT_PGO=use.
option(PORT_THINLTO "ThinLTO for the runtime and the lifted code (PORT_THINLTO_SCOPE)" OFF)
set(PORT_THINLTO_SCOPE "all" CACHE STRING "PORT_THINLTO scope: all | nopu | hot")
set(PORT_LTO_PPU_ONLY "" CACHE STRING "PPU LTO only for these ppu_recomp_NNN names (PORT_THINLTO_SCOPE=hot)")
set(PORT_LTO_JOBS "" CACHE STRING "lld-link ThinLTO backend threads (empty: automatic)")
option(PORT_LTO_CACHE "ThinLTO incremental cache (/lldltocache)" ON)
option(PORT_LINK_ICF "Link with /OPT:SAFEICF too" OFF)
option(PORT_PGO_GEN "Instrumented PGO build (= PORT_PGO=gen)" OFF)
set(PORT_PGO_USE "" CACHE FILEPATH "Merged .profdata: PGO-optimised build (= PORT_PGO=use)")
if(PORT_THINLTO)
    set(PORT_LTO ON)
    if(PORT_THINLTO_SCOPE STREQUAL "all" OR PORT_THINLTO_SCOPE STREQUAL "hot")
        set(PORT_LTO_PPU ON)
    elseif(NOT PORT_THINLTO_SCOPE STREQUAL "nopu")
        message(FATAL_ERROR "PORT_THINLTO_SCOPE must be all, nopu or hot (got '${PORT_THINLTO_SCOPE}')")
    endif()
endif()
if(PORT_PGO_GEN AND PORT_PGO_USE)
    message(FATAL_ERROR "PORT_PGO_GEN and PORT_PGO_USE are exclusive")
elseif(PORT_PGO_GEN)
    set(PORT_PGO gen)
elseif(PORT_PGO_USE)
    set(PORT_PGO use)
    set(PORT_PGO_PROFILE "${PORT_PGO_USE}")
endif()
set(PORT_PPU_PREBUILT_OBJ_DIR "" CACHE PATH "Link prebuilt lifted PPU objects from this directory")
if(PORT_ARCH STREQUAL "baseline")
    set(PORT_PPU_V3 OFF)
    set(PORT_SPU_AVX2 OFF)
endif()
set(RECOMP_PPU_COMPILED ${RECOMP_SOURCES})
if(PORT_PPU_PREBUILT_OBJ_DIR)
    set(_kept "")
    set(_nobj 0)
    foreach(src IN LISTS RECOMP_SOURCES)
        get_filename_component(_n "${src}" NAME)
        if(_n MATCHES "^ppu_recomp_[0-9]+\\.cpp$" AND EXISTS "${PORT_PPU_PREBUILT_OBJ_DIR}/${_n}.obj")
            list(APPEND _kept "${PORT_PPU_PREBUILT_OBJ_DIR}/${_n}.obj")
            math(EXPR _nobj "${_nobj} + 1")
        else()
            list(APPEND _kept "${src}")
        endif()
    endforeach()
    set(RECOMP_SOURCES ${_kept})
    set(RECOMP_PPU_COMPILED ${_kept})
    list(FILTER RECOMP_PPU_COMPILED EXCLUDE REGEX "\\.obj$")
    message(STATUS "Lifted PPU:      ${_nobj} prebuilt object(s) from ${PORT_PPU_PREBUILT_OBJ_DIR}")
endif()

# ---------------------------------------------------------------------------
# ps3recomp runtime library
# ---------------------------------------------------------------------------
# Built from source as a subdirectory rather than imported as a prebuilt
# archive. A static library carries none of its own link requirements, so an
# imported one arrives without SDL2, pthreads, the Metal frameworks or the
# Winsock/XInput imports -- the link then fails on every platform for a
# different reason. add_subdirectory hands all of that over with the target.
# Windows: the native Vulkan renderer (R2_RSX=vulkan). glslang (with HLSL
# input) is linked from build_rpcs3 below; the runtime only needs headers.
if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/../build_rpcs3/rpcs3_rsx.lib")
    set(PS3RECOMP_RSX_VULKAN ON CACHE BOOL "" FORCE)
    # FSR 3.1 (RSX_FSR=3): FidelityFX SDK 1.1.4 headers; build.bat copies the DLL.
    set(PS3RECOMP_FFX_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../tools/ffx_vk/include" CACHE PATH "" FORCE)
    set(PS3RECOMP_GLSLANG_INCLUDE_DIR
        "${PS3RECOMP_DIR}/third_party/rpcs3/3rdparty/glslang/glslang"
        "${PS3RECOMP_DIR}/third_party/rpcs3/3rdparty/glslang"
        "${CMAKE_SOURCE_DIR}/../build_rpcs3/3rdparty/glslang/glslang/include")
endif()
add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk"
                 EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# The HLE NID table
# ---------------------------------------------------------------------------
# Maps each firmware import NID onto the handler in the runtime that answers
# it. A port may keep a hand-curated one beside its lift; otherwise generate
# the canonical every-module table, which is what the toolkit's own boot
# harness uses.
if(EXISTS "${RECOMP_DIR}/ppu_hle_nids.cpp")
    set(HLE_NID_TABLE "${RECOMP_DIR}/ppu_hle_nids.cpp")
    message(STATUS "HLE NID table:   ${HLE_NID_TABLE}")
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HLE_NID_TABLE "${CMAKE_CURRENT_BINARY_DIR}/ppu_hle_nids.cpp")
    # The generator discovers handlers from library definitions. Rebuild when
    # an existing module changes, and reconfigure when a new module is added.
    file(GLOB_RECURSE HLE_NID_SOURCES CONFIGURE_DEPENDS
        "${PS3RECOMP_DIR}/libs/*.c")
    # A removed input cannot make the output older than a remaining dependency.
    # Track the list itself so deletions also remove stale handler references.
    set(HLE_NID_SOURCE_LIST "${CMAKE_CURRENT_BINARY_DIR}/hle_nid_sources.txt")
    file(GENERATE OUTPUT "${HLE_NID_SOURCE_LIST}" CONTENT "${HLE_NID_SOURCES}\n")
    add_custom_command(
        OUTPUT "${HLE_NID_TABLE}"
        COMMAND "${Python3_EXECUTABLE}"
                "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                --out "${HLE_NID_TABLE}" --all
        DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                "${PS3RECOMP_DIR}/tools/nid_database.py"
                ${HLE_NID_SOURCES} "${HLE_NID_SOURCE_LIST}"
        COMMENT "Generating the HLE NID -> handler table")
    message(STATUS "HLE NID table:   generated (every module)")
endif()

# ---------------------------------------------------------------------------
# The game executable
# ---------------------------------------------------------------------------
# The PPU boot scaffold is compiled here rather than taken from the runtime
# library: every one of these files #includes the lifter's per-game
# ppu_recomp.h, so they can only be built against a particular lift.
set(PS3RECOMP_SCAFFOLD
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
)

# ---------------------------------------------------------------------------
# Lifted SPU images (optional)
# ---------------------------------------------------------------------------
# A title that runs SPU work gets its images lifted into <SPU_DIR>/<image>/ by
# tools/spu_lifter.py, with a registration TU generated by
# tools/build_spu_workloads.py. cellSpurs dispatches them by content
# fingerprint, so the generated table is keyed to the images THIS build ships:
# a table generated from another release registers signatures that can never
# match, which is worse than shipping none. Both the table and the job-module
# registry self-register from constructors and are therefore optional -- a tree
# that has not lifted its SPU images yet configures and links without them.
set(SPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/spu" CACHE PATH
    "Directory holding the lifted SPU images (<SPU_DIR>/<image>/spu_recomp.c)")
option(PORT_ENABLE_SPU "Compile and register the title's lifted SPU images" ON)

set(PORT_SPU_INPUTS "" CACHE STRING
    "Input files to scan for all embedded SPU ELF programs (semicolon-separated decrypted ELF/PRX/data paths)")
set(SPU_SOURCES "")
if(PORT_ENABLE_SPU AND PORT_SPU_INPUTS)
    include("${PS3RECOMP_DIR}/cmake/PS3RecompSPU.cmake")
    ps3recomp_generate_spu("${CMAKE_CURRENT_BINARY_DIR}/generated_spu" ${PORT_SPU_INPUTS})
    list(APPEND SPU_SOURCES ${SPU_GENERATED_SOURCES}
         "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
elseif(PORT_ENABLE_SPU)
    file(GLOB SPU_LIFTED CONFIGURE_DEPENDS "${SPU_DIR}/*/spu_recomp.c")
    if(SPU_LIFTED)
        list(LENGTH SPU_LIFTED SPU_IMAGE_COUNT)
        message(STATUS "Lifted SPU:      ${SPU_IMAGE_COUNT} image(s)")
        # The musttail indirect-branch fast path. Excluded from the runtime
        # library on purpose (see the SDK root CMakeLists): it must be built
        # here, by clang, or the tail call degrades to a plain call and a guest
        # loop that branches indirectly walks the thread stack off its guard
        # page a few thousand iterations in.
        list(APPEND SPU_SOURCES ${SPU_LIFTED}
             "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
        # PIC plugins (igpic_*: position-independent code that jobs 403/406 load at a different LS address
        # each run, run through ctx->ls_bias) are built without direct block hops (SPU_NO_DIRECT_HOP, see
        # spu_context.h SPU_HOP). With them, a job 403 callback into plugin DB0400 sometimes ended at LS 0
        # ("pic content-match FAILED ... pc=0x00000", then "[spu] img=2 exit: synthesised stop 0x1EAD at
        # LS 0x00000" and "[pm-end] lane 1 ... lr=0x0F8F4 r3=0x00014C60"): the lane stopped and the game
        # froze. Seen once the DOF fast path armed without an upscaler (job 419 skipped changes the job
        # timing); never with SPU_DIRECT_HOP=0. The plugins are small, so the hops cost little there.
        # PORT_PIC_DIRECT_HOP=ON keeps the hops in the plugins too.
        option(PORT_PIC_DIRECT_HOP "Direct block hops in the PIC plugin images (igpic_*)" OFF)
        if(NOT PORT_PIC_DIRECT_HOP)
            set(_pic_srcs ${SPU_LIFTED})
            list(FILTER _pic_srcs INCLUDE REGEX "/igpic_[^/]*/spu_recomp\\.c$")
            if(_pic_srcs)
                # The define is added after the SPU_SOURCES COMPILE_DEFINITIONS below, which would replace it.
                list(LENGTH _pic_srcs _pic_n)
                message(STATUS "PIC plugins:     ${_pic_n} image(s) without direct block hops (PORT_PIC_DIRECT_HOP=OFF)")
            endif()
        endif()
        # Generated beside the lift; each is present only once its generator
        # has been run, so each is included only if it is actually here.
        foreach(gen IN ITEMS spu_workloads.c jobmods_register.c taskset_policy_bytes.c)
            if(EXISTS "${SPU_DIR}/${gen}")
                list(APPEND SPU_SOURCES "${SPU_DIR}/${gen}")
            else()
                message(STATUS "  (no ${gen} -- not registered)")
            endif()
        endforeach()
    else()
        message(STATUS "Lifted SPU:      none found in ${SPU_DIR}")
    endif()
endif()

# Resistance port: native ADS depth-of-field job kernels (igjob 419). Built with
# the lifted SPU flags: it calls the same SPU float/int helpers.
if(SPU_SOURCES)
    list(APPEND SPU_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/r3_native_dof.c")
    # R3_DOF_GPU_APPROX ("Aim blur: PC fast"): host approximation of job 419's output.
    list(APPEND SPU_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/r3_dof_fast.c")
    foreach(_dof_img 618 619 620 621 622 623)
        list(APPEND SPU_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/r3_native_dof_gen_${_dof_img}.c")
    endforeach()
    # R3_NATIVE_DOF_JOB>=1 (default 4, r3_native_dof.h): bit-exact AVX2 bulk loops for kernel 623
    # (scratch/native_job419). AVX2 builds only (PORT_SPU_AVX2, off for PORT_ARCH=baseline);
    # r3_native_dof.c sees R3_NATIVE_DOF_JOB_BUILT (set after the SPU source properties below).
    if(PORT_SPU_AVX2)
        list(APPEND SPU_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/r3_native_dof_job.c"
                                "${CMAKE_CURRENT_SOURCE_DIR}/r3_native_dof_job_gen_623.c")
    endif()
    # Native SPU job kernels behind r3nj_gate with a sampled runtime self-check (r3_native_jobs.c, R3_NATIVE_JOBS,
    # default on; kernel 0 = Edge zlib inflate_fast, PORT_NATIVE_EDGE_ZLIB=0 also turns it off). The gate call sits
    # at the top of spuelf0_spu_func_00008100 in the lifted spuelf0, put there by the regen from
    # tools/hooks/spu/spuelf0.json. Which hooks this build really has goes to r3_native_jobs.c as R3NJ_HOOKS
    # (bit k = kernel k): a kernel without its hook reports "hook missing" and the [native] line shows it off.
    # Kernels 1-3: job 421 (igjob_00E0A480, Bink video) 8x8 IDCT put / 2x put / add (r3_native_bink.c, AVX2
    # builds only; without AVX2 they report "needs an AVX2 build"), hooks from tools/hooks/spu/igjob_00E0A480.json.
    list(APPEND SPU_SOURCES "${PS3RECOMP_DIR}/runtime/spu/native_edge_zlib.c"
                            "${CMAKE_CURRENT_SOURCE_DIR}/r3_native_jobs.c"
                            "${CMAKE_CURRENT_SOURCE_DIR}/r3_native_bink.c"
                            "${CMAKE_CURRENT_SOURCE_DIR}/r3_native_geom.c")
    set(_r3nj_hooks 0)
    # Kernel 4: job 422 (igjob_00E29200, Edge-style geometry) 0x120 position transform + clip codes
    # (r3_native_geom.c, port of R2 dd413 0x4030; off unless R3_NATIVE_JOBS_GEOM=1 / mask bit 4).
    if(EXISTS "${SPU_DIR}/igjob_00E29200/spu_recomp.c")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SPU_DIR}/igjob_00E29200/spu_recomp.c")
        file(STRINGS "${SPU_DIR}/igjob_00E29200/spu_recomp.c" _geo_hook REGEX "r3nj_gate\\(ctx, 4 /\\* R3NJ_K_GEOM_XFORM4")
        if(_geo_hook)
            math(EXPR _r3nj_hooks "${_r3nj_hooks} | 16")
        else()
            message(WARNING "igjob_00E29200/spu_recomp.c has no job 422 geom hook (regen dropped it?): "
                            "R3_NATIVE_JOBS_GEOM has no effect. Run tools/r3_regen.py (tools/hooks/spu/igjob_00E29200.json)")
        endif()
    endif()
    if(EXISTS "${SPU_DIR}/spuelf0/spu_recomp.c")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SPU_DIR}/spuelf0/spu_recomp.c")
        file(STRINGS "${SPU_DIR}/spuelf0/spu_recomp.c" _ezl_hook REGEX "r3nj_gate\\(ctx, 0 /\\* R3NJ_K_EDGE_ZLIB")
        if(_ezl_hook)
            math(EXPR _r3nj_hooks "${_r3nj_hooks} | 1")
        endif()
    endif()
    set(_bk_have 0)
    if(EXISTS "${SPU_DIR}/igjob_00E0A480/spu_recomp.c")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SPU_DIR}/igjob_00E0A480/spu_recomp.c")
        file(STRINGS "${SPU_DIR}/igjob_00E0A480/spu_recomp.c" _bk_hooks REGEX "r3nj_gate\\(ctx, [1-3] /\\* R3NJ_K_BINK_IDCT_")
        foreach(_bk_line IN LISTS _bk_hooks)
            string(REGEX MATCH "r3nj_gate\\(ctx, ([1-3]) " _bk_m "${_bk_line}")
            math(EXPR _r3nj_hooks "${_r3nj_hooks} | (1 << ${CMAKE_MATCH_1})")
            math(EXPR _bk_have "${_bk_have} + 1")
        endforeach()
        if(NOT _bk_have EQUAL 3)
            message(WARNING "igjob_00E0A480/spu_recomp.c has ${_bk_have} of 3 Bink IDCT hooks (regen dropped them?): "
                            "those kernels stay off. Run tools/r3_regen.py (tools/hooks/spu/igjob_00E0A480.json)")
        endif()
    endif()
    math(EXPR _ezl_bit "${_r3nj_hooks} & 1")
    if(NOT _ezl_bit)
        message(WARNING "spuelf0/spu_recomp.c has no native Edge zlib hook (regen dropped it?): "
                        "R3_NATIVE_JOBS / PORT_NATIVE_EDGE_ZLIB have no effect. Run tools/r3_regen.py (tools/hooks/spu/spuelf0.json)")
    endif()
    # Native audio-synth kernels (R3_NATIVE_AUDIO_JOB, default on; port of R2's r2_native_audio).
    # The gate calls sit at the top of 7 lifted functions in the two synth jobmods, put there by the regen from
    # tools/hooks/spu/<jobmod>.json; the kernels whose hook is present go to r3_native_audio.c as R3NA_HOOKS.
    list(APPEND SPU_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/r3_native_audio.c"
                            "${CMAKE_CURRENT_SOURCE_DIR}/r3_native_audio_kern.c")
    set(_r3na_hooks 0)
    foreach(_na_mod_n jobmod_08b54bef234a_e80:5 jobmod_e2afa2e7478f_e80:2)
        string(REPLACE ":" ";" _na_pair "${_na_mod_n}")
        list(GET _na_pair 0 _na_mod)
        list(GET _na_pair 1 _na_want)
        set(_na_have 0)
        if(EXISTS "${SPU_DIR}/${_na_mod}/spu_recomp.c")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SPU_DIR}/${_na_mod}/spu_recomp.c")
            file(STRINGS "${SPU_DIR}/${_na_mod}/spu_recomp.c" _na_hooks REGEX "r3na_gate\\(ctx, [0-9]+, ")
            foreach(_na_line IN LISTS _na_hooks)
                string(REGEX MATCH "r3na_gate\\(ctx, ([0-9]+), " _na_m "${_na_line}")
                math(EXPR _r3na_hooks "${_r3na_hooks} | (1 << ${CMAKE_MATCH_1})")
                math(EXPR _na_have "${_na_have} + 1")
            endforeach()
        endif()
        if(NOT _na_have EQUAL _na_want)
            message(WARNING "${_na_mod}/spu_recomp.c has ${_na_have} of ${_na_want} R3_NATIVE_AUDIO hooks "
                            "(regen dropped them?): those kernels stay off. Run tools/r3_regen.py (tools/hooks/spu/${_na_mod}.json)")
        endif()
    endforeach()
endif()

add_executable(${PROJECT_NAME}
    main.cpp
    stubs.cpp
    port_hitchcatch.cpp   # PORT_HITCHCATCH=<ms>: key-thread stacks on frame hitches (off by default)
    r3_native_aa.cpp
    r3_pcport.cpp
    r3_patches.cpp
    r3_hot.cpp
    r3_hot_native.cpp   # R3_HOT_NATIVE (default on, =0 off): native memcpy/memset/strcmp/strcpy/memcmp (scratch\hot_r3\difftest)
    r3_pcfeat.cpp
    r3_pctext.cpp
    r3_trophies.cpp
    r3_mods.cpp
    mods_psarc.cpp
    r3_pcextra.cpp
    r3_menurows.cpp   # PC SETTINGS tab in the game's Options menu (R3_MENU_ROWS=0 off, 2 = rows in CONTROLS)
    ${PS3RECOMP_SCAFFOLD}
    ${HLE_NID_TABLE}
    ${RECOMP_SOURCES}
    ${SPU_SOURCES}
)

# The lifted SPU sources use bare #include "spu_context.h". Scope that include
# directory (and their flags) to those TUs alone: adding it to the whole target
# changes the compile command of every lifted PPU TU, and those are large enough
# that the needless rebuild is measured in tens of minutes.
#   SPU_USE_DISPATCH_MT -- route SPU_IB_DISPATCH through the musttail path.
#   -O2                 -- the target-wide -O1 exists for the huge PPU TUs; the
#                          SPU sources are small and decode-hot, so they get
#                          full optimization. The last -O on the line wins.
# Resistance port: RelWithDebInfo hands clang-cl /Ob1, which inlines ONLY
# functions spelt `inline` -- every plain static helper in the C runtime, and
# every in-class C++ method and lambda (std::atomic, lock guards...), stayed an
# out-of-line call, even under the -O2 the runtime adds (a later -O2 does not
# undo /Ob1). /Ob2 restores normal inlining for the runtime library and the
# hand-written sources. The lifted PPU and SPU TUs are left as they are: their
# helpers are already `static inline`, inlining lifted functions into each
# other would grow the host frames of deeply nested guest calls, and their
# command lines stay unchanged so they do not rebuild.
# -DPORT_FULL_INLINE=OFF builds the old way.
option(PORT_FULL_INLINE "Normal inlining (/Ob2) outside the lifted PPU code" ON)
if(PORT_FULL_INLINE AND MSVC)
    target_compile_options(ps3recomp_runtime PRIVATE /Ob2)
    set_source_files_properties(main.cpp stubs.cpp ${PS3RECOMP_SCAFFOLD}
        PROPERTIES COMPILE_OPTIONS "/Ob2")
endif()

if(SPU_SOURCES)
    # Resistance port: PORT_SPU_FAST compiles the per-access LS watch/probe check
    # (SPU_LS_NO_DBG) and the runtime SPU_XFLOAT_EXACT switch (SPU_XF_NO_EXACT)
    # out of the lifted SPU code. Each hid an out-of-line call behind a branch
    # on every LS access / float op, so the compiler kept every register in
    # memory: jobmod_6dc227fd went from 60k to 20k instructions.
    option(PORT_SPU_FAST "Lifted SPU code without LS debug probes / exact-float switch" ON)
    if(PORT_SPU_FAST)
        set(_spu_defs SPU_USE_DISPATCH_MT SPU_LS_NO_DBG SPU_XF_NO_EXACT)
    else()
        set(_spu_defs SPU_USE_DISPATCH_MT)
    endif()
    # Resistance port: inline MFC latch writes / status reads in the lifted SPU
    # code (spu_helpers.h). Runtime opt-out: SPU_INLINE_CH=0.
    option(PORT_SPU_INLINE_CH "Lifted SPU code: inline MFC parameter/status channel ops" ON)
    if(PORT_SPU_INLINE_CH)
        list(APPEND _spu_defs SPU_INLINE_CH)
    endif()
    # Resistance port: SPU_XF_RZ_TRUST drops the +-FLT_MAX output clamp of every
    # lifted float op (an identity under RZ, which the runtime then forces in
    # spu_thread_fp_init; 240M-op equivalence test in spu_native_1115.md).
    option(PORT_SPU_XF_RZ_TRUST "Lifted SPU float ops: rely on RZ for saturation" ON)
    if(PORT_SPU_FAST AND PORT_SPU_XF_RZ_TRUST)
        list(APPEND _spu_defs SPU_XF_RZ_TRUST)
        target_compile_definitions(ps3recomp_runtime PRIVATE SPU_XF_RZ_TRUST)
    endif()
    # Resistance port: SPU_W0()/SPU_H0() preferred-slot reads (tools/spu_w0.py) keep each
    # register local in one xmm instead of an SROA-split i32 + <3 x i32>. OFF = plain field read.
    option(PORT_SPU_W0_VEC "Lifted SPU code: vector preferred-slot reads" ON)
    if(NOT PORT_SPU_W0_VEC)
        list(APPEND _spu_defs SPU_W0_SCALAR)
    endif()
    set_source_files_properties(${SPU_SOURCES} PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
        COMPILE_DEFINITIONS "${_spu_defs}")
    if(_pic_srcs)
        set_property(SOURCE ${_pic_srcs} APPEND PROPERTY COMPILE_DEFINITIONS SPU_NO_DIRECT_HOP=1)
    endif()
    if(NOT MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # Resistance port: the SPU helpers have SSE4.1/AVX2 paths (pshufb shufb,
        # vector float ops) that the default x86-64 baseline compiled out --
        # shufb alone was ~20% of the SPU job thread as a byte loop.
        # -DPORT_SPU_AVX2=OFF builds the SSE4.1 baseline (the runtime needs SSE4.1 anyway).
        option(PORT_SPU_AVX2 "Build lifted SPU code with AVX2" OFF)
        if(PORT_SPU_AVX2 AND MSVC)
            set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2;/arch:AVX2;/GS-;/clang:-ffp-contract=off")
        elseif(PORT_SPU_AVX2)
            set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2;-mavx2;-ffp-contract=off")
        elseif(MSVC)
            set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2;/clang:-msse4.1;/clang:-ffp-contract=off")
        else()
            set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2;-msse4.1;-ffp-contract=off")
        endif()
    endif()
    if(PORT_SPU_AVX2)
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/r3_native_dof.c" APPEND PROPERTY
            COMPILE_DEFINITIONS R3_NATIVE_DOF_JOB_BUILT)
    endif()
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/r3_native_jobs.c" APPEND PROPERTY
        COMPILE_DEFINITIONS R3NJ_HOOKS=${_r3nj_hooks})
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/r3_native_audio.c" APPEND PROPERTY
        COMPILE_DEFINITIONS R3NA_HOOKS=${_r3na_hooks})
    # main.cpp's [native] line calls into r3_native_jobs.c / r3_native_audio.c only when they are built;
    # without lifted SPU code (PORT_ENABLE_SPU=OFF, configure before the regen) it shows them off.
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp" APPEND PROPERTY
        COMPILE_DEFINITIONS R3_SPU_NATIVE_BUILT)
endif()

# app.rc carries the icon, the version block and the manifest, and needs a
# resource compiler. Listing it as a source on a host that has none is not a
# link error, it is a "no rule to make" for a file nothing can consume.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
    target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    "${RECOMP_DIR}"                            # the generated ppu_recomp.h
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/memory"
    "${PS3RECOMP_DIR}/runtime/syscalls"
    "${PS3RECOMP_DIR}/runtime/platform"        # win32_compat.h and friends
    "${PS3RECOMP_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ps3recomp_runtime
)

# The runtime is C++, and on Apple also Objective-C. Letting CMake infer the
# link language from the generated C would leave the C++ standard library out.
set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

# Windows system import libraries. dbghelp symbolizes crash-handler addresses
# against the PDB, user32 and gdi32 are what the backend's window and its GDI
# present are made of, and winmm is timeBeginPeriod. None of the four names
# anything a POSIX linker can find; the D3D12 backend's own imports come in
# through the #pragma comment(lib) in libs/video/rsx_d3d12_backend.c.
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        dbghelp
        user32
        gdi32
        winmm
    )

    # RPCS3's RSX core (R2_RSX=rpcs3). Built separately by build.bat into
    # build_rpcs3 -- its C++23/glslang/yaml-cpp settings stay out of this
    # project -- and linked here when present; without it r2rsx_stub.c makes
    # RPCS3 mode unavailable.
    set(R2_RPCS3_RSX_BUILD "${CMAKE_SOURCE_DIR}/../build_rpcs3" CACHE PATH "rpcs3_rsx build dir")
    if(EXISTS "${R2_RPCS3_RSX_BUILD}/rpcs3_rsx.lib")
        set(_r "${R2_RPCS3_RSX_BUILD}")
        # build_rpcs3 generates vulkan-1.lib only when no Vulkan SDK is installed; with VULKAN_SDK set it links the SDK's.
        set(_vk "${_r}/vulkan-1.lib")
        if(NOT EXISTS "${_vk}" AND DEFINED ENV{VULKAN_SDK} AND EXISTS "$ENV{VULKAN_SDK}/Lib/vulkan-1.lib")
            set(_vk "$ENV{VULKAN_SDK}/Lib/vulkan-1.lib")
        endif()
        target_link_libraries(${PROJECT_NAME} PRIVATE
            "${_r}/rpcs3_rsx.lib"
            "${_vk}"
            "${_r}/rpcs3_asmjit.lib"
            "${_r}/3rdparty/glslang/glslang/SPIRV/SPIRV.lib"
            "${_r}/3rdparty/glslang/glslang/glslang/glslang.lib"
            "${_r}/3rdparty/glslang/glslang/glslang/glslang-default-resource-limits.lib"
            "${_r}/3rdparty/yaml-cpp/yaml-cpp/yaml-cpp.lib"
            pdh bcrypt ntdll shlwapi
        )
        target_compile_definitions(ps3recomp_runtime PRIVATE PS3RECOMP_HAVE_RPCS3_RSX=1)
        # RPCS3's threads take the exe's default stack size and the Vulkan
        # backend has multi-MB frames; rpcs3.exe links with 8 MB.
        target_link_options(${PROJECT_NAME} PRIVATE /STACK:8388608,1048576)
        message(STATUS "RSX:             RPCS3 core linked (${_r})")
        # build_rpcs3's code generation (ps3recomp/third_party/rpcs3/CMakeLists.txt RPCS3_RSX_*): ThinLTO
        # bitcode and PGO instrumentation need matching link options, applied in the CPU options block below.
        set(_rsx_cg_lto OFF)
        set(_rsx_cg_pgo off)
        if(EXISTS "${_r}/rpcs3_rsx_codegen.txt")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_r}/rpcs3_rsx_codegen.txt")
            file(STRINGS "${_r}/rpcs3_rsx_codegen.txt" _rsx_cg)
            foreach(_l IN LISTS _rsx_cg)
                if(_l MATCHES "^LTO=(ON|1|TRUE)$")
                    set(_rsx_cg_lto ON)
                elseif(_l MATCHES "^PGO=(gen|use)$")
                    set(_rsx_cg_pgo "${CMAKE_MATCH_1}")
                endif()
            endforeach()
            message(STATUS "RSX codegen:     ${_rsx_cg}")
        endif()
    endif()
endif()

# mods_psarc.cpp (mods folder: PSARC member / asset overrides) uses miniz. rpcs3_rsx.lib
# already contains it (r2_fossilize); without that library compile it here. The include
# directory is set on this one file so the other TUs keep their compile commands.
set(PORT_MINIZ_DIR "${PS3RECOMP_DIR}/third_party/fossilize/miniz")
set_source_files_properties(mods_psarc.cpp PROPERTIES INCLUDE_DIRECTORIES "${PORT_MINIZ_DIR}")
if(NOT (DEFINED R2_RPCS3_RSX_BUILD AND EXISTS "${R2_RPCS3_RSX_BUILD}/rpcs3_rsx.lib"))
    target_sources(${PROJECT_NAME} PRIVATE "${PORT_MINIZ_DIR}/miniz.c")
    set_source_files_properties("${PORT_MINIZ_DIR}/miniz.c" PROPERTIES COMPILE_OPTIONS "-w")
endif()

# ---------------------------------------------------------------------------
# Compiler-specific flags
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    # clang-cl reads -Wall as /Wall (= -Weverything): hundreds of MB of warnings
    # in the build log and console. /W3 is its usual level.
    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        target_compile_options(${PROJECT_NAME} PRIVATE /W3)
    else()
        target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
    target_compile_options(${PROJECT_NAME} PRIVATE
        -fno-strict-aliasing
        # The lift spells PPC arithmetic as signed C arithmetic, which must wrap;
        # without this, clang exploits overflow UB (e.g. folds "x*x < 0" to false).
        -fwrapv
    )
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
        /Zc:__cplusplus         # report correct __cplusplus value
        # ppu_recomp.h defines PPU_FENCE, the lowering of the guest's
        # sync/lwsync/eieio/isync, and in C that is <stdatomic.h>. The lifter
        # writes C++ by default, but --single-file writes ppu_recomp.c, and a
        # port that took it would not compile without this. The runtime's own
        # build already carries the same flag for the same reason.
        /experimental:c11atomics
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# The lifted translation units are machine-generated and can run to hundreds of
# thousands of lines each, so they get their own flags:
#
#   - warnings off. They are about generated code and nobody will act on them,
#     and at this volume they bury the diagnostics that matter.
#   - no floating-point contraction. arm64 fuses a multiply and an add by
#     default; a PPC fmadd is spelt as one where the lifter means one.
#   - /bigobj on MSVC. A single lifted file routinely passes the 65,536-section
#     limit of the default object format.
# Resistance port: the lifted PPU code also targets x86-64-v3 (AVX2/FMA/BMI2/
# MOVBE -- any Zen/Haswell or later). Without FMA every PPC fmadd lowered to a
# CALL into the CRT's software fma() (900+ call sites in one lifted file), and
# without SSE4.1 nearbyint/trunc were calls too. Contraction stays off so only
# the lifter's explicit fma() fuses. -DPORT_PPU_V3=OFF builds the baseline.
option(PORT_PPU_V3 "Build lifted PPU code for x86-64-v3" ON)
# v163: the lifted PPU code takes the guest-memory bounds as constants and its
# slow paths through preserve_most wrappers (ppu_recomp.h). -DPORT_PPU_FIXED=OFF
# builds the old runtime-bounds path.
option(PORT_PPU_FIXED "Lifted PPU code: constant VM bounds + preserve_most slow paths" ON)
if(PORT_PPU_FIXED)
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES COMPILE_DEFINITIONS "PPU_FIXED_BOUNDS")
endif()
# The lifted TUs' only thread_local (g_trampoline_fn, constant-initialised to
# nullptr in ppu_loader.cpp) otherwise gets clang-cl's on-demand TLS init guard
# at every trampoline drain. -DPORT_PPU_TLS_NOGUARD=OFF restores the guards.
option(PORT_PPU_TLS_NOGUARD "Lifted PPU code: no TLS init guards" ON)
set(_ppu_tlsg "")
if(MSVC AND PORT_PPU_TLS_NOGUARD)
    set(_ppu_tlsg ";/Zc:tlsGuards-")
endif()
if(MSVC AND PORT_PPU_V3)
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/w;/bigobj;/clang:-O2;/clang:-march=x86-64-v3;/clang:-ffp-contract=off;/clang:-fno-math-errno${_ppu_tlsg}")
elseif(MSVC)
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/w;/bigobj;/clang:-O2;/clang:-msse4.1;/clang:-ffp-contract=off${_ppu_tlsg}")
else()
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-w;-ffp-contract=off")
endif()

# ---------------------------------------------------------------------------
# Memory-aware build (Setup.exe, docs\INSTALL.md): one lifted ppu_recomp_*.cpp
# can take up to ~8 GB of RAM in clang-cl, so those files get their own Ninja
# job pool "ppu" and the rest of the build keeps the normal -j. Pool size:
# -DPORT_PPU_JOBS=N or the PORT_PPU_JOBS environment variable (Setup.exe sets
# it from free RAM), else max(1, (total RAM GB - 6) / 8). Per-file pools need
# CMake 4.2+; older CMake puts the whole game target in the pool (slower, same
# memory bound). -DPORT_PPU_JOB_POOL=OFF restores the old behaviour.
# ---------------------------------------------------------------------------
option(PORT_PPU_JOB_POOL "Limit parallel compiles of the lifted PPU code by RAM (Ninja job pool)" ON)
if(PORT_PPU_JOB_POOL AND CMAKE_GENERATOR MATCHES "Ninja")
    if(DEFINED PORT_PPU_JOBS AND NOT "${PORT_PPU_JOBS}" STREQUAL "")
        set(_ppu_jobs "${PORT_PPU_JOBS}")
    elseif(NOT "$ENV{PORT_PPU_JOBS}" STREQUAL "")
        set(_ppu_jobs "$ENV{PORT_PPU_JOBS}")
    else()
        cmake_host_system_information(RESULT _ram_mb QUERY TOTAL_PHYSICAL_MEMORY)
        math(EXPR _ppu_jobs "(${_ram_mb} / 1024 - 6) / 8")
    endif()
    if(NOT _ppu_jobs MATCHES "^[0-9]+$" OR _ppu_jobs LESS 1)
        set(_ppu_jobs 1)
    endif()
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS ppu=${_ppu_jobs})
    set(_ppu_pool_srcs ${RECOMP_SOURCES})
    list(FILTER _ppu_pool_srcs INCLUDE REGEX "ppu_recomp_[0-9]+\\.cpp$")
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.2)
        set_property(SOURCE ${_ppu_pool_srcs} PROPERTY JOB_POOL_COMPILE ppu)
        message(STATUS "PPU job pool:    ${_ppu_jobs} parallel lifted-PPU compile(s)")
    else()
        set_property(TARGET ${PROJECT_NAME} PROPERTY JOB_POOL_COMPILE ppu)
        message(STATUS "PPU job pool:    ${_ppu_jobs} (CMake < 4.2: whole game target)")
    endif()
endif()
# ---------------------------------------------------------------------------
# Resistance port: optional Tracy timeline (PORT_TRACY=ON; normal builds are
# unchanged -- no Tracy sources, no PORT_TRACY define). Zones: lv2 syscalls,
# HLE imports, SPU jobs and blocking channel waits, guest spin-waits (merged
# laps) and "RSX flip" frame marks; see port_tracy.h / port_tracy.cpp.
# ---------------------------------------------------------------------------
option(PORT_TRACY "Build with Tracy profiler zones (connect the Tracy viewer)" OFF)
set(PORT_TRACY_DIR "$ENV{PORT_TRACY_DIR}" CACHE PATH "Tracy source checkout (v0.15; required with PORT_TRACY=ON)")
if(PORT_TRACY)
    if(NOT EXISTS "${PORT_TRACY_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "PORT_TRACY=ON: no Tracy checkout at PORT_TRACY_DIR=${PORT_TRACY_DIR}")
    endif()
    set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
    set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
    # The port's VEH commits guest pages on access violations; Tracy's crash
    # handler would treat those as crashes.
    set(TRACY_NO_CRASH_HANDLER ON CACHE BOOL "" FORCE)
    add_subdirectory("${PORT_TRACY_DIR}" "${CMAKE_BINARY_DIR}/tracy" EXCLUDE_FROM_ALL)
    target_sources(${PROJECT_NAME} PRIVATE port_tracy.cpp)
    target_link_libraries(${PROJECT_NAME} PRIVATE Tracy::TracyClient)
    target_compile_definitions(${PROJECT_NAME} PRIVATE PORT_TRACY)
    target_compile_definitions(ps3recomp_runtime PRIVATE PORT_TRACY)
    message(STATUS "Tracy:           ON (${PORT_TRACY_DIR}, on-demand, no crash handler)")
endif()

# Resistance port: an AVX2 build (anything but PORT_ARCH=baseline) needs x86-64-v3;
# port_cpucheck.h (included by main.cpp) then refuses older CPUs with a clear message
# instead of an illegal-instruction crash.
option(PORT_CPU_CHECK "Startup check for the CPU features the build needs: AVX2, or SSE4.1 for PORT_ARCH=baseline (port_cpucheck.h)" ON)
if(PORT_CPU_CHECK AND (PORT_PPU_V3 OR PORT_SPU_AVX2 OR PORT_ARCH STREQUAL "AVX2"))
    target_compile_definitions(${PROJECT_NAME} PRIVATE PORT_REQUIRE_X86_64_V3=1)
elseif(PORT_CPU_CHECK)
    # baseline: the runtime and the lifted code are built with -msse4.1
    target_compile_definitions(${PROJECT_NAME} PRIVATE PORT_REQUIRE_SSE41=1)
endif()

# ---------------------------------------------------------------------------
# Resistance port: CPU build options, applied (see the PORT_ARCH block above)
# ---------------------------------------------------------------------------
if(MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Hand-written sources of the exe = everything but the lifted PPU/SPU code
    # (taken from the target, so files other agents add are covered too).
    get_target_property(_all_srcs ${PROJECT_NAME} SOURCES)
    set(_hand_srcs "")
    foreach(s IN LISTS _all_srcs)
        if(s MATCHES "\\.(c|cc|cpp|cxx)$" AND NOT s MATCHES "ppu_recomp_[0-9a-z_]+\\.cpp$")
            list(FIND SPU_SOURCES "${s}" _i)
            if(_i EQUAL -1)
                list(APPEND _hand_srcs "${s}")
            endif()
        endif()
    endforeach()
    set(_ppu_srcs ${RECOMP_PPU_COMPILED})
    list(FILTER _ppu_srcs INCLUDE REGEX "ppu_recomp_[0-9]+\\.cpp$")
    # PORT_FULL_INLINE also covers the hand-written sources added after the
    # list above (patches, PC features, mods, the HLE NID table); they were
    # still /Ob1, i.e. only functions spelt `inline` were inlined.
    if(PORT_FULL_INLINE)
        foreach(s IN LISTS _hand_srcs)
            get_source_file_property(_o "${s}" COMPILE_OPTIONS)
            if(NOT _o MATCHES "/Ob2")
                set_property(SOURCE "${s}" APPEND PROPERTY COMPILE_OPTIONS "/Ob2")
            endif()
        endforeach()
    endif()

    # clang-cl guards every TLS access with an on-demand-init check
    # (/Zc:tlsGuards: load + compare + branch + a second TLS address
    # computation). C has no dynamically initialised thread-locals and every
    # thread of an exe runs the TLS callback before user code, so in C sources
    # the guard is dead code -- ~70k sites in the lifted SPU code alone.
    option(PORT_C_TLS_NOGUARD "Drop clang-cl's TLS init guards in C sources" ON)
    # /debug makes lld-link default to /OPT:NOREF; REF drops unreferenced
    # COMDATs only (298 -> 283 MB on R3), relink time unchanged. Nothing in
    # the port locates code by section or by symbol name. ICF is left off.
    option(PORT_LINK_OPT_REF "Link with /OPT:REF (drop unreferenced functions/data)" ON)
    if(PORT_LINK_OPT_REF)
        target_link_options(${PROJECT_NAME} PRIVATE /OPT:REF)
    endif()
    if(PORT_C_TLS_NOGUARD)
        target_compile_options(ps3recomp_runtime PRIVATE $<$<COMPILE_LANGUAGE:C>:/Zc:tlsGuards->)
        target_compile_options(${PROJECT_NAME} PRIVATE $<$<COMPILE_LANGUAGE:C>:/Zc:tlsGuards->)
    endif()
    # /GS- drops clang-cl's stack-cookie checks (on by default: ~2300 sites in the runtime library) from the
    # runtime and the hand-written code; /Gw gives data its own COMDATs for /OPT:REF. No float or other
    # semantic change. The lifted SPU code already builds with /GS-, the lifted PPU code has no cookie sites,
    # and neither is touched here (the PPU objects are not recompiled).
    option(PORT_GS_OFF "/GS- and /Gw for the runtime and the hand-written code" ON)
    if(PORT_GS_OFF)
        target_compile_options(ps3recomp_runtime PRIVATE /GS- /Gw)
        set_property(SOURCE ${_hand_srcs} APPEND PROPERTY COMPILE_OPTIONS "/GS-;/Gw")
    endif()
    # rpcs3_rsx.lib built as ThinLTO bitcode (RPCS3_RSX_LTO, see the RSX block above): lld-link runs its
    # ThinLTO backends at every exe link; the cache keeps a relink after a small change to the modules that
    # changed. PORT_LTO adds the same options for its own LTO below.
    if(_rsx_cg_lto AND NOT PORT_LTO)
        target_link_options(${PROJECT_NAME} PRIVATE /INCREMENTAL:NO)
        if(PORT_LTO_CACHE)
            target_link_options(${PROJECT_NAME} PRIVATE "/lldltocache:${CMAKE_BINARY_DIR}/lto_cache")
        endif()
        if(NOT "${PORT_LTO_JOBS}" STREQUAL "")
            target_link_options(${PROJECT_NAME} PRIVATE /opt:lldltojobs=${PORT_LTO_JOBS})
        endif()
        message(STATUS "LTO:             rpcs3_rsx.lib is ThinLTO bitcode (RPCS3_RSX_LTO); the rest of the exe is not")
    endif()
    # An instrumented rpcs3_rsx.lib (RPCS3_RSX_PGO=gen) needs the profile runtime even when the exe is not
    # instrumented itself.
    if(_rsx_cg_pgo STREQUAL "gen" AND NOT PORT_PGO STREQUAL "gen")
        get_filename_component(_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
        file(GLOB _prof_rt "${_llvm_bin}/../lib/clang/*/lib/windows/clang_rt.profile-x86_64.lib")
        if(NOT _prof_rt)
            message(FATAL_ERROR "instrumented rpcs3_rsx.lib: clang_rt.profile-x86_64.lib not found under ${_llvm_bin}/../lib/clang")
        endif()
        list(GET _prof_rt 0 _prof_rt)
        target_link_libraries(${PROJECT_NAME} PRIVATE "${_prof_rt}")
        message(WARNING "rpcs3_rsx.lib is PGO-instrumented but PORT_PGO is not gen: counters are only written at exit")
    endif()

    if(PORT_ARCH STREQUAL "AVX2")
        # x86-64-v3 = what the lifted PPU code and port_cpucheck.h use. Contraction stays
        # off: the runtime's float paths were validated without FMA, and a fused a*b+c
        # rounds differently from the PS3's separate multiply and add.
        target_compile_options(ps3recomp_runtime PRIVATE /clang:-march=x86-64-v3 /clang:-ffp-contract=off)
        set_property(SOURCE ${_hand_srcs} APPEND PROPERTY COMPILE_OPTIONS "/clang:-march=x86-64-v3;/clang:-ffp-contract=off")
        message(STATUS "CPU target:      AVX2 / x86-64-v3 (runtime + hand-written + lifted)")
    elseif(PORT_ARCH STREQUAL "mixed")
        message(STATUS "CPU target:      mixed (AVX2 lifted code, x86-64 + SSE4.1 runtime and hand-written code)")
    elseif(PORT_ARCH STREQUAL "baseline")
        message(STATUS "CPU target:      baseline x86-64 + SSE4.1 (no AVX; PORT_PPU_V3/PORT_SPU_AVX2 off)")
    endif()

    set(_opt_srcs ${_hand_srcs} ${SPU_SOURCES})
    if(PORT_LTO)
        set(_lto_srcs ${_opt_srcs})
        set(_lto_ppu "")
        if(PORT_LTO_PPU)
            set(_lto_ppu ${_ppu_srcs})
            if(PORT_THINLTO AND PORT_THINLTO_SCOPE STREQUAL "hot")
                set(_lto_ppu "")
                foreach(s IN LISTS _ppu_srcs)
                    get_filename_component(_n "${s}" NAME_WE)
                    if(_n IN_LIST PORT_LTO_PPU_ONLY)
                        list(APPEND _lto_ppu "${s}")
                    endif()
                endforeach()
            endif()
            list(APPEND _lto_srcs ${_lto_ppu})
        endif()
        list(LENGTH _lto_ppu _lto_nppu)
        target_compile_options(ps3recomp_runtime PRIVATE -flto=thin)
        set_property(SOURCE ${_lto_srcs} APPEND PROPERTY COMPILE_OPTIONS "-flto=thin")
        if(PORT_THINLTO)
            # Per-variable COMDATs so /OPT:REF can drop dead data in the non-LTO objects too.
            target_compile_options(${PROJECT_NAME} PRIVATE /Gw)
            target_compile_options(ps3recomp_runtime PRIVATE /Gw)
        endif()
        # lld-link runs the ThinLTO backends; a backend of a lifted PPU module
        # needs about as much RAM as compiling it, so with PPU modules in LTO
        # the thread count defaults to the RAM-based PPU job pool size.
        if(NOT "${PORT_LTO_JOBS}" STREQUAL "")
            set(_lto_jobs "${PORT_LTO_JOBS}")
        elseif(_lto_nppu GREATER 0)
            cmake_host_system_information(RESULT _ram_mb QUERY TOTAL_PHYSICAL_MEMORY)
            math(EXPR _lto_jobs "(${_ram_mb} / 1024 - 6) / 8")
            if(_lto_jobs LESS 1)
                set(_lto_jobs 1)
            endif()
        else()
            set(_lto_jobs all)
        endif()
        target_link_options(${PROJECT_NAME} PRIVATE /opt:lldltojobs=${_lto_jobs} /INCREMENTAL:NO)
        # The cache lets a relink after a small change re-optimise only the modules that changed.
        if(PORT_LTO_CACHE)
            target_link_options(${PROJECT_NAME} PRIVATE "/lldltocache:${CMAKE_BINARY_DIR}/lto_cache")
        endif()
        message(STATUS "LTO:             ThinLTO (runtime, hand-written, SPU; lifted PPU files: ${_lto_nppu}; backend jobs: ${_lto_jobs})")
    endif()
    if(PORT_LINK_ICF)
        target_link_options(${PROJECT_NAME} PRIVATE /OPT:SAFEICF)
    endif()

    if(PORT_PGO STREQUAL "gen" OR PORT_PGO STREQUAL "use")
        set(_pgo_srcs ${_opt_srcs})
        if(PORT_PGO_PPU)
            list(APPEND _pgo_srcs ${_ppu_srcs})
        endif()
        if(PORT_PGO STREQUAL "gen")
            set(_pgo_flag "-fprofile-instr-generate")
            get_filename_component(_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
            file(GLOB _prof_rt "${_llvm_bin}/../lib/clang/*/lib/windows/clang_rt.profile-x86_64.lib")
            if(NOT _prof_rt)
                message(FATAL_ERROR "PORT_PGO=gen: clang_rt.profile-x86_64.lib not found under ${_llvm_bin}/../lib/clang")
            endif()
            list(GET _prof_rt 0 _prof_rt)
            target_link_libraries(${PROJECT_NAME} PRIVATE "${_prof_rt}")
            # main.cpp (cpu_profile.inc) flushes the counters every 30 s and at exit.
            set_property(SOURCE main.cpp APPEND PROPERTY COMPILE_DEFINITIONS PORT_PGO_GEN)
            message(STATUS "PGO:             instrumented (writes LLVM_PROFILE_FILE)")
        else()
            if(NOT EXISTS "${PORT_PGO_PROFILE}")
                message(FATAL_ERROR "PORT_PGO=use needs PORT_PGO_PROFILE=<merged .profdata> (got '${PORT_PGO_PROFILE}')")
            endif()
            file(TO_NATIVE_PATH "${PORT_PGO_PROFILE}" _pd)
            set(_pgo_flag "-fprofile-instr-use=${_pd};-Wno-profile-instr-unprofiled;-Wno-profile-instr-out-of-date;-Wno-backend-plugin")
            message(STATUS "PGO:             optimising with ${PORT_PGO_PROFILE}")
        endif()
        target_compile_options(ps3recomp_runtime PRIVATE ${_pgo_flag})
        set_property(SOURCE ${_pgo_srcs} APPEND PROPERTY COMPILE_OPTIONS "${_pgo_flag}")
        if(PORT_PGO STREQUAL "use")
            # The profile is an input of every PGO object: a re-merged .profdata must recompile them
            # (Ninja does not track it otherwise). The runtime's sources live in another directory scope.
            set_property(SOURCE ${_pgo_srcs} APPEND PROPERTY OBJECT_DEPENDS "${PORT_PGO_PROFILE}")
            get_target_property(_rt_srcs ps3recomp_runtime SOURCES)
            get_target_property(_rt_dir ps3recomp_runtime SOURCE_DIR)
            set(_rt_abs "")
            foreach(s IN LISTS _rt_srcs)
                if(s MATCHES "\\.(c|cc|cpp|cxx)$" AND NOT s MATCHES "^\\$<")
                    if(NOT IS_ABSOLUTE "${s}")
                        set(s "${_rt_dir}/${s}")
                    endif()
                    list(APPEND _rt_abs "${s}")
                endif()
            endforeach()
            set_property(SOURCE ${_rt_abs} TARGET_DIRECTORY ps3recomp_runtime APPEND PROPERTY OBJECT_DEPENDS "${PORT_PGO_PROFILE}")
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
