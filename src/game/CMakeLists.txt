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
)

# Windows SDK import libraries (dbghelp names the generated function a fault
# happened in). On macOS and Linux the toolkit targets bring SDL2 and
# OpenSSL, and the fault handler uses dladdr, so the executable must stay
# unstripped there.
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

if(MSVC)
    target_compile_options(burnout3 PRIVATE /W4 /WX- /O2 /bigobj /MP${BO3_COMPILE_JOBS})
    set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES COMPILE_FLAGS "/w")
    target_compile_definitions(burnout3 PRIVATE _CRT_SECURE_NO_WARNINGS)
    # Fixed image base so Xbox memory can be mapped at fixed addresses; no ICF
    # so every generated function keeps its own symbol for fault reports.
    target_link_options(burnout3 PRIVATE
        /DYNAMICBASE:NO /FIXED:NO /MAP /DEBUG /STACK:16777216 /OPT:NOICF /OPT:REF)
endif()

# The toolkit fork's environment table, when the toolkit has one: main.c
# reads RECOMP_STDIO_LOG and RECOMP_PB_BACKEND through it and flushes its
# notes. Upstream's toolkit has no recomp_env target.
if(TARGET recomp_env)
    target_compile_definitions(burnout3 PRIVATE BO3_TOOLKIT_ENV_TABLE)
    target_link_libraries(burnout3 PRIVATE recomp_env)
endif()

# The enhancements layer, when the toolkit builds it (XBOXRECOMP_ENHANCE in
# the top-level CMakeLists; the fork's recomp_env then defines
# RECOMP_ENV_HAVE_ENHANCE_KEYS, which main.c tests).
if(TARGET xbox_enhance)
    target_link_libraries(burnout3 PRIVATE xbox_enhance)
endif()

# MinGW (llvm-mingw, natively or cross from macOS and Linux): the 16 MB
# stack MSVC gets from /STACK above. With Clang (lld), also a PDB of public
# symbols beside the exe (lld writes one even without -g) so dbghelp can
# name the generated function a fault happened in; GNU ld has no --pdb.
if(MINGW)
    target_link_options(burnout3 PRIVATE "-Wl,--stack,16777216")
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_link_options(burnout3 PRIVATE
            "-Wl,--pdb=$<TARGET_FILE_DIR:burnout3>/burnout3.pdb")
    endif()
endif()

# MinGW's libxinput.a imports XINPUT1_3.dll, which Windows 10 and 11 do not
# ship (it came with the DirectX redistributable), so the exe would not
# start on a stock install. MSVC's xinput.lib imports XINPUT1_4.dll, which
# they do. xbox_input names plain "xinput"; link xinput1_4 in its place. A
# toolkit that already picks xinput1_4 under MinGW leaves nothing to replace.
if(MINGW AND TARGET xbox_input)
    get_target_property(_bo3_input_libs xbox_input INTERFACE_LINK_LIBRARIES)
    if(_bo3_input_libs)
        list(TRANSFORM _bo3_input_libs REPLACE "^xinput$" "xinput1_4")
        set_target_properties(xbox_input PROPERTIES
            INTERFACE_LINK_LIBRARIES "${_bo3_input_libs}")
        if(_bo3_input_libs MATCHES "(^|;)xinput(1_3)?(;|$)")
            message(WARNING "xbox_input still links MinGW's xinput: the exe "
                            "would import XINPUT1_3.dll")
        endif()
    endif()
endif()

# The lifter writes compares as `if ((a == b))`, which Clang flags as
# -Wparentheses-equality once per site: tens of thousands of warnings a full
# build, burying the real ones. Generated code only.
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    set_property(SOURCE ${RECOMP_GEN_SOURCES} APPEND PROPERTY
        COMPILE_OPTIONS -Wno-parentheses-equality)
endif()

# bin/ is the MSVC build's layout (README, Setup.cmd), whatever the
# generator (Visual Studio, or Ninja from VS Open Folder and VS Code). A
# MinGW build, and a macOS or Linux one, puts the exe at the top of its own
# build dir, so a cross build and a native one never overwrite each other's
# exe.
if(WIN32 AND NOT MINGW)
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
# stages the template itself (./burnout3 package). Same directory as the
# exe rule above.
if(TARGET xbox_enhance)
    if(WIN32 AND NOT MINGW)
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
