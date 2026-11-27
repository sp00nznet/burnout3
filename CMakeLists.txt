cmake_minimum_required(VERSION 3.20)
project(burnout3_recomp C)

# Burnout 3: Takedown (Xbox, 2004) - Static Recompilation
#
# Built on the shared xboxrecomp runtime, the same way hl2 is. The in-tree
# src/{kernel,d3d,audio,input,apu,nv2a,platform} fork is no longer built: it
# carried hand-written menus and render paths, and the goal now is the title's
# own boot sequence on the toolkit every other Xbox recomp uses.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Half the cores for /MP: each cl.exe holds a whole generated chunk at /O2,
# and saturating cores and RAM is what makes the desktop unusable.
cmake_host_system_information(RESULT _bo3_cores QUERY NUMBER_OF_LOGICAL_CORES)
math(EXPR _bo3_default_jobs "${_bo3_cores} / 2")
if(_bo3_default_jobs LESS 1)
    set(_bo3_default_jobs 1)
endif()
set(BO3_COMPILE_JOBS "${_bo3_default_jobs}" CACHE STRING
    "Parallel cl.exe processes for the recompiled sources (/MP<n>)")

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)

option(BO3_ICALL_FEEDBACK "Record indirect-branch targets for codegen feedback" OFF)
if(BO3_ICALL_FEEDBACK)
    add_compile_definitions(RECOMP_ICALL_FEEDBACK)
endif()

option(BO3_ABI_CHECK "Report functions that clobber callee-saved registers" OFF)
if(BO3_ABI_CHECK)
    add_compile_definitions(RECOMP_ABI_CHECK)
endif()

set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp" CACHE PATH "Path to xboxrecomp toolkit")

# Off Windows, xbox_kernel links OpenSSL::Crypto PUBLIC but finds it inside
# its own subdirectory, where the imported target stays invisible to this
# executable; a clean configure then fails with "target was not found".
# Finding it here first puts it in scope for both.
if(NOT WIN32)
    find_package(OpenSSL REQUIRED)
endif()

# The toolkit fork's enhancements layer (render scale, present filter,
# fullscreen; config/config.toml over config.default.toml beside the
# executable). ON for this title, every key stock by default; not FORCE, so
# -DXBOXRECOMP_ENHANCE=OFF builds a layer-free binary for A/B. Upstream's
# toolkit has no such option and ignores it.
set(XBOXRECOMP_ENHANCE ON CACHE BOOL
    "Build the enhancements layer (config.toml, RECOMP_RENDER_SCALE, ...)")

add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# A toolkit with an environment table (recomp_env, the fork's) takes this
# game's defaults for its keys from src/env/recomp_env_game.h: vblank on,
# one guest core, two pads. Upstream's toolkit reads getenv directly and has
# no recomp_env target; there main.c's own defaults do the job.
if(TARGET recomp_env)
    target_compile_definitions(recomp_env PUBLIC RECOMP_ENV_HAVE_GAME_KEYS)
    target_include_directories(recomp_env PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/env)
endif()

add_subdirectory(src/game)

# Line-level symbols for the runtime libraries, so a hang sample inside one
# names the file and line rather than the nearest public function.
if(MSVC)
    foreach(_lib xbox_apu xbox_usb xbox_kernel)
        if(TARGET ${_lib})
            target_compile_options(${_lib} PRIVATE /Z7)
        endif()
    endforeach()
endif()
