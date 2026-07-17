/**
 * Burnout 3: Takedown - ImGui Menu System
 *
 * xemu-inspired settings overlay + debug menu.
 * Uses Dear ImGui with D3D11/Win32 backends.
 */

#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <math.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

extern "C" {
#include "menu_gui.h"
#include "d3d/d3d8_internal.h"
extern ptrdiff_t g_xbox_mem_offset;
}

/* Forward declare the Win32 message handler from imgui_impl_win32.cpp */
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

/* ================================================================
 * Menu state
 * ================================================================ */

static struct {
    bool initialized;
    bool show_settings;
    bool show_debug;
    bool show_about;
    int  settings_tab;  /* 0=General, 1=Display, 2=Input, 3=Audio */

    /* Settings values */
    int   window_size_idx;    /* 0=640x480, 1=1280x960, 2=1920x1080, 3=custom */
    bool  vsync;
    bool  fullscreen;
    float master_volume;
    bool  show_fps;
    bool  show_debug_overlay;
    int   render_mode;        /* 0=pseudo-3D, 1=true 3D */
    int   current_track;
    bool  show_wireframe;
    float camera_fov;
    float camera_distance;
    bool  skip_intro;

    /* Screenshot / toast */
    bool  screenshot_requested;
    char  toast_msg[256];
    DWORD toast_start;
    int   shot_counter;
} g_menu = {
    false, false, false, false, 0,
    /* defaults */
    1,     /* 1280x960 */
    true,  /* vsync */
    false, /* fullscreen */
    1.0f,  /* master volume */
    true,  /* show fps */
    false, /* debug overlay */
    1,     /* true 3D */
    0,     /* track 0 */
    false, /* wireframe */
    60.0f, /* fov */
    12.0f, /* camera distance */
    false, /* skip_intro -- default to the real boot sequence (Criterion logo,
            * EA logo, title video, press start). It defaulted to true, which
            * dropped straight to gameplay and made it look like the intro was
            * unimplemented. Still toggleable in the debug menu for fast
            * iteration. */
};

/* ================================================================
 * Color theme (xemu-inspired dark green)
 * ================================================================ */

static void apply_theme(void)
{
    ImGuiStyle &style = ImGui::GetStyle();
    ImVec4 *colors = style.Colors;

    /* Base */
    colors[ImGuiCol_WindowBg]           = ImVec4(0.10f, 0.10f, 0.10f, 0.94f);
    colors[ImGuiCol_PopupBg]            = ImVec4(0.08f, 0.08f, 0.08f, 0.96f);
    colors[ImGuiCol_Border]             = ImVec4(0.30f, 0.30f, 0.30f, 0.50f);

    /* Text */
    colors[ImGuiCol_Text]               = ImVec4(0.95f, 0.95f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled]       = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);

    /* Headers / Tabs (green accent like xemu) */
    colors[ImGuiCol_Header]             = ImVec4(0.20f, 0.55f, 0.20f, 0.80f);
    colors[ImGuiCol_HeaderHovered]      = ImVec4(0.25f, 0.65f, 0.25f, 0.80f);
    colors[ImGuiCol_HeaderActive]       = ImVec4(0.30f, 0.75f, 0.30f, 1.00f);

    /* Buttons */
    colors[ImGuiCol_Button]             = ImVec4(0.20f, 0.55f, 0.20f, 0.65f);
    colors[ImGuiCol_ButtonHovered]      = ImVec4(0.25f, 0.65f, 0.25f, 0.80f);
    colors[ImGuiCol_ButtonActive]       = ImVec4(0.30f, 0.75f, 0.30f, 1.00f);

    /* Frame (inputs, checkboxes) */
    colors[ImGuiCol_FrameBg]            = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]     = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
    colors[ImGuiCol_FrameBgActive]      = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);

    /* Sliders */
    colors[ImGuiCol_SliderGrab]         = ImVec4(0.30f, 0.70f, 0.30f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]   = ImVec4(0.35f, 0.80f, 0.35f, 1.00f);

    /* Checkmarks / toggles */
    colors[ImGuiCol_CheckMark]          = ImVec4(0.30f, 0.80f, 0.30f, 1.00f);

    /* Tabs */
    colors[ImGuiCol_Tab]                = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_TabHovered]         = ImVec4(0.25f, 0.60f, 0.25f, 0.80f);
    colors[ImGuiCol_TabActive]          = ImVec4(0.20f, 0.55f, 0.20f, 1.00f);
    colors[ImGuiCol_TabSelected]        = ImVec4(0.20f, 0.55f, 0.20f, 1.00f);

    /* Separator */
    colors[ImGuiCol_Separator]          = ImVec4(0.30f, 0.30f, 0.30f, 0.50f);

    /* Title bar */
    colors[ImGuiCol_TitleBg]            = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_TitleBgActive]      = ImVec4(0.15f, 0.40f, 0.15f, 1.00f);

    /* Scrollbar */
    colors[ImGuiCol_ScrollbarBg]        = ImVec4(0.10f, 0.10f, 0.10f, 0.50f);
    colors[ImGuiCol_ScrollbarGrab]      = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);

    /* Style tweaks */
    style.WindowRounding    = 6.0f;
    style.FrameRounding     = 4.0f;
    style.GrabRounding      = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.TabRounding       = 4.0f;
    style.WindowPadding     = ImVec2(12, 12);
    style.FramePadding      = ImVec2(8, 4);
    style.ItemSpacing       = ImVec2(8, 6);
    style.WindowBorderSize  = 1.0f;
}

