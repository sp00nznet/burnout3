/**
 * Burnout 3: Takedown (Xbox) - Recompiled Game Entry Point
 *
 * Hosts the recompiled default.xbe on the shared xboxrecomp runtime
 * (../xboxrecomp), the same way hl2/src/game/main.c does. Nothing here draws,
 * drives a menu or decides game state: the title's own boot sequence runs from
 * its real entry point, and anything the host does is limited to bringing up
 * the hardware the XBE expects to find.
 *
 *   1. load the XBE                     the recompiled code reads its own
 *                                        .rdata/.data out of the mapped image
 *   2. map the Xbox memory layout       at the original virtual addresses
 *   3. bring up the MCPX APU            DirectSound spins on the audio DSP
 *   4. kernel replacement + thunk table
 *   5. set the guest stack pointer
 *   6. jump to the recompiled entry point
 *
 * Usage: burnout3 [--headless] [--record out.mp4] [--seconds N] [default.xbe]
 *
 * A window by default. --headless opens none; --record dumps every presented
 * frame and hands the sequence to ffmpeg when the run ends (after --seconds,
 * or on Ctrl+C). RECOMP_FB_DUMP=<prefix> writes frames to BMPs directly.
 * See docs/running.md.
 *
 * XBE details (retail, XDK 5849):
 *   Base addr:   0x00010000
 *   Entry point: 0x001D2807
 *   Kernel imports: 147
 */

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "recomp_icall_feedback.h"
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "usb/ohci.h"

/* RECOMP_TLS is required: the runtime defines these thread-local, and a plain
 * extern resolves to the image's TLS template rather than this thread's copy,
 * which starts the guest with every register at zero. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_seh_ebp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern ptrdiff_t g_xbox_mem_offset;

extern void xbe_entry_point(void);
int recomp_dispatch_init(void);

#define BO3_XBE_DEFAULT  "Burnout 3 Takedown\\default.xbe"
#define BO3_GAME_DIR     "Burnout 3 Takedown"
#define BO3_SAVE_DIR     "saves"

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    long size;
    void *data;

    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || !(data = malloc((size_t)size))) {
        fclose(f);
        return FALSE;
    }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }
    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

/* Every generated function is a real symbol in the image, so dbghelp turns a
 * host fault address into the guest function (sub_XXXXXXXX) that faulted. */
static void print_host_symbol(void *addr)
{
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;
    IMAGEHLP_LINE64 line;
    DWORD ldisp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)addr, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX", sym->Name, (unsigned long long)disp);
    memset(&line, 0, sizeof(line));
    line.SizeOfStruct = sizeof(line);
    if (SymGetLineFromAddr64(GetCurrentProcess(), (DWORD64)(uintptr_t)addr,
                             &ldisp, &line))
        fprintf(stderr, " (%s:%lu)", line.FileName, line.LineNumber);
    fprintf(stderr, "\n");
}

/* Return addresses on the guest stack that land in the image: a cheap guest
 * backtrace, since the host stack only shows generated C frames. */
