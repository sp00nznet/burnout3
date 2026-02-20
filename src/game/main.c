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
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

/* Compatibility layers */
#include "../kernel/kernel.h"
#include "../kernel/xbox_memory_layout.h"
#include "../d3d/d3d8_xbox.h"
#include "../audio/dsound_xbox.h"
#include "../input/xinput_xbox.h"

/* Recompiled code */
#include "recomp/gen/recomp_funcs.h"

/* ── Crash diagnostics ─────────────────────────────────────── */

static LONG WINAPI crash_veh(PEXCEPTION_POINTERS info)
{
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        void *frames[32];
        USHORT count;
        HMODULE mod;
        char modname[MAX_PATH];
        uintptr_t base;

        fprintf(stderr, "\n=== VEH: Access violation at RIP=0x%p ===\n",
                info->ExceptionRecord->ExceptionAddress);
        fprintf(stderr, "  %s address 0x%p\n",
                info->ExceptionRecord->ExceptionInformation[0] ? "Writing" : "Reading",
                (void*)info->ExceptionRecord->ExceptionInformation[1]);

        /* Get module base to compute RVA */
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                          (LPCSTR)info->ExceptionRecord->ExceptionAddress, &mod);
        base = (uintptr_t)mod;
        GetModuleFileNameA(mod, modname, sizeof(modname));
        fprintf(stderr, "  Module: %s (base=0x%p)\n", modname, (void*)base);
        fprintf(stderr, "  Crash RVA: 0x%llX\n",
                (unsigned long long)((uintptr_t)info->ExceptionRecord->ExceptionAddress - base));

        /* Native stack trace */
        count = CaptureStackBackTrace(0, 32, frames, NULL);
        fprintf(stderr, "  Native stack (%d frames):\n", count);
        for (USHORT i = 0; i < count; i++) {
            HMODULE fmod = NULL;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                              (LPCSTR)frames[i], &fmod);
            fprintf(stderr, "    [%2d] 0x%p (RVA 0x%llX)\n",
                    i, frames[i],
                    (unsigned long long)((uintptr_t)frames[i] - (uintptr_t)fmod));
        }
        fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

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
static IDirect3D8 *g_d3d8 = NULL;
static IDirect3DDevice8 *g_d3d_device = NULL;
static IDirectSound8 *g_dsound = NULL;

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
    fflush(stderr);
    xbox_kernel_init();          /* Fill thunk table with our Win32 implementations */
    fprintf(stderr, "  xbox_kernel_init() done\n");
    fflush(stderr);
    xbox_kernel_bridge_init();   /* Patch Xbox memory thunk entries with synthetic VAs */
    fprintf(stderr, "  xbox_kernel_bridge_init() done\n");
    fflush(stderr);

    /* 2b. Pre-initialize CRT bootstrap locks.
     *
     * The Xbox CRT uses a lock table at 0x3C6500 with 36 entries (8 bytes
     * each: [pointer(4), flag(4)]). Bootstrap locks have flag=1 and must be
     * initialized before any code calls __lock(). Normally _mtinitlocks()
     * (sub_0024858A) does this during CRT startup, but we bypass the CRT
     * entry point.
     *
     * Without this, __lock(8) → _mtinitlocknum(8) → __lock(10) →
     * _mtinitlocknum(10) → __lock(10) → infinite recursion → stack overflow.
     *
     * Since all CS operations are no-ops (single-threaded execution), the
     * pointers just need to be non-zero. We use the pre-allocated CS buffer
     * array at 0x41D310 (in BSS), same as the original _mtinitlocks. */
    {
        uint32_t cs_addr = 0x41D310;  /* Pre-allocated CS buffer array */
        int locks_initialized = 0;
        int i;
        for (i = 0; i < 36; i++) {
            uint32_t flag_va = 0x3C6504 + i * 8;
            uint32_t ptr_va  = 0x3C6500 + i * 8;
            if (MEM32(flag_va) == 1) {
                MEM32(ptr_va) = cs_addr;
                cs_addr += 0x1C;  /* Each CS struct is 0x1C bytes */
                locks_initialized++;
            }
        }
        fprintf(stderr, "  CRT locks: %d bootstrap locks pre-initialized\n",
                locks_initialized);
    }

    /* 2c. Pre-initialize CRT atexit callback table.
     *
     * The CRT atexit/onexit registration function (sub_0024326D) stores
     * callback function pointers in a dynamically-allocated table whose
     * base and current pointers live at Xbox VA 0x76B92C and 0x76B928.
     * These are BSS (zero-initialized), but the code doesn't handle null:
     * sub_00246E8B → sub_001D4D65 reads RW heap block metadata at negative
     * offsets from the table pointer, crashing on MEM8(0 - 11).
     *
     * Fix: allocate a zeroed buffer with 32 bytes of padding (for the
     * negative-offset metadata reads). The zeroed metadata makes
     * sub_001D4D65 return -1 (huge capacity), so sub_0024326D always
     * finds room to store entries without needing to query block size. */
    {
        uint32_t atexit_buf = xbox_HeapAlloc(1024 + 32, 4);
        if (atexit_buf) {
            uint32_t table_base = atexit_buf + 32;
            MEM32(0x76B92C) = table_base;  /* base pointer */
            MEM32(0x76B928) = table_base;  /* current = base (empty table) */
            fprintf(stderr, "  CRT atexit: table at 0x%08X (256 entries)\n", table_base);
        } else {
            fprintf(stderr, "  WARNING: could not allocate atexit table\n");
        }
    }

    /* 3. Graphics (D3D8→D3D11) */
    fprintf(stderr, "[3/4] Graphics (D3D8→D3D11)...\n");
    {
        D3DPRESENT_PARAMETERS pp;
        HRESULT hr;

        g_d3d8 = xbox_Direct3DCreate8(0);
        if (!g_d3d8) {
            fprintf(stderr, "FATAL: Direct3DCreate8 failed\n");
            return FALSE;
        }

        memset(&pp, 0, sizeof(pp));
        pp.BackBufferWidth = DEFAULT_WIDTH;
        pp.BackBufferHeight = DEFAULT_HEIGHT;
        pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        pp.BackBufferCount = 1;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow = g_hwnd;
        pp.Windowed = TRUE;
        pp.EnableAutoDepthStencil = TRUE;
        pp.AutoDepthStencilFormat = D3DFMT_D24S8;

        hr = g_d3d8->lpVtbl->CreateDevice(g_d3d8, 0, 0, g_hwnd, 0, &pp, &g_d3d_device);
        if (FAILED(hr)) {
            fprintf(stderr, "FATAL: CreateDevice failed: 0x%08lX\n", hr);
            return FALSE;
        }
    }

    /* 4. Audio + Input */
    fprintf(stderr, "[4/4] Audio + Input...\n");
    xbox_DirectSoundCreate(NULL, &g_dsound, NULL);
    xbox_InputInit();

    fprintf(stderr, "=== All subsystems initialized ===\n\n");
    return TRUE;
}