/* ================================================================
 * Settings menu (xemu-style sidebar + content panel)
 * ================================================================ */

static const char *settings_tabs[] = {
    "General", "Display", "Input", "Audio"
};
static const int num_tabs = 4;

static void draw_settings_sidebar(void)
{
    ImGui::BeginChild("Sidebar", ImVec2(160, 0), true);

    for (int i = 0; i < num_tabs; i++) {
        bool selected = (g_menu.settings_tab == i);

        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.65f, 0.25f, 1.0f));
        }

        if (ImGui::Button(settings_tabs[i], ImVec2(-1, 36))) {
            g_menu.settings_tab = i;
        }

        if (selected) {
            ImGui::PopStyleColor(2);
        }
    }

    ImGui::EndChild();
}

static void draw_tab_general(void)
{
    ImGui::Text("Burnout 3: Takedown - Recompiled");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Rendering");
    const char *render_modes[] = { "Pseudo-3D (OutRun style)", "True 3D (Chase camera)" };
    ImGui::Combo("Render Mode", &g_menu.render_mode, render_modes, 2);

    ImGui::Spacing();
    ImGui::Text("Track");
    ImGui::SliderInt("Current Track", &g_menu.current_track, 0, 36, "Track %d");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Checkbox("Show FPS Counter", &g_menu.show_fps);
    ImGui::Checkbox("Show Debug Overlay", &g_menu.show_debug_overlay);
}

static void draw_tab_display(void)
{
    ImGui::Text("Window");
    ImGui::Separator();
    ImGui::Spacing();

    const char *sizes[] = { "640x480", "1280x960", "1920x1080" };
    ImGui::Combo("Window Size", &g_menu.window_size_idx, sizes, 3);

    ImGui::Checkbox("Fullscreen", &g_menu.fullscreen);
    ImGui::Checkbox("V-Sync", &g_menu.vsync);

    ImGui::Spacing();
    ImGui::Text("Camera");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::SliderFloat("Field of View", &g_menu.camera_fov, 30.0f, 120.0f, "%.0f");
    ImGui::SliderFloat("Camera Distance", &g_menu.camera_distance, 5.0f, 30.0f, "%.1f");
    ImGui::Checkbox("Wireframe", &g_menu.show_wireframe);
}

static void draw_tab_input(void)
{
    ImGui::Text("Controllers");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Keyboard Controls:");
    ImGui::BulletText("WASD - Drive / Fly camera");
    ImGui::BulletText("Shift - Boost");
    ImGui::BulletText("V - Toggle 3D mode");
    ImGui::BulletText("T - Cycle tracks");
    ImGui::BulletText("M - Model viewer");
    ImGui::BulletText("N/P - Next/Prev vehicle");
    ImGui::BulletText("F - Toggle fly mode");
    ImGui::BulletText("F1 - Settings menu");
    ImGui::BulletText("F2 - Debug menu");
    ImGui::BulletText("ESC - Close menu / Quit");

    ImGui::Spacing();
    ImGui::Text("Gamepad Controls:");
    ImGui::BulletText("Left stick - Steer");
    ImGui::BulletText("RT/LT - Gas/Brake");
    ImGui::BulletText("A or RB - Boost");
}

static void draw_tab_audio(void)
{
    ImGui::Text("Volume");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::SliderFloat("Master Volume", &g_menu.master_volume, 0.0f, 1.0f, "%.0f%%");

    ImGui::Spacing();
    ImGui::Text("Audio is not yet implemented.");
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
        "DirectSound -> XAudio2 stubs exist but\nplayback is not connected.");
}

