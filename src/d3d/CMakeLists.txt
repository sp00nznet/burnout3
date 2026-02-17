# D3D8→D3D11 Compatibility Layer
# Implements Xbox D3D8 interface backed by Direct3D 11

add_library(d3d8_compat STATIC
    d3d8_device.c
    d3d8_resources.c
    d3d8_shaders.c
    d3d8_states.c
)

target_include_directories(d3d8_compat PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# D3D11 and DXGI libraries
target_link_libraries(d3d8_compat
    d3d11
    dxgi
    dxguid
    d3dcompiler
)

if(MSVC)
    target_compile_options(d3d8_compat PRIVATE /W4 /O2)
else()
    target_compile_options(d3d8_compat PRIVATE -Wall -Wextra -O2)
endif()