static void shutdown_subsystems(void)
{
    fprintf(stderr, "\n=== Shutting down ===\n");

    /* Reverse order of initialization */
    if (g_dsound) {
        g_dsound->lpVtbl->Release(g_dsound);
        g_dsound = NULL;
    }
    if (g_d3d_device) {
        g_d3d_device->lpVtbl->Release(g_d3d_device);
        g_d3d_device = NULL;
    }
    if (g_d3d8) {
        g_d3d8->lpVtbl->Release(g_d3d8);
        g_d3d8 = NULL;
    }
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
         * Frame rendering.
         *
         * Eventually the recompiled game code will drive this.
         * For now, clear to dark blue and present to verify D3D works.
         */
        if (g_d3d_device) {
            g_d3d_device->lpVtbl->BeginScene(g_d3d_device);
            g_d3d_device->lpVtbl->Clear(g_d3d_device, 0, NULL,
                D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                0xFF001030,  /* Dark blue */
                1.0f, 0);
            g_d3d_device->lpVtbl->EndScene(g_d3d_device);
            g_d3d_device->lpVtbl->Present(g_d3d_device, NULL, NULL, NULL, NULL);
        }
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

    /* Run recompiled code integration tests */
    {
        int recomp_run_tests(void);
        int test_result = recomp_run_tests();
        if (test_result < 0) {
            fprintf(stderr, "WARNING: Integration tests skipped\n");
        } else if (test_result != 0) {
            fprintf(stderr, "WARNING: Some integration tests failed\n");
        }
    }

    /* Register VEH for crash diagnostics */
    AddVectoredExceptionHandler(1, crash_veh);

    /* Call the recompiled game entry point with crash protection.
     * We push a dummy return address (simulating x86 'call' instruction)
     * because the translated code expects [esp] = return addr on entry. */
    fprintf(stderr, "\n=== Calling xbe_entry_point (0x001D2807) ===\n");
    fprintf(stderr, "  g_esp = 0x%08X before call\n", g_esp);
    __try {
        PUSH32(g_esp, 0); /* simulate 'call' pushing return address */
        xbe_entry_point();
        fprintf(stderr, "xbe_entry_point returned normally (g_eax=0x%08X)\n", g_eax);
    } __except(
        (fprintf(stderr, "CRASH in xbe_entry_point: exception 0x%08lX\n",
                 GetExceptionInformation()->ExceptionRecord->ExceptionCode),
         fprintf(stderr, "  Fault address: 0x%p\n",
                 GetExceptionInformation()->ExceptionRecord->ExceptionAddress),
         GetExceptionInformation()->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
            ? fprintf(stderr, "  Access violation %s address 0x%p\n",
                      GetExceptionInformation()->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                      (void*)GetExceptionInformation()->ExceptionRecord->ExceptionInformation[1])
            : 0,
         EXCEPTION_EXECUTE_HANDLER)
    ) {
        DWORD code = GetExceptionCode();
        switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
            fprintf(stderr, "  Registers: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
                    g_eax, g_ecx, g_edx, g_esp);
            fprintf(stderr, "  ebx=0x%08X esi=0x%08X edi=0x%08X seh_ebp=0x%08X\n",
                    g_ebx, g_esi, g_edi, g_seh_ebp);
            /* Dump simulated Xbox stack to find return addresses */
            {
                int j;
                uint32_t sp = g_esp;
                fprintf(stderr, "  Xbox stack dump (16 dwords from esp=0x%08X):\n", sp);
                for (j = 0; j < 16 && sp + j*4 < XBOX_STACK_TOP; j++) {
                    uint32_t val = MEM32(sp + j*4);
                    fprintf(stderr, "    [esp+%02X] 0x%08X", j*4, val);
                    /* Mark values that look like code addresses */
                    if (val >= 0x00010000 && val < 0x002CE000)
                        fprintf(stderr, " <- .text");
                    fprintf(stderr, "\n");
                }
            }
            break;
        case EXCEPTION_STACK_OVERFLOW:
            fprintf(stderr, "  Stack overflow (infinite recursion?)\n");
            break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            fprintf(stderr, "  Integer divide by zero\n");
            break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            fprintf(stderr, "  Illegal instruction (tried to execute Xbox code VA as native?)\n");
            break;
        default:
            fprintf(stderr, "  Exception code: 0x%08lX\n", code);
            break;
        }
    }

    /* Run the game window loop */
    game_loop();

    /* Clean up */
    shutdown_subsystems();

    fprintf(stderr, "\nBurnout 3 exited normally.\n");
    return 0;
}
