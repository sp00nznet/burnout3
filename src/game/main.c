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
 * Two hosts, one boot sequence (host_main): Windows x64 and POSIX (macOS
 * arm64, Linux x86-64). The host-specific parts are the fault handler (VEH
 * and dbghelp, or SIGBUS/SIGSEGV and dladdr), the window (the toolkit's SDL
 * host runs the guest on its own thread on POSIX) and the diagnostics that
 * need thread contexts, which stay Win32-only.
 *
 * Usage: burnout3 [--headless] [--record out.mp4] [--seconds N] [default.xbe]
 *
 * A window by default. --headless opens none; --record dumps every presented
 * frame and hands the sequence to ffmpeg when the run ends (after --seconds,
 * or on Ctrl+C). --seconds ends the run, recording or not.
 * RECOMP_FB_DUMP=<prefix> writes frames to BMPs directly. See docs/running.md.
 *
 * XBE details (retail, XDK 5849):
 *   Base addr:   0x00010000
 *   Entry point: 0x001D2807
 *   Kernel imports: 147
 */

#if !defined(_WIN32) && !defined(_GNU_SOURCE)
/* glibc declares Dl_info and dladdr, and the REG_RIP index into the
 * mcontext's gregs, only with _GNU_SOURCE; it must precede every header. */
#define _GNU_SOURCE
#endif
#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#else
/* <signal.h> provides ucontext_t and the Darwin mcontext; <ucontext.h> is an
 * #error on macOS without _XOPEN_SOURCE. The NT types below (BOOL, LPVOID,
 * MAX_PATH) come from the toolkit's kernel.h (platform/xbox_winnt.h). */
#include <signal.h>
#include <dlfcn.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <sys/stat.h>
#endif
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>

#include "recomp_icall_feedback.h"
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "usb/ohci.h"
#ifdef BO3_TOOLKIT_ENV_TABLE          /* the toolkit fork's environment table */
#include "platform/recomp_env.h"
#endif
#ifdef RECOMP_ENV_HAVE_ENHANCE_KEYS   /* the toolkit fork's enhancements layer */
#include "enhance.h"
#include "enhance_cfg.h"
#endif
#if !defined(_WIN32) || defined(BO3_TOOLKIT_ENV_TABLE)
/* xbox_HostWindowMain (POSIX); xbox_HostWindowSetTitle (the fork toolkit,
 * every host) */
#include "video/video_player.h"
#endif

/* RECOMP_TLS is required: the runtime defines these thread-local, and a plain
 * extern resolves to the image's TLS template rather than this thread's copy,
 * which starts the guest with every register at zero. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_seh_ebp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern ptrdiff_t g_xbox_mem_offset;

extern void xbe_entry_point(void);
int recomp_dispatch_init(void);

/* Path separator and buffer size per host: MAX_PATH (260) is the Win32
 * limit; POSIX paths under a user's home run longer than that. */
#if defined(_WIN32)
#define BO3_SEP          "\\"
#define BO3_PATH_MAX     MAX_PATH
#else
#define BO3_SEP          "/"
#define BO3_PATH_MAX     1024
#endif

#define BO3_GAME_DIR     "Burnout 3 Takedown"
#define BO3_XBE_DEFAULT  BO3_GAME_DIR BO3_SEP "default.xbe"
#define BO3_SAVE_DIR     "saves"

/* The environment defaults below are set with the host's own call: _putenv_s
 * on Windows, where an empty value removes the variable; setenv/unsetenv
 * elsewhere, with the same rule. */
