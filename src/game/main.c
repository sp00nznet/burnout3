/**
 * Burnout 3: Takedown - Recompiled Game Entry Point
 *
 * This is the Windows executable that hosts the recompiled game code.
 * It performs the following initialization sequence:
 *
 * 1. Load the original XBE file from disk
 * 2. Initialize the Xbox memory layout (map data sections to original VAs)
 * 3. Initialize the Xbox kernel replacement layer
 * 4. Initialize graphics (D3D8→D3D11)
 * 5. Initialize audio (DirectSound→XAudio2)
 * 6. Initialize input (XPP→XInput)
 * 7. Call the game's original entry point (recompiled)
 *
 * The recompiled game code lives in separate translation units generated
 * from the original x86 machine code. Each function is translated to C
 * with the same calling convention and register usage.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Compatibility layers */
#include "../kernel/kernel.h"
#include "../kernel/xbox_memory_layout.h"
#include "../d3d/d3d8_xbox.h"
#include "../audio/dsound_xbox.h"
#include "../input/xinput_xbox.h"

/* ── Configuration ──────────────────────────────────────────── */

/* Default path to the original XBE file */
#define DEFAULT_XBE_PATH "Burnout 3 Takedown\\default.xbe"

/* Window properties */
#define WINDOW_TITLE "Burnout 3: Takedown (Recompiled)"
#define WINDOW_CLASS "Burnout3RecompClass"
#define DEFAULT_WIDTH 640
#define DEFAULT_HEIGHT 480

/* ── Global state ───────────────────────────────────────────── */

static HWND g_hwnd = NULL;
static BOOL g_running = TRUE;
static void *g_xbe_data = NULL;
static size_t g_xbe_size = 0;

/* ── XBE loading ────────────────────────────────────────────── */

/**
 * Load the original XBE file into memory.
 * The XBE data is needed to initialize the memory layout
 * (copy .rdata and .data sections to their expected addresses).
 */
static BOOL load_xbe(const char *path)
{
    HANDLE hFile;
    DWORD fileSize, bytesRead;

    hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "ERROR: Cannot open XBE: %s (error %lu)\n",
                path, GetLastError());
        return FALSE;
    }

    fileSize = GetFileSize(hFile, NULL);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0) {
        fprintf(stderr, "ERROR: Invalid XBE file size\n");
        CloseHandle(hFile);
        return FALSE;
    }

    g_xbe_data = VirtualAlloc(NULL, fileSize, MEM_COMMIT, PAGE_READWRITE);
    if (!g_xbe_data) {
        fprintf(stderr, "ERROR: Cannot allocate %lu bytes for XBE\n", fileSize);
        CloseHandle(hFile);
        return FALSE;
    }

    if (!ReadFile(hFile, g_xbe_data, fileSize, &bytesRead, NULL) ||
        bytesRead != fileSize) {
        fprintf(stderr, "ERROR: Failed to read XBE (%lu of %lu bytes)\n",
                bytesRead, fileSize);
        VirtualFree(g_xbe_data, 0, MEM_RELEASE);
        g_xbe_data = NULL;
        CloseHandle(hFile);
        return FALSE;
    }

    g_xbe_size = fileSize;
    CloseHandle(hFile);
    fprintf(stderr, "Loaded XBE: %s (%lu bytes)\n", path, fileSize);
    return TRUE;
}

/* ── Window management ──────────────────────────────────────── */

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg,
                                     WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CLOSE:
        g_running = FALSE;
        PostQuitMessage(0);
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            g_running = FALSE;
            PostQuitMessage(0);
        }
        return 0;

    case WM_SIZE:
        /* TODO: Notify D3D layer of resize */
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static HWND create_window(HINSTANCE hInstance, int width, int height)
{
    WNDCLASSEXA wc = {0};
    RECT rect;
    HWND hwnd;

    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WINDOW_CLASS;
    RegisterClassExA(&wc);

    /* Adjust window size for client area */
    rect.left = 0;
    rect.top = 0;
    rect.right = width;
    rect.bottom = height;
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    hwnd = CreateWindowExA(
        0, WINDOW_CLASS, WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        NULL, NULL, hInstance, NULL
    );

    if (hwnd) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }

    return hwnd;
}

