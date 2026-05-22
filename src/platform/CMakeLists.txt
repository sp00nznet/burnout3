cmake_minimum_required(VERSION 3.20)
project(platform C)

# Platform layer
#   xbox_winnt.h   - Win32 / NT type vocabulary (header only)
#   win32_compat.* - POSIX implementation of generic Win32 host primitives
#
# On Windows the real <windows.h> is used, so this is an INTERFACE library
# that only contributes the src/ include path. On Linux it is a real static
# library carrying the POSIX primitive implementations.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(WIN32)
    add_library(platform INTERFACE)
    target_include_directories(platform INTERFACE ${CMAKE_SOURCE_DIR}/src)
else()
    add_library(platform STATIC win32_compat.c)
    target_include_directories(platform PUBLIC
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    find_package(Threads REQUIRED)
    target_link_libraries(platform PUBLIC Threads::Threads)
    target_compile_options(platform PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()