static void env_set(const char *name, const char *value)
{
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    if (*value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}

/* RECOMP_STDIO_LOG=<path>: stdout and stderr into a file. The GUI-subsystem
 * exe has no console, so a launch from Explorer or a shortcut otherwise
 * loses every line it prints (a redirect, `burnout3.exe > bo3.log 2>&1`,
 * still works from cmd), and under Proton a game's stdio is often not
 * where its launcher looks. Truncate the file, then reopen both streams in
 * append mode so their writes interleave instead of overwriting each other
 * (there is no stderr descriptor to dup onto). Called before anything
 * prints; a failure goes to whichever stream still works, and the dialog
 * path below does not depend on it. */
static void stdio_log_setup(void)
{
    const char *log = getenv("RECOMP_STDIO_LOG");
    FILE *trunc;

    if (!log || !*log)
        return;
    trunc = fopen(log, "w");
    if (trunc)
        fclose(trunc);
    if (!freopen(log, "a", stdout))
        fprintf(stderr, "[BOOT] RECOMP_STDIO_LOG: cannot open %s\n", log);
    else if (!freopen(log, "a", stderr))
        printf("[BOOT] RECOMP_STDIO_LOG: stderr NOT redirected\n");
}

static void make_one_dir(const char *path)
{
#if defined(_WIN32)
    CreateDirectoryA(path, NULL);
#else
    mkdir(path, 0755);
#endif
}

/* One directory level at a time; RECOMP_HDD_DIR may name a folder whose
 * parents do not exist yet (a fresh install's data root). */
static void make_dirs(const char *path)
{
    char buf[BO3_PATH_MAX];
    char *p;

    if (snprintf(buf, sizeof buf, "%s", path) >= (int)sizeof buf)
        return;
    for (p = buf; *p; p++) {
        if ((*p == '\\' || *p == '/') && p > buf && p[-1] != ':') {
            char c = *p;
            *p = '\0';
            make_one_dir(buf);
            *p = c;
        }
    }
    make_one_dir(buf);
}

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

/* ── Fault reports ─────────────────────────────────────────── */

/* Every line of a fault report goes through here. On Windows it is
 * fprintf(stderr), as it always was. On POSIX the report runs inside a
 * signal handler, possibly while the faulting thread holds stdio's lock
 * (the kernel logs heavily), so it formats into a stack buffer and
 * write()s: a deadlock there would lose the whole report. */
static void fault_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
#if defined(_WIN32)
    vfprintf(stderr, fmt, ap);
#else
    {
        char buf[512];
        int n = vsnprintf(buf, sizeof buf, fmt, ap);
        if (n > 0) {
            size_t len = (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1;
            ssize_t r = write(STDERR_FILENO, buf, len);
            (void)r;
        }
    }
#endif
    va_end(ap);
}

/* Every generated function is a real symbol in the image, so the host fault
 * address names the guest function (sub_XXXXXXXX) that faulted: dbghelp
 * against the PDB on Windows, dladdr on the unstripped executable elsewhere
 * (static functions included, no -export_dynamic needed). */
static void print_host_symbol(void *addr)
{
#if defined(_WIN32)
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;
    IMAGEHLP_LINE64 line;
    DWORD ldisp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)addr, &disp, sym))
        fault_printf("  in %s+0x%llX", sym->Name, (unsigned long long)disp);
    memset(&line, 0, sizeof(line));
    line.SizeOfStruct = sizeof(line);
    if (SymGetLineFromAddr64(GetCurrentProcess(), (DWORD64)(uintptr_t)addr,
                             &ldisp, &line))
        fault_printf(" (%s:%lu)", line.FileName, line.LineNumber);
    fault_printf("\n");
#else
    Dl_info info;

    if (dladdr(addr, &info) && info.dli_sname && info.dli_saddr)
        fault_printf("  in %s+0x%lX\n", info.dli_sname,
                     (unsigned long)((uintptr_t)addr - (uintptr_t)info.dli_saddr));
    else
        fault_printf("  in <unknown>\n");
#endif
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
    fault_printf("  guest stack @%08X:", g_esp);
    for (i = 0; i < 16; i++)
        fault_printf("%s%08X", (i % 8) ? " " : "\n    ", sp[i]);
    fault_printf("\n  guest stack return candidates:");
    for (i = 0; i < 256 && shown < 12; i++) {
        uint32_t v = sp[i];
        if (v >= 0x00011000u && v < 0x00400000u) {
            fault_printf(" %08X", v);
            shown++;
        }
    }
    fault_printf("\n");
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
        fault_printf("  peek %08X:", va);
        for (k = 0; k < 16; k++)
            fault_printf("%s%08X", (k % 8) ? " " : "\n    ", m[k]);
        fault_printf("\n");
        p = (*end == ',') ? end + 1 : NULL;
    }
}

