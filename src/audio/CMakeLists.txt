# Xbox DirectSound → XAudio2 Compatibility Layer

add_library(dsound_compat STATIC
    dsound_device.c
)

target_include_directories(dsound_compat PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(NOT WIN32)
    # Linux: needs the platform layer for NT types / win32_compat
    target_link_libraries(dsound_compat platform)
endif()

if(MSVC)
    target_compile_options(dsound_compat PRIVATE /W4 /O2)
else()
    target_compile_options(dsound_compat PRIVATE -Wall -Wextra -O2)
endif()
