cmake_minimum_required(VERSION 3.20)
project(burnout3_game C)

# Burnout 3 - recompiled game executable. main.c brings up the runtime and
# jumps to the XBE entry point; recomp_manual.c is where a function the lifter
# got wrong is replaced. Everything else is generated (gen/, gitignored).

file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/recomp/gen/recomp_*.c"
)

# A GUI-subsystem program on Windows, as the template and BLiNX 2 are: a
# console-subsystem exe started with no console attached (the bundle's
# launcher, Explorer, or umu-run under Proton) gets a console window of its
# own beside the game's. Under WIN32 a run from a terminal returns at once
# and printf reaches nothing unless RECOMP_STDIO_LOG names a file;
# -DBO3_CONSOLE=ON builds the console program again for terminal debugging
# (docs/running.md, TESTING.md's run lines).
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
# look for WinMain under WIN32, so name the CRT's main startup; llvm-mingw's
# CRT starts a main program under -mwindows by itself.
if(MSVC AND NOT BO3_CONSOLE)
    target_link_options(burnout3 PRIVATE /ENTRY:mainCRTStartup)
endif()

target_link_libraries(burnout3 PRIVATE
    xbox_kernel xbox_d3d8 xbox_dsound xbox_input xbox_usb xbox_video xbox_apu
)

# Windows SDK import libraries (dbghelp names the generated function a fault
# happened in). Other hosts get SDL2, the Metal frameworks and OpenSSL through
# the toolkit targets, and the fault handler uses dladdr, so the executable
# must stay unstripped there.
if(WIN32)
    target_link_libraries(burnout3 PRIVATE kernel32 user32 gdi32 dbghelp)
endif()

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

if(TARGET recomp_env)
    target_compile_definitions(burnout3 PRIVATE BO3_TOOLKIT_ENV_TABLE)
    # main.c reads the folder keys (RECOMP_GAME_FILES, RECOMP_HDD_DIR),
    # RECOMP_STDIO_LOG and RECOMP_PB_BACKEND through the table.
    target_link_libraries(burnout3 PRIVATE recomp_env)
endif()

# The enhancements layer, when the toolkit builds it (XBOXRECOMP_ENHANCE in
# the top-level CMakeLists; the fork's recomp_env then defines
# RECOMP_ENV_HAVE_ENHANCE_KEYS, which main.c tests).
if(TARGET xbox_enhance)
    target_link_libraries(burnout3 PRIVATE xbox_enhance)
endif()

if(MSVC)
    target_compile_options(burnout3 PRIVATE /W4 /WX- /O2 /bigobj /MP${BO3_COMPILE_JOBS})
    set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES COMPILE_FLAGS "/w")
    target_compile_definitions(burnout3 PRIVATE _CRT_SECURE_NO_WARNINGS)
    # Fixed image base so Xbox memory can be mapped at fixed addresses; no ICF
    # so every generated function keeps its own symbol for fault reports.
    target_link_options(burnout3 PRIVATE
        /DYNAMICBASE:NO /FIXED:NO /MAP /DEBUG /STACK:16777216 /OPT:NOICF /OPT:REF)
endif()

# MinGW (llvm-mingw, the Proton build ./burnout3 makes): the 16 MB stack
# MSVC gets from /STACK above, and a PDB of public symbols beside the exe
# (lld writes one even without -g) so dbghelp can name the generated function
# a fault happened in.
if(MINGW)
    target_link_options(burnout3 PRIVATE
        "-Wl,--stack,16777216"
        "-Wl,--pdb=$<TARGET_FILE_DIR:burnout3>/burnout3.pdb")
endif()

# The lifter writes compares as `if ((a == b))`, which clang flags as
# -Wparentheses-equality once per site: tens of thousands of warnings a full
# build, burying the real ones. Generated code only.
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    set_property(SOURCE ${RECOMP_GEN_SOURCES} APPEND PROPERTY
        COMPILE_OPTIONS -Wno-parentheses-equality)
endif()

# bin/ is upstream's layout for its Visual Studio build (README, Setup.cmd).
# Any other generator puts the exe at the top of its own build dir, where
# ./burnout3 looks for it: its developer, packaging and bench builds each
# have one, and a shared bin/ would let one overwrite another's exe.
if(CMAKE_GENERATOR MATCHES "Visual Studio")
    set_target_properties(burnout3 PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_SOURCE_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_SOURCE_DIR}/bin"
    )
else()
    set_target_properties(burnout3 PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()

# The option defaults the layer reads beside the executable. Copied at build
# time, as a file that depends on the template, so a developer build has
# the defaults layer and an edit of the template refreshes it; a bundle
# stages the template itself (./burnout3 package).
if(TARGET xbox_enhance)
    if(CMAKE_GENERATOR MATCHES "Visual Studio")
        set(_bo3_exe_dir "${CMAKE_SOURCE_DIR}/bin")
    else()
        set(_bo3_exe_dir "${CMAKE_BINARY_DIR}")
    endif()
    add_custom_command(
        OUTPUT ${_bo3_exe_dir}/config.default.toml
        COMMAND ${CMAKE_COMMAND} -E copy
                ${CMAKE_SOURCE_DIR}/packaging/config.default.toml
                ${_bo3_exe_dir}/config.default.toml
        DEPENDS ${CMAKE_SOURCE_DIR}/packaging/config.default.toml
        COMMENT "config.default.toml beside burnout3")
    add_custom_target(burnout3_config_default ALL
        DEPENDS ${_bo3_exe_dir}/config.default.toml)
    add_dependencies(burnout3 burnout3_config_default)
endif()