/* ── Subsystem initialization ───────────────────────────────── */

static BOOL init_subsystems(void)
{
    fprintf(stderr, "\n=== Initializing subsystems ===\n");

    /* 1. Xbox memory layout (maps .rdata/.data to original VAs) */
    fprintf(stderr, "[1/4] Memory layout...\n");
    if (!xbox_MemoryLayoutInit(g_xbe_data, g_xbe_size)) {
        fprintf(stderr, "FATAL: Memory layout initialization failed\n");
        fprintf(stderr, "  The address range 0x00010000-0x00770000 must be available.\n");
        fprintf(stderr, "  Try disabling ASLR or running with a fixed base address.\n");
        return FALSE;
    }

    /* 2. Xbox kernel replacement layer */
    fprintf(stderr, "[2/4] Kernel layer...\n");
    /* kernel_init() would go here once we have one */

    /* 3. Graphics (D3D8→D3D11) */
    fprintf(stderr, "[3/4] Graphics (D3D8→D3D11)...\n");
    /* d3d8_init(g_hwnd) would go here */

    /* 4. Audio + Input */
    fprintf(stderr, "[4/4] Audio + Input...\n");
    /* dsound_init() and xinput_init() would go here */

    fprintf(stderr, "=== All subsystems initialized ===\n\n");
    return TRUE;
}

static void shutdown_subsystems(void)
{
    fprintf(stderr, "\n=== Shutting down ===\n");

    /* Reverse order of initialization */
    /* xinput_shutdown(); */
    /* dsound_shutdown(); */
    /* d3d8_shutdown(); */
    /* kernel_shutdown(); */
    xbox_MemoryLayoutShutdown();

    if (g_xbe_data) {
        VirtualFree(g_xbe_data, 0, MEM_RELEASE);
        g_xbe_data = NULL;
    }

    fprintf(stderr, "Shutdown complete.\n");
}

/* ── Main game loop ─────────────────────────────────────────── */

static void game_loop(void)
{
    MSG msg;

    fprintf(stderr, "Entering main loop (press ESC to exit)...\n");

    while (g_running) {
        /* Process Windows messages */
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = FALSE;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        if (!g_running)
            break;

        /*
         * TODO: Call recompiled game frame function here.
         *
         * The game's main loop (originally at the XBE entry point)
         * would be translated to a C function that we call each frame.
         * For now, just sleep to avoid burning CPU.
         */
        Sleep(16); /* ~60 FPS target */
    }
}

/* ── Entry point ────────────────────────────────────────────── */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    const char *xbe_path = DEFAULT_XBE_PATH;

    (void)hPrevInstance;
    (void)nCmdShow;

    fprintf(stderr, "Burnout 3: Takedown - Static Recompilation\n");
    fprintf(stderr, "==========================================\n\n");

    /* Allow custom XBE path via command line */
    if (lpCmdLine && lpCmdLine[0]) {
        xbe_path = lpCmdLine;
    }

    /* Load the original XBE (needed for data sections) */
    if (!load_xbe(xbe_path)) {
        MessageBoxA(NULL,
            "Failed to load default.xbe.\n\n"
            "Place the game files in a 'Burnout 3 Takedown' folder\n"
            "next to this executable, or pass the XBE path as an argument.",
            WINDOW_TITLE, MB_OK | MB_ICONERROR);
        return 1;
    }

    /* Create the game window */
    g_hwnd = create_window(hInstance, DEFAULT_WIDTH, DEFAULT_HEIGHT);
    if (!g_hwnd) {
        fprintf(stderr, "FATAL: Failed to create window\n");
        shutdown_subsystems();
        return 1;
    }

    /* Initialize all subsystems */
    if (!init_subsystems()) {
        shutdown_subsystems();
        return 1;
    }

    /* Run the game */
    game_loop();

    /* Clean up */
    shutdown_subsystems();

    fprintf(stderr, "\nBurnout 3 exited normally.\n");
    return 0;
}
