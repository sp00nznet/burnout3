/**
 * kernel_bridge.c - Bridge between translated game code and kernel functions
 *
 * Problem:
 *   Translated game code calls kernel functions via indirect calls through
 *   the kernel thunk table at VA 0x0036B7C0. In the XBE file, these entries
 *   contain unresolved ordinals (0x80000000 | ordinal). On real Xbox hardware,
 *   the kernel loader replaces these with actual function pointers before the
 *   game runs.
 *
 * Solution:
 *   1. After xbox_MemoryLayoutInit copies .rdata, call xbox_kernel_bridge_init()
 *   2. Replace each ordinal entry in Xbox memory with a synthetic VA
 *   3. When RECOMP_ICALL encounters a synthetic VA, route it to a per-ordinal
 *      bridge function that reads args from the simulated Xbox stack, translates
 *      pointer arguments from Xbox VA→native, and calls the kernel function.
 *
 * Synthetic VA scheme:
 *   Each thunk slot i gets VA 0xFE000000 + i*4
 *   The lookup function checks this range and dispatches appropriately.
 *
 * Why per-ordinal bridges instead of a generic trampoline:
 *   Kernel functions receive Xbox pointers (32-bit VAs) that must be translated
 *   to native pointers by adding g_xbox_mem_offset. Different functions have
 *   different parameter layouts (pointer vs value), so each needs its own bridge.
 */

#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdio.h>

/* Access to recompiled code globals */
extern uint32_t g_eax, g_ecx, g_edx, g_esp;
extern uint32_t g_ebx, g_esi, g_edi;
extern ptrdiff_t g_xbox_mem_offset;

/* Dispatch table lookup (for function pointer args) */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);

/* Memory access - same as recomp_types.h MEM32 but without the #define guard */
#define BRIDGE_MEM32(addr) (*(volatile uint32_t *)((uintptr_t)(addr) + g_xbox_mem_offset))

/* Translate Xbox VA to native pointer (NULL-safe: 0 → NULL) */
#define XBOX_TO_NATIVE(va) ((va) ? (void*)((uintptr_t)(va) + g_xbox_mem_offset) : NULL)

/* ── Synthetic VA range (for function exports) ─────────── */

#define KERNEL_VA_BASE  0xFE000000u
#define KERNEL_VA_END   (KERNEL_VA_BASE + XBOX_KERNEL_THUNK_TABLE_SIZE * 4)

/* ── Kernel data exports ──────────────────────────────────
 *
 * Some kernel ordinals are DATA exports (structs/variables), not functions.
 * The game reads their thunk entries and dereferences the result to access
 * the data. These cannot use synthetic VAs — they must point to real,
 * dereferenceable addresses in the Xbox VA space.
 *
 * We allocate a "kernel data area" at XBOX_KERNEL_DATA_BASE and populate
 * it with the expected structures.
 */

#define BRIDGE_MEM16(addr) (*(volatile uint16_t *)((uintptr_t)(addr) + g_xbox_mem_offset))
#define BRIDGE_MEM8(addr)  (*(volatile uint8_t  *)((uintptr_t)(addr) + g_xbox_mem_offset))

/**
 * Get the Xbox VA of data for a kernel DATA export ordinal.
 * Returns 0 if the ordinal is not a data export (i.e., it's a function).
 */
static uint32_t kernel_data_va_for_ordinal(ULONG ordinal)
{
    switch (ordinal) {
    case  17: return XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE;
    case  65: return XBOX_KERNEL_DATA_BASE + KDATA_IO_COMPLETION_TYPE;
    case  71: return XBOX_KERNEL_DATA_BASE + KDATA_IO_DEVICE_TYPE;
    case 156: return XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT;
    case 164: return XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE;
    case 259: return XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE;
    case 322: return XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO;
    case 323: return XBOX_KERNEL_DATA_BASE + KDATA_HD_KEY;
    case 324: return XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION;
    case 325: return XBOX_KERNEL_DATA_BASE + KDATA_SIGNATURE_KEY;
    case 326: return XBOX_KERNEL_DATA_BASE + KDATA_LAN_KEY;
    case 327: return XBOX_KERNEL_DATA_BASE + KDATA_ALT_SIGNATURE_KEYS;
    case 328: return XBOX_KERNEL_DATA_BASE + KDATA_XE_IMAGE_FILENAME;
    case 355: return XBOX_KERNEL_DATA_BASE + KDATA_LAN_KEY;         /* alias */
    case 356: return XBOX_KERNEL_DATA_BASE + KDATA_ALT_SIGNATURE_KEYS; /* alias */
    case 357: return XBOX_KERNEL_DATA_BASE + KDATA_XE_PUBLIC_KEY;
    default:  return 0;  /* Not a data export */
    }
}

/**
 * Initialize kernel data export values at the kernel data area.
 * Called during bridge init, after Xbox memory is mapped.
 */
static void kernel_data_init(void)
{
    /* XboxHardwareInfo (ordinal 322) - XBOX_HARDWARE_INFO
     *   +0: ULONG Flags (0 = retail, 0x20 = devkit)
     *   +4: UCHAR GpuRevision
     *   +5: UCHAR McpRevision
     */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 0) = 0;   /* Retail */
    BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 4) = 0xA1; /* NV2A A1 */
    BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 5) = 0xB1; /* MCPX B1 */

    /* XboxKrnlVersion (ordinal 324) - XBOX_KRNL_VERSION
     *   +0: USHORT Major (1)
     *   +2: USHORT Minor (0)
     *   +4: USHORT Build (5849 = XDK version)
     *   +6: USHORT Qfe (0)
     */
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 0) = 1;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 2) = 0;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 4) = 5849;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 6) = 0;

    /* KeTickCount (ordinal 156) - starts at 0, would increment per tick */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT) = 0;

    /* LaunchDataPage (ordinal 164) - NULL (no launch data) */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) = 0;

    /* PsThreadObjectType (ordinal 259) - type object (stub: 0) */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE) = 0;

    /* ExEventObjectType (ordinal 17) - type object (stub: 0) */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE) = 0;

    /* IoCompletionObjectType (ordinal 65) - type object (stub: 0) */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IO_COMPLETION_TYPE) = 0;

    /* IoDeviceObjectType (ordinal 71) - type object (stub: 0) */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IO_DEVICE_TYPE) = 0;

    /* XboxHDKey (ordinal 323) - 16 bytes of zeros (no key) */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_HD_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxSignatureKey (ordinal 325) - 16 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_SIGNATURE_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxLANKey (ordinals 326, 355) - 16 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_LAN_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxAlternateSignatureKeys (ordinals 327, 356) - 256 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_ALT_SIGNATURE_KEYS) + g_xbox_mem_offset), 0, 256);

    /* XePublicKeyData (ordinal 357) - 284 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_XE_PUBLIC_KEY) + g_xbox_mem_offset), 0, 284);

    fprintf(stderr, "  Kernel data exports: initialized at Xbox VA 0x%08X\n",
            XBOX_KERNEL_DATA_BASE);
}

/* ── Per-slot ordinal and bridge function ────────────────── */

