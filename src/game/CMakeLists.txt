cmake_minimum_required(VERSION 3.20)
project(burnout3_game C)

# Burnout 3 - recompiled game executable. main.c brings up the runtime and
# jumps to the XBE entry point; recomp_manual.c is where a function the lifter
# got wrong is replaced. Everything else is generated (gen/, gitignored).

file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/recomp/gen/recomp_*.c"
)

# A GUI-subsystem program on Windows: a console-subsystem exe started with
# no console attached (Explorer, a shortcut, or Proton) gets a console window
# of its own beside the game's. Started from a terminal, a GUI exe returns
# the prompt at once and its printf reaches nothing unless RECOMP_STDIO_LOG
# names a file or the command redirects it; -DBO3_CONSOLE=ON builds the
# console program again for terminal debugging (docs/running.md).
option(BO3_CONSOLE "Windows: build a console-subsystem exe (terminal debugging)" OFF)
if(BO3_CONSOLE)
    set(_bo3_subsystem "")
else()
    set(_bo3_subsystem WIN32)
endif()

add_executable(burnout3 ${_bo3_subsystem}
    main.c
    recomp/recomp_manual.c
    ${RECOMP_GEN_SOURCES}
)
# main(argc, argv) stays the entry point under both subsystems: MSVC would
# look for WinMain under WIN32, so name the CRT's main startup. MinGW's CRT
# starts a main program under -mwindows by itself.
if(MSVC AND NOT BO3_CONSOLE)
    target_link_options(burnout3 PRIVATE /ENTRY:mainCRTStartup)
endif()

target_link_libraries(burnout3 PRIVATE
    xbox_kernel xbox_d3d8 xbox_dsound xbox_input xbox_usb xbox_video xbox_apu
    kernel32 user32 gdi32 dbghelp
)

target_include_directories(burnout3 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/recomp
    ${CMAKE_CURRENT_SOURCE_DIR}/recomp/gen
    ${XBOXRECOMP_DIR}/src
    ${XBOXRECOMP_DIR}/src/kernel
    # The template runtime headers, not a local copy: BO3's old copy had
    # drifted from the template by hundreds of lines.
    ${XBOXRECOMP_DIR}/templates/runtime
)

if(MSVC)
    target_compile_options(burnout3 PRIVATE /W4 /WX- /O2 /bigobj /MP${BO3_COMPILE_JOBS})
    set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES COMPILE_FLAGS "/w")
    target_compile_definitions(burnout3 PRIVATE _CRT_SECURE_NO_WARNINGS)
    # Fixed image base so Xbox memory can be mapped at fixed addresses; no ICF
    # so every generated function keeps its own symbol for fault reports.
    target_link_options(burnout3 PRIVATE
        /DYNAMICBASE:NO /FIXED:NO /MAP /DEBUG /STACK:16777216 /OPT:NOICF /OPT:REF)
endif()

set_target_properties(burnout3 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_SOURCE_DIR}/bin"
)
