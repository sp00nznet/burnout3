# Xbox DirectSound → XAudio2 Compatibility Layer

add_library(dsound_compat STATIC
    dsound_device.c
)

target_include_directories(dsound_compat PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# XAudio2 will be linked when implementation is complete
# target_link_libraries(dsound_compat xaudio2)

if(MSVC)
    target_compile_options(dsound_compat PRIVATE /W4 /O2)
else()
    target_compile_options(dsound_compat PRIVATE -Wall -Wextra -O2)
endif()