/* Ordinal for each slot (read from Xbox memory during init) */
static ULONG g_slot_ordinals[XBOX_KERNEL_THUNK_TABLE_SIZE];

/* Log counter - limit output to avoid flooding */
static int g_kernel_call_count = 0;

/* Read Xbox stack arg as uint32_t.
 * After kernel_thunk_dispatch pops the dummy return address (g_esp += 4),
 * arg0 is at g_esp+0, arg1 at g_esp+4, etc. */
#define STACK_ARG(n) ((uint32_t)BRIDGE_MEM32(g_esp + (n) * 4))

/* ── Per-ordinal bridge functions ─────────────────────────
 *
 * Each bridge reads args from the Xbox stack, translates pointer
 * args from Xbox VA→native, calls the kernel function, and stores
 * the result in g_eax.
 *
 * Xbox cdecl: args pushed right-to-left, caller cleans stack.
 * Xbox stdcall: args pushed right-to-left, callee cleans stack.
 * In our case the caller (translated code) does "PUSH32" for each arg
 * before calling, and the kernel function's ret-N is handled by the
 * translated code's own stack adjustment.
 */

/* ── PsCreateSystemThreadEx (ordinal 255) ────────────────
 * NTSTATUS PsCreateSystemThreadEx(
 *   PHANDLE ThreadHandle,      // arg0: Xbox VA → pointer
 *   ULONG ThreadExtraSize,     // arg1: value
 *   ULONG KernelStackSize,     // arg2: value
 *   ULONG TlsDataSize,         // arg3: value
 *   PULONG ThreadId,           // arg4: Xbox VA → pointer (can be NULL)
 *   PVOID StartContext1,       // arg5: Xbox VA → opaque
 *   PVOID StartContext2,       // arg6: Xbox VA → opaque
 *   BOOLEAN CreateSuspended,   // arg7: value
 *   BOOLEAN DebugStack,        // arg8: value
 *   PXBOX_SYSTEM_ROUTINE StartRoutine  // arg9: Xbox function pointer
 * )
 *
 * For static recompilation, we don't create a real thread.
 * Instead we call the StartRoutine synchronously via RECOMP_ICALL.
 * This is correct because on Xbox, the entry point creates a system
 * thread and returns, and the thread runs the actual game.
 */
static void bridge_PsCreateSystemThreadEx(void)
{
    uint32_t xbox_handle_ptr = STACK_ARG(0);
    uint32_t start_context1  = STACK_ARG(5);
    uint32_t start_context2  = STACK_ARG(6);
    uint32_t start_routine   = STACK_ARG(9);

    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: routine=0x%08X ctx1=0x%08X ctx2=0x%08X\n",
            start_routine, start_context1, start_context2);
    fflush(stderr);

    /* Write a fake handle to the output pointer */
    if (xbox_handle_ptr) {
        BRIDGE_MEM32(xbox_handle_ptr) = 0xBEEF0001;  /* fake handle */
    }

    /* Call the start routine synchronously through the recomp dispatch.
     * Xbox thread start routines receive two parameters:
     *   void ThreadRoutine(PVOID StartContext1, PVOID StartContext2)
     * We push both onto the simulated stack (right-to-left). */
    if (start_routine) {
        recomp_func_t fn = recomp_lookup(start_routine);
        if (!fn) fn = recomp_lookup_manual(start_routine);
        if (fn) {
            /* Push args right-to-left for the start routine */
            g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context2;  /* [esp+8] → ebp+0xC */
            g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context1;  /* [esp+4] → ebp+0x8 */
            /* Push dummy return addr (simulating call) */
            g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
            fn();
            /* Clean the 3 items we pushed (dummy ret + 2 args).
             * The start routine's epilog (esp = ebp + 4) leaves esp
             * pointing at our dummy return address, not past it. */
            g_esp += 12;
            fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: start routine returned (g_eax=0x%08X)\n", g_eax);
            fflush(stderr);
        } else {
            fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: start routine 0x%08X not found in dispatch!\n",
                    start_routine);
        }
    }

    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── NtClose (ordinal 187) ───────────────────────────────
 * NTSTATUS NtClose(HANDLE Handle)
 * Handle is a value (not a pointer), so safe for generic call.
 */
