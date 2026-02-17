cmake_minimum_required(VERSION 3.20)
project(burnout3_game C)

# Burnout 3: Takedown - Recompiled game executable

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Game executable sources
set(GAME_SOURCES
    main.c
)

# Create Windows GUI application
add_executable(burnout3 WIN32 ${GAME_SOURCES})

# Link compatibility layers
target_link_libraries(burnout3 PRIVATE
    xbox_kernel
    d3d8_compat
    dsound_compat
    input_compat
)

# System libraries
target_link_libraries(burnout3 PRIVATE
    kernel32
    user32
    gdi32
)

# Include paths
target_include_directories(burnout3 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

# Compiler options
if(MSVC)
    target_compile_options(burnout3 PRIVATE
        /W4
        /WX-
        /O2
    )
    target_compile_definitions(burnout3 PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
    # Disable ASLR so we can map Xbox memory at fixed addresses
    target_link_options(burnout3 PRIVATE
        /DYNAMICBASE:NO
        /FIXED:NO
    )
elseif(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(burnout3 PRIVATE
        -Wall -Wextra -Wno-unused-parameter -O2
    )
endif()

# Debug info
target_compile_definitions(burnout3 PRIVATE
    $<$<CONFIG:Debug>:_DEBUG>
)

# Output to project root for easy testing
set_target_properties(burnout3 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin"
)