static void draw_tab_about(void)
{
    ImGui::Text("Burnout 3: Takedown");
    ImGui::Text("Static Recompilation for Windows 11");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Build Information");
    ImGui::BulletText("Compiler: MSVC 19.x (Visual Studio 2022)");
    ImGui::BulletText("Functions: 22,097 recompiled");
    ImGui::BulletText("Code: 4.43M lines of generated C");
    ImGui::BulletText("Kernel: 147 Xbox imports replaced");

    ImGui::Spacing();
    ImGui::Text("Original Game");
    ImGui::BulletText("Developer: Criterion Games");
    ImGui::BulletText("Publisher: Electronic Arts");
    ImGui::BulletText("Engine: RenderWare ~3.7");
    ImGui::BulletText("XDK: 5849 (2004-07-29)");

    ImGui::Spacing();
    ImGui::Text("Community");
    ImGui::BulletText("github.com/sp00nznet/burnout3");
    ImGui::BulletText("github.com/sp00nznet/xboxrecomp");
}

static void draw_settings_menu(void)
{
    UINT w = d3d8_GetBackbufferWidth();
    UINT h = d3d8_GetBackbufferHeight();

    float menu_w = 600.0f;
    float menu_h = 440.0f;
    ImGui::SetNextWindowPos(ImVec2((w - menu_w) * 0.5f, (h - menu_h) * 0.5f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(menu_w, menu_h), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoCollapse;

    if (ImGui::Begin("Settings", &g_menu.show_settings, flags)) {
        draw_settings_sidebar();
        ImGui::SameLine();

        ImGui::BeginChild("Content", ImVec2(0, 0), true);
        switch (g_menu.settings_tab) {
        case 0: draw_tab_general(); break;
        case 1: draw_tab_display(); break;
        case 2: draw_tab_input();   break;
        case 3: draw_tab_audio();   break;
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

/* ================================================================
 * About window (standalone)
 * ================================================================ */

static void draw_about_window(void)
{
    UINT w = d3d8_GetBackbufferWidth();
    UINT h = d3d8_GetBackbufferHeight();

    float aw = 420.0f, ah = 340.0f;
    ImGui::SetNextWindowPos(ImVec2((w - aw) * 0.5f, (h - ah) * 0.5f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(aw, ah), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoCollapse;

    if (ImGui::Begin("About", &g_menu.show_about, flags)) {
        draw_tab_about();
    }
    ImGui::End();
}

/* ================================================================
 * Debug menu
 * ================================================================ */

static void draw_debug_menu(void)
{
    UINT w = d3d8_GetBackbufferWidth();

    ImGui::SetNextWindowPos(ImVec2((float)w - 360.0f, 10.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(350.0f, 500.0f), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Debug", &g_menu.show_debug)) {
        if (ImGui::CollapsingHeader("Boot", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Skip intro videos", &g_menu.skip_intro);
        }

        if (ImGui::CollapsingHeader("Game State", ImGuiTreeNodeFlags_DefaultOpen)) {
            /* Read from Xbox memory */
            /* g_xbox_mem_offset declared at file scope */
            uintptr_t base = (uintptr_t)g_xbox_mem_offset;
            uint32_t game_state = *(uint32_t *)(base + 0x4D53B8);
            uint32_t load_state = *(uint32_t *)(base + 0x4D53BC);
            float speed = *(float *)(base + 0x5FFF1C);
            float heading = *(float *)(base + 0x5FFF18);
            float pos_x = *(float *)(base + 0x5FFF10);
            float pos_y = *(float *)(base + 0x5FFF14);
            uint32_t dist = *(uint32_t *)(base + 0x5FFD14);

            ImGui::Text("Game State: %u", game_state);
            ImGui::Text("Load State: %u", load_state);
            ImGui::Separator();
            ImGui::Text("Speed: %.1f", speed);
            ImGui::Text("Heading: %.1f deg", heading * 57.2958f);
            ImGui::Text("Position: (%.1f, %.1f)", pos_x, pos_y);
            ImGui::Text("Distance: %u m", dist);
        }

        if (ImGui::CollapsingHeader("Rendering")) {
            ImGui::Text("Backbuffer: %ux%u", d3d8_GetBackbufferWidth(), d3d8_GetBackbufferHeight());
            ImGui::Checkbox("Wireframe Mode", &g_menu.show_wireframe);
            ImGui::SliderFloat("FOV", &g_menu.camera_fov, 30.0f, 120.0f);
        }

        if (ImGui::CollapsingHeader("Memory")) {
            /* g_xbox_mem_offset declared at file scope */
            uintptr_t base = (uintptr_t)g_xbox_mem_offset;
            ImGui::Text("Xbox mem offset: 0x%llX", (unsigned long long)base);
            ImGui::Text("Xbox ESP: 0x%08X", *(uint32_t *)(base + 0x4D652C));

            if (ImGui::TreeNode("Memory Inspector")) {
                static int inspect_addr = 0x5FFF00;
                ImGui::InputInt("Address (hex)", &inspect_addr, 16, 256, ImGuiInputTextFlags_CharsHexadecimal);
                if (inspect_addr >= 0 && inspect_addr < 0x4000000) {
                    uint8_t *ptr = (uint8_t *)(base + inspect_addr);
                    ImGui::Text("%08X: %02X %02X %02X %02X  %02X %02X %02X %02X",
                        inspect_addr, ptr[0], ptr[1], ptr[2], ptr[3],
                        ptr[4], ptr[5], ptr[6], ptr[7]);
                    ImGui::Text("%08X: %02X %02X %02X %02X  %02X %02X %02X %02X",
                        inspect_addr + 8, ptr[8], ptr[9], ptr[10], ptr[11],
                        ptr[12], ptr[13], ptr[14], ptr[15]);
                    ImGui::Text("As float: %.6f", *(float *)ptr);
                    ImGui::Text("As uint32: %u (0x%08X)", *(uint32_t *)ptr, *(uint32_t *)ptr);
                }
                ImGui::TreePop();
            }
        }

        if (ImGui::CollapsingHeader("Performance")) {
            ImGui::Text("%.1f FPS (%.3f ms/frame)",
                ImGui::GetIO().Framerate,
                1000.0f / ImGui::GetIO().Framerate);

            /* Simple FPS graph */
            static float fps_history[120] = {};
            static int fps_idx = 0;
            fps_history[fps_idx] = ImGui::GetIO().Framerate;
            fps_idx = (fps_idx + 1) % 120;
            ImGui::PlotLines("FPS", fps_history, 120, fps_idx, NULL, 0.0f, 60.0f, ImVec2(0, 60));
        }
    }
    ImGui::End();
}

/* ================================================================
 * Public API
 * ================================================================ */

extern "C" int menu_gui_init(void)
{
    if (g_menu.initialized) return 0;

    ID3D11Device *device = d3d8_GetD3D11Device();
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    HWND hwnd = d3d8_GetHWND();

    if (!device || !ctx || !hwnd) {
        fprintf(stderr, "  [MENU] Cannot init ImGui: D3D11 not ready\n");
        return -1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = NULL; /* Don't save layout to disk */

    apply_theme();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(device, ctx);

    g_menu.initialized = true;
    fprintf(stderr, "  [MENU] ImGui initialized\n");
    return 0;
}

extern "C" void menu_gui_shutdown(void)
{
    if (!g_menu.initialized) return;

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_menu.initialized = false;
}

extern "C" int menu_gui_wndproc(void *hwnd, unsigned int msg, unsigned long long wparam, long long lparam)
{
    if (!g_menu.initialized) return 0;
    if (!g_menu.show_settings && !g_menu.show_debug && !g_menu.show_about) return 0;

    LRESULT result = ImGui_ImplWin32_WndProcHandler((HWND)hwnd, msg, (WPARAM)wparam, (LPARAM)lparam);
    return result ? 1 : 0;
}

extern "C" void menu_gui_begin_frame(void)
{
    if (!g_menu.initialized) return;
    bool need_frame = g_menu.show_settings || g_menu.show_debug ||
                      g_menu.show_about || g_menu.toast_msg[0] != '\0';
    if (!need_frame) return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

extern "C" void menu_gui_render(void)
{
    if (!g_menu.initialized) return;
    bool need_frame = g_menu.show_settings || g_menu.show_debug ||
                      g_menu.show_about || g_menu.toast_msg[0] != '\0';
    if (!need_frame) return;

    /* Draw active menus */
    if (g_menu.show_settings) draw_settings_menu();
    if (g_menu.show_debug)    draw_debug_menu();
    if (g_menu.show_about)    draw_about_window();

    /* Draw toast notification (always, even when no menu is open) */
    menu_gui_draw_toast();

    /* Render */
    ImGui::Render();

    /* Set the render target back to the default (ImGui needs this) */
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    ID3D11RenderTargetView *rtv = d3d8_GetDefaultRTV();
    ctx->OMSetRenderTargets(1, &rtv, NULL);

    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

extern "C" void menu_gui_toggle_settings(void)
{
    g_menu.show_settings = !g_menu.show_settings;
    if (g_menu.show_settings) {
        fprintf(stderr, "  [MENU] Settings opened\n");
    }
}

extern "C" void menu_gui_toggle_debug(void)
{
    g_menu.show_debug = !g_menu.show_debug;
    if (g_menu.show_debug) {
        fprintf(stderr, "  [MENU] Debug opened\n");
    }
}

extern "C" int menu_gui_is_active(void)
{
    return (g_menu.show_settings || g_menu.show_debug || g_menu.show_about) ? 1 : 0;
}

extern "C" void menu_gui_show_about(void)
{
    g_menu.show_about = !g_menu.show_about;
}

extern "C" int menu_gui_skip_intro(void)
{
    return g_menu.skip_intro ? 1 : 0;
}

/* ================================================================
 * Screenshot capture (D3D11 backbuffer → PNG via stb_image_write)
 * ================================================================ */

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../third_party/stb_image_write.h"

extern "C" void menu_gui_take_screenshot(void)
{
    ID3D11Device *device = d3d8_GetD3D11Device();
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    IDXGISwapChain *sc = d3d8_GetSwapChain();
    if (!device || !ctx || !sc) return;

    /* Get the back buffer */
    ID3D11Texture2D *backbuf = NULL;
    HRESULT hr = sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuf);
    if (FAILED(hr) || !backbuf) return;

    D3D11_TEXTURE2D_DESC desc;
    backbuf->GetDesc(&desc);

    /* Create a staging texture for CPU readback */
    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;

    ID3D11Texture2D *staging = NULL;
    hr = device->CreateTexture2D(&staging_desc, NULL, &staging);
    if (FAILED(hr) || !staging) { backbuf->Release(); return; }

    ctx->CopyResource((ID3D11Resource*)staging, (ID3D11Resource*)backbuf);
    backbuf->Release();

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ctx->Map((ID3D11Resource*)staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) { staging->Release(); return; }

    /* Build filename: screenshot_NNNN.png */
    char filename[256];
    snprintf(filename, sizeof(filename), "screenshot_%04d.png", g_menu.shot_counter++);

    /* Convert BGRA → RGBA for stb_image_write */
    int w = (int)desc.Width, h = (int)desc.Height;
    unsigned char *pixels = (unsigned char*)malloc(w * h * 4);
    if (pixels) {
        for (int y = 0; y < h; y++) {
            unsigned char *src = (unsigned char*)mapped.pData + y * mapped.RowPitch;
            unsigned char *dst = pixels + y * w * 4;
            for (int x = 0; x < w; x++) {
                dst[x*4+0] = src[x*4+2]; /* R ← B */
                dst[x*4+1] = src[x*4+1]; /* G */
                dst[x*4+2] = src[x*4+0]; /* B ← R */
                dst[x*4+3] = 255;         /* A */
            }
        }
        if (stbi_write_png(filename, w, h, 4, pixels, w * 4)) {
            fprintf(stderr, "Screenshot saved: %s\n", filename);
            /* Show toast */
            strncpy(g_menu.toast_msg, filename, sizeof(g_menu.toast_msg) - 1);
            g_menu.toast_msg[sizeof(g_menu.toast_msg) - 1] = '\0';
            g_menu.toast_start = GetTickCount();
        } else {
            fprintf(stderr, "Screenshot FAILED: %s\n", filename);
        }
        free(pixels);
    }

    ctx->Unmap((ID3D11Resource*)staging, 0);
    staging->Release();
}

/* ================================================================
 * Toast notification (bottom-right, 2.5s with fade)
 * ================================================================ */

extern "C" void menu_gui_draw_toast(void)
{
    if (!g_menu.initialized) return;
    if (g_menu.toast_msg[0] == '\0') return;

    DWORD now = GetTickCount();
    DWORD elapsed = now - g_menu.toast_start;

    if (elapsed > 2500) {
        g_menu.toast_msg[0] = '\0';
        return;
    }

    float alpha = 1.0f;
    if (elapsed > 2000)
        alpha = 1.0f - (float)(elapsed - 2000) / 500.0f;

    ImGuiIO &io = ImGui::GetIO();
    ImVec2 pos(io.DisplaySize.x - 20, io.DisplaySize.y - 40);
    ImGui::SetNextWindowPos(pos, 0, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.7f * alpha);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_NoInputs |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoMove;

    if (ImGui::Begin("##toast", NULL, flags)) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, alpha));
        ImGui::TextUnformatted(g_menu.toast_msg);
        ImGui::PopStyleColor();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}
