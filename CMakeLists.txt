cmake_minimum_required(VERSION 3.20)
# C is only for CDetour's asm.c, which the AMXX module needs.
project(rebench_mm VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT, so the DLL needs no redistributable on the server.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# HLDS is 32-bit. MSVC gets this from the generator; GCC needs it in the flags.
# CMAKE_C_FLAGS matters as much as the C++ one: CDetour's asm.c is the only C in
# the project, and without it that object alone comes out 64-bit and the module
# fails to link.
if(NOT MSVC)
    foreach(_flags CMAKE_C_FLAGS CMAKE_CXX_FLAGS CMAKE_EXE_LINKER_FLAGS CMAKE_SHARED_LINKER_FLAGS)
        set(${_flags} "${${_flags}} -m32")
    endforeach()
endif()

set(REBENCH_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/sdk" CACHE PATH
    "Directory containing cssdk/ and metamod/")

foreach(_required
    "${REBENCH_SDK_ROOT}/cssdk/common"
    "${REBENCH_SDK_ROOT}/cssdk/dlls"
    "${REBENCH_SDK_ROOT}/cssdk/engine"
    "${REBENCH_SDK_ROOT}/cssdk/public"
    "${REBENCH_SDK_ROOT}/metamod")
    if(NOT IS_DIRECTORY "${_required}")
        message(FATAL_ERROR "Missing SDK directory: ${_required}. Set REBENCH_SDK_ROOT.")
    endif()
endforeach()

add_library(rebench_mm SHARED
    src/main.cpp
    src/amxx.cpp
    src/clock.cpp
    src/engine.cpp
    src/gamedll.cpp
    src/histogram.cpp
    src/micro.cpp
    src/phases.cpp
    src/probe.cpp
    src/regame.cpp
    src/rehlds.cpp
    src/report.cpp
    src/sysinfo.cpp)

if(WIN32)
    target_sources(rebench_mm PRIVATE src/rebench_mm.def)
endif()

# SYSTEM: the HLSDK and Metamod headers raise warnings we do not control.
target_include_directories(rebench_mm SYSTEM PRIVATE
    ${REBENCH_SDK_ROOT}/cssdk/common
    ${REBENCH_SDK_ROOT}/cssdk/dlls
    ${REBENCH_SDK_ROOT}/cssdk/engine
    ${REBENCH_SDK_ROOT}/cssdk/game_shared
    ${REBENCH_SDK_ROOT}/cssdk/pm_shared
    ${REBENCH_SDK_ROOT}/cssdk/public
    ${REBENCH_SDK_ROOT}/metamod)

# The engine probe header, vendored from the rebench ReHLDS fork.
target_include_directories(rebench_mm PRIVATE ${REBENCH_SDK_ROOT}/rehlds)

target_compile_definitions(rebench_mm PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    REBENCH_VERSION="${PROJECT_VERSION}")

if(MSVC)
    target_compile_options(rebench_mm PRIVATE /W4 /EHsc)
    target_link_options(rebench_mm PRIVATE /MACHINE:X86)
else()
    target_compile_options(rebench_mm PRIVATE -Wall -Wextra -Wno-write-strings)
    # The vendored HLSDK's tier0/dbg.h, reached through regamedll_api.h, calls
    # _vsnprintf: an MSVC spelling the SDK maps for _snprintf (port.h) but not
    # for this one, so it only shows up on a GCC build.
    target_compile_definitions(rebench_mm PRIVATE _vsnprintf=vsnprintf)
    # HLDS ships its own libstdc++ (GLIBCXX_3.4.16) and runs with
    # LD_LIBRARY_PATH=., so that copy shadows the system one and a modern
    # toolchain's symbols go unresolved. Link the runtime in and keep its
    # symbols hidden so they cannot interpose on the rest of the process.
    target_link_options(rebench_mm PRIVATE
        -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/rebench_mm.map")
    target_link_libraries(rebench_mm PRIVATE dl)
    set_target_properties(rebench_mm PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rebench_mm.map")
endif()

# Metamod's Linux plugins carry the arch suffix by convention.
set_target_properties(rebench_mm PROPERTIES
    PREFIX ""
    OUTPUT_NAME $<IF:$<BOOL:${WIN32}>,rebench_mm,rebench_mm_i386>)

# The AMXX module. Separate binary because AMXX loads modules from
# modules/<name>_amxx.dll and nowhere else, so it cannot be the same file as
# the Metamod plugin. It builds the same clock, so both halves pick the same
# source and report ns; each calibrates its own, so they agree to within the
# calibration error rather than exactly.
add_library(rebench_amxx SHARED
    amxx/module.cpp
    src/clock.cpp
    src/sysinfo.cpp
    sdk/amxx/CDetour/detours.cpp
    sdk/amxx/CDetour/asm/asm.c)

target_include_directories(rebench_amxx SYSTEM PRIVATE ${REBENCH_SDK_ROOT}/amxx)

# Vendored as-is from AMXX, warnings and all.
set_source_files_properties(
    sdk/amxx/CDetour/detours.cpp
    sdk/amxx/CDetour/asm/asm.c
    PROPERTIES COMPILE_OPTIONS $<IF:$<BOOL:${MSVC}>,/W0,-w>)

target_compile_definitions(rebench_amxx PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    REBENCH_VERSION="${PROJECT_VERSION}")

if(MSVC)
    # CDetour's page protection helpers key off WIN32, which MSVC does not
    # define on its own.
    target_compile_definitions(rebench_amxx PRIVATE WIN32)
    target_compile_options(rebench_amxx PRIVATE /W4 /EHsc)
    target_link_options(rebench_amxx PRIVATE /MACHINE:X86)
else()
    target_compile_options(rebench_amxx PRIVATE -Wall -Wextra)
    target_link_options(rebench_amxx PRIVATE
        -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL)
    # C as well as CXX: CDetour's asm.c has generic external names
    # (copy_bytes, fill_nop, inject_jmp) and the AMXX core carries its own copy
    # of the same file, so leaving them global invites the two to interpose on
    # each other.
    set_target_properties(rebench_amxx PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden)
endif()

# AMXX rewrites a modules.ini name to modules/<name>_amxx<arch>.<ext>, so the
# file must be called this and the line in modules.ini is just "rebench".
set_target_properties(rebench_amxx PROPERTIES
    PREFIX ""
    OUTPUT_NAME $<IF:$<BOOL:${WIN32}>,rebench_amxx,rebench_amxx_i386>)

# Compiled rather than read from CMAKE_SIZEOF_VOID_P, which was probed before
# -m32 reached the flags and would still say 8 on a 64-bit host.
include(CheckTypeSize)
check_type_size("void*" REBENCH_POINTER_SIZE LANGUAGE CXX)
if(NOT REBENCH_POINTER_SIZE EQUAL 4)
    message(FATAL_ERROR
        "HLDS/Metamod is 32-bit but this configuration builds ${REBENCH_POINTER_SIZE}-byte "
        "pointers. Use a Win32/x86 generator, or install 32-bit development libraries "
        "(g++-multilib, libc6-dev-i386).")
endif()
