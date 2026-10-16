cmake_minimum_required(VERSION 3.20)
project(burnout3_game C)

# Burnout 3 - recompiled game executable. main.c brings up the runtime and
# jumps to the XBE entry point; recomp_manual.c is where a function the lifter
# got wrong is replaced. Everything else is generated (gen/, gitignored).

file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/recomp/gen/recomp_*.c"
)

add_executable(burnout3
    main.c
    recomp/recomp_manual.c
    ${RECOMP_GEN_SOURCES}
)

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
# MinGW build puts the exe at the top of its own build dir, so a cross build
# and a native one never overwrite each other's exe.
if(NOT MINGW)
    set_target_properties(burnout3 PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_SOURCE_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_SOURCE_DIR}/bin"
    )
else()
    set_target_properties(burnout3 PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()