static void print_guest_regs(void)
{
    fault_printf("  guest regs: eax=%08X ecx=%08X edx=%08X ebx=%08X\n"
                 "              esp=%08X seh_ebp=%08X esi=%08X edi=%08X\n",
                 g_eax, g_ecx, g_edx, g_ebx, g_esp, g_seh_ebp, g_esi, g_edi);
}

#if defined(_WIN32)

/* ── Windows: VEH ──────────────────────────────────────────── */

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
    print_guest_regs();
    print_guest_stack();
    peek_guest_memory();
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Symbols up front: at fault time SymInitialize would have to allocate
 * inside a process that is already in a bad way. */
static void host_install_fault_handler(void)
{
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);
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

#else /* !_WIN32 */

/* ── POSIX: SIGBUS/SIGSEGV ─────────────────────────────────── */

/* Guest memory is reserved PROT_NONE, and on macOS touching a mapped but
 * inaccessible page is SIGBUS; only a truly unmapped address is SIGSEGV. So
 * the common guest fault arrives as SIGBUS, and both are handled. The
 * toolkit's Metal fb_guard and its memory-layout watchpoints install their own
 * SIGBUS/SIGSEGV handlers later and chain to the previous one, so this one is
 * the last link: faults they do not claim fall through to it.
 *
 * Known gaps, accepted for a bring-up tool: vsnprintf and dladdr are not
 * formally async-signal-safe (dladdr takes dyld's lock), and Darwin allocates
 * __thread storage lazily, so reading the guest registers on a thread that
 * never touched them can malloc. The Windows VEH has the same exposure with
 * fprintf. */