static void print_guest_stack(void)
{
    const uint32_t *sp;
    int i, shown = 0;

    if (!g_xbox_mem_offset || !g_esp || g_esp >= 0x10000000u)
        return;
    sp = (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
    fprintf(stderr, "  guest stack @%08X:", g_esp);
    for (i = 0; i < 16; i++)
        fprintf(stderr, "%s%08X", (i % 8) ? " " : "\n    ", sp[i]);
    fprintf(stderr, "\n  guest stack return candidates:");
    for (i = 0; i < 256 && shown < 12; i++) {
        uint32_t v = sp[i];
        if (v >= 0x00011000u && v < 0x00400000u) {
            fprintf(stderr, " %08X", v);
            shown++;
        }
    }
    fprintf(stderr, "\n");
}

/* BO3_PEEK=va[,va...]: dump 64 bytes of guest memory at each address, from
 * the fault report and from each hang sample. */
static void peek_guest_memory(void)
{
    const char *p = getenv("BO3_PEEK");
    while (p && *p && g_xbox_mem_offset) {
        char *end;
        uint32_t va = (uint32_t)strtoul(p, &end, 16);
        const uint32_t *m = (const uint32_t *)((uintptr_t)g_xbox_mem_offset + va);
        int k;
        fprintf(stderr, "  peek %08X:", va);
        for (k = 0; k < 16; k++)
            fprintf(stderr, "%s%08X", (k % 8) ? " " : "\n    ", m[k]);
        fprintf(stderr, "\n");
        p = (*end == ',') ? end + 1 : NULL;
    }
}

/* The emulated MCPX APU answers its own registers. */
bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                          uint32_t fault_xbox_va, int is_write);

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    static unsigned reports;

    /* USB host controller registers are trapped MMIO, not faults. */
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        g_xbox_mem_offset && er->NumberParameters >= 2) {
        uintptr_t fault = (uintptr_t)er->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)g_xbox_mem_offset;
        if (fault >= base) {
            uint32_t va = (uint32_t)(fault - base);
            if (xbox_OhciOwnsAddress(va)
             && xbox_OhciHandleMmio(ep->ContextRecord, va))
                return EXCEPTION_CONTINUE_EXECUTION;
            /* RECOMP_AC97_READY makes the APU aperture PAGE_NOACCESS so the
             * APU model can answer it. Burnout 3 needs a working
             * DirectSoundCreate: RenderWare's audio device is created during
             * engine start (sub_0020A540), and when it fails the NULL is
             * handed on until the pipeline builder faults on it. */
            if (va >= 0xFE800000u && va < 0xFE880000u
             && apu_hook_handle_mmio(ep->ContextRecord, fault, va,
                                     (int)er->ExceptionInformation[0]))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    /* BO3_TRAP_NAN: an unmasked SSE invalid-operation -- the first op that
     * made a NaN. Log the site once, re-mask for this thread, and let the op
     * run again to produce its NaN as before. */
    if (er->ExceptionCode == STATUS_FLOAT_INVALID_OPERATION
     || er->ExceptionCode == STATUS_FLOAT_MULTIPLE_TRAPS) {
        static void *seen[40];
        static int nseen;
        int i, dup = 0;
        for (i = 0; i < nseen; i++)
            dup |= seen[i] == er->ExceptionAddress;
        if (!dup && nseen < 40) {
            STACKFRAME64 sf;
            CONTEXT walk = *ep->ContextRecord;
            int f;
            seen[nseen++] = er->ExceptionAddress;
            fprintf(stderr, "\n[NAN] invalid op #%d, thread %lu, at ", nseen,
                    GetCurrentThreadId());
            print_host_symbol(er->ExceptionAddress);
            memset(&sf, 0, sizeof sf);
            sf.AddrPC.Offset = walk.Rip;    sf.AddrPC.Mode = AddrModeFlat;
            sf.AddrFrame.Offset = walk.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
            sf.AddrStack.Offset = walk.Rsp; sf.AddrStack.Mode = AddrModeFlat;
            for (f = 0; f < 8 && StackWalk64(IMAGE_FILE_MACHINE_AMD64,
                     GetCurrentProcess(), GetCurrentThread(), &sf, &walk, NULL,
                     SymFunctionTableAccess64, SymGetModuleBase64, NULL)
                     && sf.AddrPC.Offset; f++) {
                if (f) {
                    fprintf(stderr, "      <- ");
                    print_host_symbol((void *)(uintptr_t)sf.AddrPC.Offset);
                }
            }
            fprintf(stderr, "  guest regs: eax=%08X ecx=%08X esi=%08X edi=%08X"
                            " esp=%08X\n", g_eax, g_ecx, g_esi, g_edi, g_esp);
            fflush(stderr);
        }
        ep->ContextRecord->MxCsr |= 0x80;           /* IM back on */
        ep->ContextRecord->MxCsr &= ~0x3Fu;          /* clear the flags */
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    /* Report anything fatal, not only access violations: a stack overflow or
     * illegal instruction otherwise ends the process looking like a clean
     * exit. Capped so a systematic fault does not bury the first ones. */
    if (er->ExceptionCode < 0x80000000u || ++reports > 6)
        return EXCEPTION_CONTINUE_SEARCH;

    fprintf(stderr, "\n[FAULT] code 0x%08lX at host %p (thread %lu)\n",
            (unsigned long)er->ExceptionCode, er->ExceptionAddress,
            GetCurrentThreadId());
    print_host_symbol(er->ExceptionAddress);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        er->NumberParameters >= 2) {
        uintptr_t addr = (uintptr_t)er->ExceptionInformation[1];
        fprintf(stderr, "  %s host 0x%p",
                er->ExceptionInformation[0] ? "write to" : "read from",
                (void *)addr);
        if (g_xbox_mem_offset && addr >= (uintptr_t)g_xbox_mem_offset &&
            addr < (uintptr_t)g_xbox_mem_offset + 0xF0000000u)
            fprintf(stderr, " = guest 0x%08X",
                    (uint32_t)(addr - (uintptr_t)g_xbox_mem_offset));
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "  guest regs: eax=%08X ecx=%08X edx=%08X ebx=%08X\n"
                    "              esp=%08X seh_ebp=%08X esi=%08X edi=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esp, g_seh_ebp, g_esi, g_edi);
    print_guest_stack();
    peek_guest_memory();
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* RECOMP_HANG_WATCHDOG=<seconds>: sample every thread's host RIP every N seconds
 * and name the generated function each is in. A spin shows up as the same
 * symbol with the indirect-call count flat, which is what separates a hang
 * from something merely slow. Ported from hl2/src/game/main.c. */
extern volatile uint64_t g_icall_count;

static void sample_all_threads(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();

    if (snap == INVALID_HANDLE_VALUE)
        return;
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            HANDLE th;
            CONTEXT ctx;
            FILETIME c, e, k, u;
            ULONGLONG busy = 0;

            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me)
                continue;
            th = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME |
                            THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!th)
                continue;
            if (GetThreadTimes(th, &c, &e, &k, &u))
                busy = (((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) +
                        ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime)) / 10000;
            memset(&ctx, 0, sizeof(ctx));
            ctx.ContextFlags = CONTEXT_FULL;
            /* Resume before printing: a thread suspended while holding the
             * CRT's stderr lock deadlocks the sampler's own fprintf, which is
             * exactly what heavy kernel logging makes likely. The stack of a
             * busy thread is walked while it is suspended and printed after. */
            if (SuspendThread(th) != (DWORD)-1) {
                BOOL got = GetThreadContext(th, &ctx);
                DWORD64 frames[10];
                int nframes = 0;
                if (got && busy > 10000) {
                    STACKFRAME64 sf;
                    CONTEXT walk = ctx;
                    memset(&sf, 0, sizeof sf);
                    sf.AddrPC.Offset = ctx.Rip;    sf.AddrPC.Mode = AddrModeFlat;
                    sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
                    sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
                    while (nframes < 10
                        && StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(),
                                       th, &sf, &walk, NULL,
                                       SymFunctionTableAccess64,
                                       SymGetModuleBase64, NULL)
                        && sf.AddrPC.Offset)
                        frames[nframes++] = sf.AddrPC.Offset;
                }
                ResumeThread(th);
                if (got) {
                    int f;
                    fprintf(stderr, "  tid %-6lu cpu %6llums ",
                            (unsigned long)te.th32ThreadID,
                            (unsigned long long)busy);
                    print_host_symbol((void *)(uintptr_t)ctx.Rip);
                    for (f = 1; f < nframes; f++) {
                        fprintf(stderr, "      <- ");
                        print_host_symbol((void *)(uintptr_t)frames[f]);
                    }
                }
            }
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

