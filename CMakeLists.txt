cmake_minimum_required(VERSION 3.20)
project(model3recomp C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

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
    list(REMOVE_ITEM M3_SOURCES src/netplay.c)
    list(APPEND M3_SOURCES src/netplay_null.c src/platform/dreamcast/platform_dreamcast.c)
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
    target_compile_definitions(model3recomp PUBLIC M3_DREAMCAST=1)
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
    add_executable(scudrace src/games/scud/main_host.c src/games/scud/scud.c ${SCUD_LIFTED})
    target_include_directories(scudrace PRIVATE ${SCUD_LIFTED_DIR} src/games/scud)
    target_link_libraries(scudrace PRIVATE model3recomp)
    if(NOT MSVC)
        # Lifted code is one statement per guest instruction; most of what
        # -Wall says about it is the guest's business.
        target_compile_options(scudrace PRIVATE -O2 -w)
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