static void *fault_pc(void *ucv)
{
    ucontext_t *uc = (ucontext_t *)ucv;
#if defined(__APPLE__) && defined(__aarch64__)
    return (void *)(uintptr_t)uc->uc_mcontext->__ss.__pc;
#elif defined(__APPLE__) && defined(__x86_64__)
    return (void *)(uintptr_t)uc->uc_mcontext->__ss.__rip;
#elif defined(__linux__) && defined(__aarch64__)
    return (void *)(uintptr_t)uc->uc_mcontext.pc;
#elif defined(__linux__) && defined(__x86_64__)
    return (void *)(uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#else
    (void)uc;
    return NULL;
#endif
}

/* The native call chain, innermost first, by walking frame records. Apple's
 * arm64 ABI keeps a frame pointer, so starting from the signal context this
 * is exact: frame 0 is the faulting pc, frame 1 is lr (the caller even when
 * the leaf built no frame), then [fp] = caller's fp, [fp+8] = return address. */
static void print_native_stack(void *ucv)
{
#if defined(__APPLE__) && defined(__aarch64__)
    ucontext_t *uc = (ucontext_t *)ucv;
    uintptr_t pc = (uintptr_t)uc->uc_mcontext->__ss.__pc;
    uintptr_t lr = (uintptr_t)uc->uc_mcontext->__ss.__lr;
    uintptr_t fp = (uintptr_t)uc->uc_mcontext->__ss.__fp;
    int n = 0, first = 1;

    fault_printf("  native stack (innermost first):\n");
    fault_printf("    [%d] %p", n++, (void *)pc);
    print_host_symbol((void *)pc);
    fault_printf("    [%d] %p", n++, (void *)lr);
    print_host_symbol((void *)lr);
    while (n < 16 && fp && (fp & 7) == 0) {
        uintptr_t next = ((const uintptr_t *)fp)[0];
        uintptr_t ret = ((const uintptr_t *)fp)[1];

        /* A leaf that did build a frame stored lr in it: skip that one
         * repeat only; recursive frames legitimately share a return address. */
        if (!(first && ret == lr) && ret) {
            fault_printf("    [%d] %p", n++, (void *)ret);
            print_host_symbol((void *)ret);
        }
        first = 0;
        if (next <= fp)     /* the stack grows down: callers' frames are higher */
            break;
        fp = next;
    }
#else
    (void)ucv;
    fault_printf("  native stack: not available on this host\n");
#endif
}

/* Re-entry guard: a fault while writing the report must end the process, not
 * report again. SA_NODEFER lets a same-signal fault inside the handler get
 * here instead of being force-killed by the kernel with nothing said. */
static volatile sig_atomic_t g_in_fault;

static void restore_default(int sig)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

static void posix_fault_handler(int sig, siginfo_t *si, void *ucv)
{
    uintptr_t addr = (uintptr_t)si->si_addr;
    void *pc = fault_pc(ucv);

    /* Trapped hardware registers -> their models, through the toolkit's
     * dispatch: the APU aperture (RECOMP_AC97_READY), the AC'97 bus-master
     * page and the USB OHCI controllers the title's XPP driver programs. A
     * serviced access returns here with pc advanced; the kernel resumes the
     * thread after the instruction. Always 0 on a host with no decoder. */
    if (xbox_PosixMmioFault(ucv, addr))
        return;

    /* Atomic, so two threads faulting at once give one report. */
    if (__atomic_exchange_n(&g_in_fault, 1, __ATOMIC_SEQ_CST)) {
        restore_default(sig);
        return;
    }

    fault_printf("\n[FAULT] %s at host %p (thread %lx)\n",
                 sig == SIGBUS ? "SIGBUS" : sig == SIGSEGV ? "SIGSEGV" : "signal",
                 pc, (unsigned long)(uintptr_t)pthread_self());
    print_host_symbol(pc);
    fault_printf("  access to host %p", (void *)addr);
    if (g_xbox_mem_offset && addr >= (uintptr_t)g_xbox_mem_offset &&
        addr < (uintptr_t)g_xbox_mem_offset + 0xF0000000u)
        fault_printf(" = guest 0x%08X", (uint32_t)(addr - (uintptr_t)g_xbox_mem_offset));
    fault_printf("\n");
    print_guest_regs();
    print_guest_stack();
    peek_guest_memory();
    print_native_stack(ucv);

    /* Returning re-executes the faulting instruction with the default
     * disposition in place, so the process dies by the original signal: the
     * shell's exit status and any core dump say what happened. */
    restore_default(sig);
}

/* Alternate signal stack for the thread that installs the handler (the one
 * that runs the guest), so a stack overflow there still gets a report.
 * 128 KB, the Darwin SIGSTKSZ: snprintf plus a dladdr per frame does not fit
 * in much less. A literal, because SIGSTKSZ is not a compile-time constant on
 * glibc >= 2.34. */
static char g_fault_altstack[128 * 1024];

static void host_install_fault_handler(void)
{
    stack_t ss;
    struct sigaction sa;

    memset(&ss, 0, sizeof ss);
    ss.ss_sp = g_fault_altstack;
    ss.ss_size = sizeof g_fault_altstack;
    if (sigaltstack(&ss, NULL) != 0)
        fprintf(stderr, "[BOOT] sigaltstack failed; a guest-thread stack "
                        "overflow will die without a report\n");

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = posix_fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
}

/* The thread samplers need Win32 thread contexts (toolhelp, dbghelp,
 * SetThreadContext). The toolkit's RECOMP_WATCHDOG_SECS dumps the guest
 * stack at a hang on every host; say so rather than stay silently inert. */
static void hang_watchdog_start(void)
{
    if (getenv("RECOMP_HANG_WATCHDOG"))
        fprintf(stderr, "[BOOT] RECOMP_HANG_WATCHDOG is Windows-only; "
                        "RECOMP_WATCHDOG_SECS dumps the guest stack at a hang\n");
    if (getenv("BO3_TRAP_NAN"))
        fprintf(stderr, "[BOOT] BO3_TRAP_NAN is Windows-only\n");
}

#endif /* _WIN32 */

/* ── --record and --seconds ────────────────────────────────── */

/* --record: frames are dumped as BMPs while the game runs, and encoded when
 * it stops. ponytail: not a live pipe -- the software GPU runs at a few fps,
 * so buffering to disk costs nothing and survives an ffmpeg that is missing
 * or fails; pipe to ffmpeg's stdin if long recordings fill the disk. */
static char s_rec_out[BO3_PATH_MAX];
static char s_rec_dir[BO3_PATH_MAX];
#if defined(_WIN32)
static volatile LONG s_rec_done;
#define REC_DONE_TEST_AND_SET() InterlockedExchange(&s_rec_done, 1)
#else
static volatile int s_rec_done;
#define REC_DONE_TEST_AND_SET() __atomic_exchange_n(&s_rec_done, 1, __ATOMIC_SEQ_CST)
#endif

static void record_finish(void)
{
    char cmd[3 * BO3_PATH_MAX];

    if (!s_rec_out[0] || REC_DONE_TEST_AND_SET())
        return;
    snprintf(cmd, sizeof cmd,
             "ffmpeg -hide_banner -loglevel error -y -framerate 30 "
             "-i \"%s" BO3_SEP "f%%05d.bmp\" -pix_fmt yuv420p \"%s\"",
             s_rec_dir, s_rec_out);
    fprintf(stderr, "[REC] encoding %s\n", s_rec_out);
    if (system(cmd) != 0) {
        fprintf(stderr, "[REC] ffmpeg failed; the frames are in %s\n",
                s_rec_dir);
        return;
    }
#if defined(_WIN32)
    snprintf(cmd, sizeof cmd, "rmdir /s /q \"%s\"", s_rec_dir);
#else
    snprintf(cmd, sizeof cmd, "rm -rf \"%s\"", s_rec_dir);
#endif
    system(cmd);
}

/* The run's end: the recording (if any) is encoded, then the process exits.
 * exit() rather than a hard exit on POSIX so the kernel's atexit reports (the
 * missing-file summary) print; ExitProcess on Windows, as before. */
static void run_end(void)
{
    record_finish();
#if defined(_WIN32)
    ExitProcess(0);
#else
    exit(0);
#endif
}

#if defined(_WIN32)
static BOOL WINAPI record_ctrl(DWORD type)
{
    (void)type;
    run_end();
    return TRUE;
}

static DWORD WINAPI run_timer(LPVOID arg)
{
    Sleep((DWORD)(uintptr_t)arg * 1000u);
    run_end();
    return 0;
}

static void run_timer_start(unsigned seconds)
{
    CloseHandle(CreateThread(NULL, 0, run_timer,
                             (LPVOID)(uintptr_t)seconds, 0, NULL));
}
#else
static void record_sigint(int sig)
{
    (void)sig;
    run_end();
}

static void *run_timer(void *arg)
{
    sleep((unsigned)(uintptr_t)arg);
    run_end();
    return NULL;
}

static void run_timer_start(unsigned seconds)
{
    pthread_t t;
    if (pthread_create(&t, NULL, run_timer, (void *)(uintptr_t)seconds) == 0)
        pthread_detach(t);
}
#endif

static void record_start(const char *out)
{
    char prefix[BO3_PATH_MAX + 4];
#if defined(_WIN32)
    char tmp[MAX_PATH];

    GetFullPathNameA(out, MAX_PATH, s_rec_out, NULL);
    GetTempPathA(MAX_PATH, tmp);
    snprintf(s_rec_dir, sizeof s_rec_dir, "%sbo3rec_%lu", tmp,
             GetCurrentProcessId());
    CreateDirectoryA(s_rec_dir, NULL);
    SetConsoleCtrlHandler(record_ctrl, TRUE);
#else
    const char *tmp = getenv("TMPDIR");
    char cwd[BO3_PATH_MAX];

    /* An absolute output path: the encoder runs at exit, and the working
     * directory is whatever the title left it. */
    if (out[0] == '/' || !getcwd(cwd, sizeof cwd))
        snprintf(s_rec_out, sizeof s_rec_out, "%s", out);
    else
        snprintf(s_rec_out, sizeof s_rec_out, "%s/%s", cwd, out);
    snprintf(s_rec_dir, sizeof s_rec_dir, "%s/bo3rec_%ld",
             tmp && *tmp ? tmp : "/tmp", (long)getpid());
    mkdir(s_rec_dir, 0755);
    /* The toolkit's SDL host leaves Ctrl-C with its default meaning; with a
     * recording in progress it has to encode first. */
    signal(SIGINT, record_sigint);
#endif
    snprintf(prefix, sizeof prefix, "%s" BO3_SEP "f", s_rec_dir);
    env_set("RECOMP_FB_DUMP", prefix);
    env_set("RECOMP_FB_DUMP_FLIPS", "1");
    printf("Recording to %s (frames in %s)\n", s_rec_out, s_rec_dir);
}

/* ── Startup failures and the window ───────────────────────── */

static int s_headless;

/* A required startup step failed: say which, and what to do, on stderr on
 * every host -- and in a dialog on Windows, since a launch from Explorer or
 * a shortcut has no console and would otherwise end without a word.
 * --headless (or RECOMP_HEADLESS) means no dialog: one would wait for a
 * click nobody can give. */
static void host_report_fatal(const char *step, const char *detail)
{
    fprintf(stderr, "[FATAL] %s: %s\n", step, detail);
    fflush(stderr);
#if defined(_WIN32)
    if (!s_headless && !getenv("RECOMP_HEADLESS"))
        MessageBoxA(NULL, detail, "Burnout 3: Takedown", MB_ICONERROR);
#endif
}

#if defined(_WIN32)
/* The window's close button ends the run. On xboxrecomp v0.14.0 the GDI
 * presenter's WM_CLOSE only stops its own thread, which then destroys the
 * window; the game itself keeps running. A console build at least left its
 * console to close, but a GUI build would go on invisibly (audio playing,
 * the CPU busy, saves\ held open) and the next launch would be a second
 * instance on the same saves. So watch for this process's presenter window,
 * and once it has been up and is gone, end the run as --seconds does:
 * encode any recording, then ExitProcess. Gone includes replaced: the
 * pushbuffer executor reopens the window on its next surface clear, often
 * within one poll, so a new handle means the old window was closed. */
#define BO3_FB_WINDOW_CLASS "XboxRecompFramebuffer"

static BOOL CALLBACK fb_window_find(HWND h, LPARAM out)
{
    DWORD pid = 0;
    char cls[64];

    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && GetClassNameA(h, cls, sizeof cls) &&
        !strcmp(cls, BO3_FB_WINDOW_CLASS)) {
        *(HWND *)out = h;
        return FALSE;
    }
    return TRUE;
}