static void bridge_NtClose(void)
{
    uint32_t handle = STACK_ARG(0);

    if (g_kernel_call_count <= 100) {
        fprintf(stderr, "  [KERNEL] NtClose: handle=0x%08X\n", handle);
        fflush(stderr);
    }

    /* Don't actually close - might be a fake handle */
    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── MmAllocateContiguousMemory (ordinal 165) ─────────────
 * PVOID MmAllocateContiguousMemory(ULONG NumberOfBytes)
 */
static void bridge_MmAllocateContiguousMemory(void)
{
    uint32_t size = STACK_ARG(0);

    /* Allocate from Xbox heap so MEM32(result) works correctly */
    uint32_t xbox_va = xbox_HeapAlloc(size, 4096);

    if (g_kernel_call_count <= 100) {
        fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemory: size=%u → Xbox VA 0x%08X\n",
                size, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── MmAllocateContiguousMemoryEx (ordinal 166) ───────────
 * PVOID MmAllocateContiguousMemoryEx(SIZE_T size, ULONG_PTR low, ULONG_PTR high,
 *                                     ULONG alignment, ULONG protect)
 */
static void bridge_MmAllocateContiguousMemoryEx(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t low = STACK_ARG(1);
    uint32_t high = STACK_ARG(2);
    uint32_t align = STACK_ARG(3);
    uint32_t prot = STACK_ARG(4);

    /* Allocate from Xbox heap with requested alignment */
    if (align < 4096) align = 4096;
    uint32_t xbox_va = xbox_HeapAlloc(size, align);

    if (g_kernel_call_count <= 100) {
        fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemoryEx: size=%u align=%u → Xbox VA 0x%08X\n",
                size, align, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── MmFreeContiguousMemory (ordinal 171) ─────────────────
 * VOID MmFreeContiguousMemory(PVOID BaseAddress)
 */
static void bridge_MmFreeContiguousMemory(void)
{
    uint32_t addr = STACK_ARG(0);
    xbox_HeapFree(addr);
    g_eax = 0;
}

/* ── NtAllocateVirtualMemory (ordinal 184) ────────────────
 * NTSTATUS NtAllocateVirtualMemory(PVOID *BaseAddress, ULONG ZeroBits,
 *     PULONG AllocationSize, ULONG AllocationType, ULONG Protect)
 */
static void bridge_NtAllocateVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);  /* PVOID* in Xbox VA */
    uint32_t zero_bits = STACK_ARG(1);
    uint32_t size_ptr = STACK_ARG(2);  /* PULONG in Xbox VA */
    uint32_t alloc_type = STACK_ARG(3);
    uint32_t protect = STACK_ARG(4);

    /* Read the requested size from Xbox memory */
    uint32_t size = size_ptr ? BRIDGE_MEM32(size_ptr) : 0;
    /* Read the base address hint (0 = let kernel choose) */
    uint32_t base_hint = base_ptr ? BRIDGE_MEM32(base_ptr) : 0;

    if (g_kernel_call_count <= 200) {
        fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: base=0x%08X size=%u type=0x%X prot=0x%X\n",
                base_hint, size, alloc_type, protect);
        fflush(stderr);
    }

    if (size == 0) {
        g_eax = 0xC0000045u; /* STATUS_INVALID_PAGE_PROTECTION */
        return;
    }

    /*
     * Xbox NtAllocateVirtualMemory supports two modes:
     * - MEM_RESERVE (0x2000): Reserve virtual address space
     * - MEM_COMMIT  (0x1000): Commit pages within a reserved region
     * - MEM_RESERVE|MEM_COMMIT (0x3000): Both in one call
     *
     * Our Xbox heap (bump allocator) always commits memory immediately,
     * so MEM_COMMIT on an already-reserved region is a no-op.
     * Only allocate new memory when MEM_RESERVE is requested.
     */
    if (base_hint != 0 && (alloc_type & 0x2000) == 0) {
        /* MEM_COMMIT only, on an already-reserved region.
         * The memory is already committed by our bump allocator.
         * Don't change the base address - just return success. */
        if (g_kernel_call_count <= 200) {
            fprintf(stderr, "  [KERNEL] → MEM_COMMIT on existing region 0x%08X, no-op\n", base_hint);
            fflush(stderr);
        }
        g_eax = 0; /* STATUS_SUCCESS */
        return;
    }

    /* Allocate from Xbox heap (MEM_RESERVE or MEM_RESERVE|MEM_COMMIT) */
    uint32_t xbox_va = xbox_HeapAlloc(size, 4096);
    if (!xbox_va) {
        g_eax = 0xC0000017u; /* STATUS_NO_MEMORY */
        return;
    }

    /* Write back the allocated address and actual size */
    if (base_ptr) BRIDGE_MEM32(base_ptr) = xbox_va;
    if (size_ptr) BRIDGE_MEM32(size_ptr) = size;

    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── NtFreeVirtualMemory (ordinal 199) ────────────────────
 * NTSTATUS NtFreeVirtualMemory(PVOID *BaseAddress, PULONG FreeSize,
 *     ULONG FreeType)
 */
static void bridge_NtFreeVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);
    uint32_t size_ptr = STACK_ARG(1);
    uint32_t free_type = STACK_ARG(2);

    g_eax = (uint32_t)xbox_NtFreeVirtualMemory(
        XBOX_TO_NATIVE(base_ptr), XBOX_TO_NATIVE(size_ptr), free_type);
}

/* ── ExAllocatePool / ExAllocatePoolWithTag (ordinals 15, 16) ─
 * Must allocate from Xbox heap so the returned pointer is an Xbox VA
 * that can be accessed via MEM32(). Native HeapAlloc returns 64-bit
 * pointers that get truncated and produce garbage Xbox VAs.
 */
static void bridge_ExAllocatePool(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t xbox_va = xbox_HeapAlloc(size, 16);

    if (g_kernel_call_count <= 200) {
        fprintf(stderr, "  [KERNEL] ExAllocatePool: size=%u → Xbox VA 0x%08X\n",
                size, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

static void bridge_ExAllocatePoolWithTag(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t tag = STACK_ARG(1);
    uint32_t xbox_va = xbox_HeapAlloc(size, 16);

    if (g_kernel_call_count <= 200) {
        fprintf(stderr, "  [KERNEL] ExAllocatePoolWithTag: size=%u tag='%c%c%c%c' → Xbox VA 0x%08X\n",
                size,
                (char)(tag & 0xFF), (char)((tag >> 8) & 0xFF),
                (char)((tag >> 16) & 0xFF), (char)((tag >> 24) & 0xFF),
                xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── KfRaiseIrql / KfLowerIrql (ordinals 160, 161) ────── */
static void bridge_KfRaiseIrql(void)
{
    uint32_t new_irql = STACK_ARG(0);
    g_eax = (uint32_t)xbox_KfRaiseIrql((UCHAR)new_irql);
}

static void bridge_KfLowerIrql(void)
{
    uint32_t new_irql = STACK_ARG(0);
    xbox_KfLowerIrql((UCHAR)new_irql);
    g_eax = 0;
}

/* ── KeRaiseIrqlToDpcLevel (ordinal 129) ─────────────────── */
static void bridge_KeRaiseIrqlToDpcLevel(void)
{
    g_eax = (uint32_t)xbox_KeRaiseIrqlToDpcLevel();
}

/* ── RtlInitializeCriticalSection / Enter / Leave (ordinals 291, 277, 294) ─ */
static void bridge_RtlInitializeCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlInitializeCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

static void bridge_RtlEnterCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlEnterCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

static void bridge_RtlLeaveCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlLeaveCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

/* ── KeQueryPerformanceCounter / Frequency (ordinals 126, 127) ─ */
static void bridge_KeQueryPerformanceCounter(void)
{
    LARGE_INTEGER li = xbox_KeQueryPerformanceCounter();
    g_eax = (uint32_t)li.LowPart;
    g_edx = (uint32_t)li.HighPart;
}

static void bridge_KeQueryPerformanceFrequency(void)
{
    LARGE_INTEGER li = xbox_KeQueryPerformanceFrequency();
    g_eax = (uint32_t)li.LowPart;
    g_edx = (uint32_t)li.HighPart;
}

/* ── KeQuerySystemTime (ordinal 128) ─────────────────────── */
static void bridge_KeQuerySystemTime(void)
{
    uint32_t time_ptr = STACK_ARG(0);
    xbox_KeQuerySystemTime(XBOX_TO_NATIVE(time_ptr));
    g_eax = 0;
}

/* ── MmQueryStatistics (ordinal 181) ─────────────────────── */
static void bridge_MmQueryStatistics(void)
{
    uint32_t stats_ptr = STACK_ARG(0);
    g_eax = (uint32_t)xbox_MmQueryStatistics(XBOX_TO_NATIVE(stats_ptr));
}

/* ── NtCreateEvent (ordinal 189) ─────────────────────────── */
static void bridge_NtCreateEvent(void)
{
    uint32_t handle_ptr = STACK_ARG(0);
    uint32_t obj_attr_ptr = STACK_ARG(1);
    uint32_t event_type = STACK_ARG(2);
    uint32_t initial_state = STACK_ARG(3);

    g_eax = (uint32_t)xbox_NtCreateEvent(
        XBOX_TO_NATIVE(handle_ptr),
        XBOX_TO_NATIVE(obj_attr_ptr),
        event_type, initial_state);
}

/* ── KeSetEvent (ordinal 145) ────────────────────────────── */
static void bridge_KeSetEvent(void)
{
    uint32_t event_ptr = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    uint32_t wait = STACK_ARG(2);

    g_eax = (uint32_t)xbox_KeSetEvent(XBOX_TO_NATIVE(event_ptr), increment, (BOOLEAN)wait);
}

/* ── KeWaitForSingleObject (ordinal 159) ─────────────────── */
static void bridge_KeWaitForSingleObject(void)
{
    uint32_t object = STACK_ARG(0);
    uint32_t wait_reason = STACK_ARG(1);
    uint32_t wait_mode = STACK_ARG(2);
    uint32_t alertable = STACK_ARG(3);
    uint32_t timeout_ptr = STACK_ARG(4);

    g_eax = (uint32_t)xbox_KeWaitForSingleObject(
        XBOX_TO_NATIVE(object), wait_reason, wait_mode,
        (BOOLEAN)alertable, XBOX_TO_NATIVE(timeout_ptr));
}

/* ── NtYieldExecution (ordinal 238) ──────────────────────── */
static void bridge_NtYieldExecution(void)
{
    g_eax = (uint32_t)xbox_NtYieldExecution();
}

/* ── MmGetPhysicalAddress (ordinal 173) ──────────────────── */
static void bridge_MmGetPhysicalAddress(void)
{
    uint32_t addr = STACK_ARG(0);
    /* Xbox uses identity mapping (physical == virtual) for the lower 64MB.
     * Just return the Xbox VA as-is. Don't call xbox_MmGetPhysicalAddress
     * which would return a native pointer. */
    g_eax = addr;
}

/* ── MmSetAddressProtect (ordinal 182) ───────────────────── */
static void bridge_MmSetAddressProtect(void)
{
    uint32_t addr = STACK_ARG(0);
    uint32_t size = STACK_ARG(1);
    uint32_t prot = STACK_ARG(2);

    xbox_MmSetAddressProtect(XBOX_TO_NATIVE(addr), size, prot);
    g_eax = 0;
}

/* ── AvSetDisplayMode (ordinal 3) ────────────────────────── */
static void bridge_AvSetDisplayMode(void)
{
    uint32_t addr = STACK_ARG(0);
    uint32_t step = STACK_ARG(1);
    uint32_t mode = STACK_ARG(2);
    uint32_t format = STACK_ARG(3);
    uint32_t pitch = STACK_ARG(4);
    uint32_t fb = STACK_ARG(5);

    xbox_AvSetDisplayMode(XBOX_TO_NATIVE(addr), step, mode, format, pitch, fb);
    g_eax = 0;
}

/* ── PsTerminateSystemThread (ordinal 258) ───────────────
 * VOID PsTerminateSystemThread(NTSTATUS ExitStatus)
 *
 * On real Xbox, this terminates the calling thread (never returns).
 * In our recompiled version, threads run synchronously, so we just
 * return. The caller (sub_001D1818) handles this gracefully.
 */
static void bridge_PsTerminateSystemThread(void)
{
    uint32_t exit_status = STACK_ARG(0);

    fprintf(stderr, "  [KERNEL] PsTerminateSystemThread: status=0x%08X\n", exit_status);
    fflush(stderr);

    g_eax = exit_status;
    /* Simply return - caller will clean up */
}

/* ── HalReadSMCTrayState (ordinal 47) ─────────────────────
 * VOID HalReadSMCTrayState(PDWORD TrayState, PDWORD TrayStateChangeCount)
 *
 * Returns DVD tray state. 0x10 = no disc, 0x14 = tray closed with disc.
 */
static void bridge_HalReadSMCTrayState(void)
{
    uint32_t state_ptr = STACK_ARG(0);
    uint32_t count_ptr = STACK_ARG(1);

    if (state_ptr) BRIDGE_MEM32(state_ptr) = 0x10;  /* No disc */
    if (count_ptr) BRIDGE_MEM32(count_ptr) = 0;
    g_eax = 0;
}

/* ── KeInitializeDpc (ordinal 107) ────────────────────────
 * VOID KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE DeferredRoutine,
 *                       PVOID DeferredContext)
 *
 * Initializes a DPC object. The Xbox KDPC structure is 32 bytes.
 * We zero it and set the routine and context pointers.
 */
static void bridge_KeInitializeDpc(void)
{
    uint32_t dpc_va = STACK_ARG(0);
    uint32_t routine = STACK_ARG(1);
    uint32_t context = STACK_ARG(2);

    /* Zero the structure (32 bytes) */
    memset(XBOX_TO_NATIVE(dpc_va), 0, 32);

    /* Set Type (0x13 = DpcObject) and fields */
    BRIDGE_MEM16(dpc_va + 0) = 0x13;   /* Type */
    BRIDGE_MEM32(dpc_va + 12) = routine; /* DeferredRoutine */
    BRIDGE_MEM32(dpc_va + 16) = context; /* DeferredContext */
    g_eax = 0;
}

/* ── KeInitializeTimerEx (ordinal 113) ────────────────────
 * VOID KeInitializeTimerEx(PKTIMER Timer, TIMER_TYPE Type)
 *
 * Initializes a timer object. Xbox KTIMER is 40 bytes.
 */
static void bridge_KeInitializeTimerEx(void)
{
    uint32_t timer_va = STACK_ARG(0);
    uint32_t type = STACK_ARG(1);

    /* Zero the structure (40 bytes) */
    memset(XBOX_TO_NATIVE(timer_va), 0, 40);

    /* Set Type (0x08 = TimerNotificationObject, 0x09 = TimerSynchronizationObject) */
    BRIDGE_MEM16(timer_va + 0) = (uint16_t)(0x08 + (type & 1));
    g_eax = 0;
}

/* ── KeSetTimer / KeSetTimerEx (ordinal 149/150) ──────────
 * BOOLEAN KeSetTimer(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc)
 *
 * Sets a timer. We don't actually start timers - just record the state.
 * Returns FALSE (timer was not already set).
 */
static void bridge_KeSetTimer(void)
{
    /* Timer functionality is not needed for basic execution.
     * Return FALSE = timer was not previously set. */
    g_eax = 0;
}

/* ── ExQueryPoolBlockSize (ordinal 24) ────────────────────
 * ULONG ExQueryPoolBlockSize(PVOID PoolBlock)
 *
 * Returns the size of a pool memory block.
 * Since we use HeapAlloc, we can query the Windows heap.
 */
static void bridge_ExQueryPoolBlockSize(void)
{
    uint32_t block = STACK_ARG(0);
    /* Return a reasonable default size. Actual pool blocks are managed
     * by the kernel; for recompilation, returning 0 might be OK since
     * code usually uses this for debugging/stats. */
    g_eax = 0;
}

/* ── RtlNtStatusToDosError (ordinal 301) ─────────────────
 * ULONG RtlNtStatusToDosError(NTSTATUS Status)
 *
 * Converts an NTSTATUS to a Win32 error code.
 */
static void bridge_RtlNtStatusToDosError(void)
{
    uint32_t status = STACK_ARG(0);

    /* Simple mapping of common status codes */
    switch (status) {
    case 0x00000000: g_eax = 0; break;          /* STATUS_SUCCESS → ERROR_SUCCESS */
    case 0xC0000034: g_eax = 2; break;          /* STATUS_OBJECT_NAME_NOT_FOUND → ERROR_FILE_NOT_FOUND */
    case 0xC000003A: g_eax = 3; break;          /* STATUS_OBJECT_PATH_NOT_FOUND → ERROR_PATH_NOT_FOUND */
    case 0xC0000022: g_eax = 5; break;          /* STATUS_ACCESS_DENIED → ERROR_ACCESS_DENIED */
    case 0xC0000008: g_eax = 6; break;          /* STATUS_INVALID_HANDLE → ERROR_INVALID_HANDLE */
    case 0xC0000017: g_eax = 8; break;          /* STATUS_NO_MEMORY → ERROR_NOT_ENOUGH_MEMORY */
    case 0xC000000D: g_eax = 87; break;         /* STATUS_INVALID_PARAMETER → ERROR_INVALID_PARAMETER */
    default:         g_eax = 317; break;         /* ERROR_MR_MID_NOT_FOUND (generic) */
    }
}

/* ── NtCreateFile (ordinal 190) ──────────────────────────── */
static void bridge_NtCreateFile(void)
{
    /* File I/O stub - return STATUS_OBJECT_NAME_NOT_FOUND */
    g_eax = 0xC0000034u;
}

/* ── NtOpenFile (ordinal 202) ────────────────────────────── */
static void bridge_NtOpenFile(void)
{
    /* File I/O stub - return STATUS_OBJECT_NAME_NOT_FOUND */
    g_eax = 0xC0000034u;
}

/* ── NtCreateDirectoryObject (ordinal 188) ──────────────── */
static void bridge_NtCreateDirectoryObject(void)
{
    /* Return STATUS_SUCCESS with a fake handle */
    uint32_t handle_ptr = STACK_ARG(0);
    if (handle_ptr) BRIDGE_MEM32(handle_ptr) = 0xBEEF0010;
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* ── IoCreateSymbolicLink (ordinal 63) ───────────────────── */
static void bridge_IoCreateSymbolicLink(void)
{
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* ── ObReferenceObjectByHandle (ordinal 246) ─────────────── */
static void bridge_ObReferenceObjectByHandle(void)
{
    /* Xbox: NTSTATUS ObReferenceObjectByHandle(HANDLE Handle, PVOID ObjectType, PVOID* Object)
     * 3 args (not 6 like Windows NT) */
    uint32_t handle = STACK_ARG(0);
    uint32_t obj_type = STACK_ARG(1);
    uint32_t object_ptr = STACK_ARG(2);
    if (object_ptr) BRIDGE_MEM32(object_ptr) = 0;
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* ── RtlRaiseException (ordinal 302) ─────────────────────
 * VOID RtlRaiseException(PEXCEPTION_RECORD ExceptionRecord)
 *
 * Called by CRT / SEH code to raise structured exceptions.
 * On Xbox this triggers the kernel exception dispatcher.
 * For recompilation, we log and continue (no real SEH dispatch yet).
 */
static void bridge_RtlRaiseException(void)
{
    uint32_t record_ptr = STACK_ARG(0);
    uint32_t code = record_ptr ? BRIDGE_MEM32(record_ptr) : 0;

    static int raise_count = 0;
    raise_count++;
    if (raise_count <= 10) {
        fprintf(stderr, "  [KERNEL] RtlRaiseException: record=0x%08X code=0x%08X (#%d)\n",
                record_ptr, code, raise_count);
        fflush(stderr);
    }

    /* Don't actually raise - just return. Many Xbox games use SEH for
     * benign purposes (e.g., probing memory, FPU state detection). */
    g_eax = 0;
}

/* ── MmMapIoSpace (ordinal 177) ──────────────────────────
 * PVOID MmMapIoSpace(ULONG_PTR PhysicalAddress, ULONG NumberOfBytes, ULONG Protect)
 *
 * Maps physical I/O memory (GPU registers, etc.) into virtual address space.
 * Allocate from Xbox heap so the returned pointer is a valid Xbox VA.
 */
static void bridge_MmMapIoSpace(void)
{
    uint32_t phys_addr = STACK_ARG(0);
    uint32_t num_bytes = STACK_ARG(1);
    uint32_t protect = STACK_ARG(2);
    uint32_t xbox_va = xbox_HeapAlloc(num_bytes, 4096);

    fprintf(stderr, "  [KERNEL] MmMapIoSpace: phys=0x%08X size=%u → Xbox VA 0x%08X\n",
            phys_addr, num_bytes, xbox_va);
    fflush(stderr);

    g_eax = xbox_va;
}

/* ── MmPersistContiguousMemory (ordinal 178) ─────────────
 * VOID MmPersistContiguousMemory(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN Persist)
 *
 * Marks contiguous memory as persistent across reboots (for save data).
 * No-op for recompilation.
 */
static void bridge_MmPersistContiguousMemory(void)
{
    /* No-op stub */
    g_eax = 0;
}

/* ── Generic fallback for simple value-only functions ────── */
static void bridge_generic_stub(void)
{
    /* For functions we haven't specifically bridged yet, log and return 0 */
    g_eax = 0;
}

/* ── Dispatch table: ordinal → bridge function + stack arg bytes ── */

typedef void (*bridge_func_t)(void);

/**
 * stdcall arg byte count for each kernel ordinal.
 * On x86 stdcall, the callee cleans (ret N). Our bridges must do the same
 * via g_esp += N after execution so the simulated stack stays balanced.
 *
 * Special cases:
 *   - KfRaiseIrql/KfLowerIrql: fastcall (arg in ecx), 0 stack bytes
 *   - KeSetTimer: DueTime is LARGE_INTEGER (8 bytes on stack) + Timer + Dpc
 */
static int stdcall_args_for_ordinal(ULONG ordinal)
{
    switch (ordinal) {
    /* ── Display / AV ── */
    case   1: return  0;  /* AvGetSavedDataAddress(void) */
    case   2: return 16;  /* AvSendTVEncoderOption(4) */
    case   3: return 24;  /* AvSetDisplayMode(6) */
    case   4: return  4;  /* AvSetSavedDataAddress(1) */

    /* ── Unknown stubs ── */
    case   8: return  0;  /* Unknown_8(void) */
    case  23: return  0;  /* Unknown_23(void) */
    case  42: return  0;  /* Unknown_42(void) */

    /* ── Pool Allocator ── */
    case  15: return  4;  /* ExAllocatePool(1) */
    case  16: return  8;  /* ExAllocatePoolWithTag(2) */
    /* case  17: DATA export - ExEventObjectType */
    case  24: return  4;  /* ExQueryPoolBlockSize(1) */

    /* ── HAL ── */
    case  40: return  4;  /* HalClearSoftwareInterrupt(1) */
    case  41: return  8;  /* HalDisableSystemInterrupt(2) */
    case  44: return  8;  /* HalGetInterruptVector(2) */
    case  46: return  8;  /* HalReadSMCTrayState(2) */
    case  47: return 24;  /* HalReadWritePCISpace(6) */
    case  49: return  4;  /* HalRequestSoftwareInterrupt(1) */
    case 358: return  0;  /* HalIsResetOrShutdownPending(void) */

    /* ── I/O Manager ── */
    case  62: return 36;  /* IoBuildDeviceIoControlRequest(9) */
    /* case  65: DATA export - IoCompletionObjectType */
    case  67: return 40;  /* IoCreateFile(10) */
    case  69: return  4;  /* IoDeleteDevice(1) */
    /* case  71: DATA export - IoDeviceObjectType */
    case  74: return 12;  /* IoInitializeIrp(3) */
    case  81: return 20;  /* IoSetIoCompletion(5) */
    case  83: return  8;  /* IoStartNextPacket(2) */
    case  84: return 12;  /* IoStartNextPacketByKey(3) */
    case  85: return 16;  /* IoStartPacket(4) */
    case  86: return 32;  /* IoSynchronousDeviceIoControlRequest(8) */
    case  87: return 20;  /* IoSynchronousFsdRequest(5) */
    case 359: return  4;  /* IoMarkIrpMustComplete(1) */

    /* ── Kernel Synchronization ── */
    case  95: return  8;  /* KeAlertThread(2) */
    case  97: return  4;  /* KeBugCheck(1) */
    case  98: return 20;  /* KeBugCheckEx(5) */
    case  99: return  4;  /* KeCancelTimer(1) */
    case 100: return  4;  /* KeConnectInterrupt(1) */
    case 107: return 12;  /* KeInitializeDpc(3) */
    case 109: return 28;  /* KeInitializeInterrupt(7) */
    case 113: return  8;  /* KeInitializeTimerEx(2) */
    case 119: return 12;  /* KeInsertQueueDpc(3) */
    case 124: return  4;  /* KeQueryBasePriorityThread(1) */
    case 126: return  0;  /* KeQueryPerformanceCounter(void) */
    case 127: return  0;  /* KeQueryPerformanceFrequency(void) */
    case 128: return  4;  /* KeQuerySystemTime(1) */
    case 129: return  0;  /* KeRaiseIrqlToDpcLevel(void) */
    case 137: return  4;  /* KeRemoveQueueDpc(1) */
    case 139: return  4;  /* KeRestoreFloatingPointState(1) */
    case 142: return  4;  /* KeSaveFloatingPointState(1) */
    case 143: return  8;  /* KeSetBasePriorityThread(2) */
    case 145: return 12;  /* KeSetEvent(3) */
    case 149: return 16;  /* KeSetTimer(Timer+DueTime[8]+Dpc) */
    case 150: return 20;  /* KeSetTimerEx(Timer+DueTime[8]+Period+Dpc) */
    case 151: return  4;  /* KeStallExecutionProcessor(1) */
    case 153: return 12;  /* KeSynchronizeExecution(3) */
    /* case 156: DATA export - KeTickCount */
    case 158: return 32;  /* KeWaitForMultipleObjects(8) */
    case 159: return 20;  /* KeWaitForSingleObject(5) */
    case 160: return  0;  /* KfRaiseIrql (fastcall: arg in ecx) */
    case 161: return  0;  /* KfLowerIrql (fastcall: arg in ecx) */

    /* ── Launch Data ── */
    /* case 164: DATA export - LaunchDataPage */

    /* ── Memory Management ── */
    case 165: return  4;  /* MmAllocateContiguousMemory(1) */
    case 166: return 20;  /* MmAllocateContiguousMemoryEx(5) */
    case 168: return  8;  /* MmClaimGpuInstanceMemory(2) */
    case 169: return  8;  /* MmCreateKernelStack(2) */
    case 170: return  8;  /* MmDeleteKernelStack(2) */
    case 171: return  4;  /* MmFreeContiguousMemory(1) */
    case 173: return  4;  /* MmGetPhysicalAddress(1) */
    case 175: return 12;  /* MmLockUnlockBufferPages(3) */
    case 176: return  8;  /* MmLockUnlockPhysicalPage(2) */
    case 177: return 12;  /* MmMapIoSpace(3) */
    case 178: return 12;  /* MmPersistContiguousMemory(3) */
    case 179: return  4;  /* MmQueryAddressProtect(1) */
    case 180: return  4;  /* MmQueryAllocationSize(1) */
    case 181: return  4;  /* MmQueryStatistics(1) */
    case 182: return 12;  /* MmSetAddressProtect(3) */

    /* ── NT Virtual Memory ── */
    case 184: return 20;  /* NtAllocateVirtualMemory(5) */

    /* ── NT File I/O & Handle ── */
    case 187: return  4;  /* NtClose(1) */
    case 189: return 16;  /* NtCreateEvent(4) */
    case 190: return 36;  /* NtCreateFile(9) */
    case 193: return 16;  /* NtCreateSemaphore(4) */
    case 195: return  4;  /* NtDeleteFile(1) */
    case 196: return 40;  /* NtDeviceIoControlFile(10) */
    case 197: return 12;  /* NtDuplicateObject(3) */
    case 198: return  8;  /* NtFlushBuffersFile(2) */
    case 199: return 12;  /* NtFreeVirtualMemory(3) */
    case 200: return 40;  /* NtFsControlFile(10) */
    case 202: return 24;  /* NtOpenFile(6) */
    case 203: return  8;  /* NtOpenSymbolicLinkObject(2) */
    case 207: return 36;  /* NtQueryDirectoryFile(9) */
    case 210: return  8;  /* NtQueryFullAttributesFile(2) */
    case 211: return 20;  /* NtQueryInformationFile(5) */
    case 215: return 12;  /* NtQuerySymbolicLinkObject(3) */
    case 217: return 16;  /* NtQueryVirtualMemory(4) */
    case 218: return 20;  /* NtQueryVolumeInformationFile(5) */
    case 219: return 32;  /* NtReadFile(8) */
    case 222: return 12;  /* NtReleaseSemaphore(3) */
    case 225: return  8;  /* NtSetEvent(2) */
    case 226: return 20;  /* NtSetInformationFile(5) */
    case 228: return  8;  /* NtSetSystemTime(2) */
    case 233: return 20;  /* NtWaitForMultipleObjectsEx(5) */
    case 234: return 12;  /* NtWaitForSingleObject(3) */
    case 236: return 32;  /* NtWriteFile(8) */
    case 238: return  0;  /* NtYieldExecution(void) */

    /* ── Object Manager ── */
    case 246: return 12;  /* ObReferenceObjectByHandle(3) - Xbox: Handle,Type,Object* */
    case 247: return 20;  /* ObReferenceObjectByName(5) */
    case 250: return  0;  /* ObfDereferenceObject (fastcall: arg in ecx) */

    /* ── Network / PHY ── */
    case 252: return  4;  /* PhyGetLinkState(1) */
    case 253: return  8;  /* PhyInitialize(2) */

    /* ── Threading ── */
    case 255: return 40;  /* PsCreateSystemThreadEx(10) */
    case 256: return 12;  /* KeDelayExecutionThread(3) */
    case 258: return  4;  /* PsTerminateSystemThread(1) */
    /* case 259: DATA export - PsThreadObjectType */

    /* ── Runtime Library ── */
    case 260: return 12;  /* RtlAnsiStringToUnicodeString(3) */
    case 269: return 12;  /* RtlCompareMemoryUlong(3) */
    case 277: return  4;  /* RtlEnterCriticalSection(1) */
    case 279: return 12;  /* RtlEqualString(3) */
    case 289: return  8;  /* RtlInitAnsiString(2) */
    case 291: return  4;  /* RtlInitializeCriticalSection(1) */
    case 294: return  4;  /* RtlLeaveCriticalSection(1) */
    case 301: return  4;  /* RtlNtStatusToDosError(1) */
    case 302: return  4;  /* RtlRaiseException(1) */
    case 304: return  8;  /* RtlTimeFieldsToTime(2) */
    case 305: return  8;  /* RtlTimeToTimeFields(2) */
    case 308: return 12;  /* RtlUnicodeStringToAnsiString(3) */
    case 312: return 16;  /* RtlUnwind(4) */
    case 354: return 12;  /* RtlRip(3) */

    /* ── Xbox Identity (data exports) ── */
    /* cases 322-328, 355-357: DATA exports */

    /* ── Port I/O ── */
    case 335: return 12;  /* WRITE_PORT_BUFFER_USHORT(3) */
    case 336: return 12;  /* WRITE_PORT_BUFFER_ULONG(3) */

    /* ── Crypto ── */
    case 337: return  4;  /* XcSHAInit(1) */
    case 338: return 12;  /* XcSHAUpdate(3) */
    case 339: return  8;  /* XcSHAFinal(2) */
    case 340: return 12;  /* XcRC4Key(3) */
    case 344: return 12;  /* XcPKDecPrivate(3) */
    case 345: return  4;  /* XcPKGetKeyLen(1) */
    case 346: return 12;  /* XcVerifyPKCS1Signature(3) */
    case 347: return 20;  /* XcModExp(5) */
    case 349: return 12;  /* XcKeyTable(3) */
    case 353: return  8;  /* XcUpdateCrypto(2) */

    default:  return  0;  /* DATA exports or truly unknown */
    }
}

static bridge_func_t bridge_for_ordinal(ULONG ordinal)
{
    switch (ordinal) {
    /* Threading */
    case 255: return bridge_PsCreateSystemThreadEx;
    case 258: return bridge_PsTerminateSystemThread;

    /* File/Handle */
    case 187: return bridge_NtClose;
    case 190: return bridge_NtCreateFile;
    case 202: return bridge_NtOpenFile;

    /* Memory - contiguous */
    case 165: return bridge_MmAllocateContiguousMemory;
    case 166: return bridge_MmAllocateContiguousMemoryEx;
    case 171: return bridge_MmFreeContiguousMemory;
    case 173: return bridge_MmGetPhysicalAddress;
    case 182: return bridge_MmSetAddressProtect;
    case 181: return bridge_MmQueryStatistics;

    /* Memory - virtual */
    case 184: return bridge_NtAllocateVirtualMemory;
    case 199: return bridge_NtFreeVirtualMemory;

    /* Pool */
    case  15: return bridge_ExAllocatePool;
    case  16: return bridge_ExAllocatePoolWithTag;
    case  24: return bridge_ExQueryPoolBlockSize;

    /* IRQL */
    case 160: return bridge_KfRaiseIrql;
    case 161: return bridge_KfLowerIrql;
    case 129: return bridge_KeRaiseIrqlToDpcLevel;

    /* Critical sections */
    case 291: return bridge_RtlInitializeCriticalSection;
    case 277: return bridge_RtlEnterCriticalSection;
    case 294: return bridge_RtlLeaveCriticalSection;

    /* Timing */
    case 126: return bridge_KeQueryPerformanceCounter;
    case 127: return bridge_KeQueryPerformanceFrequency;
    case 128: return bridge_KeQuerySystemTime;
    case 149: return bridge_KeSetTimer;
    case 150: return bridge_KeSetTimer;  /* KeSetTimerEx */

    /* DPC / Timer init */
    case 107: return bridge_KeInitializeDpc;
    case 113: return bridge_KeInitializeTimerEx;

    /* Synchronization */
    case 189: return bridge_NtCreateEvent;
    case 145: return bridge_KeSetEvent;
    case 159: return bridge_KeWaitForSingleObject;
    case 238: return bridge_NtYieldExecution;

    /* Hardware */
    case  47: return bridge_HalReadSMCTrayState;

    /* Display */
    case   3: return bridge_AvSetDisplayMode;

    /* I/O */
    case  63: return bridge_IoCreateSymbolicLink;
    case 188: return bridge_NtCreateDirectoryObject;
    case 246: return bridge_ObReferenceObjectByHandle;

    /* Memory - I/O mapping */
    case 177: return bridge_MmMapIoSpace;
    case 178: return bridge_MmPersistContiguousMemory;

    /* RTL */
    case 301: return bridge_RtlNtStatusToDosError;
    case 302: return bridge_RtlRaiseException;

    default:  return NULL;
    }
}

/* ── Per-slot bridge functions (resolved at init) ────────── */

static bridge_func_t g_slot_bridges[XBOX_KERNEL_THUNK_TABLE_SIZE];
static int g_slot_arg_bytes[XBOX_KERNEL_THUNK_TABLE_SIZE];

/* Current dispatching slot */
static int g_kernel_dispatch_slot = -1;

static void kernel_thunk_dispatch(void)
{
    int slot = g_kernel_dispatch_slot;
    bridge_func_t bridge;
    ULONG ordinal;

    if (slot < 0 || slot >= XBOX_KERNEL_THUNK_TABLE_SIZE) {
        fprintf(stderr, "  [KERNEL] bad slot %d\n", slot);
        g_eax = 0;
        g_esp += 4;  /* pop dummy return address */
        return;
    }

    ordinal = g_slot_ordinals[slot];
    bridge = g_slot_bridges[slot];

    g_kernel_call_count++;

    if (g_kernel_call_count <= 200) {
        fprintf(stderr, "  [KERNEL] #%d: ordinal %u (slot %d) esp=0x%08X\n",
                g_kernel_call_count, ordinal, slot, g_esp);
        fflush(stderr);
    }

    /* Pop the dummy return address that PUSH32(esp, 0) pushed before RECOMP_ICALL.
     * On real x86, "call [thunk]" pushes a real return address and "ret" pops it.
     * In our model, the bridge is called directly (not via the simulated stack),
     * so we must manually consume the dummy return address. */
    g_esp += 4;

    if (bridge) {
        bridge();
    } else {
        /* No specific bridge - log warning and return 0 */
        if (g_kernel_call_count <= 200) {
            fprintf(stderr, "  [KERNEL] WARNING: no bridge for ordinal %u, returning 0\n", ordinal);
            fflush(stderr);
        }
        g_eax = 0;
    }

    /* Clean stdcall args from the simulated stack.
     * On real x86, stdcall callee does "ret N" to pop the return address
     * and N bytes of arguments. We already popped the dummy return address
     * above; now pop the args. */
    g_esp += g_slot_arg_bytes[slot];

    if (g_kernel_call_count <= 200) {
        fprintf(stderr, "  [KERNEL] → returned 0x%08X\n", g_eax);
        fflush(stderr);
    }
}

/* ── Dispatch lookup ────────────────────────────────────── */

/**
 * Look up a kernel thunk by synthetic VA.
 * Called as a fallback when recomp_lookup() returns NULL.
 */
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va)
{
    if (xbox_va >= KERNEL_VA_BASE && xbox_va < KERNEL_VA_END) {
        int slot = (xbox_va - KERNEL_VA_BASE) / 4;
        if (slot >= 0 && slot < XBOX_KERNEL_THUNK_TABLE_SIZE) {
            g_kernel_dispatch_slot = slot;
            return kernel_thunk_dispatch;
        }
    }
    return NULL;
}

/* ── Initialization ─────────────────────────────────────── */

/**
 * Resolve the kernel thunk table in Xbox memory.
 *
 * Must be called AFTER xbox_MemoryLayoutInit() so Xbox memory is mapped.
 *
 * Reads the actual ordinals from the XBE memory thunk table (0x80000000|ordinal),
 * resolves each to a per-ordinal bridge function, and replaces the entry
 * with a synthetic VA for dispatch.
 */
void xbox_kernel_bridge_init(void)
{
    int i;
    int resolved = 0;
    int bridged = 0;
    int unbridged = 0;
    DWORD old_protect;

    fprintf(stderr, "  Kernel thunk bridge: resolving %d entries at 0x%08X\n",
            XBOX_KERNEL_THUNK_TABLE_SIZE, XBOX_KERNEL_THUNK_TABLE_BASE);

    /* The thunk table lives in .rdata which is marked PAGE_READONLY.
     * Temporarily make it writable so we can patch the ordinals. */
    VirtualProtect(
        (LPVOID)((uintptr_t)XBOX_KERNEL_THUNK_TABLE_BASE + g_xbox_mem_offset),
        XBOX_KERNEL_THUNK_TABLE_SIZE * 4,
        PAGE_READWRITE,
        &old_protect
    );

    /* Initialize kernel data export values first */
    kernel_data_init();

    for (i = 0; i < XBOX_KERNEL_THUNK_TABLE_SIZE; i++) {
        uint32_t va = XBOX_KERNEL_THUNK_TABLE_BASE + i * 4;
        uint32_t current = BRIDGE_MEM32(va);

        if (current & 0x80000000) {
            /* Read the actual ordinal from Xbox memory */
            ULONG ordinal = current & 0x7FFFFFFF;
            g_slot_ordinals[i] = ordinal;

            /* Check if this is a data export */
            uint32_t data_va = kernel_data_va_for_ordinal(ordinal);
            if (data_va) {
                /* DATA export: point thunk to actual data in mapped memory.
                 * This allows the game to dereference the thunk entry. */
                BRIDGE_MEM32(va) = data_va;
                resolved++;
                bridged++;
                continue;
            }

            /* FUNCTION export: use synthetic VA for dispatch */
            g_slot_bridges[i] = bridge_for_ordinal(ordinal);
            g_slot_arg_bytes[i] = stdcall_args_for_ordinal(ordinal);
            if (g_slot_bridges[i]) {
                bridged++;
            } else {
                unbridged++;
            }

            /* Replace Xbox memory entry with synthetic VA */
            uint32_t synthetic = KERNEL_VA_BASE + i * 4;
            BRIDGE_MEM32(va) = synthetic;
            resolved++;
        }
    }

    /* Restore original protection */
    VirtualProtect(
        (LPVOID)((uintptr_t)XBOX_KERNEL_THUNK_TABLE_BASE + g_xbox_mem_offset),
        XBOX_KERNEL_THUNK_TABLE_SIZE * 4,
        old_protect,
        &old_protect
    );

    fprintf(stderr, "  Kernel thunk bridge: %d/%d resolved (%d bridged, %d stub)\n",
            resolved, XBOX_KERNEL_THUNK_TABLE_SIZE, bridged, unbridged);
    fprintf(stderr, "  Synthetic VA range: 0x%08X-0x%08X\n",
            KERNEL_VA_BASE, KERNEL_VA_BASE + (resolved - 1) * 4);

}