static DWORD WINAPI hang_watchdog(LPVOID arg)
{
    unsigned seconds = (unsigned)(uintptr_t)arg;
    unsigned long long prev = 0;
    int sample;

    for (sample = 1; ; sample++) {
        Sleep(seconds * 1000);
        fprintf(stderr, "\n[HANG] sample %d after %us, indirect calls %llu (+%llu)\n",
                sample, sample * seconds,
                (unsigned long long)g_icall_count,
                (unsigned long long)(g_icall_count - prev));
        prev = g_icall_count;
        sample_all_threads();
        peek_guest_memory();
        fflush(stderr);
    }
    return 0;
}

/* BO3_TRAP_NAN=<flag file>: once that file exists, unmask the SSE invalid
 * exception on every thread so veh_handler names the op that first makes a
 * NaN. Armed by a file, not a timer, because what matters is a moment in
 * play (the race start) that a driver script reaches at no fixed time. */
static DWORD WINAPI nan_trap_arm(LPVOID arg)
{
    const char *flag = (const char *)arg;
    HANDLE snap;
    THREADENTRY32 te;
    DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();

    while (GetFileAttributesA(flag) == INVALID_FILE_ATTRIBUTES)
        Sleep(500);
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    te.dwSize = sizeof te;
    if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
        do {
            HANDLE th;
            CONTEXT ctx;
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me)
                continue;
            th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                            THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
            if (!th)
                continue;
            if (SuspendThread(th) != (DWORD)-1) {
                memset(&ctx, 0, sizeof ctx);
                ctx.ContextFlags = CONTEXT_FLOATING_POINT;
                if (GetThreadContext(th, &ctx)) {
                    ctx.MxCsr &= ~0x80u;
                    ctx.MxCsr &= ~0x3Fu;
                    SetThreadContext(th, &ctx);
                }
                ResumeThread(th);
            }
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    if (snap != INVALID_HANDLE_VALUE)
        CloseHandle(snap);
    fprintf(stderr, "[NAN] trap armed\n");
    return 0;
}