static DWORD WINAPI fb_window_watch(LPVOID unused)
{
    HWND seen = NULL;

    (void)unused;
    for (;;) {
        HWND h = NULL;

        Sleep(250);
        EnumWindows(fb_window_find, (LPARAM)&h);
        if (seen && h != seen)
            break;
        seen = h;
    }
    fprintf(stderr, "[BOOT] window closed: ending the run\n");
    run_end();
    return 0;
}
#endif

/* The toolkit fork's enhancements (render scale, present filter,
 * fullscreen, pacing): config.default.toml beside the executable,
 * config/config.toml over it, the environment over both, every key stock
 * by default. The user root is the working directory: a developer runs
 * from the repository root, as "Burnout 3 Takedown" and saves/ already
 * assume, so their choices live in the gitignored config/config.toml; a
 * bundle's launcher names the file with RECOMP_CONFIG. Before the guest
 * and the window, so the backends and the presenter read settled values.
 * Without the layer (upstream's toolkit, or -DXBOXRECOMP_ENHANCE=OFF) this
 * is nothing. */
static void host_enhance_init(void)
{
#ifdef RECOMP_ENV_HAVE_ENHANCE_KEYS
    xbox_enhance_init(".", NULL);
    enhance_cfg_report_unused();
    fflush(stderr);
#endif
}

