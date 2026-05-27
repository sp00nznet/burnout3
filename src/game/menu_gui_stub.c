/* menu_gui_stub.c -- Linux stub for the ImGui overlay.
 *
 * The Windows build uses Dear ImGui with the dx11+win32 backends
 * (see menu_gui.cpp). On Linux we skip the overlay entirely for now;
 * the real port is to imgui_impl_sdl2 + imgui_impl_opengl3 once the
 * game runs. */

#include "menu_gui.h"

int  menu_gui_init(void)         { return 0; }
void menu_gui_shutdown(void)     {}
int  menu_gui_wndproc(void *hwnd, unsigned int msg,
                      unsigned long long wparam, long long lparam)
{ (void)hwnd; (void)msg; (void)wparam; (void)lparam; return 0; }
void menu_gui_begin_frame(void)  {}
void menu_gui_render(void)       {}
void menu_gui_take_screenshot(void) {}
