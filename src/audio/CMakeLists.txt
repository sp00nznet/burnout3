# Xbox DirectSound → XAudio2 Compatibility Layer

add_library(dsound_compat STATIC
    dsound_device.c
)

target_include_directories(dsound_compat PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# platform carries the src/ include path (and on Linux the POSIX Win32
# primitives), so both hosts need it for "platform/xbox_winnt.h".
target_link_libraries(dsound_compat platform)

if(MSVC)
    target_compile_options(dsound_compat PRIVATE /W4 /O2)
else()
    target_compile_options(dsound_compat PRIVATE -Wall -Wextra -O2)
endif()