/* ── Boot sequence (shared) ────────────────────────────────── */

static const char *s_xbe_path = BO3_XBE_DEFAULT;
static const char *s_game_dir = BO3_GAME_DIR, *s_save_dir = BO3_SAVE_DIR;
/* RECOMP_GAME_FILES when it named s_xbe_path, for the load failure. */
static const char *s_game_env;

static int host_main(void)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    /* Before anything can fault. On POSIX this also binds the alternate
     * signal stack to this thread, the one that runs the guest. */
    host_install_fault_handler();
    hang_watchdog_start();

    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    printf("=== Burnout 3: Takedown (Xbox) - Static Recompilation ===\n");

    if (!load_xbe(s_xbe_path, &xbe_data, &xbe_size)) {
        char detail[BO3_PATH_MAX + 200];
        /* Name where the path came from: a launcher's RECOMP_GAME_FILES
         * points at the install, the default at the working directory. */
        if (s_game_env)
            snprintf(detail, sizeof detail,
                     "Failed to load %s (from RECOMP_GAME_FILES=%s).",
                     s_xbe_path, s_game_env);
        else
            snprintf(detail, sizeof detail,
                     "Failed to load %s. Run from the folder that holds "
                     BO3_GAME_DIR BO3_SEP ", or pass the path to default.xbe.",
                     s_xbe_path);
        host_report_fatal("load XBE", detail);
        return 1;
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);

    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        host_report_fatal("map Xbox memory layout",
                          "Xbox memory layout init failed (the required "
                          "virtual address range may be unavailable).");
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
    make_dirs(s_save_dir);
    printf("[BOOT] game files: %s; hard disk: %s\n", s_game_dir, s_save_dir);
    xbox_path_init(s_game_dir, s_save_dir);
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