/* --record: frames are dumped as BMPs while the game runs, and encoded when
 * it stops. ponytail: not a live pipe -- the software GPU runs at a few fps,
 * so buffering to disk costs nothing and survives an ffmpeg that is missing
 * or fails; pipe to ffmpeg's stdin if long recordings fill the disk. */
static char s_rec_out[MAX_PATH];
static char s_rec_dir[MAX_PATH];
static volatile LONG s_rec_done;

static void record_finish(void)
{
    char cmd[3 * MAX_PATH];

    if (!s_rec_out[0] || InterlockedExchange(&s_rec_done, 1))
        return;
    snprintf(cmd, sizeof cmd,
             "ffmpeg -hide_banner -loglevel error -y -framerate 30 "
             "-i \"%s\\f%%05d.bmp\" -pix_fmt yuv420p \"%s\"",
             s_rec_dir, s_rec_out);
    fprintf(stderr, "[REC] encoding %s\n", s_rec_out);
    if (system(cmd) != 0) {
        fprintf(stderr, "[REC] ffmpeg failed; the frames are in %s\n",
                s_rec_dir);
        return;
    }
    snprintf(cmd, sizeof cmd, "rmdir /s /q \"%s\"", s_rec_dir);
    system(cmd);
}

static BOOL WINAPI record_ctrl(DWORD type)
{
    (void)type;
    record_finish();
    ExitProcess(0);
    return TRUE;
}

static DWORD WINAPI record_timer(LPVOID arg)
{
    Sleep((DWORD)(uintptr_t)arg * 1000u);
    record_finish();
    ExitProcess(0);
    return 0;
}

static void record_start(const char *out, unsigned seconds)
{
    char tmp[MAX_PATH];

    GetFullPathNameA(out, MAX_PATH, s_rec_out, NULL);
    GetTempPathA(MAX_PATH, tmp);
    snprintf(s_rec_dir, sizeof s_rec_dir, "%sbo3rec_%lu", tmp,
             GetCurrentProcessId());
    CreateDirectoryA(s_rec_dir, NULL);
    {
        char prefix[MAX_PATH + 4];
        snprintf(prefix, sizeof prefix, "%s\\f", s_rec_dir);
        _putenv_s("RECOMP_FB_DUMP", prefix);
    }
    _putenv_s("RECOMP_FB_DUMP_FLIPS", "1");
    SetConsoleCtrlHandler(record_ctrl, TRUE);
    if (seconds)
        CloseHandle(CreateThread(NULL, 0, record_timer,
                                 (LPVOID)(uintptr_t)seconds, 0, NULL));
    printf("Recording to %s (frames in %s)\n", s_rec_out, s_rec_dir);
}

static void hang_watchdog_start(void)
{
    const char *spec = getenv("RECOMP_HANG_WATCHDOG");
    unsigned seconds = spec ? (unsigned)atoi(spec) : 0;

    if (seconds)
        CloseHandle(CreateThread(NULL, 0, hang_watchdog,
                                 (LPVOID)(uintptr_t)seconds, 0, NULL));
    if (getenv("BO3_TRAP_NAN"))
        CloseHandle(CreateThread(NULL, 0, nan_trap_arm,
                                 (LPVOID)getenv("BO3_TRAP_NAN"), 0, NULL));
}

