# Xbox Input → XInput Compatibility Layer

add_library(input_compat STATIC
    xinput_device.c
)

target_include_directories(input_compat PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(input_compat
    xinput
)

if(MSVC)
    target_compile_options(input_compat PRIVATE /W4 /O2)
else()
    target_compile_options(input_compat PRIVATE -Wall -Wextra -O2)
endif()