int main(int argc, char **argv)
{
    const char *record = NULL;
    unsigned seconds = 0;
    const char *xbe_arg = NULL;
    static char xbe_buf[BO3_PATH_MAX];
    int headless = 0, i;

    stdio_log_setup();

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless"))
            headless = s_headless = 1;
        else if (!strcmp(argv[i], "--record") && i + 1 < argc)
            record = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (argv[i][0] == '-') {
            char detail[BO3_PATH_MAX + 200];
            snprintf(detail, sizeof detail,
                     "Unknown option %s.\nusage: burnout3 [--headless] "
                     "[--record out.mp4] [--seconds N] [default.xbe]",
                     argv[i]);
            host_report_fatal("parse arguments", detail);
            return 2;
        } else
            xbe_arg = argv[i];
    }
    if (xbe_arg)
        s_xbe_path = xbe_arg;
    /* RECOMP_GAME_FILES and RECOMP_HDD_DIR: the packaged launchers start the
     * game from its logs folder and name the dump and the emulated hard disk
     * here, so an install keeps its saves outside the program files and
     * outside the Wine prefix. Unset, the folders are the ones above,
     * relative to the working directory; a default.xbe argument still wins.
     * After the arguments, so --headless keeps the fatal below off the
     * screen. */
    {
        const char *g = getenv("RECOMP_GAME_FILES");
        const char *h = getenv("RECOMP_HDD_DIR");
        if (g && *g) {
            s_game_dir = g;
            if (!xbe_arg) {
                if (snprintf(xbe_buf, sizeof xbe_buf, "%s" BO3_SEP "default.xbe", g)
                    >= (int)sizeof xbe_buf) {
                    host_report_fatal("load XBE", "the game files path "
                                      "(RECOMP_GAME_FILES) is too long");
                    return 1;
                }
                s_xbe_path = xbe_buf;
                s_game_env = g;
            }
        }
        if (h && *h)
            s_save_dir = h;
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
#ifdef BO3_TOOLKIT_ENV_TABLE
    /* Deprecated names and bad keys found when the table was read: held
     * until stdio is where it will stay, so they land in the log. */
    recomp_env_flush_notes();
#endif

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
        env_set("RECOMP_AC97_READY", "1");
    /* The pad. Without the USB controller model there is no way past
     * "Press START"; with it the host's XInput pad (or RECOMP_PAD_SCRIPT /
     * RECOMP_PAD_LIVE) drives the title's own XPP driver. */
    if (!getenv("RECOMP_USB"))
        env_set("RECOMP_USB", "1");
    /* Two controllers: Burnout 3's split-screen modes are two-player and
     * are greyed out unless a second pad is connected. Pad 2 follows host
     * XInput pad 2, or "p2-" steps in RECOMP_PAD_LIVE / RECOMP_PAD_SCRIPT. */
    if (!getenv("RECOMP_USB_PADS"))
        env_set("RECOMP_USB_PADS", "2");
    /* The software NV2A: without it nothing is drawn. RECOMP_PB_EXEC=0 turns
     * it off (the runtime tests only for presence), which runs the boot ~50x
     * faster when following it by file accesses. */
    if (!getenv("RECOMP_PB_EXEC"))
        env_set("RECOMP_PB_EXEC", "1");
    else if (!strcmp(getenv("RECOMP_PB_EXEC"), "0"))
        env_set("RECOMP_PB_EXEC", "");
#if defined(_WIN32)
    /* The window: the CPU rasteriser shows its frames in the framebuffer
     * window. The toolkit fork's D3D11 backend (RECOMP_PB_BACKEND=d3d11)
     * opens a window of its own, so there the framebuffer window would be a
     * second one; it is asked for on every other backend, the fork's own
     * test (fb_window_wanted). */
#ifdef BO3_TOOLKIT_ENV_TABLE
    {
        const char *b = recomp_env(RENV_PB_BACKEND);
        if (!headless && !getenv("RECOMP_FB_WINDOW") && !(b && !_stricmp(b, "d3d11")))
            env_set("RECOMP_FB_WINDOW", "1");
    }
#else
    if (!headless && !getenv("RECOMP_FB_WINDOW"))
        env_set("RECOMP_FB_WINDOW", "1");
#endif
    if (!headless)
        CloseHandle(CreateThread(NULL, 0, fb_window_watch, NULL, 0, NULL));
#else
    /* The toolkit's SDL host: RECOMP_HEADLESS keeps the guest on this thread
     * with no window; otherwise xbox_HostWindowMain below opens one, which
     * xboxrecomp v0.14.0 does only with RECOMP_FB_WINDOW set (the same switch
     * as the Win32 window). */
    if (headless)
        env_set("RECOMP_HEADLESS", "1");
    else if (!getenv("RECOMP_FB_WINDOW"))
        env_set("RECOMP_FB_WINDOW", "1");
    /* D3D8 (XDK 5849) builds the KEVENT its vblank DPC sets without
     * KeInitializeEvent. The toolkit gives such an event a host event only on
     * request; without it KeSetEvent hands the guest address to the host as a
     * handle, which fails silently on Win32 and faults on POSIX. An explicit
     * value in the environment still wins. */
    if (!getenv("RECOMP_TITLE_KEVENTS"))
        env_set("RECOMP_TITLE_KEVENTS", "1");
    /* With that host event D3D's present really waits on the vblank event
     * its ISR (vector 3) signals, so the boot stops after flip 1 unless the
     * runtime raises vblanks (Windows, with no host event, boots without).
     * v0.14.0 only checks that the key is set, so any value, 0 included,
     * keeps them on. */
    if (!getenv("RECOMP_VBLANK"))
        env_set("RECOMP_VBLANK", "1");
#endif
#if defined(__APPLE__)
    /* The GPU on the Mac, where a toolkit has one: the CPU rasteriser runs
     * the front end at a few fps and a race at one or two. A toolkit without
     * a Metal backend ignores the key and draws with the CPU rasteriser in
     * the SDL window; RECOMP_PB_BACKEND=cpu asks for that anyway. */
    if (!getenv("RECOMP_PB_BACKEND"))
        env_set("RECOMP_PB_BACKEND", "metal");
#endif
    if (record)
        record_start(record);
    if (seconds)
        run_timer_start(seconds);
#ifdef BO3_TOOLKIT_ENV_TABLE
    /* The table is read once, on its first use (the notes and the backend
     * test above); re-read it so the defaults set above reach it. Without
     * this RECOMP_USB stays off and no pad (host or script) ever gets past
     * Press START. */
    recomp_env_reload();
#endif
    host_enhance_init();
#ifdef BO3_TOOLKIT_ENV_TABLE
    /* The title bar's name; otherwise the XBE certificate's ("Burnout 3"). */
    xbox_HostWindowSetTitle("Burnout 3: Takedown");
#endif

#if defined(_WIN32)
    return host_main();
#else
    /* The process main thread belongs to the window: AppKit only runs its
     * event loop there, so the toolkit opens the SDL window here and runs
     * host_main -- the whole boot and the guest -- on a "guest-main"
     * pthread. Headless, it calls host_main on this thread. */
    return xbox_HostWindowMain(host_main);
#endif
}