int main(int argc, char **argv)
{
    const char *path = BO3_XBE_DEFAULT;
    const char *record = NULL;
    unsigned seconds = 0;
    int headless = 0, i;
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless"))
            headless = 1;
        else if (!strcmp(argv[i], "--record") && i + 1 < argc)
            record = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: %s [--headless] [--record out.mp4] "
                            "[--seconds N] [default.xbe]\n", argv[0]);
            return 2;
        } else
            path = argv[i];
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    /* Symbols up front: at fault time SymInitialize would have to allocate
     * inside a process that is already in a bad way. */
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);
    hang_watchdog_start();

    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    printf("=== Burnout 3: Takedown (Xbox) - Static Recompilation ===\n");

    /* Audio has to come up for the engine to start. RenderWare creates its
     * audio device during engine start (sub_0020A540 -> DirectSoundCreate),
     * and when that fails the NULL device is handed on until the pipeline
     * builder faults on it. DirectSound needs the AC'97 codec to report ready
     * (RECOMP_AC97_READY) and then waits for the DSP to clear a command
     * doorbell it writes 3 to; with no DSP56300 here the runtime acknowledges
     * it. The doorbell is a contiguous allocation, so its address moves with
     * the toolkit's allocator (0x825F8810, observed as ebx+0x810 in the spin
     * at 0x002F8029, was an older one's). The toolkit derives it from the GP
     * scratch table (RECOMP_APU_DSP_ACK unset means auto, since xboxrecomp
     * #193), so it is left to the toolkit here. Set either variable yourself
     * to override. */
    if (!getenv("RECOMP_AC97_READY"))
        _putenv_s("RECOMP_AC97_READY", "1");
    /* The pad. Without the USB controller model there is no way past
     * "Press START"; with it the host's XInput pad (or RECOMP_PAD_SCRIPT /
     * RECOMP_PAD_LIVE) drives the title's own XPP driver. */
    if (!getenv("RECOMP_USB"))
        _putenv_s("RECOMP_USB", "1");
    /* Two controllers: Burnout 3's split-screen modes are two-player and
     * are greyed out unless a second pad is connected. Pad 2 follows host
     * XInput pad 2, or "p2-" steps in RECOMP_PAD_LIVE / RECOMP_PAD_SCRIPT. */
    if (!getenv("RECOMP_USB_PADS"))
        _putenv_s("RECOMP_USB_PADS", "2");
    /* The software NV2A: without it nothing is drawn. RECOMP_PB_EXEC=0 turns
     * it off (the runtime tests only for presence), which runs the boot ~50x
     * faster when following it by file accesses. */
    if (!getenv("RECOMP_PB_EXEC"))
        _putenv_s("RECOMP_PB_EXEC", "1");
    else if (!strcmp(getenv("RECOMP_PB_EXEC"), "0"))
        _putenv_s("RECOMP_PB_EXEC", "");
    if (!headless && !getenv("RECOMP_FB_WINDOW"))
        _putenv_s("RECOMP_FB_WINDOW", "1");
    if (record)
        record_start(record, seconds);

    if (!load_xbe(path, &xbe_data, &xbe_size))
        return 1;
    printf("XBE loaded: %zu bytes\n", xbe_size);


    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        fprintf(stderr, "Xbox memory layout init failed "
                        "(required VA range unavailable?)\n");
        free(xbe_data);
        return 1;
    }
    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped at host offset 0x%llX\n",
           (unsigned long long)g_xbox_mem_offset);

    /* The APU reads physical RAM, which is mapped at g_xbox_mem_offset. */
    {
        typedef struct MCPXAPUState MCPXAPUState;
        extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
        extern MCPXAPUState *g_apu_state;

        g_apu_state = mcpx_apu_init_standalone(
            (uint8_t *)(uintptr_t)xbox_GetMemoryOffset());
        printf("MCPX APU: %s\n", g_apu_state ? "initialised" : "FAILED");
    }

    xbox_OhciInit();
    xbox_WatchdogStart();

    printf("Initializing kernel replacement...\n");
    xbox_kernel_init();
    /* The HDD partitions live in saves\; without the folder Partition1.img
     * cannot be created and XAPI quick-reboots at once (docs/boot.md #2). */
    CreateDirectoryA(BO3_SAVE_DIR, NULL);
    xbox_path_init(BO3_GAME_DIR, BO3_SAVE_DIR);
    xbox_kernel_bridge_init();

    g_esp = XBOX_STACK_TOP;

    /* The GPU fence D3D8LTCG waits on. sub_00351770 reserves pushbuffer
     * space and spins until the GPU has consumed enough of it:
     *
     *     mov esi, [0x35FB48]   ; the D3D device
     *     mov eax, [esi+0x30]   ; -> what the GPU has consumed
     *     mov ecx, [eax]
     *     mov eax, [esi+0x2C]   ;    what the title has submitted
     *
     * Same layout as HL2 (same XDK 5849 D3D8LTCG). The runtime advances the
     * fence as an honest acknowledgement of everything submitted; the chain
     * is re-read and bounds-checked on every poll, so registering before the
     * device exists is harmless. */
    xbox_Nv2aMirrorFence(0x0035FB48u, 0x2Cu, 0x30u);

    /* The swap counter the XMV player paces on. sub_0034E0A0 hands back
     * [device+0x1DE8], which only D3D's vblank handler (sub_00350EC0)
     * increments; the intro movie (ovid\englis30.xmv) presents a frame only
     * once that count has passed the frame's timestamp, so without a vblank
     * it holds on the first frame forever and the boot never gets past it.
     * Same field the HL2 loader drives (same XDK 5849 D3D8LTCG). */
    xbox_Nv2aFrameCounter(0x0035FB48u, 0x1DE8u);

    printf("\nStarting game at entry point 0x001D2807...\n");
    RECOMP_ICALL_FEEDBACK_INIT();
    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");
    xbox_OhciReport();
    RECOMP_ICALL_FEEDBACK_DUMP();
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);
    return 0;
}
