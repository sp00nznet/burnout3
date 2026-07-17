# Xbox D3D8 Compatibility Layer
#   _WIN32 -> backed by Direct3D 11 (d3d8_*.c, d3d8_combiners.c, d3d8_vsh.c)
#   POSIX  -> backed by OpenGL 3.3 core via SDL2 + libepoxy (d3d8_gl.c)

if(WIN32)
    add_library(d3d8_compat STATIC
        d3d8_device.c
        d3d8_resources.c
        d3d8_shaders.c
        d3d8_states.c
        d3d8_combiners.c
        d3d8_vsh.c
    )
    # platform is an INTERFACE target on Windows: it supplies the src/
    # include path that "platform/xbox_winnt.h" resolves against.
    target_link_libraries(d3d8_compat
        platform
        d3d11
        dxgi
        dxguid
        d3dcompiler
    )
else()
    add_library(d3d8_compat STATIC
        d3d8_gl.c
    )
    find_package(SDL2 REQUIRED)
    # libepoxy: GL function loader (auto-resolves OpenGL/GLX/EGL symbols)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EPOXY REQUIRED epoxy)
    target_include_directories(d3d8_compat PRIVATE ${EPOXY_INCLUDE_DIRS})
    target_link_libraries(d3d8_compat PUBLIC platform SDL2::SDL2 ${EPOXY_LIBRARIES})
endif()

target_include_directories(d3d8_compat PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(MSVC)
    target_compile_options(d3d8_compat PRIVATE /W4 /O2)
else()
    target_compile_options(d3d8_compat PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()
