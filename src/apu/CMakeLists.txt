# MCPX APU Emulation Library
# Extracted from xemu (https://github.com/xemu-project/xemu)
#
# Voice Processor (VP): ADPCM decode, sample mixing, envelopes, filters
# Global Processor (GP): STUBBED - effects bypass
# Encode Processor (EP): STUBBED - direct mixbin passthrough

add_library(apu_emu STATIC
    apu_core.c
    apu_vp.c
    apu_dsp.c
    apu_mmio_hook.c
    apu_xaudio2.c
)

target_include_directories(apu_emu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../nv2a  # For qemu_shim.h
)

# MSVC-specific settings
if(MSVC)
    target_compile_options(apu_emu PRIVATE
        /W3
        /wd4244  # conversion warnings (float/double/int)
        /wd4146  # unary minus on unsigned
        /wd4267  # size_t to int conversion
    )
    target_compile_definitions(apu_emu PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _USE_MATH_DEFINES  # For M_PI, M_E
    )
endif()

if(WIN32)
    # kernel32 for threading, winmm for waveOut fallback, ole32 for CoInitializeEx
    target_link_libraries(apu_emu PRIVATE platform kernel32 winmm ole32)
else()
    # Linux: platform layer (Win32 primitives on POSIX) + SDL2 audio
    find_package(SDL2 REQUIRED)
    target_link_libraries(apu_emu PRIVATE platform SDL2::SDL2 m)
endif()
