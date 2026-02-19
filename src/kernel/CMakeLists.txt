cmake_minimum_required(VERSION 3.20)
project(xbox_kernel C)

# Xbox kernel replacement layer - static library
# Targets MSVC (Visual Studio 2022) or MinGW-w64 on Windows 11

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Source files
set(KERNEL_SOURCES
    kernel_path.c
    kernel_pool.c
    kernel_rtl.c
    kernel_memory.c
    kernel_file.c
    kernel_thread.c
    kernel_sync.c
    kernel_hal.c
    kernel_xbox.c
    kernel_ob.c
    kernel_io.c
    kernel_crypto.c
    kernel_thunks.c
    kernel_bridge.c
    xbox_memory_layout.c
)

# Create static library
add_library(xbox_kernel STATIC ${KERNEL_SOURCES})

# Include directory (kernel.h)
target_include_directories(xbox_kernel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Windows system libraries
target_link_libraries(xbox_kernel PUBLIC
    kernel32
    bcrypt
)

# Compiler-specific options
if(MSVC)
    # MSVC: Warnings, fast code, stdcall support
    target_compile_options(xbox_kernel PRIVATE
        /W4             # High warning level
        /WX-            # Don't treat warnings as errors (yet)
        /O2             # Optimize for speed
        /GS-            # Disable buffer security checks (perf)
        /Gy             # Function-level linking
    )
    target_compile_definitions(xbox_kernel PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
elseif(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    # MinGW / Clang: Equivalent options
    target_compile_options(xbox_kernel PRIVATE
        -Wall
        -Wextra
        -Wno-unused-parameter
        -O2
    )
    target_compile_definitions(xbox_kernel PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()

# Debug build adds _DEBUG define for XBOX_TRACE macros
target_compile_definitions(xbox_kernel PRIVATE
    $<$<CONFIG:Debug>:_DEBUG>
)
