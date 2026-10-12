cmake_minimum_required(VERSION 3.20)
project(model3recomp C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# No fused multiply-add the guest did not ask for. Lifted code writes a
# PowerPC fmadds as F32(a * b + c) in double; GCC for the SH-4 narrows that to
# single precision and contracts it into fmac (GNU C defaults to
# -ffp-contract=fast), which rounds differently. 575 lifted functions had one,
# and a scripted race parted from the desktop's at field 4234 (DC-BUG-017).
add_compile_options(-ffp-contract=off)
# sqrt as the instruction: no errno to set, so __builtin_sqrt is fsqrt on the
# SH-4 and sqrtsd on x86, both correctly rounded. With errno, and with KOS's
# -fno-builtin, every square root was newlib's bit-by-bit software one
# (5 % of a race field).
add_compile_options(-fno-math-errno)

# SDL2 is needed for the platform layer and the example, but not for the
# semantic tests -- so a machine without it can still run the harness.
find_package(SDL2 QUIET)

set(M3_SOURCES
    src/model3recomp.c
    src/ppc.c
    src/bus.c
    src/func_table.c
    src/irq.c
    src/scsi.c
    src/jtag.c
    src/screenshot.c
    src/scenedump.c
    src/real3d.c
    src/tilegen.c
    src/input.c
    src/netplay.c
    src/savestate.c
    src/scsp.c
    src/sound.c
    src/m68k/m68kcpu.c
    src/m68k/m68kops.c
    src/m68k/softfloat/softfloat.c
)
# A Dreamcast build (KOS's kallistios.toolchain.cmake sets the system name)
# has no SDL and no sockets: its own platform layer, and no netplay.
if(CMAKE_SYSTEM_NAME STREQUAL "dreamcast")
    set(M3_DREAMCAST ON)
    set(SDL2_FOUND OFF)
    # The sound board runs, but its voices are the AICA's (aica_voices.c):
    # chips that only keep time, a 68000 with only its own cycle table.
    # Guest work RAM is 3 MB: Scud Race's race never goes above 2 MB, and an
    # access above the window is counted and reported, never silent.
    list(REMOVE_ITEM M3_SOURCES src/netplay.c)
    list(APPEND M3_SOURCES src/netplay_null.c
         src/platform/dreamcast/platform_dreamcast.c)
    set_source_files_properties(src/scsp.c PROPERTIES COMPILE_DEFINITIONS SCSP_NO_SYNTH)
    set_source_files_properties(src/m68k/m68kops.c src/m68k/m68kcpu.c
        PROPERTIES COMPILE_DEFINITIONS M68K_ONLY_68000)
elseif(SDL2_FOUND)
    list(APPEND M3_SOURCES src/platform_sdl.c)
else()
    # Headless build: the library is still fully usable for the boot harness
    # and the conformance runs, just without a window.
    list(APPEND M3_SOURCES src/platform_null.c)
endif()

add_library(model3recomp STATIC ${M3_SOURCES})

target_include_directories(model3recomp
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(SDL2_FOUND)
    target_link_libraries(model3recomp PUBLIC SDL2::SDL2)
else()
    message(STATUS "SDL2 not found -- building headless (src/platform_null.c)")
endif()

if(WIN32)
    target_link_libraries(model3recomp PUBLIC ws2_32)
endif()
if(M3_DREAMCAST)
    target_compile_definitions(model3recomp PUBLIC M3_DREAMCAST=1
        M3_RAM_BYTES=0x300000 M3_CROM_CACHE_KB=1024u M3_VROM_CACHE_KB=1024u)
    # KOS keeps pthreads in an addon (the sound board and the banded
    # rasteriser use them).
    target_link_libraries(model3recomp PUBLIC pthread)
endif()

if(NOT MSVC)
    target_link_libraries(model3recomp PUBLIC m)
    target_compile_options(model3recomp PRIVATE -Wall -Wextra -Wno-unused-parameter)
else()
    target_compile_options(model3recomp PRIVATE /W3)
    target_compile_definitions(model3recomp PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Instrument the lifted code so a guest that spins says which of its own
# functions it is spinning in. PUBLIC because the lifted translation units
# live in the parent project and are where the counters actually are.
# Configure a second build directory for it; flipping it in place relifts
# nothing but recompiles everything.
option(MODEL3RECOMP_LOOP_GUARD "Locate guest spin loops (see lift.h)" OFF)
if(MODEL3RECOMP_LOOP_GUARD)
    target_compile_definitions(model3recomp PUBLIC M3_LOOP_GUARD)
endif()
# Every lifted function entry reports to m3_fn_trace() (M3_FNSIG): for
# narrowing a divergence between two hosts to a function. Diagnostic only.
option(MODEL3RECOMP_FN_TRACE "Hash the registers at function entries (M3_FNSIG)" OFF)
if(MODEL3RECOMP_FN_TRACE)
    target_compile_definitions(model3recomp PUBLIC M3_FN_TRACE)
endif()

# Report every store that covers this guest address, with the function that
# did it (needs MODEL3RECOMP_LOOP_GUARD for the function name). See M3_WATCH
# in lift.h.
set(MODEL3RECOMP_WATCH "" CACHE STRING "Guest RAM address to watch, e.g. 0x6FB")
if(MODEL3RECOMP_WATCH)
    target_compile_definitions(model3recomp PUBLIC M3_WATCH=${MODEL3RECOMP_WATCH})
endif()

# --- Tests ---
option(MODEL3RECOMP_BUILD_TESTS "Build test programs" ON)
if(MODEL3RECOMP_BUILD_TESTS)
    enable_testing()
    add_executable(model3recomp_test_ppc tests/test_ppc.c)
    target_link_libraries(model3recomp_test_ppc PRIVATE model3recomp)
    add_test(NAME ppc_semantics COMMAND model3recomp_test_ppc)

    # Lifted PowerPC end to end: assemble, lift at build time, run, check.
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        set(SMOKE_DIR ${CMAKE_CURRENT_BINARY_DIR}/guest_smoke)
        set(SMOKE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/tests/dreamcast/guest_smoke)
        add_custom_command(
            OUTPUT ${SMOKE_DIR}/smoke_code_000.c ${SMOKE_DIR}/smoke_register.c
                   ${SMOKE_DIR}/smoke_funcs.h
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SMOKE_DIR}
            COMMAND ${Python3_EXECUTABLE} ${SMOKE_SRC}/make_guest.py ${SMOKE_DIR}/guest.bin
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/ppc_lifter.py
                    ${SMOKE_DIR}/guest.bin ${SMOKE_DIR} smoke --base 0x00010000
            DEPENDS ${SMOKE_SRC}/make_guest.py tools/ppc_lifter.py tools/ppc_disasm.py
            COMMENT "Lifting the guest_smoke PowerPC program")
        add_executable(model3recomp_guest_smoke ${SMOKE_SRC}/main.c
            ${SMOKE_DIR}/smoke_code_000.c ${SMOKE_DIR}/smoke_register.c)
        target_include_directories(model3recomp_guest_smoke PRIVATE ${SMOKE_DIR})
        target_link_libraries(model3recomp_guest_smoke PRIVATE model3recomp)
        add_test(NAME guest_smoke COMMAND model3recomp_guest_smoke)
        add_test(NAME disasm_cases COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_disasm.py)
        add_test(NAME lifter_cases COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_lifter.py)
    endif()
endif()

# --- Scud Race (optional) ---
# Point SCUD_LIFTED_DIR at the output of tools/scud/lift.py. That code is
# generated from your own dump and never lives in this repository.
set(SCUD_LIFTED_DIR "" CACHE PATH "Directory of lifted Scud Race C (tools/scud/lift.py)")
if(SCUD_LIFTED_DIR)
    file(GLOB SCUD_LIFTED CONFIGURE_DEPENDS "${SCUD_LIFTED_DIR}/scud*.c")
    if(NOT SCUD_LIFTED)
        message(FATAL_ERROR "SCUD_LIFTED_DIR has no scud*.c -- run tools/scud/lift.py")
    endif()
    if(M3_DREAMCAST)
        set(SCUD_MAIN src/games/scud/main_dc.c src/renderer/pvr/pvr_r3d.c
                      src/platform/dreamcast/aica_voices.c)
    else()
        set(SCUD_MAIN src/games/scud/main_host.c)
    endif()
    add_executable(scudrace ${SCUD_MAIN} src/games/scud/scud.c
                   src/games/scud/scudpack.c ${SCUD_LIFTED})
    target_include_directories(scudrace PRIVATE ${SCUD_LIFTED_DIR} src/games/scud)
    target_link_libraries(scudrace PRIVATE model3recomp)
    if(NOT MSVC)
        # Lifted code is one statement per guest instruction; most of what
        # -Wall says about it is the guest's business.
        target_compile_options(scudrace PRIVATE -O2 -w)
    endif()
    if(M3_DREAMCAST)
        execute_process(COMMAND git rev-parse --short HEAD
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE SCUD_GIT_REV OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        target_compile_definitions(scudrace PRIVATE M3_GIT_REV="${SCUD_GIT_REV}")
        target_link_options(scudrace PRIVATE -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/scud_dc.map)
        # A compiler-made fneg in lifted code has lost a sign twice (DC-BUG-017).
        find_program(SH_ELF_OBJDUMP sh-elf-objdump HINTS $ENV{KOS_CC_BASE}/bin)
        if(SH_ELF_OBJDUMP)
            add_custom_command(TARGET scudrace POST_BUILD
                COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/dreamcast/check-fneg.sh
                        $<TARGET_FILE:scudrace> ${SH_ELF_OBJDUMP}
                VERBATIM)
        endif()
        option(SCUD_DC_PROFILE "Sampling profiler in the Dreamcast Scud build" OFF)
        if(SCUD_DC_PROFILE)
            target_compile_definitions(scudrace PRIVATE SCUD_DC_PROFILE=1)
        endif()
    endif()
endif()

# --- Dreamcast examples ---
if(M3_DREAMCAST)
    execute_process(COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE M3_GIT_REV OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    add_executable(dreamcast_boot examples/dreamcast_boot/main.c)
    target_link_libraries(dreamcast_boot PRIVATE model3recomp)
    target_compile_definitions(dreamcast_boot PRIVATE M3_GIT_REV="${M3_GIT_REV}")
    # The PVR on its own: flat, Gouraud and textured triangles.
    add_executable(pvr_triangle tests/dreamcast/pvr_triangle/main.c)
endif()
